#!/usr/bin/env python3
"""Decide, at build time, whether replacing a function can be complete.

Replacing a function by rewriting its entry only reaches calls that arrive
there. Anything the compiler inlined, cloned, folded with another function, or
optimized around keeps running the original, and none of that is recoverable
from an optimized binary once it is built. It is all known while building.

This reads what the build knows and writes a manifest keyed by the executable's
build id. The runtime checks that the binary it is in matches the manifest and
reports what the manifest says. Nothing rediscovers the compiler's decisions
from memory.

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
the callers is recorded as its own field. Nothing in an ELF file answers it.

How well it is known matters as much as what is claimed, so the field carries a
strength:

  proved     every caller is enumerated and each one's thread is established
  declared   the embedding states it and takes responsibility for the claim
  observed   only this was seen happening, which is not the same as this being
             all that can happen
  unknown    nobody has said

Only proved and declared may stand in for stopping execution at a request
boundary. Observed cannot: watching a function and seeing one thread reach it
does not rule out a worker that reaches it under a condition nobody exercised,
so it selects stop-all or refusal unless a caller has explicitly asked for
best effort.
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


def patchable_addresses(binary: pathlib.Path) -> list[int]:
    """Every address this binary reserved an entry area at.

    Counting them says only that something was prepared. Which addresses they
    are is what says whether this target was, and that is the difference between
    reporting on a function and reporting on a binary that happens to contain
    one.
    """
    words: list[int] = []
    for line in run(["readelf", "-x", "__patchable_function_entries",
                     str(binary)]).splitlines():
        parts = line.split()
        if len(parts) < 2 or not parts[0].startswith("0x"):
            continue
        blob = "".join(p for p in parts[1:] if all(c in "0123456789abcdefABCDEF" for c in p))
        for index in range(0, len(blob) - 15, 16):
            try:
                words.append(int.from_bytes(bytes.fromhex(blob[index:index + 16]), "little"))
            except ValueError:
                continue
    return words


CLONE = re.compile(
    r"^Callgraph clone;(?P<from>[^;]+);[^;]*;[^;]*;[^;]*;[^;]*;"
    r"(?P<to>[^;]+);.*;(?P<what>[a-z ]+)$"
)


def owning_object(compile_db: pathlib.Path,
                  binary: pathlib.Path,
                  source_hint: str) -> str | None:
    """The object file that this binary's build compiled the target from.

    More than one target can compile the same source, and this tree does: the
    application and the self-test both build cube_step.cpp, each producing its
    own object and its own dump. Matching on the source name alone picks
    whichever comes first, so the answer could describe a translation unit that
    is not in the binary being analyzed.
    """
    try:
        entries = json.loads(compile_db.read_text())
    except (OSError, ValueError):
        return None
    # CMake puts each target's objects under CMakeFiles/<target>.dir.
    marker = f"CMakeFiles/{binary.name}.dir/"
    for entry in entries:
        output = entry.get("output", "")
        if source_hint in entry.get("file", "") and marker in output:
            return output
    return None


def clone_dump_for(build_dir: pathlib.Path, object_path: str) -> pathlib.Path | None:
    """The dump GCC wrote beside that object.

    Named from the output path with the object suffix replaced, so it is found
    by where it sits and not by searching for anything that looks like one.
    """
    obj = build_dir / object_path
    stem = obj.with_suffix("")
    matches = sorted(stem.parent.glob(stem.name + "*.ipa-clones"))
    return matches[0] if matches else None


def clone_records_in(dump: pathlib.Path, name: str) -> list[dict[str, str]]:
    """What that one dump recorded the compiler doing to this function.

    An inline or a clone is a copy the entry never sees, so each one is a hole
    in what replacing the entry covers.
    """
    records = []
    for line in dump.read_text(errors="replace").splitlines():
        match = CLONE.match(line.strip())
        if match is None or match.group("from") != name:
            continue
        records.append({
            "into": match.group("to"),
            "kind": match.group("what").strip(),
            "recordedIn": dump.name,
        })
    return records


def preparation(compile_db: pathlib.Path,
                source_hint: str,
                owning: str | None) -> dict[str, object]:
    """How the build stopped callers from reasoning about the body.

    Read from the object this binary was built from. Two targets compiling the
    same source can compile it differently, and taking whichever entry comes
    first answers about one of them at random.
    """
    flags: list[str] = []
    try:
        entries = json.loads(compile_db.read_text())
    except (OSError, ValueError):
        return {"provider": "unknown", "flags": []}
    for entry in entries:
        if source_hint not in entry.get("file", ""):
            continue
        if owning is not None and entry.get("output") != owning:
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


def address_of(binary: pathlib.Path, name: str) -> int | None:
    return next((a for a, _, n in symbols(binary) if n == name), None)


def analyze(binary: pathlib.Path,
            build_dir: pathlib.Path,
            compile_db: pathlib.Path,
            target: str,
            source_hint: str,
            caller_domain: str | None,
            domain_strength: str,
            domain_caveat: str | None,
            accept_aliases: bool) -> dict[str, object]:
    # A target that is not in the binary cannot be reported on. Without this its
    # absence reads as the absence of aliases and the absence of clones, and a
    # function nobody ever compiled comes back allowed.
    target_address = address_of(binary, target)
    if target_address is None:
        return {
            "target": target,
            "buildId": build_id(binary),
            "binary": str(binary),
            "coverage": "unknown",
            "decision": "refuse",
            "patched": [],
            "skipped": [],
            "unknown": ["target symbol"],
            "evidence": [{"modality": "target symbol", "answered": False,
                          "finding": "no symbol of this name is defined in this binary"}],
            "aliases": [],
            "clones": [],
        }

    # Tied to the object this binary was built from, so neither a dump nor a set
    # of flags belonging to some other target answers for this one.
    owning = owning_object(compile_db, binary, source_hint)
    prepared = preparation(compile_db, source_hint, owning)
    found_aliases = aliases_at(binary, target)
    dump = clone_dump_for(build_dir, owning) if owning else None
    clones = clone_records_in(dump, target) if dump else []

    # This target's own reserved area, not merely the existence of somebody's.
    # The compiler records the address of the area, which is the function's own
    # address or four bytes past it where a CET landing pad comes first.
    # The compiler records the function's own address, or four bytes past it
    # where a CET landing pad comes first. Those are the two the runtime accepts,
    # so accepting a wider window here would approve a site it will refuse.
    recorded = [a for a in patchable_addresses(binary)
                if a in (target_address, target_address + 4)]

    evidence: list[dict[str, object]] = [
        {"modality": "final ELF symbols", "answered": True,
         "finding": f"{len(found_aliases)} other name(s) at this address"},
        {"modality": "prepared entry for this target", "answered": bool(recorded),
         "finding": f"reserved at +{recorded[0] - target_address}" if recorded
                    else "this target has no reserved area, so its entry cannot be "
                         "rewritten without relocating real instructions"},
        {"modality": "IPA clone dumps for the owning object",
         "answered": dump is not None,
         "finding": f"{len(clones)} record(s) naming this function in {dump.name}"
                    if dump is not None
                    else ("the object this binary compiled the target from has no clone "
                          "dump, so inlining and specialization were never recorded for "
                          "it; build with PATCH_READY" if owning
                          else "no object in this binary's build compiled the target's "
                               "source, so there is nothing to have recorded")},
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
    # Only a claim somebody takes responsibility for, or one established over
    # every caller, may stand in for stopping execution. Having watched a
    # function and seen one thread reach it is a different and weaker thing.
    strength = domain_strength if caller_domain else "unknown"
    domain = {
        "strength": strength,
        "claim": caller_domain,
        "authorizesRequestBoundary": strength in ("proved", "declared"),
        "caveat": domain_caveat,
    }

    unknown = [e["modality"] for e in evidence if not e["answered"]]
    skipped = [{"what": c["into"], "why": c["kind"]} for c in clones]

    # Two names at one address means the compiler folded two functions together,
    # and rewriting the entry changes what both of them do. Reporting that while
    # allowing would quietly alter something the caller never named, so it is a
    # refusal unless the request says it accepts every name at that address.
    if found_aliases and not accept_aliases:
        skipped.extend({"what": alias, "why": "shares this address, so replacing "
                                              "this target replaces it too"}
                       for alias in found_aliases)

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
        "compileOutput": owning,
        "cloneDump": str(dump) if dump is not None else None,
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
    parser.add_argument("--caller-domain-strength",
                        choices=("proved", "declared", "observed", "unknown"),
                        default="unknown",
                        help="how well that is known. Only proved and declared may "
                             "stand in for stopping execution")
    parser.add_argument("--caller-domain-caveat",
                        help="what would make the declaration untrue")
    parser.add_argument("--accept-aliases", action="store_true",
                        help="allow a target that shares its address with other names, "
                             "accepting that replacing it replaces all of them")
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()

    compile_db = args.compile_db or (args.build_dir / "compile_commands.json")
    report = analyze(args.binary, args.build_dir, compile_db, args.target,
                     args.source_hint, args.caller_domain,
                     args.caller_domain_strength, args.caller_domain_caveat,
                     args.accept_aliases)
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
