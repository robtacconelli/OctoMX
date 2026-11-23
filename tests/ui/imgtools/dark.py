# SPDX-License-Identifier: GPL-3.0-or-later
import sys
from PIL import Image
# dark.py IMG x0 y0 x1 y1 [thr] [dy]: bbox of pixels with luminance < thr; prints y minus dy
im=Image.open(sys.argv[1]).convert('L'); x0,y0,x1,y1=map(int,sys.argv[2:6]); thr=int(sys.argv[6]) if len(sys.argv)>6 else 100; dy=int(sys.argv[7]) if len(sys.argv)>7 else 0
pts=[(x,y) for y in range(y0,y1+1) for x in range(x0,x1+1) if im.getpixel((x,y))<thr]
if pts: print('x',min(p[0] for p in pts),max(p[0] for p in pts),'y',min(p[1] for p in pts)-dy,max(p[1] for p in pts)-dy)
else: print('none')
