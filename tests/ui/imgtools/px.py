#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# px.py: tiny image probe helper.
#  px.py row IMG y x0 x1         -> print runs of colours along a row
#  px.py col IMG x y0 y1         -> runs along a column
#  px.py zoom IMG x y w h N OUT  -> crop + nearest upscale
#  px.py size IMG
import sys
from PIL import Image
def runs(seq):
    out=[];prev=None;start=0
    for i,c in enumerate(seq):
        if c!=prev:
            if prev is not None: out.append((start,i-1,prev))
            prev=c;start=i
    out.append((start,len(seq)-1,prev))
    return out
cmd=sys.argv[1]; im=Image.open(sys.argv[2]).convert('RGB')
if cmd=='size': print(im.size)
elif cmd in('row','col'):
    a=int(sys.argv[3]);b0=int(sys.argv[4]);b1=int(sys.argv[5])
    seq=[im.getpixel((x,a)) if cmd=='row' else im.getpixel((a,x)) for x in range(b0,b1+1)]
    for s,e,c in runs(seq): print(f"{s+b0}-{e+b0} ({e-s+1}) #{c[0]:02x}{c[1]:02x}{c[2]:02x}")
elif cmd=='zoom':
    x,y,w,h,n=map(int,sys.argv[3:8]); im.crop((x,y,x+w,y+h)).resize((w*n,h*n),Image.NEAREST).save(sys.argv[8])
