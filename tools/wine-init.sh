#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Create / repair the 64-bit wine prefix at $WINEPREFIX (toolchain/wineprefix) without
# any GUI prompt (no Mono/Gecko installers). Idempotent.
#   tools/wine-init.sh
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../toolchain/env.sh"
export WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d"
if [ ! -f "$WINEPREFIX/system.reg" ]; then
  echo "initialising $WINEPREFIX"
  DISPLAY= wineboot -i >/dev/null 2>&1 || true   # "syswow64\rundll32.exe c0000135" = no wine32, harmless
else
  DISPLAY= wineboot -u >/dev/null 2>&1 || true
fi
wineserver -w

# Qt >= 6.12's qwindows plugin reads the monitor ICC profile and calls
# mscms.WcsCreateIccProfile, which wine 10 does not implement (process aborts with
# "unimplemented function mscms.dll.WcsCreateIccProfile"). Without the default sRGB
# profile file OpenColorProfileW fails and Qt silently falls back to sRGB.
C="$WINEPREFIX/drive_c/windows/system32/spool/drivers/color"
for f in "$C"/*.icm "$C"/*.icc; do
  [ -e "$f" ] && mv -f "$f" "$f.disabled-for-qt"
done
echo "wine prefix ready: $WINEPREFIX ($(wine --version))"
