# Unprivileged register access: vc4mon

`vc4top` historically required root because it mapped the V3D register block
through `/dev/mem`. Since 1.0.3 an unprivileged run can read the same block
through `vc4mon`, a read-only out-of-tree module. The root path is unchanged
and is still the only path that can sample performance counters.

## Install

`vc4mon` ships as the `vc4top-dkms` package (`Architecture: all`) and is built
for the running kernel by DKMS:

```bash
sudo apt install vc4top vc4top-dkms
dkms status                       # vc4top/1.0.3, $(uname -r), aarch64: installed
ls /sys/kernel/vc4mon/            # ident  perf  regs
```

Source lives in `/usr/src/vc4top-1.0.3`; `/etc/modules-load.d/vc4mon.conf`
loads the module at boot. `AUTOINSTALL="yes"` rebuilds it after a kernel
upgrade while the matching `linux-headers-*` package is installed. If the
headers are missing, `postinst` prints the exact `apt install
linux-headers-$(uname -r)` command and exits 0 — `vc4top` keeps working as
root through `/dev/mem`, and only the unprivileged sysfs path is unavailable.

Verify that the loaded module comes from the package:

```bash
modinfo -F filename vc4mon          # /lib/modules/$(uname -r)/updates/dkms/vc4mon.ko.xz
dkms status -m vc4top -v 1.0.3
```

Unload/reload and remove:

```bash
sudo modprobe -r vc4mon && sudo modprobe vc4mon
sudo apt remove vc4top-dkms         # prerm: dkms remove --all, modprobe -r
```

## Design

- The module finds the existing `brcm,vc4-v3d` device tree node and calls
  `of_address_to_resource()`. It does **not** register a platform driver and
  does **not** touch `3fc00000.v3d` or `vc4_v3d`.
- The 4 KiB window is mapped a second time with plain `ioremap()`
  (`0x3fc00000..0x3fc00fff`). `request_mem_region()` is not used, so ownership
  stays with the DRM driver. The mapping is dropped on `rmmod`.
- All attributes under `/sys/kernel/vc4mon/` are `0444` and have no store
  handler. There is no reset, clock/power change, page-table work or unbind.
- Reads are 32-bit `ioread32()` loads, the same accessor the DRM driver uses.
- Loading performs no MMIO access; registers are read only when a file is
  read.

Files:

| File | Content |
|---|---|
| `ident` | IDENT0/1/2 |
| `regs` | 78 fixed-format `offset value` lines (the full documented map) |
| `perf` | PCTR0..15 / PCTRS0..15, observation only |

## Verification (Pi 3, kernel 6.18.50+rpt-rpi-v8, stock clocks)

### Load does not disturb DRM

```text
vc4mon: V3D regs 0x000000003fc00000..0x000000003fc00fff mapped read-only (vc4_v3d untouched)
```

`3fc00000.v3d/power/runtime_status` stayed `active`; the KMS display,
`/sys/kernel/debug/dri/0/v3d_ident` and `v3d_regs` remained readable. No Oops
or driver message appeared in `dmesg`.

### The module's view equals the kernel's own V3D dump

78 registers compared against `drm/vc4`'s `/sys/kernel/debug/dri/0/v3d_regs`:

```text
registers compared: 78
mismatches: 0
```

### Root and unprivileged `--once` agree field for field

The `[ QPU / SCHEDULER ]` section (IDENT0/1/2 and the derived configuration,
SRQCS, VPMBASE, VPACNTL, SQRSV0/1, SQCNTL, SRQPC/SRQUA/SRQUL,
INTCTL/INTENA/INTDIS, PCTRE/PCTRC, L2CACTL/SLCACTL, ERRSTAT) is 20 keyed
lines carrying the 43 register-derived values. Across five interleaved
root/unprivileged pairs:

```text
pair 1  diffs: ERRSTAT
pair 2  diffs: none
pair 3  diffs: none
pair 4  diffs: ERRSTAT
pair 5  diffs: ERRSTAT
```

`ERRSTAT` is not a read-path difference: bit 12 is the live VCD-idle status
and flips as the display engine starts and stops. As a control, six
consecutive **root** runs also differ only in `ERRSTAT`:

```text
root-vs-root 2..6  diffs: ERRSTAT
```

Two of the five pairs are identical in all 20 QPU fields, and the only field
that ever differed is the live status bit. The raw 78-register comparison
against the kernel's own dump (above), read back to back, is exact for every
register.

System values that differ between the two runs (temperature, ARM clock,
MemAvailable) are live telemetry, not V3D registers.

`--regs` compares 73 of 78 registers byte-identically. The five that differ
(PCTRE, PCTR0/PCTRS0, PCTR1/PCTRS1) are exactly the counter state that the
root run rewrites *after* printing its QPU section, as part of counter
sampling. The unprivileged path cannot and must not perform those writes, and
its own full register dump still matches the DRM debugfs dump exactly (above).

### No `/dev/mem`

`strace -f -e trace=openat,open ./vc4top --once` as `raspi`:

```text
openat(AT_FDCWD, "/sys/kernel/vc4mon/regs", O_RDONLY) = 3
openat(AT_FDCWD, "/sys/kernel/vc4mon/regs", O_RDONLY) = 3
```

No `/dev/mem` open appears. `/proc/<pid>/fd` for a running `--poll` process
contained only `/dev/null` and the two output pipes.

### Repaint unchanged

PTY replay at 80x24 and 140x40, plus the register view and help screen:

```text
80x24 main        frames/homes=4  scrolls=0  lf=92   maxrow=23 alt=1/1
140x40 main       frames/homes=4  scrolls=0  lf=156  maxrow=39 alt=1/1
80x24 regs view   frames/homes=59 scrolls=0  lf=1357 maxrow=23 alt=1/1
80x24 help        frames/homes=6  scrolls=0  lf=138  maxrow=23 alt=1/1
```

`lf` is exactly `rows - 1` per frame (23 at 80x24, 39 at 140x40), the
alternate screen is entered and left once, and nothing scrolls.

### Clean unload

```text
vc4mon: unloaded, V3D registers unmapped
```

`/sys/kernel/vc4mon/` disappears, `lsmod` no longer lists `vc4mon`, and the
DRM/V3D state is unchanged.

## Root-only counters

Enabling, mapping and clearing the 16 performance counters requires writes to
PCTRE/PCTRC/PCTRS. `vc4mon` has no write path, so unprivileged `vc4top` marks
the counter pages as unavailable instead of pretending to sample them. Run as
root for `--all-counters` and the counter pages.
