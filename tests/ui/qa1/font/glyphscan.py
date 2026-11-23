#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# [qa-1] glyphscan.py: compare the glyph runs of known strings in a reference (genuine Windows Tahoma 8 pt)
# with the bundled font's 11-px bitmap strike. Prints per-character advance estimates and bitmaps that differ.
import sys, collections, json
from PIL import Image
from fontTools.ttLib import TTFont

FONT = sys.argv[1]
import os
SPACE = int(os.environ.get('SPACE', '3'))
f = TTFont(FONT)
idx = [i for i, s in enumerate(f['EBLC'].strikes) if s.bitmapSizeTable.ppemX == 11][0]
sd = f['EBDT'].strikeData[idx]
cmap = f.getBestCmap()

def ours(ch):
    b = sd[cmap[ord(ch)]]; m = b.metrics
    rows = []
    for y in range(m.height):
        r = b.getRow(y, bitDepth=1, metrics=m)
        rows.append(''.join('#' if (r[x // 8] >> (7 - x % 8)) & 1 else '.' for x in range(m.width)))
    return dict(bx=m.BearingX, by=m.BearingY, adv=m.Advance, w=m.width, rows=rows)

def runs(im, x0, x1, y0, y1, bright):
    def ink(x, y):
        v = im.getpixel((x, y))
        return v > 90 if bright else v < 110
    cols = [any(ink(x, y) for y in range(y0, y1)) for x in range(x0, x1)]
    out, s = [], None
    for i, c in enumerate(cols + [False]):
        if c and s is None: s = i
        if not c and s is not None: out.append((x0 + s, x0 + i - 1)); s = None
    return out

def bitmap(im, a, b, y0, y1, bright):
    rows = []
    for y in range(y0, y1):
        rows.append(''.join('#' if ((im.getpixel((x, y)) > 90) if bright else (im.getpixel((x, y)) < 110)) else '.' for x in range(a, b + 1)))
    # trim empty rows, remember top offset
    top = next((i for i, r in enumerate(rows) if '#' in r), 0)
    bot = max((i for i, r in enumerate(rows) if '#' in r), default=0)
    return top, rows[top:bot + 1]

# samples: list of (image, x0, x1, y0, y1, bright, text, baseline_y)
samples = json.load(open(sys.argv[2]))
adv = collections.defaultdict(collections.Counter)
glyphs = collections.defaultdict(collections.Counter)
bad = 0
for s in samples:
    im = Image.open(s['img']).convert('L')
    rr = runs(im, s['x0'], s['x1'], s['y0'], s['y1'], s['bright'])
    chars = [c for c in s['text'] if c != ' ']
    if len(rr) != len(chars):
        bad += 1
        print('MISMATCH', len(rr), len(chars), s['text'][:50], file=sys.stderr)
        continue
    # positions of each non-space char in the text, to count the spaces between
    pos = [i for i, c in enumerate(s['text']) if c != ' ']
    for k, (c, (a, b)) in enumerate(zip(chars, rr)):
        top, bm = bitmap(im, a, b, s['y0'], s['y1'], s['bright'])
        glyphs[c]['%d|%s' % (s['y0'] + top - s['base'], '/'.join(bm))] += 1
        if k + 1 < len(chars):
            nxt = chars[k + 1]
            spaces = pos[k + 1] - pos[k] - 1
            # pen(next) - pen(c) with our bearings as the gauge
            d = rr[k + 1][0] - a - ours(nxt)['bx'] + ours(c)['bx'] - spaces * SPACE
            adv[c][d] += 1
print('bad samples', bad)
res = {}
for c in sorted(set(adv) | set(glyphs)):
    o = ours(c)
    g = glyphs[c].most_common(1)[0][0] if glyphs[c] else ''
    otop = -o['by']
    obm = '%d|%s' % (otop, '/'.join(o['rows']))
    a = adv[c].most_common(1)[0][0] if adv[c] else None
    flag = []
    if a is not None and a != o['adv']: flag.append('ADV %d->%d %s' % (o['adv'], a, dict(adv[c])))
    if g and g != obm: flag.append('BMP')
    if flag:
        print(repr(c), ' '.join(flag))
        if 'BMP' in flag:
            print('   ours', obm); print('   ref ', g, dict(glyphs[c]) if len(glyphs[c]) > 1 else '')
    res[c] = dict(adv=a, bmp=g, n=sum(glyphs[c].values()))
json.dump(res, open(sys.argv[3], 'w'), indent=0)
