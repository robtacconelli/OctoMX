#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# tools/shots-ui-j.sh: screenshots of removing a download and deleting shared files [ui-j] (demo core).
#
#   tools/shots-ui-j.sh <out-dir> [build-dir]      (build-dir default: build/linux)
#
# The Remove Download question (finished, partial, several downloads, a drive without a trash), the
# Shared Files Delete question (a file, a folder, a shared folder, a refusal), the context menus and
# the Settings row, dark / light at 100 % and 200 %. Linux offscreen. Nothing outside <out-dir> is
# written: the profiles, the shared folder (files this script creates) and MX_OS_ROOT (the trash
# seam) live in <out-dir>/.work, and every question is closed with Cancel (nothing is deleted).
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUT=$(realpath -m "${1:?usage: tools/shots-ui-j.sh <out-dir> [build-dir]}")
BUILD=${2:-$ROOT/build/linux}
EXE="$BUILD/bin/octomx"
mkdir -p "$OUT"
WORK=$OUT/.work
rm -rf "$WORK"; mkdir -p "$WORK"
export LC_ALL=C.UTF-8
export MX_DIGIT_GROUPING=space
export MX_SOUND_OFF=1
export MX_OS_ROOT=$WORK/os-root
unset MX_TRASH

# shot <name> <scale%> <theme dark|light> <script lines...>
shot() {
    local name=$1 scale=$2 theme=$3; shift 3
    local prof=$WORK/prof-$name
    mkdir -p "$prof"
    echo "{\"ui/settingsMigrated\":1,\"ui/chatWarningDisabled\":true,\"ui/colorTheme\":\"$theme\"}" > "$prof/settings.json"
    local dpi=$(( 96 * scale / 100 ))
    local size=1024x689
    [ "$scale" = 200 ] && size=2048x1378
    cat > "$WORK/screen-$name.json" <<EOF
{ "synchronousWindowSystemEvents": false, "windowFrameMargins": false,
  "screens": [ { "name": "shot", "x": 0, "y": 0, "width": 3840, "height": 2400,
                 "logicalDpi": $dpi, "logicalBaseDpi": 96, "dpr": 1 } ] }
EOF
    local script=$WORK/$name.script
    : > "$script"
    for l in "$@"; do echo "$l" >> "$script"; done
    { echo "shot $OUT/$name.png"; echo "close-menus"; echo "close-dialogs"; echo "wait 100"; echo "quit"; } >> "$script"
    if ! QT_QPA_PLATFORM="offscreen:configfile=$WORK/screen-$name.json" MX_UI_SCALE=$scale \
            timeout 120 "$EXE" --profile "$prof" --demo-frozen --size "$size" --script "$script" > "$WORK/$name.log" 2>&1; then
        echo "FAILED: $name (see $WORK/$name.log)"; tail -3 "$WORK/$name.log"
    else
        echo "ok: $name.png"
    fi
}

# a shared folder of this script: open-film names only
SHARE="$WORK/Open Movies"
mkdir -p "$SHARE/Trailers"
head -c 524288 /dev/zero > "$SHARE/Big Buck Bunny 1080p.mkv"
head -c 262144 /dev/zero > "$SHARE/Trailers/Sintel trailer.mkv"
head -c 131072 /dev/zero > "$SHARE/Trailers/Tears of Steel trailer.mkv"

T="page transfers"
S="page shared"
for theme in dark light; do
    for scale in 100 200; do
        sfx=$theme-$scale
        # Transfers: the Remove Download question
        shot "remove-finished-$sfx" $scale $theme "$T" "wait 200" "select-text downloadList Big Buck Bunny" "click dlClear" "wait 400"
        shot "remove-partial-$sfx" $scale $theme "$T" "wait 200" "select-text downloadList Marigold" "click dlCancel" "wait 400"
        shot "remove-several-$sfx" $scale $theme "$T" "wait 200" "key downloadList Ctrl+A" "click dlClear" "wait 400"
        MX_TRASH=none shot "remove-forgood-$sfx" $scale $theme "$T" "wait 200" "select-text downloadList Big Buck Bunny" "click dlClear" "wait 400"
        shot "menu-download-finished-$sfx" $scale $theme "$T" "wait 200" "context-menu-text downloadList Big Buck Bunny" "wait 300"
        shot "menu-download-partial-$sfx" $scale $theme "$T" "wait 200" "context-menu-text downloadList debian" "wait 300"
        # Settings, File Transfers: "Removing a download"
        shot "settings-transfers-$sfx" $scale $theme "settings transfers" "wait 400"
        # Shared Files: the Delete question, the menus, a refusal
        shot "shared-delete-file-$sfx" $scale $theme "$S" "share-folder $SHARE" "wait 800" "dblclick-text sharedList Open Movies" "wait 200" \
            "select-text sharedList Big Buck Bunny 1080p" "key sharedList Delete" "wait 400"
        shot "shared-delete-folder-$sfx" $scale $theme "$S" "share-folder $SHARE" "wait 800" "dblclick-text sharedList Open Movies" "wait 200" "select-text sharedList Trailers" \
            "key sharedList Delete" "wait 400"
        shot "shared-delete-root-$sfx" $scale $theme "$S" "share-folder $SHARE" "wait 800" "select-text sharedList Open Movies" \
            "key sharedList Delete" "wait 400"
        shot "menu-shared-folder-$sfx" $scale $theme "$S" "share-folder $SHARE" "wait 800" "dblclick-text sharedList Open Movies" "wait 200" "context-menu-text sharedList Trailers" "wait 300"
        shot "menu-shared-file-$sfx" $scale $theme "$S" "share-folder $SHARE" "wait 800" "dblclick-text sharedList Open Movies" "wait 200" "context-menu-text sharedList Big Buck Bunny 1080p" "wait 300"
        shot "shared-refused-$sfx" $scale $theme "$S" "share-folder $WORK/prof-shared-refused-$sfx/incoming" "wait 800" \
            "select-text sharedList incoming" "key sharedList Delete" "wait 400"
    done
done
# nothing was deleted: the shared files are all still there, the trash seam is empty
ls "$SHARE/Big Buck Bunny 1080p.mkv" "$SHARE/Trailers/Sintel trailer.mkv" > /dev/null
[ ! -d "$MX_OS_ROOT/trash/files" ] || [ -z "$(ls -A "$MX_OS_ROOT/trash/files")" ] || echo "WARNING: something went to the trash seam"
rm -rf "$WORK"/prof-*
echo "done: $OUT"
