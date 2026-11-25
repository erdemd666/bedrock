#include "bedrock.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define WBITS (sizeof(unsigned long) * 8)
#define CHECKER ((unsigned long)0x5555555555555555ULL)

static FILE *mo;
static unsigned long errs, shown;

/* Physical address behind a virtual one, 0 if the kernel will not say. */
static unsigned long long phys(volatile void *v)
{
	static int fd = -2;
	long ps = sysconf(_SC_PAGESIZE);
	uint64_t e;
	if (fd == -2)
		fd = open("/proc/self/pagemap", O_RDONLY);
	if (fd < 0 || pread(fd, &e, 8, (off_t)((uintptr_t)v / ps) * 8) != 8 || !(e >> 63))
		return 0;
	e &= (1ULL << 55) - 1;
	return e ? e * ps + (uintptr_t)v % ps : 0;
}

static void check(volatile unsigned long *p, size_t i, unsigned long want)
{
	unsigned long got = p[i];
	if (got == want)
		return;
	errs++;
	if (mo && shown++ < 10)
		fprintf(mo, "    error at phys 0x%llx (virt %p): expected %0*lx got %0*lx\n",
			phys(&p[i]), (void *)&p[i], (int)WBITS / 4, want, (int)WBITS / 4, got);
}

static uint64_t rnd(uint64_t *x)
{
	*x ^= *x << 13;
	*x ^= *x >> 7;
	*x ^= *x << 17;
	return *x;
}

static void step(const char *name, unsigned long before)
{
	if (mo)
		fprintf(mo, "  %-22s %s\n", name, errs > before ? "ERRORS" : g_stop ? "stopped" : "ok");
}

unsigned long mem_patterns(volatile unsigned long *p, size_t n, FILE *o)
{
	size_t i;
	unsigned long b;
	mo = o;
	errs = shown = 0;

	b = errs;
	for (int inv = 0; inv < 2 && !g_stop; inv++) {
		if (mo)
			fprintf(stderr, "  stuck address (%d/2)\r", inv + 1);
		for (i = 0; i < n; i++)
			p[i] = inv ? ~(unsigned long)&p[i] : (unsigned long)&p[i];
		for (i = 0; i < n; i++)
			check(p, i, inv ? ~(unsigned long)&p[i] : (unsigned long)&p[i]);
	}
	step("stuck address", b);

	b = errs;
	for (int inv = 0; inv < 2 && !g_stop; inv++) {
		if (mo)
			fprintf(stderr, "  walking %s          \r", inv ? "0s" : "1s");
		for (i = 0; i < n; i++)
			p[i] = inv ? ~(1UL << i % WBITS) : 1UL << i % WBITS;
		for (i = 0; i < n; i++)
			check(p, i, inv ? ~(1UL << i % WBITS) : 1UL << i % WBITS);
	}
	step("walking 1s / 0s", b);

	b = errs;
	for (int inv = 0; inv < 2 && !g_stop; inv++) {
		if (mo)
			fprintf(stderr, "  checkerboard (%d/2)   \r", inv + 1);
		for (i = 0; i < n; i++)
			p[i] = ((i & 1) ^ inv) ? ~CHECKER : CHECKER;
		for (i = 0; i < n; i++)
			check(p, i, ((i & 1) ^ inv) ? ~CHECKER : CHECKER);
	}
	step("checkerboard", b);

	b = errs;
	if (!g_stop) {
		if (mo)
			fprintf(stderr, "  random (fixed seed)  \r");
		uint64_t x = 0x9e3779b97f4a7c15ULL;
		for (i = 0; i < n; i++)
			p[i] = (unsigned long)rnd(&x);
		x = 0x9e3779b97f4a7c15ULL;
		for (i = 0; i < n; i++)
			check(p, i, (unsigned long)rnd(&x));
	}
	step("random (fixed seed)", b);

	/* MATS+ march: up(w0); up(r0,w1); down(r1,w0) -- catches address decoder
	 * faults that a single-direction pass misses. */
	b = errs;
	const unsigned long pats[2] = {0, CHECKER};
	for (int k = 0; k < 2 && !g_stop; k++) {
		unsigned long pat = pats[k];
		if (mo)
			fprintf(stderr, "  moving inversions (%d/2)\r", k + 1);
		for (i = 0; i < n; i++)
			p[i] = pat;
		for (i = 0; i < n; i++) {
			check(p, i, pat);
			p[i] = ~pat;
		}
		for (i = n; i-- > 0;) {
			check(p, i, ~pat);
			p[i] = pat;
		}
	}
	step("moving inversions", b);
	if (mo)
		fprintf(stderr, "                          \r");
	return errs;
}

static long long meminfo(const char *key)
{
	char buf[4096], *s;
	if (read_file("/proc/meminfo", buf, sizeof buf) < 0 || !(s = strstr(buf, key)))
		return -1;
	return atoll(s + strlen(key)) * 1024;
}

void mem_test(FILE *o, size_t cap)
{
	long long avail = meminfo("MemAvailable:"), total = meminfo("MemTotal:");
	if (avail < 0)
		avail = meminfo("MemFree:");
	if (avail <= 0) {
		verdict(o, V_INCONCLUSIVE, "Memory", "cannot read /proc/meminfo");
		return;
	}
	/* Leave room for the kernel and us, or the OOM killer ends the test. */
	long long reserve = avail / 16 > (32 << 20) ? avail / 16 : 32 << 20;
	unsigned long long want = avail > reserve ? (unsigned long long)(avail - reserve) : 0;
	if (cap && want > cap)
		want = cap;
	if (want > (size_t)-1 / 2)
		want = (size_t)-1 / 2; /* 32-bit address space */
	size_t size = (size_t)want & ~(size_t)0xfffff;
	void *p = MAP_FAILED;
	while (size >= (1 << 20)) {
		p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (p != MAP_FAILED && !mlock(p, size))
			break;
		if (p != MAP_FAILED)
			munmap(p, size);
		p = MAP_FAILED;
		size = (size / 8 * 7) & ~(size_t)0xfffff;
	}
	if (p == MAP_FAILED) {
		verdict(o, V_INCONCLUSIVE, "Memory", "could not lock any memory to test");
		return;
	}
	fprintf(o, "Memory test: %zu MiB locked of %lld MiB total\n", size >> 20, total >> 20);
	unsigned long e = mem_patterns(p, size / sizeof(unsigned long), o);
	munlock(p, size);
	munmap(p, size);
	if (e)
		verdict(o, V_FAIL, "Memory", "%lu bit errors in %zu MiB", e, size >> 20);
	else if (g_stop)
		verdict(o, V_INCONCLUSIVE, "Memory", "stopped before all patterns ran");
	else
		verdict(o, V_PASS, "Memory", "%zu MiB tested, no errors (memory in use by the system is covered by Full memory test in the boot menu)",
			size >> 20);
}
