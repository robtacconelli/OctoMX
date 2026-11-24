#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# tools/mac/verify.sh: check the macOS package as a user gets it (the .zip, not the build tree).
#
#   tools/mac/verify.sh <OctoMX-X.Y-macos-*.zip> <work-dir>
#
#  1. unpacks the archive with ditto into an empty folder: one top folder with OctoMX.app,
#     README-mac.txt and MANIFEST.txt
#  2. Info.plist read back (identifier, versions, minimum macOS, URL schemes, document types,
#     local network text), the icon present
#  3. every Mach-O file: the architectures of the package name, linked only against the bundle,
#     /System and /usr/lib; the Qt plugins we need are there and nothing from a Qt or Homebrew
#     prefix of the build machine is referenced
#  4. codesign --verify --deep --strict, codesign -dv, spctl (recorded; ad-hoc is rejected by
#     Gatekeeper by design)
#  5. no test hooks in the program; the program's own verifier: octomx --verify-manifest
#     <package dir>; octomx --version
#  6. a demo start with the real Cocoa platform: --demo --screenshot
# Exit 1 on the first class of failure; prints OK / FAIL lines and a RESULT line.
set -uo pipefail
[ $# -eq 2 ] || { echo "usage: $0 <package.zip> <work-dir>" >&2; exit 2; }
ZIP=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
mkdir -p "$2"; W=$(cd "$2" && pwd)
FAILS=0
ok() { echo "OK   $*"; }
bad() { echo "FAIL $*"; FAILS=$((FAILS + 1)); }
info() { echo "     $*"; }
run_limited() { perl -e 'alarm shift; exec @ARGV or die "exec: $!"' "$@"; }

U=$W/unpacked
rm -rf "$U"; mkdir -p "$U"
ditto -x -k "$ZIP" "$U" || { bad "ditto could not unpack $ZIP"; exit 1; }
TOP=$(ls "$U")
NAME=$(basename "$ZIP" .zip)
[ "$TOP" = "$NAME" ] && ok "one top folder $TOP" || bad "top level of the archive: $(echo $TOP)"
P=$U/$NAME
APP=$P/OctoMX.app
for f in OctoMX.app README-mac.txt MANIFEST.txt; do [ -e "$P/$f" ] || bad "missing $f"; done
info "top folder: $(ls "$P" | tr '\n' ' ')"
TAG=${NAME#OctoMX-*-}

# ---- 2. Info.plist
PL=$APP/Contents/Info.plist
plutil -lint "$PL" > /dev/null && ok "Info.plist is valid" || bad "Info.plist does not parse"
pb() { /usr/libexec/PlistBuddy -c "Print :$1" "$PL" 2>/dev/null; }
info "CFBundleIdentifier $(pb CFBundleIdentifier), CFBundleShortVersionString $(pb CFBundleShortVersionString), CFBundleVersion $(pb CFBundleVersion), LSMinimumSystemVersion $(pb LSMinimumSystemVersion), CFBundleExecutable $(pb CFBundleExecutable), NSHighResolutionCapable $(pb NSHighResolutionCapable)"
[ "$(pb CFBundleIdentifier)" = io.github.robtacconelli.octomx ] && ok "bundle identifier" || bad "bundle identifier $(pb CFBundleIdentifier)"
SCHEMES=$(plutil -extract CFBundleURLTypes xml1 -o - "$PL" | grep -o '<string>[a-z0-9]*</string>' | sed 's/<[^>]*>//g' | tr '\n' ' ')
case "$SCHEMES" in *magnet*ed2k*) ok "URL schemes: $SCHEMES" ;; *) bad "URL schemes: $SCHEMES" ;; esac
plutil -extract CFBundleDocumentTypes xml1 -o - "$PL" | grep -q 'org.bittorrent.torrent' && ok ".torrent document type" || bad "no .torrent document type"
[ -n "$(pb NSLocalNetworkUsageDescription)" ] && ok "local network usage text" || bad "no NSLocalNetworkUsageDescription"
[ -s "$APP/Contents/Resources/octomx.icns" ] && ok "icon $(pb CFBundleIconFile), $(stat -f %z "$APP/Contents/Resources/octomx.icns") bytes" || bad "no octomx.icns"
plutil -p "$PL" > "$W/Info.plist.txt"

# ---- 3. Mach-O files
want_archs() { case "$TAG" in macos-universal) echo "arm64 x86_64" ;; macos-arm64) echo arm64 ;; *) echo x86_64 ;; esac; }
WANT=$(want_archs)
NM=0; BADA=0; BADL=0
while IFS= read -r f; do
    file -b "$f" | grep -q 'Mach-O' || continue
    NM=$((NM + 1))
    a=$(lipo -archs "$f" 2>/dev/null | tr ' ' '\n' | sort | tr '\n' ' ' | sed 's/ $//')
    [ "$a" = "$WANT" ] || { BADA=$((BADA + 1)); info "archs of ${f#$P/}: $a"; }
    # (a universal file lists its links once per architecture, under "(architecture ...):" lines)
    otool -L "$f" | grep -E '^[[:space:]]' | awk '{print $1}' | while IFS= read -r l; do
        case "$l" in @rpath/*|@executable_path/*|@loader_path/*|/System/*|/usr/lib/*) ;; *) echo "$f -> $l" ;; esac
    done >> "$W/outside-links.txt"
done < <(find "$APP" -type f)
[ $BADA = 0 ] && ok "$NM Mach-O files, all $WANT" || bad "$BADA Mach-O files without the architectures $WANT"
if [ -s "$W/outside-links.txt" ]; then bad "links outside the bundle and the system: $(head -n 3 "$W/outside-links.txt" | tr '\n' ' ')"
else ok "every Mach-O file links only inside the bundle, /System and /usr/lib"; fi
grep -a -l -E '/Users/runner|toolchain/qt|/opt/homebrew|/usr/local/opt' $(find "$APP/Contents/Frameworks" "$APP/Contents/PlugIns" -type f -perm -u+x 2>/dev/null) 2>/dev/null | head -n 3 | sed 's/^/     note: build path string in /'
info "rpaths of the program: $(otool -l "$APP/Contents/MacOS/octomx" | grep -A2 LC_RPATH | grep path | awk '{print $2}' | tr '\n' ' ')"
for p in platforms/libqcocoa.dylib sqldrivers/libqsqlite.dylib; do
    [ -f "$APP/Contents/PlugIns/$p" ] && ok "plugin $p" || bad "plugin $p missing"
done
info "plugins: $(cd "$APP/Contents/PlugIns" && find . -name '*.dylib' | sed 's|^\./||' | sort | tr '\n' ' ')"
info "frameworks: $(cd "$APP/Contents/Frameworks" && ls | tr '\n' ' ')"
[ -f "$APP/Contents/Resources/qt.conf" ] && ok "qt.conf: $(tr '\n' ' ' < "$APP/Contents/Resources/qt.conf")" || info "no qt.conf"
for f in LICENSE.txt NOTICE.txt SOURCE.txt THIRD-PARTY-NOTICES.txt fonts/tahoma.ttf fonts/tahomabd.ttf licenses/LGPL-3.0.txt; do
    [ -s "$APP/Contents/Resources/$f" ] || bad "missing Contents/Resources/$f"
done
ok "licences and fonts in Contents/Resources ($(find "$APP/Contents/Resources/licenses" -type f | wc -l | tr -d ' ') licence files)"

# ---- 4. signature
codesign --verify --deep --strict --verbose=2 "$APP" > "$W/codesign-verify.txt" 2>&1 && ok "codesign --verify --deep --strict: $(tail -n 1 "$W/codesign-verify.txt")" \
    || bad "codesign --verify: $(tail -n 3 "$W/codesign-verify.txt" | tr '\n' ' ')"
codesign -dv "$APP" 2>&1 | grep -E '^(Identifier|Format|Signature|TeamIdentifier|Sealed Resources)' | sed 's/^/     /'
info "spctl: $(spctl --assess --type execute -vv "$APP" 2>&1 | tr '\n' ' ')"
xattr -lr "$APP" 2>/dev/null | grep -c quarantine | sed 's/^/     quarantine attributes after unpacking: /'

# ---- 5. the program's own checks
EXE=$APP/Contents/MacOS/octomx
grep -qa 'TEST HOOK: corrupting served bytes' "$EXE" && bad "the program contains the overlay/testCorruptUploads hook" || ok "no fault-injection hook"
grep -qa 'TEST HOOK: trusting an extra update key' "$EXE" && bad "the program can trust test update keys" || ok "no test update key hook"
env -i HOME="$W/home" PATH=/usr/bin:/bin "$EXE" --verify-manifest "$P" > "$W/verify-manifest.out" 2>&1
rc=$?
[ $rc = 0 ] && ok "octomx --verify-manifest: $(grep -m1 'MANIFEST.txt' "$W/verify-manifest.out" | cut -c1-160)" \
            || bad "octomx --verify-manifest rc=$rc: $(grep -i fail "$W/verify-manifest.out" | head -n 3 | tr '\n' ' ' | cut -c1-300)"
V=$(env -i HOME="$W/home" PATH=/usr/bin:/bin "$EXE" --version 2>/dev/null | head -n 1)
[ -n "$V" ] && ok "--version: $V" || bad "--version printed nothing"

# ---- 6. demo start with the real platform (Cocoa)
mkdir -p "$W/home" "$W/demo-profile"
run_limited 60 env -i HOME="$W/home" PATH=/usr/bin:/bin MX_OS_ROOT="$W/os-root" "$EXE" --profile "$W/demo-profile" --demo-frozen --no-wizard \
    --size 1024x689 --screenshot "$W/demo.png" > "$W/demo.out" 2>&1
rc=$?
[ $rc = 0 ] && [ -s "$W/demo.png" ] && ok "demo start with Cocoa: $W/demo.png ($(sips -g pixelWidth -g pixelHeight "$W/demo.png" 2>/dev/null | awk '/pixel/ {print $2}' | tr '\n' 'x' | sed 's/x$//'))" \
    || bad "demo start rc=$rc: $(tail -n 3 "$W/demo.out" | tr '\n' ' ')"
grep -E 'platform|font|scale' "$W/demo.out" | head -n 4 | sed 's/^/     /'
echo
[ $FAILS = 0 ] && echo "RESULT: macOS package verified ($NAME)" || echo "RESULT: $FAILS failure(s)"
[ $FAILS = 0 ]
