/* The station run: test, document and export one PC without an operator, driven by
 * bedrock.cfg on the stick. Outputs on the stick: inventory.csv (one row per PC),
 * bedrock/records/<id>-<date>.json and .txt. */
#include "bedrock.h"
#include "vendor/qrcodegen.h"

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

/* ---- bedrock.cfg ---- */

void cfg_defaults(struct cfg *c)
{
	memset(c, 0, sizeof *c); /* menu, no prompts, quick memory, sampled scan, wipe off */
	c->cpu_seconds = 30;
}

static char *trim(char *s)
{
	char *e;
	while (isspace((unsigned char)*s))
		s++;
	for (e = s + strlen(s); e > s && isspace((unsigned char)e[-1]); )
		*--e = 0;
	return s;
}

/* Index of v in names (any case), else def: a typo never changes a setting. */
static int pick(const char *v, const char *const *names, int n, int def)
{
	for (int i = 0; i < n; i++)
		if (!strcasecmp(v, names[i]))
			return i;
	return def;
}

static void cfg_line(struct cfg *c, char *line)
{
	static const char *const mode[] = {"menu", "station"}, *const yn[] = {"no", "yes"},
		*const mem[] = {"quick", "full", "off"}, *const scan[] = {"sampled", "full", "off"},
		*const wipe[] = {"off", "ask", "auto"};
	char *eq = strchr(line = trim(line), '='), *end;
	if (!eq || *line == '#')
		return;
	*eq = 0;
	char *k = trim(line), *v = trim(eq + 1);
	long x;
	if (!strcasecmp(k, "operator"))
		snprintf(c->operator, sizeof c->operator, "%s", v);
	else if (!strcasecmp(k, "organization"))
		snprintf(c->organization, sizeof c->organization, "%s", v);
	else if (!strcasecmp(k, "location"))
		snprintf(c->location, sizeof c->location, "%s", v);
	else if (!strcasecmp(k, "mode"))
		c->station = pick(v, mode, 2, c->station);
	else if (!strcasecmp(k, "ask_asset_tag"))
		c->ask_asset_tag = pick(v, yn, 2, c->ask_asset_tag);
	else if (!strcasecmp(k, "cpu_seconds")) {
		x = strtol(v, &end, 10);
		if (end != v && !*end && x >= 0 && x <= 3600)
			c->cpu_seconds = x;
	} else if (!strcasecmp(k, "memory"))
		c->memory = pick(v, mem, 3, c->memory);
	else if (!strcasecmp(k, "disk_scan"))
		c->disk_scan = pick(v, scan, 3, c->disk_scan);
	else if (!strcasecmp(k, "wipe"))
		c->wipe = pick(v, wipe, 3, c->wipe);
	else if (!strcasecmp(k, "wipe_confirm"))
		snprintf(c->wipe_confirm, sizeof c->wipe_confirm, "%s", v);
	else if (!strcasecmp(k, "poweroff_when_done"))
		c->poweroff_when_done = pick(v, yn, 2, c->poweroff_when_done);
	/* unknown keys are ignored: a newer stick config still works on older images */
}

/* key = value lines; whole-line # comments only (a # inside a value is data);
 * LF or CRLF; a UTF-8 byte order mark from Notepad is skipped. */
void cfg_parse(struct cfg *c, const char *text)
{
	char line[512];
	if (!strncmp(text, "\xef\xbb\xbf", 3))
		text += 3;
	while (*text) {
		size_t len = strcspn(text, "\n");
		snprintf(line, sizeof line, "%.*s", (int)len, text);
		cfg_line(c, line);
		text += len + (text[len] == '\n');
	}
}

int cfg_load(struct cfg *c)
{
	static char buf[16384];
	char mp[256], path[300];
	cfg_defaults(c);
	if (report_mount(mp) || snprintf(path, sizeof path, "%s/bedrock.cfg", mp) >= (int)sizeof path ||
	    read_file(path, buf, sizeof buf) < 0)
		return -1;
	cfg_parse(c, buf);
	return 0;
}

/* ---- inventory.csv (RFC 4180: CRLF lines, quotes doubled) ---- */

void csv_field(FILE *o, const char *s)
{
	/* Firmware strings are untrusted: a leading = + @ (or - before a non-digit) would
	 * run as a formula in a spreadsheet, so it is kept as text. */
	if ((s[0] && strchr("=+@\t\r", s[0])) || (s[0] == '-' && s[1] && (s[1] < '0' || s[1] > '9')))
		fputc('\'', o);
	if (!s[strcspn(s, ",\"\r\n")]) {
		fputs(s, o);
		return;
	}
	fputc('"', o);
	for (; *s; s++) {
		if (*s == '"')
			fputc('"', o);
		fputc(*s, o);
	}
	fputc('"', o);
}

static void csv_row(FILE *o, const char *const *f, int n)
{
	for (int i = 0; i < n; i++) {
		if (i)
			fputc(',', o);
		csv_field(o, f[i]);
	}
	fputs("\r\n", o);
}

void inventory_header(FILE *o)
{
	static const char *const h[] = {"date", "asset_tag", "operator", "organization", "location",
		"manufacturer", "product", "serial", "board", "cpu", "threads", "ram_mb", "disk_count",
		"disk_total_gb", "disk_models", "disk_serials", "gpu", "battery_health", "result", "pass",
		"fail", "inconclusive", "record"};
	csv_row(o, h, sizeof h / sizeof *h);
}

static const char *fv(const char *key)
{
	const char *v = fact_get(key);
	return v ? v : "";
}

static const char *first(const char *a, const char *b)
{
	return *fv(a) ? fv(a) : fv(b);
}

/* "system.manufacturer system.product", falling back to the board's. */
static void model_str(char *buf, size_t n)
{
	snprintf(buf, n, "%s %s", first("system.manufacturer", "board.manufacturer"),
		first("system.product", "board.product"));
	trim(buf);
}

/* Fact "disk.N" is "name|model|serial|bytes|kind". Returns 0 when the key is absent. */
static int disk_fact(int i, char name[32], char model[64], char serial[64], unsigned long long *bytes)
{
	char key[16], tmp[160], *p[4];
	snprintf(key, sizeof key, "disk.%d", i);
	const char *v = fact_get(key);
	if (!v)
		return 0;
	snprintf(tmp, sizeof tmp, "%s", v);
	p[0] = tmp;
	for (int k = 1; k < 4; k++) {
		char *bar = strchr(p[k - 1], '|');
		if (bar)
			*bar++ = 0;
		p[k] = bar ? bar : p[k - 1] + strlen(p[k - 1]);
	}
	snprintf(name, 32, "%s", p[0]);
	snprintf(model, 64, "%s", p[1]);
	snprintf(serial, 64, "%s", p[2]);
	*bytes = strtoull(p[3], NULL, 10);
	return 1;
}

/* One row from the facts in memory. The boot stick (fact station.boot_disk) is not a PC disk. */
void inventory_row(FILE *o, const char *date, const char *result, const int cnt[4], const char *record)
{
	char board[200], ram[24], dcount[16], dtotal[24], dmodels[512] = "", dserials[512] = "", gpu[512] = "",
		p[3][16], name[32], dm[64], ds[64], key[16];
	unsigned long long bytes, total = 0;
	int nd = 0;
	const char *boot = fact_get("station.boot_disk");
	for (int i = 0; disk_fact(i, name, dm, ds, &bytes); i++) {
		if (boot && !strcmp(boot, name))
			continue;
		snprintf(dmodels + strlen(dmodels), sizeof dmodels - strlen(dmodels), "%s%s", nd ? "; " : "", dm);
		snprintf(dserials + strlen(dserials), sizeof dserials - strlen(dserials), "%s%s", nd ? "; " : "", ds[0] ? ds : "-");
		nd++;
		total += bytes;
	}
	for (int i = 0; i < 8; i++) {
		snprintf(key, sizeof key, "gpu.%d", i);
		if (fact_get(key))
			snprintf(gpu + strlen(gpu), sizeof gpu - strlen(gpu), "%s%s", gpu[0] ? "; " : "", fact_get(key));
	}
	snprintf(board, sizeof board, "%s %s", fv("board.manufacturer"), fv("board.product"));
	trim(board);
	snprintf(ram, sizeof ram, "%s", atoll(fv("memory.installed_mb")) > 0 ? fv("memory.installed_mb") : fv("memory.visible_mb"));
	snprintf(dcount, sizeof dcount, "%d", nd);
	snprintf(dtotal, sizeof dtotal, "%.1f", total / 1e9);
	for (int i = 0; i < 3; i++)
		snprintf(p[i], sizeof p[i], "%d", cnt[i]);
	const char *f[] = {date, fv("station.asset_tag"), fv("station.operator"), fv("station.organization"),
		fv("station.location"), first("system.manufacturer", "board.manufacturer"),
		first("system.product", "board.product"), fv("system.serial"), board, fv("cpu.brand"),
		fv("cpu.threads"), ram, dcount, dtotal, dmodels, dserials, gpu, fv("battery.0.health_pct"), result,
		p[V_PASS], p[V_FAIL], p[V_INCONCLUSIVE], record};
	csv_row(o, f, sizeof f / sizeof *f);
}

/* ---- outputs on the stick ---- */

enum verdict station_result(const int cnt[4])
{
	if (cnt[V_FAIL])
		return V_FAIL;
	return cnt[V_INCONCLUSIVE] || !cnt[V_PASS] ? V_INCONCLUSIVE : V_PASS; /* nothing proven is not a pass */
}

/* File-name safe, at most 24 characters (the QR code has to stay small). */
void safe_id(const char *s, char *out, size_t n)
{
	size_t k = 0;
	for (; *s && k < n - 1 && k < 24; s++)
		if (isalnum((unsigned char)*s) || *s == '_')
			out[k++] = *s;
		else if (k && out[k - 1] != '-')
			out[k++] = '-';
	while (k && out[k - 1] == '-')
		k--;
	out[k] = 0;
}

static char rec_name[96]; /* <id>-<yyyymmdd-hhmm>, shared by .json, .txt and the QR code */

static void rec_base(time_t t)
{
	char id[32], ts[16];
	const char *src = *fv("station.asset_tag") ? fv("station.asset_tag") :
		*fv("system.serial") ? fv("system.serial") : fv("board.product");
	safe_id(src, id, sizeof id);
	strftime(ts, sizeof ts, "%Y%m%d-%H%M", gmtime(&t));
	snprintf(rec_name, sizeof rec_name, "%s-%s", id[0] ? id : "unknown", ts);
}

/* <stick>/bedrock/records/<rec_name><ext>; -1 when no stick is mounted. */
static int records_path(char *out, size_t n, const char *ext)
{
	char mp[256], d[320];
	if (report_mount(mp))
		return -1;
	if (!rec_name[0])
		rec_base(time(NULL));
	snprintf(d, sizeof d, "%s/bedrock", mp);
	if (mkdir(d, 0777) && errno != EEXIST)
		return -1;
	snprintf(d, sizeof d, "%s/bedrock/records", mp);
	if (mkdir(d, 0777) && errno != EEXIST)
		return -1;
	return snprintf(out, n, "%s/%s%s", d, rec_name, ext) >= (int)n ? -1 : 0;
}

/* The one place a machine record is written. path (capacity n) is the file to write;
 * empty, it becomes the station location on the stick and is returned in path. */
int station_record(char *path, size_t n)
{
	if (!path[0]) /* the station's own record: signed when the stick has a key */
		return signed_save("health", NULL, "records", rec_name, path, n) < 0 ? -1 : 0;
	FILE *f = fopen(path, "w");
	if (!f)
		return -1;
	record_json(f);
	fflush(f);
	fsync(fileno(f)); /* the user pulls the stick right after */
	return fclose(f);
}

static int inventory_append(time_t t, const char *res, const int cnt[4])
{
	char mp[256], p[300], ts[32], rec[110];
	if (report_mount(mp) || snprintf(p, sizeof p, "%s/inventory.csv", mp) >= (int)sizeof p)
		return -1;
	FILE *f = fopen(p, "a");
	if (!f)
		return -1;
	fseek(f, 0, SEEK_END);
	if (!ftell(f))
		inventory_header(f);
	iso_time(t, ts, sizeof ts);
	snprintf(rec, sizeof rec, "%s.json", rec_name);
	inventory_row(f, ts, res, cnt, rec);
	fflush(f);
	fsync(fileno(f));
	return fclose(f);
}

/* ---- QR code ---- */

#define QR_VERSION_MAX 6 /* 134 bytes at low error correction; 41x41 modules */
#define QR_TEXT_MAX 134

static void clean(char *dst, size_t n, const char *s)
{
	size_t k = 0;
	for (; *s && k < n - 1; s++, k++)
		dst[k] = *s == '|' ? '/' : (unsigned char)*s < 0x20 ? ' ' : *s;
	dst[k] = 0;
}

/* BEDROCK1|asset|serial|manufacturer product|result|date|record file. Fields are
 * bounded so the whole text fits QR_TEXT_MAX; only the model is cut when needed. */
int station_qr_text(char *buf, size_t n, const char *asset, const char *serial, const char *model,
	const char *result, const char *date, const char *record)
{
	char a[25], s[25], m[QR_TEXT_MAX], r[48];
	clean(a, sizeof a, asset);
	clean(s, sizeof s, serial);
	clean(m, sizeof m, model);
	clean(r, sizeof r, record);
	int len = snprintf(buf, n, "BEDROCK1|%s|%s|%s|%s|%s|%s", a, s, m, result, date, r);
	if (len > QR_TEXT_MAX) {
		size_t cut = len - QR_TEXT_MAX, ml = strlen(m);
		m[ml > cut ? ml - cut : 0] = 0;
		len = snprintf(buf, n, "BEDROCK1|%s|%s|%s|%s|%s|%s", a, s, m, result, date, r);
	}
	return len;
}

/* Module matrix for text into qr (STATION_QR_LEN bytes); the size in modules, 0 if it does not fit. */
int station_qr(const char *text, uint8_t qr[])
{
	uint8_t tmp[STATION_QR_LEN];
	if (!qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_LOW, 1, QR_VERSION_MAX, qrcodegen_Mask_AUTO, true))
		return 0;
	return qrcodegen_getSize(qr);
}

static int lit(const uint8_t *qr, int x, int y)
{
	int size = qrcodegen_getSize(qr);
	return x >= 0 && y >= 0 && x < size && y < size && qrcodegen_getModule(qr, x, y);
}

/* Two module rows per console row with half blocks, dark modules black on a white
 * ground, quiet zone included. row > 0: each line is placed at (row+i, col); else plain lines. */
void station_qr_draw(FILE *o, const uint8_t *qr, int quiet, int row, int col)
{
	int size = qrcodegen_getSize(qr), place = row > 0;
	for (int y = -quiet; y < size + quiet; y += 2) {
		if (place)
			fprintf(o, "\033[%d;%dH\033[30;47m", row++, col);
		for (int x = -quiet; x < size + quiet; x++) {
			int t = lit(qr, x, y), b = lit(qr, x, y + 1);
			fputs(t ? (b ? "\xe2\x96\x88" : "\xe2\x96\x80") : (b ? "\xe2\x96\x84" : " "), o);
		}
		fputs(place ? "\033[0m" : "\n", o);
	}
}

/* ---- screens ---- */

/* 5x5 block letters for the result word; bit 4 is the left column. */
static const struct glyph { char c; uint8_t r[5]; } font[] = {
	{'P', {0x1e, 0x11, 0x1e, 0x10, 0x10}}, {'A', {0x0e, 0x11, 0x1f, 0x11, 0x11}},
	{'S', {0x0f, 0x10, 0x0e, 0x01, 0x1e}}, {'F', {0x1f, 0x10, 0x1e, 0x10, 0x10}},
	{'I', {0x1f, 0x04, 0x04, 0x04, 0x1f}}, {'L', {0x10, 0x10, 0x10, 0x10, 0x1f}},
	{'C', {0x0f, 0x10, 0x10, 0x10, 0x0f}}, {'H', {0x11, 0x11, 0x1f, 0x11, 0x11}},
	{'E', {0x1f, 0x10, 0x1e, 0x10, 0x1f}}, {'K', {0x11, 0x12, 0x1c, 0x12, 0x11}},
};

static void big_word(const char *w, int row, int col, const char *color)
{
	for (int y = 0; y < 5; y++) {
		printf("\033[%d;%dH%s", row + y, col, color);
		for (const char *c = w; *c; c++)
			for (size_t g = 0; g < sizeof font / sizeof *font; g++)
				if (font[g].c == *c)
					for (int x = 4; x >= -1; x--)
						fputs(x >= 0 && font[g].r[y] >> x & 1 ? "\xe2\x96\x88" : " ", stdout);
		fputs("\033[0m", stdout);
	}
}

static time_t t_start;
static int tick_fd = -1;
static volatile int tick_run;
static pthread_t tick_th;

/* Elapsed time on row 9; saves and restores the cursor so test output is not disturbed. */
static void *ticker(void *arg)
{
	(void)arg;
	for (int last = -1; tick_run; usleep(200000)) {
		char b[64];
		int s = (int)(time(NULL) - t_start);
		if (s == last)
			continue;
		last = s;
		int n = snprintf(b, sizeof b, "\033" "7\033[9;1H  Elapsed %02d:%02d:%02d\033[K\033" "8",
			s / 3600, s / 60 % 60, s % 60);
		if (write(tick_fd, b, n) < 0)
			break;
	}
	return NULL;
}

static void tick_start(void)
{
	tick_run = 1;
	if (pthread_create(&tick_th, NULL, ticker, NULL))
		tick_run = 0;
}

static void tick_stop(void)
{
	if (tick_run) {
		tick_run = 0;
		pthread_join(tick_th, NULL);
	}
}

static void progress(int i, int n, const char *name)
{
	char bar[41];
	int fill = 40 * i / n;
	memset(bar, '#', fill);
	memset(bar + fill, '.', 40 - fill);
	bar[40] = 0;
	printf("\033[2J\033[H\033[1m%s\033[0m\033[3;1H\033[1mStation run\033[0m\033[5;1HStep %d of %d"
	       "\033[6;1H\033[1;37m%s\033[0m\033[7;1H[%s]\033[11;1HCtrl-C stops the current test early."
	       "\033[13;1H", HEADER, i + 1, n, name, bar);
	fflush(stdout);
}

/* Any key ends the wait. secs < 0 waits for a key; else it counts down on row 24. */
static void wait_key(int secs)
{
	struct termios t, raw;
	struct pollfd p = {0, POLLIN, 0};
	if (tcgetattr(0, &t)) /* no console: nothing to wait for */
		return;
	raw = t;
	raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
	raw.c_cc[VMIN] = 1;
	raw.c_cc[VTIME] = 0;
	tcsetattr(0, TCSANOW, &raw);
	tcflush(0, TCIFLUSH);
	for (int left = secs; secs < 0 || left > 0; left--) {
		if (secs >= 0)
			printf("[24;1H[2KPowering off in %d s. Any key: now.", left);
		else
			printf("[24;1H[2KPress any key to continue.");
		fflush(stdout);
		if (poll(&p, 1, secs < 0 ? -1 : 1000) > 0) {
			char c;
			if (read(0, &c, 1) >= 0)
				break;
		}
	}
	tcsetattr(0, TCSANOW, &t);
}

static void result_screen(enum verdict res, const char *model, const int cnt[4], int saved)
{
	static const char *const word[] = {"PASS", "FAIL", "CHECK"}, *const col[] = {"\033[1;32m", "\033[1;31m", "\033[1;33m"};
	uint8_t qr[STATION_QR_LEN];
	char text[QR_TEXT_MAX + 8], date[16], rec[110];
	struct winsize w;
	int rows = !ioctl(1, TIOCGWINSZ, &w) && w.ws_row >= 24 ? w.ws_row : 25, k = res == V_PASS ? 0 : res == V_FAIL ? 1 : 2;
	const char *serial = fv("system.serial"), *asset = fv("station.asset_tag");
	time_t t = time(NULL);
	strftime(date, sizeof date, "%Y-%m-%d", gmtime(&t));
	snprintf(rec, sizeof rec, "%s.json", rec_name);
	printf("\033[2J\033[H\033[1m%s\033[0m", HEADER);
	big_word(word[k], 3, 2, col[k]);
	if (res == V_INCONCLUSIVE)
		printf("\033[9;2H%sINCONCLUSIVE\033[0m\033[10;2HSome checks could not be measured", col[2]);
	printf("\033[11;2HAsset  %.26s\033[12;2HModel  %.26s\033[13;2HSerial %.26s",
		*asset ? asset : "-", *model ? model : "-", *serial ? serial : "-");
	printf("\033[15;2H%d pass   %d fail   %d inconclusive", cnt[V_PASS], cnt[V_FAIL], cnt[V_INCONCLUSIVE]);
	if (saved)
		printf("\033[17;2HSaved to the USB stick:\033[18;2H%.32s", rec);
	else
		printf("\033[17;2H\033[1;31mNOT SAVED: no writable USB\033[18;2Hpartition was found.\033[0m");
	station_qr_text(text, sizeof text, asset, serial, model, verdict_name(res), date, rec);
	int size = station_qr(text, qr);
	if (size) {
		int quiet = size + 8 <= 46 && (size + 9) / 2 <= rows - 1 ? 4 : 2;
		station_qr_draw(stdout, qr, quiet, 2, 80 - (size + 2 * quiet) + 1);
	}
	fflush(stdout);
}

/* ---- steps ---- */

static struct cfg cfg;

static void off(FILE *o, const char *what)
{
	fprintf(o, "%s: off in bedrock.cfg\n", what);
}

static void step_sysinfo(FILE *o)
{
	cpu_show(o);
	board_show(o);
	gpu_show(o);
}

static void step_disks(FILE *o)
{
	disk_show(o);
}

static void step_scan(FILE *o)
{
	struct disk d[32];
	int n = disk_list(d, 32), done = 0;
	const char *boot = fact_get("station.boot_disk");
	if (cfg.disk_scan == SCAN_OFF) {
		off(o, "Surface scan");
		return;
	}
	for (int i = 0; i < n && !g_stop; i++)
		if (!boot || strcmp(boot, d[i].name)) {
			disk_scan(o, &d[i], cfg.disk_scan == SCAN_SAMPLED);
			done++;
		}
	if (!done)
		fprintf(o, "Surface scan: no disks besides the boot stick\n");
}

static void step_cpu(FILE *o)
{
	if (cfg.cpu_seconds)
		cpu_stress(o, cfg.cpu_seconds);
	else
		off(o, "CPU stress");
}

static void step_mem(FILE *o)
{
	if (cfg.memory != MEM_OFF)
		mem_test(o, cfg.memory == MEM_QUICK ? 64 << 20 : 0);
	else
		off(o, "Memory test");
}

static void step_wipe(FILE *o)
{
	if (cfg.wipe == WIPE_OFF) {
		off(o, "Wipe");
	} else if (cfg.wipe == WIPE_ASK) {
		char in[16];
		int first = g_nres;
		printf("\033[2J\033[H\033[1m%s\033[0m\n\n", HEADER);
		wipe_run();
		printf("\nPress Enter to continue.");
		fflush(stdout);
		if (!fgets(in, sizeof in, stdin))
			in[0] = 0;
		for (int i = first; i < g_nres; i++) /* wipe_run printed to the screen: copy into the log */
			fprintf(o, "  %-12s %s: %s\n", verdict_name(g_res[i].v), g_res[i].test, g_res[i].detail);
	} else if (!wipe_auto_ok(cfg.wipe_confirm)) {
		verdict(o, V_INCONCLUSIVE, "Wipe", "wipe = auto is set but wipe_confirm is not exactly \"" WIPE_AUTO_CONFIRM "\": nothing was erased");
	} else {
		/* wipe_auto prints to stdout: point it at the log while it runs. */
		fflush(o);
		fflush(stdout);
		int keep = dup(1);
		if (keep < 0 || dup2(fileno(o), 1) < 0) {
			verdict(o, V_INCONCLUSIVE, "Wipe", "cannot redirect output, nothing was erased");
			return;
		}
		wipe_auto();
		fflush(stdout);
		dup2(keep, 1);
		close(keep);
	}
}

/* The run order. A new check is one line here and one function above. */
static const struct step {
	const char *name;
	void (*run)(FILE *o);
} steps[] = {
	{"System information", step_sysinfo},
	{"Battery", battery_show},
	{"Disks and SMART", step_disks},
	{"Motherboard diagnosis", board_diag},
	{"Wi-Fi, Bluetooth, Ethernet, readers", functional_presence},
	{"Disk surface scan", step_scan},
	{"CPU stress", step_cpu},
	{"Memory test", step_mem},
	{"Wipe", step_wipe},
	{"Fleet server", fleet_upload},
};

static void prompt(const char *what, char *dst, size_t n)
{
	char in[128];
	printf("%s (Enter to skip): ", what);
	fflush(stdout);
	if (fgets(in, sizeof in, stdin) && *trim(in))
		snprintf(dst, n, "%s", trim(in));
}

static void tally(int cnt[4])
{
	memset(cnt, 0, 4 * sizeof *cnt);
	for (int i = 0; i < g_nres; i++)
		cnt[g_res[i].v]++;
}

/* Returns 1 when the machine should power off now (poweroff_when_done). */
int station_run(int from_menu)
{
	struct disk d[32];
	char tpath[512] = "", jpath[512] = "", model[200], ts[32];
	int cnt[4], saved, n = sizeof steps / sizeof *steps, nd, color = g_color;
	FILE *log;
	cfg_load(&cfg);
	g_color = 0; /* verdict lines go to a file */
	g_stop = 0;
	memset(g_count, 0, sizeof g_count);
	record_reset();
	if (isatty(0) && (cfg.ask_asset_tag || from_menu)) {
		printf("\033[2J\033[H\033[1m%s\033[0m\n\nStation run\n\n", HEADER);
		if (!cfg.operator[0])
			prompt("Operator", cfg.operator, sizeof cfg.operator);
		if (cfg.ask_asset_tag)
			prompt("Asset tag", cfg.asset_tag, sizeof cfg.asset_tag);
	}
	fact("station.operator", "%s", cfg.operator);
	fact("station.organization", "%s", cfg.organization);
	fact("station.location", "%s", cfg.location);
	fact("station.asset_tag", "%s", cfg.asset_tag);
	nd = disk_list(d, 32);
	for (int i = 0; i < nd; i++)
		if (disk_in_use(d[i].name)) {
			fact("station.boot_disk", "%s", d[i].name);
			break;
		}
	t_start = time(NULL);
	rec_base(t_start);
	log = records_path(tpath, sizeof tpath, ".txt") ? NULL : fopen(tpath, "w");
	saved = log != NULL;
	if (!log && !(log = tmpfile())) {
		g_color = color;
		return 0;
	}
	iso_time(t_start, ts, sizeof ts);
	fprintf(log, "%s\nStation run, %s\nOperator %s, organization %s, location %s, asset tag %s\n\n",
		HEADER, ts, cfg.operator, cfg.organization, cfg.location, cfg.asset_tag);

	tick_fd = dup(1);
	for (int i = 0; i < n && !g_stop; i++) {
		progress(i, n, steps[i].name);
		if (steps[i].run != step_wipe || cfg.wipe != WIPE_ASK) /* asking uses the whole screen */
			tick_start();
		steps[i].run(log);
		tick_stop();
		fflush(log);
	}
	if (g_stop)
		verdict(log, V_INCONCLUSIVE, "Station run", "stopped from the keyboard before every step finished");
	g_stop = 0;
	close(tick_fd);

	tally(cnt);
	enum verdict res = station_result(cnt);
	fprintf(log, "\nSummary: %d pass, %d fail, %d inconclusive, %d not present\nResult: %s\n",
		cnt[V_PASS], cnt[V_FAIL], cnt[V_INCONCLUSIVE], cnt[V_ABSENT], verdict_name(res));
	fact("station.result", "%s", verdict_name(res));
	fact("station.record", "%s.json", rec_name);
	fflush(log);
	if (saved)
		fsync(fileno(log));
	if (fclose(log))
		saved = 0;
	if (saved && (station_record(jpath, sizeof jpath) || inventory_append(t_start, verdict_name(res), cnt)))
		saved = 0;

	model_str(model, sizeof model);
	result_screen(res, model, cnt, saved);
	wait_key(cfg.poweroff_when_done ? 60 : -1);
	g_color = color;
	return cfg.poweroff_when_done;
}
