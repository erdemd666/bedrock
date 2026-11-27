/* Fleet mode: send the machine record to a fleet server (fleet/ in this repo).
 * Plain HTTP on purpose: the server sits on the same LAN, and a TLS library would
 * cost more RAM and image size than a Pentium III can spare. Do not point this at
 * a server across the internet. */
#include "bedrock.h"

#include <ctype.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* "http://host[:port][/prefix]". Only hostnames and IPv4 literals: the build has no IPv6. */
int fleet_url_parse(const char *url, struct fleet_url *u)
{
	if (strncmp(url, "http://", 7))
		return -1;
	const char *h = url + 7;
	size_t hl = strcspn(h, "/:"), pl = 0;
	const char *e = h + hl;
	if (!hl || hl >= sizeof u->host)
		return -1;
	for (size_t i = 0; i < hl; i++)
		if (!isalnum((unsigned char)h[i]) && h[i] != '.' && h[i] != '-')
			return -1;
	memcpy(u->host, h, hl);
	u->host[hl] = 0;
	strcpy(u->port, "80");
	if (*e == ':') {
		e++;
		pl = strspn(e, "0123456789");
		if (!pl || pl >= sizeof u->port || atoi(e) < 1 || atoi(e) > 65535)
			return -1;
		snprintf(u->port, sizeof u->port, "%d", atoi(e));
		e += pl;
	}
	if (*e && *e != '/')
		return -1;
	size_t n = strlen(e);
	while (n && e[n - 1] == '/')
		n--;
	if (n >= sizeof u->path)
		return -1;
	memcpy(u->path, e, n);
	u->path[n] = 0;
	return 0;
}

/* Request line and headers of the POST; the caller sends the body after it. 0 if buf is too small. */
size_t fleet_request(char *buf, size_t n, const struct fleet_url *u, size_t len)
{
	int dflt = !strcmp(u->port, "80");
	int r = snprintf(buf, n,
		"POST %s/api/records HTTP/1.1\r\nHost: %s%s%s\r\nUser-Agent: bedrock/" VERSION "\r\n"
		"Content-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
		u->path, u->host, dflt ? "" : ":", dflt ? "" : u->port, len);
	return r > 0 && (size_t)r < n ? (size_t)r : 0;
}

/* Status code of "HTTP/1.x NNN ...", -1 if the bytes are not a status line (RFC 9112 section 4). */
int fleet_status(const char *r, size_t n)
{
	if (n < 12 || strncmp(r, "HTTP/1.", 7) || (r[7] != '0' && r[7] != '1') || r[8] != ' ')
		return -1;
	for (int i = 9; i < 12; i++)
		if (!isdigit((unsigned char)r[i]))
			return -1;
	return (r[9] - '0') * 100 + (r[10] - '0') * 10 + (r[11] - '0');
}

/* Value of "key=value" (kernel command line) or "key = value" lines (bedrock.cfg);
 * lines starting with # are comments. The key must start a word. */
int fleet_find(const char *text, const char *key, char *out, size_t n)
{
	size_t kl = strlen(key);
	for (const char *l = text; *l;) {
		const char *eol = l + strcspn(l, "\n");
		for (const char *k = l;; k += strcspn(k, " \t\r\n")) {
			k += strspn(k, " \t\r");
			if (k >= eol || *k == '#')
				break;
			if (strncmp(k, key, kl))
				continue;
			const char *v = k + kl + strspn(k + kl, " \t");
			if (*v != '=')
				continue;
			v += 1 + strspn(v + 1, " \t");
			if (*v == '"')
				v++;
			size_t len = strcspn(v, " \t\r\n\"");
			if (!len || len >= n)
				return -1;
			memcpy(out, v, len);
			out[len] = 0;
			return 0;
		}
		l = *eol ? eol + 1 : eol;
	}
	return -1;
}

/* The server named on the boot line (bedrock.fleet=URL) or in bedrock.cfg on the stick (fleet_url=URL). */
const char *fleet_url(void)
{
	static char url[256];
	char text[4096], mp[256], path[300];
	if (read_file("/proc/cmdline", text, sizeof text) > 0 && !fleet_find(text, "bedrock.fleet", url, sizeof url))
		return url;
	if (report_mount(mp) || snprintf(path, sizeof path, "%s/bedrock.cfg", mp) >= (int)sizeof path)
		return NULL;
	if (read_file(path, text, sizeof text) > 0 && !fleet_find(text, "fleet_url", url, sizeof url))
		return url;
	return NULL;
}

/* One attempt: the HTTP status, or -1 with the reason in err. */
static int post_once(const struct fleet_url *u, const char *json, size_t len, char *err, size_t n)
{
	char hdr[512], resp[64];
	struct addrinfo hint = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *ai;
	struct timeval tmo = {.tv_sec = 10};
	size_t hl = fleet_request(hdr, sizeof hdr, u, len), got = 0;
	if (getaddrinfo(u->host, u->port, &hint, &ai)) {
		snprintf(err, n, "cannot find %s", u->host);
		return -1;
	}
	int fd = socket(ai->ai_family, SOCK_STREAM, 0), st = -1;
	if (fd < 0) {
		snprintf(err, n, "no network");
		freeaddrinfo(ai);
		return -1;
	}
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tmo, sizeof tmo); /* also bounds connect() */
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof tmo);
	if (connect(fd, ai->ai_addr, ai->ai_addrlen)) {
		snprintf(err, n, "cannot reach %s:%s", u->host, u->port);
		goto out;
	}
	if (!hl || send(fd, hdr, hl, MSG_NOSIGNAL) != (ssize_t)hl) {
		snprintf(err, n, "connection lost");
		goto out;
	}
	for (size_t off = 0; off < len;) {
		ssize_t w = send(fd, json + off, len - off, MSG_NOSIGNAL);
		if (w <= 0) {
			snprintf(err, n, "connection lost");
			goto out;
		}
		off += w;
	}
	for (ssize_t r; got < 12 && (r = recv(fd, resp + got, sizeof resp - got, 0)) > 0;)
		got += r;
	st = fleet_status(resp, got);
	if (st < 0)
		snprintf(err, n, "no answer from %s", u->host);
out:
	close(fd);
	freeaddrinfo(ai);
	return st;
}

/* POST the JSON to <url>/api/records; 0 once the server accepted it. Network errors and
 * 5xx/408/429 are retried (the server may still be starting, or the switch port still
 * negotiating); other answers are final. */
int fleet_send(const char *url, const char *json, size_t len, char *err, size_t n)
{
	struct fleet_url u;
	if (fleet_url_parse(url, &u)) {
		snprintf(err, n, "bad fleet address %s (want http://host:port)", url);
		return -1;
	}
	for (int try = 0, wait = 2;; try++, wait *= 2) {
		int st = post_once(&u, json, len, err, n);
		if (st >= 200 && st < 300)
			return 0;
		if (st >= 0)
			snprintf(err, n, "server answered %d", st);
		if ((st >= 0 && st < 500 && st != 408 && st != 429) || try == 3)
			return -1;
		sleep(wait);
	}
}

/* Station/headless entry: send this run's record if a fleet server is set, and say so. */
void fleet_upload(FILE *o)
{
	char *buf = NULL, err[160];
	size_t len = 0;
	const char *url = fleet_url();
	if (!url)
		return;
	fact("fleet.url", "%s", url);
	FILE *m = open_memstream(&buf, &len);
	if (!m)
		return;
	record_json(m);
	fclose(m);
	if (fleet_send(url, buf, len, err, sizeof err))
		fprintf(o, "Fleet server: %s\n", err);
	else
		fputs("Sent to fleet server\n", o);
	free(buf);
}
