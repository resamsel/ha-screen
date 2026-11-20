"""Material Design Icons (the icons Home Assistant uses) as one binary blob, embedded in the
firmware (main/mdi_icons.bin) and drawn by name ("mdi:thermometer").

Each icon is SIZE x SIZE pixels, 4 bpp alpha (LV_IMG_CF_ALPHA_4BIT layout: rows of SIZE/2
bytes), coloured by the image recolor style.

Blob layout (little endian):
  u32 magic 'MDI1', u32 count, u32 size
  u32 name_off[count]   offsets into the names area, names sorted (binary search)
  bitmaps               count * SIZE*SIZE/2 bytes, same order as the names
  names                 NUL-terminated names without the "mdi:" prefix

Usage (repo root):
  docker run --rm -v "$PWD/.mdi":/w -w /w node:22-alpine sh -c \
    'npm pack @mdi/font@7.4.47 && tar xzf mdi-font-7.4.47.tgz'
  python3 tools/make_icons.py
"""
import re
import struct
from PIL import Image, ImageDraw, ImageFont

PKG = ".mdi/package"
OUT = "main/mdi_icons.bin"
SIZE = 24

css = open(f"{PKG}/css/materialdesignicons.css").read()
icons = sorted({(m[0], int(m[1], 16)) for m in
                re.findall(r'\.mdi-([a-z0-9-]+)::before \{\s*content: "\\([0-9A-F]+)";', css)})
font = ImageFont.truetype(f"{PKG}/fonts/materialdesignicons-webfont.ttf", SIZE)

names, bitmaps, offs, off = b"", b"", [], 0
for name, cp in icons:
    img = Image.new("L", (SIZE, SIZE), 0)
    ImageDraw.Draw(img).text((SIZE / 2, SIZE / 2), chr(cp), font=font, fill=255, anchor="mm")
    px = [v >> 4 for v in img.getdata()]
    bitmaps += bytes(px[i] << 4 | px[i + 1] for i in range(0, len(px), 2))
    offs.append(off)
    n = name.encode() + b"\0"
    names += n
    off += len(n)

blob = struct.pack("<4sII", b"MDI1", len(icons), SIZE) + struct.pack(f"<{len(offs)}I", *offs) + bitmaps + names
open(OUT, "wb").write(blob)
print(f"{len(icons)} icons, {len(blob)} bytes -> {OUT}")
