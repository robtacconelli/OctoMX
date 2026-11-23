#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Start a private Xvnc server (1280x1024x24, no password, local only).
#   tools/xvnc-start.sh <display-number 70..99>      then: export DISPLAY=:<n>
# Log: toolchain/logs/xvnc-<n>.log   pid: toolchain/tmp/xvnc-<n>.pid
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../toolchain/env.sh"
N=${1:?usage: $0 <display-number>}; N=${N#:}
PIDF=$OCTOMX_ROOT/toolchain/tmp/xvnc-$N.pid
LOG=$OCTOMX_ROOT/toolchain/logs/xvnc-$N.log
mkdir -p "$(dirname "$LOG")"
if [ -e "/tmp/.X$N-lock" ] || [ -e "/tmp/.X11-unix/X$N" ]; then
  if [ -f "$PIDF" ] && kill -0 "$(cat "$PIDF")" 2>/dev/null; then
    echo "Xvnc :$N already running (pid $(cat "$PIDF"))"; exit 0
  fi
  echo "error: display :$N is in use by someone else (/tmp/.X$N-lock exists)" >&2; exit 1
fi
nohup Xvnc ":$N" -geometry 1280x1024 -depth 24 -localhost \
      -rfbport $((5900 + N)) -nolisten tcp -ac -alwaysshared -desktop octomx-$N >"$LOG" 2>&1 &
echo $! > "$PIDF"
for _ in $(seq 1 50); do
  [ -S "/tmp/.X11-unix/X$N" ] && { echo "Xvnc :$N started (pid $(cat "$PIDF")); export DISPLAY=:$N"; exit 0; }
  kill -0 "$(cat "$PIDF")" 2>/dev/null || break
  sleep 0.1
done
echo "error: Xvnc :$N did not start, see $LOG" >&2; tail -n 20 "$LOG" >&2; exit 1
