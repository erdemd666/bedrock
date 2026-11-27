#include "bedrock.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/nvme_ioctl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

enum { B = 1 << 20, NSAMPLE = 64 };

/* Is block device dev ("sda1", "/dev/nvme0n1p2") the disk itself or one of its partitions? */
int dev_on_disk(const char *dev, const char *disk)
{
	size_t n = strlen(disk);
	if (!strncmp(dev, "/dev/", 5))
		dev += 5;
	if (!n || strncmp(dev, disk, n))
		return 0;
	const char *r = dev + n;
	if (!*r)
		return 1;
	if (isdigit((unsigned char)disk[n - 1]) && *r++ != 'p') /* nvme0n1p1, mmcblk0p1 */
		return 0;
	if (!*r)
		return 0;
	for (; *r; r++)
		if (!isdigit((unsigned char)*r))
			return 0;
	return 1;
}

/* Text check: anything mounted or swapped from this disk, or root= on the
 * kernel command line pointing at it, means it is the disk we run from. */
int boot_disk_refuse(const char *disk, const char *mounts, const char *cmdline)
{
	char src[256];
	for (const char *l = mounts; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL)
		if (sscanf(l, "%255s", src) == 1 && dev_on_disk(src, disk))
			return 1;
	for (const char *c = cmdline; c && (c = strstr(c, "root=")); c += 5)
		if ((c == cmdline || c[-1] == ' ') && sscanf(c + 5, "%255s", src) == 1 && dev_on_disk(src, disk))
			return 1;
	return 0;
}

/* Device-number check: catches /dev/root, by-uuid and other names the text check cannot map. */
static int mounted_by_devno(const char *disk, const char *mounts)
{
	char path[300], buf[32], mp[256];
	dev_t devs[64];
	int nd = 0;
	snprintf(path, sizeof path, "/sys/block/%s/dev", disk);
	unsigned ma, mi;
	if (read_file(path, buf, sizeof buf) > 0 && sscanf(buf, "%u:%u", &ma, &mi) == 2)
		devs[nd++] = makedev(ma, mi);
	for (int i = 1; i < 64 && nd < 64; i++) {
		snprintf(path, sizeof path, "/sys/block/%s/%s%s%d/dev", disk, disk,
			isdigit((unsigned char)disk[strlen(disk) - 1]) ? "p" : "", i);
		if (read_file(path, buf, sizeof buf) > 0 && sscanf(buf, "%u:%u", &ma, &mi) == 2)
			devs[nd++] = makedev(ma, mi);
	}
	for (const char *l = mounts; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
		struct stat st;
		if (sscanf(l, "%*s %255s", mp) != 1 || stat(mp, &st))
			continue;
		for (int i = 0; i < nd; i++)
			if (st.st_dev == devs[i])
				return 1;
	}
	return 0;
}

static int in_use(const char *name, const char *mounts, const char *cmdline)
{
	return boot_disk_refuse(name, mounts, cmdline) || mounted_by_devno(name, mounts);
}

static uint64_t splitmix(uint64_t x)
{
	x += 0x9e3779b97f4a7c15ULL;
	x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
	x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
	return x ^ (x >> 31);
}

/* Content of the disk at byte offset off after the pass; random data is a
 * function of the offset so any sample can be verified without storing it. */
static void fill(uint8_t *buf, unsigned long long off, size_t len, int rnd, uint64_t seed)
{
	if (!rnd) {
		memset(buf, 0, len);
		return;
	}
	for (size_t i = 0; i < len; i += 8) {
		uint64_t v = splitmix(seed ^ ((off + i) / 8));
		memcpy(buf + i, &v, 8);
	}
}

static int ask(const char *prompt, char *buf, size_t n)
{
	printf("%s", prompt);
	fflush(stdout);
	if (!fgets(buf, n, stdin))
		return -1;
	buf[strcspn(buf, "\r\n")] = 0;
	return 0;
}

static int write_str(const char *path, const char *s)
{
	int fd = open(path, O_WRONLY), ok = fd >= 0 && write(fd, s, strlen(s)) == (ssize_t)strlen(s);
	if (fd >= 0)
		close(fd);
	return ok ? 0 : -1;
}

/* IDENTIFY DEVICE words 89/90 (ACS-3 7.12.7, IDENTIFY DEVICE data): erase time in minutes,
 * 0 = not given, -N = more than N. */
static int erase_min(uint16_t w)
{
	if (w & 0x8000) /* extended format: bits 14:0 in 2-minute units */
		return (w & 0x7fff) == 0x7fff ? -65532 : (w & 0x7fff) * 2;
	return (w & 0xff) == 255 ? -508 : (w & 0xff) * 2;
}

/* Words 82 and 85 bit 1: security supported / enabled; word 128: security status. */
void ata_sec_decode(const uint16_t id[256], struct ata_sec *s)
{
	s->supported = id[82] != 0xffff && id[82] >> 1 & 1;
	s->enabled = s->supported && id[85] >> 1 & 1;
	s->locked = s->supported && id[128] >> 2 & 1;
	s->frozen = s->supported && id[128] >> 3 & 1;
	s->expired = s->supported && id[128] >> 4 & 1;
	s->enhanced = s->supported && id[128] >> 5 & 1;
	s->normal_min = erase_min(id[89]);
	s->enhanced_min = erase_min(id[90]);
}

/* Data sector of SECURITY SET PASSWORD / UNLOCK / ERASE UNIT / DISABLE PASSWORD (ACS-3 Security
 * feature set): word 0 bit 0 = 0 user password, bit 1 = enhanced (ERASE UNIT only); words 1-16 password. */
void ata_sec_data(uint8_t buf[512], int enhanced)
{
	memset(buf, 0, 512);
	buf[0] = enhanced ? 2 : 0;
	memcpy(buf + 2, SEC_PW, sizeof SEC_PW - 1);
}

/* NVMe 1.4 Identify Controller: OACS bytes 257:256, SANICAP 331:328, FNA 524; Identify Namespace:
 * FLBAS 26, DPS 29. Sanitize CDW10 SANACT and Format NVM CDW10 as in their command sections. */
void nvme_erase_plan(const uint8_t ctrl[4096], const uint8_t ns[4096], uint32_t nsid, struct nvme_erase *e)
{
	unsigned oacs = ctrl[256] | ctrl[257] << 8, sanicap = ctrl[328], fna = ctrl[524], flbas = ns[26], dps = ns[29];
	memset(e, 0, sizeof *e);
	if (sanicap & 1) {
		e->sanitize = 4; /* SANACT crypto erase */
		e->name = "NVMe Sanitize, crypto erase";
	} else if (sanicap & 2) {
		e->sanitize = 2; /* SANACT block erase */
		e->name = "NVMe Sanitize, block erase";
	} else if (oacs & 2) {
		unsigned ses = fna & 4 ? 2 : 1; /* crypto erase if supported, else user data erase */
		/* Same LBA format, metadata setting and protection information as now. */
		e->format = (flbas & 0x1f) | (dps & 7) << 5 | (dps >> 3 & 1) << 8 | ses << 9 | (flbas >> 5 & 3) << 12;
		e->format_nsid = fna & 1 ? 0xffffffff : nsid;
		e->name = ses == 2 ? "NVMe Format, crypto erase" : "NVMe Format, user data erase";
	}
	e->all_ns = e->sanitize || (e->format && fna & 3);
}

/* NVMe 1.4 Sanitize Status log (81h): SPROG bytes 1:0 = done / 65536; SSTAT bits 2:0 =
 * 0 never sanitized, 1 completed, 2 in progress, 3 failed, 4 completed without deallocation. */
int nvme_sanitize_status(const uint8_t log[512], double *pct)
{
	*pct = (log[0] | log[1] << 8) * 100.0 / 65536;
	return log[2] & 7;
}

uint64_t sample_hash(const uint8_t *p, size_t n) /* FNV-1a */
{
	uint64_t h = 0xcbf29ce484222325ULL;
	while (n--)
		h = (h ^ *p++) * 0x100000001b3ULL;
	return h;
}

/* One post-erase sample (p NULL: unreadable); before = its hash before the erase, 0 = unknown.
 * A drive may return zeros, one fill byte, or (after crypto erase) noise: only data still
 * identical to before the erase proves the erase missed it. */
void erase_tally_add(struct erase_tally *t, const uint8_t *p, size_t n, uint64_t before)
{
	size_t i = 1;
	if (!p) {
		t->unread++;
		return;
	}
	while (i < n && p[i] == p[0])
		i++;
	if (i == n && !p[0])
		t->zero++;
	else if (i == n)
		t->filled++, t->fill = p[0];
	else if (!before)
		t->unread++;
	else if (sample_hash(p, n) != before)
		t->changed++;
	else
		t->unchanged++;
}

enum verdict erase_tally_verdict(const struct erase_tally *t, char *why, size_t n)
{
	int total = t->zero + t->filled + t->changed + t->unchanged + t->unread;
	char f[16] = "";
	if (t->filled)
		snprintf(f, sizeof f, " (0x%02x)", t->fill);
	snprintf(why, n, "%d samples: %d zero, %d one fill byte%s, %d other data changed by the erase, "
		"%d unchanged, %d unreadable or not comparable", total, t->zero, t->filled, f, t->changed, t->unchanged, t->unread);
	if (t->unchanged)
		return V_FAIL;
	return t->unread || !total ? V_INCONCLUSIVE : V_PASS;
}

/* Sample i: the first and last 1 MiB chunk and 62 evenly spaced between. */
static unsigned long long sample_off(unsigned long long bytes, int i, size_t *len)
{
	unsigned long long chunks = (bytes + B - 1) / B, off = (chunks - 1) * i / (NSAMPLE - 1) * B;
	*len = bytes - off < B ? bytes - off : B;
	return off;
}

static int open_read(const char *path)
{
	int fd = open(path, O_RDONLY | O_DIRECT);
	if (fd < 0)
		fd = open(path, O_RDONLY);
	if (fd >= 0)
		posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED); /* buffered fallback: read the disk, not the cache */
	return fd;
}

/* Before a firmware erase (t NULL) remember each sample's hash; after it, tally them. */
static void read_samples(const char *path, unsigned long long bytes, uint8_t *buf, uint64_t h[NSAMPLE], struct erase_tally *t)
{
	int fd = open_read(path);
	for (int i = 0; i < NSAMPLE; i++) {
		size_t len;
		unsigned long long off = sample_off(bytes, i, &len);
		int ok = fd >= 0 && pread(fd, buf, len, off) == (ssize_t)len;
		if (!t)
			h[i] = ok ? sample_hash(buf, len) : 0;
		else
			erase_tally_add(t, ok ? buf : NULL, len, h[i]);
	}
	if (fd >= 0)
		close(fd);
}

static enum verdict overwrite(int fd, const char *path, unsigned long long bytes, int rnd, uint8_t *buf, uint8_t *chk, char *why, size_t n)
{
	uint64_t seed = (uint64_t)time(NULL) * 0x100000001b3ULL ^ (uint64_t)getpid();
	unsigned long long off, bad = 0, first_bad = 0;
	time_t last = time(NULL), start = last;
	fill(buf, 0, B, 0, 0);
	for (off = 0; off < bytes && !g_stop; off += B) {
		size_t len = bytes - off < B ? bytes - off : B;
		if (rnd)
			fill(buf, off, len, 1, seed);
		if (pwrite(fd, buf, len, off) != (ssize_t)len && !bad++)
			first_bad = off;
		if (time(NULL) != last) {
			last = time(NULL);
			fprintf(stderr, "\r  writing %5.1f%%  %.0f MB/s   ", 100.0 * off / bytes,
				off / 1e6 / (last - start ? last - start : 1));
		}
	}
	fsync(fd);
	fprintf(stderr, "\n");
	if (g_stop) {
		snprintf(why, n, "stopped at %.1f%%: the disk is only partly erased", 100.0 * off / bytes);
		return V_INCONCLUSIVE;
	}
	if (bad) {
		snprintf(why, n, "%llu MiB chunks failed to write, first at byte %llu", bad, first_bad);
		return V_FAIL;
	}
	int rfd = open_read(path), mism = 0;
	if (rfd < 0) {
		snprintf(why, n, "written, but %s could not be reopened to verify", path);
		return V_INCONCLUSIVE;
	}
	for (int i = 0; i < NSAMPLE; i++) {
		size_t len;
		off = sample_off(bytes, i, &len);
		fill(buf, off, len, rnd, seed);
		if (pread(rfd, chk, len, off) != (ssize_t)len || memcmp(buf, chk, len))
			mism++;
	}
	close(rfd);
	snprintf(why, n, "%d of %d read-back samples %s", mism ? mism : NSAMPLE, NSAMPLE,
		mism ? "do not match what was written" : "match what was written");
	return mism ? V_FAIL : V_PASS;
}

/* Most BIOSes freeze ATA security at boot. Suspend-to-RAM cuts the drive's power and the
 * BIOS usually does not freeze it again on resume. */
static void unfreeze(int fd, uint16_t id[256], struct ata_sec *s)
{
	char st[128], in[16];
	printf("\nThe drive is FROZEN: the BIOS locked its security commands at boot, as most BIOSes do,\n"
	       "so it refuses a secure erase until it loses power. Suspending this machine to RAM and\n"
	       "waking it usually unfreezes it.\n");
	if (read_file("/sys/power/state", st, sizeof st) < 0 || !strstr(st, "mem")) {
		printf("This machine cannot suspend to RAM from here.\n");
	} else if (!ask("Suspend now? Wake it with the power button or a key. [y/N] ", in, sizeof in) && (in[0] == 'y' || in[0] == 'Y')) {
		if (read_file("/sys/power/mem_sleep", st, sizeof st) > 0 && strstr(st, "deep"))
			write_str("/sys/power/mem_sleep", "deep"); /* s2idle would keep the drive powered */
		sync();
		if (write_str("/sys/power/state", "mem"))
			printf("Suspend failed (%s).\n", strerror(errno));
		for (int i = 0; i < 10; i++) { /* the disk needs a moment after resume */
			if (!sat_cmd(fd, 0xec, 0, 0, 0, id)) {
				ata_sec_decode(id, s);
				break;
			}
			sleep(1);
		}
		if (!s->frozen) {
			printf("The drive is no longer frozen.\n");
			return;
		}
		printf("The drive is still frozen.\n");
	}
	printf("Another way: on a desktop with SATA hot-plug, unplug the drive's SATA power cable for a few\n"
	       "seconds while the machine runs, plug it back in, then start Wipe again.\n");
}

/* SECURITY SET PASSWORD, ERASE PREPARE, ERASE UNIT. Returns 0 erased, -1 refused before
 * anything was erased, 1 the erase itself failed. */
static int ata_erase(int fd, int enh, unsigned tmo_ms)
{
	uint8_t buf[512];
	uint16_t id[256];
	struct ata_sec s;
	int r = -1;
	ata_sec_data(buf, 0);
	if (sat_out(fd, 0xf1, buf, 10000)) {
		printf("The drive refused SECURITY SET PASSWORD.\n");
	} else if (sat_cmd(fd, 0xec, 0, 0, 0, id) || (ata_sec_decode(id, &s), !s.enabled)) {
		/* a USB bridge may accept the command without passing it on */
		printf("SECURITY SET PASSWORD did not reach the drive (a USB bridge or controller in the way).\n");
	} else {
		r = 1;
		ata_sec_data(buf, enh);
		if (sat_out(fd, 0xf3, NULL, 10000))
			printf("The drive refused SECURITY ERASE PREPARE.\n");
		else if (sat_out(fd, 0xf4, buf, tmo_ms))
			printf("SECURITY ERASE UNIT failed or timed out.\n");
		else
			r = 0;
	}
	/* A completed erase turns security off itself; otherwise remove our password so the
	 * drive is never left to lock at its next power-on. */
	if (!sat_cmd(fd, 0xec, 0, 0, 0, id) && (ata_sec_decode(id, &s), !s.enabled))
		return r;
	ata_sec_data(buf, 0);
	sat_out(fd, 0xf2, buf, 10000); /* SECURITY UNLOCK: harmless when not locked */
	if (sat_out(fd, 0xf6, buf, 10000) || sat_cmd(fd, 0xec, 0, 0, 0, id) || (ata_sec_decode(id, &s), s.enabled))
		printf("WARNING: the temporary password could not be removed; the drive will lock at its next\n"
		       "power-on. Its user password is \"%s\" (lower case): enter it when the BIOS asks, or run\n"
		       "  hdparm --user-master u --security-disable %s /dev/sdX\n", SEC_PW, SEC_PW);
	return r;
}

static void nvme_err(const char *what, int r)
{
	if (r < 0)
		printf("%s failed: %s.\n", what, strerror(errno));
	else
		printf("The controller rejected %s (NVMe status 0x%x).\n", what, r);
}

/* Sanitize with its status log, or Format NVM. Returns as ata_erase. */
static int nvme_erase(int fd, const struct nvme_erase *e)
{
	uint8_t log[512];
	double pct;
	int st = 0, r;
	if (!e->sanitize) {
		printf("Formatting. NVMe Format reports no progress; wait.\n");
		if ((r = nvme_admin(fd, 0x80, e->format_nsid, e->format, NULL, 0, 12 * 3600 * 1000)))
			nvme_err("Format NVM", r);
		return r > 0 ? -1 : r < 0;
	}
	if ((r = nvme_admin(fd, 0x84, 0, e->sanitize, NULL, 0, 0))) {
		nvme_err("Sanitize", r);
		return r > 0 ? -1 : 1;
	}
	while (!nvme_log(fd, 0x81, log) && (st = nvme_sanitize_status(log, &pct)) == 2) {
		fprintf(stderr, "\r  sanitizing %5.1f%%   ", pct);
		sleep(1);
	}
	fprintf(stderr, "\n");
	if (st == 1 || st == 4)
		return 0;
	printf(st == 3 ? "The drive reports that Sanitize failed; it accepts only another Sanitize until one succeeds.\n"
		       : "The Sanitize status log could not be read.\n");
	return 1;
}

static int on_usb(const char *name)
{
	char p[64], r[PATH_MAX];
	snprintf(p, sizeof p, "/sys/block/%s", name);
	return realpath(p, r) && strstr(r, "/usb");
}

static void min_str(int m, char *s, size_t n)
{
	if (m > 0)
		snprintf(s, n, "about %d min", m);
	else if (m < 0)
		snprintf(s, n, "more than %d min", -m);
	else
		snprintf(s, n, "not given");
}

/* NIST SP 800-88 Rev. 2 category for a method name built in wipe_run(). Rev. 2 leaves
 * technique detail to IEEE 2883-2022. Purge only where the command is defined to reach
 * all of the media, spare and remapped blocks included, or replaces its encryption key;
 * anything that may leave those blocks untouched is recorded as Clear. */
const char *nist_category(const char *method, const char **why)
{
	if (!strncmp(method, "NVMe Sanitize", 13)) {
		*why = "the drive's Sanitize command reaches all of its media, spare and remapped blocks included, "
		       "and its status log reported completion";
		return "Purge";
	}
	if (!strcmp(method, "NVMe Format, crypto erase")) {
		*why = "Format NVM with cryptographic erase replaced the key that encrypts all user data";
		return "Purge";
	}
	if (!strncmp(method, "ATA Security Erase", 18))
		*why = "the ATA Security feature set does not oblige every drive to erase spare and remapped blocks, "
		       "and its result cannot be inspected; Purge on ATA needs the SANITIZE feature set, not used here";
	else if (!strncmp(method, "NVMe Format", 11))
		*why = "Format NVM user data erase is not required to reach blocks outside the addressable space";
	else
		*why = "one pass over every user-addressable block; spare and remapped blocks are not reached";
	return "Clear";
}

/* Signed sanitization certificate (docs/CERTIFICATE.md) on the stick, beside the wipe log. */
static void wipe_cert(const struct disk *x, int ssd, const char *method, enum verdict v, const char *why, time_t t0, time_t dur)
{
	char *ex = NULL, ts[32], stem[128], path[512], msg[400];
	size_t len;
	const char *note, *cat = nist_category(method, &note);
	FILE *m;
	if (!fact_get("system.product") && (m = fopen("/dev/null", "w"))) {
		board_show(m); /* the machine's identity for the certificate */
		fclose(m);
	}
	if (!(m = open_memstream(&ex, &len)))
		return;
	fputs("{\"media\":{\"model\":", m);
	json_str(m, x->model);
	fputs(",\"serial\":", m);
	json_str(m, x->serial);
	fprintf(m, ",\"kind\":\"%s\",\"capacity_bytes\":%llu,\"interface\":\"%s\"},\"sanitization\":{\"category\":\"%s\",\"technique\":",
		ssd ? "SSD" : "HDD", x->bytes, x->nvme ? "NVMe" : on_usb(x->name) ? "USB" : x->ata ? "ATA" : "other", cat);
	json_str(m, method);
	fputs(",\"details\":", m);
	json_str(m, note);
	fputs(",\"verification\":", m);
	json_str(m, why);
	iso_time(t0, ts, sizeof ts);
	fprintf(m, ",\"result\":\"%s\",\"started\":\"%s\"", verdict_name(v), ts);
	iso_time(t0 + dur, ts, sizeof ts);
	fprintf(m, ",\"finished\":\"%s\"}}", ts);
	fclose(m);
	strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", gmtime(&t0));
	snprintf(stem, sizeof stem, "%s-%s", ts, x->serial);
	int r = signed_save("sanitization", ex, "certificates", stem, path, sizeof path);
	if (r < 0)
		printf("No writable USB partition: the certificate was not saved.\n");
	else if (r)
		printf("Certificate (NOT signed: %s): %s\n", (cert_key(msg, sizeof msg), msg), path);
	else
		printf("Signed certificate: %s\n", path);
	free(ex);
}

void wipe_run(void)
{
	struct disk d[32];
	char mounts[16384] = "", swaps[4096] = "", cmdline[4096] = "", in[128], path[64];
	char fw[96] = "", no[160] = "", method[128], why[256], est[2][32] = {"", ""};
	int n = disk_list(d, 32);
	read_file("/proc/mounts", mounts, sizeof mounts);
	read_file("/proc/swaps", swaps, sizeof swaps);
	read_file("/proc/cmdline", cmdline, sizeof cmdline);
	strncat(mounts, "\n", sizeof mounts - strlen(mounts) - 1);
	strncat(mounts, swaps, sizeof mounts - strlen(mounts) - 1);

	printf("Wipe destroys everything on a disk, in one of two ways:\n"
	       "  overwrite       writes zeros or random data over every block; right for hard disks,\n"
	       "                  but on an SSD it cannot reach spare and remapped blocks\n"
	       "  firmware erase  the drive erases itself (ATA Security Erase, NVMe Sanitize or Format);\n"
	       "                  the only trustworthy erase for an SSD\n\n");
	for (int i = 0; i < n; i++)
		printf("  %d  %-8s %-28s serial %-20s %6.1f GB%s\n", i + 1, d[i].name, d[i].model,
			d[i].serial[0] ? d[i].serial : "(none)", d[i].bytes / 1e9,
			in_use(d[i].name, mounts, cmdline) ? "  [in use: refused]" : "");
	if (!n || ask("\nDisk number to wipe (Enter to cancel): ", in, sizeof in) || !in[0])
		return;
	int k = atoi(in) - 1;
	if (k < 0 || k >= n) {
		printf("No such disk.\n");
		return;
	}
	struct disk *x = &d[k];
	if (in_use(x->name, mounts, cmdline)) {
		printf("Refused: %s is mounted or is the disk Bedrock booted from.\n", x->name);
		return;
	}
	if (!x->serial[0]) {
		printf("Refused: %s reports no serial number, so it cannot be confirmed.\n", x->name);
		return;
	}
	printf("\nALL DATA on /dev/%s (%s, %.1f GB) will be destroyed. This cannot be undone.\n",
		x->name, x->model, x->bytes / 1e9);
	if (ask("Type the disk's serial number to confirm: ", in, sizeof in) || strcmp(in, x->serial)) {
		printf("Serial does not match. Nothing was written.\n");
		return;
	}

	/* O_EXCL on a whole disk fails with EBUSY while any partition is mounted:
	 * the kernel's own last check. */
	snprintf(path, sizeof path, "/dev/%s", x->name);
	int fd = open(path, O_RDWR | O_EXCL | O_DIRECT);
	if (fd < 0 && errno == EINVAL)
		fd = open(path, O_RDWR | O_EXCL);
	if (fd < 0) {
		printf("Refused: cannot open %s exclusively (%s).\n", path, strerror(errno));
		return;
	}
	uint8_t *buf = NULL, *chk = NULL;
	if (posix_memalign((void **)&buf, 4096, B) || posix_memalign((void **)&chk, 4096, B))
		goto out;

	struct ata_sec s = {0};
	struct nvme_erase ne = {0};
	if (x->nvme) {
		int nsid = ioctl(fd, NVME_IOCTL_ID), busy = 0;
		size_t pre = strrchr(x->name, 'n') - x->name + 1; /* "nvme0n" of nvme0n1: same controller */
		for (int i = 0; i < n; i++)
			busy |= !strncmp(d[i].name, x->name, pre) && in_use(d[i].name, mounts, cmdline);
		if (nsid <= 0 || nvme_admin(fd, 0x06, 0, 1, buf, 4096, 0) || nvme_admin(fd, 0x06, nsid, 0, buf + 4096, 4096, 0))
			snprintf(no, sizeof no, "the controller refused Identify");
		else if (nvme_erase_plan(buf, buf + 4096, nsid, &ne), !ne.name)
			snprintf(no, sizeof no, "the controller supports neither Sanitize (crypto or block erase) nor Format NVM");
		else if (ne.all_ns && busy)
			snprintf(no, sizeof no, "%s erases every namespace on this controller, and one of them is in use", ne.name);
		else
			snprintf(fw, sizeof fw, "%s%s", ne.name, ne.all_ns ? " (every namespace on this controller)" : "");
	} else if (!x->ata) {
		snprintf(no, sizeof no, "%s", on_usb(x->name)
			? "the disk is on USB and the bridge does not pass ATA commands through"
			: "the disk does not answer ATA commands (SCSI, virtual or card-reader disk)");
	} else {
		ata_sec_decode(x->id, &s);
		if (s.supported) {
			min_str(s.normal_min, est[0], sizeof est[0]);
			min_str(s.enhanced_min, est[1], sizeof est[1]);
			printf("\nATA security: supported, %s, %s, %s; enhanced erase %s.\n"
			       "Drive's own erase time estimate: normal %s, enhanced %s.\n",
				s.enabled ? "ENABLED" : "not enabled", s.locked ? "LOCKED" : "not locked",
				s.frozen ? "FROZEN" : "not frozen", s.enhanced ? "supported" : "not supported", est[0], est[1]);
		}
		if (s.supported && s.frozen && !s.enabled)
			unfreeze(fd, x->id, &s);
		snprintf(no, sizeof no, "%s",
			!s.supported ? "the drive does not support the ATA security feature set" :
			s.locked ? "the drive is locked by a password set elsewhere; unlock it first" :
			s.enabled ? "the drive already has a security password set elsewhere; remove it first" :
			s.expired ? "too many wrong passwords were tried; power the drive off and on" :
			s.frozen ? "the drive is frozen" : "");
		if (!no[0])
			snprintf(fw, sizeof fw, "ATA Security Erase");
	}

	snprintf(why, sizeof why, "/sys/block/%s/queue/rotational", x->name);
	int ssd = !read_ll(why, 1);
	printf("\n%s\n", ssd ? "This is an SSD: firmware erase is recommended."
			     : "This is a hard disk (or reports no type): overwrite is recommended.");
	enum verdict v;
	time_t t0, dur;
	for (;;) {
		if (!fw[0])
			printf("Firmware erase is not available: %s.\n", no);
		printf("  1  overwrite with zeros%s\n  2  overwrite with random data\n", ssd && fw[0] ? "" : "   (recommended)");
		if (fw[0])
			printf("  3  firmware erase: %s%s%s\n", fw, x->ata ? ", normal" : "", ssd ? "   (recommended)" : "");
		if (fw[0] && s.enhanced)
			printf("  4  firmware erase: %s, enhanced\n", fw);
		if (ask("Method (Enter to cancel): ", in, sizeof in))
			in[0] = 0;
		int m = in[0] - '0';
		if (m == 1 || m == 2) {
			snprintf(method, sizeof method, "overwrite with %s", m == 2 ? "random data" : "zeros");
			g_stop = 0;
			t0 = time(NULL);
			v = overwrite(fd, path, x->bytes, m == 2, buf, chk, why, sizeof why);
			dur = time(NULL) - t0;
			break;
		}
		if (!fw[0] || m < 3 || m > 3 + s.enhanced) {
			printf("Cancelled. Nothing was written.\n");
			goto out;
		}
		int enh = m == 4, mins = enh ? s.enhanced_min : s.normal_min;
		snprintf(method, sizeof method, "%s%s", fw, x->ata ? (enh ? ", enhanced" : ", normal") : "");
		printf("\nFirmware erase (%s) wipes all of /dev/%s. Once started it cannot be stopped: Ctrl-C, rebooting\n"
		       "or closing this screen will not halt it, and cutting power midway can leave the drive\n"
		       "unusable. Drive's own time estimate: %s.\n",
			method, x->name, x->ata ? est[enh] : "not given");
		if (ask("Type ERASE to start: ", in, sizeof in) || strcmp(in, "ERASE")) {
			printf("Cancelled. Nothing was written.\n");
			goto out;
		}
		uint64_t h[NSAMPLE];
		struct erase_tally t = {0};
		read_samples(path, x->bytes, chk, h, NULL);
		t0 = time(NULL);
		printf("Erasing. Do not power off.\n");
		fflush(stdout);
		/* Timeout: twice the drive's estimate, else 10 days (kernel timers take at most ~24 days). */
		int r = x->nvme ? nvme_erase(fd, &ne) : ata_erase(fd, enh, (mins > 0 && mins < 7000 ? 2 * mins + 30 : 14400) * 60000u);
		dur = time(NULL) - t0;
		if (!r) {
			read_samples(path, x->bytes, chk, h, &t);
			v = erase_tally_verdict(&t, why, sizeof why);
			break;
		}
		if (r > 0)
			verdict(stdout, V_FAIL, "Wipe", "/dev/%s: %s failed after %ld s; the disk may be partly erased",
				x->name, method, (long)dur);
		snprintf(no, sizeof no, "%s %s", method, r < 0 ? "was refused, nothing was erased" : "failed");
		fw[0] = 0;
	}
	char detail[384], mp[256], lp[300], ts[32];
	snprintf(detail, sizeof detail, "/dev/%s (serial %s); method: %s; took %ld min %02ld s; verification: %s",
		x->name, x->serial, method, (long)dur / 60, (long)dur % 60, why);
	verdict(stdout, v, "Wipe", "%s", detail);
	/* The erase certificate leaves with the user: append it to the stick's wipe log. */
	time_t now = time(NULL);
	FILE *lf;
	if (!report_mount(mp) && snprintf(lp, sizeof lp, "%s/bedrock-wipe-log.txt", mp) < (int)sizeof lp &&
	    (lf = fopen(lp, "a"))) {
		strftime(ts, sizeof ts, "%Y-%m-%d %H:%M UTC", gmtime(&now));
		fprintf(lf, "%s  model %s\n  %-12s Wipe: %s\n", ts, x->model, verdict_name(v), detail);
		fflush(lf);
		fsync(fileno(lf)); /* the user pulls the stick right after */
		printf(fclose(lf) ? "Could not save the wipe log.\n" : "Saved to %s\n", lp);
	} else
		printf("No writable USB partition: the wipe result was not saved.\n");
	wipe_cert(x, ssd, method, v, why, t0, dur);
out:
	close(fd);
	free(buf);
	free(chk);
}
