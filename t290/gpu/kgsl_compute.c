/*
 * Adreno 504 compute: run an ir3 shader (assembled by t290/gpu/ir3asm) through
 * Android's kgsl, with the a5xx compute state of Mesa's fd5_compute.c
 * (cs_program_emit, fd5_launch_grid) and the relevant parts of
 * fd5_emit_restore / fd5_emit_sysmem_prep.
 *
 *   kgsl_compute SHADER.bin INSTRLEN MAXREG GX [LOCAL]
 * The shader reads c0.xy = buffer address (64-bit) and c0.z = element count;
 * the buffer (4 KiB, zeroed) is dumped afterwards.
 *
 * Build: aarch64-linux-gnu-gcc -static -O2 -D__user= -I../../research/kgsl kgsl_compute.c
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/types.h>
#include "msm_kgsl.h"

/* PM4 (adreno_pm4.xml.h) */
#define CP_WAIT_FOR_IDLE	38
#define CP_LOAD_STATE4		48
#define CP_EXEC_CS		51
#define CP_EVENT_WRITE		70
#define CP_SET_RENDER_MODE	108
#define BYPASS			1
#define CACHE_FLUSH_TS		4
#define PC_CCU_INVALIDATE_COLOR	25
#define SB4_CS_SHADER		13
#define ST4_SHADER		0
#define ST4_CONSTANTS		1
#define SS4_DIRECT		0
#define SS4_INDIRECT		2

/* a5xx registers (a5xx.xml.h) */
#define RB_CCU_CNTL		0x0cc7
#define SP_MODE_CNTL		0x0ec2
#define SP_DBG_ECO_CNTL		0x0ec0
#define TPL1_MODE_CNTL		0x0f02
#define HLSQ_TIMEOUT_THRESHOLD_0 0x0e00
#define HLSQ_MODE_CNTL		0x0e06
#define HLSQ_UPDATE_CNTL	0xe78a
#define SP_SP_CNTL		0xe580
#define SP_CS_CONFIG		0xe589
#define SP_CS_CTRL_REG0		0xe5f0
#define SP_CS_OBJ_START_LO	0xe5f3
#define HLSQ_CONTROL_0_REG	0xe784
#define HLSQ_CS_CONFIG		0xe790
#define HLSQ_CS_CNTL		0xe796
#define HLSQ_CS_NDRANGE_0	0xe7b0
#define HLSQ_CS_CNTL_0		0xe7b7
#define HLSQ_CS_KERNEL_GROUP_X	0xe7b9
#define HLSQ_CS_CONSTLEN	0xe7dc
#define REGID_NONE		0xfc	/* regid(63, 0) */

static unsigned odd_parity(unsigned v)
{
	v ^= v >> 16;
	v ^= v >> 8;
	v ^= v >> 4;
	v &= 0xf;
	return (~0x6996u >> v) & 1;
}

static uint32_t pkt7(unsigned op, unsigned cnt)
{
	return 0x70000000u | (cnt & 0x3fff) | (odd_parity(cnt) << 15) |
	       ((op & 0x7f) << 16) | (odd_parity(op) << 23);
}

static uint32_t pkt4(unsigned reg, unsigned cnt)
{
	return 0x40000000u | (cnt & 0x7f) | (odd_parity(cnt) << 7) |
	       ((reg & 0x3ffff) << 8) | (odd_parity(reg) << 27);
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

static unsigned n;
static uint32_t *cs;
#define OUT(v) (cs[n++] = (v))

static void reg(unsigned r, uint32_t v)
{
	OUT(pkt4(r, 1));
	OUT(v);
}

int main(int argc, char **argv)
{
	if (argc < 5) {
		fprintf(stderr, "usage: %s SHADER.bin INSTRLEN MAXREG GX [LOCAL]\n", argv[0]);
		return 1;
	}
	unsigned instrlen = strtoul(argv[2], 0, 0);
	int max_reg = strtol(argv[3], 0, 0);
	unsigned gx = strtoul(argv[4], 0, 0);
	unsigned local = argc > 5 ? strtoul(argv[5], 0, 0) : 1;

	int fd = open("/dev/kgsl-3d0", O_RDWR);
	if (fd < 0) {
		perror("open");
		return 1;
	}
	struct kgsl_drawctxt_create ctx = { .flags = KGSL_CONTEXT_PREAMBLE | KGSL_CONTEXT_NO_GMEM_ALLOC };
	if (ioctl(fd, IOCTL_KGSL_DRAWCTXT_CREATE, &ctx)) {
		perror("DRAWCTXT_CREATE");
		return 1;
	}
	struct gbuf cmd, prog, buf, scratch;
	if (gpu_alloc(fd, 4096, &cmd) || gpu_alloc(fd, 4096, &prog) ||
	    gpu_alloc(fd, 4096, &buf) || gpu_alloc(fd, 4096, &scratch))
		return 1;
	FILE *f = fopen(argv[1], "rb");
	if (!f) {
		perror(argv[1]);
		return 1;
	}
	size_t got = fread(prog.cpu, 1, 4096, f);
	fclose(f);
	memset(buf.cpu, 0, 4096);
	printf("shader %zu bytes at gpu 0x%llx, buffer gpu 0x%llx\n", got,
	       (unsigned long long)prog.gpuaddr, (unsigned long long)buf.gpuaddr);

	cs = cmd.cpu;
	n = 0;
	/* fd5_emit_sysmem_prep / fd5_emit_restore, compute-relevant part */
	OUT(pkt7(CP_SET_RENDER_MODE, 5));
	OUT(BYPASS);
	OUT(0);
	OUT(0);
	OUT(0);
	OUT(0);
	reg(HLSQ_UPDATE_CNTL, 0xfffff);
	OUT(pkt7(CP_EVENT_WRITE, 1));
	OUT(PC_CCU_INVALIDATE_COLOR);
	OUT(pkt7(CP_WAIT_FOR_IDLE, 0));
	reg(RB_CCU_CNTL, 0x10000000);
	reg(SP_MODE_CNTL, 0x0000001e);
	reg(SP_DBG_ECO_CNTL, 0x40000800);
	reg(TPL1_MODE_CNTL, 0x00000544);
	OUT(pkt4(HLSQ_TIMEOUT_THRESHOLD_0, 2));
	OUT(0x00000080);
	OUT(0x00000000);
	reg(HLSQ_MODE_CNTL, 0x00000001);

	/* cs_program_emit(): TWO_QUADS, preloaded shader */
	reg(SP_SP_CNTL, 0);
	reg(HLSQ_CONTROL_0_REG, 0 << 0 | 0 << 2 | 0x880);
	reg(SP_CS_CTRL_REG0, 0 << 3 | 0 << 4 | (unsigned)(max_reg + 1) << 10 | 0 << 25 | 0x4 | 0x2);
	reg(HLSQ_CS_CONFIG, 1);
	reg(HLSQ_CS_CNTL, instrlen << 1);
	reg(SP_CS_CONFIG, 1);
	OUT(pkt4(HLSQ_CS_CONSTLEN, 2));
	OUT(1);				/* one vec4 of constants */
	OUT(instrlen);
	OUT(pkt4(SP_CS_OBJ_START_LO, 6));
	OUT((uint32_t)prog.gpuaddr);
	OUT((uint32_t)(prog.gpuaddr >> 32));
	OUT(0);				/* PVT_MEM_PARAM */
	OUT(0);
	OUT(0);
	OUT(0);
	reg(HLSQ_UPDATE_CNTL, 0x1f00000);
	OUT(pkt4(HLSQ_CS_CNTL_0, 2));
	OUT(REGID_NONE | REGID_NONE << 8 | REGID_NONE << 16 | REGID_NONE << 24);
	OUT(1);
	/* fd5_emit_shader(): preload from the shader buffer */
	OUT(pkt7(CP_LOAD_STATE4, 3));
	OUT(0 | SS4_INDIRECT << 16 | SB4_CS_SHADER << 18 | instrlen << 22);
	OUT((uint32_t)prog.gpuaddr | ST4_SHADER);
	OUT((uint32_t)(prog.gpuaddr >> 32));
	/* constants: c0 = buffer address, element count */
	OUT(pkt7(CP_LOAD_STATE4, 3 + 4));
	OUT(0 | SS4_DIRECT << 16 | SB4_CS_SHADER << 18 | 1 << 22);
	OUT(ST4_CONSTANTS);
	OUT(0);
	OUT((uint32_t)buf.gpuaddr);
	OUT((uint32_t)(buf.gpuaddr >> 32));
	OUT(gx * local);
	OUT(0);

	/* fd5_launch_grid() */
	OUT(pkt4(HLSQ_CS_NDRANGE_0, 7));
	OUT(1 | (local - 1) << 2 | 0 << 12 | 0 << 22);
	OUT(local * gx);
	OUT(0);
	OUT(1);
	OUT(0);
	OUT(1);
	OUT(0);
	OUT(pkt4(HLSQ_CS_KERNEL_GROUP_X, 3));
	OUT(1);
	OUT(1);
	OUT(1);
	OUT(pkt7(CP_EXEC_CS, 4));
	OUT(0);
	OUT(gx);
	OUT(1);
	OUT(1);

	OUT(pkt7(CP_WAIT_FOR_IDLE, 0));
	OUT(pkt7(CP_EVENT_WRITE, 4));
	OUT(CACHE_FLUSH_TS);
	OUT((uint32_t)scratch.gpuaddr);
	OUT((uint32_t)(scratch.gpuaddr >> 32));
	OUT(7);
	__sync_synchronize();

	struct kgsl_command_object ib = {
		.gpuaddr = cmd.gpuaddr, .size = n * 4, .flags = KGSL_CMDLIST_IB, .id = cmd.id,
	};
	struct kgsl_gpu_command submit = {
		.cmdlist = (uint64_t)(uintptr_t)&ib, .cmdsize = sizeof(ib), .numcmds = 1,
		.context_id = ctx.drawctxt_id,
	};
	if (ioctl(fd, IOCTL_KGSL_GPU_COMMAND, &submit)) {
		perror("GPU_COMMAND");
		return 1;
	}
	struct kgsl_device_waittimestamp_ctxtid w = {
		.context_id = ctx.drawctxt_id, .timestamp = submit.timestamp, .timeout = 3000,
	};
	int werr = ioctl(fd, IOCTL_KGSL_DEVICE_WAITTIMESTAMP_CTXTID, &w) ? errno : 0;
	__sync_synchronize();
	printf("wait %s, scratch %u, cmd %u dwords\nbuffer:", werr ? strerror(werr) : "ok",
	       scratch.cpu[0], n);
	for (int i = 0; i < 16; i++)
		printf(" %08x", buf.cpu[i]);
	printf("\n");
	return 0;
}
