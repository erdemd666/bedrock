# Build + test image for Bedrock: pinned Buildroot, boot-loader binaries, QEMU.
FROM debian:trixie-20261104-slim

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential bc bzip2 ca-certificates cpio file git patch perl python3 rsync unzip wget xz-utils \
      dosfstools fdisk mtools xorriso \
      syslinux syslinux-common syslinux-utils isolinux \
      grub-common grub-efi-amd64-bin grub-efi-ia32-bin \
      memtest86+ \
      python3-pil fonts-dejavu-core console-setup-linux \
      qemu-system-x86 qemu-utils ovmf \
    && rm -rf /var/lib/apt/lists/*

ARG BR_VERSION=2025.02.20
ARG BR_SHA256=fa79fa606387b3d485a6cfd30eb2302b2cc30775ffb668f2a1269ff4c9674b6b
RUN wget -q https://buildroot.org/downloads/buildroot-${BR_VERSION}.tar.xz \
    && echo "${BR_SHA256}  buildroot-${BR_VERSION}.tar.xz" | sha256sum -c - \
    && tar -xf buildroot-${BR_VERSION}.tar.xz -C /opt \
    && mv /opt/buildroot-${BR_VERSION} /opt/buildroot \
    && rm buildroot-${BR_VERSION}.tar.xz

# Builds run as root inside the container; Buildroot's host-tar etc. need this.
ENV FORCE_UNSAFE_CONFIGURE=1 \
    BR2_DL_DIR=/work/dl
WORKDIR /src
