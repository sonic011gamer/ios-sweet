# Adreno 504 on the SM-T290 (GPU bring-up notes)

Goal (long term): GPU acceleration for the iPadOS GUI. Steps, each checkable:
1. Recon from Android (no risk). 2. "Hello, GPU" in t290/shim: power the GPU,
load the CP microcode, run one command stream (e.g. a solid fill), show the
result on the splash. 3. XNU driver (memory, rings, IRQ), IOAccelerator in
IOKit. 4. GPU blits for AppleCLCD composition (layers, scaling, format).
5. Metal driver + shader compiler (AIR -> NIR -> ir3, Mesa Freedreno).

## Live FDT (2026-10-20, logs/twrp-20261020/fdt.dts)
- `qcom,kgsl-3d0@1c00000`: chipid **0x05000400 = Adreno 504** (a5xx family).
  reg 0x1c00000 + 0x40000 (kgsl_3d0_reg_memory), qfprom 0xa0000 + 0x6fff
  (speed bin: qcom,gpu-speed-bin-vectors <0x6004 0xc00000 22>, <0x6008 0x600 7>).
  IRQ SPI 33 (0x21). highest-bank-bit 14.
- Clocks (GCC): core_clk, iface_clk, mem_iface_clk, alt_mem_iface_clk,
  rbbmtimer_clk, alwayson_clk (names only; GCC offsets from Linux
  gcc-msm8917/8937: gfx3d RCG, gcc_oxili_gfx3d/ahb/gmem/timer, gcc_bimc_gfx).
- Power: `gdsc_oxili_gx` GDSCR @ **0x185901c** (vdd-supply), `gdsc_oxili_cx`
  @ **0x1859044**. Rails: vddcx; GX rail on the RPM? (check: smpa / cpr).
- Frequencies (speed bin 0): 650, 560, 510, 400, 320, 19.2 MHz (initial lvl 3 = 400).
- **No zap shader / no qcom,firmware-name for the GPU**: a504 needs no secure
  (TZ) init, unlike A530/A540. Microcode (kgsl adreno_gpulist for A504):
  a530_pm4.fw + a530_pfp.fw expected in /vendor/firmware — to pull.
- IOMMU: `arm,smmu-kgsl@1c40000` qcom,smmu-v2, 0x10000, `qcom,tz-device-id = "GPU"`,
  skip-init, enable-static-cb, 3-level tables, IRQs SPI 225/232/233/234.
  TZ owns the global config; Linux drives context banks (user + secure).
  **Open question #1:** can the GPU reach physical memory without us
  programming a context bank (bypass), and may EL1 write the CB registers?
- Bus: msm-bus "grp3d" votes (bimc), up to 0x71ac00 KBps.

## GCC (Linux drivers/clk/qcom/gcc-msm8917.c, same block on msm8937/SDM429)
GCC base 0x1800000; offsets:
- `gfx3d_clk_src` RCG CMD 0x59000 / CFG 0x59004, parents XO 0, GPLL0 1, GPLL3 2,
  GPLL6 3 (GPLL0 800 MHz: /4 = 200 MHz, /2 = 400 MHz; HID div field = 2*div-1).
- branches: `gcc_oxili_gfx3d_clk` 0x59020, `gcc_oxili_ahb_clk` 0x59028,
  `gcc_bimc_gfx_clk` 0x59034 (CBCR bit0 enable, bit31 CLK_OFF).
- `oxili_gx_gdsc` GDSCR 0x5901c (bit0 SW_COLLAPSE, bit31 PWR_ON),
  clamp_io_ctrl 0x5b00c (unclamp after power-on), CXCs 0x59000/0x59020.

## a5xx bring-up (Linux drivers/gpu/drm/msm/adreno/a5xx_gpu.c, a5xx_hw_init)
Register numbers are dword indices (byte offset = 4 * n) into 0x1c00000.
Upstream Linux has NO entry for chip 0x05000400 (A504); A506 = a530_pm4/pfp
+ a506_zap.mdt. kgsl (Qualcomm) supports A504.
1. VBIF_ROUND_ROBIN_QOS_ARB (0x3049) = 3, RBBM_AHB_CNTL0 (0x93) = 1, UCHE/CP
   thresholds, CP_PROTECT_*, *_ADDR_MODE_CNTL = 1 (64-bit addressing).
2. CP_ME_INSTR_BASE (0x838) / CP_PFP_INSTR_BASE (0x835) = GPU addresses of
   the PM4 / PFP microcode (loaded from memory by the CP itself).
3. CP_RB_BASE (0x800), CP_RB_CNTL (0x802), CP_RB_RPTR_ADDR (0x804),
   CP_RB_WPTR (0x807); CP_PFP_ME_CNTL (0x832) = 0 starts the CP.
4. ring: CP_ME_INIT (opcode 72) with 8 dwords.
5. Secure mode: the GPU starts secure. With a zap shader: CP_SET_SECURE_MODE 0
   from the ring. Without: write RBBM_SECVID_TRUST_CNTL (0xf400) = 0 directly;
   a wrong guess = access violation.
6. Status: RBBM_STATUS 0x4f5, RBBM_INT_0_STATUS 0x4e1, CP_HW_FAULT 0xb1a,
   CP_RB_RPTR 0x806.

## Shim milestones (each with the pstore step log)
- **gpu1 (probe):** GDSC on + unclamp, gfx3d 200 MHz, branches on; log, then
  read RBBM_STATUS / INT_0_STATUS / SECVID_TRUST_CNTL / CP_HW_FAULT.
  Success = values instead of a reset.
- **gpu2 (hello):** microcode from /vendor/firmware into a buffer, ring with
  CP_ME_INIT then CP_MEM_WRITE (opcode 61) of a colour into the splash
  (0x90000000 + offset): a square drawn by the GPU's command processor.
  Needs answers to the SMMU and secure-mode questions first.

## Open questions
1. SMMU: bypass or context bank programming from EL1 (Linux kgsl does it, so
   the CB registers are reachable from the apps EE).
2. GX rail voltage: RPM request (like the SD card LDOs) or already on.
3. Register access safety: MDP reads reset the SoC (diag milestone 1) — do
   the same probe-with-pstore-log dance for 0x1c00000 (RBBM_CHIP_ID etc.)
   only after the GDSC and clocks are on.
