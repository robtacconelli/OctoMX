#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# [ui-g] Real mouse / keyboard input through the X server's XTEST extension (what xdotool does; the
# TightVNC Xvnc here has no XKEYBOARD, which crashes xdotool). Commands, as xdotool's:
#   xtest_input.py mousemove X Y | click BUTTON | key Escape|Return
import ctypes
import sys

x11 = ctypes.CDLL('libX11.so.6')
xt = ctypes.CDLL('libXtst.so.6')
x11.XOpenDisplay.restype = ctypes.c_void_p
x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
x11.XFlush.argtypes = [ctypes.c_void_p]
x11.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
x11.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
x11.XKeysymToKeycode.restype = ctypes.c_ubyte
xt.XTestFakeMotionEvent.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_ulong]
xt.XTestFakeButtonEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
xt.XTestFakeKeyEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
KEYSYMS = {'Escape': 0xff1b, 'Return': 0xff0d}

d = x11.XOpenDisplay(None)
if not d:
    sys.exit('cannot open display')
a = sys.argv[1:]
if a and a[0] == 'mousemove':
    xt.XTestFakeMotionEvent(d, -1, int(a[1]), int(a[2]), 0)
elif a and a[0] == 'click':
    b = int(a[1])
    xt.XTestFakeButtonEvent(d, b, 1, 0)
    x11.XSync(d, 0)
    xt.XTestFakeButtonEvent(d, b, 0, 30)
elif a and a[0] == 'key':
    kc = x11.XKeysymToKeycode(d, KEYSYMS[a[1]])
    xt.XTestFakeKeyEvent(d, kc, 1, 0)
    xt.XTestFakeKeyEvent(d, kc, 0, 20)
else:
    sys.exit('usage: xtest_input.py mousemove X Y | click N | key NAME')
x11.XSync(d, 0)
x11.XCloseDisplay(d)
