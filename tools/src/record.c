/* The machine record: every verdict and every identifying fact of one run, kept in
 * memory so signed certificates, inventory rows and JSON exports are built from
 * the same data the screen shows. */
#include "bedrock.h"

#include <stdarg.h>
#include <string.h>
#include <time.h>

struct result g_res[RECORD_MAX];
int g_nres;
struct fact g_fact[RECORD_MAX];
int g_nfact;

void record_reset(void)
{
	g_nres = g_nfact = 0;
}

void record_result(enum verdict v, const char *test, const char *detail)
{
	if (g_nres >= RECORD_MAX)
		return;
	struct result *r = &g_res[g_nres++];
	r->v = v;
	snprintf(r->test, sizeof r->test, "%s", test);
	snprintf(r->detail, sizeof r->detail, "%s", detail);
}

/* Later facts with the same key replace earlier ones. */
void fact(const char *key, const char *fmt, ...)
{
	va_list ap;
	struct fact *f = NULL;
	for (int i = 0; i < g_nfact && !f; i++)
		if (!strcmp(g_fact[i].key, key))
			f = &g_fact[i];
	if (!f) {
		if (g_nfact >= RECORD_MAX)
			return;
		f = &g_fact[g_nfact++];
		snprintf(f->key, sizeof f->key, "%s", key);
	}
	va_start(ap, fmt);
	vsnprintf(f->val, sizeof f->val, fmt, ap);
	va_end(ap);
	/* Firmware strings come padded; trim so CSV and JSON stay clean. */
	size_t n = strlen(f->val);
	while (n && f->val[n - 1] == ' ')
		f->val[--n] = 0;
}

const char *fact_get(const char *key)
{
	for (int i = 0; i < g_nfact; i++)
		if (!strcmp(g_fact[i].key, key))
			return g_fact[i].val;
	return NULL;
}

void json_str(FILE *o, const char *s)
{
	fputc('"', o);
	for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
		if (*c == '"' || *c == '\\')
			fprintf(o, "\\%c", *c);
		else if (*c == '\n')
			fputs("\\n", o);
		else if (*c < 0x20 || *c == 0x7f)
			fprintf(o, "\\u%04x", *c);
		else
			fputc(*c, o);
	}
	fputc('"', o);
}

const char *verdict_name(enum verdict v)
{
	static const char *n[] = {"PASS", "FAIL", "INCONCLUSIVE", "NOT PRESENT"};
	return n[v];
}

void iso_time(time_t t, char *buf, size_t n)
{
	strftime(buf, n, "%Y-%m-%dT%H:%M:%SZ", gmtime(&t));
}

/* {"tool":...,"created":...,"facts":{...},"results":[...],"summary":{...}} */
void record_json(FILE *o)
{
	char ts[32];
	iso_time(time(NULL), ts, sizeof ts);
	fprintf(o, "{\"tool\":{\"name\":\"Bedrock\",\"version\":\"%s\",\"by\":\"erdem6.com\"},\"created\":\"%s\",\"facts\":{",
		VERSION, ts);
	for (int i = 0; i < g_nfact; i++) {
		fputs(i ? "," : "", o);
		json_str(o, g_fact[i].key);
		fputc(':', o);
		json_str(o, g_fact[i].val);
	}
	fputs("},\"results\":[", o);
	int c[4] = {0};
	for (int i = 0; i < g_nres; i++) {
		c[g_res[i].v]++;
		fprintf(o, "%s{\"test\":", i ? "," : "");
		json_str(o, g_res[i].test);
		fprintf(o, ",\"verdict\":\"%s\",\"detail\":", verdict_name(g_res[i].v));
		json_str(o, g_res[i].detail);
		fputc('}', o);
	}
	fprintf(o, "],\"summary\":{\"pass\":%d,\"fail\":%d,\"inconclusive\":%d,\"not_present\":%d}}", c[0], c[1], c[2], c[3]);
}
