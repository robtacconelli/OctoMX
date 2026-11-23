#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# tools/shots-ui-e.sh: screenshots of the ui-e work: every page at 800x600, 1024x768, 1920x1080 (100 %),
# 2560x1440 at 150 % and 3840x2160 at 200 % (interface scale "Automatic" from the screen's DPI), plus the
# new controls (column chooser, filter, search history, Open File menu, Settings ▸ Interface, wizard).
# Frozen demo core. Owner: ui-e.
#
#   tools/shots-ui-e.sh <out-dir> [build-dir]          (build-dir default: build/ui-e-linux)
#   OCTOMX_EXE="wine dist/ui-e/octomx/octomx.exe" OCTOMX_PATHCONV="winepath -w" tools/shots-ui-e.sh <out-dir>
#       (under wine the scale comes from MX_UI_SCALE, set per shot; the screen is the Xvnc display)
#   ONLY=<regex> limits the shots.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUT=$(realpath -m "${1:?usage: tools/shots-ui-e.sh <out-dir> [build-dir]}")
BUILD=${2:-$ROOT/build/ui-e-linux}
mkdir -p "$OUT"
WORK=$OUT/.work-e
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
CONFIGS=("800x600:800x600:100:800x600"
         "1024x768:1024x768:100:1024x768"
         "1920x1080:1920x1080:100:1920x1040"
         "2560x1440_150:2560x1440:150:2560x1400"
         "3840x2160_200:3840x2160:200:3840x2110")
for c in "${CONFIGS[@]}"; do
    IFS=: read -r tag screen scale size <<< "$c"
    shot "${tag}_networks"  "$screen" "$scale" "$size" "" "page networks" "wait 300"
    shot "${tag}_other"     "$screen" "$scale" "$size" "" "page networks" "subtab other" "wait 300"
    shot "${tag}_search"    "$screen" "$scale" "$size" "" "${SEARCH[@]}"
    shot "${tag}_transfers" "$screen" "$scale" "$size" "" "page transfers" "wait 400"
    shot "${tag}_shared"    "$screen" "$scale" "$size" "" "page shared" "wait 400"
    shot "${tag}_chat"      "$screen" "$scale" "$size" "" "page chat" "wait 300"
    shot "${tag}_bandwidth" "$screen" "$scale" "$size" "" "page bandwidth" "wait 400"
    shot "${tag}_settings"  "$screen" "$scale" "$size" "" "page networks" "settings appearance/interface" "wait 400"
done
# the new controls (1024x768 at 100 %, and the same at 150 % on a 2560x1440 screen)
for c in "1024x768:1024x768:100:1024x768" "2560x1440_150:2560x1440:150:2560x1400"; do
    IFS=: read -r tag screen scale size <<< "$c"
    shot "${tag}_colmenu"   "$screen" "$scale" "$size" "" "${SEARCH[@]}" "header-menu searchResults" "wait 200"
    shot "${tag}_colpeers"  "$screen" "$scale" "$size" "" "${SEARCH[@]}" "column-visible searchResults 9 on" "sort searchResults 6 desc" "wait 200"
    shot "${tag}_filter"    "$screen" "$scale" "$size" "" "${SEARCH[@]}" "filter searchResults debian" "wait 300"
    shot "${tag}_history"   "$screen" "$scale" "$size" "" "page search" "search-history" "wait 200"
    shot "${tag}_searchmenu" "$screen" "$scale" "$size" "" "${SEARCH[@]}" "context-menu searchResults 1" "wait 200"
    shot "${tag}_dlmenu"    "$screen" "$scale" "$size" "" "page transfers" "wait 300" "context-menu-text downloadList Big Buck Bunny 1080p.mkv" "wait 200"
    shot "${tag}_sharedmenu" "$screen" "$scale" "$size" "" "page shared" "wait 300" "expand-row sharedList 0" "context-menu sharedList 1" "wait 200"
    shot "${tag}_exact"     "$screen" "$scale" "$size" '{"ui/exactSizes":true}' "page transfers" "wait 400"
    shot "${tag}_wizard"    "$screen" "$scale" "$size" "" "page networks" "wizard" "wait 400"
    shot "${tag}_about"     "$screen" "$scale" "$size" "" "page networks" "click btnHelp" "wait 400"
done
# Automatic on a 2560-wide screen at 100 % system scale: 125 %
shot "2560x1440_auto100_search" "2560x1440" 100 "2560x1400" "" "${SEARCH[@]}"
echo "shots in $OUT"
