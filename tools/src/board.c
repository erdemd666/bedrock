#include "bedrock.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* String number idx of the structure at h; strings run until end. */
static void sstr(const uint8_t *h, const uint8_t *end, unsigned idx, char *out, size_t n)
{
	const uint8_t *s = h + h[1];
	size_t j = 0;
	out[0] = 0;
	if (!idx)
		return;
	for (unsigned k = 1; k < idx; k++) {
		if (s >= end || !*s)
			return;
		while (s < end && *s)
			s++;
		s++;
	}
	while (s < end && *s && j + 1 < n) {
		out[j++] = *s >= 32 && *s < 127 ? *s : '?';
		s++;
	}
	out[j] = 0;
	while (j && out[j - 1] == ' ')
		out[--j] = 0;
}

static const char *mem_type(unsigned t)
{
	static const char *names[] = {
		[0x01] = "Other", [0x02] = "Unknown", [0x03] = "DRAM", [0x04] = "EDRAM", [0x05] = "VRAM",
		[0x06] = "SRAM", [0x07] = "RAM", [0x08] = "ROM", [0x09] = "Flash", [0x0a] = "EEPROM",
		[0x0b] = "FEPROM", [0x0c] = "EPROM", [0x0d] = "CDRAM", [0x0e] = "3DRAM", [0x0f] = "SDRAM",
		[0x10] = "SGRAM", [0x11] = "RDRAM", [0x12] = "DDR", [0x13] = "DDR2", [0x14] = "DDR2 FB-DIMM",
		[0x18] = "DDR3", [0x19] = "FBD2", [0x1a] = "DDR4", [0x1b] = "LPDDR", [0x1c] = "LPDDR2",
		[0x1d] = "LPDDR3", [0x1e] = "LPDDR4", [0x1f] = "NVDIMM", [0x20] = "HBM", [0x21] = "HBM2",
		[0x22] = "DDR5", [0x23] = "LPDDR5", [0x24] = "HBM3",
	};
	return t < sizeof names / sizeof names[0] && names[t] ? names[t] : "Unknown";
}

int smbios_parse(const uint8_t *t, size_t n, struct smbios *s)
{
	const uint8_t *p = t, *end = t + n;
	int found = 0, cpu_seen = 0;
	memset(s, 0, sizeof *s);
	while (p + 4 <= end) {
		const uint8_t *h = p;
		unsigned type = h[0], len = h[1];
		if (len < 4 || p + len > end)
			break;
		const uint8_t *q = p + len;
		while (q + 1 < end && (q[0] || q[1]))
			q++;
		const uint8_t *send = q + 2 < end ? q + 2 : end;
#define B(o) ((o) < len ? h[o] : 0u)
#define W(o) ((o) + 1 < len ? (unsigned)(h[o] | h[(o) + 1] << 8) : 0u)
#define DW(o) ((o) + 3 < len ? (uint32_t)W(o) | (uint32_t)W((o) + 2) << 16 : 0u)
#define S(o, f) sstr(h, send, B(o), f, sizeof f)
		switch (type) {
		case 0:
			S(4, s->bios_vendor); S(5, s->bios_ver); S(8, s->bios_date);
			break;
		case 1:
			S(4, s->sys_maker); S(5, s->sys_product); S(7, s->sys_serial);
			break;
		case 2:
			S(4, s->board_maker); S(5, s->board_product); S(6, s->board_ver);
			break;
		case 4:
			if (cpu_seen++)
				break;
			S(4, s->cpu_socket); S(0x10, s->cpu_ver);
			s->cpu_max_mhz = W(0x14);
			s->cpu_cur_mhz = W(0x16);
			s->cpu_cores = B(0x23);
			s->cpu_threads = B(0x25);
			break;
		case 17: {
			if (s->nslots >= (int)(sizeof s->dimm / sizeof s->dimm[0]))
				break;
			struct dimm *d = &s->dimm[s->nslots++];
			unsigned sz = W(0x0c);
			S(0x10, d->loc); S(0x11, d->bank); S(0x17, d->maker); S(0x18, d->serial); S(0x1a, d->part);
			d->type = mem_type(B(0x12));
			d->speed = W(0x15);
			if (sz == 0xffff)
				d->mb = DIMM_UNKNOWN;
			else if (sz == 0x7fff)
				d->mb = DW(0x1c) & 0x7fffffff; /* extended size, MB */
			else
				d->mb = sz & 0x8000 ? (sz & 0x7fff) / 1024 : sz; /* bit 15: KB units */
			break;
		}
		}
#undef B
#undef W
#undef DW
#undef S
		found++;
		if (type == 127)
			break;
		p = q + 2;
	}
	return found;
}

static void sensors(FILE *o)
{
	static const struct { const char *kind, *unit; double div; } k[] = {
		{"temp", "C", 1000}, {"in", "V", 1000}, {"fan", "RPM", 1}};
	char path[300], name[64], label[64];
	DIR *d = opendir("/sys/class/hwmon");
	struct dirent *e;
	int any = 0;
	while (d && (e = readdir(d))) {
		snprintf(path, sizeof path, "/sys/class/hwmon/%s/name", e->d_name);
		if (read_file(path, name, sizeof name) < 0)
			continue;
		for (int t = 0; t < 3; t++)
			for (int i = 0; i < 32; i++) {
				snprintf(path, sizeof path, "/sys/class/hwmon/%s/%s%d_input", e->d_name, k[t].kind, i);
				long long v = read_ll(path, -1000000000);
				if (v == -1000000000)
					continue;
				snprintf(path, sizeof path, "/sys/class/hwmon/%s/%s%d_label", e->d_name, k[t].kind, i);
				if (read_file(path, label, sizeof label) <= 0)
					snprintf(label, sizeof label, "%s%d", k[t].kind, i);
				if (!any++)
					fprintf(o, "Sensors\n");
				fprintf(o, "  %-10s %-14s %8.2f %s%s\n", name, label, v / k[t].div, k[t].unit,
					t == 2 && !v ? "  (stopped or not connected)" : "");
			}
	}
	if (d)
		closedir(d);
	if (!any)
		fprintf(o, "Sensors: none exposed by the system\n");
}

/* A clock that reads earlier than a date we know has passed was reset:
 * on a PC that means the CMOS battery let go. */
static void rtc_check(FILE *o, const char *bios_date)
{
	long long t = read_ll("/sys/class/rtc/rtc0/since_epoch", -1);
	if (t < 0) {
		verdict(o, V_ABSENT, "CMOS clock", "no RTC exposed");
		return;
	}
	int ref = atoi(__DATE__ + 7); /* build year */
	const char *slash = strrchr(bios_date, '/');
	int by = slash ? atoi(slash + 1) : 0;
	if (by && by < 100)
		by += by < 80 ? 2000 : 1900;
	if (by > ref)
		ref = by;
	time_t tt = (time_t)t;
	struct tm tm;
	gmtime_r(&tt, &tm);
	if (tm.tm_year + 1900 < ref)
		verdict(o, V_FAIL, "CMOS clock", "RTC reads %04d-%02d-%02d, before %d: the clock was reset, the CMOS battery is likely flat",
			tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, ref);
	else
		verdict(o, V_PASS, "CMOS clock", "RTC reads %04d-%02d-%02d %02d:%02d UTC, not reset",
			tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
}

void board_show(FILE *o)
{
	static struct smbios s;
	enum { MAX = 1 << 20 };
	uint8_t *t = malloc(MAX);
	FILE *f = fopen("/sys/firmware/dmi/tables/DMI", "rb");
	size_t n = t && f ? fread(t, 1, MAX, f) : 0;
	if (f)
		fclose(f);
	if (n && smbios_parse(t, n, &s) > 0) {
		fprintf(o, "Board\n  %s %s (system: %s %s, serial %s)\n", s.board_maker, s.board_product,
			s.sys_maker, s.sys_product, s.sys_serial[0] ? s.sys_serial : "-");
		fprintf(o, "  BIOS %s %s, dated %s\n", s.bios_vendor, s.bios_ver, s.bios_date);
		fact("system.manufacturer", "%s", s.sys_maker);
		fact("system.product", "%s", s.sys_product);
		fact("system.serial", "%s", s.sys_serial);
		fact("board.manufacturer", "%s", s.board_maker);
		fact("board.product", "%s", s.board_product);
		fact("bios.version", "%s %s (%s)", s.bios_vendor, s.bios_ver, s.bios_date);
		if (s.cpu_ver[0] || s.cpu_socket[0])
			fprintf(o, "  CPU socket %s: %s, %u cores / %u threads, %u MHz (max %u)\n", s.cpu_socket,
				s.cpu_ver, s.cpu_cores, s.cpu_threads, s.cpu_cur_mhz, s.cpu_max_mhz);
		unsigned long long total = 0;
		fprintf(o, "Memory slots (%d)\n", s.nslots);
		for (int i = 0; i < s.nslots; i++) {
			struct dimm *d = &s.dimm[i];
			if (!d->mb) {
				fprintf(o, "  %-16s %-12s empty\n", d->loc, d->bank);
				continue;
			}
			if (d->mb != DIMM_UNKNOWN)
				total += d->mb;
			fprintf(o, "  %-16s %-12s ", d->loc, d->bank);
			if (d->mb == DIMM_UNKNOWN)
				fprintf(o, "size unknown");
			else
				fprintf(o, "%u MB", d->mb);
			fprintf(o, " %s %u MT/s  %s %s  serial %s\n", d->type, d->speed, d->maker, d->part, d->serial);
		}
		fprintf(o, "  installed: %llu MB\n", total);
		fact("memory.installed_mb", "%llu", total);
		fact("memory.slots", "%d", s.nslots);
	} else {
		char a[64] = "", b[64] = "", c[64] = "", d[64] = "";
		read_file("/sys/class/dmi/id/board_vendor", a, sizeof a);
		read_file("/sys/class/dmi/id/board_name", b, sizeof b);
		read_file("/sys/class/dmi/id/bios_version", c, sizeof c);
		read_file("/sys/class/dmi/id/bios_date", s.bios_date, sizeof s.bios_date);
		read_file("/sys/class/dmi/id/product_name", d, sizeof d);
		if (a[0] || b[0] || c[0]) {
			fact("system.product", "%s", d);
			fact("board.manufacturer", "%s", a);
			fact("board.product", "%s", b);
			fact("bios.version", "%s (%s)", c, s.bios_date);
		}
		if (a[0] || b[0] || c[0])
			fprintf(o, "Board (no SMBIOS table, firmware summary)\n  %s %s (%s), BIOS %s dated %s\n", a, b, d, c, s.bios_date);
		else
			verdict(o, V_ABSENT, "Board", "firmware provides no SMBIOS/DMI tables");
	}
	free(t);
	sensors(o);
	rtc_check(o, s.bios_date);
}
