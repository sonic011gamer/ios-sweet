/*
 * t290launch LIB PROGRAM [ARG...]: exec PROGRAM (same pid, so launchd keeps
 * tracking the job) with DYLD_INSERT_LIBRARIES=LIB. launchd's job
 * EnvironmentVariables did not get DYLD_* to backboardd; this keeps the
 * system binary untouched and only changes its launchd job. Writes one line
 * into the kernel log (SweetGPU selector 7, kept by the pstore). If
 * /private/var/tmp/t290launch.off exists, PROGRAM runs without LIB.
 */
#include <mach/mach.h>
#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <CoreFoundation/CoreFoundation.h>

extern char **environ;

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
	const char *test = NULL;
	if (argc > 2 && strcmp(argv[1], "--test") == 0) {
		test = argv[2];
		argc -= 2;
		argv += 2;
	}
	if (argc < 3) {
		fprintf(stderr, "usage: t290launch [--test TEST_PROGRAM] LIB PROGRAM [ARG...]\n");
		return 64;
	}
	int off = access("/private/var/tmp/t290launch.off", F_OK) == 0;
	if (!off)
		setenv("DYLD_INSERT_LIBRARIES", argv[1], 1);
	else
		unsetenv("DYLD_INSERT_LIBRARIES");
	if (test && !off) {
		pid_t child;
		char *test_args[] = {(char *)test, NULL};
		int status = 0;
		int error = posix_spawn(&child, test, NULL, NULL, test_args, environ);
		pid_t waited = -1;
		if (!error) {
			do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
		}
		int passed = !error && waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
		snprintf(msg, sizeof(msg), "t290launch: GPU test %s (spawn %d status %#x); %s", test,
		    error, status, passed ? "PASS, enabling Mesa for daemon" : "FAILED, using system renderer");
		klog(msg);
		if (!passed) {
			unsetenv("DYLD_INSERT_LIBRARIES");
			off = 1;
		}
	}
	snprintf(msg, sizeof(msg), "t290launch: exec %s %s %s", argv[2],
	    off ? "WITHOUT" : "with", argv[1]);
	klog(msg);
	execv(argv[2], argv + 2);
	snprintf(msg, sizeof(msg), "t290launch: exec %s failed", argv[2]);
	klog(msg);
	return 127;
}
