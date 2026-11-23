#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# [ui-g] Hotlist with two real cores on one host (owner: "is the hotlist needed / working? how does it work?")
#
#   tests/ui/hotlist_two_instances.sh [build-dir] [work-dir]
#
# Two octomx GUI processes (offscreen, real core, no demo data), each with its own profile and ports, only
# the OctoMX peer network switched on, linked directly over 127.0.0.1 (each other's bootstrap peer).
# Every other network, UPnP / NAT-PMP (MX_PORT_MAPPING=0) and the update check are off.
#   Alice (A) meets Bob (B) in a chat room and adds him to the Hotlist from the room's user list; sends Bob
#   a message while Bob accepts messages / browsing from Hotlist users only; searches Bob's shared file,
#   downloads it (Bob's upload limit keeps it running ~40 s) and opens the menu of Bob's source row in
#   Transfers; meanwhile Bob adds Alice from his upload list. Then Alice uses Whois / Browse / Send Message
#   from the Hotlist menu. Bob quits normally, starts again (Alice should be told he is online), and is
#   finally killed as in a crash.
# Evidence: <work-dir>/A*.png B*.png (with the menus and windows), <work-dir>/*.log, <work-dir>/A.rows
# (the Hotlist rows over time).
set -uo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
source "$ROOT/toolchain/env.sh"
BUILD=$(realpath "${1:-$ROOT/build/ui-g}")
W=$(realpath -m "${2:-$ROOT/build/ui-g-hot}")
EXE=$BUILD/bin/octomx
rm -rf "$W"
mkdir -p "$W/A" "$W/B" "$W/share-B"
# a neutral 6 MB text file Bob shares
for i in $(seq 1 150000); do echo "OctoMX hotlist check sample line $i"; done > "$W/share-B/hotlist-check-sample.txt"
# MX_OVERLAY_DISCOVERY=0: the two instances meet only each other, never real users on the internet
export LC_ALL=C.UTF-8 QT_QPA_PLATFORM=offscreen MX_PORT_MAPPING=0 MX_OVERLAY_DISCOVERY=0 MX_ACCEPT_TERMS=1

settings() {   # <profile> <name> <tcp> <udp> <peer-tcp> <extra json>
    cat > "$1/settings.json" <<JSON
{ "internal/firstRunDone": true, "internal/wizardDone": true, "ui/chatWarningDisabled": true,
  "identity/screenName": "$2", "connection/tcpPort": $3, "connection/udpPort": $4,
  "connection/upnp": false, "connection/natPmp": false, "updates/checkAtStart": false,
  "bt/enabled": false, "ed2k/enabled": false, "kad/enabled": false, "gnutella/enabled": false,
  "web/enabled": false, "rats/enabled": false,
  "overlay/enabled": true, "overlay/bootstrapPeers": ["127.0.0.1:$5"],
  "transfers/incomingDir": "$1/incoming", "transfers/incompleteDir": "$1/incomplete",
  "misc/logLevel": "debug" $6 }
JSON
}
settings "$W/A" AliceNG_11111 46711 46712 46721 ''
# PRIVACY=everyone runs the same steps with Bob open to everybody (to tell privacy from delivery)
PRIV=${PRIVACY:-hotlist}
settings "$W/B" BobNG_22222 46721 46722 46711 ", \"messages/allowPrivateMessages\": \"$PRIV\", \"messages/allowBrowse\": \"$PRIV\", \"bandwidth/upLimitKBs\": 150"

# Bob: share, join the room, add Alice from his upload list, quit when told
cat > "$W/B1.script" <<EOF
page shared
share-folder $W/share-B
wait 6000
page chat
join HotlistCheck
wait 50000
page transfers
wait 500
shot $W/B01_uploads.png
context-menu-text uploadList AliceNG
shot $W/B02_uploadmenu.png
menu-pick Add To Hotlist
hotlist on
wait 1500
shot $W/B03_hotlist.png
wait-file $W/stopB 600
shot $W/B04_before_quit.png
quit
EOF
cat > "$W/B2.script" <<EOF
wait 2000
page chat
join HotlistCheck
wait 600000
EOF

R=$W/A.rows
cat > "$W/A.script" <<EOF
wait 9000
page chat
join HotlistCheck
wait 5000
report-rows chatUserList $R
shot $W/A01_room.png
context-menu-text chatUserList BobNG
shot $W/A02_usermenu.png
menu-pick Add To Hotlist
hotlist on
wait 1500
echo hotlist after adding from chat
report-rows hotlistList $R
shot $W/A03_hotlist_added.png
context-menu-text chatUserList BobNG
menu-pick Send Message
wait 500
pm-say BobNG hello from Alice (before Bob added her)
wait 2500
shot $W/A04_pm_before.png
close-dialogs
page search
type searchText hotlist check sample
key searchText Return
wait 9000
shot $W/A05_search.png
context-menu-text searchResults hotlist-check-sample
shot $W/A06_searchmenu.png
close-menus
dblclick-text searchResults hotlist-check-sample
wait 5000
page transfers
expand-row downloadList 0
wait 500
shot $W/A07_transfers.png
context-menu-text downloadList BobNG
shot $W/A08_transfermenu.png
close-menus
wait 30000
echo after Bob added Alice
report-rows hotlistList $R
context-menu-text hotlistList BobNG
shot $W/A09_hotlistmenu.png
menu-pick Whois
wait 3000
shot $W/A10_whois.png
context-menu-text hotlistList BobNG
menu-pick Browse
wait 4000
shot $W/A11_browse.png
context-menu-text hotlistList BobNG
menu-pick Send Message
wait 500
pm-say BobNG hello again from Alice (after Bob added her)
wait 3000
shot $W/A12_pm_after.png
close-dialogs
echo-file $W/A.phase1
wait 15000
echo 15 s after Bob quit normally
report-rows hotlistList $R
notice
shot $W/A13_bob_quit.png
echo-file $W/A.phase2
wait 30000
echo after Bob came back
report-rows hotlistList $R
notice
shot $W/A14_bob_online_again.png
echo-file $W/A.phase3
wait 60000
echo 60 s after Bob was killed
report-rows hotlistList $R
shot $W/A15_bob_killed.png
quit
EOF

waitfor() { for i in $(seq 1 400); do [ -e "$1" ] && return 0; kill -0 $APID 2>/dev/null || return 1; sleep 0.5; done; return 1; }
echo "== B starts ($(date +%T))"
"$EXE" --profile "$W/B" --size 1100x760 --script "$W/B1.script" > "$W/B1.log" 2>&1 &
BPID=$!
sleep 3
echo "== A starts ($(date +%T))"
"$EXE" --profile "$W/A" --size 1200x800 --script "$W/A.script" > "$W/A.log" 2>&1 &
APID=$!
waitfor "$W/A.phase1"
echo "== B quits normally ($(date +%T))"
touch "$W/stopB"
wait $BPID 2>/dev/null
waitfor "$W/A.phase2"
echo "== B starts again ($(date +%T))"
"$EXE" --profile "$W/B" --size 1100x760 --script "$W/B2.script" > "$W/B2.log" 2>&1 &
BPID=$!
waitfor "$W/A.phase3"
echo "== B is killed, as in a crash ($(date +%T))"
kill -9 $BPID 2>/dev/null; wait $BPID 2>/dev/null
wait $APID; AEXIT=$?
echo "A exit $AEXIT"
grep -h 'script line' "$W"/*.log
echo "== Hotlist rows over time (A.rows)"
cat "$R" 2>/dev/null
grep -E '^(echo|notice)' "$W/A.log"
exit $AEXIT
