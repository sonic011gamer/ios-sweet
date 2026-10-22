#!/usr/bin/env python3
"""probe_raw.py CMDS [SECONDS]: send each command byte in turn and count the
raw bytes that arrive for it (debug of the transport), then ask for info."""
import sys
import time

import viewer

cmds = sys.argv[1].encode()
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 20
s = viewer.WindowsSerial(viewer.find_port())
frames = []
try:
    for cmd in cmds:
        s.write(bytes([cmd]))
        got = bytearray()
        t0 = time.monotonic()
        last = t0
        while time.monotonic() - t0 < secs:
            c = s.read(65536)
            if c:
                got.extend(c)
                last = time.monotonic()
            elif time.monotonic() - last > 3:
                break
        line = f'{chr(cmd)}: {len(got)} bytes, last data at {last - t0:.2f} s'
        if len(got) >= 64:
            h = viewer.HEADER.unpack(bytes(got[:64]))
            line += f'; kind {h[2]} seq {h[3]} payload {h[9]} (expected total {64 + h[9]})'
        print(line)
        if cmd == ord('F'):
            frames.append(bytes(got))
    # a short frame against a full one: where is the gap (static screen)?
    full = [f for f in frames if len(f) == 3072064]
    for f in frames:
        if full and len(f) < len(full[0]):
            ref, gap = full[0], len(full[0]) - len(f)
            i = next((k for k in range(64, len(f)) if f[k] != ref[k]), len(f))
            ok = f[i:] == ref[i + gap:]
            print(f'short frame ({gap} missing): pixels first differ at byte {i} '
                  f'(USB packet {i // 512}, 16K dTD {i // 16384}); rest equals the full frame '
                  f'shifted by the gap: {ok}')
    s.write(b'I')
    p = viewer.read_packet(s.read, 5)
    print(p.payload.decode(errors='replace').strip())
finally:
    s.close()
