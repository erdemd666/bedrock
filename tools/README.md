# bedrock tools

The userland of Bedrock: one static binary, `bedrock`, with a text menu and every test.
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

`--report` exit status: 0 no failures, 1 at least one FAIL, 2 could not write.
Every check prints one of PASS, FAIL, INCONCLUSIVE or NOT PRESENT with its reason.

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
