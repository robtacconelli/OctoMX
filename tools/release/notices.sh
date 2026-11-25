#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# tools/release/notices.sh <linux|win|mac> <package-dir>      (mac: OctoMX.app/Contents/Resources, [mac-1])
# Writes LICENSE.txt, NOTICE.txt, SOURCE.txt, THIRD-PARTY-NOTICES.txt, README.txt and licenses/ into a
# package folder. The source location in SOURCE.txt comes from OCTOMX_SOURCE_URL (and OCTOMX_SOURCE_TAG,
# default v<version>); without it a clearly marked placeholder is written and a warning printed.
# Licence texts are taken from the source trees in third_party/ and the toolchain themselves
# (and Qt's own LICENSES/ + SBOM for the parts bundled inside Qt), never retyped.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
source "$ROOT/toolchain/env.sh"
PLAT=$1; DEST=$(cd "$2" && pwd)
[ "$PLAT" = mac ] && source "$ROOT/tools/mac/env.sh"
V=${OCTOMX_RELEASE_VERSION:-5.9}
SOURCE_URL=${OCTOMX_SOURCE_URL:-https://github.com/robtacconelli/OctoMX}
SOURCE_TAG=${OCTOMX_SOURCE_TAG:-v$V}
F=$ROOT/tools/release/files
TP=$ROOT/third_party/src
L=$DEST/licenses
rm -rf "$L"; mkdir -p "$L/qt" "$L/fonts"
cl=/usr/share/common-licenses

# ---- licence texts --------------------------------------------------------------------------
if [ "$PLAT" = mac ]; then   # [mac-1] no common-licenses folder on a Mac: the same texts from Qt's LICENSES/ copy
  cp "$F/licenses/qt/LGPL-3.0-only.txt" "$L/LGPL-3.0.txt"; cp "$F/licenses/qt/GPL-3.0-only.txt" "$L/GPL-3.0.txt"
else
cp "$cl/LGPL-3" "$L/LGPL-3.0.txt"; cp "$cl/GPL-3" "$L/GPL-3.0.txt"     # LGPLv3 = GPLv3 + additional permissions
fi
cp "$TP/libtorrent-rasterbar-2.0.15/COPYING" "$L/libtorrent-rasterbar-COPYING.txt"
cp "$TP/libtorrent-rasterbar-2.0.15/LICENSE" "$L/libtorrent-rasterbar-LICENSE.txt"
cp "$TP/boost-1.92.0/LICENSE_1_0.txt" "$L/Boost-BSL-1.0.txt"
cp "$TP/openssl-3.5.9/LICENSE.txt" "$L/OpenSSL-Apache-2.0.txt"
cp "$TP/zlib-1.3.2/LICENSE" "$L/zlib-LICENSE.txt"
mkdir -p "$L/librats"
cp "$TP/librats/LICENSE" "$L/librats/LICENSE.txt"
cp "$TP/librats/THIRD_PARTY_NOTICES.md" "$L/librats/THIRD_PARTY_NOTICES.md"
for f in curve25519-donna noise-c poly1305-donna; do cp "$TP/librats/licenses/$f.LICENSE" "$L/librats/$f.LICENSE.txt"; done
# UI font: LGPL-2.1+ (Wine). Ship the licence, the unmodified originals and the script that
# derives the bundled "OctoMX Tahoma" from them (= the corresponding source of the modified font).
cp "$ROOT/third_party/fonts/COPYING.LGPL-2.1" "$L/fonts/COPYING.LGPL-2.1.txt"
cp "$ROOT/third_party/fonts/README.md" "$L/fonts/README-wine-fonts.md"
cp "$ROOT/src/ui/res/fonts/patch_tahoma.py" "$L/fonts/patch_tahoma.py"
mkdir -p "$L/fonts/original-wine-10.0"
cp "$ROOT/third_party/fonts/tahoma.ttf" "$ROOT/third_party/fonts/tahomabd.ttf" "$L/fonts/original-wine-10.0/"

case "$PLAT" in
  win) QT_SBOM=$QT_WIN_DIR/sbom/qtbase-$QT_VERSION.spdx.json ;;
  mac) QT_SBOM=$QT_MAC_DIR/sbom/qtbase-$QT_VERSION.spdx.json ;;   # [mac-1]
  *)   QT_SBOM=$QT_LINUX_DIR/sbom/qtbase-$QT_VERSION.spdx.json ;;
esac
QTN=$(mktemp)
python3 -I "$ROOT/tools/release/qt_notices.py" "$QT_SBOM" "$F/licenses/qt" "$QTN" "$L/qt"

if [ "$PLAT" = win ]; then
  cp "$LLVM_MINGW_ROOT/LICENSE.TXT" "$L/LLVM-Apache-2.0-with-LLVM-exception.txt"
  M=$LLVM_MINGW_ROOT/x86_64-w64-mingw32/share/mingw32
  cp "$M/COPYING.MinGW-w64-runtime.txt" "$L/MinGW-w64-runtime.txt"
  [ -f "$DEST/libwinpthread-1.dll" ] && cp "$M/COPYING.winpthreads.txt" "$L/winpthreads.txt"
elif [ "$PLAT" = mac ]; then
  :   # [mac-1] nothing besides Qt, the static libraries and the font: macOS supplies the rest
else
  cp "$F/licenses/ICU-73.2-LICENSE.txt" "$L/ICU-73.2-LICENSE.txt"
  mkdir -p "$L/x11-libs"
  for p in libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 libxcb-render-util0 libxcb-util1 libxkbcommon-x11-0; do
    cp "/usr/share/doc/$p/copyright" "$L/x11-libs/$p.copyright.txt"
  done
fi

# ---- THIRD-PARTY-NOTICES.txt ----------------------------------------------------------------
N=$DEST/THIRD-PARTY-NOTICES.txt
{
cat <<EOF
OctoMX $V: third-party notices
=================================

This package contains or is linked with the components below. The full licence texts are in
the licenses/ folder next to this file. OctoMX itself is licensed under the GNU GPL
version 3 or later: see LICENSE.txt, NOTICE.txt and SOURCE.txt.

1. Qt $QT_VERSION  (The Qt Company Ltd. and other contributors)
   Licence: GNU Lesser General Public License v3 (LGPL-3.0-only), used unmodified and
   dynamically linked. Licence texts: licenses/LGPL-3.0.txt, licenses/GPL-3.0.txt (the LGPL
   v3 adds permissions to the GPL v3, so both texts apply).
EOF
if [ "$PLAT" = win ]; then cat <<'EOF'
   Shipped as separate files: Qt6Core.dll, Qt6Gui.dll, Qt6Widgets.dll, Qt6Network.dll,
   Qt6Sql.dll and the plugins in platforms/, tls/, sqldrivers/, imageformats/.
EOF
elif [ "$PLAT" = mac ]; then cat <<'EOF'
   Shipped as separate files inside OctoMX.app: the frameworks in Contents/Frameworks (QtCore,
   QtGui, QtWidgets, QtNetwork, QtSql, QtConcurrent and what they need) and the plugins in
   Contents/PlugIns (platforms, tls, sqldrivers, imageformats).
EOF
else cat <<'EOF'
   Shipped as separate files: lib/libQt6Core.so.6, libQt6Gui.so.6, libQt6Widgets.so.6,
   libQt6Network.so.6, libQt6Sql.so.6, libQt6DBus.so.6, libQt6XcbQpa.so.6 and the plugins in
   plugins/.
EOF
fi
cat <<EOF
   You may replace these Qt libraries with your own build of the same Qt version (keep the
   file names); OctoMX will use them. The binaries are the official Qt Company builds
   ($(case "$PLAT" in win) echo win64_llvm_mingw ;; mac) echo clang_64 ;; *) echo linux_gcc_64 ;; esac), installed with aqtinstall), without changes.
   Corresponding source: the Qt $QT_VERSION source code is available from
     https://download.qt.io/official_releases/qt/${QT_VERSION%.*}/$QT_VERSION/single/qt-everywhere-src-$QT_VERSION.tar.xz
     (submodules: https://download.qt.io/official_releases/qt/${QT_VERSION%.*}/$QT_VERSION/submodules/qtbase-everywhere-src-$QT_VERSION.tar.xz)
   and https://code.qt.io/cgit/qt/qtbase.git/tag/?h=v$QT_VERSION .
   Qt contains third-party code; those notices (taken from Qt's own SBOM) are in section 12.

2. libtorrent-rasterbar 2.0.15  (Copyright (c) 2003-2020, Arvid Norberg)
   Licence: BSD-3-Clause, statically linked. licenses/libtorrent-rasterbar-COPYING.txt.
   libtorrent includes code under other permissive terms (puff.c: zlib licence, Mark Adler;
   ed25519: zlib licence, Orson Peters; SHA-1: public domain, Steve Reid; SHA-256: public
   domain, LibTomCrypt; others): all listed in licenses/libtorrent-rasterbar-LICENSE.txt.
   (route.h in that list, APSL/BSD, is only compiled for iOS, not in this build.)

3. Boost 1.92.0 (header-only libraries: Asio, System, ...)
   Licence: Boost Software License 1.0. licenses/Boost-BSL-1.0.txt.

4. OpenSSL 3.5.9  (The OpenSSL Project Authors)
   Licence: Apache License 2.0, statically linked (used by libtorrent for SHA/TLS and by the
   OctoMX network for Ed25519 / X25519 / ChaCha20-Poly1305). licenses/OpenSSL-Apache-2.0.txt.
EOF
if [ "$PLAT" = linux ]; then cat <<'EOF'
   Qt's TLS backend (plugins/tls/libqopensslbackend.so) loads the system's own libssl.so.3 /
   libcrypto.so.3 at run time; those are not part of this package.
EOF
elif [ "$PLAT" = mac ]; then cat <<'EOF'
   Qt's TLS on macOS uses the system's Secure Transport; no OpenSSL library is shipped.
EOF
else cat <<'EOF'
   Qt's TLS on Windows uses the Windows "Secure Channel" (schannel); no OpenSSL DLL is shipped.
EOF
fi
cat <<'EOF'

5. zlib 1.3.2  (Copyright (C) 1995-2026 Jean-loup Gailly and Mark Adler)
   Licence: zlib licence, statically linked. licenses/zlib-LICENSE.txt.

6. librats (pinned commit 9eefd0a9d9b7b0ccc3c692d6e8f80224723aed49, version 3.0.6)
   Copyright (c) 2026 librats contributors. Licence: MIT, statically linked.
   It embeds curve25519-donna (BSD-3-Clause, Copyright 2008 Google Inc.), poly1305-donna (MIT,
   Andrew Moon) and parts of noise-c (MIT, Copyright (C) 2016 Southern Storm Software, Pty Ltd.).
   Effective licence for this build: MIT AND BSD-3-Clause. licenses/librats/.

7. "OctoMX Tahoma" user-interface font
   Derived from Tahoma / Tahoma Bold of the Wine project (Wine 10.0, Ubuntu package fonts-wine),
   Copyright (c) 1993-2022 Wine project authors. Licence: GNU LGPL version 2.1 or later
   (licenses/fonts/COPYING.LGPL-2.1.txt).
   Modification (2026, OctoMX): the embedded-bitmap line metrics and OS/2 / hhea metrics were
   changed to the Windows GDI values, the family was renamed to "OctoMX Tahoma", and the font is
   distributed with modified glyphs: the bitmaps of "1" and "J" in the 8-pt (11 ppem) strike of
   the regular face were redrawn (same bearings and advances). The modified fonts are
   fonts/tahoma.ttf and fonts/tahomabd.ttf (the same files
   are also compiled into the program as resources). The corresponding source is the unmodified
   originals in licenses/fonts/original-wine-10.0/ plus the script licenses/fonts/patch_tahoma.py
   that produces the modified files; Wine's font sources (.sfd) are at
   https://gitlab.winehq.org/wine/wine/-/tree/wine-10.0/fonts .

8. SQLite 3.54.1 (inside Qt's SQLite driver): public domain ("blessing").

Icons and logo: the program icons and the OctoMX logo are original artwork drawn for
OctoMX by its contributors (pixel art in the style of the Windows 2000 era toolbars); no
artwork of the original WinMX program is used. They are covered by the licence
of OctoMX itself (LICENSE.txt).

EOF
if [ "$PLAT" = win ]; then cat <<EOF
9. LLVM runtime libraries: libc++.dll, libunwind.dll (llvm-mingw 20260602, LLVM 22.1.7)
   Licence: Apache License 2.0 with LLVM Exceptions. licenses/LLVM-Apache-2.0-with-LLVM-exception.txt.
   (The LLVM exception means the notice does not have to be reproduced for code compiled into
   a program; it is included anyway.)

10. MinGW-w64 runtime (statically linked start-up code and CRT glue of every llvm-mingw program)
   Copyright (c) 2009-2013 by the mingw-w64 project and others. Licence: the mingw-w64
   BSD-style licence plus the notices for getopt, gdtoa, parts of the math library and other
   pieces it contains, all in licenses/MinGW-w64-runtime.txt.
EOF
[ -f "$DEST/libwinpthread-1.dll" ] && cat <<'EOF'
   libwinpthread-1.dll: MIT-style licence (Copyright (c) 2011 mingw-w64 project), licenses/winpthreads.txt.
EOF
cat <<'EOF'

11. (no further runtime DLLs: everything else octomx.exe imports is part of Windows 10/11.)
EOF
elif [ "$PLAT" = mac ]; then cat <<'EOF'
9. System frameworks used but NOT shipped (Cocoa, Core Text, Core Services, Security,
   SystemConfiguration, libc++, ...) are part of macOS and come with it under Apple's terms.
EOF
else cat <<'EOF'
9. ICU 73.2 (lib/libicuuc.so.73, libicui18n.so.73, libicudata.so.73; shipped by Qt's Linux
   build). Copyright (c) Unicode, Inc. Licence: Unicode/ICU licence (MIT-like) with the
   third-party notices it contains: licenses/ICU-73.2-LICENSE.txt.

10. X11 helper libraries (lib/libxcb-cursor.so.0, libxcb-icccm.so.4, libxcb-image.so.0,
   libxcb-keysyms.so.1, libxcb-render-util.so.0, libxcb-util.so.1, libxkbcommon-x11.so.0), copied
   unmodified from Ubuntu because many desktops without Qt lack them. Licence: MIT / X11-style;
   the Debian copyright files with the full notices are in licenses/x11-libs/.

11. System libraries used but NOT shipped (glibc, libstdc++, X11/xcb, fontconfig, freetype,
   GL, glib, D-Bus, OpenSSL 3, ...) come with your Linux distribution under their own licences.
EOF
fi
cat <<'EOF'

12. Third-party code inside the shipped Qt libraries and plugins
   (generated from Qt's SBOM; licence texts in licenses/qt/<licence id>.txt)
   FreeType is used under the FreeType License (FTL), which asks for this credit:
   "Portions of this software are copyright (c) The FreeType Project (www.freetype.org).
   All rights reserved."

EOF
sed 's/^/   /' "$QTN"
} > "$N"
rm -f "$QTN"

# ---- LICENSE.txt / NOTICE.txt / SOURCE.txt / README.txt -----------------------------------------
rm -f "$DEST/LICENSE-TODO.txt"
cp "$ROOT/LICENSE" "$DEST/LICENSE.txt"
cp "$ROOT/NOTICE" "$DEST/NOTICE.txt"
sed -e "s/@VERSION@/$V/g" -e "s|@SOURCE_URL@|$SOURCE_URL|g" -e "s|@SOURCE_TAG@|$SOURCE_TAG|g" "$F/SOURCE.txt" > "$DEST/SOURCE.txt"
: # default source URL is the project repository; override with OCTOMX_SOURCE_URL
GLIBC=""; GCC=""; DISTRO=""
if [ "$PLAT" = linux ]; then   # symbol-version floor of everything in the bundle
  VERS=$(find "$DEST/bin" "$DEST/lib" "$DEST/plugins" -type f -exec objdump -T {} \; 2>/dev/null)
  GLIBC=$(grep -o 'GLIBC_[0-9.]*' <<<"$VERS" | sort -uV | tail -n1 | sed 's/GLIBC_//')
  CXX=$(grep -o 'GLIBCXX_[0-9.]*' <<<"$VERS" | sort -uV | tail -n1 | sed 's/GLIBCXX_//')
  case "$CXX" in 3.4.29) GCC=11;; 3.4.30) GCC=12;; 3.4.31) GCC=13.1;; 3.4.32) GCC=13.2;; 3.4.33) GCC=14;; 3.4.34) GCC=15;; *) GCC="(GLIBCXX_$CXX)";; esac
  case "$GLIBC" in 2.3[0-5]) DISTRO="Ubuntu 22.04, Debian 12, Fedora 36";; 2.3[6-8]) DISTRO="Ubuntu 24.04, Debian 13, Fedora 39";;
                   2.39) DISTRO="Ubuntu 24.04, Debian 13, Fedora 40";; 2.4[01]) DISTRO="Ubuntu 25.04, Debian 13, Fedora 42";;
                   *) DISTRO="a distribution with glibc $GLIBC";; esac
  echo "floor: glibc $GLIBC, GLIBCXX_$CXX (GCC $GCC)"
fi
sed -e "s/@VERSION@/$V/g" -e "s/@MACOS_MIN@/${MX_MACOS_MIN:-}/g" -e "s/@GLIBC@/$GLIBC/g" -e "s/@GCC@/$GCC/g" -e "s/@DISTRO_EXAMPLE@/$DISTRO/g" \
    "$F/README-$PLAT.txt" > "$DEST/README.txt"
if [ "$PLAT" = win ]; then   # Windows users open these with Notepad: CRLF line ends
  for f in "$DEST"/*.txt; do sed -i 's/\r\?$/\r/' "$f"; done
fi
echo "notices: $N ($(wc -l < "$N") lines), $(find "$L" -type f | wc -l) licence files"
