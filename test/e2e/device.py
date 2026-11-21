"""Drives the flashed board over its USB serial port (main/screenshot.c commands).

  s / S        screenshot (S: as if the screen was just touched)
  0..9         show main page n
  V<n>         show dashboard view n
  C<n>         open config panel page n, C9 closes it
  T<x>,<y>\\n   tap at x,y

Everything else the board prints (its log) is kept in Device.log, so tests can check
for crashes.
"""
import time

import serial
from PIL import Image, ImageChops


class Device:
    def __init__(self, port, baud=115200):
        self.s = serial.Serial(port, baud, timeout=5)
        self.log = []

    def close(self):
        self.s.close()

    def _drain(self, secs):
        """Reads log lines for secs seconds."""
        end = time.time() + secs
        while time.time() < end:
            line = self.s.readline().decode(errors="replace").rstrip()
            if line:
                self.log.append(line)

    def send(self, cmd, settle=0.8):
        self.s.write(cmd.encode())
        self._drain(settle)       # animations run ~200 ms

    def page(self, n):
        self.send(str(n))

    def cfg(self, n):
        self.send("C%d" % n)

    def close_cfg(self):
        self.send("C9")

    def tap(self, x, y):
        self.send("T%d,%d\n" % (x, y))

    def screenshot(self, touch=False):
        self.s.write(b"S" if touch else b"s")
        img, area, row = None, None, 0
        deadline = time.time() + 30
        while time.time() < deadline:
            line = self.s.readline().decode(errors="replace").strip()
            if line.startswith("SHOT END"):
                break
            if line.startswith("SHOT "):
                w, h = map(int, line.split()[1:3])
                img = Image.new("RGB", (w, h))
            elif line.startswith("AREA ") and img:
                area = list(map(int, line.split()[1:5]))
                row = 0
            elif area and img and line and all(c in "0123456789abcdef" for c in line[:8]):
                x1, y1, x2, y2 = area
                try:
                    b = bytes.fromhex(line[:(x2 - x1 + 1) * 4])
                except ValueError:
                    row += 1
                    continue
                for i in range(0, len(b) - 1, 2):
                    v = b[i] << 8 | b[i + 1]
                    r, g, bl = (v >> 11) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3
                    img.putpixel((x1 + i // 2, y1 + row), (r | r >> 5, g | g >> 6, bl | bl >> 5))
                row += 1
            elif line:
                self.log.append(line)
        assert img is not None, "no screenshot received"
        return img


def diff_ratio(a, b):
    """Share of pixels that differ noticeably between two screenshots (0..1)."""
    assert a.size == b.size
    d = ImageChops.difference(a, b).convert("L").point(lambda v: 255 if v > 24 else 0)
    return sum(1 for v in d.getdata() if v) / (a.size[0] * a.size[1])
