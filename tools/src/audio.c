/* Speakers and microphone through the ALSA PCM ioctls, no library: open up the
 * mixer (codecs come up muted), then play or record 16-bit PCM. */
#include "bedrock.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <sound/asound.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* RMS level in dB relative to full scale, LEVEL_FLOOR for digital silence. */
double level_dbfs(const int16_t *s, size_t n)
{
	double sum = 0;
	for (size_t i = 0; i < n; i++)
		sum += (double)s[i] * s[i];
	if (!n || sum == 0)
		return LEVEL_FLOOR;
	double db = 10 * log10(sum / n / (32768.0 * 32768.0));
	return db < LEVEL_FLOOR ? LEVEL_FLOOR : db;
}

/* Bar length for a level: -60 dBFS empty, 0 dBFS full. */
int level_cols(double db, int width)
{
	if (db <= -60)
		return 0;
	if (db >= 0)
		return width;
	return (int)((db + 60) / 60 * width + 0.5);
}

/* /proc/asound/pcm: "CC-DD: id : name : playback 1 : capture 1". Analog first. */
static int pcm_find(int capture, char *dev, size_t n, char *name, size_t nn)
{
	char buf[4096], line[256];
	if (read_file("/proc/asound/pcm", buf, sizeof buf) <= 0)
		return -1;
	for (int pass = 0; pass < 2; pass++)
		for (char *l = buf, *nl; l; l = nl ? nl + 1 : NULL) {
			int card, d;
			nl = strchr(l, '\n');
			snprintf(line, sizeof line, "%.*s", nl ? (int)(nl - l) : (int)strlen(l), l);
			if (sscanf(line, "%d-%d: %*[^:]: %79[^:]", &card, &d, name) != 3 ||
			    !strstr(line, capture ? ": capture" : ": playback"))
				continue;
			if (!pass && (strstr(line, "HDMI") || strstr(line, "DP")))
				continue;
			name[nn - 1] = 0;
			for (size_t k = strlen(name); k && name[k - 1] == ' ';)
				name[--k] = 0;
			snprintf(dev, n, "/dev/snd/pcmC%dD%d%c", card, d, capture ? 'c' : 'p');
			return card;
		}
	return -1;
}

static int pci_audio(void)
{
	char p[300], b[32];
	int found = 0;
	DIR *d = opendir("/sys/bus/pci/devices");
	struct dirent *e;
	while (d && (e = readdir(d)) && !found) {
		snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/class", e->d_name);
		unsigned c = read_file(p, b, sizeof b) > 0 ? strtoul(b, NULL, 16) >> 8 : 0;
		found = c == 0x0401 || c == 0x0403; /* multimedia audio, HD audio */
	}
	if (d)
		closedir(d);
	return found;
}

static enum verdict no_device(const char *test)
{
	if (pci_audio()) {
		verdict(stdout, V_INCONCLUSIVE, test, "sound controller found, but no usable device (driver or firmware missing)");
		return V_INCONCLUSIVE;
	}
	verdict(stdout, V_ABSENT, test, "no sound device");
	return V_ABSENT;
}

static int mixer_wanted(const char *nm, int capture)
{
	static const char *out[] = {"Master", "Speaker", "Headphone", "PCM", "Front"};
	if (strstr(nm, "Beep"))
		return 0;
	if (capture)
		return strstr(nm, "Capture Switch") || strstr(nm, "Capture Volume");
	if (!strstr(nm, "Playback Switch") && !strstr(nm, "Playback Volume"))
		return 0; /* also keeps "Mic Playback" off: it would feed back */
	for (size_t i = 0; i < sizeof out / sizeof *out; i++)
		if (!strncmp(nm, out[i], strlen(out[i])))
			return 1;
	return 0;
}

/* Switches on, volumes at 3/4: ALSA drivers start muted and nothing here runs alsactl. */
static void mixer_open_up(int card, int capture)
{
	char p[32];
	snprintf(p, sizeof p, "/dev/snd/controlC%d", card);
	int fd = open(p, O_RDWR | O_CLOEXEC);
	struct snd_ctl_elem_list l = {0};
	if (fd < 0)
		return;
	if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &l) || !l.count) {
		close(fd);
		return;
	}
	struct snd_ctl_elem_id *ids = calloc(l.count, sizeof *ids);
	l.space = l.count;
	l.pids = ids;
	for (unsigned i = 0; ids && !ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &l) && i < l.used; i++) {
		struct snd_ctl_elem_info inf = {.id = ids[i]};
		struct snd_ctl_elem_value v = {.id = ids[i]};
		if (!mixer_wanted((const char *)ids[i].name, capture) || ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &inf) ||
		    (inf.type != SNDRV_CTL_ELEM_TYPE_BOOLEAN && inf.type != SNDRV_CTL_ELEM_TYPE_INTEGER))
			continue;
		long set = inf.type == SNDRV_CTL_ELEM_TYPE_BOOLEAN ? 1
			 : inf.value.integer.min + (inf.value.integer.max - inf.value.integer.min) * 3 / 4;
		for (unsigned c = 0; c < inf.count && c < 128; c++)
			v.value.integer.value[c] = set;
		ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &v);
	}
	free(ids);
	close(fd);
}

struct pcm { int fd; unsigned rate, ch; };

static void hw_mask(struct snd_pcm_hw_params *h, int par, unsigned v)
{
	struct snd_mask *m = &h->masks[par - SNDRV_PCM_HW_PARAM_FIRST_MASK];
	memset(m->bits, 0, sizeof m->bits);
	m->bits[v / 32] = 1u << (v % 32);
}

static struct snd_interval *hw_iv(struct snd_pcm_hw_params *h, int par)
{
	return &h->intervals[par - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
}

static void hw_range(struct snd_pcm_hw_params *h, int par, unsigned lo, unsigned hi)
{
	struct snd_interval *i = hw_iv(h, par);
	i->min = lo;
	i->max = hi;
	i->integer = 1;
}

/* S16 interleaved at 48 or 44.1 kHz. HW_PARAMS refines the rest and picks the
 * smallest period and largest buffer that fit. */
static int pcm_open(const char *dev, unsigned chmin, unsigned chmax, struct pcm *p)
{
	static const unsigned rates[] = {48000, 44100};
	p->fd = open(dev, O_RDWR | O_CLOEXEC);
	if (p->fd < 0)
		return -1;
	for (int r = 0; r < 2; r++) {
		struct snd_pcm_hw_params h;
		memset(&h, 0, sizeof h);
		memset(h.masks, 0xff, sizeof h.masks);
		for (int i = 0; i <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; i++)
			h.intervals[i].max = UINT_MAX;
		h.rmask = ~0u;
		h.info = ~0u;
		hw_mask(&h, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
		hw_mask(&h, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
		hw_mask(&h, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
		hw_range(&h, SNDRV_PCM_HW_PARAM_CHANNELS, chmin, chmax);
		hw_range(&h, SNDRV_PCM_HW_PARAM_RATE, rates[r], rates[r]);
		hw_range(&h, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, 1024, 8192);
		if (!ioctl(p->fd, SNDRV_PCM_IOCTL_HW_PARAMS, &h)) {
			p->rate = rates[r];
			p->ch = hw_iv(&h, SNDRV_PCM_HW_PARAM_CHANNELS)->min;
			if (!ioctl(p->fd, SNDRV_PCM_IOCTL_PREPARE))
				return 0;
			break;
		}
	}
	close(p->fd);
	return -1;
}

static void on_alarm(int s)
{
	(void)s;
}

/* A sound card that never moves its pointer would block forever: without
 * SA_RESTART the alarm makes the blocked ioctl return EINTR. */
static void watchdog(unsigned secs)
{
	struct sigaction sa = {0};
	sa.sa_handler = on_alarm;
	sigaction(SIGALRM, &sa, NULL);
	alarm(secs);
}

/* 1 kHz at -12 dBFS on one channel, 10 ms fades against clicks. */
static int play(struct pcm *p, unsigned chan, double secs)
{
	unsigned frames = p->rate * secs, ramp = p->rate / 100;
	int16_t *b = calloc((size_t)frames * p->ch, sizeof *b);
	int r = -1;
	if (!b)
		return -1;
	for (unsigned i = 0; i < frames; i++) {
		unsigned edge = i < frames - i ? i : frames - i;
		b[(size_t)i * p->ch + chan] = 8192 * (edge < ramp ? (double)edge / ramp : 1) * sin(2 * M_PI * 1000 * i / p->rate);
	}
	watchdog(secs + 3);
	for (unsigned done = 0; done < frames;) {
		struct snd_xferi x = {.buf = b + (size_t)done * p->ch, .frames = frames - done};
		if (ioctl(p->fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x)) {
			if (errno == EPIPE && !ioctl(p->fd, SNDRV_PCM_IOCTL_PREPARE))
				continue; /* underrun */
			goto out;
		}
		done += x.result;
	}
	r = ioctl(p->fd, SNDRV_PCM_IOCTL_DRAIN);
out:
	alarm(0);
	free(b);
	return r | ioctl(p->fd, SNDRV_PCM_IOCTL_PREPARE);
}

enum verdict speaker_test(void)
{
	char dev[48], name[80];
	struct pcm p;
	int card = pcm_find(0, dev, sizeof dev, name, sizeof name), c;
	if (card < 0)
		return no_device("Speakers");
	mixer_open_up(card, 0);
	if (pcm_open(dev, 2, 8, &p)) {
		verdict(stdout, V_INCONCLUSIVE, "Speakers", "%s refuses 16-bit stereo at 44.1/48 kHz", name);
		return V_INCONCLUSIVE;
	}
	printf("Output: %s. Unplug headphones: they mute the speakers.\n", name);
	do {
		printf("\nLeft...  ");
		fflush(stdout);
		int r = play(&p, 0, 1.2);
		printf("Right...");
		fflush(stdout);
		if (r || play(&p, 1, 1.2)) {
			close(p.fd);
			verdict(stdout, V_INCONCLUSIVE, "Speakers", "%s: playback did not run (%s)", name, strerror(errno));
			return V_INCONCLUSIVE;
		}
		c = ask_char("\n\nHeard the tone on the left, then on the right? [y/n, Enter = again] ");
	} while (c == '\n');
	close(p.fd);
	enum verdict v = c == 'y' ? V_PASS : c == 'n' ? V_FAIL : V_INCONCLUSIVE;
	verdict(stdout, v, "Speakers", "%s: %s", name, v == V_PASS ? "operator heard left and right"
						    : v == V_FAIL ? "operator did not hear both sides" : "no answer");
	return v;
}

enum verdict mic_test(void)
{
	static const char bar[] = "##################################################";
	char dev[48], name[80];
	struct pcm p;
	int card = pcm_find(1, dev, sizeof dev, name, sizeof name), got = 0;
	if (card < 0)
		return no_device("Microphone");
	mixer_open_up(card, 1);
	if (pcm_open(dev, 1, 2, &p)) {
		verdict(stdout, V_INCONCLUSIVE, "Microphone", "%s refuses 16-bit capture at 44.1/48 kHz", name);
		return V_INCONCLUSIVE;
	}
	unsigned chunk = p.rate / 10;
	int16_t *b = malloc((size_t)chunk * p.ch * sizeof *b);
	double lo = 0, hi = LEVEL_FLOOR;
	printf("Input: %s. Speak or tap near the microphone now (3 s).\n\n", name);
	for (int i = 0; b && i < 30; i++) {
		struct snd_xferi x = {.buf = b, .frames = chunk};
		watchdog(2);
		int r = ioctl(p.fd, SNDRV_PCM_IOCTL_READI_FRAMES, &x);
		alarm(0);
		if (r) {
			if (errno == EPIPE && !ioctl(p.fd, SNDRV_PCM_IOCTL_PREPARE))
				continue; /* overrun */
			break;
		}
		double db = level_dbfs(b, (size_t)x.result * p.ch);
		lo = fmin(lo, db);
		hi = fmax(hi, db);
		got++;
		int w = level_cols(db, 50);
		printf("\r  [\033[32m%.*s\033[0m%*s] %4.0f dBFS ", w, bar, 50 - w, "", db);
		fflush(stdout);
	}
	free(b);
	close(p.fd);
	if (!got) {
		verdict(stdout, V_INCONCLUSIVE, "Microphone", "%s: no sound data arrived", name);
		return V_INCONCLUSIVE;
	}
	int c = ask_char("\n\nDid the bar move when you spoke? [y/n] ");
	int moved = hi >= -50 && hi - lo >= 10;
	enum verdict v = c == 'n' ? V_FAIL : c == 'y' && moved ? V_PASS : V_INCONCLUSIVE;
	verdict(stdout, v, "Microphone", "%s: %s; level %.0f to %.0f dBFS", name,
		c == 'n' ? "operator saw no response" : c != 'y' ? "no answer" : moved ? "operator saw the bar move" : "operator said yes but the level hardly changed",
		lo, hi);
	return v;
}
