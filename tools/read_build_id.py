#!/usr/bin/env python3
"""Read the GNU build id out of an ELF file.

The build id identifies one build of the host executable. A snippet compiled
against that build carries the same string, and the agent compares the two
before letting the module run, so a snippet whose offsets into host types were
computed from different source is refused instead of writing to whatever those
offsets now address.

This parses the note directly so the build does not depend on readelf or
objcopy being installed. It is also importable, which is how the build oracle
stamps the same value.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

PT_NOTE = 4
NT_GNU_BUILD_ID = 3


class BuildIdError(RuntimeError):
    pass


def read_build_id(path: Path) -> str:
    """Return the build id of an ELF file as lowercase hex."""
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise BuildIdError(f"cannot read {path}: {exc}") from exc

    if len(data) < 64 or data[:4] != b"\x7fELF":
        raise BuildIdError(f"{path} is not an ELF file")

    elf_class = data[4]
    endian = "<" if data[5] == 1 else ">"
    if elf_class == 2:
        phoff, = struct.unpack_from(endian + "Q", data, 0x20)
        phentsize, phnum = struct.unpack_from(endian + "HH", data, 0x36)
        note_header = endian + "IIQQQQQQ"
        type_offset, offset_offset, filesz_offset = 0, 8, 32
    elif elf_class == 1:
        phoff, = struct.unpack_from(endian + "I", data, 0x1C)
        phentsize, phnum = struct.unpack_from(endian + "HH", data, 0x2A)
        note_header = endian + "IIIIIIII"
        type_offset, offset_offset, filesz_offset = 0, 4, 16
    else:
        raise BuildIdError(f"{path} has an unknown ELF class {elf_class}")

    for index in range(phnum):
        base = phoff + index * phentsize
        if base + phentsize > len(data):
            break
        fields = struct.unpack_from(note_header, data, base)
        if elf_class == 2:
            p_type, p_offset, p_filesz = fields[0], fields[2], fields[5]
        else:
            p_type, p_offset, p_filesz = fields[0], fields[1], fields[4]
        if p_type != PT_NOTE:
            continue

        cursor = p_offset
        end = p_offset + p_filesz
        while cursor + 12 <= end:
            namesz, descsz, ntype = struct.unpack_from(endian + "III", data, cursor)
            cursor += 12
            name = data[cursor:cursor + namesz]
            cursor += (namesz + 3) & ~3
            desc = data[cursor:cursor + descsz]
            cursor += (descsz + 3) & ~3
            if ntype == NT_GNU_BUILD_ID and name.rstrip(b"\0") == b"GNU":
                return desc.hex()

    raise BuildIdError(
        f"{path} carries no GNU build id note; link with --build-id to produce one"
    )


def write_header(build_id: str, output: Path) -> None:
    """Write the header the snippet targets are compiled with."""
    output.parent.mkdir(parents=True, exist_ok=True)
    contents = (
        "// Generated after the host executable links. Do not edit.\n"
        "//\n"
        "// Snippets are force-included with this file, so the definition in\n"
        "// agent_abi.h that it enables is compiled into every module the build\n"
        "// produces, whatever order that module's own includes are in.\n"
        "#pragma once\n"
        f'#define RUNTIME_AGENT_TARGET_BUILD_ID "{build_id}"\n'
    )
    # Writing only on change keeps a rebuild of the executable that produces the
    # same binary from recompiling every snippet.
    if output.is_file() and output.read_text(encoding="utf-8") == contents:
        return
    output.write_text(contents, encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path, help="ELF file to read")
    parser.add_argument("--header", type=Path, help="write a header defining the build id")
    args = parser.parse_args(argv)
    try:
        build_id = read_build_id(args.binary.resolve())
        if args.header is not None:
            write_header(build_id, args.header.resolve())
        else:
            print(build_id)
        return 0
    except BuildIdError as exc:
        print(f"read_build_id: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
