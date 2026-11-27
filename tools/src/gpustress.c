#include "bedrock.h"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define GPU_TOOL "/usr/bin/bedrock-gpu"

/* ---- bedrock-gpu output: one "tag key=value ..." line per result ---- */

void gpu_run_init(struct gpu_run *r)
{
	memset(r, 0, sizeof *r);
	r->software = -1;
	r->rounds = r->stress_errors = r->secs = r->vram_mib = r->vram_errors = -1;
}

static long long kv(const char *line, const char *key)
{
	char k[32];
	snprintf(k, sizeof k, " %s=", key);
	const char *p = strstr(line, k);
	return p ? strtoll(p + strlen(k), NULL, 10) : -1;
}

/* Returns 1 for a line it understood, 0 otherwise. */
int gpu_line(const char *line, struct gpu_run *r)
{
	size_t len = strcspn(line, "\r\n");
	if (!strncmp(line, "renderer ", 9)) {
		snprintf(r->renderer, sizeof r->renderer, "%.*s", (int)(len - 9), line + 9);
	} else if (!strncmp(line, "software ", 9)) {
		r->software = atoi(line + 9) != 0;
	} else if (!strncmp(line, "stress ", 7)) {
		r->rounds = kv(line, "rounds");
		r->stress_errors = kv(line, "errors");
		r->secs = kv(line, "secs");
	} else if (!strncmp(line, "vram ", 5)) {
		r->vram_mib = kv(line, "mib");
		r->vram_errors = kv(line, "errors");
	} else if (!strncmp(line, "fail ", 5)) {
		snprintf(r->fail, sizeof r->fail, "%.*s", (int)(len - 5), line + 5);
	} else {
		return 0;
	}
	return 1;
}

/* Hot spot far above edge means the paste no longer spreads heat off the die
 * evenly; the margins are typical of healthy cards under full load. */
enum verdict gpu_heat_verdict(const struct gpu_heat *h, char *why, size_t n)
{
	long long max = h->edge > h->junction ? h->edge : h->junction;
	if (h->mem > max)
		max = h->mem;
	if (max == NO_TEMP) {
		snprintf(why, n, "the card reports no temperature");
		return V_ABSENT;
	}
	if (h->limit) {
		snprintf(why, n, "reached its %lld C limit and throttled: worn thermal paste or pads, dust in the "
			 "cooler or a failing fan", h->limit / 1000);
		return V_FAIL;
	}
	if (h->junction != NO_TEMP && h->edge != NO_TEMP && h->junction - h->edge > 30000) {
		snprintf(why, n, "hot spot %lld C, %lld C above the edge sensor: worn thermal paste or pads, or the "
			 "cooler no longer sits flat", h->junction / 1000, (h->junction - h->edge) / 1000);
		return V_FAIL;
	}
	if (h->mem >= 100000) {
		snprintf(why, n, "video memory at %lld C: worn thermal pads on the memory chips", h->mem / 1000);
		return V_FAIL;
	}
	if (h->fan_rpm == 0 && max >= 80000) {
		snprintf(why, n, "fan stood still at %lld C: failing fan or fan cable", max / 1000);
		return V_FAIL;
	}
	int l = snprintf(why, n, "max %lld C", max / 1000);
	if (h->junction != NO_TEMP && h->edge != NO_TEMP && l > 0 && (size_t)l < n)
		l += snprintf(why + l, n - l, ", hot spot %lld C above edge", (h->junction - h->edge) / 1000);
	if (h->fan_rpm > 0 && l > 0 && (size_t)l < n)
		snprintf(why + l, n - l, ", fan %lld rpm", h->fan_rpm);
	return V_PASS;
}

/* ---- live ---- */

static void heat_sample(const char *addr, struct gpu_heat *h)
{
	char p[PATH_MAX];
	snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/hwmon", addr);
	DIR *d = opendir(p);
	struct dirent *e;
	while (d && (e = readdir(d))) {
		struct hwmon m;
		if (strncmp(e->d_name, "hwmon", 5))
			continue;
		snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/hwmon/%s", addr, e->d_name);
		if (hwmon_read(p, &m) <= 0)
			continue;
		long long edge = hwmon_temp(&m, "edge"), junc = hwmon_temp(&m, "junction"), mem = hwmon_temp(&m, "mem");
		if (edge == NO_TEMP && junc == NO_TEMP && mem == NO_TEMP)
			edge = hwmon_temp(&m, NULL); /* unlabelled: nouveau, i915 */
		long long max = edge > junc ? edge : junc, prev = h->edge > h->junction ? h->edge : h->junction;
		if (max > prev)
			h->fan_rpm = m.fan_rpm;
		if (edge > h->edge) h->edge = edge;
		if (junc > h->junction) h->junction = junc;
		if (mem > h->mem) h->mem = mem;
		if (m.power_uw > h->power_uw) h->power_uw = m.power_uw;
		for (int i = 0; i < m.ntemp; i++)
			if (m.tcrit[i] > 0 && m.temp[i] >= m.tcrit[i])
				h->limit = m.tcrit[i];
	}
	if (d)
		closedir(d);
}

/* Run bedrock-gpu, sampling the card's sensors (addr NULL: none) while it works. */
static int run_tool(const char *dev, int secs, long long vram_mib, const char *addr, struct gpu_run *r, struct gpu_heat *h)
{
	int fd[2];
	char s[16], v[24], line[256];
	size_t len = 0;
	gpu_run_init(r);
	if (pipe(fd))
		return -1;
	snprintf(s, sizeof s, "%d", secs);
	snprintf(v, sizeof v, "%lld", vram_mib);
	pid_t pid = fork();
	if (pid < 0) {
		close(fd[0]);
		close(fd[1]);
		return -1;
	}
	if (!pid) {
		dup2(fd[1], 1);
		close(fd[0]);
		close(fd[1]);
		execl(GPU_TOOL, GPU_TOOL, "--secs", s, "--vram-mib", v, dev, (char *)NULL);
		_exit(127);
	}
	close(fd[1]);
	time_t start = time(NULL), last = 0;
	struct pollfd p = {.fd = fd[0], .events = POLLIN};
	for (;;) {
		int rc = poll(&p, 1, 1000);
		if (rc > 0) {
			ssize_t got = read(fd[0], line + len, sizeof line - 1 - len);
			if (got <= 0)
				break;
			len += got;
			line[len] = 0;
			char *nl;
			while ((nl = strchr(line, '\n'))) {
				*nl = 0;
				gpu_line(line, r);
				memmove(line, nl + 1, len - (nl + 1 - line) + 1);
				len -= nl + 1 - line;
			}
			if (len == sizeof line - 1)
				len = 0; /* overlong line: drop it */
		}
		time_t t = time(NULL);
		if (addr && t - last >= 2) {
			heat_sample(addr, h);
			last = t;
		}
		fprintf(stderr, "\r  GPU test %ld s", (long)(t - start));
		if (h->edge != NO_TEMP || h->junction != NO_TEMP)
			fprintf(stderr, "  %lld C", (h->junction > h->edge ? h->junction : h->edge) / 1000);
		fprintf(stderr, "   ");
		if (g_stop)
			kill(pid, SIGINT);
	}
	close(fd[0]);
	int st;
	waitpid(pid, &st, 0);
	fprintf(stderr, "\n");
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static const char *module_for(unsigned ven)
{
	return ven == 0x8086 ? "i915" : ven == 0x1002 ? "amdgpu radeon" : ven == 0x10de ? "nouveau" : NULL;
}

/* /dev/dri/renderD* of a PCI device, loading its driver first if needed. */
static int render_node(const char *addr, unsigned ven, char *dev, size_t n)
{
	char p[PATH_MAX], cmd[64];
	const char *mods = module_for(ven);
	for (int tries = 0; tries < 20; tries++) {
		snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/drm", addr);
		DIR *d = opendir(p);
		struct dirent *e;
		while (d && (e = readdir(d)))
			if (!strncmp(e->d_name, "renderD", 7)) {
				snprintf(dev, n, "/dev/dri/%s", e->d_name);
				closedir(d);
				return 0;
			}
		if (d)
			closedir(d);
		snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/driver", addr);
		if (!tries && mods && access(p, F_OK) && !access("/lib/modules", F_OK)) {
			fprintf(stderr, "  loading the graphics driver...\n");
			for (const char *m = mods; *m;) { /* AMD: amdgpu first, radeon for older cards */
				size_t l = strcspn(m, " ");
				snprintf(cmd, sizeof cmd, "modprobe -q %.*s 2>/dev/null", (int)l, m);
				if (system(cmd)) {}
				m += l + (m[l] == ' ');
				if (!access(p, F_OK))
					break;
			}
		}
		if (access(p, F_OK))
			return -1; /* no driver bound: no node will appear */
		usleep(250000);
	}
	return -1;
}

/* How much video memory to test: amdgpu reports it; large BARs (resizable BAR,
 * Intel Arc) map all of it; 0 for integrated Intel graphics, which use system RAM. */
static long long vram_cap(const char *addr, unsigned ven)
{
	char p[PATH_MAX];
	snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/mem_info_vram_total", addr);
	long long v = read_ll(p, 0);
	if (v > 0)
		return v / 10 * 9 >> 20; /* the driver and display need some */
	if (ven == 0x8086 && !strncmp(addr, "0000:00:", 8))
		return 0;
	unsigned long long bar = 0, s, en, fl;
	snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/resource", addr);
	FILE *r = fopen(p, "r");
	while (r && fscanf(r, "%llx %llx %llx", &s, &en, &fl) == 3)
		if (fl & 0x200 && en > s && en - s + 1 > bar)
			bar = en - s + 1;
	if (r)
		fclose(r);
	/* shortcut: a small BAR says nothing about the size; test 256 MiB then */
	return bar >= 1ULL << 30 ? (long long)(bar >> 20) / 10 * 9 : 256;
}

/* Run the tool on the software renderer once, so a card without a driver still
 * shows that the test itself works (and QEMU exercises it). */
static void software_check(char *note, size_t n)
{
	static int done;
	static char saved[256];
	struct gpu_run r;
	struct gpu_heat h = {NO_TEMP, NO_TEMP, NO_TEMP, 0, -1, -1};
	if (!done++) {
		int rc = run_tool("software", 3, 16, NULL, &r, &h);
		if (!rc && r.rounds > 0 && r.software == 1)
			snprintf(saved, sizeof saved, "; the software renderer %s ran the test (%lld rounds, %lld errors), "
				 "which says nothing about the card", r.renderer, r.rounds, r.stress_errors + (r.vram_errors > 0 ? r.vram_errors : 0));
		else
			snprintf(saved, sizeof saved, "; the software renderer could not run the test either%s%s",
				 r.fail[0] ? ": " : "", r.fail);
	}
	snprintf(note, n, "%s", saved);
}

static void one_gpu(FILE *o, int idx, const char *addr, unsigned ven, int secs)
{
	char dev[64], test[32], key[48], note[256];
	struct gpu_run r;
	struct gpu_heat h = {NO_TEMP, NO_TEMP, NO_TEMP, 0, -1, -1};
	gpu_run_init(&r);
	snprintf(test, sizeof test, "GPU %d stress", idx);
	if (access(GPU_TOOL, X_OK)) {
		verdict(o, V_INCONCLUSIVE, test, "the GPU test is only in the 64-bit system; start Bedrock on a 64-bit PC");
		return;
	}
	int nodev = render_node(addr, ven, dev, sizeof dev);
	if (nodev && ven != 0x10de) {
		software_check(note, sizeof note);
		verdict(o, V_INCONCLUSIVE, test, "no hardware driver for this card%s", note);
		return;
	}
	long long cap = vram_cap(addr, ven);
	int rc = nodev ? -1 : run_tool(dev, secs, cap, addr, &r, &h);
	if (ven == 0x10de && (nodev || r.software == 1 || r.rounds <= 0)) {
		/* nouveau runs only older cards; newer ones need firmware it cannot use */
		verdict(o, V_INCONCLUSIVE, test, "open drivers cannot run this NVIDIA card%s%s; only NVIDIA's own "
			"driver can test it", r.fail[0] && !nodev ? ": " : "", nodev ? "" : r.fail);
		pcie_link(o, addr);
		return;
	}
	snprintf(key, sizeof key, "gpu.%d.renderer", idx);
	if (r.renderer[0])
		fact(key, "%s", r.renderer);
	fprintf(o, "  renderer %s, device %s\n", r.renderer[0] ? r.renderer : "unknown", dev);
	if (r.software == 1) {
		verdict(o, V_INCONCLUSIVE, test, "no hardware driver for this card (only the software renderer %s)", r.renderer);
		return;
	}
	if (r.rounds <= 0) {
		verdict(o, V_INCONCLUSIVE, test, "the test could not run on this card%s%s (exit %d)",
			r.fail[0] ? ": " : "", r.fail, rc);
		return;
	}
	char why[384];
	long long errs = r.stress_errors + (r.vram_errors > 0 ? r.vram_errors : 0);
	if (r.stress_errors)
		verdict(o, V_FAIL, test, "%lld wrong results in %lld rounds: unstable graphics core (worn, overheating "
			"or overclocked)", r.stress_errors, r.rounds);
	else
		verdict(o, V_PASS, test, "%lld rounds in %lld s, every result matched the known answer", r.rounds, r.secs);
	snprintf(test, sizeof test, "GPU %d video memory", idx);
	if (!cap)
		verdict(o, V_ABSENT, test, "integrated graphics use system memory; the memory test covers it");
	else if (r.vram_mib <= 0)
		verdict(o, V_INCONCLUSIVE, test, "could not reserve video memory to test%s%s", r.fail[0] ? ": " : "", r.fail);
	else if (r.vram_errors)
		verdict(o, V_FAIL, test, "%lld errors in %lld MiB: failing video memory", r.vram_errors, r.vram_mib);
	else
		verdict(o, V_PASS, test, "%lld MiB written and read back with three patterns, no errors", r.vram_mib);
	snprintf(test, sizeof test, "GPU %d temperature", idx);
	verdict(o, gpu_heat_verdict(&h, why, sizeof why), test, "%s", why);
	if (h.power_uw > 0)
		fprintf(o, "  peak board power %lld W\n", h.power_uw / 1000000);

	long long max = h.edge > h.junction ? h.edge : h.junction;
	if (h.mem > max)
		max = h.mem;
	snprintf(key, sizeof key, "gpu.%d.max_temp_c", idx);
	if (max != NO_TEMP)
		fact(key, "%lld", max / 1000);
	snprintf(key, sizeof key, "gpu.%d.vram_tested_mb", idx);
	fact(key, "%lld", r.vram_mib > 0 ? r.vram_mib : 0);
	snprintf(key, sizeof key, "gpu.%d.errors", idx);
	fact(key, "%lld", errs);
}

static int pci_sel(const struct dirent *e) { return e->d_name[0] != '.'; }

void gpu_stress(FILE *o, int secs)
{
	struct dirent **ls;
	char p[PATH_MAX], buf[32];
	int n = scandir("/sys/bus/pci/devices", &ls, pci_sel, alphasort), gpus = 0;
	for (int i = 0; i < n; i++) {
		snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/class", ls[i]->d_name);
		unsigned cls = read_file(p, buf, sizeof buf) > 0 ? strtoul(buf, NULL, 16) : 0;
		if (cls >> 16 == 0x03) { /* same numbering as gpu_show() */
			snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/vendor", ls[i]->d_name);
			unsigned ven = read_file(p, buf, sizeof buf) > 0 ? strtoul(buf, NULL, 16) : 0;
			snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/device", ls[i]->d_name);
			unsigned dev = read_file(p, buf, sizeof buf) > 0 ? strtoul(buf, NULL, 16) : 0;
			fprintf(o, "GPU %d: %04x:%04x at %s\n", gpus, ven, dev, ls[i]->d_name);
			one_gpu(o, gpus++, ls[i]->d_name, ven, secs);
		}
		free(ls[i]);
	}
	if (n > 0)
		free(ls);
	if (!gpus)
		verdict(o, V_ABSENT, "GPU stress", "no display controller on the PCI bus");
}
