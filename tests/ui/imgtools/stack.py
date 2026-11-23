#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# stack.py OUT IMG... -> images stacked vertically with 4-px magenta separators (ui-c contact sheets)
import sys
from PIL import Image
ims = [Image.open(p).convert('RGB') for p in sys.argv[2:]]
w = max(i.width for i in ims); h = sum(i.height for i in ims) + 4 * (len(ims) - 1)
out = Image.new('RGB', (w, h), (255, 0, 255)); y = 0
for i in ims:
    out.paste(i, (0, y)); y += i.height + 4
out.save(sys.argv[1])
