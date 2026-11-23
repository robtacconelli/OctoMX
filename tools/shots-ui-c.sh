#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# tools/shots-ui-c.sh: screenshots of the ui-c windows: every Settings page, the Setup Wizard
# pages, the close prompt, the tray and system menus. Demo core, frozen data. Owner: ui-c.
#
#   tools/shots-ui-c.sh <out-dir> [build-dir]          (build-dir default: build/ui-c-linux)
#   OCTOMX_EXE="wine dist/x/octomx/octomx.exe" OCTOMX_PATHCONV="winepath -w" tools/shots-ui-c.sh <out-dir>
#
# Dialog images are written as <out>/<name>.<dialogObjectName>.png (client area only: compare with
# the reference cropped at the client offset, e.g. settings_*.png: 3,22 for the classic shots).
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUT=$(realpath -m "${1:?usage: tools/shots-ui-c.sh <out-dir> [build-dir]}")
BUILD=${2:-$ROOT/build/ui-c-linux}
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
    PATHCONV="${OCTOMX_PATHCONV:-cat}"
fi
export LC_ALL=C.UTF-8
export MX_DIGIT_GROUPING=space
export MX_OS_INTEGRATION=fake
export MX_FAKE_TRAY=1
conv() { if [ "$PATHCONV" = cat ]; then echo "$1"; else $PATHCONV "$1"; fi; }

# run <name> <settings-json-or-empty> <script lines...>   (one app run, many shots)
run() {
    local name=$1 settings=$2; shift 2
    local prof=$WORK/prof-$name
    mkdir -p "$prof"
    [ -n "$settings" ] && echo "$settings" > "$prof/settings.json"
    local script=$WORK/$name.script
    : > "$script"
    for l in "$@"; do
        case "$l" in
            "shot "*) echo "shot $(conv "$OUT/${l#shot }.png")" >> "$script" ;;
            *) echo "$l" >> "$script" ;;
        esac
    done
    echo "quit" >> "$script"
    if ! timeout 180 $EXE --profile "$(conv "$prof")" --demo-frozen --no-wizard --size 1024x689 \
            --script "$(conv "$script")" > "$WORK/$name.log" 2>&1; then
        echo "FAILED: $name (see $WORK/$name.log)"; tail -5 "$WORK/$name.log"
    else
        echo "ok: $name"
    fi
}

PAGES="appearance appearance/main appearance/wpn appearance/other appearance/servermsgs appearance/library
appearance/channellist appearance/channel appearance/search appearance/hotlist appearance/browse appearance/private
appearance/ignored appearance/transfers appearance/bandwidth
connection connection/outgoing connection/incoming connection/udp connection/dns connection/options
wpn wpn/overlay other bt bt/seeding bt/trackers ed2k ed2k/connections kad gnutella gnutella/uploads http web rats
messages messages/private messages/autoreply library library/sharing channels channels/options
search search/options search/networks transfers transfers/queuing transfers/downloads transfers/uploads
throttle throttle/schedule launching launching/options misc misc/system misc/interface advanced"

lines=()
for p in $PAGES; do
    lines+=("settings $p" "wait 120" "shot settings_${p//\//_}" "close-dialogs" "wait 50")
done
run settings '{"transfers/incomingDir":"E:/New"}' "${lines[@]}"
find "$OUT" -maxdepth 1 -name 'settings_*.png' ! -name '*.settingsDialog.png' -delete

# wizard: every page (Next from the custom path)
wl=("wizard" "wait 300" "shot wizard_0_welcome" "click wizCustom" "wait 50")
for i in 1 2 3 4 5 6 7; do wl+=("click wizNext" "wait 250" "shot wizard_$i"); done
run wizard "" "${wl[@]}"
find "$OUT" -maxdepth 1 -name 'wizard_*.png' ! -name '*.setupWizard.png' -delete

# close prompt (with and without running downloads), tray and system menus
run close_prompt '{"misc/closeAction":"ask"}' "page transfers" "wait 200" "close-window" "wait 400" "shot close_prompt"
run tray_menu "" "page networks" "wait 200" "tray-menu" "wait 200" "shot tray_menu"
run system_menu "" "page networks" "wait 200" "system-menu" "wait 200" "shot system_menu"
echo "shots in $OUT"
