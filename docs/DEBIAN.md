# Debian package and apt repository

`vc4top` is published as an `arm64` Debian package, the kernel module as the
`all` package `vc4top-dkms`, and both through a signed apt repository hosted on
GitHub Pages.

- **Package version:** `1.0.5`
- **`vc4top` architecture:** `arm64` (Raspberry Pi OS / Debian)
- **`vc4top-dkms` architecture:** `all` (DKMS builds `vc4mon` locally)
- **Repository suite/component:** `stable main`
- **Signing key fingerprint:** `E4CB3BA058DAAB2BE60EF5D6B736BB9A48B94108`
  (`vc4top archive <rughinnit@users.noreply.github.com>`)

The package is built on Debian 13 (trixie) and needs `glibc >= 2.38`. It runs on
trixie-based Raspberry Pi OS. Bookworm has `glibc 2.36`; use the source build
instructions in the README on bookworm, or rebuild the package there.

## One-line install (no repository)

```bash
curl -fLO https://rughinnit.github.io/vc4top/pool/main/v/vc4top/vc4top_1.0.5_arm64.deb
curl -fLO https://rughinnit.github.io/vc4top/pool/main/v/vc4top/vc4top-dkms_1.0.5_all.deb
sudo apt install ./vc4top_1.0.5_arm64.deb ./vc4top-dkms_1.0.5_all.deb
```

The `.deb` files are also committed to the repository at
`https://github.com/rughinnit/vc4top/raw/master/dist/vc4top_1.0.5_arm64.deb`
and `https://github.com/rughinnit/vc4top/raw/master/dist/vc4top-dkms_1.0.5_all.deb`.
`vc4top-dkms` depends only on `dkms`, not on the `vc4top` binary package, so
the two `.debs` can be installed in either order.

## Signed apt repository

Download and import the archive key:

```bash
curl -fsSL https://rughinnit.github.io/vc4top/key.asc \
  | sudo gpg --dearmor -o /usr/share/keyrings/vc4top-archive-keyring.gpg
```

Add the source (the same line works on **bookworm** and **trixie**; the
repository is a single `stable` suite):

```bash
echo "deb [signed-by=/usr/share/keyrings/vc4top-archive-keyring.gpg] https://rughinnit.github.io/vc4top stable main" \
  | sudo tee /etc/apt/sources.list.d/vc4top.list
sudo apt update
sudo apt install vc4top vc4top-dkms
```

Trixie:

```bash
deb [signed-by=/usr/share/keyrings/vc4top-archive-keyring.gpg] https://rughinnit.github.io/vc4top stable main
```

Bookworm (packages must be rebuilt against bookworm's older glibc; see above):

```bash
deb [signed-by=/usr/share/keyrings/vc4top-archive-keyring.gpg] https://rughinnit.github.io/vc4top stable main
```

The keyring is also available as `vc4top-archive-keyring.gpg` in the repository
root. Do **not** use `[trusted=yes]`: the `Release` file is signed and apt
verifies it.

### Run

As root, `vc4top` maps `/dev/mem` exactly as before. It is deliberately not
installed setuid, so privilege is explicit:

```bash
sudo vc4top                 # realtime TUI
sudo vc4top --once          # wide snapshot
sudo vc4top --all-counters  # all 30 counter sources
sudo vc4top --regs          # full V3D register dump
sudo vc4top --help
```

Unprivileged use is supported through `vc4mon`, shipped as the separate
`vc4top-dkms` package and built for the running kernel by DKMS. It reads the
same V3D registers from `/sys/kernel/vc4mon/`, samples counters through the two
write-only counter attributes, and never opens `/dev/mem`:

```bash
sudo apt install vc4top vc4top-dkms

dkms status                            # vc4top/1.0.5, <kernel>: installed
ls -l /sys/kernel/vc4mon/               # ident perf regs 0444; perf_enable/perf_clear 0620 root:vc4
vc4top --once                          # unprivileged; registers and counters
vc4top                                 # TUI; counters sampled through vc4mon
sudo modprobe -r vc4mon                # unload until the next boot
```

The module does not claim, bind or unbind `3fc00000.v3d`. Its only writable
files are `perf_enable` (PCTRE) and `perf_clear` (PCTRC); everything else is
read-only. The package's udev helper grants the local `vc4` group write-only
access to those two files, so an unprivileged user in `vc4` can sample
counters. Counter source selection stays root-only, so unprivileged runs show
the module's fixed 16-counter map (sources 13..28 on a fresh load). See
[ui/REGISTER-ACCESS.md](ui/REGISTER-ACCESS.md).

## Kernel module package (`vc4top-dkms`)

`vc4top-dkms` is `Architecture: all`; it contains no prebuilt binary, only the
source and the DKMS glue:

```text
/usr/src/vc4top-1.0.5/vc4mon.c
/usr/src/vc4top-1.0.5/Makefile
/usr/src/vc4top-1.0.5/dkms.conf
/etc/modules-load.d/vc4mon.conf              # vc4mon
/usr/lib/udev/rules.d/90-vc4mon.rules        # module-load permission helper
/usr/lib/vc4top/vc4mon-permissions           # chgrp vc4 + chmod 0620
```

- `Depends: dkms`
- `Recommends: linux-headers-arm64 | raspberrypi-kernel-headers`
- `dkms.conf`: `PACKAGE_NAME="vc4top"`, `PACKAGE_VERSION="1.0.5"`,
  `BUILT_MODULE_NAME[0]="vc4mon"`, `DEST_MODULE_LOCATION[0]="/extra"`,
  `AUTOINSTALL="yes"`, and a `MAKE[0]` line that builds with
  `make -C $kernel_source_dir M=$dkms_tree/$PACKAGE_NAME/$PACKAGE_VERSION/build modules`.

dkms 3.x on Debian overrides `DEST_MODULE_LOCATION` with `/updates/dkms`
(`override_dest_module_location()` in `/usr/sbin/dkms`), so the built module is
actually `/lib/modules/$(uname -r)/updates/dkms/vc4mon.ko.xz`. The directive is
kept for dkms implementations that still honour it; use
`modinfo -F filename vc4mon` rather than hard-coding the path.

### What the maintainer scripts do

`postinst` is deliberately non-fatal: it never returns a failure status, so a
broken toolchain cannot leave `vc4top` half-configured.

1. If `/lib/modules/$(uname -r)/build/Makefile` is missing it prints
   `install kernel headers` (with the exact `apt install linux-headers-<kver>`
   command), exits 0, and `vc4top` keeps working as root through `/dev/mem`.
2. `dkms add -m vc4top -v 1.0.5` (idempotent), then
   `dkms build -m vc4top -v 1.0.5 -k $(uname -r)`, then
   `dkms install -m vc4top -v 1.0.5 -k $(uname -r) --force`.
3. `depmod -a $(uname -r)` and `modprobe vc4mon`; the module is installed under
   `/lib/modules/$(uname -r)/updates/dkms/` (dkms 3.x on Debian ignores
   `/extra`), `depmod` makes it visible to `modprobe`, and
   `/sys/kernel/vc4mon/` appears.
4. `/usr/lib/vc4top/vc4mon-permissions` runs once so
   `perf_enable`/`perf_clear` become `0620 root:vc4` immediately; udev runs
   the same helper on every later module load.

`prerm` unloads the module (`modprobe -r vc4mon || true`) and runs
`dkms remove -m vc4top -v 1.0.5 --all`; failures are ignored.

### Kernel upgrades

`AUTOINSTALL="yes"` means DKMS's boot-time autoinstaller rebuilds `vc4mon` for
the newly booted kernel, as long as the matching headers are installed. Keep
`linux-headers-arm64` (or the Raspberry Pi kernel headers package) installed
so the rebuild has `$kernel_source_dir`; `apt` normally pulls the new headers
with the new kernel on Raspberry Pi OS.

If the headers are missing, the kernel upgrade itself succeeds, the module is
simply not rebuilt, and `vc4top` falls back to the root `/dev/mem` path.
Install the headers and rebuild without reinstalling the package:

```bash
sudo apt install linux-headers-$(uname -r)
sudo dpkg-reconfigure vc4top-dkms      # or: sudo dkms autoinstall -k $(uname -r)
```

### Verify

```bash
dpkg -s vc4top-dkms                     # Status: install ok installed
dpkg -L vc4top-dkms                     # /usr/src/vc4top-1.0.5/*, modules-load.d, udev
dkms status                             # vc4top/1.0.5, $(uname -r), aarch64: installed
lsmod | grep vc4mon                     # vc4mon ...
ls -l /lib/modules/$(uname -r)/updates/dkms/vc4mon.ko*   # dkms 3.x path
modinfo -F filename vc4mon
cat /sys/kernel/vc4mon/regs | head      # 78 "offset value" lines
sudo modprobe -r vc4mon && sudo modprobe vc4mon   # unload/reload cycle
```

Then, as an unprivileged member of group `vc4`:

```bash
vc4top --once
strace -f -e trace=openat vc4top --once 2>&1 | grep -c /dev/mem   # 0
echo 0x10000 > /sys/kernel/vc4mon/perf_enable                     # -EINVAL
echo 2 > /sys/kernel/vc4mon/perf_clear                            # -EINVAL
```

The only `/sys/kernel/vc4mon` writes are `perf_enable` and `perf_clear`;
`ident`, `regs` and `perf` remain `0444`.

`dkms status` is the authoritative state: `installed` for the running kernel
means the module on disk was built from `/usr/src/vc4top-1.0.5` for exactly
that kernel. `added` (no build) means the headers or compiler were missing.

## Upgrading from a manual `cmake --install`

An older manual install lives in `/usr/local/bin/vc4top` and is **not owned by
dpkg**. Because `/usr/local/bin` normally precedes `/usr/bin` in `PATH`, that
stale copy would shadow the packaged binary even after `apt install vc4top`.

Inventory it first:

```bash
which -a vc4top
ls -l /usr/bin/vc4top /usr/local/bin/vc4top
dpkg -S vc4top || echo "not dpkg-owned"
find /usr /opt -name 'vc4top*' 2>/dev/null
```

The old manual install on this Pi was:

```text
/usr/local/bin/vc4top   root:root 0755  144608 bytes  Nov  7 21:31
sha256 3e31d35374013d8a169c63018dcf6886a96d633453e2e14bce3ccd65f5e0a6b7
dpkg -S: no path found (untracked manual install)
```

Remove the untracked copy, then install the package:

```bash
sudo rm -f /usr/local/bin/vc4top
hash -r
sudo apt install ./vc4top_1.0.5_arm64.deb    # or: sudo apt install vc4top
which -a vc4top                              # must print /usr/bin/vc4top only
dpkg -S /usr/bin/vc4top                      # must print: vc4top: /usr/bin/vc4top
```

If an old build directory exists, its `install_manifest.txt` records that
manual copy (historically `/usr/local/bin/vc4top`); it is not used by dpkg.
The package installs the binary at `/usr/bin/vc4top`, documentation under
`/usr/share/doc/vc4top/`, and no setuid bits.

## Upgrading from a manually built `vc4mon`

An older manual install put the module in
`/lib/modules/$(uname -r)/extra/vc4mon.ko` and wrote
`/etc/modules-load.d/vc4mon.conf` by hand, neither owned by dpkg. Remove that
state before testing or using `vc4top-dkms`, otherwise `modprobe` can load a
stale module and `dpkg -L` cannot explain it:

```bash
sudo modprobe -r vc4mon || true
sudo rm -f /lib/modules/$(uname -r)/extra/vc4mon.ko
sudo rm -f /etc/modules-load.d/vc4mon.conf
sudo depmod -a
sudo apt install vc4top vc4top-dkms
```

DKMS then installs the same paths from `/usr/src/vc4top-1.0.5`, so
`modinfo -F filename vc4mon` and `dkms status` account for what is loaded.

## Changelog

- `1.0.4` (2026-11-19): unprivileged performance counters. `vc4mon` grows two
  store-only files, `perf_enable` (PCTRE mask `0x8000ffff`) and `perf_clear`
  (literal 1, PCTRC), with an exclusive pid lease and `-EBUSY` on overlapping
  samplers. Nothing else became writable; `ident`/`regs`/`perf` stay `0444`.
  A udev helper grants the `vc4` group write-only access (`0620 root:vc4`).
  `vc4top` samples counters through sysfs when unprivileged, never opens
  `/dev/mem`, and keeps the root path unchanged. Unload clears PCTRE. No field
  or layout regression: `--once` fields, root 30 counter rows and 78 register
  rows are unchanged; 0 pty scrolls at 80x24 and 140x40. See
  [ui/REGISTER-ACCESS.md](ui/REGISTER-ACCESS.md).
- `1.0.3` (2026-11-19): unprivileged register access through the new read-only
  `vc4mon` kernel module (`/sys/kernel/vc4mon/`). `vc4top` keeps `/dev/mem` for
  root and uses sysfs otherwise; unprivileged runs never open `/dev/mem` and
  report the same QPU/register values. Counter pages still need root. No field
  or layout regression: 45 `--once` fields, 30 counter rows, 78 register rows,
  0 pty scrolls. The module now ships and builds separately as
  `vc4top-dkms` (`/usr/src/vc4top-1.0.3` + DKMS, rebuilt on kernel upgrades);
  a reference copy is still in `/usr/share/doc/vc4top/kernel`. See
  [ui/REGISTER-ACCESS.md](ui/REGISTER-ACCESS.md).
- `1.0.2` (2026-11-19): always-on footer legend, ARM and GPU/CMA memory
  bars with percentages, one-line-per-metric utilization, narrow layout order
  tuned for 80x24, and two new `--once` memory fields. The 43 baseline fields,
  30 counter rows and 78 register rows are unchanged in name/order
  (`tools/field_parity.py`). No repaint regression. See
  [ui/PARITY-1.0.2.md](ui/PARITY-1.0.2.md) and
  [ui/PERFORMANCE.md](ui/PERFORMANCE.md).
- `1.0.1` (2026-11-19): live-view repaint fix (single buffered write, alternate
  screen, autowrap off, height clamp, SIGWINCH relayout, exit-path restore) and
  round-robin system sampling. See [ui/PERFORMANCE.md](ui/PERFORMANCE.md).
- `1.0.0`: first packaged release and signed apt repository.

## Repository layout

```text
pool/main/v/vc4top/vc4top_1.0.5_arm64.deb
pool/main/v/vc4top/vc4top-dkms_1.0.5_all.deb
dists/stable/Release
dists/stable/InRelease
dists/stable/Release.gpg
dists/stable/main/binary-arm64/Packages
dists/stable/main/binary-arm64/Packages.gz
vc4top-archive-keyring.gpg
key.asc
```

## GitHub Pages

The repository tree is pushed to the orphan `gh-pages` branch. If
`https://rughinnit.github.io/vc4top/` returns 404, enable Pages in
**Settings → Pages → Build and deployment → Deploy from a branch**, branch
`gh-pages`, folder `/ (root)`.
