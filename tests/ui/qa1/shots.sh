#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# tests/ui/qa1/shots.sh: [qa-1] the extra states needed to compare every reference image (demo core,
# frozen data). Complements tools/shots.sh / shots-ui-{b,c,d}.sh.
#
#   tests/ui/qa1/shots.sh <out-dir> [build-dir]          (build-dir default: build/qa1-linux)
#   OCTOMX_EXE="wine dist/x/octomx/octomx.exe" OCTOMX_PATHCONV="winepath -w" tests/ui/qa1/shots.sh <out-dir>
#
# Every popup menu is also saved on its own as <shot>.menuN.png, every open dialog as <shot>.<objectName>.png.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
OUT=$(realpath -m "${1:?usage: tests/ui/qa1/shots.sh <out-dir> [build-dir]}")
BUILD=${2:-$ROOT/build/qa1-linux}
mkdir -p "$OUT"
WORK=$OUT/.work
mkdir -p "$WORK"
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
    PATHCONV="${OCTOMX_PATHCONV:-cat}"
fi
export LC_ALL=C.UTF-8 MX_DIGIT_GROUPING=space MX_OS_INTEGRATION=fake MX_FAKE_TRAY=1
conv() { if [ "$PATHCONV" = cat ]; then echo "$1"; else $PATHCONV "$1"; fi; }

# shot <name> <WxH> <settings-json-or-empty> <script lines...>   (a final "shot <name>" is added)
shot() {
    local name=$1 size=$2 settings=$3; shift 3
    [[ -n "${ONLY:-}" && ! "$name" =~ $ONLY ]] && return 0   # ONLY=<regex> limits the run
    local prof=$WORK/prof-$name
    mkdir -p "$prof"
    [ -n "$settings" ] && echo "$settings" > "$prof/settings.json"
    local script=$WORK/$name.script
    : > "$script"
    for l in "$@"; do echo "$l" >> "$script"; done
    echo "shot $(conv "$OUT/$name.png")" >> "$script"
    echo "quit" >> "$script"
    if ! timeout 90 $EXE --profile "$(conv "$prof")" --demo-frozen --no-wizard --size "$size" \
            --script "$(conv "$script")" > "$WORK/$name.log" 2>&1; then
        echo "FAILED: $name (see $WORK/$name.log)"; tail -3 "$WORK/$name.log"
    else
        echo "ok: $name"
    fi
}

NW='{"ui/chatWarningDisabled":true}'
MP3='{"search/defaultType":"MP3 Audio - Any Bitrate","ui/settingsMigrated":1,"ui/chatWarningDisabled":true}'
ARTIST=("page search" "type searchText velorine" "key searchText Return" "wait 600")
FERRY="Gooseberry Big Band - Moonlit"

shot networks_ref1    1024x689 '{"identity/primaryConnection":true,"ui/chatWarningDisabled":true}' "page networks" "subtab wpn" "wait 200"
shot networks_649     647x542  "$NW" "page networks" "subtab wpn" "wait 200"
shot networks_other   1024x689 "$NW" "page networks" "subtab other" "wait 200"
shot search_idle      1024x714 "$MP3" "page search" "type searchText Linux" "key searchText Return" "wait 300"
shot search_artist      841x660  "$MP3" "${ARTIST[@]}"
shot search_nonmp3    1024x714 "$NW" "page search" "type searchText health" "key searchText Return" "wait 600" "expand-row searchResults 0" "select-row searchResults 1" "wait 100"
shot search_networks  1024x714 "$MP3" "page search" "click searchNetworks" "wait 200"
shot search_connection 1024x714 "$MP3" "page search" "click searchConnection" "wait 200"
shot search_afs       1024x714 "$MP3" "page search" "click searchAfs" "wait 200"
shot search_aeq       1024x714 "$MP3" "page search" "click searchAeq" "wait 200"
shot search_bitrate   1024x714 "$MP3" "page search" "click searchFileType" "wait 100" "menu-pick MP3 only (.mp3)" "wait 100" "menu-pick Bitrate at least" "wait 200"
shot search_squeezed  700x500  "$MP3" "${ARTIST[@]}"
shot search_dangerous 1024x714 "$NW" "page search" "type searchText big buck bunny" "key searchText Return" "wait 600" \
                                    "dblclick-text searchResults .avi.exe" "wait 400"
shot transfers_690    690x445  "$NW" "page transfers" "wait 300"
shot transfers_1024   1024x689 "$NW" "page transfers" "wait 300"
shot transfers_filemenu 1024x689 "$NW" "page transfers" "context-menu-text downloadList $FERRY" "wait 200"
shot transfers_afsmenu 1024x689 "$NW" "page transfers" "context-menu-text downloadList $FERRY" "wait 100" "menu-pick Auto Find Sources - 10m" "wait 200"
shot transfers_delete 1024x689 "$NW" "page transfers" "context-menu-text downloadList $FERRY" "wait 100" "menu-pick Delete File" "wait 400"
shot transfers_dlafs  1024x689 "$NW" "page transfers" "click dlAfs" "wait 200"
shot hotlist_pane     1024x689 "$NW" "page transfers" "hotlist on" "wait 300"
shot hotlist_options  1024x689 "$NW" "page transfers" "hotlist on" "wait 200" "click hotlistOptions" "wait 200"
shot hotlist_usermenu 1024x689 "$NW" "page transfers" "hotlist on" "wait 200" "context-menu hotlistList 1" "wait 200"
shot system_menu      1024x689 "$NW" "page networks" "system-menu" "wait 200"
shot tray_menu        1024x689 "$NW" "page networks" "tray-menu" "wait 200"
shot close_prompt     1024x689 "$NW" "page networks" "close-window" "wait 400"
shot chat_warning     1024x689 "" "page chat" "wait 400"
shot chat_list_655    651x537  "$NW" "page chat" "wait 300" "select-row channelList 2" "wait 100"
shot chat_room_1013   1013x707 "$NW" "page chat" "wait 200" "join OctoMX Help_49717657BABE" "wait 300"
shot chat_commands    1024x689 "$NW" "page chat" "wait 200" "join OctoMX Help_49717657BABE" "wait 200" "click chatCommands" "wait 200"
shot chat_admin       1024x689 "$NW" "page chat" "wait 200" "join OctoMX Help_49717657BABE" "wait 200" "click chatCommands" "wait 100" "menu-pick Admin Channel Config Commands" "wait 200"
shot chat_listchannels 1024x689 "$NW" "page chat" "wait 200" "click chatListChannels" "wait 200"
shot shared_796       796x600  "$NW" "page shared" "wait 200"
shot shared_menu      796x600  "$NW" "page shared" "context-menu sharedList 0" "wait 200"
shot shared_browse    796x600  "$NW" "page shared" "click sharedShareFolder" "wait 400"
shot shared_types     796x600  "$NW" "page shared" "click sharedFileTypes" "wait 300"
shot bandwidth_901    901x629  "$NW" "page bandwidth" "wait 300"
shot browse           1024x689 "$NW" "page transfers" "open-browse zqvtra662_44120" "wait 400"
shot ignored          1024x689 "$NW" "page networks" "click btnIgnored" "wait 300"
shot about            1024x689 "$NW" "page networks" "click btnHelp" "wait 400"
shot wizard0          1024x689 "$NW" "wizard" "wait 300"
shot wizard_conn      1024x689 "$NW" "wizard" "wait 200" "click wizCustom" "click wizNext" "wait 100" "type wizName Tester" "click wizNext" "wait 300"
shot auto_responders  1024x689 '{"ui/chatWarningDisabled":true,"ui/autoResponders":[["bed","Sorry I have collapsed of tiredness on my keyboard.. leave me a message"],["gone","out"],["busy","busy"]]}' \
                               "page transfers" "hotlist on" "wait 200" "click hotlistOptions" "wait 100" "menu-pick Auto Responder" "wait 100" "menu-pick Edit Auto Responders..." "wait 300"
shot min_size         400x300  "$NW" "page search" "wait 200"
shot big_1920         1920x1080 "$NW" "page transfers" "wait 300"
shot file_download    1024x714 "$MP3" "${ARTIST[@]}" "select-row searchResults 12" "click searchManualResume" "wait 400"
for p in appearance appearance/channel appearance/bandwidth connection connection/outgoing connection/incoming \
         wpn other messages library channels search transfers transfers/queuing \
         throttle launching misc; do
    shot "settings_${p//\//_}" 1024x689 '{"ui/chatWarningDisabled":true,"transfers/incomingDir":"E:/New"}' "settings $p" "wait 200"
done
echo "shots in $OUT"
