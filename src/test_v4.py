#!/usr/bin/env python3
"""AquaOS v4 final test: one fresh QEMU per click stage (HMP input stream
proves unreliable for long multi-stage sessions under TCG, so each stage
gets its own clean VM). Stages:
  1 boot/desktop   2 open terminal    3 typing ls /
  4 typing echo    5 yellow/minimize  6 icon/restore
  7 green zoom+unzoom + red close     8 menu open + About
"""
import os, socket, subprocess, sys, time

ROOT = '/home/z/my-project/aquaos'
P = os.path.join(ROOT, 'tools/prefix')
OUT = os.path.join(ROOT, 'build/shots')
os.makedirs(OUT, exist_ok=True)

env = dict(os.environ)
env['PATH'] = f'{P}/usr/bin:{P}/bin:' + env.get('PATH', '')
env['LD_LIBRARY_PATH'] = f'{P}/usr/lib/x86_64-linux-gnu:{P}/lib/x86_64-linux-gnu'


class VM:
    def __init__(self):
        for f in ('/tmp/aqua-mon.sock', '/tmp/aqua-serial.log'):
            if os.path.exists(f):
                os.unlink(f)
        self.proc = subprocess.Popen(
            [f'{P}/usr/bin/qemu-system-x86_64',
             '-m', '2048', '-cdrom', os.path.join(ROOT, 'build/AquaOS.iso'),
             '-display', 'none', '-vga', 'std', '-no-reboot', '-accel', 'tcg',
             '-monitor', 'unix:/tmp/aqua-mon.sock,server,nowait',
             '-serial', 'file:/tmp/aqua-serial.log',
             '-L', f'{P}/usr/share/seabios', '-L', f'{P}/usr/share/qemu'],
            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(100):
            if os.path.exists('/tmp/aqua-mon.sock'):
                break
            time.sleep(0.1)
        time.sleep(0.3)
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.connect('/tmp/aqua-mon.sock')
        self.s.settimeout(10)
        time.sleep(0.5)
        try:
            self.s.recv(65536)
        except socket.timeout:
            pass

    def send(self, c, wait=0.3):
        self.s.sendall(c.encode() + b'\n')
        time.sleep(0.12)
        try:
            self.s.recv(65536)
        except socket.timeout:
            pass
        if wait:
            time.sleep(wait)

    def move(self, dx, dy, wait=0.1):
        n = max(1, (max(abs(dx), abs(dy)) + 99) // 100)
        for _ in range(n):
            self.send(f'mouse_move {dx // n} {dy // n}', wait=0.08)
        if wait:
            time.sleep(wait)

    def move_to(self, tx, ty):
        for _ in range(12):
            self.move(-255, -255, wait=0.12)
        self.move(tx, ty, wait=0.4)

    def click(self):
        self.send('mouse_button 1', wait=0.2)
        self.send('mouse_button 0', wait=0.4)

    def key(self, k, wait=0.18):
        self.send(f'sendkey {k}', wait=wait)

    def shot(self, name):
        p = os.path.join(OUT, name + '.ppm')
        self.send(f'screendump {p}', wait=1.0)
        from PIL import Image
        if os.path.exists(p):
            Image.open(p).save(os.path.join(OUT, name + '.png'))
            os.unlink(p)
            print('PNG:', name)

    def kill(self):
        self.send('quit', wait=0.5)
        time.sleep(0.7)
        if self.proc.poll() is None:
            self.proc.kill()
        self.proc.wait()


def boot():
    vm = VM()
    time.sleep(13)                     # boot + crossfade
    return vm


STAGES = [
    ('01_boot', 2.0, lambda vm: vm.shot('01_boot')),
    ('02_desktop', 9.0, lambda vm: vm.shot('02_desktop')),
    # 3: hover + open terminal
    ('04_terminal_open', 0.0, lambda vm: (vm.move_to(959, 1010),
                                          vm.shot('03_dock_magnify'),
                                          vm.click(),
                                          time.sleep(1.3),
                                          vm.shot('04_terminal_open'))),
    # 4: typing ls /
    ('05_terminal_ls', 0.0, lambda vm: (vm.move_to(700, 300),
                                        [vm.key(k) for k in 'ls'],
                                        vm.key('spc'), vm.key('slash'),
                                        vm.key('ret'), time.sleep(1.2),
                                        vm.shot('05_terminal_ls'))),
    # 5: typing echo _aquaos
    ('06_terminal_echo', 0.0, lambda vm: ([vm.key(k) for k in 'echo'],
                                          vm.key('spc'), vm.key('shift-minus'),
                                          [vm.key(k) for k in 'aquaos'],
                                          vm.key('ret'), time.sleep(0.8),
                                          vm.shot('06_terminal_echo'))),
    # 6: yellow -> minimize, icon -> restore
    ('08_restored', 0.0, lambda vm: (vm.move_to(959, 1010), vm.click(),
                                     time.sleep(1.2),
                                     vm.move_to(577, 117), vm.click(),
                                     time.sleep(0.9), vm.shot('07_minimized'),
                                     vm.move_to(959, 1010), vm.click(),
                                     time.sleep(1.0), vm.shot('08_restored'))),
    # 7: green zoom, green unzoom, red close
    ('11_closed', 0.0, lambda vm: (vm.move_to(608, 117), vm.click(),
                                   time.sleep(1.0), vm.shot('09_zoomed'),
                                   vm.click(), time.sleep(1.0),
                                   vm.shot('10_unzoomed'),
                                   vm.move_to(546, 117), vm.click(),
                                   time.sleep(1.0), vm.shot('11_closed'))),
    # 8: menu open + About click closes
    ('13_menu_closed', 0.0, lambda vm: (vm.move_to(85, 27), vm.click(),
                                        time.sleep(0.6),
                                        vm.shot('12_menu_open'),
                                        vm.move_to(103, 89), vm.click(),
                                        time.sleep(0.6),
                                        vm.shot('13_menu_closed'))),
]

results = []
for name, pre, action in STAGES:
    vm = None
    try:
        vm = boot()
        if pre:
            time.sleep(pre)
        action(vm)
        results.append((name, 'OK'))
        print('STAGE OK:', name)
    except Exception as e:
        results.append((name, f'ERR {e}'))
        print('STAGE ERR:', name, e)
    finally:
        if vm:
            vm.kill()

print('=== SUMMARY ===')
for n, r in results:
    print(f'{n}: {r}')
print('ALL DONE')
