# vc4top UI evidence (before / after)

Captured on the Raspberry Pi with the stock clocks unchanged:

- V3D clock: 300 MHz
- core clock: 400 MHz
- V3D runtime state: `active` / `active` control (`STATE LIVE`)
- all runs serialized with `flock -w 900 /tmp/vc4-gpu.lock`

## Files

| File | What it is |
|---|---|
| `before-once.txt` / `after-once.txt` | full `--once` snapshot |
| `before-regs.txt` / `after-regs.txt` | `--once --regs` (includes all 78 registers) |
| `before-all-counters.txt` / `after-all-counters.txt` | `--all-counters` (30 sources) |
| `before-frame-80x24.txt` / `after-frame-80x24.txt` | one live frame at 80x24 |
| `before-frame-140x40.txt` / `after-frame-140x40.txt` | one live frame at 140x40 |
| `FIELD-PARITY.md` | field-by-field checklist and parity result |
| `PERFORMANCE.md` | 1.0.1 repaint measurement: frame period, write counts, scrolls, CPU |

The frame dumps are plain text with ANSI colour and cursor controls stripped, so
they are reproducible and readable in a diff.

## Layout rationale

1. **Status first, then queues, then counters.** The live view always opens with
   time/temperature/throttle/clocks/power, then QPU identity, scheduler queues and
   VPM, then counters. The full register dump adds group headings in the same
   order: identity, control-list, binner, scheduler, VPM, performance counters,
   debug/errors.
2. **Units on every number.** Clocks are shown in MHz (the raw Hz value is kept in
   `--once`), temperature in °C, registers in `0x` hex, memory in MiB, counter
   values as counts and rates as counts/s, utilization as percent. Queue counters
   are named (`queue`, `req`, `done`, `qerr`) instead of being anonymous digits.
3. **Three width tiers, no overlap.** At 136+ columns the screen is split into
   system+utilization / QPU / counters+legend, which fits a 140x40 terminal
   without scrolling. At 100-135 columns it is two columns. Under 100 columns it
   is a single compact column. Every line is measured with ANSI- and UTF-8-aware
   width and is either padded or clipped with a `>` marker; nothing spills into
   the next column.
4. **80x24.** The first screen shows the complete system status block, the QPU /
   queue / VPM block and the utilization summary. `j`/`k` scrolls the view and the
   footer shows `[first-last/total]`; the full counter table is one or two
   screens down. The counter page keys still cycle all 30 sources.
5. **Colour degrades gracefully.** `g_color` is only enabled for a TTY, and is
   disabled by `--no-color` or by the presence of `NO_COLOR` (even empty).
   Piped output contains no ANSI escapes at all. Colour encodes severity:
   green = OK, yellow = warning/sticky throttle, red = current throttle or error,
   dim = idle/zero. Plain text keeps the same labels, units and alignment.
6. **Legend and help.** The footer carries the key legend; the wide view has a
   `LEGEND / HELP` panel with the colour key, all keys, all flags and the
   counter-rate explanation. `h` opens the full help screen, which also lists the
   flags and units.

## Before / after at 80x24

Baseline: the QPU panel is cut off mid-register and there is no utilization or
counter content at all.

```
VC4TOP  2026-11-19 09:35:31 WIB  58.5'C  thr 0x0 (none)  V3D ON
+- SYSTEM ---------------------------------------------------------------------+
| time     2026-11-19 09:35:31 WIB                                             |
| temp     58.5'C   throttle 0x0 (none)                                        |
| clocks   arm 1200000000  core 400000000  v3d 300000000                       |
...
| srqpc    0xde63d000  srqua 0x379a7c00                                        |
q quit  a/[ ] counters  g registers  p pause  +/- interval  h help
```

Redesigned: all system, QPU and utilization fields fit, with explicit units and
a scroll range for the counters below.

```
VC4TOP  2026-11-19 09:35:38 WIB  V3D ON  58.0 °C  thr 0x0 (none)  500ms
 status   2026-11-19 09:35:38 WIB  temp 58.0 °C  thr 0x0 (none)
 clocks   arm 1200.000 MHz  core 400.000 MHz  v3d 300.000 MHz  h264 0.000 MHz
          isp 0.000 MHz  pixel 0.337 MHz  hdmi 163.682 MHz  vec 108.000 MHz
...
 errors   ERRSTAT 0x00001000  VCD-idle
UTILIZATION --------------------------------------------------------------------
 Idle 100.0%  Vertex 0.0%  Fragment 0.0%  Valid 0.0%  Stall TMU 0.0%
 Stall SB 0.0%  Stall Vary 0.0%
q quit  a page  g regs  j/k scroll  p pause  +/- int  h help  [1-22/45]
```

## Before / after at 140x40

Baseline uses two columns and truncates counter descriptions; the redesigned
view uses three columns and keeps every register, queue and counter name intact:

```
+- SYSTEM / POWER -------------------------------+ +- QPU / QUEUES / VPM -------------------+ +- COUNTERS ---------------------------------+
| status   2026-11-19 09:35:51 WIB  temp 56.9 °C | | ident    rev 1  3 slices x 4 QPUs = 12 | | page 1/3  QPU/GPU primary (13-28)  counts  |
...
| 13  QPU idle cycles   1800322416 3600644832.0 |
```
