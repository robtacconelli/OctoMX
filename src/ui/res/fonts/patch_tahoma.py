#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Derive the UI fonts bundled with OctoMX from Wine's Tahoma (third_party/fonts, LGPL-2.1+).

Wine's tahoma.ttf/tahomabd.ttf carry embedded bitmap strikes (EBDT/EBLC). FreeType reports the
strike's line metrics (8 pt = 11 ppem: ascent 9, descent 2, 11-px line), while Windows GDI uses the
outline metrics (usWinAscent 2049 / usWinDescent 423 of 2048: ascent 11, descent 2, 13-px line).
This script rewrites only the strike line metrics (ascender/descender of each bitmapSizeTable) to
the outline values and renames the family to "OctoMX Tahoma"; OS/2 typo and hhea metrics are set
to the Win metrics (USE_TYPO_METRICS cleared) so DirectWrite agrees too; glyph bitmaps, bearings and advances are untouched. Result: Qt on Linux lays out
text with the same line height and baseline as the original WinMX on Windows.

[qa-1] Two glyphs of Wine's 11-ppem regular strike differ from genuine Windows Tahoma 8 pt (measured
glyph by glyph on web/search_results_322_native.gif and web/chat_room_window.png with
tests/ui/qa1/font/glyphscan.py; every other covered glyph and every advance matched): "1" has its
flag on the top row instead of the second, and "J" descends 2 px below the baseline instead of
having a top bar and sitting on it. fix_glyphs() redraws those two (same bearings and advances).

Usage: patch_tahoma.py <third_party/fonts dir> <output dir>   (needs fontTools)
       patch_tahoma.py --fix-glyphs <tahoma.ttf>             (only the glyph fixes, in place)
       patch_tahoma.py --mac-advances <tahoma.ttf> <tahoma-mac.ttf>   [mac-1] the macOS copy (see bitmap_advances)
"""
import sys, os
from fontTools.ttLib import TTFont

# 11-ppem regular strike: char -> rows from the top (BearingY = number of rows above the baseline)
GLYPH_FIXES_11 = {
    "1": (8, [".#.", "##.", ".#.", ".#.", ".#.", ".#.", ".#.", "###"]),
    "J": (8, [".###", "...#", "...#", "...#", "...#", "...#", "...#", "###."]),
}


def fix_glyphs(f):
    idx = [i for i, s in enumerate(f["EBLC"].strikes) if s.bitmapSizeTable.ppemX == 11][0]
    strike = f["EBDT"].strikeData[idx]
    cmap = f.getBestCmap()
    for ch, (by, rows) in GLYPH_FIXES_11.items():
        g = strike[cmap[ord(ch)]]
        m = g.metrics
        m.width, m.height, m.BearingY = len(rows[0]), len(rows), by
        packed = [[(1 if c == "#" else 0) for c in r] for r in rows]
        # bit-aligned / byte-aligned formats both accept rows via setRows
        g.setRows([bytes(int("".join(map(str, (b + [0] * 8)[i:i + 8])), 2) for i in range(0, len(b), 8)) for b in packed],
                  bitDepth=1, metrics=m)


def bitmap_advances(f, ppem=11):
    """[mac-1] hmtx advance of every glyph of the <ppem> strike := its bitmap advance, scaled to the em.
    Core Text (macOS) draws the strike's bitmaps but positions them by the outline advances, which differ
    from the bitmap ones by up to a pixel at 11 ppem ("m": 9.24 against 8); with these advances the
    positions are the Windows (and FreeType) ones. Used for the macOS copies of the fonts only."""
    idx = [i for i, s in enumerate(f["EBLC"].strikes) if s.bitmapSizeTable.ppemX == ppem][0]
    strike = f["EBDT"].strikeData[idx]
    upm = f["head"].unitsPerEm
    hmtx = f["hmtx"].metrics
    n = 0
    for g, bmp in strike.items():
        m = getattr(bmp, "metrics", None)
        if m is None or g not in hmtx:
            continue
        # rounded up: 7 px = 1303.27 units; 1303 would put the next glyph at x 6.998, which a renderer that
        # truncates draws one pixel too far left; 1304 (7.004) stays within half a pixel for 120 glyphs
        adv = -(-m.Advance * upm // ppem)
        if hmtx[g][0] != adv:
            hmtx[g] = (adv, hmtx[g][1])
            n += 1
    # glyphs without a bitmap (the space, no-break space): the whole-pixel advance FreeType's hinting
    # gives them (space 3.44 -> 3 px), so a line of words is not wider than on Windows
    for g, (a, lsb) in list(hmtx.items()):
        if g in strike:
            continue
        px = int(a * ppem / upm + 0.5)
        hmtx[g] = (-(-px * upm // ppem), lsb)
    f["hhea"].advanceWidthMax = max(a for a, _ in hmtx.values())
    # Windows GDI (and FreeType with the strikes) does not kern this font, Core Text would ("sharing
    # with the classic ..." 5 px wider): the "kern" feature goes, mark positioning stays
    gpos = f["GPOS"].table if "GPOS" in f else None
    if gpos is not None:
        keep = [i for i, fr in enumerate(gpos.FeatureList.FeatureRecord) if fr.FeatureTag != "kern"]
        remap = {old: new for new, old in enumerate(keep)}
        gpos.FeatureList.FeatureRecord = [gpos.FeatureList.FeatureRecord[i] for i in keep]
        gpos.FeatureList.FeatureCount = len(keep)
        for sr in gpos.ScriptList.ScriptRecord:
            for ls in [sr.Script.DefaultLangSys] + [r.LangSys for r in sr.Script.LangSysRecord]:
                if ls is None:
                    continue
                ls.FeatureIndex = [remap[i] for i in ls.FeatureIndex if i in remap]
                ls.FeatureCount = len(ls.FeatureIndex)
    return n


if sys.argv[1] == "--mac-advances":
    # patch_tahoma.py --mac-advances <bundled tahoma.ttf> <out.ttf>
    f = TTFont(sys.argv[2])
    n = bitmap_advances(f)
    f.save(sys.argv[3])
    print("wrote", sys.argv[3], "with", n, "advances from the 11-ppem strike")
    sys.exit(0)

if sys.argv[1] == "--fix-glyphs":
    f = TTFont(sys.argv[2])
    fix_glyphs(f)
    f.save(sys.argv[2])
    print("fixed glyphs in", sys.argv[2])
    sys.exit(0)

src, out = sys.argv[1], sys.argv[2]
for name in ("tahoma.ttf", "tahomabd.ttf"):
    f = TTFont(os.path.join(src, name))
    if name == "tahoma.ttf":
        fix_glyphs(f)
    upm = f["head"].unitsPerEm
    asc, desc = f["OS/2"].usWinAscent, f["OS/2"].usWinDescent
    for s in f["EBLC"].strikes:
        b = s.bitmapSizeTable
        ppem = b.ppemY
        a = int(ppem * asc / upm + 0.5)
        d = int(ppem * desc / upm + 0.5)
        for m in (b.hori, b.vert):
            m.ascender, m.descender = a, -d
    # all engines must agree on the Windows (GDI) metrics: DirectWrite honours USE_TYPO_METRICS
    os2 = f["OS/2"]
    os2.sTypoAscender, os2.sTypoDescender, os2.sTypoLineGap = asc, -desc, 0
    os2.fsSelection &= ~(1 << 7)
    f["hhea"].ascent, f["hhea"].descent, f["hhea"].lineGap = asc, -desc, 0
    # own family name, so it never clashes with a system "Tahoma" (Wine installs the unpatched one)
    for rec in f["name"].names:
        if rec.nameID in (1, 3, 4, 6, 16):
            old = rec.toUnicode()
            rec.string = old.replace("Tahoma", "OctoMX Tahoma") if rec.nameID != 6 else old.replace("Tahoma", "OctoMXTahoma")
    f.save(os.path.join(out, name))
    print("wrote", name)
