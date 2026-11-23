#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Stop an Xvnc started with tools/xvnc-start.sh <n>.
set -uo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../toolchain/env.sh"
N=${1:?usage: $0 <display-number>}; N=${N#:}
PIDF=$OCTOMX_ROOT/toolchain/tmp/xvnc-$N.pid
if [ -f "$PIDF" ]; then
  PID=$(cat "$PIDF")
  kill "$PID" 2>/dev/null && for _ in $(seq 1 30); do kill -0 "$PID" 2>/dev/null || break; sleep 0.1; done
  kill -9 "$PID" 2>/dev/null
  rm -f "$PIDF"
  echo "Xvnc :$N stopped"
else
  # only kill an Xvnc on this display that belongs to us
  pkill -u "$(id -u)" -f "^Xvnc :$N( |$)" && echo "Xvnc :$N stopped" || echo "no Xvnc :$N running"
fi
