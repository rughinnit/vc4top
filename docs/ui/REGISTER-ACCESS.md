# Unprivileged register access and counters: vc4mon

`vc4top` historically required root because it mapped the V3D register block
through `/dev/mem`. Since 1.0.3 an unprivileged run can read the same block
through `vc4mon`; since 1.0.4 it can also sample the performance counters
through two write-only sysfs attributes. The root path is unchanged and still
uses `/dev/mem`.

## Install

`vc4mon` ships as the `vc4top-dkms` package (`Architecture: all`) and is built
for the running kernel by DKMS:

```bash
sudo apt install vc4top vc4top-dkms
dkms status                       # vc4top/1.0.4, $(uname -r), aarch64: installed
ls -l /sys/kernel/vc4mon/         # ident perf regs 0444; perf_enable/perf_clear 0620 root:vc4
```

Source lives in `/usr/src/vc4top-1.0.4`; `/etc/modules-load.d/vc4mon.conf`
loads the module at boot. `AUTOINSTALL="yes"` rebuilds it after a kernel
upgrade while the matching `linux-headers-*` package is installed. If the
headers are missing, `postinst` prints the exact `apt install
linux-headers-$(uname -r)` command and exits 0 — `vc4top` keeps working as
root through `/dev/mem`, and only the unprivileged sysfs path is unavailable.

Unprivileged counter writes need the local `vc4` group. The package installs
`/usr/lib/udev/rules.d/90-vc4mon.rules` and
`/usr/lib/vc4top/vc4mon-permissions`, and udev runs the helper whenever the
module loads. It changes only the two counter files from `0200 root:root` to
`0620 root:vc4` (write-only for the group, never readable). The module's
default owner/mode is unchanged if the helper is not installed; use a manual
checkout, or add the user to `vc4`, for the packaged path.

Verify that the loaded module comes from the package:

```bash
modinfo -F filename vc4mon          # /lib/modules/$(uname -r)/updates/dkms/vc4mon.ko.xz
dkms status -m vc4top -v 1.0.4
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
- Reads are 32-bit `ioread32()` loads, the same accessor the DRM driver uses.
  Counter writes are 32-bit `iowrite32()` stores to PCTRE/PCTRC only.
- All other registers, resets, clocks, power domains, page tables and driver
  binding are untouched.

Files:

| File | Mode | Content |
|---|---|---|
| `ident` | `0444` | IDENT0/1/2 |
| `regs` | `0444` | 78 fixed-format `offset value` lines (the full documented map) |
| `perf` | `0444` | PCTR0..15 / PCTRS0..15, observation only |
| `perf_enable` | `0200` (`0620 root:vc4` after udev) | PCTRE value: `0`, or any value with no bits outside `0x8000ffff` |
| `perf_clear` | `0200` (`0620 root:vc4` after udev) | literal `1`: write `0xffff` to PCTRC and poll (bounded at 10 ms) for self-clear |

## Exact write surface and security rationale

`perf_enable` accepts only a value whose set bits are a subset of
`0x8000ffff`: bits 0..15 are the 16 counter enables and bit 31 is the
hardware-required enable bit. `0x10000`, `0x40000000` or any other bit outside
the mask returns `-EINVAL` without a store. `perf_clear` accepts only `1`, then
writes `0xffff` to PCTRC and waits for the hardware to clear it. A stuck
PCTRC returns `-ETIMEDOUT`; the store never sleeps forever.

A nonzero `perf_enable` takes an exclusive, module-level sampler lease keyed by
`current->pid`. A different process gets `-EBUSY` while the lease is live; the
lease expires after 30 s so a killed sampler cannot lock the counters forever.
Writing `0` releases it. The same process may update its own lease. `rmmod`
writes `0` to PCTRE if the lease is still active.

Nothing else becomes writable: there is no store handler for `ident`, `regs`,
`perf`, `SRQCS`, `PCTR*` data, `PCTRS*`, `VPMBASE`, `L2CACTL`, `SLCACTL`,
reset, clock or power.

PCTRE/PCTRC are counter-control registers inside V3D. They enable and clear 16
saturating counter latches. They cannot program DMA, cannot write memory
(there is no address/length field), cannot submit or alter a control list, and
cannot affect rendering, clocks, voltages or power. A hostile writer can only
enable/clear counters or hold the telemetry lease; the worst outcome is
unavailable or garbage counters until the lease expires or the module unloads.

At load time, and only when all 16 PCTRS registers still read zero (the
hardware reset value), the module writes the documented primary counter map
(`PCTRS[i] = 13 + i`, sources 13..28). PCTRS is likewise counter-only: it
selects which internal event drives a latch and has no DMA, memory, render,
clock or power effect. An existing map is left untouched, and no userspace
PCTRS write path exists. This gives a fresh boot a useful unprivileged map
without widening the sysfs surface.

## Verification (Pi 3, kernel 6.18.50+rpt-rpi-v8, stock clocks)

### Invalid writes are rejected

```text
echo 0x10000     > perf_enable   -> write error: Invalid argument
echo 0x40000000  > perf_enable   -> write error: Invalid argument
echo 0x1ffff     > perf_enable   -> write error: Invalid argument
echo 0x8000ffff  > perf_enable   -> ok
echo 2           > perf_clear    -> write error: Invalid argument
echo 1           > perf_clear    -> ok
```

A second process attempting a nonzero `perf_enable` while the first holds the
lease gets `write error: Device or resource busy`.

### Unprivileged and root counters agree under load

Workload: the V3D 3D pipeline rendering 20,000 triangles per frame for 10 s
(EGL surfaceless + GLES2, under `/tmp/vc4-gpu.lock`). Both runs sample 500 ms:

```text
unprivileged                              root
13 idle           1431083928             1431240624
14 vertex          105842729              105733937
15 fragment        263732064              263342416
16 valid           233881820              233814596
20 i-cache hits     58470613               58453642
21 i-cache miss         2304                   2304
22 u-cache hits     13425316               13419301
27 VPM VCD stall     1592738                1593334
28 L2C hits             1408                   1408
```

Every sampled value is within 0.2 % (sampling-window boundary); most are exact
or differ by the handful of events at the window edges. `bench_vc4cl` raw QPU
compute is not a useful V3D counter load on this hardware (the README already
documents that its QPU path leaves the 3D counters at zero), so the GL load was
used for the counter check.

Idle, same command: every workload counter is zero; only source 13 (QPU idle
clock cycles) advances, exactly as in the root path.

### strace: no `/dev/mem`, only two writable files

`strace -f -yy -e trace=openat,write ./vc4top --once` as `raspi`, with the GL
load running:

```text
write(3</sys/kernel/vc4mon/perf_enable>, "0\n", 2)
write(3</sys/kernel/vc4mon/perf_clear>, "1\n", 2)
write(3</sys/kernel/vc4mon/perf_enable>, "2147549183\n", 11)   # 0x8000ffff
write(3</sys/kernel/vc4mon/perf_enable>, "0\n", 2)
write(3</sys/kernel/vc4mon/perf_clear>, "1\n", 2)
```

All `/sys/kernel/vc4mon` writes target only `perf_enable`/`perf_clear`;
`/dev/mem` opens: 0. `perf` and `regs` are opened `O_RDONLY`.

### Repaint and unload

PTY replay of the live view remains at 0 scrolls at 80x24 and 140x40, with the
same one-buffer-per-frame discipline; the counter changes do not add output to
the repaint path. `rmmod` logs `vc4mon: PCTRE cleared on unload` when counters
are enabled, then `vc4mon: unloaded, V3D registers unmapped`; the sysfs
directory disappears and the DRM/V3D state is unchanged.

## Read-path parity (1.0.3, unchanged)

The `[ QPU / SCHEDULER ]` section (20 keyed lines, 43 register-derived values)
matched field for field across five interleaved root/unprivileged pairs. The
only differing field was the live `ERRSTAT` VCD-idle bit, which also differs
between consecutive root runs. The 78-register comparison against
`/sys/kernel/debug/dri/0/v3d_regs` had 0 mismatches. The 1.0.4 changes add only
the counter stores; the read path and every read-only file are unchanged.

## Limitation

The unprivileged write surface deliberately has no PCTRS selector. The 16
counters therefore keep the map configured in the module (the documented
13..28 primary map on a fresh load) or the map left by a previous root owner.
Root `--all-counters` still remaps and samples all 30 sources.
