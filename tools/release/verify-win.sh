#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# tools/release/verify-win.sh <OctoMX-*-win64.zip> <work-dir> [--xvnc N]
# Unzips the archive into <work-dir> and proves the package is complete and minimal:
#   1. PE checks (llvm-readobj / llvm-objdump -p): GUI subsystem, version resource + icon, no debug
#      sections, no test DLLs/exes; every import of every shipped PE file is either shipped or a
#      Windows system DLL; every shipped DLL is imported by something (no dead weight)
#   2. no fault-injection hook in octomx.exe; [upd-1] no test update key hook, MANIFEST.txt matches,
#      SHA256SUMS.sig next to the archive verifies (octomx.exe --verify-* under wine)
#   3. wine with a clean environment (env -i, PATH=/usr/bin:/bin, no WINEPATH; started from an
#      empty directory): --demo --screenshot
#   4. wine real core with a throwaway --profile: networks online, HTTPS web search via schannel,
#      SQLite database; WINEDEBUG=+loaddll proves which native DLLs and plugins were loaded and
#      that all of them came from the package folder
# Prints OK/INFO/WARN/FAIL lines; exit 1 on any FAIL.
set -uo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
source "$ROOT/toolchain/env.sh" >/dev/null
ZIP=$(realpath "$1"); W=$(realpath -m "$2"); shift 2
XN=""
[ "${1:-}" = "--xvnc" ] && XN=$2
rm -rf "$W"; mkdir -p "$W/home" "$W/cwd"
(cd "$W" && 7z x -y "$ZIP" > "$W/unzip.log") || { echo "FAIL unzip"; exit 1; }
P=$(find "$W" -mindepth 1 -maxdepth 1 -type d -name 'OctoMX-*' | head -n1)
[ -n "$P" ] || { echo "FAIL no package dir in archive"; exit 1; }
FAILS=0
ok()   { echo "OK   $*"; }
info() { echo "INFO $*"; }
warn() { echo "WARN $*"; }
bad()  { echo "FAIL $*"; FAILS=$((FAILS+1)); }
winp() { local p=${1//\//\\}; echo "Z:$p"; }

# ---- 1. PE checks -------------------------------------------------------------------------------
EXE=$P/octomx.exe
SUB=$(llvm-objdump -p "$EXE" | sed -n 's/^Subsystem\s*[0-9a-f]*\s*(\(.*\))/\1/p')
[ "$SUB" = "Windows GUI" ] && ok "octomx.exe subsystem: Windows GUI (no console window)" || bad "subsystem: $SUB"
RES=$(llvm-readobj --coff-resources "$EXE")
grep -q 'Type: VERSIONINFO' <<<"$RES" && grep -q 'Type: GROUP_ICON' <<<"$RES" \
  && ok "octomx.exe resources: VERSIONINFO (FileVersion $(strings -el "$EXE" | grep -A1 -x FileVersion | tail -n1), ProductName $(strings -el "$EXE" | grep -A1 -x ProductName | tail -n1)) + application icon (GROUP_ICON $(grep -A8 'Type: GROUP_ICON' <<<"$RES" | sed -n 's/.*Name: \([A-Za-z0-9_]*\).*/\1/p'), $(sed -n '/Type: ICON/,/Type: GROUP_ICON/p' <<<"$RES" | grep -c 'Name: (ID') sizes)" \
  || bad "octomx.exe lacks VERSIONINFO or icon resources"
DBG=$(for f in $(find "$P" -iname '*.exe' -o -iname '*.dll'); do llvm-objdump -h "$f" | grep -q '\.debug_' && echo "${f#$P/}"; done)
[ -z "$DBG" ] && ok "no shipped PE file has .debug_* sections" || bad "debug sections in: $DBG"
TESTS=$(find "$P" -iname 'Qt6Test*.dll' -o -iname 'tst_*' -o -iname '*.pdb' -o -iname '*.debug' -o -iname 'mxcli*')
[ -z "$TESTS" ] && ok "no test-tool-chain files (Qt6Test, tst_*, mxcli, symbols)" || bad "test files shipped: $TESTS"
SYSTEM_RE='^(api-ms-win-.*|ext-ms-.*|advapi32|authz|bcrypt|comctl32|comdlg32|crypt32|d3d11|d3d12|d3d9|dcomp|dnsapi|dwmapi|dwrite|dxgi|gdi32|imm32|iphlpapi|kernel32|mpr|msvcrt|mswsock|ncrypt|netapi32|ntdll|ole32|oleaut32|opengl32|secur32|setupapi|shcore|shell32|shlwapi|user32|userenv|usp10|uxtheme|version|winhttp|winmm|winspool\.drv|wintrust|wtsapi32|ws2_32|dbghelp|mscms|d2d1|msimg32|oleacc|uiautomationcore|dxcore|bcryptprimitives|wevtapi|hid|xinput1_4|cfgmgr32|powrprof|propsys|rpcrt4|sspicli|wldap32|normaliz|runtimeobject|dxva2|mf|mfplat|mfreadwrite|wininet|urlmon)(\.dll)?$'
declare -A SHIPPED IMPORTED SYS
for f in "$P"/*.dll; do SHIPPED[$(basename "$f" | tr A-Z a-z)]=1; done
while IFS= read -r f; do
  while IFS= read -r d; do
    k=$(tr A-Z a-z <<<"$d"); IMPORTED[$k]=1
    if [ -n "${SHIPPED[$k]:-}" ]; then :
    elif grep -qiE "$SYSTEM_RE" <<<"$k"; then SYS[$k]=1
    else bad "${f#$P/} imports $d: not shipped and not a Windows system DLL"; fi
  done < <(llvm-objdump -p "$f" | sed -n 's/^\s*DLL Name: \(.*\)$/\1/p')
done < <(find "$P" -iname '*.exe' -o -iname '*.dll')
ok "imports (llvm-objdump -p, all PE files) resolve to the $(echo ${!SHIPPED[@]} | wc -w) shipped DLLs + Windows: $(printf '%s\n' "${!SYS[@]}" | grep -v '^api-ms' | sort | tr '\n' ' ')+ $(printf '%s\n' "${!SYS[@]}" | grep -c '^api-ms') UCRT api-ms-win-crt-* sets"
for k in "${!SHIPPED[@]}"; do [ -n "${IMPORTED[$k]:-}" ] || bad "shipped $k is imported by nothing (not minimal)"; done
ok "every shipped DLL is imported by octomx.exe or a plugin: $(printf '%s\n' "${!SHIPPED[@]}" | sort | tr '\n' ' ')"
info "plugins: $(cd "$P" && find . -mindepth 2 -iname '*.dll' | sed 's|^\./||' | sort | tr '\n' ' ')"

# ---- 2. no test hooks -----------------------------------------------------------------------------
if grep -qa 'TEST HOOK: corrupting served bytes' "$EXE"; then bad "octomx.exe contains the overlay/testCorruptUploads hook"
else ok "octomx.exe has no fault-injection hook (MX_TEST_HOOKS=OFF)"; fi

# ---- 3./4. wine ---------------------------------------------------------------------------------
STARTED_X=0
if [ -z "$XN" ]; then
  for n in 91 92 93 94 95 96 97 98 99; do
    [ -e "/tmp/.X$n-lock" ] || [ -e "/tmp/.X11-unix/X$n" ] || { XN=$n; break; }
  done
fi
[ -n "$XN" ] || { bad "no free X display"; exit 1; }
if ! [ -S "/tmp/.X11-unix/X$XN" ]; then "$ROOT/tools/xvnc-start.sh" "$XN" > "$W/xvnc.log" 2>&1 || { bad "Xvnc :$XN"; exit 1; }; STARTED_X=1; fi
trap '[ $STARTED_X = 1 ] && "$ROOT/tools/xvnc-stop.sh" "$XN" >/dev/null 2>&1' EXIT
# clean environment: no Qt / llvm-mingw on PATH, no WINEPATH, no LD_LIBRARY_PATH
WENV=(env -i HOME="$W/home" PATH=/usr/bin:/bin LANG=C.UTF-8 DISPLAY=":$XN" TMPDIR="$W/home"
      XDG_CACHE_HOME="$W/home/.cache" XDG_DATA_HOME="$W/home/.local/share" XDG_CONFIG_HOME="$W/home/.config"
      WINEPREFIX="$WINEPREFIX" WINEARCH=win64 WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d")
"${WENV[@]}" WINEDEBUG=-all wine reg query 'HKCU\Environment' > "$W/wine-env.txt" 2>&1
"${WENV[@]}" WINEDEBUG=-all wine cmd /c echo %PATH% > "$W/wine-path.txt" 2>/dev/null
info "Windows-side PATH inside wine: $(tr -d '\r' < "$W/wine-path.txt" | head -n1)"
grep -qiE 'qt|llvm|mingw' "$W/wine-path.txt" && bad "wine PATH contains a Qt/mingw directory"

check_loaddll() { # <trace-file> <tag>
  local t=$1 tag=$2
  # e.g.  trace:loaddll:build_module Loaded L"Z:\\mnt\\...\\Qt6Core.dll" at 00006FFFF... : native
  local native; native=$(grep -a 'trace:loaddll' "$t" | grep -a ': native' | sed -n 's/.*Loaded L"\([^"]*\)".*/\1/p' | sed 's/\\\\/\\/g' | sort -u)
  local pw; pw=$(winp "$P")
  local outside; outside=$(grep -viF "$pw\\" <<<"$native" | grep -v '^$')
  local n; n=$(grep -c . <<<"$native")
  [ -z "$outside" ] && ok "$tag +loaddll: $n native modules loaded, all from the package: $(sed "s|^$(sed 's/[\\.]/\\&/g' <<<"$pw")\\\\||I" <<<"$native" | tr '\n' ' ')" \
                    || bad "$tag +loaddll: native modules from outside the package: $(tr '\n' ' ' <<<"$outside")"
  grep -aq 'Qt6Test' "$t" && bad "$tag: Qt6Test loaded"
  grep -aE 'trace:loaddll.*(not found|failed)' "$t" | head -n3 | sed "s/^/WARN $tag loaddll: /"
}

# 2b. [upd-1] update trust: no test key hook, MANIFEST.txt, SHA256SUMS.sig (the program's own verifier under wine)
if grep -qa 'TEST HOOK: trusting an extra update key' "$EXE"; then bad "octomx.exe can trust test update keys (built with MX_TEST_HOOKS)"
else ok "octomx.exe trusts only its built-in update keys (no test key hook compiled in)"; fi
(cd "$W/cwd" && timeout 120 "${WENV[@]}" WINEDEBUG=-all wine "$EXE" --verify-manifest "$(winp "$P")" \
   --verify-report "$(winp "$W/verify-manifest.out")" > /dev/null 2>&1)
rc=$?
[ $rc = 0 ] && ok "wine octomx.exe --verify-manifest: $(grep -m1 'MANIFEST.txt: version' "$W/verify-manifest.out" | tr -d '\r' | cut -c6-)" \
            || bad "wine octomx.exe --verify-manifest rc=$rc: $(grep FAIL "$W/verify-manifest.out" 2>/dev/null | tr '\n' ' ' | cut -c1-300)"
SUMDIR=$(dirname "$ZIP")
if [ -f "$SUMDIR/SHA256SUMS.sig" ]; then
  (cd "$W/cwd" && timeout 120 "${WENV[@]}" WINEDEBUG=-all wine "$EXE" --verify-update "$(winp "$SUMDIR")" \
     --verify-report "$(winp "$W/verify-update.out")" > /dev/null 2>&1)
  rc=$?
  [ $rc = 0 ] && ok "wine octomx.exe --verify-update: $(grep -m1 'signature' "$W/verify-update.out" | tr -d '\r' | cut -c6-); $(grep -c '^OK .*matches' "$W/verify-update.out") archive(s) match" \
              || bad "wine octomx.exe --verify-update rc=$rc: $(grep FAIL "$W/verify-update.out" 2>/dev/null | tr '\n' ' ' | cut -c1-300)"
else warn "no SHA256SUMS.sig next to $ZIP: the release is not signed and is never installed automatically"; fi

# 3. demo screenshot
(cd "$W/cwd" && timeout 120 "${WENV[@]}" WINEDEBUG=+loaddll wine "$EXE" --demo --profile "$(winp "$W/p-demo")" \
   --size 1024x714 --screenshot "$(winp "$W/demo.png")" > "$W/demo.out" 2> "$W/demo.trace")
rc=$?
if [ $rc = 0 ] && [ -s "$W/demo.png" ]; then ok "wine --demo --screenshot (clean env, cwd $W/cwd): rc 0, $W/demo.png ($(stat -c %s "$W/demo.png") bytes)"
else bad "wine --demo --screenshot rc=$rc: $(grep -av trace "$W/demo.trace" | tail -n 3 | tr '\n' ' ')"; fi
check_loaddll "$W/demo.trace" demo

# 4. real core
PR=$W/p-real; mkdir -p "$PR/incoming"
PRW=$(winp "$PR"); PRJ=${PRW//\\/\\\\}
cat > "$PR/settings.json" <<EOF
{ "connection/tcpPort": 47201, "connection/udpPort": 47202, "bt/listenPort": 47203,
  "ed2k/tcpPort": 47204, "ed2k/udpPort": 47205, "kad/udpPort": 47205, "gnutella/port": 47206,
  "rats/listenPort": 47207, "connection/upnp": false, "connection/natpmp": false,
  "updates/mode": "off", "updates/checkAtStart": false,
  "transfers/incomingDir": "$PRJ\\\\incoming", "misc/logLevel": "debug" }
EOF
cat > "$W/real.script" <<EOF
wait 60000
shot $(winp "$W/real-online.png")
page search
type searchText big buck bunny
key searchText Return
wait 25000
expect-rows searchResults 1
shot $(winp "$W/real-search.png")
quit
EOF
# [ren-1] no update check (GitHub) and no live OctoMX network (MX_OVERLAY_DISCOVERY=0)
(cd "$W/cwd" && timeout 240 "${WENV[@]}" MX_OVERLAY_DISCOVERY=0 WINEDEBUG=+loaddll wine "$EXE" --profile "$PRW" --size 1024x714 \
   --script "$(winp "$W/real.script")" > "$W/real.out" 2> "$W/real.trace")
rc=$?
[ $rc = 0 ] && ok "wine real core run: rc 0 (script incl. expect-rows searchResults >= 1)" || bad "wine real core rc=$rc: $(grep -av trace "$W/real.trace" | tail -n 3 | tr '\n' ' ')"
for s in real-online real-search; do [ -s "$W/$s.png" ] && info "screenshot $W/$s.png" || bad "no $s.png"; done
check_loaddll "$W/real.trace" real
for p in qwindows.dll qschannelbackend.dll qsqlite.dll; do
  grep -aq "loaddll.*$p" "$W/real.trace" && ok "plugin loaded: $p" || bad "plugin not loaded: $p"
done
grep -aq 'loaddll.*qico.dll' "$W/real.trace" "$W/demo.trace" && info "imageformats/qico.dll loaded" || info "imageformats/qico.dll not loaded in these runs (only --export-icons / .ico reads use it)"
LOGF=$PR/logs/octomx.log
if [ -f "$LOGF" ]; then
  head -c 16 "$PR/octomx.db" | grep -q 'SQLite format 3' && ok "SQLite: octomx.db created ($(grep -m1 -o 'mx.db: .*' "$LOGF" | tr -d '\r'))" || bad "no SQLite database"
  for pat in 'mx.bt: session online' 'Gnutella: connected to ultrapeer' 'eD2k: connected to' 'Kad: connected' 'OctoMX: .*listening on port' 'Rats: .*listening on port'; do
    l=$(grep -m1 -aE "$pat" "$LOGF" | tr -d '\r' | cut -c12-200)
    [ -n "$l" ] && ok "network: $l" || warn "network: no '$pat' line within the run"
  done
  grep -m1 -a 'CA source' "$LOGF" | tr -d '\r' | cut -c25-200 | sed 's/^/INFO /'
  WEB=$(grep -acE 'mx.web: search [0-9]+ .* ok .* [1-9][0-9]* hits' "$LOGF")
  [ "$WEB" -gt 0 ] && ok "HTTPS web search (schannel): $WEB index sites answered with hits, e.g. $(grep -m1 -aE 'mx.web: search [0-9]+ .* ok .* [1-9][0-9]* hits' "$LOGF" | tr -d '\r' | cut -c25-120)" \
                   || bad "no web index answered over HTTPS"
  grep -aiE 'ssl|tls|schannel' "$LOGF" | grep -aiE ' W |warn|fail|error' | head -n3 | tr -d '\r' | sed 's/^/WARN tls log: /'
  N=$(grep -acE 'Gnutella: connected to ultrapeer|eD2k: connected to|Kad: connected|mx.bt: session online' "$LOGF")
  [ "$N" -ge 2 ] && ok "networks online: $N connect events from internet networks" || bad "fewer than 2 internet networks came online"
else bad "no log $LOGF"; fi
echo
[ $FAILS = 0 ] && echo "RESULT: windows package verified" || echo "RESULT: $FAILS failure(s)"
[ $FAILS = 0 ]
