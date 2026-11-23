#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# boxes.py: find layout objects in a screenshot (ui-c helper for the Settings pages).
#   boxes.py white IMG dx dy [minArea]          white rectangles (edit fields, lists): client bbox
#   boxes.py text  IMG dx dy x0 y0 x1 y1 [thr]  dark text lines inside a client rect: bbox per line
#   boxes.py frame IMG dx dy x0 y0 x1 y1        rows/cols of gray (#808080-ish) lines (group boxes)
# dx,dy = client offset in IMG (classic dialog: 3,22). Prints "x0,y0 - x1,y1 (w x h)".
import sys
import numpy as np
from PIL import Image

a = sys.argv
im = np.asarray(Image.open(a[2]).convert('RGB')).astype(int)
dx, dy = int(a[3]), int(a[4])


def comps(mask, min_area):
    h, w = mask.shape
    seen = np.zeros_like(mask, dtype=bool)
    out = []
    for y in range(h):
        for x in range(w):
            if mask[y, x] and not seen[y, x]:
                st = [(y, x)]
                seen[y, x] = True
                x0 = x1 = x
                y0 = y1 = y
                n = 0
                while st:
                    cy, cx = st.pop()
                    n += 1
                    x0, x1, y0, y1 = min(x0, cx), max(x1, cx), min(y0, cy), max(y1, cy)
                    for ny, nx in ((cy + 1, cx), (cy - 1, cx), (cy, cx + 1), (cy, cx - 1)):
                        if 0 <= ny < h and 0 <= nx < w and mask[ny, nx] and not seen[ny, nx]:
                            seen[ny, nx] = True
                            st.append((ny, nx))
                if n >= min_area:
                    out.append((x0, y0, x1, y1, n))
    return out


if a[1] == 'white':
    mn = int(a[5]) if len(a) > 5 else 60
    m = (im[:, :, 0] >= 250) & (im[:, :, 1] >= 250) & (im[:, :, 2] >= 250)
    for x0, y0, x1, y1, n in sorted(comps(m, mn), key=lambda c: (c[1], c[0])):
        if (x1 - x0 + 1) * (y1 - y0 + 1) > 0 and n > 0.5 * (x1 - x0 + 1) * (y1 - y0 + 1):
            print(f"{x0-dx},{y0-dy} - {x1-dx},{y1-dy} ({x1-x0+1}x{y1-y0+1})")
elif a[1] == 'text':
    x0, y0, x1, y1 = (int(v) for v in a[5:9])
    thr = int(a[9]) if len(a) > 9 else 100
    sub = im[y0 + dy:y1 + dy + 1, x0 + dx:x1 + dx + 1]
    dark = sub.sum(axis=2) < thr * 3
    rows = dark.any(axis=1)
    y = 0
    while y < len(rows):
        if rows[y]:
            s = y
            while y < len(rows) and rows[y]:
                y += 1
            cols = np.where(dark[s:y].any(axis=0))[0]
            print(f"{x0+cols[0]},{y0+s} - {x0+cols[-1]},{y0+y-1} ({cols[-1]-cols[0]+1}x{y-s})")
        y += 1
elif a[1] == 'frame':
    x0, y0, x1, y1 = (int(v) for v in a[5:9])
    sub = im[y0 + dy:y1 + dy + 1, x0 + dx:x1 + dx + 1]
    g = (abs(sub[:, :, 0] - 128) < 20) & (abs(sub[:, :, 1] - 128) < 20) & (abs(sub[:, :, 2] - 132) < 20)
    for yy in range(g.shape[0]):
        r = g[yy]
        if r.sum() > 40:
            xs = np.where(r)[0]
            print(f"hline y={y0+yy} x={x0+xs[0]}..{x0+xs[-1]} n={r.sum()}")
    for xx in range(g.shape[1]):
        c = g[:, xx]
        if c.sum() > 40:
            ys = np.where(c)[0]
            print(f"vline x={x0+xx} y={y0+ys[0]}..{y0+ys[-1]} n={c.sum()}")
