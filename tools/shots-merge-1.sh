#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# tools/shots-merge-1.sh: live Search screenshots of the merge-1 work (real core, legal content):
# the same search with search/mergeByNameAndSize off (the strict rule) and on, expanded rows, the
# tool tips. 100 % (1024x714) and 200 % (2048x1428).
#
#   tools/shots-merge-1.sh <out-dir> <tag> <merge true|false> <scale 100|200> "<search words>" [script lines...]
# Uses build/linux/bin/octomx and a profile under <out-dir>/.work (Kad / eD2k nodes copied from
# build/live/p44 when present, so the networks come up quickly).
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUT=$(realpath -m "${1:?out dir}")
TAG=${2:?tag}
MERGE=${3:?true|false}
SCALE=${4:-100}
WORDS=${5:?search words}
shift 5
WORK=$OUT/.work
mkdir -p "$WORK"
export LC_ALL=C.UTF-8
export MX_DIGIT_GROUPING=space
export MX_FAKE_TRAY=1
export MX_OVERLAY_DISCOVERY=0
EXE="$ROOT/build/linux/bin/octomx"
prof=$WORK/prof-$TAG
rm -rf "$prof"
mkdir -p "$prof"
if [ -d "$ROOT/build/live/p44" ]; then
    for d in ed2k kad gnutella bt rats web overlay; do [ -d "$ROOT/build/live/p44/$d" ] && cp -r "$ROOT/build/live/p44/$d" "$prof/"; done
fi
cat > "$prof/settings.json" <<JSON
{"ui/settingsMigrated":1,"ui/chatWarningDisabled":true,"search/mergeByNameAndSize":$MERGE,"search/adultFilter":true,
 "transfers/incomingDir":"$prof/incoming"}
JSON
script=$WORK/$TAG.script
{
    echo "page search"
    echo "wait 30000"
    echo "type searchText $WORDS"
    echo "click searchGo"
    echo "wait 75000"
    echo "type searchFilter sintel"
    echo "wait 500"
    echo "report-rows searchResults $WORK/$TAG.rows"
    for l in "$@"; do echo "$l"; done
    echo "shot $OUT/$TAG.png"
    echo "quit"
} > "$script"
if [ "$SCALE" = 200 ]; then screen=3840x2160; size=2048x1428; else screen=1024x768; size=1024x714; fi
sw=${screen%x*}; sh=${screen#*x}; dpi=$(( 96 * SCALE / 100 ))
cat > "$WORK/screen-$TAG.json" <<JSON
{ "synchronousWindowSystemEvents": false, "windowFrameMargins": false,
  "screens": [ { "name": "shot", "x": 0, "y": 0, "width": $sw, "height": $sh,
                 "logicalDpi": $dpi, "logicalBaseDpi": 96, "dpr": 1 } ] }
JSON
QT_QPA_PLATFORM="offscreen:configfile=$WORK/screen-$TAG.json" timeout "${SHOT_TIMEOUT:-300}" "$EXE" --profile "$prof" \
    --size "$size" --script "$script" > "$WORK/$TAG.log" 2>&1 || { echo "FAILED: $TAG (see $WORK/$TAG.log)"; tail -3 "$WORK/$TAG.log"; exit 1; }
echo "ok: $TAG.png"
