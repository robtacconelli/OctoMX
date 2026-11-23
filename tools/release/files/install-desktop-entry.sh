#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Adds OctoMX to your application menu (per user, no root): writes
#   ~/.local/share/applications/octomx.desktop          (Exec = this folder's ./octomx)
#   ~/.local/share/icons/hicolor/{48x48,256x256}/apps/octomx.png
# Run it again after moving the folder. Remove with:  ./install-desktop-entry.sh --remove
# A legacy menu entry of a copy installed before 5.0 (and its icon winmx.png) is removed as well
# when it starts the program in this folder (OctoMX itself does the same at start).
set -e
HERE=$(dirname "$(readlink -f "$0")")
DATA=${XDG_DATA_HOME:-$HOME/.local/share}
OLD=$DATA/applications/winmx-ng.desktop
if [ -f "$OLD" ]; then
  if grep -q "^Exec=\"\{0,1\}$HERE/" "$OLD"; then
    rm -f "$OLD" "$DATA/icons/hicolor/48x48/apps/winmx.png" "$DATA/icons/hicolor/256x256/apps/winmx.png"
    echo "removed the legacy menu entry of this folder"
  else
    echo "note: $OLD starts a program in another folder and is left as it is"
  fi
fi
if [ "${1:-}" = "--remove" ]; then
  rm -f "$DATA/applications/octomx.desktop" "$DATA/icons/hicolor/48x48/apps/octomx.png" \
        "$DATA/icons/hicolor/256x256/apps/octomx.png"
  echo "removed the OctoMX menu entry"; exit 0
fi
mkdir -p "$DATA/applications" "$DATA/icons/hicolor/48x48/apps" "$DATA/icons/hicolor/256x256/apps"
sed "s|@PREFIX@|$HERE|g" "$HERE/share/applications/octomx.desktop" > "$DATA/applications/octomx.desktop"
cp "$HERE/share/icons/hicolor/48x48/apps/octomx.png" "$DATA/icons/hicolor/48x48/apps/octomx.png"
cp "$HERE/share/icons/hicolor/256x256/apps/octomx.png" "$DATA/icons/hicolor/256x256/apps/octomx.png"
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$DATA/applications" >/dev/null 2>&1 || true
echo "installed $DATA/applications/octomx.desktop -> $HERE/octomx"
