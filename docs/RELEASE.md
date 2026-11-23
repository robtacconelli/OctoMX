# How the OctoMX release is produced and verified

One command (from the checkout root; it also works from any directory with its full path):

```bash
tools/release.sh          # ~10 min: 2 fresh builds, 2 packages, 2 verifications
```

Exit status 0 means every build, packaging and verification step passed. Evidence:
`build/release/summary.txt` (short) and `build/release/*.log` (everything). Options:
`--reuse` (incremental rebuild of the existing release trees instead of wiping them),
`--no-verify`, `--skip-linux`, `--skip-win`, `--xvnc N` (X display for wine; default the first
free of :91-:99, started and stopped by the script).

## What it does

| step | script | output |
|---|---|---|
| version | `project(OctoMX VERSION x.y.z)` in `CMakeLists.txt` | package names use `x.y` |
| Linux build | `tools/configure-linux.sh build/release-linux` | `build/release/linux.{cmake,build}.log` |
| Windows build | `tools/configure-win.sh build/release-win` | `build/release/win.{cmake,build}.log` |
| Linux package | `tools/release/pack-linux.sh` | `dist/OctoMX-<v>-linux-x86_64/`, `.tar.xz` |
| Windows package | `tools/release/pack-win.sh` | `dist/OctoMX-<v>-win64/`, `.zip` |
| notices, READMEs | `tools/release/notices.sh` (+ `qt_notices.py`) | `LICENSE.txt` (GPL-3.0), `SOURCE.txt` (source offer), `THIRD-PARTY-NOTICES.txt`, `licenses/`, `README.txt` in each package |
| manifests [upd-1] | `tools/release/manifest.sh` | `MANIFEST.txt` in each package (sha256, size, path of every file) |
| checksums | | `dist/SHA256SUMS` |
| signature [upd-1] | `tools/release/sign.sh` | `dist/SHA256SUMS.sig` (Ed25519, the release key) |
| Linux check | `tools/release/verify-linux.sh` | `build/release/verify-linux.log`, `build/release/verify-linux/` |
| Windows check | `tools/release/verify-win.sh` | `build/release/verify-win.log`, `build/release/verify-win/` |

Build configuration (both trees, always configured from scratch unless `--reuse`):
`CMAKE_BUILD_TYPE=Release` with `-O2 -g -DNDEBUG`, `MX_BUILD_TESTS=OFF` (no Qt Test, no mxcli),
**`MX_TEST_HOOKS=OFF`** (the fault-injection hook `overlay/testCorruptUploads` is not compiled in),
all modules on (`MX_UI_ONLY=OFF`, empty `MX_SKIP_MODULES`). Linux adds
`CMAKE_BUILD_WITH_INSTALL_RPATH=ON`, `CMAKE_INSTALL_RPATH=$ORIGIN/../lib` (there is no patchelf
on the build host). Windows: `WIN32_EXECUTABLE` (no console window; `main.cpp` attaches to the
parent console when started from one), `src/app/res/octomx.rc` gives the icon and the version
resource.

Debug info is split off before packaging: `build/release/symbols/octomx.debug` and
`octomx.exe.debug` (`objcopy --only-keep-debug`); the shipped binaries are `strip --strip-all`ed
and carry a `.gnu_debuglink` to those files. Third-party DLLs / .so files get `--strip-debug`
(libc++.dll and libunwind.dll ship with DWARF otherwise). Keep `symbols/` of every published
release (they are not in the archives).

## Package contents

**Windows** (`OctoMX-<v>-win64/`): `octomx.exe`, `qt.conf`, the DLLs found by walking PE imports
from the exe and the plugins (Qt6Core, Qt6Gui, Qt6Widgets, Qt6Network, Qt6Sql, libc++,
libunwind), plugins `platforms/qwindows.dll`, `tls/qschannelbackend.dll`,
`sqldrivers/qsqlite.dll`, `imageformats/qico.dll`, `fonts/` (the LGPL UI font as separate
files), the notices. Deliberately not shipped (the dev `tools/package-win.sh` ships them):
`styles/` (the app's ClassicStyle sits on Qt's built-in "Windows" style), `iconengines/`,
other image formats, `networkinformation/`, `multimedia/`, `opengl32sw.dll`, translations,
`Qt6Test.dll`.

**Linux** (`OctoMX-<v>-linux-x86_64/`): launcher `octomx`, `bin/octomx` (+ `bin/qt.conf`:
`Prefix=..`, `Libraries=lib`, `Plugins=plugins`), `lib/` with every library `ldd` resolves inside
the Qt tree (Qt6 Core/Gui/Widgets/Network/Sql/DBus/XcbQpa, ICU 73) plus the X11 helper
libraries the xcb plugin needs and that desktops without Qt often lack (libxcb-cursor,
-icccm, -image, -keysyms, -render-util, -util, libxkbcommon-x11), `plugins/` (platforms xcb +
offscreen, compose input context, tls openssl + certonly, sqlite, ico), `share/applications/
octomx.desktop` + `share/icons/hicolor/{48x48,256x256}/apps/octomx.png`,
`install-desktop-entry.sh`, `fonts/`, the notices. The launcher only forces `QT_QPA_PLATFORM=xcb`
when the environment asks for a platform the bundle does not have (e.g. wayland); `bin/octomx`
runs directly too, which is what the app's own OS integration (link handlers, autostart) uses.
The desktop file id is `octomx.desktop` (a legacy entry of a copy installed before 5.0 is moved by
the program and by `install-desktop-entry.sh` when it points into the same folder), the same id the app writes when it registers itself as
link handler, so there is only one menu entry.

## What the verification proves

Both scripts unpack the **archive** (not the dist folder) into a fresh work dir and fail on any
problem.

`verify-linux.sh`:
1. every `NEEDED` entry of every ELF file is bundled or on a base-system allow list; `env -i ldd`
   shows nothing "not found"; no Qt / ICU / xcb-util library resolves outside the bundle; reports
   the glibc / GLIBCXX floor and the full list of system libraries used;
2. `bin/octomx` is stripped, has RUNPATH `$ORIGIN/../lib` and does not contain the corrupting hook;
3. `env -i HOME=<scratch> PATH=/usr/bin:/bin QT_QPA_PLATFORM=offscreen octomx --demo --screenshot`;
4. the real core under `env -i` with a throwaway `--profile` (own ports 47101-47107, UPnP/NAT-PMP
   off, incoming dir in the profile): runs a UI script (wait 45 s, screenshot, search "big buck
   bunny", `expect-rows searchResults 1`, screenshot); checks the log for networks coming online,
   web index sites answering over HTTPS (Qt TLS through the system libssl.so.3), the SQLite
   database; samples `/proc/<pid>/maps` to show that every Qt library and plugin was mapped from
   the bundle.

`verify-win.sh`:
1. `llvm-objdump -p` / `llvm-readobj`: GUI subsystem, VERSIONINFO + icon, no `.debug_*` sections,
   no test files; every import of every shipped PE file is shipped or a Windows system DLL, and
   every shipped DLL is imported by something (minimal);
2. no corrupting hook in `octomx.exe`;
3. wine under `env -i` (PATH=/usr/bin:/bin, no WINEPATH, the Windows-side PATH is printed and
   checked), started from an empty directory: `--demo --screenshot` on a private Xvnc;
4. the real core the same way (ports 47201-47207) with the same UI script (60 s + 25 s);
   `WINEDEBUG=+loaddll` lists every native module loaded, all must come from the package
   folder, and qwindows, qschannelbackend and qsqlite must be among them; the log must show
   networks online, HTTPS web search hits (schannel) and the SQLite database.

Screenshots of both runs are left in `build/release/verify-*/` (`demo.png`, `real-online.png`,
`real-search.png`); look at them.

## Signing and the self-update [upd-1]

The program installs newer releases itself (Settings ▸ Miscellaneous ▸ System Integration ▸ Updates; default "Install new versions automatically"). It trusts a release only through the
signature of its `SHA256SUMS`:

* **The key.** An Ed25519 key made with OpenSSL (`openssl genpkey -algorithm ED25519`), private half
  at `keys/release_ed25519.pem` in the checkout (directory mode 700, file mode 600). `keys/` and
  `*.pem` are in `.gitignore`, and `tools/make-source-tree.sh` refuses to copy `keys/`, any `.pem`
  and any file holding a private key, so it is never published. The public half is a table in
  `src/api/update_keys.cpp` (id `6b4bb49f391b4521` = first 16 hex digits of the SHA-256 of the raw
  public key). **Keep a copy of the private key offline (an encrypted USB stick and a printed
  copy of `openssl pkey -in keys/release_ed25519.pem -outform DER | base64`); whoever has it can
  publish updates that every installed OctoMX accepts.**
* **Signing.** `tools/release.sh` calls `tools/release/sign.sh dist/SHA256SUMS` after the checksums:
  `openssl pkeyutl -sign -rawin` over the exact bytes of `SHA256SUMS`, checked with
  `openssl pkeyutl -verify` before it is written, and refused when the key's public half is not in
  `update_keys.cpp`. `MX_RELEASE_KEY=<file>` uses another key file. Without a key the release is
  built anyway and the summary says in capitals that it is UNSIGNED: such a release is never
  installed automatically (users get the download page), so do not publish one by accident.
* **Upload** all four assets: `OctoMX-<v>-win64.zip`, `OctoMX-<v>-linux-x86_64.tar.xz`, `SHA256SUMS`,
  `SHA256SUMS.sig`. A release without `SHA256SUMS.sig` is only offered as a download page.
* **Verification.** `verify-linux.sh` / `verify-win.sh` run the packaged program's own verifier
  (`octomx --verify-update <dist dir>`, `octomx --verify-manifest <package dir>`; under wine with
  `--verify-report <file>`), so the shipped code itself checks the signature, every archive hash and
  every file of the manifest, and they check that the binary has no test key hook.
* **Test key hook.** Tests trust a throwaway key through `mx::upd::addTestTrustedKey()`, which only
  exists when the build has `MX_TEST_HOOKS=ON` (CMake defines `MX_UPDATE_TEST_HOOKS` for it).
  `tools/release.sh` always configures `MX_TEST_HOOKS=OFF`, fails if the cache says otherwise, and
  the verify scripts fail when the binary contains the hook's log text.
* **Rotation.** Make a new key, add its public half as a second line in `update_keys.cpp`, publish
  one release signed with the OLD key (it then carries both), and sign later releases with the new
  key; drop the old line in a later release.
* **Lost or leaked key.** Versions that only know the lost key cannot verify anything newer. Publish
  a release by hand that carries a new public key (and, if the old key leaked, without the old
  one), signed with the new key; installed versions then show the download page for it ("signed
  with a key this version does not know"), users install it once by hand, and from then on updates
  are automatic again. Say so in the release notes.

What a client does with a release: `src/api/mx/update_install.h`, `src/api/mx/update_download.h`,
`src/ui/update_ui.h` (download into `<profile>/updates/`, unpack into `<program dir>/.new-<v>/`,
check the tree against its `MANIFEST.txt`, swap with a journal, old files in
`<program dir>/.old-<v>/` until the next start).

Test releases: `tools/release.sh --out build/<dir> --version 5.2.0` builds everything under
`build/<dir>` (build trees, logs, `dist/`) with another version number (CMake
`-DCMAKE_PROJECT_OctoMX_INCLUDE=tools/release/version-override.cmake -DMX_VERSION_OVERRIDE=5.2.0`),
without touching `dist/` or the version line. The update end-to-end test uses this
(`tests/update_e2e/e2e.sh`).

Without the release key [ren-1]: `tools/release.sh --test-hooks --out build/e2eA` and
`tools/release.sh --test-hooks --version 5.2.0 --out build/e2eB` build packages WITH the test hooks
(never signed, never verified, never to be published), and
`tests/update_e2e/e2e.sh --a build/e2eA/dist --b build/e2eB/dist ... --throwaway-key` signs a copy
of B's `SHA256SUMS` with a throwaway Ed25519 key made in its work folder and passes the public key
to A in `MX_UPDATE_TEST_PUBKEY`, which only builds with test hooks read.

The real-core run of the verification has no update check (`updates/mode` off) and
`MX_OVERLAY_DISCOVERY=0`: a release run contacts neither GitHub nor the live OctoMX network.

## Folders with legacy starter files [upd-2]

Copies installed before 5.0 could update themselves to 5.0 only through packages under the old
package names (`WinMX-<v>-<platform>`), which held small starter files under the old program file
names (`winmx.exe`, resp. `winmx` + `bin/winmx`) next to the real program. Releases no longer have
such packages, but folders updated that way exist, and the updater keeps them working:

* Such a folder is a published package like any other (its manifest lists `octomx.exe` /
  `bin/octomx`; `Layout::bridged` says that it also lists starter files) and takes the normal
  `OctoMX-<v>-<platform>` asset; a 5.x never takes an asset under the old package name
  (`isPackageArchiveName()` only matches `OctoMX-`).
* The swap **keeps the starter files in place** (`legacyStarterFiles()` in `update_install.cpp`):
  they are left out of the journal's file lists, so the swap, the roll-back and the recovery never
  move them (the journal names them on `k <path>` lines), and those still exactly as installed are
  appended to the new `MANIFEST.txt` after a line
  `# kept in place from <v> (starter files under the legacy program names, not part of this package):`,
  so the installed manifest keeps listing them and the next update keeps them again. A starter file
  the user changed stays too but is no longer listed. `tests/ui/tst_update_bridge.cpp` covers this.
* The update journal and the signature file keep their format identifiers (`kJournalHeader`,
  `kSignatureHeader` in `update_install.cpp`), so a journal left by an earlier version is finished,
  and every installed copy reads the signature.
* This code must stay as long as such folders may exist.

## Re-running after code changes

Just run `tools/release.sh` again: it wipes and reconfigures `build/release-{linux,win}`, rebuilds,
replaces `dist/OctoMX-<v>-*` and the archives, and re-verifies. Real-network checks depend on the
internet (a web index site or an eD2k server may be down): the network lines are WARN, the
summary requires at least two internet networks online and at least one HTTPS index answering.

## Changing what is shipped

* A new Qt plugin family: add it to `PLUGINS` in `pack-win.sh` / `pack-linux.sh`. The import walk
  adds its DLLs; `verify-win.sh` fails if a shipped DLL becomes unused.
* A new third-party library: add its licence text and a section to `notices.sh`.
* The Qt third-party list is generated from Qt's SBOM (`toolchain/qt/<ver>/<arch>/sbom/`); the
  licence texts come from Qt's `LICENSES/` (`tools/release/files/licenses/qt/`, fetched from
  github.com/qt/qtbase at tag 6.12.1). After a Qt upgrade, refresh that folder.
