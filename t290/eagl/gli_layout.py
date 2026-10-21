#!/usr/bin/env python3
"""gli_layout.py GLIDISPATCH_H
Members of struct __GLIFunctionDispatchRec (macOS SDK OpenGL.framework
gliDispatch.h) in order: 'index name | return | params'. The iOS EAGL macro
context (getMacroContextPrivate) is { GLIContext rend; GLIFunctionDispatch
disp; }, so slot i sits at byte 8 + 8*i (QuartzCore: glFinish at 720 = 89)."""
import re
import sys

s = open(sys.argv[1]).read()
s = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
s = re.sub(r'//[^\n]*', '', s)
m = re.search(r'typedef\s+struct\s+__GLIFunctionDispatchRec\s*\{(.*?)\}\s*GLIFunctionDispatch', s, re.S)
body = m.group(1)
for i, mem in enumerate(x.strip() for x in body.split(';') if x.strip()):
    mm = re.match(r'(.*?)\(\s*\*\s*(\w+)\s*\)\s*\((.*)\)\s*$', ' '.join(mem.split()))
    if not mm:
        print(f'{i} ?? {mem}')
        continue
    print(f'{i} {mm.group(2)} | {mm.group(1).strip()} | {mm.group(3).strip()}')
