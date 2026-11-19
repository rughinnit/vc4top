# Live-view repaint: flicker and refresh-rate fix (1.0.1)

## Symptom

The live TUI flickered: a frame was printed line by line and the next frame was
drawn over it, visible for a moment. The view also refreshed slowly, and every
repaint was followed by a visible one-line jump.

## Root cause

`render_tui()` appended a newline after **every** row, including the last row of
the frame. The live view fills the terminal exactly (`rows` lines), so that
final newline is printed while the cursor is on the bottom row, which scrolls
the whole alternate screen by one line on every tick. The frame was also emitted
through `fwrite()`; with the default stdio buffer an 80x24 frame became two
1024-byte `write(2)` calls instead of one. Finally, every frame forked a shell
and `vcgencmd` 21 times (`date`, `awk`, temperature, throttle, 11 clocks,
2 voltages, 2 memory splits), which cost roughly 250 ms per frame on top of the
sampling interval.

## Fix (1.0.1)

- Start in the alternate screen with the cursor hidden and autowrap disabled
  (`ESC[?1049h`, `ESC[?25l`, `ESC[?7l`, `ESC[2J`).
- Build the entire frame in a `std::string` and emit it in **one** `write(2)`:
  one `ESC[H` home, each row clipped/padded to the real terminal width with
  `ESC[K`, `CR LF` only *between* rows, and **no newline after the last row**.
- Never emit more rows than the terminal has (`ws_row`); the counter table stays
  behind `j`/`k` scrolling.
- Recompute the layout on `SIGWINCH` and repaint immediately.
- Restore line wrap, cursor and the primary screen on every exit path: `q`,
  `SIGINT`, `SIGTERM` and `SIGHUP`. The signal handler writes the restore
  sequence directly with async-signal-safe `write(2)`.
- Reduce per-frame work: the mailbox clocks/voltages are refreshed round-robin
  (one slot per frame), temperature and throttle every 500 ms, and the firmware
  memory split every 15 s. `date` and `awk` forks were replaced by in-process
  `strftime()` and `/proc/meminfo` parsing.
- Non-interactive modes (`--once`, `--all-counters`, `--regs`, `--poll`,
  `--frame` when piped) are unchanged and contain no escape sequences.

`VC4TOP_STATS=1` makes the live view print average timings and write counts to
stderr on exit; it is a measurement aid only.

## Measurements

Raspberry Pi 3, stock clocks, 80x24 pty (`script`), default 500 ms interval,
one instance at a time:

| metric | 1.0.0 | 1.0.1 |
|---|---|---|
| frame period | 0.755 s (min 0.743, max 0.841) | 0.568 s (min 0.539, max 0.580) |
| `write(2)` calls per frame | 2 (2 x 1024 B) | 1 (2304 B) |
| newlines per frame | 24 (bottom-row newline) | 23 (`rows - 1`) |
| screen scrolls (pty replay) | 11 of 11 frames | 0 of 13 frames |
| CPU over 10 s (parent + reaped children) | 1.60 % | 0.70 % |
| frame build + write, 80x24 | per-line stdio | 0.18-0.30 ms |
| frame build + write, 140x40 | per-line stdio | 0.50-0.54 ms |
| frame build + write, 8x40 | per-line stdio | 0.06 ms |
| system sample (3 `vcgencmd` + sysfs) | ~250 ms | ~23 ms |

The 500 ms sampling window dominates the frame period by design; the repaint
itself is now 0.04 % of it at 80x24.

## Verification evidence

PTY replay (`script -q -c "stty rows 24 cols 80; ..." /dev/null`) analysed with a
small VT parser that tracks the alternate screen, autowrap, newlines and scrolls:

```text
before: frames=11 homes=11 scrolls=11 newlines_per_frame=[24 x 11]
after : frames=13 homes=13 scrolls=0  newlines_per_frame=[23 x 13]
        alt_enter=1 alt_exit=1 hide=1 show=1 wrap_off=13 wrap_on=13
```

`strace -tt -f -e trace=write` on the main process:

```text
before: 22 x write(1, ..., 1024)   -> 2 frame writes per frame
after : 13 x write(1, ..., 2304)   -> 1 frame write per frame, all identical
```

Resize (`SIGWINCH`) at 24x80 -> 30x100 -> 12x60:

```text
newlines_per_frame = [23, 23, 23, 29, 11, 11, 11, 11, 11]
scrolls = 0, homes = 9, alt_enter = 1, alt_exit = 1
```

An 8-row terminal emits 7 newlines per frame and scrolls 0 times; a 140x40
terminal emits 39 newlines per frame and scrolls 0 times. `SIGINT`, `SIGTERM`
and `q` all produce `ESC[?7h ESC[?25h ESC[?1049l`.

Field parity is unchanged: `--once` has the same **43** keyed fields in the same
order, `--all-counters` the same 30 sources, and `--regs` the same 78 register
rows in the same order (see `FIELD-PARITY.md`).
