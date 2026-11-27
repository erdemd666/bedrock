#include "bedrock.h"

#include <fcntl.h>
#include <linux/nvme_ioctl.h>
#include <scsi/sg.h>
#include <stdarg.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* dir: SAT_NONE, SAT_IN or SAT_OUT. */
static int sg_io_sense(int fd, const uint8_t *cdb, int cdblen, int dir, void *buf, unsigned len, unsigned tmo_ms,
	uint8_t sense[32])
{
	static const int sgdir[] = {SG_DXFER_NONE, SG_DXFER_FROM_DEV, SG_DXFER_TO_DEV};
	memset(sense, 0, 32);
	sg_io_hdr_t h;
	memset(&h, 0, sizeof h);
	h.interface_id = 'S';
	h.dxfer_direction = sgdir[dir];
	h.cmd_len = cdblen;
	h.mx_sb_len = 32;
	h.dxfer_len = len;
	h.dxferp = buf;
	h.cmdp = (unsigned char *)cdb;
	h.sbp = sense;
	h.timeout = tmo_ms;
	if (ioctl(fd, SG_IO, &h) < 0 || h.host_status || (h.driver_status & ~0x08))
		return -1;
	if (h.status == 0)
		return 0;
	/* libata answers some pass-through commands with CHECK CONDITION carrying
	 * NO SENSE / RECOVERED ERROR; the data is still valid. */
	if (h.status == 2 && h.sb_len_wr) {
		int key = (sense[0] & 0x7f) >= 0x72 ? sense[1] & 0xf : sense[2] & 0xf;
		return key <= 1 ? 0 : -1;
	}
	return -1;
}

static int sg_io(int fd, const uint8_t *cdb, int cdblen, int dir, void *buf, unsigned len, unsigned tmo_ms)
{
	uint8_t sense[32];
	return sg_io_sense(fd, cdb, cdblen, dir, buf, len, tmo_ms, sense);
}

/* ATA PASS-THROUGH(16) (SAT-4 6.2.2): PROTOCOL non-data / PIO data-in / PIO data-out;
 * T_DIR, BYTE_BLOCK and T_LENGTH = 2 so one 512-byte sector is counted in the COUNT field. */
void sat_cdb(uint8_t cdb[16], int dir, uint8_t cmd, uint8_t feat, uint8_t lba_mid, uint8_t lba_hi)
{
	static const uint8_t proto[] = {3 << 1, 4 << 1, 5 << 1}, flags[] = {0, 0x0e, 0x06};
	memset(cdb, 0, 16);
	cdb[0] = 0x85;
	cdb[1] = proto[dir];
	cdb[2] = flags[dir];
	cdb[4] = feat;
	cdb[6] = dir != SAT_NONE;
	cdb[10] = lba_mid;
	cdb[12] = lba_hi;
	cdb[14] = cmd;
}

static int sat(int fd, int dir, uint8_t cmd, uint8_t feat, uint8_t lba_mid, uint8_t lba_hi, void *buf, unsigned tmo_ms)
{
	uint8_t cdb[16];
	sat_cdb(cdb, dir, cmd, feat, lba_mid, lba_hi);
	return sg_io(fd, cdb, 16, dir, buf, dir == SAT_NONE ? 0 : 512, tmo_ms);
}

/* PIO data-in, one 512-byte sector. 10 s: a USB bridge that ignores the command must not hang us. */
int sat_cmd(int fd, uint8_t cmd, uint8_t feat, uint8_t lba_mid, uint8_t lba_hi, void *buf)
{
	memset(buf, 0, 512);
	return sat(fd, SAT_IN, cmd, feat, lba_mid, lba_hi, buf, 10000);
}

/* PIO data-out of one sector, or non-data when buf is NULL. */
int sat_out(int fd, uint8_t cmd, void *buf, unsigned tmo_ms)
{
	return sat(fd, buf ? SAT_OUT : SAT_NONE, cmd, 0, 0, 0, buf, tmo_ms);
}

/* ATA Status Return sense data descriptor (SAT-4 12.2.2.2): descriptor format sense, code 0x09,
 * registers as low/high-order pairs. Without EXTEND only the low-order bytes and the device
 * register's low nibble (LBA 27:24) are meaningful. */
int sat_sense_regs(const uint8_t *s, size_t n, struct ata_regs *r)
{
	if (n < 8 || ((s[0] & 0x7f) != 0x72 && (s[0] & 0x7f) != 0x73))
		return -1;
	size_t end = 8u + s[7] < n ? 8u + s[7] : n;
	for (size_t i = 8; i + 2 <= end; i += 2 + s[i + 1]) {
		const uint8_t *d = s + i;
		if (d[0] != 0x09 || d[1] < 0x0c || i + 14 > end)
			continue;
		r->ext = d[2] & 1;
		r->err = d[3];
		r->dev = d[12];
		r->status = d[13];
		/* SAT-3 table 63 and libata's ata_tf_to_descriptor(): bytes 7, 9, 11 hold LBA 7:0,
		 * 15:8, 23:16; the high-order bytes 6, 8, 10 hold LBA 31:24, 39:32, 47:40. */
		r->lba = d[7] | d[9] << 8 | d[11] << 16;
		if (r->ext)
			r->lba |= (unsigned long long)d[6] << 24 | (unsigned long long)d[8] << 32 | (unsigned long long)d[10] << 40;
		else
			r->lba |= (unsigned long long)(d[12] & 0xf) << 24;
		return 0;
	}
	return -1;
}

/* Non-data command with CK_COND set: libata answers with the output registers in the sense data
 * (key RECOVERED ERROR, ASC/ASCQ 00h/1Dh), which sg_io() accepts. -1 when the drive or bridge
 * aborts the command or returns no registers. Only for commands that read, never change. */
int sat_regs(int fd, uint8_t cmd, int lba48, struct ata_regs *r)
{
	uint8_t cdb[16] = {[0] = 0x85, [1] = 3 << 1 | !!lba48, [2] = 0x20, [13] = 0x40, [14] = cmd}, sense[32];
	if (sg_io_sense(fd, cdb, 16, SAT_NONE, NULL, 0, 10000, sense) || sat_sense_regs(sense, sizeof sense, r))
		return -1;
	return r->status & 0xa1 ? -1 : 0; /* BSY, DF or ERR */
}

/* SCSI INQUIRY, Unit Serial Number VPD page. */
int scsi_serial(int fd, char *out, size_t n)
{
	uint8_t cdb[6] = {0x12, 1, 0x80, 0, 252, 0}, b[252] = {0};
	if (sg_io(fd, cdb, 6, SAT_IN, b, sizeof b, 10000) || b[1] != 0x80)
		return -1;
	size_t len = b[3] < sizeof b - 4 ? b[3] : sizeof b - 4, j = 0;
	for (size_t i = 0; i < len && j + 1 < n; i++)
		if (b[4 + i] > ' ' && b[4 + i] < 127)
			out[j++] = b[4 + i];
	out[j] = 0;
	return j ? 0 : -1;
}

/* 0 on success, an NVMe status (> 0) or -1 with errno. tmo_ms 0 = the kernel's admin timeout. */
int nvme_admin(int fd, uint8_t op, uint32_t nsid, uint32_t cdw10, void *buf, unsigned len, unsigned tmo_ms)
{
	struct nvme_admin_cmd c;
	memset(&c, 0, sizeof c);
	c.opcode = op;
	c.nsid = nsid;
	c.addr = (uintptr_t)buf;
	c.data_len = len;
	c.cdw10 = cdw10;
	c.timeout_ms = tmo_ms;
	return ioctl(fd, NVME_IOCTL_ADMIN_CMD, &c);
}

/* Get Log Page, first 512 bytes of log lid. */
int nvme_log(int fd, uint8_t lid, uint8_t buf[512])
{
	/* Some controllers only accept the global NSID, some only 0. */
	static const uint32_t nsid[2] = {0xffffffff, 0};
	for (int i = 0; i < 2; i++) {
		memset(buf, 0, 512);
		if (!nvme_admin(fd, 0x02, nsid[i], (127u << 16) | lid, buf, 512, 0)) /* 128 dwords */
			return 0;
	}
	return -1;
}

int nvme_smart_log(int fd, uint8_t buf[512])
{
	return nvme_log(fd, 0x02, buf); /* SMART / Health */
}

static void app(char *why, size_t n, const char *fmt, ...)
{
	size_t len = strlen(why);
	va_list ap;
	if (len >= n)
		return;
	va_start(ap, fmt);
	vsnprintf(why + len, n - len, fmt, ap);
	va_end(ap);
}

void smart_ata_parse(const uint8_t s[512], struct ata_smart *a)
{
	uint8_t sum = 0;
	for (int i = 0; i < 512; i++)
		sum += s[i];
	a->checksum_ok = sum == 0;
	a->realloc = a->pending = a->uncorrect = a->reported_uncorrect = a->crc = a->hours = a->temp = -1;
	for (int k = 0; k < 30; k++) {
		const uint8_t *e = s + 2 + 12 * k; /* id, flags[2], value, worst, raw[6], reserved */
		/* Low 32 bits only: several vendors pack unrelated data into the top raw bytes. */
		long long raw = e[5] | e[6] << 8 | e[7] << 16 | (long long)e[8] << 24;
		switch (e[0]) {
		case 5: a->realloc = raw; break;
		case 9: a->hours = raw; break;
		case 187: a->reported_uncorrect = raw; break;
		case 190: if (a->temp < 0) a->temp = e[5]; break;
		case 194: a->temp = e[5]; break;
		case 197: a->pending = raw; break;
		case 198: a->uncorrect = raw; break;
		case 199: a->crc = raw; break;
		}
	}
}

enum verdict smart_ata_verdict(const struct ata_smart *a, char *why, size_t n)
{
	why[0] = 0;
	if (!a->checksum_ok) {
		app(why, n, "SMART data checksum is wrong, values cannot be trusted");
		return V_INCONCLUSIVE;
	}
	if (a->realloc > 0) app(why, n, "%lld reallocated sectors; ", a->realloc);
	if (a->pending > 0) app(why, n, "%lld sectors pending (currently unreadable); ", a->pending);
	if (a->uncorrect > 0) app(why, n, "%lld offline-uncorrectable sectors; ", a->uncorrect);
	if (a->reported_uncorrect > 0) app(why, n, "%lld reported uncorrectable errors; ", a->reported_uncorrect);
	if (a->crc > 0) app(why, n, "%lld interface CRC errors (cable, port or backplane); ", a->crc);
	if (why[0]) {
		why[strlen(why) - 2] = 0;
		return V_FAIL;
	}
	if (a->realloc < 0 && a->pending < 0 && a->uncorrect < 0) {
		app(why, n, "drive reports none of the reallocated/pending/uncorrectable counters");
		return V_INCONCLUSIVE;
	}
	app(why, n, "zero");
	if (a->realloc == 0) app(why, n, " reallocated");
	if (a->pending == 0) app(why, n, " pending");
	if (a->uncorrect == 0) app(why, n, " uncorrectable");
	app(why, n, " sectors (the counters this drive reports)");
	return V_PASS;
}

static unsigned long long le(const uint8_t *p, int bytes)
{
	unsigned long long v = 0;
	for (int i = bytes - 1; i >= 0; i--)
		v = v << 8 | p[i];
	return v;
}

/* 128-bit counter, saturated to 64 bits. */
static unsigned long long le128(const uint8_t *p)
{
	return le(p + 8, 8) ? ~0ULL : le(p, 8);
}

void smart_nvme_parse(const uint8_t s[512], struct nvme_smart *m)
{
	m->crit = s[0];
	m->temp_k = le(s + 1, 2);
	m->spare = s[3];
	m->spare_thresh = s[4];
	m->used = s[5];
	m->hours = le128(s + 128);
	m->unsafe_shutdowns = le128(s + 144);
	m->media_errors = le128(s + 160);
}

enum verdict smart_nvme_verdict(const struct nvme_smart *m, char *why, size_t n)
{
	static const char *bits[] = {"spare capacity below threshold", "temperature out of range",
		"reliability degraded", "media is read-only", "volatile memory backup failed",
		"persistent memory region read-only"};
	why[0] = 0;
	for (int i = 0; i < 6; i++)
		if (m->crit >> i & 1)
			app(why, n, "critical warning: %s; ", bits[i]);
	if (m->media_errors)
		app(why, n, "%llu media and data integrity errors; ", m->media_errors);
	if (m->used >= 100)
		app(why, n, "rated endurance used up (%u%%); ", m->used);
	if (why[0]) {
		why[strlen(why) - 2] = 0;
		return V_FAIL;
	}
	app(why, n, "no critical warnings, no media errors, %u%% of rated endurance used", m->used);
	return V_PASS;
}

void smart_show(FILE *o, const struct disk *d)
{
	char path[64], why[512], test[64];
	uint8_t buf[512];
	snprintf(path, sizeof path, "/dev/%s", d->name);
	snprintf(test, sizeof test, "SMART %s", d->name);
	if (!d->nvme && !d->ata) {
		verdict(o, V_ABSENT, test, "no SMART access (USB bridge without ATA pass-through, or virtual disk)");
		return;
	}
	if (d->ata && !(d->id[82] & 1)) {
		verdict(o, V_ABSENT, test, "drive does not support SMART");
		return;
	}
	if (d->ata && !(d->id[85] & 1)) {
		verdict(o, V_INCONCLUSIVE, test, "SMART is switched off in the drive (Bedrock does not change drive settings)");
		return;
	}
	int fd = open(path, O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		verdict(o, V_INCONCLUSIVE, test, "cannot open %s", path);
		return;
	}
	if (d->nvme) {
		struct nvme_smart m;
		if (nvme_smart_log(fd, buf)) {
			verdict(o, V_INCONCLUSIVE, test, "controller refused the SMART log request");
		} else {
			smart_nvme_parse(buf, &m);
			fprintf(o, "  temperature %d C, spare %u%% (threshold %u%%), endurance used %u%%, power-on %llu h, unsafe shutdowns %llu\n",
				(int)m.temp_k - 273, m.spare, m.spare_thresh, m.used, m.hours, m.unsafe_shutdowns);
			enum verdict v = smart_nvme_verdict(&m, why, sizeof why);
			verdict(o, v, test, "%s", why);
		}
	} else if (sat_cmd(fd, 0xb0, 0xd0, 0x4f, 0xc2, buf)) { /* SMART READ DATA */
		verdict(o, V_INCONCLUSIVE, test, "drive refused SMART READ DATA");
	} else {
		struct ata_smart a;
		smart_ata_parse(buf, &a);
		fprintf(o, "  power-on %lld h, temperature %lld C, reallocated %lld, pending %lld, uncorrectable %lld, CRC errors %lld (-1 = not reported)\n",
			a.hours, a.temp, a.realloc, a.pending, a.uncorrect, a.crc);
		enum verdict v = smart_ata_verdict(&a, why, sizeof why);
		verdict(o, v, test, "%s", why);
	}
	close(fd);
}
