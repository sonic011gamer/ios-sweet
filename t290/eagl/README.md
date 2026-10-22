# EAGL on Mesa / Adreno 504

`eaglshim.m` supplies the EAGL object CoreAnimation uses for its GLES
renderer. It opens Mesa locally with `dlopen`; GL calls do not bind to
Apple's OpenGLES implementation. `gen_dispatch.py` generates typed adapters
for the SDK's 975-entry GLI dispatch table. Each removes GLI's first renderer
argument and selects that renderer's Mesa context. Shader-object ARB entries
are translated to GLES shader/program operations.

IOSurface textures and renderbuffers use Mesa patch 0003 and SweetGPU
selector 6. Mesa patch 0004 creates actual `API_OPENGLES2` contexts through
the OSMesa frontend. The required runtime files are `libOSMesa.8.dylib` and
`libglapi.0.dylib` in `/usr/lib`.

## Build and verification

The `eagl` workflow builds an iOS dylib, the original observation-only probe,
and `eagltest`. Its macOS smoke test downloads the last successful `mesa-mac`
artifact and runs the same context, GLI, shader and IOSurface paths using
`SWEET_FAKE=1`. **That test does not execute GPU commands or verify pixels.**

On the tablet run:

```
/usr/bin/t290launch /usr/lib/t290eagl-mesa.dylib /usr/bin/eagltest
```

`eagltest` must report renderer FD504, framebuffer status `0x8cd5`, no GL
errors and the expected BGRA bytes for both a clear and a triangle. It also
checks CA's format-less 120x120 scratch buffer, BGRA800x1280 output through
finish_object_APPLE, and LA08 228x162 sampling after CPU updates. Only
the real-device version verifies the pixels. It uses GLI calls throughout,
including the ARB shader adapters used by QuartzCore.

For a guarded compositor test, change backboardd's launchd ProgramArguments to:

```
/usr/bin/t290launch --test /usr/bin/eagltest /usr/lib/t290eagl-mesa.dylib /usr/libexec/backboardd
```

The launcher waits for the test. On exit 0 it injects the compositor; on
failure it runs backboardd with the system renderer. The off marker also
bypasses the test. This guards test failures, not later compositor crashes.

Logs go to `/private/var/tmp/t290eagl.log` and SweetGPU selector 7 (pstore).
Look for context creation, IOSurface imports, GPU finish counts, missing
GLI entries, shader errors, and backboardd crashes. A passing standalone
test alone is **not evidence that the GUI uses the GPU**.

## Rollback and current limits

`t290launch.off` in `/private/var/tmp` disables injection. Alternatively,
restore backboardd's ProgramArguments to `/usr/libexec/backboardd` using the
SD file-transfer tool. Preserve the previous launchd cache and Mesa library
before replacing them. No kernel/recovery reflash is needed with kernel 0129.

Rows not aligned to Freedreno's 64-pixel import pitch (256 bytes for BGRA,
128 for LA08, 64 for A8/L8) use a padded GPU IOSurface. Texture/renderbuffer
aliases share one Mesa EGL image per IOSurface/layout. CA treats IOSurface
textures as zero-copy, so before every draw the bridge checks the IOSurface
seed of each padded texture bound on any unit and re-uploads changed ones
(a bind-time check missed updates from other processes: stale or missing
images). GPU writes are tracked as a row range from viewport and scissor;
finish/object fences copy only those rows back, ordinary flushes copy
only for display surface 1, which the CPU framebuffer driver consumes.
Aligned surfaces import directly (zero-copy).

Mesa entry points used by the bridge itself are resolved once at load time
(OSMesaGetProcAddress is a linear search over the glapi table). Every 300
presented frames the log has fps, GPU wait and copy volume per frame.

The bridge handles the IOSurface path observed in this firmware. Drawable
renderbuffer allocation is not implemented; unsupported surface formats
fail import. Apple-only extensions are not advertised. Image inversion,
additional private EAGL parameters and unobserved entry points still need
validation against the actual compositor.
