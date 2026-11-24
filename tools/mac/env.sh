# SPDX-License-Identifier: GPL-3.0-or-later
# tools/mac/env.sh: the build environment on a Mac (sourced by every tools/mac script).
#   source tools/mac/env.sh
# Sets everything toolchain/env.sh sets (TMPDIR, caches, MX_OS_ROOT ... under the checkout) plus:
#   QT_MAC_DIR    Qt for macOS (aqtinstall, universal arm64 + x86_64) in toolchain/qt/<ver>/macos
#   TP_MAC        static zlib / OpenSSL / Boost headers / libtorrent (universal) in third_party/mac
#   MX_MAC_ARCHS  the architectures of the build (CMake list)
#   MACOSX_DEPLOYMENT_TARGET  the oldest macOS the result runs on: Qt's own floor (read from its mkspecs)
# Written for the bash 3.2 that macOS ships (no associative arrays).
# shellcheck disable=SC1091
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/toolchain/env.sh"
unset QT_HOST_PATH QT_LINUX_DIR QT_WIN_DIR CMAKE_C_COMPILER_LAUNCHER CMAKE_CXX_COMPILER_LAUNCHER
export QT_MAC_DIR=$OCTOMX_ROOT/toolchain/qt/$QT_VERSION/macos
export TP_MAC=$OCTOMX_ROOT/third_party/mac
export MX_MAC_ARCHS=${MX_MAC_ARCHS:-arm64;x86_64}
export CCACHE_DIR=${CCACHE_DIR:-$OCTOMX_ROOT/toolchain/cache/ccache}
export JOBS=${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}
# CMake 4 refuses projects that declare a minimum older than 3.5 (some third-party code does)
export CMAKE_POLICY_VERSION_MINIMUM=3.5
if [ -z "${MACOSX_DEPLOYMENT_TARGET:-}" ]; then
    _mx_min=
    if [ -f "$QT_MAC_DIR/mkspecs/common/macx.conf" ]; then
        _mx_min=$(sed -n 's/^QMAKE_MACOSX_DEPLOYMENT_TARGET *= *\([0-9.]*\).*/\1/p' "$QT_MAC_DIR/mkspecs/common/macx.conf" | head -n1)
    fi
    export MACOSX_DEPLOYMENT_TARGET=${_mx_min:-13.0}
    unset _mx_min
fi
case ":$PATH:" in *":$QT_MAC_DIR/bin:"*) ;; *) PATH="$PATH:$QT_MAC_DIR/bin" ;; esac
export PATH
