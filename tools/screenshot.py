"""Screenshot of the board's screen over USB (main/screenshot.c).

Usage: python3 tools/screenshot.py [port] [out.png] [--touch] [--page=N]
Needs pyserial and Pillow.
"""
import sys
import time
import serial
from PIL import Image

port = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem14401"
out = sys.argv[2] if len(sys.argv) > 2 else "screenshot.png"
touch = "--touch" in sys.argv          # show what a touch shows (status line, page dots)
page = next((a[7:] for a in sys.argv if a.startswith("--page=")), None)   # switch page first

s = serial.Serial(port, 115200, timeout=5)
s.reset_input_buffer()
if page is not None:
    s.write(page.encode())
    time.sleep(1.0)                    # page slide animation
s.write(b"S" if touch else b"s")
img, area, row = None, None, 0
deadline = time.time() + 30
while time.time() < deadline:
    line = s.readline().decode(errors="replace").strip()
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
        n = (x2 - x1 + 1) * 4
        try:                                   # a log line from another task can cut in
            b = bytes.fromhex(line[:n])
        except ValueError:
            row += 1
            continue
        for i in range(0, len(b) - 1, 2):
            v = b[i] << 8 | b[i + 1]          # LV_COLOR_16_SWAP: high byte first
            r, g, bl = (v >> 11) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3
            img.putpixel((x1 + i // 2, y1 + row), (r | r >> 5, g | g >> 6, bl | bl >> 5))
        row += 1
if not img:
    sys.exit("no screenshot received")
img.save(out)
print("saved", out)
