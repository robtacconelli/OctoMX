#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Configure a Linux -> Windows x86_64 cross build (llvm-mingw + Qt win64_llvm_mingw
# + static third_party/win). Uses Qt's own qt.toolchain.cmake, chain-loading
# cmake/toolchain-win64.cmake; host tools (moc/rcc/uic) come from QT_HOST_PATH.
#
#   tools/configure-win.sh [-S <source-dir>] <build-dir> [extra cmake args...]
#
# Afterwards: ninja -C <build-dir>   then   tools/package-win.sh <build-dir> <out-dir>
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../toolchain/env.sh"

SRC_DIR=$OCTOMX_ROOT
if [ "${1:-}" = "-S" ]; then SRC_DIR=$(realpath "$2"); shift 2; fi
[ $# -ge 1 ] || { echo "usage: $0 [-S <source-dir>] <build-dir> [cmake args...]" >&2; exit 2; }
BUILD_DIR=$1; shift

exec cmake -S "$SRC_DIR" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_TOOLCHAIN_FILE="$QT_WIN_DIR/lib/cmake/Qt6/qt.toolchain.cmake" \
  -DQT_CHAINLOAD_TOOLCHAIN_FILE="$OCTOMX_TOOLCHAIN_FILE" \
  -DQT_HOST_PATH="$QT_LINUX_DIR" \
  -DQT_WIN_DIR="$QT_WIN_DIR" \
  -DCMAKE_PREFIX_PATH="$QT_WIN_DIR;$TP_WIN" \
  -DOPENSSL_ROOT_DIR="$TP_WIN" -DOPENSSL_USE_STATIC_LIBS=ON \
  -DZLIB_ROOT="$TP_WIN" -DZLIB_USE_STATIC_LIBS=ON \
  -DCMAKE_POLICY_DEFAULT_CMP0167=NEW \
  -DQT_NO_QTPATHS_DEPLOYMENT_WARNING=ON \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  "$@"
