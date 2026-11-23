#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# [ui-i] Check the Transfers bar map of a live download against the store's ranges and the bytes on disk.
#
#   tests/engine/map_check.py <maplog.jsonl> [--torrent FILE.torrent] [--ref-url URL] [--samples N]
#
# <maplog.jsonl> comes from `mxcli maplog <id> <file>`. For every sample line:
#   1. the map (TransferInfo::pieceMap / pieceFill) is recomputed from the verified ranges the engine
#      reported at the same instant: every "complete" cell must be verified, every fill must match;
#   2. every "in progress" cell must touch a range that is requested now (part file: in-flight requests
#      and the received bytes of the unit they belong to; torrent: pieces in libtorrent's download queue
#      or reserved for other networks), and something must be requested at all;
#   3. bytes on disk: for complete cells (a sample), the data is read from the part file / the torrent's
#      files and compared with the original: SHA-1 of every whole piece in the cell against the .torrent
#      (--torrent), or a byte range fetched from the original URL with curl (--ref-url).
# Prints one line per sample and a summary; exit code 1 when any check failed.
import hashlib
import json
import os
import random
import subprocess
import sys


def bdecode(data, i=0):
    c = data[i:i + 1]
    if c == b'i':
        j = data.index(b'e', i)
        return int(data[i + 1:j]), j + 1
    if c == b'l':
        i += 1
        out = []
        while data[i:i + 1] != b'e':
            v, i = bdecode(data, i)
            out.append(v)
        return out, i + 1
    if c == b'd':
        i += 1
        out = {}
        while data[i:i + 1] != b'e':
            k, i = bdecode(data, i)
            v, i = bdecode(data, i)
            out[k] = v
        return out, i + 1
    j = data.index(b':', i)
    n = int(data[i:j])
    return data[j + 1:j + 1 + n], j + 1 + n


class Ranges:
    def __init__(self, pairs):
        self.r = sorted((int(a), int(b)) for a, b in pairs if int(b) > int(a))

    def covered(self, s, e):
        n = 0
        for a, b in self.r:
            if b <= s:
                continue
            if a >= e:
                break
            n += min(b, e) - max(a, s)
        return n

    def contains(self, s, e):
        return self.covered(s, e) == e - s

    def intersects(self, s, e):
        return self.covered(s, e) > 0

    def total(self):
        return sum(b - a for a, b in self.r)


def subtract(a, b):
    """ranges a minus ranges b (both sorted lists of (start, end))"""
    out = []
    for s, e in a:
        cur = s
        for bs, be in b:
            if be <= cur or bs >= e:
                continue
            if bs > cur:
                out.append((cur, bs))
            cur = max(cur, be)
        if cur < e:
            out.append((cur, e))
    return out


def axis_to_store(spans, s, e):
    """store ranges of map axis [s, e)"""
    out, a = [], 0
    for (fs, fe) in spans:
        ln = fe - fs
        lo, hi = max(s, a), min(e, a + ln)
        if hi > lo:
            out.append((fs + lo - a, fs + hi - a))
        a += ln
    return out


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__ or 'usage: map_check.py <maplog.jsonl> [--torrent F] [--ref-url URL]')
        return 2
    log = args[0]
    torrent = ref_url = None
    samples = 24
    if '--torrent' in args:
        torrent = args[args.index('--torrent') + 1]
    if '--ref-url' in args:
        ref_url = args[args.index('--ref-url') + 1]
    if '--samples' in args:
        samples = int(args[args.index('--samples') + 1])
    piece_hashes = None
    if torrent:
        meta, _ = bdecode(open(torrent, 'rb').read())
        info = meta[b'info']
        p = info[b'pieces']
        piece_hashes = [p[k:k + 20] for k in range(0, len(p), 20)]
    rnd = random.Random(7)
    failures = 0
    lines = 0
    disk_checked = 0
    pieces_checked = 0
    for raw in open(log):
        raw = raw.strip()
        if not raw:
            continue
        rec = json.loads(raw)
        lines += 1
        errs = []
        m = rec['map']
        fill = bytes.fromhex(rec['fill']) if rec.get('fill') else b''
        d = rec['dbg']
        state = rec['state']
        if not m:
            print('[%7.1fs] state %d: no map (%s)' % (rec['t'], state, rec.get('status', '')))
            continue
        n = len(m)
        if len(fill) != n:
            errs.append('fill has %d cells, map %d' % (len(fill), n))
        torrent_store = d.get('store') == 'torrent' or d.get('mapFrom') == 'snapshot'
        if torrent_store:
            L = int(d['pieceLength'])
            spans = []
            for f in d.get('files', []):
                pad = f['path'].startswith('.pad/') or '/.pad/' in f['path']
                if int(f['priority']) > 0 and not pad and int(f['size']) > 0:
                    spans.append((int(f['offset']), int(f['offset']) + int(f['size'])))
            complete = Ranges(d.get('heldRanges', []))
            prog = Ranges([(p * L, (p + 1) * L) for p in d.get('downloadingPieces', [])] +
                          [(p * L, (p + 1) * L) for p in d.get('reservedPieceList', [])])
            requested = bool(d.get('downloadingPieces')) or bool(d.get('inFlightRanges'))
        else:
            size = int(d.get('partSize', rec['size']))
            spans = [(0, size)]
            held = Ranges(d.get('heldRanges', []))
            ver = Ranges(d.get('verifiedRanges', []))
            complete = held if int(d.get('scheme', 0)) == 0 else ver
            inflight = Ranges(d.get('inFlightRanges', []))
            pending = subtract(held.r, ver.r)            # received, not verified yet
            if int(d.get('scheme', 0)) == 1:             # eD2k parts: only the units that are arriving
                unit = 9728000
                pending = [(a, b) for a, b in pending if inflight.intersects(a // unit * unit, (b - 1) // unit * unit + unit)]
            prog = Ranges(inflight.r + pending)
            requested = bool(d.get('inFlightRanges'))
        total = sum(b - a for a, b in spans)
        n_expect = min(512, total)
        if n != n_expect:
            errs.append('%d cells, expected %d' % (n, n_expect))
        n2 = n1 = 0
        complete_cells = []
        for i in range(n):
            s, e = total * i // n, total * (i + 1) // n
            parts = axis_to_store(spans, s, e)
            done = sum(complete.covered(a, b) for a, b in parts)
            ln = e - s
            f = (done * 255 + ln // 2) // ln
            if done > 0 and f < 1:
                f = 1
            if done < ln and f > 254:
                f = 254
            st = m[i]
            if (st == '2') != (done == ln):
                errs.append('cell %d: map %s, verified %d of %d bytes' % (i, st, done, ln))
            if i < len(fill) and fill[i] != f:
                errs.append('cell %d: fill %d, expected %d' % (i, fill[i], f))
            if st == '2':
                n2 += 1
                complete_cells.append((i, parts))
            if st == '1':
                n1 += 1
                if not any(prog.intersects(a, b) for a, b in parts):
                    errs.append('cell %d drawn in progress, nothing requested there' % i)
        if n1 and not requested:
            errs.append('%d cells in progress while nothing is requested' % n1)
        # bytes on disk
        disk = ''
        hashed = set()
        if complete_cells and (piece_hashes or ref_url):
            pick = complete_cells if len(complete_cells) <= samples else rnd.sample(complete_cells, samples)
            for i, parts in pick:
                for a, b in parts:
                    if torrent_store and piece_hashes:
                        # every piece the complete cell touches is a piece libtorrent has: hash it from the
                        # files on disk (pieces that reach into a file not wanted live in libtorrent's
                        # part file, not in the files: skipped)
                        L = int(d['pieceLength'])
                        for p in range(a // L, (b - 1) // L + 1):
                            ps, pe = p * L, min((p + 1) * L, store_end(d))
                            if p in hashed or not inside_wanted(d, ps, pe):
                                continue
                            hashed.add(p)
                            data = read_torrent(d, ps, pe)
                            if data is None or hashlib.sha1(data).digest() != piece_hashes[p]:
                                errs.append('cell %d: piece %d on disk does not match the .torrent' % (i, p))
                            pieces_checked += 1
                    elif not torrent_store and ref_url:
                        ln = min(b - a, 65536)
                        path = d.get('partPath') or rec.get('savePath')
                        try:
                            with open(path, 'rb') as fh:
                                fh.seek(a)
                                got = fh.read(ln)
                        except OSError as ex:
                            errs.append('cell %d: cannot read %s: %s' % (i, path, ex))
                            continue
                        want = subprocess.run(['curl', '-sfL', '--max-time', '60', '-r', '%d-%d' % (a, a + ln - 1), ref_url],
                                              capture_output=True).stdout
                        if len(want) != ln:
                            errs.append('cell %d: reference fetch gave %d bytes' % (i, len(want)))
                        elif got != want:
                            errs.append('cell %d: bytes at %d on disk differ from the original' % (i, a))
                    disk_checked += 1
            disk = ' disk-sampled=%d' % len(pick)
        status = 'OK' if not errs else 'FAIL'
        print('[%7.1fs] %s state=%d done=%d/%d complete=%d inprogress=%d requested=%s%s' %
              (rec['t'], status, state, int(rec['done']), int(rec['size']), n2, n1, requested, disk))
        for x in errs[:8]:
            print('    ' + x)
        if errs:
            failures += 1
    print('samples=%d failed=%d complete-cells-read=%d torrent-pieces-hashed=%d' % (lines, failures, disk_checked, pieces_checked))
    return 1 if failures else 0


def inside_wanted(d, a, b):
    """[a, b) lies only in wanted (or pad) files"""
    for f in d['files']:
        fo, fs = int(f['offset']), int(f['size'])
        if min(b, fo + fs) > max(a, fo):
            pad = f['path'].startswith('.pad/') or '/.pad/' in f['path']
            if int(f['priority']) <= 0 and not pad:
                return False
    return True


def store_end(d):
    return max(int(f['offset']) + int(f['size']) for f in d['files'])


def read_torrent(d, a, b):
    out = b''
    for f in d['files']:
        fo, fs = int(f['offset']), int(f['size'])
        lo, hi = max(a, fo), min(b, fo + fs)
        if hi <= lo:
            continue
        if f['path'].startswith('.pad/') or '/.pad/' in f['path']:
            out += bytes(hi - lo)                         # pad files are zeros, never written
            continue
        path = os.path.join(d['savePath'], f['path'])
        try:
            with open(path, 'rb') as fh:
                fh.seek(lo - fo)
                out += fh.read(hi - lo)
        except OSError:
            return None
    return out


if __name__ == '__main__':
    sys.exit(main())
