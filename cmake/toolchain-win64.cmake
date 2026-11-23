# SPDX-License-Identifier: GPL-3.0-or-later
# CMake toolchain file: cross-compile from Linux to 64-bit Windows (x86_64, UCRT)
# with llvm-mingw (clang + lld + libc++), matching Qt's win64_llvm_mingw binaries.
#
# Usually you do not pass this file yourself: tools/configure-win.sh does it,
# together with CMAKE_PREFIX_PATH (Qt + third_party/win) and QT_HOST_PATH.
#
# Location of llvm-mingw: $LLVM_MINGW_ROOT if set, else <repo>/toolchain/llvm-mingw.
# Location of the Windows Qt: $QT_WIN_DIR if set (added to the find root path).

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

get_filename_component(_octomx_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

if(DEFINED ENV{LLVM_MINGW_ROOT} AND NOT "$ENV{LLVM_MINGW_ROOT}" STREQUAL "")
  set(LLVM_MINGW_ROOT "$ENV{LLVM_MINGW_ROOT}" CACHE PATH "llvm-mingw root")
else()
  set(LLVM_MINGW_ROOT "${_octomx_root}/toolchain/llvm-mingw" CACHE PATH "llvm-mingw root")
endif()

set(_triple x86_64-w64-mingw32)
set(CMAKE_C_COMPILER   "${LLVM_MINGW_ROOT}/bin/${_triple}-clang")
set(CMAKE_CXX_COMPILER "${LLVM_MINGW_ROOT}/bin/${_triple}-clang++")
set(CMAKE_RC_COMPILER  "${LLVM_MINGW_ROOT}/bin/${_triple}-windres")
set(CMAKE_AR           "${LLVM_MINGW_ROOT}/bin/llvm-ar"     CACHE FILEPATH "")
set(CMAKE_RANLIB       "${LLVM_MINGW_ROOT}/bin/llvm-ranlib" CACHE FILEPATH "")
set(CMAKE_OBJDUMP      "${LLVM_MINGW_ROOT}/bin/llvm-objdump" CACHE FILEPATH "")
set(CMAKE_STRIP        "${LLVM_MINGW_ROOT}/bin/llvm-strip"  CACHE FILEPATH "")

# Where target (Windows) headers/libs/packages live. Host programs are never
# searched there (moc/rcc/uic come from QT_HOST_PATH, i.e. the Linux Qt).
set(CMAKE_FIND_ROOT_PATH
    "${LLVM_MINGW_ROOT}/${_triple}"
    "${_octomx_root}/third_party/win")
if(DEFINED ENV{QT_WIN_DIR} AND NOT "$ENV{QT_WIN_DIR}" STREQUAL "")
  list(APPEND CMAKE_FIND_ROOT_PATH "$ENV{QT_WIN_DIR}")
endif()
if(DEFINED QT_WIN_DIR)
  list(APPEND CMAKE_FIND_ROOT_PATH "${QT_WIN_DIR}")
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Make try_compile() see the same settings.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES LLVM_MINGW_ROOT QT_WIN_DIR)
