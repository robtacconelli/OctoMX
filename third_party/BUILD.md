# third_party: how the static dependencies are built

Everything here is produced by **`tools/build-third-party.sh`** (reproducible, no root).

```bash
source toolchain/env.sh   # from the checkout root
tools/build-third-party.sh                    # all libs, linux + win (~8 min with -j8)
tools/build-third-party.sh linux              # only Linux
tools/build-third-party.sh win openssl libtorrent   # only some libs for one target
KEEP_BUILD=1 tools/build-third-party.sh ...  # keep third_party/build/<target>/<lib>
```

## Layout

| path | content |
|---|---|
| `src/dl/` | downloaded release tarballs + `SHA256SUMS` |
| `src/<name>-<ver>/` | pristine unpacked sources (all builds are out-of-tree) |
| `build/<target>/<lib>/` | temporary build trees, deleted after install |
| `linux/` (`$TP_LINUX`) | Linux x86_64 prefix: static, `-fPIC`, built with system GCC 15 |
| `win/` (`$TP_WIN`) | Windows x86_64 prefix: static, built with llvm-mingw (clang 22.1.7, UCRT, libc++) |
| `fonts/` | Wine's LGPL Tahoma / MS Sans Serif / Small Fonts / Marlett (see fonts/README.md) |

Each prefix has `include/`, `lib/*.a`, `lib/cmake/{OpenSSL,zlib,Boost-1.92.0,boost_headers-1.92.0,LibtorrentRasterbar}`.
No shared libraries are installed. `bin/openssl[.exe]` is a by-product (handy for tests).

## Versions

| lib | version | source |
|---|---|---|
| zlib | 1.3.2 | https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.xz |
| OpenSSL | 3.5.9 (LTS line) | https://github.com/openssl/openssl/releases/download/openssl-3.5.9/openssl-3.5.9.tar.gz |
| Boost | 1.92.0, **headers only** | https://github.com/boostorg/boost/releases/download/boost-1.92.0/boost-1.92.0-b2-nodocs.tar.xz |
| libtorrent-rasterbar | 2.0.15 (latest 2.0.x) | https://github.com/arvidn/libtorrent/releases/download/v2.0.15/libtorrent-rasterbar-2.0.15.tar.gz |

## Exact build commands (what the script runs)

Common: `P=$TP_LINUX` or `$TP_WIN`; Linux uses `CC=gcc CXX=g++ CFLAGS/CXXFLAGS="-fPIC -O2"`,
Windows uses `-DCMAKE_TOOLCHAIN_FILE=$OCTOMX_ROOT/cmake/toolchain-win64.cmake` for CMake projects.
CMake common flags: `-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$P -DCMAKE_INSTALL_LIBDIR=lib
-DCMAKE_PREFIX_PATH=$P -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON`.

**zlib**
```
cmake -S src/zlib-1.3.2 -B build/<t>/zlib <common> [toolchain] -DZLIB_BUILD_SHARED=OFF -DZLIB_BUILD_STATIC=ON -DZLIB_BUILD_TESTING=OFF
cmake --build ... && cmake --install ...
# win: zlib names the archive libzs.a -> copied to libz.a so CMake's FindZLIB finds it
```

**OpenSSL** (out of tree, `make build_sw && make install_sw`, never touches openssldir)
```
# Linux
src/openssl-3.5.9/Configure linux-x86_64 no-shared no-tests no-docs no-module --prefix=$P --libdir=lib -fPIC --openssldir=/etc/ssl
# Windows
src/openssl-3.5.9/Configure mingw64 no-shared no-tests no-docs no-module --prefix=$P --libdir=lib \
    --cross-compile-prefix=$LLVM_MINGW_ROOT/bin/x86_64-w64-mingw32- CC=clang CXX=clang++ AR=llvm-ar RANLIB=llvm-ranlib RC=windres
```
`--openssldir=/etc/ssl` makes the Linux static libcrypto find Ubuntu's CA store by default.
The full path in `--cross-compile-prefix` matters: the host *also* has Ubuntu's mingw-w64 GCC 13
(msvcrt) in /usr/bin; objects built with it fail to link against UCRT (`__imp__vsnprintf`).

**Boost** (headers + CMake config only; Boost.System is header-only since 1.69, libtorrent 2.0 needs nothing compiled)
```
cd src/boost-1.92.0 && ./bootstrap.sh --with-toolset=gcc && ./b2 --build-dir=build/<t>/boost --prefix=$P --with-headers install
```

**libtorrent-rasterbar**
```
cmake -S src/libtorrent-rasterbar-2.0.15 -B build/<t>/libtorrent <common> [toolchain] -DCMAKE_CXX_STANDARD=17 \
  -DOPENSSL_ROOT_DIR=$P -DOPENSSL_USE_STATIC_LIBS=ON -DBoost_ROOT=$P \
  -Dencryption=ON -Ddht=ON -Dpython-bindings=OFF -Dbuild_examples=OFF -Dbuild_tests=OFF -Dbuild_tools=OFF -Dstatic_runtime=OFF
cmake --build ... && cmake --install ...
# post-install: drop "$<$<CONFIG:Debug>:TORRENT_USE_ASSERTS>" from the exported target so
# Debug consumers stay ABI-compatible with this Release library
```
Exported target `LibtorrentRasterbar::torrent-rasterbar` carries the needed defines
(`TORRENT_USE_OPENSSL`, `TORRENT_USE_LIBCRYPTO`, `TORRENT_SSL_PEERS`, `BOOST_ASIO_*`, win: `WIN32_LEAN_AND_MEAN`)
and link deps (OpenSSL::SSL, Boost::headers, Threads; win: bcrypt mswsock ws2_32 iphlpapi crypt32).

## Using them

```cmake
find_package(LibtorrentRasterbar 2.0 CONFIG REQUIRED)   # -> LibtorrentRasterbar::torrent-rasterbar
find_package(OpenSSL 3 REQUIRED)                        # -> OpenSSL::SSL OpenSSL::Crypto (static)
find_package(ZLIB REQUIRED)                             # -> ZLIB::ZLIB (static)
find_package(Boost 1.80 CONFIG REQUIRED)                # -> Boost::headers
```
`tools/configure-linux.sh` / `tools/configure-win.sh` already pass `CMAKE_PREFIX_PATH`,
`OPENSSL_ROOT_DIR`, `OPENSSL_USE_STATIC_LIBS=ON`, `ZLIB_ROOT`, `ZLIB_USE_STATIC_LIBS=ON`,
`CMAKE_POLICY_DEFAULT_CMP0167=NEW` (use BoostConfig.cmake, not the removed FindBoost).
See `tests/toolchain-hello/CMakeLists.txt` for a working example.
