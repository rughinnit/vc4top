# 1.0.5 wide / horizontal desktop layout

Captured on the Raspberry Pi with stock clocks, one run at a time under
`flock /tmp/vc4-gpu.lock`, unprivileged (16-counter `vc4mon` map), plain text
(`--frame` piped, so no ANSI escapes).

| Size | Layout | Before last used row | After last used row | After blank rows |
|---|---|---|---|---|
| 80x24 | narrow, scrolls | 24 | 24 | 0 |
| 120x40 | two columns | 40 | 40 | 0 |
| 140x40 | three columns | 37 | 37 | 3 |
| 160x45 | **wide** | 37 | 45 | 0 |
| 192x48 | **wide** | 37 | 48 | 0 |
| 200x50 | **wide** | 37 | 50 | 0 |
| 240x60 | **wide** | 37 | 60 | 0 |
| 256x70 | **wide** | 37 | 70 | 0 |

At 160+ the 1.0.4 layout left the bottom 8-33 rows blank and kept the QPU
panel at ~20 rows beside a 33-row counters/notes column. 1.0.5 uses three
content-sized panels on the top row and a full-width, two-up counter table plus
a full-width notes strip below; both grid rows share the spare height.

Checked with the committed tools:

```text
$ for s in 80x24 ... 256x70; do tools/check_wide_capture.py docs/ui/wide/after-$s.txt ${s%x*} ${s#*x}; done
PASS: 24 lines, widths=[80], 0 counter rows, 0 clip markers
PASS: 40 lines, widths=[120], 0 counter rows, 0 clip markers
PASS: 40 lines, widths=[140], 0 counter rows, 0 clip markers
PASS: 45 lines, widths=[160], 16 counter rows, 0 clip markers
PASS: 48 lines, widths=[192], 16 counter rows, 0 clip markers
PASS: 50 lines, widths=[200], 16 counter rows, 0 clip markers
PASS: 60 lines, widths=[240], 16 counter rows, 0 clip markers
PASS: 70 lines, widths=[256], 16 counter rows, 0 clip markers

$ tools/check_pty_replay.py /tmp/pty-160x45.log 45 160
PASS: frames=9 homes=9 scrolls=0 newlines=[44] alt=1/1 wrap=10/10 over=0
$ tools/check_pty_replay.py /tmp/pty-256x70.log 70 256
PASS: frames=9 homes=9 scrolls=0 newlines=[69] alt=1/1 wrap=10/10 over=0
```

All eight sizes replay with `scrolls=0`, `rows - 1` newlines per frame, one
alternate-screen enter/exit and one `ESC[H` per frame. `VC4TOP_STATS=1` reports
`frame_write_calls=1.00`.

The 136-159 column, 100-135 column and under-100 column layouts are unchanged;
80x24/120x40/140x40 after files differ from 1.0.4 only in volatile values.
