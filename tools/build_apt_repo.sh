#!/bin/sh
# Rebuild the signed apt repository and dist/ artifacts for one vc4top version.
#
#   GNUPGHOME=~/.gnupg-vc4top tools/build_apt_repo.sh 1.0.3
#
# Expects build/vc4top_<version>_arm64.deb (cmake --build build && cd build &&
# cpack -G DEB). Only the newest package is kept in the pool.
set -eu

ver=${1:?usage: tools/build_apt_repo.sh VERSION}
root=$(cd "$(dirname "$0")/.." && pwd)
repo=$root/repo
arch=arm64
deb=$root/build/vc4top_${ver}_${arch}.deb

[ -f "$deb" ] || { echo "missing $deb" >&2; exit 1; }
: "${GNUPGHOME:=$HOME/.gnupg-vc4top}"
export GNUPGHOME

# dist/ (committed on master) and the apt pool.
rm -f "$root"/dist/vc4top_*_"$arch".deb
cp "$deb" "$root/dist/"
install -d "$repo/pool/main/v/vc4top"
rm -f "$repo"/pool/main/v/vc4top/vc4top_*_"$arch".deb
cp "$deb" "$repo/pool/main/v/vc4top/"

cd "$repo"
apt-ftparchive packages pool > "dists/stable/main/binary-$arch/Packages"
gzip -9 -c "dists/stable/main/binary-$arch/Packages" \
	> "dists/stable/main/binary-$arch/Packages.gz"

# Release must not exist while apt-ftparchive scans the tree, otherwise it
# hashes the previous Release into itself.
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
rm -f dists/stable/Release dists/stable/Release.gpg dists/stable/InRelease
apt-ftparchive \
	-o APT::FTPArchive::Release::Origin=vc4top \
	-o APT::FTPArchive::Release::Label=vc4top \
	-o APT::FTPArchive::Release::Suite=stable \
	-o APT::FTPArchive::Release::Codename=stable \
	-o APT::FTPArchive::Release::Architectures="$arch" \
	-o APT::FTPArchive::Release::Components=main \
	-o APT::FTPArchive::Release::Description="vc4top arm64 packages (VideoCore IV telemetry)" \
	release dists/stable > "$tmp"
chmod 0644 "$tmp"
mv "$tmp" dists/stable/Release

gpg --batch --yes --armor --digest-algo SHA512 \
	--detach-sign -o dists/stable/Release.gpg dists/stable/Release
gpg --batch --yes --clearsign --digest-algo SHA512 \
	-o dists/stable/InRelease dists/stable/Release

gpgv --keyring "$repo/vc4top-archive-keyring.gpg" dists/stable/InRelease

sha256sum "pool/main/v/vc4top/vc4top_${ver}_${arch}.deb" \
	key.asc vc4top-archive-keyring.gpg > SHA256SUMS.txt

cd "$root"
sha256sum "dist/vc4top_${ver}_${arch}.deb" \
	dist/vc4top-archive-keyring.gpg dist/key.asc > dist/SHA256SUMS

echo "repository rebuilt for vc4top $ver"
