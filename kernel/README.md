# vc4mon — read-only V3D register window

`vc4mon` is a small out-of-tree module that lets an unprivileged `vc4top`
read the VideoCore IV (V3D) register block without `/dev/mem`.

It is deliberately **read-only**:

- no platform driver is registered, and nothing is bound or unbound;
- the `brcm,vc4-v3d` device tree node is looked up, its MMIO resource is
  ioremapped a second time with plain `ioremap()`, and the mapping is dropped
  on unload;
- `request_mem_region()` is not called, so the existing `vc4_v3d`/DRM driver
  keeps ownership of the resource;
- every attribute is `0444` and has no store handler;
- register reads are 32-bit `ioread32()` loads. There is no reset, clock,
  power or counter write anywhere in the module.

## Why not claim the device?

`3fc00000.v3d` is owned by the `vc4` DRM driver. Unbinding it is unsafe on
this kernel (it Oopses), and a second platform driver would fail to probe or
would fight for the same resource. A second read-only ioremap of the same
4 KiB window is not a claim: it has the same effect as the existing
`/sys/kernel/debug/dri/0/v3d_regs` debugfs reader, which also reads the live
block.

## Why `ioread32()` and not a 4-byte `memcpy_fromio()`?

`lib/iomap.c` implements a 4-byte `memcpy_fromio()` as four byte accesses.
The V3D block only returns valid data on 32-bit accesses (byte reads at
`+1..+3` come back as zero), so a 4-byte `memcpy_fromio()` returns only the
low byte. A bulk `memcpy_fromio()` would use 64-bit loads; V3D is a 32-bit
register block, and `readl()`/`ioread32()` is what the DRM driver itself uses.
`ioread32()` is therefore the correct MMIO accessor here.

## Attributes

`/sys/kernel/vc4mon/` contains only read-only files:

| File | Content |
|---|---|
| `ident` | `IDENT0`, `IDENT1`, `IDENT2` as `0x%08x` text |
| `regs` | 78 lines, `%05x %08x` (offset, value), matching the documented register map in `vc4top`'s `vc4_defines.h` |
| `perf` | `PCTR0..15` and `PCTRS0..15`, observation only (never enables or clears counters) |

`regs` covers everything `vc4top` needs: IDENT, control-list, binner, QPU
scheduler/queues, VPM, performance counters (PCTR/PCTRS/PCTRC/PCTRE), debug
and ERRSTAT.

## Build and install

Kernel headers are required (`linux-headers-$(uname -r)`).

```bash
make -C kernel
sudo make -C kernel install     # /lib/modules/$(uname -r)/extra/vc4mon.ko
sudo modprobe vc4mon
```

Load at boot:

```bash
echo vc4mon | sudo tee /etc/modules-load.d/vc4mon.conf
```

Unload:

```bash
sudo modprobe -r vc4mon
```

`rmmod` removes the sysfs files and unmaps the window; the DRM driver and the
V3D hardware state are untouched.

## Notes

- Loading the module performs no MMIO access; registers are only read when a
  sysfs file is read.
- If the V3D power domain is off, reads return `0xdeadbeef`, exactly like the
  `/dev/mem` path. Keep the block powered (`vc4cl-v3d-power.service`).
- The module taints the kernel (`loading out-of-tree module`). That is
  expected for an out-of-tree helper and does not indicate a fault.
- Tested on Raspberry Pi 3, kernel `6.18.50+rpt-rpi-v8`, Debian 13, stock
  clocks, with `vc4` KMS active.
