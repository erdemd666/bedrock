/* Hidden disk areas: an HPA (Host Protected Area) or DCO (Device Configuration Overlay) makes a
 * drive report less capacity than it has, and a wipe never touches the part it hides.
 * Read-only: only READ NATIVE MAX ADDRESS (EXT) and DEVICE CONFIGURATION IDENTIFY are issued,
 * never SET MAX, DCO SET/RESTORE or FREEZE LOCK. */
#include "bedrock.h"

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

/* A feature bit of IDENTIFY word 82-84 counts only when the word's bits 15:14 are 0:1 (ACS-4 7.12). */
static int feat(const uint16_t id[256], int w, int bit)
{
	return (id[w] & 0xc000) == 0x4000 && (id[w] >> bit & 1);
}

static unsigned long long words(const uint16_t *w, int n)
{
	unsigned long long v = 0;
	for (int i = n - 1; i >= 0; i--)
		v = v << 16 | w[i];
	return v;
}

/* Visible size in sectors: words 100-103 with the 48-bit feature set, else words 60-61. */
unsigned long long ata_user_sectors(const uint16_t id[256])
{
	unsigned long long n = feat(id, 83, 10) ? words(id + 100, 4) : 0;
	return n ? n : words(id + 60, 2);
}

/* Logical sector size in bytes: words 117-118 (in words) when word 106 says they are valid. */
unsigned ata_sector_bytes(const uint16_t id[256])
{
	if ((id[106] & 0xd000) == 0x5000) {
		unsigned long long b = words(id + 117, 2) * 2;
		if (b >= 512 && b <= 65536)
			return b;
	}
	return 512;
}

/* DEVICE CONFIGURATION IDENTIFY data (ACS-4 7.7): words 3-6 hold the factory maximum LBA.
 * Needs the 0xA5 signature and checksum in word 255 so a bridge's garbage is not believed. */
int dco_max_lba(const uint8_t blk[512], unsigned long long *max)
{
	uint8_t sum = 0;
	uint16_t w[4];
	for (int i = 0; i < 512; i++)
		sum += blk[i];
	if (blk[510] != 0xa5 || sum)
		return -1;
	for (int i = 0; i < 4; i++)
		w[i] = blk[6 + 2 * i] | blk[7 + 2 * i] << 8;
	*max = words(w, 4);
	return *max ? 0 : -1;
}

static unsigned long long le64(const uint8_t *p)
{
	unsigned long long v = 0;
	for (int i = 7; i >= 0; i--)
		v = v << 8 | p[i];
	return v;
}

/* Identify Controller (NVMe 1.4 5.15): bytes of capacity not assigned to any namespace.
 * 0 when the drive has no namespace management (nothing can be unassigned), -1 unknown. */
long long nvme_unallocated(const uint8_t c[4096])
{
	if (!(c[256] & 8)) /* OACS bit 3 */
		return 0;
	unsigned long long t = le64(c + 280), u = le64(c + 296); /* TNVMCAP, UNVMCAP: low halves of 128 bits */
	if (!t || u > t || t >> 62 || le64(c + 288) || le64(c + 304))
		return -1;
	return u;
}

static void gb(unsigned long long b, char *s, size_t n)
{
	snprintf(s, n, b >= 1000000000ULL ? "%.1f GB" : "%.1f MB", b >= 1000000000ULL ? b / 1e9 : b / 1e6);
}

/* The drive's visible, native (HPA removed) and factory (DCO removed) sizes, all counts of
 * logical sectors. Returns hidden bytes (0 none), -1 when the numbers cannot be trusted. */
long long hidden_calc(const struct hidden *h, char *why, size_t n)
{
	unsigned long long lim = 1ULL << 48, hpa = 0, dco = 0, base = h->cur;
	char a[32], b[32];
	if (!h->cur || h->native > lim || h->dco > lim || (h->native_q == Q_OK && h->native < h->cur)) {
		snprintf(why, n, "the drive reports sizes that do not add up, so they cannot be trusted");
		return -1;
	}
	if (h->native_q == Q_OK) {
		hpa = h->native - h->cur;
		base = h->native;
	}
	if (h->dco_q == Q_OK) {
		if (h->dco < base) {
			snprintf(why, n, "the drive reports sizes that do not add up, so they cannot be trusted");
			return -1;
		}
		dco = h->dco - base;
	}
	if (hpa || dco) {
		unsigned long long bytes = (hpa + dco) * h->ss;
		gb(hpa * h->ss, a, sizeof a);
		gb(dco * h->ss, b, sizeof b);
		snprintf(why, n, "%s%s%s%s%s hidden; a wipe does not reach it",
			hpa ? a : "", hpa ? " by an HPA" : "", hpa && dco ? " and " : "",
			dco ? b : "", dco ? " by a DCO" : "");
		return bytes;
	}
	if (h->native_q == Q_REFUSED || h->dco_q == Q_REFUSED) {
		snprintf(why, n, "the drive or its USB bridge refused the %s query", h->native_q == Q_REFUSED ? "HPA" : "DCO");
		return -1;
	}
	snprintf(why, n, "none found, the drive's native size equals its visible size");
	return 0;
}

static long long ata_hidden(const struct disk *d, int fd, char *why, size_t n)
{
	struct hidden h = {.cur = ata_user_sectors(d->id), .ss = ata_sector_bytes(d->id)};
	int lba48 = feat(d->id, 83, 10);
	struct ata_regs r;
	uint8_t blk[512];
	unsigned long long m;
	/* READ NATIVE MAX ADDRESS EXT (27h) or READ NATIVE MAX ADDRESS (F8h), ACS-4 7.37 / SAT-4 12.2.2.2 */
	if (sat_regs(fd, lba48 ? 0x27 : 0xf8, lba48, &r) || r.ext != lba48) {
		h.native_q = feat(d->id, 82, 10) ? Q_REFUSED : Q_NO; /* no HPA feature set: nothing to hide with */
	} else {
		h.native_q = Q_OK;
		h.native = r.lba + 1;
	}
	if (!feat(d->id, 83, 11)) { /* DCO feature set supported */
		h.dco_q = Q_NO;
	} else if (sat_cmd(fd, 0xb1, 0xc2, 0, 0, blk) || dco_max_lba(blk, &m)) { /* DEVICE CONFIGURATION IDENTIFY */
		h.dco_q = Q_REFUSED;
	} else {
		h.dco_q = Q_OK;
		h.dco = m + 1;
	}
	return hidden_calc(&h, why, n);
}

static long long nvme_hidden(int fd, char *why, size_t n)
{
	uint8_t c[4096];
	char s[32];
	if (nvme_admin(fd, 0x06, 0, 1, c, sizeof c, 0)) { /* Identify, CNS 1 = controller */
		snprintf(why, n, "the controller refused the Identify request");
		return -1;
	}
	long long u = nvme_unallocated(c);
	if (u < 0) {
		snprintf(why, n, "the drive does not report its unassigned capacity");
		return -1;
	}
	gb(u, s, sizeof s);
	if (u)
		snprintf(why, n, "%s is not assigned to any namespace (an overwrite cannot reach it)", s);
	else
		snprintf(why, n, "none found, no capacity is left unassigned");
	return 0;
}

/* Hidden bytes of one disk: 0 none, > 0 hidden by an HPA/DCO (a wipe does not reach them),
 * -1 unknown (why says whether the drive refused or cannot be asked). NVMe unassigned capacity
 * is not counted (the drive's own sanitize covers it); it is only described in why. */
long long hidden_area(const struct disk *d, char *why, size_t n)
{
	char path[64];
	long long r;
	if (!d->nvme && !d->ata) {
		snprintf(why, n, "this drive cannot be asked (virtual disk, or a USB bridge without ATA pass-through)");
		return -1;
	}
	snprintf(path, sizeof path, "/dev/%s", d->name);
	int fd = open(path, O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		snprintf(why, n, "cannot open %s", path);
		return -1;
	}
	r = d->nvme ? nvme_hidden(fd, why, n) : ata_hidden(d, fd, why, n);
	close(fd);
	return r;
}

void hidden_show(FILE *o, const struct disk *d, int idx)
{
	char why[384], test[64], key[48];
	long long b = hidden_area(d, why, sizeof why);
	snprintf(test, sizeof test, "Hidden area %s", d->name);
	if (b >= 0) {
		snprintf(key, sizeof key, "disk.%d.hidden_bytes", idx);
		fact(key, "%lld", b);
	}
	verdict(o, b < 0 ? V_INCONCLUSIVE : b ? V_FAIL : V_PASS, test, "%s", why);
}
