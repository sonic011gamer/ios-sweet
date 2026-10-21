/*
 * glprobe: step-by-step check of Mesa Freedreno (libOSMesa, t290/mesa) on
 * iPadOS without linking it: dlopen + dlsym, a mark after every step on
 * /dev/console (stdout) and in /private/var/tmp/glprobe.log (fsync'ed), and
 * an alarm that names the step a hang happened in. From 8 s on a watchdog
 * thread samples the main thread every 15 s (pc/lr, frame chain, run state,
 * CPU time) to tell a spin from a sleep in the kernel.
 */
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int logfd = -1;
static struct timespec t0;
static const char *volatile step = "start";

static void
mark(const char *fmt, ...)
{
	char buf[512];
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	double dt = (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9;
	int h = snprintf(buf, sizeof(buf), "[%7.2f] ", dt);
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf + h, sizeof(buf) - 1 - h, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	n += h;
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
	mark("glprobe: TIMEOUT in step '%s', backtrace:", step);
	void *bt[64];
	int n = backtrace(bt, 64);
	for (int i = 0; i < n; i++) {
		Dl_info di;
		if (dladdr(bt[i], &di) && di.dli_fname) {
			const char *base = strrchr(di.dli_fname, '/');
			mark("glprobe:   %2d %s+%#lx (%s)", i, base ? base + 1 : di.dli_fname,
			    (unsigned long)((char *)bt[i] - (char *)di.dli_fbase),
			    di.dli_sname ? di.dli_sname : "?");
		} else {
			mark("glprobe:   %2d %p", i, bt[i]);
		}
	}
	_exit(3);
}

static thread_t main_thread;

static void
where(const char *tag, uint64_t a)
{
	Dl_info di;
	if (a && dladdr((void *)(uintptr_t)a, &di) && di.dli_fname) {
		const char *base = strrchr(di.dli_fname, '/');
		mark("glprobe:   %s %#llx %s+%#lx (%s+%#lx)", tag, (unsigned long long)a,
		    base ? base + 1 : di.dli_fname, (unsigned long)(a - (uintptr_t)di.dli_fbase),
		    di.dli_sname ? di.dli_sname : "?",
		    di.dli_saddr ? (unsigned long)(a - (uintptr_t)di.dli_saddr) : 0ul);
	} else {
		mark("glprobe:   %s %#llx", tag, (unsigned long long)a);
	}
}

static void *
watchdog(void *arg)
{
	(void)arg;
	sleep(8);
	for (int k = 0; k < 60; k++) {
		thread_basic_info_data_t bi;
		mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
		thread_info(main_thread, THREAD_BASIC_INFO, (thread_info_t)&bi, &n);
		arm_thread_state64_t st;
		n = ARM_THREAD_STATE64_COUNT;
		kern_return_t kr = thread_get_state(main_thread, ARM_THREAD_STATE64, (thread_state_t)&st, &n);
		mark("glprobe: sample %d step '%s': run_state %d flags %#x cpu %d user %d.%06d sys %d.%06d kr %#x",
		    k, step, bi.run_state, bi.flags, bi.cpu_usage, bi.user_time.seconds,
		    bi.user_time.microseconds, bi.system_time.seconds, bi.system_time.microseconds, kr);
		if (!kr) {
			where("pc", arm_thread_state64_get_pc(st));
			where("lr", arm_thread_state64_get_lr(st));
			uint64_t *fp = (uint64_t *)(uintptr_t)arm_thread_state64_get_fp(st);
			for (int d = 0; d < 6 && fp && ((uintptr_t)fp & 7) == 0; d++) {
				where("fr", fp[1]);
				if ((uint64_t *)(uintptr_t)fp[0] <= fp)
					break;
				fp = (uint64_t *)(uintptr_t)fp[0];
			}
		}
		sleep(15);
	}
	return NULL;
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
	clock_gettime(CLOCK_MONOTONIC, &t0);
	alarm(900);
	main_thread = mach_thread_self();
	pthread_t wd;
	pthread_create(&wd, NULL, watchdog, NULL);
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
