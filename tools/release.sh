#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# tools/release.sh: build, package and verify the OctoMX release in one command.
#
#   tools/release.sh [--reuse] [--no-verify] [--skip-linux] [--skip-win] [--xvnc N]
#                    [--out DIR] [--version X.Y.Z] [--src DIR] [--test-hooks]
#
#     --reuse       keep existing build/release-{linux,win} trees (incremental rebuild) instead of
#                   wiping and configuring them fresh (the default; use for iterating on scripts)
#     --no-verify   build + package only (skip the wine / env -i runs and the import/ldd checks)
#     --skip-linux / --skip-win   only do one platform
#     --xvnc N      X display number for the wine runs (default: first free of 91..99)
#     --out DIR     [upd-1] everything under DIR (DIR/linux, DIR/win build trees, DIR/log, DIR/dist)
#                   instead of build/release-{linux,win}, build/release and dist/ (test releases)
#     --version V   [upd-1] build with version V instead of project() (tools/release/version-override.cmake;
#                   the update end-to-end test makes its "newer" package B this way)
#     --test-hooks  [ren-1] packages WITH the test hooks (MX_TEST_HOOKS=ON), never signed or verified,
#                   only for tests/update_e2e/e2e.sh --throwaway-key (needs --out; NEVER publish them)
#     --src DIR     [upd-1] build from this copy of the source (tools/make-source-tree.sh), e.g. a
#                   snapshot taken while other people edit the checkout; packaging files come from here
#
# Produces (version from the top-level CMakeLists project() VERSION, major.minor):
#   dist/OctoMX-<v>-win64/            + dist/OctoMX-<v>-win64.zip
#   dist/OctoMX-<v>-linux-x86_64/     + dist/OctoMX-<v>-linux-x86_64.tar.xz
#   dist/SHA256SUMS                  (sha256 of the archives)
#   dist/SHA256SUMS.sig              [upd-1] Ed25519 signature of SHA256SUMS (tools/release/sign.sh) with
#                                    keys/release_ed25519.pem or $MX_RELEASE_KEY; without a key the
#                                    release is built UNSIGNED (said loudly) and is never self-installed
#   MANIFEST.txt in each package     [upd-1] sha256, size, path of every file (tools/release/manifest.sh)
#   build/release/symbols/           split debug info of octomx / octomx.exe (not shipped)
#   build/release/*.log              every step's log; build/release/summary.txt the evidence
# Exit status 0 only when every enabled build, package and verification step passed.
# Details: docs/RELEASE.md.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source "$ROOT/toolchain/env.sh"
cd "$ROOT"

REUSE=0; VERIFY=1; DO_LINUX=1; DO_WIN=1; XVNC=""; OUT=""; VOVERRIDE=""; SRC=$ROOT; HOOKS=OFF
while [ $# -gt 0 ]; do
  case "$1" in
    --reuse) REUSE=1 ;;
    --no-verify) VERIFY=0 ;;
    --skip-linux) DO_LINUX=0 ;;
    --skip-win) DO_WIN=0 ;;
    --xvnc) XVNC=$2; shift ;;
    --out) OUT=$(realpath -m "$2"); shift ;;
    --version) VOVERRIDE=$2; shift ;;
    --src) SRC=$(realpath "$2"); shift ;;
    --test-hooks) HOOKS=ON; VERIFY=0 ;;
    -h|--help) sed -n 2,30p "$0"; exit 0 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
  shift
done

[ $HOOKS = OFF ] || [ -n "$OUT" ] || { echo "--test-hooks needs --out (test packages never go to dist/)" >&2; exit 2; }
VERSION_FULL=$(sed -n 's/^project(OctoMX VERSION \([0-9.]*\).*/\1/p' "$SRC/CMakeLists.txt")
[ -n "$VERSION_FULL" ] || { echo "cannot read the project version from CMakeLists.txt" >&2; exit 1; }
VERSION_ARGS=()
if [ -n "$VOVERRIDE" ]; then
  [[ "$VOVERRIDE" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "--version wants x.y.z" >&2; exit 2; }
  VERSION_FULL=$VOVERRIDE
  VERSION_ARGS=(-DCMAKE_PROJECT_OctoMX_INCLUDE="$ROOT/tools/release/version-override.cmake" -DMX_VERSION_OVERRIDE="$VOVERRIDE")
fi
VERSION=$(cut -d. -f1-2 <<<"$VERSION_FULL")
export OCTOMX_RELEASE_VERSION=$VERSION
LOG=$ROOT/build/release
DIST=$ROOT/dist
BL=$ROOT/build/release-linux
BW=$ROOT/build/release-win
if [ -n "$OUT" ]; then LOG=$OUT/log; DIST=$OUT/dist; BL=$OUT/linux; BW=$OUT/win; fi
SYM=$LOG/symbols
WIN_NAME=OctoMX-$VERSION-win64
LIN_NAME=OctoMX-$VERSION-linux-x86_64
H=$ROOT/tools/release
mkdir -p "$LOG" "$SYM" "$DIST"
SUMMARY=$LOG/summary.txt
: > "$SUMMARY"
say() { echo "$*"; echo "$*" >> "$SUMMARY"; }
step() { echo; echo "=== $* ($(date +%T))"; }
fail() { say "FAILED: $*"; exit 1; }

say "OctoMX release $VERSION ($VERSION_FULL), $(date -u +%Y-%m-%dT%H:%MZ)$([ "$SRC" != "$ROOT" ] && echo ", source $SRC")"
# Optimised build with full debug info (split off below), no tests, no fault-injection hooks.
COMMON_ARGS=(-DCMAKE_BUILD_TYPE=Release
             "-DCMAKE_CXX_FLAGS_RELEASE=-O2 -g -DNDEBUG" "-DCMAKE_C_FLAGS_RELEASE=-O2 -g -DNDEBUG"
             -DMX_BUILD_TESTS=OFF -DMX_TEST_HOOKS=$HOOKS -DMX_UI_ONLY=OFF "-DMX_SKIP_MODULES=" "${VERSION_ARGS[@]}")
# a source copy has no third_party/src (not published): librats from the checkout
[ "$SRC" != "$ROOT" ] && COMMON_ARGS+=("-DMX_LIBRATS_DIR=$ROOT/third_party/src/librats")

build_tree() { # <configure script> <dir> <tag> [extra cmake args]
  local conf=$1 dir=$2 tag=$3; shift 3
  if [ $REUSE = 0 ]; then rm -rf "$dir"; fi
  "$conf" -S "$SRC" "$dir" "${COMMON_ARGS[@]}" "$@" > "$LOG/$tag.cmake.log" 2>&1 \
    || { tail -n 20 "$LOG/$tag.cmake.log"; fail "$tag configure (see $LOG/$tag.cmake.log)"; }
  ninja -C "$dir" -j8 octomx > "$LOG/$tag.build.log" 2>&1 \
    || { grep -m5 -E 'error|FAILED' "$LOG/$tag.build.log"; fail "$tag build (see $LOG/$tag.build.log)"; }
  grep -q "MX_TEST_HOOKS:BOOL=$HOOKS" "$dir/CMakeCache.txt" || fail "$tag: MX_TEST_HOOKS is not $HOOKS"
  say "$tag: built $(basename "$dir")/bin/octomx* ($(grep -c 'octomx: using plugin' "$LOG/$tag.cmake.log") network plugin modules)"
}

if [ $DO_LINUX = 1 ]; then
  step "Linux release build"
  # $ORIGIN rpath at link time (no patchelf on this host): bin/octomx finds ../lib/libQt6*.so.6
  build_tree tools/configure-linux.sh "$BL" linux \
    -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON '-DCMAKE_INSTALL_RPATH=$ORIGIN/../lib'
fi
if [ $DO_WIN = 1 ]; then
  step "Windows release build"
  build_tree tools/configure-win.sh "$BW" win
fi

if [ $DO_LINUX = 1 ]; then
  step "Linux package"
  "$H/pack-linux.sh" "$BL" "$DIST/$LIN_NAME" "$SYM" > "$LOG/pack-linux.log" 2>&1 \
    || { tail -n 20 "$LOG/pack-linux.log"; fail "Linux packaging (see $LOG/pack-linux.log)"; }
  "$H/notices.sh" linux "$DIST/$LIN_NAME" > "$LOG/notices-linux.log" 2>&1 || fail "Linux notices"
  "$H/manifest.sh" "$DIST/$LIN_NAME" "$VERSION_FULL" linux-x86_64 > "$LOG/manifest-linux.log" 2>&1 \
    || { cat "$LOG/manifest-linux.log"; fail "Linux manifest"; }
  say "linux: $(cat "$LOG/manifest-linux.log")"
  rm -f "$DIST/$LIN_NAME.tar.xz"
  tar -C "$DIST" --owner=0 --group=0 --sort=name -cf - "$LIN_NAME" | xz -T4 -9 > "$DIST/$LIN_NAME.tar.xz"
  say "linux: $LIN_NAME.tar.xz $(du -h "$DIST/$LIN_NAME.tar.xz" | cut -f1), $(find "$DIST/$LIN_NAME" -type f | wc -l) files, $(du -sh "$DIST/$LIN_NAME" | cut -f1) unpacked"
  grep -E '^(bundled|plugins|system)' "$LOG/pack-linux.log" | sed 's/^/  /' | tee -a "$SUMMARY"
fi
if [ $DO_WIN = 1 ]; then
  step "Windows package"
  "$H/pack-win.sh" "$BW" "$DIST/$WIN_NAME" "$SYM" > "$LOG/pack-win.log" 2>&1 \
    || { tail -n 20 "$LOG/pack-win.log"; fail "Windows packaging (see $LOG/pack-win.log)"; }
  "$H/notices.sh" win "$DIST/$WIN_NAME" > "$LOG/notices-win.log" 2>&1 || fail "Windows notices"
  "$H/manifest.sh" "$DIST/$WIN_NAME" "$VERSION_FULL" win64 > "$LOG/manifest-win.log" 2>&1 \
    || { cat "$LOG/manifest-win.log"; fail "Windows manifest"; }
  say "win: $(cat "$LOG/manifest-win.log")"
  rm -f "$DIST/$WIN_NAME.zip"
  (cd "$DIST" && 7z a -tzip -mx=9 "$WIN_NAME.zip" "$WIN_NAME" > "$LOG/zip-win.log")
  say "win: $WIN_NAME.zip $(du -h "$DIST/$WIN_NAME.zip" | cut -f1), $(find "$DIST/$WIN_NAME" -type f | wc -l) files, $(du -sh "$DIST/$WIN_NAME" | cut -f1) unpacked"
  grep -E '^(shipped|plugins|system)' "$LOG/pack-win.log" | sed 's/^/  /' | tee -a "$SUMMARY"
fi

step "SHA256SUMS"
# [mac-1] the macOS package of the same version, fetched before from CI (tools/release/fetch-mac.sh)
(cd "$DIST" && rm -f SHA256SUMS && for f in "$WIN_NAME.zip" "$LIN_NAME.tar.xz" "${WIN_NAME%-win64}"-macos-*.zip; do
   [ -f "$f" ] && sha256sum "$f"; done > SHA256SUMS)
sed 's/^/  /' "$DIST/SHA256SUMS" | tee -a "$SUMMARY"

step "SHA256SUMS.sig (release signature)"
UNSIGNED=0
rm -f "$DIST/SHA256SUMS.sig"
if [ $HOOKS = ON ]; then
  UNSIGNED=1
  say "TEST PACKAGES (MX_TEST_HOOKS=ON): not signed; tests/update_e2e/e2e.sh --throwaway-key signs a copy. NEVER PUBLISH."
elif "$H/sign.sh" "$DIST/SHA256SUMS" > "$LOG/sign.log" 2>&1; then
  say "signature: $(tail -n1 "$LOG/sign.log")"
  grep -q 'is NOT in src/api/update_keys.cpp' "$LOG/sign.log" && fail "the signing key is not the one built into the program (see $LOG/sign.log)"
else
  rc=$?
  if [ $rc = 3 ]; then
    UNSIGNED=1
    say "################################################################################"
    say "WARNING: NO RELEASE KEY ($(cat "$LOG/sign.log")): SHA256SUMS IS NOT SIGNED."
    say "WARNING: OctoMX will NOT install this release by itself; users get the download page."
    say "################################################################################"
  else
    cat "$LOG/sign.log"; fail "signing SHA256SUMS (see $LOG/sign.log)"
  fi
fi

if [ $VERIFY = 1 ]; then
  if [ $DO_LINUX = 1 ]; then
    step "Linux verification (env -i, ldd, real core)"
    if "$H/verify-linux.sh" "$DIST/$LIN_NAME.tar.xz" "$LOG/verify-linux" > "$LOG/verify-linux.log" 2>&1; then
      grep -E '^(OK|INFO|WARN)' "$LOG/verify-linux.log" | sed 's/^/  linux /' | tee -a "$SUMMARY"
    else
      grep -E '^(OK|INFO|WARN|FAIL)' "$LOG/verify-linux.log" | sed 's/^/  linux /' | tee -a "$SUMMARY"
      fail "Linux verification (see $LOG/verify-linux.log)"
    fi
  fi
  if [ $DO_WIN = 1 ]; then
    step "Windows verification (wine, clean PATH, imports)"
    if "$H/verify-win.sh" "$DIST/$WIN_NAME.zip" "$LOG/verify-win" ${XVNC:+--xvnc "$XVNC"} > "$LOG/verify-win.log" 2>&1; then
      grep -E '^(OK|INFO|WARN)' "$LOG/verify-win.log" | sed 's/^/  win /' | tee -a "$SUMMARY"
    else
      grep -E '^(OK|INFO|WARN|FAIL)' "$LOG/verify-win.log" | sed 's/^/  win /' | tee -a "$SUMMARY"
      fail "Windows verification (see $LOG/verify-win.log)"
    fi
  fi
fi
if [ $HOOKS = ON ]; then say "TEST PACKAGES OK (with test hooks, not a release), $DIST (summary: $SUMMARY)"
elif [ $UNSIGNED = 1 ]; then say "RELEASE OK (UNSIGNED: no release key), $DIST (summary: $SUMMARY)"
else say "RELEASE OK, $DIST (summary: $SUMMARY)"; fi
