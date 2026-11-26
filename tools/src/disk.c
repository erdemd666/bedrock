#include "bedrock.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int skip(const char *n)
{
	static const char *p[] = {"loop", "ram", "zram", "dm-", "md", "sr", "fd", "nbd", "mtdblock"};
	for (size_t i = 0; i < sizeof p / sizeof p[0]; i++)
		if (!strncmp(n, p[i], strlen(p[i])))
			return 1;
	return n[0] == '.';
}

/* ATA strings: two characters per word, high byte first, space padded. */
static void ata_str(const uint16_t *w, int first, int words, char *out, size_t n)
{
	size_t j = 0;
	for (int i = first; i < first + words && j + 2 < n; i++) {
		out[j++] = w[i] >> 8;
		out[j++] = w[i] & 0xff;
	}
	out[j] = 0;
	while (j && out[j - 1] == ' ')
		out[--j] = 0;
	char *s = out;
	while (*s == ' ')
		s++;
	memmove(out, s, strlen(s) + 1);
}

static int identify_ok(const uint16_t *w)
{
	if (w[0] & 0x8000 || !w[27]) /* not an ATA device, or no model string */
		return 0;
	if ((w[255] & 0xff) != 0xa5)
		return 1;
	uint8_t sum = 0;
	for (int i = 0; i < 256; i++)
		sum += (w[i] & 0xff) + (w[i] >> 8);
	return sum == 0;
}

static int by_name(const void *a, const void *b)
{
	return strcmp(((const struct disk *)a)->name, ((const struct disk *)b)->name);
}

int disk_list(struct disk *d, int max)
{
	DIR *dir = opendir("/sys/block");
	struct dirent *e;
	char p[300];
	int n = 0;
	while (dir && n < max && (e = readdir(dir))) {
		if (skip(e->d_name) || strlen(e->d_name) >= sizeof d->name)
			continue;
		snprintf(p, sizeof p, "/sys/block/%s/device", e->d_name);
		if (access(p, F_OK))
			continue;
		struct disk *x = &d[n++];
		memset(x, 0, sizeof *x);
		strcpy(x->name, e->d_name);
		snprintf(p, sizeof p, "/sys/block/%s/size", x->name);
		x->bytes = read_ll(p, 0) * 512ULL; /* sysfs size is always in 512-byte units */
		snprintf(p, sizeof p, "/sys/block/%s/device/model", x->name);
		read_file(p, x->model, sizeof x->model);
		snprintf(p, sizeof p, "/sys/block/%s/device/serial", x->name);
		if (read_file(p, x->serial, sizeof x->serial) <= 0) {
			snprintf(p, sizeof p, "/sys/block/%s/serial", x->name); /* virtio */
			read_file(p, x->serial, sizeof x->serial);
		}
		snprintf(p, sizeof p, "/sys/block/%s/device/firmware_rev", x->name);
		if (read_file(p, x->fw, sizeof x->fw) <= 0) {
			snprintf(p, sizeof p, "/sys/block/%s/device/rev", x->name);
			read_file(p, x->fw, sizeof x->fw);
		}
		x->nvme = !strncmp(x->name, "nvme", 4);
		if (x->nvme)
			continue;
		snprintf(p, sizeof p, "/dev/%s", x->name);
		int fd = open(p, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;
		if (!sat_cmd(fd, 0xec, 0, 0, 0, x->id) && identify_ok(x->id)) { /* IDENTIFY DEVICE */
			x->ata = 1;
			ata_str(x->id, 10, 10, x->serial, sizeof x->serial);
			ata_str(x->id, 23, 4, x->fw, sizeof x->fw);
			ata_str(x->id, 27, 20, x->model, sizeof x->model);
		} else if (!x->serial[0]) {
			scsi_serial(fd, x->serial, sizeof x->serial);
		}
		close(fd);
	}
	if (dir)
		closedir(dir);
	qsort(d, n, sizeof *d, by_name);
	return n;
}

void hist_add(struct hist *h, uint32_t us)
{
	static const uint32_t lim[5] = {5000, 20000, 50000, 150000, 500000};
	int i = 0;
	if (us == HIST_ERR)
		i = 6;
	else
		while (i < 5 && us >= lim[i])
			i++;
	h->b[i]++;
}

enum verdict hist_verdict(const struct hist *h, char *why, size_t n)
{
	unsigned long long total = 0;
	for (int i = 0; i < 7; i++)
		total += h->b[i];
	if (h->b[6]) {
		snprintf(why, n, "%llu unreadable blocks", h->b[6]);
		return V_FAIL;
	}
	if (h->b[5]) {
		snprintf(why, n, "%llu blocks took 500 ms or more (the drive is retrying weak sectors)", h->b[5]);
		return V_FAIL;
	}
	if (!total) {
		snprintf(why, n, "nothing was read");
		return V_INCONCLUSIVE;
	}
	snprintf(why, n, "%llu blocks read, none unreadable or over 500 ms (%llu took 150-500 ms)", total, h->b[4]);
	return V_PASS;
}

static void size_str(unsigned long long b, char *s, size_t n)
{
	snprintf(s, n, b >= 1000000000000ULL ? "%.2f TB" : "%.1f GB", b / (b >= 1000000000000ULL ? 1e12 : 1e9));
}

void disk_show(FILE *o)
{
	struct disk d[32];
	char sz[32];
	int n = disk_list(d, 32);
	fprintf(o, "Disks\n");
	if (!n)
		verdict(o, V_ABSENT, "Disks", "no disks found");
	for (int i = 0; i < n; i++) {
		size_str(d[i].bytes, sz, sizeof sz);
		fprintf(o, "  %s  %s  serial %s  firmware %s  %s\n", d[i].name,
			d[i].model[0] ? d[i].model : "(no model)", d[i].serial[0] ? d[i].serial : "(none)",
			d[i].fw[0] ? d[i].fw : "?", sz);
		/* name|model|serial|bytes|kind, split by inventory and certificates */
		char key[32];
		snprintf(key, sizeof key, "disk.%d", i);
		fact(key, "%s|%s|%s|%llu|%s", d[i].name, d[i].model, d[i].serial, d[i].bytes,
			d[i].nvme ? "NVMe" : d[i].ata ? (d[i].id[217] == 1 ? "ATA SSD" : "ATA") : "other");
		smart_show(o, &d[i]);
	}
}

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

void disk_scan(FILE *o, const struct disk *d, int sampled)
{
	enum { B = 128 << 10 }; /* 256 sectors: the block size the classic latency bands assume */
	char path[64], test[64], why[256];
	struct hist h = {{0}};
	void *buf = NULL;
	snprintf(path, sizeof path, "/dev/%s", d->name);
	snprintf(test, sizeof test, "Surface %s", d->name);
	int direct = 1, fd = open(path, O_RDONLY | O_DIRECT);
	if (fd < 0 && errno == EINVAL) {
		direct = 0;
		fd = open(path, O_RDONLY);
	}
	if (fd < 0 || posix_memalign(&buf, 4096, B)) {
		verdict(o, V_INCONCLUSIVE, test, "cannot open %s", path);
		if (fd >= 0)
			close(fd);
		return;
	}
	if (!direct)
		posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
	unsigned long long nblk = (d->bytes + B - 1) / B, count = sampled && nblk > 256 ? 256 : nblk, k;
	unsigned shown = 0;
	double last = now();
	for (k = 0; k < count && !g_stop; k++) {
		unsigned long long blk = count == nblk ? k : (nblk - 1) * k / (count - 1);
		unsigned long long off = blk * B;
		size_t len = d->bytes - off < B ? d->bytes - off : B;
		double t0 = now();
		ssize_t r = pread(fd, buf, len, off);
		double us = (now() - t0) * 1e6;
		if (r != (ssize_t)len) {
			hist_add(&h, HIST_ERR);
			if (shown++ < 10)
				fprintf(o, "  read error at sector %llu\n", off / 512);
		} else {
			hist_add(&h, us < HIST_ERR - 1 ? (uint32_t)us : HIST_ERR - 1);
		}
		if (now() - last >= 1) {
			last = now();
			fprintf(stderr, "\r  %s %5.1f%%  slow(>=500ms) %llu  errors %llu   ", d->name,
				100.0 * k / count, h.b[5], h.b[6]);
		}
	}
	fprintf(stderr, "\r                                                        \r");
	close(fd);
	free(buf);
	fprintf(o, "Surface scan %s: %s, %llu of %llu blocks of 128 KiB%s\n", d->name,
		count == nblk ? "full" : "sampled across the disk", k, nblk, direct ? "" : " (buffered: O_DIRECT unsupported)");
	fprintf(o, "  <5 ms %llu | <20 %llu | <50 %llu | <150 %llu | <500 %llu | >=500 %llu | error %llu\n",
		h.b[0], h.b[1], h.b[2], h.b[3], h.b[4], h.b[5], h.b[6]);
	enum verdict v = hist_verdict(&h, why, sizeof why);
	if (v != V_FAIL && k < count)
		verdict(o, V_INCONCLUSIVE, test, "stopped at %.1f%%", 100.0 * k / count);
	else
		verdict(o, v, test, "%s", why);
}
