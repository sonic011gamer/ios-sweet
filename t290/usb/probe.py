#!/usr/bin/env python3
"""probe.py [OUT_DIR]: headless check of the T290 USB link (Windows):
info request, one frame saved as PNG, the newest kernel log text."""
import sys
import time
from pathlib import Path

import viewer

out = Path(sys.argv[1] if len(sys.argv) > 1 else 't290-usb-captures')
out.mkdir(parents=True, exist_ok=True)
port = viewer.find_port()
print('port', port)
s = viewer.WindowsSerial(port)
try:
    for cmd in (b'I', b'F', b'L'):
        t = time.monotonic()
        s.write(cmd)
        p = viewer.read_packet(s.read)
        dt = time.monotonic() - t
        print(f'{cmd.decode()}: kind {p.kind} seq {p.sequence} {len(p.payload)} bytes in {dt:.2f} s')
        if p.kind == 1:
            name = out / f'frame-{time.strftime("%Y%m%d-%H%M%S")}.png'
            viewer.decode_frame(p).save(name)
            print(f'   {p.width}x{p.height} stride {p.stride} shift {p.shift} -> {name}')
        else:
            text = p.payload.decode(errors='replace')
            print('   ' + text[-1500:].replace('\n', '\n   '))
finally:
    s.close()
