#!/bin/sh
# Builds a .deb from the current tree, on the machine it runs on: the
# dependencies are computed by dpkg-shlibdeps against the libraries
# installed here, so build on each target distro.
# Usage: tools/build-deb.sh [OUTDIR]   (needs build-essential, dpkg-dev,
# gettext, libglib2.0-dev, libgtk2.0-dev and libudev-dev).
set -eu

cd "$(dirname "$0")/.."
root=$(pwd)
out=${1:-$root/dist}
version=$(sed -n 's/^VERSION *= *//p' config.mk)
arch=$(dpkg --print-architecture)
maint=${DEB_MAINTAINER:-Kiyoshi Spreclerg <kiyoshi_pip@protonmail.com>}

mkdir -p "$out"
out=$(cd "$out" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
pkg=$work/pkg

make clean
make -j"$(nproc)" PREFIX=/usr
make PREFIX=/usr DESTDIR="$pkg" install
strip --strip-unneeded "$pkg"/usr/bin/kidisks
find "$pkg" -type d -exec chmod 755 {} +

# dpkg-shlibdeps insists on a debian/control in the current directory.
mkdir -p "$work/debian"
printf 'Source: kidisks\n\nPackage: kidisks\nArchitecture: any\n' > "$work/debian/control"
(
	cd "$work"
	dpkg-shlibdeps -O "-e$pkg/usr/bin/kidisks" > "$work/substvars"
)
depends=$(sed -n 's/^shlibs:Depends=//p' "$work/substvars")

mkdir -p "$pkg/DEBIAN"
size=$(du -sk "$pkg" | cut -f1)
# udisks2 is talked to over D-Bus, so no library links to it.
cat > "$pkg/DEBIAN/control" <<CTL
Package: kidisks
Version: $version-1
Architecture: $arch
Maintainer: $maint
Installed-Size: $size
Depends: $depends
Recommends: udisks2
Section: utils
Priority: optional
Homepage: https://github.com/${GITHUB_REPOSITORY:-kidisks/kidisks}
Description: lightweight disk manager
 GParted, Filelight and BleachBit in one small GTK2 program, in four tabs:
 drives (health, mount, eject), partitions, space (ring chart) and cleanup.
CTL

deb=$out/kidisks_${version}-1_${arch}.deb
dpkg-deb --root-owner-group --build "$pkg" "$deb"
echo "$deb"
