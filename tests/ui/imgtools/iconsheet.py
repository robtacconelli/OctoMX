#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# [ui-e] iconsheet.py <icon-dir> <out.png> [old-icon-dir] : every exported UI icon at 16/24/32/48 px on
# the button face (zoomed 4x for 16 px), optionally next to an older export, for looking at the art.
import os, sys
from PIL import Image, ImageDraw
d, out = sys.argv[1], sys.argv[2]
old = sys.argv[3] if len(sys.argv) > 3 else None
names = sorted({f.rsplit('-', 1)[0] for f in os.listdir(os.path.join(d, 'ui'))})
face = (212, 208, 200)
cols = ([('old', old, 16)] if old else []) + [('new', d, 16), ('new', d, 24), ('new', d, 32), ('new', d, 48)]
cw = [s * (4 if s == 16 else 2 if s == 24 else 1) + 8 for _, _, s in cols]
W = 90 + sum(cw); H = 14 + len(names) * 72
sheet = Image.new('RGB', (W, H), face)
dr = ImageDraw.Draw(sheet)
for i, n in enumerate(names):
    y = 14 + i * 72
    dr.text((2, y + 28), n, fill=(0, 0, 0))
    x = 90
    for (tag, base, s), w in zip(cols, cw):
        p = os.path.join(base, 'ui', f'{n}-{s}.png')
        if os.path.exists(p):
            im = Image.open(p).convert('RGBA')
            z = 4 if s == 16 else 2 if s == 24 else 1
            im = im.resize((im.width * z, im.height * z), Image.NEAREST)
            bg = Image.new('RGBA', im.size, face + (255,))
            bg.alpha_composite(im)
            sheet.paste(bg.convert('RGB'), (x, y))
        if i == 0:
            dr.text((x, 1), f'{tag} {s}', fill=(0, 0, 0))
        x += w
sheet.save(out)
