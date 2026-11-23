#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# OctoMX engine: three instances on ONE host: per-part verification of overlay-only downloads.
# [engine-3] sibling of two_instances.sh.
#
#   tests/engine/corrupt_peer.sh [path/to/mxcli] [workdir]
#
# A (Alice) and C (Carol) share the same generated 44 MiB file (5 eD2k parts). Carol runs with the
# hidden test hook overlay/testCorruptUploads=<part 1>, so every byte she serves inside part 1
# (bytes 9 728 000 ... 19 455 999) is flipped. B (Bob) finds the file by keyword search on the
# overlay (one hit, two OctoMX sources), gets the eD2k part hash set over the overlay (hs_req →
# hs, checked against the hit's ed2k hash), downloads from both, and must
#   * finish byte-identical,
#   * verify per part: only part 1 fails and only part 1 is fetched again (bytes served by A + C
#     minus the file size ≤ one part + the in-flight slack),
#   * stop using Carol (banned for this download; the black box proves her bytes were bad).
# Only the overlay network runs (mDNS on the LAN interface, no DHT/UPnP, all internet networks off).
set -uo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
source "$ROOT/toolchain/env.sh"
MXCLI=$(realpath "${1:-$ROOT/build/engine3-linux/bin/mxcli}")
W=$(realpath -m "${2:-$ROOT/build/engine3-corrupt}")
# [release] the corrupting hook is compiled only with -DMX_TEST_HOOKS=ON (the default; release builds
# use OFF). Skip cleanly (exit 77, the automake/ctest "skipped" code) when this mxcli lacks it.
CACHE=$(dirname "$MXCLI")/../CMakeCache.txt
if [ -f "$CACHE" ] && grep -qE '^MX_TEST_HOOKS:BOOL=(OFF|0|FALSE|NO)$' "$CACHE"; then
  echo "SKIP: $MXCLI was built with MX_TEST_HOOKS=OFF (overlay/testCorruptUploads not compiled in)"
  exit 77
fi
rm -rf "$W"
mkdir -p "$W/shareA" "$W/shareC"
NAME=octomx-corrupt-payload.bin
SIZE=$((44 * 1024 * 1024))
PART=9728000
head -c $SIZE /dev/urandom > "$W/shareA/$NAME"
cp "$W/shareA/$NAME" "$W/shareC/$NAME"
WANT=$(sha256sum "$W/shareA/$NAME" | cut -d' ' -f1)
START=$(date +%s)

export MXCLI_DISABLE="eD2k,Kad,Web,Rats,Gnutella,BitTorrent,HTTP"
COMMON=(--set connection/upnp=false --set connection/natPmp=false --set bt/dht=false --set bt/lsd=false
        --set bandwidth/limitLan=true --set misc/logLevel=debug)
A_OPTS=(--profile "$W/A" "${COMMON[@]}" --set connection/tcpPort=46701 --set bt/listenPort=46711
        --set identity/screenName=AliceNG_11111 --set bandwidth/upLimitKBs=6000)
C_OPTS=(--profile "$W/C" "${COMMON[@]}" --set connection/tcpPort=46703 --set bt/listenPort=46713
        --set identity/screenName=CarolNG_33333 --set bandwidth/upLimitKBs=6000
        --set "overlay/testCorruptUploads=$PART-$((2 * PART))")
B_OPTS=(--profile "$W/B" "${COMMON[@]}" --set connection/tcpPort=46702 --set bt/listenPort=46712
        --set identity/screenName=BobNG_22222)

for X in A C; do
  cat > "$W/$X.mx" <<EOF
share $W/share$X
wait-hashed --count 1 --timeout 180
hashes
wait-file $W/stop --timeout 900
EOF
done
cat > "$W/B.mx" <<EOF
wait-peers OctoMX 2 --timeout 90
search octomx corrupt payload --wait 12 --warmup 0 --details 1
get 1 --until-done --timeout 300 --every 1
info last
EOF

echo "== A and C (corrupting part 1): share + hash"
"$MXCLI" --script "$W/A.mx" "${A_OPTS[@]}" > "$W/A.log" 2>&1 &
APID=$!
"$MXCLI" --script "$W/C.mx" "${C_OPTS[@]}" > "$W/C.log" 2>&1 &
CPID=$!
for i in $(seq 1 360); do grep -q '^wait-hashed: ok' "$W/A.log" && grep -q '^wait-hashed: ok' "$W/C.log" && break; sleep 0.5; done
grep -q '^wait-hashed: ok' "$W/A.log" && grep -q '^wait-hashed: ok' "$W/C.log" || { echo "sharers did not hash"; touch "$W/stop"; exit 1; }
if grep -qs 'TEST HOOK unavailable' "$W/C/logs/octomx.log"; then   # [release] no CMakeCache next to mxcli
  echo "SKIP: Carol's mxcli was built without MX_TEST_HOOKS"; touch "$W/stop"; wait $APID $CPID; exit 77
fi

echo "== B: overlay search → download from A and C"
"$MXCLI" --script "$W/B.mx" "${B_OPTS[@]}" > "$W/B.log" 2>&1
GOT=$(sha256sum "$W/B/incoming/$NAME" 2>/dev/null | cut -d' ' -f1)
touch "$W/stop"
wait $APID $CPID
END=$(date +%s)

# bytes each sharer uploaded (overlay "upload ... closed: ... N bytes" lines; one or more sessions)
sent() { grep -h "upload \"$NAME\" to" "$1/logs/octomx.log" | sed -E 's/.* ([0-9]+) bytes$/\1/' | awk '{s+=$1} END {print s+0}'; }
SA=$(sent "$W/A"); SC=$(sent "$W/C")
TOTAL=$((SA + SC)); EXTRA=$((TOTAL - SIZE))
jget() { grep -E "^ *\"$1\":" "$W/B.log" | tail -1 | sed -E 's/^ *"[^"]+": *//; s/,$//'; }
UF=$(jget unitFailures); FB=$(jget failedUnitBytes); PROV=$(jget hashSetProvider); PROVIS=$(jget hashSetProvisional)

echo
echo "================ summary ($((END - START)) s) ================"
echo "payload $SIZE bytes ($(( (SIZE + PART - 1) / PART )) parts), sha256 $WANT"
grep -E '^(search |  1 |       hashes:)' "$W/B.log" | cut -c1-200
grep -E '^\[' "$W/B.log" | sed -n '1p;$p' | cut -c1-200
grep -h -E 'ed2k hash set|failed verification|strike|banned source|black box|corrupt' "$W/B/logs/octomx.log" | cut -c25-260 | head -12
grep -h 'TEST HOOK' "$W/C/logs/octomx.log" | head -1 | cut -c25-200
echo "uploaded: Alice $SA, Carol $SC, total $TOTAL = size + $EXTRA (one part = $PART)"
echo "engine: unitFailures=$UF failedUnitBytes=$FB hashSetProvider=$PROV provisional=$PROVIS"
echo "B sha256 $GOT $([ "$GOT" = "$WANT" ] && echo IDENTICAL || echo MISMATCH)"
OK=1
[ "$GOT" = "$WANT" ] || OK=0
[ "${UF:-0}" -ge 1 ] || { echo "FAIL: no unit failed (Carol never served part 1?)"; OK=0; }
[ "${FB:-0}" -le $((2 * PART)) ] || { echo "FAIL: more than the bad part failed"; OK=0; }
# part 1 is fetched again once (from Alice) or twice (the isolated re-fetch went to Carol first,
# failed again and banned her); never any other part
[ "$EXTRA" -le $((2 * PART + 8 * 1024 * 1024)) ] || { echo "FAIL: re-fetched more than the bad part"; OK=0; }
[ "${UF:-0}" -le 2 ] || { echo "FAIL: more than two failures of the bad part"; OK=0; }
CID=$(grep -E 'source: OctoMX user="CarolNG' "$W/B.log" | sed -E 's/.* id=([0-9a-f]+) .*/\1/')
AID=$(grep -E 'source: OctoMX user="AliceNG' "$W/B.log" | sed -E 's/.* id=([0-9a-f]+) .*/\1/')
echo "Carol id $CID...: banned $(grep -c "banned source \"6|$CID" "$W/B/logs/octomx.log")x; Alice id $AID...: banned $(grep -c "banned source \"6|$AID" "$W/B/logs/octomx.log")x"
[ -n "$CID" ] && grep -q "banned source \"6|$CID" "$W/B/logs/octomx.log" || { echo "FAIL: Carol not banned"; OK=0; }
[ -n "$AID" ] && ! grep -q "banned source \"6|$AID" "$W/B/logs/octomx.log" || { echo "FAIL: Alice banned"; OK=0; }
# after the ban Carol gets no further request: her upload stops while Alice serves the rest
grep -h -E 'black box|innocent' "$W/B/logs/octomx.log" | cut -c25-200
[ $OK = 1 ] && echo "PASS" || echo "FAILED"
[ $OK = 1 ]
