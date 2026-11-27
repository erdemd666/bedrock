/* Host tests for the pure parsers. Run from tests/: fixtures are read from fixtures/. */
#include "bedrock.h"
#include "vendor/qrcodegen.h"
#include "vendor/monocypher-ed25519.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "%s:%d: FAILED: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint8_t *load(const char *name, size_t *n)
{
	char p[256];
	static uint8_t buf[1 << 16];
	snprintf(p, sizeof p, "fixtures/%s", name);
	FILE *f = fopen(p, "rb");
	if (!f) {
		fprintf(stderr, "missing fixture %s\n", p);
		exit(1);
	}
	*n = fread(buf, 1, sizeof buf, f);
	fclose(f);
	return buf;
}

static int has_flag(const char *flags, const char *f)
{
	size_t n = strlen(f);
	for (const char *s = flags; (s = strstr(s, f)); s++)
		if ((s == flags || s[-1] == ' ') && (s[n] == ' ' || !s[n]))
			return 1;
	return 0;
}

static struct cpu_info decode(const char *fixture)
{
	struct cpu_info ci;
	size_t n;
	const struct cpuid_leaf *l = (const void *)load(fixture, &n);
	CHECK(cpu_decode(l, n / sizeof *l, &ci) == 0);
	return ci;
}

static void test_cpuid(void)
{
	char fl[256];
	struct cpu_info ci = decode("cpuid_p3.bin");
	CHECK(!strcmp(ci.vendor, "GenuineIntel"));
	CHECK(ci.family == 6 && ci.model == 8 && ci.stepping == 3);
	CHECK(ci.brand[0] == 0); /* no extended leaves: the garbage 0x80000000 must be ignored */
	CHECK(ci.l1d == 0 && ci.l2 == 0 && ci.l3 == 0);
	cpu_flags(&ci, fl, sizeof fl);
	CHECK(has_flag(fl, "sse") && has_flag(fl, "cmov") && has_flag(fl, "pae"));
	CHECK(!has_flag(fl, "sse2") && !has_flag(fl, "lm") && !has_flag(fl, "nx"));

	ci = decode("cpuid_zen3.bin");
	CHECK(!strcmp(ci.vendor, "AuthenticAMD"));
	CHECK(ci.family == 0x19 && ci.model == 0x21 && ci.stepping == 0); /* extended family/model */
	CHECK(!strcmp(ci.brand, "AMD Ryzen 7 5800X 8-Core Processor"));
	CHECK(ci.l1d == 32 && ci.l1i == 32 && ci.l2 == 512 && ci.l3 == 32768);
	cpu_flags(&ci, fl, sizeof fl);
	CHECK(has_flag(fl, "avx2") && has_flag(fl, "sse2") && has_flag(fl, "lm") && has_flag(fl, "nx"));
	CHECK(!has_flag(fl, "avx512f") && !has_flag(fl, "hypervisor"));

	ci = decode("cpuid_i5-13500h.bin"); /* captured in a Hyper-V guest */
	CHECK(!strcmp(ci.vendor, "GenuineIntel"));
	CHECK(ci.family == 6 && ci.model == 186 && ci.stepping == 2);
	CHECK(!strcmp(ci.brand, "13th Gen Intel(R) Core(TM) i5-13500H"));
	CHECK(ci.l1d == 48 && ci.l1i == 32 && ci.l2 == 1280 && ci.l3 == 18432); /* leaf 4 */
	cpu_flags(&ci, fl, sizeof fl);
	CHECK(has_flag(fl, "avx2") && has_flag(fl, "hypervisor"));

	CHECK(cpu_decode(NULL, 0, &ci) == -1);
}

static void test_workloads(void)
{
	CHECK(work_sieve() == SIEVE_PRIMES);
	CHECK(work_matrix() == MATRIX_SUM);
	if (__builtin_cpu_supports("sse2"))
		CHECK(work_matrix_sse2() == MATRIX_SUM);
	if (__builtin_cpu_supports("avx"))
		CHECK(work_matrix_avx() == MATRIX_SUM);
}

static void test_smbios(void)
{
	static struct smbios s;
	size_t n;
	const uint8_t *t = load("smbios_qemu.bin", &n); /* QEMU q35 with -smbios strings set */
	CHECK(smbios_parse(t, n, &s) >= 9);
	CHECK(!strcmp(s.bios_vendor, "Probe6BIOS") && !strcmp(s.bios_ver, "1.2.3") && !strcmp(s.bios_date, "03/15/2021"));
	CHECK(!strcmp(s.sys_maker, "ProbeSys") && !strcmp(s.sys_product, "P6-Test") && !strcmp(s.sys_serial, "SYS123"));
	CHECK(!strcmp(s.board_maker, "ProbeBoard") && !strcmp(s.board_product, "P6-MB") && !strcmp(s.board_ver, "rev1.0"));
	CHECK(!strcmp(s.cpu_socket, "CPUSOCK 0") && !strcmp(s.cpu_ver, "P6-CPU"));
	CHECK(s.cpu_max_mhz == 3000 && s.cpu_cur_mhz == 2000 && s.cpu_cores == 2 && s.cpu_threads == 2);
	CHECK(s.nslots == 1);
	CHECK(!strcmp(s.dimm[0].loc, "DIMM 0") && !strcmp(s.dimm[0].bank, "BANK0"));
	CHECK(!strcmp(s.dimm[0].maker, "Kingston") && !strcmp(s.dimm[0].part, "KF3200C16") && !strcmp(s.dimm[0].serial, "D1234"));
	CHECK(s.dimm[0].mb == 1024 && s.dimm[0].speed == 3200 && !strcmp(s.dimm[0].type, "RAM"));

	/* Every truncation must parse without reading past the end (run under valgrind to prove it). */
	for (size_t len = 0; len <= n; len++) {
		uint8_t *cut = malloc(len ? len : 1);
		memcpy(cut, t, len);
		smbios_parse(cut, len, &s);
		free(cut);
	}

	/* Type 17 edge cases: extended size, empty slot, short v2.1 structure, KB units. */
	uint8_t b[256] = {0}, *p = b;
	p[0] = 17, p[1] = 0x28, p[0x0c] = 0xff, p[0x0d] = 0x7f, p[0x10] = 1, p[0x12] = 0x1a;
	p[0x15] = 3200 & 0xff, p[0x16] = 3200 >> 8, p[0x17] = 2, p[0x1a] = 3, p[0x1c] = 0x00, p[0x1d] = 0x80;
	p += 0x28;
	memcpy(p, "DIMM_A1\0Samsung\0M393A4K40DB3\0\0", 30);
	p += 30;
	p[0] = 17, p[1] = 0x15, p[0x10] = 5; /* size 0 = empty; string 5 does not exist */
	p += 0x15 + 2;
	p[0] = 17, p[1] = 0x15, p[0x0c] = 0x00, p[0x0d] = 0x88; /* 0x8800: 2048 KB */
	p += 0x15 + 2;
	p[0] = 127, p[1] = 4;
	p += 6;
	CHECK(smbios_parse(b, p - b, &s) == 4);
	CHECK(s.nslots == 3);
	CHECK(s.dimm[0].mb == 32768 && s.dimm[0].speed == 3200 && !strcmp(s.dimm[0].type, "DDR4"));
	CHECK(!strcmp(s.dimm[0].loc, "DIMM_A1") && !strcmp(s.dimm[0].maker, "Samsung") && !strcmp(s.dimm[0].part, "M393A4K40DB3"));
	CHECK(s.dimm[0].bank[0] == 0);
	CHECK(s.dimm[1].mb == 0 && s.dimm[1].loc[0] == 0 && s.dimm[1].speed == 0);
	CHECK(s.dimm[2].mb == 2);
}

static void fix_checksum(uint8_t *s)
{
	uint8_t sum = 0;
	for (int i = 0; i < 511; i++)
		sum += s[i];
	s[511] = -sum;
}

static void set_attr(uint8_t *s, int slot, uint8_t id, uint32_t raw)
{
	uint8_t *e = s + 2 + 12 * slot;
	e[0] = id;
	e[5] = raw, e[6] = raw >> 8, e[7] = raw >> 16, e[8] = raw >> 24;
	fix_checksum(s);
}

static void test_ata_smart(void)
{
	struct ata_smart a;
	char why[512];
	size_t n;
	uint8_t s[512];
	memcpy(s, load("ata_smart_qemu.bin", &n), 512); /* SMART READ DATA from QEMU's AHCI disk */
	CHECK(n == 512);
	smart_ata_parse(s, &a);
	CHECK(a.checksum_ok && a.realloc == 0 && a.hours == 1 && a.temp == 31);
	CHECK(a.pending == -1 && a.uncorrect == -1 && a.crc == -1);
	CHECK(smart_ata_verdict(&a, why, sizeof why) == V_PASS && strstr(why, "reallocated") && !strstr(why, "pending"));

	set_attr(s, 7, 197, 8);
	smart_ata_parse(s, &a);
	CHECK(a.pending == 8);
	CHECK(smart_ata_verdict(&a, why, sizeof why) == V_FAIL && strstr(why, "8 sectors pending"));

	memcpy(s, load("ata_smart_qemu.bin", &n), 512);
	set_attr(s, 7, 199, 3);
	smart_ata_parse(s, &a);
	CHECK(smart_ata_verdict(&a, why, sizeof why) == V_FAIL && strstr(why, "3 interface CRC"));

	s[100] ^= 1; /* corrupt: checksum no longer matches */
	smart_ata_parse(s, &a);
	CHECK(!a.checksum_ok && smart_ata_verdict(&a, why, sizeof why) == V_INCONCLUSIVE);

	memset(s, 0, sizeof s); /* valid checksum, no attributes */
	smart_ata_parse(s, &a);
	CHECK(smart_ata_verdict(&a, why, sizeof why) == V_INCONCLUSIVE);
}

static void test_nvme_smart(void)
{
	struct nvme_smart m;
	char why[512];
	size_t n;
	uint8_t s[512];
	memcpy(s, load("nvme_smart_qemu.bin", &n), 512); /* log page 02h from QEMU's NVMe controller */
	smart_nvme_parse(s, &m);
	CHECK(m.crit == 0 && m.temp_k == 323 && m.used == 0 && m.media_errors == 0);
	CHECK(smart_nvme_verdict(&m, why, sizeof why) == V_PASS);

	s[0] = 0x04; /* reliability degraded */
	s[160] = 3;  /* media errors */
	smart_nvme_parse(s, &m);
	CHECK(m.media_errors == 3);
	CHECK(smart_nvme_verdict(&m, why, sizeof why) == V_FAIL && strstr(why, "reliability degraded") && strstr(why, "3 media"));

	memcpy(s, load("nvme_smart_qemu.bin", &n), 512);
	s[5] = 100;
	smart_nvme_parse(s, &m);
	CHECK(smart_nvme_verdict(&m, why, sizeof why) == V_FAIL && strstr(why, "endurance used up"));

	s[168] = 1; /* upper half of the 128-bit counter: saturate, do not wrap */
	smart_nvme_parse(s, &m);
	CHECK(m.media_errors == ~0ULL);
}

static void test_histogram(void)
{
	struct hist h = {{0}};
	char why[256];
	size_t n;
	const uint8_t *b = load("latency_us.bin", &n);
	for (size_t i = 0; i + 4 <= n; i += 4) {
		uint32_t us;
		memcpy(&us, b + i, 4);
		hist_add(&h, us);
	}
	static const unsigned long long want[7] = {3, 2, 2, 2, 2, 2, 2}; /* band edges belong to the slower band */
	CHECK(!memcmp(h.b, want, sizeof want));
	CHECK(hist_verdict(&h, why, sizeof why) == V_FAIL && strstr(why, "2 unreadable"));

	struct hist slow = {{10, 0, 0, 0, 0, 1, 0}}, ok = {{100, 5, 3, 2, 1, 0, 0}}, none = {{0}};
	CHECK(hist_verdict(&slow, why, sizeof why) == V_FAIL);
	CHECK(hist_verdict(&ok, why, sizeof why) == V_PASS);
	CHECK(hist_verdict(&none, why, sizeof why) == V_INCONCLUSIVE);
}

static void test_wipe_refusal(void)
{
	CHECK(dev_on_disk("sda", "sda") && dev_on_disk("/dev/sda1", "sda") && dev_on_disk("sda15", "sda"));
	CHECK(!dev_on_disk("sdaa1", "sda") && !dev_on_disk("sdb1", "sda") && !dev_on_disk("sd", "sda"));
	CHECK(!dev_on_disk("sda1x", "sda") && !dev_on_disk("", "sda") && !dev_on_disk("sda1", ""));
	CHECK(dev_on_disk("nvme0n1p2", "nvme0n1") && dev_on_disk("/dev/nvme0n1", "nvme0n1"));
	CHECK(!dev_on_disk("nvme0n10", "nvme0n1") && !dev_on_disk("nvme0n1p", "nvme0n1"));
	CHECK(dev_on_disk("mmcblk0p1", "mmcblk0") && !dev_on_disk("mmcblk0boot0", "mmcblk0"));

	const char *mounts =
		"rootfs / rootfs rw 0 0\n"
		"proc /proc proc rw,nosuid 0 0\n"
		"devtmpfs /dev devtmpfs rw 0 0\n"
		"/dev/sdb1 /media/probe6 vfat rw,relatime 0 0\n"
		"/dev/loop0 /lib/modules squashfs ro 0 0";
	CHECK(boot_disk_refuse("sdb", mounts, ""));     /* the stick's FAT partition is mounted */
	CHECK(!boot_disk_refuse("sda", mounts, ""));
	CHECK(!boot_disk_refuse("nvme0n1", mounts, ""));
	CHECK(boot_disk_refuse("sda", "", "BOOT_IMAGE=/vmlinuz root=/dev/sda2 quiet"));
	CHECK(!boot_disk_refuse("sda", "", "BOOT_IMAGE=/vmlinuz noroot=/dev/sda2 quiet"));
	CHECK(!boot_disk_refuse("sda", "", "root=UUID=1234-5678")); /* left to the device-number check */
	CHECK(boot_disk_refuse("nvme0n1", "Filename Type Size Used Priority\n/dev/nvme0n1p3 partition 1000 0 -2\n", ""));
	CHECK(!boot_disk_refuse("sda", "", ""));
}

static void test_ata_security(void)
{
	/* IDENTIFY DEVICE words 82/85/89/90/128, ACS-3 7.12.7 (IDENTIFY DEVICE data). */
	uint16_t id[256] = {0};
	struct ata_sec s;
	ata_sec_decode(id, &s); /* QEMU IDE/AHCI: no security feature set */
	CHECK(!s.supported && !s.enabled && !s.frozen && !s.enhanced && s.normal_min == 0 && s.enhanced_min == 0);

	id[82] = 0x0002, id[128] = 0x0029, id[89] = 0x0001, id[90] = 0x00ff; /* supported, frozen, enhanced */
	ata_sec_decode(id, &s);
	CHECK(s.supported && !s.enabled && !s.locked && s.frozen && !s.expired && s.enhanced);
	CHECK(s.normal_min == 2 && s.enhanced_min == -508); /* 255 = more than 508 min */

	id[85] = 0x0002, id[128] = 0x0017, id[89] = 0x803c, id[90] = 0xffff; /* enabled, locked, count expired */
	ata_sec_decode(id, &s);
	CHECK(s.enabled && s.locked && !s.frozen && s.expired && !s.enhanced);
	CHECK(s.normal_min == 120 && s.enhanced_min == -65532); /* bit 15: extended format, 7FFFh = more than 65532 min */

	id[89] = 0x0100, id[90] = 0x00fe; /* bits 14:8 reserved in the short format */
	ata_sec_decode(id, &s);
	CHECK(s.normal_min == 0 && s.enhanced_min == 508);

	id[82] = 0xffff; /* word not valid */
	ata_sec_decode(id, &s);
	CHECK(!s.supported && !s.enabled && !s.locked && !s.frozen);

	/* ATA PASS-THROUGH(16), SAT-4 ATA PASS-THROUGH(16) command: PROTOCOL 3 non-data, 4 PIO in, 5 PIO out. */
	uint8_t cdb[16];
	static const uint8_t ident[16] = {0x85, 0x08, 0x0e, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0xec, 0};
	static const uint8_t smart[16] = {0x85, 0x08, 0x0e, 0, 0xd0, 0, 1, 0, 0, 0, 0x4f, 0, 0xc2, 0, 0xb0, 0};
	static const uint8_t prep[16] = {0x85, 0x06, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xf3, 0};
	sat_cdb(cdb, SAT_IN, 0xec, 0, 0, 0);
	CHECK(!memcmp(cdb, ident, 16));
	sat_cdb(cdb, SAT_IN, 0xb0, 0xd0, 0x4f, 0xc2);
	CHECK(!memcmp(cdb, smart, 16));
	sat_cdb(cdb, SAT_NONE, 0xf3, 0, 0, 0); /* SECURITY ERASE PREPARE */
	CHECK(!memcmp(cdb, prep, 16));
	static const uint8_t out_cmds[] = {0xf1, 0xf2, 0xf4, 0xf6}; /* SET PASSWORD, UNLOCK, ERASE UNIT, DISABLE PASSWORD */
	for (int i = 0; i < 4; i++) {
		uint8_t want[16] = {0x85, 0x0a, 0x06, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, out_cmds[i], 0};
		sat_cdb(cdb, SAT_OUT, out_cmds[i], 0, 0, 0);
		CHECK(!memcmp(cdb, want, 16));
	}

	uint8_t b[512], zero[512] = {0};
	ata_sec_data(b, 0);
	CHECK(b[0] == 0 && b[1] == 0 && !memcmp(b + 2, SEC_PW, strlen(SEC_PW)) && !memcmp(b + 2 + strlen(SEC_PW), zero, 510 - strlen(SEC_PW)));
	ata_sec_data(b, 1);
	CHECK(b[0] == 2 && b[1] == 0); /* word 0 bit 1: enhanced erase, bit 0 clear: user password */
}

static void test_nvme_erase(void)
{
	/* NVMe 1.4 Identify Controller (OACS 256, SANICAP 328, FNA 524) and Identify Namespace (FLBAS 26, DPS 29). */
	static uint8_t c[4096], ns[4096];
	struct nvme_erase e;
	nvme_erase_plan(c, ns, 1, &e);
	CHECK(!e.name && !e.sanitize && !e.format);

	c[256] = 0x02, ns[26] = 0x01; /* Format NVM supported (QEMU), LBA format 1 */
	nvme_erase_plan(c, ns, 1, &e);
	CHECK(!e.sanitize && e.format == (1 | 1 << 9) && e.format_nsid == 1 && !e.all_ns && strstr(e.name, "user data"));

	c[524] = 0x04; /* FNA: crypto erase supported -> SES 2 */
	nvme_erase_plan(c, ns, 1, &e);
	CHECK(e.format == (1 | 2 << 9) && e.format_nsid == 1 && !e.all_ns && strstr(e.name, "crypto"));

	c[524] = 0x05; /* format applies to all namespaces: NSID FFFFFFFFh */
	nvme_erase_plan(c, ns, 3, &e);
	CHECK(e.format_nsid == 0xffffffff && e.all_ns);
	c[524] = 0x02; /* secure erase applies to all namespaces, format itself per namespace */
	nvme_erase_plan(c, ns, 3, &e);
	CHECK(e.format_nsid == 3 && e.all_ns && e.format == (1 | 1 << 9));

	c[524] = 0, ns[26] = 0x72, ns[29] = 0x09; /* MSET, LBAF 2, upper LBAF 3; PI type 1 in first bytes */
	nvme_erase_plan(c, ns, 1, &e);
	CHECK(e.format == (0x12 | 1 << 5 | 1 << 8 | 1 << 9 | 3 << 12));

	c[328] = 0x04; /* SANICAP: overwrite only -> still Format */
	nvme_erase_plan(c, ns, 1, &e);
	CHECK(!e.sanitize && e.format);
	c[328] = 0x06; /* block erase + overwrite */
	nvme_erase_plan(c, ns, 1, &e);
	CHECK(e.sanitize == 2 && !e.format && e.all_ns && strstr(e.name, "block"));
	c[328] = 0x07; /* crypto erase preferred */
	nvme_erase_plan(c, ns, 1, &e);
	CHECK(e.sanitize == 4 && e.all_ns && strstr(e.name, "crypto"));

	/* Sanitize Status log 81h, NVMe 1.4: SPROG bytes 1:0, SSTAT bytes 3:2. */
	uint8_t log[512] = {0x00, 0x80, 0x02, 0x00};
	double pct;
	CHECK(nvme_sanitize_status(log, &pct) == 2 && pct == 50.0);
	log[0] = log[1] = 0xff, log[2] = 0x01, log[3] = 0x01; /* completed, global data erased */
	CHECK(nvme_sanitize_status(log, &pct) == 1);
	log[2] = 0x0b; /* 1 overwrite pass done (bits 7:3), failed */
	CHECK(nvme_sanitize_status(log, &pct) == 3);
	log[2] = 0x04;
	CHECK(nvme_sanitize_status(log, &pct) == 4);
}

static void test_erase_verify(void)
{
	enum { N = 4096 };
	static uint8_t before[N], after[N];
	char why[256];
	for (int i = 0; i < N; i++)
		before[i] = i * 7 + 3;
	uint64_t h = sample_hash(before, N);
	struct erase_tally t = {0};

	memset(after, 0, N);
	erase_tally_add(&t, after, N, h);
	memset(after, 0xff, N);
	erase_tally_add(&t, after, N, h);
	for (int i = 0; i < N; i++) /* crypto erase: noise, but not the old data */
		after[i] = i * 13 + 1;
	erase_tally_add(&t, after, N, h);
	erase_tally_add(&t, after, N, sample_hash(after, N) ^ 1);
	CHECK(t.zero == 1 && t.filled == 1 && t.fill == 0xff && t.changed == 2 && !t.unchanged && !t.unread);
	CHECK(erase_tally_verdict(&t, why, sizeof why) == V_PASS && strstr(why, "0xff"));

	memset(before, 0, N); /* blank before and after: zeros pass */
	struct erase_tally z = {0};
	erase_tally_add(&z, before, N, sample_hash(before, N));
	CHECK(z.zero == 1 && erase_tally_verdict(&z, why, sizeof why) == V_PASS);

	erase_tally_add(&t, after, N, 0); /* pre-erase read failed: cannot compare */
	CHECK(t.unread == 1 && erase_tally_verdict(&t, why, sizeof why) == V_INCONCLUSIVE);
	erase_tally_add(&t, NULL, N, h); /* unreadable after the erase */
	CHECK(t.unread == 2 && erase_tally_verdict(&t, why, sizeof why) == V_INCONCLUSIVE);
	erase_tally_add(&t, after, N, sample_hash(after, N)); /* old data survived */
	CHECK(t.unchanged == 1 && erase_tally_verdict(&t, why, sizeof why) == V_FAIL);

	struct erase_tally none = {0};
	CHECK(erase_tally_verdict(&none, why, sizeof why) == V_INCONCLUSIVE);
}

static void test_pciids(void)
{
	char v[128], d[160];
	FILE *f = fopen("fixtures/pci.ids", "r");
	CHECK(f != NULL);
	if (!f)
		return;
	CHECK(pciids_lookup(f, 0x10de, 0x1b80, v, sizeof v, d, sizeof d) == 2);
	CHECK(!strcmp(v, "NVIDIA Corporation") && !strcmp(d, "GP104 [GeForce GTX 1080]"));
	CHECK(pciids_lookup(f, 0x1234, 0x1111, v, sizeof v, d, sizeof d) == 2 && !strcmp(d, "QEMU Virtual Video Controller"));
	CHECK(pciids_lookup(f, 0x1002, 0x0b37, v, sizeof v, d, sizeof d) == 1 && d[0] == 0); /* subsystem line is not a device */
	CHECK(pciids_lookup(f, 0x8086, 0x1234, v, sizeof v, d, sizeof d) == 0 && v[0] == 0);
	fclose(f);
}

static void test_mem_patterns(void)
{
	size_t n = (1 << 20) / sizeof(unsigned long);
	unsigned long *p = malloc(n * sizeof *p);
	CHECK(p && mem_patterns(p, n, NULL) == 0); /* good RAM: no false positives */
	free(p);
}

static void test_record(void)
{
	char *buf = NULL;
	size_t len = 0;
	record_reset();
	fact("system.serial", "AB\"C\\ %d  ", 7);
	CHECK(!strcmp(fact_get("system.serial"), "AB\"C\\ 7")); /* trailing padding trimmed */
	fact("system.serial", "SN-%d", 42); /* same key replaces */
	record_result(V_FAIL, "Memory", "3 bit errors\n\x01");
	CHECK(g_nfact == 1 && !strcmp(fact_get("system.serial"), "SN-42"));
	FILE *m = open_memstream(&buf, &len);
	record_json(m);
	fclose(m);
	CHECK(strstr(buf, "\"facts\":{\"system.serial\":\"SN-42\"}") != NULL);
	CHECK(strstr(buf, "\"detail\":\"3 bit errors\\n\\u0001\"") != NULL);
	CHECK(strstr(buf, "\"summary\":{\"pass\":0,\"fail\":1,") != NULL);
	free(buf);
	record_reset();
}

/* ATA Status Return descriptor (SAT-4 12.2.2.2) as libata builds it after a CK_COND command. */
static void sense_regs(uint8_t s[32], int ext, const uint8_t lo[3], const uint8_t hob[3], uint8_t dev, uint8_t status)
{
	memset(s, 0, 32);
	s[0] = 0x72, s[1] = 1, s[3] = 0x1d, s[7] = 14;
	s[8] = 0x09, s[9] = 0x0c, s[10] = ext, s[12] = 1;
	s[14] = lo[0], s[15] = hob[0], s[16] = lo[1], s[17] = hob[1], s[18] = lo[2], s[19] = hob[2];
	s[20] = dev, s[21] = status;
}

static void test_sense_regs(void)
{
	static const uint8_t lo[3] = {0x9a, 0x78, 0x56}, hob[3] = {0x34, 0x12, 0x00}, junk[3] = {0xff, 0xff, 0xff};
	uint8_t s[32];
	struct ata_regs r;
	sense_regs(s, 1, lo, hob, 0x40, 0x50);
	CHECK(sat_sense_regs(s, sizeof s, &r) == 0 && r.ext && r.lba == 0x123456789aULL && r.status == 0x50);
	sense_regs(s, 0, lo, junk, 0x4a, 0x50); /* 28-bit: high-order bytes ignored, LBA 27:24 in the device register */
	CHECK(sat_sense_regs(s, sizeof s, &r) == 0 && !r.ext && r.lba == 0x0a56789aULL);
	/* another descriptor first: the walk must skip it */
	sense_regs(s, 1, lo, hob, 0x40, 0x50);
	memmove(s + 16, s + 8, 14);
	memset(s + 8, 0, 8);
	s[7] = 22, s[9] = 6;
	CHECK(sat_sense_regs(s, 32, &r) == 0 && r.lba == 0x123456789aULL);
	s[0] = 0x70; /* fixed format carries no registers */
	CHECK(sat_sense_regs(s, 32, &r) == -1);
	sense_regs(s, 1, lo, hob, 0x40, 0x50);
	s[8] = 0x02;
	CHECK(sat_sense_regs(s, 32, &r) == -1); /* wrong descriptor code */
	sense_regs(s, 1, lo, hob, 0x40, 0x50);
	CHECK(sat_sense_regs(s, 16, &r) == -1); /* truncated descriptor */
	CHECK(sat_sense_regs(s, 4, &r) == -1);
}

static void put_word(uint8_t *blk, int w, uint16_t v)
{
	blk[2 * w] = v & 0xff, blk[2 * w + 1] = v >> 8;
}

static void test_hidden(void)
{
	uint16_t id[256] = {0};
	uint8_t blk[512] = {0}, sum = 0;
	unsigned long long m;
	char why[256];
	struct hidden h = {.cur = 1000, .native = 1000, .dco = 1000, .ss = 512, .native_q = Q_OK, .dco_q = Q_OK};

	id[60] = 0x1234, id[61] = 1;
	CHECK(ata_user_sectors(id) == 0x11234);
	id[100] = 0x5678, id[101] = 0x1234, id[102] = 0x0002, id[103] = 0;
	CHECK(ata_user_sectors(id) == 0x11234); /* word 83 not valid: 28-bit count */
	id[83] = 0x4400;
	CHECK(ata_user_sectors(id) == 0x212345678ULL);
	id[83] = 0x8400; /* bits 15:14 wrong: feature word not valid */
	CHECK(ata_user_sectors(id) == 0x11234);
	CHECK(ata_sector_bytes(id) == 512);
	id[106] = 0x5000, id[117] = 2048;
	CHECK(ata_sector_bytes(id) == 4096);
	id[106] = 0x4000; /* 4K bit not set */
	CHECK(ata_sector_bytes(id) == 512);

	put_word(blk, 3, 0x5678), put_word(blk, 4, 0x1234), put_word(blk, 5, 0x0002);
	blk[510] = 0xa5;
	for (int i = 0; i < 511; i++)
		sum += blk[i];
	blk[511] = -sum;
	CHECK(dco_max_lba(blk, &m) == 0 && m == 0x212345678ULL);
	blk[511]++;
	CHECK(dco_max_lba(blk, &m) == -1); /* checksum */
	blk[511]--;
	blk[510] = 0;
	blk[511] = 0;
	CHECK(dco_max_lba(blk, &m) == -1); /* no signature */
	memset(blk, 0, sizeof blk);
	CHECK(dco_max_lba(blk, &m) == -1); /* a bridge that returns zeros */

	CHECK(hidden_calc(&h, why, sizeof why) == 0 && strstr(why, "none found"));
	h.native = 1500;
	h.dco = 1500;
	CHECK(hidden_calc(&h, why, sizeof why) == 500 * 512 && strstr(why, "HPA") && !strstr(why, "DCO") && strstr(why, "wipe"));
	h.dco = 2500; /* the DCO hides another 1000 sectors */
	CHECK(hidden_calc(&h, why, sizeof why) == 1500 * 512 && strstr(why, "HPA") && strstr(why, "DCO"));
	h.native = 1000;
	CHECK(hidden_calc(&h, why, sizeof why) == 1500 * 512 && !strstr(why, "HPA") && strstr(why, "DCO"));
	h.ss = 4096;
	h.dco = 1000 + (1ULL << 30) / 4096 * 3; /* 3 GiB */
	CHECK(hidden_calc(&h, why, sizeof why) == 3LL << 30 && strstr(why, "3.2 GB"));
	h.ss = 512;
	h.dco = 1000;
	h.native_q = Q_REFUSED; /* a bridge that refuses cannot be taken as "none" */
	CHECK(hidden_calc(&h, why, sizeof why) == -1 && strstr(why, "refused"));
	h.native_q = Q_NO;
	CHECK(hidden_calc(&h, why, sizeof why) == 0);
	h.dco_q = Q_REFUSED;
	CHECK(hidden_calc(&h, why, sizeof why) == -1);
	h.dco_q = Q_NO;
	h.native_q = Q_OK;
	h.native = 999; /* native below visible: bogus */
	CHECK(hidden_calc(&h, why, sizeof why) == -1);
	h.native = 0; /* a bridge that answers with zeros */
	CHECK(hidden_calc(&h, why, sizeof why) == -1);
	h.native = 1ULL << 50;
	CHECK(hidden_calc(&h, why, sizeof why) == -1);
	h.native = 1000;
	h.dco_q = Q_OK;
	h.dco = 900; /* factory max below native: bogus */
	CHECK(hidden_calc(&h, why, sizeof why) == -1);

	uint8_t c[4096] = {0};
	CHECK(nvme_unallocated(c) == 0); /* no namespace management: nothing unassigned */
	c[256] = 8;
	CHECK(nvme_unallocated(c) == -1); /* management supported but capacity not reported */
	for (int i = 0; i < 8; i++)
		c[280 + i] = (1000000000000ULL >> 8 * i) & 0xff, c[296 + i] = (200000000000ULL >> 8 * i) & 0xff;
	CHECK(nvme_unallocated(c) == 200000000000LL);
	c[296 + 8] = 1; /* UNVMCAP above 64 bits */
	CHECK(nvme_unallocated(c) == -1);
	c[296 + 8] = 0;
	c[296 + 7] = 0xff; /* more unassigned than the total */
	CHECK(nvme_unallocated(c) == -1);
}

static void put(const char *dir, const char *name, const char *val)
{
	char p[300];
	snprintf(p, sizeof p, "%s/%s", dir, name);
	FILE *f = fopen(p, "w");
	if (f) {
		fprintf(f, "%s\n", val);
		fclose(f);
	}
}

static enum verdict bcheck(long long ef, long long ed, long long cf, long long cd, long long vmin, struct batt_calc *c, char *why)
{
	struct batt b = {.ef = ef, .ed = ed, .cf = cf, .cd = cd, .vmin = vmin, .cycles = -1, .capacity = -1};
	return batt_check(&b, c, why, 384);
}

static void test_battery(void)
{
	struct batt_calc c;
	char why[384];
	/* energy_*, uWh: 41.2 of 45 Wh is 91% */
	CHECK(bcheck(41200000, 45000000, -1, -1, -1, &c, why) == V_PASS && (int)c.pct == 91);
	CHECK(strstr(why, "Excellent") && strstr(why, "41.2 of 45.0 Wh") && c.design_wh == 45.0);
	CHECK(bcheck(42500000, 50000000, -1, -1, -1, &c, why) == V_PASS && strstr(why, "Excellent")); /* 85% exactly */
	CHECK(bcheck(42400000, 50000000, -1, -1, -1, &c, why) == V_PASS && strstr(why, "Good"));
	CHECK(bcheck(40000000, 50000000, -1, -1, -1, &c, why) == V_PASS && strstr(why, "Good")); /* 80% exactly */
	CHECK(bcheck(39000000, 50000000, -1, -1, -1, &c, why) == V_PASS && strstr(why, "Fair"));
	CHECK(bcheck(25000000, 50000000, -1, -1, -1, &c, why) == V_PASS && strstr(why, "Fair")); /* 50% exactly */
	CHECK(bcheck(24500000, 50000000, -1, -1, -1, &c, why) == V_FAIL && strstr(why, "Poor") && (int)c.pct == 49);
	/* charge_*, uAh: 3 of 4 Ah = 75%; Wh needs the design voltage */
	CHECK(bcheck(-1, -1, 3000000, 4000000, 11400000, &c, why) == V_PASS && (int)c.pct == 75);
	CHECK(c.design_wh > 45.5 && c.design_wh < 45.7 && strstr(why, "Fair"));
	CHECK(bcheck(-1, -1, 3000000, 4000000, -1, &c, why) == V_PASS && (int)c.pct == 75 && c.design_wh < 0 && !strstr(why, "Wh"));
	/* design in energy, full in charge: converted with the design voltage */
	CHECK(bcheck(-1, 45000000, 3500000, -1, 11400000, &c, why) == V_PASS && (int)c.pct == 88);
	CHECK(bcheck(-1, 45000000, 3500000, -1, -1, &c, why) == V_INCONCLUSIVE); /* cannot convert */
	/* things that must never become a verdict */
	CHECK(bcheck(41000000, 0, -1, -1, -1, &c, why) == V_INCONCLUSIVE && c.pct < 0); /* design 0 */
	CHECK(bcheck(41000000, -1, -1, -1, -1, &c, why) == V_INCONCLUSIVE); /* no design value */
	CHECK(bcheck(-1, -1, -1, -1, -1, &c, why) == V_INCONCLUSIVE);
	CHECK(bcheck(0, 45000000, -1, -1, -1, &c, why) == V_INCONCLUSIVE); /* full 0 */
	CHECK(bcheck(60000000, 45000000, -1, -1, -1, &c, why) == V_INCONCLUSIVE && strstr(why, "credible")); /* 133% */
	CHECK(bcheck(58000000, 45000000, -1, -1, -1, &c, why) == V_PASS); /* 129%: rounding in firmware, plausible */
	CHECK(bcheck(4294967295LL, 4294967295LL, -1, -1, -1, &c, why) == V_INCONCLUSIVE); /* 4295 Wh */
	CHECK(bcheck(-1, -1, 3000000, 4000000, 0x7fffffff, &c, why) == V_INCONCLUSIVE); /* absurd voltage */
	CHECK(bcheck(-1, -1, 900000000, 900000000, -1, &c, why) == V_INCONCLUSIVE); /* 900 Ah */

	char dir[] = "/tmp/bedrock-XXXXXX";
	static const char *files[] = {"present", "model_name", "manufacturer", "technology", "status", "capacity",
		"cycle_count", "charge_full", "charge_full_design", "voltage_min_design"};
	struct batt b;
	CHECK(mkdtemp(dir) != NULL);
	put(dir, "model_name", "L17M3PB0");
	put(dir, "manufacturer", "SMP");
	put(dir, "technology", "Li-poly");
	put(dir, "status", "Discharging");
	put(dir, "capacity", "87");
	put(dir, "cycle_count", "312");
	put(dir, "charge_full", "3300000");
	put(dir, "charge_full_design", "4000000");
	put(dir, "voltage_min_design", "11400000");
	CHECK(batt_load(dir, &b) == 0);
	CHECK(!strcmp(b.model, "L17M3PB0") && !strcmp(b.maker, "SMP") && !strcmp(b.tech, "Li-poly") && !strcmp(b.status, "Discharging"));
	CHECK(b.capacity == 87 && b.cycles == 312 && b.cf == 3300000 && b.cd == 4000000 && b.vmin == 11400000);
	CHECK(b.ef == -1 && b.ed == -1 && !strncmp(b.name, "bedrock-", 8)); /* missing files read as -1 */
	CHECK(batt_check(&b, &c, why, sizeof why) == V_PASS && (int)c.pct == 82 && strstr(why, "Good") && strstr(why, "312 cycles"));
	put(dir, "present", "0");
	CHECK(batt_load(dir, &b) == -1); /* empty bay */
	for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
		char p[300];
		snprintf(p, sizeof p, "%s/%s", dir, files[i]);
		remove(p);
	}
	rmdir(dir);
}

static void unhex(const char *h, uint8_t *out)
{
	for (; h[0] && h[1]; h += 2)
		sscanf(h, "%2hhx", out++);
}

/* RFC 8032 section 7.1, TEST 1 and TEST 2 */
static void test_ed25519(void)
{
	static const struct { const char *seed, *pub, *msg, *sig; } v[] = {
		{"9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
		 "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
		 "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
		{"4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
		 "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
		 "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
	};
	for (size_t i = 0; i < sizeof v / sizeof *v; i++) {
		uint8_t seed[32], sk[64], pk[32], wpk[32], msg[1], sig[64], wsig[64];
		size_t mn = strlen(v[i].msg) / 2;
		unhex(v[i].seed, seed);
		unhex(v[i].pub, wpk);
		unhex(v[i].msg, msg);
		unhex(v[i].sig, wsig);
		crypto_ed25519_key_pair(sk, pk, seed);
		CHECK(!memcmp(pk, wpk, 32));
		crypto_ed25519_sign(sig, sk, msg, mn);
		CHECK(!memcmp(sig, wsig, 64));
		CHECK(crypto_ed25519_check(sig, pk, msg, mn) == 0);
		sig[0] ^= 1;
		CHECK(crypto_ed25519_check(sig, pk, msg, mn) != 0);
	}
}

/* RFC 4648 section 10 */
static void test_base64(void)
{
	static const char *v[][2] = {{"", ""}, {"f", "Zg=="}, {"fo", "Zm8="}, {"foo", "Zm9v"},
		{"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="}, {"foobar", "Zm9vYmFy"}};
	char b[404];
	uint8_t in[300], out[300];
	for (size_t i = 0; i < sizeof v / sizeof *v; i++) {
		size_t n = strlen(v[i][0]);
		CHECK(b64_encode((const uint8_t *)v[i][0], n, b, sizeof b) == (int)strlen(v[i][1]));
		CHECK(!strcmp(b, v[i][1]));
		CHECK(b64_decode(v[i][1], out, sizeof out) == (int)n && !memcmp(out, v[i][0], n));
	}
	for (int n = 0; n < 300; n++) {
		in[n] = (uint8_t)(n * 151 + 7);
		CHECK(b64_encode(in, n, b, sizeof b) >= 0 && b64_decode(b, out, sizeof out) == n && !memcmp(in, out, n));
	}
	CHECK(b64_encode(in, 3, b, 4) == -1); /* no room for the terminator */
	CHECK(b64_decode("Zm9vYg==", out, 3) == -1); /* no room */
	CHECK(b64_decode("Zg=", out, 8) == -1 && b64_decode("Z===", out, 8) == -1);
	CHECK(b64_decode("Zg=a", out, 8) == -1 && b64_decode("Zg==Zg==", out, 8) == -1);
	CHECK(b64_decode("Zm9v!A==", out, 8) == -1 && b64_decode("Zm9 v", out, 8) == -1);
}

static void test_key_id(void)
{
	uint8_t pk[32];
	char id[17];
	unhex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", pk);
	key_id(pk, id);
	CHECK(!strcmp(id, "0e02a50225b4baaa")); /* first 16 hex digits of SHA-512, computed independently */
}

static void test_json_merge(void)
{
	char *buf = NULL;
	size_t len;
	const char *ok[] = {"{\"a\":1}", " { } ", NULL, "{\"b\":{\"c\":\"}\"}} \n"};
	const char *bad[] = {"{\"a\":1}", "\"b\":2"};
	FILE *m = open_memstream(&buf, &len);
	CHECK(json_merge(m, ok, 4) == 0);
	fclose(m);
	CHECK(!strcmp(buf, "{\"a\":1,\"b\":{\"c\":\"}\"}}"));
	free(buf);
	m = open_memstream(&buf, &len);
	CHECK(json_merge(m, bad, 2) == -1);
	fclose(m);
	free(buf);
}

static void test_nist(void)
{
	const char *why;
	CHECK(!strcmp(nist_category("NVMe Sanitize, crypto erase", &why), "Purge"));
	CHECK(!strcmp(nist_category("NVMe Sanitize, block erase", &why), "Purge"));
	CHECK(!strcmp(nist_category("NVMe Format, crypto erase", &why), "Purge"));
	CHECK(!strcmp(nist_category("NVMe Format, user data erase", &why), "Clear"));
	CHECK(!strcmp(nist_category("ATA Security Erase, normal", &why), "Clear"));
	CHECK(!strcmp(nist_category("ATA Security Erase, enhanced", &why), "Clear"));
	CHECK(!strcmp(nist_category("overwrite with zeros", &why), "Clear"));
	CHECK(!strcmp(nist_category("overwrite with random data", &why), "Clear") && strstr(why, "spare"));
}

static const char SAMPLE_EXTRA[] =
	"{\"media\":{\"model\":\"SAMPLE SSD 500GB\",\"serial\":\"S0000SAMPLE\",\"kind\":\"SSD\","
	"\"capacity_bytes\":500107862016,\"interface\":\"NVMe\"},\"sanitization\":{\"category\":\"Purge\","
	"\"technique\":\"NVMe Sanitize, crypto erase\",\"details\":\"the drive's Sanitize command reaches all of its "
	"media, spare and remapped blocks included, and its status log reported completion\","
	"\"verification\":\"64 samples: 0 zero, 0 one fill byte, 64 other data changed by the erase, 0 unchanged, "
	"0 unreadable or not comparable\",\"result\":\"PASS\",\"started\":\"2026-11-26T10:00:00Z\","
	"\"finished\":\"2026-11-26T10:00:42Z\"}}";

/* A made-up machine with one wipe, as wipe_run() records it. */
static void sample_record(void)
{
	record_reset();
	fact("system.manufacturer", "SAMPLE (not a real machine)");
	fact("system.product", "Sample PC <1>");
	fact("system.serial", "SAMPLE-0001");
	fact("cpu.brand", "Sample CPU");
	fact("memory.installed_mb", "16384");
	fact("disk.0", "nvme0n1|SAMPLE SSD 500GB|S0000SAMPLE|500107862016|NVMe");
	fact("station.operator", "Sample Operator");
	fact("station.asset_tag", "A-0001");
	record_result(V_PASS, "Wipe", "/dev/nvme0n1 (serial S0000SAMPLE); method: NVMe Sanitize, crypto erase; took 0 min 42 s");
}

static char *envelope(const char *kind, const char *extra, char sig[89])
{
	char *buf = NULL;
	size_t len;
	FILE *m = open_memstream(&buf, &len);
	CHECK(cert_envelope(m, kind, extra, sig) == 0);
	fclose(m);
	return buf;
}

static void test_envelope(void)
{
	uint8_t seed[32], pl[8192];
	char id[17], sig[89], *p;
	unhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", seed);
	cert_key_seed(seed);
	sample_record();
	char *e = envelope("sanitization", SAMPLE_EXTRA, sig);
	CHECK(strstr(e, "\"format\": \"bedrock-signed-1\"") && strstr(e, "\"key_id\": \"0e02a50225b4baaa\""));
	CHECK(cert_verify(e, id) == 1 && !strcmp(id, "0e02a50225b4baaa"));
	CHECK(strlen(sig) == 88 && strstr(e, sig));

	/* the payload is kind, station identity, the record, then the extras, at the top level */
	char *b = strstr(e, "\"payload\": \"") + 12;
	*strchr(b, '"') = 0;
	int n = b64_decode(b, pl, sizeof pl - 1);
	CHECK(n > 0);
	pl[n > 0 ? n : 0] = 0;
	p = (char *)pl;
	CHECK(!strncmp(p, "{\"kind\":\"sanitization\",\"operator\":\"Sample Operator\",\"asset_tag\":\"A-0001\",\"tool\":", 80));
	CHECK(strstr(p, "\"system.serial\":\"SAMPLE-0001\"") && strstr(p, "\"verdict\":\"PASS\""));
	CHECK(strstr(p, "},\"media\":{\"model\":\"SAMPLE SSD 500GB\"") && strstr(p, "\"category\":\"Purge\""));
	CHECK(n > 3 && p[n - 1] == '}' && p[n - 2] == '}' && p[n - 3] == '"');
	b[strlen(b)] = '"';

	/* change one payload byte: still valid base64, no longer the signed bytes */
	char *t = strdup(e), *q = strstr(t, "\"payload\": \"") + 40;
	*q = *q == 'A' ? 'B' : 'A';
	CHECK(cert_verify(t, id) == 0);
	free(t);
	t = strdup(e); /* relabel the kind outside the signature */
	memcpy(strstr(t, "\"kind\": \"sanitization\"") + 9, "health\"     ", 12);
	CHECK(cert_verify(t, id) == 0);
	free(t);
	t = strdup(e); /* a key ID that is not this key's */
	strstr(t, "0e02a50225b4baaa")[0] = '1';
	CHECK(cert_verify(t, id) == 0);
	free(t);
	t = strdup(e);
	memcpy(strstr(t, "signed-1"), "signed-9", 8);
	CHECK(cert_verify(t, id) == -1);
	free(t);
	CHECK(cert_verify("{}", id) == -1 && cert_verify("", id) == -1);
	free(e);

	/* printable twin: extras as sections, everything escaped */
	char *h = NULL;
	size_t hl;
	FILE *m = open_memstream(&h, &hl);
	cert_html(m, "sanitization", SAMPLE_EXTRA, sig, "x.json");
	fclose(m);
	CHECK(strstr(h, "<h1>Data sanitization certificate</h1>") && strstr(h, "<h2>Media</h2>"));
	CHECK(strstr(h, "<tr><th>Category</th><td>Purge</td></tr>") && strstr(h, "<tr><th>Operator</th><td>Sample Operator</td></tr>"));
	CHECK(strstr(h, "<th>Capacity</th><td>500107862016 bytes (500.1 GB)</td>") && strstr(h, "Sample PC &lt;1&gt;"));
	CHECK(strstr(h, "0e02a50225b4baaa") && strstr(h, "verify") && !strstr(h, "Sample PC <1>"));
	free(h);
	m = open_memstream(&h, &hl);
	sheet_html(m, "x.json", "0e02a50225b4baaa");
	fclose(m);
	CHECK(strstr(h, "<h1>SAMPLE (not a real machine) Sample PC &lt;1&gt;</h1>") && strstr(h, "<td>16 GB</td>"));
	CHECK(strstr(h, "SAMPLE SSD 500GB, 500 GB, NVMe; health (SMART): not checked"));
	CHECK(strstr(h, "All 1 checks passed"));
	free(h);
	record_reset();
}

/* docs/sample-certificate.json: the sample sanitization certificate, sample key = 32 bytes of 0x42 */
static int write_sample(void)
{
	uint8_t seed[32];
	memset(seed, 0x42, sizeof seed);
	cert_key_seed(seed);
	sample_record();
	return cert_envelope(stdout, "sanitization", SAMPLE_EXTRA, NULL) != 0;
}

#define QR_HELLO_HASH 0x5a5873fbf75041f1ULL

static void test_cfg(void)
{
	struct cfg c;
	cfg_defaults(&c);
	CHECK(!c.station && !c.ask_asset_tag && c.cpu_seconds == 30 && c.memory == MEM_QUICK &&
	      c.disk_scan == SCAN_SAMPLED && c.wipe == WIPE_OFF && !c.poweroff_when_done);
	cfg_parse(&c, "\xef\xbb\xbf# station config\r\n"
		"Operator =  Erdem Dogan  \r\n"
		"organization=Acme # not a comment\r\n"
		"\tlocation\t=\tShelf 3\r\n"
		"   mode = station\r\n"
		"ask_asset_tag = YES\r\n"
		"cpu_seconds = 5\r\n"
		"memory = full\r\n"
		"disk_scan = Off\r\n"
		"wipe = auto\r\n"
		"wipe_confirm = ERASE ALL INTERNAL DISKS\r\n"
		"poweroff_when_done = yes\r\n"
		"bogus = 1\r\n"
		"# wipe = off\r\n"
		"no equals sign here\r\n"
		"cpu_seconds = abc\r\n"
		"memory = lots\r\n"
		"= no key\r\n"
		"last = no newline");
	CHECK(!strcmp(c.operator, "Erdem Dogan") && !strcmp(c.organization, "Acme # not a comment"));
	CHECK(!strcmp(c.location, "Shelf 3") && c.station && c.ask_asset_tag);
	CHECK(c.cpu_seconds == 5); /* a bad number leaves the setting alone */
	CHECK(c.memory == MEM_FULL && c.disk_scan == SCAN_OFF && c.wipe == WIPE_AUTO && c.poweroff_when_done);
	CHECK(!strcmp(c.wipe_confirm, WIPE_AUTO_CONFIRM)); /* CRLF stripped */
	cfg_parse(&c, "mode = menu\ncpu_seconds = 99999\nwipe = maybe\nask_asset_tag = perhaps\n");
	CHECK(!c.station && c.cpu_seconds == 5 && c.wipe == WIPE_AUTO && c.ask_asset_tag);
	cfg_defaults(&c);
	cfg_parse(&c, "");
	CHECK(!c.station && c.cpu_seconds == 30);
}

static void test_wipe_gate(void)
{
	struct cfg c;
	CHECK(wipe_auto_ok("ERASE ALL INTERNAL DISKS"));
	CHECK(!wipe_auto_ok("erase all internal disks") && !wipe_auto_ok("ERASE ALL INTERNAL DISKS ") &&
	      !wipe_auto_ok("ERASE ALL INTERNAL DISK") && !wipe_auto_ok("ERASE") && !wipe_auto_ok("") && !wipe_auto_ok(NULL));
	cfg_defaults(&c);
	cfg_parse(&c, "wipe = auto\n"); /* no confirmation: nothing may run */
	CHECK(c.wipe == WIPE_AUTO && !wipe_auto_ok(c.wipe_confirm));
	cfg_parse(&c, "wipe_confirm = ERASE ALL INTERNAL DISKS   \r\n"); /* padding is not part of the value */
	CHECK(wipe_auto_ok(c.wipe_confirm));
	cfg_parse(&c, "wipe_confirm = \"ERASE ALL INTERNAL DISKS\"\n"); /* no quote handling: not an exact match */
	CHECK(!wipe_auto_ok(c.wipe_confirm));
	/* the stick is the disk Bedrock runs from, so the shared refusal covers it */
	CHECK(boot_disk_refuse("sdb", "/dev/sdb1 /mnt/report vfat rw 0 0\n", ""));
}

static void csv_one(FILE *o, void *s) { csv_field(o, s); }

/* Run fn into a string. */
static char *slurp(void (*fn)(FILE *, void *), void *arg)
{
	char *buf = NULL;
	size_t len = 0;
	FILE *m = open_memstream(&buf, &len);
	fn(m, arg);
	fclose(m);
	return buf;
}

static void test_csv(void)
{
	static const char *const in[] = {"plain", "", "a,b", "say \"hi\"", "two\nlines", "cr\rhere", "\"", " lead",
		"=cmd()", "+1", "@x", "-x", "-12", "-"};
	static const char *const out[] = {"plain", "", "\"a,b\"", "\"say \"\"hi\"\"\"", "\"two\nlines\"", "\"cr\rhere\"", "\"\"\"\"", " lead",
		"'=cmd()", "'+1", "'@x", "'-x", "-12", "-"};
	for (size_t i = 0; i < sizeof in / sizeof *in; i++) {
		char *s = slurp(csv_one, (void *)in[i]);
		CHECK(!strcmp(s, out[i]));
		free(s);
	}
}

static void inv_header(FILE *o, void *arg) { (void)arg; inventory_header(o); }

static void inv_row(FILE *o, void *arg)
{
	const int cnt[4] = {12, 0, 0, 1};
	(void)arg;
	inventory_row(o, "2026-01-01T12:00:00Z", "PASS", cnt, "A-1-20260101-1200.json");
}

static void test_inventory(void)
{
	char *s = slurp(inv_header, NULL);
	CHECK(!strcmp(s, "date,asset_tag,operator,organization,location,manufacturer,product,serial,board,cpu,"
			 "threads,ram_mb,disk_count,disk_total_gb,disk_models,disk_serials,gpu,battery_health,result,"
			 "pass,fail,inconclusive,record\r\n"));
	free(s);
	record_reset();
	fact("station.asset_tag", "A-1");
	fact("station.operator", "Erdem, \"E\"");
	fact("station.organization", "Acme");
	fact("station.location", "Shelf 3");
	fact("station.boot_disk", "sdb");
	fact("system.manufacturer", "LENOVO");
	fact("system.product", "ThinkPad T480");
	fact("system.serial", "PF123");
	fact("board.manufacturer", "LENOVO");
	fact("board.product", "20L5");
	fact("cpu.brand", "Intel(R) Core(TM) i5-8350U");
	fact("cpu.threads", "8");
	fact("memory.installed_mb", "16384");
	fact("memory.visible_mb", "15900");
	fact("disk.0", "sda|Samsung SSD 860|S3Z9|500107862016|ATA SSD");
	fact("disk.1", "sdb|BOOTSTICK|XYZ|8000000000|other"); /* the stick is not a PC disk */
	fact("disk.2", "nvme0n1|WD, 256||256060514304|NVMe");
	fact("gpu.0", "Intel UHD [8086:5917]");
	s = slurp(inv_row, NULL);
	CHECK(!strcmp(s, "2026-01-01T12:00:00Z,A-1,\"Erdem, \"\"E\"\"\",Acme,Shelf 3,LENOVO,ThinkPad T480,PF123,"
			 "LENOVO 20L5,Intel(R) Core(TM) i5-8350U,8,16384,2,756.2,\"Samsung SSD 860; WD, 256\",S3Z9; -,"
			 "Intel UHD [8086:5917],,PASS,12,0,0,A-1-20260101-1200.json\r\n"));
	free(s);
	fact("battery.0.health_pct", "87");
	fact("memory.installed_mb", "0"); /* unknown DIMM sizes: fall back to what the system sees */
	s = slurp(inv_row, NULL);
	CHECK(strstr(s, ",8,15900,2,") && strstr(s, ",Intel UHD [8086:5917],87,PASS,"));
	free(s);
	record_reset();
}

static void test_station_names(void)
{
	char id[32];
	int c1[4] = {5, 0, 0, 0}, c2[4] = {5, 1, 2, 0}, c3[4] = {5, 0, 1, 0}, c4[4] = {0, 0, 0, 3};
	safe_id("To be filled by O.E.M.", id, sizeof id);
	CHECK(!strcmp(id, "To-be-filled-by-O-E-M"));
	safe_id("../etc/passwd", id, sizeof id);
	CHECK(!strcmp(id, "etc-passwd"));
	safe_id("0123456789012345678901234567890123456789", id, sizeof id);
	CHECK(strlen(id) == 24);
	safe_id("???", id, sizeof id);
	CHECK(!id[0]);
	CHECK(station_result(c1) == V_PASS && station_result(c2) == V_FAIL && station_result(c3) == V_INCONCLUSIVE);
	CHECK(station_result(c4) == V_INCONCLUSIVE); /* nothing measured is not a pass */
}

static void test_station_record(void)
{
	char path[64] = "station-test.json", buf[4096] = "";
	FILE *null = fopen("/dev/null", "w");
	record_reset();
	memset(g_count, 0, sizeof g_count);
	fact("station.asset_tag", "A-1");
	verdict(null, V_PASS, "CPU compute", "ok");
	CHECK(station_record(path, sizeof path) == 0 && !strcmp(path, "station-test.json"));
	CHECK(read_file(path, buf, sizeof buf) > 0 && strstr(buf, "\"station.asset_tag\":\"A-1\"") && strstr(buf, "\"pass\":1"));
	remove(path);
	fclose(null);
	record_reset();
	memset(g_count, 0, sizeof g_count);
}

static uint64_t qr_hash(const uint8_t *qr)
{
	uint64_t h = 0xcbf29ce484222325ULL;
	int size = qrcodegen_getSize(qr);
	for (int y = 0; y < size; y++)
		for (int x = 0; x < size; x++)
			h = (h ^ (qrcodegen_getModule(qr, x, y) ? 'X' : '.')) * 0x100000001b3ULL;
	return h;
}

static void qr_plain(FILE *o, void *qr) { station_qr_draw(o, qr, 4, 0, 0); }

static int utf8_len(const char *s, size_t n)
{
	int k = 0;
	for (size_t i = 0; i < n; i++)
		k += ((unsigned char)s[i] & 0xc0) != 0x80;
	return k;
}

static void test_qr(void)
{
	uint8_t qr[STATION_QR_LEN];
	char t[256], big[200];
	int n;
	n = station_qr("HELLO WORLD", qr);
	CHECK(n == 21); /* version 1 */
	for (int i = 0; i < 7; i++) /* top-left finder pattern */
		CHECK(qrcodegen_getModule(qr, i, 0) && qrcodegen_getModule(qr, 0, i));
	CHECK(!qrcodegen_getModule(qr, 7, 0) && qrcodegen_getModule(qr, 2, 2) && !qrcodegen_getModule(qr, 1, 1));
	CHECK(qr_hash(qr) == QR_HELLO_HASH); /* the matrix a phone decoded back to HELLO WORLD */

	n = station_qr_text(t, sizeof t, "A-1", "PF123", "LENOVO ThinkPad T480", "PASS", "2026-01-01", "A-1-20260101-1200.json");
	CHECK(!strcmp(t, "BEDROCK1|A-1|PF123|LENOVO ThinkPad T480|PASS|2026-01-01|A-1-20260101-1200.json") && n == (int)strlen(t));
	n = station_qr(t, qr);
	CHECK(n > 0 && n <= 37); /* a typical record fits version 5 */
	station_qr_text(t, sizeof t, "A|1", "S\n1", "x|y", "FAIL", "2026-01-01", "r.json");
	CHECK(!strcmp(t, "BEDROCK1|A/1|S 1|x/y|FAIL|2026-01-01|r.json")); /* '|' and control characters cannot split fields */

	memset(big, 'M', sizeof big - 1);
	big[sizeof big - 1] = 0;
	n = station_qr_text(t, sizeof t, "123456789012345678901234", "123456789012345678901234", big, "INCONCLUSIVE", "2026-01-01",
		"123456789012345678901234-20260101-1200.json");
	CHECK(n <= 134 && n == (int)strlen(t) && !strncmp(t, "BEDROCK1|123456789012345678901234|", 34));
	CHECK(strstr(t, "|INCONCLUSIVE|2026-01-01|123456789012345678901234-20260101-1200.json") != NULL); /* only the model is cut */
	CHECK(station_qr(t, qr) > 0); /* the worst case still fits version 6 */

	/* drawing: two module rows per line, quiet zone of 4 around 21 modules = 29 columns, 15 lines */
	station_qr("HELLO WORLD", qr);
	char *s = slurp(qr_plain, qr), *line = s;
	int lines = 0;
	for (char *nl; (nl = strchr(line, '\n')); line = nl + 1, lines++)
		CHECK(utf8_len(line, nl - line) == 29);
	CHECK(lines == 15 && !strncmp(s, "                             \n", 30)); /* quiet rows are blank */
	CHECK(strstr(s, "\xe2\x96\x88") && strstr(s, "\xe2\x96\x80") && strstr(s, "\xe2\x96\x84"));
	free(s);
}

static void test_fleet(void)
{
	struct fleet_url u;
	char buf[512], v[64];
	CHECK(!fleet_url_parse("http://10.0.2.2:8080", &u) && !strcmp(u.host, "10.0.2.2") && !strcmp(u.port, "8080") && !u.path[0]);
	CHECK(!fleet_url_parse("http://fleet.lan", &u) && !strcmp(u.host, "fleet.lan") && !strcmp(u.port, "80"));
	CHECK(!fleet_url_parse("http://fleet.lan:9//", &u) && !strcmp(u.port, "9") && !u.path[0]);
	CHECK(!fleet_url_parse("http://h/prefix/x/", &u) && !strcmp(u.path, "/prefix/x"));
	CHECK(fleet_url_parse("https://h", &u) && fleet_url_parse("h:80", &u) && fleet_url_parse("http://", &u));
	CHECK(fleet_url_parse("http://:80", &u) && fleet_url_parse("http://h:", &u) && fleet_url_parse("http://h:0", &u));
	CHECK(fleet_url_parse("http://h:65536", &u) && fleet_url_parse("http://h:80x", &u) && fleet_url_parse("http://[::1]:80", &u));
	CHECK(fleet_url_parse("http://a b", &u) && fleet_url_parse("http://h?x=1", &u));

	fleet_url_parse("http://10.0.2.2:8080/f", &u);
	size_t n = fleet_request(buf, sizeof buf, &u, 1234);
	CHECK(n == strlen(buf) && !strncmp(buf, "POST /f/api/records HTTP/1.1\r\nHost: 10.0.2.2:8080\r\n", 50));
	CHECK(strstr(buf, "\r\nContent-Length: 1234\r\n") && strstr(buf, "\r\nContent-Type: application/json\r\n"));
	CHECK(!strcmp(buf + n - 4, "\r\n\r\n")); /* headers end; the body follows */
	fleet_url_parse("http://fleet.lan", &u);
	fleet_request(buf, sizeof buf, &u, 0);
	CHECK(strstr(buf, "POST /api/records HTTP/1.1\r\nHost: fleet.lan\r\n") == buf);
	CHECK(fleet_request(buf, 20, &u, 5) == 0); /* too small */

	CHECK(fleet_status("HTTP/1.1 201 Created\r\n", 22) == 201);
	CHECK(fleet_status("HTTP/1.0 200 OK", 15) == 200);
	CHECK(fleet_status("HTTP/1.1 503", 12) == 503);
	CHECK(fleet_status("HTTP/1.1 20", 11) == -1 && fleet_status("HTTP/2 200 OK", 13) == -1);
	CHECK(fleet_status("HTTP/1.1 2x0 OK", 15) == -1 && fleet_status("garbage garbage", 15) == -1);

	CHECK(!fleet_find("quiet bedrock.fleet=http://a:1 console=tty0\n", "bedrock.fleet", v, sizeof v) && !strcmp(v, "http://a:1"));
	CHECK(fleet_find("quiet xbedrock.fleet=http://a bedrock.fleet2=x", "bedrock.fleet", v, sizeof v));
	CHECK(!fleet_find("operator = Ann\n# fleet_url = http://old\n  fleet_url = \"http://b:2\"  \n", "fleet_url", v, sizeof v) && !strcmp(v, "http://b:2"));
	CHECK(fleet_find("# fleet_url=http://x\nfleet_url\n", "fleet_url", v, sizeof v));
	CHECK(fleet_find("fleet_url=http://averyveryverylongaddress", "fleet_url", v, 8));
}

static const struct finding *find(const struct finding *f, int n, const char *cause)
{
	for (int i = 0; i < n; i++)
		if (strstr(f[i].cause, cause))
			return &f[i];
	return NULL;
}

static void test_klog(void)
{
	static struct finding f[FINDINGS_MAX];
	size_t len;
	char *log = (char *)load("klog.txt", &len);
	log[len] = 0;
	int n = klog_scan(log, f, 0);
	const struct finding *x;
	x = find(f, n, "memory controller (bank 7)");
	CHECK(x && x->part == PART_MEM && x->sev == SEV_MED && !strncmp(x->cause, "corrected", 9));
	x = find(f, n, "internal parity check (bank 0)");
	CHECK(x && x->part == PART_CPU && x->sev == SEV_HIGH && !strncmp(x->cause, "fatal", 5));
	x = find(f, n, "(bank 27)"); /* bus-error code, but the IPID says UMC */
	CHECK(x && x->part == PART_MEM && strstr(x->cause, "memory controller"));
	x = find(f, n, "corrected PCIe errors at 0000:03:00.0");
	CHECK(x && x->part == PART_PCIE && x->sev == SEV_LOW && x->count == 2);
	x = find(f, n, "fatal PCIe errors at 0000:00:01.0");
	CHECK(x && x->sev == SEV_HIGH);
	x = find(f, n, "SATA link saw signal errors");
	CHECK(x && x->part == PART_SATA && x->count == 1);
	CHECK((x = find(f, n, "SATA commands failed")) && x->sev == SEV_MED);
	CHECK(find(f, n, "slowed down after errors") != NULL);
	CHECK((x = find(f, n, "checksum")) && x->part == PART_SATA);
	CHECK((x = find(f, n, "could not read some sectors")) && x->part == PART_DISK && x->sev == SEV_HIGH);
	CHECK((x = find(f, n, "NVMe commands timed out")) && x->part == PART_NVME && x->sev == SEV_MED);
	CHECK((x = find(f, n, "NVMe drive stopped responding")) && x->sev == SEV_HIGH);
	CHECK((x = find(f, n, "USB device did not answer")) && x->count == 2 && x->sev == SEV_LOW);
	CHECK((x = find(f, n, "over-current")) && x->part == PART_USB && x->sev == SEV_MED);
	CHECK((x = find(f, n, "overheated")) && x->part == PART_COOLING);
	x = find(f, n, "corrected memory errors on CPU_SrcID#0_Ha#0_Chan#1_DIMM#0");
	CHECK(x && x->part == PART_MEM && x->count == 2 && x->sev == SEV_MED);
	CHECK((x = find(f, n, "ACPI bugs")) && x->sev == SEV_NOTE && x->count == 2);
	CHECK((x = find(f, n, "reserves some hardware")) && x->sev == SEV_NOTE);
	CHECK(find(f, n, "could not describe") == NULL); /* events logged, but decoded above */
	/* Benign lines must not count: link down, AER enabled, thermal monitoring, EDAC version, new USB device */
	int real = 0;
	for (int i = 0; i < n; i++)
		real += f[i].sev > SEV_NOTE;
	CHECK(real == 16);

	n = klog_scan("<6>[ 1.0] mce: [Hardware Error]: Machine check events logged\n"
		      "<6>[ 1.1] ata1: SATA link down (SStatus 0 SControl 300)\n", f, 0);
	CHECK(n == 1 && f[0].part == PART_CPU && strstr(f[0].cause, "could not describe"));
	CHECK(klog_scan("", f, 0) == 0);
}

static void test_mce_decode(void)
{
	struct mce_info m;
	enum part p;
	mce_decode(0xbe00000000800400ULL, &m); /* Intel internal timer, uncorrected, PCC */
	CHECK(m.valid && m.uc && m.pcc && m.part == PART_CPU && strstr(m.unit, "timer"));
	mce_decode(0x9c00000000000813ULL, &m); /* bus error, II = 00: a memory read */
	CHECK(m.valid && !m.uc && m.part == PART_MEM && !strcmp(m.unit, "memory bus"));
	mce_decode(0x9000004000011150ULL, &m); /* cache read error, filter bit 12 set */
	CHECK(m.valid && !m.uc && !strcmp(m.unit, "cache"));
	mce_decode(0x8800000000000017ULL, &m);
	CHECK(!strcmp(m.unit, "TLB"));
	mce_decode(0x880000000000000fULL, &m);
	CHECK(!strcmp(m.unit, "cache hierarchy"));
	mce_decode(0x8800000000000e0bULL, &m); /* generic I/O error: bus, not memory */
	CHECK(m.part == PART_BOARD);
	mce_decode(0x8800000000000003ULL, &m);
	CHECK(m.part == PART_BOARD);
	mce_decode(0x7c0000000000009fULL, &m); /* VAL clear: nothing to report */
	CHECK(!m.valid);
	CHECK(smca_unit(0x000000b000000000ULL, &p) && p == PART_CPU);
	CHECK(!strcmp(smca_unit(0x000700b000000000ULL, &p), "L3 cache") && p == PART_CPU);
	CHECK(smca_unit(0x0000004600000000ULL, &p) && p == PART_PCIE);
	CHECK(smca_unit(0x0004b00000000000ULL, &p) == NULL);
}

static void test_pcie_link(void)
{
	char why[160];
	struct pcie_link l = {.cw = 16, .mw = 16, .pw = 16, .cs = 16, .ms = 16, .ps = 16};
	CHECK(pcie_link_judge(&l, 1, why, sizeof why) == V_PASS);
	l.cw = 8; /* x16 card, x16 slot, trained x8: dirty contacts */
	CHECK(pcie_link_judge(&l, 1, why, sizeof why) == V_FAIL && strstr(why, "x16"));
	l.pw = 4, l.cw = 4; /* x16 card in an x4-wired slot: normal */
	CHECK(pcie_link_judge(&l, 1, why, sizeof why) == V_PASS);
	l.pw = -1, l.cw = 16, l.cs = 2.5; /* GPU idling at Gen1 */
	CHECK(pcie_link_judge(&l, 1, why, sizeof why) == V_INCONCLUSIVE && strstr(why, "idles"));
	CHECK(pcie_link_judge(&l, 0, why, sizeof why) == V_INCONCLUSIVE && !strstr(why, "idles"));
	l.cs = 8, l.ms = 16, l.ps = 8; /* Gen4 card in a Gen3 slot */
	CHECK(pcie_link_judge(&l, 0, why, sizeof why) == V_PASS);
	l.cw = 0;
	CHECK(pcie_link_judge(&l, 0, why, sizeof why) == V_INCONCLUSIVE && strstr(why, "down"));
	CHECK(aer_total("RxErr 2\nBadTLP 0\nTOTAL_ERR_COR 5\n") == 5);
	CHECK(aer_total("TOTAL_ERR_FATAL 0\n") == 0 && aer_total("") == -1 && aer_total(NULL) == -1);
}

static void test_sata(void)
{
	char why[160];
	CHECK(sata_gbps10("6.0 Gbps") == 60 && sata_gbps10("1.5 Gbps") == 15 && sata_gbps10("<unknown>") == 0);
	CHECK(sata_judge(60, 60, 60, 60, 0, why, sizeof why) == V_PASS);
	CHECK(sata_judge(60, 60, 30, 30, 0, why, sizeof why) == V_PASS); /* SATA II drive */
	CHECK(sata_judge(30, 30, 30, 60, 0, why, sizeof why) == V_PASS); /* SATA II port */
	CHECK(sata_judge(60, 30, 30, 60, 0, why, sizeof why) == V_FAIL && strstr(why, "after errors"));
	CHECK(sata_judge(60, 60, 15, 60, 0, why, sizeof why) == V_INCONCLUSIVE);
	CHECK(sata_judge(60, 60, 60, 0, 3, why, sizeof why) == V_INCONCLUSIVE && strstr(why, "3 recent errors"));
	CHECK(sata_judge(0, 0, 0, 60, 0, why, sizeof why) == V_ABSENT);
	CHECK(sata_judge(0, 0, 15, 0, 0, why, sizeof why) == V_PASS); /* nothing known to compare */
}

static void test_rails(void)
{
	char why[160];
	CHECK(rail_of("nct6798", 3, "") == R_3V3 && rail_of("nct6798", 7, "") == R_3VSB && rail_of("nct6798", 8, "") == R_VBAT);
	CHECK(rail_of("nct6798", 0, "") == -1 && rail_of("nct6798", 1, "") == -1); /* board dividers unknown */
	CHECK(rail_of("nct6106", 8, "") == -1 && rail_of("w83627uhg", 8, "") == -1);
	CHECK(rail_of("w83627ehf", 8, "") == R_VBAT && rail_of("f71882fg", 8, "") == -1);
	CHECK(rail_of("it8728", 8, "Vbat") == R_VBAT && rail_of("it87", 7, "3VSB") == R_3VSB);
	CHECK(rail_of("it87", 3, "+5V") == -1 && rail_of("it87", 7, "5VSB") == -1 && rail_of("it87", 2, "") == -1);
	CHECK(rail_of("asus_wmi_sensors", 0, "+12V Voltage") == R_12V && rail_of("asus_wmi_sensors", 0, "+5V Voltage") == R_5V);
	CHECK(rail_of("asus_wmi_sensors", 0, "3VSB Voltage") == R_3VSB && rail_of("asus_wmi_sensors", 0, "VBAT Voltage") == R_VBAT);
	CHECK(rail_of("asus_wmi_sensors", 0, "SB 1.05V Voltage") == -1 && rail_of("asus_wmi_sensors", 0, "CPU Core Voltage") == -1);
	CHECK(rail_of("atk0110", 0, "+3.3 Voltage") == R_3V3 && rail_of("atk0110", 0, "+5 Voltage") == R_5V);
	CHECK(rail_of("atk0110", 0, "Vcore Voltage") == -1 && rail_of("coretemp", 3, "") == -1);
	CHECK(rail_judge(R_12V, 12.1, why, sizeof why) == V_PASS);
	CHECK(rail_judge(R_12V, 10.7, why, sizeof why) == V_FAIL && strstr(why, "10.70"));
	CHECK(rail_judge(R_5V, 5.6, why, sizeof why) == V_FAIL);
	CHECK(rail_judge(R_3V3, 3.0, why, sizeof why) == V_PASS);
	CHECK(rail_judge(R_VBAT, 3.3, why, sizeof why) == V_PASS && rail_judge(R_VBAT, 3.9, why, sizeof why) == V_PASS);
	CHECK(rail_judge(R_VBAT, 2.7, why, sizeof why) == V_FAIL && strstr(why, "battery"));
	CHECK(rail_judge(R_12V, 0.0, why, sizeof why) == V_INCONCLUSIVE);
}

static void dimm(struct smbios *s, const char *loc, unsigned mb, unsigned speed, const char *part)
{
	struct dimm *d = &s->dimm[s->nslots++];
	snprintf(d->loc, sizeof d->loc, "%s", loc);
	snprintf(d->part, sizeof d->part, "%s", part);
	d->mb = mb, d->speed = speed;
}

static void test_mem_judge(void)
{
	static struct smbios s;
	char why[200];
	int notes;
	memset(&s, 0, sizeof s);
	dimm(&s, "ChannelA-DIMM0", 8192, 3200, "X1");
	dimm(&s, "ChannelA-DIMM1", 0, 0, "");
	dimm(&s, "ChannelB-DIMM0", 8192, 3200, "X1");
	dimm(&s, "ChannelB-DIMM1", 0, 0, "");
	CHECK(mem_judge(&s, 16000, 64, &notes, why, sizeof why) == V_PASS && !notes);
	CHECK(mem_judge(&s, 15000, 64, &notes, why, sizeof why) == V_PASS); /* 1 GB shared with the GPU */
	CHECK(mem_judge(&s, 7900, 64, &notes, why, sizeof why) == V_FAIL && strstr(why, "missing"));
	CHECK(mem_judge(&s, 2500, 32, &notes, why, sizeof why) == V_INCONCLUSIVE); /* 32-bit build: cannot see it all */
	CHECK(mem_judge(&s, 1000, 32, &notes, why, sizeof why) == V_INCONCLUSIVE);
	s.dimm[2].mb = 0, s.dimm[1].mb = 4096, s.dimm[1].speed = 2666; /* both on channel A, different modules */
	CHECK(mem_judge(&s, 12200, 64, &notes, why, sizeof why) == V_PASS && notes == (MEM_MIXED | MEM_ONE_CHANNEL));
	s.dimm[1].mb = DIMM_UNKNOWN;
	CHECK(mem_judge(&s, 12200, 64, &notes, why, sizeof why) == V_INCONCLUSIVE);

	memset(&s, 0, sizeof s); /* Pentium III board: three 128 MB modules, no channel names */
	dimm(&s, "DIMM 0", 128, 133, "");
	dimm(&s, "DIMM 1", 128, 133, "");
	dimm(&s, "DIMM 2", 128, 133, "");
	CHECK(mem_judge(&s, 383, 32, &notes, why, sizeof why) == V_PASS && !notes);
	CHECK(mem_judge(&s, 255, 32, &notes, why, sizeof why) == V_FAIL);
	memset(&s, 0, sizeof s);
	dimm(&s, "DIMM_A1", 4096, 1600, "P");
	dimm(&s, "DIMM_B1", 0, 0, "");
	CHECK(mem_judge(&s, 4000, 64, &notes, why, sizeof why) == V_PASS && notes == MEM_ONE_CHANNEL);
	memset(&s, 0, sizeof s);
	CHECK(mem_judge(&s, 4000, 64, &notes, why, sizeof why) == V_INCONCLUSIVE);
}

int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "--sample"))
		return write_sample();
	test_cpuid();
	test_workloads();
	test_smbios();
	test_ata_smart();
	test_nvme_smart();
	test_histogram();
	test_wipe_refusal();
	test_ata_security();
	test_nvme_erase();
	test_erase_verify();
	test_pciids();
	test_mem_patterns();
	test_record();
	test_klog();
	test_mce_decode();
	test_pcie_link();
	test_sata();
	test_rails();
	test_mem_judge();
	test_fleet();
	test_cfg();
	test_wipe_gate();
	test_csv();
	test_inventory();
	test_station_names();
	test_station_record();
	test_qr();
	test_sense_regs();
	test_hidden();
	test_battery();
	test_ed25519();
	test_base64();
	test_key_id();
	test_json_merge();
	test_nist();
	test_envelope();
	printf("%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
