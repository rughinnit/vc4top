# VC4TOP

Realtime terminal monitor for the Broadcom **VideoCore IV** GPU/QPU (Raspberry Pi 1, 2, 3, Zero, Zero 2 W and CM1/CM3). It is a btop-style TUI with the whole V3D block exposed, not just the seven 3D-pipeline percentages of the original vc4top.

![status](https://img.shields.io/badge/GPU-VideoCore%20IV-blue) ![license](https://img.shields.io/badge/license-GPL--3.0-green)

## What it shows

- **System / power**: arm, core, V3D, H264, ISP, pixel, HDMI, VEC and eMMC clocks; core/SDRAM voltages; temperature; decoded throttle bits; GPU/ARM memory split; CMA; V3D runtime-PM state; undervoltage alarm; HVS underrun; DRM name; HDMI status/mode; buffer/client counts.
- **Memory utilization**: the raw firmware split and CMA free stay visible, plus two live bars with percentage and used/total MiB — `ARM` (`MemTotal - MemAvailable` from `/proc/meminfo`) and `GPU/CMA` (`CmaTotal - CmaFree`, the pool carved out for the GPU).
- **QPU / scheduler / VPM**: IDENT0/1/2, revision, slices, QPUs/slice, TMUs/slice, total QPUs/TMUs, semaphores, VPM size, HDR support, SRQCS queue/requests/completed/qerr, VPMBASE reserved bytes, VPACNTL, SQRSV0/1, SQCNTL, SRQPC/SRQUA/SRQUL, INTCTL/INTENA/INTDIS, PCTRE/PCTRC, L2CACTL/SLCACTL and decoded ERRSTAT.
- **Utilization**: idle, vertex, fragment, valid and TMU/scoreboard/varying stall metrics from counter sources 13-19, one line each: label + bar + a single percentage.
- **Counters**: all **30** V3D counter sources, on three pages (the hardware has 16 counters, so pages are used):
  - page 1: QPU/GPU primary, sources 13-28
  - page 2: FEP/TLB/PTB/PSE, sources 0-15
  - page 3: QPU/TMU/VPM/L2C, sources 16-29
- **Registers**: the complete documented V3D register map (IDENT, control-list, binner, scheduler, VPM, performance counters, debug/error).

## Build

No GTK, no X11, no ncurses. Only a C++14 compiler, CMake and pthreads:

```bash
cmake -S . -B build
cmake --build build -j"$(nproc)"
sudo ./build/vc4top
```

Install:

```bash
sudo cmake --install build
vc4top
```

### Debian package (arm64)

A prebuilt package and a signed apt repository are available:

```bash
curl -fLO https://rughinnit.github.io/vc4top/pool/main/v/vc4top/vc4top_1.0.2_arm64.deb
sudo apt install ./vc4top_1.0.2_arm64.deb
```

See [docs/DEBIAN.md](docs/DEBIAN.md) for the repository key, apt source line,
run instructions and how to remove an older manual `cmake --install` copy.

## Usage

`vc4top` with no arguments launches the realtime TUI (like btop). It must run as root because it maps `/dev/mem`.

```bash
sudo vc4top
sudo vc4top --interval 250
sudo vc4top --no-color
```

The layout adapts to the terminal:

- **136+ columns**: three columns (system/power/memory + utilization, QPU/queues/VPM, counters + flags/notes), so status, memory, queues and counters are visible together at 140x40.
- **100-135 columns**: two columns (system/QPU/utilization on the left, counters/flags on the right).
- **Under 100 columns**: one compact column: system status + memory bars, all seven utilization bars, the counter table, then QPU/queues/VPM. `j`/`k` scrolls the view and the footer shows the visible line range; long values are truncated with `>` rather than overlapping.
- At 80x24 the first screen shows system status with both memory bars, every utilization metric, and the counter table header with its first row; the QPU/queue/VPM section and the rest of the table are one scroll down. The always-on two-row legend never overlaps it.

Every number carries a unit: hex for registers, `MHz` for clocks (raw `Hz` is kept in `--once`), `°C` for temperature, `MiB` for memory, `counts`/`cnt/s` for counters and `%` for utilization. Colour is automatic: `NO_COLOR=1`, `--no-color`, or piping the output selects plain text. A compact key/unit/colour legend is always visible in the footer; `h` opens the full help screen with the flags and the memory/utilization definitions.

### Keys

| Key | Action |
|---|---|
| `q` | quit |
| `a` / `[` / `]` | cycle counter pages |
| `g` | toggle the full V3D register dump |
| `j` / `k` | scroll the register dump, or the live view when narrow |
| `p` | pause/resume sampling |
| `+` / `-` | increase/decrease the sample interval |
| `h` | help |

### Non-interactive modes

```bash
sudo vc4top --once            # wide snapshot (system + QPU + primary counters)
sudo vc4top --all-counters    # all 30 counter sources, two passes
sudo vc4top --regs            # full register dump
sudo vc4top --poll 50         # CSV of SRQCS/SRQPC/SRQUA/VPMBASE/ERRSTAT at 50 ms
sudo vc4top --frame           # render one TUI frame and exit
```

If stdout is not a TTY, `vc4top` prints the snapshot instead of starting the TUI.

## Requirements

- Raspberry Pi with VideoCore IV: Pi 1, 2, 3, Zero, Zero 2 W, CM1/CM3. Not Pi 4/5 (VideoCore VI/VII).
- Root for `/dev/mem`.
- A powered V3D block. The `vc4cl-v3d-power.service` keeps `3fc00000.v3d/power/control=on`. Otherwise write `on` there yourself.
- The V3D block must not be suspended (`0xdeadbeef` reads).

## Limitations

- The 16 V3D hardware counters are **3D-pipeline oriented**. Raw QPU compute launched through VC4CL's register-poking backend may leave the QPU idle/valid counters at zero even while a kernel runs. There is no per-QPU utilization register on VideoCore IV. Use `--poll` for scheduler state and `VC4CL_DEBUG=perf,execution` for per-kernel timing.
- `V3D_ERRSTAT=0x1000` is the normal VCD-idle bit. `0x1030` means **VCD-idle plus VPM write-range/read-range errors** (bits 4/5) and is not normal; it usually points at a missing/incorrect VPMBASE reservation. The TUI and `--once` now decode the ERRSTAT bits instead of treating 0x1030 as benign.
- `VPMBASE` (Table 69) is the amount of VPM reserved for user programs in 256-byte units, **not** a base address. `0` means no VPM reserved. Raw QPU compute needs a correct non-zero reservation; the `vc4` KMS driver sets it to 0 because it has no user-QPU interface.
- Only one process should own the V3D block; a GL/compositor client will conflict with VC4CL and with direct register monitoring.
- The GPU has no MMU between QPU and RAM; VC4CL compute is root-only by design.

## Changelog

### 1.0.2 (2026-11-19)

- Always-on compact legend: two fixed footer rows show the keys (with the
  visible line range) and the units/colour key at every terminal size. `h`
  still opens the full help/flags screen, which is now also clamped to the
  terminal height so it cannot scroll.
- Real memory utilization: `ARM` (`MemTotal - MemAvailable`) and `GPU/CMA`
  (`CmaTotal - CmaFree`) bars with percentage and used/total MiB next to the
  unchanged raw firmware split and CMA free values.
- Utilization is represented once per metric: label + bar + one percentage per
  line, no duplicated percentage summary. All seven values (idle, vertex,
  fragment, valid, TMU/scoreboard/varying stall) are kept.
- Narrow layout order: status/memory, utilization, counters, then QPU/queues/
  VPM, so the 80x24 first screen shows the memory bars, all utilization lines
  and the counter table header without the legend pushing anything away.
- `--once` gains exactly two fields (`arm mem used:`, `gpu cma used:`); the 43
  baseline fields, 30 counter rows and 78 register rows are unchanged in
  name/order. The new `tools/field_parity.py` checks this and reports PASS.
- No repaint regression: still one `write(2)` per frame, one `ESC[H`, no
  newline on the last row, 0 scrolls at 80x24, 0.20 ms frame build
  (0.41 ms at 140x40), ~0.85 % CPU — equal to 1.0.1.

### 1.0.1 (2026-11-19)

- Fix live-view flicker and scrolling: paint the full frame in one buffered
  `write(2)` on the alternate screen, home with a single `ESC[H`, clear each row
  with `ESC[K`, disable autowrap and never print a newline on the last row.
  The layout is clamped to the terminal height, so the counter table stays
  behind `j`/`k` scrolling instead of scrolling the screen.
- Restore the screen, cursor and line wrap on every exit path (`q`, `SIGINT`,
  `SIGTERM`, `SIGHUP`), and recompute the layout on `SIGWINCH`.
- Cut per-frame work: mailbox clocks/voltages refresh round-robin, temperature
  and throttle every 500 ms, no shell forks for `date`/CMA. Frame period
  0.755 s -> 0.568 s at the default 500 ms interval; CPU 1.60 % -> 0.70 %;
  one `write(2)` per frame. See
  [docs/ui/PERFORMANCE.md](docs/ui/PERFORMANCE.md).

### 1.0.0

- First packaged release: widened V3D telemetry, paged counters, Debian
  package and signed apt repository.

## Credits

- Original `vc4top`: Jonas Pfeil (GTK version).
- Terminal fork and ASCII-bar idea: FASTSHIFT (`terminal_version`).
- This version: widened to the full V3D register/counter set, btop-style two-column TUI, paged counters and headless modes.

## License

GPL-3.0. See `LICENSE.txt`.
