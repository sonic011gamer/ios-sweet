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

## Open questions
1. SMMU: bypass or context bank programming from EL1 (Linux kgsl does it, so
   the CB registers are reachable from the apps EE).
2. GX rail voltage: RPM request (like the SD card LDOs) or already on.
3. Register access safety: MDP reads reset the SoC (diag milestone 1) — do
   the same probe-with-pstore-log dance for 0x1c00000 (RBBM_CHIP_ID etc.)
   only after the GDSC and clocks are on.
