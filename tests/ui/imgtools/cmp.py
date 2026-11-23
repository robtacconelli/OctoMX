#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# cmp.py REF dx dy MINE x y w h scale OUT  -> ref crop (client coords, ref offset dx,dy) above my crop, nearest zoom
# cmp.py diff REF dx dy MINE OUT          -> per-pixel diff mask (red = differs) over full overlap
import sys
from PIL import Image, ImageChops
a=sys.argv
if a[1]=='diff':
    r=Image.open(a[2]).convert('RGB'); dx,dy=int(a[3]),int(a[4]); m=Image.open(a[5]).convert('RGB')
    w=min(r.width-dx,m.width); h=min(r.height-dy,m.height)
    rc=r.crop((dx,dy,dx+w,dy+h)); mc=m.crop((0,0,w,h))
    d=ImageChops.difference(rc,mc).convert('L').point(lambda v:255 if v>40 else 0)
    out=Image.blend(mc,Image.new('RGB',mc.size,(255,0,0)),0.0); out.paste((255,0,0),mask=d)
    out.save(a[6]); print('differing px:',sum(1 for v in d.get_flattened_data() if v))
else:
    r=Image.open(a[1]).convert('RGB'); dx,dy=int(a[2]),int(a[3]); m=Image.open(a[4]).convert('RGB')
    x,y,w,h,s=map(int,a[5:10])
    rc=r.crop((x+dx,y+dy,x+dx+w,y+dy+h)).resize((w*s,h*s),Image.NEAREST)
    mc=m.crop((x,y,x+w,y+h)).resize((w*s,h*s),Image.NEAREST)
    out=Image.new('RGB',(w*s,h*s*2+4),(255,0,255)); out.paste(rc,(0,0)); out.paste(mc,(0,h*s+4)); out.save(a[10])
