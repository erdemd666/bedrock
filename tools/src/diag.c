/* Motherboard diagnosis: collect what the hardware has already reported (system
 * error log, PCIe and SATA links, memory population, board sensors, CMOS clock)
 * and rank the parts most likely at fault. Every rule is tuned to stay quiet on
 * healthy machines: a false alarm costs more trust than a missed hint. */
#include "bedrock.h"

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/klog.h>
#include <unistd.h>

static const char *const part_names[NPART] = {
	[PART_CPU] = "Processor (CPU)",
	[PART_MEM] = "Memory: modules, slots or the CPU's memory controller",
	[PART_PSU] = "Power supply",
	[PART_CMOS] = "CMOS battery",
	[PART_PCIE] = "PCIe slot, riser or card contacts",
	[PART_SATA] = "SATA cable, port or drive",
	[PART_DISK] = "Disk drive",
	[PART_NVME] = "NVMe drive or its M.2 slot",
	[PART_USB] = "USB port, cable or device",
	[PART_COOLING] = "CPU cooling",
	[PART_BOARD] = "Motherboard",
	[PART_FIRMWARE] = "BIOS firmware",
};

const char *part_name(enum part p)
{
	return p < NPART ? part_names[p] : "?";
}

/* Same part and cause count as one finding seen several times. */
struct finding *finding_add(struct finding *f, int *n, enum part part, int sev, const char *cause, const char *fix)
{
	for (int i = 0; i < *n; i++)
		if (f[i].part == part && !strcmp(f[i].cause, cause)) {
			f[i].count++;
			if (sev > f[i].sev)
				f[i].sev = sev;
			return &f[i];
		}
	if (*n >= FINDINGS_MAX)
		return NULL;
	struct finding *x = &f[(*n)++];
	x->part = part, x->sev = sev, x->count = 1;
	snprintf(x->cause, sizeof x->cause, "%s", cause);
	snprintf(x->fix, sizeof x->fix, "%s", fix ? fix : "");
	return x;
}

static const char FIX_CPU[] = "load BIOS defaults (no overclock), update the BIOS, run the CPU stress test; errors that persist point to the CPU";
static const char FIX_MEM[] = "run the memory test; then test one module at a time in the first slot to find the bad module or slot; turn off XMP/EXPO";
static const char FIX_BOARD[] = "load BIOS defaults; check the CPU socket for bent pins and the board for swollen capacitors; the CPU is the other suspect";
static const char FIX_PCIE[] = "reseat the card or drive, clean its contacts, remove any riser or extension cable, try another slot";
static const char FIX_SATA[] = "replace the SATA data cable, try another port, check the drive's SMART";
static const char FIX_NVME[] = "reseat the drive, try another M.2 slot, update the drive's firmware; check its SMART";
static const char FIX_USB[] = "try another cable, port or device; unplug the front-panel USB cable to test the rear ports alone";
static const char FIX_CMOS[] = "replace the CR2032 coin cell on the board, then set the date and time in the BIOS";
static const char FIX_FW[] = "harmless unless the machine misbehaves; then update the BIOS";

/* IA32_MCi_STATUS, Intel SDM Vol. 3B 16.3.2.2 (status bits) and 16.9 (MCA
 * error codes, bits 15:0). The compound codes are architectural, so they tell
 * memory controller from cache from bus on every Intel and pre-Zen AMD CPU,
 * where bank numbers differ per model. */
void mce_decode(uint64_t st, struct mce_info *m)
{
	unsigned c = st & 0xffff, cf = c & 0xefff; /* bit 12: correction report filtering */
	memset(m, 0, sizeof *m);
	m->valid = st >> 63 & 1;
	m->over = st >> 62 & 1;
	m->uc = st >> 61 & 1;
	m->pcc = st >> 57 & 1;
	m->code = c;
	m->part = PART_CPU;
	if (c == 0x0001)
		m->unit = "unclassified unit";
	else if (c == 0x0002)
		m->unit = "microcode ROM";
	else if (c == 0x0003)
		m->unit = "external agent (chipset or another CPU)", m->part = PART_BOARD;
	else if (c == 0x0004)
		m->unit = "redundancy check (FRC)";
	else if (c == 0x0005)
		m->unit = "internal parity check";
	else if (c == 0x0006)
		m->unit = "SMM handler";
	else if (c == 0x0400)
		m->unit = "internal timer (a core hung)";
	else if ((c & 0xfc00) == 0x0400)
		m->unit = "internal logic";
	else if ((cf & 0xeffc) == 0x000c)
		m->unit = "cache hierarchy";
	else if ((cf & 0xeff0) == 0x0010)
		m->unit = "TLB";
	else if ((cf & 0xef80) == 0x0080)
		m->unit = "memory controller", m->part = PART_MEM;
	else if ((cf & 0xef00) == 0x0100)
		m->unit = "cache";
	else if ((cf & 0xe800) == 0x0800) {
		/* 000F 1PPT RRRR IILL: II = 00 is a memory access (AMD DRAM ECC is reported this way) */
		if ((c >> 2 & 3) == 0)
			m->unit = "memory bus", m->part = PART_MEM;
		else
			m->unit = "bus or interconnect", m->part = PART_BOARD;
	} else
		m->unit = "unknown unit";
}

/* AMD Scalable MCA: the bank type is in MCA_IPID (HardwareID bits 43:32,
 * McaType bits 63:48), table as in the kernel's smca_hwid_mcatypes[]. */
const char *smca_unit(uint64_t ipid, enum part *part)
{
	static const struct { unsigned hwid, type; enum part part; const char *unit; } t[] = {
		{0xb0, 0x0, PART_CPU, "load-store unit"}, {0xb0, 0x10, PART_CPU, "load-store unit"},
		{0xb0, 0x1, PART_CPU, "instruction fetch unit"}, {0xb0, 0x2, PART_CPU, "L2 cache"},
		{0xb0, 0x3, PART_CPU, "decode unit"}, {0xb0, 0x5, PART_CPU, "execution unit"},
		{0xb0, 0x6, PART_CPU, "floating-point unit"}, {0xb0, 0x7, PART_CPU, "L3 cache"},
		{0x2e, 0x0, PART_CPU, "data fabric"}, {0x2e, 0x1, PART_CPU, "data fabric"},
		{0x2e, 0x2, PART_CPU, "data fabric"}, {0x2e, 0x4, PART_CPU, "memory-side cache"},
		{0x96, 0x0, PART_MEM, "memory controller"}, {0x96, 0x1, PART_MEM, "memory controller"},
		{0x05, 0x0, PART_CPU, "parameter block"}, {0xff, 0x0, PART_CPU, "security processor"},
		{0xff, 0x1, PART_CPU, "security processor"}, {0x01, 0x0, PART_CPU, "system management unit"},
		{0x01, 0x1, PART_CPU, "system management unit"}, {0x01, 0x2, PART_CPU, "MP5 unit"},
		{0x01, 0x3, PART_CPU, "MPDMA unit"}, {0x18, 0x0, PART_BOARD, "northbridge I/O unit"},
		{0x46, 0x0, PART_PCIE, "PCIe unit"}, {0x46, 0x1, PART_PCIE, "PCIe unit"},
		{0x50, 0x0, PART_CPU, "xGMI link"}, {0x6c, 0x0, PART_BOARD, "NBIF unit"},
		{0x80, 0x0, PART_BOARD, "SHUB unit"}, {0xa8, 0x0, PART_SATA, "SATA unit"},
		{0xaa, 0x0, PART_USB, "USB unit"}, {0x241, 0x0, PART_CPU, "GMI link"},
		{0x259, 0x0, PART_CPU, "xGMI link"}, {0x267, 0x0, PART_CPU, "WAFL link"},
	};
	unsigned hwid = ipid >> 32 & 0xfff, type = ipid >> 48 & 0xffff;
	for (size_t i = 0; i < sizeof t / sizeof *t; i++)
		if (t[i].hwid == hwid && t[i].type == type) {
			*part = t[i].part;
			return t[i].unit;
		}
	return NULL;
}

/* Kernel messages by source file in Linux 6.12; strings checked against it.
 * The line must contain a and any one of the '|' separated words in b. */
static const struct sig {
	const char *a, *b;
	enum part part;
	int sev;
	const char *cause, *fix;
} sigs[] = {
	/* drivers/thermal/intel/therm_throt.c */
	{"temperature is above threshold", "throttled", PART_COOLING, SEV_MED, "the CPU overheated and slowed itself down",
	 "check the CPU fan spins; reseat the cooler with fresh thermal paste; clean dust from heatsink and case"},
	/* drivers/usb/core/hub.c */
	{"over-current condition", NULL, PART_USB, SEV_MED, "a USB port reported over-current (a short)",
	 "unplug every USB device and the front-panel USB cables; check the front USB header for bent pins"},
	{"device descriptor read", "error", PART_USB, SEV_LOW, "a USB device did not answer", FIX_USB},
	{"device not accepting address", NULL, PART_USB, SEV_LOW, "a USB device did not answer", FIX_USB},
	{"unable to enumerate USB device", NULL, PART_USB, SEV_LOW, "a USB device did not answer", FIX_USB},
	/* drivers/ata/libata-eh.c, libata-sata.c */
	{"error: { ", "UNC ", PART_DISK, SEV_HIGH, "a disk could not read some sectors",
	 "copy off any data you need now, then run the SMART check and surface scan"},
	{"error: { ", "ICRC ", PART_SATA, SEV_MED, "SATA transfers failed their checksum (interface CRC)", FIX_SATA},
	{"SError: { ", "10B8B |Dispar |BadCRC |Handshk |LinkSeq ", PART_SATA, SEV_MED, "the SATA link saw signal errors", FIX_SATA},
	{"failed command: ", NULL, PART_SATA, SEV_MED, "SATA commands failed", FIX_SATA},
	{"limiting SATA link speed", NULL, PART_SATA, SEV_MED, "a SATA link was slowed down after errors", FIX_SATA},
	/* drivers/nvme/host/pci.c, core.c */
	{"controller is down; will reset", NULL, PART_NVME, SEV_HIGH, "an NVMe drive stopped responding", FIX_NVME},
	{"Disabling device after reset failure", NULL, PART_NVME, SEV_HIGH, "an NVMe drive failed and was disabled", FIX_NVME},
	{"QID ", " timeout, ", PART_NVME, SEV_MED, "NVMe commands timed out", FIX_NVME},
	{"Device not ready; aborting", NULL, PART_NVME, SEV_MED, "an NVMe drive did not become ready", FIX_NVME},
	/* drivers/firmware/efi/cper.c: errors the firmware reports (APEI GHES) */
	{"section_type: memory error", NULL, PART_MEM, SEV_MED, "the firmware reported memory errors", FIX_MEM},
	{"section_type: PCIe error", NULL, PART_PCIE, SEV_MED, "the firmware reported PCIe errors", FIX_PCIE},
	{"section_type: ", "general processor error|IA32/X64 processor error", PART_CPU, SEV_MED, "the firmware reported processor errors", FIX_CPU},
	/* drivers/rtc/class.c */
	{"hctosys: unable to read the hardware clock", NULL, PART_CMOS, SEV_LOW, "the real-time clock could not be read", FIX_CMOS},
	/* ACPICA (utxferror.c, utaddress.c), include/linux/printk.h: firmware bugs are
	 * on most machines and almost always harmless */
	{"ACPI BIOS Error", NULL, PART_FIRMWARE, SEV_NOTE, "the BIOS has ACPI bugs (common, usually harmless)", FIX_FW},
	{"ACPI Error: ", NULL, PART_FIRMWARE, SEV_NOTE, "the BIOS has ACPI bugs (common, usually harmless)", FIX_FW},
	{"conflicts with OpRegion", NULL, PART_FIRMWARE, SEV_NOTE,
	 "the BIOS reserves some hardware, so some voltages and fans may not be readable", NULL},
	{"[Firmware Bug]: ", NULL, PART_FIRMWARE, SEV_NOTE, "the BIOS has bugs the system works around (common, usually harmless)", FIX_FW},
};

static int has_any(const char *line, const char *words)
{
	char w[64];
	while (words && *words) {
		size_t k = strcspn(words, "|");
		snprintf(w, sizeof w, "%.*s", (int)k, words);
		if (strstr(line, w))
			return 1;
		words += k + (words[k] == '|');
	}
	return 0;
}

static const char *pci_addr(const char *s, char out[13])
{
	for (; *s; s++) {
		unsigned d, b, sl, fn;
		int k = 0;
		if (sscanf(s, "%4x:%2x:%2x.%1x%n", &d, &b, &sl, &fn, &k) == 4 && k == 12) {
			snprintf(out, 13, "%.12s", s);
			return out;
		}
	}
	return NULL;
}

struct mce_ev { int bank, lines; uint64_t status, ipid; };

static void mce_finish(struct mce_ev *e, struct finding *f, int *n)
{
	struct mce_info m;
	char cause[160];
	if (e->lines < 0)
		return;
	mce_decode(e->status, &m);
	e->lines = -1;
	if (!m.valid)
		return;
	enum part part = m.part;
	const char *unit = e->ipid ? smca_unit(e->ipid, &part) : NULL;
	if (!unit)
		unit = m.unit, part = m.part;
	snprintf(cause, sizeof cause, "%s hardware errors from the CPU's %s (bank %d)",
		 m.uc ? (m.pcc ? "fatal" : "uncorrected") : "corrected", unit, e->bank);
	finding_add(f, n, part, m.uc ? SEV_HIGH : SEV_MED, cause,
		    part == PART_MEM ? FIX_MEM : part == PART_CPU ? FIX_CPU : part == PART_PCIE ? FIX_PCIE :
		    part == PART_SATA ? FIX_SATA : part == PART_USB ? FIX_USB : FIX_BOARD);
}

/* One line of the kernel log; ev carries a machine check across the lines that
 * describe it (status first, IPID on a later line). Returns 1 if it matched. */
static int klog_line(const char *l, struct mce_ev *ev, int *logged, struct finding *f, int *n)
{
	char cause[160], addr[13];
	const char *p;
	unsigned long long v;
	int bank;
	if (ev->lines >= 0 && ++ev->lines > 6)
		mce_finish(ev, f, n);
	/* arch/x86/kernel/cpu/mce/core.c __print_mce() */
	if (strstr(l, "[Hardware Error]: CPU ") && strstr(l, "Machine Check") && (p = strstr(l, " Bank ")) &&
	    sscanf(p, " Bank %d: %llx", &bank, &v) == 2) {
		mce_finish(ev, f, n);
		*ev = (struct mce_ev){bank, 0, v, 0};
		return 1;
	}
	/* drivers/edac/mce_amd.c decode_mce(), which replaces the above on AMD */
	if (strstr(l, "[Hardware Error]: ") && (p = strstr(l, " MC")) && sscanf(p, " MC%d_STATUS[", &bank) == 1 &&
	    (p = strstr(p, "]: 0x")) && sscanf(p, "]: 0x%llx", &v) == 1) {
		mce_finish(ev, f, n);
		*ev = (struct mce_ev){bank, 0, v, 0};
		return 1;
	}
	if (ev->lines >= 0 && strstr(l, "[Hardware Error]: ") && (p = strstr(l, "IPID")) &&
	    (sscanf(p, "IPID: 0x%llx", &v) == 1 || sscanf(p, "IPID %llx", &v) == 1)) {
		ev->ipid = v;
		return 1;
	}
	if (strstr(l, "Machine check events logged")) {
		(*logged)++;
		return 1;
	}
	/* drivers/edac/edac_mc.c: "EDAC MC0: 1 CE <msg> on <label> (<location> ...)" */
	if ((p = strstr(l, "EDAC MC")) && (strstr(p, " CE ") || strstr(p, " UE "))) {
		int ue = strstr(p, " UE ") != NULL, cnt = 1;
		const char *c = strchr(p, ':'), *paren = strstr(p, " (");
		if (c)
			sscanf(c + 1, "%d", &cnt);
		const char *on = NULL;
		for (const char *q = p; paren && (q = strstr(q, " on ")) && q < paren; q++)
			on = q;
		if (on)
			snprintf(cause, sizeof cause, "%s memory errors on %.*s", ue ? "uncorrected" : "corrected",
				 (int)(paren - on - 4), on + 4);
		else
			snprintf(cause, sizeof cause, "%s memory errors", ue ? "uncorrected" : "corrected");
		struct finding *x = finding_add(f, n, PART_MEM, ue ? SEV_HIGH : SEV_MED, cause, FIX_MEM);
		if (x && cnt > 1)
			x->count += cnt - 1;
		return 1;
	}
	/* drivers/pci/pcie/aer.c aer_print_error(): one line per error and device */
	if ((p = strstr(l, "PCIe Bus Error: severity="))) {
		int fatal = strstr(p, "(Fatal)") != NULL, nonfatal = strstr(p, "(Non-Fatal)") != NULL;
		const char *dev = pci_addr(l, addr);
		snprintf(cause, sizeof cause, "%s PCIe errors at %s", fatal ? "fatal" : nonfatal ? "uncorrected" : "corrected",
			 dev ? dev : "an unknown device");
		finding_add(f, n, PART_PCIE, fatal ? SEV_HIGH : nonfatal ? SEV_MED : SEV_LOW, cause, FIX_PCIE);
		return 1;
	}
	for (size_t i = 0; i < sizeof sigs / sizeof *sigs; i++)
		if (strstr(l, sigs[i].a) && (!sigs[i].b || has_any(l, sigs[i].b))) {
			finding_add(f, n, sigs[i].part, sigs[i].sev, sigs[i].cause, sigs[i].fix);
			return 1;
		}
	return 0;
}

/* Classify a whole kernel log into findings; returns the new count. */
int klog_scan(const char *log, struct finding *f, int n)
{
	struct mce_ev ev = {.lines = -1};
	int logged = 0, before = n;
	char line[512];
	while (*log) {
		size_t k = strcspn(log, "\n");
		snprintf(line, sizeof line, "%.*s", (int)(k < sizeof line ? k : sizeof line - 1), log);
		klog_line(line, &ev, &logged, f, &n);
		log += k + (log[k] == '\n');
	}
	mce_finish(&ev, f, &n);
	int decoded = 0;
	for (int i = before; i < n; i++)
		decoded |= strstr(f[i].cause, "hardware errors from the CPU") != NULL;
	if (logged && !decoded)
		finding_add(f, &n, PART_CPU, SEV_MED, "the CPU logged hardware errors it could not describe here",
			    "run the CPU stress test and the memory test; load BIOS defaults");
	return n;
}

static char *klog_read(void)
{
	int size = klogctl(10, NULL, 0); /* SYSLOG_ACTION_SIZE_BUFFER */
	char *buf = size > 0 ? malloc(size + 1) : NULL;
	int len = buf ? klogctl(3, buf, size) : -1; /* SYSLOG_ACTION_READ_ALL */
	if (len < 0) {
		free(buf);
		return NULL;
	}
	buf[len] = 0;
	return buf;
}

/* "TOTAL_ERR_COR 3" style line of aer_dev_* (drivers/pci/pcie/aer.c). */
long long aer_total(const char *text)
{
	const char *p = text ? strstr(text, "TOTAL_") : NULL;
	if (!p)
		return -1;
	p = strchr(p, ' ');
	return p ? strtoll(p + 1, NULL, 10) : -1;
}

/* sata_spd_string() in libata-core.c: "1.5 Gbps", "3.0 Gbps", "6.0 Gbps", "<unknown>". */
int sata_gbps10(const char *s)
{
	return (int)(atof(s) * 10 + 0.5);
}

/* Speeds in tenths of Gbps, 0 unknown. limit below hw means libata slowed the
 * link after errors (sata_down_spd_limit()). A drive or port that is simply
 * older runs slower legitimately, so only the lower of the two ends counts. */
enum verdict sata_judge(int hw, int limit, int cur, int drive, int errs, char *why, size_t n)
{
	int want = hw && drive ? (hw < drive ? hw : drive) : hw ? hw : drive;
	if (!cur) {
		snprintf(why, n, "no device, or link down");
		return V_ABSENT;
	}
	if (hw && limit && limit < hw) {
		snprintf(why, n, "slowed to %.1f Gbps after errors (port supports %.1f)", limit / 10.0, hw / 10.0);
		return V_FAIL;
	}
	if (want && cur < want) {
		snprintf(why, n, "runs at %.1f Gbps although port and drive support %.1f", cur / 10.0, want / 10.0);
		return V_INCONCLUSIVE;
	}
	if (errs > 0) {
		snprintf(why, n, "%d recent error%s on this link", errs, errs > 1 ? "s" : "");
		return V_INCONCLUSIVE;
	}
	snprintf(why, n, "%.1f Gbps, no errors", cur / 10.0);
	return V_PASS;
}

static const struct { const char *name; double lo, hi; } rails[] = {
	[R_12V] = {"+12V", 10.8, 13.2}, [R_5V] = {"+5V", 4.5, 5.5}, [R_3V3] = {"+3.3V", 2.97, 3.63},
	[R_3VSB] = {"3VSB", 2.97, 3.63}, [R_5VSB] = {"5VSB", 4.5, 5.5}, [R_VBAT] = {"VBAT", 2.8, 0},
};

static int prefix(const char *s, const char *p)
{
	return !strncasecmp(s, p, strlen(p));
}

/* Which rail input idx of chip measures, or -1. Only inputs whose scaling is
 * known: raw Super I/O pins sit behind board-specific dividers. */
int rail_of(const char *chip, int idx, const char *label)
{
	/* Fixed on-chip dividers the driver applies: 3VCC, 3VSB, VBAT
	 * (nct6775-core.c scale_in/scale_in_6798, w83627ehf.c scale_in_common). */
	if (!strncmp(chip, "nct67", 5) || !strcmp(chip, "w83627ehf") || !strcmp(chip, "w83627dhg") ||
	    !strcmp(chip, "w83667hg"))
		return idx == 3 ? R_3V3 : idx == 7 ? R_3VSB : idx == 8 ? R_VBAT : -1;
	/* it87.c labels: "+3.3V" and "3VSB" only on chips whose internal inputs it
	 * scales; Vbat is measured directly. "+5V", "5VSB", "AVCC" are raw on old chips. */
	if (!strcmp(chip, "it87") || !strncmp(chip, "it8", 3))
		return !strcmp(label, "+3.3V") ? R_3V3 : !strcmp(label, "3VSB") ? R_3VSB : !strcmp(label, "Vbat") ? R_VBAT : -1;
	/* Firmware-scaled values with names (ASUS ACPI ATK0110, ASUS WMI). */
	if (!strcmp(chip, "atk0110") || !strcmp(chip, "asus_wmi_sensors")) {
		const char *s = label + (label[0] == '+');
		if (prefix(s, "12"))
			return R_12V;
		if (prefix(s, "5VSB"))
			return R_5VSB;
		if (s[0] == '5' && !isdigit((unsigned char)s[1]) && s[1] != '.')
			return R_5V;
		if (prefix(s, "3VSB"))
			return R_3VSB;
		if (prefix(s, "3.3") || prefix(s, "3VCC"))
			return R_3V3;
		if (prefix(s, "VBAT"))
			return R_VBAT;
	}
	return -1;
}

/* ±10%: wider than ATX's ±5% because Super I/O readings are approximate.
 * CMOS battery: a CR2032 is 3.0 V nominal; below 2.8 V it no longer holds the clock reliably. */
enum verdict rail_judge(int r, double v, char *why, size_t n)
{
	if (v < 0.5) {
		snprintf(why, n, "%s reads %.2f V: not wired to the sensor", rails[r].name, v);
		return V_INCONCLUSIVE;
	}
	if (v < rails[r].lo || (rails[r].hi && v > rails[r].hi)) {
		if (r == R_VBAT)
			snprintf(why, n, "VBAT %.2f V, below 2.8 V: the CMOS battery is weak", v);
		else
			snprintf(why, n, "%s at %.2f V, outside %.2f-%.2f V", rails[r].name, v, rails[r].lo, rails[r].hi);
		return V_FAIL;
	}
	snprintf(why, n, "%s %.2f V", rails[r].name, v);
	return V_PASS;
}

/* Channel letter from SMBIOS locators: "ChannelA-DIMM0", "P0 CHANNEL B",
 * "DIMM_A1", "A2". 0 when the names do not say. */
static char dimm_channel(const struct dimm *d)
{
	const char *s[2] = {d->bank, d->loc};
	for (int i = 0; i < 2; i++)
		for (const char *p = s[i]; *p; p++)
			if (prefix(p, "channel")) {
				p += 7;
				while (*p == ' ' || *p == '_' || *p == '-')
					p++;
				if (isalpha((unsigned char)*p))
					return toupper((unsigned char)*p);
			}
	const char *l = d->loc;
	if (prefix(l, "DIMM"))
		l += 4;
	while (*l == ' ' || *l == '_' || *l == '-')
		l++;
	if (toupper((unsigned char)l[0]) >= 'A' && toupper((unsigned char)l[0]) <= 'H' && isdigit((unsigned char)l[1]))
		return toupper((unsigned char)l[0]);
	return 0;
}

/* SMBIOS type 17 against the RAM the firmware handed over (ram_mb). A whole
 * module that the firmware lists but the system cannot use is a bad module,
 * slot, channel or memory controller. bits = 32 for the non-PAE build, which
 * cannot see memory remapped above 4 GB. */
enum verdict mem_judge(const struct smbios *s, unsigned long long ram_mb, int bits, int *notes, char *why, size_t n)
{
	unsigned long long installed = 0;
	unsigned smallest = 0, n_pop = 0, chans = 0, popchans = 0;
	const struct dimm *first = NULL;
	*notes = 0;
	for (int i = 0; i < s->nslots; i++) {
		const struct dimm *d = &s->dimm[i];
		char c = dimm_channel(d);
		if (c)
			chans |= 1u << (c - 'A');
		if (!d->mb)
			continue;
		if (d->mb == DIMM_UNKNOWN) {
			snprintf(why, n, "the firmware does not give every module's size");
			return V_INCONCLUSIVE;
		}
		if (c)
			popchans |= 1u << (c - 'A');
		installed += d->mb;
		if (!smallest || d->mb < smallest)
			smallest = d->mb;
		if (first && (first->mb != d->mb || first->speed != d->speed || strcmp(first->part, d->part)))
			*notes |= MEM_MIXED;
		first = first ? first : d;
		n_pop++;
	}
	if (__builtin_popcount(chans) >= 2 && __builtin_popcount(popchans) == 1)
		*notes |= MEM_ONE_CHANNEL;
	if (!installed || !ram_mb) {
		snprintf(why, n, "the firmware lists no memory modules");
		return V_INCONCLUSIVE;
	}
	/* 32-bit, or nothing above 4 GB: the PCI hole below 4 GB can hide up to 1.5 GB */
	unsigned long long expected = installed;
	if (bits == 32 && expected > 2560)
		expected = 2560;
	unsigned long long missing = expected > ram_mb ? expected - ram_mb : 0;
	if (missing >= smallest * 3ull / 4 && missing >= 32) {
		snprintf(why, n, "%llu MB installed in %u module%s, only %llu MB usable: about %llu MB (a whole module) is missing",
			 installed, n_pop, n_pop > 1 ? "s" : "", ram_mb, missing);
		return V_FAIL;
	}
	snprintf(why, n, "%llu MB installed in %u module%s, %llu MB usable%s", installed, n_pop, n_pop > 1 ? "s" : "", ram_mb,
		 expected < installed ? ": the 32-bit build cannot see it all, use the 64-bit one to check" : "");
	return expected < installed ? V_INCONCLUSIVE : V_PASS;
}

/* ---- Evidence from the running system ---- */

static void slot_of(const char *addr, char *out, size_t n)
{
	char p[PATH_MAX], a[32];
	DIR *d = opendir("/sys/bus/pci/slots");
	struct dirent *e;
	out[0] = 0;
	while (d && (e = readdir(d))) {
		snprintf(p, sizeof p, "/sys/bus/pci/slots/%s/address", e->d_name);
		if (e->d_name[0] != '.' && read_file(p, a, sizeof a) > 0 && !strncmp(addr, a, strlen(a))) {
			snprintf(out, n, " slot %s", e->d_name);
			break;
		}
	}
	if (d)
		closedir(d);
}

static void ev_pcie(FILE *o, struct finding *f, int *n)
{
	static const char *idpaths[] = {"/usr/share/misc/pci.ids", "/usr/share/hwdata/pci.ids"};
	FILE *ids = NULL;
	for (int i = 0; i < 2 && !ids; i++)
		ids = fopen(idpaths[i], "r");
	DIR *d = opendir("/sys/bus/pci/devices");
	struct dirent *e;
	int links = 0, bad = 0, nodrv = 0;
	char p[PATH_MAX], buf[4096], why[160], cause[160], vn[128], dn[160], slot[64];
	fprintf(o, "PCIe links\n");
	while (d && (e = readdir(d))) {
		const char *a = e->d_name;
		if (a[0] == '.')
			continue;
		snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/class", a);
		unsigned cls = read_file(p, buf, sizeof buf) > 0 ? strtoul(buf, NULL, 16) : 0;
		if (cls >> 8 == 0x0c03) { /* USB controller */
			snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/driver", a);
			if (access(p, F_OK))
				nodrv++;
		}
		/* AER counters: totals since boot, also kept when the log wrapped */
		static const char *aer[] = {"aer_dev_fatal", "aer_dev_nonfatal", "aer_dev_correctable"};
		for (int k = 0; k < 3; k++) {
			snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/%s", a, aer[k]);
			long long t = read_file(p, buf, sizeof buf) > 0 ? aer_total(buf) : -1;
			if (t <= 0)
				continue;
			snprintf(cause, sizeof cause, "%s PCIe errors at %s", k == 0 ? "fatal" : k == 1 ? "uncorrected" : "corrected", a);
			struct finding *x = finding_add(f, n, PART_PCIE, k == 0 ? SEV_HIGH : k == 1 ? SEV_MED : SEV_LOW, cause, FIX_PCIE);
			if (x && x->count - 1 < t)
				x->count = t; /* the log lines of these errors were already counted */
			else if (x)
				x->count--;
		}
		struct pcie_link l;
		if (cls >> 8 == 0x0604 || pcie_link_read(a, &l)) /* bridges: judged through their endpoints */
			continue;
		links++;
		unsigned ven = 0, dev = 0;
		snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/vendor", a);
		if (read_file(p, buf, sizeof buf) > 0)
			ven = strtoul(buf, NULL, 16);
		snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/device", a);
		if (read_file(p, buf, sizeof buf) > 0)
			dev = strtoul(buf, NULL, 16);
		vn[0] = dn[0] = 0;
		if (ids)
			pciids_lookup(ids, ven, dev, vn, sizeof vn, dn, sizeof dn);
		slot_of(a, slot, sizeof slot);
		int gpu = cls >> 16 == 0x03;
		enum verdict v = pcie_link_judge(&l, gpu, why, sizeof why);
		fprintf(o, "  %s%s %.40s: x%lld %.1f GT/s (max x%lld %.1f GT/s)%s\n", a, slot, dn[0] ? dn : vn[0] ? vn : "device",
			l.cw, l.cs, l.mw, l.ms, v == V_FAIL ? "  NARROW" : "");
		if (v == V_FAIL) {
			bad++;
			snprintf(cause, sizeof cause, "the link at %s%s is narrower than both ends support (x%lld)", a, slot, l.cw);
			finding_add(f, n, PART_PCIE, SEV_MED, cause, FIX_PCIE);
		} else if (v == V_INCONCLUSIVE && l.cw && !gpu) {
			snprintf(cause, sizeof cause, "the link at %s%s runs slower than both ends support", a, slot);
			finding_add(f, n, PART_PCIE, SEV_LOW, cause, FIX_PCIE);
		}
	}
	if (d)
		closedir(d);
	if (ids)
		fclose(ids);
	if (!links)
		verdict(o, V_ABSENT, "PCIe links", "no PCI Express devices");
	else if (bad)
		verdict(o, V_FAIL, "PCIe links", "%d of %d links narrower than both ends support", bad, links);
	else
		verdict(o, V_PASS, "PCIe links", "%d links at full width", links);
	if (nodrv) {
		snprintf(cause, sizeof cause, "%d USB controller%s without a driver here: %s ports do not work in Bedrock",
			 nodrv, nodrv > 1 ? "s" : "", nodrv > 1 ? "their" : "its");
		finding_add(f, n, PART_USB, SEV_NOTE, cause, "not a fault by itself");
	}
}

static void ev_sata(FILE *o, struct finding *f, int *n)
{
	DIR *d = opendir("/sys/class/ata_link");
	struct dirent *e;
	char p[PATH_MAX], hw[32], lim[32], cur[32], buf[4096], why[160], cause[160];
	int links = 0, fails = 0;
	fprintf(o, "SATA links\n");
	while (d && (e = readdir(d))) {
		const char *k = e->d_name;
		if (strncmp(k, "link", 4) || strchr(k, '.')) /* port multiplier links: skip */
			continue;
		snprintf(p, sizeof p, "/sys/class/ata_link/%s/sata_spd", k);
		if (read_file(p, cur, sizeof cur) <= 0)
			continue;
		snprintf(p, sizeof p, "/sys/class/ata_link/%s/hw_sata_spd_limit", k);
		read_file(p, hw, sizeof hw);
		snprintf(p, sizeof p, "/sys/class/ata_link/%s/sata_spd_limit", k);
		read_file(p, lim, sizeof lim);
		/* IDENTIFY word 76 bits 1-3: SATA Gen1/2/3 support (ACS-3 7.12.7.37) */
		int drive = 0, errs = 0;
		unsigned w[256];
		snprintf(p, sizeof p, "/sys/class/ata_device/dev%s.0/id", k + 4);
		FILE *idf = fopen(p, "r");
		int nw = 0;
		while (idf && nw < 256 && fscanf(idf, "%x", &w[nw]) == 1)
			nw++;
		if (idf)
			fclose(idf);
		if (nw > 76 && w[76] && w[76] != 0xffff)
			drive = w[76] & 8 ? 60 : w[76] & 4 ? 30 : w[76] & 2 ? 15 : 0;
		snprintf(p, sizeof p, "/sys/class/ata_device/dev%s.0/ering", k + 4);
		if (read_file(p, buf, sizeof buf) > 0)
			for (char *q = buf; (q = strchr(q, '[')); q++)
				errs++;
		enum verdict v = sata_judge(sata_gbps10(hw), sata_gbps10(lim), sata_gbps10(cur), drive, errs, why, sizeof why);
		if (v == V_ABSENT)
			continue;
		links++;
		fprintf(o, "  ata%s: %s\n", k + 4, why);
		if (v == V_FAIL || v == V_INCONCLUSIVE) {
			snprintf(cause, sizeof cause, "SATA port ata%s %s", k + 4, why);
			struct finding *x = finding_add(f, n, PART_SATA, v == V_FAIL ? SEV_MED : SEV_LOW, cause, FIX_SATA);
			if (x && errs > 1)
				x->count = errs;
			fails += v == V_FAIL;
		}
	}
	if (d)
		closedir(d);
	if (!links)
		verdict(o, V_ABSENT, "SATA links", "no SATA device connected");
	else if (fails)
		verdict(o, V_FAIL, "SATA links", "%d of %d links slowed down after errors", fails, links);
	else
		verdict(o, V_PASS, "SATA links", "%d links checked", links);
}

/* RAM the firmware handed to the system: top-level "System RAM" in /proc/iomem. */
static unsigned long long ram_mb(void)
{
	FILE *f = fopen("/proc/iomem", "r");
	char l[256];
	unsigned long long a, b, sum = 0;
	while (f && fgets(l, sizeof l, f))
		if (l[0] != ' ' && strstr(l, ": System RAM") && sscanf(l, "%llx-%llx", &a, &b) == 2 && b > a)
			sum += b - a + 1;
	if (f)
		fclose(f);
	if (sum)
		return sum >> 20;
	/* No addresses (not root): MemTotal plus what the kernel keeps for itself. */
	f = fopen("/proc/meminfo", "r");
	unsigned long long kb = 0;
	if (f && fscanf(f, "MemTotal: %llu", &kb) != 1)
		kb = 0;
	if (f)
		fclose(f);
	return (kb + kb / 32) >> 10;
}

static void ev_memory(FILE *o, const struct smbios *s, struct finding *f, int *n)
{
	char why[200];
	int notes;
	fprintf(o, "Memory\n");
	enum verdict v = mem_judge(s, ram_mb(), sizeof(long) * 8, &notes, why, sizeof why);
	verdict(o, v, "Memory population", "%s", why);
	if (v == V_FAIL)
		finding_add(f, n, PART_MEM, SEV_HIGH, why,
			    "reseat every module; try each module alone in the first slot; a slot that never works points to "
			    "the board or the CPU (check the socket for bent pins). Check the BIOS shared graphics memory first");
	if (notes & MEM_MIXED)
		finding_add(f, n, PART_MEM, SEV_NOTE, "modules differ in size, speed or part number",
			    "mixed modules run at the slowest one's speed; a matched set is more stable");
	if (notes & MEM_ONE_CHANNEL)
		finding_add(f, n, PART_MEM, SEV_NOTE, "all modules are on one memory channel",
			    "for speed, move one module to the other channel's slot (see the board manual)");
}

static void ev_sensors(FILE *o, struct finding *f, int *n)
{
	DIR *d = opendir("/sys/class/hwmon");
	struct dirent *e;
	char p[300], chip[64], label[64], why[160], cause[200];
	int judged = 0, bad = 0, idle_fans = 0;
	fprintf(o, "Board sensors\n");
	while (d && (e = readdir(d))) {
		snprintf(p, sizeof p, "/sys/class/hwmon/%s/name", e->d_name);
		if (read_file(p, chip, sizeof chip) < 0)
			continue;
		for (int i = 0; i < 32; i++) {
			snprintf(p, sizeof p, "/sys/class/hwmon/%s/in%d_input", e->d_name, i);
			long long mv = read_ll(p, -1);
			if (mv < 0)
				continue;
			snprintf(p, sizeof p, "/sys/class/hwmon/%s/in%d_label", e->d_name, i);
			if (read_file(p, label, sizeof label) < 0)
				label[0] = 0;
			int r = rail_of(chip, i, label);
			if (r < 0)
				continue;
			enum verdict v = rail_judge(r, mv / 1000.0, why, sizeof why);
			fprintf(o, "  %-10s %s%s\n", chip, why, v == V_FAIL ? "  OUT OF RANGE" : "");
			if (v == V_INCONCLUSIVE)
				continue;
			judged++;
			if (v != V_FAIL)
				continue;
			bad++;
			if (r == R_VBAT)
				finding_add(f, n, PART_CMOS, SEV_MED, why, FIX_CMOS);
			else
				finding_add(f, n, PART_PSU, SEV_MED, why,
					    "measure the rail with a multimeter or try a known-good power supply; "
					    "board sensors can be off, so confirm before replacing");
		}
		for (int i = 1; i < 16; i++) {
			snprintf(p, sizeof p, "/sys/class/hwmon/%s/fan%d_input", e->d_name, i);
			if (read_ll(p, -1) != 0)
				continue;
			snprintf(p, sizeof p, "/sys/class/hwmon/%s/fan%d_label", e->d_name, i);
			if (read_file(p, label, sizeof label) > 0 && strcasestr(label, "CPU") && !strcasestr(label, "OPT")) {
				snprintf(cause, sizeof cause, "the CPU fan (%s) reads 0 RPM", label);
				finding_add(f, n, PART_COOLING, SEV_MED, cause,
					    "check the fan spins and is plugged into the CPU_FAN header; pumps and fanless coolers read 0 normally");
			} else
				idle_fans++;
		}
		for (int i = 1; i < 16; i++) {
			snprintf(p, sizeof p, "/sys/class/hwmon/%s/temp%d_label", e->d_name, i);
			if (read_file(p, label, sizeof label) <= 0 ||
			    !(prefix(label, "SYSTIN") || strcasestr(label, "Motherboard") || prefix(label, "MB ")))
				continue;
			snprintf(p, sizeof p, "/sys/class/hwmon/%s/temp%d_input", e->d_name, i);
			long long t = read_ll(p, -1000000) / 1000;
			if (t >= 65 && t <= 100) { /* outside that a Super I/O input is usually not connected */
				snprintf(cause, sizeof cause, "the board reads %lld C (%s) at idle", t, label);
				finding_add(f, n, PART_COOLING, SEV_LOW, cause, "check the case fans and airflow");
			}
		}
	}
	if (d)
		closedir(d);
	if (idle_fans) {
		snprintf(cause, sizeof cause, "%d fan input%s read 0 RPM: nothing connected, or the fan stopped", idle_fans,
			 idle_fans > 1 ? "s" : "");
		finding_add(f, n, PART_COOLING, SEV_NOTE, cause, "check that every fan you can see spins");
	}
	if (!judged)
		verdict(o, V_ABSENT, "Board voltages", "no voltage sensor with a known scale (many boards hide it from the system)");
	else if (bad)
		verdict(o, V_FAIL, "Board voltages", "%d of %d rails out of range", bad, judged);
	else
		verdict(o, V_PASS, "Board voltages", "%d rails within range", judged);
	long long crit, t = cpu_temp(&crit);
	if (t <= -1000000) {
		verdict(o, V_ABSENT, "CPU idle temperature", "no CPU temperature sensor");
	} else if (t >= 80000) {
		verdict(o, V_FAIL, "CPU idle temperature", "%lld C at idle", t / 1000);
		snprintf(cause, sizeof cause, "the CPU is at %lld C while idle", t / 1000);
		finding_add(f, n, PART_COOLING, SEV_MED, cause,
			    "reseat the cooler with fresh thermal paste and check its fan (if a stress test just ran, wait a minute and retry)");
	} else if (t >= 70000) {
		verdict(o, V_INCONCLUSIVE, "CPU idle temperature", "%lld C: warm for an idle CPU", t / 1000);
		snprintf(cause, sizeof cause, "the CPU is at %lld C while idle", t / 1000);
		finding_add(f, n, PART_COOLING, SEV_LOW, cause, "clean the heatsink and check the CPU fan");
	} else {
		verdict(o, V_PASS, "CPU idle temperature", "%lld C", t / 1000);
	}
}

static int by_rank(const void *a, const void *b)
{
	const struct finding *x = a, *y = b;
	return x->sev != y->sev ? y->sev - x->sev : y->count - x->count;
}

static void rank(FILE *o, struct finding *f, int n)
{
	static const char *sev[] = {"note", "minor", "likely", "strong"};
	int shown = 0, done[NPART] = {0}, real = 0;
	for (int i = 0; i < n; i++)
		if (f[i].sev == SEV_LOW && f[i].count >= 10) /* the same small error over and over */
			f[i].sev = SEV_MED;
	qsort(f, n, sizeof *f, by_rank);
	fprintf(o, "\nLikely faulty parts, most likely first\n");
	for (int i = 0; i < n; i++) {
		if (f[i].sev == SEV_NOTE || done[f[i].part])
			continue;
		done[f[i].part] = 1;
		fprintf(o, "  %d. %s [%s]\n", ++shown, part_name(f[i].part), sev[f[i].sev]);
		for (int j = i; j < n; j++)
			if (f[j].part == f[i].part && f[j].sev > SEV_NOTE) {
				fprintf(o, "     - %s%s", f[j].cause, f[j].count > 1 ? "" : "\n");
				if (f[j].count > 1)
					fprintf(o, " (%d times)\n", f[j].count);
			}
		fprintf(o, "     Try: %s\n", f[i].fix);
	}
	if (!shown)
		fprintf(o, "  None. Nothing here points to a faulty part. If the machine still fails,\n"
			   "  run the memory test and the CPU stress test, then check the disks.\n");
	for (int i = 0; i < n; i++)
		if (f[i].sev == SEV_NOTE) {
			if (!real++)
				fprintf(o, "Notes\n");
			fprintf(o, "  - %s%s%s\n", f[i].cause, f[i].fix[0] ? ": " : "", f[i].fix);
		}
}

void board_diag(FILE *o)
{
	static struct finding f[FINDINGS_MAX];
	static struct smbios s;
	int n = 0;
	fprintf(o, "Motherboard diagnosis\n\nSystem error log\n");
	char *log = klog_read();
	int log_ok = log != NULL;
	if (log) {
		n = klog_scan(log, f, n);
		free(log);
		int worst = 0, cnt = 0;
		for (int i = 0; i < n; i++)
			if (f[i].sev > SEV_NOTE) {
				fprintf(o, "  %s (%d)\n", f[i].cause, f[i].count);
				cnt++;
				worst = f[i].sev > worst ? f[i].sev : worst;
			}
		if (worst >= SEV_MED)
			verdict(o, V_FAIL, "Hardware error log", "%d kinds of hardware errors reported since start", cnt);
		else
			verdict(o, V_PASS, "Hardware error log", cnt ? "%d kinds of minor events only" : "no hardware errors reported", cnt);
	} else {
		verdict(o, V_INCONCLUSIVE, "Hardware error log", "the system error log is not readable");
	}
	ev_pcie(o, f, &n);
	ev_sata(o, f, &n);
	smbios_read(&s);
	ev_memory(o, &s, f, &n);
	ev_sensors(o, f, &n);
	fprintf(o, "CMOS clock\n");
	if (rtc_check(o, s.bios_date) == V_FAIL)
		finding_add(f, &n, PART_CMOS, SEV_MED, "the clock was reset: the CMOS battery is likely flat", FIX_CMOS);

	rank(o, f, n);
	int real = 0, worst = 0;
	for (int i = 0; i < n; i++)
		if (f[i].sev > SEV_NOTE) {
			real++;
			worst = f[i].sev > worst ? f[i].sev : worst;
		}
	fact("board.findings", "%d", real);
	if (real) /* f is sorted: the first real finding ranks highest */
		fact("board.top_finding", "%s: %s", part_name(f[0].part), f[0].cause);
	else
		fact("board.top_finding", "none");
	fprintf(o, "\nA machine that does not start far enough to run this: open\n"
		   "no-post-guide.html on this stick (any phone or PC can read it) for\n"
		   "beep codes, board lights and the minimum-parts test.\n");
	if (worst >= SEV_MED)
		verdict(o, V_FAIL, "Motherboard diagnosis", "%s", f[0].cause);
	else if (real || !log_ok)
		verdict(o, V_INCONCLUSIVE, "Motherboard diagnosis", "%d minor finding%s, no clear fault%s", real,
			real == 1 ? "" : "s", log_ok ? "" : " (error log not readable)");
	else
		verdict(o, V_PASS, "Motherboard diagnosis", "no faulty part found");
}
