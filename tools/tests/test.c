/* Host tests for the pure parsers. Run from tests/: fixtures are read from fixtures/. */
#include "bedrock.h"
#include "vendor/qrcodegen.h"

#include <stdlib.h>
#include <string.h>

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
	static const char *const in[] = {"plain", "", "a,b", "say \"hi\"", "two\nlines", "cr\rhere", "\"", " lead"};
	static const char *const out[] = {"plain", "", "\"a,b\"", "\"say \"\"hi\"\"\"", "\"two\nlines\"", "\"cr\rhere\"", "\"\"\"\"", " lead"};
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
	fact("battery.health", "87%%");
	fact("memory.installed_mb", "0"); /* unknown DIMM sizes: fall back to what the system sees */
	s = slurp(inv_row, NULL);
	CHECK(strstr(s, ",8,15900,2,") && strstr(s, ",Intel UHD [8086:5917],87%,PASS,"));
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

int main(void)
{
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
	test_cfg();
	test_wipe_gate();
	test_csv();
	test_inventory();
	test_station_names();
	test_station_record();
	test_qr();
	printf("%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
