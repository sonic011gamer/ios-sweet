/*
 * t290launch LIB PROGRAM [ARG...]: exec PROGRAM (same pid, so launchd keeps
 * tracking the job) with DYLD_INSERT_LIBRARIES=LIB. launchd's job
 * EnvironmentVariables did not get DYLD_* to backboardd; this keeps the
 * system binary untouched and only changes its launchd job. Writes one line
 * into the kernel log (SweetGPU selector 7, kept by the pstore). If
 * /private/var/tmp/t290launch.off exists, PROGRAM runs without LIB.
 */
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <CoreFoundation/CoreFoundation.h>

extern const mach_port_t kIOMasterPortDefault;
CFMutableDictionaryRef IOServiceMatching(const char *name);
mach_port_t IOServiceGetMatchingService(mach_port_t main, CFDictionaryRef matching);
kern_return_t IOServiceOpen(mach_port_t service, mach_port_t task, uint32_t type, mach_port_t *conn);
kern_return_t IOServiceClose(mach_port_t conn);
kern_return_t IOObjectRelease(mach_port_t object);
kern_return_t IOConnectCallStructMethod(mach_port_t conn, uint32_t sel, const void *in, size_t inSize,
    void *out, size_t *outSize);

static void
klog(const char *msg)
{
	mach_port_t svc = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("SweetGPU"));
	mach_port_t conn = 0;
	if (!svc)
		return;
	if (IOServiceOpen(svc, mach_task_self(), 0, &conn) == KERN_SUCCESS) {
		IOConnectCallStructMethod(conn, 7, msg, strlen(msg), NULL, NULL);
		IOServiceClose(conn);
	}
	IOObjectRelease(svc);
}

int
main(int argc, char **argv)
{
	char msg[512];
	if (argc < 3) {
		fprintf(stderr, "usage: t290launch LIB PROGRAM [ARG...]\n");
		return 64;
	}
	int off = access("/private/var/tmp/t290launch.off", F_OK) == 0;
	if (!off)
		setenv("DYLD_INSERT_LIBRARIES", argv[1], 1);
	snprintf(msg, sizeof(msg), "t290launch: exec %s %s %s", argv[2],
	    off ? "WITHOUT" : "with", argv[1]);
	klog(msg);
	execv(argv[2], argv + 2);
	snprintf(msg, sizeof(msg), "t290launch: exec %s failed", argv[2]);
	klog(msg);
	return 127;
}
