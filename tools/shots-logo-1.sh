#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# [logo-1] The application icon review: exports the icon at every size, takes screenshots of the
# demo program (offscreen, 100 % and 200 %: main window, About box), dumps the window / tray icon
# pixmaps the program hands to the system (tst_logo with MX_LOGO_SHOTS), and puts it all on one
# contact sheet (sheet.png: every size 1:1 on black, Windows grey, white and a dark task bar; the
# small sizes enlarged 8x with nearest neighbour; the screenshots).
#
#   tools/shots-logo-1.sh [<out-dir> [<build-dir>]]     (default build/shots/logo-1, build/linux)
#
# With WIN_PKG=<folder made by tools/package-win.sh> it also runs the Windows build under wine (its own
# prefix build/wineprefix-logo, a wine virtual desktop on its own Xvnc :87) and grabs the real Win32
# title bar and the wine system tray with the icon (wine-titlebar-100.png, wine-titlebar-200.png).
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source "$ROOT/toolchain/env.sh"
OUT=$(realpath -m "${1:-$ROOT/build/shots/logo-1}")
BUILD=$(realpath -m "${2:-$ROOT/build/linux}")
EXE=$BUILD/bin/octomx
WORK=$OUT/.work
rm -rf "$WORK" "$OUT/export" "$OUT/tray" "$OUT/app"
mkdir -p "$WORK" "$OUT/app"
export LC_ALL=C.UTF-8

# every size of the icon (the program's own export, plus the in-between UI sizes)
QT_QPA_PLATFORM=offscreen "$EXE" --profile "$WORK/prof-export" --export-icons "$OUT/export" > "$WORK/export.log" 2>&1
for s in 16 20 24 28 32 40 48; do cp "$OUT/export/ui/app-$s.png" "$OUT/app/" 2>/dev/null || true; done
for s in 16 20 24 32 40 48 64 128 256 512; do cp "$OUT/export/octomx-$s.png" "$OUT/app/app-$s.png"; done

# the window and tray icon pixmaps as the program hands them to the system
QT_QPA_PLATFORM=offscreen MX_LOGO_SHOTS="$OUT/tray" "$BUILD/bin/tst_logo" -o "$WORK/tst_logo.txt,txt" > /dev/null 2>&1 \
    || { echo "tst_logo failed:"; tail -5 "$WORK/tst_logo.txt"; }

shot() {   # shot <name> <screen WxH> <scale %> <script lines...>
    local name=$1 screen=$2 scale=$3; shift 3
    local prof=$WORK/prof-$name script=$WORK/$name.script
    mkdir -p "$prof"
    printf '%s\n' "$@" "shot $OUT/$name.png" "quit" > "$script"
    local sw=${screen%x*} sh=${screen#*x} dpi=$(( 96 * scale / 100 ))
    cat > "$WORK/screen-$name.json" <<JSON
{ "synchronousWindowSystemEvents": false, "windowFrameMargins": false,
  "screens": [ { "name": "shot", "x": 0, "y": 0, "width": $sw, "height": $sh,
                 "logicalDpi": $dpi, "logicalBaseDpi": 96, "dpr": 1 } ] }
JSON
    if QT_QPA_PLATFORM="offscreen:configfile=$WORK/screen-$name.json" timeout 90 "$EXE" --profile "$prof" \
            --demo-frozen --no-wizard --size "$(( 800 * scale / 100 ))x$(( 560 * scale / 100 ))" --script "$script" \
            > "$WORK/$name.log" 2>&1; then
        echo "ok: $name.png"
    else
        echo "FAILED: $name (see $WORK/$name.log)"
    fi
}
for scale in 100 200; do
    shot "main-$scale" 2560x1600 $scale "page networks" "wait 300"
    shot "about-$scale" 2560x1600 $scale "page networks" "help-menu" "wait 100" "menu-pick About OctoMX..." "wait 400"
done

if [ -n "${WIN_PKG:-}" ]; then
    (
    export WINEPREFIX=$ROOT/build/wineprefix-logo WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d"
    if [ ! -f "$WINEPREFIX/system.reg" ]; then
        DISPLAY= wineboot -i > /dev/null 2>&1 || true
        wineserver -w
        for f in "$WINEPREFIX"/drive_c/windows/system32/spool/drivers/color/*.ic[cm]; do
            [ -e "$f" ] && mv -f "$f" "$f.disabled-for-qt"
        done
    fi
    # a virtual desktop: wine draws the window frames itself (Xvnc has no window manager)
    DISPLAY= wine reg add 'HKCU\Software\Wine\Explorer' /v Desktop /d Default /f > /dev/null 2>&1
    DISPLAY= wine reg add 'HKCU\Software\Wine\Explorer\Desktops' /v Default /d 1200x900 /f > /dev/null 2>&1
    wineserver -w
    "$ROOT/tools/xvnc-start.sh" 87 > /dev/null
    export DISPLAY=:87
    cd "$WIN_PKG"
    for scale in 100 200; do
        rm -rf "$WORK/wprof-$scale"
        MX_UI_SCALE=$scale wine octomx.exe --profile "$(winepath -w "$WORK/wprof-$scale" 2>/dev/null)" --demo-frozen --no-wizard \
            --size $(( 700 * scale / 100 ))x$(( 400 * scale / 100 )) > "$WORK/wine-$scale.log" 2>&1 &
        timeout 30 tail -f /dev/null || true
        "$ROOT/tools/x-screenshot.sh" 87 "$OUT/wine-titlebar-$scale.png" > /dev/null && echo "ok: wine-titlebar-$scale.png"
        wineserver -k
        wait
    done
    "$ROOT/tools/xvnc-stop.sh" 87 > /dev/null
    )
fi

python3 - "$OUT" <<'PY'
import glob, os, sys
from PIL import Image, ImageDraw
out = sys.argv[1]
app = lambda s: Image.open(f'{out}/app/app-{s}.png').convert('RGBA')
sizes = [16, 20, 24, 28, 32, 40, 48, 64, 128, 256]
bgs = [('black', (0, 0, 0)), ('Windows grey #D4D0C8', (0xD4, 0xD0, 0xC8)), ('white', (255, 255, 255)),
       ('dark task bar #1F2430', (0x1F, 0x24, 0x30)), ('blue task bar #245EDC', (0x24, 0x5E, 0xDC))]
pad, label = 12, 16
W = 1700
blocks = []
# 1:1 rows
for name, bg in bgs:
    h = 256 + 2 * pad + label
    im = Image.new('RGB', (W, h), bg)
    d = ImageDraw.Draw(im)
    fg = (255, 255, 255) if sum(bg) < 400 else (0, 0, 0)
    d.text((pad, 4), f'1:1 on {name}: ' + ', '.join(map(str, sizes)) + ' px', fill=fg)
    x = pad
    for s in sizes:
        a = app(s)
        im.paste(a, (x, label + pad + 256 - s), a)
        x += s + pad
    blocks.append(im)
# small sizes enlarged 8x (nearest neighbour)
for group in ([16, 20, 24, 28, 32], [40, 48]):
    z = 8
    h = max(group) * z + 2 * pad + label
    im = Image.new('RGB', (W, h), (0xD4, 0xD0, 0xC8))
    d = ImageDraw.Draw(im)
    d.text((pad, 4), '8x, nearest neighbour: ' + ', '.join(map(str, group)) + ' px', fill=(0, 0, 0))
    x = pad
    for s in group:
        a = app(s).resize((s * z, s * z), Image.NEAREST)
        im.paste(a, (x, label + pad), a)
        x += s * z + pad
    blocks.append(im)
# the pixmaps the program hands to the system, 1:1 and 4x
tray = sorted(glob.glob(f'{out}/tray/*.png'), key=lambda p: (os.path.basename(p).split('-')[0], int(p.rsplit('-', 1)[1][:-4])))
if tray:
    h = 48 * 4 + 2 * pad + label * 2
    im = Image.new('RGB', (W, h), (0x1F, 0x24, 0x30))
    d = ImageDraw.Draw(im)
    d.text((pad, 4), 'QSystemTrayIcon::icon() and windowIcon() pixmaps (tray-*, window-*), 1:1 and 4x', fill=(255, 255, 255))
    x = pad
    for p in tray:
        a = Image.open(p).convert('RGBA')
        s = a.width
        if x + s * 4 + s > W:
            break
        im.paste(a, (x, label + pad), a)
        b = a.resize((s * 4, s * 4), Image.NEAREST)
        im.paste(b, (x + s + 4, label + pad), b)
        d.text((x, h - label), os.path.basename(p)[:-4], fill=(255, 255, 255))
        x += s * 5 + 4 + pad
    blocks.append(im)
# screenshots: the About box (its own window), the main window's top left corner
shots = []
for scale in (100, 200):
    about = glob.glob(f'{out}/about-{scale}.*.png')
    about = [p for p in about if 'menu' not in p]
    if about:
        shots.append((f'About box {scale} %', Image.open(about[0]).convert('RGB')))
    if os.path.exists(f'{out}/main-{scale}.png'):
        m = Image.open(f'{out}/main-{scale}.png').convert('RGB')
        shots.append((f'main window {scale} % (top left)', m.crop((0, 0, min(m.width, 400 * scale // 100), min(m.height, 140 * scale // 100)))))
for scale in (100, 200):
    p = f'{out}/wine-titlebar-{scale}.png'
    if os.path.exists(p):
        w = Image.open(p).convert('RGB')
        # the window's top left corner: the first pixel of the active caption colour
        px = w.load()
        at = next(((x, y) for y in range(w.height) for x in range(0, w.width, 2) if px[x, y] == (50, 150, 250)), (0, 0))
        cap = w.crop((max(0, at[0] - 4), max(0, at[1] - 2), max(0, at[0] - 4) + 300, max(0, at[1] - 2) + 26))
        shots.append((f'wine, Win32 title bar {scale} % (4x)', cap.resize((cap.width * 4, cap.height * 4), Image.NEAREST)))
        if scale == 100:   # wine shows the tray icons in a small window at the top left
            tr = w.crop((0, 0, 170, 52))
            shots.append(('wine system tray (2x)', tr.resize((340, 104), Image.NEAREST)))
rows, row, used = [], [], pad
for t, img in shots:   # pack the screenshots into rows, each as high as its tallest one
    if row and used + img.width > W:
        rows.append(row)
        row, used = [], pad
    row.append((t, img))
    used += img.width + pad
if row:
    rows.append(row)
for row in rows:
    h = max(i.height for _, i in row) + 2 * pad + label
    im = Image.new('RGB', (W, h), (0x80, 0x80, 0x80))
    d = ImageDraw.Draw(im)
    x = pad
    for t, img in row:
        d.text((x, 4), t, fill=(255, 255, 255))
        im.paste(img, (x, label + pad))
        x += img.width + pad
    blocks.append(im)
sheet = Image.new('RGB', (W, sum(b.height for b in blocks)), (0x80, 0x80, 0x80))
y = 0
for b in blocks:
    sheet.paste(b, (0, y))
    y += b.height
sheet.save(f'{out}/sheet.png')
print(f'{out}/sheet.png: {sheet.width}x{sheet.height}')
PY
[ -n "${KEEP_WORK:-}" ] || rm -rf "$WORK"
