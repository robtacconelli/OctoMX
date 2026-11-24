#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# [mac-1] tools/mac/check-manifest.py <package dir>: check an unpacked macOS package against its
# MANIFEST.txt on any system (Linux too): every listed file with its size and SHA-256, every
# "# link" line as a symbolic link with that target, and nothing in the folder that is not listed.
# Prints a summary; exit 1 on any difference. Python standard library only.
import hashlib
import os
import re
import sys


def main(argv):
    if len(argv) != 1:
        sys.stderr.write('usage: check-manifest.py <package dir>\n')
        return 2
    top = argv[0]
    files, links, meta = {}, {}, {}
    with open(os.path.join(top, 'MANIFEST.txt'), encoding='utf-8') as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip('\r\n')
            m = re.match(r'^# link (.+) -> (.+)$', line)
            if m:
                links[m.group(1)] = m.group(2)
                continue
            if not line or line.startswith('#'):
                continue
            if line.startswith('version ') or line.startswith('platform '):
                k, v = line.split(' ', 1)
                meta[k] = v
                continue
            m = re.match(r'^([0-9a-f]{64}) (\d+) (.+)$', line)
            if not m:
                print('line %d malformed' % n)
                return 1
            files[m.group(3)] = (m.group(1), int(m.group(2)))
    bad = 0
    for path, (sha, size) in sorted(files.items()):
        p = os.path.join(top, path)
        if os.path.islink(p) or not os.path.isfile(p):
            print('MISSING %s' % path)
            bad += 1
            continue
        if os.path.getsize(p) != size:
            print('SIZE %s: %d, manifest %d' % (path, os.path.getsize(p), size))
            bad += 1
            continue
        h = hashlib.sha256()
        with open(p, 'rb') as f:
            for chunk in iter(lambda: f.read(1 << 20), b''):
                h.update(chunk)
        if h.hexdigest() != sha:
            print('HASH %s' % path)
            bad += 1
    for path, target in sorted(links.items()):
        p = os.path.join(top, path)
        if not os.path.islink(p) or os.readlink(p) != target:
            print('LINK %s (want -> %s)' % (path, target))
            bad += 1
    extra = 0
    for root, dirs, names in os.walk(top):
        for name in names + [d for d in dirs if os.path.islink(os.path.join(root, d))]:
            rel = os.path.relpath(os.path.join(root, name), top)
            if rel == 'MANIFEST.txt':
                continue
            if rel not in files and rel not in links:
                print('NOT LISTED %s' % rel)
                extra += 1
    print('%s: version %s, platform %s, %d files, %d links checked: %d problems, %d unlisted'
          % (top, meta.get('version', '?'), meta.get('platform', '?'), len(files), len(links), bad, extra))
    return 1 if bad or extra else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
