#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# [ui-e] iconsim.py <icon-dir> [--locate] : how similar are our 16x16 icons (octomx --export-icons <dir>,
# files <dir>/ui/<name>-16.png) to the icons in the original WinMX screenshots under reference/?
# For each icon the reference crop is searched near a known position (best alignment within +-3 px, so
# a shifted copy still counts as a copy). Reports
#   same%  identical pixels of the 16x16 box (our transparent pixels count as the button face)
#   ink%   identical pixels among those where either icon has ink (not face): the meaningful number
import os, sys
from PIL import Image

ROOT = os.path.join(os.path.dirname(__file__), '..', '..', '..')
REF = os.path.join(ROOT, 'reference', 'web')
# name -> (reference file, x, y) of the icon's top-left in that image (found with --locate)
SPOTS = {
    'app':       ('shared_files_page_columns_353.png', 3, 4),
    'ignored':   ('shared_files_page_columns_353.png', 496, 26),
    'settings':  ('shared_files_page_columns_353.png', 561, 26),
    'help':      ('shared_files_page_columns_353.png', 625, 26),
    'networks':  ('shared_files_page_columns_353.png', 7, 50),
    'sharedfiles': ('shared_files_page_columns_353.png', 104, 51),
    'chat':      ('shared_files_page_columns_353.png', 202, 50),
    'search':    ('shared_files_page_columns_353.png', 302, 52),
    'hotlist':   ('shared_files_page_columns_353.png', 399, 51),
    'transfers': ('shared_files_page_columns_353.png', 496, 50),
    'bandwidth': ('shared_files_page_columns_353.png', 595, 51),
}
# both images are reduced to the 16-colour VGA palette + the button face before comparing (screenshot
# colours drift by a few levels; the art is in the palette index)
PAL = {'k': (0, 0, 0), 'w': (255, 255, 255), 'g': (128, 128, 128), 's': (192, 192, 192), 't': (0, 128, 128),
       'G': (0, 255, 0), 'd': (0, 128, 0), 'r': (255, 0, 0), 'm': (128, 0, 0), 'y': (255, 255, 0), 'o': (128, 128, 0),
       'b': (0, 0, 255), 'n': (0, 0, 128), 'c': (0, 255, 255), 'M': (255, 0, 255), 'p': (255, 128, 128),
       '.': (212, 208, 200), ',': (236, 233, 216)}

def cls(p):
    c = min(PAL, key=lambda k: sum((a - b) ** 2 for a, b in zip(PAL[k], p)))
    return '.' if c == ',' else c

def same(a, b):
    return cls(a) == cls(b)

def compare(icon, ref, x0, y0):
    # the button face of that screenshot (silver in 3.53 / Win98, #D4D0C8 in 2000/XP): the most common
    # colour class around the icon; our transparent pixels count as that face
    from collections import Counter
    ring = [cls(ref.getpixel((x, y))) for x in range(x0 - 4, x0 + 20) for y in (y0 - 3, y0 + 18)
            if 0 <= x < ref.width and 0 <= y < ref.height]
    face = Counter(ring).most_common(1)[0][0] if ring else '.'
    ours = [[cls(p[:3]) if p[3] > 127 else face for p in (icon.getpixel((x, y)) for x in range(16))] for y in range(16)]
    best = None
    for dy in range(-3, 4):
        for dx in range(-3, 4):
            n_same = n_ink = n_ink_same = 0
            for y in range(16):
                for x in range(16):
                    rx, ry = x0 + dx + x, y0 + dy + y
                    if not (0 <= rx < ref.width and 0 <= ry < ref.height):
                        continue
                    r = cls(ref.getpixel((rx, ry)))
                    o = ours[y][x]
                    eq = o == r
                    n_same += eq
                    if o != face or r != face:
                        n_ink += 1
                        n_ink_same += eq
            score = (n_ink_same / n_ink if n_ink else 1.0, n_same / 256.0)
            if best is None or score > best[0]:
                best = (score, dx, dy)
    return best

def main():
    d = sys.argv[1]
    locate = '--locate' in sys.argv
    print(f"{'icon':12s} {'ink%':>6s} {'same%':>6s}  reference")
    for name, (f, x0, y0) in SPOTS.items():
        icon = Image.open(os.path.join(d, 'ui', f'{name}-16.png')).convert('RGBA')
        ref = Image.open(os.path.join(REF, f)).convert('RGB')
        if locate:   # wide search for the best spot (prints it)
            bestv = None
            for y in range(max(0, y0 - 12), min(ref.height - 16, y0 + 12)):
                for x in range(max(0, x0 - 20), min(ref.width - 16, x0 + 20)):
                    s = compare(icon, ref, x, y)
                    if bestv is None or s[0] > bestv[0]:
                        bestv = (s[0], x + s[1], y + s[2])
            print(f"{name:12s} located at {bestv[1]},{bestv[2]} ink {bestv[0][0]*100:.0f}%")
            continue
        (ink, allp), dx, dy = compare(icon, ref, x0, y0)
        print(f"{name:12s} {ink*100:6.1f} {allp*100:6.1f}  {f} ({x0 + dx},{y0 + dy})")

main()
