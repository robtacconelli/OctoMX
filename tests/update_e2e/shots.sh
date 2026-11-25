#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# [upd-1] Screenshots of every update box at 100 % and 200 % without any network: the demo core and
# the script verb "update-demo <kind> <release.json>" (notify, ask, progress, ready, failed,
# updated), plus the Settings page with the Updates radios and the About box.
#   tests/update_e2e/shots.sh <octomx program (bin/octomx or the package launcher)> <out dir> <work dir>
set -uo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
source "$ROOT/toolchain/env.sh" >/dev/null
EXE=$(realpath "$1"); OUT=$(realpath -m "$2"); W=$(realpath -m "$3")
mkdir -p "$OUT" "$W"
cat > "$W/release.json" <<'EOF'
{"tag_name":"v5.8.0","name":"OctoMX 5.8","draft":false,"prerelease":false,
 "html_url":"https://github.com/robtacconelli/OctoMX/releases/tag/v5.8.0",
 "body":"## What's new\n\n* Updates that install themselves, checked against the release signature\n* Columns that always fill the list\n* A filter in the search results",
 "assets":[{"name":"OctoMX-5.8-linux-x86_64.tar.xz","browser_download_url":"https://github.com/robtacconelli/OctoMX/releases/download/v5.8.0/OctoMX-5.8-linux-x86_64.tar.xz","size":30000000},
           {"name":"SHA256SUMS","browser_download_url":"https://github.com/robtacconelli/OctoMX/releases/download/v5.8.0/SHA256SUMS","size":200},
           {"name":"SHA256SUMS.sig","browser_download_url":"https://github.com/robtacconelli/OctoMX/releases/download/v5.8.0/SHA256SUMS.sig","size":140}]}
EOF
# a screen large enough for 200 % (the default offscreen screen is 800x800)
cat > "$W/offscreen.json" <<'JSON'
{ "synchronousWindowSystemEvents": false, "windowFrameMargins": false,
  "screens": [ { "name": "shot", "x": 0, "y": 0, "width": 2560, "height": 1600,
                 "logicalDpi": 96, "logicalBaseDpi": 96, "dpr": 1 } ] }
JSON
RC=0
for scale in 100 200; do
  P=$W/prof-$scale
  rm -rf "$P"; mkdir -p "$P"
  echo "{ \"ui/scale\": \"$scale\" }" > "$P/settings.json"
  {
    for k in notify ask progress ready failed updated; do
      echo "update-demo $k $W/release.json"; echo "wait 400"; echo "shot $OUT/box-$k-$scale.png"; echo "close-dialogs"; echo "wait 200"
    done
    echo "settings misc/system"; echo "wait 500"; echo "shot $OUT/settings-updates-$scale.png"; echo "close-dialogs"; echo "wait 200"
    echo "update-about"; echo "wait 500"; echo "shot $OUT/about-$scale.png"; echo "close-dialogs"; echo "wait 200"
    echo "quit"
  } > "$W/shots-$scale.script"
  (cd "$W" && timeout 120 env -i HOME="$W" PATH=/usr/bin:/bin LANG=C.UTF-8 QT_QPA_PLATFORM="offscreen:configfile=$W/offscreen.json" MX_OS_ROOT="$W/osroot" \
     XDG_RUNTIME_DIR="$W" "$EXE" --demo-frozen --profile "$P" --size $((1024 * scale / 100))x$((714 * scale / 100)) --script "$W/shots-$scale.script" > "$W/shots-$scale.out" 2>&1) \
     || { echo "FAIL shots $scale: $(tail -n 3 "$W/shots-$scale.out")"; RC=1; }
done
ls "$OUT" | grep -E '^(box|settings)-' | sed 's/^/  /'
exit $RC
