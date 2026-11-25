#!/bin/sh
# Usage: mkiso.sh OUT.iso I686_BZIMAGE
# BIOS-only El Torito ISO with the i686 kernel, for the v86 browser emulator.
set -eu
out=$1
st=$(mktemp -d)
trap 'rm -rf "$st"' EXIT

mkdir -p "$st/isolinux"
cp /usr/lib/ISOLINUX/isolinux.bin /usr/lib/syslinux/modules/bios/ldlinux.c32 "$st/isolinux/"
cp "$2" "$st/vmlinuz32"
cat >"$st/isolinux/isolinux.cfg" <<'EOF'
SERIAL 0 115200
DEFAULT bedrock
LABEL bedrock
	LINUX /vmlinuz32
	APPEND quiet loglevel=0 vt.global_cursor_default=0 console=ttyS0,115200 console=tty0
EOF
xorriso -as mkisofs -quiet -o "$out" -V BEDROCK_V86 \
	-b isolinux/isolinux.bin -c isolinux/boot.cat \
	-no-emul-boot -boot-load-size 4 -boot-info-table "$st"
ls -l "$out"
