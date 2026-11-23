#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# [ovl-2] The OctoMX network between real program instances: PASS / FAIL per scenario.
#
#   tests/overlay/net_instances.sh [--wine] [build-dir] [work-dir]
#
# Up to three octomx GUI processes (offscreen, real core, own profiles and ports, script driver), only the
# OctoMX peer network on, no DHT / LAN discovery / peer exchange (MX_OVERLAY_DISCOVERY=0: they meet only
# each other, never real users), no port mapping, no update check.
#   Run 1, direct:    Alice <-> Bob (each other's bootstrap peer). Bob accepts messages and browsing from
#                     Hotlist users only: refused first, accepted after Bob adds Alice. Receipts, browse,
#                     whois, a download, the ignore list, a normal quit (bye), a message to the offline
#                     user that goes out when he is back, a crash.
#                     Also: our own row in the room has the Networks-page name and Browse on it lists
#                     our own files (the owner's "User is not online" report).
#   Run 2, relayed:   Alice and Bob both firewalled (MX_OVERLAY_FIREWALLED=1: no direct inbound links, no
#                     address, no hole punch), each linked only to Carol, who relays and is not in the
#                     room. Presence, the room, a private message, browse and a download through Carol's
#                     relayed circuit, a quit.
# --wine runs the Windows build (build-dir default build/ovl-2-win) under wine.
# Evidence: <work-dir>/*.log, *.png. Exit status 0 when every check passed.
set -uo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
source "$ROOT/toolchain/env.sh"
WINE=0
if [ "${1:-}" = "--wine" ]; then WINE=1; shift; fi
if [ $WINE = 1 ]; then
    BUILD=$(realpath "${1:-$ROOT/build/ovl-2-win}")
    W=$(realpath -m "${2:-$ROOT/build/ovl-2-inst-wine}")
else
    BUILD=$(realpath "${1:-$ROOT/build/ovl-2}")
    W=$(realpath -m "${2:-$ROOT/build/ovl-2-inst}")
fi
case "$W" in "$ROOT"/build/*) ;; *) echo "work dir must be under $ROOT/build" >&2; exit 2;; esac
rm -rf "$W"
mkdir -p "$W/A" "$W/B" "$W/C" "$W/share-A" "$W/share-B" "$W/sync"
for i in $(seq 1 200); do echo "Alice's own list line $i"; done > "$W/share-A/own-list-sample.txt"
# neutral text files Bob shares (the second one appears for run 2)
for i in $(seq 1 60000); do echo "OctoMX network check sample line $i"; done > "$W/share-B/net-check-sample.txt"
export LC_ALL=C.UTF-8 QT_QPA_PLATFORM=offscreen MX_PORT_MAPPING=0 MX_OVERLAY_DISCOVERY=0
export QT_LOGGING_RULES="mx.overlay.debug=true"
if [ $WINE = 1 ]; then
    export WINEDEBUG=-all WINEPATH="$(winepath -w "$QT_WIN_DIR/bin");$(winepath -w "$LLVM_MINGW_ROOT/x86_64-w64-mingw32/bin")"
    run() { exec wine "$BUILD/bin/octomx.exe" "$@"; }
else
    # [ren-1] NET_EXE_A / NET_EXE_B / NET_EXE_C: another program for that instance (old meets new:
    # a 4.5 binary against this build; NET_WHOIS_B: the client name Bob's whois then shows)
    run() {   # exec: with "run ... &", \$! is the program itself (kill -9 hits it)
        local exe=$BUILD/bin/octomx
        case " $* " in
            *" $W/A "*) exe=${NET_EXE_A:-$exe} ;;
            *" $W/B "*) exe=${NET_EXE_B:-$exe} ;;
            *" $W/C "*) exe=${NET_EXE_C:-$exe} ;;
        esac
        exec "$exe" "$@"
    }
fi
# free ports: a random base, tcp = base + 10 k, udp = tcp + 1
BASE=$(( 40000 + (RANDOM % 1500) * 10 ))
PA=$BASE; PB=$((BASE + 10)); PC=$((BASE + 20))

settings() {   # <profile> <name> <tcp> <bootstrap port> <extra json>
    cat > "$1/settings.json" <<JSON
{ "internal/firstRunDone": true, "internal/wizardDone": true, "internal/termsAccepted": 1, "ui/chatWarningDisabled": true,
  "identity/screenName": "$2", "connection/tcpPort": $3, "connection/udpPort": $(( $3 + 1 )),
  "connection/upnp": false, "connection/natPmp": false, "updates/checkAtStart": false,
  "bt/enabled": false, "ed2k/enabled": false, "kad/enabled": false, "gnutella/enabled": false,
  "web/enabled": false, "rats/enabled": false, "http/enabled": false,
  "overlay/enabled": true, "overlay/bootstrapPeers": ["127.0.0.1:$4"],
  "messages/openWindowOnMessage": true,
  "transfers/incomingDir": "$1/incoming", "transfers/incompleteDir": "$1/incomplete",
  "misc/logLevel": "debug" $5 }
JSON
}
S=$W/sync

# ------------------------------------------------------------------------------------------ run 1
settings "$W/A" AliceNG $PA $PB ''
settings "$W/B" BobNG $PB $PA ', "messages/allowPrivateMessages": "hotlist", "messages/allowBrowse": "hotlist", "bandwidth/upLimitKBs": 150'

cat > "$W/B1.script" <<EOF
page shared
share-folder $W/share-B
wait 3000
page chat
join NetCheck
echo-file $S/B.ready
wait-file $S/A.refused 180
context-menu-text chatUserList AliceNG
menu-pick Add To Hotlist
wait 1000
echo-file $S/B.added
expect-net D-pm-received pm AliceNG 60 hello Bob, direct
shot $W/B1_pm.png
wait-file $S/A.downloaded 300
context-menu-text chatUserList AliceNG
menu-pick Ignore
wait 500
echo-file $S/B.ignored
expect-net D-ignore-list no-pm AliceNG 10 a message Bob ignores
context-menu-text chatUserList AliceNG
menu-pick Unignore
echo-file $S/B.unignored
wait-file $S/A.quitB 300
quit
EOF
cat > "$W/B2.script" <<EOF
page chat
join NetCheck
expect-net D-pm-retry-received pm AliceNG 90 sent while you were away
shot $W/B2_pm.png
wait-file $S/never 900
EOF
cat > "$W/A1.script" <<EOF
page shared
share-folder $W/share-A
wait-file $S/B.ready 120
page chat
join NetCheck
wait 5000
expect-net D-own-name own-name netcheck 20
context-menu-text chatUserList AliceNG
menu-pick Browse
expect-net D-browse-self browse AliceNG 15 1
close-window browseWindow
context-menu-text chatUserList BobNG
menu-pick Add To Hotlist
expect-net D-presence-online online BobNG 30
context-menu-text hotlistList BobNG
menu-pick Send Message
pm-say BobNG may I write to you?
expect-net D-pm-refused-hotlist pm-status BobNG 30 Refused
shot $W/A1_refused.png
context-menu-text hotlistList BobNG
menu-pick Browse
expect-net D-browse-refused-hotlist browse-fail BobNG 30 only users on their Hotlist
close-window browseWindow
echo-file $S/A.refused
wait-file $S/B.added 120
wait 1500
pm-say BobNG hello Bob, direct
expect-net D-pm-delivered pm-status BobNG 30 Delivered
shot $W/A1_delivered.png
context-menu-text hotlistList BobNG
menu-pick Browse
expect-net D-browse browse BobNG 40 1
close-window browseWindow
context-menu-text hotlistList BobNG
menu-pick Whois
expect-net D-whois whois BobNG 30 ${NET_WHOIS_B:-OctoMX}
close-window whoisWindow
page search
type searchText net check sample
key searchText Return
wait 8000
dblclick-text searchResults net-check-sample
wait-download 180 net-check-sample
echo-file $S/A.downloaded
wait-file $S/B.ignored 120
pm-say BobNG a message Bob ignores
wait-file $S/B.unignored 120
echo-file $S/A.quitB
expect-net D-presence-bye offline BobNG 20
expect-net D-pm-window-offline pm BobNG 20 is offline
pm-say BobNG sent while you were away
expect-net D-pm-not-delivered pm-status BobNG 15 Not delivered
shot $W/A1_notdelivered.png
echo-file $S/A.restartB
expect-net D-presence-back online BobNG 60
expect-net D-pm-retry-delivered pm-status BobNG 90 Delivered
shot $W/A1_retried.png
echo-file $S/A.killB
expect-net D-presence-crash offline BobNG 30
shot $W/A1_end.png
quit
EOF

waitfile() { for _ in $(seq 1 1200); do [ -e "$1" ] && return 0; kill -0 "$2" 2>/dev/null || return 1; sleep 0.5; done; return 1; }
echo "== run 1: direct (ports $PA / $PB)"
run --profile "$W/B" --size 1000x700 --script "$W/B1.script" > "$W/B1.log" 2>&1 &
BPID=$!
run --profile "$W/A" --size 1100x760 --script "$W/A1.script" > "$W/A1.log" 2>&1 &
APID=$!
waitfile "$S/A.quitB" $APID && { wait $BPID 2>/dev/null; }
if waitfile "$S/A.restartB" $APID; then
    run --profile "$W/B" --size 1000x700 --script "$W/B2.script" > "$W/B2.log" 2>&1 &
    BPID=$!
fi
waitfile "$S/A.killB" $APID && { sleep 1; kill -9 $BPID 2>/dev/null; wait $BPID 2>/dev/null; }
wait $APID 2>/dev/null
kill -9 $BPID 2>/dev/null; wait $BPID 2>/dev/null

# ------------------------------------------------------------------------------------------ run 2
for i in $(seq 1 40000); do echo "OctoMX relayed check line $i"; done > "$W/share-B/net-check-relay.txt"
settings "$W/A" AliceNG $PA $PC ''
settings "$W/B" BobNG $PB $PC ', "messages/allowPrivateMessages": "hotlist", "messages/allowBrowse": "hotlist", "bandwidth/upLimitKBs": 100'
settings "$W/C" CarolNG $PC $PA ', "overlay/relay": true'
cat > "$W/C.script" <<EOF
wait-file $S/A2.done 900
quit
EOF
cat > "$W/B3.script" <<EOF
page chat
join NetCheck
wait 12000
say hello room from Bob
expect-net R-pm-received pm AliceNG 90 hello through Carol
wait-file $S/A2.quitB 600
quit
EOF
cat > "$W/A2.script" <<EOF
page chat
join NetCheck
expect-net R-presence-online online BobNG 60
expect-net R-room-message room netcheck 60 hello room from Bob
context-menu-text hotlistList BobNG
menu-pick Send Message
pm-say BobNG hello through Carol
expect-net R-pm-delivered pm-status BobNG 40 Delivered
shot $W/A2_pm.png
context-menu-text hotlistList BobNG
menu-pick Browse
expect-net R-browse browse BobNG 60 2
shot $W/A2_browse.png
close-window browseWindow
page search
type searchText net check relay
key searchText Return
wait 9000
dblclick-text searchResults net-check-relay
page transfers
expect-net R-transfer-relayed transfer BobNG 90 relayed
shot $W/A2_transfer.png
wait-download 240 net-check-relay
echo-file $S/A2.quitB
expect-net R-presence-bye offline BobNG 30
echo-file $S/A2.done
quit
EOF
echo "== run 2: both firewalled, relayed by Carol (port $PC)"
run --profile "$W/C" --size 900x650 --script "$W/C.script" > "$W/C.log" 2>&1 &
CPID=$!
sleep 2
MX_OVERLAY_FIREWALLED=1 run --profile "$W/B" --size 1000x700 --script "$W/B3.script" > "$W/B3.log" 2>&1 &
BPID=$!
MX_OVERLAY_FIREWALLED=1 run --profile "$W/A" --size 1100x760 --script "$W/A2.script" > "$W/A2.log" 2>&1 &
APID=$!
wait $APID 2>/dev/null
for p in $BPID $CPID; do
    for _ in $(seq 1 20); do kill -0 $p 2>/dev/null || break; sleep 0.5; done
    kill -9 $p 2>/dev/null; wait $p 2>/dev/null
done

# ------------------------------------------------------------------------------------------ results
echo "== results"
FAILS=0
# under wine the program writes nothing to the redirected output; its own log file has the lines
if [ $WINE = 1 ]; then
    cat "$W/A/logs/"*.log >> "$W/A1.log" 2>/dev/null; cp "$W/A1.log" "$W/A2.log"
    cat "$W/B/logs/"*.log >> "$W/B1.log" 2>/dev/null; cp "$W/B1.log" "$W/B2.log"; cp "$W/B1.log" "$W/B3.log"
    cat "$W/C/logs/"*.log >> "$W/C.log" 2>/dev/null
fi
report() {   # <label> <log...>
    local label=$1; shift
    if grep -hq "CHECK PASS $label " "$@" 2>/dev/null; then
        echo "PASS  $label  ($(grep -ho "CHECK PASS $label ([0-9]* ms)" "$@" | head -1 | sed 's/.*(\(.*\))/\1/'))"
    else
        echo "FAIL  $label  $(grep -ho "CHECK FAIL $label: .*" "$@" | head -1 | cut -d: -f2- | cut -c1-120)"
        FAILS=$((FAILS + 1))
    fi
}
dl() {   # <label> <name> <log>
    if grep -q "wait-download: $2.* complete" "$3"; then echo "PASS  $1"; else echo "FAIL  $1  (download of $2 not complete)"; FAILS=$((FAILS + 1)); fi
}
L1="$W/A1.log"; L2="$W/A2.log"
for c in D-own-name D-browse-self D-presence-online D-pm-refused-hotlist D-browse-refused-hotlist D-pm-delivered D-browse D-whois; do report $c "$L1"; done
report D-pm-received "$W/B1.log"
dl D-transfer-direct net-check-sample "$L1"
report D-ignore-list "$W/B1.log"
for c in D-presence-bye D-pm-window-offline D-pm-not-delivered D-presence-back D-pm-retry-delivered; do report $c "$L1"; done
report D-pm-retry-received "$W/B2.log"
report D-presence-crash "$L1"
for c in R-presence-online R-room-message R-pm-delivered R-browse R-transfer-relayed; do report $c "$L2"; done
report R-pm-received "$W/B3.log"
dl R-transfer-relayed-complete net-check-relay "$L2"
report R-presence-bye "$L2"
grep -h 'script line' "$W"/*.log | grep -v 'CHECK FAIL' | head -20
# nothing of ours may keep running
for p in $(pgrep -f "^.*bin/[a-z]*mx(.exe)? --profile $W/"); do kill -9 "$p" 2>/dev/null; done
echo "== $FAILS check(s) failed; evidence in $W"
exit $(( FAILS > 0 ))
