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
static int cscale = 2;		/* font scale: 2 console, 3 menu buttons, 1 log viewer */
#define CH_W		(8 * cscale)
#define CH_H		(16 * cscale)
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
static int con_quiet;	/* 1: log to pstore only, keep the screen for new results */
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
			for (int sy = 0; sy < cscale; sy++)
				for (int sx = 0; sx < cscale; sx++)
					put_px(x0 + gx * cscale + sx, y0 + gy * cscale + sy, c);
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
static void __attribute__((unused)) draw_ruler(void)
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
	if (con_quiet)
		return;
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
	if (con_quiet)
		return;
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
 * Open rpm_requests once per boot: smd_open zeroes our FIFO indices, so a
 * second open (menu touch, GPU diagnostics, perf votes) would desync them.
 */
static int rpm_up(void)
{
	static int state = -1;
	if (state < 0)
		state = smd_find_rpm() && smd_open();
	return state;
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
	STEP("rpm up");
	if (!rpm_up())
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

#define HC_BLK		0x04	/* block size | block count << 16 */
#define HC_DATA		0x20
#define RSP_48B		0x1b	/* 48-bit busy, CRC, index */
#define CMD_DATA	0x20	/* data present */
#define XFER_READ	0x10

static uint32_t sd_sec[128];
static int sd_ok;		/* card selected (CMD7), sd_read_block usable */

/* Wait for normal-interrupt bits; returns the status, 0x8000 set on error. */
static uint32_t sd_wait_int(uint32_t bits, unsigned ms)
{
	uint64_t end = now() + timer_hz() / 1000 * ms;
	do {
		uint32_t st = hc(HC_INT);
		if (st & (bits | 0x8000)) {
			hc_wr(HC_INT, st & (bits | 0xffff0000U | 0x8000));
			return st;
		}
	} while (now() < end);
	return 0;
}

/* CMD17, PIO through the buffer data port. 0 = ok. */
static uint32_t sd_read_block(uint32_t lba)
{
	for (int i = 0; hc(HC_PSTATE) & 3; i++) {	/* CMD and DAT inhibit */
		if (i > 100)
			return 0x40000;
		delay_ms(1);
	}
	hc_wr(HC_INT, 0xffffffff);
	hc_wr(HC_BLK, 512 | 1 << 16);
	hc_wr(HC_ARG, lba);					/* SDHC: block address */
	hc_wr(HC_XFER_CMD, XFER_READ | (17 << 8 | CMD_DATA | RSP_48) << 16);
	uint32_t st = sd_wait_int(1, 100);
	if (!(st & 1) || (st & 0x8000))
		goto fail;
	st = sd_wait_int(0x20, 500);			/* buffer read ready */
	if (!(st & 0x20) || (st & 0x8000))
		goto fail;
	for (int i = 0; i < 128; i++)
		sd_sec[i] = hc(HC_DATA);
	st = sd_wait_int(2, 500);			/* transfer complete */
	if (!(st & 2) || (st & 0x8000))
		goto fail;
	return 0;
fail:
	sd_reset(2 | 4);				/* CMD and DAT lines */
	return (st >> 16) | 0x10000 | (st ? 0 : 0x20000);
}

/* CMD24, PIO: write sd_sec to one block. 0 = ok. */
static uint32_t sd_write_block(uint32_t lba)
{
	for (int i = 0; hc(HC_PSTATE) & 3; i++) {
		if (i > 100)
			return 0x40000;
		delay_ms(1);
	}
	hc_wr(HC_INT, 0xffffffff);
	hc_wr(HC_BLK, 512 | 1 << 16);
	hc_wr(HC_ARG, lba);
	hc_wr(HC_XFER_CMD, (24 << 8 | CMD_DATA | RSP_48) << 16);	/* transfer mode 0: write */
	uint32_t st = sd_wait_int(1, 100);
	if (!(st & 1) || (st & 0x8000))
		goto fail;
	st = sd_wait_int(0x10, 500);			/* buffer write ready */
	if (!(st & 0x10) || (st & 0x8000))
		goto fail;
	for (int i = 0; i < 128; i++)
		hc_wr(HC_DATA, sd_sec[i]);
	st = sd_wait_int(2, 1000);			/* transfer complete (after card busy) */
	if (!(st & 2) || (st & 0x8000))
		goto fail;
	return 0;
fail:
	sd_reset(2 | 4);
	return (st >> 16) | 0x10000 | (st ? 0 : 0x20000);
}

static void sd_show_sector(const char *label, uint32_t lba)
{
	uint32_t e = sd_read_block(lba), sum = 0;
	for (int i = 0; i < 128; i++)
		sum += sd_sec[i];
	con_hex3(label, "e=", e, "sum=", sum, "w0=", sd_sec[0]);
	con_hex3(label, "w110=", sd_sec[110], "w127=", sd_sec[127], 0, 0);
}

/* SDCC2 apps RCG: source (0 XO, 1 GPLL0), pre-divider, M/N (dual edge). */
static void sd_set_rcg(uint32_t src, uint32_t prediv, uint32_t m, uint32_t n)
{
	wr32(GCC_BASE + GCC_SDCC2_M, m);
	wr32(GCC_BASE + GCC_SDCC2_N, ~(n - m) & 0xff);
	wr32(GCC_BASE + GCC_SDCC2_D, ~n & 0xff);
	wr32(GCC_BASE + GCC_SDCC2_CFG, (2 * prediv - 1) | src << 8 | 2 << 12);
	wr32(GCC_BASE + GCC_SDCC2_CMD, rd32(GCC_BASE + GCC_SDCC2_CMD) | 1);
	for (int i = 0; i < 100 && (rd32(GCC_BASE + GCC_SDCC2_CMD) & 1); i++)
		delay_ms(1);
}

static void sd_probe(void)
{
	char name[6];

	STEP("sdc2 pads");
	/* clk 16 mA no pull, cmd/data pull-up 10 mA (Linux sdc2 "active") */
	wr32(TLMM_BASE + TLMM_SDC2_CTL, 3 << 11 | 3 << 9 | 7 << 6 | 4 << 3 | 4);
	int pwr = sd_power_on();
	con_quiet = 0;
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

	STEP("cmd7 select");
	uint32_t e7 = sd_cmd(7, rca << 16, RSP_48B);
	sd_wait_int(2, 100);	/* R1b: busy end = transfer complete */
	if (e7) {
		con_hex3("CMD7", "e=", e7, 0, 0, 0, 0);
		return;
	}
	sd_ok = 1;
	STEP("read lba0 400k 1bit");
	sd_show_sector("LBA0", 0);

	STEP("acmd6 4bit");
	uint32_t e6 = sd_cmd(55, rca << 16, RSP_48);
	if (!e6)
		e6 = sd_cmd(6, 2, RSP_48);
	if (!e6)
		hc_wr(HC_CTL, hc(HC_CTL) | 0x02);	/* host 4-bit */
	STEP("25 MHz");
	hc_wr(HC_CLK, hc(HC_CLK) & ~0x0004U);		/* SD clock off while switching */
	sd_set_rcg(1, 16, 1, 2);			/* GPLL0 800 / 16 = 50, M/N 1/2 -> 25 MHz */
	hc_wr(HC_CLK, hc(HC_CLK) | 0x0004);
	con_hex3("FAST", "e6=", e6, "ctl=", hc(HC_CTL), "rcg=", rd32(GCC_BASE + GCC_SDCC2_CMD));
	STEP("read lba32768 25M 4bit");
	sd_show_sector("LBA32K", 32768);

	STEP("speed 1 MiB");
	uint64_t t0 = now();
	uint32_t bad = 0;
	for (uint32_t i = 0; i < 2048 && !bad; i++)
		bad = sd_read_block(65536 + i);
	uint64_t us = (now() - t0) * 1000000 / timer_hz();
	con_hex3("SPEED", "err=", bad, "us=", (uint32_t)us, "KBps=", us ? (uint32_t)(1048576ULL * 1000 / us) : 0);
}

/*
 * ---- gpu1: is the Adreno 504 reachable? (build with -DT290_GPU_PROBE) ----
 * t290/GPU.md: oxili_gx GDSC on and unclamped, gfx3d at 200 MHz from GPLL0,
 * the GPU clock branches on, then read a few status registers. Every step is
 * logged first, so a reset names the access that caused it.
 */
#define GCC_GFX3D_CMD		0x59000
#define GCC_GFX3D_CFG		0x59004
#define GCC_OXILI_GX_GDSCR	0x5901c
#define GCC_OXILI_CX_GDSCR	0x59044
#define GCC_OXILI_GFX3D_CBCR	0x59020
#define GCC_OXILI_AHB_CBCR	0x59028
#define GCC_BIMC_GFX_CBCR	0x59034
#define GCC_GX_CLAMP_IO		0x5b00c
#define GPU_BASE		0x01c00000UL
#define A5XX(r)			(GPU_BASE + 4 * (r))
#define A5XX_RBBM_STATUS	0x4f5
#define A5XX_RBBM_INT_0_STATUS	0x4e1
#define A5XX_RBBM_SECVID_TRUST	0xf400
#define A5XX_CP_HW_FAULT	0xb1a
#define A5XX_CP_PFP_ME_CNTL	0x832
#define A5XX_RBBM_AHB_CNTL0	0x93

static uint32_t gdsc_on(uint32_t gdscr)
{
	uintptr_t r = GCC_BASE + gdscr;
	wr32(r, rd32(r) & ~1U);			/* clear SW_COLLAPSE */
	for (int i = 0; i < 100 && !(rd32(r) & (1U << 31)); i++)
		delay_ms(1);			/* PWR_ON */
	return rd32(r);
}

#if defined(T290_GPU2)
/*
 * ---- gpu2: "Hello, GPU" without Android (build with -DT290_GPU2 and FW_DIR) ----
 * Linux a5xx_hw_init()/a5xx_me_init() on bare metal: PM4/PFP microcode
 * (firmware minus its first dword, as adreno_fw_create_bo) and a 32 KiB ring
 * in plain DRAM, CP started, CP_ME_INIT, out of secure mode by writing
 * RBBM_SECVID_TRUST_CNTL (the A504 loads no zap shader under Android), then
 * CP_MEM_WRITE draws a green 100x100 square into the splash. The GPU sees
 * physical addresses only if its SMMU passes them through: that is the test.
 */
extern const uint8_t gpu_pm4_fw[], gpu_pm4_fw_end[], gpu_pfp_fw[], gpu_pfp_fw_end[];	/* fw.S */

#define GPU_MEM		0x90800000UL	/* spare part of the splash reservation (0xb0000000 hung, gpu2 runs 1-8) */
#define GPU_PM4		(GPU_MEM + 0x00000)
#define GPU_PFP		(GPU_MEM + 0x10000)
#define GPU_RING	(GPU_MEM + 0x20000)
#define GPU_RING_SZ	0x8000
#define A5XX_CP_RB_BASE		0x800
#define A5XX_CP_RB_CNTL		0x802
#define A5XX_CP_RB_RPTR		0x806
#define A5XX_CP_RB_WPTR		0x807
#define A5XX_CP_PFP_INSTR_BASE	0x835
#define A5XX_CP_ME_INSTR_BASE	0x838

static unsigned odd_parity(unsigned v)
{
	v ^= v >> 16;
	v ^= v >> 8;
	v ^= v >> 4;
	v &= 0xf;
	return (~0x6996u >> v) & 1;
}

static uint32_t pm4_pkt7(unsigned op, unsigned cnt)
{
	return 0x70000000u | (cnt & 0x3fff) | (odd_parity(cnt) << 15) |
	       ((op & 0x7f) << 16) | (odd_parity(op) << 23);
}

static void gpu_wr(uint32_t r, uint32_t v) { wr32(A5XX(r), v); }
static uint32_t gpu_rd(uint32_t r) { return rd32(A5XX(r)); }

static void copy_fw(uintptr_t dst, const uint8_t *fw, const uint8_t *end)
{
	/* skip the version dword; bytes because the blob may be unaligned */
	uint32_t len = (uint32_t)(end - fw);
	for (uint32_t i = 4; i + 3 < len; i += 4)
		wr32(dst + i - 4, fw[i] | fw[i + 1] << 8 | fw[i + 2] << 16 | (uint32_t)fw[i + 3] << 24);
}

static uint32_t ring_n;
static void ring_out(uint32_t v)
{
	wr32(GPU_RING + 4 * ring_n, v);
	ring_n = (ring_n + 1) % (GPU_RING_SZ / 4);
}

/* in case LK left the D-cache on: push microcode, ring, page table to DRAM */
static void gpu_flush(void)
{
	for (uintptr_t a = GPU_MEM; a < GPU_MEM + 0x40000; a += 64)
		__asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

static void gpu_kick_wait(const char *what)
{
	gpu_flush();
	gpu_wr(A5XX_CP_RB_WPTR, ring_n);
	for (int i = 0; i < 500 && gpu_rd(A5XX_CP_RB_RPTR) != ring_n; i++)
		delay_ms(1);
	con_hex3(what, "rptr=", gpu_rd(A5XX_CP_RB_RPTR), "wptr=", ring_n,
		 "status=", gpu_rd(A5XX_RBBM_STATUS));
}

/*
 * gpu_boot = 1: bring the GPU up for xnu only (no square, no BIMC/regdump);
 * gpu_ready says whether the CP consumed CP_ME_INIT. xnu's
 * bsd/dev/sweet_t290_gpu.c then continues on the same ring (boot-arg t290gpu=1).
 */
static int gpu_boot, gpu_ready;

static void gpu_hello(void)
{
	/*
	 * gpu2 run 1: CP took no ring dword (rptr 0, status c00003c1, no fault).
	 * The GPU's memory path: gcc_bimc_gpu_clk (kgsl alt_mem_iface_clk) was
	 * still off; also vote the GFX TBU/TCU and SMMU config clocks.
	 */
	STEP("gpu2 mem clocks");
	int bg = clk_on(0x59030);			/* gcc_bimc_gpu_clk */
	wr32(GCC_BASE + 0x4500c, rd32(GCC_BASE + 0x4500c) | 1U << 2 | 1U << 3 | 1U << 12 | 1U << 13);
	delay_ms(1);
	con_hex3("MEMCLK", "bimcgpu=", rd32(GCC_BASE + 0x59030) | bg, "vote=", rd32(GCC_BASE + 0x4500c),
		 "tbu=", rd32(GCC_BASE + 0x12010));

	/*
	 * gpu2 run 3: RBBM_STATUS c00003c1 = CP + VBIF busy, no SMMU fault: the
	 * CP's reads leave and never return. kgsl-iommu has
	 * qcom,micro-mmu-control = <0x6000>: the SMMU's implementation-defined
	 * MICRO_MMU_CTRL (bit 2 LOCAL_HALT_REQ, bit 3 IDLE), which kgsl sets to
	 * halt the GPU's TBU around page-table switches. Release a halt.
	 */
	/*
	 * gpu2 run 6: hwcg as Linux, still CP + VBIF busy. kgsl votes the bus
	 * through the RPM ("grp3d": mas-oxili, mas-rpm-id 6 -> EBI) and the RPM
	 * clock bimc_gpu (MEM_CLK id 2): do the same.
	 */
	STEP("gpu2 rpm votes");
	{
		static const uint32_t khz[] = { 0x007a484b /* "KHz" */, 400000 };
		static const uint32_t bw[] = { 0x00007762 /* "bw" */, 2000000000 };
		con_quiet = 1;
		int rpm = rpm_up();
		uint32_t g = rpm ? rpm_req(0x326b6c63 /* "clk2" */, 2, khz, 1) : 0;
		uint32_t m = rpm ? rpm_req(0x73616d62 /* "bmas" */, 6, bw, 1) : 0;
		uint32_t s = rpm ? rpm_req(0x766c7362 /* "bslv" */, 0, bw, 1) : 0;
		con_quiet = 0;
		con_hex3("RPMBW", "gpuclk=", g | (uint32_t)rpm << 31, "mas=", m, "slv=", s);
	}

	/*
	 * gpu2 runs 8-9: VBIF port 0 keeps one read outstanding whatever the
	 * buffer address (0xb0000000 or the splash reservation): stuck in the
	 * SMMU. Downstream arm-smmu, for an SMMU with qcom,tz-device-id, asks TZ
	 * to restore its secure configuration at every power-up:
	 * scm_restore_sec_cfg(TZ_DEVICE_GPU = 18, 0), SIP MP (2) cmd 2, SMC64.
	 */
	/*
	 * gpu2 run 10: TZ refused that (ret -5). Do what kgsl does instead:
	 * translate through context bank 0 (stream 0 -> CB0, set up by TZ) with
	 * an identity table, AArch64 format, 4 KiB granule, 32-bit VA starting at
	 * level 1: two 1 GiB blocks for the DRAM at 0x80000000-0xffffffff.
	 */
	/* run 10 used service 2 (PIL) by mistake: SCM_SVC_MP is 0xC */
	STEP("gpu2 scm restore");
	uint64_t sret = smc(0x42000C02, 2 /* SCM_ARGS(2) */, 18 /* TZ_DEVICE_GPU */, 0);
	con_hex3("SCM", "ret=", (uint32_t)sret, "cr0=", rd32(0x01c40000UL), "s2cr0=", rd32(0x01c40000UL + 0xc00));

	STEP("gpu2 smmu cb0");
	{
		/*
		 * run 11: CB0 fsr 0x402 (translation fault, AArch64) at the PFP
		 * microcode 0x90810000, inside the mapped block: the walk saw
		 * zeros? Log SCTLR_EL1 (caches?), clean the buffers to DRAM, use
		 * the usual 39-bit VA (T0SZ 25, level 1 = 512 entries).
		 */
		const uintptr_t smmu = 0x01c40000UL, cb = smmu + 0x8000, pt = GPU_MEM + 0x30000;
		for (unsigned i = 0; i < 512; i++)
			wr32(pt + 8 * i, 0), wr32(pt + 8 * i + 4, 0);
		for (unsigned i = 2; i < 4; i++)	/* block | AttrIdx 0 | AP[1] | SH inner | AF */
			wr32(pt + 8 * i, (uint32_t)i << 30 | 1U << 10 | 1U << 6 | 1);	/* AF=0 gave AFF at the PFP fetch (run 19): the walk reads DRAM */
		for (uintptr_t a = GPU_MEM; a < GPU_MEM + 0x40000; a += 64)
			__asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
		__asm__ volatile("dsb sy" ::: "memory");
		con_hex3("EL1", "sctlr=", (uint32_t)rsr("sctlr_el1"), "el=", (uint32_t)rsr("CurrentEL"),
			 "pt2=", rd32(pt + 16));
		wr32(cb + 0x0, 0);			/* CB_SCTLR: off while changing */
		wr32(smmu + 0x1800, 0);			/* CBA2R0: AArch32 format (TZ owns it, run 12) */
		wr32(cb + 0x38, 0x44);			/* CB_MAIR0: attr0 normal non-cacheable */
		wr32(cb + 0x3c, 0);
		wr32(cb + 0x10, 1);			/* CB_TCR2: PASize 36 bit */
		wr32(cb + 0x30, 1U << 31 | 1U << 23);	/* CB_TTBCR: EAE (LPAE), T0SZ 0 = 32-bit VA from level 1, EPD1 */
		wr32(cb + 0x20, (uint32_t)pt);		/* CB_TTBR0, ASID 0 */
		wr32(cb + 0x24, 0);
		wr32(cb + 0x58, rd32(cb + 0x58));	/* clear CB_FSR */
		wr32(cb + 0x618, 0);			/* CB_S1_TLBIALL */
		wr32(cb + 0x7f0, 0);			/* CB_TLBSYNC */
		for (int i = 0; i < 100 && (rd32(cb + 0x7f4) & 1); i++)
			delay_ms(1);
		wr32(cb + 0x0, 1 | 1U << 1 | 1U << 2 | 1U << 5 | 1U << 6);	/* M TRE AFE CFRE CFIE */
		__asm__ volatile("dsb sy" ::: "memory");
		con_hex3("CB0SET", "sctlr=", rd32(cb + 0x0), "tcr=", rd32(cb + 0x30), "ttbr=", rd32(cb + 0x20));
		con_hex3("CB0SET", "cba2r=", rd32(smmu + 0x1800), "fsr=", rd32(cb + 0x58), "tlbst=", rd32(cb + 0x7f4));
	}

	/*
	 * run 18: TZ restore_sec_cfg (MP 0xC cmd 2) succeeded, nothing changed:
	 * the CP's reads leave the SMMU and never come back. Next stop: the GPU's
	 * BIMC master port (mas-oxili, qport 2, "fixed" QoS, ap-owned: Linux
	 * msm_bus programs it on the first vote). Registers per Linux
	 * msm_bus_bimc_adhoc.c: M_REG_BASE = bimc + 0x8000, 0x4000 per port.
	 * Port 0 (apps, works) for comparison; then BKE (bandwidth keeper) off.
	 */
	STEP("gpu2 bimc read");
	if (!gpu_boot) {
		static const uint16_t off[] = { 0x000, 0x200, 0x210, 0x230, 0x240, 0x250, 0x300, 0x310, 0x320, 0x330 };
		for (unsigned port = 0; port <= 2; port += 2) {
			uintptr_t m = 0x00400000UL + 0x8000 + 0x4000 * port;
			for (unsigned i = 0; i < sizeof(off) / sizeof(off[0]); i += 3) {
				plog_str(">> bimc ");
				plog_hex(port, 1);
				plog_putc(' ');
				plog_hex(off[i], 3);
				plog_str("\n");
				con_hex3(port ? "BIMC2" : "BIMC0", "a=", rd32(m + off[i]),
					 i + 1 < 10 ? "b=" : 0, i + 1 < 10 ? rd32(m + off[i + 1]) : 0,
					 i + 2 < 10 ? "c=" : 0, i + 2 < 10 ? rd32(m + off[i + 2]) : 0);
			}
		}
		uintptr_t m2 = 0x00400000UL + 0x8000 + 0x4000 * 2;
		STEP("gpu2 bimc bke off");
		if (rd32(m2 + 0x300) & 1)
			wr32(m2 + 0x300, rd32(m2 + 0x300) & ~1U);	/* M_BKE_EN */
		con_hex3("BIMC2", "bke=", rd32(m2 + 0x300), "prio=", rd32(m2 + 0x230), 0, 0);
	}

	STEP("gpu2 micro mmu");
	const uintptr_t mmu_ctrl = 0x01c40000UL + 0x6000;
	uint32_t mm0 = rd32(mmu_ctrl);
	wr32(mmu_ctrl, mm0 & ~(1U << 2));
	delay_ms(1);
	con_hex3("UMMU", "before=", mm0, "after=", rd32(mmu_ctrl), 0, 0);

	STEP("gpu2 microcode");
	copy_fw(GPU_PM4, gpu_pm4_fw, gpu_pm4_fw_end);
	copy_fw(GPU_PFP, gpu_pfp_fw, gpu_pfp_fw_end);
	con_hex3("FW", "pm4=", (uint32_t)(gpu_pm4_fw_end - gpu_pm4_fw), "pfp=",
		 (uint32_t)(gpu_pfp_fw_end - gpu_pfp_fw), "pm4v=", rd32(GPU_PM4));
	for (uint32_t i = 0; i < GPU_RING_SZ; i += 4)
		wr32(GPU_RING + i, 0);

	/*
	 * gpu2 run 4: micro-MMU not halted (0x378), still CP + VBIF busy.
	 * Linux a5xx_pm_resume for A50x: soft reset, halt sp_input_clk, hwcg on
	 * (a50x_hwcg table), sp_input_clk back on.
	 */
	/* (RBBM_SW_RESET_CMD here reset the whole SoC: gpu2 run 5) */
	STEP("gpu2 hwcg");
	static const uint32_t a50x_hwcg[][2] = {
		{ 0x0068, 0x02222222 }, { 0x006c, 0x02222220 }, { 0x0070, 0x0000F3CF }, { 0x0074, 0x00000080 },
		{ 0x004a, 0x22222222 }, { 0x004e, 0x22222222 }, { 0x0052, 0x00002222 }, { 0x00b0, 0x77777777 },
		{ 0x00b4, 0x77777777 }, { 0x00b8, 0x00007777 }, { 0x00a4, 0x11111111 }, { 0x00a8, 0x11111111 },
		{ 0x00ac, 0x00001111 }, { 0x005b, 0x22222222 }, { 0x005c, 0x22222222 }, { 0x005d, 0x00222222 },
		{ 0x005a, 0x22222222 }, { 0x005e, 0x00FFFFF4 }, { 0x005f, 0x00000002 }, { 0x0078, 0x22222222 },
		{ 0x007c, 0x00222222 }, { 0x0082, 0x00022220 }, { 0x008a, 0x05522222 }, { 0x008b, 0x00505555 },
		{ 0x0086, 0x04040404 }, { 0x0080, 0x07444044 }, { 0x008c, 0x00000002 }, { 0x0081, 0x00010011 },
		{ 0x0063, 0x04222222 }, { 0x0060, 0x02222222 }, { 0x0091, 0x00002222 }, { 0x0064, 0x00000000 },
		{ 0x0062, 0x04104004 }, { 0x0090, 0x00000000 }, { 0x0066, 0x00000000 }, { 0x0065, 0x00004000 },
		{ 0x0061, 0x00000200 }, { 0x0092, 0x00002222 },
	};
	gpu_wr(0x0067, 0x00000055);		/* RBBM_CLOCK_CNTL: halt sp_input_clk */
	for (unsigned i = 0; i < sizeof(a50x_hwcg) / sizeof(a50x_hwcg[0]); i++)
		gpu_wr(a50x_hwcg[i][0], a50x_hwcg[i][1]);
	gpu_wr(0x0067, 0xaaa8aa00);
	gpu_wr(0x0533, 0x182);			/* RBBM_ISDB_CNT */
	gpu_wr(0x0067, gpu_rd(0x0067) & ~0xffU);	/* sp_input_clk on */
	con_hex3("HWCG", "clkcntl=", gpu_rd(0x0067), "status=", gpu_rd(A5XX_RBBM_STATUS), 0, 0);

	/*
	 * gpu2 run 7: RPM bus votes acked, still CP + VBIF busy. VBIF state:
	 * XIN_HALT_CTRL0 (Linux halts the XIN ports before power collapse),
	 * CTRL1 (halt ack), CLKON; release any halt.
	 */
	STEP("gpu2 vbif");
	con_hex3("VBIF", "ver=", gpu_rd(0x3000), "clkon=", gpu_rd(0x3001), "halt=", gpu_rd(0x3080));
	gpu_wr(0x3080, 0);			/* VBIF_XIN_HALT_CTRL0 */
	con_hex3("VBIF", "hack=", gpu_rd(0x3081), "ahberr=", gpu_rd(0x04f4), "st3=", gpu_rd(0x0530));

	STEP("gpu2 hw init");
	gpu_wr(0x3049, 0x00000003);		/* VBIF_ROUND_ROBIN_QOS_ARB */
	gpu_wr(0x0093, 0x00000001);		/* RBBM_AHB_CNTL0 */
	gpu_wr(0x002f, 1U << 30 | 0xffff);	/* RBBM_INTERFACE_HANG_INT_CNTL */
	gpu_wr(0x0464, 0x01);			/* RBBM_PERFCTR_CNTL */
	gpu_wr(0x0e96, 0x02);			/* UCHE_CACHE_WAYS */
	gpu_wr(0x0e89, 0xffff0000);		/* UCHE_TRAP_BASE */
	gpu_wr(0x0e8a, 0x0001ffff);
	gpu_wr(0x0e87, 0xffff0000);		/* UCHE_WRITE_THRU_BASE */
	gpu_wr(0x0e88, 0x0001ffff);
	gpu_wr(0x0e8b, 0x00100000);		/* UCHE_GMEM_RANGE_MIN */
	gpu_wr(0x0e8c, 0x00000000);
	gpu_wr(0x0e8d, 0x00100000 + 0x22000 - 1);	/* MAX: gmem 0x22000 (kgsl on A504) */
	gpu_wr(0x0e8e, 0x00000000);
	gpu_wr(0x0825, 0x20);			/* CP_MEQ_THRESHOLDS (a506 values) */
	gpu_wr(0x0826, 0x400);			/* CP_MERCIU_SIZE */
	gpu_wr(0x0820, 0x40000030);		/* CP_ROQ_THRESHOLDS_2 */
	gpu_wr(0x081f, 0x20100d0a);		/* CP_ROQ_THRESHOLDS_1 */
	gpu_wr(0x0d00, 0x100 << 11 | 0x100 << 22);	/* PC_DBG_ECO_CNTL */
	gpu_wr(0x0e81, 1U << 29);		/* UCHE_MODE_CNTL */
	gpu_wr(0x0833, 0x02000000);		/* CP_CHICKEN_DBG */
	gpu_wr(0x0094, 0xa6ffffff);		/* RBBM_AHB_CNTL1 */
	gpu_wr(0x0095, 0x0000003f);		/* RBBM_AHB_CNTL2 */
	gpu_wr(0x0f02, 1U << 7);		/* TPL1_MODE_CNTL */
	gpu_wr(0x0cc6, 1U << 1);		/* RB_MODE_CNTL */
	gpu_wr(0x08a0, 0);			/* CP_PROTECT_CNTL: no register protection */
	static const uint16_t addr_mode[] = { 0x860, 0xbc1, 0xc81, 0xcc5, 0xd01, 0xe05, 0xe41,
					      0xe61, 0xe80, 0xec1, 0xf01, 0xf810 };
	for (unsigned i = 0; i < sizeof(addr_mode) / sizeof(addr_mode[0]); i++)
		gpu_wr(addr_mode[i], 1);	/* *_ADDR_MODE_CNTL: 64-bit addresses */
	gpu_wr(0xf800, 0);			/* RBBM_SECVID_TSB_TRUSTED_BASE */
	gpu_wr(0xf801, 0);
	gpu_wr(0xf802, 0);			/* RBBM_SECVID_TSB_TRUSTED_SIZE */

	/*
	 * gpu2 run 2: SMMU on, stream 0 -> CB0 with translation off (bypass),
	 * no global fault, yet the CP read nothing. The GPU still was secure
	 * when the CP fetched its microcode (SECVID went 0 only after ME_INIT):
	 * leave secure mode before the CP starts.
	 */
	STEP("gpu2 secvid off");
	gpu_wr(A5XX_RBBM_SECVID_TRUST, 0);
	con_hex3("SECVID", "trust=", gpu_rd(A5XX_RBBM_SECVID_TRUST), "int0=",
		 gpu_rd(A5XX_RBBM_INT_0_STATUS), 0, 0);

	STEP("gpu2 cp start");
	gpu_flush();
	gpu_wr(A5XX_CP_ME_INSTR_BASE, (uint32_t)GPU_PM4);
	gpu_wr(A5XX_CP_ME_INSTR_BASE + 1, 0);
	gpu_wr(A5XX_CP_PFP_INSTR_BASE, (uint32_t)GPU_PFP);
	gpu_wr(A5XX_CP_PFP_INSTR_BASE + 1, 0);
	gpu_wr(A5XX_CP_RB_BASE, (uint32_t)GPU_RING);
	gpu_wr(A5XX_CP_RB_BASE + 1, 0);
	gpu_wr(A5XX_CP_RB_CNTL, 12 | 2 << 8 | 1U << 27);	/* 32 KiB, 32 B blocks, NO_UPDATE */
	gpu_wr(A5XX_CP_RB_WPTR, 0);
	ring_n = 0;
	gpu_wr(A5XX_CP_PFP_ME_CNTL, 0);
	delay_ms(5);
	con_hex3("CPSTART", "pfp_me=", gpu_rd(A5XX_CP_PFP_ME_CNTL), "status=", gpu_rd(A5XX_RBBM_STATUS),
		 "fault=", gpu_rd(A5XX_CP_HW_FAULT));

	uint32_t ps[2], ms[2];
	for (unsigned i = 0; i < 2; i++) {
		gpu_wr(0x0808, i);		/* CP_PFP_STAT_ADDR */
		ps[i] = gpu_rd(0x0809);
		gpu_wr(0x081a, i);		/* CP_ME_STAT_ADDR */
		ms[i] = gpu_rd(0x0b14);
	}
	con_hex3("CPSTAT", "pfp0=", ps[0], "pfp1=", ps[1], "me0=", ms[0]);

	STEP("gpu2 me init");
	ring_out(pm4_pkt7(72, 8));		/* CP_ME_INIT */
	ring_out(0x0000002f);
	ring_out(0x00000003);
	ring_out(0x20000000);
	ring_out(0);
	ring_out(0);
	ring_out(0);
	ring_out(0);
	ring_out(0);
	gpu_kick_wait("MEINIT");
	gpu_ready = gpu_rd(A5XX_CP_RB_RPTR) == ring_n;
	if (gpu_boot)
		return;

	/*
	 * Green square: screen x 301..400 = buffer x 72..171 (229 px shift),
	 * 300 bytes = 75 dwords per row, bytes R,B,G.
	 */
	STEP("gpu2 mem write");
	static const uint8_t px[3] = { 0x00, 0x00, 0xff };	/* R, B, G: green */
	for (unsigned y = 0; y < 100; y++) {
		uintptr_t a = SPLASH_PHYS + (uintptr_t)(700 + y) * FB_W * 3 + 72 * 3;
		ring_out(pm4_pkt7(61, 2 + 75));		/* CP_MEM_WRITE */
		ring_out((uint32_t)a);
		ring_out(0);
		for (unsigned w = 0; w < 75; w++) {
			uint32_t v = 0;
			for (unsigned k = 0; k < 4; k++)
				v |= (uint32_t)px[(4 * w + k) % 3] << (8 * k);
			ring_out(v);
		}
	}
	gpu_kick_wait("MEMWR");
	con_hex3("AFTER", "int0=", gpu_rd(A5XX_RBBM_INT_0_STATUS), "fault=", gpu_rd(A5XX_CP_HW_FAULT),
		 "pix=", rd32(SPLASH_PHYS + 750 * FB_W * 3 + 72 * 3));
	con_str("GPU2", "green square at screen 301..400 x 700..799 = drawn by the GPU");

	/* the GPU's SMMU (qcom,smmu-v2, TZ owns the global space): who blocks? */
	STEP("gpu2 smmu read");
	const uintptr_t smmu = 0x01c40000UL, cb = smmu + 0x8000;
	con_hex3("SMMU", "cr0=", rd32(smmu + 0x0), "gfsr=", rd32(smmu + 0x48), "idr1=", rd32(smmu + 0x24));
	con_hex3("SMMU", "smr0=", rd32(smmu + 0x800), "s2cr0=", rd32(smmu + 0xc00), "cbar0=", rd32(smmu + 0x1000));
	con_hex3("CB0", "sctlr=", rd32(cb + 0x0), "fsr=", rd32(cb + 0x58), "far=", rd32(cb + 0x60));
	con_hex3("CB0", "fsynr=", rd32(cb + 0x68), "ttbr0=", rd32(cb + 0x20), "farhi=", rd32(cb + 0x64));
	con_hex3("SMR", "1=", rd32(smmu + 0x804), "2=", rd32(smmu + 0x808), "3=", rd32(smmu + 0x80c));
	con_hex3("S2CR", "1=", rd32(smmu + 0xc04), "2=", rd32(smmu + 0xc08), "3=", rd32(smmu + 0xc0c));
	con_hex3("CB", "0fsr=", rd32(cb + 0x58), "1fsr=", rd32(cb + 0x1058), "2fsr=", rd32(cb + 0x2058));
	con_hex3("CB2", "sctlr=", rd32(cb + 0x2000), "far=", rd32(cb + 0x2060), "cbar2=", rd32(smmu + 0x1008));
	con_hex3("VBIF", "halt=", gpu_rd(0x3080), "hack=", gpu_rd(0x3081), "clkon=", gpu_rd(0x3001));
	con_hex3("AHB", "err=", gpu_rd(0x04f4), "mesplit=", gpu_rd(0x04f0), "pfpsplit=", gpu_rd(0x04f1));
	con_hex3("UMMU", "end=", rd32(smmu + 0x6000), "rptr=", gpu_rd(A5XX_CP_RB_RPTR),
		 "status=", gpu_rd(A5XX_RBBM_STATUS));

	/*
	 * The same ranges as t290/gpu/kgsl_regdump (Android's working GPU,
	 * logs/kgsl-regdump.txt), pstore only: "R reg value", nonzero only.
	 */
	STEP("gpu2 regdump");
	static const uint32_t dump[][2] = {
		{ 0x0000, 0x00c0 }, { 0x0460, 0x0010 }, { 0x04e0, 0x0060 }, { 0x0800, 0x0040 },
		{ 0x0860, 0x0002 }, { 0x08a0, 0x0020 }, { 0x0b00, 0x0020 }, { 0x0bc0, 0x0002 },
		{ 0x0c80, 0x0002 }, { 0x0cc0, 0x0008 }, { 0x0d00, 0x0002 }, { 0x0e00, 0x0008 },
		{ 0x0e40, 0x0002 }, { 0x0e60, 0x0002 }, { 0x0e80, 0x0020 }, { 0x0ec0, 0x0002 },
		{ 0x0f00, 0x0004 }, { 0x3000, 0x0090 }, { 0xf400, 0x0004 }, { 0xf800, 0x0012 },
	};
	for (unsigned r = 0; r < sizeof(dump) / sizeof(dump[0]); r++)
		for (uint32_t i = 0; i < dump[r][1]; i++) {
			uint32_t reg = dump[r][0] + i, v = gpu_rd(reg);
			if (!v)
				continue;
			plog_str("R ");
			plog_hex(reg, 5);
			plog_putc(' ');
			plog_hex(v, 8);
			plog_str("\n");
		}
	con_str("REGDUMP", "in pstore");
}
#endif

static void __attribute__((unused)) gpu_probe(void)
{
	con_quiet = 0;
	STEP("gpu gdsc cx");
	uint32_t cx = gdsc_on(GCC_OXILI_CX_GDSCR);
	STEP("gpu gdsc gx");
	uint32_t gx = gdsc_on(GCC_OXILI_GX_GDSCR);
	con_hex3("GDSC", "cx=", cx, "gx=", gx, "clamp=", rd32(GCC_BASE + GCC_GX_CLAMP_IO));
	STEP("gpu unclamp");
	wr32(GCC_BASE + GCC_GX_CLAMP_IO, 0);
	STEP("gpu gfx3d 200M");
	wr32(GCC_BASE + GCC_GFX3D_CFG, (2 * 4 - 1) | 1 << 8);	/* GPLL0 / 4 */
	wr32(GCC_BASE + GCC_GFX3D_CMD, rd32(GCC_BASE + GCC_GFX3D_CMD) | 1);
	for (int i = 0; i < 100 && (rd32(GCC_BASE + GCC_GFX3D_CMD) & 1); i++)
		delay_ms(1);
	STEP("gpu branches");
	int a = clk_on(GCC_OXILI_AHB_CBCR);
	int b = clk_on(GCC_BIMC_GFX_CBCR);
	int c = clk_on(GCC_OXILI_GFX3D_CBCR);
	con_hex3("GPUCLK", "ahb=", rd32(GCC_BASE + GCC_OXILI_AHB_CBCR) | a,
		 "bimc=", rd32(GCC_BASE + GCC_BIMC_GFX_CBCR) | b,
		 "core=", rd32(GCC_BASE + GCC_OXILI_GFX3D_CBCR) | c);
	con_hex3("GFX3D", "cmd=", rd32(GCC_BASE + GCC_GFX3D_CMD), "cfg=", rd32(GCC_BASE + GCC_GFX3D_CFG), 0, 0);
	/*
	 * run 19: the live FDT puts gdsc_oxili_cx at 0x185904c, not 0x59044
	 * (which reads like a GDSC too: a0222800). kgsl also enables
	 * rbbmtimer_clk and alwayson_clk, never switched on here. Look at
	 * 0x59040-0x5904c and switch on what each looks like: a GDSC (wait
	 * fields 0x222xxx) is powered up, a branch CBCR is enabled.
	 */
	STEP("gpu gcc 59040-5904c");
	con_hex3("GCC59", "40=", rd32(GCC_BASE + 0x59040), "44=", rd32(GCC_BASE + 0x59044),
		 "48=", rd32(GCC_BASE + 0x59048));
	con_hex3("GCC59", "4c=", rd32(GCC_BASE + 0x5904c), 0, 0, 0, 0);
	for (uint32_t off = 0x59040; off <= 0x5904c; off += 4) {
		uint32_t v = rd32(GCC_BASE + off);
		if (off == GCC_OXILI_CX_GDSCR)
			continue;			/* handled above since gpu1 */
		if ((v & 0x00fff000) == 0x00222000) {
			plog_str(">> gpu gdsc on ");
			plog_hex(off, 5);
			plog_str("\n");
			gdsc_on(off);
		} else if (v & CBCR_CLK_OFF) {
			plog_str(">> gpu clk on ");
			plog_hex(off, 5);
			plog_str("\n");
			clk_on(off);
		}
	}
	con_hex3("GCC59", "40=", rd32(GCC_BASE + 0x59040), "48=", rd32(GCC_BASE + 0x59048),
		 "4c=", rd32(GCC_BASE + 0x5904c));
	if (!a || !c) {
		con_str("GPU", "clocks did not start; not touching the GPU");
		return;
	}
	STEP("gpu read rbbm_status");
	uint32_t st = rd32(A5XX(A5XX_RBBM_STATUS));
	STEP("gpu read int/fault");
	con_hex3("RBBM", "status=", st, "int0=", rd32(A5XX(A5XX_RBBM_INT_0_STATUS)),
		 "cpfault=", rd32(A5XX(A5XX_CP_HW_FAULT)));
	STEP("gpu read me_cntl/ahb");
	con_hex3("CP", "pfp_me=", rd32(A5XX(A5XX_CP_PFP_ME_CNTL)), "ahb0=", rd32(A5XX(A5XX_RBBM_AHB_CNTL0)), 0, 0);
	STEP("gpu read secvid");
	con_hex3("SECVID", "trust=", rd32(A5XX(A5XX_RBBM_SECVID_TRUST)), 0, 0, 0, 0);
	con_str("GPU", "registers answered");
#if defined(T290_GPU2)
	gpu_hello();
#endif
}

/*
 * ---- touch1: FocalTech FT5452 on BLSP1 QUP3 I2C (build with -DT290_TOUCH_PROBE) ----
 * Live FDT: i2c@78b7000 (qcom,i2c-msm-v2, 400 kHz from 19.2 MHz), SDA/SCL
 * GPIO10/11 function 2 (blsp_i2c3), touch @0x38, reset GPIO64, IRQ GPIO65
 * (active low), rails pm8953 L10 3.3 V / L5 1.8 V (RPM). QUP v2 tags in
 * FIFO mode as Linux i2c-qup.c: START+addr, DATAWR n, data, START+addr|1,
 * DATARD_STOP n; the input FIFO returns data only.
 */
#define QUP_PHYS		0x078b7000UL
#define QUP_CONFIG		0x000
#define QUP_STATE		0x004
#define QUP_IO_MODE		0x008
#define QUP_SW_RESET		0x00c
#define QUP_OPERATIONAL		0x018
#define QUP_ERROR_FLAGS		0x01c
#define QUP_HW_VERSION		0x030
#define QUP_OUT_FIFO		0x110
#define QUP_MX_WRITE_CNT	0x150
#define QUP_MX_READ_CNT		0x208
#define QUP_IN_FIFO		0x218
#define QUP_I2C_CLK_CTL		0x400
#define QUP_I2C_STATUS		0x404
#define QUP_I2C_MASTER_GEN	0x408
#define GCC_BLSP1_QUP3_I2C_CMD	0x04000
#define GCC_BLSP1_QUP3_I2C_CBCR	0x04020
#define GCC_APCS_BRANCH_VOTE	0x45004		/* bit 10: blsp1 ahb */
#define FTS_ADDR		0x38

static uint32_t qup(uint32_t off) { return rd32(QUP_PHYS + off); }
static void qup_wr(uint32_t off, uint32_t v) { wr32(QUP_PHYS + off, v); }

static int qup_state(uint32_t s)
{
	for (int i = 0; i < 100; i++) {
		if (qup(QUP_STATE) & 4)		/* STATE_VALID */
			break;
		delay_ms(1);
	}
	qup_wr(QUP_STATE, s);
	for (int i = 0; i < 100; i++) {
		uint32_t v = qup(QUP_STATE);
		if ((v & 4) && (v & 3) == s)
			return 1;
		delay_ms(1);
	}
	return 0;
}

static void tlmm_cfg(unsigned gpio, unsigned func, unsigned pull, unsigned ma, int out)
{
	wr32(TLMM_BASE + gpio * 0x1000, pull | func << 2 | (ma / 2 - 1) << 6 | (out ? 1U << 9 : 0));
}

static void tlmm_set(unsigned gpio, int high)
{
	wr32(TLMM_BASE + gpio * 0x1000 + 4, high ? 2 : 0);
}

static int tlmm_get(unsigned gpio)
{
	return rd32(TLMM_BASE + gpio * 0x1000 + 4) & 1;
}

/* Write reg, then read n bytes (repeated start). Returns 0 or error bits. */
static uint32_t i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *buf, uint32_t n)
{
	uint8_t tx[12];
	uint32_t t = 0;
	tx[t++] = 0x81; tx[t++] = (uint8_t)(addr << 1);		/* START, write */
	tx[t++] = 0x82; tx[t++] = 1; tx[t++] = reg;		/* DATAWR 1 */
	tx[t++] = 0x81; tx[t++] = (uint8_t)(addr << 1 | 1);	/* START, read */
	tx[t++] = 0x87; tx[t++] = (uint8_t)n;			/* DATARD_STOP n */

	qup_wr(QUP_SW_RESET, 1);
	delay_ms(1);
	if (!qup_state(0))
		return 0x100000;
	qup_wr(QUP_CONFIG, 2 << 8 | 7);			/* I2C mini core, N 7 (v2) */
	qup_wr(QUP_IO_MODE, 3 << 14);			/* FIFO mode, pack/unpack */
	qup_wr(QUP_I2C_CLK_CTL, 3 << 8 | ((19200000 / 400000) / 2 - 3));
	qup_wr(QUP_I2C_MASTER_GEN, 1);			/* V2 tags */
	/* Linux qup_i2c_poll_state_i2c_master: wait for an idle bus first */
	for (int i = 0; i < 50 && (qup(QUP_I2C_STATUS) & (1 << 8)); i++)
		delay_ms(1);
	qup_wr(QUP_MX_WRITE_CNT, t);
	qup_wr(QUP_MX_READ_CNT, n + 2);			/* v2: 2 tag bytes precede the data */
	if (!qup_state(1))
		return 0x200000;
	for (uint32_t i = 0; i < t; i += 4) {
		uint32_t w = 0;
		for (uint32_t k = 0; k < 4 && i + k < t; k++)
			w |= (uint32_t)tx[i + k] << (8 * k);
		qup_wr(QUP_OUT_FIFO, w);
	}
	uint32_t got = 0, err = 0, skip = 2;
	uint64_t end = now() + timer_hz() / 1000 * 50;
	while (got < n && now() < end) {
		uint32_t st = qup(QUP_I2C_STATUS);
		if (st & 0x38000fcU) {			/* I2C_STATUS_ERROR_MASK (incl. NACK) */
			err = st;
			break;
		}
		if (qup(QUP_OPERATIONAL) & (1 << 5)) {	/* IN_NOT_EMPTY */
			uint32_t w = qup(QUP_IN_FIFO);
			for (int k = 0; k < 4 && got < n; k++) {
				if (skip) {
					skip--;
					continue;
				}
				buf[got++] = (uint8_t)(w >> (8 * k));
			}
		}
	}
	uint32_t ef = qup(QUP_ERROR_FLAGS);
	qup_state(0);
	if (err || got < n)
		return (err ? err : 0x400000) | (ef & 0x7c);
	return 0;
}

/*
 * Bit-banged I2C on GPIO10 (SDA) / GPIO11 (SCL), open drain by switching
 * the output enable (line low = OE with OUT 0, released = input). ~100 kHz.
 * Tells a QUP set-up problem from an electrical one.
 */
#define BB_SDA	10
#define BB_SCL	11

static void bb_line(unsigned gpio, int high)
{
	if (high) {
		tlmm_cfg(gpio, 0, 0, 2, 0);		/* release: input, external pull-up */
	} else {
		tlmm_set(gpio, 0);
		tlmm_cfg(gpio, 0, 0, 2, 1);		/* drive low */
	}
}

static void bb_wait(void)
{
	uint64_t end = now() + timer_hz() / 200000;	/* 5 us */
	while (now() < end)
		;
}

static void bb_start(void)
{
	bb_line(BB_SDA, 1); bb_line(BB_SCL, 1); bb_wait();
	bb_line(BB_SDA, 0); bb_wait();
	bb_line(BB_SCL, 0); bb_wait();
}

static void bb_stop(void)
{
	bb_line(BB_SDA, 0); bb_wait();
	bb_line(BB_SCL, 1); bb_wait();
	bb_line(BB_SDA, 1); bb_wait();
}

/* returns ACK (1) */
static int bb_write(uint8_t v)
{
	for (int i = 7; i >= 0; i--) {
		bb_line(BB_SDA, (v >> i) & 1); bb_wait();
		bb_line(BB_SCL, 1); bb_wait();
		bb_line(BB_SCL, 0);
	}
	bb_line(BB_SDA, 1); bb_wait();
	bb_line(BB_SCL, 1); bb_wait();
	int ack = !tlmm_get(BB_SDA);
	bb_line(BB_SCL, 0); bb_wait();
	return ack;
}

static uint8_t bb_read(int ack)
{
	uint8_t v = 0;
	bb_line(BB_SDA, 1);
	for (int i = 7; i >= 0; i--) {
		bb_wait();
		bb_line(BB_SCL, 1); bb_wait();
		v |= (uint8_t)(tlmm_get(BB_SDA) << i);
		bb_line(BB_SCL, 0);
	}
	bb_line(BB_SDA, ack ? 0 : 1); bb_wait();
	bb_line(BB_SCL, 1); bb_wait();
	bb_line(BB_SCL, 0);
	bb_line(BB_SDA, 1);
	return v;
}

/* 0x100 | value, or acks of address/register/read-address in the low bits */
static uint32_t bb_read_reg(uint8_t addr, uint8_t reg)
{
	bb_start();
	int a1 = bb_write((uint8_t)(addr << 1));
	int a2 = a1 && bb_write(reg);
	bb_start();
	int a3 = a2 && bb_write((uint8_t)(addr << 1 | 1));
	uint8_t v = a3 ? bb_read(0) : 0;
	bb_stop();
	return a3 ? 0x100U | v : (uint32_t)(a1 | a2 << 1 | a3 << 2);
}

/* n bytes from reg over the bit-banged bus; 0 or 1 (no ACK) */
static uint32_t bb_read_buf(uint8_t addr, uint8_t reg, uint8_t *buf, uint32_t n)
{
	bb_start();
	if (!bb_write((uint8_t)(addr << 1)) || !bb_write(reg)) {
		bb_stop();
		return 1;
	}
	bb_start();
	if (!bb_write((uint8_t)(addr << 1 | 1))) {
		bb_stop();
		return 1;
	}
	for (uint32_t i = 0; i < n; i++)
		buf[i] = bb_read(i + 1 < n);
	bb_stop();
	return 0;
}

/* a 5x5 dot at screen (x, y) */
static void dot(int x, int y, uint32_t c)
{
	for (int dy = -2; dy <= 2; dy++)
		for (int dx = -2; dx <= 2; dx++)
			if (x + dx >= 0 && x + dx < FB_W && y + dy >= 0 && y + dy < FB_H)
				put_px(x + dx, y + dy, c);
}

static void __attribute__((unused)) touch_probe(void)
{
	static const uint32_t l10[] = { RPM_KEY_UV, 3300000, RPM_KEY_SWEN, 1 };
	static const uint32_t l5[] = { RPM_KEY_UV, 1800000, RPM_KEY_SWEN, 1 };

	con_quiet = 1;
	STEP("touch rpm");
	int rpm = rpm_up();
	uint32_t a5 = rpm ? rpm_req(RPM_LDOA, 5, l5, 2) : 0;
	uint32_t a10 = rpm ? rpm_req(RPM_LDOA, 10, l10, 2) : 0;
	con_quiet = 0;
	con_hex3("TSPWR", "rpm=", rpm, "l5=", a5, "l10=", a10);

	STEP("touch pins");
	con_hex3("PINS", "g10=", rd32(TLMM_BASE + 10 * 0x1000), "g11=", rd32(TLMM_BASE + 11 * 0x1000),
		 "g64=", rd32(TLMM_BASE + 64 * 0x1000));
	tlmm_cfg(65, 0, 3, 2, 0);		/* IRQ: input, pull-up */
	tlmm_cfg(64, 0, 3, 8, 1);		/* RESET: output */
	tlmm_set(64, 0);
	delay_ms(10);
	tlmm_set(64, 1);
	delay_ms(250);				/* FT5452 boot after reset */

	/* Electrical check: lines released (GPIO input, no pull) must read 1. */
	STEP("touch line levels");
	tlmm_cfg(10, 0, 0, 2, 0);
	tlmm_cfg(11, 0, 0, 2, 0);
	delay_ms(1);
	uint32_t sda = tlmm_get(10), scl = tlmm_get(11);
	STEP("touch bitbang id");
	uint32_t bb = bb_read_reg(FTS_ADDR, 0xa3);
	con_hex3("BITBANG", "sda=", sda, "scl=", scl, "id=", bb);

	tlmm_cfg(10, 2, 0, 2, 0);		/* SDA: blsp_i2c3, no pull */
	tlmm_cfg(11, 2, 0, 2, 0);		/* SCL */

	STEP("touch qup clocks");
	wr32(GCC_BASE + GCC_APCS_BRANCH_VOTE, rd32(GCC_BASE + GCC_APCS_BRANCH_VOTE) | 1U << 10);
	wr32(GCC_BASE + GCC_BLSP1_QUP3_I2C_CMD + 4, 0);		/* CFG: XO, no divider */
	wr32(GCC_BASE + GCC_BLSP1_QUP3_I2C_CMD, rd32(GCC_BASE + GCC_BLSP1_QUP3_I2C_CMD) | 1);
	delay_ms(2);
	int qc = clk_on(GCC_BLSP1_QUP3_I2C_CBCR);
	delay_ms(250);				/* FT5452 boot after reset */
	STEP("touch qup regs");
	con_hex3("QUP", "clk=", qc, "hwver=", qup(QUP_HW_VERSION), "io=", qup(QUP_IO_MODE));

	STEP("touch read chip id");
	uint8_t id[2] = { 0, 0 };
	uint32_t e = i2c_read_reg(FTS_ADDR, 0xa3, id, 1);
	con_hex3("FTS", "err=", e, "chip=", id[0], "irq=", tlmm_get(65));
	int use_bb = 0;
	if (e) {
		if (!(bb & 0x100)) {
			con_str("TOUCH", "no answer from the touch controller");
			return;
		}
		use_bb = 1;				/* QUP fails, the chip answers: bit-bang */
		tlmm_cfg(10, 0, 0, 2, 0);
		tlmm_cfg(11, 0, 0, 2, 0);
		con_str("TOUCH", "QUP failed, using bit-banged I2C");
	}

	STEP("touch loop");
	con_str("TOUCH", "draw with a finger for 60 s");
	int last_row = row;
	uint64_t end = now() + timer_hz() * 60;
	uint32_t reports = 0, errs = 0;
	while (now() < end) {
		if (tlmm_get(65)) {			/* IRQ line high: nothing new */
			continue;
		}
		uint8_t b[63];
		if (use_bb ? bb_read_buf(FTS_ADDR, 0, b, sizeof(b)) : i2c_read_reg(FTS_ADDR, 0, b, sizeof(b))) {
			errs++;
			delay_ms(5);
			continue;
		}
		reports++;
		for (unsigned i = 0; i < 10; i++) {
			const uint8_t *p = b + 3 + 6 * i;
			unsigned ev = p[0] >> 6, tid = p[2] >> 4;
			if (tid >= 10 || ev == 1)
				continue;
			int x = (p[0] & 15) << 8 | p[1], y = (p[2] & 15) << 8 | p[3];
			dot(x, y, tid == 0 ? YELLOW : tid == 1 ? GREEN : RED);
			row = last_row;
			con_hex3("XY", "x=", (uint32_t)x, "y=", (uint32_t)y, "id=", tid);
		}
	}
	row = last_row + 1;
	con_hex3("TOUCH", "reports=", reports, "errors=", errs, 0, 0);
}

static void show_sd(void)
{
	con_quiet = 1;
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

/*
 * ---- act as iBoot for xnu (as shim/loader.c on the phone) ----
 * tools/mkpayload.py resolved the kernel's chained fixups and built the
 * ADT. LK put the payload (our ramdisk) where /chosen linux,initrd-* says:
 * copy the segments to phys_base + (vmaddr - virt_base), the ADT and
 * boot_args behind the kernel, jump to the entry with the MMU and caches
 * off, x0 = boot_args (physical).
 */
#define BOOT_LINE_LENGTH	608
#define PAGE_16K		0x4000UL

struct payload_seg {
	uint64_t vmaddr, vmsize, file_off, file_size;
};

struct payload {
	char magic[8];
	uint32_t version, nsegs;
	uint64_t virt_base, entry_va, adt_off, adt_size, kernel_end;
	uint64_t phys_base, mem_size;
	char cmdline[BOOT_LINE_LENGTH];
	struct payload_seg segs[];
};

struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

struct boot_args {
	uint16_t Revision, Version;
	uint64_t virtBase, physBase, memSize, topOfKernelData;
	struct boot_video Video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char CommandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags, memSizeActual;
};

_Static_assert(__builtin_offsetof(struct payload, segs) == 0x2a8, "payload header layout");
_Static_assert(sizeof(struct boot_args) == 736, "boot_args layout");

int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out);	/* ../../shim/fdt.c */

static void copy64(uintptr_t dst, uintptr_t src, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	volatile const uint64_t *s = (volatile const uint64_t *)src;
	for (uint64_t i = 0; i < (n + 7) / 8; i++)
		d[i] = s[i];
}

static void zero64(uintptr_t dst, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	for (uint64_t i = 0; i < n / 8; i++)
		d[i] = 0;
}

typedef void (*xnu_entry_t)(uint64_t boot_args_pa);

/* Returns only when there is nothing to boot (or it cannot be booted). */
static const char *xnu_boot(uintptr_t dtb)
{
	uint64_t rd_start, rd_end;
	STEP("initrd");
	if (!fdt_chosen_u64(dtb, "linux,initrd-start", &rd_start) ||
	    !fdt_chosen_u64(dtb, "linux,initrd-end", &rd_end))
		return "no initrd in /chosen";
	con_hex3("INITRD", "lo=", (uint32_t)rd_start, "hi=", (uint32_t)rd_end, 0, 0);

	const struct payload *p = (const struct payload *)rd_start;
	static const char want[8] = { 'S', 'W', 'E', 'E', 'T', 'X', 'N', 'U' };
	for (int i = 0; i < 8; i++)
		if (((volatile const char *)p->magic)[i] != want[i])
			return "no SWEETXNU payload";
	if (p->version != 1 || (p->adt_off & 7) || (rd_start & 7))
		return "bad payload header";

	uint64_t pa_of_va_base = p->phys_base - p->virt_base;
	con_hex3("PHYS", "base=", (uint32_t)p->phys_base, "size=", (uint32_t)p->mem_size,
		 "segs=", p->nsegs);
	if (rd_end > p->phys_base && rd_start < p->phys_base + p->mem_size)
		return "ramdisk overlaps kernel memory";

	STEP("copy segments");
	for (uint32_t i = 0; i < p->nsegs; i++) {
		const struct payload_seg *s = &p->segs[i];
		uintptr_t dst = s->vmaddr + pa_of_va_base;
		copy64(dst, rd_start + s->file_off, s->file_size);
		if (s->vmsize > s->file_size)
			zero64(dst + ((s->file_size + 7) & ~7UL), s->vmsize - ((s->file_size + 7) & ~7UL));
	}

	STEP("adt + boot_args");
	uintptr_t adt_pa = p->kernel_end + pa_of_va_base;
	copy64(adt_pa, rd_start + p->adt_off, p->adt_size);
	uintptr_t ba_pa = (adt_pa + p->adt_size + PAGE_16K - 1) & ~(PAGE_16K - 1);
	uintptr_t top = ba_pa + PAGE_16K;
	zero64(ba_pa, PAGE_16K);
	volatile struct boot_args *ba = (volatile struct boot_args *)ba_pa;
	ba->Revision = 2;
	ba->Version = 2;
	ba->virtBase = p->virt_base;
	ba->physBase = p->phys_base;
	ba->memSize = p->mem_size;
	ba->topOfKernelData = top;
	/*
	 * The splash is 24 bpp and scanned out shifted (see put_px); xnu's own
	 * video console only knows 32 bpp, so it stays off. The kernel draws
	 * through the ADT's sweet-fbcon (bpp 3, shift 229) instead.
	 */
	ba->Video.v_baseAddr = SPLASH_PHYS;
	ba->Video.v_display = 0;
	ba->Video.v_rowBytes = FB_W * 3;
	ba->Video.v_width = FB_W;
	ba->Video.v_height = FB_H;
	ba->Video.v_depth = 24;
	ba->deviceTreeP = adt_pa - pa_of_va_base;	/* xnu reads it with the MMU on: a VA */
	ba->deviceTreeLength = (uint32_t)p->adt_size;
	for (int i = 0; i < BOOT_LINE_LENGTH; i++)
		ba->CommandLine[i] = p->cmdline[i];
#if defined(T290_GPU2)
	if (gpu_ready) {
		/* the GPU runs our ring: tell bsd/dev/sweet_t290_gpu.c it may touch it */
		static const char arg[] = " t290gpu=1";
		int n = 0;
		while (n < BOOT_LINE_LENGTH && ba->CommandLine[n])
			n++;
		if (n + (int)sizeof(arg) <= BOOT_LINE_LENGTH)
			for (int i = 0; i < (int)sizeof(arg); i++)
				ba->CommandLine[n + i] = arg[i];
	}
#endif
	ba->memSizeActual = p->mem_size;
	con_hex3("BOOTARG", "pa=", (uint32_t)ba_pa, "top=", (uint32_t)top, "adt=", (uint32_t)adt_pa);

	uintptr_t entry_pa = p->entry_va + pa_of_va_base;
	con_hex3("JUMP", "pa=", (uint32_t)entry_pa, 0, 0, 0, 0);
	con_str("XNU", "jumping");
	STEP("jump to xnu");
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
	((xnu_entry_t)entry_pa)(ba_pa);
	return "xnu returned";
}

/*
 * ---- boot menu: big buttons driven by the touchscreen, like TWRP ----
 * iPadOS boots the xnu payload, Android warm-resets into LK with a normal
 * restart reason, the diagnostics run the GPU probe, and the log viewer
 * pages through this run's pstore log. Touch: FT5452 over bit-banged I2C
 * (QUP3 bus-errors, see touch_probe), polled.
 */
#define PSCI_SYSTEM_RESET	0x84000009UL
#define IMEM_RESTART_REASON	0x0860065cUL	/* qcom,msm-imem-restart_reason */
#define RESTART_NORMAL		0x77665501U
#define PSHOLD_BASE		0x004ab000UL	/* qcom,pshold */

#define DARK	0x203040U
#define GREY	0x505a68U
#define IOSBLUE	0x1060d0U

struct button {
	int x, y, w, h;
	const char *label;
	uint32_t color;
};

static int ts_ok;

static int touch_init(void)
{
	static const uint32_t l10[] = { RPM_KEY_UV, 3300000, RPM_KEY_SWEN, 1 };
	static const uint32_t l5[] = { RPM_KEY_UV, 1800000, RPM_KEY_SWEN, 1 };

	STEP("menu touch power");
	int q = con_quiet;
	con_quiet = 1;
	if (rpm_up()) {
		rpm_req(RPM_LDOA, 5, l5, 2);
		rpm_req(RPM_LDOA, 10, l10, 2);
	}
	con_quiet = q;
	STEP("menu touch reset");
	tlmm_cfg(65, 0, 3, 2, 0);		/* IRQ: input, pull-up */
	tlmm_cfg(64, 0, 3, 8, 1);		/* RESET: output */
	tlmm_set(64, 0);
	delay_ms(10);
	tlmm_set(64, 1);
	delay_ms(250);
	tlmm_cfg(10, 0, 0, 2, 0);		/* bit-banged SDA/SCL: GPIO, no pull */
	tlmm_cfg(11, 0, 0, 2, 0);
	delay_ms(1);
	STEP("menu touch id");
	return (bb_read_reg(FTS_ADDR, 0xa3) & 0x100) != 0;
}

/* 1: finger down at (x, y), 0: no finger, -1: read failed */
static int touch_get(int *x, int *y)
{
	uint8_t b[9];
	if (bb_read_buf(FTS_ADDR, 0, b, sizeof(b)))
		return -1;
	if ((b[2] & 15) == 0 || (b[3] >> 6) == 1)	/* no points or lift-up event */
		return 0;
	*x = (b[3] & 15) << 8 | b[4];
	*y = (b[5] & 15) << 8 | b[6];
	return 1;
}

static void fill_rect(int x, int y, int w, int h, uint32_t c)
{
	for (int j = y; j < y + h; j++)
		for (int i = x; i < x + w; i++)
			put_px(i, j, c);
}

static void text_at(int x, int y, const char *s, uint32_t fg, uint32_t back, int scale)
{
	int old = cscale;
	uint32_t ob = bg;
	cscale = scale;
	bg = back;
	for (; *s; s++, x += CH_W)
		draw_glyph(x, y, *s, fg);
	cscale = old;
	bg = ob;
}

static int str_len(const char *s)
{
	int n = 0;
	while (s[n])
		n++;
	return n;
}

static void draw_button(const struct button *b, int pressed)
{
	uint32_t c = pressed ? WHITE : b->color, t = pressed ? b->color : WHITE;
	fill_rect(b->x, b->y, b->w, b->h, c);
	int len = str_len(b->label), sc = b->h >= 120 ? 3 : 2;
	text_at(b->x + (b->w - len * 8 * sc) / 2, b->y + (b->h - 16 * sc) / 2, b->label, t, c, sc);
}

/* filled in code: a static table of string pointers needs ABS64 relocations */
static void set_btn(struct button *b, int x, int y, int w, int h, const char *label, uint32_t color)
{
	b->x = x;
	b->y = y;
	b->w = w;
	b->h = h;
	b->label = label;
	b->color = color;
}

static int hit(const struct button *b, int x, int y)
{
	return x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h;
}

/* Wait for a tap (press and release on the same button); returns its index. */
static int wait_tap(const struct button *bs, int n)
{
	int down = -1;
	for (;;) {
		int x = 0, y = 0, t = touch_get(&x, &y);
		if (t == 1 && down < 0) {
			for (int i = 0; i < n; i++)
				if (hit(&bs[i], x, y)) {
					down = i;
					draw_button(&bs[i], 1);
				}
		} else if (t == 0 && down >= 0) {
			draw_button(&bs[down], 0);
			return down;
		}
		delay_ms(15);
	}
}

static void log_viewer(void)
{
	struct button nav[3];
	set_btn(&nav[0], 16, 1150, 248, 114, "Up", GREY);
	set_btn(&nav[1], 276, 1150, 248, 114, "Down", GREY);
	set_btn(&nav[2], 536, 1150, 248, 114, "Back", IOSBLUE);
	const volatile char *log = (const volatile char *)(PLOG_BASE + 12);
	const int rows = 70, cols = 96;			/* 8x16 font above the buttons */
	int first = 0;
	con_clear(DARK);
	for (int i = 0; i < 3; i++)
		draw_button(&nav[i], 0);
	for (;;) {
		/* count lines, find the start of line `first` */
		int lines = 1, start = 0, l = 0;
		for (uint32_t i = 0; i < plog_len; i++)
			if (log[i] == '\n' && i + 1 < plog_len) {
				lines++;
				if (++l == first)
					start = (int)i + 1;
			}
		if (first == 0)
			start = 0;
		fill_rect(0, 0, FB_W, 1140, DARK);
		uint32_t p = (uint32_t)start;
		for (int r = 0; r < rows && p < plog_len; r++) {
			char buf[97];
			int c = 0;
			while (p < plog_len && log[p] != '\n') {
				if (c < cols)
					buf[c++] = log[p];
				p++;
			}
			p++;
			buf[c] = 0;
			uint32_t fg = buf[0] == '>' ? GREEN : buf[0] == 'R' && buf[1] == ' ' ? GREY : WHITE;
			text_at(8, 8 + r * 16, buf, fg, DARK, 1);
		}
		int k = wait_tap(nav, 3);
		if (k == 2)
			return;
		first += k == 0 ? -(rows - 4) : rows - 4;
		if (first > lines - rows)
			first = lines - rows;
		if (first < 0)
			first = 0;
	}
}

static void boot_android(void)
{
	con_clear(DARK);
	text_at(40, 600, "Rebooting to Android...", WHITE, DARK, 3);
	STEP("menu android reset");
	wr32(IMEM_RESTART_REASON, RESTART_NORMAL);
	__asm__ volatile("dsb sy" ::: "memory");
	delay_ms(300);
	smc(PSCI_SYSTEM_RESET, 0, 0, 0);
	/*
	 * First menu build: the button did not reboot (TZ ignores PSCI
	 * SYSTEM_RESET?). Linux msm-poweroff resets by dropping PS_HOLD
	 * (qcom,pshold 0x4ab000): the PMIC then resets the SoC.
	 */
	STEP("menu android ps_hold");
	wr32(PSHOLD_BASE, 0);
	__asm__ volatile("dsb sy" ::: "memory");
	delay_ms(2000);
	text_at(40, 700, "reset failed", RED, DARK, 3);
}

/*
 * CPU clock as LK left it: 64 dependent adds per loop iteration (1 cycle
 * each on the A53) timed against the 19.2 MHz counter, with the I-cache on
 * so the loop does not run from uncached DRAM.
 */
static uint32_t cpu_mhz(void)
{
	uint64_t sctlr = rsr("sctlr_el1");
	__asm__ volatile("msr sctlr_el1, %0; isb" :: "r"(sctlr | 1U << 12));
	uint64_t iters = 200000, x = 0;
	uint64_t t0 = now();
	for (uint64_t i = 0; i < iters; i++)
		__asm__ volatile(
			".rept 64\n\tadd %0, %0, #1\n\t.endr" : "+r"(x));
	uint64_t t1 = now();
	__asm__ volatile("msr sctlr_el1, %0; isb" :: "r"(sctlr));
	return (uint32_t)(iters * 64 * timer_hz() / (t1 - t0) / 1000000);
}

/*
 * iPadOS draws on the CPU: memory bandwidth matters. Android's drivers vote
 * the DDR bus (BIMC) and the system NoC up through the RPM; nobody does
 * that for xnu, so vote them before jumping (the active-set vote stays).
 * Linux clk-smd-rpm msm8937: bimc = MEM_CLK "clk2" id 0, snoc = BUS_CLK
 * "clk1" id 1; rates in KHz, the RPM rounds to a supported level.
 */
static void perf_votes(void)
{
	static const uint32_t bimc[] = { 0x007a484b /* "KHz" */, 800000 };
	static const uint32_t snoc[] = { 0x007a484b, 400000 };
	STEP("perf votes");
	int q = con_quiet;
	con_quiet = 1;
	int rpm = rpm_up();
	uint32_t b = rpm ? rpm_req(0x326b6c63 /* "clk2" */, 0, bimc, 1) : 0;
	uint32_t s = rpm ? rpm_req(0x316b6c63 /* "clk1" */, 1, snoc, 1) : 0;
	con_quiet = q;
	con_hex3("PERF", "rpm=", (uint32_t)rpm, "bimc=", b, "snoc=", s);
}

/*
 * Unattended probes: Android (root) writes "T290CMD1", a nonce and a command
 * into one microSD sector 1 MiB past the 16 GiB HFSX root volume, then
 * "adb reboot recovery". A nonce not seen before runs the command, which
 * ends by rebooting to Android; the last nonce lives next to the run counter
 * (DDR, kept across warm resets), so entering recovery by hand afterwards
 * shows the menu.
 */
#define CMD_LBA		33556480u
#define CMD_NONCE_AT	(RUNS_AT + 8)

static __attribute__((unused)) int auto_command(void)
{
	con_hex3("BOOT", "reason=", rd32(IMEM_RESTART_REASON), "nonce=", rd32(CMD_NONCE_AT), 0, 0);
	/*
	 * The SDCC2 AHB clock first: sd_probe's first register write with it
	 * off reset the SoC, and Samsung's LK then booted recovery again, five
	 * times in a row (2026-10-21). show_sd() did this; auto_command forgot.
	 */
	STEP("auto sdcc2 ahb");
	if (!clk_on(GCC_SDCC2_AHB))
		return 0;
	STEP("auto sd probe");
	int q = con_quiet;
	con_quiet = 1;
	sd_probe();
	con_quiet = q;
	if (!sd_ok || sd_read_block(CMD_LBA))
		return 0;
	const char *c = (const char *)sd_sec;
	static const char magic[8] = { 'T', '2', '9', '0', 'C', 'M', 'D', '1' };
	for (int i = 0; i < 8; i++)
		if (c[i] != magic[i])
			return 0;
	uint32_t nonce = sd_sec[2];
	con_hex3("AUTOCMD", "nonce=", nonce, "cmd=", sd_sec[3], 0, 0);
	if (nonce == rd32(CMD_NONCE_AT))
		return 0;
	wr32(CMD_NONCE_AT, nonce);
	int gpu = c[12] == 'g' && c[13] == 'p' && c[14] == 'u';
	/* one-shot: wipe the command before running it (no loop even if LK comes back here) */
	for (int i = 0; i < 128; i++)
		sd_sec[i] = 0;
	uint32_t we = sd_write_block(CMD_LBA);
	con_hex3("AUTOCMD", "wipe=", we, "gpu=", (uint32_t)gpu, 0, 0);
	if (we)
		return 0;			/* could not wipe: do not risk a loop */
	/* a crash during the command resets into Android, not back here */
	wr32(IMEM_RESTART_REASON, RESTART_NORMAL);
	return gpu;
}

static void boot_menu(uint64_t dtb, uint32_t runs)
{
	struct button menu[4], again[2];
	set_btn(&menu[0], 50, 260, 700, 180, "iPadOS 16", IOSBLUE);
	set_btn(&menu[1], 50, 480, 700, 180, "Android", GREEN);
	set_btn(&menu[2], 50, 700, 700, 150, "GPU diagnostics", GREY);
	set_btn(&menu[3], 50, 890, 700, 150, "Boot log", GREY);
	set_btn(&again[0], 16, 1150, 380, 114, "Log", GREY);
	set_btn(&again[1], 404, 1150, 380, 114, "Back", IOSBLUE);
	STEP("menu");
#if defined(T290_AUTO)
	if (auto_command()) {
		con_clear(DARK);
		con_str("AUTO", "GPU diagnostics, then Android");
		gpu_probe();
		boot_android();
	}
#endif
	ts_ok = touch_init();
	for (;;) {
		con_clear(DARK);
		text_at(50, 80, "SM-T290", WHITE, DARK, 4);
		text_at(50, 160, "Choose a system", YELLOW, DARK, 2);
		for (int i = 0; i < 4; i++)
			draw_button(&menu[i], 0);
		if (!ts_ok) {
			/* no touch: the menu cannot be used, keep the old behaviour */
			text_at(50, 1100, "touch not found: iPadOS in 5 s", RED, DARK, 2);
			delay_ms(5000);
			break;
		}
		char run[] = "run 0000";
		fmt_hex(run + 4, runs, 4);
		text_at(50, 1200, run, GREY, DARK, 2);
		char mhz[] = "CPU 0000 MHz";
		uint32_t m = cpu_mhz();
		for (int i = 3; i >= 0; i--, m /= 10)
			mhz[4 + i] = (char)('0' + m % 10);
		text_at(330, 1200, mhz, GREY, DARK, 2);
		plog_str(mhz);
		plog_putc('\n');
		int k = wait_tap(menu, 4);
		if (k == 0)
			break;
		if (k == 1) {
			boot_android();
			continue;
		}
		if (k == 2) {
			con_clear(DARK);
			con_str("T290", "GPU diagnostics");
			gpu_probe();
			for (int i = 0; i < 2; i++)
				draw_button(&again[i], 0);
			if (wait_tap(again, 2) == 0)
				log_viewer();
			continue;
		}
		log_viewer();
	}
	con_clear(DARK);
	con_str("T290", "booting iPadOS 16");
#if defined(T290_GPU2)
	gpu_boot = 1;
	gpu_probe();
	con_hex3("GPUBOOT", "ready=", (uint32_t)gpu_ready, 0, 0, 0, 0);
#endif
	perf_votes();
	(void)dtb;
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
#if !defined(T290_TOUCH_PROBE) && !defined(T290_GPU_PROBE)
	boot_menu(dtb, runs);
#else
	draw_ruler();
#endif
	con_str("T290", "milestone 5: xnu loader / diagnostics");
	con_hex3("RUN", "n=", runs, "dtb=", (uint32_t)dtb, 0, 0);
	/* An xnu payload in the ramdisk means: boot it. */
#if defined(T290_TOUCH_PROBE)
	touch_probe();
	con_str("DONE", "touch1 probe: take a photo");
	STEP("done");
	hold();
#endif
#if defined(T290_GPU_PROBE)
	gpu_probe();
	con_str("DONE", "gpu probe: take a photo");
	STEP("done");
	hold();
#endif
	const char *why = xnu_boot(dtb);
	con_str("LOADER", why);
	STEP("regs");
	con_hex3("WDT", "en_was=", wdt_was, "en_now=", rd32(WDT_BASE + WDT_EN), 0, 0);
	show_sd();
	STEP("psci");
	con_hex3("PSCI", "ver=", (uint32_t)smc(PSCI_VERSION, 0, 0, 0), 0, 0, 0, 0);
	con_str("DONE", "take a photo of this screen");
	STEP("done");
	hold();
}
