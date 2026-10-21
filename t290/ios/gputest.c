/*
 * gputest: the SweetGPU user client (xnu iokit/Kernel/arm/SweetGPU.cpp) from
 * iPadOS user space. Allocates two GPU buffers, maps them, writes an
 * indirect buffer with CP_MEM_WRITE into the second, submits it, waits for
 * the fence and checks the marker; then times 1000 empty-ish submissions.
 * Output: stdout and /private/var/tmp/gputest.log (fetch with t290-xfer.ps1).
 *
 * Built for iOS arm64 by .github/workflows/ios-tools.yml. The iOS SDK has
 * no IOKit headers: the few calls are declared here.
 */
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <CoreFoundation/CoreFoundation.h>

typedef mach_port_t io_object_t;
typedef io_object_t io_service_t, io_connect_t;
extern const mach_port_t kIOMasterPortDefault;
CFMutableDictionaryRef IOServiceMatching(const char *name);
io_service_t IOServiceGetMatchingService(mach_port_t main, CFDictionaryRef matching);
kern_return_t IOServiceOpen(io_service_t service, task_port_t task, uint32_t type, io_connect_t *connect);
kern_return_t IOServiceClose(io_connect_t connect);
kern_return_t IOObjectRelease(io_object_t object);
kern_return_t IOConnectCallScalarMethod(io_connect_t c, uint32_t sel, const uint64_t *in, uint32_t nin,
    uint64_t *out, uint32_t *nout);
kern_return_t IOConnectMapMemory64(io_connect_t c, uint32_t type, task_port_t task,
    mach_vm_address_t *addr, mach_vm_size_t *size, uint32_t options);
#define kIOMapAnywhere 1

static FILE *logf;

static void
say(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	if (logf) {
		va_start(ap, fmt);
		vfprintf(logf, fmt, ap);
		va_end(ap);
		fflush(logf);
	}
}

static unsigned
odd_parity(unsigned v)
{
	v ^= v >> 16;
	v ^= v >> 8;
	v ^= v >> 4;
	v &= 0xf;
	return (~0x6996u >> v) & 1;
}

static uint32_t
pkt7(unsigned op, unsigned cnt)
{
	return 0x70000000u | (cnt & 0x3fff) | (odd_parity(cnt) << 15) |
	       ((op & 0x7f) << 16) | (odd_parity(op) << 23);
}

static double
ms_since(uint64_t t0)
{
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	return (double)(mach_absolute_time() - t0) * tb.numer / tb.denom / 1e6;
}

int
main(void)
{
	logf = fopen("/private/var/tmp/gputest.log", "w");
	io_service_t svc = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("SweetGPU"));
	if (!svc) {
		say("gputest: no SweetGPU service\n");
		return 1;
	}
	io_connect_t c = 0;
	kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &c);
	IOObjectRelease(svc);
	if (kr) {
		say("gputest: IOServiceOpen %#x\n", kr);
		return 1;
	}
	uint64_t out[4] = { 0 };
	uint32_t n = 4;
	kr = IOConnectCallScalarMethod(c, 0, NULL, 0, out, &n);
	say("gputest: info kr=%#x chip %#llx gmem %#llx ready %llu window %#llx\n", kr, out[0], out[1], out[2], out[3]);

	uint64_t ib_h[3], tg_h[3], sz = 65536;
	n = 3;
	kr = IOConnectCallScalarMethod(c, 1, &sz, 1, ib_h, &n);
	n = 3;
	kr |= IOConnectCallScalarMethod(c, 1, &sz, 1, tg_h, &n);
	if (kr) {
		say("gputest: alloc %#x\n", kr);
		return 1;
	}
	mach_vm_address_t ib_va = 0, tg_va = 0;
	mach_vm_size_t ib_sz = 0, tg_sz = 0;
	kr = IOConnectMapMemory64(c, (uint32_t)ib_h[0], mach_task_self(), &ib_va, &ib_sz, kIOMapAnywhere);
	kr |= IOConnectMapMemory64(c, (uint32_t)tg_h[0], mach_task_self(), &tg_va, &tg_sz, kIOMapAnywhere);
	say("gputest: buffers ib gpu %#llx cpu %#llx, target gpu %#llx cpu %#llx (map kr=%#x)\n",
	    ib_h[1], (unsigned long long)ib_va, tg_h[1], (unsigned long long)tg_va, kr);
	if (kr) {
		return 1;
	}
	volatile uint32_t *ib = (volatile uint32_t *)ib_va, *tg = (volatile uint32_t *)tg_va;
	tg[0] = 0;
	unsigned k = 0;
	ib[k++] = pkt7(61, 3);                          /* CP_MEM_WRITE */
	ib[k++] = (uint32_t)tg_h[1];
	ib[k++] = (uint32_t)(tg_h[1] >> 32);
	ib[k++] = 0x600dc0de;
	__sync_synchronize();
	uint64_t t0 = mach_absolute_time();
	uint64_t in[2] = { ib_h[1], k }, seq = 0;
	n = 1;
	kr = IOConnectCallScalarMethod(c, 3, in, 2, &seq, &n);
	uint64_t win[2] = { seq, 1000 };
	kern_return_t kw = IOConnectCallScalarMethod(c, 4, win, 2, NULL, NULL);
	say("gputest: submit kr=%#x seq %llu, wait kr=%#x, marker %#x -> %s in %.3f ms\n", kr, seq, kw,
	    tg[0], tg[0] == 0x600dc0de ? "OK, THE GPU RAN OUR BUFFER" : "FAILED", ms_since(t0));

	t0 = mach_absolute_time();
	for (int i = 0; i < 1000; i++) {
		ib[3] = (uint32_t)i;
		__sync_synchronize();
		n = 1;
		IOConnectCallScalarMethod(c, 3, in, 2, &seq, &n);
		win[0] = seq;
		IOConnectCallScalarMethod(c, 4, win, 2, NULL, NULL);
	}
	say("gputest: 1000 submit+wait round trips in %.1f ms, last marker %u\n", ms_since(t0), tg[0]);
	IOServiceClose(c);
	return 0;
}
