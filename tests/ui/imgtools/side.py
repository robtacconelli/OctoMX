#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# side.py REF dx dy MINE OUT [w h]  -> reference client crop (offset dx,dy) left, my image right, 1:1 (ui-c)
import sys
from PIL import Image
a = sys.argv
r = Image.open(a[1]).convert('RGB'); dx, dy = int(a[2]), int(a[3]); m = Image.open(a[4]).convert('RGB')
w = int(a[6]) if len(a) > 6 else m.width
h = int(a[7]) if len(a) > 7 else m.height
rc = r.crop((dx, dy, dx + w, dy + h))
out = Image.new('RGB', (w * 2 + 6, h), (255, 0, 255))
out.paste(rc, (0, 0)); out.paste(m.crop((0, 0, w, h)), (w + 6, 0))
out.save(a[5])
