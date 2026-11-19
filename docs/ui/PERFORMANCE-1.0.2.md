# 1.0.2 re-measurement: repaint, flicker and CPU

Same Pi 3, stock clocks (`vcgencmd measure_clock core` = 400 MHz before and
after every run), default 500 ms interval, one instance at a time, serialized
with `flock /tmp/vc4-gpu.lock`. Baseline: the installed 1.0.1 package binary.

## Result

| metric | 1.0.1 | 1.0.2 |
|---|---|---|
| frame period, 80x24 | 0.544 s (min 0.539, max 0.575) | 0.544 s (min 0.539, max 0.575) |
| `write(2)` calls per frame | 1 | 1 |
| frame write size, 80x24 | 2327 B | 2469 B |
| newlines per frame, 80x24 | 23 (`rows - 1`) | 23 (`rows - 1`) |
| screen scrolls, 80x24 pty | 0 | 0 |
| frame build + write, 80x24 | 0.19 ms | 0.19 ms |
| frame build + write, 140x40 | 0.67 ms | 0.41 ms |
| system sample (`sys_avg`) | 14.58 ms | 14.46 ms (80x24) |
| CPU over 30 s, 80x24 | 0.77 / 0.80 % | 0.73 / 0.87 % |

CPU is within one 10 ms clock tick between the two binaries: 1.0.1 averaged
0.785 %, 1.0.2 averaged 0.80 %. The frame period, repaint time and write count
are unchanged; 140x40 got faster because the old 16-row legend panel was
replaced by the two fixed footer rows.

## PTY evidence (parsed, not eyeballed)

`script -q -c "stty rows 24 cols 80; exec vc4top --interval 250"` captured
through a real pty and replayed through a VT parser that tracks the alternate
screen, wrap mode, newlines and scrolls:

```text
80x24  frames=7   homes=7   scrolls=0  newlines_per_frame=[23]
       alt_enter=1 alt_exit=1 hide=1 show=1 wrap_off=8 wrap_on=8
       legend keys : 'q quit  a page  g regs  j/k scroll  p pause  +/- int  h help  [1-21/48]'
       legend units: 'units  MHz °C MiB % cnt/s   ok warn err idle'
       memory bars : ARM 18.3% 166/905 MiB   GPU/CMA 2.0% 5/256 MiB
       utilization : 7 lines, one percentage each (Idle ... Stall Vary)

140x40 frames=7   homes=7   scrolls=0  newlines_per_frame=[39]
       alt_enter=1 alt_exit=1 hide=1 show=1 wrap_off=8 wrap_on=8

resize 24x80 -> 30x100 -> 12x60 -> 24x80
       scrolls=0, newlines_per_frame=[23 x4, 29 x4, 11 x4, 23 x3]
       alt_enter=1 alt_exit=1

scroll to the end (j): [28-48/48], last row `errors ERRSTAT ...`
       directly above the two legend rows; scrolls=0 over 42 frames
page 3 (a a): [27-47/47], row 29 L2C misses visible; scrolls=0
help screen (h) at 24x80: 24 rows, scrolls=0, clean ESC[?1049l restore
```

`strace -tt -f -e trace=write` on the main process:

```text
1.0.1: 10 frame writes, 2327 B each, one per frame
1.0.2: 10 frame writes, 2469 B each, one per frame
```

The 1.0.1 repaint discipline is intact: alternate screen, one `ESC[H` per
frame, wrap off, `ESC[K` per row, no newline on the last row, single `write(2)`
per frame, SIGWINCH relayout and restore on `q`/`SIGINT`/`SIGTERM`/`SIGHUP`.
