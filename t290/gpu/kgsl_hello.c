/*
 * Hello, Adreno 504: one PM4 command through Android's kgsl driver.
 *
 * kgsl powers the GPU, loads the CP microcode and the zap shader and owns
 * the SMMU, so a user program can learn the command language safely: a
 * fault only resets the GPU. This writes CP_MEM_WRITE (opcode 61) into an
 * indirect buffer, submits it with IOCTL_KGSL_GPU_COMMAND, waits for the
 * timestamp and checks that the GPU stored the value.
 *
 * Build: aarch64-linux-gnu-gcc -static -O2 -I../../research/kgsl kgsl_hello.c
 * Run on the tablet as root: /data/local/tmp/kgsl_hello
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/types.h>
#include "msm_kgsl.h"

#define CP_MEM_WRITE	61

static unsigned odd_parity(unsigned v)
{
	v ^= v >> 16;
	v ^= v >> 8;
	v ^= v >> 4;
	v &= 0xf;
	return (~0x6996u >> v) & 1;
}

/* a5xx type-7 packet header (freedreno adreno_pm4: pm4_pkt7_hdr) */
static uint32_t pkt7(unsigned opcode, unsigned cnt)
{
	return 0x70000000u | (cnt & 0x3fff) | (odd_parity(cnt) << 15) |
	       ((opcode & 0x7f) << 16) | (odd_parity(opcode) << 23);
}

struct gbuf {
	unsigned id;
	uint64_t gpuaddr;
	uint32_t *cpu;
	size_t size;
};

static int gpu_alloc(int fd, size_t size, struct gbuf *b)
{
	struct kgsl_gpumem_alloc_id a = { .size = size, .flags = 0 };
	if (ioctl(fd, IOCTL_KGSL_GPUMEM_ALLOC_ID, &a)) {
		perror("GPUMEM_ALLOC_ID");
		return -1;
	}
	b->id = a.id;
	b->gpuaddr = a.gpuaddr;
	b->size = a.mmapsize ? a.mmapsize : size;
	b->cpu = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)a.id << 12);
	if (b->cpu == MAP_FAILED) {
		perror("mmap");
		return -1;
	}
	return 0;
}

int main(void)
{
	int fd = open("/dev/kgsl-3d0", O_RDWR);
	if (fd < 0) {
		perror("open /dev/kgsl-3d0");
		return 1;
	}

	struct kgsl_devinfo info;
	struct kgsl_device_getproperty gp = {
		.type = KGSL_PROP_DEVICE_INFO, .value = &info, .sizebytes = sizeof(info),
	};
	memset(&info, 0, sizeof(info));
	if (ioctl(fd, IOCTL_KGSL_DEVICE_GETPROPERTY, &gp) == 0)
		printf("device: chip_id 0x%08x mmu %u gmem 0x%x+0x%zx\n", info.chip_id,
		       info.mmu_enabled, (unsigned)info.gmem_gpubaseaddr, (size_t)info.gmem_sizebytes);

	struct kgsl_drawctxt_create ctx = {
		.flags = KGSL_CONTEXT_PREAMBLE | KGSL_CONTEXT_NO_GMEM_ALLOC,
	};
	if (ioctl(fd, IOCTL_KGSL_DRAWCTXT_CREATE, &ctx)) {
		perror("DRAWCTXT_CREATE");
		return 1;
	}
	printf("context %u\n", ctx.drawctxt_id);

	struct gbuf cmd, target;
	if (gpu_alloc(fd, 4096, &cmd) || gpu_alloc(fd, 4096, &target))
		return 1;
	printf("cmd buffer gpu 0x%llx, target gpu 0x%llx\n",
	       (unsigned long long)cmd.gpuaddr, (unsigned long long)target.gpuaddr);

	target.cpu[0] = 0x11111111;
	unsigned n = 0;
	cmd.cpu[n++] = pkt7(CP_MEM_WRITE, 3);
	cmd.cpu[n++] = (uint32_t)target.gpuaddr;
	cmd.cpu[n++] = (uint32_t)(target.gpuaddr >> 32);
	cmd.cpu[n++] = 0xc0ffee01;
	__sync_synchronize();

	struct kgsl_command_object ib = {
		.gpuaddr = cmd.gpuaddr, .size = n * 4, .flags = KGSL_CMDLIST_IB, .id = cmd.id,
	};
	struct kgsl_gpu_command submit = {
		.flags = 0,
		.cmdlist = (uint64_t)(uintptr_t)&ib,
		.cmdsize = sizeof(ib),
		.numcmds = 1,
		.context_id = ctx.drawctxt_id,
	};
	if (ioctl(fd, IOCTL_KGSL_GPU_COMMAND, &submit)) {
		perror("GPU_COMMAND");
		return 1;
	}
	printf("submitted, timestamp %u\n", submit.timestamp);

	struct kgsl_device_waittimestamp_ctxtid w = {
		.context_id = ctx.drawctxt_id, .timestamp = submit.timestamp, .timeout = 2000,
	};
	if (ioctl(fd, IOCTL_KGSL_DEVICE_WAITTIMESTAMP_CTXTID, &w))
		printf("wait: %s\n", strerror(errno));

	__sync_synchronize();
	uint32_t v = target.cpu[0];
	printf("target = 0x%08x -> %s\n", v, v == 0xc0ffee01 ? "THE GPU WROTE IT" : "unchanged");
	return v == 0xc0ffee01 ? 0 : 2;
}
