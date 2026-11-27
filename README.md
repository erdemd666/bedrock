# Bedrock

A USB stick that tests, wipes and documents a used PC in one pass, from a
Pentium III to a current desktop, and leaves a signed record anyone can check.
built by erdem6.com

## What it does

- **Station run.** Plug in, boot, walk away. `bedrock.cfg` on the stick decides the
  steps; the PC ends on a large PASS or FAIL screen with a QR code, and one row is
  added to `inventory.csv` on the stick.
- **Hardware tests.** CPU burn-in that watches heat, clocks, throttling and wrong
  results (worn cooling, unstable or degraded silicon, Intel 13th/14th Gen microcode);
  GPU compute and video-memory test (Intel and AMD; NVIDIA where the open driver
  works); memory; SMART and surface scan; hidden disk areas (HPA/DCO); laptop
  battery wear.
- **Motherboard diagnosis.** Reads the errors the hardware has already reported
  (machine checks, memory ECC, PCIe, SATA, NVMe, USB), checks PCIe and SATA links,
  missing memory, voltages, CMOS battery and fans, and ranks the likely faulty part.
  For machines that do not start, `no-post-guide.html` on the stick explains beep,
  LED and POST codes.
- **Functional checklist.** Every key, touchpad and buttons, speakers left and right,
  microphone, webcam picture, each USB port, and whether Wi-Fi, Bluetooth and card
  readers are present.
- **Disk wipe with a certificate.** ATA Security Erase, NVMe Sanitize/Format or
  overwrite, read back to verify, then a certificate with the fields NIST SP 800-88
  Rev. 2 lists (Clear or Purge), signed with Ed25519. Check it at
  [erdem6.com/verify](https://erdem6.com/verify); see [docs/CERTIFICATE.md](docs/CERTIFICATE.md).
  Bedrock records those fields; it is not a certification of compliance.
- **Resale sheet.** A printable one-page spec and health sheet for listing the PC.
- **Fleet mode.** Boot many PCs over the network; each sends its record to one
  server with a dashboard and CSV export. See [fleet/README.md](fleet/README.md).

## Use it

Download `bedrock.img` from the latest release and write it to a USB stick with
Rufus (DD mode), balenaEtcher or `dd`. Boot from the stick.

- BIOS and UEFI machines (x64 and ia32) get the same menu: **Start Bedrock** or
  **Full memory test**. On BIOS the 64-bit or 32-bit build is picked automatically.
- When Bedrock is closed: Enter starts it again, R reboots, P powers off.

Bedrock runs on the screen and on COM1 (115200 baud). Reports are saved to the
stick's FAT partition (label `BEDROCK`), so you can read them on any computer.

`bedrock-v86.iso` is a small 32-bit BIOS-only build for the
[v86](https://copy.sh/v86) browser emulator: choose it as the CD image.

## Build it

Everything runs in Docker; the only host requirements are Docker and GNU make.

```sh
make img        # out/bedrock.img
make v86        # out/bedrock-v86.iso
make qemu-p3    # boot bedrock.img on a Pentium III with SeaBIOS (serial on stdio, Ctrl-A x quits)
make qemu-uefi  # boot bedrock.img on x86_64 with OVMF
make boottest   # headless report over serial on both, as CI runs it; menu screenshots in out/
make fleet      # out/fleet/: network-boot server and kernels, see fleet/README.md
make fleettest  # boot the x86_64 kernel in QEMU and check the server received its record
```

Without make (for example on Windows), run the same targets inside the build image:

```sh
docker build -t bedrock-build docker
docker run --rm -it -v "%cd%":/src -v bedrock-work:/work bedrock-build make img
```

The first build takes a while (toolchain, kernel). Buildroot's output and
downloads stay in the Docker volume `bedrock-work`, so later builds are incremental.

## For developers

- **Maintenance shell:** on the "Bedrock has closed" screen, type `maintain`
  (nothing is echoed). Leaving the shell starts Bedrock again.
- **Boot messages:** boot is silent (`quiet loglevel=0`). Run `dmesg` in the
  maintenance shell, or edit the kernel line in `os/board/bedrock/syslinux.cfg` /
  `grub.cfg` (for example `loglevel=7`). The serial console shows the same.
- **Headless report:** `bedrock.report` on the kernel command line runs
  `bedrock --report` once on COM1 before the UI; `make boottest` relies on it.
- **Look:** `os/board/bedrock/art.py` draws the menu background, the GRUB theme
  images and the boot splash at build time; `theme.txt` and `syslinux.cfg` hold
  the menu colours and layout.

## Layout

See [ARCHITECTURE.md](ARCHITECTURE.md).

- `tools/` the diagnostics program (C, static, musl)
- `fleet/` the network-boot fleet server (Go, standard library only)
- `os/` Buildroot external tree: defconfigs, kernel config and patch, boot menus and artwork, rootfs overlay, image scripts
- `docker/Dockerfile` pinned Buildroot, boot loaders, QEMU
- `.github/workflows/build.yml` on a `v*` tag: build, boot-test in QEMU, attach the images to the release

## Licence

MIT for the code in this repository (see LICENSE). The images also contain
Linux, BusyBox, SYSLINUX, GRUB 2 and memtest86+, which are GPL, and the
Terminus console font (SIL OFL).
