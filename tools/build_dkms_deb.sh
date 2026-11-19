#!/bin/sh
# Assemble and build the vc4top-dkms_<version>_all.deb package.
#
#   tools/build_dkms_deb.sh 1.0.4
#
# The package ships the module source under /usr/src/vc4top-<version>/ so DKMS
# can rebuild it for every kernel.  No kernel build happens here: the compiler
# runs on the target machine, in postinst (and again on kernel upgrades).
set -eu

ver=${1:?usage: tools/build_dkms_deb.sh VERSION}
root=$(cd "$(dirname "$0")/.." && pwd)
src=$root/packaging/dkms
stage=$root/build/dkms-deb/vc4top-dkms_${ver}_all
out=$root/build/vc4top-dkms_${ver}_all.deb

[ -f "$root/kernel/vc4mon.c" ] || { echo "missing kernel/vc4mon.c" >&2; exit 1; }

rm -rf "$root/build/dkms-deb"
install -d "$stage/DEBIAN" \
	"$stage/usr/src/vc4top-$ver" \
	"$stage/etc/modules-load.d" \
	"$stage/usr/lib/udev/rules.d" \
	"$stage/usr/lib/vc4top" \
	"$stage/usr/share/doc/vc4top-dkms"

install -m 0644 "$root/kernel/vc4mon.c"       "$stage/usr/src/vc4top-$ver/vc4mon.c"
install -m 0644 "$src/Makefile"               "$stage/usr/src/vc4top-$ver/Makefile"
install -m 0644 "$src/dkms.conf"              "$stage/usr/src/vc4top-$ver/dkms.conf"
install -m 0644 "$src/vc4mon.conf"            "$stage/etc/modules-load.d/vc4mon.conf"
install -m 0755 "$src/vc4mon-permissions"     "$stage/usr/lib/vc4top/vc4mon-permissions"
install -m 0644 "$src/90-vc4mon.rules"        "$stage/usr/lib/udev/rules.d/90-vc4mon.rules"
install -m 0644 "$root/kernel/README.md"      "$stage/usr/share/doc/vc4top-dkms/README.md"

sed "s/@VERSION@/$ver/g" "$src/control.in" > "$stage/DEBIAN/control"
install -m 0755 "$src/postinst"               "$stage/DEBIAN/postinst"
install -m 0755 "$src/prerm"                  "$stage/DEBIAN/prerm"
sed -i "s/@VERSION@/$ver/g" "$stage/DEBIAN/postinst" "$stage/DEBIAN/prerm"

size=$(du -sk --exclude=DEBIAN "$stage" | cut -f1)
sed -i "s/@INSTALLED_SIZE@/$size/g" "$stage/DEBIAN/control"

# Deterministic ownership and timestamps keep the .deb reproducible.
find "$stage" -newermt '1970-01-02' -exec touch -d '@1' {} + 2>/dev/null || true
dpkg-deb --root-owner-group -Zxz --build "$stage" "$out" >/dev/null

echo "built $out"
dpkg-deb --info "$out"
