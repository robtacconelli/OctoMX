#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# tools/shots-post-1.sh: screenshots of the post-1 work: the close prompt and the close /
# minimise settings, every page and the main dialogs and menus in the Dark and the Light colour
# theme, the Settings button / tray "Color Theme" menus, the Help menu, the About box and the update
# box (from release files, never GitHub). 100 % (1024x714 client) and 200 %
# (2048x1428), plus 800x600 for the main window. Frozen demo core.
#
#   tools/shots-post-1.sh <out-dir> [build-dir]          (build-dir default: build/post-1)
#   OCTOMX_EXE="wine pkg/octomx/octomx.exe" OCTOMX_PATHCONV="winepath -w" DISPLAY=:N tools/shots-post-1.sh <out-dir>
#   ONLY=<regex> limits the shots.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUT=$(realpath -m "${1:?usage: tools/shots-post-1.sh <out-dir> [build-dir]}")
BUILD=${2:-$ROOT/build/post-1}
mkdir -p "$OUT/theme"
WORK=$OUT/.work-p1
rm -rf "$WORK"; mkdir -p "$WORK"
export LC_ALL=C.UTF-8
export MX_DIGIT_GROUPING=space
export MX_FAKE_TRAY=1
export MX_LAUNCH_LOG=$WORK/launch.log   # "Open Download Page" records instead of opening a browser
WINE=0
if [ -n "${OCTOMX_EXE:-}" ]; then WINE=1; EXE="$OCTOMX_EXE"; PATHCONV="${OCTOMX_PATHCONV:-cat}"; else EXE="$BUILD/bin/octomx"; PATHCONV=cat; fi
conv() { if [ "$PATHCONV" = cat ]; then echo "$1"; else $PATHCONV "$1"; fi; }

# two "releases/latest" answers (files, read by the script verb update-show)
SRV=$WORK/srv
mkdir -p "$SRV"
cat > "$SRV/newer" <<'JSON'
{"tag_name":"v5.4.0","name":"OctoMX 5.4","draft":false,"prerelease":false,
 "html_url":"https://github.com/robtacconelli/OctoMX/releases/tag/v5.4.0",
 "body":"## What's new in 5.4\n\n* Example entry one (demo text from the screenshot script)\n* Example entry two\n* Example entry three\n\nFull notes on the release page."}
JSON
cat > "$SRV/same" <<JSON
{"tag_name":"v$(sed -n 's/^project(OctoMX VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")","name":"same","draft":false,"prerelease":false,"html_url":"https://github.com/robtacconelli/OctoMX/releases","body":""}
JSON

# shot <name> <screenWxH> <scale%> <clientWxH> <settings-json-or-empty> <script lines...>
shot() {
    local name=$1 screen=$2 scale=$3 size=$4 settings=$5; shift 5
    if [ -n "${ONLY:-}" ] && ! [[ $name =~ $ONLY ]]; then return; fi
    local prof=$WORK/prof-${name//\//_}
    mkdir -p "$prof"
    [ -n "$settings" ] && echo "$settings" > "$prof/settings.json"
    local script=$WORK/${name//\//_}.script
    : > "$script"
    for l in "$@"; do echo "$l" >> "$script"; done
    echo "shot $(conv "$OUT/$name.png")" >> "$script"
    echo "quit" >> "$script"
    local sw=${screen%x*} sh=${screen#*x}
    if [ $WINE = 0 ]; then
        local dpi=$(( 96 * scale / 100 ))
        cat > "$WORK/screen.json" <<JSON
{ "synchronousWindowSystemEvents": false, "windowFrameMargins": false,
  "screens": [ { "name": "shot", "x": 0, "y": 0, "width": $sw, "height": $sh,
                 "logicalDpi": $dpi, "logicalBaseDpi": 96, "dpr": 1 } ] }
JSON
        if ! QT_QPA_PLATFORM="offscreen:configfile=$WORK/screen.json" timeout 90 "$EXE" --profile "$prof" \
                --demo-frozen --size "$size" --script "$script" > "$WORK/${name//\//_}.log" 2>&1; then
            echo "FAILED: $name (see $WORK/${name//\//_}.log)"; tail -3 "$WORK/${name//\//_}.log"; return
        fi
    else
        if ! MX_UI_SCALE=$scale timeout 120 $EXE --profile "$(conv "$prof")" --demo-frozen --size "$size" \
                --script "$(conv "$script")" > "$WORK/${name//\//_}.log" 2>&1; then
            echo "FAILED: $name (see $WORK/${name//\//_}.log)"; tail -3 "$WORK/${name//\//_}.log"; return
        fi
    fi
    echo "ok: $name.png"
}

base() { echo "{\"ui/settingsMigrated\":1,\"ui/chatWarningDisabled\":true,\"ui/colorTheme\":\"$1\"$2}"; }
SEARCH=("page search" "type searchText sintel" "key searchText Return" "wait 700")

for c in "100:1024x768:100:1024x714" "200:3840x2160:200:2048x1428"; do
    IFS=: read -r tag screen scale size <<< "$c"
    # ---- item 2: every page and the main dialogs in both themes
    for th in dark light; do
        S=$(base $th "")
        d="theme/${th}_${tag}"
        shot "${d}_networks_wpn"   "$screen" "$scale" "$size" "$S" "page networks" "subtab wpn" "wait 200"
        shot "${d}_networks_other" "$screen" "$scale" "$size" "$S" "page networks" "subtab other" "wait 200"
        shot "${d}_shared"         "$screen" "$scale" "$size" "$S" "page shared" "wait 200"
        shot "${d}_chat_list"      "$screen" "$scale" "$size" "$S" "page chat" "wait 300"
        shot "${d}_chat_room"      "$screen" "$scale" "$size" "$S" "page chat" "join Linux Users" "wait 500"
        shot "${d}_search"         "$screen" "$scale" "$size" "$S" "${SEARCH[@]}"
        shot "${d}_search_tabs"    "$screen" "$scale" "$size" "$S" "${SEARCH[@]}" "type searchText ubuntu" \
                                        "click searchNewWindow" "wait 700"
        shot "${d}_transfers"      "$screen" "$scale" "$size" "$S" "page transfers" "hotlist on" "wait 300"
        shot "${d}_bandwidth"      "$screen" "$scale" "$size" "$S" "page bandwidth" "wait 600"
        shot "${d}_float"          "$screen" "$scale" "$size" "$S" "page search" "float bandwidth" "wait 600"
        shot "${d}_conntest"       "$screen" "$scale" "$size" "$S" "page networks" "subtab wpn" "click btnConnectionTest" "wait 600"
        shot "${d}_windows"        "$screen" "$scale" "$size" "$S" "page transfers" "open-pm helping_holly_88120" "pm-say helping_holly_88120 hi holly, yes that is me" \
                                        "open-whois Twirl836_24616" "open-browse Twirl836_24616" "open-ignored" "wait 600"
        shot "${d}_settings_appearance" "$screen" "$scale" "$size" "$S" "settings appearance" "wait 200"
        shot "${d}_settings_search_colors" "$screen" "$scale" "$size" "$S" "settings appearance/search" "wait 200"
        shot "${d}_tray_menu"      "$screen" "$scale" "$size" "$S" "page networks" "tray-menu" "wait 150" "menu-pick Color Theme" "wait 200"
        shot "${d}_settings_menu"  "$screen" "$scale" "$size" "$S" "page search" "settings-menu" "wait 150" "menu-pick Color Theme" "wait 200"
    done
    # ---- item 1: the close prompt and the settings
    S=$(base dark ',"misc/closeAction":"ask"')
    shot "${tag}_close_prompt"     "$screen" "$scale" "$size" "$S" "page transfers" "wait 200" "close-window" "wait 500"
    shot "${tag}_settings_misc"    "$screen" "$scale" "$size" "$S" "settings misc" "wait 200"
    shot "${tag}_settings_system"  "$screen" "$scale" "$size" "$S" "settings misc/system" "wait 200"
    # ---- item 3: Help menu, About, the update box, "You have the latest version"
    # (the demo core never contacts the update server: "update-show" feeds the same answers to the
    # result handling; "check-updates" shows the demo-mode message. tst_update runs the real check
    # against a local server.)
    S=$(base dark "")
    shot "${tag}_help_menu"        "$screen" "$scale" "$size" "$S" "page networks" "help-menu" "wait 200"
    shot "${tag}_about"            "$screen" "$scale" "$size" "$S" "page networks" "help-menu" "wait 100" "menu-pick About OctoMX..." "wait 400"
    shot "${tag}_update_box"       "$screen" "$scale" "$size" "$S" "page networks" "update-show $(conv "$SRV/newer")" "wait 300"
    shot "${tag}_update_light"     "$screen" "$scale" "$size" "$(base light "")" "page search" "update-show $(conv "$SRV/newer")" "wait 300"
    shot "${tag}_update_latest"    "$screen" "$scale" "$size" "$S" "page networks" "update-show $(conv "$SRV/same")" "wait 300"
    shot "${tag}_update_demo"      "$screen" "$scale" "$size" "$S" "page networks" "check-updates" "wait 300"
done
# the main window at 800x600 (100 %)
for th in dark light; do
    S=$(base $th "")
    shot "theme/${th}_800x600_search"    "1024x768" 100 "800x600" "$S" "${SEARCH[@]}"
    shot "theme/${th}_800x600_transfers" "1024x768" 100 "800x600" "$S" "page transfers"
    shot "theme/${th}_800x600_chat"      "1024x768" 100 "800x600" "$S" "page chat" "join Linux Users" "wait 500"
done
shot "800x600_close_prompt" "1024x768" 100 "800x600" "$(base dark ',"misc/closeAction":"ask"')" "page transfers" "close-window" "wait 500"
shot "800x600_update_box"   "1024x768" 100 "800x600" "$(base dark "")" "update-show $(conv "$SRV/newer")" "wait 300"
