#!/usr/bin/env python3
"""Create a deterministic mutated callback fixture for AOT identity tests.

The mutation changes the FNV offset immediate in .aevtext from 0x811c9dc5 to
0x811c9dc6.  It keeps the function size, instruction boundaries and block
addresses unchanged, so only the input identity/hash check can reject a dylib
built from this object when paired with the canonical COFF.
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

PATTERN = bytes.fromhex("c59d1c81")
REPLACEMENT = bytes.fromhex("c69d1c81")


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <input.obj> <output.obj>", file=sys.stderr)
        return 2
    src = bytearray(Path(sys.argv[1]).read_bytes())
    if len(src) < 20:
        raise SystemExit("input too small for COFF header")
    machine, nsections, _timestamp, _symptr, _nsyms, opt_size = struct.unpack_from(
        "<HHIIIH", src, 0
    )
    if machine != 0x8664:
        raise SystemExit("input is not x86-64 COFF")
    section_off = 20 + opt_size
    for i in range(nsections):
        off = section_off + i * 40
        name = bytes(src[off : off + 8]).split(b"\0", 1)[0].decode("ascii", "replace")
        raw_size, raw_ptr = struct.unpack_from("<II", src, off + 16)
        if name != ".aevtext":
            continue
        if raw_size == 0 or raw_ptr + raw_size > len(src):
            raise SystemExit("invalid .aevtext section")
        section = src[raw_ptr : raw_ptr + raw_size]
        pos = section.find(PATTERN)
        if pos < 0:
            raise SystemExit("FNV offset pattern not found in .aevtext")
        section[pos : pos + 4] = REPLACEMENT
        src[raw_ptr : raw_ptr + raw_size] = section
        Path(sys.argv[2]).write_bytes(src)
        print(f"mutated .aevtext offset 0x{pos:x} -> {sys.argv[2]}")
        return 0
    raise SystemExit(".aevtext section not found")


if __name__ == "__main__":
    raise SystemExit(main())
