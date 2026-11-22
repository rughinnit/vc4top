# Utilisation history graph captures

Frames and dumps from the QPU busy history graph, captured on the Raspberry Pi
with the build in this tree (V3D 300 MHz, core 400 MHz).

## Files

| File | What it is |
|---|---|
| `after-160x45-load.txt` | 160x45 desktop frame during a `vc4infer bench modelB` loop: 38 completed one-second samples plus the live sample, mean 37.8 %, peak 100 % |
| `after-160x45-overlay.txt` | same run after `o`: temperature (fixed 20-90 °C) and ARM memory (0-100 %) below the QPU graph, each with its own header, unit and axis |
| `after-160x45-auto.txt` | same run after `f`: auto scale, axes 42-47 °C and 34-41 %, header and caption say `auto` |
| `after-80x24.txt` | 80x24 idle: seven numeric bars, graph header, two-row graph with its axis; the counter table is one scroll down |
| `dump-load-90s.txt` | `--dump-samples 90` over the same loop: 87 samples, 0 gaps, mean 30.7 %, peak 100 % |
| `dump-gap-lease.txt` | `--dump-samples 8` while another process held the vc4mon counter lease: 8 one-second samples with the QPU column empty (gaps) while temperature and ARM memory were still recorded |

## How the frames were captured

The frames are the last complete repaint of a live TUI under a pty, without a
terminal emulator. The TUI paints exactly `rows` lines per frame with one
`ESC[H` home and `rows - 1` CRLFs, so the capture keeps the block after the last
`ESC[H`, splits it on CRLF and strips the CSI sequences. The result is plain
UTF-8 with no cursor controls, and every line is exactly the terminal width
(160 or 80 characters, including the braille cells as one character each).

Load: `vc4infer bench /usr/share/vc4infer/models/modelB.onnx --reps 100` in a
loop (11 clients, `qpu_duty` 27.1-28.8 % per client), run under
`tools/thermal-guard.sh --max 75`. Peak 46.2 °C, no thermal throttle, so every
sample was taken at the stock clocks.

Samples: 500 ms sampling interval, one graph sample per second of measured
counter time. The live sample is the rightmost graph column, so the header's
`now` value and the graph's right edge are the same sample.
