# SPDX-License-Identifier: GPL-3.0-or-later
# Source this in EVERY shell before doing anything:  source <checkout>/toolchain/env.sh  (bash)
# Keeps every toolchain, cache and temporary file inside the checkout (nothing in / or $HOME).
# OCTOMX_ROOT = the checkout this file lives in (derived from this file's own location).
export OCTOMX_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)
export TMPDIR=$OCTOMX_ROOT/toolchain/tmp
export XDG_CACHE_HOME=$OCTOMX_ROOT/toolchain/cache
export PIP_CACHE_DIR=$OCTOMX_ROOT/toolchain/cache/pip
export CCACHE_DIR=$OCTOMX_ROOT/toolchain/cache/ccache
export WINEPREFIX=$OCTOMX_ROOT/toolchain/wineprefix
# Development shells must never register file/link handlers or autostart entries on the developer's
# own desktop: redirect the app's OS integration (XDG files / registry layout) into the build tree.
export MX_OS_ROOT=$OCTOMX_ROOT/build/os-root
export WINEDEBUG=-all
export npm_config_cache=$OCTOMX_ROOT/toolchain/cache/npm
# --- sections below are appended by the toolchain task (PATH for llvm-mingw, Qt dirs, etc.) ---

# ===== toolchain task (do not edit by hand; see docs/status/toolchain.md) =====
mkdir -p "$TMPDIR" "$XDG_CACHE_HOME" 2>/dev/null
__octomx_path_add() { case ":$PATH:" in *":$1:"*) ;; *) PATH="$PATH:$1" ;; esac; }
__octomx_path_prepend() { local p=":$PATH:"; p="${p//:$1:/:}"; p="${p#:}"; p="${p%:}"; PATH="$1${p:+:$p}"; }
__octomx_ld_add() { case ":${LD_LIBRARY_PATH:-}:" in *":$1:"*) ;; *) LD_LIBRARY_PATH="${LD_LIBRARY_PATH:+$LD_LIBRARY_PATH:}$1" ;; esac; }

export QT_VERSION=6.12.1
export QT_LINUX_DIR=$OCTOMX_ROOT/toolchain/qt/$QT_VERSION/gcc_64          # native Linux Qt (also QT_HOST_PATH)
export QT_WIN_DIR=$OCTOMX_ROOT/toolchain/qt/$QT_VERSION/llvm-mingw_64     # Windows Qt (win64_llvm_mingw)
export QT_HOST_PATH=$QT_LINUX_DIR
export LLVM_MINGW_ROOT=$OCTOMX_ROOT/toolchain/llvm-mingw                 # llvm-mingw 20260602 (LLVM 22.1.7, UCRT)
export TP_LINUX=$OCTOMX_ROOT/third_party/linux                          # static zlib/openssl/boost/libtorrent (Linux, PIC)
export TP_WIN=$OCTOMX_ROOT/third_party/win                              # same, for Windows x86_64
export OCTOMX_TOOLCHAIN_FILE=$OCTOMX_ROOT/cmake/toolchain-win64.cmake
export OCTOMX_VENV=$OCTOMX_ROOT/toolchain/venv                           # python venv: aqtinstall, Pillow

# llvm-mingw is PREPENDED: the host also has Ubuntu's mingw-w64 GCC 13 (msvcrt) in /usr/bin whose
# x86_64-w64-mingw32-* names would otherwise win. Only x86_64-w64-mingw32-* names collide, so
# native Linux builds (gcc/g++/ld/ar) are unaffected.
__octomx_path_prepend "$LLVM_MINGW_ROOT/bin"
__octomx_path_add "$QT_LINUX_DIR/bin"
__octomx_path_add "$OCTOMX_ROOT/tools"
export PATH
# Compiler cache (optional): toolchain/ccache/ccache, one cache for the checkout and its worktrees
# (CCACHE_DIR above). CMake picks the launcher up when a build directory is first configured.
if [ -x "$OCTOMX_ROOT/toolchain/ccache/ccache" ]; then
    __octomx_path_add "$OCTOMX_ROOT/toolchain/ccache"
    export PATH
    export CMAKE_C_COMPILER_LAUNCHER=ccache CMAKE_CXX_COMPILER_LAUNCHER=ccache
fi

# extra runtime libs fetched with apt download + dpkg -x (only if any were needed)
if [ -d "$OCTOMX_ROOT/toolchain/sysroot-extra/usr/lib/x86_64-linux-gnu" ]; then
  __octomx_ld_add "$OCTOMX_ROOT/toolchain/sysroot-extra/usr/lib/x86_64-linux-gnu"
  export LD_LIBRARY_PATH
fi

# wine: never pop up Mono/Gecko installers, never write menu entries into $HOME
: "${WINEDLLOVERRIDES:=mscoree,mshtml=;winemenubuilder.exe=d}"
export WINEDLLOVERRIDES
export WINEARCH=win64
# tools using ~/.local/share (aqtinstall temp dir, etc.) must not grow $HOME either
export XDG_DATA_HOME=$OCTOMX_ROOT/toolchain/xdg-data
mkdir -p "$XDG_DATA_HOME" 2>/dev/null
unset -f __octomx_path_add __octomx_path_prepend __octomx_ld_add
# ===== end toolchain task =====
