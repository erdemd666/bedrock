# bedrock tools

The userland of Bedrock: one static binary, `bedrock`, with a text menu and every test.
`gpu/bedrock-gpu.c` is the one exception: a small dynamic program (EGL, GBM, OpenGL ES
3.1) for the GPU test, built by its own package for the 64-bit system only.
See `../ARCHITECTURE.md` for what each module does.

## Build

```sh
make                      # bedrock (static)
make CC=i686-linux-cc     # what the OS package does: make -C tools CC=$(TARGET_CC)
make test                 # host tests in tests/ (needs an x86 host)
make clean
```

`CFLAGS` defaults to `-O2 -Wall -Wextra`; `-std=c11 -D_GNU_SOURCE` are always added.
No SSE2 is assumed: SSE2/AVX paths are picked at run time from CPUID.

Without a local compiler, use Docker:

```sh
# x86_64
docker run --rm -v "$PWD:/src" -w /src alpine:3.20 \
  sh -c "apk add build-base linux-headers && make test"
# i686 (Alpine's 32-bit gcc defaults to pentium-m with SSE maths, so override it)
docker run --rm --platform linux/386 -v "$PWD:/src" -w /src alpine:3.20 \
  sh -c "apk add build-base linux-headers && make test CFLAGS='-O2 -Wall -Wextra -march=i686 -mtune=generic -mfpmath=387'"
```

The i686 Docker build checks that our code compiles without SSE2; the binary that
ships must be linked against the Buildroot i686 musl, because Alpine's own 32-bit
libc is built for Pentium M.

## Run

```sh
bedrock                  # menu
bedrock --report         # all non-destructive quick checks, plain text to stdout
bedrock --report FILE    # same, to FILE
```

```sh
bedrock --station        # the station run (menu key s, first in the menu): test, document and export this PC
```

## Station run

One pass over a PC: system info, disks and SMART, surface scan, CPU stress, memory,
wipe, as set in `bedrock.cfg` on the stick's FAT partition (shipped commented, see
`os/board/bedrock/bedrock.cfg`; `key = value`, whole-line `#` comments, LF or CRLF).
With `mode = station` the image runs it by itself at boot, before the menu. The steps
are the `steps[]` table in `src/station.c`; a new check is one line there.

The run ends on a full-screen PASS, FAIL or CHECK (inconclusive) with the asset tag,
model, serial, counts and a QR code. It leaves on the stick:

- `inventory.csv`: one row per run (RFC 4180, CRLF), header created when missing
- `bedrock/records/<asset tag or serial>-<yyyymmdd-hhmm>.json` and `.txt`
- the "Save a report" menu entry writes the same `.json` next to its `.txt`

`wipe = auto` erases every internal disk with no questions and only starts when
`wipe_confirm` is exactly `ERASE ALL INTERNAL DISKS`; the disk Bedrock runs from and
USB disks are skipped. The QR text is
`BEDROCK1|asset|serial|manufacturer product|result|date|record file`, at most 134 bytes.
`src/vendor/` holds Nayuki's QR Code generator (MIT, licence next to it).
With `bedrock.fleet=URL` on the boot line or `fleet_url = URL` in `bedrock.cfg`, `--report`
also POSTs the record to that fleet server (plain HTTP, LAN only; see `../fleet/README.md`)
and prints "Sent to fleet server" or the error. `bedrock --fleet-url` prints the URL, exit 1 if none.

`--report` exit status: 0 no failures, 1 at least one FAIL, 2 could not write.
Every check prints one of PASS, FAIL, INCONCLUSIVE or NOT PRESENT with its reason.

`make sample` rewrites `../docs/sample-certificate.json` (signed with the published sample key).

## Vendored code (`src/vendor/`)

Monocypher 4.0.2 (`monocypher.c/.h`, and `monocypher-ed25519.c/.h` for RFC 8032 Ed25519 with
SHA-512), copied unchanged from the official release tarball `monocypher-4.0.2.tar.gz`
(SHA-256 `38d07179738c0c90677dba3ceb7a7b8496bcfea758ba1a53e803fed30ae0879c`). Licence:
`src/vendor/MONOCYPHER-LICENCE.md` (BSD-2-Clause or CC0).

## Test fixtures (`tests/fixtures/`)

| File | Origin |
|---|---|
| `cpuid_p3.bin`, `cpuid_zen3.bin` | Built from documented CPUID values (Pentium III 0x683, Ryzen 7 5800X) |
| `cpuid_i5-13500h.bin` | Captured from an i5-13500H (Hyper-V guest) |
| `smbios_qemu.bin` | `/sys/firmware/dmi/tables/DMI` from QEMU q35 with `-smbios` strings set |
| `ata_smart_qemu.bin` | SMART READ DATA from a QEMU AHCI disk, read by `sat_cmd()` |
| `nvme_smart_qemu.bin` | NVMe log page 02h from a QEMU NVMe controller, read by `nvme_smart_log()` |
| `latency_us.bin` | Read latencies (uint32 microseconds, `0xffffffff` = error) on every band edge |
| `pci.ids` | Excerpt in pci.ids format |
| `klog.txt` | Kernel log lines in the exact formats of Linux 6.12: machine checks, AER, libata, NVMe, USB, EDAC, ACPI noise |

CPUID fixtures are records of six little-endian uint32: leaf, subleaf, eax, ebx, ecx, edx.
Failing SMART cases are made in the test by patching the captured sectors.
