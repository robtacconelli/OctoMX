#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# tools/mac/build.sh: configure and build OctoMX on a Mac (universal arm64 + x86_64 by default).
#
#   tools/mac/build.sh <build-dir> [--release] [extra cmake args...]
#
# --release: the package build (Release -O2 -g, no tests, no test hooks; only the program).
# Needs tools/mac/deps.sh first. Uses ccache when it is on PATH. The build log is
# <build-dir>.build.log; new warnings from our own code fail the build when MX_WERROR=1
# (warnings of third-party code, librats is compiled with -w, do not count).
set -euo pipefail
source "$(cd "$(dirname "$0")" && pwd)/env.sh"
[ $# -ge 1 ] || { echo "usage: $0 <build-dir> [cmake args...]" >&2; exit 2; }
B=$1; shift
mkdir -p "$B"; B=$(cd "$B" && pwd)
TARGETS=()
if [ "${1:-}" = --release ]; then
    # what tools/release.sh does for the other platforms: no tests, no test hooks, -O2 -g
    shift
    set -- -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_FLAGS_RELEASE=-O2 -g -DNDEBUG" "-DCMAKE_CXX_FLAGS_RELEASE=-O2 -g -DNDEBUG" \
           -DMX_BUILD_TESTS=OFF -DMX_TEST_HOOKS=OFF "$@"
    TARGETS=(octomx)
fi
# the oldest macOS the result can run on is the one the Qt frameworks were built for (their
# LC_BUILD_VERSION minos, the highest of the architectures); deps.sh may have used an older one
QTCORE=$QT_MAC_DIR/lib/QtCore.framework/QtCore
if [ -f "$QTCORE" ]; then
    for a in $(echo "$MX_MAC_ARCHS" | tr ';' ' '); do
        m=$(otool -arch "$a" -l "$QTCORE" 2>/dev/null | awk '/LC_BUILD_VERSION/ {f=1} f && $1 == "minos" {print $2; exit}')
        [ -n "$m" ] && [ "$(printf '%s\n%s\n' "$m" "$MACOSX_DEPLOYMENT_TARGET" | sort -t. -k1,1n -k2,2n | tail -n1)" = "$m" ] && MACOSX_DEPLOYMENT_TARGET=$m
    done
fi
export MACOSX_DEPLOYMENT_TARGET
echo "deployment target: macOS $MACOSX_DEPLOYMENT_TARGET (Qt's frameworks)"
LAUNCH=()
if command -v ccache > /dev/null; then
    LAUNCH=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
    ccache --zero-stats > /dev/null || true
fi
cmake -S "$OCTOMX_ROOT" -B "$B" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_PREFIX_PATH="$QT_MAC_DIR;$TP_MAC" \
    -DOPENSSL_ROOT_DIR="$TP_MAC" -DOPENSSL_USE_STATIC_LIBS=ON \
    -DZLIB_ROOT="$TP_MAC" -DZLIB_USE_STATIC_LIBS=ON \
    -DCMAKE_POLICY_DEFAULT_CMP0167=NEW \
    "-DCMAKE_OSX_ARCHITECTURES=$MX_MAC_ARCHS" \
    "-DCMAKE_EXE_LINKER_FLAGS=-Wl,-no_warn_duplicate_libraries" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET" \
    ${LAUNCH[@]+"${LAUNCH[@]}"} "$@" > "$B.cmake.log" 2>&1 || { grep -E -A5 'CMake (Error|Warning)' "$B.cmake.log" | head -n 60; exit 1; }
grep -E 'octomx:|OctoMX:' "$B.cmake.log" || true
set +e
ninja -C "$B" -j"$JOBS" -k 50 ${TARGETS[@]+"${TARGETS[@]}"} > "$B.build.log" 2>&1
rc=$?
set -e
if command -v ccache > /dev/null; then ccache --show-stats 2>/dev/null | grep -E -i 'hits|misses' | head -n 4 || true; fi
nwarn=$(grep -c 'warning:' "$B.build.log" || true)
nerr=$(grep -c 'error:' "$B.build.log" || true)
echo "build: exit $rc, $nerr error lines, $nwarn warning lines (log $B.build.log)"
if [ "$nerr" != 0 ]; then grep -E 'error:' "$B.build.log" | sort | uniq | head -n 80 | cut -c1-300; fi
if [ "$nwarn" != 0 ]; then grep -E 'warning:' "$B.build.log" | sort | uniq -c | sort -rn | head -n 60 | cut -c1-300; fi
[ $rc = 0 ] || { grep -E '^FAILED|ld: |Undefined symbols|referenced from' "$B.build.log" | head -n 40 | cut -c1-300; exit $rc; }
if [ "${MX_WERROR:-0}" = 1 ] && [ "$nwarn" != 0 ]; then echo "build: warnings are failures here (MX_WERROR=1)"; exit 1; fi
