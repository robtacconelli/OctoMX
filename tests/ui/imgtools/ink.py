# SPDX-License-Identifier: GPL-3.0-or-later
import sys
from PIL import Image
# ink.py IMG x0 y0 x1 y1 : bounding box of non-background pixels (bg = color at x0,y0)
im=Image.open(sys.argv[1]).convert('RGB'); x0,y0,x1,y1=map(int,sys.argv[2:6]); bg=im.getpixel((x0,y0))
xs=[];ys=[]
for y in range(y0,y1+1):
  for x in range(x0,x1+1):
    if im.getpixel((x,y))!=bg: xs.append(x);ys.append(y)
print('bbox',min(xs),min(ys),max(xs),max(ys)) if xs else print('empty')
