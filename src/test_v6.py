#!/usr/bin/env python3
"""AquaOS v6 final test: one fresh QEMU per click stage (HMP input stream
proves unreliable for long multi-stage sessions under TCG, so each stage
gets its own clean VM). Stages:
  1 boot/desktop (macOS menu bar + clock)
  2 open terminal (rounded corners, baked soft shadow)
  3 SHADOW-ERASE TEST: sweep the cursor through the shadow ring, then
    compare the region pixel-by-pixel with the pristine screenshot
  4 typing ls /
  5 minimize + restore
  6 zoom + unzoom + close
  7 menu open (macOS dropdown w/ shadow) -> About panel -> close
"""
import os, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
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

    def sweep(self, x0, y0, x1, y1, steps=14):
        """move the cursor slowly along a line (small deltas only)"""
        self.move_to(x0, y0)
        self._px, self._py = x0, y0
        for i in range(1, steps + 1):
            px = x0 + (x1 - x0) * i // steps
            py = y0 + (y1 - y0) * i // steps
            self.send(f'mouse_move {min(100, max(-100, px - self._px))} '
                      f'{min(100, max(-100, py - self._py))}', wait=0.06)
            self._px, self._py = px, py
        time.sleep(0.3)

    _px = 0
    _py = 0

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


def region_diff(png_a, png_b, box):
    """mean abs RGB diff of a region between two PNGs (0 = identical)"""
    from PIL import Image, ImageChops, ImageStat
    a = Image.open(png_a).convert('RGB').crop(box)
    b = Image.open(png_b).convert('RGB').crop(box)
    st = ImageStat.Stat(ImageChops.difference(a, b))
    return sum(st.mean) / 3.0


def boot():
    vm = VM()
    time.sleep(13)                     # boot + crossfade
    return vm


results = []

# 1: boot + desktop (menu bar, droplet, Settings+chevron, FPS+clock)
vm = None
try:
    vm = boot()
    vm.shot('01_boot')
    vm.shot('02_desktop')
    results.append(('01-02 desktop', 'OK'))
except Exception as e:
    results.append(('01-02 desktop', f'ERR {e}'))
finally:
    if vm:
        vm.kill()

# 2: open terminal (rounded + shadow)
vm = None
try:
    vm = boot()
    vm.move_to(959, 1010)
    vm.shot('03_dock_magnify')
    vm.click()
    time.sleep(1.4)
    vm.shot('04_terminal_open')
    results.append(('04 terminal open', 'OK'))
except Exception as e:
    results.append(('04 terminal open', f'ERR {e}'))
finally:
    if vm:
        vm.kill()

# 3: SHADOW-ERASE TEST (window is at ~(537,52) 845x583 at 1080p)
vm = None
try:
    vm = boot()
    vm.move_to(959, 1010)
    vm.click()
    time.sleep(1.4)
    vm.move_to(300, 400)               # cursor away, left of the window
    time.sleep(0.5)
    vm.shot('05a_shadow_before')
    # sweep right through the shadow band ABOVE the window (y ~ 30..52)
    vm.sweep(420, 38, 1050, 38, steps=16)
    vm.shot('05b_shadow_cursor_in')
    # sweep down through the left shadow band (x 515..537), then away
    vm.sweep(525, 60, 525, 500, steps=12)
    vm.move_to(300, 400)
    time.sleep(0.5)
    vm.shot('05c_shadow_after')
    d1 = region_diff(os.path.join(OUT, '05a_shadow_before.png'),
                     os.path.join(OUT, '05c_shadow_after.png'),
                     (400, 24, 1120, 51))     # top band incl. shadow
    d2 = region_diff(os.path.join(OUT, '05a_shadow_before.png'),
                     os.path.join(OUT, '05c_shadow_after.png'),
                     (508, 55, 542, 520))     # left band incl. shadow
    print(f'SHADOW DIFF top={d1:.3f} left={d2:.3f} (must be ~0)')
    ok = d1 < 1.0 and d2 < 1.0
    results.append(('05 shadow erase', 'OK' if ok else f'FAIL d={d1:.2f}/{d2:.2f}'))
except Exception as e:
    results.append(('05 shadow erase', f'ERR {e}'))
finally:
    if vm:
        vm.kill()

# 4: typing ls /
vm = None
try:
    vm = boot()
    vm.move_to(959, 1010)
    vm.click()
    time.sleep(1.3)
    vm.move_to(700, 300)
    for k in 'ls':
        vm.key(k)
    vm.key('spc')
    vm.key('slash')
    vm.key('ret')
    time.sleep(1.2)
    vm.shot('06_terminal_ls')
    results.append(('06 ls', 'OK'))
except Exception as e:
    results.append(('06 ls', f'ERR {e}'))
finally:
    if vm:
        vm.kill()

# 5: minimize + restore
vm = None
try:
    vm = boot()
    vm.move_to(959, 1010)
    vm.click()
    time.sleep(1.3)
    vm.move_to(577, 67)
    vm.click()
    time.sleep(0.9)
    vm.shot('07_minimized')
    vm.move_to(959, 1010)
    vm.click()
    time.sleep(1.1)
    vm.shot('08_restored')
    results.append(('07-08 min/restore', 'OK'))
except Exception as e:
    results.append(('07-08 min/restore', f'ERR {e}'))
finally:
    if vm:
        vm.kill()

# 6: zoom + unzoom + close
vm = None
try:
    vm = boot()
    vm.move_to(959, 1010)
    vm.click()
    time.sleep(1.3)
    vm.move_to(597, 67)
    vm.click()
    time.sleep(1.0)
    vm.shot('09_zoomed')
    vm.click()
    time.sleep(1.0)
    vm.shot('10_unzoomed')
    vm.move_to(537, 67)
    vm.click()
    time.sleep(1.0)
    vm.shot('11_closed')
    results.append(('09-11 zoom/close', 'OK'))
except Exception as e:
    results.append(('09-11 zoom/close', f'ERR {e}'))
finally:
    if vm:
        vm.kill()

# 7: menu open -> About panel -> click away
vm = None
try:
    vm = boot()
    vm.move_to(60, 14)
    vm.click()
    time.sleep(0.7)
    vm.shot('12_menu_open')
    vm.move_to(90, 49)
    time.sleep(0.3)
    vm.shot('13_menu_hover')
    vm.click()
    time.sleep(0.7)
    vm.shot('14_about_open')
    vm.move_to(1500, 900)
    vm.click()
    time.sleep(0.6)
    vm.shot('15_about_closed')
    results.append(('12-15 menu/about', 'OK'))
except Exception as e:
    results.append(('12-15 menu/about', f'ERR {e}'))
finally:
    if vm:
        vm.kill()

print('=== SUMMARY ===')
fails = 0
for n, r in results:
    print(f'{n}: {r}')
    if not r.startswith('OK'):
        fails += 1
print(f'ALL DONE, fails={fails}')
sys.exit(1 if fails else 0)
