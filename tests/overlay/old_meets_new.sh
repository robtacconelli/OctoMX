#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# [ren-1] Old meets new: a user of a copy installed before 5.0 (version 4.5, legacy names) moves to
# the current version and loses nothing. PASS / FAIL per check.
#
#   tests/overlay/old_meets_new.sh <old program (4.5 bin/winmx)> <new program (bin/octomx)> [work-dir]
#
# HOME and the XDG folders point into <work-dir> (never the real ones); the programs run offscreen
# with their default profile (no --profile), so they use the standard folders below that HOME:
#   1. 4.5 writes its profile at the old default place (~/.local/share/WinMX): settings, a shared
#      folder, a Hotlist entry (Bob, a second instance on the loopback only) and a download from Bob
#      that is paused half way (a generated text file).
#   2. The new version starts on the same HOME: it uses ~/.local/share/WinMX as it is (no OctoMX folder, the
#      database stays winmx.db), finds the settings, the shared folder, the Hotlist entry and the
#      paused download, resumes it and completes it into ~/Downloads/WinMX (the 4.x default).
#      (OMN_EARLIER_DOWNLOAD=1: ~/Downloads/WinMX already exists, so it stays the default folder;
#      without it the folder does not exist yet and the new version uses ~/Downloads/OctoMX.)
#   3. Single instance: 4.5 runs on that profile, the new version started then hands over and exits; and the
#      other way round.
# Only the two instances' own OctoMX network on 127.0.0.1 (MX_OVERLAY_DISCOVERY=0), no update check,
# no port mapping, no OS registrations (fake backend). Evidence: <work-dir>/*.log, *.png.
set -uo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
source "$ROOT/toolchain/env.sh"
[ $# -ge 2 ] || { echo "usage: $0 <old program> <new program> [work-dir]" >&2; exit 2; }
OLD=$(realpath "$1"); NEW=$(realpath "$2")
W=$(realpath -m "${3:-$ROOT/build/old-meets-new}")
case "$W" in "$ROOT"/build/*) ;; *) echo "work dir must be under $ROOT/build" >&2; exit 2;; esac
rm -rf "$W"
H=$W/home
mkdir -p "$H/Downloads" "$H/.config" "$H/.local/share" "$H/.cache" "$W/B" "$W/share-A" "$W/share-B" "$W/sync"
S=$W/sync
for i in $(seq 1 120000); do echo "old meets new sample line $i of a plain text file"; done > "$W/share-B/omn-sample.txt"
echo "Alice's own list" > "$W/share-A/alice-list.txt"
# OMN_EARLIER_DOWNLOAD=1: the user has downloaded with 4.x before (its default folder exists)
if [ "${OMN_EARLIER_DOWNLOAD:-0}" = 1 ]; then mkdir -p "$H/Downloads/WinMX"; echo "an earlier download" > "$H/Downloads/WinMX/earlier.txt"; fi
WANT=$(sha256sum "$W/share-B/omn-sample.txt" | cut -d' ' -f1)
# the user's environment: HOME and XDG inside the work dir; no MX_OS_ROOT (that would make the
# profile "isolated"), so the programs use their default folders below this HOME
unset MX_OS_ROOT MX_PROFILE_EXPLICIT
export HOME=$H XDG_CONFIG_HOME=$H/.config XDG_DATA_HOME=$H/.local/share XDG_CACHE_HOME=$H/.cache
export LC_ALL=C.UTF-8 QT_QPA_PLATFORM=offscreen MX_PORT_MAPPING=0 MX_OVERLAY_DISCOVERY=0 MX_OS_INTEGRATION=fake
export QT_LOGGING_RULES="mx.overlay.debug=true"
BASE=$(( 41000 + (RANDOM % 1500) * 10 ))
PA=$BASE; PB=$((BASE + 10))
OLDPROF=$XDG_DATA_HOME/WinMX
NEWPROF=$XDG_DATA_HOME/OctoMX

settings() {   # <file> <name> <tcp> <bootstrap port> <extra json>
    mkdir -p "$(dirname "$1")"
    cat > "$1" <<JSON
{ "internal/firstRunDone": true, "internal/wizardDone": true, "internal/termsAccepted": 1, "ui/chatWarningDisabled": true,
  "identity/screenName": "$2", "connection/tcpPort": $3, "connection/udpPort": $(( $3 + 1 )),
  "connection/upnp": false, "connection/natPmp": false, "updates/checkAtStart": false, "updates/mode": "off",
  "bt/enabled": false, "ed2k/enabled": false, "kad/enabled": false, "gnutella/enabled": false,
  "web/enabled": false, "rats/enabled": false, "http/enabled": false,
  "overlay/enabled": true, "overlay/bootstrapPeers": ["127.0.0.1:$4"],
  "misc/logLevel": "debug" $5 }
JSON
}
# Alice: the user who updates (default profile); Bob: a second instance that shares the file
settings "$OLDPROF/settings.json" AliceNG $PA $PB ''
settings "$W/B/settings.json" BobNG $PB $PA ', "transfers/incomingDir": "'"$W"'/B/incoming", "bandwidth/upLimitKBs": 60, "bandwidth/limitLan": true'

cat > "$W/B.script" <<EOF
page shared
share-folder $W/share-B
wait 3000
page chat
join NetCheck
echo-file $S/B.ready
wait-file $S/A.done 900
quit
EOF
cat > "$W/A45.script" <<EOF
set misc/logKeepFiles 7
page shared
share-folder $W/share-A
wait-file $S/B.ready 120
page chat
join NetCheck
wait 6000
context-menu-text chatUserList BobNG
menu-pick Add To Hotlist
wait 1000
select-text hotlistList BobNG
page search
type searchText omn sample
key searchText Return
wait 8000
dblclick-text searchResults omn-sample
wait 12000
page transfers
context-menu-text downloadList omn-sample
menu-pick Pause
wait 2000
shot $W/A45_paused.png
echo-file $S/A.done
quit
EOF
a50_script() {   # <expected default incoming folder> <screen name 4.5 stored>
cat > "$W/A50.script" <<EOF
expect-setting identity/screenName $2
expect-setting misc/logKeepFiles 7
expect-setting transfers/incomingDir $1
page shared
expect-rows sharedList 1
select-text sharedList share-A
select-text hotlistList BobNG
page transfers
select-text downloadList omn-sample
shot $W/A50_found.png
wait-file $S/B.ready 120
wait 4000
context-menu-text downloadList omn-sample
menu-pick Resume
wait-download 300 omn-sample
shot $W/A50_done.png
echo-file $S/A.done
quit
EOF
}

pass=0; fails=0
ok() { echo "PASS  $*"; pass=$((pass + 1)); }
bad() { echo "FAIL  $*"; fails=$((fails + 1)); }
waitfile() { for _ in $(seq 1 1200); do [ -e "$1" ] && return 0; kill -0 "$2" 2>/dev/null || return 1; sleep 0.5; done; return 1; }
stop() { for p in "$@"; do for _ in $(seq 1 40); do kill -0 "$p" 2>/dev/null || break; sleep 0.5; done; kill -9 "$p" 2>/dev/null; wait "$p" 2>/dev/null; done; }

# ------------------------------------------------------------------------- 1: 4.5 writes the profile
echo "== 1: version 4.5 on the default profile ($OLDPROF), Bob on $PB"
(cd "$W" && exec "$OLD" --profile "$W/B" --size 900x650 --script "$W/B.script") > "$W/B1.log" 2>&1 &
BPID=$!
(cd "$W" && exec "$OLD" --size 1024x714 --script "$W/A45.script") > "$W/A45.log" 2>&1 &
APID=$!
wait $APID; ARC=$?
stop $BPID
[ $ARC = 0 ] && ok "4.5: every script command passed" || bad "4.5 script rc=$ARC: $(grep -m1 'script line' "$W/A45.log" | cut -c1-160)"
[ -f "$OLDPROF/winmx.db" ] && ok "4.5 wrote $OLDPROF/winmx.db" || bad "no winmx.db in the old profile"
[ ! -e "$NEWPROF" ] && ok "no OctoMX folder yet" || bad "$NEWPROF exists after the 4.5 run"
PART=$(find "$OLDPROF/incomplete" -type f 2>/dev/null | head -n1)
[ -n "$PART" ] && ok "4.5 left a partial download: $(basename "$PART") ($(stat -c %s "$PART") bytes)" || bad "no partial file of the paused download"
rm -f "$S/B.ready" "$S/A.done"
cp -a "$OLDPROF" "$W/profile-written-by-4.5"
# the default download folder of 5.0: the 4.x one when it exists (rule of src/api/legacy_names.cpp)
if [ -d "$H/Downloads/WinMX" ]; then INC=$H/Downloads/WinMX; else INC=$H/Downloads/OctoMX; fi
echo "   ~/Downloads/WinMX $( [ -d "$H/Downloads/WinMX" ] && echo exists || echo 'does not exist (yet)'): 5.0 should default to $INC"
NAME=$(sed -n 's/.*"identity\/screenName": "\([^"]*\)".*/\1/p' "$OLDPROF/settings.json" | head -n1)
echo "   4.5 stored the screen name $NAME"
a50_script "$INC" "$NAME"

# ---------------------------------------------------------------------- 2: 5.0 opens it where it is
echo "== 2: OctoMX 5.0 on the same HOME"
(cd "$W" && exec "$NEW" --profile "$W/B" --size 900x650 --script "$W/B.script") > "$W/B2.log" 2>&1 &
BPID=$!
(cd "$W" && exec "$NEW" --size 1024x714 --script "$W/A50.script") > "$W/A50.log" 2>&1 &
APID=$!
wait $APID; ARC=$?
stop $BPID
[ $ARC = 0 ] && ok "5.0: every script command passed (settings, shared folder, Hotlist, paused download found; resumed; complete)" \
             || bad "5.0 script rc=$ARC: $(grep -m1 'script line' "$W/A50.log" | cut -c1-200)"
[ ! -e "$NEWPROF" ] && ok "5.0 used the old profile folder: no $NEWPROF was created" || bad "$NEWPROF was created"
[ -f "$OLDPROF/winmx.db" ] && [ ! -e "$OLDPROF/octomx.db" ] && ok "the database is still winmx.db (no octomx.db)" || bad "database file names: $(ls "$OLDPROF" | tr '\n' ' ')"
[ -f "$OLDPROF/logs/octomx.log" ] && ok "5.0 logs to logs/octomx.log ($(grep -m1 -o 'OctoMX [0-9.]* starting.*database [^ ]*' "$OLDPROF/logs/octomx.log" | cut -c1-140))" || bad "no logs/octomx.log"
[ -f "$OLDPROF/logs/winmx.log" ] && ok "the 4.5 log logs/winmx.log is left alone" || bad "logs/winmx.log is gone"
# the resumed download completes into the default folder of 5.0 (the 4.x one when it existed)
GOT=$(sha256sum "$INC/omn-sample.txt" 2>/dev/null | cut -d' ' -f1)
[ "$GOT" = "$WANT" ] && ok "the resumed download completed in ${INC#$H/}, SHA-256 matches" \
                     || bad "download: $(find "$H/Downloads" -type f | head -3 | tr '\n' ' ')"
if [ "$INC" = "$H/Downloads/WinMX" ]; then
    [ ! -e "$H/Downloads/OctoMX" ] && ok "no ~/Downloads/OctoMX (the 4.x folder stays the default)" || bad "~/Downloads/OctoMX was created"
fi
NEWV=$("$NEW" --version 2>/dev/null | grep -o '[0-9][0-9.]*' | head -n1)
grep -q '"internal/lastVersion": "'"$NEWV"'"' "$OLDPROF/settings.json" && ok "internal/lastVersion is now $NEWV" || bad "lastVersion: $(grep -o '"internal/lastVersion": "[^"]*"' "$OLDPROF/settings.json")"

# ---------------------------------------------------------------------------- 3: single instance
echo "== 3: one program per profile, old and new"
for pair in "4.5:$OLD:5.0:$NEW" "5.0:$NEW:4.5:$OLD"; do
    IFS=: read -r n1 p1 n2 p2 <<<"$pair"
    before=$(cat "$OLDPROF"/logs/*.log | grep -c "link(s) forwarded by a second instance")
    (cd "$W" && MX_ACCEPT_TERMS=1 exec "$p1" --no-wizard --size 800x600) > "$W/single-$n1.log" 2>&1 &
    P1=$!
    sleep 6
    start=$(date +%s)
    (cd "$W" && timeout 30 "$p2" "http://127.0.0.1:9/single-instance-check-$n2.bin") > "$W/single-$n2-second.log" 2>&1
    rc=$?; took=$(( $(date +%s) - start ))
    sleep 2
    if [ $rc = 0 ] && [ $took -lt 15 ] && kill -0 $P1 2>/dev/null; then
        ok "$n2 started while $n1 runs on the same profile: handed over and exited in ${took} s (rc 0)"
    else
        bad "$n2 second start: rc=$rc after ${took} s, first instance alive: $(kill -0 $P1 2>/dev/null && echo yes || echo no)"
    fi
    if [ "$(cat "$OLDPROF"/logs/*.log | grep -c "link(s) forwarded by a second instance")" -gt "$before" ]; then
        ok "$n1 received the link of the second program"
    else
        bad "$n1 did not log the forwarded link"
    fi
    kill $P1 2>/dev/null; stop $P1
done
for p in $(pgrep -f "^$OLD|^$NEW"); do kill -9 "$p" 2>/dev/null; done
echo "== $pass passed, $fails failed; evidence in $W"
exit $(( fails > 0 ))
