#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# [qa-1] glyphs.py IMG x0 y0 x1 y1 [thr] -> x runs of ink columns (glyph/word segmentation of a text line)
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('L')
x0, y0, x1, y1 = map(int, sys.argv[2:6]); thr = int(sys.argv[6]) if len(sys.argv) > 6 else 100
cols = [any(im.getpixel((x, y)) < thr for y in range(y0, y1 + 1)) for x in range(x0, x1 + 1)]
runs, s = [], None
for i, c in enumerate(cols + [False]):
    if c and s is None: s = i
    if not c and s is not None: runs.append((x0 + s, x0 + i - 1)); s = None
print(' '.join(f'{a}-{b}' for a, b in runs))
