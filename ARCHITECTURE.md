# Bedrock — architecture

Bootable USB hardware diagnostics. Plug it into anything from a Pentium III to a
current desktop, boot it, and find out what is broken: RAM, CPU, disks, GPU,
motherboard. built by erdem6.com

## Goals

1. **Boots everywhere.** BIOS and UEFI, 32-bit and 64-bit, Pentium III (1999) to today.
2. **Correct before fast.** Every test has a known-good answer it checks against.
   A test that cannot prove a fault says "inconclusive", never "pass".
3. **Read-only by default.** Nothing writes to a disk except the wipe tool,
   and that one asks for the disk's serial number before it starts.
4. **One file to flash.** `bedrock.img`, written with Rufus, balenaEtcher or `dd`.

## Non-goals (v1)

- A GPU stress test on the 32-bit system. The 64-bit system carries graphics
  drivers (loaded only when the GPU test runs), Mesa and GPU firmware for it.
- Networking, a desktop, a package manager.
- Windows tools. Everything runs from the USB stick.

## Layers

```
┌──────────────────────────────────────────────────────────┐
│ bedrock (C, static, musl) — text UI + all tests          │
├──────────────────────────────────────────────────────────┤
│ Linux kernel (minimal config) + busybox init             │
├──────────────────────────────────────────────────────────┤
│ Boot: SYSLINUX (BIOS) · GRUB2 (UEFI x64 + ia32)          │
│       + memtest86+ entry for full-coverage RAM testing   │
└──────────────────────────────────────────────────────────┘
```

Why Linux underneath and not bare metal: disk controllers (IDE, AHCI, NVMe, USB
mass storage), USB keyboards and framebuffers on 25 years of hardware are
millions of lines of driver code. Linux already has them. Everything the user
sees and every test is ours.

## One product on screen

From the boot menu to power-off the user sees Bedrock, not the layers under it:

- **Menus:** SYSLINUX `vesamenu` (BIOS) and a GRUB theme (UEFI) with the same
  artwork, drawn at build time by `os/board/bedrock/art.py`, and the same two
  entries: *Start Bedrock*, *Full memory test*.
- **Boot:** `quiet loglevel=0`, no decompressor messages, no logo, no cursor.
  `rcS` shows the splash with busybox `fbsplash` (centred, 2x on screens ≥1600 px
  wide) or as text on a VGA text console. Terminus 16x32 replaces the 8x16 font
  on screens ≥1600 px wide.
- **Identity:** hostname, `/etc/os-release`, `/etc/issue` and `uname`
  (one-line kernel patch in `os/board/bedrock/patches/`) all say Bedrock.
- **Exit:** `bedrock-run` shows "Bedrock has closed" with restart, reboot and
  power off; `bedrock-off` unmounts the report partition and reboots or powers
  off without console messages. The maintenance shell is behind a typed word
  (see README).

Still visible and outside our code: the firmware's own screens, the one-line
SYSLINUX banner before the BIOS menu, the BIOS text cursor while SYSLINUX loads
the kernel, and memtest86+'s own UI.

## Two builds, one stick

| Kernel | Runs on | Why |
|---|---|---|
| i686, non-PAE | Pentium III, Pentium M, Atom, any 32-bit-only CPU | PAE is missing on early Pentium M; i686 is the floor |
| x86_64 | every 64-bit CPU, all UEFI x64 machines | sees all RAM (i686 non-PAE stops at 4 GB) |

BIOS boot picks automatically with SYSLINUX `ifcpu64.c32`. UEFI x64 always boots
x86_64; UEFI ia32 (some Atom tablets) boots i686.

Userland is compiled twice: `-march=i686 -mtune=generic` (no SSE2 assumed) and
`x86_64`. SSE/AVX code paths are chosen at runtime from CPUID.

## Modules

All code in `tools/src/`. Each module is one `.c` file with a `run_*()` entry and
a pure parser that the host test suite feeds with captured fixtures.

| Module | Reads from | What it does |
|---|---|---|
| `cpu.c` | `cpuid`, `/sys/devices/system/cpu`, hwmon | Decode vendor/family/model/stepping, caches, flags. Per-core stress with workloads whose result is known (integer sieve checksum, FP matrix product, SSE path if present). Watch temperature and clock while it runs; flag throttling and machine-check errors. |
| `mem.c` | `mlock`'d memory | memtester-style passes over as much free RAM as can be locked: stuck address, walking 1s/0s, moving inversions, random with fixed seed, checkerboard. Reports failing physical address via `/proc/self/pagemap`. For full coverage (memory under the kernel) the boot menu offers memtest86+. |
| `disk.c` | `/sys/block`, `SG_IO`, NVMe admin ioctl | Identify (model, serial, firmware, capacity). Read-only surface scan with `O_DIRECT`: latency histogram (<5, <20, <50, <150, <500, >500 ms, error) — the classic way to spot a dying drive before SMART does. |
| `smart.c` | ATA SMART READ DATA / NVMe SMART log | Own parser for the attributes that predict failure: reallocated, pending, uncorrectable sectors, CRC errors, power-on hours, NVMe percentage used / media errors. Health verdict with the reason shown. |
| `board.c` | `/sys/firmware/dmi/tables` (SMBIOS), hwmon, RTC | Board vendor/model, BIOS version and date, every DIMM slot (size, type, speed, part number), voltages and fans, CMOS battery sanity (RTC year/time drift). |
| `cpuage.c` | hwmon, cpufreq, RAPL, throttle counters, `/dev/kmsg`, microcode | Burn-in of 2, 10 or 30 minutes on every core with the checked workloads, sampling clocks, temperature, package power and throttling. Separate verdicts for wrong results, worn cooling (throttling, still heating at the end), clocks held below base without heat, one lagging core, machine-check reports, and Intel 13th/14th Gen microcode older than 0x12B. |
| `gpustress.c` | PCI, DRM render nodes, card hwmon | Loads the card's driver, runs `bedrock-gpu` (tools/gpu: headless OpenGL ES 3.1 compute on GBM, results checked against a CPU reference, video memory written and read back with three patterns) and watches edge/hot-spot/memory temperatures, power and fan. The software renderer never counts as a GPU test. |
| `gpu.c` | PCI config space, DRM/fbdev | Vendor/device from `pci.ids`, VRAM BAR size, PCIe link width/speed vs maximum. Full-screen display test: solid colours for dead pixels, gradients for banding. |
| `diag.c` | system error log, PCIe/SATA links, SMBIOS, hwmon, RTC | Motherboard diagnosis: classifies hardware errors already reported (machine checks, memory ECC, PCIe AER, SATA, NVMe, USB, thermal), compares links and installed memory with what the system can use, judges voltage rails with a known scale, and ranks the likely faulty parts. `no-post-guide.html` on the stick covers machines that never get that far. |
| `wipe.c` | block device | Zero pass, random pass, ATA Secure Erase, NVMe Format/Sanitize. Refuses the boot stick. Requires typing the disk serial. Verifies by reading back a sample. |
| `cert.c` | the record in memory, the stick's key | Signed records (docs/CERTIFICATE.md): Ed25519 key per stick, sanitization certificates after each wipe, health records, a printable page for each and a one-page resale sheet. |
| `report.c` | results of the above | Writes `bedrock-<board>-<date>.txt` to the stick's FAT partition so the result leaves with the user. |
| `station.c` | `bedrock.cfg` on the stick | The unattended run: a `steps[]` table of checks, a full-screen PASS/FAIL result with a QR code, one row in `inventory.csv`, and `bedrock/records/*.json` + `.txt` on the stick. `wipe = auto` needs an exact confirmation text and never touches the boot stick. |
| `ui.c` | — | Plain VGA/fbcon text UI, no ncurses dependency: works on a 1999 graphics card and over a serial console. Header on every screen: `BEDROCK · built by erdem6.com`. |

## Repository layout

```
bedrock/
  tools/                 C sources, Makefile, host tests (tests/*.c + fixtures/)
  os/                    Buildroot external tree (BR2_EXTERNAL)
    configs/             bedrock_i686_defconfig, bedrock_x86_64_defconfig
    board/bedrock/       boot menus + artwork, kernel config + patch, rootfs overlay
    package/bedrock/     Buildroot package that builds tools/
  docker/Dockerfile      pinned Buildroot + QEMU build image
  Makefile               `make img`, `make test`, `make qemu-p3`, `make qemu-uefi`
  .github/workflows/     build image on tag, attach bedrock.img to the GitHub release
```

## Testing

- **Host unit tests** for every parser (CPUID decode, SMBIOS, SMART, latency
  histogram) using captured binary fixtures. Run in CI on every push.
- **QEMU boot matrix** in CI: `-cpu pentium3` + SeaBIOS, `-cpu qemu64` + OVMF
  (UEFI), with a virtual IDE disk, AHCI disk and NVMe disk. The image must reach
  the menu and produce a report over the serial console.
- **Real hardware** before each release: one pre-2008 machine, one modern UEFI machine.

## Milestones

| Version | Ships |
|---|---|
| 0.1 | Boots on the QEMU matrix. System info (CPU, board, DIMMs, PCI), memtest86+ entry, disk identify + SMART. |
| 0.2 | Disk surface scan, CPU stress, in-OS RAM test, report file. |
| 0.3 | Wipe (with serial confirmation), GPU/display test. |
| 1.0 | Tested on real old and new hardware, release on GitHub, page on erdem6.com. |

## Licensing

Own code: MIT. The image also contains Linux, busybox, SYSLINUX, GRUB and
memtest86+ (GPL); their licences and sources are linked from the release notes,
as the GPL requires.
