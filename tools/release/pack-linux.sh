#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# tools/release/pack-linux.sh <build-dir> <dest-dir> <symbols-dir>
# Relocatable Linux bundle for bin/octomx (called by tools/release.sh).
#
#   <dest>/octomx                     launcher (optional: bin/octomx runs directly too)
#   <dest>/bin/octomx                 the app, RUNPATH $ORIGIN/../lib, + bin/qt.conf
#   <dest>/lib/                      Qt 6 + ICU (from the Qt build) and the small X11 helper libs
#                                    Qt's xcb plugin needs that are often missing on non-Qt systems
#   <dest>/plugins/<family>/         Qt plugins actually used (RUNPATH $ORIGIN/../../lib)
#   <dest>/share/applications/octomx.desktop, share/icons/hicolor/*/apps/octomx.png
#   <dest>/install-desktop-entry.sh  writes a menu entry with the absolute path of this folder
#   <dest>/fonts/                    the UI font files (also compiled into the binary)
# Libraries are collected by walking `ldd` of the binary and every plugin; anything resolved from
# the Qt tree is copied, plus the explicit X11 helper list below. Everything else must be on the
# base-system allow list (verify-linux.sh checks that again on the extracted archive).
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
source "$ROOT/toolchain/env.sh"
unset LD_LIBRARY_PATH
BD=$(realpath "$1"); DEST=$(realpath -m "$2"); SYM=$(realpath -m "$3")
BIN=$BD/bin/octomx
[ -x "$BIN" ] || { echo "no $BIN" >&2; exit 1; }
readelf -d "$BIN" | grep -q 'R.*PATH.*\$ORIGIN/../lib' || { echo "$BIN has no \$ORIGIN/../lib runpath" >&2; exit 1; }
rm -rf "$DEST"; mkdir -p "$DEST/bin" "$DEST/lib" "$DEST/plugins" "$SYM"

objcopy --only-keep-debug "$BIN" "$SYM/octomx.debug"
strip --strip-all -o "$DEST/bin/octomx" "$BIN"
(cd "$SYM" && objcopy --add-gnu-debuglink=octomx.debug "$DEST/bin/octomx")

QP=$QT_LINUX_DIR/plugins
PLUGINS=(platforms/libqxcb.so platforms/libqoffscreen.so
         platforminputcontexts/libcomposeplatforminputcontextplugin.so
         tls/libqopensslbackend.so tls/libqcertonlybackend.so
         sqldrivers/libqsqlite.so imageformats/libqico.so)
for p in "${PLUGINS[@]}"; do mkdir -p "$DEST/plugins/$(dirname "$p")"; cp "$QP/$p" "$DEST/plugins/$p"; done

# X11 helper libraries of the xcb QPA (MIT/X11 licence) that a desktop without Qt often lacks
# (Qt >= 6.5 needs libxcb-cursor0, which is not installed by default on many distributions)
EXTRA_SYS=(libxcb-cursor.so.0 libxcb-icccm.so.4 libxcb-image.so.0 libxcb-keysyms.so.1
           libxcb-render-util.so.0 libxcb-util.so.1 libxkbcommon-x11.so.0)

declare -A DONE
copy_from_ldd() { # elf
  local name path
  while read -r name path; do
    [ -n "$path" ] || continue
    [ -z "${DONE[$name]:-}" ] || continue
    if [[ "$path" == "$QT_LINUX_DIR"/* ]] || printf '%s\n' "${EXTRA_SYS[@]}" | grep -qx "$name"; then
      DONE[$name]=1
      cp -L "$path" "$DEST/lib/$name"
      copy_from_ldd "$path"   # walk the original (its RUNPATH resolves inside the Qt tree)
    fi
  done < <(LD_LIBRARY_PATH="$QT_LINUX_DIR/lib" ldd "$1" | sed -n 's/^\s*\(\S*\) => \(\/\S*\) .*/\1 \2/p')
}
copy_from_ldd "$BIN"
for p in "${PLUGINS[@]}"; do copy_from_ldd "$QP/$p"; done

# Qt libraries / plugins: drop debug sections (dynamic symbols stay)
find "$DEST/lib" "$DEST/plugins" -name '*.so*' -type f -exec strip --strip-debug {} \;
chmod 0644 "$DEST"/lib/* ; find "$DEST/plugins" -name '*.so' -exec chmod 0755 {} \;

cat > "$DEST/bin/qt.conf" <<'EOF'
[Paths]
Prefix = ..
Libraries = lib
Plugins = plugins
EOF

F=$ROOT/tools/release/files
install -m 0755 "$F/octomx.sh" "$DEST/octomx"
install -m 0755 "$F/install-desktop-entry.sh" "$DEST/install-desktop-entry.sh"
mkdir -p "$DEST/share/applications" "$DEST/share/icons/hicolor/48x48/apps" "$DEST/share/icons/hicolor/256x256/apps"
install -m 0644 "$F/octomx.desktop" "$DEST/share/applications/octomx.desktop"
install -m 0644 "$ROOT/src/app/res/octomx-48.png" "$DEST/share/icons/hicolor/48x48/apps/octomx.png"
install -m 0644 "$ROOT/src/app/res/octomx-256.png" "$DEST/share/icons/hicolor/256x256/apps/octomx.png"
mkdir -p "$DEST/fonts"
cp "$ROOT/src/ui/res/fonts/tahoma.ttf" "$ROOT/src/ui/res/fonts/tahomabd.ttf" "$DEST/fonts/"

echo "bundled libs: $(cd "$DEST/lib" && ls | tr '\n' ' ')"
echo "plugins: ${PLUGINS[*]}"
echo "system libs still required (outside the bundle): $(
  for f in "$DEST/bin/octomx" "$DEST"/lib/* $(find "$DEST/plugins" -name '*.so'); do ldd "$f"; done \
    | sed -n 's/^\s*\(\S*\) => \(\/\S*\) .*/\1 \2/p' | grep -v " $DEST/" | awk '{print $1}' | sort -u | tr '\n' ' ')"
echo "symbols: $SYM/octomx.debug ($(du -h "$SYM/octomx.debug" | cut -f1)); binary $(du -h "$DEST/bin/octomx" | cut -f1)"
