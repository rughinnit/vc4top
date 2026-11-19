# VC4TOP

Realtime terminal monitor for the Broadcom **VideoCore IV** GPU/QPU (Raspberry Pi 1, 2, 3, Zero, Zero 2 W and CM1/CM3). It is a btop-style TUI with the whole V3D block exposed, not just the seven 3D-pipeline percentages of the original vc4top.

Register reads and counter sampling work without root through `vc4mon`, a
small out-of-tree kernel module (source in [`kernel/`](kernel/)). Root is only
needed to remap the counter sources across all 30 pages; the unprivileged path
uses the module's fixed 16-counter map.


![status](https://img.shields.io/badge/GPU-VideoCore%20IV-blue) ![license](https://img.shields.io/badge/license-GPL--3.0-green)

## What it shows

- **System / power**: arm, core, V3D, H264, ISP, pixel, HDMI, VEC and eMMC clocks; core/SDRAM voltages; temperature; decoded throttle bits; GPU/ARM memory split; CMA; V3D runtime-PM state; undervoltage alarm; HVS underrun; DRM name; HDMI status/mode; buffer/client counts.
- **Memory utilization**: the raw firmware split and CMA free stay visible, plus two live bars with percentage and used/total MiB — `ARM` (`MemTotal - MemAvailable` from `/proc/meminfo`) and `GPU/CMA` (`CmaTotal - CmaFree`, the pool carved out for the GPU).
- **QPU / scheduler / VPM**: IDENT0/1/2, revision, slices, QPUs/slice, TMUs/slice, total QPUs/TMUs, semaphores, VPM size, HDR support, SRQCS queue/requests/completed/qerr, VPMBASE reserved bytes, VPACNTL, SQRSV0/1, SQCNTL, SRQPC/SRQUA/SRQUL, INTCTL/INTENA/INTDIS, PCTRE/PCTRC, L2CACTL/SLCACTL and decoded ERRSTAT.
- **Utilization**: idle, vertex, fragment, valid and TMU/scoreboard/varying stall metrics from counter sources 13-19, one line each: label + bar + a single percentage.
- **Counters**: root samples all **30** V3D counter sources, on three pages
  (the hardware has 16 counters, so pages are used):
  - page 1: QPU/GPU primary, sources 13-28
  - page 2: FEP/TLB/PTB/PSE, sources 0-15
  - page 3: QPU/TMU/VPM/L2C, sources 16-29

  Unprivileged runs sample the module's fixed 16-counter map (13-28 on a fresh
  load) through `perf_enable`/`perf_clear`; source remapping is root-only.
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

### Register and counter module (vc4mon)

`vc4top` reads the V3D register block through `/dev/mem` when run as root. To
read the same registers and sample counters unprivileged, install the packaged
DKMS module. It does **not** bind, unbind or claim `3fc00000.v3d`; it maps the
existing MMIO resource a second time and exposes three `0444` read files plus
two `0200` write-only counter files:

```bash
sudo apt install vc4top vc4top-dkms          # from the vc4top apt repository
dkms status                                  # vc4top/1.0.5, <kernel>: installed
ls -l /sys/kernel/vc4mon/                    # ident perf regs 0444; perf_enable/perf_clear
```

`vc4top-dkms` installs the source in `/usr/src/vc4top-1.0.5` and builds it for
the running kernel in `postinst`; `/etc/modules-load.d/vc4mon.conf` loads it at
boot. Kernel upgrades rebuild it automatically (`AUTOINSTALL="yes"`) as long as
the matching `linux-headers-*` package is installed. If the headers are
missing the install still succeeds: `vc4top` keeps working as root and the
package prints the `apt install linux-headers-$(uname -r)` command to run.

The package's udev helper grants the local `vc4` group write-only access to
`perf_enable`/`perf_clear` (`0620 root:vc4`); add users who should sample
counters to that group. Unprivileged source remapping is deliberately not
exposed, so those runs show the module's fixed 13-28 map.

Building from a git checkout instead (no DKMS):

```bash
sudo apt install linux-headers-$(uname -r)   # if not already installed
make -C kernel
sudo make -C kernel install                   # /lib/modules/$(uname -r)/extra
sudo modprobe vc4mon
echo vc4mon | sudo tee /etc/modules-load.d/vc4mon.conf
sudo chgrp vc4 /sys/kernel/vc4mon/perf_enable /sys/kernel/vc4mon/perf_clear
sudo chmod 0620 /sys/kernel/vc4mon/perf_enable /sys/kernel/vc4mon/perf_clear
```

Inspect or unload:

```bash
cat /sys/kernel/vc4mon/ident      # IDENT0/1/2
cat /sys/kernel/vc4mon/regs       # 78 registers, "<offset> <value>" lines
cat /sys/kernel/vc4mon/perf       # PCTR0..15 / PCTRS0..15, observation only
sudo modprobe -r vc4mon
```

The writable surface is only `perf_enable` (PCTRE, mask `0x8000ffff`) and
`perf_clear` (literal 1, PCTRC). No other register, reset, clock, power,
queue, VPM or L2 control is writable. PCTRE/PCTRC only enable/clear counter
latches: they cannot DMA, write memory, affect rendering or change power.
See [`kernel/README.md`](kernel/README.md) and
[docs/ui/REGISTER-ACCESS.md](docs/ui/REGISTER-ACCESS.md).

### Debian package (arm64)

A prebuilt package, the `vc4top-dkms` module package and a signed apt
repository are available:

```bash
curl -fLO https://rughinnit.github.io/vc4top/pool/main/v/vc4top/vc4top_1.0.5_arm64.deb
curl -fLO https://rughinnit.github.io/vc4top/pool/main/v/vc4top/vc4top-dkms_1.0.5_all.deb
sudo apt install ./vc4top_1.0.5_arm64.deb ./vc4top-dkms_1.0.5_all.deb
```

See [docs/DEBIAN.md](docs/DEBIAN.md) for the repository key, apt source line,
run instructions and how to remove an older manual `cmake --install` copy.

## Usage

`vc4top` with no arguments launches the realtime TUI (like btop). As root it
maps `/dev/mem`; as a normal user it reads the same registers through
`/sys/kernel/vc4mon/` and samples counters through `perf_enable`/`perf_clear`
when the `vc4mon` module is loaded, and never opens `/dev/mem`.

```bash
sudo vc4top
sudo vc4top --interval 250
sudo vc4top --no-color
vc4top --once            # unprivileged, if vc4mon is loaded
```

The layout adapts to the terminal:

- **160+ columns (wide / horizontal desktop)**: three content-sized panels across the top — system/power/memory, QPU/queues/VPM and utilization — over a **full-width counter table** split into two side-by-side halves, then a full-width flags/units strip. All sampled counter sources stay visible at once, the utilization bars widen to fill their panel, and the top row and counter table share the spare height, so a short desktop terminal (160x45, 200x50) fills its rows instead of leaving a blank band under fixed-percentage columns. Captures: [`docs/ui/wide/`](docs/ui/wide/).
- **136-159 columns**: three columns (system/power/memory + utilization, QPU/queues/VPM, counters + flags/notes), so status, memory, queues and counters are visible together at 140x40.
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
sudo vc4top --all-counters    # root: all 30 sources; unprivileged: fixed 16
sudo vc4top --regs            # full register dump
sudo vc4top --poll 50         # CSV of SRQCS/SRQPC/SRQUA/VPMBASE/ERRSTAT at 50 ms
sudo vc4top --frame           # render one TUI frame and exit
```

If stdout is not a TTY, `vc4top` prints the snapshot instead of starting the TUI.

Unprivileged runs print the same QPU/register fields as the root path and now
sample the counters through vc4mon's `perf_enable`/`perf_clear`, with no
`/dev/mem`. Source remapping remains root-only, so the unprivileged counter
table shows the module's fixed 16-counter map.

## Requirements

- Raspberry Pi with VideoCore IV: Pi 1, 2, 3, Zero, Zero 2 W, CM1/CM3. Not Pi 4/5 (VideoCore VI/VII).
- Root for `/dev/mem` and for remapping all 30 counter sources; the `vc4mon`
  module and membership of group `vc4` for unprivileged registers and counters.
- A powered V3D block. The `vc4cl-v3d-power.service` keeps `3fc00000.v3d/power/control=on`. Otherwise write `on` there yourself.
- The V3D block must not be suspended (`0xdeadbeef` reads).

## Limitations

- The 16 V3D hardware counters are **3D-pipeline oriented**. Raw QPU compute launched through VC4CL's register-poking backend may leave the QPU idle/valid counters at zero even while a kernel runs. There is no per-QPU utilization register on VideoCore IV. Use `--poll` for scheduler state and `VC4CL_DEBUG=perf,execution` for per-kernel timing.
- `V3D_ERRSTAT=0x1000` is the normal VCD-idle bit. `0x1030` means **VCD-idle plus VPM write-range/read-range errors** (bits 4/5) and is not normal; it usually points at a missing/incorrect VPMBASE reservation. The TUI and `--once` now decode the ERRSTAT bits instead of treating 0x1030 as benign.
- `VPMBASE` (Table 69) is the amount of VPM reserved for user programs in 256-byte units, **not** a base address. `0` means no VPM reserved. Raw QPU compute needs a correct non-zero reservation; the `vc4` KMS driver sets it to 0 because it has no user-QPU interface.
- Only one process should own the V3D block; a GL/compositor client will conflict with VC4CL and with direct register monitoring.
- The GPU has no MMU between QPU and RAM; VC4CL compute is root-only by design.

## Changelog

### 1.0.5 (2026-11-19)

- New wide/horizontal desktop layout at **160+ columns**: system/power/memory,
  QPU/queues/VPM and utilization are content-sized panels on the top row, with
  a full-width counter table (two side-by-side halves, all sampled sources
  visible) and a full-width flags strip below. Spare rows are shared by the top
  row and the counter table instead of being left blank, so 160x45, 192x48,
  200x50, 240x60 and 256x70 fill the screen with balanced panels.
- Nothing was removed: all system/QPU/utilization lines, every counter source
  and all flag/definition notes remain in the wide view. The 136-159 column
  three-column layout, the 100-135 two-column layout and the narrow scrolling
  layout are unchanged, and 80x24/120x40/140x40 renders match 1.0.4.
- Verification: 0 pty scrolls and 0 clipped lines at 80x24, 120x40, 140x40,
  160x45, 192x48, 200x50, 240x60 and 256x70; one `write(2)` and one `ESC[H`
  per frame; 44..69 newlines (`rows - 1`); field parity PASS (43 + 2 `--once`
  fields, 30 counter rows, 78 register rows). Unprivileged `--once` still shows
  the 16 fixed-map counters through `vc4mon`.
- Before/after frames and a parser check are in
  [`docs/ui/wide/`](docs/ui/wide/). `vc4top` and `vc4top-dkms` are 1.0.5.

### 1.0.4 (2026-11-19)

- Unprivileged performance counters: `vc4mon` now exposes exactly two
  store-only files, `perf_enable` (PCTRE mask `0x8000ffff`, invalid bits
  rejected with `-EINVAL`) and `perf_clear` (literal 1, PCTRC, bounded poll).
  `ident`/`regs`/`perf` stay `0444`; no queue, VPM, L2, reset, clock or power
  register became writable. A pid-keyed exclusive lease returns `-EBUSY` to a
  second sampler and expires after 30 s; unload clears PCTRE.
- The packaged udev helper grants group `vc4` write-only access
  (`0620 root:vc4`). Unprivileged `vc4top` enables, samples, displays and
  clears the counters through sysfs, still never opens `/dev/mem`; the root
  `/dev/mem` path is unchanged.
- Loading the module initialises the PCTRS primary map (13..28) only when all
  PCTRS registers read the reset value zero, so a fresh boot has useful
  unprivileged counters without exposing a selector write.
- No read-path or layout regression: root `--all-counters` still has 30 rows,
  `--regs` 78 rows, 0 pty scrolls at 80x24 and 140x40.
- `vc4top-dkms` and `vc4top` are version 1.0.4.

### 1.0.3 (2026-11-19)

- Unprivileged register access: new read-only `vc4mon` kernel module exposes
  IDENT0/1/2, the 78 documented V3D registers and a PCTR/PCTRS observation
  view under `/sys/kernel/vc4mon/`. The module does not claim or unbind
  `3fc00000.v3d`; it only ioremaps the existing resource read-only. No write
  path exists (no store handler, reset, clock change or unbind).
- `vc4top` uses `/sys/kernel/vc4mon/` when unprivileged and keeps `/dev/mem`
  for root. Unprivileged runs never open `/dev/mem`; the QPU/register values
  match the root path field for field. Counter pages need root because
  sampling writes PCTRE/PCTRC, so they are marked unavailable unprivileged.
- Field parity with 1.0.2 is unchanged: the same 45 `--once` fields in order,
  30 counter rows and 78 register rows. PTY check: 0 scrolls at 80x24, 140x40,
  in the register view and on the help screen.
- Module source ships under `kernel/` in the repository and separately as the
  `vc4top-dkms` package (`/usr/src/vc4top-1.0.3`, DKMS, auto-rebuilt on kernel
  upgrades); a reference copy remains in `/usr/share/doc/vc4top/kernel`.

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
