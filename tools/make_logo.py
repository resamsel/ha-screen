"""Render a Home Assistant style logo to an LVGL 8 RGB565 (byte-swapped) C image."""
import sys
from PIL import Image, ImageDraw

SIZE = 128
SS = 4  # supersampling
S = SIZE * SS
BLUE = (0x18, 0xBC, 0xF2)
WHITE = (255, 255, 255)

img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)

def p(x, y):
    return (x * S, y * S)

# House silhouette with rounded corners (polygon + thick rounded outline)
house = [p(0.5, 0.08), p(0.92, 0.48), p(0.92, 0.92), p(0.08, 0.92), p(0.08, 0.48)]
d.polygon(house, fill=BLUE)
d.line(house + [house[0]], fill=BLUE, width=int(0.08 * S), joint="curve")
for pt in house:
    r = 0.04 * S
    d.ellipse([pt[0] - r, pt[1] - r, pt[0] + r, pt[1] + r], fill=BLUE)

# Circuit tree
w = int(0.055 * S)
def seg(a, b):
    d.line([p(*a), p(*b)], fill=WHITE, width=w)
def node(c, r=0.07):
    x, y = p(*c)
    d.ellipse([x - r * S, y - r * S, x + r * S, y + r * S], fill=WHITE)

seg((0.5, 0.95), (0.5, 0.32))      # trunk
seg((0.5, 0.76), (0.29, 0.56))     # left branch
seg((0.5, 0.64), (0.71, 0.45))     # right branch
node((0.5, 0.31))
node((0.29, 0.56))
node((0.71, 0.45))

img = img.resize((SIZE, SIZE), Image.LANCZOS)
img.save(sys.argv[2])

px = []
for r, g, b, a in img.getdata():
    v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    px += [(v >> 8) & 0xFF, v & 0xFF, a]  # swapped RGB565 (LV_COLOR_16_SWAP=1) + alpha

with open(sys.argv[1], "w") as f:
    f.write('#include "lvgl.h"\n\n')
    f.write("static const uint8_t ha_logo_map[] = {\n")
    for i in range(0, len(px), 24):
        f.write("  " + ",".join(f"0x{v:02x}" for v in px[i:i + 24]) + ",\n")
    f.write("};\n\n")
    f.write("const lv_img_dsc_t ha_logo = {\n")
    f.write("  .header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA,\n  .header.always_zero = 0,\n")
    f.write(f"  .header.w = {SIZE},\n  .header.h = {SIZE},\n")
    f.write(f"  .data_size = {len(px)},\n  .data = ha_logo_map,\n}};\n")
