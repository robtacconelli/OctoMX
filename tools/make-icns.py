#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# [mac-1] tools/make-icns.py: merge the application icon PNGs into one macOS .icns, and check an
# .icns (or the one inside an app bundle) against those PNGs. Python standard library only, so the
# file can be made on any platform; macOS reads PNG entries of every size since 10.7.
#
#   tools/make-icns.py <out.icns> <dir>            dir = output of `octomx --export-icons <dir>`
#   tools/make-icns.py --check <icns|App.app> <dir>  every entry must be the exported PNG of its
#                                                  size, byte for byte; exit 1 otherwise
#   tools/make-icns.py --list <icns|App.app>       type, pixel size and length of each entry
#
# The application icon: src/app/res/octomx.icns, made with
#   octomx --export-icons <dir> && tools/make-icns.py src/app/res/octomx.icns <dir>
import os
import struct
import sys

# (type, pixel size): the PNG entries Finder, the Dock and the app switcher use (1x and 2x)
ENTRIES = [
    (b'icp4', 16), (b'icp5', 32), (b'icp6', 64), (b'ic07', 128), (b'ic08', 256), (b'ic09', 512),
    (b'ic10', 1024), (b'ic11', 32), (b'ic12', 64), (b'ic13', 256), (b'ic14', 512),
]


def png_size(data):
    if data[:8] != b'\x89PNG\r\n\x1a\n' or data[12:16] != b'IHDR':
        raise ValueError('not a PNG')
    return struct.unpack('>II', data[16:24])


def png_for(d, px):
    p = os.path.join(d, 'octomx-%d.png' % px)
    with open(p, 'rb') as f:
        data = f.read()
    if png_size(data) != (px, px):
        raise ValueError('%s is not %dx%d' % (p, px, px))
    return data


def make(out, d):
    body = b''
    for kind, px in ENTRIES:
        data = png_for(d, px)
        body += kind + struct.pack('>I', 8 + len(data)) + data
    with open(out, 'wb') as f:
        f.write(b'icns' + struct.pack('>I', 8 + len(body)) + body)
    print('%s: %d entries, %d bytes' % (out, len(ENTRIES), 8 + len(body)))


def entries(path):
    if os.path.isdir(path):
        path = os.path.join(path, 'Contents', 'Resources', 'octomx.icns')
    with open(path, 'rb') as f:
        data = f.read()
    if data[:4] != b'icns' or struct.unpack('>I', data[4:8])[0] != len(data):
        raise ValueError('%s: not an icns file' % path)
    pos, out = 8, []
    while pos < len(data):
        kind, n = struct.unpack('>4sI', data[pos:pos + 8])
        out.append((kind, data[pos + 8:pos + n]))
        pos += n
    return out


def main(argv):
    if len(argv) == 3 and argv[0] == '--check':
        want = dict(ENTRIES)
        got = entries(argv[1])
        bad = 0
        for kind, data in got:
            px = want.get(kind)
            if px is None:
                continue
            if data != png_for(argv[2], px):
                print('MISMATCH %s (%d px)' % (kind.decode(), px))
                bad += 1
        missing = set(want) - set(k for k, _ in got)
        for k in sorted(missing):
            print('MISSING %s' % k.decode())
        print('%s: %d entries checked, %d mismatches, %d missing' % (argv[1], len(got), bad, len(missing)))
        return 1 if bad or missing else 0
    if len(argv) == 2 and argv[0] == '--list':
        for kind, data in entries(argv[1]):
            try:
                w, h = png_size(data)
                print('%s %dx%d %d bytes' % (kind.decode(), w, h, len(data)))
            except ValueError:
                print('%s (not PNG) %d bytes' % (kind.decode('latin-1'), len(data)))
        return 0
    if len(argv) == 2 and not argv[0].startswith('-'):
        make(argv[0], argv[1])
        return 0
    sys.stderr.write(__doc__ if __doc__ else 'usage: make-icns.py <out.icns> <dir> | --check <icns> <dir> | --list <icns>\n')
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
