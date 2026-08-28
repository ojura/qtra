#!/usr/bin/env python3
"""Compile a project-native replacement DSO and activate it in the live app."""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

from agentctl import AgentClient, ProtocolError, default_socket_path
from compile_snippet import (
    BuildOracleError,
    command_record,
    derive_shared_object_command,
    load_database,
    select_entry,
)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="C++ source exporting cube_step_patch_init")
    parser.add_argument("--compile-db", required=True, type=Path)
    parser.add_argument("--context", required=True, help="translation-unit path/output match")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--socket", default=default_socket_path())
    parser.add_argument("--timeout", type=float, default=20.0)
    parser.add_argument(
        "--optimization",
        default="O3",
        choices=("O0", "Og", "O1", "O2", "O3", "Os", "Oz"),
    )
    parser.add_argument("--mode", default="entry", choices=("dispatch", "entry"))
    parser.add_argument("--respect-access-control", action="store_true")
    parser.add_argument("--extra", action="append", default=[])
    parser.add_argument("--compile-only", action="store_true")
    parser.add_argument("--compact", action="store_true")
    return parser


def default_output(source: Path, compile_db: Path) -> Path:
    # A unique path matters: dlopen() reuses an already-loaded object with the
    # same canonical filename, even when that file has since been overwritten.
    digest = hashlib.sha256(
        source.read_bytes() + str(time.time_ns()).encode("ascii")
    ).hexdigest()[:12]
    directory = compile_db.resolve().parent / "runtime-patches"
    return directory / f"{source.stem}-{digest}.so"


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        source = args.source.resolve()
        if not source.is_file():
            raise BuildOracleError(f"source is not a regular file: {source}")
        compile_db = args.compile_db.resolve()
        output = args.output.resolve() if args.output else default_output(source, compile_db)
        output.parent.mkdir(parents=True, exist_ok=True)

        entry = select_entry(load_database(compile_db), args.context)
        command = derive_shared_object_command(
            entry,
            source,
            output,
            optimization=args.optimization,
            disable_access_control=not args.respect_access_control,
            extra=args.extra,
        )
        result: dict[str, Any] = {
            "compile": command_record(entry, command, output),
        }
        completed = subprocess.run(command, cwd=entry.directory, check=False)
        if completed.returncode != 0:
            raise BuildOracleError(f"compiler exited with status {completed.returncode}")
        if not output.is_file():
            raise BuildOracleError(f"compiler succeeded but did not create {output}")

        if not args.compile_only:
            with AgentClient(args.socket, args.timeout) as client:
                loaded = client.request("patch.load", {"path": str(output)})
                activated = client.request(
                    "patch.activate",
                    {"moduleId": loaded["moduleId"], "mode": args.mode},
                )
                result["runtime"] = {"loaded": loaded, "activated": activated}

        print(
            json.dumps(
                result,
                indent=None if args.compact else 2,
                separators=(",", ":") if args.compact else None,
                sort_keys=not args.compact,
            )
        )
        return 0
    except (BuildOracleError, ConnectionError, OSError, ProtocolError, TimeoutError) as exc:
        print(f"jit_patch: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
