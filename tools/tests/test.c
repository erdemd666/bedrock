/* Host tests for the pure parsers. Run from tests/: fixtures are read from fixtures/. */
#include "bedrock.h"
#include "vendor/monocypher-ed25519.h"

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
	test_ed25519();
	test_base64();
	test_key_id();
	test_json_merge();
	test_nist();
	test_envelope();
	printf("%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
