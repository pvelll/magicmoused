#!/bin/sh
# Build a Debian/Ubuntu package from the binaries "make" has already built.
#
#   packaging/build-deb.sh VERSION ARCH OUTDIR
#
# Called by "make deb"; the release workflow runs it for amd64 and arm64.
set -eu

version=$1
arch=$2
outdir=$3

# Debian versions start with a digit, and a pre-release such as 1.1.0-rc1
# must sort before 1.1.0: 1.1.0~rc1.
case "$version" in
    [0-9]*) ;;
    *) version="0~$version" ;;
esac
version=$(echo "$version" | sed 's/-/~/')

here=$(cd "$(dirname "$0")" && pwd)
root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT
chmod 755 "$root"

make --no-print-directory install DESTDIR="$root" PREFIX=/usr UNITDIR=/usr/lib/systemd/system >/dev/null
install -Dm644 README.md "$root/usr/share/doc/magicmoused/README.md"

# The newest glibc symbol version the binaries use is the oldest glibc
# they run on.
glibc=$(readelf --dyn-syms -W magicmoused magicmouse-dump 2>/dev/null |
        grep -o 'GLIBC_[0-9][0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -n 1)

depends="systemd"
if [ -n "$glibc" ]; then
    depends="libc6 (>= $glibc), $depends"
fi

size=$(du -sk "$root" | cut -f1)

mkdir -p "$root/DEBIAN"
sed -e "s/@VERSION@/$version/" -e "s/@ARCH@/$arch/" \
    -e "s/@DEPENDS@/$depends/" -e "s/@SIZE@/$size/" \
    "$here/deb/control" > "$root/DEBIAN/control"
echo /etc/magicmouse/magicmouse.conf > "$root/DEBIAN/conffiles"
for script in postinst prerm postrm; do
    install -m755 "$here/deb/$script" "$root/DEBIAN/$script"
done

mkdir -p "$outdir"
deb="$outdir/magicmoused_${version}_${arch}.deb"
dpkg-deb --root-owner-group --build "$root" "$deb" >/dev/null
echo "$deb"
