#!/usr/bin/env python3
"""Decode the glide-pico flash log (see src/glide-pico/src/flashlog.h).

Input: a binary dump of the log area (flash offset 0x080000..0x200000), made with
  picotool save -r 0x10080000 0x10200000 dump.bin -t bin
Output: the log text of each boot, oldest first.

Usage: pico-log-decode.py dump.bin [--last N]
"""
import struct
import sys

SLOT_SIZE = 0x40000
SLOTS = 6
PAGE = 256
MAGIC = 0x474F4C47  # "GLOG"
VERSION = 1


def slots(data):
    for i in range(SLOTS):
        base = i * SLOT_SIZE
        if base + PAGE > len(data):
            break
        magic, version, seq, size = struct.unpack_from("<4I", data, base)
        if magic != MAGIC or version != VERSION or size != SLOT_SIZE:
            continue
        fw = data[base + 16:base + 32].split(b"\0", 1)[0].decode("ascii", "replace")
        text = data[base + PAGE:base + SLOT_SIZE]
        end = text.find(b"\xff")
        full = end < 0
        text = text if full else text[:end]
        yield seq, i, fw, full, text.decode("ascii", "replace")


def main():
    args = sys.argv[1:]
    if not args or args[0] in ("-h", "--help"):
        print(__doc__)
        return 1
    last = None
    if "--last" in args:
        k = args.index("--last")
        last = int(args[k + 1])
        del args[k:k + 2]
    with open(args[0], "rb") as f:
        data = f.read()
    found = sorted(slots(data))
    if not found:
        print("no log slots found")
        return 1
    if last:
        found = found[-last:]
    for seq, slot, fw, full, text in found:
        print(f"===== boot {seq} (slot {slot}, firmware {fw}, {len(text)} bytes{', slot full' if full else ''}) =====")
        print(text.replace("\r\n", "\n"), end="" if text.endswith("\n") else "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
