# ios-t290 — iPadOS 16 на Samsung Galaxy Tab A 8.0 (2019, SM-T290, `gtowifi`)

Порт по образцу [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet)
(XNU + неизменённый userland iOS 16 на Redmi Note 10 Pro). Локальная копия:
`D:\test1\ios-sweet`, его `NOTES.md` — подробный журнал того, как это делалось.

## Active handoff 2026-10-22: FPS and missing UI, kernel 0135 preparation

- User still reports missing icons/labels/keyboard, purple text or regions,
  broken rendering in some apps; wants both higher FPS and graphical fixes.
- Fresh Android pstore saved to `ios-t290/logs/fps-bugs1/`: same boot as
  `logs/b9/`. ES2/ES3 GPU pixel guard PASS, GPU clock 400 MHz. Bridge counters
  report 29.0/35.5/24.6/46.0 rendered-frame flushes per second over 300-frame
  windows; waits 3.1-5.3 ms/frame, no padded copies. These counters are not
  proof of panel FPS or correct UI. No GL errors/GPU faults seen in this log.
- Recovery read-back is still **0133**, SHA256
  `51d3531b86f0412cb38820b5f13020b783a763a85128f15607708968aa1eb9ee`.
  Prepared 0134 has NOT been installed. Previous SD request is done 2/2:
  old bridge log retrieved to `dl/k-t290-0134/before/eagl9.log`, updated
  647024-byte bridge installed. CI 39774871567 (EAGL) and 39774877169 (XNU)
  both passed for 7095860.
- New patch 0135 handles surface 1 through the same synchronous cached
  native presentation path as CA's other buffer. Previously surface 1 went
  through a deferred timer while CA could reuse it: a concrete race in the
  code, but whether it explains the reported artifacts needs a device test.
  Direct presentation clears a pending redraw and mutes the console too.
  Actual SwapEnd/convert timing now logs every 300 swaps for both buffers.
- Additional concrete graphics bug: display-off fade wrote into fPixels,
  the GPU shadow, destroying retained CA pixels. On T290 the fade now only
  changes the packed physical panel, under the USB capture lock. The GPU
  surfaces survive lock/unlock for subsequent damage-only redraws.
- Local XNU compile of the changed framebuffer source PASS (0/1 failures),
  buildtree `/home/kirill/fps0135`, based on cached source plus 0133/0134.
  Generator `t290/display/mkpatch.py` takes the post-0134 framebuffer source.
- EAGL changes: raw TLS context mirror (pthread key retains ownership and
  exit cleanup), no redundant Flush before Finish on a full-screen frame.
  New guard tests sample/blend real CA formats: RGBA/BGRA icons, A8/L8
  glyphs, RGBA16F intermediates, ASTC icons, all with a mipmapped filter.
  Draw tracking now also covers base-vertex/multi/indirect variants, and
  compressed texture replacements/updates participate in IOSurface tracking
  and GL error checks. Previously these could bypass the presentation wait
  or retain stale image metadata after replacing texture storage.
  CI/device results for these new changes are pending; no FPS gain claimed.
- Initial commit a0cd21f pushed; XNU CI 39780060304 and EAGL 39780057223
  queued at the last check (GitHub macOS runners). Kernel source is locally
  syntax-checked; real GPU format/blend tests need the next tablet boot.
- Next: commit/build 0135 and the bridge/tests through CI, prepare a checked
  recovery image and SD request. A new recovery write requires user approval
  under AGENTS.md; no new approval has been requested or received yet.

## Previous handoff: kernel 0133 + bridge8/Mesa (GPU only)

- Bridge7 boot (logs `ios-t290/logs/b7`): eagltest PASS on hardware incl. new
  checks; UI 4.9 fps, full-screen copy 3.9 MB/frame, GPU wait 5.8 ms/frame.
  User: icons missing, lock screen purple, very low fps. User wants GPU only.
- Commit 7cc0625: YCbCr 420f plane imports (Mesa R8/RG8 formats in patch
  0003), mipmap clamp (single-level textures with trilinear filters sampled
  black), frame timing split (GL calls / GPU wait / copies / outside GL).
- Commit 875614a, kernel patch 0133: IOSurface rows rounded to 256 bytes
  (no padded copies), display shadow rows 3328 (GPU renders into scanout
  surface 1), panel conversion reads the shadow via the cached alias after a
  flush (was uncached WC reads), SweetGPU selector 8 Prepare = per-submit
  cache upkeep (Mesa sends BO handles), VDD_CX (pm8953 S2 "smpa" 2) voted
  NOM via RPM "vlvl" 256 then gfx3d GPLL0/2 = 400 MHz at the first GPU client
  (boot-arg t290gpu_mhz=200 disables). Not verified on hardware.
- CI: eagl 39771029402 PASS, mesa-ios 39771029491 PASS, xnu 39771042195 PASS.
  Artifacts `dl/k-t290-0133/{eagl,mesa,xnu-kernel}`. Recovery = 0131 image
  (953ba242...) repacked with the 0133 kernel only (round trip verified).
- User approved ("да"). **INSTALLED** recovery 0133 `dl/k-t290-0133/recovery-0133.img`
  SHA256 51d3531b86f0412cb38820b5f13020b783a763a85128f15607708968aa1eb9ee, full
  read-back verified; kernel e1b3d978... Rollback image = 0131 `dl/k-t290usb1/t290-ios.img`.
- SD request STAGED (7 ops, read back cdbbd3e5...): get old log/Mesa/glapi to
  `dl/k-t290-0133/before/`, put bridge (`t290eagl-mesa.dylib`), eagltest,
  libOSMesa.8 and libglapi.0 from these builds. Next: boot iPadOS, pstore,
  `t290-xfer.ps1 -Result`; check "core clock 400 MHz", eagltest PASS, fps line.

## Active handoff 2026-10-22 (evening): bridge7 — GPU UI artifacts and speed

- User report on bridge6 + kernel 0131 (boot pulled to `ios-t290/logs/now/`):
  GPU UI works but is slow, with many artifacts. Log: eagltest PASS, no GL
  errors, no missing GLI slots, no GPU faults; GPU waits avg ~4 ms; display
  is surface 1 (bpr 3200, padded to 3328 by the bridge, copied back by CPU).
- Causes found in `t290/eagl/eaglshim.m` (bridge6):
  1. Padded textures were refreshed from their IOSurface only on
     glBindTexture. CA does not rebind a texture after another process
     draws into the surface (zero-copy assumption) -> stale/missing images.
  2. Every display flush compared the whole 800x1280 surface three times
     (backing/snapshot/source) and copied changed rows, although CA redraws
     only the damage rectangle.
  3. Every draw/error check resolved Mesa functions by name
     (OSMesaGetProcAddress = linear strcmp over the glapi table).
  4. Padding used a 256-byte rule for all formats; Freedreno needs 64 px
     (LA08: 128 bytes, A8/L8: 64), so some LA08/A8 surfaces were padded needlessly.
- Commit 6bbe958 (bridge7): seed check of every bound padded texture before
  each draw; GPU writes tracked as row ranges from viewport/scissor, only
  those rows copied back (no snapshot); GL entry points cached; per-format
  pitch; log line every 300 frames: fps, GPU wait, KB copied per frame.
  eagltest adds: LA08 update without rebind, scissored redraw rows 10-19.
- **EAGL CI 39765269483 PASSED** (iOS build; mac fake-GPU ES2/ES3 incl. the
  new checks; fake GPU does not verify pixels). Artifacts `dl/eagl-bridge7`:
  bridge 449488 bytes SHA256 a942fc24f92e009a2e43d28109c6ca91927409d58b082b7c33a0f28173554727,
  eagltest 88720 bytes SHA256 47bdc2357451314e6d7f418e9d107c7d081817073bb46ba3ab638f048f85b4fc.
- Previous request (bridge6) confirmed done 4/4. **New SD request STAGED, 4 ops:**
  get `/private/var/tmp/t290eagl.log` -> `dl/eagl-bridge7/eagl6.before.log`,
  get `/usr/lib/t290eagl-mesa.dylib` -> `dl/eagl-bridge7/bridge.before.dylib`,
  put bridge7 as `/usr/lib/t290eagl-mesa.dylib`, put `/usr/bin/eagltest` (755).
  Whole 34138256-byte request read back from the SD, SHA256 811fcaf7...0ab75 matches.
  **Next:** user boots iPadOS (USB cable to the PC allows `t290/usb/viewer.py`
  screenshots), checks UI after ~3 min, returns to Android; then pstore +
  `t290-xfer.ps1 -Result`; look for the eagltest PASS lines and the
  "fps over 300 frames" lines. Not verified on hardware yet.
- Kernel-side follow-ups (NOT done, need CI + recovery flash approval):
  IOSurface rows always 256-aligned (no padding at all -> zero-copy), display
  surface 1 with bpr 3328 (GPU renders straight into the scanout surface),
  cache maintenance per submitted buffer instead of all imported surfaces on
  every submit/wait (`SweetGPU.cpp flush_surfaces`), GPU clock 200 MHz
  (shim GPLL0/4; Android runs up to 650 MHz) — needs the GFX rail corner.
- Separate stability issue seen in the bridge6 boot (`eagl6-console.bin`):
  panic `recount.c:414 rsn_time_mach 0 >= ...` (non-monotonic time, cpus=4).

## Handoff 2026-10-22 (later): USB CDC screen/debug link, kernel 0131

- Patch 0131 (`t290/usb`, commit with "xnu 0131"): HSUSB/ChipIdea device
  mode as CDC ACM (VID 1209 PID 0001, "T290 Screen Debug"), commands F/L/I
  return panel pixels, kernel log, info; PC side `t290/usb/viewer.py`
  (Windows usbser, tkinter + Pillow; unit test passes).
- Fixed vs the uncommitted draft: USB PHY rails verified on Android
  (`/sys/class/regulator`): hsusb_vdd_dig = **L23 0.8 V** (shared with DSI
  vdda), HSUSB_1p8 = L7 1.8 V, HSUSB_3p3 = L13 3.05 V. The draft used L2
  1.2 V (disabled, no consumers on Android). Init now runs in its own kernel
  thread so the root mount never waits for USB.
- Kernel CI 39749834055 PASSED. Image `dl/k-t290usb1/t290-ios.img`
  (mkboot NATIVE=1, -DT290_GPU2, EXTRA="t290usb=2"; boot args otherwise equal
  to recovery 0130), SHA256 953ba242a5e9ac695c905937986c4de0ed99e8bd24d16af12aecf826bb9bd4e0.
- User approved ("Да, записать recovery 0131 с USB"). Backup of 0130 first:
  `ios-t290/backup-20261022-usb/recovery-before-0131.img` (144be875...).
  **INSTALLED** and full-partition readback verified (953ba242...). Writes to
  mmcblk0p57 sometimes fail with a transient "Permission denied"; retry.
- Bridge6 SD request (below) is still pending and unaffected.
- **Next:** user boots iPadOS with the USB cable on the PC; check that
  Windows enumerates VID_1209&PID_0001 (COM port), run viewer.py; if not,
  read pstore lines "T290 USB:" (rails, id/cap, bus reset, setup).

## Active handoff 2026-10-22: EAGL bridge

### Latest result: bridge5 real tests PASS, GUI still slow/incomplete

- User booted bridge5 and reports slower-than-CPU UI, artifacts and missing
  elements. Fresh logs `ios-t290/logs/eagl5-{console,head}.bin`. All4 SD ops
  completed successfully; old log/bridge retrieved to `dl/eagl-bridge5`.
- Bridge5 **real-device ES2 and ES3 tests PASS**, including all rows of
  format0 120x120, BGRA800x1280, and LA08 samples127/127/127/255 then
  63/63/63/128 after an update. Final testexit0. No compositor import
  failures, missing GLI calls or GPU faults/timeouts observed in this log.
  This does not imply correct composition or acceptable UI performance.
- Concrete code problems: every flush became a full GPU wait and scanned
  all padded surfaces; bindings compared whole CPU images. The same
  surface65 was padded twice in backboardd, at independent texture names,
  so aliases did not share image storage or GPU resource tracking.
- Current fix: share a retained Mesa EGL image per IOSurface/layout within
  the sharegroup, including direct imports; use IOSurface seed changes
  from Lock/Unlock to refresh CPU images; track framebuffer GPU writes;
  publish dirty padded outputs only. Flush is asynchronous except when
  dirty display surface1 must be copied for the CPU framebuffer driver.
  Repeated object fences reuse a completed wait when no new work occurred.
- Regression test now aliases the same IOSurface through two texture names,
  renders green through the second and checks CPU pixels through the first
  object's fence. It would fail with bridge5's separate shadows. LA08 CPU
  updates now use IOSurfaceLock/Unlock, exercising the seed protocol.
- Added throttled GL error diagnostics (pending errors preserved for callers)
  and wait-time/flush/draw counters for the next actual compositor run.
  Hardware performance/correctness of these changes is still pending.
- CI39744412227 failed before compilation: parsed SDK return types retained
  trailing whitespace (`void `). Fix178794f normalizes them, which also
  makes diagnostic hooks reachable after void calls. Framebuffer attachment
  tracking now follows image storage replacement after an FBO was attached.
- **CI39744887745 PASSED**, commit5d22e0c: all3 jobs, iOS build and mac fake
  ES2/ES3 alias/seed/GL checks. Earlier CI39744753980 (178794f) also passed.
  **Bridge6 STAGED**, not yet verified on hardware. Files `dl/eagl-bridge6`:
  bridge429264 bytes SHA256 a66a72c321b005a333b7c4cf7f4aaecef5ea8c7bd6115a5080672aba6f053e2f;
  test88720 bytes SHA256 195b1a5366e04698e2cf6ec4ee5866a0757f694a0fbffff841f8ab77d12c8c15.
- **Pending SD request4 ops:** get previous log to
  `dl/eagl-bridge6/eagl5.before.log`, get active bridge to
  `dl/eagl-bridge6/bridge.before.dylib`, put new
  `/usr/lib/t290eagl-mesa.dylib` and `/usr/bin/eagltest` (mode755).
  Entire34118288-byte staged request read back from actual SD and SHA256
  verified17440700cbf91e88103b93f0088cf1e00372e1e5e7db1924f763a4f31ac022bb.
  User asked to boot manually, inspect speed/completeness after~3minutes,
  then return to Android. Next: pull fresh pstore, xfer-Result; check alias
  pixel tests, GL error diagnostics, GPU wait times and actual user report.
  Do not claim artifacts or slowness resolved before this hardware test.

### Previous bridge5 preparation

- User reports that an image appears with kernel 0130, but the UI is broken,
  incomplete and freezes. Fresh pstore pulled from connected Android:
  `ios-t290/logs/eagl0130-{console,head}.bin`. Both GPU pixel tests still
  PASS; backboardd creates ES3, submits draws and swaps surface 1. The old
  backboardd SIGSEGV is absent. No GPU timeout/fault observed in these logs.
- Concrete failures: format-less CA scratch surfaces 120x120/stride480,
  40x40/160 and 266x75/1064; BGRA display surface 1 800x1280/3200;
  LA08 source images 228x162/480 and 900x150/1824. Freedreno rejects rows
  not aligned to its import requirements. GLI slot470 finish_object_APPLE
  is also missing, so CA's wait before presentation does nothing.
- Bridge fix being built: infer missing pixel format from EAGL arguments;
  pad incompatible rows in a retained IOSurface, preserve packed original
  rows with CPU upload/publish copies, refresh CPU updates on texture bind,
  and implement finish_object_APPLE/flush as a real GPU wait plus publish.
  Direct aligned imports remain unchanged. Additional CPU copies and full
  waits are conservative; smoothness remains unverified on hardware.
- Regression test now covers format0 120x120, BGRA800x1280 with object-fence
  output/all rows, and LA08 228x162 sampling plus a later CPU update. Both
  ES2/ES3 run these cases before backboardd can use the new bridge.
- CI 39742047004 (09a40d0) passes the iOS build and both GLES fake-GPU
  regression paths; this does NOT validate GPU pixels. Follow-up 3f0bd95
  limits object-fence publication to that object and copies only changed
  rows, avoiding full-display copies for every small image fence.
  CI 39742295266 iOS build/mac smoke passed; layout job still pending when
  checked. Corrected signed tolerance arithmetic in the LA08 pixel check.
- **Final CI 39742477239 PASSED**, commit ca94264: all jobs including the
  iOS build and mac fake ES2/ES3 regression tests. These tests report GL
  error0 for both LA08 updates and exercise strides480/3200, but fake GPU
  does not verify rendered pixels. Device artifacts `dl/eagl-bridge5`:
  bridge426240 bytes SHA256 b3f1e7de6cdbb11324498da53cc29a43431397eaca11d1265c8c5f7d84839571;
  test88608 bytes SHA256 e402943b33a0e68e584f6acb1faf29b301b19fef6176871e428238cc04b9e353.
- Previous SD request confirmed done2/2. **New request STAGED: 4 ops**,
  get previous `/private/var/tmp/t290eagl.log` to
  `dl/eagl-bridge5/eagl0130.before.log`, get previous active bridge to
  `dl/eagl-bridge5/bridge.before.dylib`, then put updated
  `/usr/lib/t290eagl-mesa.dylib` and `/usr/bin/eagltest`. Entire 34115104-byte
  raw SD request read back from device and SHA256-verified:
  47d49628d5ff3132954e49af0ae6a8add0f8a31a010b34b1d29ad91a51ef1175.
  Files apply on the next iPadOS boot before the launcher guard runs.
  Recovery0130 and Mesa libraries are unchanged. User was asked to boot
  manually, inspect UI after ~3 minutes and return to Android. **Next:
  read fresh pstore and xfer -Result; new hardware results still pending.**

### Earlier kernel-0130 preparation and installation

- Boot with bridge3 completed all 5 SD operations. `eagltest` reported ES2
  and ES3 renderer FD504, FBO 0x8cd5, clear BGRA 191/128/64/255, triangle
  centre 0/0/255/255, and final exit 0. **Real GPU pixels are now verified.**
  Logs: `ios-t290/logs/eagl3-{console,head}.bin`.
- The guard injected backboardd, which created ES3 and imported its 800x1280
  display surface. It then crashed (SIGSEGV, far 0x30): memcpy from NULL+0x30
  inside `IOSurfaceClientGetBulkAttachments`, called by CA's
  `iosurface_get_edr_factor` / `WindowServer::IOSurface::wrap_buffer`.
- Exact cause from firmware disassembly: IOSurface selector 6 =
  `IOSurfaceClientWrapClientImage` (32-byte input, 0xa68-byte lock result).
  The generic success/zero-output stub creates an invalid IOSurfaceClient
  with no state page. The GPU itself did not fault.
- Fix 3dd79a4 interposes `IOSurfaceWrapClientImage` to return NULL on iOS,
  allowing CA's existing GL texture-upload fallback for CPU images. Native
  IOSurfaces still import directly. The new device test checks that this
  interposition actually takes effect; otherwise the guard uses CPU GUI.
- **EAGL build/mac smoke CI 39737962131 passed**. Artifacts `dl/eagl-bridge4`.
  Boot4 consumed the 2 puts successfully. Its test confirms interposition
  and real GPU pixels, but calls from the prebound QuartzCore shared-cache
  stubs bypass the interposed public entry point. Backboardd crashes at
  the identical IOSurfaceClientGetBulkAttachments memcpy from NULL+0x30.
  New logs: `ios-t290/logs/eagl4-{console,head}.bin`. The workaround is not
  sufficient for the compositor; no further shim-only reboot is useful.
- Kernel fix 0c84f68 / patch 0130 returns kIOReturnUnsupported for selector
  6 instead of a bogus success. **Kernel CI 39738089026 PASSED**,
  SWEET_BOARD=t290. Kernel downloaded and SHA256 matches the CI manifest:
  c0cca894f3347ef0452317e44d09241ea25fc819ff8bece549259400bd87d387.
- Current recovery backed up and SHA-verified on device and PC at
  `ios-t290/backup-20261022-gpu/recovery-before-0130.img`, SHA256
  dba7a21a196c3fe04a0ed432c29bb32dacabb0bfdecf0e823fe4dffbbf7b00cf.
  `dl/eagl-k0130/repack.py` replaces only the XNU payload, preserving shim,
  appended DTB, ADT, boot args and opaque Samsung/AVB metadata. The unchanged
  payload packing round trip is byte-identical to the backup.
- **INSTALLED:** `dl/eagl-k0130/recovery-0130.img` (64 MiB), SHA256
  144be875b8fca287c46e45e2fcd622255709b2f4deb341a7923023454cb50dc4.
  Preserved shim/DTB, ADT, boot args and opaque AVB metadata; unpack/check
  passed. Pushed to `/data/local/tmp/recovery-0130.img`; device SHA matches.
  Partition size 67108864. Original dba7a21a... recovery remains in the backup.
- **User explicitly approved writing recovery** (reply "Да, записать recovery"
  on 2026-10-22). After an adb disconnect, user reconnected USB and Android
  became available. The approved helper successfully wrote recovery, synced
  and read back the full partition: SHA256 144be875...cb50dc4 (matches image).
- Approved flash helper `dl/eagl-k0130/flash-approved.sh` verifies image hash,
  original partition hash and 64 MiB partition size before dd, then syncs
  and verifies the entire partition SHA256. Write completed successfully.
  Existing guard and bridge4 remain active. **Next: manual iPadOS boot with
  kernel 0130, then return to Android and read pstore.** No adb reboot
  recovery command (LK misc/recovery-loop issue); user uses buttons/menu.
- `t290-xfer.ps1` now checks every adb exit code and refuses to use stale
  local transfer headers after connection errors. Tested normal result
  reading and an injected native-command failure (exit 7); both pass.

### Earlier steps

- Verified from `ios-t290/logs/ps-eagl4.bin`: `t290launch` successfully injects
  the probe into backboardd, but Apple's `initWithAPI:properties:` returns nil
  for both ES3 and ES2. The GUI is still software rendered.
- Kernel 0129 already supports IOSurface imports and 256-byte row alignment.
  Mesa run 39729025501 failed: missing prototypes for OSMesaT290CreateImage /
  DestroyImage. Fixed in 4b07e1a, with real GLES contexts in patch 0004 and the
  shared libglapi runtime. **Mesa CI 39734142174 passed** (both jobs).
- Bridge in commits 5b07c6e / 6eba320: 975 typed GLI adapters, real GLES
  contexts, ARB shader adapters, IOSurface textures/renderbuffers. Keep the
  original observation probe separately. See `t290/eagl/README.md`.
- **EAGL CI 39735351364 passed** (6ebb7cc): iOS build and macOS smoke test
  with fake GPU. ES2/ES3 contexts report FD504, FBO 0x8cd5, shaders compile
  and draw commands generate without GL errors. Fake tests DO NOT verify
  GPU execution or pixels. Earlier iOS bridge build 39734954270 also passed.
- Local Mesa artifacts: `D:/test1/dl/mesa-eagl1/out`; initial bridge artifacts
  `D:/test1/dl/eagl-bridge1` (39734954270, clear-only test). The newer test
  additionally draws a triangle; it is not staged on the tablet yet.
- Staged 7-op SD transfer: get old `/System/Library/xpc/launchd.plist` and
  `/usr/lib/libOSMesa.8.dylib` to `dl/eagl-bridge1/*.before.*`; put new Mesa,
  libglapi, `/usr/lib/t290eagl-mesa.dylib`, `/usr/bin/eagltest`, and a launchd
  cache that runs the test once. Backboardd runs without the old verbose
  probe for this test. Existing `/usr/lib/t290eagl.dylib` stays untouched.
- **First device test completed**, logs `ios-t290/logs/eagl-bridge1-{head,console}.bin`.
  SD transfer: state 1, 7/7 successful; original Mesa and launchd saved to
  `dl/eagl-bridge1/*.before.*`. EAGL ES2 and ES3 both created on the device;
  each imported a 64x64 BGRA IOSurface, submitted GPU work, and reached
  `GPU finish`. ES2 necessarily passed its pixel check (otherwise ES3 would
  not start), but the final printf/PASS was not captured. Do not claim the
  entire test passed based on this log alone.
- That boot later panicked: `Non-monotonic time: last_dispatch ... ctime 0`
  at sched_prim.c:796, CPU 2. No GPU timeout/fault or eagltest crash observed.
  GUI was still the system software renderer in this boot.
- Commit dd2d40e adds kernel logging of every pixel result and final exit
  code, plus `t290launch --test TEST LIB PROGRAM`: run TEST with LIB; only
  inject PROGRAM if TEST exits 0, otherwise use the system renderer.
  **EAGL CI 39735995758 and ios-tools CI 39735995793 passed.** Artifacts:
  `dl/eagl-bridge3` and `dl/ios-tools-eagl3`.
- **Current pending SD request: 5 ops**, get old `/usr/bin/t290launch` to
  `dl/eagl-bridge3/t290launch.before`, then put updated bridge, shader/pixel
  test, launcher, and `launchd.guarded.plist`. Backboardd arguments are now
  `/usr/bin/t290launch --test /usr/bin/eagltest /usr/lib/t290eagl-mesa.dylib
  /usr/libexec/backboardd`. No separate test launchd job. On the next boot,
  shader/pixel test gates the first attempt to render the GUI with Mesa.
- User is back in rooted Android; request written and signature read back.
  Next: boot iPadOS, return to Android, read pstore and `t290-xfer.ps1 -Result`.
- GPU GUI still UNVERIFIED. Do not describe it as accelerated until the
  compositor runs through this bridge on the actual tablet. No partitions
  flashed in this session; only root files via the SD transfer were changed.

## Handoff 2026-10-21 (вечер)

- **GPU на голом железе работает** (gpu21, `t290/GPU.md` «SOLVED»): CP выполнил наше
  кольцо и нарисовал зелёный квадрат. Ключ — клоки GCC 0x59040 (timer) и 0x5904c (always-on).
  Следующий шаг: перенести эту инициализацию в XNU (драйвер GPU), затем present через GPU.
- **В recovery** (хэш 248a9321…, `D:\test1\dl\k-t290dev6\t290-gpu21.img`): ядро CI run
  39559502455 (патчи 0118 кнопки/батарея, 0119 родное разрешение 800×1280@1x, 0120 ленивое
  отображение SPMI), shim с тач-меню: iPadOS / Android (PSCI не работает → сброс PS_HOLD,
  проверено) / GPU diagnostics / Boot log. boot-args: NATIVE=1, sweetfb_hz=30, sweetbattery=1.
- Кнопки/батарея/30 Гц на планшете в iPadOS ещё не подтверждены пользователем.
- **Осторожно:** любое падение shim → LK снова грузит recovery (петля). Автокоманды
  (`t290/tools/t290-auto.ps1`, сектор SD 33556480) выключены (`T290_MENU_EXTRAS`), первая
  версия падала без AHB-клока SDCC2. Не включать без согласия пользователя.
- Выключение/перезагрузка из iPadOS не реализованы (нужен PS_HOLD + тип сброса в PMIC PON).

## Статус (2026-10-19)

Этап 0 — разведка. Планшет подключён по adb (только чтение), на нём ничего не меняли.

### Проверено на живом планшете (adb, без root, лог `logs/probe-20261019-225532.txt`, не в git)

- SM-T290, `gtowifi` / product `gtowifiser`, Android 11, прошивка **T290XXS5CWG5**
  (загрузчик ревизии 5 — откат на прошивку с меньшей ревизией невозможен), патч 2023-05-01.
- `ro.board.platform msm8937`, Hardware `Qualcomm Technologies, Inc SDM429`.
- 4 ядра, `CPU part 0xd03` = **Cortex-A53** r0p4; Features `fp asimd evtstrm aes pmull sha1 sha2 crc32` —
  **нет `atomics` (LSE)**, то есть чистый ARMv8.0. Подтверждает выбор userland iPad A9.
- RAM: MemTotal 1 892 156 kB (~1.8 ГБ доступно ядру).
- Экран 800×1280, density 213.
- Тач: `fts_ts` (FocalTech) на **i2c-3 @ 0x38** — совпадает с DTS P81081. Значит, файлы
  `sdm429-mtp-P81081*` действительно от T290.
- Остальное на I²C тоже совпадает с P81081: 5-0020 ABOV SAR («ABOV Cap Touch bottom»),
  8-0058/8-0059 усилители Awinic; ещё 2-0008/000c/000f/0045 (зарядка/fuel gauge/датчики — уточнить).
- Кнопки: `qpnp_pon` (питание и, видимо, громкость−) и `soc:gpio_keys` (громкость+).
- Загрузчик: **заблокирован** (`ro.boot.flash.locked 1`, `verifiedbootstate green`).
  `sys.oem_unlock_allowed 1` — переключатель «Заводская разблокировка» уже включён.
- eMMC `mmcblk0`, раздел **aboot** (= LK) есть. Ключевые разделы: boot p58, recovery p57,
  vbmeta p17, dtbo p27, userdata p69, persist p48, efs p43, sec_efs p42, modemst1/2 p40/p41,
  fsg p36, fsc p30, param p47. Размеры разделов, cmdline и /proc/device-tree без root не читаются.

## 🎉 2026-10-20 ~20:10: XNU + iPadOS userland на ПЛАНШЕТЕ с microSD

Как запускать: `t290/tools/mkboot-t290.sh KERNEL OUT.img STOCK_BOOT MAGISKBOOT FIX.py` → recovery-образ
(t290 shim как iBoot + payload mkpayload: ядро + ADT mkadt_t290 без --qemu; boot-args rd=sdroot0 sdrw=1 cpus=1).
Пишется из Android: `adb push` + `su -c dd of=/dev/block/by-name/recovery`. Запуск: выкл → Vol+ +Power.
Выход: Vol− + Power → Android; журнал ядра: `/sys/fs/pstore/console-ramoops-0` (последние 256К),
с 0116 ещё `dmesg-ramoops-0` (первые 256К загрузки). Логи: `D:\test1\ios-t290\logs\xnu-boot*-pstore.txt`.
Корень iPadOS на карте: `D:\test1\ios-t290\tools\write-sd.ps1` (админ; диск 2 «Generic Flash-Disk» USB,
проверка имени/шины/размера, 16 ГиБ за 23 мин ≈ 12 МБ/с, сверка 6 участков). Вся карта = том HFSX, без MBR.

- Загрузка 1 (CI 39412629412, 0113+0114): ядро доходит до IOKit, panic `SWEET CPU: expected boot MPIDR 0`
  (SweetCPU.cpp:169) — у планшета загрузочное ядро MPIDR 0x100. → **0115**.
- Загрузка 2 (CI 39418230815): sdroot0 смонтирован, launchd и десятки служб работают; GUI-процессы падают:
  `NSInternalInconsistencyException: We expect a valid bounds for configuration … Main-External mode 0x0`
  — AppleCLCD отказался от 24-bpp буфера (`fRowBytes < fWidth*4`), основного дисплея нет. Журнал
  (256К) весь забит этим спамом — ранние строки ядра потеряны. → **0116**: теневой буфер 1536×2048 BGRA
  (iPad 5) → на каждом flush ½ (nearest), по центру 800×1280, байты R,B,G, сдвиг из ADT sweet-fbcon;
  первые 256К консоли в зону dump ramoops (0x92100000) без перезаписи.
- 🎉🎉 **Загрузка 3 (CI 39422813087, 0116): ЭКРАН БЛОКИРОВКИ iPadOS 16 на планшете** (~20:47 → ~21:05,
  1 CPU). Логотип Apple от backboardd через теневой буфер, затем консольный этап (`SK[0]: __nexus_register …
  identityservices`), как в QEMU, затем lock screen. Журнал: `logs/xnu-boot3-head.txt` (dmesg-ramoops-0) и
  `xnu-boot3-tail.txt`. Карта: `SWEET SDHC: … 50 MHz, 1 MiB read in 44120 us` (~23 МБ/с, High Speed прошёл).
- Загрузка 4 (CPUS=4): `SWEET CPU: 4/4 CPUs initialized` (PSCI CPU_ON по SMC, MPIDR 0x101–0x103) → lock screen.
  Зеркало pstore при 4 CPU обрывается рано (параллельные t290_prz_put без блокировки) — починить.
- **Тач (0117, CI 39432676892, ~22:30): работает в iPadOS** (подтвердил пользователь). FT5452 @0x38:
  QUP3 (0x78b7000) даёт `I2C_STATUS 0x1321c4` BUS ERROR (причина не найдена; FIFO v2 с 2 байтами
  rx-тегов не помог), а bit-bang I2C на GPIO10/11 работает (shim touch1c: `BITBANG sda=1 scl=1 id=0x154`,
  374 отчёта). Драйвер `bsd/dev/sweet_t290_touch.c`: L5/L10 через `sweet_t290_rpm_ldo()` (канал SMD
  sweet_sdhc), reset GPIO64, опрос GPIO65 раз в 10 мс, bit-bang ~250 кГц; SweetHID: панель 800×1280 →
  окно iPad 768×1024 @(16,128). mkboot: тач включён, CPUS по умолчанию 4.
- GPU — `t290/GPU.md`. Итог автономной сессии ~22:35–23:15 (только из Android через kgsl, без
  перезагрузок в shim): своя PM4-команда выполнена; 2D-блиттер копирует (масштаб — нет); собран
  ассемблер ir3 из Mesa; первый вычислительный шейдер и шейдер вывода на панель (байт-в-байт верный,
  но медленный: номер нити внутри группы не доходит до шейдера — открытый вопрос).
- Состояние планшета на конец сессии: Android с root, в `recovery` — образ iPadOS
  `dl/k-t290dev4/t290-ios-recovery.img` (0113–0117, тач, 4 CPU); `/data/local/tmp` очищен.

## 2026-10-20 ~18:40: microSD отвечает на голом железе (diag milestone 3b)

Цепочка, которая работает (всё в `t290/shim/diag.c`):
1. **Питание карты через RPM** (L11 = карта, L12 = контакты; оба `qcom,rpm-smd-regulator`, напрямую по SPMI нельзя).
   SMEM 0x86300000 (ver 0xb), легаси-TOC с +208: item 13 = таблица каналов, `rpm_requests` cid 4
   (ctype & 0xff == 0x0f), item 14+cid = половинки канала (word access, по 11 слов; APPS tx, RPM tx),
   item 338+cid = FIFO (TOC reserved = база 0x60000 — RPM msg RAM): sh 0x60000, fifo 0x60800, 0x400 на сторону.
   LK оставляет канал tx=CLOSED, rx=CLOSING. Открытие как в Linux smd_state_change: **обнулить tx.head
   и rx.tail**, OPENING (+fSTATE, IPC 0xb011008 bit0), дождаться RPM OPENING/OPENED, OPENED + DSR/CTS/CD.
   Без обнуления первый запрос валит RPM → сброс SoC. Запрос: SMD-заголовок 5 слов (len,0,0,0,0),
   "req" 0x00716572, len, msg_id, set 0 (active), "ldoa", id, data_len, KVP {"uv" 2950000}, {"swen" 1}.
   Ответ RPM — 0x28 байт на каждый запрос.
2. Контакты sdc2: TLMM+0x109000 = 0x1fe4 (clk 16 мА без подтяжки, cmd/data pull-up 10 мА); LK оставляет 0xbdb (pull-down).
3. Частота: GCC SDCC2 RCG 0x43004 (M 1, N ~3, D ~4, CFG XO/12 dual-edge = 400 кГц), ветки ahb 0x4301c и apps 0x43018.
4. SDHCI: core+0x78 HC_MODE = 1|1<<13, SW reset, опрос/ACK pwr-irq (core 0xdc/0xe4/0xe8), clk ctl int+SD,
   power 3.3 В. pstate 0x01f80000 (CMD/DAT в единице).
5. CMD0, CMD8 → 0x1aa, ACMD41(0x40ff8000) → OCR 0xc0ff8000 (SDHC/SDXC), CMD2 CID (ADATA 0x1d "AD" "SD"),
   CMD3 RCA 0x59b4, CMD9 C_SIZE 0x1ce7f = 59200 МиБ (карта 64 ГБ пользователя).

Экран: байты в буфере **R, B, G**; видимая картинка = буфер со сдвигом **229 px вправо** с переносом
(экранный x → буферный x−229, при x<0 +800 и строка −1) — линейка подтвердила.
**Чтение работает (milestone 4, ~18:50):** CMD7 select (R1b), CMD17 PIO через буфер 0x20, блок 512 по
блочному адресу. LBA0 на 400 кГц/1 бит и LBA32768 на 25 МГц/4 бит (ACMD6 arg 2 + host ctl bit1;
RCG: GPLL0 /16, M/N 1/2) — суммы совпали с `dd` из Android (6dee0c04, 2b6e2cdb). 2048 × CMD17 = 1 МиБ
за 0,59 с ≈ 1,7 МБ/с (MMU/кеш выключены, PIO по одному блоку). Для XNU: CMD18/CMD25 + ADMA2 + 50 МГц HS.
Следующее: драйвер sweet_sdhc для XNU (rd=sdroot0, вся карта = том, без таблицы разделов).

## 2026-10-20 ~18:05: первый наш код на планшете (t290/shim/diag.c) — работает

`t290/shim/build.sh STOCK_BOOT MAGISKBOOT FIX_BOOTABLE_IMAGE.py` → `out/t290-diag.img`:
shim (start.S/shim.ld из ../../shim + diag.c) упакован в стоковый заголовок (magiskboot repack:
gzip Image + стоковые appended DTB) + SignerVer02/AVBf. Пишется в `recovery` из Android:
`adb push` → `su -c dd of=/dev/block/by-name/recovery` (+ sha256). Запуск: выключить → Vol+ +Power.
Выход: Vol− + Power ~7 с → **сразу Android** (круга нет, Odin не нужен), если shim не сбросил планшет сам.
TWRP обратно: тот же dd с `D:\test1\dl\twrp\recovery.img`.

Грабли (каждая стоила круга загрузки + Odin):
- **Чтение регистров MDP (0x1a01000 + пайпы) сбрасывает планшет** (XPU/защита?). Не трогать дисплейный блок.
- `adb reboot recovery` → LK грузит recovery после КАЖДОГО сброса (выход только Odin+TWRP).
  Запускать пробы кнопками. Shim не должен сам перезагружаться (теперь висит `HOLD`).
- Отладка без экрана: shim пишет журнал в **ramoops console** (`0x92140000`, заголовок
  persistent_ram_buffer sig 0x43474244, start=size=len) ДО каждого шага → после сброса Android/TWRP
  показывает его в `/sys/fs/pstore/console-ramoops-0`. Так найден сброс на MDP.

Результат (журнал `D:\test1\ios-t290\logs\diag-run1-pstore.txt`, фото у пользователя):
- EL1, MPIDR 0x80000100, MIDR 0x410fd034 (A53 r0p4); CNTFRQ 19.2 МГц; PSCI 1.0 по smc.
- LK: x0 = DTB 0x83400000 (0x597a6 Б), /memory 0x80000000+1G, 0xc0000000+1G.
- APSS WDT LK **не включает** (en=0).
- Splash 0x90000000: **24 bpp (RGB888), stride 2400**, порядок байт в памяти **R,G,B**
  (жёлтый вышел голубым при B,G,R). Видимая картинка **сдвинута по горизонтали** (строки
  «заворачиваются», хвост слева) — точку начала скан-аута уточнить линейкой.
- GCC: SDCC1 apps/ahb включены LK; **SDCC2 RCG (cmd=0x80000000) и ветки apps/ahb выключены**.
  После включения ahb (CBCR 0x4301c bit0) регистры sdhc2 читаются: HC ver 0x4d02 (SDHCI 3.0),
  cap 0x302dc8b2 (база 200 МГц), cap1 0x8007, clk 0x403, pstate 0x80000; core ver 0x1000004d.
- Карта: CD GPIO67 in=0, pstate CD=0 — карта не вставлена (или полярность — проверить с картой).
Следующее: цвета R,G,B + линейка для сдвига; включить apps-clk SDCC2 (RCG 0x43004/0x43008),
питание карты (PM8953 L11/L12 через SPMI), карта в слоте → CMD0/CMD8/ACMD41.

## 2026-10-20 ~17:00: планшет разблокирован, TWRP, живой FDT

- Загрузчик разблокирован пользователем (`ro.boot.flash.locked=0`, `verifiedbootstate=orange`),
  прошивка та же T290XXS5CWG5 / Android 11. Knox сгорел (необратимо).
- TWRP 3.7.0_11-HeribertYavuz (XDA «OFRP & TWRP For TAB A 2019 (T290) (Binary 5)») прошит Odin 3.14
  в AP одним tar: `recovery.img` автора + свой пустой `vbmeta.img` (AVB0, flags=2, 4096 Б) —
  `D:\test1\dl\twrp\twrp-AP.tar`. Стоковый vbmeta перезаписан → для отката нужна стоковая CWG5.
  Вход в TWRP: Odin без Auto Reboot → Vol−+Power до чёрного → сразу Vol+ +Power.
  Android грузится как раньше (boot/system/userdata не трогали). В TWRP adb = root.
- Дампы (вне git): `D:\test1\ios-t290\logs\twrp-20261020\` (fdt.dtb/fdt.dts, iomem, dmesg, partitions,
  getprop, interrupts); бэкап всех разделов ≤200 МБ (64 шт., 712 МБ, sha256): `D:\test1\ios-t290\backup-20261020\`
  (RPMB не читается — норма). Большие (system/vendor/product/oem/cache/userdata) не копировались.

- **Root + постоянный TWRP (17:20).** Android при каждой загрузке восстанавливал стоковый recovery
  (`/system/bin/install-recovery.sh` + `/system/recovery-from-boot.p`, applypatch только если boot = сток,
  sha1 10b1f840…). Решение: Magisk v30.7 (GitHub, sha256 e0d32d21…) патчит стоковый boot на ПК
  (`magiskboot` x86_64 из APK, `boot_patch.sh`, `LEGACYSAR=true KEEPFORCEENCRYPT=true KEEPVERITY=false`;
  в стоковом boot нет ramdisk, root=PARTUUID — legacy SAR) → `fix_bootable_image.py`
  (GalaxyA20s/A20s-Fix-Bootable-Image: `SignerVer02` + AVBf-размер, иначе SECURE CHECK FAIL) →
  Odin AP `D:\test1\dl\root\magisk-twrp-AP.tar` (boot+TWRP+пустой vbmeta). Откат boot: `stock-boot-AP.tar`.
  Проверено: Android грузится, `magiskd` работает, `adb shell /debug_ramdisk/su -c id` = root;
  после загрузки Android sha256 разделов recovery/boot = прошитым образам → TWRP больше не слетает.
  **Значит, наше ядро iPadOS в `recovery` Android тоже не затрёт.**

Живой FDT (board_id P81080BA5, `qcom,sdm429`, msm-id 0x162; панель `jd9367_wxga_video_txd`):
| Что | Значение |
| --- | --- |
| RAM | `0x80000000`+1 ГБ и `0xc0000000`+1 ГБ = сплошные 2 ГБ `0x80000000..0x100000000` |
| Свободно для XNU | крупнейший кусок System RAM: **`0x92600000..0xffffffff` (~1754 МБ)**; ниже — дыры reserved |
| reserved no-map | 0x85000000+0x800000, 0x85b00000+0xd00000, 0x86800000+0x5500000, 0x8bd00000+0x1100000, 0x8ce00000+0x700000; splash 0x90000000+0x1400000; ramoops 0x92100000+0x100000; 0x92200000+0x400000 |
| Splash fb | 0x90000000, 800×1280, 32 bpp, stride 3200 (`mdssfb_80000`) |
| GIC | v2 `qcom,msm-qgic2`, GICD 0x0b000000, GICC 0x0b002000 (подтверждено) |
| Таймер | 19.2 МГц, PPI <1 2>,<1 3>,<1 4>,<1 1> → **vtimer INTID 20** (подтверждено) |
| CPU | 4× cortex-a53, MPIDR 0x100..0x103, `enable-method = psci`, **PSCI 1.0 method = smc** (в QEMU — hvc) |
| eMMC | sdhc1 `qcom,sdhci-msm` hc 0x7824900, core 0x7824000, cmdq 0x7824e00; IRQ SPI 123 (hc), 138 (pwr); 8 бит; 32 ГБ (QX63AB) |
| **microSD** | **sdhc2** hc 0x7864900, core 0x7864000; IRQ SPI 125 (hc, INTID 157), 221 (pwr); 4 бит; **card-detect tlmm GPIO 67**; питание vdd/vdd-io — регуляторы PMIC (могут быть выключены LK — проверить) |
| UART | msm-uartdm 0x78b0000 (earlycon), IRQ SPI 108 |

План пользователя: iPadOS-корень на **microSD** (sdhc2), ядро — в раздел `recovery` (вход Vol+ +Power),
Android в `boot` не трогать. Нужно: драйвер SDHCI (проверить в QEMU), карта памяти в mkadt_t290
(база 0x92600000?), PSCI smc, формат Samsung boot.img v1 (ядро + дописанный DTB) для LK.

## 🎉🎉 2026-10-20 ~16:40: ЭКРАН БЛОКИРОВКИ iPadOS 16 в QEMU (прогон ios3)

Тот же прогон ios3 (CI 39321740004, SMP=1, TCG): логотип Apple ~240–1620 с, затем ~1620–1860 с
на экране консоль ядра (`SK[0]: __nexus_register identityservices(53)...`), первый `SWEET FB: swap`
~1840 с, к **1920 с — lock screen SpringBoard**: «Wednesday, December 31», часы, «Press home to open»,
батарея 100% (снимки `D:\test1\qemu-out\ios3\shot-1920.png` … `shot-2160.png`).
Вывод: ядро T290 (A53, 4K, GICv2, virtio-blk) + неизменённый userland iPad 5 доходят до GUI;
«зависание» на логотипе было просто медленной первой загрузкой (datamigrator) на 1 vCPU.
Следующее: SMP=4 (быстрее?), ввод в QEMU (Home/тач) → домашний экран; потом путь на планшет
(LK/Odin, eMMC SDHCI, экран 800×1280, тач FocalTech).

## 2026-10-20 ~16:00: iPadOS userland в QEMU, логотип Apple (прогон ios3)

CI 39321740004 (189dc00), `qemu-ios.sh` SMP=1 на свежей копии корня iPad 5:
корень `vblkroot0` смонтирован, **launchd стартовал**, через ~800 с — около 100 демонов
(`exec ...` в msgbuf), SpringBoard (pid 25) и backboardd (pid 56) живы, на экране **логотип Apple**.
Ошибок virtio-blk нет. Стеки SpringBoard/backboardd на 242 с и 365 с совпадают — ждут в mach_msg
(наблюдение); при этом работает `com.apple.datamigrator` — гипотеза: первая загрузка, SpringBoard
ждёт миграцию данных, а TCG на одном vCPU очень медленный. Не проверено.
Результаты: `D:\test1\qemu-out\ios3\shot-*.png`, `D:\test1\qemu-out\mb-ios3.txt` (вне git).

Почему ios2 падал (`SWEET VBLK: ... status 255`, `load_init_program ... errno 5`): кончилось место на C:
(ext4.vhdx WSL вырос до 105 ГБ). Теперь большие файлы — на D:: чистый корень
`D:\test1\ios-images\ipad5-root.pristine.img`, рабочая копия `/root/ios/TAG.img` в WSL (удалять после прогона),
старое содержимое WSL — `D:\wsl-backup\` (tar). QEMU нужно держать в foreground-вызове `wsl`:
`nohup ... &` внутри `wsl bash -c` умирает при выходе wsl.exe.

## 🎉 2026-10-20 ~03:40: ядро T290 доходит до userspace в QEMU

CI 39309753629 (0101–0110 + firehose -mcpu в workflow), `qemu-t290.sh` (cortex-a53, GICv2,
RAM-диск init): `BSD root: md0` → `*** SWEET userspace: init running as PID 1 ***`.
Цепочка исправлений этого дня (каждое найдено в QEMU):
- 0104 ранняя консоль телефона (0x9c000000, 16K-таблицы) → выключена для T290;
- 0105 `casa` в commpage (pfz trylock) → ldaxr/stxr;
- `-mcpu=cortex-a53` (clang для arm64-apple-macos держит apple-m1 и LSE при любом -march),
  то же для libfirehose_kernel (workflow, xcconfig `OTHER_CFLAGS[arch=arm64]`);
- 0106/0108 kmem claims — из самой большой дыры kernel_map + запас 1/32, лог каждого claim;
- 0107 zone map iOS (24 ГБ VA вместо 128), 0109 пул компрессора iOS (RAM вместо 16×RAM);
- ADT: CPU compatible должен содержать `qcom,kryo470` (IONameMatch SweetCPU), иначе вечное ожидание CPU;
- 0110 commpage text VA выровнен на PAGE_RATIO (4K железо + 16K VM-страницы → write_pte_fast panic).
Загрузка ждёт ~30 с RTC (в QEMU его нет) — не блокер.

Отладка: `t290/tools/kmem.py PORT KERNEL msgbuf|bt|regs|read` через gdb-stub QEMU
(`qemu-sweet.py --gdbport`); `D:\test1\dl\ci-boot.sh RUN TAG` = дождаться CI → скачать →
скан LSE во всём ядре (`dl/bt/kscan.sh`) → загрузка с msgbuf/bt (`dl/bt/dbgboot.sh`).

Образ iPadOS: чистый — `D:\test1\ios-images\ipad5-root.pristine.img` (раньше `/root/ios/` в WSL), 11 ГБ на HFS+ 16 ГБ, fsck OK.
Собран `t290/tools/mkroot-ipad5.sh` (stage в ext4 → tar → KVM-гость с Ubuntu-ядром,
hfsplus.ko + nls_utf8.ko + busybox распаковывает в HFS+). Предупреждение: в образе iPad нет
system-контейнера mobileactivationd (data ark не записан).

## Состояние 2026-10-20 (утро)

- Форк: github.com/terx1232/ios-sweet, ветка `t290` (ветка по умолчанию форка — иначе GitHub
  не регистрировал workflow). Сборка: `gh workflow run xnu.yml -R terx1232/ios-sweet --ref t290 -f sweet_board=t290`.
- CI 39280792571 (0101+0102 старой версии): упал только на pe_fiq.c и IOKitBSDInit.cpp —
  `\n` в трёх строках kprintf/IOLog превратился в перевод строки (heredoc через Bash-инструмент
  схлопывает `\\` → `\`; писать такие правки через Write/Edit). Флаги T290 в CI дошли до 1101
  файла, патчи 0101/0102 применились. Исправлено в eee22c1, CI 39285650537 запущен.
- Патчи: 0101 (A53/4K/GICv2), 0102 (virtio-blk root, `rd=vblkroot0 vblkrw=1`),
  0103 (PSCI через HVC по `sweetpsci=hvc`, для QEMU SMP).
- **Локальная проверка компиляции работает** (WSL, clang 14 из Ubuntu):
  дерево `xnu-buildtree` + лог CI автора (run 39158169212) → `~/bt`, на него наложены 0101–0103;
  `XNU_CC_EXTRA=-enable-trivial-auto-var-init-zero-knowing-it-will-be-removed-from-clang`,
  `XNU_CC_POST="-DSWEET_T290=1 -march=armv8-a+crc+crypto -UARM_LARGE_MEMORY -UVM_KERNEL_LINK_ADDRESS -DVM_KERNEL_LINK_ADDRESS=0xfffffff007004000"`,
  `python3 tools/xnu-cc.py ~/bt/run.log ~/bt --per-file` (новые файлы вроде sweet_vblk.c — только явным путём).
  Результат: всё ядро проходит, падают лишь хостовые `SETUP/*` (как и у sweet). Ошибки CI совпали с локальными 1:1.
- iOS: iPad 5 (iPad6,11/J71sAP) iPadOS 16.7.16 20H392, IPSW качается в `D:\test1\ipsw\` (вне git).
  OS = 087-86634-044.dmg, SystemOS cryptex = 087-86624-047.dmg, AppOS = 087-86715-047.dmg.
  `research/ipsw-ipad5/j71sap.adt` (DeviceTree, вне git) → /product для mkadt_t290.
- WSL без сети: служба Windows **HNS отключена** (StartupType Disabled), нужен админ
  (`Set-Service hns -StartupType Manual; Start-Service hns`). Обход: .deb скачаны через Windows
  (`D:\test1\dl\debs`: hfsprogs, libfuse3-dev, libbz2-dev), apfs-fuse собран g++ без cmake
  (`/usr/local/bin/apfs-fuse`, скрипт `D:\test1\dl\build-apfs.sh`).
- QEMU: `t290/tools/qemu-ios.sh KERNEL ROOT.img OUT` — 4×A53, GICv2, virtio-blk корень,
  экран 1536×2048 (AppleCLCD сам выбирает родной размер, если буфер больше 1125×2436),
  снимки каждые 30 с (`--shots`) = замер времени загрузки.

## Ядро T290 (2026-10-19) — первая версия

Порт живёт веткой `t290` форка ios-sweet. Отдельной MACHINE_CONFIG нет: весь код
sweet (80+ мест `ARM64_BOARD_CONFIG_SWEET`, стейдж-скрипты по `MACHINE_CONFIG == SWEET`)
используется как есть, плата выбирается переменной окружения **`SWEET_BOARD=t290`**
(CI: input `sweet_board: t290`). Без неё патч 0101 ничего не меняет — сборка sweet прежняя.

`xnu/patches/0101-t290-a53-4k-gicv2.patch` (сделан на xnu-8796.141.3 + 0001–0081, все 81 патч sweet
легли локально; патчи darwin-xnu-build не пересекаются с файлами 0101):
- `MakeInc.def`: `-DSWEET_T290=1 -march=armv8-a+crc+crypto`; iOS-раскладка ядра
  (`-UARM_LARGE_MEMORY`, link 0xfffffff007004000) — macOS-раскладка на −2 ТБ не помещается
  в 38-битное пространство 4K-режима (T1SZ 26).
- `SWEET.h`: при `SWEET_T290` нет `__ARM_16K_PG__` и `__ARM_PAN_AVAILABLE__`,
  есть `__ARM64_PMAP_SUBPAGE_L1__` (как BCM2837), `HAS_GIC_V2`, 4 CPU, vtimer INTID 20,
  регистры GICv2.
- `vm_param.h`: `MACH_VM_MAX_ADDRESS` = iOS 0xFC0000000 (макОС-значение 47 бит не влезает в T0SZ 26).
- `pe_fiq.c`: драйвер GICv2 (GICD+GICC по `/arm-io/gic reg`, маска интерфейса ЦП из
  GICD_ITARGETSR0, `sweet_gic_ack/eoi/send_sgi`); `sleh.c`: IAR/EOIR через GICC;
  `SweetCPU.cpp`: IPI через GICD_SGIR; `start.s`: ICC_SRE_EL2 только при GICv3.

Проверено на практике: готовое ядро sweet (CI 18.10) на QEMU `-cpu cortex-a53` падает на
первой же LSE-инструкции `ldadd` (0xb8280028, `lck_grp_reference`) — флаг `-march=armv8-a`
обязателен. QEMU 6.2 при этом не отверг 16K-таблицы (MMU включился) — гранулу он не
проверяет; для строгой проверки нужен QEMU ≥ 7.2.

Инструменты: `t290/tools/mkadt_t290.py [--qemu]` (дерево sweet + GICv2, 4×A53 MPIDR 0x10x,
RAM 0xa0000000+1.5 ГБ, fb 0x90000000 800×1280, без sweet-ufs), `tools/qemu-sweet.py`
получил `--gic-version` и `--fb`, `t290/tools/qemu-t290.sh KERNEL OUT` — запуск в QEMU.

Следующий шаг: форк на GitHub пользователя → `gh workflow run xnu.yml -f sweet_board=t290`
→ исправлять ошибки компиляции → `qemu-t290.sh`. Ожидаемый успех: `SWEET GIC: GICv2 ...`,
`init running as PID 1`, как у sweet.

Не проверено / известные вопросы:
- 4K-путь pmap в xnu-8796 никто не собирал для этой конфигурации — возможны ошибки компиляции.
- Драйверы sweet (кнопки SPMI, тач GENI, USB, RPMh) обращаются к MMIO SM7150 — на планшете их
  надо выключить boot-args (`sweetkeys=0 sweettouch=0`, sweetusb по умолчанию выкл.) или загейтить по плате.
- PSCI на SDM429 (smc/hvc) не проверен; в QEMU virt PSCI идёт через hvc — пока cpus=1.
- Формат splash-буфера (32 bpp?) и точная карта памяти — сверить с живым FDT.

## Решения

- **Личность устройства: iPad (5th gen), iPad6,11 / J71sAP, iPadOS 16.7.x.**
  A9 = ARMv8.0 (как Cortex-A53), 2 ГБ RAM (как T290), планшет, официально
  поддерживает iPadOS 16. Userland iPhone X (A11, ARMv8.2) мог бы содержать
  инструкции, которых нет на A53 (LSE-атомики и т.п.) — с A9 этот риск снят.
- **4K-страницы вместо 16K.** Cortex-A53 не поддерживает гранулу 16 КБ
  (ARM DDI 0500F, ID_AA64MMFR0_EL1.TGran16 = 0). XNU 8796 сохранил 4K-путь:
  `osfmk/arm64/proc_reg.h` `#ifdef __ARM_16K_PG__ ... #else ARM_PGSHIFT 12`,
  `osfmk/arm/pmap/pmap.c` `native_pt_attr = &pmap_pt_attr_4k` без `__ARM_16K_PG__`.
  Образец — официальная конфигурация Apple **BCM2837** (Raspberry Pi 3, тоже A53):
  `pexpert/pexpert/arm64/BCM2837.h` — без `__ARM_16K_PG__`, с
  `__ARM64_PMAP_SUBPAGE_L1__`, MAX_CPUS 4. sweet (patch 0004) наоборот
  задаёт `__ARM_16K_PG__ 1` — его конфиг платы НЕ переносить как есть.
- Ядро собирается так же, как в sweet: GitHub Actions macOS + blacktop/darwin-xnu-build
  (нужен форк и аккаунт GitHub пользователя).

## Железо (из LineageOS android_kernel_samsung_sdm429, ветка lineage-22.2, 56a0722)

ВНИМАНИЕ: файлы платы `sdm429-mtp-P81081*.dts*` (board-id <8 0>, Wingtech,
комментарии «N8-R Project») — предположительно T290, **сверить с живым FDT
планшета** (`/sys/firmware/fdt`, нужен root/TWRP).

| Что | Значение | Отличие от sweet |
| --- | --- | --- |
| SoC | SDM429 (семейство MSM8937), 4× Cortex-A53, MPIDR 0x100..0x103 (Aff1=1) | A55/A76 |
| Прерывания | **GICv2** `qcom,msm-qgic2`: GICD 0x0b000000, GICC 0x0b002000 | GICv3 → нужен драйвер GICv2 |
| Таймер | 19.2 МГц, PPI по-квалкомовски `<1 2>,<1 3>,<1 4>,<1 1>` → виртуальный таймер = PPI 4 = INTID 20 (проверить) | у sweet INTID 19 |
| SMP | PSCI (`enable-method = "psci"`) | так же |
| Накопитель | **eMMC**, `sdhc_1` = `qcom,sdhci-msm` @ 0x7824900 (hc), 0x7824000 (core); стандартный SDHCI | UFS → новый драйвер, но SDHCI проще |
| Экран | 800×1280 DSI, continuous splash: `cont_splash_mem` 0x90000000, 0x1400000 | 1080×2400 @ 0x9c000000 |
| Тач | FocalTech (ft3528/ft8201…) I²C 0x38 на `i2c_3`, IRQ tlmm 65, reset tlmm 64, питание pm8953 L10 / L5 | Goodix GT9896 |
| PMIC | PM8953 + PMI632 | PM6150/PM6150L |
| Загрузчик | LK/aboot эпохи MSM8937, `CONFIG_BUILD_ARM64_APPENDED_DTB_IMAGE=y` (DTB дописывается к Image) | ABL, boot.img v2 с отдельным DTB |
| Прошивка | Odin / Download mode, нет `fastboot boot` | fastboot |

Reserved-memory (no-map, низкий банк): 0x85b00000+0xd00000, 0x86800000+0x5000000,
0x8b800000+0x1100000, 0x8c900000+0x700000, ramoops 0x92100000, ss_plog 0x92200000,
removed-dma-pool 0x85000000+0x800000.

## Риски (по убыванию)

1. 4K-путь XNU для iOS 16 никто давно не собирал — возможны поломки компиляции/рантайма.
   Userland iPadOS для A9 исполнялся на 16K-ядре: проверить, не зашиты ли 16K-предположения
   (dyld shared cache, libmalloc, vm_page_size).
2. Samsung: разблокировка сжигает Knox (необратимо), стирает данные; vbmeta/AVB;
   каждый тест = прошивка boot через Odin (нет временной загрузки).
3. Гипервизор Qualcomm/Samsung на EL2 может ограничивать доступ к памяти/MMIO.
4. 2 ГБ RAM и 4×A53 — рисование интерфейса CPU будет медленным.

## План

0. **Разведка (сейчас).** Подключить планшет по adb (`t290/tools/t290-probe.sh`): модель,
   прошивка, статус OEM unlock, ядро. Потом TWRP → бэкап разделов + живой FDT.
1. Shim: свой код стартует из LK и рисует текст в splash-буфере 0x90000000.
2. XNU (4K, конфиг SWEET→T290 по образцу BCM2837, GICv2) — сначала в QEMU (`virt`,
   `-cpu cortex-a53`, gic-version=2), потом на планшете, лог в splash/eMMC.
3. Драйвер eMMC (SDHCI), корень HFS+ из IPSW iPad6,11.
4. Экран (AppleCLCD 800×1280 → iPad 1536×2048?), тач FocalTech, кнопки PM8953 → SpringBoard.
