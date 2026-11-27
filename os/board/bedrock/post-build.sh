#!/bin/sh
# Buildroot post-build script ($1 = target dir): boot splash and the 16x32 console font.
set -eu
here=$(dirname "$0")
art=$(mktemp -d)
trap 'rm -rf "$art"' EXIT
/usr/bin/python3 "$here/art.py" "$art"
mkdir -p "$1/usr/share/bedrock" "$1/usr/share/consolefonts"
cp "$art/splash.ppm" "$art/splash2x.ppm" "$1/usr/share/bedrock/"
gunzip -c /usr/share/consolefonts/Uni2-Terminus32x16.psf.gz >"$1/usr/share/consolefonts/terminus-32.psf"
# Graphics firmware (64-bit system only): xz-compressed; the kernel unpacks the
# files a card asks for. Links are renamed to point at the compressed files.
if [ -d "$1/lib/firmware" ]; then
	find "$1/lib/firmware" -type f ! -name '*.xz' -exec xz -C crc32 -T0 {} +
	find "$1/lib/firmware" -type l ! -name '*.xz' | while read -r l; do
		ln -s "$(readlink "$l").xz" "$l.xz" && rm "$l"
	done
fi
