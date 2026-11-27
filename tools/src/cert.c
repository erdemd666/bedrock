/* Signed records: certificates and health records anyone can check offline at
 * erdem6.com/verify. Format: docs/CERTIFICATE.md (a contract with that page). */
#include "bedrock.h"
#include "vendor/monocypher-ed25519.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

/* Top-level payload keys taken from the "station.<key>" facts. */
static const char *station[] = {"operator", "organization", "location", "asset_tag"};

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* RFC 4648 section 4, with padding. Returns the length written, -1 if out is too small. */
int b64_encode(const uint8_t *in, size_t n, char *out, size_t outn)
{
	size_t need = (n + 2) / 3 * 4;
	if (outn <= need)
		return -1;
	for (size_t i = 0; i < n; i += 3) {
		uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
		*out++ = B64[v >> 18 & 63];
		*out++ = B64[v >> 12 & 63];
		*out++ = i + 1 < n ? B64[v >> 6 & 63] : '=';
		*out++ = i + 2 < n ? B64[v & 63] : '=';
	}
	*out = 0;
	return (int)need;
}

/* Strict: padded, no whitespace. Returns the bytes decoded, -1 on bad input or no room. */
int b64_decode(const char *in, uint8_t *out, size_t outn)
{
	size_t len = strlen(in), k = 0;
	if (len % 4)
		return -1;
	for (size_t i = 0; i < len; i += 4) {
		uint32_t v = 0;
		int pad = 0;
		for (int j = 0; j < 4; j++) {
			const char *c = strchr(B64, in[i + j]);
			if (in[i + j] == '=' && i + 4 == len && j >= 2)
				pad++;
			else if (!c || pad)
				return -1;
			v = v << 6 | (c ? (uint32_t)(c - B64) : 0);
		}
		if (k + 3 - pad > outn)
			return -1;
		for (int j = 0; j < 3 - pad; j++)
			out[k++] = v >> (16 - 8 * j) & 0xff;
	}
	return (int)k;
}

void key_id(const uint8_t pub[32], char id[17])
{
	uint8_t h[64];
	crypto_sha512(h, pub, 32);
	for (int i = 0; i < 8; i++)
		snprintf(id + 2 * i, 3, "%02x", h[i]);
}

static uint8_t g_sk[64], g_pk[32];
static char g_kid[17], g_where[300], g_note[400]; /* key file; why records are unsigned */
static int g_key;

void cert_key_seed(const uint8_t seed[32])
{
	uint8_t s[32];
	memcpy(s, seed, 32);
	crypto_ed25519_key_pair(g_sk, g_pk, s); /* wipes s */
	key_id(g_pk, g_kid);
	g_key = 1;
}

static int random_bytes(uint8_t *p, size_t n)
{
	if (getrandom(p, n, 0) == (ssize_t)n)
		return 0;
	int fd = open("/dev/urandom", O_RDONLY);
	ssize_t r = fd >= 0 ? read(fd, p, n) : -1;
	if (fd >= 0)
		close(fd);
	return r == (ssize_t)n ? 0 : -1;
}

static int read_seed(const char *path, uint8_t seed[32])
{
	uint8_t b[33];
	int fd = open(path, O_RDONLY);
	ssize_t r = fd >= 0 ? read(fd, b, sizeof b) : -1;
	if (fd >= 0)
		close(fd);
	if (r != 32)
		return -1;
	memcpy(seed, b, 32);
	return 0;
}

/* The stick's key, made on first use. 0: ready, msg says where; -1: msg says why not.
 * An existing key file is never replaced, and no throwaway key is ever used. */
int cert_key(char *msg, size_t n)
{
	char mp[256], p[300];
	uint8_t seed[32];
	if (g_key) {
		snprintf(msg, n, "ID %s, kept at %s", g_kid, g_where);
		return 0;
	}
	if (report_mount(mp)) {
		snprintf(g_note, sizeof g_note, "no writable USB stick partition to keep a signing key on");
		goto fail;
	}
	snprintf(p, sizeof p, "%s/bedrock", mp);
	mkdir(p, 0755);
	snprintf(p, sizeof p, "%s/bedrock/keys", mp);
	mkdir(p, 0700);
	snprintf(g_where, sizeof g_where, "%s/station.key", p);
	int fd = open(g_where, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (fd >= 0) {
		int ok = !random_bytes(seed, 32) && write(fd, seed, 32) == 32 && !fsync(fd);
		if (close(fd) || !ok) {
			unlink(g_where); /* ours, just made, never used */
			snprintf(g_note, sizeof g_note, "could not create a signing key at %s", g_where);
			goto fail;
		}
	} else if ((fd = errno) != EEXIST || read_seed(g_where, seed)) {
		snprintf(g_note, sizeof g_note, fd == EEXIST ? "%s is not a 32-byte key; it was left as it is"
			: "cannot create %s", g_where);
		goto fail;
	}
	cert_key_seed(seed);
	memset(seed, 0, sizeof seed);
	snprintf(p, sizeof p, "%s/bedrock/keys/station.pub", mp);
	if ((fd = open(p, O_WRONLY | O_CREAT | O_EXCL, 0644)) >= 0) {
		if (write(fd, g_pk, 32) == 32)
			fsync(fd);
		close(fd);
	}
	return cert_key(msg, n);
fail:
	snprintf(msg, n, "%s", g_note);
	return -1;
}

/* {members of each object in turn}; NULL inputs are skipped. -1 if one is not an object. */
int json_merge(FILE *o, const char *const *obj, int n)
{
	int any = 0;
	fputc('{', o);
	for (int i = 0; i < n; i++) {
		const char *p = obj[i], *e;
		if (!p)
			continue;
		while (isspace((unsigned char)*p))
			p++;
		e = p + strlen(p);
		while (e > p && isspace((unsigned char)e[-1]))
			e--;
		if (e - p < 2 || *p != '{' || e[-1] != '}')
			return -1;
		for (p++, e--; p < e && isspace((unsigned char)*p); p++)
			;
		while (e > p && isspace((unsigned char)e[-1]))
			e--;
		if (p < e)
			fprintf(o, "%s%.*s", any++ ? "," : "", (int)(e - p), p);
	}
	fputc('}', o);
	return 0;
}

/* The payload: kind first, station identity, the machine record, then the extras. */
static char *payload(const char *kind, const char *extra, size_t *len)
{
	char *head = NULL, *rec = NULL, *out = NULL, key[48];
	size_t hl, rl;
	FILE *m = open_memstream(&head, &hl);
	if (m) {
		fputs("{\"kind\":", m);
		json_str(m, kind);
		for (size_t i = 0; i < sizeof station / sizeof *station; i++) {
			snprintf(key, sizeof key, "station.%s", station[i]);
			const char *v = fact_get(key);
			if (v && *v) {
				fprintf(m, ",\"%s\":", station[i]);
				json_str(m, v);
			}
		}
		fputc('}', m);
		fclose(m);
	}
	if ((m = open_memstream(&rec, &rl))) {
		record_json(m);
		fclose(m);
	}
	const char *parts[] = {head, rec, extra};
	if (head && rec && (m = open_memstream(&out, len))) {
		int bad = json_merge(m, parts, 3);
		fclose(m);
		if (bad) {
			free(out);
			out = NULL;
		}
	}
	free(head);
	free(rec);
	return out;
}

/* The envelope of docs/CERTIFICATE.md. 0 signed, 1 written unsigned (no key), -1 error.
 * sig (may be NULL) gets the base64 signature, "" when unsigned. */
int cert_envelope(FILE *o, const char *kind, const char *extra_json, char sig[89])
{
	size_t len;
	uint8_t s[64];
	char sb[89] = "", pb[45];
	char *pl = payload(kind, extra_json, &len), *b = pl ? malloc(len / 3 * 4 + 5) : NULL;
	if (!b || b64_encode((uint8_t *)pl, len, b, len / 3 * 4 + 5) < 0) {
		free(pl);
		free(b);
		return -1;
	}
	if (g_key) {
		crypto_ed25519_sign(s, g_sk, (uint8_t *)pl, len);
		b64_encode(s, 64, sb, sizeof sb);
		b64_encode(g_pk, 32, pb, sizeof pb);
	}
	fprintf(o, "{\n  \"format\": \"%s\",\n  \"kind\": ", g_key ? "bedrock-signed-1" : "bedrock-unsigned-1");
	json_str(o, kind);
	fprintf(o, ",\n  \"payload\": \"%s\",\n", b);
	if (g_key) {
		fprintf(o, "  \"signature\": \"%s\",\n  \"public_key\": \"%s\",\n  \"key_id\": \"%s\"\n}\n", sb, pb, g_kid);
	} else {
		fputs("  \"note\": ", o);
		json_str(o, g_note);
		fputs("\n}\n", o);
	}
	if (sig)
		snprintf(sig, 89, "%s", sb);
	free(pl);
	free(b);
	return !g_key;
}

/* Value of a top-level string member. shortcut: plain search, enough for envelope
 * members whose values never hold quotes; use a JSON parser if they ever do. */
static char *field(const char *j, const char *name)
{
	char k[40];
	snprintf(k, sizeof k, "\"%s\"", name);
	const char *p = strstr(j, k), *e;
	if (!p)
		return NULL;
	for (p += strlen(k); isspace((unsigned char)*p); p++)
		;
	if (*p++ != ':')
		return NULL;
	while (isspace((unsigned char)*p))
		p++;
	if (*p++ != '"' || !(e = strchr(p, '"')) || memchr(p, '\\', e - p))
		return NULL;
	return strndup(p, e - p);
}

/* 1: the signature matches and the key ID belongs to the key; 0: the record was changed;
 * -1: not a signed Bedrock record. key_id gets the key's ID when one could be read. */
int cert_verify(const char *json, char key_id_out[17])
{
	char *fmt = field(json, "format"), *kind = field(json, "kind"), *pl = field(json, "payload");
	char *sg = field(json, "signature"), *pk = field(json, "public_key"), *kid = field(json, "key_id");
	uint8_t sig[64], pub[32], *msg = NULL;
	int r = -1, n;
	key_id_out[0] = 0;
	if (fmt && !strcmp(fmt, "bedrock-signed-1") && kind && pl && sg && pk && kid &&
	    b64_decode(sg, sig, 64) == 64 && b64_decode(pk, pub, 32) == 32 &&
	    (msg = malloc(strlen(pl) / 4 * 3 + 1)) && (n = b64_decode(pl, msg, strlen(pl) / 4 * 3)) >= 0) {
		char want[80];
		key_id(pub, key_id_out);
		msg[n] = 0;
		/* kind is outside the signature: it must repeat the payload's own, which comes first */
		snprintf(want, sizeof want, "{\"kind\":\"%s\"", kind);
		r = !strcmp(kid, key_id_out) && !strncmp((char *)msg, want, strlen(want)) &&
		    !crypto_ed25519_check(sig, pub, msg, n);
	}
	free(fmt), free(kind), free(pl), free(sg), free(pk), free(kid), free(msg);
	return r;
}

/* ---- printable pages ---- */

static const char CSS[] =
	"@page{size:A4;margin:16mm 18mm}"
	"body{font:10pt/1.45 'Helvetica Neue',Helvetica,Arial,sans-serif;color:#000;background:#fff;"
	"max-width:174mm;margin:0 auto;padding:12mm 4mm}"
	"h1,h2{font-family:Georgia,'Times New Roman',serif;font-weight:normal;margin:0}"
	"h1{font-size:22pt;line-height:1.2}h2{font-size:13pt;margin:9mm 0 2mm;padding-bottom:1mm;border-bottom:.6pt solid #000}"
	".sub{margin:1.5mm 0 0;color:#333}.top{font-size:8.5pt;letter-spacing:.08em;text-transform:uppercase;margin:0 0 6mm}"
	"table{width:100%;border-collapse:collapse}td,th{text-align:left;vertical-align:top;padding:1.3mm 3mm 1.3mm 0;"
	"border-bottom:.3pt solid #bbb;font-weight:normal}th{width:34%;color:#333}tr{page-break-inside:avoid}"
	".v{white-space:nowrap;width:1%}.bad{font-weight:bold}"
	".mono{font-family:'Courier New',Courier,monospace;font-size:9pt;word-break:break-all}"
	"p{margin:2mm 0}.foot{margin-top:9mm;font-size:9pt;color:#333}"
	"@media print{body{padding:0}}";

static void html_esc(FILE *o, const char *s)
{
	for (; *s; s++)
		switch (*s) {
		case '<': fputs("&lt;", o); break;
		case '>': fputs("&gt;", o); break;
		case '&': fputs("&amp;", o); break;
		case '"': fputs("&quot;", o); break;
		default: fputc(*s, o);
		}
}

static void html_head(FILE *o, const char *title)
{
	fputs("<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">"
	      "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>", o);
	html_esc(o, title);
	fprintf(o, "</title><style>%s</style></head><body>\n<p class=\"top\">%s</p>\n<h1>", CSS, HEADER);
	html_esc(o, title);
	fputs("</h1>\n", o);
}

/* One key/value row; "disk_size_bytes" shows as "Disk size" with GB added. */
static void html_row(FILE *o, const char *key, const char *val)
{
	char k[64];
	size_t n = strlen(key);
	int bytes = n > 6 && !strcmp(key + n - 6, "_bytes");
	if (!val || !*val)
		return;
	snprintf(k, sizeof k, "%.*s", (int)(bytes ? n - 6 : n), key);
	for (char *c = k; *c; c++)
		*c = *c == '_' ? ' ' : *c;
	k[0] = toupper((unsigned char)k[0]);
	fputs("<tr><th>", o);
	html_esc(o, k);
	fputs("</th><td>", o);
	html_esc(o, val);
	if (bytes)
		fprintf(o, " bytes (%.1f GB)", strtod(val, NULL) / 1e9);
	fputs("</td></tr>\n", o);
}

/* JSON string body at p (just after the quote) into out; returns just after its end. */
static const char *jstr(const char *p, char *out, size_t n)
{
	size_t k = 0;
	for (; *p && *p != '"'; p++) {
		char c = *p;
		if (c == '\\' && p[1]) {
			c = *++p;
			if (c == 'n')
				c = ' ';
			else if (c == 'u')
				for (c = '?'; isxdigit((unsigned char)p[1]); p++)
					;
		}
		if (k + 1 < n)
			out[k++] = c;
	}
	out[k] = 0;
	return *p ? p + 1 : p;
}

/* Next member of an object: key into k and a scalar into v (1), or p left on a nested '{' (2); 0 at the end. */
static int jmember(const char **pp, char *k, size_t kn, char *v, size_t vn)
{
	const char *p = *pp;
	size_t i = 0;
	while (isspace((unsigned char)*p) || *p == ',')
		p++;
	if (*p != '"')
		return 0;
	for (p = jstr(p + 1, k, kn); isspace((unsigned char)*p); p++)
		;
	if (*p++ != ':')
		return 0;
	while (isspace((unsigned char)*p))
		p++;
	if (*p == '{') {
		*pp = p;
		return 2;
	}
	if (*p == '"') {
		*pp = jstr(p + 1, v, vn);
		return 1;
	}
	for (; *p && !strchr(",}", *p) && !isspace((unsigned char)*p); p++)
		if (i + 1 < vn)
			v[i++] = *p;
	v[i] = 0;
	*pp = p;
	return 1;
}

/* Each object in the extras as its own section ("media", "sanitization").
 * shortcut: reads the flat objects our callers build; top-level scalars and deeper nesting are not shown. */
static void html_extra(FILE *o, const char *j)
{
	char k[64], k2[64], v[512];
	const char *p = j ? strchr(j, '{') : NULL;
	int r;
	if (!p)
		return;
	for (p++; (r = jmember(&p, k, sizeof k, v, sizeof v)) > 0;) {
		if (r == 1)
			continue;
		k[0] = toupper((unsigned char)k[0]);
		fputs("<h2>", o);
		html_esc(o, k);
		fputs("</h2>\n<table>\n", o);
		for (p++; jmember(&p, k2, sizeof k2, v, sizeof v) == 1;)
			html_row(o, k2, v);
		fputs("</table>\n", o);
		if (!(p = strchr(p, '}')))
			return;
		p++;
	}
}

/* Who ran the station, from bedrock.cfg or typed in: the same values the payload carries. */
static void html_station(FILE *o)
{
	char f[48];
	int rows = 0;
	for (size_t i = 0; i < sizeof station / sizeof *station; i++) {
		snprintf(f, sizeof f, "station.%s", station[i]);
		if (fact_get(f) && *fact_get(f) && !rows++)
			fputs("<h2>Station</h2>\n<table>\n", o);
		html_row(o, station[i], fact_get(f));
	}
	if (rows)
		fputs("</table>\n", o);
}

static const struct { const char *label, *key; } machine[] = {
	{"Manufacturer", "system.manufacturer"}, {"Model", "system.product"}, {"Serial number", "system.serial"},
	{"Board", "board.product"}, {"BIOS", "bios.version"}, {"Processor", "cpu.brand"},
	{"Memory (MB)", "memory.installed_mb"},
};

/* The printable twin of a record: what it says, and how to check it. */
void cert_html(FILE *o, const char *kind, const char *extra_json, const char *sig, const char *file)
{
	char ts[64];
	time_t t = time(NULL);
	int c[4] = {0};
	strftime(ts, sizeof ts, "%Y-%m-%d %H:%M UTC", gmtime(&t));
	html_head(o, !strcmp(kind, "sanitization") ? "Data sanitization certificate" :
		     !strcmp(kind, "health") ? "Hardware health record" : kind);
	fprintf(o, "<p class=\"sub\">Record kind: ");
	html_esc(o, kind);
	fprintf(o, " &middot; written %s by Bedrock %s</p>\n", ts, VERSION);
	html_station(o);
	html_extra(o, extra_json);
	fputs("<h2>Machine</h2>\n<table>\n", o);
	for (size_t i = 0; i < sizeof machine / sizeof *machine; i++)
		if (fact_get(machine[i].key) && *fact_get(machine[i].key)) {
			fputs("<tr><th>", o);
			html_esc(o, machine[i].label);
			fputs("</th><td>", o);
			html_esc(o, fact_get(machine[i].key));
			fputs("</td></tr>\n", o);
		}
	fputs("</table>\n<h2>Results</h2>\n<table>\n", o);
	for (int i = 0; i < g_nres; i++) {
		c[g_res[i].v]++;
		fprintf(o, "<tr><td>");
		html_esc(o, g_res[i].test);
		fprintf(o, "</td><td class=\"v%s\">%s</td><td>", g_res[i].v == V_FAIL ? " bad" : "", verdict_name(g_res[i].v));
		html_esc(o, g_res[i].detail);
		fputs("</td></tr>\n", o);
	}
	fprintf(o, "</table>\n<p>%d pass, %d fail, %d inconclusive, %d not present.</p>\n<h2>Signature</h2>\n<table>\n",
		c[V_PASS], c[V_FAIL], c[V_INCONCLUSIVE], c[V_ABSENT]);
	html_row(o, "record_file", file);
	if (sig && *sig) {
		fprintf(o, "<tr><th>Key ID</th><td class=\"mono\">%s</td></tr>\n"
			   "<tr><th>Signature (Ed25519)</th><td class=\"mono\">%.16s&hellip;%s</td></tr>\n</table>\n",
			g_kid, sig, sig + strlen(sig) - 8);
		fputs("<p class=\"foot\">To check this record, open <b>erdem6.com/verify</b> and drop the record file "
		      "on it. The check runs in the browser and nothing is uploaded. A valid signature proves the record "
		      "was not changed after this key signed it; to know whose key it is, compare the key ID with the one "
		      "the issuing organisation publishes.</p>\n", o);
	} else {
		fputs("<tr><th>Signature</th><td class=\"bad\">Not signed: ", o);
		html_esc(o, g_note);
		fputs("</td></tr>\n</table>\n<p class=\"foot\">This record is not signed, so nobody can check that it "
		      "was not changed.</p>\n", o);
	}
	fputs("</body></html>\n", o);
}

static int close_synced(FILE *f)
{
	int r = fflush(f) | fsync(fileno(f)); /* the user pulls the stick right after */
	sync(); /* FAT keeps a new file's directory entry in a separate block */
	return fclose(f) | r;
}

/* <stick>/bedrock/<dir>/<stem>.json and its printable .html. 0 signed, 1 saved unsigned
 * (no usable key), -1 not saved (no writable stick partition, or a write failed). */
int signed_save(const char *kind, const char *extra_json, const char *dir, const char *stem, char *path, size_t n)
{
	char mp[256], d[300], s[96], hp[512], sig[89], msg[8];
	snprintf(s, sizeof s, "%s", stem);
	for (char *c = s; *c; c++)
		if (!isalnum((unsigned char)*c) && *c != '-' && *c != '.')
			*c = '-';
	if (report_mount(mp))
		return -1;
	cert_key(msg, sizeof msg); /* failure leaves the record unsigned, with the reason in it */
	snprintf(d, sizeof d, "%s/bedrock", mp);
	mkdir(d, 0755);
	snprintf(d, sizeof d, "%s/bedrock/%s", mp, dir);
	mkdir(d, 0755);
	if (snprintf(path, n, "%s/%s.json", d, s) >= (int)n || snprintf(hp, sizeof hp, "%s/%s.html", d, s) >= (int)sizeof hp)
		return -1;
	FILE *f = fopen(path, "w");
	if (!f)
		return -1;
	int r = cert_envelope(f, kind, extra_json, sig);
	if (close_synced(f) || r < 0 || !(f = fopen(hp, "w")))
		return -1;
	snprintf(hp, sizeof hp, "%s.json", s);
	cert_html(f, kind, extra_json, sig, hp);
	return close_synced(f) ? -1 : r;
}

/* ---- resale sheet ---- */

static const struct result *result_get(const char *test)
{
	for (int i = g_nres - 1; i >= 0; i--)
		if (!strcmp(g_res[i].test, test))
			return &g_res[i];
	return NULL;
}

static void spec_row(FILE *o, const char *label, const char *val)
{
	if (!val || !*val)
		return;
	fputs("<tr><th>", o);
	html_esc(o, label);
	fputs("</th><td>", o);
	html_esc(o, val);
	fputs("</td></tr>\n", o);
}

static void sheet_storage(FILE *o)
{
	char key[32], v[160], row[512], tst[64];
	for (int i = 0; i < 32; i++) {
		snprintf(key, sizeof key, "disk.%d", i);
		if (!fact_get(key))
			break;
		snprintf(v, sizeof v, "%s", fact_get(key));
		char *p = v, *name = strsep(&p, "|"), *model = strsep(&p, "|");
		char *serial = strsep(&p, "|"), *bytes = strsep(&p, "|"), *kind = p;
		(void)serial;
		int len = snprintf(row, sizeof row, "%s, %.0f GB%s%s", model && *model ? model : name,
			bytes ? strtod(bytes, NULL) / 1e9 : 0.0, kind ? ", " : "", kind ? kind : "");
		snprintf(tst, sizeof tst, "SMART %s", name);
		const struct result *s = result_get(tst);
		snprintf(tst, sizeof tst, "Surface %s", name);
		const struct result *f = result_get(tst);
		if (len > 0 && len < (int)sizeof row)
			snprintf(row + len, sizeof row - len, "; health (SMART): %s; surface scan: %s",
				s ? verdict_name(s->v) : "not checked", f ? verdict_name(f->v) : "not checked");
		spec_row(o, i ? "" : "Storage", row);
	}
}

/* One printable page for selling the machine, plus the signed health record it summarises.
 * Returns as signed_save; path gets the sheet's path. */
int sheet_save(char *path, size_t n)
{
	char stem[96], rec[400], date[32];
	const char *s;
	time_t t = time(NULL);
	strftime(date, sizeof date, "%Y%m%d-%H%M%S", gmtime(&t));
	snprintf(stem, sizeof stem, "%s-%s", date, (s = fact_get("system.product")) && *s ? s : "machine");
	int r = signed_save("health", NULL, "records", stem, rec, sizeof rec);
	if (r < 0 || snprintf(path, n, "%.*s-sheet.html", (int)strlen(rec) - 5, rec) >= (int)n)
		return -1;
	FILE *o = fopen(path, "w");
	if (!o)
		return -1;
	sheet_html(o, strrchr(rec, '/') + 1, r ? NULL : g_kid);
	return close_synced(o) ? -1 : r;
}

void sheet_html(FILE *o, const char *record_file, const char *kid)
{
	char v[400], key[32], ts[64];
	const char *s, *m = fact_get("system.manufacturer"), *p = fact_get("system.product");
	int c[4] = {0};
	time_t t = time(NULL);
	strftime(ts, sizeof ts, "%d %B %Y", gmtime(&t));
	snprintf(v, sizeof v, "%s%s%s", m ? m : "", m && p ? " " : "", p ? p : "");
	html_head(o, v[0] ? v : "Computer");
	fprintf(o, "<p class=\"sub\">Specification and test results, %s</p>\n<h2>Specification</h2>\n<table>\n", ts);
	spec_row(o, "Serial number", fact_get("system.serial"));
	if ((s = fact_get("cpu.brand")))
		snprintf(v, sizeof v, "%s%s%s%s", s, fact_get("cpu.threads") ? ", " : "",
			fact_get("cpu.threads") ? fact_get("cpu.threads") : "", fact_get("cpu.threads") ? " threads" : "");
	spec_row(o, "Processor", s ? v : NULL);
	if ((s = fact_get("memory.installed_mb")) || (s = fact_get("memory.visible_mb"))) {
		long long mb = strtoll(s, NULL, 10);
		snprintf(v, sizeof v, mb < 1024 ? "%.0f MB" : mb % 1024 ? "%.1f GB" : "%.0f GB", mb < 1024 ? mb : mb / 1024.0);
	}
	spec_row(o, "Memory", s ? v : NULL);
	sheet_storage(o);
	for (int i = 0; i < 8; i++) {
		snprintf(key, sizeof key, "gpu.%d", i);
		spec_row(o, i ? "" : "Graphics", fact_get(key));
	}
	if ((s = fact_get("battery.0.health_pct"))) {
		const char *cy = fact_get("battery.0.cycles");
		snprintf(v, sizeof v, "%s%% of design capacity%s%s%s", s, cy ? ", " : "", cy ? cy : "", cy ? " charge cycles" : "");
		spec_row(o, "Battery", v);
	}
	spec_row(o, "BIOS", fact_get("bios.version"));
	fputs("</table>\n<h2>Test results</h2>\n", o);
	for (int i = 0; i < g_nres; i++)
		c[g_res[i].v]++;
	if (!g_nres)
		fputs("<p>No tests were run.</p>\n", o);
	else if (!c[V_FAIL] && !c[V_INCONCLUSIVE])
		fprintf(o, "<p>All %d checks passed%s.</p>\n", c[V_PASS], c[V_ABSENT] ? "; parts not fitted are listed below" : "");
	else
		fprintf(o, "<p>%d checks passed, %d failed, %d could not be measured.</p>\n", c[V_PASS], c[V_FAIL], c[V_INCONCLUSIVE]);
	if (g_nres > c[V_PASS]) {
		fputs("<table>\n", o);
		for (int i = 0; i < g_nres; i++)
			if (g_res[i].v != V_PASS) {
				fputs("<tr><td>", o);
				html_esc(o, g_res[i].test);
				fprintf(o, "</td><td class=\"v%s\">%s</td><td>", g_res[i].v == V_FAIL ? " bad" : "", verdict_name(g_res[i].v));
				html_esc(o, g_res[i].detail);
				fputs("</td></tr>\n", o);
			}
		fputs("</table>\n", o);
	}
	strftime(ts, sizeof ts, "%Y-%m-%d %H:%M UTC", gmtime(&t));
	fprintf(o, "<p class=\"foot\">Tested with Bedrock %s on %s. Every measurement behind this page is in the "
		   "health record <span class=\"mono\">", VERSION, ts);
	html_esc(o, record_file);
	if (kid)
		fprintf(o, "</span>, signed with key ID <span class=\"mono\">%s</span>. Anyone can check that the record "
			   "is unchanged at <b>erdem6.com/verify</b>.</p>\n", kid);
	else
		fputs("</span>, which is not signed.</p>\n", o);
	fputs("</body></html>\n", o);
}

/* ---- station and menu ---- */

/* Station run: sign a health record of the checks run so far and write the resale sheet. */
void cert_save_health(FILE *o)
{
	char msg[400], path[400];
	int k = cert_key(msg, sizeof msg), r = sheet_save(path, sizeof path);
	fprintf(o, "Signing key: %s\n", msg);
	if (r < 0)
		fprintf(o, "No writable USB stick partition: the health record and resale sheet were not saved.\n");
	else
		fprintf(o, "Saved %s\nand its health record (.json, %s) next to it.\n", path, r || k ? "NOT signed" : "signed");
}

static int is_json(const struct dirent *e)
{
	size_t n = strlen(e->d_name);
	return n > 5 && !strcmp(e->d_name + n - 5, ".json");
}

/* The newest records in each folder, each checked against its signature. */
static void cert_list(FILE *o)
{
	static const char *dirs[] = {"certificates", "records"};
	static const char *state[] = {"unsigned", "CHANGED", "valid"};
	char mp[256], p[600], id[17];
	char *buf = malloc(1 << 20);
	if (!buf || report_mount(mp)) {
		free(buf);
		return;
	}
	for (int i = 0; i < 2; i++) {
		struct dirent **e;
		snprintf(p, sizeof p, "%s/bedrock/%s", mp, dirs[i]);
		int n = scandir(p, &e, is_json, alphasort);
		fprintf(o, "\n%s on the stick: %d%s\n", i ? "Health records" : "Wipe certificates", n > 0 ? n : 0,
			n > 6 ? " (newest 6)" : "");
		for (int j = 0; j < n; j++) {
			snprintf(p, sizeof p, "%s/bedrock/%s/%s", mp, dirs[i], e[j]->d_name);
			if (j >= n - 6) {
				id[0] = 0;
				int v = read_file(p, buf, 1 << 20) > 0 ? cert_verify(buf, id) : -1;
				fprintf(o, "  %-9s %-44.44s %s\n", state[v + 1], e[j]->d_name, id);
			}
			free(e[j]);
		}
		if (n > 0)
			free(e);
	}
	free(buf);
}

static int ask(const char *prompt, char *buf, size_t n)
{
	printf("%s", prompt);
	fflush(stdout);
	if (!fgets(buf, n, stdin))
		return -1;
	buf[strcspn(buf, "\r\n")] = 0;
	return 0;
}

int cert_run(void)
{
	char msg[400], in[16], *junk = NULL;
	size_t len;
	if (!cert_key(msg, sizeof msg))
		printf("Signing key: %s\nAnyone holding this stick can sign with it: keep the stick safe.\n", msg);
	else
		printf("No signing key: %s.\nRecords can be saved, but not signed.\n", msg);
	cert_list(stdout);
	if (ask("\n  s  run quick checks, then save a signed health record and a resale sheet\n"
		"Choice (Enter to return): ", in, sizeof in) || in[0] != 's')
		return 0;
	printf("Running quick checks (about a minute)...\n");
	fflush(stdout);
	FILE *m = open_memstream(&junk, &len);
	if (!m)
		return 1;
	report_run(m, "Health record");
	fclose(m);
	free(junk);
	cert_save_health(stdout);
	return 1;
}
