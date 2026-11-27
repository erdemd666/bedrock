#include "bedrock.h"

#include <cpuid.h>
#include <dirent.h>
#include <immintrin.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <sys/klog.h>
#include <time.h>
#include <unistd.h>

static const struct cpuid_leaf *find(const struct cpuid_leaf *l, int n, uint32_t leaf, uint32_t sub)
{
	for (int i = 0; i < n; i++)
		if (l[i].leaf == leaf && l[i].sub == sub)
			return &l[i];
	return NULL;
}

int cpu_decode(const struct cpuid_leaf *l, int n, struct cpu_info *ci)
{
	const struct cpuid_leaf *x = find(l, n, 0, 0);
	memset(ci, 0, sizeof *ci);
	if (!x)
		return -1;
	uint32_t max = x->r[0];
	memcpy(ci->vendor, &x->r[1], 4);
	memcpy(ci->vendor + 4, &x->r[3], 4);
	memcpy(ci->vendor + 8, &x->r[2], 4);

	if (max >= 1 && (x = find(l, n, 1, 0))) {
		uint32_t a = x->r[0], fam = (a >> 8) & 0xf;
		ci->stepping = a & 0xf;
		ci->model = (a >> 4) & 0xf;
		ci->family = fam;
		if (fam == 0xf)
			ci->family += (a >> 20) & 0xff;
		if (fam == 0x6 || fam == 0xf)
			ci->model |= ((a >> 16) & 0xf) << 4;
		ci->f1c = x->r[2];
		ci->f1d = x->r[3];
	}
	if (max >= 7 && (x = find(l, n, 7, 0)))
		ci->f7b = x->r[1];

	/* CPUs without extended leaves (e.g. Pentium III) echo the highest basic leaf. */
	uint32_t ext = (x = find(l, n, 0x80000000, 0)) ? x->r[0] : 0;
	if (ext < 0x80000000 || ext > 0x8000ffff)
		ext = 0;
	if (ext >= 0x80000001 && (x = find(l, n, 0x80000001, 0))) {
		ci->f81c = x->r[2];
		ci->f81d = x->r[3];
	}
	if (ext >= 0x80000004) {
		for (uint32_t i = 0; i < 3; i++)
			if ((x = find(l, n, 0x80000002 + i, 0)))
				memcpy(ci->brand + 16 * i, x->r, 16);
		char *s = ci->brand;
		while (*s == ' ')
			s++;
		memmove(ci->brand, s, strlen(s) + 1);
		for (size_t k = strlen(ci->brand); k && ci->brand[k - 1] == ' ';)
			ci->brand[--k] = 0;
	}

	if (!strcmp(ci->vendor, "GenuineIntel") && max >= 4) {
		for (uint32_t sub = 0; (x = find(l, n, 4, sub)); sub++) {
			uint32_t type = x->r[0] & 0x1f, level = (x->r[0] >> 5) & 7;
			if (!type)
				break;
			unsigned kib = (((x->r[1] >> 22) + 1) * (((x->r[1] >> 12) & 0x3ff) + 1) *
					((x->r[1] & 0xfff) + 1) * (x->r[2] + 1)) / 1024;
			if (level == 1 && type == 1) ci->l1d = kib;
			if (level == 1 && type == 2) ci->l1i = kib;
			if (level == 2) ci->l2 = kib;
			if (level == 3) ci->l3 = kib;
		}
	} else {
		if (ext >= 0x80000005 && (x = find(l, n, 0x80000005, 0))) {
			ci->l1d = x->r[2] >> 24;
			ci->l1i = x->r[3] >> 24;
		}
		if (ext >= 0x80000006 && (x = find(l, n, 0x80000006, 0))) {
			ci->l2 = x->r[2] >> 16;
			ci->l3 = (x->r[3] >> 18) * 512;
		}
	}
	return 0;
}

void cpu_flags(const struct cpu_info *ci, char *buf, size_t n)
{
	static const struct { const char *name; int reg, bit; } f[] = {
		{"fpu", 1, 0}, {"tsc", 1, 4}, {"pae", 1, 6}, {"cx8", 1, 8}, {"cmov", 1, 15},
		{"mmx", 1, 23}, {"sse", 1, 25}, {"sse2", 1, 26}, {"ht", 1, 28}, {"sse3", 0, 0},
		{"vmx", 0, 5}, {"ssse3", 0, 9}, {"sse4_1", 0, 19}, {"sse4_2", 0, 20},
		{"popcnt", 0, 23}, {"aes", 0, 25}, {"avx", 0, 28}, {"hypervisor", 0, 31},
		{"avx2", 2, 5}, {"avx512f", 2, 16}, {"svm", 3, 2}, {"nx", 4, 20}, {"lm", 4, 29},
	};
	const uint32_t r[] = {ci->f1c, ci->f1d, ci->f7b, ci->f81c, ci->f81d};
	size_t len = 0;
	buf[0] = 0;
	for (size_t i = 0; i < sizeof f / sizeof f[0]; i++)
		if (r[f[i].reg] >> f[i].bit & 1 && len < n)
			len += snprintf(buf + len, n - len, "%s%s", len ? " " : "", f[i].name);
}

/* ---- workloads with known answers ---- */

unsigned work_sieve(void)
{
	enum { N = 1000000 };
	char *c = calloc(N, 1);
	unsigned count = 0;
	if (!c)
		return 0;
	for (unsigned i = 2; i < N; i++) {
		if (c[i])
			continue;
		count++;
		for (unsigned j = i * 2; j < N; j += i)
			c[j] = 1;
	}
	free(c);
	return count;
}

/* Small-integer matrices: every product and sum is exact in double, so any
 * deviation from MATRIX_SUM is a hardware fault, not rounding. */
enum { MN = 64 };
static void mat_init(double a[MN][MN], double b[MN][MN], double c[MN][MN])
{
	for (int i = 0; i < MN; i++)
		for (int j = 0; j < MN; j++) {
			a[i][j] = (i * 3 + j) % 11 - 5;
			b[i][j] = (i + j * 7) % 13 - 6;
			c[i][j] = 0;
		}
}

static double mat_sum(double c[MN][MN])
{
	double s = 0;
	for (int i = 0; i < MN; i++)
		for (int j = 0; j < MN; j++)
			s += c[i][j] * ((i * MN + j) % 17 + 1);
	return s;
}

double work_matrix(void)
{
	double a[MN][MN], b[MN][MN], c[MN][MN];
	mat_init(a, b, c);
	for (int i = 0; i < MN; i++)
		for (int k = 0; k < MN; k++)
			for (int j = 0; j < MN; j++)
				c[i][j] += a[i][k] * b[k][j];
	return mat_sum(c);
}

__attribute__((target("sse2"))) double work_matrix_sse2(void)
{
	double a[MN][MN], b[MN][MN], c[MN][MN];
	mat_init(a, b, c);
	for (int i = 0; i < MN; i++)
		for (int k = 0; k < MN; k++) {
			__m128d x = _mm_set1_pd(a[i][k]);
			for (int j = 0; j < MN; j += 2)
				_mm_storeu_pd(&c[i][j], _mm_add_pd(_mm_loadu_pd(&c[i][j]),
						_mm_mul_pd(x, _mm_loadu_pd(&b[k][j]))));
		}
	return mat_sum(c);
}

__attribute__((target("avx"))) double work_matrix_avx(void)
{
	double a[MN][MN], b[MN][MN], c[MN][MN];
	mat_init(a, b, c);
	for (int i = 0; i < MN; i++)
		for (int k = 0; k < MN; k++) {
			__m256d x = _mm256_set1_pd(a[i][k]);
			for (int j = 0; j < MN; j += 4)
				_mm256_storeu_pd(&c[i][j], _mm256_add_pd(_mm256_loadu_pd(&c[i][j]),
						_mm256_mul_pd(x, _mm256_loadu_pd(&b[k][j]))));
		}
	return mat_sum(c);
}

/* ---- live ---- */

static int cpuid_collect(struct cpuid_leaf *l, int max)
{
	int n = 0;
#define ADD(lf, sb) do { if (n < max) { l[n].leaf = (lf); l[n].sub = (sb); \
	__cpuid_count((lf), (sb), l[n].r[0], l[n].r[1], l[n].r[2], l[n].r[3]); n++; } } while (0)
	ADD(0, 0);
	uint32_t top = l[0].r[0];
	if (top >= 1) ADD(1, 0);
	for (uint32_t s = 0; top >= 4 && s < 16; s++) {
		ADD(4, s);
		if (!(l[n - 1].r[0] & 0x1f))
			break;
	}
	if (top >= 7) ADD(7, 0);
	ADD(0x80000000, 0);
	uint32_t ext = l[n - 1].r[0];
	for (uint32_t x = 0x80000001; ext >= 0x80000001 && ext <= 0x8000ffff && x <= ext && x <= 0x80000006; x++)
		ADD(x, 0);
#undef ADD
	return n;
}

static void cpu_get(struct cpu_info *ci)
{
	struct cpuid_leaf l[32];
	cpu_decode(l, cpuid_collect(l, 32), ci);
}

static int avx_usable(const struct cpu_info *ci)
{
	/* AVX needs the OS to save YMM state: OSXSAVE and XCR0 bits 1-2. */
	if (!(ci->f1c >> 28 & 1) || !(ci->f1c >> 27 & 1))
		return 0;
	uint32_t lo, hi;
	__asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
	return (lo & 6) == 6;
}

void cpu_show(FILE *o)
{
	struct cpu_info ci;
	char flags[256];
	cpu_get(&ci);
	cpu_flags(&ci, flags, sizeof flags);
	fprintf(o, "CPU\n  %s\n  %s  family %u model %u (0x%x) stepping %u\n",
		ci.brand[0] ? ci.brand : "(no brand string)", ci.vendor, ci.family, ci.model, ci.model, ci.stepping);
	fprintf(o, "  logical CPUs online: %ld\n", sysconf(_SC_NPROCESSORS_ONLN));
	fprintf(o, "  cache: L1d %u KiB, L1i %u KiB, L2 %u KiB, L3 %u KiB (0 = not reported)\n",
		ci.l1d, ci.l1i, ci.l2, ci.l3);
	fprintf(o, "  flags: %s\n", flags);
	fact("cpu.brand", "%s", ci.brand[0] ? ci.brand : ci.vendor);
	fact("cpu.id", "%s family %u model 0x%x stepping %u", ci.vendor, ci.family, ci.model, ci.stepping);
	fact("cpu.threads", "%ld", sysconf(_SC_NPROCESSORS_ONLN));
}

static int is_cpu_hwmon(const char *name)
{
	return !strcmp(name, "coretemp") || !strcmp(name, "k10temp") || !strcmp(name, "zenpower") ||
	       !strcmp(name, "k8temp") || !strcmp(name, "via_cputemp");
}

/* Highest CPU temperature in milli-degC, or -1000000 if no sensor. */
long long cpu_temp(long long *crit)
{
	long long best = -1000000, c = 0;
	char path[300], name[64];
	DIR *d = opendir("/sys/class/hwmon");
	struct dirent *e;
	while (d && (e = readdir(d))) {
		snprintf(path, sizeof path, "/sys/class/hwmon/%s/name", e->d_name);
		if (read_file(path, name, sizeof name) < 0 || !is_cpu_hwmon(name))
			continue;
		for (int i = 1; i < 64; i++) {
			snprintf(path, sizeof path, "/sys/class/hwmon/%s/temp%d_input", e->d_name, i);
			long long t = read_ll(path, -1000000);
			if (t > best)
				best = t;
			snprintf(path, sizeof path, "/sys/class/hwmon/%s/temp%d_crit", e->d_name, i);
			long long tc = read_ll(path, 0);
			if (tc && (!c || tc < c))
				c = tc;
		}
	}
	if (d)
		closedir(d);
	*crit = c;
	return best;
}

static long long sum_cpus(const char *leaf)
{
	long long sum = -1;
	char path[200];
	for (int i = 0; i < 4096; i++) {
		snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d", i);
		if (access(path, F_OK))
			break;
		snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/%s", i, leaf);
		long long v = read_ll(path, -1);
		if (v >= 0)
			sum = (sum < 0 ? 0 : sum) + v;
	}
	return sum;
}

/* Kernel log lines that report machine-check errors, or -1 if the log is unreadable. */
static int mce_count(void)
{
	int size = klogctl(10, NULL, 0), n = 0;
	char *buf = size > 0 ? malloc(size + 1) : NULL;
	if (!buf)
		return -1;
	int len = klogctl(3, buf, size);
	if (len < 0) {
		free(buf);
		return -1;
	}
	buf[len] = 0;
	for (char *p = buf; (p = strstr(p, "Hardware Error")); p++)
		n++;
	for (char *p = buf; (p = strstr(p, "Machine check events logged")); p++)
		n++;
	free(buf);
	return n;
}

struct worker {
	pthread_t t;
	int cpu, sse2, avx;
	unsigned long iters, bad_sieve, bad_fpu, bad_sse2, bad_avx;
};
static volatile int stress_run;

static void *worker(void *arg)
{
	struct worker *w = arg;
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(w->cpu, &set);
	sched_setaffinity(0, sizeof set, &set);
	while (stress_run && !g_stop) {
		w->bad_sieve += work_sieve() != SIEVE_PRIMES;
		w->bad_fpu += work_matrix() != MATRIX_SUM;
		if (w->sse2)
			w->bad_sse2 += work_matrix_sse2() != MATRIX_SUM;
		if (w->avx)
			w->bad_avx += work_matrix_avx() != MATRIX_SUM;
		w->iters++;
	}
	return NULL;
}

void cpu_stress(FILE *o, int secs)
{
	struct cpu_info ci;
	cpu_set_t set;
	cpu_get(&ci);
	int sse2 = ci.f1d >> 26 & 1, avx = avx_usable(&ci), n = 0, started = 0;
	if (sched_getaffinity(0, sizeof set, &set))
		CPU_ZERO(&set);
	int total = CPU_COUNT(&set) ? CPU_COUNT(&set) : 1;
	struct worker *w = calloc(total, sizeof *w);
	if (!w) {
		verdict(o, V_INCONCLUSIVE, "CPU stress", "out of memory");
		return;
	}
	long long crit, t0 = cpu_temp(&crit), tmax = t0;
	long long thr0 = sum_cpus("thermal_throttle/core_throttle_count");
	long long fmin = 0, fmax = 0;
	int mce0 = mce_count();

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 1 << 20); /* three 32 KiB matrices on the stack */
	stress_run = 1;
	for (int c = 0; c < CPU_SETSIZE && n < total; c++) {
		if (CPU_COUNT(&set) && !CPU_ISSET(c, &set))
			continue;
		w[n].cpu = c;
		w[n].sse2 = sse2;
		w[n].avx = avx;
		if (!pthread_create(&w[n].t, &attr, worker, &w[n]))
			started++;
		else
			w[n].cpu = -1;
		n++;
	}
	pthread_attr_destroy(&attr);

	for (int s = 0; s < secs && !g_stop; s++) {
		sleep(1);
		long long t = cpu_temp(&crit), f = sum_cpus("cpufreq/scaling_cur_freq");
		if (t > tmax)
			tmax = t;
		f = f > 0 ? f / started : 0;
		if (f && (!fmin || f < fmin)) fmin = f;
		if (f > fmax) fmax = f;
		fprintf(stderr, "\r  CPU stress %d/%d s  %d threads", s + 1, secs, started);
		if (t > -1000000)
			fprintf(stderr, "  %lld C", t / 1000);
		if (f)
			fprintf(stderr, "  %lld MHz", f / 1000);
		fprintf(stderr, "   ");
	}
	stress_run = 0;
	unsigned long iters = 0, bad = 0;
	for (int i = 0; i < n; i++) {
		if (w[i].cpu < 0)
			continue;
		pthread_join(w[i].t, NULL);
		iters += w[i].iters;
		unsigned long b = w[i].bad_sieve + w[i].bad_fpu + w[i].bad_sse2 + w[i].bad_avx;
		if (b)
			fprintf(o, "  cpu%d: %lu wrong results (sieve %lu, x87/fpu %lu, sse2 %lu, avx %lu) in %lu rounds\n",
				w[i].cpu, b, w[i].bad_sieve, w[i].bad_fpu, w[i].bad_sse2, w[i].bad_avx, w[i].iters);
		bad += b;
	}
	fprintf(stderr, "\n");
	free(w);

	fprintf(o, "CPU stress: %d threads, %d s, paths: integer, fpu%s%s\n",
		started, secs, sse2 ? ", sse2" : "", avx ? ", avx" : "");
	if (fmax)
		fprintf(o, "  clock under load: %lld-%lld MHz\n", fmin / 1000, fmax / 1000);
	if (bad)
		verdict(o, V_FAIL, "CPU compute", "%lu wrong results in %lu rounds", bad, iters);
	else if (!iters)
		verdict(o, V_INCONCLUSIVE, "CPU compute", "no round completed");
	else
		verdict(o, V_PASS, "CPU compute", "%lu rounds, every result matched the known answer", iters);

	long long thr1 = sum_cpus("thermal_throttle/core_throttle_count");
	if (thr0 >= 0 && thr1 > thr0)
		verdict(o, V_FAIL, "CPU thermal", "throttled %lld times under load (max %lld C): check cooler and paste",
			thr1 - thr0, tmax / 1000);
	else if (tmax > -1000000 && crit && tmax >= crit)
		verdict(o, V_FAIL, "CPU thermal", "reached %lld C, critical limit %lld C", tmax / 1000, crit / 1000);
	else if (thr0 >= 0)
		verdict(o, V_PASS, "CPU thermal", "no thermal throttling%s", tmax > -1000000 ? "" : " (no temperature sensor)");
	else if (tmax > -1000000 && crit)
		verdict(o, V_PASS, "CPU thermal", "max %lld C, below critical %lld C", tmax / 1000, crit / 1000);
	else if (tmax > -1000000)
		verdict(o, V_INCONCLUSIVE, "CPU thermal", "max %lld C, no throttle counter or critical limit to judge against", tmax / 1000);
	else
		verdict(o, V_ABSENT, "CPU thermal", "no temperature sensor or throttle counter");

	int mce1 = mce_count();
	if (access("/sys/devices/system/machinecheck", F_OK))
		verdict(o, V_ABSENT, "Machine check", "no machine-check support on this CPU");
	else if (mce0 < 0 || mce1 < 0)
		verdict(o, V_INCONCLUSIVE, "Machine check", "system error log not readable");
	else if (mce1 > mce0)
		verdict(o, V_FAIL, "Machine check", "%d new hardware error reports in the system error log", mce1 - mce0);
	else
		verdict(o, V_PASS, "Machine check", "no hardware errors reported during the run");
}
