#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# tools/shots-bw-1.sh: screenshots of the speed limits [bw-1]: the Bandwidth page with its limit row
# and limit lines (Dark and Light, 100 % and 200 %, 800x600), the tray menu Speed Limits with its
# Download sub menu, Settings > Bandwidth Throttle and > Bandwidth Schedule, a schedule in force.
# Frozen demo core.
#
#   tools/shots-bw-1.sh <out-dir> [build-dir]          (build-dir default: build/bw-1)
#   OCTOMX_EXE="wine pkg/octomx/octomx.exe" OCTOMX_PATHCONV="winepath -w" DISPLAY=:N tools/shots-bw-1.sh <out-dir>
#   ONLY=<regex> limits the shots.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUT=$(realpath -m "${1:?usage: tools/shots-bw-1.sh <out-dir> [build-dir]}")
BUILD=${2:-$ROOT/build/bw-1}
mkdir -p "$OUT"
WORK=$OUT/.work-bw1
rm -rf "$WORK"; mkdir -p "$WORK"
export LC_ALL=C.UTF-8
export MX_DIGIT_GROUPING=space
export MX_FAKE_TRAY=1
WINE=0
if [ -n "${OCTOMX_EXE:-}" ]; then WINE=1; EXE="$OCTOMX_EXE"; PATHCONV="${OCTOMX_PATHCONV:-cat}"; else EXE="$BUILD/bin/octomx"; PATHCONV=cat; fi
conv() { if [ "$PATHCONV" = cat ]; then echo "$1"; else $PATHCONV "$1"; fi; }

# shot <name> <screenWxH> <scale%> <clientWxH> <settings-json> <script lines...>
shot() {
    local name=$1 screen=$2 scale=$3 size=$4 settings=$5; shift 5
    if [ -n "${ONLY:-}" ] && ! [[ $name =~ $ONLY ]]; then return; fi
    local prof=$WORK/prof-${name//\//_}
    mkdir -p "$prof"
    echo "$settings" > "$prof/settings.json"
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

# limits on: download 500 KB/s, upload 24 KB/s (just above the demo core's traffic, which does not
# obey limits: it is demo data)
lim() { echo "{\"ui/settingsMigrated\":1,\"ui/chatWarningDisabled\":true,\"ui/colorTheme\":\"$1\",\"bandwidth/downLimitKBs\":500,\"bandwidth/lastDownLimitKBs\":500,\"bandwidth/upLimitKBs\":24,\"bandwidth/lastUpLimitKBs\":24$2}"; }
ALWAYS=',"bandwidth/scheduleEnabled":true,"bandwidth/scheduleStart":"00:00","bandwidth/scheduleEnd":"23:59","bandwidth/scheduleDays":127,"bandwidth/scheduleDownKBs":450,"bandwidth/scheduleUpKBs":16'

for th in dark light; do
    shot "${th}_100_bandwidth"   "1024x768"  100 "1024x714"  "$(lim $th "")" "page bandwidth" "wait 2500"
    shot "${th}_200_bandwidth"   "3840x2160" 200 "2048x1428" "$(lim $th "")" "page bandwidth" "wait 2500"
    shot "${th}_800x600_bandwidth" "1024x768" 100 "800x600"  "$(lim $th "")" "page bandwidth" "wait 2500"
done
shot "dark_150_bandwidth"        "3840x2160" 150 "1536x1071" "$(lim dark "")" "page bandwidth" "wait 2500"
shot "dark_300_bandwidth"        "3840x2160" 300 "3072x2142" "$(lim dark "")" "page bandwidth" "wait 2500"
shot "dark_100_bandwidth_off"    "1024x768"  100 "1024x714"  "{\"ui/settingsMigrated\":1,\"ui/chatWarningDisabled\":true}" "page bandwidth" "wait 2500"
shot "dark_100_bandwidth_schedule" "1024x768" 100 "1024x714" "$(lim dark "$ALWAYS")" "page bandwidth" "wait 2500"
shot "dark_800x600_bandwidth_schedule" "1024x768" 100 "800x600" "$(lim dark "$ALWAYS")" "page bandwidth" "wait 2500"
shot "dark_100_tray_speed"       "1024x768"  100 "1024x714"  "$(lim dark "")" "page bandwidth" "wait 500" "tray-menu" "wait 150" \
                                     "menu-pick Speed Limits" "wait 150" "menu-pick Download: 500 KB/s" "wait 200"
shot "dark_200_tray_speed"       "3840x2160" 200 "2048x1428" "$(lim dark "")" "page bandwidth" "wait 500" "tray-menu" "wait 150" \
                                     "menu-pick Speed Limits" "wait 150" "menu-pick Upload: 24 KB/s" "wait 200"
shot "dark_100_tray_schedule"    "1024x768"  100 "1024x714"  "$(lim dark "$ALWAYS")" "page bandwidth" "wait 500" "tray-menu" "wait 150" \
                                     "menu-pick Speed Limits" "wait 200"
shot "dark_100_settings_throttle" "1024x768" 100 "1024x714"  "$(lim dark "")" "settings throttle" "wait 300"
shot "light_100_settings_throttle" "1024x768" 100 "1024x714" "$(lim light "")" "settings throttle" "wait 300"
shot "dark_100_settings_schedule" "1024x768" 100 "1024x714"  "$(lim dark "$ALWAYS")" "settings throttle/schedule" "wait 300"
shot "dark_200_settings_throttle" "3840x2160" 200 "2048x1428" "$(lim dark "")" "settings throttle" "wait 300"
