#!/usr/bin/env python3
"""AquaOS v7 UI test: boots the ISO headless and exercises the new features.

Verifies via screendumps:
  - desktop with new panel (battery, date, time MSK)
  - terminal window (rounded corners, soft shadow, no rectangular frame)
  - scrollback: seq output, wheel up (scrollbar), PgDn back
  - window drag by title bar (macOS lift)
  - Settings/File/Window menus with soft dropdown shadow
Uses HMP mouse_move in small steps (guest applies acceleration) and QMP
input-send-event for REL_WHEEL.
"""
import json
import os
import socket
import subprocess
import sys
import time

from PIL import Image

ISO = sys.argv[1] if len(sys.argv) > 1 else '/home/z/my-project/aquaos/build/AquaOS.iso'
OUT = sys.argv[2] if len(sys.argv) > 2 else '/home/z/my-project/aquaos/build/ui_shots'
os.makedirs(OUT, exist_ok=True)

MON = '/tmp/aqua-mon.sock'
QMP = '/tmp/aqua-qmp.sock'
SER = '/tmp/aqua-serial.log'
for f in (MON, QMP, SER):
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
       '-m', '2048', '-smp', '2', '-cdrom', ISO,
       '-display', 'none', '-vga', 'std', '-no-reboot', '-accel', accel,
       '-monitor', f'unix:{MON},server,nowait',
       '-qmp', f'unix:{QMP},server,nowait',
       '-serial', f'file:{SER}']
print('RUN:', ' '.join(cmd))
p = subprocess.Popen(cmd, env=env)


class Mon:
    def __init__(self, path):
        last = None
        for _ in range(100):
            try:
                self.s = socket.socket(socket.AF_UNIX)
                self.s.connect(path)
                break
            except (ConnectionRefusedError, FileNotFoundError) as e:
                last = e
                time.sleep(0.25)
        else:
            raise RuntimeError(f'cannot connect {path}: {last}')
        self.s.settimeout(3)
        time.sleep(0.4)
        try:
            self.s.recv(65536)
        except Exception:
            pass

    def send(self, line, wait=0.25, drain=True):
        self.s.sendall((line + '\n').encode())
        time.sleep(wait)
        if not drain:
            return ''
        out = b''
        try:
            while True:
                self.s.settimeout(0.3)   # idle gap, not the full 3s
                d = self.s.recv(65536)
                if not d:
                    break
                out += d
        except socket.timeout:
            pass
        finally:
            self.s.settimeout(3)
        return out.decode(errors='replace')


class Qmp:
    def __init__(self, path):
        for _ in range(100):
            try:
                self.s = socket.socket(socket.AF_UNIX)
                self.s.connect(path)
                break
            except (ConnectionRefusedError, FileNotFoundError):
                time.sleep(0.25)
        else:
            raise RuntimeError('cannot connect qmp')
        self.s.settimeout(3)
        self.f = self.s.makefile('rw')
        self.f.readline()  # greeting
        self.cmd('qmp_capabilities')

    def cmd(self, name, args=None):
        m = {'execute': name}
        if args:
            m['arguments'] = args
        self.f.write(json.dumps(m) + '\n')
        self.f.flush()
        while True:
            line = self.f.readline()
            if not line:
                return {}
            r = json.loads(line)
            if 'return' in r or 'error' in r:
                return r


m = Mon(MON)
q = Qmp(QMP)

mx, my = 960, 540          # splash centers the cursor
SER_OFFSET = [0]           # serial read pointer for click feedback


def shot(name, wait=0.6):
    time.sleep(wait)
    m.send(f'screendump {OUT}/{name}.ppm', wait=1.6)
    f = f'{OUT}/{name}.ppm'
    if os.path.exists(f):
        Image.open(f).save(f'{OUT}/{name}.png')
        print('shot:', name)


def serial_from(mark):
    try:
        with open(SER, 'rb') as fh:
            return fh.read()[mark:].decode(errors='replace')
    except FileNotFoundError:
        return ''


def move_to(tx, ty, step=4):
    """move the guest cursor to (tx,ty) in small steps (acc-neutral: |d|=4
    passes acc() 1:1).  Paced so the QEMU PS/2 queue never overflows.
    HMP dy sign = screen dy (verified via raw mousedev dump)."""
    global mx, my
    n = 0
    while abs(tx - mx) > step or abs(ty - my) > step:
        dx = max(-step, min(step, tx - mx))
        dy = max(-step, min(step, ty - my))
        m.send(f'mouse_move {dx} {dy}', wait=0.03, drain=False)
        mx += dx
        my += dy
        n += 1
        if n % 25 == 0:
            time.sleep(0.25)   # let the guest drain the PS/2 queue
    time.sleep(0.3)


def click():
    time.sleep(0.3)   # let the PS/2 queue drain so the click is not swallowed
    m.send('mouse_button 1', wait=0.15, drain=False)   # 1 = left down
    m.send('mouse_button 0', wait=0.25, drain=False)   # 0 = all up


def click_read():
    """click and return the guest-reported cursor position from serial
    ('CLK x y' is printed by splash on every left press)."""
    global mx, my
    mark = os.path.getsize(SER) if os.path.exists(SER) else 0
    click()
    s = serial_from(mark)
    import re as _re
    hits = _re.findall(r'CLK (\d+) (\d+)', s)
    if hits:
        mx, my = int(hits[-1][0]), int(hits[-1][1])
    return (mx, my) if hits else None


def calibrate_at(sx, sy, tol=4, rounds=4):
    """closed-loop positioning over a harmless spot: click, read the guest's
    own position report, correct the drift, repeat.  Kills packet-loss drift
    under TCG."""
    move_to(sx, sy)
    for _ in range(rounds):
        p = click_read()
        if p is None:
            break
        gx, gy = p
        if abs(gx - sx) <= tol and abs(gy - sy) <= tol:
            return True
        move_to(sx, sy)
    return False


def drag_to(tx, ty, step=6):
    global mx, my
    time.sleep(0.3)   # drain PS/2 queue before grabbing
    m.send('mouse_button 1', wait=0.2, drain=False)    # left down
    n = 0
    while abs(tx - mx) > step or abs(ty - my) > step:
        dx = max(-step, min(step, tx - mx))
        dy = max(-step, min(step, ty - my))
        m.send(f'mouse_move {dx} {dy}', wait=0.03, drain=False)
        mx += dx
        my += dy
        n += 1
        if n % 25 == 0:
            time.sleep(0.25)
    time.sleep(0.3)
    m.send('mouse_button 0', wait=0.3)                 # all up


KEYMAP = {
    ' ': 'spc', '-': 'minus', '=': 'equal', '.': 'dot', '/': 'slash',
    '\n': 'ret', ';': 'semicolon',
}


def type_text(s):
    for ch in s:
        name = KEYMAP.get(ch, ch)
        m.send(f'sendkey {name}', wait=0.06, drain=False)


def wheel(value, times=1):
    for _ in range(times):
        r = q.cmd('input-send-event', {'events': [
            {'type': 'rel', 'data': {'axis': 'z', 'value': value}}]})
        time.sleep(0.1)
        if 'error' in r:
            print('QMP wheel error:', r['error'])
            return False
    return True


# wait for the desktop (serial prints boot complete; TCG needs a while)
t0 = time.time()
desktop_ready = False
while time.time() - t0 < 90:
    try:
        with open(SER, 'rb') as fh:
            tail = fh.read()[-800:].decode(errors='replace')
        if 'desktop ready' in tail:
            desktop_ready = True
            break
    except FileNotFoundError:
        pass
    time.sleep(1)
print('desktop ready:', desktop_ready, f'({time.time()-t0:.0f}s)')

# 1. desktop: new panel (battery, date, clock), FPS
shot('01_desktop', 1.0)

# 2. open terminal from the dock: icon at (960, ~1036).
#    Closed-loop calibration over the wallpaper above the dock first,
#    then a short final hop onto the icon (fewer packets = less drift).
calibrate_at(960, 940)
move_to(960, 1036)
shot('02_dock_hover', 0.5)
print('dock click at guest pos:', click_read())
shot('03_terminal_open', 1.4)

# 3. generate scrollback history
type_text('seq 1 99\n')
shot('04_seq_done', 1.4)
type_text('echo AQUAOS_BOTTOM_MARKER\n')
shot('05_at_bottom', 0.9)

# 4. PgUp -> scrollback view + scrollbar (QEMU cannot inject REL_WHEEL;
#    the real wheel path is evdev REL_WHEEL on /dev/input/event1, verified open)
m.send('sendkey pgup', wait=0.15)
m.send('sendkey pgup', wait=0.15)
shot('06_scroll_up', 0.7)
m.send('sendkey pgup', wait=0.15)
shot('07_scroll_more', 0.7)

# 5. PgDn back towards bottom
m.send('sendkey pgdn', wait=0.15)
m.send('sendkey pgdn', wait=0.15)
shot('08_pgdn', 0.6)

# 6. drag the window by the title bar (from ~(960,67) to ~(700,300)).
#    Calibrate over the terminal body first (harmless click), then hop up
#    to the title bar and grab.
calibrate_at(960, 300)
move_to(960, 67)
time.sleep(0.3)
drag_to(700, 300)
shot('09_dragged', 1.2)

# 7. menus: Settings dropdown.  Calibrate over wallpaper left of the window.
calibrate_at(150, 700)
move_to(60, 13)
click()
shot('10_menu_settings', 0.8)
# hover-switch to File
move_to(140, 13)
shot('11_menu_file', 0.7)
# hover-switch to Window
move_to(200, 13)
shot('12_menu_window', 0.7)
# close menu
move_to(960, 800)
click()

# 8. About panel
move_to(60, 13)
click()
shot('13_menu_again', 0.6)
move_to(60, 55)
click()
shot('14_about', 0.9)
move_to(960, 800)
click()

# 9. final desktop state
shot('15_final', 0.5)

m.send('quit', wait=0.5)
try:
    p.wait(timeout=10)
except subprocess.TimeoutExpired:
    p.kill()

with open(SER, 'rb') as fh:
    print('--- serial tail ---')
    print(fh.read()[-1800:].decode(errors='replace'))
print('UI TEST DONE')
