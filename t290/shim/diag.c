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

static void put_px(int x, int y, uint32_t rgb)
{
	uintptr_t p = fb + (uintptr_t)y * fb_stride + (uintptr_t)x * fb_bpp;
	volatile uint8_t *b = (volatile uint8_t *)p;
	/* MDP RGB888 / ARGB8888 in memory: B, G, R (, A) on Qualcomm splash */
	b[0] = rgb & 0xff;
	b[1] = (rgb >> 8) & 0xff;
	b[2] = (rgb >> 16) & 0xff;
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

static void put_char(int col, char ch, uint32_t fg)
{
	if (ch < 0x20 || ch > 0x7e)
		ch = '?';
	const unsigned char *g = font8x16[ch - 0x20];
	int x0 = MARGIN + col * CH_W, y0 = MARGIN + row * CH_H;
	if (x0 + CH_W > FB_W || y0 + CH_H > FB_H)
		return;
	for (int gy = 0; gy < 16; gy++)
		for (int gx = 0; gx < 8; gx++) {
			uint32_t c = (g[gy] & (0x80 >> gx)) ? fg : bg;
			for (int sy = 0; sy < SCALE; sy++)
				for (int sx = 0; sx < SCALE; sx++)
					put_px(x0 + gx * SCALE + sx, y0 + gy * SCALE + sy, c);
		}
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

static void show_dtb(uintptr_t dtb)
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

static void show_splash_format(void)
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
	con_hex3("CORE2", "pwrst=", rd32(SDC2_CORE + 0xdc), 0, 0, 0, 0);
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
	con_str("T290", "ios-t290 milestone 1: diagnostics");
	con_hex3("RUN", "n=", runs, "dtb=", (uint32_t)dtb, 0, 0);
	row++;
	STEP("regs");
	con_hex3("CPU", "el=", (uint32_t)(rsr("CurrentEL") >> 2), "mpidr=", (uint32_t)rsr("mpidr_el1"),
		 "midr=", (uint32_t)rsr("midr_el1"));
	con_hex3("WDT", "en_was=", wdt_was, "en_now=", rd32(WDT_BASE + WDT_EN), "sts=", rd32(WDT_BASE + WDT_STS));
	con_hex3("TIMER", "frq=", (uint32_t)rsr("cntfrq_el0"), "sctlr=", (uint32_t)rsr("sctlr_el1"), 0, 0);
	STEP("dtb");
	show_dtb(dtb);
	row++;
	STEP("fb format");
	show_splash_format();
	row++;
	show_sd();
	row++;
	STEP("psci");
	con_hex3("PSCI", "ver=", (uint32_t)smc(PSCI_VERSION, 0, 0, 0), 0, 0, 0, 0);
	row++;
	con_str("DONE", "take a photo of this screen");
	STEP("done");
	hold();
}
