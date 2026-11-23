#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# tools/shots.sh: take one PNG per page/state of the OctoMX UI (demo core, frozen data).
# Builds nothing. Owner: ui-a.
#
#   tools/shots.sh <out-dir> [build-dir]          (build-dir default: build/ui-a-linux)
#   OCTOMX_EXE="wine dist/x/octomx/octomx.exe" tools/shots.sh <out-dir>   (any launcher)
#
# Linux: runs with QT_QPA_PLATFORM=offscreen on a 1920x1200 virtual screen, LC_ALL=C.UTF-8.
# Client sizes match the key references:
#   ref1_wikipedia_1024x709.jpg          -> 1024x689 (networks_wpn)
#   web/search_idle_354b4_wikipedia.png  -> 1024x714 (search_idle)
#   web/search_results_322_native.gif    ->  841x660 (search_artist)
#   web/transfers_page_354b3.png         ->  690x445 (transfers_690) + 1024x689 variants
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUT=$(realpath -m "${1:?usage: tools/shots.sh <out-dir> [build-dir]}")
BUILD=${2:-$ROOT/build/ui-a-linux}
mkdir -p "$OUT"
WORK=$OUT/.work
rm -rf "$WORK"; mkdir -p "$WORK"

if [ -z "${OCTOMX_EXE:-}" ]; then
    EXE="$BUILD/bin/octomx"
    cat > "$WORK/offscreen.json" <<'EOF'
{ "synchronousWindowSystemEvents": false, "windowFrameMargins": false,
  "screens": [ { "name": "shot", "x": 0, "y": 0, "width": 1920, "height": 1200,
                 "logicalDpi": 96, "logicalBaseDpi": 96, "dpr": 1 } ] }
EOF
    export QT_QPA_PLATFORM="offscreen:configfile=$WORK/offscreen.json"
    PATHCONV=cat
else
    EXE="$OCTOMX_EXE"
    PATHCONV="${OCTOMX_PATHCONV:-cat}"   # e.g. "winepath -w" for wine
fi
export LC_ALL=C.UTF-8
export MX_DIGIT_GROUPING=space   # same digit grouping everywhere (wine ignores LC_ALL)
conv() { if [ "$PATHCONV" = cat ]; then echo "$1"; else $PATHCONV "$1"; fi; }

# shot <name> <WxH> <settings-json-or-empty> <script lines...>
shot() {
    local name=$1 size=$2 settings=$3; shift 3
    local prof=$WORK/prof-$name
    mkdir -p "$prof"
    [ -n "$settings" ] && echo "$settings" > "$prof/settings.json"
    local script=$WORK/$name.script
    : > "$script"
    for l in "$@"; do echo "$l" >> "$script"; done
    echo "shot $(conv "$OUT/$name.png")" >> "$script"
    echo "quit" >> "$script"
    if ! timeout 60 $EXE --profile "$(conv "$prof")" --demo-frozen --size "$size" \
            --script "$(conv "$script")" > "$WORK/$name.log" 2>&1; then
        echo "FAILED: $name (see $WORK/$name.log)"; tail -3 "$WORK/$name.log"
    else
        echo "ok: $name.png"
    fi
}

MP3='{"search/defaultType":"MP3 Audio - Any Bitrate","ui/settingsMigrated":1}'
shot networks_wpn      1024x689 "" "page networks" "subtab wpn" "wait 200"
shot networks_other    1024x689 "" "page networks" "subtab other" "wait 200"
shot networks_menu     1024x689 "" "page networks" "subtab other" "context-menu serverList 5" "wait 200"
shot search_idle       1024x714 "$MP3" "page search" "type searchText Linux" "wait 200"
shot search_artist       841x660  "$MP3" "page search" "type searchText velorine" "key searchText Return" "wait 600"
shot search_artist_1024  1024x714 "$MP3" "page search" "type searchText velorine" "key searchText Return" "wait 600" \
                                     "expand-row searchResults 0" "select-row searchResults 2" "wait 200"
shot search_menu       1024x714 "$MP3" "page search" "type searchText velorine" "key searchText Return" "wait 600" \
                                     "context-menu searchResults 13" "wait 200"
shot search_filetype   1024x714 "$MP3" "page search" "click searchFileType" "wait 200" "menu-pick MP3 only (.mp3)" "wait 200"
shot search_linux      1024x714 "" "page search" "type searchText linux" "key searchText Return" "wait 600" \
                                     "expand-row searchResults 0" "wait 200"
shot transfers_690     690x445  "" "page transfers" "wait 300"
shot transfers         1024x689 "" "page transfers" "wait 300"
shot transfers_menu    1024x689 "" "page transfers" "context-menu downloadList 1" "wait 200"
shot transfers_hotlist 1024x689 "" "page transfers" "hotlist on" "wait 300"
shot hotlist_menu      1024x689 "" "page transfers" "hotlist on" "wait 200" "context-menu hotlistList 1" "wait 200"
shot float_transfers  1024x689 "" "page transfers" "float transfers" "wait 300"
shot shared_placeholder 1024x689 "" "page shared" "wait 200"
shot about             1024x689 "" "page networks" "click btnHelp" "wait 400"
echo "shots in $OUT"
