#include "bedrock.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <termios.h>
#include <unistd.h>

static int hex4(const char *s, unsigned *v)
{
	for (int i = 0; i < 4; i++)
		if (!isxdigit((unsigned char)s[i]))
			return 0;
	if (s[4] != ' ')
		return 0;
	*v = strtoul(s, NULL, 16);
	return 1;
}

static void copy_name(char *out, size_t n, const char *s)
{
	while (*s == ' ')
		s++;
	snprintf(out, n, "%s", s);
	out[strcspn(out, "\r\n")] = 0;
}

/* pci.ids: "vvvv  Vendor" lines, then "\tdddd  Device" lines under each.
 * Returns 0 nothing found, 1 vendor only, 2 vendor and device. */
int pciids_lookup(FILE *f, unsigned ven, unsigned dev, char *vname, size_t vn, char *dname, size_t dn)
{
	char line[512];
	unsigned id;
	int in = 0;
	vname[0] = dname[0] = 0;
	rewind(f);
	while (fgets(line, sizeof line, f)) {
		if (line[0] != '\t' && hex4(line, &id)) {
			if (in)
				break;
			if (id == ven) {
				in = 1;
				copy_name(vname, vn, line + 4);
			}
		} else if (in && line[0] == '\t' && line[1] != '\t' && hex4(line + 1, &id) && id == dev) {
			copy_name(dname, dn, line + 5);
			return 2;
		} else if (line[0] == 'C' && line[1] == ' ') {
			break; /* device classes follow the vendor list */
		}
	}
	return in;
}

static int by_str(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static void pcie_link(FILE *o, const char *addr)
{
	char p[PATH_MAX + 32], real[PATH_MAX], cs[32] = "", ms[32] = "", ps[32] = "", test[64];
	snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/current_link_width", addr);
	long long cw = read_ll(p, -1);
	snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/max_link_width", addr);
	long long mw = read_ll(p, -1), pw = -1;
	if (cw < 0 || mw <= 0)
		return; /* conventional PCI/AGP, or kernel does not expose the link */
	snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/current_link_speed", addr);
	read_file(p, cs, sizeof cs);
	snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/max_link_speed", addr);
	read_file(p, ms, sizeof ms);
	/* The upstream port's maximum is what the slot is wired for. */
	snprintf(p, sizeof p, "/sys/bus/pci/devices/%s", addr);
	if (realpath(p, real) && strrchr(real, '/')) {
		*strrchr(real, '/') = 0;
		const char *parent = strrchr(real, '/') ? strrchr(real, '/') + 1 : real;
		if (strchr(parent, ':') && strchr(parent, '.')) {
			snprintf(p, sizeof p, "%s/max_link_width", real);
			pw = read_ll(p, -1);
			snprintf(p, sizeof p, "%s/max_link_speed", real);
			read_file(p, ps, sizeof ps);
		}
	}
	long long want = pw > 0 && pw < mw ? pw : mw;
	double c = atof(cs), m = atof(ms), u = atof(ps), wants = u > 0 && u < m ? u : m;
	snprintf(test, sizeof test, "PCIe link %s", addr);
	fprintf(o, "    link x%lld %s (card max x%lld %s", cw, cs, mw, ms);
	if (pw > 0)
		fprintf(o, ", slot max x%lld %s", pw, ps);
	fprintf(o, ")\n");
	if (cw == 0)
		verdict(o, V_INCONCLUSIVE, test, "link is down (device may be powered off)");
	else if (cw < want)
		verdict(o, V_FAIL, test, "trained at x%lld although both ends support x%lld: reseat the card, clean the slot", cw, want);
	else if (c > 0 && wants > 0 && c < wants)
		verdict(o, V_INCONCLUSIVE, test, "width ok; speed %s below %.1f GT/s, normal while the GPU idles in power saving", cs, wants);
	else
		verdict(o, V_PASS, test, "x%lld at full speed", cw);
}

void gpu_show(FILE *o)
{
	static const char *idpaths[] = {"/usr/share/misc/pci.ids", "/usr/share/hwdata/pci.ids", "/usr/share/pci.ids"};
	FILE *ids = NULL;
	for (int i = 0; i < 3 && !ids; i++)
		ids = fopen(idpaths[i], "r");
	DIR *d = opendir("/sys/bus/pci/devices");
	struct dirent *e;
	char *names[256], p[PATH_MAX], vn[128], dn[160], buf[64];
	int n = 0, gpus = 0;
	while (d && n < 256 && (e = readdir(d)))
		if (e->d_name[0] != '.')
			names[n++] = strdup(e->d_name);
	if (d)
		closedir(d);
	qsort(names, n, sizeof *names, by_str);
	fprintf(o, "PCI devices (%d)%s\n", n, ids ? "" : "  [pci.ids not found: IDs only]");
	for (int i = 0; i < n; i++) {
		if (!names[i])
			continue;
		snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/class", names[i]);
		unsigned cls = read_file(p, buf, sizeof buf) > 0 ? strtoul(buf, NULL, 16) : 0;
		snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/vendor", names[i]);
		unsigned ven = read_file(p, buf, sizeof buf) > 0 ? strtoul(buf, NULL, 16) : 0;
		snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/device", names[i]);
		unsigned dev = read_file(p, buf, sizeof buf) > 0 ? strtoul(buf, NULL, 16) : 0;
		vn[0] = dn[0] = 0;
		if (ids)
			pciids_lookup(ids, ven, dev, vn, sizeof vn, dn, sizeof dn);
		fprintf(o, "  %s [%06x] %04x:%04x %s %s\n", names[i], cls, ven, dev, vn, dn);
		if (cls >> 16 == 0x03) {
			gpus++;
			char drv[PATH_MAX] = "";
			snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/driver", names[i]);
			ssize_t l = readlink(p, drv, sizeof drv - 1);
			drv[l > 0 ? l : 0] = 0;
			unsigned long long bar = 0, s, en, fl;
			snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/resource", names[i]);
			FILE *r = fopen(p, "r");
			while (r && fscanf(r, "%llx %llx %llx", &s, &en, &fl) == 3)
				if (fl & 0x200 && en > s && en - s + 1 > bar) /* IORESOURCE_MEM */
					bar = en - s + 1;
			if (r)
				fclose(r);
			fprintf(o, "    display controller, driver %s, largest memory BAR %llu MiB (VRAM window, not always all VRAM)\n",
				l > 0 ? strrchr(drv, '/') + 1 : "none", bar >> 20);
			pcie_link(o, names[i]);
			char key[32];
			snprintf(key, sizeof key, "gpu.%d", gpus - 1);
			fact(key, "%s %s [%04x:%04x]", vn, dn, ven, dev);
		}
		free(names[i]);
	}
	if (ids)
		fclose(ids);
	if (!gpus)
		verdict(o, V_ABSENT, "GPU", "no display controller on the PCI bus");
}

static unsigned chan(unsigned c, const struct fb_bitfield *b)
{
	return (b->length >= 8 ? c << (b->length - 8) : c >> (8 - b->length)) << b->offset;
}

static int wait_enter(void)
{
	int c;
	while ((c = getchar()) != EOF && c != '\n')
		;
	return c != EOF;
}

void gpu_display_test(void)
{
	struct fb_var_screeninfo v;
	struct fb_fix_screeninfo f;
	int fd = open("/dev/fb0", O_RDWR);
	if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &v) || ioctl(fd, FBIOGET_FSCREENINFO, &f)) {
		verdict(stdout, V_ABSENT, "Display test", "no framebuffer (/dev/fb0)");
		if (fd >= 0)
			close(fd);
		return;
	}
	unsigned bpp = v.bits_per_pixel / 8;
	if (bpp < 2 || bpp > 4 || f.visual != FB_VISUAL_TRUECOLOR ||
	    (size_t)(v.yoffset + v.yres) * f.line_length > f.smem_len) {
		verdict(stdout, V_ABSENT, "Display test", "framebuffer is %u bpp palette/other mode, not supported", v.bits_per_pixel);
		close(fd);
		return;
	}
	uint8_t *fb = mmap(NULL, f.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (fb == MAP_FAILED) {
		verdict(stdout, V_INCONCLUSIVE, "Display test", "cannot map the framebuffer");
		close(fd);
		return;
	}
	printf("Six full screens follow: red, green, blue, white, black, gradients.\n"
	       "Look for pixels that differ from the rest (dead or stuck) and for steps\n"
	       "in the gradients (banding). Press Enter to go to the next screen.\n"
	       "Press Enter to start.");
	fflush(stdout);
	struct termios old, raw;
	int tty = !tcgetattr(0, &old);
	if (!wait_enter())
		goto out;
	if (tty) {
		raw = old;
		raw.c_lflag &= ~ECHO; /* an echoed newline would make the console draw over the test */
		tcsetattr(0, TCSANOW, &raw);
	}
	int graphics = !ioctl(0, KDSETMODE, KD_GRAPHICS); /* stop fbcon drawing (and blinking the cursor) */
	static const uint8_t solid[5][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}, {0, 0, 0}};
	uint8_t *base = fb + (size_t)v.yoffset * f.line_length + (size_t)v.xoffset * bpp;
	int done = 1;
	for (int s = 0; s < 6 && done; s++) {
		for (unsigned y = 0; y < v.yres; y++) {
			uint8_t *row = base + (size_t)y * f.line_length;
			for (unsigned x = 0; x < v.xres; x++) {
				unsigned r, g, b, l = v.xres > 1 ? x * 255 / (v.xres - 1) : 0, band = y * 4 / v.yres;
				if (s < 5) {
					r = solid[s][0], g = solid[s][1], b = solid[s][2];
				} else {
					r = band == 0 || band == 3 ? l : 0;
					g = band == 1 || band == 3 ? l : 0;
					b = band == 2 || band == 3 ? l : 0;
				}
				uint32_t px = chan(r, &v.red) | chan(g, &v.green) | chan(b, &v.blue);
				memcpy(row + x * bpp, &px, bpp);
			}
		}
		done = wait_enter();
	}
	if (graphics)
		ioctl(0, KDSETMODE, KD_TEXT);
	if (tty)
		tcsetattr(0, TCSANOW, &old);
	printf("\033[2J\033[H%s\n\nDid every screen look clean: no dead or stuck pixels, no banding? [y/n] ", HEADER);
	fflush(stdout);
	int c = getchar();
	if (c != '\n' && c != EOF)
		wait_enter();
	if (c == 'y' || c == 'Y')
		verdict(stdout, V_PASS, "Display test", "user saw no defects");
	else if (c == 'n' || c == 'N')
		verdict(stdout, V_FAIL, "Display test", "user saw dead pixels or banding");
	else
		verdict(stdout, V_INCONCLUSIVE, "Display test", "no answer");
out:
	munmap(fb, f.smem_len);
	close(fd);
}
