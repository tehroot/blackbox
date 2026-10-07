#!/usr/bin/env python3
"""Print UTF-16LE strings (Windows wide strings) and selected ASCII strings from a binary.

Usage: python3 -I strings_utf16.py <file> [min_len]

macOS `strings` has no UTF-16 option. This finds the URL paths, format strings and
dialog text in the Glide and Switch tray application (for example "/cgi-bin/config").
ASCII strings are printed with an "A:" prefix only when they look like paths, URLs or
format strings.
"""
import re
import sys


def main(path, min_len=5):
    data = open(path, "rb").read()
    for m in re.finditer(rb"(?:[\x20-\x7e]\x00){%d,}" % min_len, data):
        print(m.group().decode("utf-16le"))
    pattern = r"(/|\.cgi|\.htm|\.xml|\.json|POST|GET|%d|%s|monitor|Monitor|screen|Screen|layout)"
    for m in re.finditer(rb"[\x20-\x7e]{%d,}" % min_len, data):
        s = m.group().decode()
        if re.search(pattern, s):
            print("A:", s)


if __name__ == "__main__":
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    main(sys.argv[1], int(sys.argv[2]) if len(sys.argv) == 3 else 5)
