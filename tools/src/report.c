#include "bedrock.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

volatile sig_atomic_t g_stop;
int g_count[4], g_color;

void verdict(FILE *o, enum verdict v, const char *test, const char *fmt, ...)
{
	static const char *name[] = {"PASS", "FAIL", "INCONCLUSIVE", "NOT PRESENT"};
	static const char *col[] = {"\033[1;32m", "\033[1;31m", "\033[1;33m", "\033[2m"};
	va_list ap;
	char detail[384];
	g_count[v]++;
	va_start(ap, fmt);
	vsnprintf(detail, sizeof detail, fmt, ap);
	va_end(ap);
	record_result(v, test, detail);
	fprintf(o, "  %s%-12s%s %s: %s\n", g_color ? col[v] : "", name[v], g_color ? "\033[0m" : "", test, detail);
}

/* Whole small file as a string, trailing whitespace removed. -1 if unreadable. */
int read_file(const char *path, char *buf, size_t n)
{
	int fd = open(path, O_RDONLY), len = 0;
	ssize_t r = 0;
	buf[0] = 0;
	if (fd < 0)
		return -1;
	while ((size_t)len + 1 < n && (r = read(fd, buf + len, n - 1 - len)) > 0)
		len += r;
	close(fd);
	if (r < 0)
		return -1;
	while (len && isspace((unsigned char)buf[len - 1]))
		len--;
	buf[len] = 0;
	return len;
}

long long read_ll(const char *path, long long def)
{
	char buf[64], *end;
	if (read_file(path, buf, sizeof buf) <= 0)
		return def;
	long long v = strtoll(buf, &end, 10);
	return end == buf ? def : v;
}

void report_run(FILE *o, const char *title)
{
	struct disk d[32];
	struct utsname u;
	time_t t = time(NULL);
	char ts[64];
	int color = g_color;
	g_color = 0; /* plain text in files and on serial */
	memset(g_count, 0, sizeof g_count);
	record_reset();
	strftime(ts, sizeof ts, "%Y-%m-%d %H:%M UTC", gmtime(&t));
	uname(&u);
	fprintf(o, "%s\n%s, %s, kernel %s %s\n"
		   "Quick checks: 10 s CPU stress, up to 64 MiB RAM, 256-block sampled surface scan.\n\n",
		HEADER, title, ts, u.release, u.machine);
	cpu_show(o);
	board_show(o);
	gpu_show(o);
	disk_show(o);
	int n = disk_list(d, 32);
	for (int i = 0; i < n; i++)
		disk_scan(o, &d[i], 1);
	cpu_stress(o, 10);
	mem_test(o, 64 << 20);
	fprintf(o, "\nSummary: %d pass, %d fail, %d inconclusive, %d not present\n",
		g_count[V_PASS], g_count[V_FAIL], g_count[V_INCONCLUSIVE], g_count[V_ABSENT]);
	g_color = color;
}

/* First writable FAT mount (the stick's data partition) into mp[256]; -1 if none. */
int report_mount(char *mp)
{
	char mounts[16384], dev[256], type[32], opts[256];
	if (read_file("/proc/mounts", mounts, sizeof mounts) < 0)
		return -1;
	for (char *l = mounts; l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL)
		if (sscanf(l, "%255s %255s %31s %255s", dev, mp, type, opts) == 4 &&
		    !strcmp(type, "vfat") && !strncmp(opts, "rw", 2))
			return 0;
	mp[0] = 0;
	return -1;
}

/* Write the report to the stick's data partition. */
int report_save(char *path, size_t n)
{
	char mp[256], board[64] = "", date[32];
	if (report_mount(mp))
		return -1;
	if (read_file("/sys/class/dmi/id/board_name", board, sizeof board) <= 0)
		read_file("/sys/class/dmi/id/product_name", board, sizeof board);
	for (char *c = board; *c; c++)
		if (!isalnum((unsigned char)*c))
			*c = '-';
	time_t t = time(NULL);
	strftime(date, sizeof date, "%Y%m%d-%H%M", gmtime(&t));
	snprintf(path, n, "%s/bedrock-%s-%s.txt", mp, board[0] ? board : "unknown", date);
	FILE *f = fopen(path, "w");
	if (!f)
		return -1;
	report_run(f, "Report");
	fflush(f);
	fsync(fileno(f)); /* the user pulls the stick right after */
	return fclose(f);
}
