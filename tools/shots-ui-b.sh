#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# tools/shots-ui-b.sh: screenshots of the pages/windows built by worker ui-b (Shared Files, Chat,
# Bandwidth, Browse, Whois, Private Message, Ignored Users) on the frozen demo core.
# Same conventions as tools/shots.sh (owner ui-a):
#   tools/shots-ui-b.sh <out-dir> [build-dir]          (build-dir default: build/ui-b-linux)
#   OCTOMX_EXE="wine dist/x/octomx/octomx.exe" OCTOMX_PATHCONV="winepath -w" tools/shots-ui-b.sh <out-dir>
# Client sizes match the references:
#   web/shared_files_page_columns_353.png -> 792x600 (shared_792; ref client offset 2,21)
#   web/chat_channel_list_354b3.png       -> 651x537 (chat_list_651; ref client offset 1,19)
#   web/chat_room_window.png              -> 1013x707 (chat_room_1013; ref = page area)
#   web/bandwidth_graph_page.png          -> 901x629 (bandwidth_901)
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUT=$(realpath -m "${1:?usage: tools/shots-ui-b.sh <out-dir> [build-dir]}")
BUILD=${2:-$ROOT/build/ui-b-linux}
mkdir -p "$OUT"
WORK=$OUT/.work
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

NOWARN='{"ui/chatWarningDisabled":true}'
HELP="OctoMX Help_49717657BABE"
shot shared_792        792x600  "" "page shared" "wait 200"
shot shared_expanded   1024x689 "" "page shared" "expand-row sharedList 0" "expand-row sharedList 4" "select-row sharedList 6" "wait 200"
shot shared_menu       792x600  "" "page shared" "context-menu sharedList 0" "wait 200"
shot shared_unshare    792x600  "" "page shared" "click sharedUnshareFolder" "wait 200"
shot shared_types      792x600  "" "page shared" "click sharedFileTypes" "wait 300"
shot chat_warning      1024x689 "" "page chat" "wait 400"
shot chat_list_651     651x537  "$NOWARN" "page chat" "wait 300" "select-row channelList 2" "wait 100"
shot chat_list_menu    1024x689 "$NOWARN" "page chat" "wait 300" "context-menu channelList 3" "wait 200"
shot chat_favorites    1024x689 "$NOWARN" "page chat" "wait 300" "click chatFavorites" "wait 200"
shot chat_room_1013    1013x707 "$NOWARN" "page chat" "wait 200" "join $HELP" "wait 300"
shot chat_room_1024    1024x689 "$NOWARN" "page chat" "wait 200" "join $HELP" "wait 200" "join Linux Users" "wait 200" \
                                         "say hello from the demo" "say /me waves" "wait 200"
shot chat_commands     1024x689 "$NOWARN" "page chat" "wait 200" "join $HELP" "wait 200" "click chatCommands" "wait 200" \
                                         "menu-pick Admin Channel Config Commands" "wait 200"
shot chat_user_menu    1024x689 "$NOWARN" "page chat" "wait 200" "join $HELP" "wait 200" "context-menu chatUserList 4" "wait 200"
shot chat_listmenu     1024x689 "$NOWARN" "page chat" "wait 200" "click chatListChannels" "wait 200"
shot bandwidth_901     901x629  "" "page bandwidth" "wait 300"
shot bandwidth_1024    1024x689 "" "page bandwidth" "wait 300"
shot browse            1024x689 "" "page transfers" "open-browse zqvtra662_44120" "wait 400"
shot whois             1024x689 "" "page transfers" "open-whois Twirl836_24616" "wait 400"
shot pm                1024x689 "" "page transfers" "open-pm helping_holly_88120" "pm-say helping_holly_88120 hi holly, yes that is me" "wait 400"
shot ignored           1024x689 "" "page networks" "click btnIgnored" "wait 300" "select-row ignoredList 1" "wait 100"
shot hotlist_whois     1024x689 "" "page transfers" "hotlist on" "wait 200" "context-menu hotlistList 1" "wait 100" "menu-pick Whois" "wait 400"
echo "shots in $OUT"
