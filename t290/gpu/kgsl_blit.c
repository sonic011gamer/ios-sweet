/*
 * Adreno 504 2D engine: scale a 1536x2048 BGRA frame (the iPad display)
 * down to 768x1024 with CP_BLIT / BLIT_OP_SCALE, as freedreno's a5xx
 * blitter drives it (CP_SET_RENDER_MODE BLIT2D, RB_2D_SRC/DST_*,
 * GRAS_2D_*_INFO, CP_BLIT, END2D), through Android's kgsl.
 *
 * The source has one colour per 2x2 block, so nearest and bilinear give the
 * same answer and every destination pixel can be checked exactly. The CPU
 * does the same halving afterwards for a timing comparison.
 *
 * Build: aarch64-linux-gnu-gcc -static -O2 -D__user= -I../../research/kgsl kgsl_blit.c
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/types.h>
#include "msm_kgsl.h"

/* PM4 opcodes and enums (Linux drivers/gpu/drm/msm/adreno/adreno_pm4.xml.h) */
#define CP_BLIT			44
#define CP_EVENT_WRITE		70
#define CP_SET_RENDER_MODE	108
#define CP_WAIT_FOR_IDLE	38
#define BLIT2D			5
#define END2D			8
#define BLIT_OP_COPY		1
#define BLIT_OP_SCALE		3
#define CACHE_FLUSH_TS		4
#define PC_CCU_FLUSH_COLOR_TS	29

/* a5xx registers (dword index, a5xx.xml.h) */
#define RB_RENDER_CNTL		0xe141
#define RB_CNTL			0xe140
#define RB_CNTL_BYPASS		0x00020000
#define RB_MODE_CNTL		0x0cc6
#define SP_MODE_CNTL		0x0ec2
#define TPL1_MODE_CNTL		0x0f02
#define HLSQ_MODE_CNTL		0x0e06
#define GRAS_CL_CNTL		0xe000
#define RB_2D_BLIT_CNTL		0x2100
#define RB_2D_SRC_INFO		0x2107
#define RB_2D_DST_INFO		0x2110
#define GRAS_2D_BLIT_CNTL	0x2180
#define GRAS_2D_SRC_INFO	0x2181
#define GRAS_2D_DST_INFO	0x2182
#define UNKNOWN_2184		0x2184
#define RB_CCU_CNTL		0x0cc7
#define PC_POWER_CNTL		0xe3b0
#define VFD_POWER_CNTL		0xe4f0
#define HLSQ_UPDATE_CNTL	0xe78a
#define BYPASS			1
#define PC_CCU_INVALIDATE_COLOR	25
#define RB5_R8G8B8A8_UNORM	48
#define RB5_R32_UINT		75
#define RB5_R32G32_UINT		104
#define TILE5_LINEAR		0

#define SW 1536
#define SH 2048
#define DW (SW / 2)
#define DH (SH / 2)

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

static double now_us(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

static uint32_t colour(unsigned bx, unsigned by)
{
	return 0xff000000u | (bx * 7 & 0xff) << 16 | (by * 3 & 0xff) << 8 | ((bx ^ by) & 0xff);
}

static unsigned n;
static uint32_t *cs;
#define OUT(v) (cs[n++] = (v))

static void reg(unsigned r, uint32_t v)
{
	OUT(pkt4(r, 1));
	OUT(v);
}

static void event_ts(unsigned ev, uint64_t addr, uint32_t val)
{
	OUT(pkt7(CP_EVENT_WRITE, 4));
	OUT(ev);
	OUT((uint32_t)addr);
	OUT((uint32_t)(addr >> 32));
	OUT(val);
}

int main(int argc, char **argv)
{
	int copy = argc > 1 && !strcmp(argv[1], "copy");	/* same-size copy of the top-left 768x1024 */
	/*
	 * "half": a 2:1 shrink with the copy engine only. Every other row via a
	 * doubled source pitch; each pixel pair read as one R32G32 texel and
	 * written as R32, which keeps the first pixel and drops the second.
	 */
	int half = argc > 1 && !strncmp(argv[1], "half", 4);
	int dbg = argc > 1 && !strcmp(argv[1], "halfdbg");	/* source pixels hold x | y << 16 */
	if (half)
		copy = 1;
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
	struct gbuf cmd, src, dst, scratch;
	if (gpu_alloc(fd, 4096, &cmd) || gpu_alloc(fd, (size_t)SW * SH * 4, &src) ||
	    gpu_alloc(fd, (size_t)DW * DH * 4, &dst) || gpu_alloc(fd, 4096, &scratch))
		return 1;

	for (unsigned y = 0; y < SH; y++)
		for (unsigned x = 0; x < SW; x++)
			src.cpu[y * SW + x] = dbg ? (x | y << 16) : colour(x / 2, y / 2);
	memset(dst.cpu, 0, (size_t)DW * DH * 4);

	cs = cmd.cpu;
	n = 0;
	/*
	 * fd5_emit_sysmem_prep(): render straight to memory (BYPASS), colour
	 * cache in bypass mode, one SP core on the A504.
	 */
	OUT(pkt7(CP_SET_RENDER_MODE, 5));
	OUT(BYPASS);
	OUT(0);
	OUT(0);
	OUT(0);
	OUT(0);
	reg(HLSQ_UPDATE_CNTL, 0xfffff);
	OUT(pkt7(CP_EVENT_WRITE, 1));
	OUT(PC_CCU_INVALIDATE_COLOR);
	reg(PC_POWER_CNTL, 0);
	reg(VFD_POWER_CNTL, 0);
	OUT(pkt7(CP_WAIT_FOR_IDLE, 0));
	reg(RB_CCU_CNTL, 0x10000000);
	/* fd5_blitter emit_setup() */
	reg(RB_RENDER_CNTL, 0x00000008);
	reg(RB_2D_BLIT_CNTL, 0x86000000);
	reg(GRAS_2D_BLIT_CNTL, 0x86000000);
	reg(UNKNOWN_2184, 0x00000009);
	reg(RB_CNTL, RB_CNTL_BYPASS);
	reg(RB_MODE_CNTL, 0x00000004);
	reg(SP_MODE_CNTL, 0x0000000c);
	reg(TPL1_MODE_CNTL, 0x00000344);
	reg(HLSQ_MODE_CNTL, 0x00000002);
	reg(GRAS_CL_CNTL, 0x00000181);

	OUT(pkt7(CP_SET_RENDER_MODE, 1));
	OUT(BLIT2D);

	uint32_t info = RB5_R8G8B8A8_UNORM | TILE5_LINEAR << 8 | 0 << 10;	/* swap WZYX, same both sides */
	uint32_t sinfo = half ? RB5_R32G32_UINT : info, dinfo = half ? RB5_R32_UINT : info;
	OUT(pkt4(RB_2D_SRC_INFO, 9));
	OUT(sinfo);
	OUT((uint32_t)src.gpuaddr);
	OUT((uint32_t)(src.gpuaddr >> 32));
	OUT(((SW * 4 * (half ? 2 : 1)) >> 6) & 0xffff);	/* SIZE: pitch / 64, array pitch 0 */
	for (int i = 0; i < 5; i++)
		OUT(0);
	reg(GRAS_2D_SRC_INFO, sinfo);

	OUT(pkt4(RB_2D_DST_INFO, 9));
	OUT(dinfo);
	OUT((uint32_t)dst.gpuaddr);
	OUT((uint32_t)(dst.gpuaddr >> 32));
	OUT(((DW * 4) >> 6) & 0xffff);
	for (int i = 0; i < 5; i++)
		OUT(0);
	reg(GRAS_2D_DST_INFO, dinfo);

	OUT(pkt7(CP_BLIT, 5));
	OUT(copy ? BLIT_OP_COPY : BLIT_OP_SCALE);
	OUT(0 | 0 << 16);				/* src x1, y1 */
	OUT(copy ? (DW - 1) | (DH - 1) << 16 : (SW - 1) | (SH - 1) << 16);	/* src x2, y2 */
	OUT(0 | 0 << 16);				/* dst x1, y1 */
	OUT((DW - 1) | (DH - 1) << 16);			/* dst x2, y2 */

	OUT(pkt7(CP_SET_RENDER_MODE, 1));
	OUT(END2D);
	event_ts(PC_CCU_FLUSH_COLOR_TS, scratch.gpuaddr, 1);
	event_ts(CACHE_FLUSH_TS, scratch.gpuaddr + 4, 2);
	OUT(pkt7(CP_WAIT_FOR_IDLE, 0));
	__sync_synchronize();

	struct kgsl_command_object ib = {
		.gpuaddr = cmd.gpuaddr, .size = n * 4, .flags = KGSL_CMDLIST_IB, .id = cmd.id,
	};
	struct kgsl_gpu_command submit = {
		.cmdlist = (uint64_t)(uintptr_t)&ib, .cmdsize = sizeof(ib), .numcmds = 1,
		.context_id = ctx.drawctxt_id,
	};
	double t0 = now_us();
	if (ioctl(fd, IOCTL_KGSL_GPU_COMMAND, &submit)) {
		perror("GPU_COMMAND");
		return 1;
	}
	struct kgsl_device_waittimestamp_ctxtid w = {
		.context_id = ctx.drawctxt_id, .timestamp = submit.timestamp, .timeout = 3000,
	};
	int werr = ioctl(fd, IOCTL_KGSL_DEVICE_WAITTIMESTAMP_CTXTID, &w) ? errno : 0;
	double t_gpu = now_us() - t0;
	__sync_synchronize();

	unsigned bad = 0, first_bad = 0;
	for (unsigned y = 0; y < DH; y++)
		for (unsigned x = 0; x < DW; x++)
			if (dst.cpu[y * DW + x] != (copy && !half ? colour(x / 2, y / 2) : colour(x, y)) && bad++ == 0)
				first_bad = y * DW + x;
	printf("GPU %s %dx%d -> %dx%d: wait %s, %.0f us, %u/%u pixels wrong",
	       half ? "half (R32G32->R32 copy)" : copy ? "copy" : "scale", copy ? DW : SW, copy ? DH : SH, DW, DH, werr ? strerror(werr) : "ok", t_gpu, bad, DW * DH);
	if (bad)
		printf(" (first at %u,%u: got 0x%08x want 0x%08x)", first_bad % DW, first_bad / DW,
		       dst.cpu[first_bad], copy && !half ? colour(first_bad % DW / 2, first_bad / DW / 2) : colour(first_bad % DW, first_bad / DW));
	printf("\nscratch %u %u, cmd %u dwords\n", scratch.cpu[0], scratch.cpu[1], n);
	if (dbg) {
		for (unsigned y = 0; y < 3; y++) {
			printf("dst row %u:", y);
			for (unsigned x = 0; x < 20; x++)
				printf(" %u,%u", dst.cpu[y * DW + x] & 0xffff, dst.cpu[y * DW + x] >> 16);
			printf("\n");
		}
		printf("dst row 0 x=64..67:");
		for (unsigned x = 64; x < 68; x++)
			printf(" %u,%u", dst.cpu[x] & 0xffff, dst.cpu[x] >> 16);
		printf("\n");
	}

	/* the same halving on one CPU core, uncached-ish memory as the GPU buffers */
	static uint32_t cpu_dst[DW * DH];
	t0 = now_us();
	for (unsigned y = 0; y < DH; y++) {
		const uint32_t *s = src.cpu + (size_t)(2 * y) * SW;
		uint32_t *d = cpu_dst + (size_t)y * DW;
		for (unsigned x = 0; x < DW; x++)
			d[x] = s[2 * x];
	}
	printf("CPU nearest halving: %.0f us\n", now_us() - t0);
	return bad || werr ? 2 : 0;
}
