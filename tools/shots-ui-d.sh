#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# tools/shots-ui-d.sh: screenshots of the ui-d work (new controls, fonts, small window sizes),
# frozen demo core. Owner: ui-d.
#
#   tools/shots-ui-d.sh <out-dir> [build-dir]          (build-dir default: build/ui-d-linux)
#   OCTOMX_EXE="wine dist/ui-d/octomx/octomx.exe" OCTOMX_PATHCONV="winepath -w" tools/shots-ui-d.sh <out-dir>
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUT=$(realpath -m "${1:?usage: tools/shots-ui-d.sh <out-dir> [build-dir]}")
BUILD=${2:-$ROOT/build/ui-d-linux}
mkdir -p "$OUT"
WORK=$OUT/.work-d
rm -rf "$WORK"; mkdir -p "$WORK"

if [ -z "${OCTOMX_EXE:-}" ]; then
    EXE="$BUILD/bin/octomx"
    cat > "$WORK/offscreen.json" <<'JSON'
{ "synchronousWindowSystemEvents": false, "windowFrameMargins": false,
  "screens": [ { "name": "shot", "x": 0, "y": 0, "width": 1920, "height": 1200,
                 "logicalDpi": 96, "logicalBaseDpi": 96, "dpr": 1 } ] }
JSON
    export QT_QPA_PLATFORM="offscreen:configfile=$WORK/offscreen.json"
    PATHCONV=cat
else
    EXE="$OCTOMX_EXE"
    PATHCONV="${OCTOMX_PATHCONV:-cat}"
fi
export LC_ALL=C.UTF-8
export MX_DIGIT_GROUPING=space
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

S='{"ui/chatWarningDisabled":true}'
# ---- new controls
shot d_search_suspects 1024x714 "$S" "page search" "type searchText big buck bunny" "key searchText Return" "wait 600" \
                                     "select-text searchResults DVDRip.avi.rar" "wait 200"
shot d_search_ping     1024x714 "$S" "page search" "type searchText velorine" "key searchText Return" "wait 600" \
                                     "click searchPingAll" "wait 300"
shot d_search_hash     1024x714 "$S" "page search" \
     "type searchText magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567&dn=Sintel.2010.720p.mkv&xl=652087296" \
     "key searchText Return" "wait 600" "expand-row searchResults 0" "wait 200"
shot d_file_download   1024x714 "$S" "page search" "type searchText velorine" "key searchText Return" "wait 600" \
                                     "select-text searchResults Velvet Engine" "click searchManualResume" "wait 300"
shot d_load_incomplete 1024x689 "$S" "page transfers" "click dlLoadIncomplete" "wait 300"
shot d_source_menu     1024x689 "$S" "page transfers" "context-menu-text downloadList Bobwib234" "wait 100" \
                                     "menu-pick Bandwidth Priority" "wait 200"
shot d_upload_menu     1024x689 "$S" "page transfers" "context-menu-text uploadList Velvet Engine" "wait 200"
shot d_upload_prio     1024x689 "$S" "page transfers" "context-menu-text uploadList Air On The G String" "wait 100" \
                                     "menu-pick Bandwidth Priority" "wait 200"
shot d_server_msgs     1024x689 "$S" "page networks" "subtab other" "context-menu-text serverList eMule Security" "wait 100" \
                                     "menu-pick View Server Messages" "wait 300"
shot d_list_channels   1024x689 "$S" "page chat" "click chatListChannels" "wait 200"
shot d_gnut_channels   1024x689 "$S" "page chat" "click chatListChannels" "wait 100" "menu-pick Gnutella" "wait 300"
shot d_shared_types    792x600  "$S" "page shared" "click sharedFileTypes" "wait 300"
shot d_shared_792      792x600  "$S" "page shared" "wait 200"
shot d_transfers_1024  1024x689 "$S" "page transfers" "wait 300"
shot d_bandwidth       1024x689 "$S" "page bandwidth" "wait 300"
shot d_networks_status 1024x689 '{"ui/chatWarningDisabled":true,"identity/primaryConnection":true}' "page networks" "subtab wpn" "wait 200"
# ---- fonts (Settings > Appearance): bigger dynamic (lists) and normal fonts, a different large font
F='{"ui/chatWarningDisabled":true,"ui/fontDynamic":"Tahoma,10","ui/fontNormal":"Tahoma,9","ui/fontLarge":"Tahoma,12,bold"}'
shot d_fonts_transfers 1024x689 "$F" "page transfers" "wait 300"
shot d_fonts_search    1024x714 "$F" "page search" "type searchText velorine" "key searchText Return" "wait 600" \
                                     "context-menu searchResults 3" "wait 200"
shot d_fonts_chat      1024x689 "$F" "page chat" "wait 200" "join OctoMX Help_49717657BABE" "wait 300"
# ---- every page at 800x600 and 1024x768
for size in 800x600 1024x768; do
    shot "d_${size}_networks_wpn"   $size "$S" "page networks" "subtab wpn" "wait 200"
    shot "d_${size}_networks_other" $size "$S" "page networks" "subtab other" "wait 200"
    shot "d_${size}_shared"         $size "$S" "page shared" "wait 200"
    shot "d_${size}_chat_list"      $size "$S" "page chat" "subtab list" "wait 200"
    shot "d_${size}_chat_room"      $size "$S" "page chat" "wait 200" "join OctoMX Help_49717657BABE" "wait 300"
    shot "d_${size}_search"         $size "$S" "page search" "type searchText velorine" "key searchText Return" "wait 600"
    shot "d_${size}_transfers"      $size "$S" "page transfers" "wait 300"
    shot "d_${size}_bandwidth"      $size "$S" "page bandwidth" "wait 300"
    shot "d_${size}_hotlist"        $size "$S" "page transfers" "hotlist on" "wait 300"
done
echo "shots in $OUT"
