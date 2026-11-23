#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# [logo-1] tools/make-ico.py: merge PNG files into one multi-size Windows .ico, and check the icon
# that a built .exe (or an .ico) carries against those PNGs. Python standard library only.
#
#   tools/make-ico.py <out.ico> <png>...           sizes below 256 as 32-bit BMP with an AND mask,
#                                                  256 as PNG (as Windows expects)
#   tools/make-ico.py --check <exe|ico> <png>...   reads the icon group out of the PE resources of
#                                                  the .exe (or the .ico), compares each size whose
#                                                  PNG is given pixel by pixel; exit 1 on a mismatch
#
# The application icon: `octomx --export-icons <dir>` writes octomx-<size>.png, then
#   tools/make-ico.py src/app/res/octomx.ico <dir>/octomx-{16,20,24,32,40,48,64,128,256}.png
import struct
import sys
import zlib


def read_png(data):
    """returns (width, height, [(r, g, b, a), ...]) of an 8-bit RGBA / RGB / grey PNG"""
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('not a PNG')
    pos, idat, plte, trns = 8, b'', None, None
    while pos < len(data):
        n, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b'IHDR':
            w, h, depth, ctype, _, _, interlace = struct.unpack('>IIBBBBB', body)
        elif kind == b'PLTE':
            plte = body
        elif kind == b'tRNS':
            trns = body
        elif kind == b'IDAT':
            idat += body
        pos += 12 + n
    if depth != 8 or interlace != 0 or ctype not in (0, 2, 3, 4, 6):
        raise ValueError('unsupported PNG (depth %d, colour type %d, interlace %d)' % (depth, ctype, interlace))
    bpp = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(idat)
    stride = w * bpp
    prev = bytearray(stride)
    pixels = []
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        for x in range(w):
            px = line[x * bpp:(x + 1) * bpp]
            if ctype == 6:
                pixels.append(tuple(px))
            elif ctype == 2:
                pixels.append((px[0], px[1], px[2], 255))
            elif ctype == 0:
                pixels.append((px[0], px[0], px[0], 255))
            elif ctype == 4:
                pixels.append((px[0], px[0], px[0], px[1]))
            else:
                i = px[0]
                alpha = trns[i] if trns and i < len(trns) else 255
                pixels.append((plte[3 * i], plte[3 * i + 1], plte[3 * i + 2], alpha))
    return w, h, pixels


def bmp_entry(w, h, pixels):
    """a 32-bit DIB (BITMAPINFOHEADER, bottom-up BGRA, AND mask) as an .ico entry stores it"""
    mask_stride = ((w + 31) // 32) * 4
    xor, mask = bytearray(), bytearray()
    for y in range(h - 1, -1, -1):
        row = pixels[y * w:(y + 1) * w]
        for r, g, b, a in row:
            xor += bytes((b, g, r, a))
        bits = bytearray(mask_stride)
        for x, (_, _, _, a) in enumerate(row):
            if a == 0:
                bits[x // 8] |= 0x80 >> (x % 8)
        mask += bits
    header = struct.pack('<IiiHHIIiiII', 40, w, 2 * h, 1, 32, 0, len(xor) + len(mask), 0, 0, 0, 0)
    return header + bytes(xor) + bytes(mask)


def read_entry(data):
    """decodes one icon image (PNG, or a 32-bit DIB) to (w, h, pixels)"""
    if data[:8] == b'\x89PNG\r\n\x1a\n':
        return read_png(data)
    size, w, h2, _, bits, comp = struct.unpack('<IiiHHI', data[:20])
    if bits != 32 or comp != 0:
        raise ValueError('unsupported DIB (%d bit, compression %d)' % (bits, comp))
    h = h2 // 2
    pixels = [None] * (w * h)
    for y in range(h):
        off = size + (h - 1 - y) * w * 4
        for x in range(w):
            b, g, r, a = data[off + 4 * x:off + 4 * x + 4]
            pixels[y * w + x] = (r, g, b, a)
    return w, h, pixels


def make_ico(out, pngs):
    images = []
    for path in pngs:
        data = open(path, 'rb').read()
        w, h, pixels = read_png(data)
        images.append((w, h, data if w >= 256 else bmp_entry(w, h, pixels)))
    images.sort(key=lambda i: i[0])
    blob = struct.pack('<HHH', 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for w, h, data in images:
        blob += struct.pack('<BBBBHHII', w % 256, h % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    for _, _, data in images:
        blob += data
    open(out, 'wb').write(blob)
    print('%s: %s' % (out, ', '.join(str(i[0]) for i in images)))


def ico_images(data):
    """{size: image bytes} of an .ico file"""
    _, kind, n = struct.unpack('<HHH', data[:6])
    out = {}
    for i in range(n):
        w, _, _, _, _, _, length, offset = struct.unpack('<BBBBHHII', data[6 + 16 * i:22 + 16 * i])
        out[w or 256] = data[offset:offset + length]
    return out


def pe_icon_images(data):
    """{size: image bytes} of the first icon group in the resources of a PE file (.exe / .dll)"""
    pe = struct.unpack('<I', data[0x3c:0x40])[0]
    if data[pe:pe + 4] != b'PE\0\0':
        raise ValueError('not a PE file')
    nsect, opt_size = struct.unpack('<H12xH', data[pe + 6:pe + 22])
    opt = pe + 24
    magic = struct.unpack('<H', data[opt:opt + 2])[0]
    dirs = opt + (112 if magic == 0x20b else 96)
    res_rva, _ = struct.unpack('<II', data[dirs + 16:dirs + 24])
    sections = []
    for i in range(nsect):
        s = opt + opt_size + 40 * i
        vsize, va, rawsize, raw = struct.unpack('<IIII', data[s + 8:s + 24])
        sections.append((va, max(vsize, rawsize), raw))

    def at(rva):
        for va, size, raw in sections:
            if va <= rva < va + size:
                return raw + rva - va
        raise ValueError('RVA %#x outside the sections' % rva)

    base = at(res_rva)

    def entries(off):
        named, ids = struct.unpack('<HH', data[base + off + 12:base + off + 16])
        for i in range(named + ids):
            name, target = struct.unpack('<II', data[base + off + 16 + 8 * i:base + off + 24 + 8 * i])
            yield name, target

    def leaf(target):   # follow the first entry down to the data
        while target & 0x80000000:
            target = next(entries(target & 0x7fffffff))[1]
        rva, size = struct.unpack('<II', data[base + target:base + target + 8])
        return data[at(rva):at(rva) + size]

    types = dict(entries(0))
    icons = {name: target for name, target in entries(types[3] & 0x7fffffff)}
    group = leaf(next(entries(types[14] & 0x7fffffff))[1])
    _, _, n = struct.unpack('<HHH', group[:6])
    out = {}
    for i in range(n):
        w, _, _, _, _, _, _, nid = struct.unpack('<BBBBHHIH', group[6 + 14 * i:20 + 14 * i])
        out[w or 256] = leaf(icons[nid])
    return out


def check(path, pngs):
    data = open(path, 'rb').read()
    images = ico_images(data) if data[:4] == b'\0\0\1\0' else pe_icon_images(data)
    print('%s: icon sizes %s' % (path, ', '.join(str(s) for s in sorted(images))))
    bad = 0
    for p in pngs:
        w, h, want = read_png(open(p, 'rb').read())
        if w not in images:
            print('  %d: MISSING' % w)
            bad += 1
            continue
        gw, gh, got = read_entry(images[w])
        diff = sum(1 for a, b in zip(want, got) if a != b and not (a[3] == 0 and b[3] == 0))
        kind = 'PNG' if images[w][:4] == b'\x89PNG' else 'BMP'
        ok = (gw, gh) == (w, h) and diff == 0
        print('  %d: %s, %s' % (w, kind, 'identical to ' + p if ok else '%d pixels differ from %s' % (diff, p)))
        bad += 0 if ok else 1
    return bad


if __name__ == '__main__':
    if len(sys.argv) >= 4 and sys.argv[1] == '--check':
        sys.exit(1 if check(sys.argv[2], sys.argv[3:]) else 0)
    if len(sys.argv) < 3 or sys.argv[1].startswith('-'):
        sys.exit(__doc__ or 'usage: make-ico.py <out.ico> <png>... | --check <exe|ico> <png>...')
    make_ico(sys.argv[1], sys.argv[2:])
