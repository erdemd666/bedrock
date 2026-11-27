/* Host tests for the pure parsers. Run from tests/: fixtures are read from fixtures/. */
#include "bedrock.h"

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
	test_klog();
	test_mce_decode();
	test_pcie_link();
	test_sata();
	test_rails();
	test_mem_judge();
	printf("%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
