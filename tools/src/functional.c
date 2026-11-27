/* Functional checklist: the hands-on checks before a used PC is listed as
 * refurbished. Keyboard and pointer through evdev, USB ports through sysfs,
 * radios and readers by PCI/USB class. Audio is in audio.c, the camera in camera.c. */
#include "bedrock.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <poll.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* The kernel's struct input_event. musl's 64-bit time_t on i686 makes the
 * header's struct timeval version larger than what the kernel writes. */
struct ev { unsigned long sec, usec; uint16_t type, code; int32_t value; };

#define LBITS (8 * sizeof(long))
#define NLONGS(n) ((n) / LBITS + 1)
#define MAXDEV 16

static int bit(const unsigned long *b, unsigned i)
{
	return b[i / LBITS] >> (i % LBITS) & 1;
}

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static void fn_screen(const char *title)
{
	printf("\033[0m\033[2J\033[H\033[1m%s\033[0m\n%s\n\n", HEADER, title);
}

/* First character of the operator's answer, lower case; '\n' for a bare Enter. */
int ask_char(const char *prompt)
{
	char in[16];
	printf("%s", prompt);
	fflush(stdout);
	if (!fgets(in, sizeof in, stdin))
		return EOF;
	if (!strchr(in, '\n'))
		for (int c; (c = getchar()) != EOF && c != '\n';)
			;
	return tolower((unsigned char)in[0]);
}

static void append(char *b, size_t n, const char *fmt, ...)
{
	va_list ap;
	size_t l = strlen(b);
	va_start(ap, fmt);
	vsnprintf(b + l, n - l, fmt, ap);
	va_end(ap);
}

static int stdin_line(int ms)
{
	struct pollfd p = {0, POLLIN, 0};
	if (poll(&p, 1, ms) <= 0)
		return 0;
	char in[64];
	return !fgets(in, sizeof in, stdin) || strchr(in, '\n');
}

/* --- Keyboard ------------------------------------------------------------- */

/* ANSI layout in three blocks: main, navigation, numpad. Columns are placed by
 * kb_layout(); a code-0 entry is a gap. ISO's extra key (<>) sits after Shift. */
static struct kbkey kb[] = {
	{KEY_ESC, 0, 0, 0, "Esc"}, {KEY_F1, 0, 0, 0, "F1"}, {KEY_F2, 0, 0, 0, "F2"}, {KEY_F3, 0, 0, 0, "F3"},
	{KEY_F4, 0, 0, 0, "F4"}, {KEY_F5, 0, 0, 0, "F5"}, {KEY_F6, 0, 0, 0, "F6"}, {KEY_F7, 0, 0, 0, "F7"},
	{KEY_F8, 0, 0, 0, "F8"}, {KEY_F9, 0, 0, 0, "F9"}, {KEY_F10, 0, 0, 0, "F10"}, {KEY_F11, 0, 0, 0, "F11"},
	{KEY_F12, 0, 0, 0, "F12"},
	{KEY_SYSRQ, 0, 1, 0, "Prt"}, {KEY_SCROLLLOCK, 0, 1, 0, "Scr"}, {KEY_PAUSE, 0, 1, 0, "Pau"},

	{KEY_GRAVE, 1, 0, 0, "`"}, {KEY_1, 1, 0, 0, "1"}, {KEY_2, 1, 0, 0, "2"}, {KEY_3, 1, 0, 0, "3"},
	{KEY_4, 1, 0, 0, "4"}, {KEY_5, 1, 0, 0, "5"}, {KEY_6, 1, 0, 0, "6"}, {KEY_7, 1, 0, 0, "7"},
	{KEY_8, 1, 0, 0, "8"}, {KEY_9, 1, 0, 0, "9"}, {KEY_0, 1, 0, 0, "0"}, {KEY_MINUS, 1, 0, 0, "-"},
	{KEY_EQUAL, 1, 0, 0, "="}, {KEY_BACKSPACE, 1, 0, 0, "Bksp"},
	{KEY_INSERT, 1, 1, 0, "Ins"}, {KEY_HOME, 1, 1, 0, "Hom"}, {KEY_PAGEUP, 1, 1, 0, "PgU"},
	{KEY_NUMLOCK, 1, 2, 0, "Num"}, {KEY_KPSLASH, 1, 2, 0, "/"}, {KEY_KPASTERISK, 1, 2, 0, "*"},
	{KEY_KPMINUS, 1, 2, 0, "-"},

	{KEY_TAB, 2, 0, 0, "Tab"}, {KEY_Q, 2, 0, 0, "Q"}, {KEY_W, 2, 0, 0, "W"}, {KEY_E, 2, 0, 0, "E"},
	{KEY_R, 2, 0, 0, "R"}, {KEY_T, 2, 0, 0, "T"}, {KEY_Y, 2, 0, 0, "Y"}, {KEY_U, 2, 0, 0, "U"},
	{KEY_I, 2, 0, 0, "I"}, {KEY_O, 2, 0, 0, "O"}, {KEY_P, 2, 0, 0, "P"}, {KEY_LEFTBRACE, 2, 0, 0, "["},
	{KEY_RIGHTBRACE, 2, 0, 0, "]"}, {KEY_BACKSLASH, 2, 0, 0, "\\"},
	{KEY_DELETE, 2, 1, 0, "Del"}, {KEY_END, 2, 1, 0, "End"}, {KEY_PAGEDOWN, 2, 1, 0, "PgD"},
	{KEY_KP7, 2, 2, 0, "7"}, {KEY_KP8, 2, 2, 0, "8"}, {KEY_KP9, 2, 2, 0, "9"}, {KEY_KPPLUS, 2, 2, 0, "+"},

	{KEY_CAPSLOCK, 3, 0, 0, "Caps"}, {KEY_A, 3, 0, 0, "A"}, {KEY_S, 3, 0, 0, "S"}, {KEY_D, 3, 0, 0, "D"},
	{KEY_F, 3, 0, 0, "F"}, {KEY_G, 3, 0, 0, "G"}, {KEY_H, 3, 0, 0, "H"}, {KEY_J, 3, 0, 0, "J"},
	{KEY_K, 3, 0, 0, "K"}, {KEY_L, 3, 0, 0, "L"}, {KEY_SEMICOLON, 3, 0, 0, ";"},
	{KEY_APOSTROPHE, 3, 0, 0, "'"}, {KEY_ENTER, 3, 0, 0, "Enter"},
	{KEY_KP4, 3, 2, 0, "4"}, {KEY_KP5, 3, 2, 0, "5"}, {KEY_KP6, 3, 2, 0, "6"},

	{KEY_LEFTSHIFT, 4, 0, 0, "Shift"}, {KEY_102ND, 4, 0, 0, "<>"}, {KEY_Z, 4, 0, 0, "Z"},
	{KEY_X, 4, 0, 0, "X"}, {KEY_C, 4, 0, 0, "C"}, {KEY_V, 4, 0, 0, "V"}, {KEY_B, 4, 0, 0, "B"},
	{KEY_N, 4, 0, 0, "N"}, {KEY_M, 4, 0, 0, "M"}, {KEY_COMMA, 4, 0, 0, ","}, {KEY_DOT, 4, 0, 0, "."},
	{KEY_SLASH, 4, 0, 0, "/"}, {KEY_RIGHTSHIFT, 4, 0, 0, "Shift"},
	{0, 4, 1, 0, "  "}, {KEY_UP, 4, 1, 0, "Up"},
	{KEY_KP1, 4, 2, 0, "1"}, {KEY_KP2, 4, 2, 0, "2"}, {KEY_KP3, 4, 2, 0, "3"}, {KEY_KPENTER, 4, 2, 0, "Ent"},

	{KEY_LEFTCTRL, 5, 0, 0, "Ctrl"}, {KEY_LEFTMETA, 5, 0, 0, "Win"}, {KEY_LEFTALT, 5, 0, 0, "Alt"},
	{KEY_SPACE, 5, 0, 0, "    Space    "}, {KEY_RIGHTALT, 5, 0, 0, "Alt"}, {KEY_RIGHTMETA, 5, 0, 0, "Win"},
	{KEY_COMPOSE, 5, 0, 0, "Menu"}, {KEY_RIGHTCTRL, 5, 0, 0, "Ctrl"},
	{KEY_LEFT, 5, 1, 0, "Lf"}, {KEY_DOWN, 5, 1, 0, "Dn"}, {KEY_RIGHT, 5, 1, 0, "Rt"},
	{KEY_KP0, 5, 2, 0, "0"}, {KEY_KPDOT, 5, 2, 0, "."},
};
#define KB_N ((int)(sizeof kb / sizeof *kb))
#define KB_TOP 5 /* screen row of the F-row */

const struct kbkey *kb_layout(int *n)
{
	static const uint8_t start[] = {0, 47, 61};
	static int placed;
	for (int i = 0, x = 0; !placed && i < KB_N; i++) {
		if (!i || kb[i].row != kb[i - 1].row || kb[i].block != kb[i - 1].block)
			x = start[kb[i].block];
		kb[i].col = x;
		x += strlen(kb[i].label) + 1;
	}
	placed = 1;
	*n = KB_N;
	return kb;
}

int kb_index(unsigned code)
{
	for (int i = 0; code && i < KB_N; i++)
		if (kb[i].code == code)
			return i;
	return -1;
}

static void kb_name(const struct kbkey *k, char *out, size_t n)
{
	const char *l = k->label;
	while (*l == ' ')
		l++;
	int right = k->code == KEY_RIGHTSHIFT || k->code == KEY_RIGHTCTRL || k->code == KEY_RIGHTALT || k->code == KEY_RIGHTMETA;
	snprintf(out, n, "%s%.*s%s", k->block == 2 ? "num " : "", (int)strcspn(l, " "), l, right ? " right" : "");
}

static void kb_draw(const struct kbkey *k, int pressed)
{
	printf("\033[%d;%dH%s%s\033[0m", KB_TOP + 2 * k->row, 3 + k->col, pressed ? "\033[30;42m" : "\033[7m", k->label);
}

static void close_all(int *fd, int n, int grabbed)
{
	for (int i = 0; i < n; i++)
		if (fd[i] >= 0) {
			if (grabbed)
				ioctl(fd[i], EVIOCGRAB, 0);
			close(fd[i]);
		}
}

static enum verdict keyboard_test(void)
{
	static unsigned long have[NLONGS(KEY_MAX)], held[NLONGS(KEY_MAX)];
	unsigned long bits[NLONGS(KEY_MAX)], st[NLONGS(KEY_MAX)];
	static unsigned char seen[KEY_CNT];
	struct pollfd pf[MAXDEV];
	int nfd = 0, keyboard = 0, others = 0, n, timeout = 0;
	const struct kbkey *k = kb_layout(&n);
	memset(have, 0, sizeof have);
	memset(held, 0, sizeof held);
	memset(seen, 0, sizeof seen);
	for (int i = 0; i < 32 && nfd < MAXDEV; i++) {
		char p[32];
		snprintf(p, sizeof p, "/dev/input/event%d", i);
		int fd = open(p, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;
		memset(bits, 0, sizeof bits);
		int any = 0;
		ioctl(fd, EVIOCGBIT(EV_KEY, sizeof bits), bits);
		for (unsigned c = 1; c < BTN_MISC && !any; c++)
			any = bit(bits, c);
		if (!any || bit(bits, BTN_LEFT) || bit(bits, BTN_TOUCH)) {
			close(fd); /* pointers are tested separately */
			continue;
		}
		keyboard |= bit(bits, KEY_A);
		memset(st, 0, sizeof st);
		ioctl(fd, EVIOCGKEY(sizeof st), st);
		for (size_t w = 0; w < NLONGS(KEY_MAX); w++)
			have[w] |= bits[w], held[w] |= st[w];
		ioctl(fd, EVIOCGRAB, 1); /* keys must not reach the console (Ctrl-C, Ctrl-Alt-Del) */
		pf[nfd++] = (struct pollfd){fd, POLLIN, 0};
	}
	if (!keyboard) {
		int fds[MAXDEV];
		for (int i = 0; i < nfd; i++)
			fds[i] = pf[i].fd;
		close_all(fds, nfd, 1);
		verdict(stdout, V_ABSENT, "Keyboard", "no keyboard found");
		return V_ABSENT;
	}
	printf("Press every key once; it turns green. Hold Esc for 2 seconds to finish\n"
	       "(or leave the keyboard alone for 60 seconds). Fn only changes other keys.\n\033[?25l");
	for (int i = 0; i < n; i++)
		if (k[i].code && bit(have, k[i].code))
			kb_draw(&k[i], 0);
	fflush(stdout);
	double last = now(), esc = 0;
	for (;;) {
		struct ev e[64];
		if (poll(pf, nfd, 100) > 0)
			for (int d = 0; d < nfd; d++) {
				if (pf[d].revents & (POLLERR | POLLHUP | POLLNVAL)) {
					close(pf[d].fd); /* unplugged */
					pf[d].fd = -1;
					continue;
				}
				ssize_t r = pf[d].revents & POLLIN ? read(pf[d].fd, e, sizeof e) : 0;
				for (int j = 0; j < r / (ssize_t)sizeof *e; j++) {
					if (e[j].type != EV_KEY || e[j].value == 2 || e[j].code >= KEY_CNT)
						continue;
					if (e[j].code == KEY_ESC)
						esc = e[j].value ? now() : 0;
					if (!e[j].value)
						continue;
					last = now();
					if (seen[e[j].code])
						continue;
					seen[e[j].code] = 1;
					int i = kb_index(e[j].code);
					if (i >= 0)
						kb_draw(&k[i], 1);
					else
						printf("\033[%d;3HOther keys pressed: %d (last code %u)", KB_TOP + 12, ++others, e[j].code);
				}
			}
		fflush(stdout);
		if (esc && now() - esc >= 2)
			break;
		if (now() - last >= 60) {
			timeout = 1;
			break;
		}
	}
	int fds[MAXDEV];
	for (int i = 0; i < nfd; i++)
		fds[i] = pf[i].fd;
	close_all(fds, nfd, 1);
	tcflush(0, TCIFLUSH);

	char missed[256] = "", stuck[128] = "", name[24];
	int nmiss = 0, npressed = 0;
	for (int i = 0; i < n; i++) {
		if (!k[i].code || !bit(have, k[i].code))
			continue;
		kb_name(&k[i], name, sizeof name);
		if (bit(held, k[i].code) && strlen(stuck) + strlen(name) + 3 < sizeof stuck)
			snprintf(stuck + strlen(stuck), sizeof stuck - strlen(stuck), "%s%s", stuck[0] ? ", " : "", name);
		if (seen[k[i].code]) {
			npressed++;
			continue;
		}
		if (strlen(missed) + strlen(name) + 3 < sizeof missed)
			snprintf(missed + strlen(missed), sizeof missed - strlen(missed), "%s%s", nmiss ? ", " : "", name);
		nmiss++;
	}
	printf("\033[%d;1H\033[J\033[?25h%s%d keys pressed, %d never pressed%s%s\n", KB_TOP + 13,
	       timeout ? "Stopped after 60 s without a key.\n" : "", npressed, nmiss, nmiss ? ": " : "", missed);
	if (stuck[0])
		printf("Held down when the test started (stuck?): %s\n", stuck);
	int c = ask_char("All keys work? [y/n] ");
	char why[384];
	snprintf(why, sizeof why, "%d keys pressed%s%s%s%s", npressed, nmiss ? "; never pressed: " : "", missed,
		 stuck[0] ? "; held at start: " : "", stuck);
	enum verdict v = c == 'y' ? V_PASS : c == 'n' ? V_FAIL : V_INCONCLUSIVE;
	verdict(stdout, v, "Keyboard", "%s %s", v == V_PASS ? "operator: all keys work;" : v == V_FAIL ? "operator: faulty keys;" : "no answer;", why);
	return v;
}

/* --- Touchpad, mouse, touchscreen -------------------------------------- */

struct ptr {
	int rel, direct, buttonpad, moves, left, right, used;
	char name[64];
	struct input_absinfo ax, ay;
};

#define BOX_W 60
#define BOX_H 10
#define BOX_TOP 5

static const char *ptr_kind(const struct ptr *p)
{
	return p->rel ? "mouse" : p->direct ? "touchscreen" : "touchpad";
}

static void ptr_status(const struct ptr *p, int n)
{
	for (int i = 0; i < n; i++)
		printf("\033[%d;1H\033[K  %-11s %-30.30s moved %-3s left %-3d right %d", BOX_TOP + BOX_H + 3 + i,
		       ptr_kind(&p[i]), p[i].name, p[i].moves >= 10 ? "yes" : "no", p[i].left, p[i].right);
}

static enum verdict ptr_verdict(const struct ptr *p)
{
	int moved = p->moves >= 10, right = p->right || p->buttonpad || p->direct;
	if (!p->used) {
		verdict(stdout, V_INCONCLUSIVE, "Pointer", "%s %s: no input at all (not tried, or dead)", ptr_kind(p), p->name);
		return V_INCONCLUSIVE;
	}
	if (moved && (p->left || p->direct) && right) {
		verdict(stdout, V_PASS, "Pointer", "%s %s: movement, %d left and %d right clicks%s", ptr_kind(p), p->name,
			p->left, p->right, p->buttonpad && !p->right ? " (clickpad: right click is a software zone)" : "");
		return V_PASS;
	}
	verdict(stdout, V_FAIL, "Pointer", "%s %s: never seen:%s%s%s", ptr_kind(p), p->name, moved ? "" : " movement",
		p->left || p->direct ? "" : " left button", right ? "" : " right button");
	return V_FAIL;
}

static enum verdict pointer_test(void)
{
	struct ptr p[MAXDEV];
	struct pollfd pf[MAXDEV + 1];
	int n = 0;
	for (int i = 0; i < 32 && n < MAXDEV; i++) {
		unsigned long keys[NLONGS(KEY_MAX)] = {0}, rel[NLONGS(REL_MAX)] = {0}, abs[NLONGS(ABS_MAX)] = {0}, prop[NLONGS(INPUT_PROP_MAX)] = {0};
		char path[32];
		snprintf(path, sizeof path, "/dev/input/event%d", i);
		int fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;
		ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys);
		ioctl(fd, EVIOCGBIT(EV_REL, sizeof rel), rel);
		ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs);
		ioctl(fd, EVIOCGPROP(sizeof prop), prop);
		int isrel = bit(rel, REL_X) && bit(rel, REL_Y), isabs = bit(abs, ABS_X) && bit(abs, ABS_Y);
		if (!(bit(keys, BTN_LEFT) || bit(keys, BTN_TOUCH)) || !(isrel || isabs)) {
			close(fd);
			continue;
		}
		memset(&p[n], 0, sizeof p[n]);
		p[n].rel = isrel && !isabs;
		p[n].direct = bit(prop, INPUT_PROP_DIRECT);
		p[n].buttonpad = bit(prop, INPUT_PROP_BUTTONPAD);
		ioctl(fd, EVIOCGNAME(sizeof p[n].name), p[n].name);
		ioctl(fd, EVIOCGABS(ABS_X), &p[n].ax);
		ioctl(fd, EVIOCGABS(ABS_Y), &p[n].ay);
		pf[n++] = (struct pollfd){fd, POLLIN, 0};
	}
	if (!n) {
		verdict(stdout, V_ABSENT, "Pointer", "no touchpad, mouse or touchscreen found");
		return V_ABSENT;
	}
	pf[n] = (struct pollfd){0, POLLIN, 0};
	printf("Move the pointer in the box and click left and right on every device.\n"
	       "Press Enter on the keyboard when done.\033[?25l");
	printf("\033[%d;3H+", BOX_TOP);
	for (int x = 0; x < BOX_W; x++)
		putchar('-');
	putchar('+');
	for (int y = 1; y <= BOX_H; y++)
		printf("\033[%d;3H|\033[%d;%dH|", BOX_TOP + y, BOX_TOP + y, 4 + BOX_W);
	printf("\033[%d;3H+", BOX_TOP + BOX_H + 1);
	for (int x = 0; x < BOX_W; x++)
		putchar('-');
	putchar('+');
	ptr_status(p, n);
	double fx = 0.5, fy = 0.5;
	int cx = -1, cy = -1, done = 0;
	while (!done) {
		struct ev e[64];
		fflush(stdout);
		if (poll(pf, n + 1, 200) <= 0)
			continue;
		if (pf[n].revents)
			done = stdin_line(0);
		for (int d = 0; d < n; d++) {
			if (pf[d].revents & (POLLERR | POLLHUP | POLLNVAL)) {
				close(pf[d].fd);
				pf[d].fd = -1;
				continue;
			}
			ssize_t r = pf[d].revents & POLLIN ? read(pf[d].fd, e, sizeof e) : 0;
			for (int j = 0; j < r / (ssize_t)sizeof *e; j++) {
				struct input_absinfo *a = e[j].code == ABS_X ? &p[d].ax : &p[d].ay;
				double range = a->maximum > a->minimum ? a->maximum - a->minimum : 1;
				if (e[j].type == EV_REL && (e[j].code == REL_X || e[j].code == REL_Y)) {
					*(e[j].code == REL_X ? &fx : &fy) += e[j].value / (e[j].code == REL_X ? 600.0 : 300.0);
					p[d].moves++;
				} else if (e[j].type == EV_ABS && (e[j].code == ABS_X || e[j].code == ABS_Y)) {
					*(e[j].code == ABS_X ? &fx : &fy) = (e[j].value - a->minimum) / range;
					p[d].moves++;
				} else if (e[j].type == EV_KEY && e[j].value == 1) {
					p[d].left += e[j].code == BTN_LEFT || (p[d].direct && e[j].code == BTN_TOUCH);
					p[d].right += e[j].code == BTN_RIGHT;
				} else {
					continue;
				}
				p[d].used = 1;
			}
			if (r > 0)
				ptr_status(p, n);
		}
		fx = fx < 0 ? 0 : fx > 1 ? 1 : fx;
		fy = fy < 0 ? 0 : fy > 1 ? 1 : fy;
		int x = fx * (BOX_W - 1), y = fy * (BOX_H - 1);
		if (x != cx || y != cy) {
			if (cx >= 0)
				printf("\033[%d;%dH ", BOX_TOP + 1 + cy, 4 + cx);
			printf("\033[%d;%dH\033[1;32m@\033[0m", BOX_TOP + 1 + y, 4 + x);
			cx = x, cy = y;
		}
	}
	printf("\033[%d;1H\033[J\033[?25h", BOX_TOP + BOX_H + 3 + n);
	enum verdict worst = V_INCONCLUSIVE;
	for (int d = 0; d < n; d++) {
		enum verdict v = ptr_verdict(&p[d]);
		if (v == V_FAIL || (v == V_PASS && worst != V_FAIL))
			worst = v;
		if (pf[d].fd >= 0)
			close(pf[d].fd);
	}
	return worst;
}

/* --- USB ports ---------------------------------------------------------- */

/* sysfs port name to the device path a device on that port gets:
 * "usb2-port3" (root hub 2) -> "2-3", "1-1-port4" (hub 1-1) -> "1-1.4". */
int usb_port_path(const char *portname, char *out, size_t n)
{
	const char *p = strstr(portname, "-port");
	if (!p || !isdigit((unsigned char)p[5]))
		return -1;
	int port = atoi(p + 5);
	if (!strncmp(portname, "usb", 3) && isdigit((unsigned char)portname[3]))
		snprintf(out, n, "%d-%d", atoi(portname + 3), port);
	else if (p > portname && isdigit((unsigned char)portname[0]))
		snprintf(out, n, "%.*s.%d", (int)(p - portname), portname, port);
	else
		return -1;
	return 0;
}

/* sysfs "speed" values: 1.5, 12, 480, 5000, 10000, 20000 (Mbit/s). */
const char *usb_speed_name(double mbps)
{
	return mbps >= 20000 ? "USB 3.2 20 Gbps" : mbps >= 10000 ? "USB 3.1 10 Gbps" : mbps >= 5000 ? "USB 3 5 Gbps"
	     : mbps >= 480 ? "USB 2 480 Mbps" : mbps >= 12 ? "USB 1.1 12 Mbps" : mbps > 0 ? "USB 1 1.5 Mbps" : "-";
}

struct port { char path[32], peer[32]; double cap, seen; int dup; };
#define MAXPORT 48
static struct port ports[MAXPORT];
static int nports;

static double dev_speed(const char *path)
{
	char p[96], b[16];
	snprintf(p, sizeof p, "/sys/bus/usb/devices/%s/speed", path);
	return read_file(p, b, sizeof b) > 0 ? atof(b) : 0;
}

/* Ports of one hub. Hubs already attached (rate-matching hubs, laptop internal
 * hubs) count as part of the machine: their ports are listed instead. */
static void add_ports(const char *hub, double cap)
{
	char dir[96], p[PATH_MAX], path[32], type[32], link[PATH_MAX];
	int bus;
	if (sscanf(hub, "usb%d", &bus) == 1)
		snprintf(dir, sizeof dir, "/sys/bus/usb/devices/%s/%d-0:1.0", hub, bus);
	else
		snprintf(dir, sizeof dir, "/sys/bus/usb/devices/%s/%s:1.0", hub, hub);
	DIR *d = opendir(dir);
	struct dirent *e;
	while (d && (e = readdir(d)) && nports < MAXPORT) {
		if (usb_port_path(e->d_name, path, sizeof path))
			continue;
		snprintf(p, sizeof p, "%s/%s/connect_type", dir, e->d_name);
		read_file(p, type, sizeof type); /* ACPI _UPC/_PLD; empty without them */
		if (!strcmp(type, "not used"))
			continue;
		snprintf(p, sizeof p, "/sys/bus/usb/devices/%s/maxchild", path);
		if (read_ll(p, 0) > 0) {
			add_ports(path, dev_speed(path));
			continue;
		}
		if (!strcmp(type, "hardwired"))
			continue; /* internal camera, Bluetooth, card reader */
		struct port *q = &ports[nports++];
		memset(q, 0, sizeof *q);
		snprintf(q->path, sizeof q->path, "%s", path);
		q->cap = cap;
		snprintf(p, sizeof p, "%s/%s/peer", dir, e->d_name); /* the same connector on the USB 3 bus */
		ssize_t l = readlink(p, link, sizeof link - 1);
		if (l > 0) {
			link[l] = 0;
			usb_port_path(strrchr(link, '/') ? strrchr(link, '/') + 1 : link, q->peer, sizeof q->peer);
		}
	}
	if (d)
		closedir(d);
}

static void pci_parent(const char *usbdev, char *addr, size_t n, char *drv, size_t dn)
{
	char p[300], real[PATH_MAX], link[PATH_MAX];
	addr[0] = drv[0] = 0;
	snprintf(p, sizeof p, "/sys/bus/usb/devices/%s/..", usbdev);
	if (!realpath(p, real))
		return;
	snprintf(addr, n, "%s", strrchr(real, '/') + 1);
	strncat(real, "/driver", sizeof real - strlen(real) - 1);
	ssize_t l = readlink(real, link, sizeof link - 1);
	if (l > 0) {
		link[l] = 0;
		snprintf(drv, dn, "%s", strrchr(link, '/') + 1);
	}
}

static int usb_ports(FILE *o)
{
	char addr[16][32], drv[16][32];
	int bus[16], nb = 0;
	DIR *d = opendir("/sys/bus/usb/devices");
	struct dirent *e;
	while (d && (e = readdir(d)) && nb < 16)
		if (sscanf(e->d_name, "usb%d", &bus[nb]) == 1) {
			pci_parent(e->d_name, addr[nb], sizeof addr[nb], drv[nb], sizeof drv[nb]);
			nb++;
		}
	if (d)
		closedir(d);
	nports = 0;
	for (int i = 0; i < nb; i++) {
		char hub[16];
		snprintf(hub, sizeof hub, "usb%d", bus[i]);
		double sp = dev_speed(hub);
		int companion = 0;
		/* UHCI/OHCI next to an EHCI in the same PCI slot only carry slow devices
		 * plugged into the EHCI ports: list the connector once, under EHCI. */
		for (int j = 0; j < nb && sp < 480; j++)
			companion |= j != i && !strncmp(drv[j], "ehci", 4) && strlen(addr[i]) > 2 &&
				     !strncmp(addr[i], addr[j], strlen(addr[i]) - 2);
		fprintf(o, "  Controller %s (%s), bus %d: %s%s\n", addr[i], drv[i][0] ? drv[i] : "no driver", bus[i],
			usb_speed_name(sp), companion ? ", companion of EHCI" : "");
		if (!companion)
			add_ports(hub, sp);
	}
	for (int i = 0; i < nports; i++)
		for (int j = 0; j < nports; j++)
			if (i != j && ports[i].peer[0] && !strcmp(ports[i].peer, ports[j].path) && ports[j].cap > ports[i].cap) {
				ports[j].dup = 1; /* shown with its USB 2 half */
				ports[i].cap = ports[j].cap;
			}
	return nb;
}

static enum verdict usb_test(void)
{
	if (!usb_ports(stdout)) {
		verdict(stdout, V_ABSENT, "USB ports", "no USB controller found");
		return V_ABSENT;
	}
	printf("\nInsert a USB stick (USB 2 or newer) or any device into each port, one by one.\n"
	       "Wait for the port to show a speed. Press Enter when done.\n\n");
	printf("\0337"); /* save cursor: the table is redrawn from here */
	int redraw = 1, done = 0;
	while (!done) {
		for (int i = 0; i < nports; i++) {
			double s = dev_speed(ports[i].path), t = ports[i].peer[0] ? dev_speed(ports[i].peer) : 0;
			if (t > s)
				s = t;
			if (s > ports[i].seen)
				ports[i].seen = s, redraw = 1;
		}
		if (redraw) {
			printf("\0338\033[J  %-14s %-18s %s\n", "Port", "Port type", "Device seen at");
			for (int i = 0; i < nports; i++) {
				if (ports[i].dup)
					continue;
				char name[40];
				snprintf(name, sizeof name, "%s%s%s", ports[i].path, ports[i].peer[0] ? " / " : "", ports[i].peer);
				printf("  %-14s %-18s %s%s\033[0m\n", name, usb_speed_name(ports[i].cap),
				       ports[i].seen ? "\033[32m" : "", ports[i].seen ? usb_speed_name(ports[i].seen) : "not yet");
			}
			fflush(stdout);
			redraw = 0;
		}
		done = stdin_line(500);
	}
	char seen[384] = "", untested[256] = "";
	int nu = 0, ns = 0;
	for (int i = 0; i < nports; i++) {
		if (ports[i].dup)
			continue;
		if (ports[i].seen)
			append(seen, sizeof seen, "%s%s %s", ns++ ? ", " : "", ports[i].path, usb_speed_name(ports[i].seen));
		else
			append(untested, sizeof untested, "%s%s", nu++ ? ", " : "", ports[i].path);
	}
	if (!nu) {
		verdict(stdout, V_PASS, "USB ports", "all %d ports saw a device: %s", ns, seen);
		return V_PASS;
	}
	printf("\nNo device seen on: %s\n", untested);
	int c = ask_char("Did you insert a device into each of these ports? [y/n] ");
	enum verdict v = c == 'y' ? V_FAIL : V_INCONCLUSIVE;
	verdict(stdout, v, "USB ports", "%s: %s; working: %s", v == V_FAIL ? "no device detected on" : "untested", untested,
		ns ? seen : "none");
	return v;
}

/* --- Presence: radios, Ethernet, readers -------------------------------- */

enum { P_WIFI, P_BT, P_ETH, P_CARD, P_FP, P_N };
static const char *const pname[] = {"Wi-Fi", "Bluetooth", "Ethernet", "Card reader", "Fingerprint reader"};
static const char *const pkey[] = {"functional.wifi", "functional.bluetooth", "functional.ethernet",
				   "functional.cardreader", "functional.fingerprint"};

/* PCI class codes: PCI Code and ID Assignment spec, section 1. */
static int pci_kind(unsigned cls, unsigned ven)
{
	switch (cls >> 8) {
	case 0x0280: return P_WIFI;
	case 0x0d11: return P_BT;
	case 0x0200: return P_ETH;
	case 0x0805: return P_CARD;
	case 0xff00: return ven == 0x10ec || ven == 0x1217 ? P_CARD : -1; /* Realtek, O2 readers use a vendor class */
	}
	return -1;
}

/* Fingerprint readers have vendor-specific interfaces, so they are known by vendor. */
static int usb_kind(unsigned cls, unsigned sub, unsigned proto, unsigned ven, unsigned prod, const char *name)
{
	static const unsigned fp[] = {0x138a, 0x27c6, 0x08ff, 0x147e, 0x1c7a, 0x10a5, 0x06cb};
	if (cls == 0xe0 && sub == 1 && proto == 1) /* USB class codes: Bluetooth programming interface */
		return P_BT;
	for (size_t i = 0; i < sizeof fp / sizeof *fp; i++)
		if (ven == fp[i])
			return P_FP;
	if (strcasestr(name, "finger") || (ven == 0x04f3 && (prod & 0xff00) == 0x0c00))
		return P_FP;
	if (strcasestr(name, "reader") || strstr(name, "CRW"))
		return P_CARD;
	return -1;
}

static unsigned hexfile(const char *dir, const char *name, const char *attr)
{
	char p[PATH_MAX], b[32];
	snprintf(p, sizeof p, "%s/%s/%s", dir, name, attr);
	return read_file(p, b, sizeof b) > 0 ? strtoul(b, NULL, 16) : 0;
}

static void driver_of(const char *dir, const char *name, char *out, size_t n)
{
	char p[PATH_MAX], l[PATH_MAX];
	snprintf(p, sizeof p, "%s/%s/driver", dir, name);
	ssize_t r = readlink(p, l, sizeof l - 1);
	l[r > 0 ? r : 0] = 0;
	snprintf(out, n, "%s", r > 0 ? strrchr(l, '/') + 1 : "");
}

void functional_presence(FILE *o)
{
	char found[P_N][128] = {{0}}, drv[64];
	const char *pci = "/sys/bus/pci/devices", *usb = "/sys/bus/usb/devices";
	DIR *d = opendir(pci);
	struct dirent *e;
	while (d && (e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		unsigned ven = hexfile(pci, e->d_name, "vendor");
		int k = pci_kind(hexfile(pci, e->d_name, "class"), ven);
		if (k < 0 || found[k][0])
			continue;
		driver_of(pci, e->d_name, drv, sizeof drv);
		snprintf(found[k], sizeof found[k], "PCI %04x:%04x at %.16s, %s%s", ven, hexfile(pci, e->d_name, "device"),
			 e->d_name, drv[0] ? "driver " : "no driver", drv);
	}
	if (d)
		closedir(d);
	d = opendir(usb);
	while (d && (e = readdir(d))) {
		char dev[32], p[PATH_MAX], prod[64] = "";
		if (!strchr(e->d_name, ':') || isalpha((unsigned char)e->d_name[0]))
			continue; /* interfaces only, not root hubs */
		snprintf(dev, sizeof dev, "%.*s", (int)strcspn(e->d_name, ":"), e->d_name);
		snprintf(p, sizeof p, "%s/%s/product", usb, dev);
		read_file(p, prod, sizeof prod);
		unsigned ven = hexfile(usb, dev, "idVendor"), pid = hexfile(usb, dev, "idProduct");
		int k = usb_kind(hexfile(usb, e->d_name, "bInterfaceClass"), hexfile(usb, e->d_name, "bInterfaceSubClass"),
				 hexfile(usb, e->d_name, "bInterfaceProtocol"), ven, pid, prod);
		if (k < 0 || found[k][0])
			continue;
		driver_of(usb, e->d_name, drv, sizeof drv);
		snprintf(found[k], sizeof found[k], "USB %04x:%04x %s at %s, %s%s", ven, pid, prod, dev,
			 drv[0] ? "driver " : "no driver", drv);
	}
	if (d)
		closedir(d);
	for (int k = 0; k < P_N; k++) {
		if (found[k][0]) {
			verdict(o, V_PASS, pname[k], "present: %s (presence only, function not tested)", found[k]);
			fact(pkey[k], "present: %s", found[k]);
		} else {
			verdict(o, V_ABSENT, pname[k], "none found on PCI or USB%s", k == P_WIFI ? " (USB Wi-Fi adapters are not recognised)" : "");
			fact(pkey[k], "not present");
		}
	}
}

/* --- The checklist ------------------------------------------------------ */

static const struct {
	const char *name, *key, *how;
	enum verdict (*run)(void);
} items[] = {
	{"Keyboard", "functional.keyboard", "Press every key once; each turns green. Hold Esc for 2 s to finish.", keyboard_test},
	{"Touchpad, mouse, touchscreen", "functional.pointer", "Move the pointer and click left and right on each device.", pointer_test},
	{"Speakers", "functional.speakers", "A tone plays on the left speaker, then on the right. Turn the volume up.", speaker_test},
	{"Microphone", "functional.microphone", "Speak or tap near the microphone for 3 seconds and watch the bar.", mic_test},
	{"Camera", "functional.camera", "The camera picture fills the screen; check it is sharp and clean.", camera_test},
	{"USB ports", "functional.usb", "Insert a USB stick into each port, one by one.", usb_test},
};
#define NITEMS ((int)(sizeof items / sizeof *items))

void functional_run(void)
{
	int res[NITEMS], stop = 0;
	char title[96];
	for (int i = 0; i < NITEMS; i++) {
		snprintf(title, sizeof title, "Functional checklist %d/%d: %s", i + 1, NITEMS, items[i].name);
		fn_screen(title);
		int c = stop ? 's' : (printf("%s\n\n", items[i].how),
				      ask_char("Enter = start, s = skip, q = skip the rest: "));
		if (c == EOF || c == 'q')
			stop = 1, c = 's';
		if (c == 's') {
			res[i] = -1;
			verdict(stdout, V_INCONCLUSIVE, items[i].name, "skipped by the operator");
			fact(items[i].key, "SKIPPED");
			continue;
		}
		fn_screen(title);
		res[i] = items[i].run();
		fact(items[i].key, "%s", verdict_name(res[i]));
		if (ask_char("\nPress Enter for the next item. ") == EOF)
			stop = 1;
	}
	fn_screen("Functional checklist: summary");
	for (int i = 0; i < NITEMS; i++)
		printf("  %-30s %s\n", items[i].name, res[i] < 0 ? "SKIPPED" : verdict_name(res[i]));
	printf("\n");
	functional_presence(stdout);
	ask_char("\nPress Enter to return to the menu. ");
}
