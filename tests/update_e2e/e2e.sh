#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# [upd-1] End-to-end test of the signed self-update with two real release packages.
#
#   tests/update_e2e/e2e.sh --a <dist of package A> --b <dist of package B> --work <dir> --shots <dir>
#                           [--linux] [--wine] [--xvnc N] [--throwaway-key] [--only <scenario>]
#
# A = this version, B = a higher version (tools/release.sh --out ... --version 5.2.0), both signed
# with the release key (SHA256SUMS.sig in B's dist). [ren-1] Without the release key: --throwaway-key
# makes a throwaway Ed25519 key in <work>, signs a copy of B's SHA256SUMS with it and hands its public
# key to A in MX_UPDATE_TEST_PUBKEY, which only packages built with test hooks honour (tools/release.sh
# --test-hooks, never published). The whole test runs in its own network
# namespace (unshare -rn: only the loopback device exists), so nothing can reach GitHub or any other
# real server; a local fake API + asset server (fake_release_server.py) plays GitHub. Every run uses
# a package unpacked into <work>, a throwaway --profile, MX_OS_ROOT inside <work>, all networks off.
# Scenarios (Linux natively, the Windows package under wine with a private copy of the wine prefix):
#   restart   auto mode: quiet download, Ready box, Restart Now; the program comes back as B, shows
#             "Updated to", the old files are gone, portable.txt / profile / downloads / a foreign file
#             and (Linux) the desktop entry are intact
#   later     auto mode: Ready box, Later, quit: B is installed at close; B's first start cleans up
#   tampered  one byte of the archive flipped: refused after the download, the download page box
#   unsigned  no SHA256SUMS.sig: never downloaded, the download page box with the reason
#   download  [safe-1] an HTTP download (local, throttled) runs while the update downloads and Restart
#             Now installs it: A closes safely (every download saved), B waits for A to be gone,
#             continues the download where A left it and finishes it with the right SHA-256
# Logs, the server request logs and screenshots (100 % and 200 %) go to <work> and <shots>.
# Prints OK / FAIL lines and "E2E OK" at the end; exit 1 on any FAIL.
set -uo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
HERE=$ROOT/tests/update_e2e
A=""; B=""; WORK=""; SHOTS=""; DO_LINUX=0; DO_WINE=0; XN=""; THROWAWAY=0; ONLY=""
ARGS=("$@")
while [ $# -gt 0 ]; do
  case "$1" in
    --a) A=$(realpath "$2"); shift ;;
    --b) B=$(realpath "$2"); shift ;;
    --work) WORK=$(realpath -m "$2"); shift ;;
    --shots) SHOTS=$(realpath -m "$2"); shift ;;
    --linux) DO_LINUX=1 ;;
    --wine) DO_WINE=1 ;;
    --xvnc) XN=$2; shift ;;
    --throwaway-key) THROWAWAY=1 ;;
    --only) ONLY=$2; shift ;;   # [safe-1] one scenario (restart, later, tampered, unsigned, download)
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
  shift
done
[ -n "$A" ] && [ -n "$B" ] && [ -n "$WORK" ] && [ -n "$SHOTS" ] || { sed -n 2,20p "$0"; exit 2; }
# ---- our own network namespace: loopback only (a user namespace to bring lo up, then a nested one
# that maps us back to our own uid: wine refuses to run as the namespace's root)
if [ -z "${MX_E2E_NETNS:-}" ]; then
  exec unshare -rn bash -c 'ip link set lo up && exec unshare --map-user='"$(id -u)"' --map-group='"$(id -g)"' env MX_E2E_NETNS=1 bash "$@"' \
       e2e "$0" "${ARGS[@]}"
fi
source "$ROOT/toolchain/env.sh" >/dev/null
mkdir -p "$WORK" "$SHOTS"
FAILS=0
ok()  { echo "OK   $*"; }
bad() { echo "FAIL $*"; FAILS=$((FAILS+1)); }
info() { echo "INFO $*"; }
python3 -I -c 'import socket; s=socket.socket(); s.settimeout(2); import sys; sys.exit(0 if s.connect_ex(("140.82.112.6",443)) else 1)' \
  && ok "network namespace: api.github.com's address is unreachable, only loopback exists" || bad "the outside network is reachable"

LIN_A=$(cd "$A" && ls OctoMX-*-linux-x86_64.tar.xz 2>/dev/null | head -n1)
WIN_A=$(cd "$A" && ls OctoMX-*-win64.zip 2>/dev/null | head -n1)
VA=$(sed -n 's/^OctoMX-\([0-9.]*\)-.*/\1/p' <<<"${LIN_A:-$WIN_A}")
LIN_B=$(cd "$B" && ls OctoMX-*-linux-x86_64.tar.xz 2>/dev/null | head -n1)
VB=$(sed -n 's/^OctoMX-\([0-9.]*\)-.*/\1/p' <<<"${LIN_B:-$(cd "$B" && ls OctoMX-*-win64.zip | head -n1)}")
VBFULL=$VB.0
info "package A $VA, package B $VB (tag v$VBFULL)"
TESTKEY=""
if [ $THROWAWAY = 1 ]; then
  mkdir -p "$WORK/throwaway" && chmod 700 "$WORK/throwaway"
  rm -rf "$WORK/distB"; mkdir -p "$WORK/distB"
  cp "$B"/OctoMX-* "$B/SHA256SUMS" "$WORK/distB/"
  openssl genpkey -algorithm ed25519 -out "$WORK/throwaway/key.pem" 2>/dev/null && chmod 600 "$WORK/throwaway/key.pem"
  "$ROOT/tools/release/sign.sh" "$WORK/distB/SHA256SUMS" "$WORK/throwaway/key.pem" > "$WORK/throwaway/sign.log" 2>&1 \
    && ok "throwaway key: B's SHA256SUMS signed ($(sed -n 2p "$WORK/distB/SHA256SUMS.sig"))" || bad "throwaway signing: $(cat "$WORK/throwaway/sign.log")"
  TESTKEY=$(openssl pkey -in "$WORK/throwaway/key.pem" -pubout -outform DER 2>/dev/null | tail -c 32 | xxd -p -c 32)
  B=$WORK/distB
fi
[ -f "$B/SHA256SUMS.sig" ] && ok "B is signed: $(sed -n 2p "$B/SHA256SUMS.sig")" || bad "B has no SHA256SUMS.sig"
PORT=47391
SRV_PID=""
PAYLOAD=""
server() { # <mode> <log>
  stop_server
  python3 -I "$HERE/fake_release_server.py" --port $PORT --dist "$B" --tag "v$VBFULL" --mode "$1" --log "$2" \
    ${PAYLOAD:+--payload "$PAYLOAD" --rate $((1536 * 1024))} > "$2.out" 2>&1 &
  SRV_PID=$!
  for _ in $(seq 1 50); do grep -q 'fake release server' "$2.out" 2>/dev/null && return 0; sleep 0.1; done
  bad "fake server did not start: $(cat "$2.out")"
}
stop_server() { [ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null && wait "$SRV_PID" 2>/dev/null; SRV_PID=""; }
trap 'stop_server; [ -n "${XVNC_PID:-}" ] && kill "$XVNC_PID" 2>/dev/null' EXIT

settings() { # <profile dir> <scale> <url> [more JSON entries]
  mkdir -p "$1"
  cat > "$1/settings.json" <<EOF
{ "bt/enabled": false, "ed2k/enabled": false, "kad/enabled": false, "gnutella/enabled": false,
  "http/enabled": false, "overlay/enabled": false, "web/enabled": false, "rats/enabled": false,
  "connection/upnp": false, "connection/natpmp": false, "connection/tcpPort": 47381, "connection/udpPort": 47382,
  "bt/listenPort": 47383, "ed2k/tcpPort": 47384, "ed2k/udpPort": 47385, "kad/udpPort": 47385,
  "gnutella/port": 47386, "rats/listenPort": 47387, "misc/logLevel": "debug", "ui/scale": "$2",
  "updates/url": "$3", "internal/wizardDone": true, "internal/termsAccepted": 1${4:+, $4} }
EOF
}
user_files() { # <package dir>
  echo "portable" > "$1/portable.txt"
  mkdir -p "$1/profile" "$1/downloads"
  echo "keep me" > "$1/profile/marker.txt"
  echo "a finished download" > "$1/downloads/keep.ogg"
  echo "the user's own notes" > "$1/my notes.txt"
}
user_files_intact() { # <package dir> <tag>
  local p=$1 t=$2 bad_=""
  [ "$(cat "$p/portable.txt" 2>/dev/null)" = portable ] || bad_+=" portable.txt"
  [ "$(cat "$p/profile/marker.txt" 2>/dev/null)" = "keep me" ] || bad_+=" profile/marker.txt"
  [ "$(cat "$p/downloads/keep.ogg" 2>/dev/null)" = "a finished download" ] || bad_+=" downloads/keep.ogg"
  [ "$(cat "$p/my notes.txt" 2>/dev/null)" = "the user's own notes" ] || bad_+=" 'my notes.txt'"
  [ -z "$bad_" ] && ok "$t: portable.txt, profile/, downloads/ and a foreign file are untouched" || bad "$t: changed or missing:$bad_"
}
tree_is() { # <package dir> <version x.y.z> <tag>: every file of its MANIFEST.txt, that version, no update leftovers
  local p=$1 v=$2 t=$3
  local mv; mv=$(sed -n 's/^version //p' "$p/MANIFEST.txt" | tr -d '\r')
  [ "$mv" = "$v" ] || { bad "$t: MANIFEST.txt says '$mv', expected $v"; return; }
  local n bad_
  n=$(grep -c '^[0-9a-f]\{64\} ' "$p/MANIFEST.txt")
  bad_=$(cd "$p" && tr -d '\r' < MANIFEST.txt | sed -n 's/^\([0-9a-f]\{64\}\) [0-9]* \(.*\)$/\1  \2/p' | sha256sum -c --quiet 2>&1 | head -n3)
  [ -z "$bad_" ] && ok "$t: the program folder is exactly $v: all $n files of its MANIFEST.txt match" || bad "$t: $bad_"
  local left; left=$(cd "$p" && ls -A | grep -E '^\.(old|new|update)-' | tr '\n' ' ')
  [ -z "$left" ] && ok "$t: no .old-*, .new-* or .update-* left" || info "$t: left in the folder: $left"
}
wait_gone() { # <pattern> <seconds>
  for _ in $(seq 1 $(( $2 * 2 ))); do pgrep -f -- "$1" > /dev/null || return 0; sleep 0.5; done
  return 1
}

# ================================================================================ Linux
linux_scenario() { # <name> <server mode> <scale>
  local name=$1 mode=$2 scale=$3
  local D=$WORK/linux-$name
  rm -rf "$D"; mkdir -p "$D/cwd" "$D/home" "$D/osroot" "$D/xdg"
  tar -C "$D" -xf "$A/$LIN_A" || { bad "linux $name: extract A"; return; }
  local P=$D/OctoMX-$VA-linux-x86_64 PR=$D/prof-$name
  user_files "$P"
  HOME=$D/home XDG_DATA_HOME=$D/xdg "$P/install-desktop-entry.sh" > "$D/desktop.out" 2>&1
  settings "$PR" "$scale" "http://127.0.0.1:$PORT/repos/robtacconelli/OctoMX/releases/latest"
  local S=$SHOTS/linux-$name-$scale
  case "$name" in
    restart) printf '%s\n' "update-expect-version $VA" "update-wait state:ready 180000" "wait 400" "shot $S-ready.png" \
                           "click updateRestart" "wait 60000" > "$D/a.script" ;;
    later)   printf '%s\n' "update-wait state:ready 180000" "wait 400" "shot $S-ready.png" "click updateInstallLater" \
                           "wait 1500" "quit" > "$D/a.script" ;;
    *)       printf '%s\n' "update-wait updateBox 180000" "wait 400" "shot $S-refused.png" "click updateLater" "wait 500" "quit" > "$D/a.script" ;;
  esac
  printf '%s\n' "update-expect-version $VB" "update-wait updatedBox 30000" "wait 400" "shot $S-updated.png" "click updatedOk" \
                "wait 500" "quit" > "$D/b.script"
  server "$mode" "$D/server.log"
  cat > "$D/offscreen.json" <<'JSON'
{ "synchronousWindowSystemEvents": false, "windowFrameMargins": false,
  "screens": [ { "name": "shot", "x": 0, "y": 0, "width": 2560, "height": 1600,
                 "logicalDpi": 96, "logicalBaseDpi": 96, "dpr": 1 } ] }
JSON
  local ENVV=(env -i HOME="$D/home" PATH=/usr/bin:/bin LANG=C.UTF-8 ${TESTKEY:+MX_UPDATE_TEST_PUBKEY=$TESTKEY} QT_QPA_PLATFORM="offscreen:configfile=$D/offscreen.json" MX_OS_ROOT="$D/osroot"
              XDG_RUNTIME_DIR="$D/home" XDG_DATA_HOME="$D/xdg" XDG_CONFIG_HOME="$D/home/.config" XDG_CACHE_HOME="$D/home/.cache")
  (cd "$D/cwd" && timeout 400 "${ENVV[@]}" MX_UPDATE_RESTART_SCRIPT="$D/b.script" "$P/octomx" --profile "$PR" --size $((1024 * scale / 100))x$((714 * scale / 100)) \
     --script "$D/a.script" > "$D/a.out" 2>&1)
  local rc=$?
  [ $rc = 0 ] && ok "linux $name: A ran and ended (rc 0)" || bad "linux $name: A rc=$rc: $(tail -n 3 "$D/a.out" | tr '\n' ' ')"
  case "$name" in
    restart)
      wait_gone "bin/octomx --profile $PR" 120 && ok "linux restart: the restarted program ran its script and ended" \
                                               || { bad "linux restart: the restarted program is still running"; pkill -f -- "--profile $PR"; }
      grep -q 'update-expect-version\|this is version' "$D/a.out" && info "$(grep -m1 'this is version' "$D/a.out")"
      grep -hq "update: started .*bin/octomx" "$PR/logs/octomx.log" "$D/a.out" && ok "linux restart: A started the new program: $(grep -hm1 'update: started' "$PR/logs/octomx.log" "$D/a.out" | sed 's/.*octomx: //' | cut -c1-200)" \
                                                                 || bad "linux restart: no restart line in the log"
      grep -q "waited .* for the previous OctoMX" "$PR/logs/octomx.log" && ok "linux restart: B $(grep -m1 'waited .* for the previous' "$PR/logs/octomx.log" | sed 's/.*update: //')"
      ;;
    later)
      tree_is "$P" "$VBFULL" "linux later (after close, before the next start)"
      [ -d "$P/.old-$VA.0" ] && ok "linux later: the old files wait in .old-$VA.0 until the next start" || info "linux later: no .old-$VA.0"
      pgrep -f -- "--profile $PR" > /dev/null && bad "linux later: something was restarted"
      (cd "$D/cwd" && timeout 120 "${ENVV[@]}" "$P/octomx" --profile "$PR" --size $((1024 * scale / 100))x$((714 * scale / 100)) --script "$D/b.script" > "$D/b.out" 2>&1)
      rc=$?
      [ $rc = 0 ] && ok "linux later: the next start is $VB and shows the Updated box (rc 0)" || bad "linux later: B rc=$rc: $(tail -n 3 "$D/b.out" | tr '\n' ' ')"
      ;;
  esac
  case "$name" in
    restart|later)
      tree_is "$P" "$VBFULL" "linux $name"
      user_files_intact "$P" "linux $name"
      local ver; ver=$(env -i PATH=/usr/bin:/bin LANG=C.UTF-8 QT_QPA_PLATFORM=offscreen "$P/bin/octomx" --version 2>/dev/null | head -n1)
      grep -q "$VB" <<<"$ver" && ok "linux $name: bin/octomx --version: $ver" || bad "linux $name: --version says '$ver'"
      grep -m1 "replaced $VA.0" "$PR/logs/octomx.log" > /dev/null && ok "linux $name: log: $(grep -m1 "replaced $VA.0" "$PR/logs/octomx.log" | cut -c25-220)" \
                                                                  || bad "linux $name: no 'replaced' line"
      local exe; exe=$(sed -n 's/^Exec=\([^ ]*\).*/\1/p' "$D/xdg/applications/octomx.desktop" | head -n1)
      [ "$exe" = "$P/octomx" ] && [ -x "$exe" ] && cmp -s "$exe" <(tar -xJOf "$B/$LIN_B" "OctoMX-$VB-linux-x86_64/octomx") \
        && ok "linux $name: the desktop entry still starts $exe, now B's launcher" || bad "linux $name: desktop entry Exec '$exe'"
      for f in "$S-ready.png" "$S-ready.updateReadyBox.png" "$S-updated.png" "$S-updated.updatedBox.png"; do
        [ -s "$f" ] && info "screenshot $f" || bad "linux $name: no $f"
      done
      ;;
    tampered|unsigned)
      tree_is "$P" "$VA.0" "linux $name (unchanged)"
      user_files_intact "$P" "linux $name"
      local why; why=$(grep -m1 'no automatic install' "$PR/logs/octomx.log" | sed 's/.*no automatic install of //')
      [ -n "$why" ] && ok "linux $name: refused: $why" || bad "linux $name: no refusal in the log"
      [ -s "$S-refused.updateBox.png" ] && info "screenshot $S-refused.updateBox.png" || bad "linux $name: no box screenshot"
      if [ "$name" = unsigned ]; then
        grep -q "/assets/OctoMX-" "$D/server.log" && bad "linux unsigned: the archive was downloaded" || ok "linux unsigned: the archive was never requested"
      fi
      ;;
  esac
  info "linux $name: server requests: $(grep -c '^GET' "$D/server.log"): $(grep '^GET' "$D/server.log" | awk '{print $2}' | tr '\n' ' ' | cut -c1-300)"
  stop_server
}

# ================================================================================ [safe-1] download
make_payload() {
  PAYLOAD=$WORK/payload/ubuntu-like-test.iso
  [ -s "$PAYLOAD" ] && return
  mkdir -p "$WORK/payload"
  head -c $((96 * 1024 * 1024)) /dev/urandom > "$PAYLOAD"
}
# <label> <log file> <incoming file> <pid of B or empty> <seconds>: B finishes the file; then B is closed safely
download_finished() {
  local t=$1 log=$2 file=$3 pid=$4 n
  local want; want=$(sha256sum "$PAYLOAD" | cut -c1-64)
  for n in $(seq 1 $(( $5 * 2 ))); do
    [ -f "$file" ] && [ "$(stat -c %s "$file")" = "$(stat -c %s "$PAYLOAD")" ] && break
    sleep 0.5
  done
  if [ -f "$file" ] && [ "$(sha256sum "$file" | cut -c1-64)" = "$want" ]; then
    ok "$t: B finished the download, SHA-256 right ($(stat -c %s "$file") bytes)"
  else
    bad "$t: the download is not finished or wrong: $(ls -la "$(dirname "$file")" 2>&1 | tail -n +2 | tr '\n' ' ' | cut -c1-300)"
  fi
  [ -z "$(ls -A "$(dirname "$file")" | grep -v "^$(basename "$file")\$")" ] && ok "$t: nothing else in the incoming folder" \
    || bad "$t: more in the incoming folder: $(ls -A "$(dirname "$file")" | tr '\n' ' ')"
  [ -n "$pid" ] && kill -TERM "$pid" 2>/dev/null   # B, by the pid A's log gives
}
linux_download() {
  local D=$WORK/linux-download
  rm -rf "$D"; mkdir -p "$D/cwd" "$D/home" "$D/osroot" "$D/xdg"
  tar -C "$D" -xf "$A/$LIN_A" || { bad "linux download: extract A"; return; }
  local P=$D/OctoMX-$VA-linux-x86_64 PR=$D/prof-download
  make_payload
  settings "$PR" 100 "http://127.0.0.1:$PORT/repos/robtacconelli/OctoMX/releases/latest" \
    "\"http/enabled\": true, \"transfers/incomingDir\": \"$PR/incoming\", \"transfers/incompleteDir\": \"$PR/incomplete\""
  printf '%s\n' "update-expect-version $VA" "update-wait state:ready 180000" "wait 3000" "click updateRestart" "wait 60000" > "$D/a.script"
  printf '%s\n' "update-expect-version $VB" "update-wait updatedBox 30000" "click updatedOk" "wait 400000" "quit" > "$D/b.script"
  server good "$D/server.log"
  local ENVV=(env -i HOME="$D/home" PATH=/usr/bin:/bin LANG=C.UTF-8 ${TESTKEY:+MX_UPDATE_TEST_PUBKEY=$TESTKEY} QT_QPA_PLATFORM=offscreen MX_OS_ROOT="$D/osroot"
              MX_OVERLAY_DISCOVERY=0 XDG_RUNTIME_DIR="$D/home" XDG_DATA_HOME="$D/xdg" XDG_CONFIG_HOME="$D/home/.config" XDG_CACHE_HOME="$D/home/.cache")
  (cd "$D/cwd" && timeout 400 "${ENVV[@]}" MX_UPDATE_RESTART_SCRIPT="$D/b.script" "$P/octomx" --profile "$PR" --script "$D/a.script" \
     "http://127.0.0.1:$PORT/payload/$(basename "$PAYLOAD")" > "$D/a.out" 2>&1)
  local rc=$?
  [ $rc = 0 ] && ok "linux download: A ran, installed the update and ended (rc 0)" || bad "linux download: A rc=$rc: $(tail -n 3 "$D/a.out" | tr '\n' ' ')"
  local held; held=$(du -b "$PR"/incomplete/*.part 2>/dev/null | awk '{s+=$1} END {print s+0}')
  grep -q "engine shut down .*every download is saved" "$PR/logs/octomx.log" && ok "linux download: A saved every download before the install ($(grep -m1 'engine shut down' "$PR/logs/octomx.log" | sed 's/.*engine shut down/engine shut down/' | cut -c1-90))" \
                                                                         || bad "linux download: A did not say every download is saved"
  local pid; pid=$(grep -ahm1 "update: started .*bin/octomx" "$PR/logs/octomx.log" "$D/a.out" | sed -n 's/.*(pid \([0-9]*\)).*/\1/p' | head -n1)
  [ -n "$pid" ] && ok "linux download: A started B (pid $pid), the part file holds $held bytes" || bad "linux download: no pid of B"
  download_finished "linux download" "$PR/logs/octomx.log" "$PR/incoming/$(basename "$PAYLOAD")" "$pid" 240
  grep -q "waited .* for the previous OctoMX" "$PR/logs/octomx.log" && ok "linux download: B $(grep -m1 'waited .* for the previous' "$PR/logs/octomx.log" | sed 's/.*update: //')" \
                                                                  || bad "linux download: B did not wait for A"
  grep -q "not closed properly" "$PR/logs/octomx.log" && bad "linux download: B found an unclean stop" || ok "linux download: B found A closed properly"
  [ "$(grep -c 'engine: restored 1 downloads' "$PR/logs/octomx.log")" -ge 1 ] && ok "linux download: B took the download up again ($held bytes in the part file right after A ended)" \
                                                                           || bad "linux download: B did not restore the download"
  wait_gone "--profile $PR" 60 || bad "linux download: B still runs"
  tree_is "$P" "$VBFULL" "linux download"
  stop_server
}

# ================================================================================ wine
wine_scenario() { # <name> <server mode> <scale>
  local name=$1 mode=$2 scale=$3
  local D=$WORK/wine-$name
  rm -rf "$D"; mkdir -p "$D/cwd" "$D/home" "$D/osroot"
  (cd "$D" && 7z x -y "$A/$WIN_A" > "$D/unzip.log") || { bad "wine $name: unzip A"; return; }
  local P=$D/OctoMX-$VA-win64 PR=$D/wprof-$name
  user_files "$P"
  winp() { local p=${1//\//\\}; echo "Z:$p"; }
  settings "$PR" "$scale" "http://127.0.0.1:$PORT/repos/robtacconelli/OctoMX/releases/latest"
  local S=$SHOTS/wine-$name-$scale SW
  SW=$(winp "$S")
  case "$name" in
    restart) printf '%s\r\n' "update-expect-version $VA" "update-wait state:ready 240000" "wait 600" "shot $SW-ready.png" \
                             "click updateRestart" "wait 90000" > "$D/a.script" ;;
    later)   printf '%s\r\n' "update-wait state:ready 240000" "wait 600" "shot $SW-ready.png" "click updateInstallLater" \
                             "wait 1500" "quit" > "$D/a.script" ;;
    *)       printf '%s\r\n' "update-wait updateBox 240000" "wait 600" "shot $SW-refused.png" "click updateLater" "wait 500" "quit" > "$D/a.script" ;;
  esac
  printf '%s\r\n' "update-expect-version $VB" "update-wait updatedBox 60000" "wait 600" "shot $SW-updated.png" "click updatedOk" \
                  "wait 500" "quit" > "$D/b.script"
  server "$mode" "$D/server.log"
  local WENV=(env -i HOME="$D/home" PATH=/usr/bin:/bin LANG=C.UTF-8 ${TESTKEY:+MX_UPDATE_TEST_PUBKEY=$TESTKEY} DISPLAY=":$XN" TMPDIR="$D/home" MX_OS_ROOT="$(winp "$D/osroot")"
              WINEPREFIX="$WPFX" WINEARCH=win64 WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d")
  (cd "$D/cwd" && timeout 500 "${WENV[@]}" MX_UPDATE_RESTART_SCRIPT="$(winp "$D/b.script")" wine "$P/octomx.exe" \
     --profile "$(winp "$PR")" --size $((1024 * scale / 100))x$((714 * scale / 100)) --script "$(winp "$D/a.script")" > "$D/a.out" 2>&1)
  local rc=$?
  [ $rc = 0 ] && ok "wine $name: A ran and ended (rc 0)" || bad "wine $name: A rc=$rc: $(tail -n 3 "$D/a.out" | tr '\n' ' ')"
  local LOGF=$PR/logs/octomx.log
  case "$name" in
    restart)
      sleep 3
      wait_gone "wprof-$name" 180 && ok "wine restart: the restarted program ran its script and ended" \
                                   || { bad "wine restart: the restarted program is still running"; pkill -f -- "wprof-$name"; }
      grep -ahq "update: started .*octomx.exe" "$LOGF" "$D/a.out" && ok "wine restart: A started the new program: $(grep -ahm1 'update: started' "$LOGF" "$D/a.out" | tr -d '\r' | sed 's/.*octomx: //' | cut -c1-200)" \
                                                     || info "wine restart: no restart line in A's output (a GUI program's stderr under wine)"
      grep -aq "waited .* for the previous OctoMX" "$LOGF" && ok "wine restart: $(grep -am1 'waited .* for the previous' "$LOGF" | tr -d '\r' | cut -c25-200)"
      ;;
    later)
      tree_is "$P" "$VBFULL" "wine later (after close, before the next start)"
      (cd "$D/cwd" && timeout 200 "${WENV[@]}" wine "$P/octomx.exe" --profile "$(winp "$PR")" --size $((1024 * scale / 100))x$((714 * scale / 100)) \
         --script "$(winp "$D/b.script")" > "$D/b.out" 2>&1)
      rc=$?
      [ $rc = 0 ] && ok "wine later: the next start is $VB and shows the Updated box (rc 0)" || bad "wine later: B rc=$rc"
      ;;
  esac
  case "$name" in
    restart|later)
      tree_is "$P" "$VBFULL" "wine $name"
      user_files_intact "$P" "wine $name"
      local fv; fv=$(strings -el "$P/octomx.exe" | grep -A1 -x FileVersion | tail -n1)
      [[ "$fv" == "$VBFULL"* ]] && ok "wine $name: octomx.exe FileVersion $fv" || bad "wine $name: octomx.exe FileVersion '$fv'"
      grep -aq "replaced $VA.0" "$LOGF" && ok "wine $name: log: $(grep -am1 "replaced $VA.0" "$LOGF" | tr -d '\r' | cut -c25-220)" || bad "wine $name: no 'replaced' line"
      for f in "$S-ready.png" "$S-ready.updateReadyBox.png" "$S-updated.png" "$S-updated.updatedBox.png"; do
        [ -s "$f" ] && info "screenshot $f" || bad "wine $name: no $f"
      done
      ;;
    tampered|unsigned)
      tree_is "$P" "$VA.0" "wine $name (unchanged)"
      user_files_intact "$P" "wine $name"
      local why; why=$(grep -am1 'no automatic install' "$LOGF" | tr -d '\r' | sed 's/.*no automatic install of //')
      [ -n "$why" ] && ok "wine $name: refused: $why" || bad "wine $name: no refusal in the log"
      [ -s "$S-refused.updateBox.png" ] && info "screenshot $S-refused.updateBox.png" || bad "wine $name: no box screenshot"
      if [ "$name" = unsigned ]; then
        grep -q "/assets/OctoMX-" "$D/server.log" && bad "wine unsigned: the archive was downloaded" || ok "wine unsigned: the archive was never requested"
      fi
      ;;
  esac
  info "wine $name: server requests: $(grep '^GET' "$D/server.log" | awk '{print $2}' | tr '\n' ' ' | cut -c1-300)"
  stop_server
}

wine_download() {
  local D=$WORK/wine-download
  rm -rf "$D"; mkdir -p "$D/cwd" "$D/home" "$D/osroot"
  (cd "$D" && 7z x -y "$A/$WIN_A" > "$D/unzip.log") || { bad "wine download: unzip A"; return; }
  local P=$D/OctoMX-$VA-win64 PR=$D/wprof-download
  winp() { local p=${1//\//\\}; echo "Z:$p"; }
  make_payload
  settings "$PR" 100 "http://127.0.0.1:$PORT/repos/robtacconelli/OctoMX/releases/latest" \
    "\"http/enabled\": true, \"transfers/incomingDir\": \"Z:$PR/incoming\", \"transfers/incompleteDir\": \"Z:$PR/incomplete\""
  printf '%s\r\n' "update-expect-version $VA" "update-wait state:ready 240000" "wait 3000" "click updateRestart" "wait 90000" > "$D/a.script"
  printf '%s\r\n' "update-expect-version $VB" "update-wait updatedBox 60000" "click updatedOk" "wait 150000" "quit" > "$D/b.script"
  server good "$D/server.log"
  local WENV=(env -i HOME="$D/home" PATH=/usr/bin:/bin LANG=C.UTF-8 ${TESTKEY:+MX_UPDATE_TEST_PUBKEY=$TESTKEY} DISPLAY=":$XN" TMPDIR="$D/home" MX_OS_ROOT="$(winp "$D/osroot")"
              MX_OVERLAY_DISCOVERY=0 WINEPREFIX="$WPFX" WINEARCH=win64 WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d")
  (cd "$D/cwd" && timeout 500 "${WENV[@]}" MX_UPDATE_RESTART_SCRIPT="$(winp "$D/b.script")" wine "$P/octomx.exe" \
     --profile "$(winp "$PR")" --script "$(winp "$D/a.script")" "http://127.0.0.1:$PORT/payload/$(basename "$PAYLOAD")" > "$D/a.out" 2>&1)
  local rc=$?
  [ $rc = 0 ] && ok "wine download: A ran, installed the update and ended (rc 0)" || bad "wine download: A rc=$rc: $(tail -n 3 "$D/a.out" | tr '\n' ' ')"
  local LOGF=$PR/logs/octomx.log
  grep -aq "engine shut down .*every download is saved" "$LOGF" && ok "wine download: A saved every download before the install" \
                                                                || bad "wine download: A did not say every download is saved"
  download_finished "wine download" "$LOGF" "$PR/incoming/$(basename "$PAYLOAD")" "" 300
  grep -aq "waited .* for the previous OctoMX" "$LOGF" && ok "wine download: B $(grep -am1 'waited .* for the previous' "$LOGF" | tr -d '\r' | sed 's/.*update: //')" \
                                                       || bad "wine download: B did not wait for A"
  grep -aq "not closed properly" "$LOGF" && bad "wine download: B found an unclean stop" || ok "wine download: B found A closed properly"
  grep -aq "engine: restored 1 downloads" "$LOGF" && ok "wine download: B took the download up again" || bad "wine download: B did not restore the download"
  # B is closed through its script's quit (wine has no SIGTERM for it): end it when the file is there
  sleep 2
  wait_gone "wprof-download" 420 || { bad "wine download: B still runs"; pkill -f -- "$PR"; }
  tree_is "$P" "$VBFULL" "wine download"
  stop_server
}

want() { [ -z "$ONLY" ] || [ "$ONLY" = "$1" ]; }
if [ $DO_LINUX = 1 ]; then
  [ -n "$LIN_A" ] && [ -n "$LIN_B" ] || bad "no Linux archives"
  want restart && linux_scenario restart good 100
  want later && linux_scenario later good 200
  want tampered && linux_scenario tampered tampered 100
  want unsigned && linux_scenario unsigned unsigned 200
  want download && linux_download
fi
if [ $DO_WINE = 1 ]; then
  [ -n "$WIN_A" ] || bad "no Windows archive"
  WPFX=$WORK/wineprefix
  [ -d "$WPFX/drive_c" ] || cp -a "$ROOT/toolchain/wineprefix" "$WPFX"
  if [ -z "$XN" ]; then for n in 80 81 82 83 84 85 86 88 89; do [ -e "/tmp/.X$n-lock" ] || [ -e "/tmp/.X11-unix/X$n" ] || { XN=$n; break; }; done; fi
  if ! [ -S "/tmp/.X11-unix/X$XN" ]; then   # our own Xvnc, large enough for 200 %
    Xvnc ":$XN" -geometry 2600x1700 -depth 24 -localhost -rfbport $((5900 + XN)) -nolisten tcp -ac -desktop octomx-e2e > "$WORK/xvnc.log" 2>&1 &
    XVNC_PID=$!
    for _ in $(seq 1 50); do [ -S "/tmp/.X11-unix/X$XN" ] && break; sleep 0.1; done
    [ -S "/tmp/.X11-unix/X$XN" ] || bad "Xvnc :$XN did not start"
  fi
  want restart && wine_scenario restart good 100
  want later && wine_scenario later good 200
  want tampered && wine_scenario tampered tampered 200
  want unsigned && wine_scenario unsigned unsigned 100
  want download && wine_download
  WINEPREFIX=$WPFX wineserver -k 2>/dev/null
fi
echo
[ $FAILS = 0 ] && echo "E2E OK" || echo "E2E: $FAILS failure(s)"
[ $FAILS = 0 ]
