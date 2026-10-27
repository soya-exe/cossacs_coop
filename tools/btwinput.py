#!/usr/bin/env python3
"""Drive a headless test instance through its private Xwayland (XTEST), no xdotool needed.

usage: tools/btwinput.py <name|:N> <command> [args] [<command> [args] ...]

  <name>  instance name (a, b, ...): the display is read from $BTW_TEST_ROOT/<name>/state/env
  :N      an X display directly (only ever a headless one!)

commands (several can be chained, executed in order):
  move X Y              move the pointer (screen pixels; the game is fullscreen at 0,0)
  click X Y [BTN]       move + press/release (BTN 1 left (default), 2 middle, 3 right)
  dclick X Y            double left click
  down BTN / up BTN     raw button press / release
  key K[+K...] ...      key presses; K is an X keysym name: Return Escape Tab space a F10 ...
                        combos with '+': ctrl+a, alt+F4, shift+Tab
  type TEXT             type ASCII text
  sleep S               wait S seconds (float)
  windows               list mapped top-level windows (id, geometry, name)
  shot OUT.png          capture the mapped top-level windows into a PNG (fallback screenshot)
"""
import os
import sys
import time

from Xlib import X, XK, display
from Xlib.ext import xtest

DELAY = 0.04


def resolve_display(target):
    if target.startswith(':'):
        return target
    root = os.environ.get('BTW_TEST_ROOT', os.path.expanduser('~/btw-test'))
    env = os.path.join(root, target, 'state', 'env')
    try:
        with open(env) as f:
            for line in f:
                if line.startswith('GS_DISPLAY='):
                    return line.split('=', 1)[1].strip()
    except OSError:
        pass
    sys.exit(f'instance {target} not running (no GS_DISPLAY in {env})')


class Driver:
    def __init__(self, disp):
        if disp in (os.environ.get('DISPLAY'), os.environ.get('BTW_REAL_DISPLAY'), ':0'):
            sys.exit(f'refusing to send input to {disp} (looks like the real display)')
        self.d = display.Display(disp)
        if not self.d.has_extension('XTEST'):
            sys.exit('XTEST missing on ' + disp)
        self.root = self.d.screen().root

    def _sync(self):
        self.d.sync()
        time.sleep(DELAY)

    def move(self, x, y):
        xtest.fake_input(self.d, X.MotionNotify, x=int(x), y=int(y))
        self._sync()

    def button(self, btn, press):
        xtest.fake_input(self.d, X.ButtonPress if press else X.ButtonRelease, int(btn))
        self._sync()

    def click(self, x, y, btn=1):
        self.move(x, y)
        time.sleep(0.05)
        self.button(btn, True)
        time.sleep(0.05)
        self.button(btn, False)

    def keycode(self, name):
        sym = XK.string_to_keysym(name)
        if sym == 0 and len(name) == 1:
            sym = ord(name)
        kc = self.d.keysym_to_keycode(sym)
        if not kc:
            sys.exit(f'unknown key {name!r}')
        return kc

    def combo(self, spec):
        alias = {'ctrl': 'Control_L', 'alt': 'Alt_L', 'shift': 'Shift_L', 'super': 'Super_L',
                 'enter': 'Return', 'esc': 'Escape'}
        parts = [alias.get(p.lower(), p) for p in spec.split('+')] if spec != '+' else ['plus']
        codes = [self.keycode(p) for p in parts]
        for c in codes:
            xtest.fake_input(self.d, X.KeyPress, c)
            self._sync()
        for c in reversed(codes):
            xtest.fake_input(self.d, X.KeyRelease, c)
            self._sync()

    def type(self, text):
        shift = self.keycode('Shift_L')
        for ch in text:
            sym = XK.string_to_keysym(ch) or ord(ch)
            if ch == ' ':
                sym = XK.string_to_keysym('space')
            kc = self.d.keysym_to_keycode(sym)
            if not kc:
                continue
            need_shift = self.d.keycode_to_keysym(kc, 0) != sym
            if need_shift:
                xtest.fake_input(self.d, X.KeyPress, shift)
            xtest.fake_input(self.d, X.KeyPress, kc)
            self._sync()
            xtest.fake_input(self.d, X.KeyRelease, kc)
            if need_shift:
                xtest.fake_input(self.d, X.KeyRelease, shift)
            self._sync()

    def toplevels(self):
        out = []
        for w in self.root.query_tree().children:  # bottom-to-top stacking order
            try:
                a = w.get_attributes()
                if a.map_state != X.IsViewable:
                    continue
                g = w.get_geometry()
                if g.width <= 1 or g.height <= 1:
                    continue
                out.append((w, g, w.get_wm_name()))
            except Exception:
                continue
        return out

    def windows(self):
        for w, g, name in self.toplevels():
            print(f'{w.id:#x} {g.width}x{g.height}+{g.x}+{g.y} {name!r}')

    def shot(self, path):
        from PIL import Image
        s = self.d.screen()
        canvas = Image.new('RGB', (s.width_in_pixels, s.height_in_pixels))
        for w, g, _ in self.toplevels():
            try:
                raw = w.get_image(0, 0, g.width, g.height, X.ZPixmap, 0xffffffff)
                im = Image.frombytes('RGB', (g.width, g.height), raw.data, 'raw', 'BGRX')
                canvas.paste(im, (g.x, g.y))
            except Exception as e:
                print(f'skip {w.id:#x}: {e}', file=sys.stderr)
        canvas.save(path)
        print(path)


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    drv = Driver(resolve_display(argv[1]))
    args = argv[2:]
    i = 0
    while i < len(args):
        cmd = args[i]
        i += 1
        if cmd == 'move':
            drv.move(args[i], args[i + 1]); i += 2
        elif cmd == 'click':
            btn = 1
            x, y = args[i], args[i + 1]; i += 2
            if i < len(args) and args[i] in ('1', '2', '3'):
                btn = int(args[i]); i += 1
            drv.click(x, y, btn)
        elif cmd == 'dclick':
            drv.click(args[i], args[i + 1]); time.sleep(0.08); drv.click(args[i], args[i + 1]); i += 2
        elif cmd in ('down', 'up'):
            drv.button(args[i], cmd == 'down'); i += 1
        elif cmd == 'key':
            while i < len(args) and args[i] not in COMMANDS:
                drv.combo(args[i]); i += 1
        elif cmd == 'type':
            drv.type(args[i]); i += 1
        elif cmd == 'sleep':
            time.sleep(float(args[i])); i += 1
        elif cmd == 'windows':
            drv.windows()
        elif cmd == 'shot':
            drv.shot(args[i]); i += 1
        else:
            sys.exit(f'unknown command {cmd!r}')
    return 0


COMMANDS = {'move', 'click', 'dclick', 'down', 'up', 'key', 'type', 'sleep', 'windows', 'shot'}

if __name__ == '__main__':
    sys.exit(main(sys.argv))
