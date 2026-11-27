#!/bin/sh
# Usage: mkimg.sh OUT.img I686_BZIMAGE X86_64_BZIMAGE
# One MBR disk with one FAT32 partition labelled BEDROCK (also where reports go):
# BIOS via SYSLINUX (ifcpu64 picks the kernel), UEFI via GRUB x86_64-efi and i386-efi.
set -eu
here=$(dirname "$0")
out=$1
st=$(mktemp -d)
trap 'rm -rf "$st"' EXIT

mkdir -p "$st/boot/syslinux" "$st/boot/grub/theme" "$st/EFI/BOOT"
cp "$2" "$st/boot/vmlinuz32"
cp "$3" "$st/boot/vmlinuz64"
cp /boot/memtest86+ia32.bin "$st/boot/memtest32.bin"
cp /boot/memtest86+x64.bin "$st/boot/memtest64.bin"
cp /boot/memtest86+ia32.efi "$st/boot/memtest32.efi"
cp /boot/memtest86+x64.efi "$st/boot/memtest64.efi"
for m in libcom32 libutil vesamenu ifcpu64; do
	cp /usr/lib/syslinux/modules/bios/$m.c32 "$st/boot/syslinux/"
done
python3 "$here/art.py" "$st/art"
cp "$here/syslinux.cfg" "$st/art/background.png" "$st/boot/syslinux/"
cp "$here/grub.cfg" "$st/boot/grub/"
cp "$here/theme.txt" "$st/art/logo.png" "$st/art"/select_?.png "$st/boot/grub/theme/"
for s in 14 18; do
	grub-mkfont -s $s -r 0x20-0x7e -o "$st/boot/grub/theme/sans-$s.pf2" /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf
done
# Only the firmware GOP/UGA drivers: native ones (all_video) change the mode behind
# the firmware's back, and the kernel then inherits a framebuffer of the wrong size.
mods="part_msdos fat normal configfile test echo serial terminal linux chain efi_gop efi_uga font gfxterm gfxmenu png"
grub-mkimage -O x86_64-efi -p /boot/grub -o "$st/EFI/BOOT/BOOTX64.EFI" $mods
grub-mkimage -O i386-efi -p /boot/grub -o "$st/EFI/BOOT/BOOTIA32.EFI" $mods

# Disk sized to the files plus room for reports (at least 64 MiB), partition from 1 MiB.
mb=$(($(du -sm "$st" | cut -f1) + 32))
[ $mb -ge 64 ] || mb=64
part="$st/part.img"
mkfs.fat -C -F 32 -s 1 -n BEDROCK "$part" $(((mb - 1) * 1024)) >/dev/null
mcopy -s -i "$part" "$st/boot" "$st/EFI" ::
syslinux --install --directory /boot/syslinux "$part"

rm -f "$out"
truncate -s ${mb}M "$out"
echo 'start=2048, type=c, bootable' | sfdisk -q "$out"
dd if=/usr/lib/syslinux/mbr/mbr.bin of="$out" bs=440 count=1 conv=notrunc status=none
dd if="$part" of="$out" bs=1M seek=1 conv=notrunc status=none
ls -l "$out"
