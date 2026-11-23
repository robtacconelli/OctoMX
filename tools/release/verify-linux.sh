#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# tools/release/verify-linux.sh <OctoMX-*-linux-x86_64.tar.xz> <work-dir>
# Extracts the archive into <work-dir> and proves the bundle is self-contained:
#   1. dependency check: every NEEDED entry of every ELF file is either in the bundle or on the
#      base-system allow list; `env -i ldd` shows nothing "not found" and lists what resolves
#      outside the bundle; glibc / libstdc++ symbol-version floor
#   2. the release binary contains no fault-injection hook; [upd-1] no test update key hook, its
#      MANIFEST.txt matches every file, SHA256SUMS.sig next to the archive verifies (octomx --verify-*)
#   3. env -i (no LD_LIBRARY_PATH, no Qt on PATH, QT_QPA_PLATFORM=offscreen): --demo --screenshot
#   4. env -i real core with a throwaway --profile: networks come online, an HTTPS web search
#      returns hits (Qt TLS via the openssl backend), SQLite profile database, and
#      /proc/<pid>/maps shows every Qt library / plugin mapped from inside the bundle
# Prints OK/INFO/WARN/FAIL lines; exit 1 on any FAIL.
set -uo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
source "$ROOT/toolchain/env.sh" >/dev/null
ARCHIVE=$(realpath "$1"); W=$(realpath -m "$2")
rm -rf "$W"; mkdir -p "$W/home"
tar -C "$W" -xf "$ARCHIVE" || { echo "FAIL extract"; exit 1; }
P=$(find "$W" -mindepth 1 -maxdepth 1 -type d -name 'OctoMX-*' | head -n1)
[ -n "$P" ] || { echo "FAIL no package dir in archive"; exit 1; }
FAILS=0
ok()   { echo "OK   $*"; }
info() { echo "INFO $*"; }
warn() { echo "WARN $*"; }
bad()  { echo "FAIL $*"; FAILS=$((FAILS+1)); }
CLEAN_ENV=(env -i HOME="$W/home" PATH=/usr/bin:/bin LANG=C.UTF-8 XDG_RUNTIME_DIR="$W/home/run"
           XDG_CACHE_HOME="$W/home/.cache" XDG_CONFIG_HOME="$W/home/.config" XDG_DATA_HOME="$W/home/.local/share")
mkdir -p "$W/home/run"; chmod 700 "$W/home/run"

# ---- 1. dependencies -------------------------------------------------------------------------
# base system = what every x86_64 desktop distribution has; Qt, ICU and the xcb-util helpers are bundled
ALLOW='^(libc\.so\.6|libm\.so\.6|libpthread\.so\.0|libdl\.so\.2|librt\.so\.1|libresolv\.so\.2|ld-linux-x86-64\.so\.2|libstdc\+\+\.so\.6|libgcc_s\.so\.1|libatomic\.so\.1|libz\.so\.1|libzstd\.so\.1|libglib-2\.0\.so\.0|libgthread-2\.0\.so\.0|libdbus-1\.so\.3|libX11\.so\.6|libX11-xcb\.so\.1|libxcb\.so\.1|libxcb-(randr|render|shape|shm|sync|xfixes|xkb|xinput|glx)\.so\.[0-9]+|libxkbcommon\.so\.0|libGL\.so\.1|libGLX\.so\.0|libOpenGL\.so\.0|libEGL\.so\.1|libfontconfig\.so\.1|libfreetype\.so\.6|libgssapi_krb5\.so\.2|libbrotlidec\.so\.1|libbrotlicommon\.so\.1|libpng16\.so\.16)$'
mapfile -t ELFS < <(find "$P" -type f \( -name '*.so*' -o -path '*/bin/octomx' \) -exec sh -c 'head -c4 "$1" | grep -q ELF && echo "$1"' _ {} \;)
info "ELF files in bundle: ${#ELFS[@]}"
declare -A INB; for f in "$P"/lib/*; do INB[$(basename "$f")]=1; done
declare -A OUTSIDE_DIRECT
for f in "${ELFS[@]}"; do
  while read -r need; do
    [ -n "${INB[$need]:-}" ] && continue
    if grep -qE "$ALLOW" <<<"$need"; then OUTSIDE_DIRECT[$need]=1
    else bad "${f#$P/} needs $need: neither bundled nor base system"; fi
  done < <(readelf -d "$f" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
  "${CLEAN_ENV[@]}" ldd "$f" 2>&1 | grep -q 'not found' && bad "env -i ldd ${f#$P/}: $("${CLEAN_ENV[@]}" ldd "$f" | grep 'not found' | tr -s ' \t' ' ' | tr '\n' ';')"
done
ok "direct system dependencies (all on the base-system list): $(printf '%s\n' "${!OUTSIDE_DIRECT[@]}" | sort | tr '\n' ' ')"
# the load roots (the binary and the plugins) give the closure exactly as the dynamic linker builds it
# at run time; the bundled xcb-util libs have no RUNPATH, their deps are found already loaded by soname
ROOTS=("$P/bin/octomx" $(find "$P/plugins" -name '*.so'))
CLOSURE=$(for f in "${ROOTS[@]}"; do "${CLEAN_ENV[@]}" ldd "$f"; done | sed -n 's/^\s*\(\S*\) => \(\/\S*\) .*/\1 \2/p' \
          | grep -v " $P/" | awk '{print $2}' | sort -u)
info "env -i ldd of bin/octomx + $(( ${#ROOTS[@]} - 1 )) plugins: $(wc -l <<<"$CLOSURE") libraries resolve outside the bundle, all from $(dirname $CLOSURE | sort -u | tr '\n' ' ')"
info "  $(xargs -n1 basename <<<"$CLOSURE" | tr '\n' ' ')"
QTOUT=$(grep -iE 'Qt6|icu|xcb-(cursor|icccm|image|keysyms|render-util|util)\.|xkbcommon-x11' <<<"$CLOSURE")
[ -z "$QTOUT" ] && ok "no Qt / ICU / xcb-util library resolves outside the bundle" || bad "resolved outside: $QTOUT"
GLIBC=$(for f in "${ELFS[@]}"; do objdump -T "$f" 2>/dev/null; done | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -n1)
GLIBCXX=$(for f in "${ELFS[@]}"; do objdump -T "$f" 2>/dev/null; done | grep -o 'GLIBCXX_[0-9.]*' | sort -uV | tail -n1)
CXXABI=$(for f in "${ELFS[@]}"; do objdump -T "$f" 2>/dev/null; done | grep -o 'CXXABI_[0-9.]*' | sort -uV | tail -n1)
GLIBC_APP=$(objdump -T "$P/bin/octomx" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -n1)
GLIBC_QT=$(for f in "$P"/lib/libQt6*; do objdump -T "$f"; done | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -n1)
info "symbol-version floor: $GLIBC (bin/octomx $GLIBC_APP, Qt libs $GLIBC_QT), $GLIBCXX, $CXXABI"
# dlopen()ed at run time, not visible to ldd
LDC=$(/sbin/ldconfig -p 2>/dev/null)
for so in libssl.so.3 libcrypto.so.3; do
  if grep -q "$so" <<<"$LDC"; then info "dlopen target $so present on this host (Qt TLS backend needs it)"
  else warn "dlopen target $so not found by ldconfig"; fi
done

# ---- 2. no test hooks --------------------------------------------------------------------------
if grep -qa 'TEST HOOK: corrupting served bytes' "$P/bin/octomx"; then bad "bin/octomx contains the overlay/testCorruptUploads hook"
else ok "bin/octomx has no fault-injection hook (MX_TEST_HOOKS=OFF; 'TEST HOOK unavailable' guard present: $(grep -ca 'TEST HOOK unavailable' "$P/bin/octomx"))"; fi
readelf -d "$P/bin/octomx" | grep -q 'R.*PATH.*\[\$ORIGIN/../lib\]' && ok "bin/octomx RUNPATH is \$ORIGIN/../lib" || bad "bin/octomx RUNPATH: $(readelf -d "$P/bin/octomx" | grep PATH)"
readelf -S "$P/bin/octomx" | grep -q '\.debug_info' && bad "bin/octomx still has debug info" || ok "bin/octomx is stripped (debuglink $(readelf -p .gnu_debuglink "$P/bin/octomx" 2>/dev/null | sed -n 's/.*\] *\(octomx.debug\).*/\1/p'))"

# ---- 2b. [upd-1] update trust: no test key hook, MANIFEST.txt, SHA256SUMS.sig (the program's own verifier)
if grep -qa 'TEST HOOK: trusting an extra update key' "$P/bin/octomx"; then bad "bin/octomx can trust test update keys (built with MX_TEST_HOOKS)"
else ok "bin/octomx trusts only its built-in update keys (no test key hook compiled in)"; fi
"${CLEAN_ENV[@]}" "$P/bin/octomx" --verify-manifest "$P" > "$W/verify-manifest.out" 2>&1
rc=$?
[ $rc = 0 ] && ok "octomx --verify-manifest: $(grep -m1 'MANIFEST.txt: version' "$W/verify-manifest.out" | cut -c6-)" \
            || bad "octomx --verify-manifest rc=$rc: $(grep FAIL "$W/verify-manifest.out" | tr '\n' ' ' | cut -c1-300)"
SUMDIR=$(dirname "$ARCHIVE")
if [ -f "$SUMDIR/SHA256SUMS.sig" ]; then
  "${CLEAN_ENV[@]}" "$P/bin/octomx" --verify-update "$SUMDIR" > "$W/verify-update.out" 2>&1
  rc=$?
  [ $rc = 0 ] && ok "octomx --verify-update: $(grep -m1 'signature' "$W/verify-update.out" | cut -c6-); $(grep -c '^OK .*matches' "$W/verify-update.out") archive(s) match" \
              || bad "octomx --verify-update rc=$rc: $(grep FAIL "$W/verify-update.out" | tr '\n' ' ' | cut -c1-300)"
else warn "no SHA256SUMS.sig next to $ARCHIVE: the release is not signed and is never installed automatically"; fi

# ---- 3. demo screenshot under env -i ------------------------------------------------------------
cat > "$W/offscreen.json" <<'JSON'
{ "synchronousWindowSystemEvents": false, "windowFrameMargins": false,
  "screens": [ { "name": "shot", "x": 0, "y": 0, "width": 1920, "height": 1200,
                 "logicalDpi": 96, "logicalBaseDpi": 96, "dpr": 1 } ] }
JSON
QPA="offscreen:configfile=$W/offscreen.json"
(cd "$W" && timeout 60 "${CLEAN_ENV[@]}" QT_QPA_PLATFORM="$QPA" "$P/octomx" --demo --profile "$W/p-demo" \
   --size 1024x714 --screenshot "$W/demo.png" > "$W/demo.out" 2>&1)
rc=$?
if [ $rc = 0 ] && [ -s "$W/demo.png" ]; then ok "env -i --demo --screenshot: rc 0, $W/demo.png ($(stat -c %s "$W/demo.png") bytes)"
else bad "env -i --demo --screenshot rc=$rc: $(tail -n 3 "$W/demo.out" | tr '\n' ' ')"; fi
"${CLEAN_ENV[@]}" QT_QPA_PLATFORM="offscreen" "$P/octomx" --version > "$W/version.out" 2>&1 && info "--version: $(head -n1 "$W/version.out")"

# ---- 3b. the real X11 path: launcher defaults to xcb, on a private Xvnc --------------------------
XN=""
for n in 91 92 93 94 95 96 97 98 99; do [ -e "/tmp/.X$n-lock" ] || [ -e "/tmp/.X11-unix/X$n" ] || { XN=$n; break; }; done
if [ -n "$XN" ] && command -v Xvnc >/dev/null && "$ROOT/tools/xvnc-start.sh" "$XN" > "$W/xvnc.log" 2>&1; then
  (cd "$W" && exec timeout 60 "${CLEAN_ENV[@]}" DISPLAY=":$XN" "$P/octomx" --demo --profile "$W/p-xcb" --size 1024x714 \
     --screenshot "$W/demo-xcb.png" --screenshot-delay 4000 > "$W/xcb.out" 2>&1) &
  XPID=$!
  sleep 2.5
  XAPP=$(pgrep -f "^$P/bin/octomx --demo --profile $W/p-xcb" | head -n1)
  [ -n "$XAPP" ] && cp "/proc/$XAPP/maps" "$W/xcb.maps" 2>/dev/null
  wait $XPID; rc=$?
  "$ROOT/tools/xvnc-stop.sh" "$XN" >/dev/null 2>&1
  if [ $rc = 0 ] && [ -s "$W/demo-xcb.png" ]; then ok "env -i xcb on Xvnc :$XN (launcher default platform): rc 0, $W/demo-xcb.png"
  else bad "xcb run rc=$rc: $(tail -n 3 "$W/xcb.out" | tr '\n' ' ')"; fi
  if [ -s "$W/xcb.maps" ]; then
    XM=$(awk '{print $6}' "$W/xcb.maps" | grep '\.so' | sort -u)
    XB=$(grep -E 'libqxcb|XcbQpa|xcb-(cursor|icccm|image|keysyms|render-util|util)\.|xkbcommon-x11' <<<"$XM")
    XO=$(grep -v "^$P/" <<<"$XB")
    [ -z "$XO" ] && ok "xcb /proc/maps: $(wc -l <<<"$XB") xcb plugin / helper objects, all from the bundle: $(xargs -n1 basename <<<"$XB" | tr '\n' ' ')" \
                 || bad "xcb /proc/maps from outside the bundle: $XO"
  else warn "could not sample /proc/maps of the xcb run"; fi
else warn "no Xvnc / free display: xcb path not exercised"; fi

# ---- 4. real core under env -i ------------------------------------------------------------------
PR=$W/p-real; mkdir -p "$PR/incoming"
cat > "$PR/settings.json" <<EOF
{ "connection/tcpPort": 47101, "connection/udpPort": 47102, "bt/listenPort": 47103,
  "ed2k/tcpPort": 47104, "ed2k/udpPort": 47105, "kad/udpPort": 47105, "gnutella/port": 47106,
  "rats/listenPort": 47107, "connection/upnp": false, "connection/natpmp": false,
  "updates/mode": "off", "updates/checkAtStart": false,
  "transfers/incomingDir": "$PR/incoming", "misc/logLevel": "debug" }
EOF
cat > "$W/real.script" <<EOF
wait 45000
shot $W/real-online.png
page search
type searchText big buck bunny
key searchText Return
wait 20000
expect-rows searchResults 1
shot $W/real-search.png
quit
EOF
# [ren-1] no update check (GitHub) and no live OctoMX network (MX_OVERLAY_DISCOVERY=0): the overlay
# still listens on its port; the other networks are contacted for real
(cd "$W" && exec timeout 150 "${CLEAN_ENV[@]}" MX_OVERLAY_DISCOVERY=0 QT_QPA_PLATFORM="$QPA" "$P/octomx" --profile "$PR" --size 1024x714 \
   --script "$W/real.script" > "$W/real.out" 2>&1) &
RPID=$!
sleep 40
APID=$(pgrep -f "^$P/bin/octomx --profile $PR" | head -n1)
[ -n "$APID" ] && cp "/proc/$APID/maps" "$W/real.maps" 2>/dev/null
wait $RPID; rc=$?
LOGF=$PR/logs/octomx.log
[ $rc = 0 ] && ok "env -i real core run: rc 0 (script incl. expect-rows searchResults >= 1)" || bad "env -i real core run rc=$rc: $(tail -n 3 "$W/real.out" | tr '\n' ' ')"
for s in real-online real-search; do [ -s "$W/$s.png" ] && info "screenshot $W/$s.png" || bad "no $s.png"; done
if [ -s "$W/real.maps" ]; then
  MAPPED=$(awk '{print $6}' "$W/real.maps" | grep '\.so' | sort -u)
  QTM=$(grep -E 'Qt6|/plugins/|icu' <<<"$MAPPED")
  OUTQ=$(grep -v "^$P/" <<<"$QTM")
  [ -z "$OUTQ" ] && ok "/proc/maps: $(wc -l <<<"$QTM") Qt/ICU/plugin objects mapped, all from the bundle: $(grep /plugins/ <<<"$QTM" | sed "s|$P/plugins/||" | tr '\n' ' ')" \
                 || bad "/proc/maps: mapped from outside the bundle: $OUTQ"
  grep -q 'libssl.so.3' <<<"$MAPPED" && ok "/proc/maps: system $(grep -m1 'libssl.so.3' <<<"$MAPPED") loaded by the Qt TLS backend"
  info "/proc/maps: libraries mapped from outside the bundle: $(grep -v "^$P/" <<<"$MAPPED" | xargs -n1 basename | sort -u | tr '\n' ' ')"
else bad "could not sample /proc/<pid>/maps"; fi
if [ -f "$LOGF" ]; then
  head -c 16 "$PR/octomx.db" | grep -q 'SQLite format 3' && ok "SQLite: $PR/octomx.db created ($(grep -m1 -o 'mx.db: .*' "$LOGF"))" || bad "no SQLite database"
  for pat in 'mx.bt: session online' 'Gnutella: connected to ultrapeer' 'eD2k: connected to' 'Kad: connected' 'OctoMX: .*listening on port' 'Rats: .*listening on port'; do
    l=$(grep -m1 -E "$pat" "$LOGF" | cut -c12-200)
    [ -n "$l" ] && ok "network: $l" || warn "network: no '$pat' line within the run"
  done
  WEB=$(grep -E 'mx.web: search [0-9]+ .* ok .* [1-9][0-9]* hits' "$LOGF" | wc -l)
  [ "$WEB" -gt 0 ] && ok "HTTPS web search: $WEB index sites answered with hits, e.g. $(grep -m1 -E 'mx.web: search [0-9]+ .* ok .* [1-9][0-9]* hits' "$LOGF" | cut -c25-120)" \
                   || bad "no web index answered over HTTPS"
  grep -iE 'ssl|tls' "$LOGF" | grep -iE 'warn|fail|error|W mx|W qt' | head -n3 | sed 's/^/WARN tls log: /'
  N=$(grep -cE 'Gnutella: connected to ultrapeer|eD2k: connected to|Kad: connected|mx.bt: session online' "$LOGF")
  [ "$N" -ge 2 ] && ok "networks online: at least $N connect events from internet networks" || bad "fewer than 2 internet networks came online"
else bad "no log $LOGF"; fi
echo
[ $FAILS = 0 ] && echo "RESULT: linux bundle verified" || echo "RESULT: $FAILS failure(s)"
[ $FAILS = 0 ]
