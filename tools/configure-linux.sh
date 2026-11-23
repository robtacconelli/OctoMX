#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Configure a native Linux build (GCC + Qt linux_gcc_64 + static third_party/linux).
#
#   tools/configure-linux.sh [-S <source-dir>] <build-dir> [extra cmake args...]
#
# Source dir defaults to the repo root. Generator Ninja, CMAKE_BUILD_TYPE
# RelWithDebInfo (override with -DCMAKE_BUILD_TYPE=... in the extra args).
# Afterwards: ninja -C <build-dir>
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../toolchain/env.sh"

SRC_DIR=$OCTOMX_ROOT
if [ "${1:-}" = "-S" ]; then SRC_DIR=$(realpath "$2"); shift 2; fi
[ $# -ge 1 ] || { echo "usage: $0 [-S <source-dir>] <build-dir> [cmake args...]" >&2; exit 2; }
BUILD_DIR=$1; shift

exec cmake -S "$SRC_DIR" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PREFIX_PATH="$QT_LINUX_DIR;$TP_LINUX" \
  -DOPENSSL_ROOT_DIR="$TP_LINUX" -DOPENSSL_USE_STATIC_LIBS=ON \
  -DZLIB_ROOT="$TP_LINUX" -DZLIB_USE_STATIC_LIBS=ON \
  -DCMAKE_POLICY_DEFAULT_CMP0167=NEW \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  "$@"
