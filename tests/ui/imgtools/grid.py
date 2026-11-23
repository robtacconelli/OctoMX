#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# grid.py OUT COLS IMG... -> images in a grid (4-px magenta gaps) (ui-c contact sheets)
import sys
from PIL import Image
out_path, cols = sys.argv[1], int(sys.argv[2]); ims = [Image.open(p).convert('RGB') for p in sys.argv[3:]]
w = max(i.width for i in ims); h = max(i.height for i in ims); rows = (len(ims) + cols - 1) // cols
out = Image.new('RGB', (cols * w + (cols - 1) * 4, rows * h + (rows - 1) * 4), (255, 0, 255))
for k, i in enumerate(ims):
    out.paste(i, ((k % cols) * (w + 4), (k // cols) * (h + 4)))
out.save(out_path)
