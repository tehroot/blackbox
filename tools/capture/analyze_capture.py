#!/usr/bin/env python3
"""Analyze a GSCap capture: segments of constant monitor index, and monitor crossings.

Usage: python3 analyze_capture.py <gscap log>   (a .gz file is also accepted)

For each segment of constant monitor index (byte 5 of report 0x63), it shows the
X/Y range, the display that held the cursor, and which display mapping fits the cursor
position (macOS maps the absolute position onto the display that holds the cursor).
Then it lists each index change with its exit and entry edge.
"""
import collections
import gzip
import re
import sys

EDGE = 1200  # a position nearer than this to a border counts as "at the edge"


def read_lines(path):
    opener = gzip.open if path.endswith(".gz") else open
    with opener(path, "rt") as f:
        yield from f


def edge(x, y):
    e = ("L" if x < EDGE else "") + ("R" if x > 32767 - EDGE else "")
    e += ("T" if y < EDGE else "") + ("B" if y > 32767 - EDGE else "")
    return e or "-"


def main(path):
    displays, rows = {}, []
    for line in read_lines(path):
        m = re.match(r"DISPLAY id=(\d+) .*bounds=\((-?\d+),(-?\d+) (\d+)x(\d+)\)", line)
        if m:
            displays[int(m[1])] = tuple(int(v) for v in m.groups()[1:])
            continue
        m = re.match(r"\s*([\d.]+) id=63 .*x=\s*(\d+) y=\s*(\d+) mon=\s*(\d+) cursor=\((-?\d+),(-?\d+)\)", line)
        if m:
            rows.append((float(m[1]),) + tuple(int(v) for v in m.groups()[1:]))

    def on(px, py):
        for k, (x, y, w, h) in displays.items():
            if x <= px < x + w and y <= py < y + h:
                return k
        return None

    print("displays:", displays)
    print("segments:")
    seg = None
    segments = []
    for t, x, y, mon, cx, cy in rows:
        if seg is None or mon != seg["mon"]:
            seg = {"mon": mon, "t0": t, "n": 0, "xs": [], "ys": [],
                   "disp": collections.Counter(), "fit": collections.Counter()}
            segments.append(seg)
        seg["n"] += 1
        seg["t1"] = t
        seg["xs"].append(x)
        seg["ys"].append(y)
        seg["disp"][on(cx, cy)] += 1
        for k, (dx, dy, w, h) in displays.items():
            if abs(dx + x * w / 32768 - cx) <= 3 and abs(dy + y * h / 32768 - cy) <= 3:
                seg["fit"][k] += 1
    for s in segments:
        print(f" t={s['t0']:6.2f}-{s['t1']:6.2f} mon={s['mon']} n={s['n']:5d} "
              f"x={min(s['xs'])}..{max(s['xs'])} y={min(s['ys'])}..{max(s['ys'])} "
              f"cursor_on={dict(s['disp'])} mapping_fits={dict(s['fit'])}")

    print("crossings:")
    counts = collections.Counter()
    for a, b in zip(rows, rows[1:]):
        if a[3] != b[3]:
            key = (a[3], edge(a[1], a[2]), b[3], edge(b[1], b[2]))
            counts[key] += 1
            print(f" t={b[0]:6.2f} mon {a[3]} ({a[1]:5d},{a[2]:5d}) exit {key[1]:2} -> "
                  f"mon {b[3]} ({b[1]:5d},{b[2]:5d}) enter {key[3]}")
    print("crossing summary (from, exit edge, to, entry edge):")
    for k, v in sorted(counts.items()):
        print(f" {v:3d} {k}")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
