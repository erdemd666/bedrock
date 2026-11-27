/* Webcam: V4L2 capture in YUYV (every UVC camera offers it), drawn scaled to
 * fit on the framebuffer so the operator can judge the picture. */
#include "bedrock.h"

#include <fcntl.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <termios.h>
#include <unistd.h>

static uint8_t clamp8(int x)
{
	return x < 0 ? 0 : x > 255 ? 255 : x;
}

/* ITU-R BT.601 studio range (Y 16-235, Cb/Cr 16-240) to full-range RGB, in
 * 8.8 fixed point so slow CPUs keep up. */
void yuyv_rgb(int y, int u, int v, uint8_t rgb[3])
{
	int c = 298 * (y - 16), d = u - 128, e = v - 128;
	rgb[0] = clamp8((c + 409 * e + 128) >> 8);
	rgb[1] = clamp8((c - 100 * d - 208 * e + 128) >> 8);
	rgb[2] = clamp8((c + 516 * d + 128) >> 8);
}

/* Largest w x h with the source's aspect ratio inside dw x dh. */
void fit_rect(unsigned sw, unsigned sh, unsigned dw, unsigned dh, unsigned *w, unsigned *h)
{
	if ((unsigned long long)sw * dh > (unsigned long long)sh * dw) {
		*w = dw;
		*h = (unsigned long long)sh * dw / sw;
	} else {
		*h = dh;
		*w = (unsigned long long)sw * dh / sh;
	}
}

static unsigned chan(unsigned c, const struct fb_bitfield *b)
{
	return (b->length >= 8 ? c << (b->length - 8) : c >> (8 - b->length)) << b->offset;
}

struct cam {
	int fd;
	unsigned w, h, stride, n;
	void *buf[4];
	size_t len[4];
	char name[32];
};

static void cam_close(struct cam *c)
{
	enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	ioctl(c->fd, VIDIOC_STREAMOFF, &t);
	for (unsigned i = 0; i < 4; i++)
		if (c->buf[i])
			munmap(c->buf[i], c->len[i]);
	close(c->fd);
	memset(c, 0, sizeof *c);
	c->fd = -1;
}

/* First capture node that does YUYV, streaming into 4 mmap buffers. *other
 * counts cameras that offer only other formats (IR cameras: GREY). */
static int cam_open(struct cam *c, int *other)
{
	for (int i = 0; i < 16; i++) {
		char p[24];
		struct v4l2_capability cap = {0};
		struct v4l2_format f = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
		struct v4l2_requestbuffers rb = {.count = 4, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP};
		const unsigned need = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
		snprintf(p, sizeof p, "/dev/video%d", i);
		memset(c, 0, sizeof *c);
		if ((c->fd = open(p, O_RDWR | O_NONBLOCK | O_CLOEXEC)) < 0)
			continue;
		unsigned caps = ioctl(c->fd, VIDIOC_QUERYCAP, &cap) ? 0
			      : cap.capabilities & V4L2_CAP_DEVICE_CAPS ? cap.device_caps : cap.capabilities;
		if ((caps & need) != need) { /* UVC's second node carries metadata only */
			close(c->fd);
			continue;
		}
		f.fmt.pix.width = 640;
		f.fmt.pix.height = 480;
		f.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
		f.fmt.pix.field = V4L2_FIELD_ANY;
		if (ioctl(c->fd, VIDIOC_S_FMT, &f) || f.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV || !f.fmt.pix.width || !f.fmt.pix.height) {
			(*other)++;
			close(c->fd);
			continue;
		}
		c->w = f.fmt.pix.width;
		c->h = f.fmt.pix.height;
		c->stride = f.fmt.pix.bytesperline ? f.fmt.pix.bytesperline : c->w * 2;
		snprintf(c->name, sizeof c->name, "%s", (const char *)cap.card);
		int ok = !ioctl(c->fd, VIDIOC_REQBUFS, &rb) && rb.count;
		c->n = rb.count < 4 ? rb.count : 4;
		for (unsigned b = 0; ok && b < c->n; b++) {
			struct v4l2_buffer vb = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = b};
			ok = !ioctl(c->fd, VIDIOC_QUERYBUF, &vb);
			void *m = ok ? mmap(NULL, vb.length, PROT_READ, MAP_SHARED, c->fd, vb.m.offset) : MAP_FAILED;
			ok = m != MAP_FAILED && !ioctl(c->fd, VIDIOC_QBUF, &vb);
			if (m != MAP_FAILED)
				c->buf[b] = m, c->len[b] = vb.length;
		}
		enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		if (ok && !ioctl(c->fd, VIDIOC_STREAMON, &t))
			return 0;
		cam_close(c);
		(*other)++;
	}
	return -1;
}

/* A dequeued full frame, or NULL. The caller requeues vb. */
static const uint8_t *cam_frame(struct cam *c, struct v4l2_buffer *vb, int ms)
{
	struct pollfd p = {c->fd, POLLIN, 0};
	memset(vb, 0, sizeof *vb);
	vb->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	vb->memory = V4L2_MEMORY_MMAP;
	if (poll(&p, 1, ms) <= 0 || ioctl(c->fd, VIDIOC_DQBUF, vb))
		return NULL;
	if (vb->index >= c->n || vb->bytesused < c->stride * c->h) {
		ioctl(c->fd, VIDIOC_QBUF, vb); /* short frame: skip it */
		return NULL;
	}
	return c->buf[vb->index];
}

static void draw(uint8_t *base, const struct fb_var_screeninfo *v, unsigned line, const uint8_t *src, const struct cam *c)
{
	unsigned w, h, bpp = v->bits_per_pixel / 8;
	fit_rect(c->w, c->h, v->xres, v->yres, &w, &h);
	base += (size_t)(v->yres - h) / 2 * line + (size_t)(v->xres - w) / 2 * bpp;
	for (unsigned y = 0; y < h; y++) {
		const uint8_t *s = src + (size_t)(y * c->h / h) * c->stride;
		uint8_t *d = base + (size_t)y * line, rgb[3];
		for (unsigned x = 0; x < w; x++) {
			unsigned sx = x * c->w / w;
			const uint8_t *pair = s + (sx & ~1u) * 2; /* Y0 U Y1 V */
			yuyv_rgb(s[sx * 2], pair[1], pair[3], rgb);
			uint32_t px = chan(rgb[0], &v->red) | chan(rgb[1], &v->green) | chan(rgb[2], &v->blue);
			memcpy(d + x * bpp, &px, bpp);
		}
	}
}

enum verdict camera_test(void)
{
	struct cam c;
	struct v4l2_buffer vb;
	struct fb_var_screeninfo v;
	struct fb_fix_screeninfo f;
	int other = 0, frames = 0;
	if (cam_open(&c, &other)) {
		enum verdict r = other ? V_INCONCLUSIVE : V_ABSENT;
		verdict(stdout, r, "Camera", "%s", other ? "camera found, but it does not stream YUYV" : "no camera");
		return r;
	}
	char name[32];
	snprintf(name, sizeof name, "%s", c.name);
	int fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
	uint8_t *fb = MAP_FAILED;
	if (fd >= 0 && !ioctl(fd, FBIOGET_VSCREENINFO, &v) && !ioctl(fd, FBIOGET_FSCREENINFO, &f) &&
	    v.bits_per_pixel >= 16 && v.bits_per_pixel <= 32 && f.visual == FB_VISUAL_TRUECOLOR &&
	    (size_t)(v.yoffset + v.yres) * f.line_length <= f.smem_len)
		fb = mmap(NULL, f.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (fb == MAP_FAILED) {
		for (; frames < 10 && cam_frame(&c, &vb, 3000); frames++)
			ioctl(c.fd, VIDIOC_QBUF, &vb);
		cam_close(&c);
		if (fd >= 0)
			close(fd);
		enum verdict r = frames ? V_INCONCLUSIVE : V_FAIL;
		verdict(stdout, r, "Camera", frames ? "%s sends pictures, but this screen is text-only, so they cannot be shown"
						  : "%s: no picture arrives", name);
		return r;
	}
	printf("Camera: %s, %ux%u. The picture fills the screen; press Enter to end it.\n", c.name, c.w, c.h);
	ask_char("Press Enter to start. ");
	struct termios old, raw;
	int tty = !tcgetattr(0, &old);
	if (tty) {
		raw = old;
		raw.c_lflag &= ~ECHO; /* an echoed newline would draw over the picture */
		tcsetattr(0, TCSANOW, &raw);
	}
	int graphics = !ioctl(0, KDSETMODE, KD_GRAPHICS);
	uint8_t *base = fb + (size_t)v.yoffset * f.line_length + (size_t)v.xoffset * (v.bits_per_pixel / 8);
	memset(fb, 0, f.smem_len);
	for (;;) {
		struct pollfd p[2] = {{c.fd, POLLIN, 0}, {0, POLLIN, 0}};
		if (poll(p, 2, 3000) <= 0)
			break; /* camera stopped sending */
		if (p[1].revents) {
			ask_char("");
			break;
		}
		const uint8_t *img = cam_frame(&c, &vb, 0);
		if (img) {
			draw(base, &v, f.line_length, img, &c);
			ioctl(c.fd, VIDIOC_QBUF, &vb);
			frames++;
		}
	}
	if (graphics)
		ioctl(0, KDSETMODE, KD_TEXT);
	if (tty)
		tcsetattr(0, TCSANOW, &old);
	munmap(fb, f.smem_len);
	close(fd);
	cam_close(&c);
	printf("\033[2J\033[H\033[1m%s\033[0m\n\n", HEADER);
	if (!frames) {
		verdict(stdout, V_FAIL, "Camera", "%s: no picture arrives", name);
		return V_FAIL;
	}
	int a = ask_char("Is the image OK: sharp, right colours, no lines or dead spots? [y/n] ");
	enum verdict r = a == 'y' ? V_PASS : a == 'n' ? V_FAIL : V_INCONCLUSIVE;
	verdict(stdout, r, "Camera", "%s: %d frames shown; %s", name, frames,
		r == V_PASS ? "operator: image OK" : r == V_FAIL ? "operator: image faulty" : "no answer");
	return r;
}
