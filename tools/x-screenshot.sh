#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Grab the root window of an X display into a PNG (xwd + ffmpeg, no ImageMagick).
#   tools/x-screenshot.sh <display, e.g. 70 or :70> <out.png>
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../toolchain/env.sh"
D=${1:?usage: $0 <display> <out.png>}; OUT=${2:?usage: $0 <display> <out.png>}
D=":${D#:}"
mkdir -p "$(dirname "$OUT")"
xwd -root -silent -display "$D" | ffmpeg -nostdin -loglevel error -y -f xwd_pipe -i - -frames:v 1 "$OUT"
echo "$OUT"
