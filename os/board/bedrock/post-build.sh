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
