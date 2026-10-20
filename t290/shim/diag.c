/*
 * T290 milestone 1: first code of ours on the tablet. LK boots this from the
 * recovery partition like a Linux kernel (EL1, MMU off, x0 = DTB) and it
 * reports, on the continuous-splash framebuffer, what the XNU port needs:
 *   - the splash framebuffer format, guessed from LK's logo pixels
 *   - the DTB LK passed: /memory and /chosen bootargs
 *   - GCC SDCC1/SDCC2 clock branches; the SDCC2 AHB clock is switched on so
 *     the microSD controller's registers can be read
 *   - SDHCI standard + Qualcomm core registers of sdhc2 (microSD), card-detect GPIO 67
 *   - PSCI version over SMC
 * The APSS watchdog LK leaves running is switched off first.
 * The screen then stays until the user forces a restart (Vol- + Power).
 * Any exception paints a blue screen with ESR/ELR/FAR.
 */
#include <stdint.h>
#include "../../shim/font.h"

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define isb() __asm__ volatile("isb" ::: "memory")

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

/* ---- hardware (live FDT of SM-T290, board P81080BA5) ---- */
#define SPLASH_PHYS	0x90000000UL	/* reserved-memory cont_splash_mem */
#define FB_W		800
#define FB_H		1280

/* MDP registers (0x1a01000): reading them resets the tablet, leave alone. */

#define GCC_BASE	0x01800000UL
#define GCC_SDCC1_CMD	0x42004
#define GCC_SDCC1_APPS	0x42018
#define GCC_SDCC1_AHB	0x4201c
#define GCC_SDCC2_CMD	0x43004
#define GCC_SDCC2_CFG	0x43008
#define GCC_SDCC2_APPS	0x43018
#define GCC_SDCC2_AHB	0x4301c
#define CBCR_CLK_OFF	(1U << 31)
#define CBCR_CLK_ENABLE	1U

#define SDC2_HC		0x07864900UL	/* SDHCI standard registers */
#define SDC2_CORE	0x07864000UL	/* Qualcomm core_mem */
#define TLMM_BASE	0x01000000UL
#define SD_CD_GPIO	67
#define TLMM_SDC2_CTL	0x109000	/* sdc2 clk/cmd/data pull + drive (pinctrl-msm8937) */

/* APSS watchdog (qcom,msm-watchdog): LK leaves it running, ~18 s to bite. */
#define WDT_BASE	0x0b017000UL
#define WDT_RST		0x04
#define WDT_EN		0x08
#define WDT_STS		0x0c

#define PSCI_VERSION		0x84000000UL

/*
 * Progress log in the ramoops console zone (reserved-memory ramoops@92100000:
 * one 256K dump record, then console at +0x40000). DDR keeps it across the
 * warm reset, and the next Linux boot (TWRP) shows it as
 * /sys/fs/pstore/console-ramoops-0. Each step is logged before it runs, so
 * the last line names the access that reset the tablet.
 */
#define PLOG_BASE	0x92140000UL
#define PLOG_SIZE	0x40000U
#define PRZ_SIG		0x43474244U	/* "DBGC" persistent_ram_buffer */
#define RUNS_AT		0x913f0000UL	/* end of cont_splash_mem, unused by the 800x1280 fb */
#define RUNS_MAGIC	0x54323930U

static uint32_t plog_len;
static const char *stage;

static void plog_commit(void)
{
	/* start == size == len: Linux copies data[0..len) as the old log */
	wr32(PLOG_BASE + 4, plog_len);
	wr32(PLOG_BASE + 8, plog_len);
	__asm__ volatile("dsb sy" ::: "memory");
}

static void plog_putc(char c)
{
	if (plog_len < PLOG_SIZE - 12)
		*(volatile char *)(PLOG_BASE + 12 + plog_len++) = c;
}

static void plog_str(const char *s)
{
	while (*s)
		plog_putc(*s++);
	plog_commit();
}

static void plog_hex(uint64_t v, int digits)
{
	for (int i = digits - 1; i >= 0; i--) {
		unsigned d = (v >> (4 * i)) & 0xf;
		plog_putc(d < 10 ? '0' + d : 'a' + d - 10);
	}
}

static uint32_t plog_init(void)
{
	uint32_t runs = rd32(RUNS_AT) == RUNS_MAGIC ? rd32(RUNS_AT + 4) + 1 : 1;
	wr32(RUNS_AT, RUNS_MAGIC);
	wr32(RUNS_AT + 4, runs);
	wr32(PLOG_BASE, PRZ_SIG);
	plog_len = 0;
	plog_str("T290 diag run ");
	plog_hex(runs, 4);
	plog_str("\n");
	return runs;
}

#define STEP(s) do { stage = s; plog_str(">> " s "\n"); } while (0)

/* ---- console: format follows whatever LK programmed into the MDP ---- */
#define SCALE		2
#define CH_W		(8 * SCALE)
#define CH_H		(16 * SCALE)
#define MARGIN		16
#define LABEL_COLS	8

#define RED	0xff0000U
#define BLUE	0x0000a0U
#define WHITE	0xffffffU
#define YELLOW	0xffff00U
#define GREEN	0x00c000U

static uintptr_t fb = SPLASH_PHYS;
static uint32_t fb_stride = FB_W * 4, fb_bpp = 4;
static uint32_t bg;
static int row;

/*
 * The panel shows the buffer shifted right by ~229 pixels with wrap-around
 * (milestone 2 ruler photo: buffer x 0 appeared at screen x ~229), so
 * screen (x, y) lives at buffer (x - SCAN_SHIFT, y), one row up when that
 * goes negative.
 */
#define SCAN_SHIFT	229

static void put_px(int x, int y, uint32_t rgb)
{
	x -= SCAN_SHIFT;
	if (x < 0) {
		x += FB_W;
		y -= 1;
	}
	if (y < 0)
		return;
	uintptr_t p = fb + (uintptr_t)y * fb_stride + (uintptr_t)x * fb_bpp;
	volatile uint8_t *b = (volatile uint8_t *)p;
	/*
	 * LK's splash bytes are R, B, G: with B,G,R yellow showed cyan, with
	 * R,G,B green showed blue and yellow magenta.
	 */
	b[0] = (rgb >> 16) & 0xff;
	b[1] = rgb & 0xff;
	b[2] = (rgb >> 8) & 0xff;
	if (fb_bpp == 4)
		b[3] = 0xff;
}

static void con_clear(uint32_t c)
{
	for (int y = 0; y < FB_H; y++)
		for (int x = 0; x < FB_W; x++)
			put_px(x, y, c);
	bg = c;
	row = 0;
}

static void draw_glyph(int x0, int y0, char ch, uint32_t fg)
{
	if (ch < 0x20 || ch > 0x7e)
		ch = '?';
	const unsigned char *g = font8x16[ch - 0x20];
	if (x0 < 0 || x0 + CH_W > FB_W || y0 + CH_H > FB_H)
		return;
	for (int gy = 0; gy < 16; gy++)
		for (int gx = 0; gx < 8; gx++) {
			uint32_t c = (g[gy] & (0x80 >> gx)) ? fg : bg;
			for (int sy = 0; sy < SCALE; sy++)
				for (int sx = 0; sx < SCALE; sx++)
					put_px(x0 + gx * SCALE + sx, y0 + gy * SCALE + sy, c);
		}
}

static void put_char(int col, char ch, uint32_t fg)
{
	draw_glyph(MARGIN + col * CH_W, MARGIN + row * CH_H, ch, fg);
}

/*
 * Ruler along the top: a bar and a digit every 50 pixels of buffer x
 * (digit = x / 50 mod 10, bars alternate red/green). A photo then shows
 * which buffer column the panel's left edge scans out.
 */
static void draw_ruler(void)
{
	/* screen coordinates now: if SCAN_SHIFT is right, bar 0 hugs the left edge */
	for (int n = 0; n * 50 < FB_W; n++) {
		uint32_t c = (n & 1) ? GREEN : RED;
		for (int y = 0; y < 40; y++)
			for (int x = 0; x < 4; x++)
				put_px(n * 50 + x, y, c);
		draw_glyph(n * 50 + 8, 4, '0' + n % 10, n % 10 == 0 ? YELLOW : WHITE);
	}
	/* fine ticks every 10 px along the left edge, for the next correction */
	for (int x = 0; x < 100; x += 10)
		for (int y = 40; y < (x % 50 ? 52 : 60); y++)
			put_px(x, y, WHITE);
	row = 2;
}

static int put_str(int col, const char *s, uint32_t fg)
{
	while (*s)
		put_char(col++, *s++, fg);
	return col;
}

static void fmt_hex(char *buf, uint64_t v, int digits)
{
	for (int i = 0; i < digits; i++) {
		unsigned d = (v >> (4 * (digits - 1 - i))) & 0xf;
		buf[i] = d < 10 ? '0' + d : 'A' + d - 10;
	}
	buf[digits] = 0;
}

static void con_str(const char *label, const char *s)
{
	plog_str(label);
	plog_str(" ");
	plog_str(s);
	plog_str("\n");
	put_str(0, label, YELLOW);
	put_str(LABEL_COLS, s, WHITE);
	row++;
}

/* label a=xxxxxxxx b=xxxxxxxx c=xxxxxxxx (NULL names end the list) */
static void con_hex3(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb,
		     const char *c, uint32_t vc)
{
	char buf[9];
	const char *n[3] = { a, b, c };
	uint32_t v[3] = { va, vb, vc };
	plog_str(label);
	for (int i = 0; i < 3 && n[i]; i++) {
		plog_putc(' ');
		plog_str(n[i]);
		plog_hex(v[i], 8);
	}
	plog_str("\n");
	int col = put_str(0, label, YELLOW);
	col = col < LABEL_COLS ? LABEL_COLS : col + 1;
	for (int i = 0; i < 3 && n[i]; i++) {
		col = put_str(col, n[i], YELLOW);
		fmt_hex(buf, v[i], 8);
		col = put_str(col, buf, WHITE) + 1;
	}
	row++;
}

/* ---- timing, PSCI ---- */

static uint64_t now(void)
{
	isb();
	return rsr("cntpct_el0");
}

/* LK may leave CNTFRQ_EL0 at 0; the MSM8937 QTimer runs at 19.2 MHz. */
static uint64_t timer_hz(void)
{
	uint64_t f = rsr("cntfrq_el0");
	return f ? f : 19200000;
}

static void delay_ms(unsigned ms)
{
	uint64_t end = now() + timer_hz() / 1000 * ms;
	while (now() < end)
		;
}

static uint64_t smc(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile("smc #0"
			 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
			 :
			 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
			   "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

/*
 * No automatic reset: a reset while the misc partition still says
 * "boot-recovery" (adb reboot recovery) brings LK straight back here.
 * The screen stays until the user holds Vol- + Power.
 */
static void __attribute__((noreturn)) hold(void)
{
	row++;
	con_str("HOLD", "photo, then hold Vol- + Power");
	for (;;)
		__asm__ volatile("wfe");
}

void exc_handler(uint64_t slot, uint64_t *regs)
{
	plog_str("!! exception\n");
	con_clear(BLUE);
	con_str("PANIC", (slot & 3) == 0 ? "SYNC" : (slot & 3) == 3 ? "SERROR" : "IRQ/FIQ");
	con_str("STAGE", stage);
	con_hex3("ESR", "", (uint32_t)rsr("esr_el1"), 0, 0, 0, 0);
	con_hex3("ELR", "hi=", (uint32_t)(rsr("elr_el1") >> 32), "lo=", (uint32_t)rsr("elr_el1"), 0, 0);
	con_hex3("FAR", "hi=", (uint32_t)(rsr("far_el1") >> 32), "lo=", (uint32_t)rsr("far_el1"), 0, 0);
	con_hex3("X0", "lo=", (uint32_t)regs[0], 0, 0, 0, 0);
	hold();
}

/* start.S secondary entry: unused here, CPUs other than 0 stay off. */
void secondary_main(uint64_t idx)
{
	(void)idx;
	for (;;)
		__asm__ volatile("wfe");
}

/* ---- DTB: /memory reg and /chosen bootargs ---- */

static uint32_t be32(uintptr_t a)
{
	return __builtin_bswap32(*(volatile uint32_t *)a);
}

static int name_is(uintptr_t p, const char *s)
{
	while (*s && *(volatile char *)p == *s)
		p++, s++;
	return *s == 0 && (*(volatile char *)p == 0 || *(volatile char *)p == '@');
}

static int prop_is(uintptr_t p, const char *s)
{
	while (*s && *(volatile char *)p == *s)
		p++, s++;
	return *s == 0 && *(volatile char *)p == 0;
}

static void __attribute__((unused)) show_dtb(uintptr_t dtb)
{
	if (!dtb || be32(dtb) != 0xd00dfeed) {
		con_hex3("DTB", "at=", (uint32_t)dtb, "bad=", dtb ? be32(dtb) : 0, 0, 0);
		return;
	}
	con_hex3("DTB", "at=", (uint32_t)dtb, "size=", be32(dtb + 4), 0, 0);
	uintptr_t p = dtb + be32(dtb + 8), strings = dtb + be32(dtb + 12);
	int depth = 0, in_mem = 0, in_chosen = 0, nmem = 0;
	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		if (tok == 1) {			/* BEGIN_NODE */
			depth++;
			in_mem = depth == 2 && name_is(p, "memory");
			in_chosen = depth == 2 && name_is(p, "chosen");
			while (*(volatile char *)p)
				p++;
			p = (p + 4) & ~3UL;
		} else if (tok == 2) {		/* END_NODE */
			depth--;
			in_mem = in_chosen = 0;
		} else if (tok == 3) {		/* PROP */
			uint32_t len = be32(p), nameoff = be32(p + 4);
			uintptr_t val = p + 8;
			if (in_mem && prop_is(strings + nameoff, "reg")) {
				/* #address-cells = #size-cells = 2 at the root */
				for (uint32_t i = 0; i + 16 <= len && nmem < 4; i += 16, nmem++)
					con_hex3("MEM", "base=", be32(val + i + 4), "size=", be32(val + i + 12),
						 "hi=", be32(val + i) | be32(val + i + 8));
			}
			if (in_chosen && prop_is(strings + nameoff, "bootargs")) {
				/* first 40 characters of the command line LK built */
				char buf[41];
				uint32_t n = len < 40 ? len : 40;
				for (uint32_t i = 0; i < n; i++)
					buf[i] = *(volatile char *)(val + i);
				buf[n] = 0;
				con_str("ARGS", buf);
			}
			p = (val + len + 3) & ~3UL;
		} else if (tok == 4) {		/* NOP */
			continue;
		} else {
			break;
		}
	}
	if (!nmem)
		con_str("MEM", "no /memory reg");
}

/*
 * Splash format from the logo LK left in the buffer. MDP registers are off
 * limits (reading them resets the tablet), so look at the pixels instead:
 * in a 32 bpp buffer every 4th byte is alpha and never varies, in RGB888 it
 * is colour like the rest. Counts over the first 4 MiB of the buffer.
 */
static uint32_t fb_vary[4], fb_nonzero;

static void detect_splash_format(void)
{
	uint8_t first[4];
	for (int k = 0; k < 4; k++)
		first[k] = *(volatile uint8_t *)(SPLASH_PHYS + k);
	for (uintptr_t off = 0; off < FB_W * FB_H * 4; off += 4) {
		uint32_t w = rd32(SPLASH_PHYS + off);
		if (w)
			fb_nonzero++;
		for (int k = 0; k < 4; k++)
			if (((w >> (8 * k)) & 0xff) != first[k])
				fb_vary[k]++;
	}
	/* alpha byte constant while the colour bytes vary -> 32 bpp */
	if (fb_vary[3] == 0 && (fb_vary[0] || fb_vary[1] || fb_vary[2])) {
		fb_bpp = 4;
		fb_stride = FB_W * 4;
	} else if (fb_vary[3]) {
		fb_bpp = 3;
		fb_stride = FB_W * 3;
	}
	/* all-constant buffer: no evidence, keep the 32 bpp default */
}

static void __attribute__((unused)) show_splash_format(void)
{
	con_hex3("FBSCAN", "nz=", fb_nonzero, "v0=", fb_vary[0], "v3=", fb_vary[3]);
	con_hex3("FB", "at=", (uint32_t)fb, "stride=", fb_stride, "bpp=", fb_bpp * 8);
}

/* ---- clocks and the microSD controller ---- */

static int clk_on(uint32_t cbcr)
{
	uintptr_t r = GCC_BASE + cbcr;
	wr32(r, rd32(r) | CBCR_CLK_ENABLE);
	for (int i = 0; i < 1000; i++) {
		if (!(rd32(r) & CBCR_CLK_OFF))
			return 1;
		delay_ms(1);
	}
	return 0;
}

/*
 * microSD bring-up on sdhc2, read-only: 400 kHz from XO through the SDCC2
 * RCG, SDHCI mode, bus power (with the Qualcomm power-control handshake),
 * then CMD0, CMD8, ACMD41, CMD2 (CID), CMD3 (RCA), CMD9 (CSD).
 */
#define GCC_SDCC2_M	0x4300c
#define GCC_SDCC2_N	0x43010
#define GCC_SDCC2_D	0x43014

#define HC_ARG		0x08
#define HC_XFER_CMD	0x0c	/* transfer mode | command << 16, one 32-bit write */
#define HC_RESP		0x10
#define HC_PSTATE	0x24
#define HC_CTL		0x28	/* host ctl | power << 8 */
#define HC_CLK		0x2c	/* clock ctl | timeout << 16 | reset << 24 */
#define HC_INT		0x30	/* normal | error << 16, write 1 to clear */
#define HC_INT_EN	0x34

#define CORE_HC_MODE	0x78
#define CORE_PWR_ST	0xdc
#define CORE_PWR_CLR	0xe4
#define CORE_PWR_CTL	0xe8

#define RSP_NONE	0x00
#define RSP_136		0x09	/* 136-bit, CRC */
#define RSP_48		0x1a	/* 48-bit, CRC, index */
#define RSP_48_NOCRC	0x02

static uint32_t sd_resp[4];

static uint32_t hc(uint32_t off) { return rd32(SDC2_HC + off); }
static void hc_wr(uint32_t off, uint32_t v) { wr32(SDC2_HC + off, v); }

/* Acknowledge the core's power-control request (sdhci-msm pwr_irq) by polling. */
static uint32_t sd_pwr_ack(unsigned ms)
{
	for (unsigned i = 0; i < ms; i++) {
		uint32_t st = rd32(SDC2_CORE + CORE_PWR_ST);
		if (st) {
			uint32_t ctl = 0;
			wr32(SDC2_CORE + CORE_PWR_CLR, st);
			if (st & 3)		/* bus off / on */
				ctl |= 1;	/* bus success */
			if (st & 0xc)		/* io low / high */
				ctl |= 4;	/* io success */
			wr32(SDC2_CORE + CORE_PWR_CTL, ctl);
			return st;
		}
		delay_ms(1);
	}
	return 0;
}

static int sd_reset(uint32_t bit)
{
	hc_wr(HC_CLK, hc(HC_CLK) | bit << 24);
	for (int i = 0; i < 100; i++) {
		if (!(hc(HC_CLK) & bit << 24))
			return 1;
		delay_ms(1);
	}
	return 0;
}

/* 0 = ok; else error-interrupt bits | 0x10000 (0x20000 = no completion). */
static uint32_t sd_cmd(uint32_t idx, uint32_t arg, uint32_t rsp)
{
	for (int i = 0; hc(HC_PSTATE) & 1; i++) {
		if (i > 100)
			return 0x40000;
		delay_ms(1);
	}
	hc_wr(HC_INT, 0xffffffff);
	hc_wr(HC_ARG, arg);
	hc_wr(HC_XFER_CMD, (idx << 8 | rsp) << 16);
	for (int i = 0; i < 200; i++) {
		uint32_t st = hc(HC_INT);
		if (st & 0x8000) {
			hc_wr(HC_INT, st);
			sd_reset(2);	/* CMD line */
			return (st >> 16) | 0x10000;
		}
		if (st & 1) {
			hc_wr(HC_INT, 1);
			for (int k = 0; k < 4; k++)
				sd_resp[k] = hc(HC_RESP + 4 * k);
			return 0;
		}
		delay_ms(1);
	}
	sd_reset(2);
	return 0x20000;
}

/*
 * RPM requests over SMD, the way Qualcomm's LK does it (rpm-smd.c, smd.c):
 * the microSD rails pm8953 L11 (card) and L12 (pads) belong to the RPM.
 * SMEM 0x86300000, legacy global TOC: item 13 = channel allocation table,
 * 14 + cid = the two word-access half channels (APPS tx, RPM tx),
 * 338 + cid = FIFO (APPS->RPM half, then RPM->APPS). Android's debugfs:
 * rpm_requests is cid 4, packet mode, 0x400-byte FIFOs.
 */
#define SMEM_BASE		0x86300000UL
#define SMEM_TOC		(SMEM_BASE + 208)	/* after proc_comm[4], version[32], heap_info */
#define SMEM_ALLOC_TBL		13
#define SMEM_SMD_BASE		14
#define SMEM_SMD_FIFO		338
#define APCS_IPC		0x0b011008UL
#define RPM_IPC_BIT		1

/* smd_half_channel_word_access, 11 words */
#define HC_STATE	0
#define HC_DSR		1
#define HC_CTS		2
#define HC_CD		3
#define HC_RI		4
#define HC_FHEAD	5
#define HC_FTAIL	6
#define HC_FSTATE	7
#define HC_FBLOCK	8
#define HC_TAIL		9
#define HC_HEAD		10
#define SS_OPENING	1
#define SS_OPENED	2

static uintptr_t smd_tx, smd_rx, smd_fifo_tx, smd_fifo_rx;
static uint32_t smd_fifo_sz, rpm_msg_id;

static uintptr_t smem_item(uint32_t id, uint32_t *size)
{
	uintptr_t e = SMEM_TOC + id * 16;
	if (!rd32(e))
		return 0;
	if (size)
		*size = rd32(e + 8);
	uint32_t base = rd32(e + 12) & ~3U;
	return (base ? base : SMEM_BASE) + rd32(e + 4);
}

static uint32_t hch(uintptr_t h, int w) { return rd32(h + 4 * w); }
static void hch_wr(uintptr_t h, int w, uint32_t v) { wr32(h + 4 * w, v); }

static void rpm_kick(void)
{
	__asm__ volatile("dsb sy" ::: "memory");
	wr32(APCS_IPC, RPM_IPC_BIT);
}

static int smd_find_rpm(void)
{
	uint32_t tsize = 0;
	uintptr_t tbl = smem_item(SMEM_ALLOC_TBL, &tsize);
	con_hex3("SMEM", "ver=", rd32(SMEM_BASE + 64 + 7 * 4), "tbl=", (uint32_t)tbl, "tsz=", tsize);
	if (!tbl)
		return 0;
	for (uint32_t i = 0; i + 32 <= tsize && i < 64 * 32; i += 32) {
		static const char want[] = "rpm_requests";
		int j = 0;
		while (want[j] && *(volatile char *)(tbl + i + j) == want[j])
			j++;
		uint32_t ctype = rd32(tbl + i + 24);
		if (want[j] || *(volatile char *)(tbl + i + j) || (ctype & 0xff) != 0x0f)
			continue;	/* name or edge (APPS<->RPM = 0x0f) differs */
		uint32_t cid = rd32(tbl + i + 20), fsz = 0;
		uintptr_t sh = smem_item(SMEM_SMD_BASE + cid, 0);
		uintptr_t fifo = smem_item(SMEM_SMD_FIFO + cid, &fsz);
		con_hex3("RPMCH", "cid=", cid, "sh=", (uint32_t)sh, "fifo=", (uint32_t)fifo);
		if (!sh || !fifo || fsz < 2 * 64)
			return 0;
		smd_tx = sh;
		smd_rx = sh + 11 * 4;
		smd_fifo_sz = fsz / 2;
		smd_fifo_tx = fifo;
		smd_fifo_rx = fifo + smd_fifo_sz;
		return 1;
	}
	con_str("RPMCH", "rpm_requests not in the allocation table");
	return 0;
}

static int smd_open(void)
{
	con_hex3("SMDST", "tx=", hch(smd_tx, HC_STATE), "rx=", hch(smd_rx, HC_STATE),
		 "fsz=", smd_fifo_sz);
	con_hex3("SMDIX", "txh=", hch(smd_tx, HC_HEAD), "txt=", hch(smd_tx, HC_TAIL),
		 "rxh=", hch(smd_rx, HC_HEAD));
	if (hch(smd_tx, HC_STATE) == SS_OPENED && hch(smd_rx, HC_STATE) == SS_OPENED)
		return 1;
	/*
	 * LK's smd_set_state(OPENING) then (OPENED). Linux smd_state_change():
	 * going OPENING from CLOSED/CLOSING restarts both FIFOs at 0 (our head,
	 * our read position in RPM's half); RPM does the same on its side.
	 * Writing on at LK's old head made RPM read garbage and reset the SoC.
	 */
	hch_wr(smd_tx, HC_HEAD, 0);
	hch_wr(smd_rx, HC_TAIL, 0);
	hch_wr(smd_tx, HC_FBLOCK, 0);
	hch_wr(smd_tx, HC_STATE, SS_OPENING);
	hch_wr(smd_tx, HC_DSR, 0);
	hch_wr(smd_tx, HC_CTS, 0);
	hch_wr(smd_tx, HC_CD, 0);
	hch_wr(smd_tx, HC_FSTATE, 1);
	rpm_kick();
	for (int i = 0; i < 1000; i++) {
		uint32_t r = hch(smd_rx, HC_STATE);
		if (r == SS_OPENING || r == SS_OPENED)
			break;
		delay_ms(1);
	}
	uint32_t r = hch(smd_rx, HC_STATE);
	if (r != SS_OPENING && r != SS_OPENED) {
		con_hex3("SMDOPEN", "rpm_state=", r, 0, 0, 0, 0);
		return 0;
	}
	hch_wr(smd_tx, HC_STATE, SS_OPENED);
	hch_wr(smd_tx, HC_DSR, 1);
	hch_wr(smd_tx, HC_CTS, 1);
	hch_wr(smd_tx, HC_CD, 1);
	hch_wr(smd_tx, HC_FSTATE, 1);
	rpm_kick();
	for (int i = 0; i < 1000 && hch(smd_rx, HC_STATE) != SS_OPENED; i++)
		delay_ms(1);
	con_hex3("SMDOPEN", "tx=", hch(smd_tx, HC_STATE), "rx=", hch(smd_rx, HC_STATE), 0, 0);
	con_hex3("SMDIX2", "txh=", hch(smd_tx, HC_HEAD), "txt=", hch(smd_tx, HC_TAIL),
		 "rxh=", hch(smd_rx, HC_HEAD));
	return hch(smd_rx, HC_STATE) == SS_OPENED;
}

static void smd_put(uint32_t *head, uint32_t w)
{
	wr32(smd_fifo_tx + *head, w);
	*head = (*head + 4) % smd_fifo_sz;
}

/*
 * One "req" to the active set: resource type/id with key/value pairs
 * {key, len 4, value}. Returns RPM's reply word count seen, 0 if none.
 */
static uint32_t rpm_req(uint32_t rtype, uint32_t rid, const uint32_t *kv, int nkv)
{
	uint32_t data_len = nkv * 12;
	uint32_t req_len = 5 * 4 + data_len;		/* msg_id, set, type, id, data_len + kvps */
	uint32_t pkt_len = 2 * 4 + req_len;		/* service 'req', length */
	uint32_t head = hch(smd_tx, HC_HEAD);
	uint32_t free = (hch(smd_tx, HC_TAIL) + smd_fifo_sz - head - 4) % smd_fifo_sz;
	if (free < pkt_len + 20)
		return 0;
	uint32_t rx_before = hch(smd_rx, HC_HEAD);

	/* SMD packet header: payload length, then four reserved words */
	smd_put(&head, pkt_len);
	for (int i = 0; i < 4; i++)
		smd_put(&head, 0);
	smd_put(&head, 0x00716572);		/* "req" */
	smd_put(&head, req_len);
	smd_put(&head, ++rpm_msg_id);
	smd_put(&head, 0);			/* active set */
	smd_put(&head, rtype);
	smd_put(&head, rid);
	smd_put(&head, data_len);
	for (int i = 0; i < nkv; i++) {
		smd_put(&head, kv[2 * i]);
		smd_put(&head, 4);
		smd_put(&head, kv[2 * i + 1]);
	}
	__asm__ volatile("dsb sy" ::: "memory");
	hch_wr(smd_tx, HC_HEAD, head);
	hch_wr(smd_tx, HC_FHEAD, 1);
	rpm_kick();

	for (int i = 0; i < 200; i++) {
		if (hch(smd_rx, HC_HEAD) != rx_before)
			break;
		delay_ms(1);
	}
	uint32_t rx_head = hch(smd_rx, HC_HEAD);
	uint32_t got = (rx_head + smd_fifo_sz - rx_before) % smd_fifo_sz;
	if (got) {
		/* consume RPM's reply: tail of the RPM half channel is ours */
		hch_wr(smd_rx, HC_TAIL, rx_head);
		hch_wr(smd_rx, HC_FTAIL, 1);
		rpm_kick();
	}
	return got;
}

#define RPM_LDOA	0x616f646c	/* "ldoa" */
#define RPM_KEY_SWEN	0x6e657773	/* "swen" */
#define RPM_KEY_UV	0x00007675	/* "uv" */

static int sd_power_on(void)
{
	STEP("smem find rpm");
	if (!smd_find_rpm())
		return 0;
	STEP("smd open");
	if (!smd_open())
		return 0;
	static const uint32_t on_2v95[] = { RPM_KEY_UV, 2950000, RPM_KEY_SWEN, 1 };
	STEP("rpm L12 on");
	uint32_t a12 = rpm_req(RPM_LDOA, 12, on_2v95, 2);
	STEP("rpm L11 on");
	uint32_t a11 = rpm_req(RPM_LDOA, 11, on_2v95, 2);
	con_hex3("RPMREQ", "l12=", a12, "l11=", a11, "id=", rpm_msg_id);
	delay_ms(50);	/* rail ramp */
	return a12 && a11;
}

static void sd_probe(void)
{
	char name[6];

	STEP("sdc2 pads");
	/* clk 16 mA no pull, cmd/data pull-up 10 mA (Linux sdc2 "active") */
	wr32(TLMM_BASE + TLMM_SDC2_CTL, 3 << 11 | 3 << 9 | 7 << 6 | 4 << 3 | 4);
	int pwr = sd_power_on();
	con_hex3("SDPWR", "ok=", pwr, "pads=", rd32(TLMM_BASE + TLMM_SDC2_CTL), 0, 0);

	STEP("sdcc2 rcg 400k");
	wr32(GCC_BASE + GCC_SDCC2_M, 1);
	wr32(GCC_BASE + GCC_SDCC2_N, ~(4U - 1) & 0xff);
	wr32(GCC_BASE + GCC_SDCC2_D, ~4U & 0xff);
	wr32(GCC_BASE + GCC_SDCC2_CFG, (2 * 12 - 1) | 0 << 8 | 2 << 12);	/* XO /12, M/N 1/4, dual edge */
	wr32(GCC_BASE + GCC_SDCC2_CMD, rd32(GCC_BASE + GCC_SDCC2_CMD) | 1);	/* update */
	for (int i = 0; i < 100 && (rd32(GCC_BASE + GCC_SDCC2_CMD) & 1); i++)
		delay_ms(1);
	STEP("sdcc2 apps on");
	int apps = clk_on(GCC_SDCC2_APPS);
	con_hex3("SDCLK", "apps=", rd32(GCC_BASE + GCC_SDCC2_APPS), "cmd=", rd32(GCC_BASE + GCC_SDCC2_CMD),
		 "ok=", apps);
	if (!apps)
		return;

	STEP("sdhci mode + reset");
	wr32(SDC2_CORE + CORE_HC_MODE, 1 | 1U << 13);	/* HC_MODE_EN, FF_CLK_SW_RST_DIS */
	int rst = sd_reset(1);
	uint32_t pw0 = sd_pwr_ack(20);
	STEP("sd clock + power");
	hc_wr(HC_INT_EN, 0xffffffff);
	hc_wr(HC_CLK, 0x0e << 16 | 0x0001);		/* internal clock, base / 1 */
	for (int i = 0; i < 100 && !(hc(HC_CLK) & 2); i++)
		delay_ms(1);
	hc_wr(HC_CLK, hc(HC_CLK) | 0x0004);		/* SD clock on */
	hc_wr(HC_CTL, (hc(HC_CTL) & ~0xff00U) | 0x0f00);	/* 3.3 V, bus power */
	uint32_t pw1 = sd_pwr_ack(50);
	delay_ms(20);
	con_hex3("SDHC", "rst=", rst, "pw=", pw0 << 16 | pw1, "clk=", hc(HC_CLK));
	con_hex3("SDHC", "ctl=", hc(HC_CTL), "pstate=", hc(HC_PSTATE), 0, 0);

	STEP("cmd0");
	uint32_t e0 = sd_cmd(0, 0, RSP_NONE);
	delay_ms(2);
	STEP("cmd8");
	uint32_t e8 = sd_cmd(8, 0x1aa, RSP_48);
	uint32_t r8 = e8 ? 0 : sd_resp[0];
	con_hex3("CMD0/8", "e0=", e0, "e8=", e8, "r8=", r8);
	if (e8 & 0x30000) {
		con_str("SD", "no answer to CMD8: card absent or unpowered");
		return;
	}

	STEP("acmd41");
	uint32_t ocr = 0, e41 = 0;
	for (int i = 0; i < 100; i++) {
		e41 = sd_cmd(55, 0, RSP_48);
		if (!e41)
			e41 = sd_cmd(41, (r8 & 0xfff) == 0x1aa ? 0x40ff8000 : 0x00ff8000, RSP_48_NOCRC);
		if (e41)
			break;
		ocr = sd_resp[0];
		if (ocr & 0x80000000)
			break;
		delay_ms(10);
	}
	con_hex3("ACMD41", "err=", e41, "ocr=", ocr, 0, 0);
	if (e41 || !(ocr & 0x80000000))
		return;

	STEP("cmd2 cid");
	uint32_t e2 = sd_cmd(2, 0, RSP_136);
	con_hex3("CID", "e=", e2, "w3=", sd_resp[3], "w2=", sd_resp[2]);
	/* SDHCI drops the CRC byte: product name CID[103:64] = w2, w1 >> 24 */
	name[0] = sd_resp[2] >> 24;
	name[1] = sd_resp[2] >> 16;
	name[2] = sd_resp[2] >> 8;
	name[3] = sd_resp[2];
	name[4] = sd_resp[1] >> 24;
	name[5] = 0;
	con_str("CARD", name);

	STEP("cmd3 rca");
	uint32_t e3 = sd_cmd(3, 0, RSP_48);
	uint32_t rca = sd_resp[0] >> 16;
	STEP("cmd9 csd");
	uint32_t e9 = sd_cmd(9, rca << 16, RSP_136);
	/* CSD v2: C_SIZE = CSD[69:48] = w1[29:8]; capacity (C_SIZE + 1) * 512 KiB */
	uint32_t csize = (sd_resp[1] >> 8) & 0x3fffff;
	con_hex3("CSD", "e=", e3 << 16 | e9, "rca=", rca, "csize=", csize);
	con_hex3("SIZE", "MiB=", (csize + 1) / 2, "csd3=", sd_resp[3], 0, 0);
}

static void show_sd(void)
{
	STEP("gcc");
	con_hex3("SDCC1", "cmd=", rd32(GCC_BASE + GCC_SDCC1_CMD), "apps=", rd32(GCC_BASE + GCC_SDCC1_APPS),
		 "ahb=", rd32(GCC_BASE + GCC_SDCC1_AHB));
	con_hex3("SDCC2", "cmd=", rd32(GCC_BASE + GCC_SDCC2_CMD), "apps=", rd32(GCC_BASE + GCC_SDCC2_APPS),
		 "ahb=", rd32(GCC_BASE + GCC_SDCC2_AHB));
	con_hex3("SDCC2", "cfg=", rd32(GCC_BASE + GCC_SDCC2_CFG), 0, 0, 0, 0);

	STEP("tlmm gpio67");
	uintptr_t g = TLMM_BASE + SD_CD_GPIO * 0x1000;
	con_hex3("CD67", "cfg=", rd32(g), "in=", rd32(g + 4) & 1, 0, 0);

	STEP("sdcc2 ahb on");
	if (!clk_on(GCC_SDCC2_AHB)) {
		con_str("SDC2", "AHB clock stays off; not touching it");
		return;
	}
	STEP("sdc2 regs");
	con_hex3("SDC2", "ver=", rd32(SDC2_HC + 0xfc) >> 16, "pstate=", rd32(SDC2_HC + 0x24),
		 "cap=", rd32(SDC2_HC + 0x40));
	con_hex3("SDC2", "hostpwr=", rd32(SDC2_HC + 0x28), "clk=", rd32(SDC2_HC + 0x2c),
		 "cap1=", rd32(SDC2_HC + 0x44));
	con_hex3("CORE2", "pwr=", rd32(SDC2_CORE + 0x00), "ver=", rd32(SDC2_CORE + 0x50),
		 "mode=", rd32(SDC2_CORE + 0x78));
	con_hex3("CORE2", "pwrst=", rd32(SDC2_CORE + 0xdc), "pads=", rd32(TLMM_BASE + TLMM_SDC2_CTL), 0, 0);

	sd_probe();
}

void shim_main(uint64_t dtb)
{
	uint32_t runs = plog_init();
	STEP("wdt off");
	uint32_t wdt_was = rd32(WDT_BASE + WDT_EN);
	wr32(WDT_BASE + WDT_RST, 1);
	wr32(WDT_BASE + WDT_EN, 0);
	__asm__ volatile("dsb sy" ::: "memory");

	STEP("fb scan");
	detect_splash_format();
	STEP("clear screen");
	con_clear(0x203040);
	draw_ruler();
	con_str("T290", "milestone 3b: microSD power via RPM");
	con_hex3("RUN", "n=", runs, "dtb=", (uint32_t)dtb, 0, 0);
	STEP("regs");
	con_hex3("WDT", "en_was=", wdt_was, "en_now=", rd32(WDT_BASE + WDT_EN), 0, 0);
	show_sd();
	STEP("psci");
	con_hex3("PSCI", "ver=", (uint32_t)smc(PSCI_VERSION, 0, 0, 0), 0, 0, 0, 0);
	con_str("DONE", "take a photo of this screen");
	STEP("done");
	hold();
}
