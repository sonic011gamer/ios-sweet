/*
 * Reference register dump of the Adreno 504 while Android's kgsl runs it:
 * the GPU copies its own registers to memory (CP_REG_TO_MEM, opcode 62),
 * one indirect buffer and context per range so a protected range only
 * costs that range. Compare with the bare-metal gpu2 probe in t290/shim.
 * Ranges 0x10000+ are the GPU SMMU seen from the CP (qcom,protect
 * <0x40000 0x10000>: byte 0x40000 = dword 0x10000; CB0 at 0x48000).
 *
 * Build: aarch64-linux-gnu-gcc -static -O2 -D__user= -I../../research/kgsl kgsl_regdump.c
 * Run on the tablet as root: /data/local/tmp/kgsl_regdump
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

#define CP_REG_TO_MEM	62

static unsigned odd_parity(unsigned v)
{
	v ^= v >> 16;
	v ^= v >> 8;
	v ^= v >> 4;
	v &= 0xf;
	return (~0x6996u >> v) & 1;
}

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

static const struct { uint32_t reg, cnt; const char *name; } ranges[] = {
	{ 0x0000, 0x00c0, "RBBM clocks/AHB" },
	{ 0x0460, 0x0010, "RBBM perfctr" },
	{ 0x04e0, 0x0060, "RBBM status/int" },
	{ 0x0800, 0x0040, "CP ring/ME" },
	{ 0x0860, 0x0002, "CP addr mode" },
	{ 0x08a0, 0x0020, "CP protect" },
	{ 0x0b00, 0x0020, "CP misc" },
	{ 0x0bc0, 0x0002, "VSC" },
	{ 0x0c80, 0x0002, "GRAS" },
	{ 0x0cc0, 0x0008, "RB" },
	{ 0x0d00, 0x0002, "PC" },
	{ 0x0e00, 0x0008, "HLSQ" },
	{ 0x0e40, 0x0002, "VFD" },
	{ 0x0e60, 0x0002, "VPC" },
	{ 0x0e80, 0x0020, "UCHE" },
	{ 0x0ec0, 0x0002, "SP" },
	{ 0x0f00, 0x0004, "TPL1" },
	{ 0x3000, 0x0090, "VBIF" },
	{ 0xf400, 0x0004, "SECVID" },
	{ 0xf800, 0x0012, "SECVID TSB" },
	{ 0x10000, 0x0012, "SMMU global" },
	{ 0x10200, 0x0004, "SMMU SMR" },
	{ 0x10300, 0x0004, "SMMU S2CR" },
	{ 0x10400, 0x0004, "SMMU CBAR" },
	{ 0x10600, 0x0004, "SMMU CBA2R" },
	{ 0x11800, 0x0004, "SMMU micro-mmu" },
	{ 0x12000, 0x0020, "SMMU CB0" },
	{ 0x12016, 0x0006, "SMMU CB0 fsr/far" },
};

static int dump_range(int fd, struct gbuf *cmd, struct gbuf *out, uint32_t reg, uint32_t cnt)
{
	struct kgsl_drawctxt_create ctx = {
		.flags = KGSL_CONTEXT_PREAMBLE | KGSL_CONTEXT_NO_GMEM_ALLOC,
	};
	if (ioctl(fd, IOCTL_KGSL_DRAWCTXT_CREATE, &ctx)) {
		perror("DRAWCTXT_CREATE");
		return -1;
	}
	for (uint32_t i = 0; i < cnt; i++)
		out->cpu[i] = 0xdeadbeef;
	unsigned n = 0;
	cmd->cpu[n++] = pkt7(CP_REG_TO_MEM, 3);
	cmd->cpu[n++] = reg | cnt << 18 | 1u << 30;	/* REG, CNT, 64B */
	cmd->cpu[n++] = (uint32_t)out->gpuaddr;
	cmd->cpu[n++] = (uint32_t)(out->gpuaddr >> 32);
	__sync_synchronize();

	struct kgsl_command_object ib = {
		.gpuaddr = cmd->gpuaddr, .size = n * 4, .flags = KGSL_CMDLIST_IB, .id = cmd->id,
	};
	struct kgsl_gpu_command submit = {
		.cmdlist = (uint64_t)(uintptr_t)&ib,
		.cmdsize = sizeof(ib),
		.numcmds = 1,
		.context_id = ctx.drawctxt_id,
	};
	int ret = 0;
	if (ioctl(fd, IOCTL_KGSL_GPU_COMMAND, &submit)) {
		perror("GPU_COMMAND");
		ret = -1;
	} else {
		struct kgsl_device_waittimestamp_ctxtid w = {
			.context_id = ctx.drawctxt_id, .timestamp = submit.timestamp, .timeout = 2000,
		};
		if (ioctl(fd, IOCTL_KGSL_DEVICE_WAITTIMESTAMP_CTXTID, &w)) {
			printf("  wait: %s\n", strerror(errno));
			ret = -1;
		}
	}
	struct kgsl_drawctxt_destroy d = { .drawctxt_id = ctx.drawctxt_id };
	ioctl(fd, IOCTL_KGSL_DRAWCTXT_DESTROY, &d);
	__sync_synchronize();
	return ret;
}

int main(void)
{
	int fd = open("/dev/kgsl-3d0", O_RDWR);
	if (fd < 0) {
		perror("open /dev/kgsl-3d0");
		return 1;
	}
	struct gbuf cmd, out;
	if (gpu_alloc(fd, 4096, &cmd) || gpu_alloc(fd, 4096, &out))
		return 1;
	printf("# cmd 0x%llx out 0x%llx\n", (unsigned long long)cmd.gpuaddr,
	       (unsigned long long)out.gpuaddr);
	for (unsigned r = 0; r < sizeof(ranges) / sizeof(ranges[0]); r++) {
		printf("## %s 0x%05x+0x%x\n", ranges[r].name, ranges[r].reg, ranges[r].cnt);
		fflush(stdout);
		if (dump_range(fd, &cmd, &out, ranges[r].reg, ranges[r].cnt))
			continue;
		for (uint32_t i = 0; i < ranges[r].cnt; i++)
			printf("%05x %08x\n", ranges[r].reg + i, out.cpu[i]);
	}
	return 0;
}
