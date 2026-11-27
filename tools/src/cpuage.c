#include "bedrock.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---- sensors (pure enough to test against fixture directories) ---- */

/* One hwmon directory: tempN_input/_label/_crit, power1_average (or _input),
 * fan1_input, freq1_input. Returns the number of temperatures found. */
int hwmon_read(const char *dir, struct hwmon *h)
{
	char p[512];
	memset(h, 0, sizeof *h);
	snprintf(p, sizeof p, "%s/name", dir);
	read_file(p, h->name, sizeof h->name);
	for (int i = 1; i < 32 && h->ntemp < HWMON_T; i++) {
		snprintf(p, sizeof p, "%s/temp%d_input", dir, i);
		long long t = read_ll(p, NO_TEMP);
		if (t == NO_TEMP)
			continue;
		snprintf(p, sizeof p, "%s/temp%d_label", dir, i);
		if (read_file(p, h->label[h->ntemp], sizeof h->label[0]) < 0)
			h->label[h->ntemp][0] = 0;
		h->temp[h->ntemp++] = t;
		snprintf(p, sizeof p, "%s/temp%d_crit", dir, i);
		long long c = h->tcrit[h->ntemp - 1] = read_ll(p, 0);
		if (c > 0 && (!h->crit || c < h->crit))
			h->crit = c;
	}
	snprintf(p, sizeof p, "%s/power1_average", dir);
	if ((h->power_uw = read_ll(p, -1)) < 0) {
		snprintf(p, sizeof p, "%s/power1_input", dir);
		h->power_uw = read_ll(p, -1);
	}
	snprintf(p, sizeof p, "%s/fan1_input", dir);
	h->fan_rpm = read_ll(p, -1);
	snprintf(p, sizeof p, "%s/freq1_input", dir);
	h->freq_hz = read_ll(p, -1);
	return h->ntemp;
}

long long hwmon_temp(const struct hwmon *h, const char *label)
{
	long long best = NO_TEMP;
	for (int i = 0; i < h->ntemp; i++)
		if ((!label || !strcmp(h->label[i], label)) && h->temp[i] > best)
			best = h->temp[i];
	return best;
}

/* RAPL zone counter in microjoules (-1 unreadable) and its wrap point. */
long long rapl_energy(const char *zone, long long *range)
{
	char p[512];
	snprintf(p, sizeof p, "%s/max_energy_range_uj", zone);
	*range = read_ll(p, 0);
	snprintf(p, sizeof p, "%s/energy_uj", zone);
	return read_ll(p, -1);
}

/* Average watts between two counter reads; the counter wraps at range. */
double rapl_watts(long long e0, long long e1, long long range, double secs)
{
	if (e0 < 0 || e1 < 0 || secs <= 0)
		return -1;
	long long d = e1 - e0;
	if (d < 0) {
		if (range <= 0)
			return -1;
		d += range + 1;
	}
	return d / 1e6 / secs;
}

/* Kernel log records seen during the run. Thermal notices also carry "mce" in
 * some kernels, so they are checked first. */
int kmsg_kind(const char *m)
{
	if (strstr(m, "temperature above threshold") || strstr(m, "clock throttled"))
		return 2;
	if (strstr(m, "Hardware Error") || strstr(m, "Machine check") || strstr(m, "machine check"))
		return 1;
	return 0;
}

/* "cpu MHz : 4800.123" per "processor : N" block of /proc/cpuinfo, into khz[N].
 * Returns the highest processor number seen + 1. */
int cpuinfo_mhz(const char *text, long long *khz, int max)
{
	int cpu = -1, n = 0;
	for (const char *l = text; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
		const char *c = strchr(l, ':');
		if (!c)
			continue;
		if (!strncmp(l, "processor", 9))
			cpu = atoi(c + 1);
		else if (!strncmp(l, "cpu MHz", 7) && cpu >= 0 && cpu < max) {
			khz[cpu] = (long long)(strtod(c + 1, NULL) * 1000);
			if (cpu + 1 > n)
				n = cpu + 1;
		}
	}
	return n;
}

/* Intel 13th/14th Gen desktop "Vmin Shift Instability": a clock-tree circuit in
 * the cores ages under high voltage and temperature. Intel's fix is microcode
 * 0x12B (Sept 2024, with 0x12F as a later addition), loaded by a BIOS update; it
 * stops further wear but does not undo it. Mobile parts and the Alder Lake die
 * versions (06_BF, e.g. i5-13400 C0, i5-14400) are not affected.
 * Intel, "Intel Core 13th and 14th Gen Desktop Instability Root Cause" (community.intel.com,
 * Blogs/Tech-Innovation/Client, post 1640606) and its 0x12F follow-up.
 * Returns -1 not one of these CPUs, 0 Raptor Lake but not affected, 1 affected. */
int raptor_check(unsigned family, unsigned model, const char *brand)
{
	static const struct { unsigned model; int affected; } t[] = {
		{0xb7, 1}, /* Raptor Lake-S/HX, Raptor Lake die (B0): i5-13600K .. i9-14900KS, 65 W non-K */
		{0xbf, 0}, /* Raptor Lake-S, Alder Lake die (C0/H0) */
		{0xba, 0}, /* Raptor Lake-P/U, mobile */
	};
	if (family != 6)
		return -1;
	for (size_t i = 0; i < sizeof t / sizeof *t; i++)
		if (t[i].model == model)
			return t[i].affected && !strstr(brand, "HX"); /* HX: same die, but mobile */
	return -1;
}

/* ---- verdict logic ---- */

/* Above this, reaching the heat limit says more about the board's power
 * settings than about the cooler (unlocked desktop chips on stock boards). */
#define HIGH_WATTS 150

enum verdict burn_thermal(const struct burn *b, char *why, size_t n)
{
	if (b->throttle > 0 || b->t_crit_s >= 0) {
		char what[96];
		if (b->t_crit_s >= 0)
			snprintf(what, sizeof what, "reached its %lld C limit after %d:%02d", b->crit / 1000,
				b->t_crit_s / 60, b->t_crit_s % 60);
		else
			snprintf(what, sizeof what, "throttled %lld times (max %lld C)", b->throttle, b->tmax / 1000);
		if (b->watts >= HIGH_WATTS) {
			snprintf(why, n, "%s at %.0f W: the board lets it draw more power than most coolers remove, "
				 "so this may be board settings rather than a worn cooler", what, b->watts);
			return V_INCONCLUSIVE;
		}
		snprintf(why, n, "%s: cooling worn: dried thermal paste, clogged heatsink or failing fan", what);
		return V_FAIL;
	}
	if (b->tmax == NO_TEMP && b->throttle < 0) {
		snprintf(why, n, "no temperature sensor or throttle counter");
		return V_ABSENT;
	}
	if (b->throttle < 0 && !b->crit) {
		snprintf(why, n, "max %lld C; no throttle counter or limit to judge against", b->tmax / 1000);
		return V_INCONCLUSIVE;
	}
	if (b->tmax == NO_TEMP)
		snprintf(why, n, "no thermal throttling (no temperature sensor)");
	else
		snprintf(why, n, "max %lld C, no thermal throttling", b->tmax / 1000);
	return V_PASS;
}

/* A cooler that works reaches a steady temperature within minutes; one that is
 * still climbing at the end of a long run is not keeping up. */
enum verdict burn_heatup(const struct burn *b, char *why, size_t n)
{
	if (b->tmax == NO_TEMP) {
		snprintf(why, n, "no temperature sensor");
		return V_ABSENT;
	}
	if (b->secs < 300) {
		snprintf(why, n, "rose %.0f C in the first minute; run 10 or 30 minutes to see whether it settles",
			 b->rise_first_min);
		return V_INCONCLUSIVE;
	}
	if (b->slope_end >= 2.0) {
		snprintf(why, n, "still rising %.1f C per minute at the end: cooler not keeping up: "
			 "failing fan or pump, or clogged fins", b->slope_end);
		return V_FAIL;
	}
	snprintf(why, n, "rose %.0f C in the first minute, then settled (max %lld C)", b->rise_first_min, b->tmax / 1000);
	return V_PASS;
}

enum verdict burn_clock(const struct burn *b, char *why, size_t n)
{
	int pct = (int)(b->clock_ratio * 100 + 0.5);
	if (b->clock_ratio <= 0) {
		snprintf(why, n, "rated base clock or live clock not reported");
		return V_INCONCLUSIVE;
	}
	if (b->clock_ratio < 0.9 && (b->throttle > 0 || b->t_crit_s >= 0)) {
		snprintf(why, n, "%d%% of rated base clock, held back by heat (see CPU thermal)", pct);
		return V_INCONCLUSIVE;
	}
	if (b->clock_ratio < 0.75) {
		snprintf(why, n, "held at %d%% of its rated base clock with no heat to explain it: power limit set "
			 "too low in the BIOS, or worn voltage regulators on the board", pct);
		return V_FAIL;
	}
	if (b->clock_ratio < 0.9) {
		snprintf(why, n, "%d%% of rated base clock under load: check power limits in the BIOS", pct);
		return V_INCONCLUSIVE;
	}
	snprintf(why, n, "%d%% of rated base clock under load", pct);
	return V_PASS;
}

static int cmp_ll(const void *a, const void *b)
{
	long long x = *(const long long *)a, y = *(const long long *)b;
	return (x > y) - (x < y);
}

/* Index of the core furthest below the median of the cores that share its
 * group (same rated maximum: P-cores and E-cores are compared separately),
 * if below 80% of it; -1 if none. *ratio = that core / its group median. */
int core_outlier(const long long *khz, const long long *group, int n, double *ratio)
{
	int worst = -1;
	double wr = 1;
	long long *v = malloc(n * sizeof *v);
	if (!v)
		return -1;
	for (int i = 0; i < n; i++) {
		int m = 0;
		if (khz[i] <= 0)
			continue;
		for (int j = 0; j < n; j++)
			if (group[j] == group[i] && khz[j] > 0)
				v[m++] = khz[j];
		if (m < 3)
			continue;
		qsort(v, m, sizeof *v, cmp_ll);
		double r = (double)khz[i] / v[m / 2];
		if (r < 0.8 && r < wr) {
			wr = r;
			worst = i;
		}
	}
	free(v);
	*ratio = wr;
	return worst;
}

/* ---- live ---- */

#define MAXCPU 1024
#define STEP 2 /* seconds between samples */

static long long cpu_khz(int cpu, const char *leaf)
{
	char p[128];
	snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/%s", cpu, leaf);
	return read_ll(p, 0);
}

/* Current clock of every CPU in kHz; cpufreq if present, else /proc/cpuinfo
 * (which the kernel fills from APERF/MPERF on CPUs that have them). */
static int clocks(long long *khz, int ncpu)
{
	int got = 0;
	for (int i = 0; i < ncpu; i++)
		got += (khz[i] = cpu_khz(i, "scaling_cur_freq")) > 0;
	if (got)
		return got;
	static char buf[1 << 18];
	if (read_file("/proc/cpuinfo", buf, sizeof buf) > 0)
		got = cpuinfo_mhz(buf, khz, ncpu);
	return got;
}

/* Rated base clock in kHz: intel_pstate's base_frequency, else the brand string's "@ 3.60GHz". */
static long long base_khz(int cpu, const char *brand)
{
	long long b = cpu_khz(cpu, "base_frequency");
	const char *at = strstr(brand, "@ ");
	if (!b && at && strstr(at, "GHz"))
		b = (long long)(strtod(at + 2, NULL) * 1e6);
	return b;
}

/* Count new hardware-error and thermal records in the kernel log. */
static void kmsg_scan(int fd, int *hw, int *therm)
{
	char rec[8192]; /* a record must fit whole or read() fails */
	ssize_t r;
	while (fd >= 0 && ((r = read(fd, rec, sizeof rec - 1)) > 0 || (r < 0 && errno == EPIPE))) {
		if (r <= 0)
			continue; /* EPIPE: records were overwritten; keep reading */
		rec[r] = 0;
		char *msg = strchr(rec, ';');
		int k = kmsg_kind(msg ? msg + 1 : rec);
		*hw += k == 1;
		*therm += k == 2;
	}
}

static long long throttle_count(void)
{
	long long core = sum_cpus("thermal_throttle/core_throttle_count");
	long long pkg = read_ll("/sys/devices/system/cpu/cpu0/thermal_throttle/package_throttle_count", 0);
	return core < 0 ? -1 : core + pkg;
}

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static void raptor_verdict(FILE *o, const struct cpu_info *ci, unsigned long errors)
{
	int r = raptor_check(ci->family, ci->model, ci->brand);
	char buf[32];
	if (r < 0)
		return;
	if (!r) {
		verdict(o, V_PASS, "CPU aging (Intel 13th/14th Gen)", "this model is not affected by Intel's 2024 instability issue");
		return;
	}
	if (read_file("/sys/devices/system/cpu/cpu0/microcode/version", buf, sizeof buf) <= 0) {
		verdict(o, V_INCONCLUSIVE, "CPU aging (Intel 13th/14th Gen)",
			"affected model; microcode revision not readable, so the BIOS fix cannot be confirmed");
		return;
	}
	unsigned long rev = strtoul(buf, NULL, 0);
	fact("cpu.microcode", "0x%lx", rev);
	if (rev < RAPTOR_FIXED_UCODE)
		verdict(o, V_FAIL, "CPU aging (Intel 13th/14th Gen)",
			"microcode 0x%lx is older than Intel's fix 0x12B: a BIOS update is needed. This model wears out on "
			"older microcode, so %s", rev, errors ? "the errors found here may be permanent" :
			"instability found later may be permanent damage");
	else if (errors)
		verdict(o, V_FAIL, "CPU aging (Intel 13th/14th Gen)",
			"microcode 0x%lx has Intel's fix, but the errors above suggest wear from before the update: "
			"it does not heal", rev);
	else
		verdict(o, V_PASS, "CPU aging (Intel 13th/14th Gen)",
			"microcode 0x%lx has Intel's fix (0x12B or later) and no errors were found", rev);
}

void cpu_burnin(FILE *o, int secs)
{
	struct cpu_info ci;
	struct load l;
	struct burn b = {.secs = secs, .t_crit_s = -1, .watts = -1};
	char why[384];
	int ncpu = sysconf(_SC_NPROCESSORS_CONF), nsamp = secs / STEP + 2, ns = 0;
	if (ncpu < 1 || ncpu > MAXCPU)
		ncpu = ncpu < 1 ? 1 : MAXCPU;
	cpu_get(&ci);
	long long *sum = calloc(ncpu, sizeof *sum), *cnt = calloc(ncpu, sizeof *cnt);
	long long *khz = calloc(ncpu, sizeof *khz), *grp = calloc(ncpu, sizeof *grp);
	long long *temps = calloc(nsamp, sizeof *temps);
	if (!sum || !cnt || !khz || !grp || !temps) {
		verdict(o, V_INCONCLUSIVE, "CPU burn-in", "out of memory");
		goto out;
	}

	b.t0 = b.tmax = cpu_temp(&b.crit);
	long long thr0 = throttle_count(), range, e0 = rapl_energy("/sys/class/powercap/intel-rapl:0", &range), e1;
	int kfd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK), hw = 0, therm = 0;
	if (kfd >= 0)
		lseek(kfd, 0, SEEK_END);
	double t0 = now(), joules = 0, tprev = t0;

	if (!load_start(&l)) {
		free(l.w);
		if (kfd >= 0)
			close(kfd);
		verdict(o, V_INCONCLUSIVE, "CPU burn-in", "could not start the test threads");
		goto out;
	}
	for (int s = 0; s < secs && !g_stop; s += STEP) {
		sleep(STEP);
		double t = now();
		long long temp = cpu_temp(&b.crit);
		if ((e1 = rapl_energy("/sys/class/powercap/intel-rapl:0", &range)) >= 0 && e0 >= 0) {
			double w = rapl_watts(e0, e1, range, t - tprev);
			if (w >= 0)
				joules += w * (t - tprev);
		}
		e0 = e1;
		tprev = t;
		if (ns < nsamp)
			temps[ns++] = temp;
		if (temp > b.tmax)
			b.tmax = temp;
		if (temp != NO_TEMP && b.crit && temp >= b.crit && b.t_crit_s < 0)
			b.t_crit_s = (int)(t - t0);
		clocks(khz, ncpu);
		long long all = 0, nall = 0;
		for (int i = 0; i < ncpu; i++)
			if (khz[i] > 0) {
				sum[i] += khz[i];
				cnt[i]++;
				all += khz[i];
				nall++;
			}
		kmsg_scan(kfd, &hw, &therm);
		int el = (int)(t - t0);
		fprintf(stderr, "\r  CPU burn-in %d:%02d / %d:%02d  %d threads", el / 60, el % 60, secs / 60, secs % 60, l.started);
		if (temp != NO_TEMP)
			fprintf(stderr, "  %lld C", temp / 1000);
		if (nall)
			fprintf(stderr, "  %lld MHz", all / nall / 1000);
		if (joules > 0)
			fprintf(stderr, "  %.0f W", joules / (t - t0));
		fprintf(stderr, "   ");
	}
	double elapsed = now() - t0;
	int worst, ncpus;
	unsigned long iters, bad = load_stop(&l, o, &iters, &worst, &ncpus);
	fprintf(stderr, "\n");
	if (kfd >= 0) {
		kmsg_scan(kfd, &hw, &therm);
		close(kfd);
	}
	b.secs = (int)elapsed;

	/* Summaries for the verdicts. */
	long long thr1 = throttle_count();
	b.throttle = thr0 >= 0 && thr1 >= 0 ? thr1 - thr0 : therm ? therm : -1; /* log lines only without counters */
	if (joules > 0)
		b.watts = joules / elapsed;
	int s60 = 60 / STEP - 1, third = ns * 2 / 3;
	if (ns && b.t0 != NO_TEMP && temps[ns - 1] != NO_TEMP) {
		b.rise_first_min = (temps[s60 < ns ? s60 : ns - 1] - b.t0) / 1000.0;
		if (ns - third >= 2)
			b.slope_end = (temps[ns - 1] - temps[third]) / 1000.0 / ((ns - 1 - third) * STEP / 60.0);
	}
	double ratio = 0;
	long long mhz = 0;
	int rated = 0, measured = 0;
	for (int i = 0; i < ncpu; i++) {
		if (!cnt[i])
			continue;
		khz[i] = sum[i] / cnt[i];
		grp[i] = cpu_khz(i, "cpuinfo_max_freq");
		mhz += khz[i] / 1000;
		measured++;
		long long base = base_khz(i, ci.brand);
		if (base > 0) {
			ratio += (double)khz[i] / base;
			rated++;
		}
	}
	b.clock_ratio = rated ? ratio / rated : 0;
	if (measured)
		mhz /= measured;

	fprintf(o, "CPU burn-in: %d threads, %d:%02d, paths: integer, fpu%s%s\n", l.started,
		b.secs / 60, b.secs % 60, l.sse2 ? ", sse2" : "", l.avx ? ", avx" : "");
	if (b.tmax != NO_TEMP)
		fprintf(o, "  temperature %lld -> %lld C%s", b.t0 / 1000, b.tmax / 1000, b.crit ? "" : "\n");
	if (b.tmax != NO_TEMP && b.crit)
		fprintf(o, " (limit %lld C)\n", b.crit / 1000);
	if (measured)
		fprintf(o, "  average clock under load %lld MHz\n", mhz);
	if (b.watts >= 0)
		fprintf(o, "  package power %.0f W average\n", b.watts);

	if (bad)
		verdict(o, V_FAIL, "CPU burn-in compute", "%lu wrong results on %d CPU(s)%s: unstable: degraded silicon "
			"or bad power delivery", bad, ncpus, ncpus == 1 ? ", all on one core" : "");
	else if (!iters)
		verdict(o, V_INCONCLUSIVE, "CPU burn-in compute", "no round completed");
	else
		verdict(o, V_PASS, "CPU burn-in compute", "%lu rounds, every result matched the known answer", iters);
	verdict(o, burn_thermal(&b, why, sizeof why), "CPU thermal", "%s", why);
	verdict(o, burn_heatup(&b, why, sizeof why), "CPU heat-up", "%s", why);
	verdict(o, burn_clock(&b, why, sizeof why), "CPU clock", "%s", why);
	double r;
	int slow = core_outlier(khz, grp, ncpu, &r);
	if (measured < 2)
		verdict(o, V_INCONCLUSIVE, "CPU core spread", "per-core clocks not reported");
	else if (slow >= 0)
		verdict(o, V_FAIL, "CPU core spread", "cpu%d ran at %lld MHz, %d%% of the cores like it: that core is held "
			"back by a local hot spot or is failing", slow, khz[slow] / 1000, (int)(r * 100));
	else if (worst >= 0 && ncpus == 1)
		verdict(o, V_FAIL, "CPU core spread", "all wrong results came from cpu%d: that core is failing", worst);
	else
		verdict(o, V_PASS, "CPU core spread", "no core lagged behind the others");
	if (kfd < 0)
		verdict(o, V_INCONCLUSIVE, "Machine check", "system error log not readable");
	else if (hw)
		verdict(o, V_FAIL, "Machine check", "%d hardware error reports during the run (corrected or not): "
			"the processor or its memory path is failing", hw);
	else
		verdict(o, V_PASS, "Machine check", "no hardware errors reported during the run");
	raptor_verdict(o, &ci, bad);

	if (b.tmax != NO_TEMP)
		fact("cpu.max_temp_c", "%lld", b.tmax / 1000);
	if (measured)
		fact("cpu.avg_mhz_load", "%lld", mhz);
	if (b.throttle >= 0)
		fact("cpu.throttle_events", "%lld", b.throttle);
	if (b.watts >= 0)
		fact("cpu.package_w", "%.0f", b.watts);
	fact("cpu.errors", "%lu", bad + hw);
out:
	free(sum);
	free(cnt);
	free(khz);
	free(grp);
	free(temps);
}
