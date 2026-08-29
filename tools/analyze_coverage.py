#!/usr/bin/env python3
"""Decide, at build time, whether replacing a function can be complete.

Replacing a function by rewriting its entry only reaches calls that arrive
there. Anything the compiler inlined, cloned, folded with another function, or
optimized around keeps running the original, and none of that is recoverable
from an optimized binary once it is built. It is all known while building.

This reads what the build knows and writes a manifest keyed by the executable's
build id. The runtime checks that the binary it is in matches the manifest and
reports what the manifest says, rather than trying to rediscover the compiler's
decisions from memory.

Evidence, and what each piece rules out:

  final ELF symbols        two names at one address, so patching one silently
                           patches the other
  patchable entries        whether the function has a reserved area at all
  IPA clone dumps          inlined copies and specialized clones, recorded by
                           the compiler as it made them
  preparation provider     how the build stopped callers reasoning about the
                           body, which is what makes DWARF unnecessary here
  build id                 that all of the above describes the binary in front
                           of you
  caller execution domain  which threads the callers run on, which none of the
                           above answers

Coverage is complete only when every modality that could hide a copy has been
answered. A target prepared as an opaque non-LTO translation unit needs no
inline evidence from DWARF: callers in other units could only see the
declaration, so there is nothing for them to have inlined.

Knowing every call site is not knowing when they run. A complete call graph
says who calls a function and never on which thread, so the execution domain of
the callers is recorded as its own field and is declared by whoever embeds the
agent rather than derived here. Nothing in an ELF file answers it.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import subprocess
import sys


def run(command: list[str]) -> str:
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    return result.stdout if result.returncode == 0 else ""


def build_id(binary: pathlib.Path) -> str | None:
    for line in run(["readelf", "-n", str(binary)]).splitlines():
        if "Build ID:" in line:
            return line.split("Build ID:")[1].strip()
    return None


def symbols(binary: pathlib.Path) -> list[tuple[int, str, str]]:
    """Address, type and name for every defined symbol."""
    found = []
    for line in run(["nm", "--defined-only", str(binary)]).splitlines():
        parts = line.split()
        if len(parts) == 3:
            try:
                found.append((int(parts[0], 16), parts[1], parts[2]))
            except ValueError:
                continue
    return found


def aliases_at(binary: pathlib.Path, name: str) -> list[str]:
    """Every other name sharing this function's address.

    Two names at one address means the compiler folded two functions together,
    and patching the entry reaches callers of both. That is a coverage fact, not
    a failure, but a caller has to be told.
    """
    table = symbols(binary)
    address = next((a for a, _, n in table if n == name), None)
    if address is None:
        return []
    return sorted(n for a, _, n in table if a == address and n != name)


def patchable_entries(binary: pathlib.Path) -> int:
    """How many functions this binary reserved an entry area for."""
    for line in run(["readelf", "-SW", str(binary)]).splitlines():
        if "__patchable_function_entries" in line:
            fields = line.split()
            # readelf -SW columns after the type are Address, Off, Size. The
            # section is an array of addresses with no lengths, so its size in
            # bytes over eight is how many functions were prepared.
            for index, field in enumerate(fields):
                if field == "PROGBITS" and index + 3 < len(fields):
                    return int(fields[index + 3], 16) // 8
    return 0


CLONE = re.compile(
    r"^Callgraph clone;(?P<from>[^;]+);[^;]*;[^;]*;[^;]*;[^;]*;"
    r"(?P<to>[^;]+);.*;(?P<what>[a-z ]+)$"
)


def clone_records(build_dir: pathlib.Path,
                  name: str) -> tuple[list[dict[str, str]], int]:
    """What the compiler recorded doing to this function, and how many dumps said so.

    An inline or a clone is a copy the entry never sees, so each one is a hole
    in what replacing the entry covers.

    The dump count is returned because no records and no dumps are different
    answers. A build that emitted none has not been asked the question, and
    reporting that as "no clones" would turn a missing modality into a clean
    bill of health.
    """
    records = []
    dumps = list(build_dir.rglob("*.ipa-clones"))
    for dump in dumps:
        for line in dump.read_text(errors="replace").splitlines():
            match = CLONE.match(line.strip())
            if match is None:
                continue
            if match.group("from") != name:
                continue
            records.append({
                "into": match.group("to"),
                "kind": match.group("what").strip(),
                "recordedIn": dump.name,
            })
    return records, len(dumps)


def preparation(compile_db: pathlib.Path, source_hint: str) -> dict[str, object]:
    """How the build stopped callers from reasoning about the body."""
    flags: list[str] = []
    try:
        entries = json.loads(compile_db.read_text())
    except (OSError, ValueError):
        return {"provider": "unknown", "flags": []}
    for entry in entries:
        if source_hint not in entry.get("file", ""):
            continue
        command = entry.get("command") or " ".join(entry.get("arguments", []))
        flags = [f for f in command.split() if f.startswith("-f") or f.startswith("-g")]
        break

    opaque = "-fno-lto" in flags
    unfolded = "-fno-ipa-icf" in flags
    prepared = any(f.startswith("-fpatchable-function-entry") for f in flags)
    if opaque and unfolded and prepared:
        provider = "opaque non-LTO TU"
    elif prepared:
        provider = "prepared entry only"
    else:
        provider = "none"
    return {
        "provider": provider,
        "flags": sorted(flags),
        "opaqueToCallers": opaque,
        "foldingDisabled": unfolded,
        "entryReserved": prepared,
    }


def analyze(binary: pathlib.Path,
            build_dir: pathlib.Path,
            compile_db: pathlib.Path,
            target: str,
            source_hint: str,
            caller_domain: str | None,
            domain_caveat: str | None) -> dict[str, object]:
    prepared = preparation(compile_db, source_hint)
    found_aliases = aliases_at(binary, target)
    clones, dump_count = clone_records(build_dir, target)

    evidence: list[dict[str, object]] = [
        {"modality": "final ELF symbols", "answered": True,
         "finding": f"{len(found_aliases)} other name(s) at this address"},
        {"modality": "patchable entries", "answered": patchable_entries(binary) > 0,
         "finding": f"{patchable_entries(binary)} prepared entr(y/ies) in this binary"},
        {"modality": "IPA clone dumps", "answered": dump_count > 0,
         "finding": f"{len(clones)} record(s) naming this function, from {dump_count} dump(s)"
                    if dump_count
                    else "no clone dumps in this build, so inlining and specialization "
                         "were never recorded; build with PATCH_READY"},
        {"modality": "preparation", "answered": prepared["provider"] != "none",
         "finding": prepared["provider"]},
    ]

    # Callers in other translation units saw only the declaration, so there was
    # nothing for them to inline and no inline instance for DWARF to record.
    # This is why the running binary needs no debug information.
    dwarf_required = not prepared.get("opaqueToCallers", False)
    evidence.append({
        "modality": "split DWARF inline instances",
        "answered": not dwarf_required,
        "finding": "not required: the body was opaque to callers in other units"
                   if not dwarf_required
                   else "required: callers could see this body and may hold copies",
    })

    # Declared, never derived. A build system can see every call site and still
    # know nothing about which thread reaches them, so this is recorded as
    # somebody's claim with their caveat attached, and it is deliberately not
    # folded into the coverage verdict: it answers whether the target can be
    # written safely at a given moment, which is a different question from
    # whether replacing it reaches everything.
    domain = {
        "declared": caller_domain,
        "provenance": "declared by the embedding, not derived from the binary",
        "caveat": domain_caveat,
    }

    unknown = [e["modality"] for e in evidence if not e["answered"]]
    skipped = [{"what": c["into"], "why": c["kind"]} for c in clones]

    if unknown:
        coverage = "unknown"
    elif skipped:
        coverage = "incomplete"
    else:
        coverage = "complete"

    return {
        "target": target,
        "buildId": build_id(binary),
        "binary": str(binary),
        "coverage": coverage,
        "decision": "allow" if coverage == "complete" else "refuse",
        "patched": [target] if coverage == "complete" else [],
        "skipped": skipped,
        "unknown": unknown,
        "evidence": evidence,
        "callerExecutionDomain": domain,
        "aliases": found_aliases,
        "clones": clones,
        "preparation": prepared,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=pathlib.Path)
    parser.add_argument("--build-dir", required=True, type=pathlib.Path)
    parser.add_argument("--compile-db", type=pathlib.Path)
    parser.add_argument("--target", default="cube_step_builtin")
    parser.add_argument("--source-hint", default="cube_step.cpp")
    parser.add_argument("--caller-domain",
                        help="which threads the callers of this target run on, if the "
                             "embedding knows. Recorded as a claim, never derived")
    parser.add_argument("--caller-domain-caveat",
                        help="what would make the declaration untrue")
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()

    compile_db = args.compile_db or (args.build_dir / "compile_commands.json")
    report = analyze(args.binary, args.build_dir, compile_db, args.target,
                     args.source_hint, args.caller_domain, args.caller_domain_caveat)
    if report["buildId"] is None:
        print("the binary carries no build id, so a manifest could not be tied to it",
              file=sys.stderr)
        return 1

    text = json.dumps(report, indent=2)
    if args.output:
        args.output.write_text(text + "\n")
    else:
        print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
