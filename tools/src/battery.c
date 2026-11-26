/* Laptop battery health from the power-supply sysfs class: full-charge capacity against the
 * design capacity, plus the AC adapter state. */
#include "bedrock.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>

#define PS "/sys/class/power_supply"

static long long attr(const char *dir, const char *a)
{
	char p[320];
	snprintf(p, sizeof p, "%s/%s", dir, a);
	return read_ll(p, -1);
}

static void sattr(const char *dir, const char *a, char *out, size_t n)
{
	char p[320];
	snprintf(p, sizeof p, "%s/%s", dir, a);
	read_file(p, out, n);
}

/* 0 loaded, -1 when the bay is empty (present = 0). */
int batt_load(const char *dir, struct batt *b)
{
	const char *base = strrchr(dir, '/');
	memset(b, 0, sizeof *b);
	snprintf(b->name, sizeof b->name, "%s", base ? base + 1 : dir);
	if (attr(dir, "present") == 0)
		return -1;
	sattr(dir, "model_name", b->model, sizeof b->model);
	sattr(dir, "manufacturer", b->maker, sizeof b->maker);
	sattr(dir, "technology", b->tech, sizeof b->tech);
	sattr(dir, "status", b->status, sizeof b->status);
	b->ef = attr(dir, "energy_full");
	b->ed = attr(dir, "energy_full_design");
	b->cf = attr(dir, "charge_full");
	b->cd = attr(dir, "charge_full_design");
	b->vmin = attr(dir, "voltage_min_design");
	b->cycles = attr(dir, "cycle_count");
	b->capacity = attr(dir, "capacity");
	return 0;
}

/* One side reported as energy, the other as charge: convert charge with the design voltage. */
static double uwh(long long e, long long c, long long vmin)
{
	return e > 0 ? e : c > 0 && vmin > 0 ? (double)c * vmin / 1e6 : 0;
}

enum verdict batt_check(const struct batt *b, struct batt_calc *c, char *why, size_t n)
{
	double d, f, k, pct;
	int chg = 0, len;
	const char *grade;
	c->pct = c->full_wh = c->design_wh = -1;
	if (b->ed > 0 && b->ef > 0) {
		d = b->ed, f = b->ef;
	} else if (b->cd > 0 && b->cf > 0) {
		d = b->cd, f = b->cf, chg = 1; /* same unit on both sides: no conversion needed */
	} else {
		d = uwh(b->ed, b->cd, b->vmin);
		f = uwh(b->ef, b->cf, b->vmin);
	}
	if (d <= 0) {
		snprintf(why, n, "the battery does not report a usable design capacity");
		return V_INCONCLUSIVE;
	}
	if (f <= 0) {
		snprintf(why, n, "the battery does not report its full-charge capacity");
		return V_INCONCLUSIVE;
	}
	k = chg ? (b->vmin > 0 ? b->vmin / 1e12 : 0) : 1e-6; /* to Wh, 0 = cannot convert */
	/* Real laptop packs are 1-500 Wh; anything else (or 50 Ah unconverted) is a bad register. */
	if ((k && (d * k < 1 || d * k > 500)) || (!k && d > 50e6) || f * 10 > d * 13) {
		snprintf(why, n, "the battery reports capacity values that are not credible (design %.0f, full %.0f)", d, f);
		return V_INCONCLUSIVE;
	}
	pct = 100.0 * f / d;
	c->pct = pct;
	if (k) {
		c->design_wh = d * k;
		c->full_wh = f * k;
	}
	grade = pct >= 85 ? "Excellent" : pct >= 80 ? "Good" : pct >= 50 ? "Fair" : "Poor";
	len = snprintf(why, n, "health %d%% (%s)", (int)pct, grade);
	if (k)
		len += snprintf(why + len, n - len, ", %.1f of %.1f Wh", c->full_wh, c->design_wh);
	if (b->cycles >= 0)
		snprintf(why + len, n - len, ", %lld cycles", b->cycles);
	return pct >= 50 ? V_PASS : V_FAIL;
}

static int by_name(const void *a, const void *b)
{
	return strcmp(a, b);
}

/* Peripherals (wireless mice, pens) show up here too, with scope "Device". */
static int ps_is(const char *name, const char *type)
{
	char d[300], t[32], s[16];
	snprintf(d, sizeof d, PS "/%s", name);
	sattr(d, "type", t, sizeof t);
	sattr(d, "scope", s, sizeof s);
	return !strcmp(t, type) && strcmp(s, "Device");
}

#define OR(s) ((s)[0] ? (s) : "?")

static void batt_one(FILE *o, const struct batt *b, int i)
{
	struct batt_calc c;
	char why[384], test[64], key[48];
	snprintf(test, sizeof test, "Battery %s", b->name);
	fprintf(o, "  %s  %s %s  %s  %s", b->name, OR(b->maker), OR(b->model), OR(b->tech), OR(b->status));
	if (b->capacity >= 0)
		fprintf(o, ", charge %lld%%", b->capacity);
	fputc('\n', o);
	enum verdict v = batt_check(b, &c, why, sizeof why);
	verdict(o, v, test, "%s", why);
	snprintf(key, sizeof key, "battery.%d.model", i);
	fact(key, "%s", b->model[0] ? b->model : b->name);
	if (c.pct >= 0) {
		snprintf(key, sizeof key, "battery.%d.health_pct", i);
		fact(key, "%d", (int)c.pct);
	}
	if (c.design_wh >= 0) {
		snprintf(key, sizeof key, "battery.%d.design_wh", i);
		fact(key, "%.1f", c.design_wh);
		snprintf(key, sizeof key, "battery.%d.full_wh", i);
		fact(key, "%.1f", c.full_wh);
	}
	if (b->cycles >= 0) {
		snprintf(key, sizeof key, "battery.%d.cycles", i);
		fact(key, "%lld", b->cycles);
	}
}

void battery_show(FILE *o)
{
	char names[16][32], dir[300];
	int n = 0, nb = 0;
	DIR *d = opendir(PS);
	struct dirent *e;
	while (d && n < 16 && (e = readdir(d)))
		if (e->d_name[0] != '.' && strlen(e->d_name) < sizeof names[0])
			strcpy(names[n++], e->d_name);
	if (d)
		closedir(d);
	qsort(names, n, sizeof names[0], by_name);
	fprintf(o, "Battery\n");
	for (int i = 0; i < n; i++) {
		struct batt b;
		snprintf(dir, sizeof dir, PS "/%s", names[i]);
		if (ps_is(names[i], "Battery") && !batt_load(dir, &b))
			batt_one(o, &b, nb++);
	}
	if (!nb)
		verdict(o, V_ABSENT, "Battery", "no battery (desktop, or removed)");
	for (int i = 0; i < n; i++) {
		if (!ps_is(names[i], "Mains"))
			continue;
		snprintf(dir, sizeof dir, PS "/%s", names[i]);
		long long on = attr(dir, "online");
		fprintf(o, "  AC adapter %s: %s\n", names[i], on == 1 ? "plugged in" : on == 0 ? "not plugged in" : "unknown");
	}
}
