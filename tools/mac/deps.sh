#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# tools/mac/deps.sh: install Qt and build the static third-party libraries on a Mac (CI or by hand).
#
#   tools/mac/deps.sh [qt] [zlib] [openssl] [boost] [libtorrent] [librats]    (default: all of them)
#
# The same versions and SHA-256 pins as the Linux and Windows builds: they are read from
# tools/build-third-party.sh (the one place that names them). Qt: the same version as the other
# platforms (QT_VERSION in toolchain/env.sh), installed with aqtinstall at the pinned commit of
# tools/install-toolchain.sh; aqtinstall checks every archive against Qt's repository metadata.
# Everything is built for every architecture in MX_MAC_ARCHS (default arm64 and x86_64) and the
# oldest macOS in MACOSX_DEPLOYMENT_TARGET (see tools/mac/env.sh). OpenSSL cannot build two
# architectures at once: it is built once per architecture and the archives are merged with lipo.
# Parts that are already installed are skipped. Needs the Xcode command line tools, python3, curl.
set -euo pipefail
source "$(cd "$(dirname "$0")" && pwd)/env.sh"
R=$OCTOMX_ROOT
BTP=$R/tools/build-third-party.sh
pin() { sed -n "s/^ *$1=\([^ ]*\).*/\1/p" "$BTP" | head -n1; }
sha() { sed -n "s/^ *\[$1\]=\([0-9a-f]*\).*/\1/p" "$BTP" | head -n1; }
ZLIB_VER=$(pin ZLIB_VER); OPENSSL_VER=$(pin OPENSSL_VER); BOOST_VER=$(pin BOOST_VER)
LIBTORRENT_VER=$(pin LIBTORRENT_VER); LIBRATS_URL=$(pin LIBRATS_URL); LIBRATS_COMMIT=$(pin LIBRATS_COMMIT)
AQT_COMMIT=$(sed -n 's/^AQT_COMMIT=\([0-9a-f]*\).*/\1/p' "$R/tools/install-toolchain.sh")
for v in ZLIB_VER OPENSSL_VER BOOST_VER LIBTORRENT_VER LIBRATS_COMMIT AQT_COMMIT; do
    [ -n "${!v}" ] || { echo "deps: cannot read $v" >&2; exit 1; }
done
ARCHS=$(echo "$MX_MAC_ARCHS" | tr ';' ' ')
SRC=$R/third_party/src
DL=$SRC/dl
BLD=$R/third_party/build/mac
P=$TP_MAC
mkdir -p "$DL" "$BLD" "$P"

say() { echo "== deps: $*"; }

fetch() { # url file
    local out=$DL/$2 want
    want=$(sha "$2")
    [ -n "$want" ] || { echo "no pinned SHA-256 for $2 in $BTP" >&2; exit 1; }
    [ -s "$out" ] || { say "download $1"; curl -fsSL --retry 3 -o "$out.part" "$1"; mv "$out.part" "$out"; }
    echo "$want  $out" | shasum -a 256 -c -s - || { echo "SHA-256 mismatch for $out" >&2; rm -f "$out"; exit 1; }
}
# (a source tree that prune_sources reduced to its licence files is unpacked again when needed)
unpack() {
    local d=$SRC/$2
    [ -e "$d/CMakeLists.txt" ] || [ -e "$d/Configure" ] || [ -e "$d/bootstrap.sh" ] && return 0
    say "unpack $1"; rm -rf "$d"; tar -C "$SRC" -xf "$DL/$1"
}

# after a complete build only the licence files of the sources are kept (tools/release/notices.sh
# copies them into the package), so the CI cache stays small
prune_sources() {
    local d
    for d in "zlib-$ZLIB_VER" "openssl-$OPENSSL_VER" "boost-$BOOST_VER" "libtorrent-rasterbar-$LIBTORRENT_VER"; do
        [ -d "$SRC/$d" ] || continue
        (cd "$SRC/$d" && find . -type f ! -path './LICENSE*' ! -path './COPYING*' ! -path './LICENSE_1_0.txt' -delete && find . -type d -empty -delete)
    done
    rm -rf "$DL" "$BLD"
    say "sources pruned to their licence files: $(cd "$SRC" && find . -maxdepth 2 -type f | grep -v '/librats/' | tr '\n' ' ')"
}

cmake_common() {
    echo -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$P -DCMAKE_INSTALL_LIBDIR=lib \
         -DCMAKE_PREFIX_PATH=$P -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
         -DCMAKE_OSX_DEPLOYMENT_TARGET=$MACOSX_DEPLOYMENT_TARGET
}

do_qt() {
    if [ -x "$QT_MAC_DIR/bin/qmake" ] || [ -x "$QT_MAC_DIR/bin/qtpaths" ]; then say "Qt $QT_VERSION already in $QT_MAC_DIR"; return; fi
    local venv=$R/toolchain/venv-mac
    if [ ! -x "$venv/bin/aqt" ]; then
        say "aqtinstall at $AQT_COMMIT"
        python3 -m venv "$venv"
        "$venv/bin/pip" install -q "git+https://github.com/miurahr/aqtinstall.git@$AQT_COMMIT"
    fi
    say "Qt $QT_VERSION for macOS (qtbase, qttools)"
    "$venv/bin/aqt" install-qt mac desktop "$QT_VERSION" clang_64 -O "$R/toolchain/qt" --archives qtbase qttools > "$BLD/aqt.log" 2>&1 \
        || { tail -n 30 "$BLD/aqt.log"; exit 1; }
    [ -d "$QT_MAC_DIR" ] || { ls "$R/toolchain/qt/$QT_VERSION"; echo "Qt not where expected: $QT_MAC_DIR" >&2; exit 1; }
    sed -n 's/^QMAKE_MACOSX_DEPLOYMENT_TARGET.*/Qt floor: &/p' "$QT_MAC_DIR/mkspecs/common/macx.conf"
    lipo -archs "$QT_MAC_DIR/lib/QtCore.framework/QtCore"
}

do_zlib() {
    [ -f "$P/lib/libz.a" ] && { say "zlib already built"; return; }
    fetch "https://github.com/madler/zlib/releases/download/v$ZLIB_VER/zlib-$ZLIB_VER.tar.xz" "zlib-$ZLIB_VER.tar.xz"
    unpack "zlib-$ZLIB_VER.tar.xz" "zlib-$ZLIB_VER"
    local bd=$BLD/zlib; rm -rf "$bd"
    # shellcheck disable=SC2046
    cmake -S "$SRC/zlib-$ZLIB_VER" -B "$bd" $(cmake_common) "-DCMAKE_OSX_ARCHITECTURES=$MX_MAC_ARCHS" \
          -DZLIB_BUILD_SHARED=OFF -DZLIB_BUILD_STATIC=ON -DZLIB_BUILD_TESTING=OFF > "$bd.log" 2>&1 || { tail -n 30 "$bd.log"; exit 1; }
    cmake --build "$bd" -j"$JOBS" >> "$bd.log" 2>&1 && cmake --install "$bd" >> "$bd.log" 2>&1 || { tail -n 30 "$bd.log"; exit 1; }
    rm -f "$P"/lib/libz*.dylib
    [ -f "$P/lib/libz.a" ] || { [ -f "$P/lib/libzs.a" ] && cp -f "$P/lib/libzs.a" "$P/lib/libz.a"; }
    rm -rf "$bd"
}

do_openssl() {
    [ -f "$P/lib/libcrypto.a" ] && { say "OpenSSL already built"; return; }
    fetch "https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VER/openssl-$OPENSSL_VER.tar.gz" "openssl-$OPENSSL_VER.tar.gz"
    unpack "openssl-$OPENSSL_VER.tar.gz" "openssl-$OPENSSL_VER"
    local a first=
    for a in $ARCHS; do
        local bd=$BLD/openssl-$a pre=$BLD/openssl-prefix-$a
        rm -rf "$bd" "$pre"; mkdir -p "$bd"
        say "OpenSSL $OPENSSL_VER for $a"
        # --openssldir: the compiled-in default CA location; /private/etc/ssl has macOS's cert.pem
        (cd "$bd" && "$SRC/openssl-$OPENSSL_VER/Configure" "darwin64-$a-cc" no-shared no-tests no-docs no-module \
            --prefix="$pre" --libdir=lib --openssldir=/private/etc/ssl "-mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET" \
            && make -j"$JOBS" build_sw && make install_sw) > "$bd.log" 2>&1 || { tail -n 40 "$bd.log"; exit 1; }
        [ -n "$first" ] || first=$a
    done
    mkdir -p "$P/lib" "$P/include"
    rm -rf "$P/include/openssl"; cp -R "$BLD/openssl-prefix-$first/include/openssl" "$P/include/"
    for lib in libcrypto.a libssl.a; do
        local ins=()
        for a in $ARCHS; do ins+=("$BLD/openssl-prefix-$a/lib/$lib"); done
        lipo -create "${ins[@]}" -output "$P/lib/$lib"
    done
    # headers that differ between the architectures (configuration.h) get a small switch
    for a in $ARCHS; do
        [ "$a" = "$first" ] && continue
        local h
        for h in $(cd "$BLD/openssl-prefix-$first/include/openssl" && ls); do
            if ! cmp -s "$BLD/openssl-prefix-$first/include/openssl/$h" "$BLD/openssl-prefix-$a/include/openssl/$h"; then
                say "OpenSSL header $h differs between $first and $a: per-architecture switch"
                local base=${h%.h}
                mkdir -p "$P/include/openssl/arch"
                cp "$BLD/openssl-prefix-$first/include/openssl/$h" "$P/include/openssl/arch/${base}_$first.h"
                cp "$BLD/openssl-prefix-$a/include/openssl/$h" "$P/include/openssl/arch/${base}_$a.h"
                { echo "/* merged by tools/mac/deps.sh for a universal build */"
                  echo "#if defined(__arm64__) || defined(__aarch64__)"
                  echo "#include \"arch/${base}_arm64.h\""
                  echo "#else"
                  echo "#include \"arch/${base}_x86_64.h\""
                  echo "#endif"; } > "$P/include/openssl/$h"
            fi
        done
    done
    # CMake's FindOpenSSL reads the version from opensslv.h; pkg-config files are not needed
    lipo -info "$P/lib/libcrypto.a"
    [ "${KEEP_BUILD:-0}" = 1 ] || rm -rf "$BLD"/openssl-*
}

do_boost() {
    [ -f "$P/include/boost/version.hpp" ] && { say "Boost already installed"; return; }
    fetch "https://github.com/boostorg/boost/releases/download/boost-$BOOST_VER/boost-$BOOST_VER-b2-nodocs.tar.xz" "boost-$BOOST_VER-b2-nodocs.tar.xz"
    unpack "boost-$BOOST_VER-b2-nodocs.tar.xz" "boost-$BOOST_VER"
    local s=$SRC/boost-$BOOST_VER bd=$BLD/boost
    rm -rf "$bd"; mkdir -p "$bd"
    say "Boost $BOOST_VER headers"
    ( cd "$s" && { [ -x ./b2 ] || ./bootstrap.sh --with-toolset=clang; } \
      && ./b2 -j"$JOBS" --build-dir="$bd" --prefix="$P" --with-headers install ) > "$bd.log" 2>&1 || { tail -n 30 "$bd.log"; exit 1; }
    rm -rf "$bd"
}

do_libtorrent() {
    [ -f "$P/lib/libtorrent-rasterbar.a" ] && { say "libtorrent already built"; return; }
    fetch "https://github.com/arvidn/libtorrent/releases/download/v$LIBTORRENT_VER/libtorrent-rasterbar-$LIBTORRENT_VER.tar.gz" "libtorrent-rasterbar-$LIBTORRENT_VER.tar.gz"
    unpack "libtorrent-rasterbar-$LIBTORRENT_VER.tar.gz" "libtorrent-rasterbar-$LIBTORRENT_VER"
    local bd=$BLD/libtorrent; rm -rf "$bd"
    say "libtorrent $LIBTORRENT_VER"
    # shellcheck disable=SC2046
    cmake -S "$SRC/libtorrent-rasterbar-$LIBTORRENT_VER" -B "$bd" $(cmake_common) "-DCMAKE_OSX_ARCHITECTURES=$MX_MAC_ARCHS" \
          -DCMAKE_CXX_STANDARD=17 -DCMAKE_POLICY_DEFAULT_CMP0167=NEW \
          -DOPENSSL_ROOT_DIR=$P -DOPENSSL_USE_STATIC_LIBS=ON -DBoost_ROOT=$P \
          -Dencryption=ON -Ddht=ON -Dpython-bindings=OFF -Dbuild_examples=OFF \
          -Dbuild_tests=OFF -Dbuild_tools=OFF -Dstatic_runtime=OFF > "$bd.log" 2>&1 || { tail -n 40 "$bd.log"; exit 1; }
    cmake --build "$bd" -j"$JOBS" >> "$bd.log" 2>&1 && cmake --install "$bd" >> "$bd.log" 2>&1 || { grep -m5 -E 'error' "$bd.log"; tail -n 20 "$bd.log"; exit 1; }
    # the same edit as tools/build-third-party.sh: no TORRENT_USE_ASSERTS for consumer Debug builds
    sed -i '' 's/\\\$<\\\$<CONFIG:Debug>:TORRENT_USE_ASSERTS>;//' "$P/lib/cmake/LibtorrentRasterbar/LibtorrentRasterbarTargets.cmake"
    lipo -info "$P/lib/libtorrent-rasterbar.a"
    rm -rf "$bd"
}

do_librats() {
    local d=$SRC/librats
    if [ ! -d "$d/.git" ]; then
        say "librats $LIBRATS_COMMIT"
        rm -rf "$d.part"; git init -q "$d.part"
        git -C "$d.part" fetch -q --depth 1 "$LIBRATS_URL" "$LIBRATS_COMMIT"
        git -C "$d.part" -c advice.detachedHead=false checkout -q FETCH_HEAD
        mv "$d.part" "$d"
    fi
    [ "$(git -C "$d" rev-parse HEAD)" = "$LIBRATS_COMMIT" ] || { echo "librats is not at the pinned commit" >&2; exit 1; }
}

PARTS=("$@"); [ ${#PARTS[@]} -gt 0 ] || PARTS=(qt zlib openssl boost libtorrent librats)
say "macOS $(sw_vers -productVersion) $(uname -m), deployment target $MACOSX_DEPLOYMENT_TARGET, archs $ARCHS, $(xcrun clang --version | head -n1)"
for p in "${PARTS[@]}"; do
    "do_$p"
    # the libraries are built for the deployment target that Qt (just installed) states
    if [ "$p" = qt ]; then unset MACOSX_DEPLOYMENT_TARGET; source "$R/tools/mac/env.sh"; say "deployment target $MACOSX_DEPLOYMENT_TARGET"; fi
done
[ "${MX_KEEP_SOURCES:-0}" = 1 ] || [ ${#PARTS[@]} -lt 6 ] || prune_sources
say "done: $(ls "$P/lib" | tr '\n' ' ')"
