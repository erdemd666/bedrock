# Bedrock boot images. Every target runs inside the bedrock-build Docker image;
# on the host each target re-runs itself there. Buildroot output and downloads
# live in the named volume bedrock-work, so rebuilds are incremental.
#
#   make img        out/bedrock.img      (BIOS + UEFI, i686 + x86_64, memtest86+)
#   make v86        out/bedrock-v86.iso  (i686, BIOS only, for the v86 emulator)
#   make qemu-p3    boot bedrock.img: Pentium III, SeaBIOS, serial on stdio
#   make qemu-uefi  boot bedrock.img: x86_64, OVMF, serial on stdio
#   make boottest   headless report over serial on both, fails on timeout;
#                   also saves the boot menus as out/menu-bios.png, out/menu-uefi.png
#   make fleet      out/fleet/: the bedrock-fleet server (Go, built in a golang container)
#                   and the two network-boot kernels bzImage-x86_64, bzImage-i686
#   make fleettest  boot out/fleet/bzImage-x86_64 in QEMU and check the server got its record

TARGETS := img v86 qemu-p3 qemu-uefi boottest
.PHONY: $(TARGETS) fleet fleettest

ifeq ($(wildcard /opt/buildroot),)

IMAGE  := bedrock-build
VOLUME ?= bedrock-work
TTY    := $(shell [ -t 0 ] && echo -t)

$(TARGETS):
	docker build -q -t $(IMAGE) docker
	docker run --rm -i $(TTY) -v "$(CURDIR)":/src -v $(VOLUME):/work $(IMAGE) make $@

GOIMAGE := golang:1.24-alpine

fleet:
	docker build -q -t $(IMAGE) docker
	docker run --rm -i $(TTY) -v "$(CURDIR)":/src -v $(VOLUME):/work $(IMAGE) make fleet-images
	docker run --rm -v "$(CURDIR)/fleet":/src -v "$(CURDIR)/out/fleet":/out -w /src $(GOIMAGE) \
		sh -c 'go vet ./... && go test ./... && CGO_ENABLED=0 go build -trimpath -ldflags=-s -o /out/bedrock-fleet .'

fleettest: fleet
	docker run --rm -i $(TTY) -v "$(CURDIR)":/src $(IMAGE) sh os/board/bedrock/fleettest.sh /src/out/fleet

else

BR  := /opt/buildroot
W   := /work
OUT := /src/out

build-%:
	$(MAKE) -C $(BR) O=$(W)/$* BR2_EXTERNAL=/src/os bedrock_$*_defconfig
	$(MAKE) -C $(W)/$* bedrock-rebuild all

img: build-i686 build-x86_64
	mkdir -p $(OUT)
	sh os/board/bedrock/mkimg.sh $(OUT)/bedrock.img $(W)/i686/images/bzImage $(W)/x86_64/images/bzImage

v86: build-i686
	mkdir -p $(OUT)
	sh os/board/bedrock/mkiso.sh $(OUT)/bedrock-v86.iso $(W)/i686/images/bzImage

$(OUT)/bedrock.img:
	$(MAKE) img

# The kernels already hold the whole OS (initramfs): one file per architecture is all a netboot client needs.
fleet-images: build-i686 build-x86_64
	mkdir -p $(OUT)/fleet
	cp $(W)/i686/images/bzImage $(OUT)/fleet/bzImage-i686
	cp $(W)/x86_64/images/bzImage $(OUT)/fleet/bzImage-x86_64

# Empty test disks: one each on IDE, AHCI and NVMe.
DISKS := $(W)/disk-ide.img $(W)/disk-ahci.img $(W)/disk-nvme.img
$(W)/disk-%.img:
	truncate -s 64M $@

QEMU_DISKS := \
	-drive file=$(W)/disk-ide.img,format=raw,if=ide,index=1 \
	-device ahci,id=ahci -drive file=$(W)/disk-ahci.img,format=raw,if=none,id=sata0 \
	-device ide-hd,drive=sata0,bus=ahci.0 \
	-drive file=$(W)/disk-nvme.img,format=raw,if=none,id=nvme0 \
	-device nvme,drive=nvme0,serial=BEDROCKNVME
QEMU_P3   := qemu-system-i386 -cpu pentium3 -m 512 $(QEMU_DISKS)
QEMU_UEFI := qemu-system-x86_64 -m 1024 -bios /usr/share/ovmf/OVMF.fd $(QEMU_DISKS)
BOOT      := -drive file=$(OUT)/bedrock.img,format=raw,if=ide,index=0

qemu-p3: $(OUT)/bedrock.img $(DISKS)
	$(QEMU_P3) $(BOOT) -snapshot -nographic

qemu-uefi: $(OUT)/bedrock.img $(DISKS)
	$(QEMU_UEFI) $(BOOT) -snapshot -nographic

boottest: $(OUT)/bedrock.img $(DISKS)
	sh os/board/bedrock/boottest.sh $(OUT)/bedrock.img $(OUT)/boottest-p3.log $(OUT)/menu-bios.png $(QEMU_P3)
	sh os/board/bedrock/boottest.sh $(OUT)/bedrock.img $(OUT)/boottest-uefi.log $(OUT)/menu-uefi.png $(QEMU_UEFI)

endif
