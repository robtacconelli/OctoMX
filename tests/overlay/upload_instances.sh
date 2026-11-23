#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# [up-1] Uploading between real program instances on every network: PASS / FAIL per scenario and the
# proof table (network x complete / partial: result, speed, wasted bytes, bans).
#
#   tests/overlay/upload_instances.sh [--wine] [--old <5.2 mxcli>] [--only "<scenario> ..."] [build-dir] [work-dir]
#
# Every instance is an mxcli process (the real core and every plugin, no GUI) with its own profile and
# ports. The whole run happens inside its own network namespace (unshare -rn): the instances reach
# each other on 127.0.0.1 and nothing else, so no eD2k server, Gnutella host cache, DHT or OctoMX user
# outside is ever contacted (MX_OVERLAY_DISCOVERY=0 as well). Content: generated random files only.
# Roles: S seeds a torrent, C shares a complete file on the overlay, H is a throttled local HTTP
# server; A downloads from one of them slowly and shares while it downloads; B downloads from A ONLY
# (or a foreign client does: aria2c for BitTorrent, curl for the Gnutella HTTP upload).
# --old: B (and A, in the "a52" scenarios) run that binary instead: the 5.2 pairings.
# --wine runs the Windows build (build-dir default build/win) under wine; foreign clients are skipped.
# Evidence: <work-dir>/<scenario>/*.log, *.up (upload rows), *.src (source rows). Exit 0 when all passed.
set -uo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
source "$ROOT/toolchain/env.sh"
if [ -z "${UP1_IN_NS:-}" ]; then   # a private network namespace: loopback only
    # outer user namespace (root inside: may bring lo up), inner one with our own uid again (wine
    # refuses to run as root); both share the new network namespace
    export UP1_IN_NS=1 UP1_UID=$(id -u) UP1_GID=$(id -g)
    exec unshare -rn bash -c 'ip link set lo up && exec unshare -U --map-user="$UP1_UID" --map-group="$UP1_GID" "$0" "$@"' "$0" "$@"
fi
WINE=0; OLD=""; ONLY=""
while [ $# -gt 0 ]; do
    case "$1" in
        --wine) WINE=1; shift ;;
        --old) OLD=$(realpath "$2"); shift 2 ;;
        --only) ONLY=" $2 "; shift 2 ;;
        *) break ;;
    esac
done
if [ $WINE = 1 ]; then
    BUILD=$(realpath "${1:-$ROOT/build/win}"); W=$(realpath -m "${2:-$ROOT/build/up1-inst-wine}")
else
    BUILD=$(realpath "${1:-$ROOT/build/linux}"); W=$(realpath -m "${2:-$ROOT/build/up1-inst}")
fi
case "$W" in "$ROOT"/build/*) ;; *) echo "work dir must be under $ROOT/build" >&2; exit 2;; esac
rm -rf "$W"; mkdir -p "$W"
export LC_ALL=C.UTF-8 QT_QPA_PLATFORM=offscreen MX_PORT_MAPPING=0 MX_OVERLAY_DISCOVERY=0
if [ $WINE = 1 ]; then
    export WINEDEBUG=-all WINEPATH="$(winepath -w "$QT_WIN_DIR/bin");$(winepath -w "$LLVM_MINGW_ROOT/x86_64-w64-mingw32/bin")"
    NEW=("wine" "$BUILD/bin/mxcli.exe")
else
    NEW=("$BUILD/bin/mxcli")
fi
SIZE=${UP1_SIZE:-$((30 * 1024 * 1024 + 12345))}   # 4 eD2k parts, the last one short
SLOW=${UP1_SLOW_KBS:-700}                          # the source A downloads from (KB/s)
PY=(python3 -I)
FAILS=0
TABLE="$W/table.txt"
: > "$TABLE"

# ------------------------------------------------------------------------------------------ helpers
# a path as the program sees it: under wine a Unix path needs its drive (libtorrent takes "/mnt/..." as
# relative on Windows)
wp() { if [ $WINE = 1 ]; then echo "Z:$1"; else echo "$1"; fi; }
wpw() { if [ $WINE = 1 ]; then winepath -w "$1" 2>/dev/null; else echo "$1"; fi; }   # a file argument (backslashes)
port_base() { echo $(( 30000 + (RANDOM % 2000) * 10 )); }
# settings <dir> <name> <base> <nets "bt ed2k gnut ovl http"> <bootstrap ports...> -- <extra json>
settings() {
    local dir=$1 name=$2 base=$3 nets=$4; shift 4
    local boot="" extra=""
    while [ $# -gt 0 ] && [ "$1" != "--" ]; do boot="$boot${boot:+, }\"127.0.0.1:$1\""; shift; done
    [ "${1:-}" = "--" ] && { shift; extra=${1:-}; }
    on() { case " $nets " in *" $1 "*) echo true ;; *) echo false ;; esac; }
    mkdir -p "$dir/incoming" "$dir/incomplete"
    cat > "$dir/settings.json" <<JSON
{ "internal/firstRunDone": true, "internal/wizardDone": true, "internal/termsAccepted": 1,
  "identity/screenName": "$name", "connection/tcpPort": $base, "connection/udpPort": $((base + 1)),
  "connection/upnp": false, "connection/natPmp": false, "updates/checkAtStart": false,
  "bt/enabled": $(on bt), "bt/listenPort": $((base + 2)), "bt/dht": false, "bt/lsd": false, "bt/extraTrackers": "",
  "bt/seedRatioLimit": 0, "bt/allowMultipleConnectionsPerIp": true,
  "ed2k/enabled": $(on ed2k), "ed2k/tcpPort": $((base + 3)), "ed2k/udpPort": $((base + 4)), "ed2k/autoUpdateServerList": false, "ed2k/allowLan": true,
  "kad/enabled": false, "gnutella/enabled": $(on gnut), "gnutella/port": $((base + 5)),
  "web/enabled": false, "rats/enabled": false, "http/enabled": $(on http),
  "overlay/enabled": $(on ovl), "overlay/bootstrapPeers": [$boot],
  "bandwidth/limitLan": true, "transfers/incomingDir": "$(wp "$dir/incoming")", "transfers/incompleteDir": "$(wp "$dir/incomplete")",
  "misc/logLevel": "debug" $extra }
JSON
}
# run <exe-array-name> <scenario dir> <instance> <script lines...>: in the background, pid in RUNPID
run() {
    local -n exe=$1; local sd=$2 who=$3; shift 3
    printf '%s\n' "$@" > "$sd/$who.mx"
    MXCLI_UPLOG="$sd/$who.up" MXCLI_SRCLOG="$sd/$who.src" "${exe[@]}" --script "$sd/$who.mx" --profile "$sd/$who" > "$sd/$who.log" 2>&1 &
    RUNPID=$!
}
waitfile() { for _ in $(seq 1 $(( ${2:-300} * 2 ))); do [ -e "$1" ] && return 0; sleep 0.5; done; return 1; }
payload() { head -c "$2" /dev/urandom > "$1"; }
hashes_of() {   # <file> → "ed2k sha1 sha1base32", computed by the program's own library hasher
    local hd; hd=$(mktemp -d "$W/hash.XXXXXX")
    settings "$hd/P" Guest000_00000 $(port_base) ""
    printf '%s\n' "share $(dirname "$1")" "wait-hashed --count 1 --timeout 120" "hashes" > "$hd/P.mx"
    "${NEW[@]}" --script "$hd/P.mx" --profile "$hd/P" > "$hd/P.log" 2>&1
    local line; line=$(grep "^shared: $(basename "$1") " "$hd/P.log" | head -1)
    local e s; e=$(echo "$line" | grep -o 'ed2k=[0-9a-f]*' | cut -d= -f2); s=$(echo "$line" | grep -o 'sha1=[0-9a-f]*' | cut -d= -f2)
    echo "$e $s $("${PY[@]}" -c 'import base64,sys; print(base64.b32encode(bytes.fromhex(sys.argv[1])).decode())' "$s")"
    rm -rf "$hd"
}
magnet() { echo "magnet:?xt=urn:ed2k:$2&xt=urn:sha1:$3&xl=$4&dn=$1"; }
# verdict <scenario> <network> <kind> <B log> <file got> <file want> [extra checks status] [note]
verdict() {
    local scen=$1 net=$2 kind=$3 blog=$4 got=$5 want=$6 extra=${7:-0} note=${8:-}
    local res
    res=$("${PY[@]}" - "$blog" "$got" "$want" "$extra" "$note" <<'PYEOF'
import hashlib, json, os, re, sys
blog, got, want, extra, note = sys.argv[1:6]
def sha(p):
    if not os.path.exists(p): return None
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''): h.update(b)
    return h.hexdigest()
text = open(blog, errors='replace').read() if os.path.exists(blog) else ''
st = {}
for line in text.splitlines():
    if line.startswith('stats: '):
        try: st = json.loads(line[7:])
        except Exception: pass
secs = None
m = re.findall(r'\[\s*(\d+)s\] Complete', text)
if m: secs = max(1, int(m[-1]))
size = os.path.getsize(want)
ok_sha = sha(got) == sha(want) and sha(want) is not None
wasted = int(st.get('failedUnitBytes', 0) or 0)
bans = sum(1 for s in st.get('sources', []) if s.get('banned'))
why = '; '.join(s.get('why', '') for s in st.get('sources', []) if s.get('banned'))
speed = '%.0f KB/s' % (size / 1024.0 / secs) if secs else '-'
passed = ok_sha and wasted == 0 and bans == 0 and extra == '0'
print('%s|%s|%s|%s|%d|%d|%s' % ('PASS' if passed else 'FAIL', 'sha256 equal' if ok_sha else 'sha256 DIFFERS/missing',
      speed, wasted, bans, int(st.get('unitFailures', 0) or 0), (note + (' banned: ' + why if why else '')).strip()))
PYEOF
)
    local pf=${res%%|*}
    echo "$pf  $scen  ($net, $kind): ${res#*|}"
    printf '%-22s %-10s %-8s %s\n' "$scen" "$net" "$kind" "${res}" >> "$TABLE"
    [ "$pf" = PASS ] || FAILS=$((FAILS + 1))
}
# rowcheck <A.up> <net short name> <B log> → 0 when the uploader's rows agree with what B received
# from it: a running row showed bytes and a rate, and the last row's bytes match B's count (+1 MiB)
rowcheck() {
    "${PY[@]}" - "$1" "$2" "$3" <<'PYEOF'
import json, os, re, sys
up, net, blog = sys.argv[1:4]
rows = [l for l in open(up)] if os.path.exists(up) else []
rows = [l for l in rows if (' net=%s ' % net) in l and ('to=Guest200' in l or 'to=127.0.0.1' in l)]
def f(l, k):
    m = re.search(r'\b%s=(\d+)' % k, l)
    return int(m.group(1)) if m else 0
running = any(f(l, 'state') == 4 and f(l, 'done') > 0 and f(l, 'rate') > 0 for l in rows)
lastById = {}
for l in rows:
    m = re.search(r' id=(\S+) ', l)
    lastById[m.group(1) if m else ''] = f(l, 'done')
last = sum(lastById.values())   # every upload row (session) to B, as the Transfers page lists them
got = 0
for l in open(blog, errors='replace'):
    if l.startswith('stats: '):
        st = json.loads(l[7:])
        names = {'ed2k': 'eD2k', 'Gnut': 'Gnutella', 'MX': 'OctoMX'}
        got = sum(int(x.get('bytes', 0)) for x in st.get('sources', []) if x.get('net') == names.get(net, net))
ok = running and got > 0 and got <= last <= got + (1 << 20)
print('%d row-bytes=%d received-by-B=%d running-row-with-rate=%s' % (0 if ok else 1, last, got, 'yes' if running else 'NO'))
PYEOF
}
# partial_verdict <scenario> <network> <B log> <original> <rowcheck result>: B stopped before the end
# (the network learns a partial source's new parts only when it asks again); what it holds must be
# right byte for byte, nothing wasted, nobody banned
partial_verdict() {
    local n=$1 net=$2 blog=$3 orig=$4 rc=$5
    local res; res=$("${PY[@]}" - "$blog" "$orig" "$net" <<'PYEOF'
import json, sys
blog, orig, net = sys.argv[1:4]
st = {}
for l in open(blog, errors='replace'):
    if l.startswith('stats: '): st = json.loads(l[7:])
src = [x for x in st.get('sources', []) if x.get('net') == net]
got = sum(int(x.get('bytes', 0)) for x in src)
bans = sum(1 for x in src if x.get('banned')); waste = int(st.get('failedUnitBytes', 0) or 0)
held = st.get('heldRanges') or []
path = st.get('partPath') or st.get('path')
right = 0; wrong = 0
try:
    with open(path, 'rb') as a, open(orig, 'rb') as b:
        for s, e in held:
            a.seek(s); b.seek(s)
            if a.read(e - s) == b.read(e - s): right += e - s
            else: wrong += e - s
except Exception as ex:
    wrong = -1
done = st.get('state') == 7
ok = got > 0 and wrong == 0 and bans == 0 and waste == 0
print('%s|%d bytes from A while it was partial, %d held bytes equal to the original%s|-|%d|%d|%d|%s' % (
    'PASS' if ok else 'FAIL', got, right, '' if wrong == 0 else ' (%d differ)' % wrong, waste, bans,
    int(st.get('unitFailures', 0) or 0), 'complete' if done else 'the rest comes when the network asks the source again (not waited for)'))
PYEOF
)
    local pf=${res%%|*}; [ "${rc%% *}" = 0 ] || pf=FAIL
    [ "$pf" = PASS ] || FAILS=$((FAILS + 1))
    echo "$pf  $n ($net, partial): ${res#*|}; upload row vs receiver: ${rc#* }"
    printf '%-22s %-10s %-8s %s\n' "$n" "$net" partial "$pf|${res#*|}|row vs receiver: ${rc#* }" >> "$TABLE"
}
stopall() { touch "$1/stop"; for p in "${@:2}"; do for _ in $(seq 1 40); do kill -0 "$p" 2>/dev/null || break; sleep 0.25; done; kill -9 "$p" 2>/dev/null; wait "$p" 2>/dev/null; done; }
want() { [ -z "$ONLY" ] || case "$ONLY" in *" $1 "*) return 0 ;; *) return 1 ;; esac; }
BEXE=("${NEW[@]}"); [ -n "$OLD" ] && BEXE=("$OLD")
AEXE=("${NEW[@]}")

# ------------------------------------------------------------------------------------------ scenarios
# overlay, complete file: A shares it, B asks by its hashes
scen_ovl_complete() {
    local n=ovl-complete sd="$W/ovl-complete"; mkdir -p "$sd/share"; local b; b=$(port_base)
    local f="$sd/share/octomx-upload-ovl-complete.bin"; payload "$f" $SIZE; read -r E S1 B32 < <(hashes_of "$f")
    settings "$sd/A" Guest100_00101 $b "ovl" $((b + 10)) -- ", \"bandwidth/upLimitKBs\": $SLOW"; settings "$sd/B" Guest200_00202 $((b + 10)) "ovl" $b
    run AEXE "$sd" A "share $sd/share" "wait-hashed --count 1 --timeout 120" "touch $sd/A.ready" "wait-file $sd/stop --timeout 900" "uploads"; local pa=$RUNPID
    waitfile "$sd/A.ready" 120
    run BEXE "$sd" B "wait-peers OctoMX 1 --timeout 60" "get $(magnet octomx-upload-ovl-complete.bin $E $S1 $SIZE) --until-done --timeout 300 --every 5" "stats last"; local pb=$RUNPID
    wait $pb; sleep 2; stopall "$sd" $pa
    local rows; rows=$(grep -c 'upload-row: .*net=MX.*status="Complete"' "$sd/A.log")
    local rc; rc=$(rowcheck "$sd/A.up" MX "$sd/B.log")
    verdict $n OctoMX complete "$sd/B.log" "$sd/B/incoming/octomx-upload-ovl-complete.bin" "$f" $([ "$rows" -ge 1 ] && [ "${rc%% *}" = 0 ] && echo 0 || echo 1) "upload row vs receiver: ${rc#* }; A's row: $(grep -o 'upload-row: .*net=MX.*' "$sd/A.log" | tail -1 | grep -o 'status=.*')"
}
# overlay, partial (case 1): A downloads from C (complete, one slot, no queue, slow) with the
# trusted ed2k set; B gets it from A while A is still downloading (C refuses B: no slot)
scen_ovl_partial() {
    local n=ovl-partial sd="$W/ovl-partial"; mkdir -p "$sd/share"; local b; b=$(port_base)
    local f="$sd/share/octomx-upload-ovl-partial.bin"; payload "$f" $SIZE; read -r E S1 B32 < <(hashes_of "$f")
    local M; M=$(magnet octomx-upload-ovl-partial.bin $E $S1 $SIZE)
    settings "$sd/C" Guest300_00303 $((b + 20)) "ovl" $b -- ", \"transfers/maxUploads\": 1, \"queue/enabled\": false, \"bandwidth/upLimitKBs\": $SLOW"
    settings "$sd/A" Guest100_00101 $b "ovl" $((b + 20)) $((b + 10)); settings "$sd/B" Guest200_00202 $((b + 10)) "ovl" $b
    run AEXE "$sd" C "share $sd/share" "wait-hashed --count 1 --timeout 120" "touch $sd/C.ready" "wait-file $sd/stop --timeout 900" "uploads"; local pc=$RUNPID
    waitfile "$sd/C.ready" 120
    run AEXE "$sd" A "wait-peers OctoMX 1 --timeout 60" "get $M --until-bytes $((SIZE / 3)) --timeout 300 --every 5" "touch $sd/A.partial" "watch last --timeout 400 --every 5" "touch $sd/A.done" "wait-file $sd/stop --timeout 900" "uploads"; local pa=$RUNPID
    waitfile "$sd/A.partial" 300
    run BEXE "$sd" B "wait-peers OctoMX 1 --timeout 60" "get $M --until-done --timeout 500 --every 5" "stats last"; local pb=$RUNPID
    wait $pb; sleep 2; stopall "$sd" $pa $pc
    # B got nothing from C, and something from A before A was complete
    local fromC; fromC=$(grep -c 'net=MX to=Guest200' "$sd/C.up" 2>/dev/null)
    local early; early=$("${PY[@]}" - "$sd/A.up" "$sd/A.log" <<'PYEOF'
import re, sys
rows = [l for l in open(sys.argv[1]) if 'net=MX to=Guest200' in l and 'done=0 ' not in l] if __import__('os').path.exists(sys.argv[1]) else []
print(len(rows))
PYEOF
)
    verdict $n OctoMX partial "$sd/B.log" "$sd/B/incoming/octomx-upload-ovl-partial.bin" "$f" $([ "${fromC:-0}" = 0 ] && [ "$early" -gt 0 ] && echo 0 || echo 1) "rows from C to B: ${fromC:-0}; A's last row: $(grep 'to=Guest200' "$sd/A.up" | tail -1 | grep -o 'done=[0-9]* .*')"
}
# overlay, case 2: A knows the ed2k hash but has no part set (HTTP source): it offers nothing until
# complete, then B gets the whole file from it
scen_ovl_noset() {
    local n=ovl-noset sd="$W/ovl-noset"; mkdir -p "$sd/web"; local b; b=$(port_base)
    local f="$sd/web/octomx-upload-ovl-noset.bin"; payload "$f" $SIZE; read -r E S1 B32 < <(hashes_of "$f")
    local M; M=$(magnet octomx-upload-ovl-noset.bin $E $S1 $SIZE)
    "${PY[@]}" "$ROOT/tests/overlay/slow_http.py" "$f" $((b + 30)) $((SLOW * 1024)) > "$sd/H.log" 2>&1 & local ph=$!
    waitfile "$sd/H.log" 10; sleep 1
    settings "$sd/A" Guest100_00101 $b "ovl http" $((b + 10)); settings "$sd/B" Guest200_00202 $((b + 10)) "ovl" $b
    run AEXE "$sd" A "wait-peers OctoMX 1 --timeout 60" "get $M --source http://127.0.0.1:$((b + 30))/octomx-upload-ovl-noset.bin --until-bytes $((SIZE / 3)) --timeout 300 --every 5" "touch $sd/A.partial" "watch last --timeout 400 --every 5" "touch $sd/A.done" "wait-file $sd/stop --timeout 900" "uploads"; local pa=$RUNPID
    waitfile "$sd/A.partial" 300
    run BEXE "$sd" B "wait-peers OctoMX 1 --timeout 60" "get $M --until-done --timeout 500 --every 5" "stats last"; local pb=$RUNPID
    wait $pb; sleep 2; stopall "$sd" $pa; kill $ph 2>/dev/null; wait $ph 2>/dev/null
    # nothing went to B before A's download was complete
    local done_at first_row
    done_at=$(date -r "$sd/A.done" +%H:%M:%S.%3N 2>/dev/null); first_row=$(grep -m1 'to=Guest200' "$sd/A.up" 2>/dev/null | cut -c1-12)
    local early=1; [ -n "$done_at" ] && [ -n "$first_row" ] && [[ ! "$first_row" < "$done_at" ]] && early=0
    verdict $n OctoMX "no set" "$sd/B.log" "$sd/B/incoming/octomx-upload-ovl-noset.bin" "$f" $early "A offered nothing while partial (A complete $done_at, first row to B $first_row)"
}
# overlay, case 3: A downloads a torrent from S (slow) into its shared incoming folder; B has the
# .torrent but no BitTorrent peer: it gets every piece from A over the overlay, while A downloads
scen_ovl_torrent() {
    local n=ovl-torrent sd="$W/ovl-torrent"; mkdir -p "$sd/share"; local b; b=$(port_base)
    local f="$sd/share/octomx-upload-ovl-torrent.bin"; payload "$f" $SIZE
    settings "$sd/S" Guest400_00404 $((b + 20)) "bt" -- ", \"bandwidth/upLimitKBs\": $SLOW"
    settings "$sd/A" Guest100_00101 $b "bt ovl" $((b + 10)); settings "$sd/B" Guest200_00202 $((b + 10)) "bt ovl" $b
    run AEXE "$sd" S "share $sd/share" "wait-hashed --count 1 --timeout 120" "seed $(wp "$f")" "touch $sd/S.ready" "wait-file $sd/stop --timeout 900"; local ps=$RUNPID
    waitfile "$sd/S.ready" 180
    local BT; BT=$(grep '^seeded ' "$sd/S.log" | sed 's/.* btih //')
    local TF="$sd/S/bt/torrents/$BT.torrent"
    run AEXE "$sd" A "get magnet:?xt=urn:btih:$BT&dn=octomx-upload-ovl-torrent.bin --peer 127.0.0.1:$((b + 22)) --until-bytes $((SIZE / 3)) --timeout 300 --every 5" "touch $sd/A.partial" "hashes" "watch last --timeout 400 --every 5" "touch $sd/A.done" "wait-file $sd/stop --timeout 900" "uploads"; local pa=$RUNPID
    waitfile "$sd/A.partial" 300
    run BEXE "$sd" B "wait-peers OctoMX 1 --timeout 60" "get $(wpw "$TF") --until-done --timeout 500 --every 5" "stats last"; local pb=$RUNPID
    wait $pb; sleep 2; stopall "$sd" $pa $ps
    local shared; shared=$(grep -c '^shared: octomx-upload-ovl-torrent' "$sd/A.log")
    verdict $n OctoMX "partial (torrent)" "$sd/B.log" "$sd/B/incoming/octomx-upload-ovl-torrent.bin" "$f" $([ "$shared" = 0 ] && echo 0 || echo 1) "A's library did not share the half file: $([ "$shared" = 0 ] && echo yes || echo NO)"
}
# overlay, case 5: A has another file with the same name and size; B downloads the real one by
# torrent from S and meets A by name and size: the ban is right, and A's row says so
scen_ovl_different() {
    local n=ovl-different sd="$W/ovl-different"; mkdir -p "$sd/share" "$sd/other"; local b; b=$(port_base)
    local f="$sd/share/octomx-upload-same-name.bin"; payload "$f" $SIZE; payload "$sd/other/octomx-upload-same-name.bin" $SIZE
    settings "$sd/S" Guest400_00404 $((b + 20)) "bt" -- ", \"bandwidth/upLimitKBs\": $SLOW"
    settings "$sd/A" Guest100_00101 $b "ovl" $((b + 10)); settings "$sd/B" Guest200_00202 $((b + 10)) "bt ovl" $b
    run AEXE "$sd" S "share $sd/share" "wait-hashed --count 1 --timeout 120" "seed $(wp "$f")" "touch $sd/S.ready" "wait-file $sd/stop --timeout 900"; local ps=$RUNPID
    run AEXE "$sd" A "share $sd/other" "wait-hashed --count 1 --timeout 120" "touch $sd/A.ready" "wait-file $sd/stop --timeout 900" "uploads"; local pa=$RUNPID
    waitfile "$sd/S.ready" 180; waitfile "$sd/A.ready" 120
    local BT; BT=$(grep '^seeded ' "$sd/S.log" | sed 's/.* btih //')
    run BEXE "$sd" B "wait-peers OctoMX 1 --timeout 60" "get magnet:?xt=urn:btih:$BT&dn=octomx-upload-same-name.bin --peer 127.0.0.1:$((b + 22)) --until-done --timeout 500 --every 5" "stats last"; local pb=$RUNPID
    wait $pb; sleep 2; stopall "$sd" $pa $ps
    local banned stopped
    banned=$(grep -o '"banned":true[^}]*' "$sd/B.log" | head -1)
    stopped=$(grep -c 'status="Stopped by the other side' "$sd/A.up" 2>/dev/null)
    # here the ban is right: count it as expected, not as a failure
    "${PY[@]}" - "$sd/B.log" > "$sd/B.stats.json" <<'PYEOF'
import json, sys
for l in open(sys.argv[1], errors='replace'):
    if l.startswith('stats: '): print(l[7:].strip())
PYEOF
    local ok=1; [ -n "$banned" ] && [ "${stopped:-0}" -ge 1 ] && ok=0
    local sha_ok=FAIL; cmp -s "$sd/B/incoming/octomx-upload-same-name.bin" "$f" && sha_ok=PASS
    local pf=PASS; [ $ok = 0 ] && [ $sha_ok = PASS ] || { pf=FAIL; FAILS=$((FAILS + 1)); }
    echo "$pf  $n (OctoMX, different content): B complete from S: $sha_ok; A banned by B (right): $([ -n "$banned" ] && echo yes || echo no); A's row: $(grep 'Stopped by the other side' "$sd/A.up" | tail -1 | grep -o 'status=.*')"
    printf '%-22s %-10s %-8s %s\n' "$n" OctoMX different "$pf|B complete: $sha_ok|A banned (right): $([ -n "$banned" ] && echo yes || echo no)|A's row: $(grep 'Stopped by the other side' "$sd/A.up" | tail -1 | grep -o 'status=.*')" >> "$TABLE"
}
# BitTorrent complete: S seeds, B downloads from S
scen_bt_complete() {
    local n=bt-complete sd="$W/bt-complete"; mkdir -p "$sd/share"; local b; b=$(port_base)
    local f="$sd/share/octomx-upload-bt-complete.bin"; payload "$f" $SIZE
    settings "$sd/A" Guest100_00101 $b "bt"; settings "$sd/B" Guest200_00202 $((b + 10)) "bt"
    run AEXE "$sd" A "share $sd/share" "wait-hashed --count 1 --timeout 120" "seed $(wp "$f")" "touch $sd/A.ready" "wait-file $sd/stop --timeout 900" "uploads"; local pa=$RUNPID
    waitfile "$sd/A.ready" 180
    local BT; BT=$(grep '^seeded ' "$sd/A.log" | sed 's/.* btih //')
    run BEXE "$sd" B "get magnet:?xt=urn:btih:$BT&dn=octomx-upload-bt-complete.bin --peer 127.0.0.1:$((b + 2)) --until-done --timeout 300 --every 5" "stats last"; local pb=$RUNPID
    wait $pb; sleep 2; stopall "$sd" $pa
    verdict $n BitTorrent complete "$sd/B.log" "$sd/B/incoming/octomx-upload-bt-complete.bin" "$f" 0
    if [ $WINE = 0 ] && command -v aria2c > /dev/null; then scen_bt_aria2 "$sd" "$f" "$BT"; fi
}
# BitTorrent partial: A downloads from S (slow); B downloads from A only (its only peer)
scen_bt_partial() {
    local n=bt-partial sd="$W/bt-partial"; mkdir -p "$sd/share"; local b; b=$(port_base)
    local f="$sd/share/octomx-upload-bt-partial.bin"; payload "$f" $SIZE
    settings "$sd/S" Guest400_00404 $((b + 20)) "bt" -- ", \"bandwidth/upLimitKBs\": $SLOW"
    settings "$sd/A" Guest100_00101 $b "bt"; settings "$sd/B" Guest200_00202 $((b + 10)) "bt"
    run AEXE "$sd" S "share $sd/share" "wait-hashed --count 1 --timeout 120" "seed $(wp "$f")" "touch $sd/S.ready" "wait-file $sd/stop --timeout 900"; local ps=$RUNPID
    waitfile "$sd/S.ready" 180
    local BT; BT=$(grep '^seeded ' "$sd/S.log" | sed 's/.* btih //')
    run AEXE "$sd" A "get magnet:?xt=urn:btih:$BT&dn=octomx-upload-bt-partial.bin --peer 127.0.0.1:$((b + 22)) --until-bytes $((SIZE / 3)) --timeout 300 --every 5" "touch $sd/A.partial" "watch last --timeout 400 --every 5" "wait-file $sd/stop --timeout 900" "uploads"; local pa=$RUNPID
    waitfile "$sd/A.partial" 300
    run BEXE "$sd" B "get magnet:?xt=urn:btih:$BT&dn=octomx-upload-bt-partial.bin --peer 127.0.0.1:$((b + 2)) --until-done --timeout 500 --every 5" "stats last"; local pb=$RUNPID
    wait $pb; sleep 2
    verdict $n BitTorrent partial "$sd/B.log" "$sd/B/incoming/octomx-upload-bt-partial.bin" "$f" 0 "B's only peer: A"
    stopall "$sd" $pa $ps
}
# aria2c (a foreign BitTorrent client) downloads from our seeder only
scen_bt_aria2() {
    local sd0=$1 f=$2 BT=$3 n=bt-aria2c sd="$W/bt-aria2c"; mkdir -p "$sd/aria2"; local b; b=$(port_base)
    settings "$sd/A" Guest100_00101 $b "bt"
    mkdir -p "$sd/share"; cp "$f" "$sd/share/"
    local TF; TF=$(ls "$sd0"/A/bt/torrents/*.torrent | head -1)
    run AEXE "$sd" A "share $sd/share" "wait-hashed --count 1 --timeout 120" "seed $sd/share/$(basename "$f")" "touch $sd/A.ready" "wait-file $sd/aria.listening --timeout 60" "peer $BT 127.0.0.1:$((b + 40))" "wait-file $sd/stop --timeout 600" "uploads"; local pa=$RUNPID
    waitfile "$sd/A.ready" 180
    local start; start=$(date +%s)
    timeout 300 aria2c --dir="$sd/aria2" --enable-dht=false --enable-dht6=false --bt-enable-lpd=false --enable-peer-exchange=false \
        --listen-port=$((b + 40)) --seed-time=0 --file-allocation=none --summary-interval=0 --console-log-level=warn \
        "$TF" > "$sd/aria2.log" 2>&1 & local pr=$!
    sleep 1.5; touch "$sd/aria.listening"
    wait $pr; local rc=$? secs=$(( $(date +%s) - start ))
    stopall "$sd" $pa
    local pf=FAIL; cmp -s "$sd/aria2/$(basename "$f")" "$f" && [ $rc = 0 ] && pf=PASS
    [ $pf = PASS ] || FAILS=$((FAILS + 1))
    echo "$pf  $n (BitTorrent, complete, foreign client aria2c): exit $rc after ${secs}s"
    printf '%-22s %-10s %-8s %s\n' "$n" BitTorrent complete "$pf|aria2c from our seeder, sha256 $([ $pf = PASS ] && echo equal || echo DIFFERS)|${secs}s" >> "$TABLE"
}
# aria2c (a foreign client) downloads a torrent A is still downloading from S, from A only
scen_bt_aria2_partial() {
    local n=bt-aria2c-partial sd="$W/bt-aria2c-partial"; mkdir -p "$sd/share" "$sd/aria2"; local b; b=$(port_base)
    local f="$sd/share/octomx-upload-bt-aria2-partial.bin"; payload "$f" $SIZE
    settings "$sd/S" Guest400_00404 $((b + 20)) "bt" -- ", \"bandwidth/upLimitKBs\": $SLOW"
    settings "$sd/A" Guest100_00101 $b "bt"
    run AEXE "$sd" S "share $sd/share" "wait-hashed --count 1 --timeout 120" "seed $(wp "$f")" "touch $sd/S.ready" "wait-file $sd/stop --timeout 900"; local ps=$RUNPID
    waitfile "$sd/S.ready" 180
    local BT; BT=$(grep '^seeded ' "$sd/S.log" | sed 's/.* btih //')
    run AEXE "$sd" A "get magnet:?xt=urn:btih:$BT&dn=octomx-upload-bt-aria2-partial.bin --peer 127.0.0.1:$((b + 22)) --until-bytes $((SIZE / 3)) --timeout 300 --every 5" "touch $sd/A.partial" "wait-file $sd/aria.listening --timeout 30" "peer last 127.0.0.1:$((b + 40))" "watch last --timeout 400 --every 5" "wait-file $sd/stop --timeout 900" "uploads"; local pa=$RUNPID
    waitfile "$sd/A.partial" 300
    local start; start=$(date +%s)
    timeout 400 aria2c --dir="$sd/aria2" --enable-dht=false --enable-dht6=false --bt-enable-lpd=false --enable-peer-exchange=false \
        --listen-port=$((b + 40)) --seed-time=0 --file-allocation=none --summary-interval=0 --console-log-level=warn \
        "$sd/S/bt/torrents/$BT.torrent" > "$sd/aria2.log" 2>&1 & local pr=$!
    sleep 1.5; touch "$sd/aria.listening"
    wait $pr; local rc=$? secs=$(( $(date +%s) - start ))
    stopall "$sd" $pa $ps
    local pf=FAIL; cmp -s "$sd/aria2/$(basename "$f")" "$f" && [ $rc = 0 ] && pf=PASS
    [ $pf = PASS ] || FAILS=$((FAILS + 1))
    echo "$pf  $n (BitTorrent, partial, foreign client aria2c, its only peer A): exit $rc after ${secs}s"
    printf '%-22s %-10s %-8s %s\n' "$n" BitTorrent partial "$pf|aria2c from A only (A still downloading), sha256 $([ $pf = PASS ] && echo equal || echo DIFFERS)|${secs}s" >> "$TABLE"
}
# eD2k: complete (A shares) and partial (A downloads from C over the overlay with the trusted set, B
# fetches from A over eD2k only)
scen_ed2k() {
    local kind=$1 n=ed2k-$1 sd="$W/ed2k-$1"; mkdir -p "$sd/share"; local b; b=$(port_base)
    local f="$sd/share/octomx-upload-ed2k-$kind.bin"; payload "$f" $SIZE; read -r E S1 B32 < <(hashes_of "$f")
    local L="ed2k://|file|octomx-upload-ed2k-$kind.bin|$SIZE|$E|/"
    settings "$sd/B" Guest200_00202 $((b + 10)) "ed2k"
    local pc="" pa
    if [ "$kind" = complete ]; then
        settings "$sd/A" Guest100_00101 $b "ed2k" -- ", \"bandwidth/upLimitKBs\": $SLOW"
        run AEXE "$sd" A "share $sd/share" "wait-hashed --count 1 --timeout 120" "touch $sd/A.partial" "wait-file $sd/stop --timeout 900" "uploads"; pa=$RUNPID
    else
        settings "$sd/C" Guest300_00303 $((b + 20)) "ovl" $b -- ", \"bandwidth/upLimitKBs\": $SLOW"
        settings "$sd/A" Guest100_00101 $b "ovl ed2k" $((b + 20)) -- ", \"bandwidth/upLimitKBs\": $((SLOW * 2))"
        run AEXE "$sd" C "share $sd/share" "wait-hashed --count 1 --timeout 120" "touch $sd/C.ready" "wait-file $sd/stop --timeout 900"; pc=$RUNPID
        waitfile "$sd/C.ready" 120
        run AEXE "$sd" A "wait-peers OctoMX 1 --timeout 60" "get $(magnet octomx-upload-ed2k-$kind.bin $E $S1 $SIZE) --until-bytes $((SIZE / 3)) --timeout 300 --every 5" "touch $sd/A.partial" "watch last --timeout 400 --every 5" "wait-file $sd/stop --timeout 900" "uploads"; pa=$RUNPID
    fi
    waitfile "$sd/A.partial" 300
    # eD2k learns a partial source's new parts only when it asks again (FILEREASKTIME, 29 min, as
    # eMule): the partial case checks what A had verified, not the whole file
    local tmo=500; [ "$kind" = partial ] && tmo=90
    run BEXE "$sd" B "get $L --source ed2k-peer://127.0.0.1:$((b + 3)) --until-done --timeout $tmo --every 5" "stats last"; local pb=$RUNPID
    wait $pb; sleep 2; stopall "$sd" $pa $pc
    local rc; rc=$(rowcheck "$sd/A.up" ed2k "$sd/B.log")
    if [ "$kind" = partial ]; then partial_verdict $n eD2k "$sd/B.log" "$f" "$rc"; return; fi
    verdict $n eD2k $kind "$sd/B.log" "$sd/B/incoming/octomx-upload-ed2k-$kind.bin" "$f" "${rc%% *}" "upload row vs receiver: ${rc#* }; A's last eD2k row: $(grep 'net=ed2k' "$sd/A.up" | tail -1 | grep -o 'status=.*')"
}
# Gnutella: complete and partial as eD2k; plus curl (a foreign HTTP client) against A's servent
scen_gnut() {
    local kind=$1 n=gnut-$1 sd="$W/gnut-$1"; mkdir -p "$sd/share"; local b; b=$(port_base)
    local name=octomx-upload-gnut-$kind.bin f="$sd/share/octomx-upload-gnut-$kind.bin"; payload "$f" $SIZE; read -r E S1 B32 < <(hashes_of "$f")
    settings "$sd/B" Guest200_00202 $((b + 10)) "gnut"
    local pc="" pa
    if [ "$kind" = complete ]; then
        settings "$sd/A" Guest100_00101 $b "gnut" -- ", \"bandwidth/upLimitKBs\": $SLOW"
        run AEXE "$sd" A "share $sd/share" "wait-hashed --count 1 --timeout 120" "touch $sd/A.partial" "wait-file $sd/stop --timeout 900" "uploads"; pa=$RUNPID
    else
        settings "$sd/C" Guest300_00303 $((b + 20)) "ovl" $b -- ", \"bandwidth/upLimitKBs\": $SLOW"
        settings "$sd/A" Guest100_00101 $b "ovl gnut" $((b + 20)) -- ", \"bandwidth/upLimitKBs\": $((SLOW * 2))"
        run AEXE "$sd" C "share $sd/share" "wait-hashed --count 1 --timeout 120" "touch $sd/C.ready" "wait-file $sd/stop --timeout 900"; pc=$RUNPID
        waitfile "$sd/C.ready" 120
        run AEXE "$sd" A "wait-peers OctoMX 1 --timeout 60" "get $(magnet $name $E $S1 $SIZE) --until-bytes $((SIZE / 3)) --timeout 300 --every 5" "touch $sd/A.partial" "watch last --timeout 400 --every 5" "wait-file $sd/stop --timeout 900" "uploads"; pa=$RUNPID
    fi
    waitfile "$sd/A.partial" 300
    local curlres="-"
    if [ $WINE = 0 ] && command -v curl > /dev/null; then   # the servent's HTTP upload with a foreign client
        local url="http://127.0.0.1:$((b + 5))/uri-res/N2R?urn:sha1:$B32"
        curl -s -o "$sd/curl.head" -D "$sd/curl.hdr" -r 0-1048575 "$url"
        local code; code=$(head -1 "$sd/curl.hdr" | awk '{print $2}')
        local good=no; cmp -s "$sd/curl.head" <(head -c 1048576 "$f") && good=yes
        curlres="curl 0-1 MiB: HTTP $code, bytes right: $good"
        if [ "$kind" = partial ]; then
            curl -s -o /dev/null -D "$sd/curl.tail.hdr" -r $((SIZE - 1048576))-$((SIZE - 1)) "$url"
            curlres="$curlres; last MiB: $(head -1 "$sd/curl.tail.hdr" | tr -d '\r') $(grep -i 'X-Available-Ranges' "$sd/curl.tail.hdr" | tr -d '\r')"
        fi
    fi
    local tmo=500; [ "$kind" = partial ] && tmo=90
    run BEXE "$sd" B "get $(magnet $name $E $S1 $SIZE) --source gnut://127.0.0.1:$((b + 5)) --until-done --timeout $tmo --every 5" "stats last"; local pb=$RUNPID
    wait $pb; sleep 2; stopall "$sd" $pa $pc
    local rc; rc=$(rowcheck "$sd/A.up" Gnut "$sd/B.log")
    if [ "$kind" = partial ]; then partial_verdict $n Gnutella "$sd/B.log" "$f" "$rc ($curlres)"; return; fi
    verdict $n Gnutella $kind "$sd/B.log" "$sd/B/incoming/$name" "$f" "${rc%% *}" "upload rows vs receiver: ${rc#* }; $curlres"
}

want ovl-complete && scen_ovl_complete
want ovl-partial && scen_ovl_partial
want ovl-noset && scen_ovl_noset
want ovl-torrent && scen_ovl_torrent
want ovl-different && scen_ovl_different
want bt-complete && scen_bt_complete
want bt-partial && scen_bt_partial
if [ $WINE = 0 ] && command -v aria2c > /dev/null; then want bt-aria2c-partial && scen_bt_aria2_partial; fi
want ed2k-complete && scen_ed2k complete
want ed2k-partial && scen_ed2k partial
want gnut-complete && scen_gnut complete
want gnut-partial && scen_gnut partial

echo "== table (scenario, network, kind, result|sha256|speed|wasted bytes|bans|unit failures|notes)"
cat "$TABLE"
for p in $(pgrep -f "mxcli(.exe)? --script $W/"); do kill -9 "$p" 2>/dev/null; done
echo "== $FAILS scenario(s) failed; evidence in $W"
exit $(( FAILS > 0 ))
