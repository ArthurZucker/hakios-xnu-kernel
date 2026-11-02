#!/usr/bin/env python3
"""Parse and validate the 64-byte arm64 Linux `Image` header of a flat image.

Usage: check_image.py <image> [--quiet]

Exit status 0 = header is valid. Anything else is printed and exit is 1.
Standard library only.
"""

import struct
import sys

MAGIC = 0x644D5241  # "ARM\x64" little-endian

FIELDS = [
    ("code0", 0x00, 4),
    ("code1", 0x04, 4),
    ("text_offset", 0x08, 8),
    ("image_size", 0x10, 8),
    ("flags", 0x18, 8),
    ("res2", 0x20, 8),
    ("res3", 0x28, 8),
    ("res4", 0x30, 8),
    ("magic", 0x38, 4),
    ("res5", 0x3C, 4),
]


def main(argv):
    if len(argv) < 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    path = argv[1]
    quiet = "--quiet" in argv[2:]

    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 64:
        print(f"FAIL: {path} is {len(data)} bytes, shorter than a 64-byte header")
        return 1

    hdr = data[:64]
    fields = {}
    for name, off, size in FIELDS:
        raw = hdr[off:off + size]
        # every field is read little-endian, matching arm64 Image format
        fields[name] = int.from_bytes(raw, "little")

    ok = True
    if not quiet:
        print(f"header of {path} ({len(data)} bytes total):")
        for name, off, size in FIELDS:
            print(f"  0x{off:02x}  {name:<11} = 0x{fields[name]:0{size * 2}x}")
        print(f"  code0 as instruction : 0x{fields['code0']:08x}")
        print(f"  magic as bytes       : {hdr[0x38:0x3c]!r}")

    if fields["magic"] != MAGIC:
        print(f"FAIL: magic at 0x38 is 0x{fields['magic']:08x}, want 0x{MAGIC:08x}")
        ok = False
    if fields["image_size"] != len(data):
        # not fatal for a raw blob, but the kernel would mis-size us
        print(
            f"WARN: image_size 0x{fields['image_size']:x} != file size 0x{len(data):x}"
        )
    if fields["text_offset"] != 0:
        print(f"WARN: text_offset 0x{fields['text_offset']:x}, expected 0")
    if fields["flags"] != 0:
        print(f"WARN: flags 0x{fields['flags']:x}, expected 0 (LE, 4K pages)")

    # code0 must be a real branch (top 6 bits == 0b000101)
    if (fields["code0"] >> 26) != 0b000101:
        print(f"FAIL: code0 0x{fields['code0']:08x} is not a B instruction")
        ok = False

    print("OK: valid arm64 Image header" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
