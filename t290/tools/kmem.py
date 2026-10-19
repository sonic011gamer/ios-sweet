#!/usr/bin/env python3
"""Inspect a running SWEET kernel through QEMU's gdb stub (no gdb needed).

  kmem.py PORT KERNEL msgbuf [N]    last N lines of the kernel message buffer
  kmem.py PORT KERNEL regs          pc/lr/sp/fp of the stopped vCPU, symbolized
  kmem.py PORT KERNEL bt            frame-pointer backtrace of the stopped vCPU
  kmem.py PORT KERNEL read ADDR LEN hex dump of kernel virtual memory
  kmem.py PORT KERNEL sym NAME      address of a symbol

The guest is stopped on connect and resumed on exit (QEMU: tools/qemu-sweet.py
--gdbport PORT). Addresses are virtual and translated by the vCPU's MMU.
KERNEL is the unstripped kernel (llvm-nm). msgbuf has every printf/IOLog line,
including ones the framebuffer console never drew.
"""
import bisect, socket, struct, subprocess, sys


class Stub:
    def __init__(self, port):
        self.s = socket.create_connection(('127.0.0.1', port), timeout=10)
        self.buf = b''
        self.s.sendall(b'\x03')		# stop the guest
        self.packet()			# stop reply

    def _read(self):
        data = self.s.recv(65536)
        if not data:
            raise EOFError('stub closed')
        self.buf += data

    def packet(self):
        while True:
            while b'$' not in self.buf:
                self._read()
            start = self.buf.index(b'$')
            while b'#' not in self.buf[start:] or len(self.buf) < self.buf.index(b'#', start) + 3:
                self._read()
            end = self.buf.index(b'#', start)
            body = self.buf[start + 1:end]
            self.buf = self.buf[end + 3:]
            self.s.sendall(b'+')
            return body.decode('latin-1')

    def cmd(self, c):
        c = c.encode()
        self.s.sendall(b'$' + c + b'#%02x' % (sum(c) & 0xff))
        while True:
            while not self.buf:
                self._read()
            if self.buf[:1] == b'+':
                self.buf = self.buf[1:]
                break
            if self.buf[:1] == b'$':
                break
            self.buf = self.buf[1:]
        return self.packet()

    def read(self, addr, n):
        out = b''
        while n:
            k = min(n, 2048)
            r = self.cmd(f'm{addr:x},{k:x}')
            if not r or r.startswith('E'):
                raise IOError(f'cannot read 0x{addr:x}: {r}')
            out += bytes.fromhex(r)
            addr += k
            n -= k
        return out

    def u64(self, addr):
        return struct.unpack('<Q', self.read(addr, 8))[0]

    def regs(self):
        g = bytes.fromhex(self.cmd('g'))
        x = list(struct.unpack('<31Q', g[:248]))
        sp, pc = struct.unpack('<QQ', g[248:264])
        return x, sp, pc

    def resume(self):
        c = b'c'
        self.s.sendall(b'$' + c + b'#%02x' % (sum(c) & 0xff))
        self.s.close()


class Syms:
    def __init__(self, kernel):
        rows = []
        for line in subprocess.check_output(['llvm-nm', '-n', '--defined-only', kernel], text=True).splitlines():
            parts = line.split()
            if len(parts) >= 3:
                rows.append((int(parts[0], 16), parts[2]))
        self.addrs = [a for a, _ in rows]
        self.names = [n for _, n in rows]
        self.by_name = {n: a for a, n in rows}

    def __call__(self, addr):
        i = bisect.bisect_right(self.addrs, addr) - 1
        return f'{self.names[i]}+0x{addr - self.addrs[i]:x}' if i >= 0 else '?'


def main():
    port, kernel, what = int(sys.argv[1]), sys.argv[2], sys.argv[3]
    syms = Syms(kernel)
    st = Stub(port)
    try:
        if what == 'msgbuf':
            n = int(sys.argv[4]) if len(sys.argv) > 4 else 80
            mb = st.u64(syms.by_name['_msgbufp'])
            magic, size, bufx, bufr = struct.unpack('<4I', st.read(mb, 16))
            bufc = st.u64(mb + 16)
            data = st.read(bufc, size)
            text = (data[bufx:] + data[:bufx]).replace(b'\0', b'').decode('utf-8', 'replace')
            print(f'msgbuf 0x{mb:x} magic 0x{magic:x} size {size} bufx {bufx}', file=sys.stderr)
            print('\n'.join(text.splitlines()[-n:]))
        elif what in ('regs', 'bt'):
            x, sp, pc = st.regs()
            print(f'pc 0x{pc:x} {syms(pc)}\nlr 0x{x[30]:x} {syms(x[30])}\nsp 0x{sp:x} fp 0x{x[29]:x}')
            if what == 'bt':
                fp = x[29]
                for _ in range(40):
                    if fp == 0 or fp & 7:
                        break
                    try:
                        nfp, lr = struct.unpack('<QQ', st.read(fp, 16))
                    except IOError:
                        break
                    print(f'  0x{lr:x} {syms(lr)}')
                    if nfp <= fp:
                        break
                    fp = nfp
        elif what == 'read':
            addr, n = int(sys.argv[4], 0), int(sys.argv[5], 0)
            data = st.read(addr, n)
            for o in range(0, n, 16):
                print(f'{addr + o:016x}: {data[o:o + 16].hex(" ")}')
        elif what == 'sym':
            print(hex(syms.by_name[sys.argv[4]]))
    finally:
        st.resume()


if __name__ == '__main__':
    main()
