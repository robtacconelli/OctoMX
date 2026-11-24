#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# tools/mac/smoke.sh: live smoke test on a Mac (real core, real networks, this machine's internet).
#
#   tools/mac/smoke.sh <OctoMX.app> <test-build-dir> <out-dir>
#
# 1. the packaged program: real core with a throwaway profile (own ports, UPnP / NAT-PMP off, no
#    update check, MX_OVERLAY_DISCOVERY=0), UI script: wait for the networks, screenshot, search
#    "big buck bunny", at least one result row, screenshot, quit; the log must show networks online,
#    HTTPS answers of the web index (Qt TLS on macOS) and the SQLite database, and the program must
#    end by itself (clean quit).
# 2. mxcli of the test build (the same core, headless): search "big buck bunny", download one small
#    row that several networks merged (an Internet Archive torrent with web seeds when there is one),
#    for at most a few minutes; the engine checks every piece / part and the whole-file hash.
# 3. the HTTP / Internet Archive path: the 62 MB Big Buck Bunny mp4 through OctoMX's HTTP network,
#    then its SHA-1 against the archive's own metadata.
# Writes <out-dir>/smoke-summary.txt; exit 1 when 1 or 3 fails (2 depends on peers being online and
# reachable from the runner and is reported, not required).
set -uo pipefail
source "$(cd "$(dirname "$0")" && pwd)/env.sh"
[ $# -eq 3 ] || { echo "usage: $0 <OctoMX.app> <test-build-dir> <out-dir>" >&2; exit 2; }
APP=$(cd "$1" && pwd); TB=$(cd "$2" && pwd); mkdir -p "$3"; OUT=$(cd "$3" && pwd)
EXE=$APP/Contents/MacOS/octomx
CLI=$TB/bin/mxcli
SUM=$OUT/smoke-summary.txt
: > "$SUM"
run_limited() { perl -e 'alarm shift; exec @ARGV or die "exec: $!"' "$@"; }
note() { echo "$*" | tee -a "$SUM"; }
export MX_OVERLAY_DISCOVERY=0 MX_SOUND_OFF=1 LC_ALL=en_US.UTF-8
RC=0

profile() { # <dir> <port base>: own ports, no port mapping, no update check, incoming in the profile
    mkdir -p "$1"
    cat > "$1/settings.json" <<EOF
{ "connection/tcpPort": $2, "connection/udpPort": $(($2 + 1)), "bt/listenPort": $(($2 + 2)),
  "ed2k/tcpPort": $(($2 + 3)), "ed2k/udpPort": $(($2 + 4)), "kad/udpPort": $(($2 + 4)), "gnutella/port": $(($2 + 5)),
  "rats/listenPort": $(($2 + 6)), "connection/upnp": false, "connection/natpmp": false,
  "updates/mode": "off", "updates/checkAtStart": false,
  "transfers/incomingDir": "$1/incoming", "misc/logLevel": "debug" }
EOF
}

# ---- 1. the packaged program, real core, UI script
P1=$OUT/app-profile
rm -rf "$P1"; profile "$P1" 47301
cat > "$OUT/app.script" <<EOF
wait 45000
shot $OUT/real-online.png
page search
type searchText big buck bunny
key searchText Return
wait 25000
expect-rows searchResults 1
shot $OUT/real-search.png
quit
EOF
T0=$(date +%s)
MX_OS_ROOT=$OUT/app-os-root run_limited 240 "$EXE" --profile "$P1" --no-wizard --size 1024x714 --script "$OUT/app.script" > "$OUT/app.out" 2>&1
APP_RC=$?
T1=$(date +%s)
LOG=$P1/logs/octomx.log
note "1. packaged program: exit $APP_RC after $((T1 - T0)) s (2 = a script line failed, 142 = killed by the time limit)"
if [ -f "$LOG" ]; then
    head -c 16 "$P1/octomx.db" | grep -q 'SQLite format 3' && note "   SQLite: octomx.db created" || { note "   no SQLite database"; RC=1; }
    for pat in 'mx.bt: session online' 'Gnutella: connected to ultrapeer' 'eD2k: connected to' 'Kad: connected' 'OctoMX: .*listening on port' 'Rats: .*listening on port'; do
        l=$(grep -m1 -E "$pat" "$LOG" | cut -c12-160)
        note "   network: ${l:-no '$pat' line within the run}"
    done
    N=$(grep -cE 'Gnutella: connected to ultrapeer|eD2k: connected to|Kad: connected|mx.bt: session online' "$LOG")
    note "   internet networks online: $N connect events"; [ "$N" -ge 2 ] || RC=1
    WEB=$(grep -cE 'mx.web: search [0-9]+ .* ok .* [1-9][0-9]* hits' "$LOG")
    note "   HTTPS web search: $WEB index sites answered with hits $(grep -m1 -E 'mx.web: search [0-9]+ .* ok .* [1-9][0-9]* hits' "$LOG" | cut -c25-120)"
    [ "$WEB" -gt 0 ] || RC=1
    note "   TLS warnings: $(grep -iE 'ssl|tls' "$LOG" | grep -ciE 'warn|fail|error')  $(grep -iE 'ssl|tls' "$LOG" | grep -iE 'warn|fail|error' | head -n 2 | cut -c1-120 | tr '\n' ';')"
    note "   firewall / local network: $(grep -m2 -E 'macOS Firewall|default gateway|gateway' "$LOG" | cut -c12-140 | tr '\n' ';')"
    note "   last lines: $(tail -n 2 "$LOG" | cut -c1-120 | tr '\n' ';')"
else
    note "   no log at $LOG"
fi
[ "$APP_RC" = 0 ] || RC=1
[ -s "$OUT/real-search.png" ] || { note "   no real-search.png"; RC=1; }

# ---- 2. a small Big Buck Bunny file from a merged row (mxcli, the same core)
if [ -x "$CLI" ]; then
    P2=$OUT/cli-profile
    rm -rf "$P2"; profile "$P2" 47401
    run_limited 200 "$CLI" --profile "$P2" search big buck bunny --wait 60 --max 80 --save "$P2/q.json" > "$OUT/cli-search.txt" 2>&1
    note "2. mxcli search: $(grep -c -E '^ *[0-9]+  ' "$OUT/cli-search.txt") rows shown; $(grep -E 'merged hits per network' "$OUT/cli-search.txt" | head -n 1 | cut -c1-160)"
    # a merged row (several networks) of 1-80 MB, plain file
    # a row that several networks merged, 1-80 MB: the Internet Archive torrents with their web
    # seeds (HTTP + BitTorrent) first, then other merged rows, then the best seeded plain row
    PICK=$(python3 -I - "$OUT/cli-search.txt" <<'PY'
import re, sys
best = None
for line in open(sys.argv[1], encoding='utf-8', errors='replace'):
    m = re.match(r'^\s*(\d+)\s+([\d.]+)\s*(B|KB|KiB|MB|MiB|GB|GiB)\s+(\d+)\s+(\S+)\s+(.*)$', line)
    if not m:
        continue
    n, num, unit, srcs, nets, rest = m.groups()
    mb = float(num) * {'B': 1e-6, 'KB': 1e-3, 'KiB': 1e-3, 'MB': 1, 'MiB': 1, 'GB': 1e3, 'GiB': 1e3}[unit]
    if not (1 <= mb <= 80):
        continue
    kinds = set(k.rstrip('?') for k in nets.split(',') if not k.endswith('?') and '(in)' not in k)
    merged = len(kinds)
    seeds = int((re.search(r'\((\d+) seeds\)', rest) or [0, 0])[1])
    key = (merged >= 2 and 'HTTP' in kinds, merged, '[bundle]' not in rest, int(srcs) + seeds)
    if best is None or key > best[0]:
        best = (key, n, line.strip())
print(best[1] + '\t' + best[2] if best else '')
PY
)
    if [ -n "$PICK" ]; then
        N=${PICK%%	*}
        note "   picked row $N: ${PICK#*	}"
        run_limited 330 "$CLI" --profile "$P2" get "$N" --from "$P2/q.json" --until-done --timeout 300 --every 15 > "$OUT/cli-get.txt" 2>&1
        note "   $(grep -E '^result:' "$OUT/cli-get.txt" | tail -n 1 | cut -c1-200)"
        note "   progress: $(grep -E 'verified|B/s|of ' "$OUT/cli-get.txt" | tail -n 2 | cut -c1-160 | tr '\n' ';')"
    else
        note "   no merged row of 1-80 MB in the results (peers not reachable from this runner?)"
    fi

    # ---- 3. the HTTP / Internet Archive path, SHA-1 against the archive's metadata
    P3=$OUT/http-profile
    rm -rf "$P3"; profile "$P3" 47501
    URL=https://archive.org/download/BigBuckBunny_124/Content/big_buck_bunny_720p_surround.mp4
    run_limited 400 "$CLI" --profile "$P3" get "$URL" --until-done --timeout 360 --every 20 > "$OUT/http-get.txt" 2>&1
    note "3. HTTP (Internet Archive): $(grep -E '^result:' "$OUT/http-get.txt" | tail -n 1 | cut -c1-200)"
    F=$(find "$P3/incoming" -type f -name 'big_buck_bunny_720p_surround*.mp4' 2>/dev/null | head -n 1)
    WANT=$(curl -fsS --max-time 60 https://archive.org/metadata/BigBuckBunny_124/files | python3 -I -c '
import json, sys
for f in json.load(sys.stdin).get("result", []):
    if f.get("name") == "Content/big_buck_bunny_720p_surround.mp4":
        print(f.get("sha1", ""), f.get("size", ""))')
    if [ -n "$F" ]; then
        GOT=$(shasum -a 1 "$F" | cut -d' ' -f1)
        note "   file $(basename "$F") $(stat -f %z "$F") bytes, sha1 $GOT; archive.org says ${WANT:-(metadata not reachable)}"
        [ "$GOT" = "${WANT%% *}" ] && note "   SHA-1 MATCHES" || { note "   SHA-1 DIFFERS"; RC=1; }
    else
        note "   no completed file"; RC=1
    fi
else
    note "2./3. skipped: no $CLI (test build tree needed)"
    RC=1
fi
note "smoke: $([ $RC = 0 ] && echo OK || echo FAILED)"
exit $RC
