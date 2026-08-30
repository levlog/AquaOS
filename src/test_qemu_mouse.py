#!/usr/bin/env python3
"""AquaOS interactive QEMU test: boots headless, drives the PS/2 mouse via the
HMP monitor and screendumps every UI state (desktop, menu open, About hover,
menu closed)."""
import os
import socket
import subprocess
import sys
import time

from PIL import Image

ISO = sys.argv[1] if len(sys.argv) > 1 else '/home/z/my-project/aquaos/build/AquaOS.iso'
OUT = sys.argv[2] if len(sys.argv) > 2 else '/home/z/my-project/aquaos/build/shots'

MON = '/tmp/aqua-mon.sock'
SER = '/tmp/aqua-serial.log'
os.makedirs(OUT, exist_ok=True)
for f in (MON, SER):
    try:
        os.unlink(f)
    except FileNotFoundError:
        pass

accel = 'kvm' if os.access('/dev/kvm', os.W_OK) else 'tcg'

TOOLS = '/home/z/my-project/aquaos/tools'
env = dict(os.environ)
env['PATH'] = f'{TOOLS}/prefix/usr/bin:{TOOLS}/prefix/usr/sbin:{TOOLS}/prefix/bin:' + env.get('PATH', '')
env['LD_LIBRARY_PATH'] = (f'{TOOLS}/prefix/usr/lib/x86_64-linux-gnu:'
                          f'{TOOLS}/prefix/lib/x86_64-linux-gnu:').rstrip(':')
QEMU = f'{TOOLS}/prefix/usr/bin/qemu-system-x86_64'

cmd = [QEMU, '-L', f'{TOOLS}/prefix/usr/share/seabios', '-L', f'{TOOLS}/prefix/usr/share/qemu',
       '-m', '2048', '-cdrom', ISO,
       '-display', 'none', '-vga', 'std', '-no-reboot', '-accel', accel,
       '-monitor', f'unix:{MON},server,nowait', '-serial', f'file:{SER}']
print('RUN:', ' '.join(cmd))
p = subprocess.Popen(cmd, env=env)


class Mon:
    def __init__(self, path):
        last_err = None
        for _ in range(80):
            try:
                self.s = socket.socket(socket.AF_UNIX)
                self.s.connect(path)
                break
            except (ConnectionRefusedError, FileNotFoundError) as e:
                last_err = e
                time.sleep(0.25)
        else:
            raise RuntimeError(f'cannot connect monitor: {last_err}')
        self.s.settimeout(2)
        time.sleep(0.5)
        try:
            self.s.recv(65536)
        except Exception:
            pass

    def send(self, line, wait=1.0):
        self.s.sendall((line + '\n').encode())
        time.sleep(wait)
        out = b''
        try:
            while True:
                d = self.s.recv(65536)
                if not d:
                    break
                out += d
        except socket.timeout:
            pass
        return out.decode(errors='replace')


m = Mon(MON)
t0 = time.time()

# cursor starts at screen center (512,384).
# target: menu title "Settings >" at (40,16); QEMU clamps |d|<=255 per event,
# so approach in 8 steps of (-59,-46) = (-472,-368).
SEQ = [
    (9.0,  'shot',   '01_boot'),
    (17.0, 'shot',   '02_desktop'),
    (17.5, 'steps',  'mouse_move -59 46', 8),   # HMP Y is screen-inverted
    (18.3, 'shot',   '03_title_hover'),
    (18.8, 'cmd',    'mouse_button 1'),
    (19.0, 'cmd',    'mouse_button 0'),
    (20.5, 'shot',   '04_menu_open'),
    (21.0, 'cmd',    'mouse_move 20 -29'),      # down to "About"
    (22.0, 'shot',   '05_about_hover'),
    (22.5, 'cmd',    'mouse_button 1'),
    (22.7, 'cmd',    'mouse_button 0'),
    (24.5, 'shot',   '06_menu_closed'),
    (26.0, 'shot',   '07_desktop_stable'),
]

for item in SEQ:
    t, kind = item[0], item[1]
    d = t - (time.time() - t0)
    if d > 0:
        time.sleep(d)
    if kind == 'shot':
        m.send(f"screendump {OUT}/{item[2]}.ppm", wait=2.0)
    elif kind == 'cmd':
        m.send(item[2], wait=0.4)
    elif kind == 'steps':
        for _ in range(item[3]):
            m.send(item[2], wait=0.12)

m.send('quit', wait=1.0)
try:
    p.wait(timeout=15)
except subprocess.TimeoutExpired:
    p.kill()

for f in sorted(os.listdir(OUT)):
    if f.endswith('.ppm'):
        Image.open(f'{OUT}/{f}').save(f'{OUT}/{f[:-4]}.png')
        print('PNG:', f'{OUT}/{f[:-4]}.png')

if os.path.exists(SER):
    with open(SER, 'rb') as fh:
        data = fh.read()[-1200:]
    print('--- serial tail ---')
    print(data.decode(errors='replace'))
print('TEST DONE')
