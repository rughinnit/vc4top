# 1.0.2 field parity

Baseline: `/tmp/vc4top_101`, the binary installed by the 1.0.1 package.
Candidate: `build/vc4top` from this commit (1.0.2). Same boot, stock clocks
(`vcgencmd measure_clock core` = 400 MHz), V3D state `active`/`on`, runs
serialized with `flock /tmp/vc4-gpu.lock`.

```console
$ sudo python3 tools/field_parity.py --baseline /tmp/vc4top_101 --new build/vc4top
vc4top field parity
  baseline: /tmp/vc4top_101
  new     : build/vc4top

--once keyed fields
  baseline count         43
  candidate count        45
  old fields in order    yes (43/43)
  new fields             arm mem used:, gpu cma used:
  memory additions       2 (arm mem used: + gpu cma used:)

counter / register rows
  all-counter rows       30 -> 30  identical
  register rows          78 -> 78  identical

RESULT: PASS  (43 old fields + 2 new memory fields = 45 fields; 30 counters; 78 registers)
```

## What the additions are

Both new `--once` rows sit in the `memory` subsection, after the unchanged
`gpu mem:`, `arm mem:` and `cma:` rows:

| field | value example | meaning |
|---|---|---|
| `arm mem used:` | `166 / 905 MiB (18.3% used, MemTotal-MemAvailable)` | ARM/system RAM from `/proc/meminfo` |
| `gpu cma used:` | `9 / 256 MiB (3.5% used, CmaTotal-CmaFree)` | GPU/CMA pool |

Percentage and MiB are derived from the same rounded MiB numbers, so the bar,
the percentage and `used/total MiB` always agree (for example `166/905 MiB` is
`18.3%`, and `9/256 MiB` is `3.5%`).

## Unchanged contract

- 43 baseline `--once` keyed fields: same spelling and same relative order.
- 30 `--all-counters` rows: identical id, name and order.
- 78 `--regs` rows: identical name and order.
- Volatile values (time, temperature, clocks, CMA free, live counters, SRQCS,
  PCTRE/PCTRC) still change between runs; the check is name/order/count, as
  documented in [FIELD-PARITY.md](FIELD-PARITY.md).
