#!/usr/bin/env python3
"""AquaOS QEMU smoke test: boots the ISO headless and takes timed screendumps."""
import os
import socket
import subprocess
import sys
import time

from PIL import Image

ISO = sys.argv[1] if len(sys.argv) > 1 else '/home/z/my-project/aquaos/build/AquaOS.iso'
OUT = sys.argv[2] if len(sys.argv) > 2 else '/home/z/my-project/aquaos/build/shots'
SHOTS = [int(x) for x in sys.argv[3].split(',')] if len(sys.argv) > 3 else [8, 13, 18, 26, 40]

MON = '/tmp/aqua-mon.sock'
SER = '/tmp/aqua-serial.log'
os.makedirs(OUT, exist_ok=True)
for f in (MON, SER):
    try:
        os.unlink(f)
    except FileNotFoundError:
        pass

accel = 'kvm' if os.access('/dev/kvm', os.W_OK) else 'tcg'

# Local toolchain environment (no root install)
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
for t in SHOTS:
    d = t - (time.time() - t0)
    if d > 0:
        time.sleep(d)
    m.send(f'screendump {OUT}/shot_{t:02d}.ppm', wait=2.0)
m.send('quit', wait=1.0)
try:
    p.wait(timeout=15)
except subprocess.TimeoutExpired:
    p.kill()

for t in SHOTS:
    f = f'{OUT}/shot_{t:02d}.ppm'
    if os.path.exists(f):
        Image.open(f).save(f'{OUT}/shot_{t:02d}.png')
        print('PNG:', f'{OUT}/shot_{t:02d}.png')

if os.path.exists(SER):
    with open(SER, 'rb') as fh:
        data = fh.read()[-2500:]
    print('--- serial tail ---')
    print(data.decode(errors='replace'))
print('TEST DONE')
