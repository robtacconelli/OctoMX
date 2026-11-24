#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# tools/mac/package.sh: make the macOS package from a release build tree.
#
#   tools/mac/package.sh <release-build-dir> <out-dir>
#
# The build tree must be configured like tools/release.sh does it for the other platforms
# (Release with -O2 -g, MX_BUILD_TESTS=OFF, MX_TEST_HOOKS=OFF): tools/mac/build.sh <dir> --release
# does that. Result in <out-dir>:
#   OctoMX-<X.Y>-<tag>/                 tag = macos-universal (or macos-arm64 / macos-x86_64)
#     OctoMX.app                        macdeployqt'ed (Qt frameworks + the plugins we use), our fonts
#                                       and the licence files in Contents/Resources, ad-hoc signed
#     README-mac.txt                    first start, where the profile is, how to remove it
#     MANIFEST.txt                      sha256, size, path of every file (written after signing; the
#                                       bundle's symbolic links as "# link" lines)
#   OctoMX-<X.Y>-<tag>.zip              made with ditto (keeps the signature and the links)
#   octomx.dSYM.zip                     debug symbols (not shipped; keep them for crash reports)
#   package-info.txt                    archs, minimum macOS, codesign / spctl output
#
# Signing: ad-hoc (codesign -s -) unless MX_MAC_SIGN_IDENTITY names a Developer ID certificate in
# the keychain (then with the hardened runtime and a timestamp); notarization runs only when
# MX_MAC_NOTARY_PROFILE names a notarytool keychain profile. Both are off in CI (no certificates).
set -euo pipefail
source "$(cd "$(dirname "$0")" && pwd)/env.sh"
[ $# -eq 2 ] || { echo "usage: $0 <release-build-dir> <out-dir>" >&2; exit 2; }
B=$(cd "$1" && pwd); mkdir -p "$2"; OUT=$(cd "$2" && pwd)
R=$OCTOMX_ROOT
cache() { sed -n "s/^$1:[A-Z]*=//p" "$B/CMakeCache.txt" | head -n1; }
[ "$(cache MX_TEST_HOOKS)" = OFF ] || { echo "package: $B has MX_TEST_HOOKS on: not a release tree" >&2; exit 1; }
VFULL=$(sed -n 's/^project(OctoMX VERSION \([0-9.]*\).*/\1/p' "$R/CMakeLists.txt")
[ -n "$(cache MX_VERSION_OVERRIDE)" ] && VFULL=$(cache MX_VERSION_OVERRIDE)
VS=$(echo "$VFULL" | cut -d. -f1-2)
APP_SRC=$B/bin/octomx.app
[ -x "$APP_SRC/Contents/MacOS/octomx" ] || { echo "package: no $APP_SRC" >&2; exit 1; }
ARCHS=$(lipo -archs "$APP_SRC/Contents/MacOS/octomx")
case "$ARCHS" in *x86_64*arm64*|*arm64*x86_64*) TAG=macos-universal ;; *) TAG=macos-$ARCHS ;; esac
MACOS_MIN=$(/usr/libexec/PlistBuddy -c 'Print :LSMinimumSystemVersion' "$APP_SRC/Contents/Info.plist")
NAME=OctoMX-$VS-$TAG
DEST=$OUT/$NAME
APP=$DEST/OctoMX.app
INFO=$OUT/package-info.txt
rm -rf "$DEST" "$OUT/$NAME.zip" "$OUT/octomx.dSYM.zip"
mkdir -p "$DEST"
echo "== package $NAME (archs: $ARCHS, minimum macOS $MACOS_MIN)"

# 1. the bundle and its Qt (macdeployqt copies the frameworks and the plugins of the linked modules)
ditto "$APP_SRC" "$APP"
dsymutil "$APP/Contents/MacOS/octomx" -o "$OUT/octomx.dSYM" > /dev/null 2>&1 || true
strip -S -x "$APP/Contents/MacOS/octomx"
"$QT_MAC_DIR/bin/macdeployqt" "$APP" -verbose=1 > "$OUT/macdeployqt.log" 2>&1 || { tail -n 30 "$OUT/macdeployqt.log"; exit 1; }
grep -i -E 'error|warning' "$OUT/macdeployqt.log" | head -n 20 || true
# only the plugins OctoMX uses (as on Windows and Linux): the Cocoa platform, TLS through the
# system (Secure Transport) and the certificate-only backend, SQLite, the ico reader/writer
PL=$APP/Contents/PlugIns
KEEP="platforms/libqcocoa.dylib tls/libqsecuretransportbackend.dylib tls/libqcertonlybackend.dylib sqldrivers/libqsqlite.dylib imageformats/libqico.dylib"
if [ -d "$PL" ]; then
    (cd "$PL" && find . -type f -name '*.dylib' | sed 's|^\./||') | while IFS= read -r p; do
        case " $KEEP " in *" $p "*) ;; *) rm -f "$PL/$p" ;; esac
    done
    find "$PL" -type d -empty -delete
fi
for p in $KEEP; do
    case "$p" in tls/libqsecuretransportbackend.dylib) [ -f "$PL/$p" ] || echo "note: $p not in this Qt build" ;;
                 *) [ -f "$PL/$p" ] || { echo "package: plugin $p missing" >&2; exit 1; } ;; esac
done
# frameworks nothing links any more (otool walk from the program and the kept plugins)
FW=$APP/Contents/Frameworks
need=$(for f in "$APP/Contents/MacOS/octomx" $(find "$PL" -name '*.dylib'); do otool -L "$f"; done | grep -o '@rpath/[A-Za-z0-9_]*\.framework' | sort -u | sed 's|@rpath/||')
grow=1
while [ $grow = 1 ]; do
    grow=0
    for f in $need; do
        n=${f%.framework}
        for d in $(otool -L "$FW/$f/Versions/A/$n" 2>/dev/null | grep -o '@rpath/[A-Za-z0-9_]*\.framework' | sed 's|@rpath/||' | sort -u); do
            case " $need " in *" $d "*) ;; *) need="$need $d"; grow=1 ;; esac
        done
    done
done
for f in $(cd "$FW" && ls -d *.framework 2>/dev/null); do
    case " $(echo $need) " in *" $f "*) ;; *) echo "drop unused $f"; rm -rf "${FW:?}/$f" ;; esac
done
# headers inside frameworks are not needed at run time
find "$FW" -name Headers -prune -exec rm -rf {} + 2>/dev/null || true

# 2. fonts, licences, notices, README
RES=$APP/Contents/Resources
mkdir -p "$RES/fonts"
# the macOS copies of the UI font (the same as compiled into the program, see patch_tahoma.py --mac-advances)
cp "$R/src/ui/res/fonts/tahoma-mac.ttf" "$RES/fonts/tahoma.ttf"
cp "$R/src/ui/res/fonts/tahomabd-mac.ttf" "$RES/fonts/tahomabd.ttf"
OCTOMX_RELEASE_VERSION=$VS MX_MACOS_MIN=$MACOS_MIN bash "$R/tools/release/notices.sh" mac "$RES"
cp "$RES/README.txt" "$DEST/README-mac.txt"

# 3. signature (the last change to the bundle)
if [ -n "${MX_MAC_SIGN_IDENTITY:-}" ]; then
    codesign --force --deep --options runtime --timestamp --sign "$MX_MAC_SIGN_IDENTITY" "$APP"
else
    codesign --force --deep --sign - "$APP"
fi
{
    echo "package: $NAME"
    echo "version: $VFULL"
    echo "architectures: $ARCHS"
    echo "minimum macOS: $(/usr/libexec/PlistBuddy -c 'Print :LSMinimumSystemVersion' "$APP/Contents/Info.plist")"
    echo "bundle id: $(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$APP/Contents/Info.plist")"
    echo "built on: macOS $(sw_vers -productVersion) $(uname -m), $(xcodebuild -version 2>/dev/null | head -n1)"
    echo "frameworks: $(cd "$FW" && ls -d *.framework | tr '\n' ' ')"
    echo "plugins: $(cd "$PL" && find . -name '*.dylib' | sed 's|^\./||' | sort | tr '\n' ' ')"
    echo "--- codesign --verify --deep --strict --verbose=2"
    codesign --verify --deep --strict --verbose=2 "$APP" 2>&1 | grep -v -E '^--(prepared|validated):' 
    echo "--- codesign -dvv"
    codesign -dvv "$APP" 2>&1 | grep -v -E '^(Executable|CDHash|Hash type|CandidateCDHash)' || true
    echo "--- spctl --assess --type execute -vv (Gatekeeper; an ad-hoc signature is expected to be rejected: first start = right-click > Open)"
    spctl --assess --type execute -vv "$APP" 2>&1 || true
} > "$INFO"
cat "$INFO"
codesign --verify --deep --strict "$APP"
if [ -n "${MX_MAC_NOTARY_PROFILE:-}" ] && [ -n "${MX_MAC_SIGN_IDENTITY:-}" ]; then
    ditto -c -k --keepParent "$APP" "$OUT/notarize.zip"
    xcrun notarytool submit "$OUT/notarize.zip" --keychain-profile "$MX_MAC_NOTARY_PROFILE" --wait
    xcrun stapler staple "$APP"
    rm -f "$OUT/notarize.zip"
fi

# 4. manifest (after signing: it lists the signed files), archive
bash "$R/tools/mac/manifest.sh" "$DEST" "$VFULL" "$TAG"
(cd "$OUT" && ditto -c -k --sequesterRsrc --keepParent "$NAME" "$NAME.zip")
[ -d "$OUT/octomx.dSYM" ] && (cd "$OUT" && ditto -c -k --keepParent octomx.dSYM octomx.dSYM.zip && rm -rf octomx.dSYM)
echo "archive: $OUT/$NAME.zip ($(du -h "$OUT/$NAME.zip" | cut -f1)), unpacked $(du -sh "$DEST" | cut -f1)"
shasum -a 256 "$OUT/$NAME.zip"
