/*
 * glprobe: step-by-step check of Mesa Freedreno (libOSMesa, t290/mesa) on
 * iPadOS without linking it: dlopen + dlsym, a mark after every step on
 * /dev/console (stdout) and in /private/var/tmp/glprobe.log (fsync'ed), and
 * an alarm that names the step a hang happened in.
 */
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int logfd = -1;
static const char *volatile step = "start";

static void
mark(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if (n > (int)sizeof(buf) - 2)
		n = sizeof(buf) - 2;
	buf[n++] = '\n';
	write(1, buf, n);
	if (logfd >= 0) {
		write(logfd, buf, n);
		fsync(logfd);
	}
}

static void
on_alarm(int sig)
{
	(void)sig;
	mark("glprobe: TIMEOUT in step '%s'", step);
	_exit(3);
}

typedef void *(*create_fn)(unsigned, int, int, int, void *);
typedef unsigned char (*make_fn)(void *, void *, unsigned, int, int);
typedef const unsigned char *(*str_fn)(unsigned);
typedef void (*v_fn)(void);
typedef void (*clearc_fn)(float, float, float, float);
typedef void (*u_fn)(unsigned);
typedef void (*f2_fn)(float, float);
typedef void (*f3_fn)(float, float, float);
typedef unsigned (*err_fn)(void);

#define STEP(s) do { step = s; mark("glprobe: %s", s); } while (0)
#define SYM(t, n) t n = (t)dlsym(h, #n); if (!n) { mark("glprobe: no symbol " #n); return 2; }

int
main(void)
{
	logfd = open("/private/var/tmp/glprobe.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	signal(SIGALRM, on_alarm);
	alarm(60);
	setenv("FD_MESA_DEBUG", "msgs", 1);
	setenv("MESA_LOG_FILE", "/private/var/tmp/mesa.log", 1);
	STEP("dlopen");
	void *h = dlopen("/usr/lib/libOSMesa.8.dylib", RTLD_NOW | RTLD_LOCAL);
	if (!h) {
		mark("glprobe: dlopen failed: %s", dlerror());
		return 1;
	}
	SYM(create_fn, OSMesaCreateContextExt)
	SYM(make_fn, OSMesaMakeCurrent)
	SYM(str_fn, glGetString)
	SYM(clearc_fn, glClearColor)
	SYM(u_fn, glClear)
	SYM(u_fn, glBegin)
	SYM(v_fn, glEnd)
	SYM(f3_fn, glColor3f)
	SYM(f2_fn, glVertex2f)
	SYM(v_fn, glFinish)
	SYM(err_fn, glGetError)

	STEP("OSMesaCreateContextExt");
	void *ctx = OSMesaCreateContextExt(0x1908 /* OSMESA_RGBA */, 16, 0, 0, NULL);
	mark("glprobe: ctx %p", ctx);
	if (!ctx)
		return 1;
	static unsigned char buf[64 * 64 * 4];
	STEP("OSMesaMakeCurrent");
	if (!OSMesaMakeCurrent(ctx, buf, 0x1401 /* GL_UNSIGNED_BYTE */, 64, 64)) {
		mark("glprobe: MakeCurrent failed");
		return 1;
	}
	STEP("glGetString");
	mark("glprobe: GL_RENDERER %s / GL_VERSION %s", glGetString(0x1F01), glGetString(0x1F02));
	STEP("clear + triangle");
	glClearColor(0, 0, 0.5f, 1);
	glClear(0x4000);
	glBegin(4);
	glColor3f(1, 0, 0);
	glVertex2f(-0.8f, -0.8f);
	glColor3f(0, 1, 0);
	glVertex2f(0.8f, -0.8f);
	glColor3f(0, 0, 1);
	glVertex2f(0, 0.8f);
	glEnd();
	STEP("glFinish");
	glFinish();
	unsigned char *c = buf, *m = buf + (32 * 64 + 32) * 4;
	mark("glprobe: gl error %#x, corner %u,%u,%u centre %u,%u,%u", glGetError(), c[0], c[1], c[2],
	    m[0], m[1], m[2]);
	STEP("done");
	return 0;
}
