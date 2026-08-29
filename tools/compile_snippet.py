#!/usr/bin/env python3
"""Compile a runtime snippet by cloning one translation unit's build context.

The tool consumes CMake/Ninja ``compile_commands.json``, keeps the original
compiler, working directory, include order, preprocessor state, language mode,
and architecture/ABI flags, then replaces the input/output/dependency flags and
builds a PIC shared object suitable for ``snippet.load`` or ``patch.load``.
"""

from __future__ import annotations

import argparse
import json
import os
import shlex
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Sequence

from read_build_id import BuildIdError, read_build_id


class BuildOracleError(RuntimeError):
    pass


@dataclass(frozen=True)
class CompileEntry:
    directory: Path
    file: Path
    output: str | None
    arguments: tuple[str, ...]


_DEPENDENCY_FLAGS = {"-MD", "-MMD", "-MP", "-MG"}
_DEPENDENCY_VALUE_FLAGS = {"-MF", "-MT", "-MQ", "-MJ"}
_DISCARD_EXACT_FLAGS = {
    "-ffat-lto-objects",
    "-fno-fat-lto-objects",
    "-fuse-linker-plugin",
    "-fprofile",
    "-ftest-coverage",
    "-pg",
}
# The build force-includes this into its own module targets. A reconstructed
# command must not inherit it, because then whether a module carries a build id
# would depend on which translation unit was named as the context. This tool
# stamps explicitly instead, so the outcome is the same for every context.
_BUILD_ID_HEADER = "runtime_agent_build_id.h"

_PROFILE_PREFIXES = (
    "-flto",
    "-fno-lto",
    "-fprofile-",
    "-fauto-profile",
    "-fno-auto-profile",
    "-fcoverage-",
    "-fcondition-coverage",
    "-fpath-coverage",
)


def load_database(path: Path) -> list[CompileEntry]:
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise BuildOracleError(f"cannot read compilation database {path}: {exc}") from exc
    if not isinstance(raw, list):
        raise BuildOracleError("compilation database root must be an array")

    entries: list[CompileEntry] = []
    for index, item in enumerate(raw):
        if not isinstance(item, dict):
            raise BuildOracleError(f"entry {index} is not an object")
        try:
            directory = Path(item["directory"]).resolve()
            source = Path(item["file"])
        except (KeyError, TypeError) as exc:
            raise BuildOracleError(f"entry {index} lacks directory/file") from exc
        if not source.is_absolute():
            source = directory / source
        if "arguments" in item:
            arguments = tuple(str(value) for value in item["arguments"])
        elif "command" in item:
            arguments = tuple(shlex.split(str(item["command"]), posix=True))
        else:
            raise BuildOracleError(f"entry {index} lacks command/arguments")
        if not arguments:
            raise BuildOracleError(f"entry {index} has an empty compiler command")
        entries.append(
            CompileEntry(
                directory=directory,
                file=source.resolve(),
                output=str(item["output"]) if item.get("output") is not None else None,
                arguments=arguments,
            )
        )
    return entries


def select_entry(entries: Sequence[CompileEntry], context: str) -> CompileEntry:
    context_path = Path(context)
    exact: list[CompileEntry] = []
    suffix: list[CompileEntry] = []
    output_matches: list[CompileEntry] = []

    for entry in entries:
        if context_path.is_absolute() and entry.file == context_path.resolve():
            exact.append(entry)
        if entry.file.as_posix().endswith(context_path.as_posix()):
            suffix.append(entry)
        if entry.output and context in entry.output:
            output_matches.append(entry)

    candidates = exact or suffix or output_matches
    if len(candidates) == 1:
        return candidates[0]
    if not candidates:
        examples = "\n  ".join(str(entry.file) for entry in entries[:12])
        raise BuildOracleError(
            f"no compilation entry matches {context!r}; examples:\n  {examples}"
        )
    # Candidates matched on the same source path, so printing that path once per
    # candidate says nothing. What tells them apart is the object each produced,
    # and an output fragment is itself a valid context, so these are the answers.
    matches = "\n  ".join(entry.output or str(entry.file) for entry in candidates[:20])
    raise BuildOracleError(
        f"ambiguous compilation context {context!r}; {len(candidates)} compilations "
        f"produced:\n  {matches}\nName one by passing enough of its output path to "
        f"--context to be unique.")


def _same_file_argument(token: str, entry: CompileEntry) -> bool:
    candidate = Path(token)
    if not candidate.is_absolute():
        candidate = entry.directory / candidate
    try:
        return candidate.resolve() == entry.file
    except OSError:
        return False


def derive_shared_object_command(
    entry: CompileEntry,
    source: Path,
    output: Path,
    *,
    optimization: str = "Og",
    disable_access_control: bool = True,
    extra: Iterable[str] = (),
) -> list[str]:
    if optimization not in {"O0", "Og", "O1", "O2", "O3", "Os", "Oz"}:
        raise BuildOracleError(f"unsupported optimization setting: {optimization}")

    retained: list[str] = []
    tokens = list(entry.arguments)
    index = 0
    while index < len(tokens):
        token = tokens[index]

        if (token == "-c" or token in _DEPENDENCY_FLAGS
                or token in _DISCARD_EXACT_FLAGS or token == "--coverage"):
            index += 1
            continue
        if token in {"-o", *_DEPENDENCY_VALUE_FLAGS}:
            index += 2
            continue
        if (token == "-include" and index + 1 < len(tokens)
                and Path(tokens[index + 1]).name == _BUILD_ID_HEADER):
            index += 2
            continue
        if token.startswith("-o") and token != "-openmp" and len(token) > 2:
            index += 1
            continue
        if any(token.startswith(flag) and token != flag for flag in _DEPENDENCY_VALUE_FLAGS):
            index += 1
            continue
        if token.startswith("-O") and len(token) >= 2:
            index += 1
            continue
        if token == "-fPIE" or token == "-fpie" or token == "-pie":
            index += 1
            continue
        if token.startswith(_PROFILE_PREFIXES):
            index += 1
            continue
        if _same_file_argument(token, entry):
            index += 1
            continue

        retained.append(token)
        index += 1

    # Preserve the compiler/launcher ordering from the original command. GCC's
    # driver accepts these mode options after the ordinary compile options.
    retained.append(f"-{optimization}")
    if "-fPIC" not in retained and "-fpic" not in retained:
        retained.append("-fPIC")
    if disable_access_control and "-fno-access-control" not in retained:
        retained.append("-fno-access-control")
    retained.extend(str(value) for value in extra)
    retained.extend(["-shared", str(source.resolve()), "-o", str(output.resolve())])
    return retained


def find_host_binary(compile_db: Path, explicit: Path | None) -> Path | None:
    """Locate the executable whose build id a module should be stamped with.

    A module compiled here reaches into the application's types, so it belongs
    to one build of that application. The compile database sits in the build
    directory that produced the executable, so that is where to look.
    """
    if explicit is not None:
        if not explicit.is_file():
            raise BuildOracleError(f"host binary is not a regular file: {explicit}")
        return explicit
    candidate = compile_db.parent / "qt_runtime_cube"
    return candidate if candidate.is_file() else None


def build_id_definition(host_binary: Path | None) -> str | None:
    """The define that makes the module report the build it was compiled for."""
    if host_binary is None:
        return None
    try:
        return f'-DRUNTIME_AGENT_TARGET_BUILD_ID="{read_build_id(host_binary)}"'
    except BuildIdError as exc:
        raise BuildOracleError(str(exc)) from exc


def command_record(entry: CompileEntry, command: Sequence[str], output: Path) -> dict[str, Any]:
    return {
        "contextFile": str(entry.file),
        "workingDirectory": str(entry.directory),
        "output": str(output.resolve()),
        "arguments": list(command),
        "shell": shlex.join(command),
    }


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compile-db", required=True, type=Path)
    parser.add_argument("--context", required=True, help="source path/suffix or output fragment")
    parser.add_argument("--source", required=True, type=Path, help="snippet/patch C++ source")
    parser.add_argument("--output", required=True, type=Path, help="output shared object")
    parser.add_argument(
        "--optimization",
        choices=("O0", "Og", "O1", "O2", "O3", "Os", "Oz"),
        default="Og",
    )
    parser.add_argument(
        "--respect-access-control",
        action="store_true",
        help="do not add GCC -fno-access-control",
    )
    parser.add_argument("--extra", action="append", default=[], help="extra compiler/linker token")
    parser.add_argument(
        "--host-binary",
        type=Path,
        help="executable whose build id stamps the module; "
             "defaults to qt_runtime_cube beside the compile database",
    )
    parser.add_argument(
        "--no-build-id",
        action="store_true",
        help="do not stamp the module, which the agent then reports as unstamped",
    )
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--compact", action="store_true")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        compile_db = args.compile_db.resolve()
        entries = load_database(compile_db)
        entry = select_entry(entries, args.context)
        source = args.source.resolve()
        output = args.output.resolve()
        if not source.is_file():
            raise BuildOracleError(f"source is not a regular file: {source}")
        output.parent.mkdir(parents=True, exist_ok=True)

        extra = list(args.extra)
        if not args.no_build_id:
            definition = build_id_definition(find_host_binary(compile_db, args.host_binary))
            if definition is not None:
                extra.append(definition)
            else:
                print(
                    "compile_snippet: no host binary found beside the compile database, so "
                    "the module carries no build id and the agent will report it as unstamped",
                    file=sys.stderr,
                )

        command = derive_shared_object_command(
            entry,
            source,
            output,
            optimization=args.optimization,
            disable_access_control=not args.respect_access_control,
            extra=extra,
        )
        record = command_record(entry, command, output)
        print(
            json.dumps(
                record,
                indent=None if args.compact else 2,
                separators=(",", ":") if args.compact else None,
            )
        )
        if args.dry_run:
            return 0
        completed = subprocess.run(command, cwd=entry.directory, check=False)
        if completed.returncode != 0:
            raise BuildOracleError(f"compiler exited with status {completed.returncode}")
        if not output.is_file():
            raise BuildOracleError(f"compiler succeeded but did not create {output}")
        return 0
    except (BuildOracleError, OSError) as exc:
        print(f"compile_snippet: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
