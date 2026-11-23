#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# [qa-1] sheet.py OUT REF[@x,y,w,h] MINE[@x,y,w,h] [zoom]
#   Reference crop and our crop side by side (stacked if wide), 1:1 or nearest-zoomed, magenta gap.
#   A crop of "-" means the whole image. Used for build/qa1-cmp/ sheets.
import sys
from PIL import Image

def load(spec):
    path, _, box = spec.partition('@')
    im = Image.open(path).convert('RGB')
    if box:
        x, y, w, h = map(int, box.split(','))
        im = im.crop((x, y, x + w, y + h))
    return im

out, a, b = sys.argv[1], load(sys.argv[2]), load(sys.argv[3])
z = int(sys.argv[4]) if len(sys.argv) > 4 else 1
if z > 1:
    a = a.resize((a.width * z, a.height * z), Image.NEAREST)
    b = b.resize((b.width * z, b.height * z), Image.NEAREST)
wide = max(a.width, b.width) > 700 and max(a.height, b.height) < max(a.width, b.width)
if wide:
    im = Image.new('RGB', (max(a.width, b.width), a.height + b.height + 6), (255, 0, 255))
    im.paste(a, (0, 0)); im.paste(b, (0, a.height + 6))
else:
    im = Image.new('RGB', (a.width + b.width + 6, max(a.height, b.height)), (255, 0, 255))
    im.paste(a, (0, 0)); im.paste(b, (a.width + 6, 0))
im.save(out)
