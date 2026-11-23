#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# OctoMX engine: two-instance end-to-end scenario on ONE host (owner's headline feature).
#
#   tests/engine/two_instances.sh [path/to/mxcli] [workdir]
#
# A and B are two mxcli processes with their own profiles and ports. They find each other through
# the overlay ("OctoMX" network, mDNS); UPnP/NAT-PMP, BitTorrent DHT/LSD and every internet network
# are off. Steps:
#   (a) A shares a folder with a generated 80 MiB file and waits until the library hashed it.
#       B1 searches by keyword, sees A's overlay hit (user name + all hashes), downloads it over the
#       overlay RangeSource, the engine verifies it and B1's library shares it.
#   (b) A also seeds the file over BitTorrent (the engine links btih ↔ library hashes and publishes
#       the hash link). B2 gets ONLY the magnet; it learns the other hashes through the overlay
#       hash-link lookup, opens A as an overlay source and receives pieces from A's libtorrent
#       (explicit 127.0.0.1 peer) AND injected from the overlay.
#   (c) B2 is stopped in the middle of (b) and restarted on the same profile: it resumes.
# Evidence: <workdir>/*.log (trimmed summary printed at the end).
set -uo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
source "$ROOT/toolchain/env.sh"
MXCLI=$(realpath "${1:-$ROOT/build/engine-linux/bin/mxcli}")
W=$(realpath -m "${2:-$ROOT/build/engine-2inst}")
rm -rf "$W"
mkdir -p "$W/share"
PAYLOAD="$W/share/octomx-e2e-payload.bin"
head -c $((80 * 1024 * 1024)) /dev/urandom > "$PAYLOAD"
WANT=$(sha256sum "$PAYLOAD" | cut -d' ' -f1)
START=$(date +%s)

export MXCLI_DISABLE="eD2k,Kad,Web,Rats,Gnutella"
COMMON=(--set connection/upnp=false --set connection/natPmp=false --set bt/dht=false --set bt/lsd=false
        --set bt/extraTrackers= --set bandwidth/limitLan=true --set bt/seedRatioLimit=0)
A_OPTS=(--profile "$W/A" "${COMMON[@]}" --set connection/tcpPort=46601 --set bt/listenPort=46611
        --set identity/screenName=AliceNG_11111 --set bandwidth/upLimitKBs=8000)
B_PORTS=(--set misc/logLevel=debug --set connection/tcpPort=46602 --set bt/listenPort=46612 --set identity/screenName=BobNG_22222)

cat > "$W/A.mx" <<EOF
share $W/share
wait-hashed --count 1 --timeout 180
seed $PAYLOAD
hashes
wait-file $W/stop --timeout 900
EOF
cat > "$W/B1.mx" <<EOF
wait-peers OctoMX 1 --timeout 60
search octomx e2e payload --wait 12 --warmup 0 --details 1
get 1 --until-done --timeout 300 --every 2
info last --brief
wait-hashed --count 1 --timeout 120
EOF

echo "== A: share + hash + seed"
"$MXCLI" --script "$W/A.mx" "${A_OPTS[@]}" > "$W/A.log" 2>&1 &
APID=$!
for i in $(seq 1 240); do grep -q '^seeded ' "$W/A.log" && break; sleep 0.5; done
BTIH=$(grep '^seeded ' "$W/A.log" | sed 's/.* btih //')
[ -n "$BTIH" ] || { echo "A did not seed"; tail -n 20 "$W/A.log"; kill $APID; exit 1; }
echo "   A seeds btih $BTIH"

echo "== (a) B1: keyword search on the overlay → download from A"
"$MXCLI" --script "$W/B1.mx" --profile "$W/B1" "${COMMON[@]}" "${B_PORTS[@]}" > "$W/B1.log" 2>&1
GOT_A=$(sha256sum "$W/B1/incoming/octomx-e2e-payload.bin" 2>/dev/null | cut -d' ' -f1)

echo "== (b) B2: magnet only (btih), hash-link lookup + BitTorrent + overlay; stop in the middle"
cat > "$W/B2a.mx" <<EOF
wait-peers OctoMX 1 --timeout 60
get magnet:?xt=urn:btih:$BTIH&dn=octomx-e2e-payload.bin --peer 127.0.0.1:46611 --until-bytes 36000000 --timeout 300 --every 2
info last --brief
EOF
cat > "$W/B2b.mx" <<EOF
list
wait-peers OctoMX 1 --timeout 60
peer last 127.0.0.1:46611
watch last --timeout 300 --every 2
info last --brief
list
EOF
"$MXCLI" --script "$W/B2a.mx" --profile "$W/B2" "${COMMON[@]}" "${B_PORTS[@]}" > "$W/B2a.log" 2>&1
echo "== (c) B2 restarted on the same profile"
"$MXCLI" --script "$W/B2b.mx" --profile "$W/B2" "${COMMON[@]}" "${B_PORTS[@]}" > "$W/B2b.log" 2>&1
GOT_B=$(sha256sum "$W/B2/incoming/octomx-e2e-payload.bin" 2>/dev/null | cut -d' ' -f1)

touch "$W/stop"
wait $APID
END=$(date +%s)

echo
echo "================ summary ($((END - START)) s) ================"
echo "payload sha256 $WANT"
echo "-- A"; grep -E '^(wait-hashed|shared:|seeded)' "$W/A.log" | cut -c1-230
grep -h 'hash link published' "$W/A/logs/octomx.log" | cut -c25-220
echo "-- (a) B1"; grep -E '^(wait-peers|search |  1 |       (hashes|source):|result:)' "$W/B1.log" | cut -c1-230
grep -E '^\[' "$W/B1.log" | sed -n '1p;$p' | cut -c1-200
grep -E '"(store|verifiedByNet|OctoMX|scheme)"' "$W/B1.log" | tr -s ' ' | head -6
grep -E '^shared:' "$W/B1.log" | cut -c1-120
echo "   B1 sha256 $GOT_A $([ "$GOT_A" = "$WANT" ] && echo IDENTICAL || echo MISMATCH)"
echo "-- (b) B2 first run"; grep -E '^\[' "$W/B2a.log" | sed -n '1,2p;$p' | cut -c1-260
grep -h -E 'hint from|learned .* from|confirmed|hash link' "$W/B2/logs/octomx.log" | cut -c25-200 | head -6
echo "-- (c) B2 restart"; grep -E '^(d[0-9a-f]{16} )' "$W/B2b.log" | head -1 | cut -c1-120
grep -E '^\[' "$W/B2b.log" | sed -n '1,2p;$p' | cut -c1-260
grep -h 'torrent complete' "$W/B2/logs/octomx.log" | cut -c25-260
grep -E '"(injectedPieces|injectedBytes|btBytesEstimate|totalDone|OctoMX|BitTorrent)"' "$W/B2b.log" | tr -s ' ' | head -8
echo "   B2 sha256 $GOT_B $([ "$GOT_B" = "$WANT" ] && echo IDENTICAL || echo MISMATCH)"
[ "$GOT_A" = "$WANT" ] && [ "$GOT_B" = "$WANT" ]
