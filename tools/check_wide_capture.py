#!/usr/bin/env python3
"""Check a vc4top --frame capture for width, height, clipping and counters.

Usage: tools/check_wide_capture.py FILE COLS ROWS

The frame must have exactly ROWS lines, every line must occupy exactly COLS
terminal cells (UTF-8 aware), no line may end in the `>` clip marker, and on a
wide (>= 160 column) capture all 16 sampled counter rows must be visible.
"""
import re
import sys
import unicodedata


def cell_width(s):
    w = 0
    for ch in s:
        if unicodedata.combining(ch):
            continue
        w += 2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1
    return w


def main(path, cols, rows):
    lines = open(path, encoding="utf-8").read().split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    errors = []
    if len(lines) != rows:
        errors.append("%d lines, expected %d" % (len(lines), rows))
    widths = [cell_width(l) for l in lines]
    wide = [i + 1 for i, w in enumerate(widths) if w > cols]
    if wide:
        errors.append("rows wider than %d: %s" % (cols, wide[:5]))
    clipped = [i + 1 for i, l in enumerate(lines) if l.rstrip().endswith(">")]
    if clipped:
        errors.append("clip markers on rows %s" % (clipped[:8],))
    if cols >= 160 and len(lines) >= 2 and \
            re.search(r"\[\d+-\d+/\d+\]", lines[-2]):
        errors.append("viewport is scrolling: %r" % lines[-2].rstrip())

    # Counter rows: a two-up wide table prints two per physical line. Count
    # only inside the COUNTERS box so notes/values cannot be mistaken for rows.
    counter_rows = 0
    if cols >= 160:
        inside = False
        for l in lines:
            if l.startswith("+- COUNTERS"):
                inside = True
            elif inside and l.startswith("+-"):
                inside = False
            elif inside:
                counter_rows += len(
                    re.findall(r"(?<![\d.])\d{1,2}\s{2,}[A-Za-z]", l))
        if counter_rows != 16:
            errors.append("%d counter rows visible, expected 16" % counter_rows)

    if errors:
        print("FAIL: " + "; ".join(errors))
        return 1
    print("PASS: %d lines, widths=%s, %d counter rows, %d clip markers" %
          (len(lines), sorted(set(widths)), counter_rows, len(clipped)))
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1], int(sys.argv[2]), int(sys.argv[3])))
