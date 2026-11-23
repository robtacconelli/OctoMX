# Building OctoMX from source

Everything the build needs that is not in this repository (Qt, the cross compiler, the static
libraries) is downloaded by two scripts into the checkout itself (`toolchain/`, `third_party/`),
pinned by version and checked by SHA-256 / commit. Nothing is installed system-wide and nothing
is written to `$HOME`. The scripts work from any checkout location.

The reference build host is Ubuntu 25.10 (x86_64, GCC 15). Other recent x86_64 Linux
distributions should work; the binaries you build need the glibc/libstdc++ of the build host.

## 1. Host prerequisites

Install with your package manager (Debian/Ubuntu names):

| for | packages |
|---|---|
| everything | `build-essential` (GCC ≥ 13), `cmake` (≥ 3.21), `ninja-build`, `git`, `curl`, `xz-utils`, `python3`, `python3-venv`, `perl`, `make` |
| running the Linux build / tests | the usual X11/OpenGL/fontconfig desktop libraries Qt needs, plus `libxcb-cursor0` `libxkbcommon-x11-0` `libxcb-icccm4` `libxcb-image0` `libxcb-keysyms1` `libxcb-render-util0`; `libssl3` (Qt's TLS backend loads the system OpenSSL) |
| Windows cross build: running/verifying under Wine (optional) | `wine` (64-bit, Wine 10 tested) |
| screenshot tools (optional) | `tightvncserver` (Xvnc), `x11-apps` (xwd), `ffmpeg` |

## 2. Toolchain and dependencies (once)

```bash
source toolchain/env.sh          # in every shell: sets OCTOMX_ROOT, TMPDIR, caches, PATH, Qt dirs
tools/install-toolchain.sh       # toolchain/venv (aqtinstall, Pillow, fonttools), Qt 6.12.1 for
                                 # linux_gcc_64 + win64_llvm_mingw (~3 GB), llvm-mingw 20260602, wine prefix
tools/build-third-party.sh       # static zlib 1.3.2, OpenSSL 3.5.9, Boost 1.92 (headers), libtorrent 2.0.15
                                 # for linux + win (~8 min with -j8); clones librats at its pinned commit
```

* `tools/build-third-party.sh linux` builds only the Linux prefix (enough for a native build);
  `tools/build-third-party.sh fetch` only downloads, verifies and unpacks the sources.
* No Windows cross build wanted? Qt for Windows and llvm-mingw are still installed by
  `install-toolchain.sh`; you can stop it after the Linux Qt and run `build-third-party.sh linux`.
* Several checkouts can share one toolchain: in the second checkout create symlinks
  `toolchain/qt`, `toolchain/llvm-mingw` (and, if you like, `third_party/linux`, `third_party/win`,
  `third_party/src`) pointing into the first one.
* Disk: Qt ≈ 3 GB, llvm-mingw 0.6 GB, wine prefix 0.8 GB, third-party sources 0.8 GB, prefixes
  2 × 250 MB, a full build tree 2-4 GB.

## 3. Build

```bash
tools/configure-linux.sh build/linux && ninja -C build/linux -j8     # native Linux (RelWithDebInfo)
tools/configure-win.sh   build/win   && ninja -C build/win   -j8     # Windows x86_64 cross build
```

Both scripts take `[-S <source-dir>] <build-dir> [extra cmake args]`. Useful options:

| option | default | meaning |
|---|---|---|
| `-DCMAKE_BUILD_TYPE=Release` | RelWithDebInfo | |
| `-DMX_BUILD_TESTS=OFF` | ON | no Qt Test executables / probes |
| `-DMX_TEST_HOOKS=OFF` | ON | leave out fault-injection hooks (release builds do) |
| `-DMX_WEB_DEFAULT_PROFILE=conservative` | standard | which web index sites start enabled, see `src/net/web/README.md` |
| `-DMX_SKIP_MODULES="net/overlay;net/ed2k"` | empty | leave modules (and their tests) out |
| `-DMX_UI_ONLY=ON` | OFF | only the UI with the demo core |

The Linux binary is `build/linux/bin/octomx`; try it with `--demo` (canned data, no network).
For a runnable Windows folder: `tools/package-win.sh build/win dist/win` (then
`wine dist/win/octomx/octomx.exe --demo`; run `tools/wine-init.sh` first if Wine complains).

## 4. Tests

```bash
ctest --test-dir build/linux -j4 --output-on-failure
```

All ctest tests are offline (loopback servers, synthetic data). GUI tests use
`QT_QPA_PLATFORM=offscreen` themselves. Tests write only below the build tree. The `*_probe`
executables (`bt_probe`, `ed2k_probe`, `gnutella_probe`, `web_probe`, ...) are not tests: they
talk to the real networks and are meant for manual checks; use them only with legal content.
Windows test executables run under Wine; QTest prints nothing to a pipe there, use
`-o result.txt,txt`.

## 5. Release packages

`tools/release.sh` builds both platforms from scratch, packages them into `dist/` and verifies
the archives (`docs/RELEASE.md`). Set `OCTOMX_SOURCE_URL` (and optionally `OCTOMX_SOURCE_TAG`)
so the packages' `SOURCE.txt` names the public source repository; without it a placeholder is
written and a warning printed.
