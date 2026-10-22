#!/usr/bin/env python3
"""Build 0135 against SweetFramebuffer.cpp after patches through 0134."""
from pathlib import Path
import difflib
import sys

source, output = map(Path, sys.argv[1:])
before = source.read_text()
after = before

def replace(old, new):
    global after
    assert after.count(old) == 1, old
    after = after.replace(old, new)

replace('''		if (step <= 6) {
			for (uint32_t y = 0; y < fb->fHeight; y++) {''', '''		if (step <= 6) {
#if defined(SWEET_T290)
			if (fb->fT290) {
				/* Fade the physical panel only. CA retains the GPU shadow
				 * across display-off and may redraw just a damage rectangle.
				 * Fading that shadow corrupts its retained pixels on wake. */
				IOLockLock(t290CaptureLock);
				for (uint32_t y = 0; y < fb->fPanelH; y++) {
					volatile uint32_t *row = (volatile uint32_t *)(fb->fPanel +
					    (uint64_t)y * fb->fPanelRow);
					for (uint32_t x = 0; x < fb->fPanelRow / 4; x++) {
						row[x] = step < 6 ? (row[x] >> 1) & 0x7f7f7f7f : 0;
					}
				}
				IOLockUnlock(t290CaptureLock);
			} else
#endif
			{
			for (uint32_t y = 0; y < fb->fHeight; y++) {''')
replace('''#if defined(SWEET_T290)
			if (fb->fT290) {
				fb->t290Panel();
			}
#endif
		}
		sender->setTimeoutUS''', '''			}
		}
		sender->setTimeoutUS''')

replace('''		memcpy(fPending, rec, SWEET_FB_SWAP_REC_SIZE);  /* for a redraw after display on */
		IOLockUnlock(fLock);''', '''		memcpy(fPending, rec, SWEET_FB_SWAP_REC_SIZE);  /* for a redraw after display on */
		fDirty = false;                 /* do not replay an older deferred frame */
		IOLockUnlock(fLock);''')

replace('''	bool ok = sf != fSurface && sf->memory != NULL && sf->elementWidth == 1 &&''', '''	bool ok = sf->memory != NULL && sf->pixelFormat == SWEET_FB_FORMAT_BGRA && sf->elementWidth == 1 &&''')

replace('''		IOBufferMemoryDescriptor *bmd = OSDynamicCast(IOBufferMemoryDescriptor, sf->memory);
		uintptr_t pix = bmd != NULL ? (uintptr_t)bmd->getBytesNoCopy() : 0;''', '''		/* Both CA buffers must be read before SwapEnd returns. Deferring the
		 * default surface to the vsync timer let CA start the next GPU frame
		 * while that timer was still reading it (tearing and missing regions).
		 * The default surface wraps physical memory; its cached owner is
		 * fShadow, whereas CA's other buffer owns its descriptor directly. */
		IOBufferMemoryDescriptor *bmd = sf == fSurface ? fShadow :
		    OSDynamicCast(IOBufferMemoryDescriptor, sf->memory);
		uintptr_t pix = bmd != NULL ? (uintptr_t)bmd->getBytesNoCopy() : 0;''')

replace('''	if (ok) {
		uint64_t t0 = mach_absolute_time();''', '''	if (ok) {
		if (!fConsolePaused) {
			int quiet = 0;
			fConsolePaused = true;
			if (PE_parse_boot_argn("sweetfb_noconsole", &quiet, sizeof(quiet)) && quiet != 0) {
				sweet_console_muted = 1;
			}
		}
		uint64_t t0 = mach_absolute_time();''')

replace('''		} else if (fStatFrames == 600) {''', '''		} else if (fStatFrames == 300) {''')
replace('fps (600 frames)', 'fps (300 swaps)')
replace('599ULL * 1000000000ULL / all, (5990ULL * 1000000000ULL / all) % 10',
        '299ULL * 1000000000ULL / all, (2990ULL * 1000000000ULL / all) % 10')
replace('conv / 600 / 1000', 'conv / 300 / 1000')

path = 'iokit/Kernel/arm/SweetFramebuffer.cpp'
output.write_text(''.join(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
    fromfile='a/' + path, tofile='b/' + path)), newline='\n')
print(output)
