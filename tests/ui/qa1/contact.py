#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# [qa-1] contact.py OUT REF MINE [REF MINE ...] -> pairs (ref | ours) in rows, for quick visual review of many crops
import sys
from PIL import Image, ImageDraw
def load(spec):
    path, _, box = spec.partition('@')
    im = Image.open(path).convert('RGB')
    if box:
        x, y, w, h = map(int, box.split(',')); im = im.crop((x, y, x + w, y + h))
    return im
pairs = [(load(a), load(b)) for a, b in zip(sys.argv[2::2], sys.argv[3::2])]
W = max(a.width + b.width + 12 for a, b in pairs)
H = sum(max(a.height, b.height) + 8 for a, b in pairs)
out = Image.new('RGB', (W, H), (255, 0, 255)); y = 0
for a, b in pairs:
    out.paste(a, (0, y)); out.paste(b, (a.width + 6, y)); y += max(a.height, b.height) + 8
out.save(sys.argv[1]); print(out.size)
