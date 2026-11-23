#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# tools/shots-qa-2.sh: qa-2 screenshot runs: a list of shots, each "<name>|<screen WxH>|<scale %|auto>|<client WxH>|<settings json>|<script; lines; ...>"
# read from a file (or stdin), run against the frozen demo core. Owner: qa-2.
#
#   tools/shots-qa-2.sh <out-dir> <shot-list-file> [octomx binary]      (binary default: build/qa-2/bin/octomx)
#   OCTOMX_EXE="wine build/qa-2-pkg/octomx/octomx.exe" OCTOMX_PATHCONV="winepath -w" DISPLAY=:7N tools/shots-qa-2.sh ...
#   ONLY=<regex> limits the shots. Every shot also writes <name>.log (script echo / geometry lines).
# On Linux the screen is an offscreen screen of that size whose logical DPI = the scale (so "auto"
# picks the same factor); a numeric scale is also forced with MX_UI_SCALE.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUT=$(realpath -m "${1:?usage: tools/shots-qa-2.sh <out-dir> <list> [binary]}")
LIST=${2:?shot list}
BIN=${3:-$ROOT/build/qa-2/bin/octomx}
mkdir -p "$OUT"
WORK=$OUT/.work
mkdir -p "$WORK"
export LC_ALL=${LC_ALL:-C.UTF-8}
export MX_DIGIT_GROUPING=${MX_DIGIT_GROUPING-space}
WINE=0
if [ -n "${OCTOMX_EXE:-}" ]; then WINE=1; EXE="$OCTOMX_EXE"; PATHCONV="${OCTOMX_PATHCONV:-cat}"; else EXE="$BIN"; PATHCONV=cat; fi
conv() { if [ "$PATHCONV" = cat ]; then echo "$1"; else $PATHCONV "$1"; fi; }

shot() {
    local name=$1 screen=$2 scale=$3 size=$4 settings=$5 lines=$6
    if [ -n "${ONLY:-}" ] && ! [[ $name =~ $ONLY ]]; then return; fi
    local prof=$WORK/prof-$name
    rm -rf "$prof"; mkdir -p "$prof"
    [ -n "$settings" ] && echo "$settings" > "$prof/settings.json"
    local script=$WORK/$name.script
    : > "$script"
    local outc; outc=$(conv "$OUT")   # before IFS changes ("winepath -w" must split at the space)
    local IFS=';'
    for l in $lines; do l="${l#"${l%%[![:space:]]*}"}"; [ -n "$l" ] && echo "${l//@OUT@/$outc}" >> "$script"; done
    unset IFS
    grep -q '^shot ' "$script" || echo "shot $(conv "$OUT/$name.png")" >> "$script"
    grep -q '^quit' "$script" || echo "quit" >> "$script"
    local sw=${screen%x*} sh=${screen#*x}
    local pct=$scale; [ "$scale" = auto ] && pct=100
    local force=(); [ "$scale" != auto ] && force=(MX_UI_SCALE=$scale)
    if [ $WINE = 0 ]; then
        local dpi=$(( 96 * pct / 100 ))
        cat > "$WORK/screen-$name.json" <<JSON
{ "synchronousWindowSystemEvents": false, "windowFrameMargins": false,
  "screens": [ { "name": "shot", "x": 0, "y": 0, "width": $sw, "height": $sh,
                 "logicalDpi": $dpi, "logicalBaseDpi": 96, "dpr": 1 } ] }
JSON
        if ! env "${force[@]}" QT_QPA_PLATFORM="offscreen:configfile=$WORK/screen-$name.json" timeout 120 "$EXE" --profile "$prof" \
                --demo-frozen --no-wizard --size "$size" --script "$script" > "$OUT/$name.log" 2>&1; then
            echo "FAILED: $name"; tail -3 "$OUT/$name.log"; return
        fi
    else
        if ! env "${force[@]}" timeout 180 $EXE --profile "$(conv "$prof")" --demo-frozen --no-wizard --size "$size" \
                --script "$(conv "$script")" > "$OUT/$name.log" 2>&1; then
            echo "FAILED: $name"; tail -3 "$OUT/$name.log"; return
        fi
    fi
    echo "ok: $name"
}

while IFS='|' read -r name screen scale size settings lines; do
    [[ -z "$name" || $name == \#* ]] && continue
    shot "$name" "$screen" "$scale" "$size" "$settings" "$lines"
done < "$LIST"
echo "shots in $OUT"
