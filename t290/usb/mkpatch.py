#!/usr/bin/env python3
"""Generate the USB patch against an XNU tree with patches through0130."""
from pathlib import Path
import difflib
import sys

root, output = map(Path, sys.argv[1:])
here = Path(__file__).resolve().parent
patch = []

def emit(path, before, after):
    patch.extend(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
        fromfile='a/' + path if before else '/dev/null', tofile='b/' + path))

def edit(path, replacements):
    before = (root / path).read_text()
    after = before
    for old, new in replacements:
        assert after.count(old) == 1, (path, old)
        after = after.replace(old, new)
    emit(path, before, after)

edit('bsd/conf/files', [('bsd/dev/sweet_t290_gpu.c\t\tstandard\n',
    'bsd/dev/sweet_t290_gpu.c\t\tstandard\nbsd/dev/sweet_t290_usb.c\t\tstandard\n')])
edit('bsd/dev/sweet_sdhc.c', [('\tsweet_t290_gpu_init();\n',
    '\tsweet_t290_gpu_init();\n\textern void sweet_t290_usb_start(void);\n\tsweet_t290_usb_start();\n')])
capture = '''
#if defined(SWEET_T290)
static AppleCLCD *t290CaptureFB;
static IOLock *t290CaptureLock;
extern "C" uint32_t sweet_t290_screen_copy(void *, uint32_t, uint32_t *);
extern "C" uint32_t
sweet_t290_screen_copy(void *dst, uint32_t capacity, uint32_t *meta)
{
    AppleCLCD *fb = t290CaptureFB;
    if (!fb || !t290CaptureLock || !dst || !meta) return 0;
    uint64_t bytes = (uint64_t)fb->fPanelRow * fb->fPanelH;
    if (!bytes || bytes > capacity) return 0;
    IOLockLock(t290CaptureLock);
    bcopy((const void *)fb->fPanel, dst, (size_t)bytes);
    meta[0]=fb->fPanelW; meta[1]=fb->fPanelH;
    meta[2]=fb->fPanelRow; meta[3]=fb->fPanelShift;
    IOLockUnlock(t290CaptureLock);
    return (uint32_t)bytes;
}
#endif
'''
edit('iokit/Kernel/arm/SweetFramebuffer.cpp', [
    ('OSDefineMetaClassAndStructors(AppleCLCD, IOMobileFramebuffer);\n',
     'OSDefineMetaClassAndStructors(AppleCLCD, IOMobileFramebuffer);\n' + capture),
    ('AppleCLCD::t290Setup(void)\n{\n',
     'AppleCLCD::t290Setup(void)\n{\n\tt290CaptureLock = IOLockAlloc();\n\tif (!t290CaptureLock) return false;\n'),
    ('\tfT290 = true;\n', '\tfT290 = true;\n\tt290CaptureFB = this;\n'),
    ('AppleCLCD::t290Convert(uintptr_t pix, uint32_t rowBytes)\n{\n',
     'AppleCLCD::t290Convert(uintptr_t pix, uint32_t rowBytes)\n{\n\tIOLockLock(t290CaptureLock);\n'),
    ('\t\t\tbcopy(fRowBuf, row - fPanelRow + (n - sh), sh);\n\t\t}\n\t}\n}',
     '\t\t\tbcopy(fRowBuf, row - fPanelRow + (n - sh), sh);\n\t\t}\n\t}\n\tIOLockUnlock(t290CaptureLock);\n}'),
    ('\tuint32_t outW = fWidth / 2, outH = fHeight / 2;\n',
     '\tIOLockLock(t290CaptureLock);\n\tuint32_t outW = fWidth / 2, outH = fHeight / 2;\n'),
    ('\t\t\td[2] = (uint8_t)(v >> 8);       /* G */\n\t\t}\n\t}\n}',
     '\t\t\td[2] = (uint8_t)(v >> 8);       /* G */\n\t\t}\n\t}\n\tIOLockUnlock(t290CaptureLock);\n}'),
])
emit('bsd/dev/sweet_t290_usb.c', '', (here / 'sweet_t290_usb.c').read_text())
emit('bsd/dev/sweet_t290_usb_protocol.h', '', (here / 'protocol.h').read_text())
output.write_text(''.join(patch))
print(f'{output}: {len(patch)} lines')
