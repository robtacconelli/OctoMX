#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Overlay PoC scenarios (deliverable 2). Usage:
#   tests/research-librats/run_overlay.sh [mdns|dht|chain|all] [seconds]
# Logs go to build/research-librats/logs/overlay-<scenario>-<node>.log
set -u
source "$(dirname "${BASH_SOURCE[0]}")/../../toolchain/env.sh"
B=$OCTOMX_ROOT/build/research-librats
BIN=${BIN:-$B/poc-linux/rats_poc}
WINE_BIN=${WINE_BIN:-}            # set to poc-win64/rats_poc.exe path to run node B under wine
L=$B/logs; mkdir -p "$L" "$B/run"
SCEN=${1:-all}; SECS=${2:-90}
KEY="octomx-poc/$(date +%s)-$RANDOM"   # unique: only our two nodes share this DHT key

run_pair() {  # $1 scenario name, $2 discovery mode
  local s=$1 d=$2
  rm -rf "$B/run/ov-$s-A" "$B/run/ov-$s-B"; mkdir -p "$B/run/ov-$s-A" "$B/run/ov-$s-B"
  echo "== scenario $s (discovery=$d, key=$KEY, ${SECS}s)"
  "$BIN" overlay --name A --key "$KEY" --discovery "$d" --data "$B/run/ov-$s-A" --seconds "$SECS" \
      > "$L/overlay-$s-A.log" 2>&1 &
  local pa=$!
  if [ -n "$WINE_BIN" ]; then
    WINEPREFIX=$B/wineprefix wine "$WINE_BIN" overlay --name B --key "$KEY" --discovery "$d" \
      --data "$B/run/ov-$s-B" --seconds "$SECS" > "$L/overlay-$s-B.log" 2>&1 &
  else
    "$BIN" overlay --name B --key "$KEY" --discovery "$d" --data "$B/run/ov-$s-B" --seconds "$SECS" \
      > "$L/overlay-$s-B.log" 2>&1 &
  fi
  local pb=$!
  wait $pa $pb
  grep -h 'PEER+\|SUMMARY' "$L/overlay-$s-A.log" "$L/overlay-$s-B.log"
  echo "   first gossip/pm lines:"; grep -h 'GOSSIP\|PM from' "$L/overlay-$s-A.log" "$L/overlay-$s-B.log" | head -4
}

run_chain() {  # A <- B <- C by direct dial, no discovery: gossip must hop through B
  echo "== scenario chain (direct dials A<-B<-C, no discovery)"
  for n in A B C; do rm -rf "$B/run/ov-chain-$n"; mkdir -p "$B/run/ov-chain-$n"; done
  "$BIN" overlay --name A --key "$KEY" --discovery none --no-pex --port 47100 --data "$B/run/ov-chain-A" --seconds "$SECS" > "$L/overlay-chain-A.log" 2>&1 &
  sleep 1
  "$BIN" overlay --name B --key "$KEY" --discovery none --no-pex --port 47101 --connect 127.0.0.1:47100 --data "$B/run/ov-chain-B" --seconds "$SECS" > "$L/overlay-chain-B.log" 2>&1 &
  sleep 1
  "$BIN" overlay --name C --key "$KEY" --discovery none --no-pex --port 47102 --connect 127.0.0.1:47101 --data "$B/run/ov-chain-C" --seconds "$SECS" > "$L/overlay-chain-C.log" 2>&1 &
  wait
  grep -h 'PEER+\|SUMMARY' "$L"/overlay-chain-?.log
  echo "   A received from C (2 hops):"; grep -h 'GOSSIP.*C says' "$L/overlay-chain-A.log" | head -2
}

case $SCEN in
  mdns)  run_pair mdns mdns ;;
  dht)   run_pair dht dht ;;
  chain) run_chain ;;
  all)   run_pair mdns mdns; run_pair dht dht; run_chain ;;
esac
