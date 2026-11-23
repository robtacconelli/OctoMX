#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# tools/shots-demo-1.sh: one picture of every page of the built-in demo (--demo-frozen), offscreen,
# 100 %, to read every name the demo shows (no real artist, title or user may appear). Builds nothing.
#
#   tools/shots-demo-1.sh <out-dir> [build-dir]          (build-dir default: build/linux)
#
# Pictures: networks, search (the artist search, its group opened, Linux, the open films, a free
# text search), transfers, shared files, chat (channel list, help room, a busy room), hotlist,
# bandwidth, a browse window and a whois window (saved next to the main picture as
# <name>.<window>.png).
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUT=$(realpath -m "${1:?usage: tools/shots-demo-1.sh <out-dir> [build-dir]}")
BUILD=${2:-$ROOT/build/linux}
mkdir -p "$OUT"
WORK=$OUT/.work
rm -rf "$WORK"; mkdir -p "$WORK"
cat > "$WORK/offscreen.json" <<'EOF'
{ "synchronousWindowSystemEvents": false, "windowFrameMargins": false,
  "screens": [ { "name": "shot", "x": 0, "y": 0, "width": 1920, "height": 1200,
                 "logicalDpi": 96, "logicalBaseDpi": 96, "dpr": 1 } ] }
EOF
export QT_QPA_PLATFORM="offscreen:configfile=$WORK/offscreen.json"
export LC_ALL=C.UTF-8 MX_DIGIT_GROUPING=space MX_OS_INTEGRATION=fake MX_FAKE_TRAY=1
EXE=$BUILD/bin/octomx

# shot <name> <WxH> <settings-json-or-empty> <script lines...>
shot() {
    local name=$1 size=$2 settings=$3; shift 3
    local prof=$WORK/prof-$name
    mkdir -p "$prof"
    [ -n "$settings" ] && echo "$settings" > "$prof/settings.json"
    local script=$WORK/$name.script
    : > "$script"
    for l in "$@"; do echo "$l" >> "$script"; done
    printf 'shot %s\nquit\n' "$OUT/$name.png" >> "$script"
    if ! timeout 60 "$EXE" --profile "$prof" --demo-frozen --no-wizard --size "$size" \
            --script "$script" > "$WORK/$name.log" 2>&1; then
        echo "FAILED: $name (see $WORK/$name.log)"; tail -3 "$WORK/$name.log"
    else
        echo "ok: $name.png"
    fi
}

NW='{"ui/chatWarningDisabled":true,"ui/settingsMigrated":1}'
MP3='{"search/defaultType":"MP3 Audio - Any Bitrate","ui/settingsMigrated":1,"ui/chatWarningDisabled":true}'
shot networks        1024x689 "$NW"  "page networks" "subtab wpn" "wait 200"
shot search_artist   1024x714 "$MP3" "page search" "type searchText velorine" "key searchText Return" "wait 600"
shot search_artist2  1024x714 "$MP3" "page search" "type searchText velorine" "key searchText Return" "wait 600" \
                                     "sort searchResults 0 asc" "wait 100"
shot search_group    1024x714 "$MP3" "page search" "type searchText velorine" "key searchText Return" "wait 600" \
                                     "expand-row searchResults 0" "select-row searchResults 2" "wait 200"
shot search_linux    1024x714 "$NW"  "page search" "type searchText linux" "key searchText Return" "wait 600"
shot search_films    1024x714 "$NW"  "page search" "type searchText sintel" "key searchText Return" "wait 600"
shot search_free     1024x714 "$NW"  "page search" "type searchText gooseberry" "key searchText Return" "wait 600"
shot transfers       1024x689 "$NW"  "page transfers" "wait 300"
shot shared          1024x689 "$NW"  "page shared" "wait 300"
shot shared_open     1024x1200 "$NW" "page shared" "wait 200" "expand-row sharedList 9" "expand-row sharedList 4" \
                                     "expand-row sharedList 3" "expand-row sharedList 2" "expand-row sharedList 1" \
                                     "expand-row sharedList 0" "wait 200"
shot shared_jazz     1024x1200 "$NW" "page shared" "wait 200" "expand-row sharedList 5" "wait 200"
shot chat_list       1024x689 "$NW"  "page chat" "wait 300"
shot chat_help       1024x689 "$NW"  "page chat" "wait 200" "join OctoMX Help_49717657BABE" "wait 300"
shot chat_room       1024x689 "$NW"  "page chat" "wait 200" "join The Jazz Lounge" "wait 300"
shot hotlist         1024x689 "$NW"  "page transfers" "hotlist on" "wait 300"
shot bandwidth       1024x689 "$NW"  "page bandwidth" "wait 300"
shot browse          1024x689 "$NW"  "page transfers" "open-browse zqvtra662_44120" "wait 400"
shot whois           1024x689 "$NW"  "page transfers" "open-whois Twirl836_24616" "wait 400"
echo "shots in $OUT"
