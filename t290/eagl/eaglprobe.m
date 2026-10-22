/*
 * t290eagl probe: injected into backboardd (DYLD_INSERT_LIBRARIES in its
 * launchd job) to check that injection works and to log what CoreAnimation
 * asks of EAGLContext before the real shim replaces it. Log:
 * /private/var/tmp/t290eagl.log (backboardd runs as mobile).
 */
#import <Foundation/Foundation.h>
#include <fcntl.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int logfd = -1;
static mach_port_t klog;        /* SweetGPU user client: selector 7 = kernel log */

/* IOKit, declared here (the iOS SDK has no IOKit headers) */
extern const mach_port_t kIOMasterPortDefault;
CFMutableDictionaryRef IOServiceMatching(const char *name);
mach_port_t IOServiceGetMatchingService(mach_port_t main, CFDictionaryRef matching);
kern_return_t IOServiceOpen(mach_port_t service, mach_port_t task, uint32_t type, mach_port_t *conn);
kern_return_t IOObjectRelease(mach_port_t object);
kern_return_t IOConnectCallStructMethod(mach_port_t conn, uint32_t sel, const void *in, size_t inSize,
    void *out, size_t *outSize);

static void
klog_open(void)
{
	mach_port_t svc = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("SweetGPU"));
	if (svc) {
		IOServiceOpen(svc, mach_task_self(), 0, &klog);
		IOObjectRelease(svc);
	}
}

static void
plog(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if (n > (int)sizeof(buf) - 2)
		n = sizeof(buf) - 2;
	if (klog)
		IOConnectCallStructMethod(klog, 7, buf, n, NULL, NULL);
	buf[n++] = '\n';
	if (logfd >= 0) {
		write(logfd, buf, n);
		fsync(logfd);
	}
}

static IMP orig_initWithAPI, orig_initWithAPIProps, orig_macro, orig_setCurrent;

static id
my_initWithAPI(id self, SEL sel, unsigned api)
{
	id r = ((id(*)(id, SEL, unsigned))orig_initWithAPI)(self, sel, api);
	plog("t290eagl: initWithAPI:%u -> %p", api, r);
	return r;
}

static id
my_initWithAPIProps(id self, SEL sel, unsigned api, id props)
{
	plog("t290eagl: initWithAPI:%u properties:%s", api,
	    props ? [[props description] UTF8String] : "nil");
	id r = ((id(*)(id, SEL, unsigned, id))orig_initWithAPIProps)(self, sel, api, props);
	plog("t290eagl:   -> %p", r);
	return r;
}

static void *
my_macro(id self, SEL sel)
{
	void *r = ((void *(*)(id, SEL))orig_macro)(self, sel);
	plog("t290eagl: getMacroContextPrivate %p -> %p", self, r);
	return r;
}

static BOOL
my_setCurrent(id cls, SEL sel, id ctx)
{
	BOOL r = ((BOOL(*)(id, SEL, id))orig_setCurrent)(cls, sel, ctx);
	static int n;
	if (n++ < 8)
		plog("t290eagl: setCurrentContext:%p -> %d", ctx, r);
	return r;
}

static IMP
hook(Class c, const char *name, IMP imp, BOOL meta)
{
	Class k = meta ? object_getClass(c) : c;
	Method m = class_getInstanceMethod(k, sel_registerName(name));
	if (!m) {
		plog("t290eagl: no %c[EAGLContext %s]", meta ? '+' : '-', name);
		return NULL;
	}
	return method_setImplementation(m, imp);
}

__attribute__((constructor)) static void
t290eagl_init(void)
{
	char name[64] = "?";
	const char *p = getprogname();
	if (p)
		strlcpy(name, p, sizeof(name));
	klog_open();
	logfd = open("/private/var/tmp/t290eagl.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
	plog("t290eagl: loaded into %s pid %d uid %d", name, getpid(), getuid());
	Class c = objc_getClass("EAGLContext");
	plog("t290eagl: EAGLContext class %p", c);
	if (!c)
		return;
	orig_initWithAPI = hook(c, "initWithAPI:", (IMP)my_initWithAPI, NO);
	orig_initWithAPIProps = hook(c, "initWithAPI:properties:", (IMP)my_initWithAPIProps, NO);
	orig_macro = hook(c, "getMacroContextPrivate", (IMP)my_macro, NO);
	orig_setCurrent = hook(c, "setCurrentContext:", (IMP)my_setCurrent, YES);
	unsigned n = 0;
	Method *ms = class_copyMethodList(c, &n);
	plog("t290eagl: EAGLContext has %u methods", n);
	for (unsigned i = 0; i < n; i++)
		plog("t290eagl:   -%s %s", sel_getName(method_getName(ms[i])), method_getTypeEncoding(ms[i]));
	free(ms);
}
