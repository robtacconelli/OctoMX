#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# tools/shots-ui-i.sh: screenshots of the download bar as a map of the file [ui-i] (demo core).
#
#   tools/shots-ui-i.sh <out-dir> [build-dir]      (build-dir default: build/linux)
#
# Transfers page dark / light at 100 % and 200 %, the live demo (maps moving), a narrow and a very
# wide Status column, 800x600, and 8x enlargements of single bars (tests/ui/imgtools/px.py zoom).
# Linux offscreen; nothing outside <out-dir> is written (profiles live in <out-dir>/.work).
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUT=$(realpath -m "${1:?usage: tools/shots-ui-i.sh <out-dir> [build-dir]}")
BUILD=${2:-$ROOT/build/linux}
EXE="$BUILD/bin/octomx"
mkdir -p "$OUT"
WORK=$OUT/.work
rm -rf "$WORK"; mkdir -p "$WORK"
export LC_ALL=C.UTF-8
export MX_DIGIT_GROUPING=space
export MX_SOUND_OFF=1

# shot <name> <scale%> <clientWxH> <frozen 1|0> <theme dark|light> <script lines...>
shot() {
    local name=$1 scale=$2 size=$3 frozen=$4 theme=$5; shift 5
    local prof=$WORK/prof-$name
    mkdir -p "$prof"
    echo "{\"ui/settingsMigrated\":1,\"ui/chatWarningDisabled\":true,\"ui/colorTheme\":\"$theme\"}" > "$prof/settings.json"
    local dpi=$(( 96 * scale / 100 ))
    cat > "$WORK/screen-$name.json" <<EOF
{ "synchronousWindowSystemEvents": false, "windowFrameMargins": false,
  "screens": [ { "name": "shot", "x": 0, "y": 0, "width": 3840, "height": 2400,
                 "logicalDpi": $dpi, "logicalBaseDpi": 96, "dpr": 1 } ] }
EOF
    local script=$WORK/$name.script
    : > "$script"
    for l in "$@"; do echo "$l" >> "$script"; done
    echo "shot $OUT/$name.png" >> "$script"
    echo "quit" >> "$script"
    local demo=--demo-frozen
    [ "$frozen" = 1 ] || demo=--demo
    if ! QT_QPA_PLATFORM="offscreen:configfile=$WORK/screen-$name.json" MX_UI_SCALE=$scale \
            timeout 120 "$EXE" --profile "$prof" $demo --size "$size" --script "$script" > "$WORK/$name.log" 2>&1; then
        echo "FAILED: $name (see $WORK/$name.log)"; tail -3 "$WORK/$name.log"
    else
        echo "ok: $name.png"
    fi
}

T="page transfers"
for theme in dark light; do
    shot "transfers-$theme-100" 100 1024x689 1 "$theme" "$T" "wait 300"
    shot "transfers-$theme-200" 200 2048x1378 1 "$theme" "$T" "wait 300"
done
shot transfers-800x600        100 800x600   1 dark "$T" "wait 300"
shot transfers-demo-live      100 1024x689  0 dark "$T" "wait 6000"
shot transfers-demo-live-light 100 1024x689 0 light "$T" "wait 6000"
# narrow and very wide Status column (column 4 of the download list)
shot transfers-status-narrow  100 1024x689  1 dark "$T" "wait 200" "set-column downloadList 4 64" "wait 200"
shot transfers-status-wide    100 2200x689  1 dark "$T" "wait 200" "set-column downloadList 4 900" "wait 200"
shot transfers-status-wide-light 100 2200x689 1 light "$T" "wait 200" "set-column downloadList 4 900" "wait 200"
shot transfers-dark-150       150 1536x1034 1 dark "$T" "wait 300"
shot transfers-dark-300       300 3072x2067 1 dark "$T" "wait 300"
# every demo download on one screen (source rows collapsed): the maps side by side
COLLAPSE=("collapse-row downloadList 0" "collapse-row downloadList 1" "collapse-row downloadList 2"
          "collapse-row downloadList 3" "collapse-row downloadList 6" "collapse-row downloadList 8")
for theme in dark light; do
    shot "transfers-$theme-collapsed" 100 1024x689 1 "$theme" "$T" "wait 200" "${COLLAPSE[@]}" "wait 200"
    shot "transfers-$theme-collapsed-200" 200 2048x1378 1 "$theme" "$T" "wait 200" "${COLLAPSE[@]}" "wait 200"
done
# Settings, Appearance, Transfers Window: the colour of the parts arriving now (17 colours: two columns)
shot settings-transfers-colors 100 1024x689 1 dark "settings appearance/transfers" "wait 300"
rm -rf "$WORK"/prof-*

# 8x enlargements: every bar of the collapsed download list (found by its frame: a long run of the
# bar colour), stacked
for theme in dark light; do
    python3 - "$OUT/transfers-$theme-collapsed.png" "$OUT/bars-x8-$theme.png" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB')
W, H = im.size
def longrun(y):
    row = [im.getpixel((x, y)) for x in range(W // 2, W - 120)]
    best = run = 0
    for i in range(1, len(row)):
        run = run + 1 if row[i] == row[i - 1] and row[i][2] > 150 and row[i][0] < 40 else 0
        best = max(best, run)
    return best > 100
bars = []
for y in range(110, H // 2):
    if (not bars or y - bars[-1] > 12) and longrun(y) and not longrun(y - 1):
        bars.append(y)
x0 = None
for x in range(W // 2, W):
    c = im.getpixel((x, bars[0] + 3)) if bars else (0, 0, 0)
    if c[2] > 150 and c[0] < 40:
        x0 = x
        break
tiles = [im.crop((x0 - 2, y - 2, x0 + 170, y + 14)).resize((172 * 8, 16 * 8), Image.NEAREST) for y in bars]
out = Image.new('RGB', (172 * 8, sum(t.height + 8 for t in tiles)), (212, 208, 200))
yy = 0
for t in tiles:
    out.paste(t, (0, yy))
    yy += t.height + 8
out.save(sys.argv[2])
print("bars:", len(tiles))
PY
done
