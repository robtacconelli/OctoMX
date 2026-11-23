#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# tools/shots-ui-f.sh: screenshots of the ui-f work (honest file types in Search): the file-type menu
# and its Audio / MP3 submenus, the latched quick buttons (All | Audio | Video | Pictures) with the
# results of the demo "sintel" search per type, Custom Extensions, Settings ▸ Search, Shared File
# Types; at 100 % (1024x768 and 800x600) and 200 % (3840x2160). Frozen demo core. Owner: ui-f.
#
#   tools/shots-ui-f.sh <out-dir> [build-dir]          (build-dir default: build/ui-f)
#   OCTOMX_EXE="wine pkg/octomx/octomx.exe" OCTOMX_PATHCONV="winepath -w" DISPLAY=:N tools/shots-ui-f.sh <out-dir>
#       (under wine the scale comes from MX_UI_SCALE, set per shot; the screen is the X display)
#   ONLY=<regex> limits the shots.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUT=$(realpath -m "${1:?usage: tools/shots-ui-f.sh <out-dir> [build-dir]}")
BUILD=${2:-$ROOT/build/ui-f}
mkdir -p "$OUT"
WORK=$OUT/.work-f
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

BASE='{"ui/settingsMigrated":1,"ui/chatWarningDisabled":true}'
CUSTOM='{"ui/settingsMigrated":1,"ui/chatWarningDisabled":true,"search/defaultType":"custom:mkv,srt","search/customExtensions":"mkv srt"}'
OLD='{"ui/settingsMigrated":1,"ui/chatWarningDisabled":true,"search/defaultType":"MP3 Audio - At least 128 kbps","search/typeButtonAudio":"MP3 Audio - Any Bitrate"}'
SINTEL=("type searchText sintel" "key searchText Return" "wait 700")
AUDIO_MENU="Audio (.mp3 .flac .m4a .ogg ...)"
for c in "100:1024x768:100:1024x714" "200:3840x2160:200:2048x1428"; do
    IFS=: read -r tag screen scale size <<< "$c"
    shot "${tag}_menu"         "$screen" "$scale" "$size" "$BASE" "page search" "click searchFileType" "wait 200"
    shot "${tag}_menu_audio"   "$screen" "$scale" "$size" "$BASE" "page search" "click searchFileType" "wait 100" \
                                    "menu-pick $AUDIO_MENU" "wait 100" "menu-pick Bitrate at least" "wait 200"
    shot "${tag}_menu_mp3"     "$screen" "$scale" "$size" "$BASE" "page search" "click searchFileType" "wait 100" \
                                    "menu-pick MP3 only (.mp3)" "wait 200"
    shot "${tag}_all"          "$screen" "$scale" "$size" "$BASE" "page search" "click searchAll" "${SINTEL[@]}"
    shot "${tag}_audio"        "$screen" "$scale" "$size" "$BASE" "page search" "click searchAudio" "${SINTEL[@]}"
    shot "${tag}_video"        "$screen" "$scale" "$size" "$BASE" "page search" "click searchVideo" "${SINTEL[@]}"
    shot "${tag}_pictures"     "$screen" "$scale" "$size" "$BASE" "page search" "click searchPictures" "${SINTEL[@]}"
    for t in "Documents (.pdf .epub .docx .txt ...):documents" "Software (.exe .msi .apk .iso ...):software" \
             "Archives (.zip .rar .7z .tar ...):archives" "Lossless audio (.flac .wav .ape .alac ...):lossless"; do
        label=${t%:*}; key=${t##*:}
        shot "${tag}_${key}"   "$screen" "$scale" "$size" "$BASE" "page search" "click searchFileType" "wait 100" \
                                    "menu-pick $label" "wait 100" "${SINTEL[@]}"
    done
    shot "${tag}_custom"       "$screen" "$scale" "$size" "$CUSTOM" "page search" "${SINTEL[@]}"
    shot "${tag}_migrated"     "$screen" "$scale" "$size" "$OLD" "page search" "wait 200"
    shot "${tag}_settings"     "$screen" "$scale" "$size" "$BASE" "page search" "settings search" "wait 300"
    shot "${tag}_sharedtypes"  "$screen" "$scale" "$size" "$BASE" "page shared" "click sharedFileTypes" "wait 300"
done
# the quick-button row at the smallest classic window
shot "800x600_video" "800x600" 100 "800x600" "$BASE" "page search" "click searchVideo" "${SINTEL[@]}"
shot "150_video" "2560x1440" 150 "1536x1071" "$BASE" "page search" "click searchVideo" "${SINTEL[@]}"
