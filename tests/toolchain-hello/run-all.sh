#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# End-to-end toolchain proof: build hello for Linux + Windows, run offscreen, on Xvnc,
# and the packaged .exe under wine. Screenshots land in build/toolchain-hello-shots/.
#   tests/toolchain-hello/run-all.sh [xvnc-display, default 77]
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../../toolchain/env.sh"
cd "$OCTOMX_ROOT"
N=${1:-77}
S=$OCTOMX_ROOT/build/toolchain-hello-shots; mkdir -p "$S"
HELLO=tests/toolchain-hello
FONT=$OCTOMX_ROOT/third_party/fonts/tahoma.ttf

echo "== configure + build"
rm -rf build/toolchain-hello-linux build/toolchain-hello-win
tools/configure-linux.sh -S $HELLO build/toolchain-hello-linux >/dev/null
tools/configure-win.sh   -S $HELLO build/toolchain-hello-win   >/dev/null
ninja -C build/toolchain-hello-linux | tail -n 1
ninja -C build/toolchain-hello-win   | tail -n 1

echo "== Linux, Qt Test"
build/toolchain-hello-linux/toolchain_hello_test | grep Totals
echo "== Linux, offscreen"
QT_QPA_PLATFORM=offscreen build/toolchain-hello-linux/toolchain_hello --font "$FONT" --screenshot "$S/linux-offscreen.png"

echo "== Linux, xcb on Xvnc :$N"
tools/xvnc-start.sh "$N"
trap 'tools/xvnc-stop.sh '"$N"' >/dev/null' EXIT
DISPLAY=:$N build/toolchain-hello-linux/toolchain_hello --font "$FONT" --exit-after 8 > "$S/linux-xcb.log" 2>&1 &
sleep 5; tools/x-screenshot.sh "$N" "$S/linux-xvnc.png"; wait
grep -E "Platform|HTTPS|RESULT" "$S/linux-xcb.log"

echo "== Windows: package + wine on Xvnc :$N"
tools/package-win.sh build/toolchain-hello-win build/toolchain-hello-win/dist
D=build/toolchain-hello-win/dist/toolchain_hello
DISPLAY=:$N wine $D/toolchain_hello_test.exe -o "$(winepath -w "$S")\\wine-qtest.txt,txt" >/dev/null 2>&1
grep Totals "$S/wine-qtest.txt"
DISPLAY=:$N wine $D/toolchain_hello.exe --screenshot "$(winepath -w "$S")\\wine.png" 2>/dev/null | tee "$S/wine.log"
DISPLAY=:$N wine $D/toolchain_hello.exe --exit-after 10 >/dev/null 2>&1 &
sleep 7; tools/x-screenshot.sh "$N" "$S/wine-xvnc.png"; wait
echo "screenshots: $(ls "$S"/*.png | tr '\n' ' ')"
