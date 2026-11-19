# Debian package and apt repository

`vc4top` is published as an `arm64` Debian package and through a signed apt
repository hosted on GitHub Pages.

- **Package version:** `1.0.2`
- **Architecture:** `arm64` (Raspberry Pi OS / Debian)
- **Repository suite/component:** `stable main`
- **Signing key fingerprint:** `E4CB3BA058DAAB2BE60EF5D6B736BB9A48B94108`
  (`vc4top archive <rughinnit@users.noreply.github.com>`)

The package is built on Debian 13 (trixie) and needs `glibc >= 2.38`. It runs on
trixie-based Raspberry Pi OS. Bookworm has `glibc 2.36`; use the source build
instructions in the README on bookworm, or rebuild the package there.

## One-line install (no repository)

```bash
curl -fLO https://rughinnit.github.io/vc4top/pool/main/v/vc4top/vc4top_1.0.2_arm64.deb
sudo apt install ./vc4top_1.0.2_arm64.deb
```

The `.deb` is also committed to the repository at
`https://github.com/rughinnit/vc4top/raw/master/dist/vc4top_1.0.2_arm64.deb`.

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
sudo apt install vc4top
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

`vc4top` maps `/dev/mem` and **must run as root**. It is deliberately not
installed setuid, so privilege is explicit:

```bash
sudo vc4top                 # realtime TUI
sudo vc4top --once          # wide snapshot
sudo vc4top --all-counters  # all 30 counter sources
sudo vc4top --regs          # full V3D register dump
sudo vc4top --help
```

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
sudo apt install ./vc4top_1.0.2_arm64.deb    # or: sudo apt install vc4top
which -a vc4top                              # must print /usr/bin/vc4top only
dpkg -S /usr/bin/vc4top                      # must print: vc4top: /usr/bin/vc4top
```

If an old build directory exists, its `install_manifest.txt` records that
manual copy (historically `/usr/local/bin/vc4top`); it is not used by dpkg.
The package installs the binary at `/usr/bin/vc4top`, documentation under
`/usr/share/doc/vc4top/`, and no setuid bits.

## Changelog

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
pool/main/v/vc4top/vc4top_1.0.2_arm64.deb
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
