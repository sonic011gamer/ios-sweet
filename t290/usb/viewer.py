#!/usr/bin/env python3
"""Live native-panel pixels and kernel/GL diagnostics over T290 CDC ACM.

Windows uses the built-in usbser driver, with no pyserial dependency.
Only F(frame), L(log) and I(info) requests exist; this transport cannot
write files, execute commands or access arbitrary device memory.
"""
import argparse
import ctypes
from ctypes import wintypes
from dataclasses import dataclass
import json
from pathlib import Path
import queue
import struct
import threading
import time

HEADER = struct.Struct('<8s8I3Q')
MAGIC = b'T290USB1'
MAX_BYTES = 800 * 1280 * 3
RBG3 = 0x52424733

@dataclass
class Packet:
    kind: int
    sequence: int
    width: int
    height: int
    stride: int
    format: int
    shift: int
    nanoseconds: int
    payload: bytes
    wire: bytes

def read_packet(read, deadline=15):
    end = time.monotonic() + deadline
    buffered = bytearray()
    def exact(n):
        while len(buffered) < n:
            if time.monotonic() > end:
                raise TimeoutError('USB response incomplete')
            # Avoid header-only host reads: this usbser link returned
            # incomplete frames with them. Large reads gave full frames;
            # parse the header from our own buffered bytes.
            chunk = read(MAX_BYTES + HEADER.size)
            if not chunk:
                continue
            buffered.extend(chunk)
            if len(buffered) > MAX_BYTES + HEADER.size:
                raise ValueError('USB response exceeds the packet limit')
        out = bytes(buffered[:n])
        del buffered[:n]
        return out
    raw = exact(HEADER.size)
    magic, version, kind, seq, width, height, stride, fmt, shift, n, ns, reserved = HEADER.unpack(raw)
    if magic != MAGIC or version != 1 or kind not in (1, 2, 3, 4) or reserved:
        raise ValueError('invalid USB protocol header')
    if n > MAX_BYTES:
        raise ValueError('USB payload exceeds limit')
    if kind == 1 and (not width or not height or width > 800 or height > 1280 or
                      stride < width * 3 or stride > 800 * 3 or n != stride * height or
                      fmt != RBG3 or shift >= width):
        raise ValueError('invalid native-panel layout')
    if kind in (2, 3, 4) and n > 65536:
        raise ValueError('text response exceeds limit')
    payload = exact(n)
    return Packet(kind, seq, width, height, stride, fmt, shift, ns, payload, raw + payload)

def decode_frame(packet):
    from PIL import Image
    if packet.kind != 1:
        raise ValueError('packet is not an image')
    packed = b''.join(packet.payload[y * packet.stride:y * packet.stride + packet.width * 3]
                      for y in range(packet.height))
    # Actual panel: logical pixel i >= shift lives at buffer i - shift.
    # The prefix before the mapped framebuffer is unavailable; show black.
    n = packet.shift * 3
    if n:
        packed = bytes(n) + packed[:-n]
    raw = Image.frombytes('RGB', (packet.width, packet.height), packed)
    red, blue, green = raw.split()
    return Image.merge('RGB', (red, green, blue))

def find_port():
    import winreg
    root = r'SYSTEM\CurrentControlSet\Enum\USB'
    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, root) as usb:
        for i in range(winreg.QueryInfoKey(usb)[0]):
            name = winreg.EnumKey(usb, i)
            if not name.upper().startswith('VID_1209&PID_0001'):
                continue
            with winreg.OpenKey(usb, name) as device:
                for j in range(winreg.QueryInfoKey(device)[0]):
                    instance = winreg.EnumKey(device, j)
                    try:
                        with winreg.OpenKey(device, instance + r'\Device Parameters') as params:
                            port = winreg.QueryValueEx(params, 'PortName')[0]
                            if str(port).upper().startswith('COM'):
                                return port
                    except FileNotFoundError:
                        pass
    return None

class WindowsSerial:
    def __init__(self, port):
        self.api = ctypes.WinDLL('kernel32', use_last_error=True)
        a = self.api
        a.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                 wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        a.CreateFileW.restype = wintypes.HANDLE
        a.ReadFile.argtypes = a.WriteFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
                                                     ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
        a.CloseHandle.argtypes = [wintypes.HANDLE]
        a.PurgeComm.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        a.SetCommState.argtypes = [wintypes.HANDLE, ctypes.c_void_p]
        a.SetCommTimeouts.argtypes = [wintypes.HANDLE, ctypes.c_void_p]
        a.SetupComm.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD]
        a.EscapeCommFunction.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        a.BuildCommDCBW.argtypes = [wintypes.LPCWSTR, ctypes.c_void_p]
        device_path = chr(92) * 2 + '.' + chr(92) + port
        self.handle = a.CreateFileW(device_path, 0xc0000000, 0, None, 3, 0, None)
        if self.handle == ctypes.c_void_p(-1).value:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            # A native frame is 3 MB. Keep it queued while Python parses
            # the header; the small default usbser queue can lose packets.
            self.check(a.SetupComm(self.handle, MAX_BYTES + HEADER.size + 65536, 4096))
            dcb = ctypes.create_string_buffer(28)
            struct.pack_into('<I', dcb, 0, 28)
            self.check(a.BuildCommDCBW('baud=115200 parity=n data=8 stop=1', dcb))
            self.check(a.SetCommState(self.handle, dcb))
            timeouts = (wintypes.DWORD * 5)(50, 0, 1000, 0, 1000)
            self.check(a.SetCommTimeouts(self.handle, timeouts))
            self.check(a.PurgeComm(self.handle, 15))
            self.check(a.EscapeCommFunction(self.handle, 5))  # SETDTR
        except BaseException:
            self.close()
            raise
    @staticmethod
    def check(ok):
        if not ok:
            raise ctypes.WinError(ctypes.get_last_error())
    def read(self, count):
        buf = ctypes.create_string_buffer(count)
        n = wintypes.DWORD()
        self.check(self.api.ReadFile(self.handle, buf, count, ctypes.byref(n), None))
        return buf.raw[:n.value]
    def write(self, data):
        n = wintypes.DWORD()
        self.check(self.api.WriteFile(self.handle, data, len(data), ctypes.byref(n), None))
        if n.value != len(data):
            raise IOError('short USB command write')
    def close(self):
        if self.handle is not None:
            self.api.CloseHandle(self.handle)
            self.handle = None

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', help='COM port; automatically detect T290 by default')
    parser.add_argument('--fps', type=float, default=2)
    parser.add_argument('--output', type=Path, default=Path('t290-usb-captures'))
    args = parser.parse_args()
    if not 0.1 <= args.fps <= 10:
        parser.error('--fps must be between 0.1 and 10')
    import tkinter as tk
    from tkinter import ttk
    from PIL import Image, ImageTk
    args.output.mkdir(parents=True, exist_ok=True)
    app = tk.Tk()
    app.title('T290 — экран iPadOS по USB')
    app.geometry('1000x900')
    status = tk.StringVar(value='Ожидание USB-интерфейса T290 Screen Debug…')
    ttk.Label(app, textvariable=status).pack(fill='x')
    canvas = tk.Canvas(app, bg='#17191d', highlightthickness=0)
    canvas.pack(fill='both', expand=True)
    logs = tk.Text(app, height=9, wrap='word')
    logs.pack(fill='x')
    logs.tag_config('error', foreground='#d02020')
    stop = threading.Event()
    events = queue.Queue()
    frames = queue.Queue(maxsize=1)
    latest = [None, None]
    def save():
        packet, im = latest
        if packet is None:
            return
        stem = args.output / f'frame-{time.strftime("%Y%m%d-%H%M%S")}-{packet.sequence}'
        im.save(stem.with_suffix('.png'))
        stem.with_suffix('.t290').write_bytes(packet.wire)
        stem.with_suffix('.json').write_text(json.dumps({
            k: getattr(packet, k) for k in ('sequence', 'width', 'height', 'stride', 'format', 'shift', 'nanoseconds')
        }, indent=2))
        status.set(f'Сохранено: {stem.with_suffix(".png")}')
    ttk.Button(app, text='Сохранить PNG и исходный кадр', command=save).pack(fill='x')
    def worker():
        while not stop.is_set():
            connection = None
            try:
                port = args.port or find_port()
                if not port:
                    stop.wait(1)
                    continue
                connection = WindowsSerial(port)
                events.put(('status', f'{port}: подключён, запрос кадров'))
                connection.write(b'I\n')
                info = read_packet(connection.read)
                events.put(('log', info.payload.decode(errors='replace')))
                log_path = args.output / f'usb-{time.strftime("%Y%m%d-%H%M%S")}.log'
                with log_path.open('a', encoding='utf-8') as log:
                    while not stop.is_set():
                        started = time.monotonic()
                        connection.write(b'F\n')
                        packet = read_packet(connection.read)
                        if packet.kind == 1:
                            im = decode_frame(packet)
                            if frames.full():
                                try: frames.get_nowait()
                                except queue.Empty: pass
                            frames.put((packet, im))
                        else:
                            events.put(('log', packet.payload.decode(errors='replace')))
                        connection.write(b'L\n')
                        log_packet = read_packet(connection.read)
                        message = log_packet.payload.decode(errors='replace')
                        if message:
                            log.write(message)
                            log.flush()
                            events.put(('log', message))
                        stop.wait(max(0, 1 / args.fps - (time.monotonic() - started)))
            except Exception as exc:
                events.put(('status', f'USB: {exc}. Повторное подключение…'))
                stop.wait(2)
            finally:
                if connection:
                    connection.close()
    def update():
        try:
            while True:
                kind, value = events.get_nowait()
                if kind == 'status': status.set(value)
                else:
                    tag = 'error' if any(s in value.lower() for s in ('gl error', 'timeout', 'panic', 'missing gli')) else ''
                    logs.insert('end', value, tag)
                    if int(logs.index('end-1c').split('.')[0]) > 600:
                        logs.delete('1.0', '300.0')
                    logs.see('end')
        except queue.Empty:
            pass
        try:
            packet, im = frames.get_nowait()
            latest[:] = [packet, im]
            display = im.copy()
            display.thumbnail((max(1, canvas.winfo_width()), max(1, canvas.winfo_height())), Image.Resampling.NEAREST)
            canvas.photo = ImageTk.PhotoImage(display)
            canvas.delete('all')
            canvas.create_image(canvas.winfo_width()//2, canvas.winfo_height()//2, image=canvas.photo)
            status.set(f'Кадр {packet.sequence} · {packet.width}×{packet.height} · {args.fps:g} запросов/с · полный кадр')
        except queue.Empty:
            pass
        if not stop.is_set(): app.after(50, update)
    def close():
        stop.set()
        app.destroy()
    app.protocol('WM_DELETE_WINDOW', close)
    threading.Thread(target=worker, daemon=True).start()
    update()
    app.mainloop()

if __name__ == '__main__':
    main()
