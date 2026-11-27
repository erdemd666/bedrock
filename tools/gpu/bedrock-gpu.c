/* bedrock-gpu: headless GPU compute stress and video memory test.
 *
 *   bedrock-gpu [--secs N] [--vram-mib M] /dev/dri/renderD128 | software
 *
 * Runs on a render node through GBM + EGL with an OpenGL ES 3.1 context (no
 * display needed). "software" uses the surfaceless platform and the software
 * renderer, only to check that the test itself works. Prints one line per
 * result for bedrock to parse:
 *   renderer <GL_RENDERER>
 *   software 0|1
 *   stress rounds=R errors=E secs=S
 *   vram mib=M errors=E
 *   fail <why>
 * Exit 0 when the tests ran (errors are reported, not an exit status). */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>
#include <fcntl.h>
#include <gbm.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "stressref.h"

#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif

static volatile sig_atomic_t stop;
static void on_int(int s) { (void)s; stop = 1; }

static int fail(const char *why)
{
	printf("fail %s\n", why);
	return 1;
}

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

/* ---- stress: integer hash chain plus float products of small integers.
 * Every float value stays an integer below 2^24, so it is exact on any IEEE
 * hardware with or without fused multiply-add: the CPU computes the same bits. */

#define STRESS_SRC \
	"#version 310 es\n" \
	"layout(local_size_x = 64) in;\n" \
	"layout(std430, binding = 0) readonly buffer Ref { uint ref[]; };\n" \
	"layout(std430, binding = 1) buffer Out { uint res[]; };\n" \
	"layout(std430, binding = 2) buffer Err { uint err; };\n" \
	"uniform uint iters;\n" \
	"void main() {\n" \
	"	uint i = gl_GlobalInvocationID.x;\n" \
	"	uint x = i * 2654435761u + 12345u;\n" \
	"	float f = 0.0;\n" \
	"	for (uint k = 0u; k < iters; k++) {\n" \
	"		x ^= x << 13; x ^= x >> 17; x ^= x << 5;\n" \
	"		x = x * 1664525u + 1013904223u;\n" \
	"		f += float(x & 1023u) * float((x >> 10) & 1023u);\n" \
	"		if (f >= 8388608.0) f -= 8388608.0;\n" \
	"	}\n" \
	"	uint r = x ^ uint(f);\n" \
	"	res[i] = r;\n" \
	"	if (r != ref[i]) atomicAdd(err, 1u);\n" \
	"}\n"

/* ---- video memory: write a pattern into every chunk, then verify every chunk,
 * so data sits while the rest is written and address faults show up. ---- */

#define VRAM_SRC \
	"#version 310 es\n" \
	"layout(local_size_x = 256) in;\n" \
	"layout(std430, binding = 0) buffer Mem { uint m[]; };\n" \
	"layout(std430, binding = 2) buffer Err { uint err; };\n" \
	"uniform uint base, mode, seed, check, words;\n" \
	"uint pat(uint a) {\n" \
	"	if (mode == 0u) return 1u << ((a + seed) & 31u);\n" /* walking ones */ \
	"	if (mode == 1u) return a ^ seed;\n" /* address in address */ \
	"	uint x = (a ^ seed) * 2654435761u; x ^= x >> 15; x *= 2246822519u; return x ^ (x >> 13);\n" \
	"}\n" \
	"void main() {\n" \
	"	uint n = gl_NumWorkGroups.x * 256u;\n" \
	"	for (uint i = gl_GlobalInvocationID.x; i < words; i += n) {\n" \
	"		if (check == 0u) m[i] = pat(base + i);\n" \
	"		else if (m[i] != pat(base + i)) atomicAdd(err, 1u);\n" \
	"	}\n" \
	"}\n"

static GLuint program(const char *src)
{
	GLuint s = glCreateShader(GL_COMPUTE_SHADER), p;
	GLint ok;
	char log[200];
	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		glGetShaderInfoLog(s, sizeof log, NULL, log);
		log[strcspn(log, "\n")] = 0;
		printf("fail shader: %s\n", log);
		return 0;
	}
	p = glCreateProgram();
	glAttachShader(p, s);
	glLinkProgram(p);
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	return ok ? p : 0;
}

static GLuint buffer(GLsizeiptr size, const void *data)
{
	GLuint b;
	glGenBuffers(1, &b);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, b);
	glBufferData(GL_SHADER_STORAGE_BUFFER, size, data, GL_STATIC_DRAW); /* STATIC: drivers place it in VRAM */
	return b;
}

static uint32_t read_err(GLuint err, int reset)
{
	uint32_t v = 0, zero = 0;
	glMemoryBarrier(GL_ALL_BARRIER_BITS);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, err);
	void *p = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, 4, GL_MAP_READ_BIT);
	if (p)
		memcpy(&v, p, 4);
	else
		v = UINT32_MAX;
	glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
	if (reset)
		glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, 4, &zero);
	return v;
}

static int stress(int secs, int small)
{
	const uint32_t n = small ? 1 << 12 : 1 << 18, iters = 512;
	uint32_t *ref = malloc(n * 4), *got;
	GLuint prog = program(STRESS_SRC);
	if (!ref || !prog)
		return fail("cannot set up the stress test");
	for (uint32_t i = 0; i < n; i++)
		ref[i] = stress_ref(i, iters);
	GLuint bref = buffer(n * 4, ref), bout = buffer(n * 4, NULL), berr = buffer(4, NULL);
	uint32_t zero = 0;
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, berr);
	glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, 4, &zero);
	glUseProgram(prog);
	glUniform1ui(glGetUniformLocation(prog, "iters"), iters);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, bref);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, bout);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, berr);

	long long rounds = 0, errors = 0;
	int batch = 1;
	double t0 = now(), t;
	do {
		double b0 = now();
		/* A cleared result buffer proves each batch really ran: stale data cannot pass. */
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, bout);
		glBufferData(GL_SHADER_STORAGE_BUFFER, n * 4, NULL, GL_STATIC_DRAW);
		for (int k = 0; k < batch; k++)
			glDispatchCompute(n / 64, 1, 1);
		glMemoryBarrier(GL_ALL_BARRIER_BITS);
		glFinish();
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, bout);
		got = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, n * 4, GL_MAP_READ_BIT);
		if (!got)
			return fail("cannot read results back");
		for (uint32_t i = 0; i < n; i++)
			errors += got[i] != ref[i];
		glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
		errors += read_err(berr, 1);
		rounds += batch;
		t = now();
		/* Aim for about a quarter second per batch: long enough to keep the GPU
		 * busy, short enough that no single submission trips a GPU watchdog. */
		double bt = t - b0;
		if (bt < 0.1 && batch < 4096)
			batch *= 2;
		else if (bt > 0.5 && batch > 1)
			batch /= 2;
	} while (t - t0 < secs && !stop);
	printf("stress rounds=%lld errors=%lld secs=%d\n", rounds, errors, (int)(t - t0 + 0.5));
	free(ref);
	return 0;
}

static int vram(long long mib)
{
	GLint maxblock = 0;
	glGetIntegerv(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &maxblock);
	long long chunk = 64LL << 20;
	if (maxblock > 0 && maxblock < chunk)
		chunk = maxblock & ~0xfffffLL;
	if (chunk > mib << 20)
		chunk = mib << 20;
	int want = (int)((mib << 20) / chunk), have = 0;
	GLuint *b = calloc(want ? want : 1, sizeof *b), prog = program(VRAM_SRC), err = buffer(4, NULL);
	if (!b || !prog || chunk <= 0)
		return fail("cannot set up the video memory test");
	uint32_t words = chunk / 4, zero = 0;
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, err);
	glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, 4, &zero);
	glUseProgram(prog);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, err);
	GLint u_base = glGetUniformLocation(prog, "base"), u_mode = glGetUniformLocation(prog, "mode"),
	      u_seed = glGetUniformLocation(prog, "seed"), u_check = glGetUniformLocation(prog, "check");
	glUniform1ui(glGetUniformLocation(prog, "words"), words);
	GLuint groups = words / 256 / 16;

	/* Reserve chunks; a write per chunk makes the driver really place it. */
	glUniform1ui(u_mode, 1);
	glUniform1ui(u_seed, 0);
	glUniform1ui(u_check, 0);
	while (glGetError() != GL_NO_ERROR)
		;
	for (; have < want && !stop; have++) {
		b[have] = buffer(chunk, NULL);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, b[have]);
		glUniform1ui(u_base, (uint32_t)((uint64_t)have * words));
		glDispatchCompute(groups, 1, 1);
		glFinish();
		if (glGetError() != GL_NO_ERROR) {
			glDeleteBuffers(1, &b[have]);
			break;
		}
	}
	if (!have)
		return fail("no video memory could be reserved");

	/* Self-check: verifying chunk 0 against the wrong seed must flag every word,
	 * or a broken check could report a clean result it never measured. */
	glUniform1ui(u_check, 1);
	glUniform1ui(u_seed, 1);
	glUniform1ui(u_base, 0);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, b[0]);
	glDispatchCompute(groups, 1, 1);
	if (read_err(err, 1) != words)
		return fail("video memory check does not detect errors on this driver");

	long long errors = 0;
	static const uint32_t seeds[3] = {0, 0, 0x9e3779b9u};
	for (int mode = 0; mode < 3 && !stop; mode++)
		for (int check = 0; check < 2; check++) {
			glUniform1ui(u_mode, mode);
			glUniform1ui(u_seed, seeds[mode]);
			glUniform1ui(u_check, check);
			for (int c = 0; c < have; c++) {
				glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, b[c]);
				glUniform1ui(u_base, (uint32_t)((uint64_t)c * words));
				glDispatchCompute(groups, 1, 1);
				glFinish();
			}
			if (check)
				errors += read_err(err, 1);
		}
	printf("vram mib=%lld errors=%lld\n", (long long)have * chunk >> 20, errors);
	glDeleteBuffers(have, b);
	free(b);
	return 0;
}

int main(int argc, char **argv)
{
	int secs = 60;
	long long mib = 0;
	const char *dev = NULL;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--secs") && i + 1 < argc)
			secs = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--vram-mib") && i + 1 < argc)
			mib = atoll(argv[++i]);
		else
			dev = argv[i];
	}
	if (!dev) {
		fprintf(stderr, "usage: bedrock-gpu [--secs N] [--vram-mib M] /dev/dri/renderD128|software\n");
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	signal(SIGINT, on_int);
	setenv("MESA_SHADER_CACHE_DISABLE", "true", 1); /* the system runs from RAM */
	int sw = !strcmp(dev, "software");
	EGLDisplay dpy;
	if (sw) {
		setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);
		dpy = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
	} else {
		int fd = open(dev, O_RDWR | O_CLOEXEC);
		struct gbm_device *gbm = fd >= 0 ? gbm_create_device(fd) : NULL;
		if (!gbm)
			return fail("cannot open the graphics device");
		dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, NULL);
	}
	static const EGLint ctxattr[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE};
	EGLContext ctx;
	if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL))
		return fail("no graphics driver for this device");
	if (!eglBindAPI(EGL_OPENGL_ES_API) ||
	    (ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctxattr)) == EGL_NO_CONTEXT ||
	    !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
		return fail("the driver has no OpenGL ES 3.1 compute support for this card");
	const char *r = (const char *)glGetString(GL_RENDERER);
	printf("renderer %s\n", r ? r : "unknown");
	int soft = r && (strstr(r, "llvmpipe") || strstr(r, "softpipe") || strstr(r, "Software"));
	printf("software %d\n", soft);
	if (soft && !sw)
		return 0; /* bedrock says why: no hardware driver, nothing to test */
	if (stress(secs, sw))
		return 1;
	if (mib > 0 && !stop && vram(mib))
		return 1;
	eglTerminate(dpy);
	return 0;
}
