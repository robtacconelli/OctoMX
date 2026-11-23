#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# tools/shots-ui-g.sh: screenshots of the ui-g work: the Search form with the results filter at
# 800x600 / 1024x768 / 1920x1080 design sizes and 100 / 150 / 200 %, full-width columns of every list,
# context menus, tool tips. SETS=<regex> picks the groups (search, lists, menus, tips; default all).
# Frozen demo core. Owner: ui-g.
#
#   tools/shots-ui-g.sh <out-dir> [build-dir]          (build-dir default: build/ui-g)
#   OCTOMX_EXE="wine dist/ui-e/octomx/octomx.exe" OCTOMX_PATHCONV="winepath -w" tools/shots-ui-g.sh <out-dir>
#       (under wine the scale comes from MX_UI_SCALE, set per shot; the screen is the Xvnc display)
#   ONLY=<regex> limits the shots.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUT=$(realpath -m "${1:?usage: tools/shots-ui-g.sh <out-dir> [build-dir]}")
BUILD=${2:-$ROOT/build/ui-g}
mkdir -p "$OUT"
WORK=$OUT/.work-g
rm -rf "$WORK"; mkdir -p "$WORK"
export LC_ALL=C.UTF-8
export MX_DIGIT_GROUPING=space
WINE=0
if [ -n "${OCTOMX_EXE:-}" ]; then WINE=1; EXE="$OCTOMX_EXE"; PATHCONV="${OCTOMX_PATHCONV:-cat}"; else EXE="$BUILD/bin/octomx"; PATHCONV=cat; fi
conv() { if [ "$PATHCONV" = cat ]; then echo "$1"; else $PATHCONV "$1"; fi; }

# shot <name> <screenWxH> <scale%> <clientWxH> <settings-json-or-empty> <script lines...>
shot() {
    local name=$1 screen=$2 scale=$3 size=$4 settings=$5; shift 5
    if [ -n "${ONLY:-}" ] && ! [[ $name =~ $ONLY ]]; then return; fi
    local prof=$WORK/prof-$name
    mkdir -p "$prof"
    [ -n "$settings" ] && echo "$settings" > "$prof/settings.json"
    local script=$WORK/$name.script
    : > "$script"
    for l in "$@"; do echo "$l" >> "$script"; done
    echo "shot $(conv "$OUT/$name.png")" >> "$script"
    echo "quit" >> "$script"
    local sw=${screen%x*} sh=${screen#*x}
    if [ $WINE = 0 ]; then
        # the screen's logical DPI is the "system scale": Automatic must pick the same factor
        local dpi=$(( 96 * scale / 100 ))
        cat > "$WORK/screen-$name.json" <<JSON
{ "synchronousWindowSystemEvents": false, "windowFrameMargins": false,
  "screens": [ { "name": "shot", "x": 0, "y": 0, "width": $sw, "height": $sh,
                 "logicalDpi": $dpi, "logicalBaseDpi": 96, "dpr": 1 } ] }
JSON
        if ! QT_QPA_PLATFORM="offscreen:configfile=$WORK/screen-$name.json" timeout 90 "$EXE" --profile "$prof" \
                --demo-frozen --size "$size" --script "$script" > "$WORK/$name.log" 2>&1; then
            echo "FAILED: $name (see $WORK/$name.log)"; tail -3 "$WORK/$name.log"; return
        fi
    else
        if ! MX_UI_SCALE=$scale timeout 120 $EXE --profile "$(conv "$prof")" --demo-frozen --size "$size" \
                --script "$(conv "$script")" > "$WORK/$name.log" 2>&1; then
            echo "FAILED: $name (see $WORK/$name.log)"; tail -3 "$WORK/$name.log"; return
        fi
    fi
    echo "ok: $name.png"
}


SEARCH=("page search" "type searchText linux" "key searchText Return" "wait 700" "expand-row searchResults 0" "wait 200")
G=${SETS:-.}
# design size x scale: the client is the design size times the scale (what the owner sees at 150 %)
CONFIGS=("800x600_100:800x600:100:800x600"
         "1024x768_100:1024x768:100:1024x768"
         "1920x1080_100:1920x1080:100:1920x1040"
         "800x600_150:1200x900:150:1200x900"
         "1024x768_150:1536x1152:150:1536x1152"
         "1920x1080_150:2880x1620:150:2880x1560"
         "800x600_200:1600x1200:200:1600x1200"
         "1024x768_200:2048x1536:200:2048x1536"
         "1920x1080_200:3840x2160:200:3840x2080")
for c in "${CONFIGS[@]}"; do
    IFS=: read -r tag screen scale size <<< "$c"
    if [[ search =~ $G ]]; then
        shot "${tag}_search"      "$screen" "$scale" "$size" "" "${SEARCH[@]}"
        shot "${tag}_searchvideo" "$screen" "$scale" "$size" "" "page search" "click searchVideo" "type searchText sintel" "key searchText Return" "wait 700"
        shot "${tag}_searchfilter" "$screen" "$scale" "$size" "" "${SEARCH[@]}" "filter searchResults ubuntu -server" "wait 300"
    fi
    if [[ lists =~ $G ]]; then
        shot "${tag}_transfers" "$screen" "$scale" "$size" "" "page transfers" "wait 400"
        shot "${tag}_shared"    "$screen" "$scale" "$size" "" "page shared" "wait 400" "expand-row sharedList 0"
        shot "${tag}_chat"      "$screen" "$scale" "$size" "" "page chat" "wait 300"
        shot "${tag}_hotlist"   "$screen" "$scale" "$size" "" "page search" "hotlist on" "wait 300"
    fi
done
for c in "1024x768_100:1024x768:100:1024x768" "1024x768_150:1536x1152:150:1536x1152" "1024x768_200:2048x1536:200:2048x1536"; do
    IFS=: read -r tag screen scale size <<< "$c"
    if [[ menus =~ $G ]]; then
        shot "${tag}_searchmenu" "$screen" "$scale" "$size" "" "${SEARCH[@]}" "context-menu searchResults 1" "wait 200"
        shot "${tag}_dlmenu"     "$screen" "$scale" "$size" "" "page transfers" "wait 300" "context-menu-text downloadList Big Buck Bunny 1080p.mkv" "wait 200"
        shot "${tag}_hotmenu"    "$screen" "$scale" "$size" "" "page search" "hotlist on" "wait 300" "context-menu hotlistList 1" "wait 200"
    fi
    if [[ tips =~ $G ]]; then
        shot "${tag}_tipafs"   "$screen" "$scale" "$size" "" "page search" "tooltip searchAfs" "wait 300"
        shot "${tag}_tipload"  "$screen" "$scale" "$size" "" "page transfers" "tooltip dlLoadIncomplete" "wait 300"
    fi
done
echo "shots in $OUT"
