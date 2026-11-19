# Field parity evidence

Baseline: `build/vc4top` from commit `4e72634` ("fix: correct V3D IDENT1, SRQCS,
VPMBASE and ERRSTAT decodes"), saved as `/tmp/vc4top_baseline` before the redesign.
Redesigned: `vc4top.cpp` in this commit.

Both snapshots were taken on the same boot with stock clocks (core 400 MHz, V3D
300 MHz) and the V3D block live. The checker normalises live values and compares:

- the ordered list of `key:` fields (must be identical in count and order);
- the numeric set of every stable field (hex and decimal);
- the first Hz value of every clock (MHz is derived, raw Hz is retained);
- counter IDs and full descriptions, in order;
- register names and order in `--regs`;
- every all-counter row in `--all-counters`.

Volatile fields (`time`, `temp`, clocks, `cma`, `hvs underrun`, `SRQCS`,
`SRQPC/SRQUA`, `PCTRE/PCTRC`, counter values) are read live and are expected to
change between runs. Everything else is bit-identical. The `clients` field was
never part of `--once`; it remains in the live TUI exactly as before.

## --once field checklist

| # | field | baseline value | redesigned value | parity |
|---|-------|----------------|------------------|--------|
| 1 | `time:` | 2026-11-19 09:35:17 WIB | 2026-11-19 09:35:19 WIB | ok (volatile) |
| 2 | `temp:` | 56.4'C | 56.4 °C | ok (volatile) |
| 3 | `throttled:` | 0x0 (none) | 0x0 (none) | ok (numbers match) |
| 4 | `core volts:` | 1.3125V | 1.3125V | ok (numbers match) |
| 5 | `sdram_c volts:` | 1.2000V | 1.2000V | ok (numbers match) |
| 6 | `arm clock:` | 1200000000 Hz | 1200000000 Hz (1200.000 MHz) | ok (live Hz within 2%) |
| 7 | `core clock:` | 400000000 Hz | 400000000 Hz (400.000 MHz) | ok (live Hz within 2%) |
| 8 | `v3d clock:` | 300000000 Hz | 300000000 Hz (300.000 MHz) | ok (live Hz within 2%) |
| 9 | `h264 clock:` | 0 Hz | 0 Hz (0.000 MHz) | ok (live Hz within 2%) |
| 10 | `isp clock:` | 0 Hz | 0 Hz (0.000 MHz) | ok (live Hz within 2%) |
| 11 | `pixel clock:` | 337000 Hz | 338000 Hz (0.338 MHz) | ok (live Hz within 2%) |
| 12 | `hdmi clock:` | 163683000 Hz | 163683000 Hz (163.683 MHz) | ok (live Hz within 2%) |
| 13 | `vec clock:` | 108000000 Hz | 108000000 Hz (108.000 MHz) | ok (live Hz within 2%) |
| 14 | `emmc clock:` | 200000000 Hz | 200000000 Hz (200.000 MHz) | ok (live Hz within 2%) |
| 15 | `gpu mem:` | 76M | 76 MiB | ok (numbers match) |
| 16 | `arm mem:` | 948M | 948 MiB | ok (numbers match) |
| 17 | `cma:` | CMA 256M/250M free | CMA 256 MiB / 250 MiB free | ok (volatile) |
| 18 | `v3d power:` | active / on | active / on | ok (text match) |
| 19 | `undervoltage:` | 0 | 0 (ok) | ok (numbers match) |
| 20 | `hvs underrun:` | 0 | 0 (events) | ok (volatile) |
| 21 | `drm:` | vc4 dev=soc:gpu unique=soc:gpu | vc4 dev=soc:gpu unique=soc:gpu | ok (text match) |
| 22 | `hdmi:` | disconnected | disconnected | ok (text match) |
| 23 | `bo_stats:` |  |  | ok (text match) |
| 24 | `IDENT0:` | 0x02443356 | 0x02443356 | ok (numbers match) |
| 25 | `IDENT1:` | 0xc1102431 | 0xc1102431 | ok (numbers match) |
| 26 | `IDENT2:` | 0x00000121 | 0x00000121 | ok (numbers match) |
| 27 | `revision:` | 1 | 1 | ok (numbers match) |
| 28 | `slices:` | 3 | 3 slices | ok (numbers match) |
| 29 | `QPUs/slice:` | 4 (total 12) | 4 QPUs/slice (total 12 QPUs) | ok (numbers match) |
| 30 | `TMUs/slice:` | 2 (total 6) | 2 TMUs/slice (total 6 TMUs) | ok (numbers match) |
| 31 | `semaphores:` | 16 | 16 semaphores | ok (numbers match) |
| 32 | `VPM size:` | 12K | 12 KiB | ok (numbers match) |
| 33 | `HDR support:` | 1 | 1 (yes) | ok (numbers match) |
| 34 | `SRQCS:` | 0x00010100 queue=0 requests=1 completed=1 qerr=0 | 0x00010100 queue=0 requests=1 completed=1 qerr=0 | ok (volatile) |
| 35 | `VPMBASE:` | 0x00000010 reserved=4096 bytes of 12K | 0x00000010 reserved=4096 bytes of 12 KiB | ok (numbers match) |
| 36 | `VPACNTL:` | 0x00000000 | 0x00000000 | ok (numbers match) |
| 37 | `SQRSV0/1:` | 0x00000000 / 0x00000000 | 0x00000000 / 0x00000000 | ok (numbers match) |
| 38 | `SQCNTL:` | 0x00000000 | 0x00000000 | ok (numbers match) |
| 39 | `SRQPC/SRQUA:` | 0xde63d000 / 0x379a7c00 (len 0x00000000) | 0xde63d000 / 0x379a7c00 (len 0x00000000) | ok (volatile) |
| 40 | `INTCTL/ENA/DIS:` | 0x00000004 / 0x00000003 / 0x00000003 | 0x00000004 / 0x00000003 / 0x00000003 | ok (numbers match) |
| 41 | `PCTRE/PCTRC:` | 0x80000003 / 0x00000000 | 0x80000000 / 0x00000000 | ok (volatile) |
| 42 | `L2CACTL/SLCACTL:` | 0x00000001 / 0x00000000 | 0x00000001 / 0x00000000 | ok (numbers match) |
| 43 | `ERRSTAT:` | 0x00001000 VCD-idle | 0x00001000 VCD-idle | ok (numbers match) |

Field count: **43 -> 43** (exact match)
Primary counter rows: 16 -> 16
Counter IDs and full names: exact match

## Register / counter mode parity

- `--regs` register rows: 78 -> 78
- register name/order match: True
- `--all-counters` rows: 30 -> 30
- all-counter id/name/order match: True

RESULT: PASS
