# vc4mon — V3D register window with a narrow counter write path

`vc4mon` is a small out-of-tree module that lets an unprivileged `vc4top`
read the VideoCore IV (V3D) register block without `/dev/mem`, and sample the
performance counters through two write-only files.

It is **read-mostly**:

- no platform driver is registered, and nothing is bound or unbound;
- the `brcm,vc4-v3d` device tree node is looked up, its MMIO resource is
  ioremapped a second time with plain `ioremap()`, and the mapping is dropped
  on unload;
- `request_mem_region()` is not called, so the existing `vc4_v3d`/DRM driver
  keeps ownership of the resource;
- `ident`, `regs` and `perf` are `0444` and have no store handler;
- only `perf_enable` and `perf_clear` exist as `0200` store-only files (the
  packaged helper grants group `vc4` write access, making them `0620`, still
  with no read permission);
- register reads are 32-bit `ioread32()` loads; counter writes are 32-bit
  `iowrite32()` stores to PCTRE/PCTRC only. There is no reset, clock, power,
  queue, VPM, L2 or general register write anywhere in the module.

## Why not claim the device?

`3fc00000.v3d` is owned by the `vc4` DRM driver. Unbinding it is unsafe on
this kernel (it Oopses), and a second platform driver would fail to probe or
would fight for the same resource. A second ioremap of the same 4 KiB window
is not a claim: it has the same effect as the existing
`/sys/kernel/debug/dri/0/v3d_regs` debugfs reader, which also reads the live
block.

## Why `ioread32()` and not a 4-byte `memcpy_fromio()`?

`lib/iomap.c` implements a 4-byte `memcpy_fromio()` as four byte accesses.
The V3D block only returns valid data on 32-bit accesses (byte reads at
`+1..+3` come back as zero), so a 4-byte `memcpy_fromio()` returns only the
low byte. A bulk `memcpy_fromio()` would use 64-bit loads; V3D is a 32-bit
register block, and `readl()`/`ioread32()` is what the DRM driver itself uses.
`ioread32()` is therefore the correct MMIO accessor here.

## Write surface

| File | Mode | Contract |
|---|---|---|
| `perf_enable` | `0200` (`0620 root:vc4` after udev) | write `0` to disable and release the sampler lease, or a value with **no bits outside** `0x8000ffff`. Bits 0..15 are the 16 counter enables; bit 31 is marked reserved but is required on this V3D for the counters to advance. Any other bit is rejected with `-EINVAL` and no store occurs. |
| `perf_clear` | `0200` (`0620 root:vc4` after udev) | write exactly `1`. The module writes `0xffff` to PCTRC and polls until it reads back zero, bounded at 10 ms; a timeout returns `-ETIMEDOUT` instead of blocking forever. Any other value is `-EINVAL`. |

Nothing else becomes writable. In particular there is no write path to
`SRQCS`, `PCTR*` data/index registers, `PCTRS*`, `VPMBASE`, `L2CACTL`,
`SLCACTL`, reset, clock or power.

### Security argument

PCTRE and PCTRC are counter-control registers inside V3D. They enable and
clear the 16 saturating counter latches. They cannot program a DMA engine,
cannot write memory (there is no address or length field), cannot submit or
alter a control list, and cannot affect rendering, clocks, voltages or power
domains. A hostile writer can at most enable counters (a tiny amount of
counter activity), clear them, or hold the exclusive lease for the lease
period. The worst outcome is unavailable/garbage telemetry until the lease
expires or the module is unloaded.

At load time, and only when all 16 PCTRS registers still read the hardware
reset value of zero, the module writes the documented primary map
(`PCTRS[i] = 13 + i`, sources 13..28). PCTRS is also a counter-only register:
it selects which internal event drives a latch. It has no DMA, memory,
render, clock or power effect. An existing map (for example one programmed by
a root `vc4top` run) is left untouched, and there is no userspace PCTRS write
path.

### Exclusive sampler lease

sysfs gives a store handler no client identity beyond the calling task, so a
nonzero `perf_enable` write claims a module-level lease keyed by `current->pid`.
Another process receives `-EBUSY` until the owner writes `0` or the 30-second
lease expires. A crashed sampler therefore cannot lock the counters forever.
The same process may update its own lease. `rmmod` writes 0 to PCTRE when the
lease is active, so unloading never leaves counters enabled.

## Attributes

`/sys/kernel/vc4mon/` contains:

| File | Content |
|---|---|
| `ident` | `IDENT0`, `IDENT1`, `IDENT2` as `0x%08x` text |
| `regs` | 78 lines, `%05x %08x` (offset, value), matching the documented register map in `vc4top`'s `vc4_defines.h` |
| `perf` | `PCTR0..15` and `PCTRS0..15`; observation only (never enables or clears counters) |
| `perf_enable` | PCTRE write-only counter enable |
| `perf_clear` | PCTRC write-only counter clear |

`regs` covers everything `vc4top` needs: IDENT, control-list, binner, QPU
scheduler/queues, VPM, performance counters (PCTR/PCTRS/PCTRC/PCTRE), debug
and ERRSTAT.

## Install (packaged, DKMS)

`vc4mon` is shipped by the `vc4top-dkms` package and built for the running
kernel by DKMS:

```bash
sudo apt install vc4top vc4top-dkms
dkms status                    # vc4top/1.0.4, $(uname -r), aarch64: installed
ls -l /sys/kernel/vc4mon/      # ident perf regs 0444; perf_enable/perf_clear 0620 root:vc4
```

Unprivileged counter sampling requires membership of the local `vc4` group.
The package installs `/usr/lib/udev/rules.d/90-vc4mon.rules` and
`/usr/lib/vc4top/vc4mon-permissions`; udev runs the helper on every module
load, and `postinst` runs it once immediately. The helper only changes
`perf_enable` and `perf_clear` to `root:vc4 0620`.

Package contents:

```text
/usr/src/vc4top-1.0.4/vc4mon.c
/usr/src/vc4top-1.0.4/Makefile
/usr/src/vc4top-1.0.4/dkms.conf
/etc/modules-load.d/vc4mon.conf              # loads the module at boot
/usr/lib/udev/rules.d/90-vc4mon.rules        # group access on module load
/usr/lib/vc4top/vc4mon-permissions           # chgrp vc4 + chmod 0620
```

`dkms.conf` uses `DEST_MODULE_LOCATION[0]="/extra"`; dkms 3.x on Debian
overrides that with `/updates/dkms`, so the built module is
`/lib/modules/$(uname -r)/updates/dkms/vc4mon.ko.xz` (check with
`modinfo -F filename vc4mon`). `postinst` runs
`dkms add/build/install -m vc4top -v 1.0.4 -k $(uname -r)`, `depmod -a`,
`modprobe vc4mon` and the permission helper, but **never fails the dpkg run**:
with missing headers or compiler it prints the
`apt install linux-headers-$(uname -r)` command and leaves `vc4top`'s root
`/dev/mem` path intact.

`AUTOINSTALL="yes"` rebuilds the module for a newly booted kernel as long as
the matching `linux-headers-*` package is present. After a kernel upgrade the
module is rebuilt at boot; if the headers are missing, install them and run
`sudo dpkg-reconfigure vc4top-dkms` (or `sudo dkms autoinstall -k $(uname -r)`).

`prerm` unloads the module (`modprobe -r vc4mon`) and runs
`dkms remove -m vc4top -v 1.0.4 --all`, ignoring failures.

## Build and install from a git checkout

Kernel headers are required (`linux-headers-$(uname -r)`).

```bash
make -C kernel
sudo make -C kernel install     # /lib/modules/$(uname -r)/extra/vc4mon.ko
sudo modprobe vc4mon

# Manual checkout: grant the two counter files to the vc4 group once per load.
sudo chgrp vc4 /sys/kernel/vc4mon/perf_enable /sys/kernel/vc4mon/perf_clear
sudo chmod 0620 /sys/kernel/vc4mon/perf_enable /sys/kernel/vc4mon/perf_clear
```

Load at boot:

```bash
echo vc4mon | sudo tee /etc/modules-load.d/vc4mon.conf
```

Unload:

```bash
sudo modprobe -r vc4mon
```

`rmmod` removes the sysfs files, disables PCTRE if it was left enabled, and
unmaps the window; the DRM driver and the rest of the V3D state are untouched.

## Verify

```bash
cat /sys/kernel/vc4mon/ident
cat /sys/kernel/vc4mon/regs | wc -l        # 78
cat /sys/kernel/vc4mon/perf | wc -l        # 32
echo 0x10000 > /sys/kernel/vc4mon/perf_enable   # -EINVAL (bit outside mask)
echo 0x8000ffff > /sys/kernel/vc4mon/perf_enable
echo 1 > /sys/kernel/vc4mon/perf_clear
echo 0 > /sys/kernel/vc4mon/perf_enable
lsmod | grep vc4mon
dkms status                                # vc4top/1.0.4: installed
vc4top --once                              # as a member of group vc4
```

`dkms status` reporting `installed` for the running kernel is the authoritative
check that the loaded module was built from the packaged source for exactly
that kernel. `added` without a build means the headers or compiler were
missing.

## Notes

- Loading the module performs MMIO reads of the PCTRS map and writes the
  documented primary map only if all 16 PCTRS values are zero. Registers are
  otherwise read only when a sysfs file is read.
- If the V3D power domain is off, reads return `0xdeadbeef`, exactly like the
  `/dev/mem` path. Keep the block powered (`vc4cl-v3d-power.service`).
- The module taints the kernel (`loading out-of-tree module`). That is
  expected for an out-of-tree helper and does not indicate a fault.
- Tested on Raspberry Pi 3, kernel `6.18.50+rpt-rpi-v8`, Debian 13, stock
  clocks, with `vc4` KMS active.
