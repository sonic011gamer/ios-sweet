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

## Through Android's kgsl (2026-10-20 evening, safe: kgsl owns power/SMMU/zap)
Firmware on the tablet (copied to `D:\test1\ios-t290\fw`, not in git):
a530_pm4.fw, a530_pfp.fw and **a506_zap.mdt/.b00-.b02** (so bare-metal
bring-up will need the zap shader through TZ, as kgsl does). kgsl: Adreno504v1,
gpuclk 320 MHz, /dev/kgsl-3d0 world-rw. uapi header: research/kgsl/msm_kgsl.h
(LineageOS msm8953 lineage-20; build user code with -D__user=).
- `t290/gpu/kgsl_hello.c`: hand-built type-7 CP_MEM_WRITE via
  IOCTL_KGSL_GPU_COMMAND -> **"target = 0xc0ffee01 -> THE GPU WROTE IT"**.
  chip_id 0x05000400, gmem 0x22000, GPU VAs 0x5_0000_0000.
- `t290/gpu/kgsl_blit.c copy`: 2D engine (fd5_blitter sequence: sysmem prep
  CP_SET_RENDER_MODE BYPASS + RB_CCU_CNTL 0x10000000 + PC_CCU_INVALIDATE_COLOR,
  emit_setup, BLIT2D, RB_2D_SRC/DST, CP_BLIT COPY, END2D, CCU/CACHE flush TS):
  **768x1024 RGBA copied in 2.7 ms incl. submit, 0 wrong pixels**; the CPU
  halving of the same buffer (WC memory) takes 91 ms.
  Without the BYPASS prologue the blit wrote nothing.
- BLIT_OP_SCALE: no output. Mesa's a5xx blitter refuses scaling too ("until we
  figure out a few more registers"). Format trick (R32G32 src -> R32 dst, double
  pitch) does not drop channels: both pixels land, in a blocked order
  (dst row0 = src 16,17 @y0, 16,17 @y2, ...). Dead end for now.
- The panel's byte order R,B,G is not reachable with the 2D engine's four
  colour swaps (they form the dihedral group of the 4-cycle); the final
  shuffle + 229 px shift needs a shader (or the CPU).
### Compute shaders (2026-10-20 night)
- **Assembler:** Mesa 24.2.8 (archive.mesa3d.org, D:\test1\dl\mesa) built in
  WSL with meson 1.5.2 + mako wheels fetched through Windows (WSL has no
  network); `t290/gpu/ir3asm` = Mesa's ir3 assembler without a DRM device
  (gpu_id 506 = a5xx ISA). Build: `t290/gpu/ir3asm/build-ir3asm.sh ~/mesa-src/mesa-24.2.8`.
  Gotchas: ALU immediates are 11-bit signed (3072 encodes as -1024); cat3 (mad)
  sources cannot be constants (c1.x silently became r1.x); stg.u8 stores from
  half registers (cov.u32u16 first); release build skips the "wgid >= r48" assert.
- **State:** `t290/gpu/kgsl_compute.c` = Mesa fd5_compute.c (cs_program_emit,
  fd5_launch_grid) + compute-relevant fd5_emit_restore over kgsl:
  SP_CS_CTRL_REG0, HLSQ_CS_CONFIG/CNTL/CONSTLEN, SP_CS_OBJ_START, CP_LOAD_STATE4
  (shader indirect + constants direct), HLSQ_CS_NDRANGE, CP_EXEC_CS.
  `shaders/store1.asm` -> buffer[0] = 0xc0de. **First shader on the A504.**
- **Present shader:** `shaders/present1.asm` (one invocation per group, grid
  768x1024): halve + R,B,G + 229 px shift, **0 of 3072000 bytes differ** from the
  CPU reference (kgsl_present). But 243 ms: the local invocation id never
  arrives - in 2/4/8/16/64-wide groups only lane 0 writes (r0.x = 0 for all
  or other lanes idle). Tried KERNELDIM 1/2/3, SP_CS_CTRL_REG0 VARYING,
  FOUR_QUADS, r0.x in every HLSQ_CS_CNTL_0 field: no change. WGIDCONSTID = r10.x
  does deliver the work-group id. Unsolved.
- CPU reference of the same present from cached memory: ~5.5 ms (WC memory:
  91 ms). So the present path is not the bottleneck worth a GPU; CoreAnimation's
  software rendering is. GPU work should target composition/rendering.

**Next ideas:** (1) local id: compile a real NIR compute shader with Mesa's ir3
for a5xx (load_local_invocation_id) and diff the variant/sysval setup against
ours; capture the blob's command stream (kgsl, e.g. a GLES compute dispatch
from an Android app) and compare HLSQ/SP CS registers. (2) 2D scaling: the
"few more registers" Mesa mentions - capture a scaled blit from the blob
(C2D / GLES glBlitFramebuffer). (3) Bare-metal: zap shader via TZ (a506_zap).

## GPU rendering of the UI: how CoreAnimation picks a renderer (2026-10-21)
Static analysis of the iPadOS 16.7.16 SystemOS cryptex (087-86624-047.dmg,
apfs-fuse; dyld cache tools t290/src/dsc_*.py, no ipsw tool needed):
- `CA::WindowServer::AppleDisplay::new_server()` always builds an
  `EAGLServer(IOMFBDisplay*, name)` for the main display (our AppleCLCD).
- `EAGLServer::renderer()` (0x18826af3c): Metal device -> MetalContext;
  **no Metal and CA_ENABLE_OGL (default 1, read once in the EAGLServer ctor)
  -> `[[EAGLContext alloc] initWithAPI:3]`, then API 2, -> GLES renderer**;
  only if no EAGLContext -> software (today's path).
- Renderer table in __DATA_CONST 0x1d57e7c68: gles_new_context,
  metal_new_context, sw_new_context; GLES context = CA::OGL::GLESContext.
- OpenGLES.framework on iOS 16 renders through AppleMetalGLRenderer (GL on
  Metal), so EAGLContext fails without a Metal device.
**Plan:** a working EAGLContext + GLES 2/3 in backboardd is enough for CA to
render the UI on the GPU. Pieces: (1) kernel: IOKit user client for GPU
memory (alloc/map), command submission and fences (kgsl-like) on top of
sweet_t290_gpu.c; (2) Mesa Freedreno (gallium a5xx, GLES 3.1) built for
iOS arm64 with a winsys over (1); (3) a dylib injected into backboardd that
interposes the OpenGLES C API and EAGLContext (initWithAPI, setCurrentContext,
renderbufferStorage:fromDrawable:, IOSurface textures) with Mesa's.
Next research: which OpenGLES/EAGL symbols QuartzCore imports (the surface
to interpose).

## SOLVED 2026-10-21 (gpu21): the GPU runs commands on bare metal
**The green square was drawn by the CP** (rptr = wptr = 0x1e81, splash pixel
00ff0000, RBBM_STATUS 1, no SMMU fault). The missing piece were two GPU clocks
kgsl enables and we did not: **GCC 0x59040 (rbbmtimer, oxili timer CBCR)** and
**GCC 0x5904c (alwayson CBCR)**, both read 0x80000000 (CLK_OFF). Without them
the CP microcode waits forever at start (CPSTAT pfp0 0x100 / me0 0x80; after:
0x102 / 0xfe). 0x59044 reads like a GDSC (a0222800, the "cx" gdsc_on of gpu1).
Working sequence (t290/shim/diag.c gpu_probe + gpu_hello): GDSCs, gfx3d 200 MHz,
ahb/bimc_gfx/gfx3d/bimc_gpu/timer/alwayson clocks, microcode at INSTR_BASE,
a5xx_hw_init values, SMMU CB0 AArch32 LPAE identity (TTBCR EAE, 1 GiB blocks,
MAIR0 0x44, SH 0), SECVID 0 before CP start, CP_PFP_ME_CNTL 0, CP_ME_INIT,
then any PM4. Not needed after all (kept for now): RPM bus votes, SCM
restore_sec_cfg (MP 0xC: ret 0), BIMC port 2 (same config as port 0).

## gpu2 bare metal (2026-10-21, t290/shim -DT290_GPU_PROBE -DT290_GPU2, FW_DIR)
Microcode (fw.S .incbin), a5xx_hw_init values, ring 32 KiB, CP_ME_INIT, SECVID 0,
CP_MEM_WRITE square. Logs: ios-t290/logs/gpu*-pstore.txt. Result so far: the CP
never takes a ring dword (rptr 0), RBBM_STATUS c00003c1, no interrupt/fault.
- Ruled out: bimc_gpu clock (0x59030, was off: now on), GFX TBU/TCU votes,
  micro-MMU halt (SMMU+0x6000 = 0x378, not halted), VBIF XIN halt, buffer
  address (0xb0000000 vs splash reservation 0x90800000), D-cache (SCTLR_EL1 =
  0x00c00800), RPM bus votes (clk2 id 2, bmas 6, bslv 0: acked), SECVID before
  CP start, a50x hwcg. RBBM_SW_RESET_CMD resets the whole SoC: never write it.
- SCM restore_sec_cfg: my call used service 2 (PIL) instead of 0xC (MP): -5.
  Retry with fn 0x42000C02, args (18 = TZ_DEVICE_GPU, 0).
- SMMU (0x1c40000): sCR0 0x00200436 (client on, USFCFG), SMR0 id 0 mask 1 ->
  S2CR0 translate CB0, CBAR0 0x1b003. CB0 SCTLR 0 (bypass) = silent hang.
  **The CB format is AArch32 (TZ):** an LPAE table (TTBCR EAE, T0SZ 0, 1 GiB
  identity blocks at L1, MAIR0 0x44) translates without fault (fsr 0x200);
  CBA2R VA64 / AArch64 TCR gave translation faults at the PFP microcode.
  With SH=0 the micro-MMU ends idle; the CP still takes nothing.
- Reference from Android (t290/gpu/kgsl_regdump.c, CP_REG_TO_MEM, logs/
  kgsl-regdump.txt): VBIF identical; RBBM_STATUS c00003c2 there too (busy
  bits are normal); CP/UCHE/SECVID/RBBM clock/SMMU ranges are CP-protected
  (unreadable). Differences are kgsl quirk registers (TPL1 0xf02 0x4d4, PC
  0xd00 |0x100, HLSQ 0xe00/0xe06, VPC 0xe60, SP 0xec0) and live CP counters.

## Open questions
1. SMMU: bypass or context bank programming from EL1 (Linux kgsl does it, so
   the CB registers are reachable from the apps EE).
2. GX rail voltage: RPM request (like the SD card LDOs) or already on.
3. Register access safety: MDP reads reset the SoC (diag milestone 1) — do
   the same probe-with-pstore-log dance for 0x1c00000 (RBBM_CHIP_ID etc.)
   only after the GDSC and clocks are on.

## Kernel GPU interface (2026-10-21, patch 0124): verified on the tablet
`SWEET GPU: self-test IB at 0x10000000 -> OK in 126 us (fence 1/1, marker
0x5a5aa5a5)`: an indirect buffer in the user window (our level-2/3 SMMU
tables under the shim's level-1 table, 4 KiB pages, 0x10000000-0x7fffffff)
ran from the ring and the CACHE_FLUSH_TS fence landed. SweetGPU (IOKit,
IOResources) offers info/alloc+map/free/submit/wait/fence to user space.
Next: an iOS user-space test of SweetGPU (needs an iOS arm64 build path and
a way to put files on the HFS root), then Mesa Freedreno's winsys on it.

## User space reaches the GPU (2026-10-21): gputest on iPadOS
`t290/ios/gputest.c` (CI ios-tools, ad-hoc signed), copied with
`t290/tools/t290-xfer.ps1` (patch 0125), started by a launchd job
(`t290/tools/launchd-add-job.py`, original kept as
/System/Library/xpc/launchd.plist.t290orig), output via /dev/console:
`OK, THE GPU RAN OUR BUFFER in 0.078 ms`, `1000 submit+wait round trips in
52.3 ms`. SweetGPU alloc + IOConnectMapMemory64 + submit + fence all work.

Next: Mesa Freedreno for iOS. Plan: Mesa 24.2 gallium freedreno (a5xx) +
the GL/GLES state tracker; a new src/freedreno/drm backend over SweetGPU
(fd_device/fd_pipe/fd_bo/fd_submit: alloc+map, submit, fence wait) instead
of the msm DRM ioctls; a small frontend modelled on gallium osmesa that
makes st contexts on the freedreno screen (later the EAGL shim for
backboardd). First target: a GLES triangle into a buffer, read back.
Build: macOS CI, meson cross file for iphoneos arm64.
