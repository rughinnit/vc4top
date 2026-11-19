#!/usr/bin/env python3
"""Replay a `script` capture of the live TUI and count repaint/scroll events.

Usage: tools/check_pty_replay.py CAPTURE ROWS COLS

Checks that the alternate screen is entered and left once, that each frame is
one `ESC[H` repaint with exactly ROWS-1 CRLFs (no newline on the bottom row),
that no frame scrolls and that no printable run exceeds COLS cells.
"""
import re
import sys


def main(path, rows, cols):
    data = open(path, "rb").read().decode("utf-8", "replace")
    if data.startswith("Script "):
        data = data.split("\n", 1)[-1]
    alt_enter = data.count("\033[?1049h")
    alt_exit = data.count("\033[?1049l")
    wrap_off = data.count("\033[?7l")
    wrap_on = data.count("\033[?7h")
    frames = data.split("\033[H")[1:]
    homes = 0
    scrolls = 0
    newlines = []
    over = 0
    for f in frames:
        homes += 1
        # A frame ends with wrap-on; anything after it is restore/trailer.
        end = f.find("\033[?7h")
        if end >= 0:
            f = f[:end + len("\033[?7h")]
        r = 0
        c = 0
        nl = 0
        i = 0
        while i < len(f):
            ch = f[i]
            if ch == "\033":
                m = re.match(r"\033\[[0-9;?]*[@-~]", f[i:])
                if m:
                    seq = m.group(0)
                    if seq in ("\033[2J", "\033[H"):
                        r = 0
                    i += len(seq)
                    continue
                i += 1
                continue
            if ch == "\r":
                c = 0
            elif ch == "\n":
                nl += 1
                if r >= rows - 1:
                    scrolls += 1
                else:
                    r += 1
                c = 0
            else:
                if c >= cols:
                    over += 1
                c += 1
            i += 1
        newlines.append(nl)
    ok = (scrolls == 0 and over == 0 and alt_enter == 1 and alt_exit == 1 and
          homes >= 3 and all(n == rows - 1 for n in newlines))
    print("%s: frames=%d homes=%d scrolls=%d newlines=%s alt=%d/%d wrap=%d/%d over=%d" %
          ("PASS" if ok else "FAIL", len(frames), homes, scrolls,
           sorted(set(newlines)), alt_enter, alt_exit, wrap_off, wrap_on, over))
    return 0 if ok else 1


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1], int(sys.argv[2]), int(sys.argv[3])))
