#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# (Re)install the compilers/Qt under toolchain/: what the toolchain task did, as a script.
#   tools/install-toolchain.sh
# Idempotent: skips parts that already exist. Needs network (github, download.qt.io).
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../toolchain/env.sh"
T=$OCTOMX_ROOT/toolchain
LLVM_MINGW_TAG=20260602          # LLVM 22.1.7 == the clang Qt 6.12.1 win64_llvm_mingw was built with
AQT_COMMIT=5a49f2b391ae8ee1d3c0dd87f8b33c1d49a4deb7   # aqtinstall 3.3.0 (PyPI) cannot parse the 6.12 Windows repo layout
LLVM_MINGW_SHA256=9d191203f9768ead60662d3ae53cdf28e0a28b1e6d44b7f329b9202cb2add337
PY_PKGS=(Pillow==13.0.0 fonttools==4.68.0)   # Pillow: screenshot tools; fonttools: src/ui/res/fonts/patch_tahoma.py
# Qt archives: aqtinstall verifies each download against the checksums in Qt's repository metadata.

# 1. python venv: aqtinstall + Pillow
if [ ! -x "$T/venv/bin/aqt" ]; then
  python3 -m venv "$T/venv"
  "$T/venv/bin/pip" install -q "git+https://github.com/miurahr/aqtinstall.git@$AQT_COMMIT" "${PY_PKGS[@]}"
fi

# 2. Qt (same version for both)
if [ ! -d "$QT_LINUX_DIR" ]; then
  "$T/venv/bin/aqt" install-qt linux desktop "$QT_VERSION" linux_gcc_64 -O "$T/qt" -m qtmultimedia
fi
if [ ! -d "$QT_WIN_DIR" ]; then
  "$T/venv/bin/aqt" install-qt windows desktop "$QT_VERSION" win64_llvm_mingw -O "$T/qt" -m qtmultimedia
fi
grep -E "QT_CLANG_(MAJOR|MINOR|PATCH)_VERSION" "$QT_WIN_DIR/mkspecs/qconfig.pri"

# 3. llvm-mingw (ubuntu x86_64 host, UCRT)
if [ ! -x "$LLVM_MINGW_ROOT/bin/x86_64-w64-mingw32-clang++" ]; then
  f=llvm-mingw-$LLVM_MINGW_TAG-ucrt-ubuntu-22.04-x86_64
  mkdir -p "$T/downloads"
  [ -s "$T/downloads/$f.tar.xz" ] || curl -fL -o "$T/downloads/$f.tar.xz" \
      "https://github.com/mstorsjo/llvm-mingw/releases/download/$LLVM_MINGW_TAG/$f.tar.xz"
  echo "$LLVM_MINGW_SHA256  $T/downloads/$f.tar.xz" | sha256sum -c --quiet - || { echo "llvm-mingw checksum mismatch" >&2; exit 1; }
  tar -C "$T" -xf "$T/downloads/$f.tar.xz"
  rm -rf "$LLVM_MINGW_ROOT"; mv "$T/$f" "$LLVM_MINGW_ROOT"
fi
"$LLVM_MINGW_ROOT/bin/x86_64-w64-mingw32-clang++" --version | head -n1

# 4. wine prefix
"$OCTOMX_ROOT/tools/wine-init.sh"
echo "toolchain ready. Next: tools/build-third-party.sh"
