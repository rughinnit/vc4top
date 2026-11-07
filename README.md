# VC4TOP

Realtime terminal monitor for the Broadcom **VideoCore IV** GPU/QPU (Raspberry Pi 1, 2, 3, Zero, Zero 2 W and CM1/CM3). It is a btop-style TUI with the whole V3D block exposed, not just the seven 3D-pipeline percentages of the original vc4top.

![status](https://img.shields.io/badge/GPU-VideoCore%20IV-blue) ![license](https://img.shields.io/badge/license-GPL--3.0-green)

## What it shows

- **System / power**: arm, core, V3D, H264, ISP, pixel, HDMI, VEC and eMMC clocks; core/SDRAM voltages; temperature; decoded throttle bits; GPU/ARM memory split; CMA; V3D runtime-PM state; undervoltage alarm; HVS underrun; DRM name; HDMI status/mode; buffer/client counts.
- **QPU / scheduler / VPM**: IDENT0/1/2, slices/TMUs/QPUs/semaphores, SRQCS requests/completed/qpu_count, VPMBASE, SQRSV0/1, SQCNTL, SRQPC/SRQUA/SRQUL, INTCTL/INTENA/INTDIS, PCTRE/PCTRC, L2CACTL/SLCACTL, ERRSTAT.
- **Utilization**: idle, vertex, fragment, valid and TMU/scoreboard/varying stall bars computed from counter sources 13-19.
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

## Usage

`vc4top` with no arguments launches the realtime TUI (like btop). It must run as root because it maps `/dev/mem`.

```bash
sudo vc4top
sudo vc4top --interval 250
sudo vc4top --no-color
```

### Keys

| Key | Action |
|---|---|
| `q` | quit |
| `a` / `[` / `]` | cycle counter pages |
| `g` | toggle the full V3D register dump |
| `j` / `k` | scroll the register dump |
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
- `V3D_ERRSTAT=0x1000` / `0x1030` is normal on this board and is not a pass/fail signal.
- Only one process should own the V3D block; a GL/compositor client will conflict with VC4CL and with direct register monitoring.
- The GPU has no MMU between QPU and RAM; VC4CL compute is root-only by design.

## Credits

- Original `vc4top`: Jonas Pfeil (GTK version).
- Terminal fork and ASCII-bar idea: FASTSHIFT (`terminal_version`).
- This version: widened to the full V3D register/counter set, btop-style two-column TUI, paged counters and headless modes.

## License

GPL-3.0. See `LICENSE.txt`.
