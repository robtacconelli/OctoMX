#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# tools/shots-dl-1.sh: screenshots of the dl-1 work: the download menu with "Download In Sequential
# Order" / "Download First And Last Pieces First" / "Preview", the marks after the file names, the
# "Preview With" dialog, the multi-file preview chooser, Add Link, and the Settings pages (File
# Transfers > Downloads, File Launching > Preview Player). 100 % (1024x714) and 200 % (2048x1428).
# Frozen demo core; MX_LAUNCH_LOG records instead of starting a player.
#
#   tools/shots-dl-1.sh <out-dir> [build-dir]          (build-dir default: build/dl-1)
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUT=$(realpath -m "${1:?usage: tools/shots-dl-1.sh <out-dir> [build-dir]}")
BUILD=${2:-$ROOT/build/dl-1}
WORK=$OUT/.work
rm -rf "$WORK"; mkdir -p "$WORK"
export LC_ALL=C.UTF-8
export MX_DIGIT_GROUPING=space
export MX_FAKE_TRAY=1
export MX_LAUNCH_LOG=$WORK/launch.log
EXE="$BUILD/bin/octomx"

shot() {
    local name=$1 screen=$2 scale=$3 size=$4 settings=$5; shift 5
    if [ -n "${ONLY:-}" ] && ! [[ $name =~ $ONLY ]]; then return; fi
    local prof=$WORK/prof-$name
    mkdir -p "$prof"
    echo "$settings" > "$prof/settings.json"
    local script=$WORK/$name.script
    : > "$script"
    for l in "$@"; do echo "$l" >> "$script"; done
    echo "shot $OUT/$name.png" >> "$script"
    echo "quit" >> "$script"
    local sw=${screen%x*} sh=${screen#*x} dpi=$(( 96 * scale / 100 ))
    cat > "$WORK/screen.json" <<JSON
{ "synchronousWindowSystemEvents": false, "windowFrameMargins": false,
  "screens": [ { "name": "shot", "x": 0, "y": 0, "width": $sw, "height": $sh,
                 "logicalDpi": $dpi, "logicalBaseDpi": 96, "dpr": 1 } ] }
JSON
    if ! QT_QPA_PLATFORM="offscreen:configfile=$WORK/screen.json" timeout 90 "$EXE" --profile "$prof" \
            --demo-frozen --size "$size" --script "$script" > "$WORK/$name.log" 2>&1; then
        echo "FAILED: $name (see $WORK/$name.log)"; tail -3 "$WORK/$name.log"; return
    fi
    echo "ok: $name.png"
}

S='{"ui/settingsMigrated":1,"ui/chatWarningDisabled":true,"ui/colorTheme":"dark"}'
MARKS=("page transfers" "wait 200"
       "context-menu-text downloadList Best Of Velorine" "wait 150" "menu-pick Download In Sequential Order" "wait 150"
       "context-menu-text downloadList Best Of Velorine" "wait 150" "menu-pick Download First And Last Pieces First" "wait 150"
       "context-menu-text downloadList Sintel.2010" "wait 150" "menu-pick Download First And Last Pieces First" "wait 150"
       "context-menu-text downloadList debian-13" "wait 150" "menu-pick Download In Sequential Order" "wait 200")
for c in "100:1024x768:100:1024x714" "200:3840x2160:200:2048x1428"; do
    IFS=: read -r tag screen scale size <<< "$c"
    shot "${tag}_menu_ready"    "$screen" "$scale" "$size" "$S" "page transfers" "wait 200" "context-menu-text downloadList Best Of Velorine" "wait 300"
    shot "${tag}_menu_notyet"   "$screen" "$scale" "$size" "$S" "page transfers" "wait 200" "context-menu-text downloadList Sintel.2010" "wait 300"
    shot "${tag}_menu_complete" "$screen" "$scale" "$size" "$S" "page transfers" "wait 200" "context-menu-text downloadList Big Buck Bunny" "wait 300"
    shot "${tag}_marks"         "$screen" "$scale" "$size" "$S" "${MARKS[@]}" "wait 300"
    shot "${tag}_menu_checked"  "$screen" "$scale" "$size" "$S" "${MARKS[@]}" "context-menu-text downloadList Best Of Velorine" "wait 300"
    shot "${tag}_preview_with"  "$screen" "$scale" "$size" "$S" "page transfers" "wait 200" "context-menu-text downloadList Best Of Velorine" \
                                    "wait 150" "menu-pick Preview" "wait 500"
    shot "${tag}_preview_files" "$screen" "$scale" "$size" "$S" "page transfers" "wait 200" "context-menu-text downloadList Elephants Dream" \
                                    "wait 150" "menu-pick Preview..." "wait 500"
    shot "${tag}_add_link"      "$screen" "$scale" "$size" "$S" "page transfers" "wait 200" "key downloadList Ctrl+V" "wait 150" \
                                    "wait 500"
    shot "${tag}_settings_downloads" "$screen" "$scale" "$size" "$S" "settings transfers/downloads" "wait 300"
    shot "${tag}_settings_preview"   "$screen" "$scale" "$size" "$S" "settings launching/preview" "wait 300"
done
