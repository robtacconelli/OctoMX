#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# [qa-1] charmap.py IMG x y [w h] -> nearest-palette character map of an icon (k w g s t G d r m y o b n c M, '.' = face)
import sys
from PIL import Image
pal = {'k': (0, 0, 0), 'w': (255, 255, 255), 'g': (128, 128, 128), 's': (192, 192, 192), 't': (0, 128, 128),
       'G': (0, 255, 0), 'd': (0, 128, 0), 'r': (255, 0, 0), 'm': (128, 0, 0), 'y': (255, 255, 0), 'o': (128, 128, 0),
       'b': (0, 0, 255), 'n': (0, 0, 128), 'c': (0, 255, 255), 'M': (255, 0, 255), '.': (212, 208, 200), ',': (236, 233, 216)}
im = Image.open(sys.argv[1]).convert('RGB'); x0, y0 = int(sys.argv[2]), int(sys.argv[3])
w = int(sys.argv[4]) if len(sys.argv) > 4 else 16; h = int(sys.argv[5]) if len(sys.argv) > 5 else 16
for y in range(y0, y0 + h):
    row = ''
    for x in range(x0, x0 + w):
        p = im.getpixel((x, y))
        row += min(pal, key=lambda k: sum((a - b) ** 2 for a, b in zip(pal[k], p)))
    print(row)
