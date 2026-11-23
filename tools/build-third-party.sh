#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the static third-party libraries for OctoMX.
#
#   tools/build-third-party.sh [linux|win|all] [zlib openssl boost libtorrent ...]
#   tools/build-third-party.sh fetch      # only download + verify + unpack the sources (incl. librats)
#
# Defaults: target "all", every library. Sources are downloaded to
# third_party/src/dl, unpacked to third_party/src/<name>-<ver>, built out of tree
# in third_party/build/<target>/<name> (deleted after a successful install, set
# KEEP_BUILD=1 to keep) and installed into third_party/linux or third_party/win.
# Everything is static (no shared libs installed), PIC on Linux. Every download is checked against
# the SHA-256 pinned below; librats (built from source by src/net/overlay) is cloned at a pinned
# commit into third_party/src/librats.
set -euo pipefail

source "$(dirname "${BASH_SOURCE[0]}")/../toolchain/env.sh"
R=$OCTOMX_ROOT
JOBS=${JOBS:-8}

ZLIB_VER=1.3.2
OPENSSL_VER=3.5.9          # 3.5 = current OpenSSL LTS line
BOOST_VER=1.92.0
LIBTORRENT_VER=2.0.15
LIBRATS_URL=https://github.com/librats/librats.git
LIBRATS_COMMIT=9eefd0a9d9b7b0ccc3c692d6e8f80224723aed49   # = MX_LIBRATS_PIN in src/net/overlay/CMakeLists.txt
declare -A SHA256=(
  [zlib-1.3.2.tar.xz]=d7a0654783a4da529d1bb793b7ad9c3318020af77667bcae35f95d0e42a792f3
  [openssl-3.5.9.tar.gz]=603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a
  [boost-1.92.0-b2-nodocs.tar.xz]=ea7b982002cc9dfbe59b0b217b206f470dc75f3de0bb2973d844118934d82411
  [libtorrent-rasterbar-2.0.15.tar.gz]=5e2e79129823b7ea48721164c32b5aaf83d3fd733b5502100f6705b29f27bb02
)

SRC=$R/third_party/src
DL=$SRC/dl
mkdir -p "$DL"

fetch() { # url file
  local url=$1 out=$DL/$2
  if [ ! -s "$out" ]; then
    echo ">> download $url"
    curl -fL --retry 3 -o "$out.part" "$url" && mv "$out.part" "$out"
  fi
  local want=${SHA256[$2]:-}
  [ -n "$want" ] || { echo "no pinned SHA-256 for $2 (add it to SHA256 above)" >&2; exit 1; }
  echo "$want  $out" | sha256sum -c --quiet - || { echo "SHA-256 mismatch for $out" >&2; exit 1; }
}

unpack() { # archive dirname
  if [ ! -d "$SRC/$2" ]; then
    echo ">> unpack $1"
    tar -C "$SRC" -xf "$DL/$1"
  fi
}

prepare_sources() {
  fetch https://github.com/madler/zlib/releases/download/v$ZLIB_VER/zlib-$ZLIB_VER.tar.xz zlib-$ZLIB_VER.tar.xz
  fetch https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VER/openssl-$OPENSSL_VER.tar.gz openssl-$OPENSSL_VER.tar.gz
  fetch https://github.com/boostorg/boost/releases/download/boost-$BOOST_VER/boost-$BOOST_VER-b2-nodocs.tar.xz boost-$BOOST_VER-b2-nodocs.tar.xz
  fetch https://github.com/arvidn/libtorrent/releases/download/v$LIBTORRENT_VER/libtorrent-rasterbar-$LIBTORRENT_VER.tar.gz libtorrent-rasterbar-$LIBTORRENT_VER.tar.gz
  unpack zlib-$ZLIB_VER.tar.xz zlib-$ZLIB_VER
  unpack openssl-$OPENSSL_VER.tar.gz openssl-$OPENSSL_VER
  unpack boost-$BOOST_VER-b2-nodocs.tar.xz boost-$BOOST_VER
  unpack libtorrent-rasterbar-$LIBTORRENT_VER.tar.gz libtorrent-rasterbar-$LIBTORRENT_VER
  (cd "$DL" && sha256sum *.tar.* > SHA256SUMS)
  fetch_librats
}

fetch_librats() {
  local d=$SRC/librats
  if [ ! -d "$d/.git" ]; then
    echo ">> clone librats $LIBRATS_COMMIT"
    rm -rf "$d.part"; git init -q "$d.part"
    git -C "$d.part" fetch -q --depth 1 "$LIBRATS_URL" "$LIBRATS_COMMIT"
    git -C "$d.part" -c advice.detachedHead=false checkout -q FETCH_HEAD
    git -C "$d.part" remote add origin "$LIBRATS_URL"
    mv "$d.part" "$d"
  fi
  local head; head=$(git -C "$d" rev-parse HEAD)
  [ "$head" = "$LIBRATS_COMMIT" ] || echo "WARNING: $d is at $head, pinned commit is $LIBRATS_COMMIT" >&2
}

# --- per-target settings ------------------------------------------------------
setup_target() {
  T=$1
  B=$R/third_party/build/$T
  if [ "$T" = linux ]; then
    P=$TP_LINUX
    CMAKE_TC=()
    export CC=gcc CXX=g++
    export CFLAGS="-fPIC -O2" CXXFLAGS="-fPIC -O2"
  else
    P=$TP_WIN
    CMAKE_TC=(-DCMAKE_TOOLCHAIN_FILE=$R/cmake/toolchain-win64.cmake)
    unset CC CXX
    export CFLAGS="-O2" CXXFLAGS="-O2"
  fi
  mkdir -p "$P" "$B"
}

cmake_common() {
  echo -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$P -DCMAKE_INSTALL_LIBDIR=lib \
       -DCMAKE_PREFIX_PATH=$P -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON
}

done_build() { [ "${KEEP_BUILD:-0}" = 1 ] || rm -rf "$1"; }

build_zlib() {
  local bd=$B/zlib; rm -rf "$bd"
  cmake -S "$SRC/zlib-$ZLIB_VER" -B "$bd" $(cmake_common) "${CMAKE_TC[@]}" \
        -DZLIB_BUILD_SHARED=OFF -DZLIB_BUILD_STATIC=ON -DZLIB_BUILD_TESTING=OFF
  cmake --build "$bd" -j"$JOBS"
  cmake --install "$bd"
  # never leave shared variants around, FindZLIB must pick the static archive
  rm -f "$P"/lib/libz.so* "$P"/lib/libz.dll.a "$P"/bin/zlib*.dll "$P"/bin/libz*.dll
  # zlib 1.3.2 names the static lib libzs.a on Windows; CMake's FindZLIB looks for libz.a
  if [ -f "$P/lib/libzs.a" ]; then cp -f "$P/lib/libzs.a" "$P/lib/libz.a"; fi
  done_build "$bd"
}

build_openssl() {
  local bd=$B/openssl; rm -rf "$bd"; mkdir -p "$bd"
  local cfg=(no-shared no-tests no-docs no-module --prefix="$P" --libdir=lib)
  (
    cd "$bd"
    if [ "$T" = linux ]; then
      # --openssldir only sets the compiled-in default CA/config location;
      # install_sw below never writes there. /etc/ssl = Ubuntu system CA store.
      "$SRC/openssl-$OPENSSL_VER/Configure" linux-x86_64 "${cfg[@]}" -fPIC --openssldir=/etc/ssl
    else
      "$SRC/openssl-$OPENSSL_VER/Configure" mingw64 "${cfg[@]}" \
          --cross-compile-prefix="$LLVM_MINGW_ROOT/bin/x86_64-w64-mingw32-" CC=clang CXX=clang++ AR=llvm-ar RANLIB=llvm-ranlib RC=windres
    fi
    make -j"$JOBS" build_sw
    make install_sw
  )
  done_build "$bd"
}

build_boost() {
  # Header-only Boost is enough for libtorrent 2.0 (Boost.System is header-only
  # since 1.69). b2 install also generates BoostConfig.cmake + boost_headers config.
  local bd=$B/boost; rm -rf "$bd"; mkdir -p "$bd"
  local s=$SRC/boost-$BOOST_VER
  if [ ! -x "$s/b2" ]; then (cd "$s" && CC=gcc CXX=g++ ./bootstrap.sh --with-toolset=gcc); fi
  (cd "$s" && ./b2 -j"$JOBS" --build-dir="$bd" --prefix="$P" --with-headers install)
  done_build "$bd"
}

build_libtorrent() {
  local bd=$B/libtorrent; rm -rf "$bd"
  cmake -S "$SRC/libtorrent-rasterbar-$LIBTORRENT_VER" -B "$bd" $(cmake_common) "${CMAKE_TC[@]}" \
        -DCMAKE_CXX_STANDARD=17 \
        -DOPENSSL_ROOT_DIR=$P -DOPENSSL_USE_STATIC_LIBS=ON -DBoost_ROOT=$P \
        -Dencryption=ON -Ddht=ON -Dpython-bindings=OFF -Dbuild_examples=OFF \
        -Dbuild_tests=OFF -Dbuild_tools=OFF -Dstatic_runtime=OFF
  cmake --build "$bd" -j"$JOBS"
  cmake --install "$bd"
  # The exported target adds TORRENT_USE_ASSERTS for *consumer* Debug builds, which would
  # not match this Release library (ABI mismatch). Drop it so Debug apps link safely.
  sed -i 's/\\\$<\\\$<CONFIG:Debug>:TORRENT_USE_ASSERTS>;//' "$P/lib/cmake/LibtorrentRasterbar/LibtorrentRasterbarTargets.cmake"
  done_build "$bd"
}

# --- main ---------------------------------------------------------------------
TARGETS=${1:-all}; shift || true
if [ "$TARGETS" = fetch ]; then prepare_sources; echo "sources ready in $SRC"; exit 0; fi
LIBS=("$@"); [ ${#LIBS[@]} -gt 0 ] || LIBS=(zlib openssl boost libtorrent)
[ "$TARGETS" = all ] && TARGETS="linux win"

prepare_sources
for t in $TARGETS; do
  setup_target "$t"
  for l in "${LIBS[@]}"; do
    echo "================ $t: $l ================"
    "build_$l"
  done
done
rmdir "$R/third_party/build/"* "$R/third_party/build" 2>/dev/null || true
echo "third-party build finished: $TARGETS (${LIBS[*]})"
