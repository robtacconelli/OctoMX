#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# tools/check-no-dashes.sh: fail when a published text file contains an em dash (U+2014), an en
# dash (U+2013) or the ellipsis character (U+2026). Owner request: "remove the [em dash] from
# all texts, I prefer the comma" (number ranges use a plain hyphen, the ellipsis is three dots).
#
#   tools/check-no-dashes.sh [<source-root>]
#
# Checks the files that are published (the same exclusions as .gitignore: build trees, toolchain,
# third-party sources, reference screenshots, internal notes), not the third-party licence texts
# (LICENSE, COPYING*). Binary files are skipped. Prints file:line for each hit; exit 1 if any.
# Wired into ctest as "tools_check_no_dashes".
set -euo pipefail
ROOT=$(cd "${1:-$(dirname "${BASH_SOURCE[0]}")/..}" && pwd)
cd "$ROOT"
EM=$'\xe2\x80\x94'; EN=$'\xe2\x80\x93'; ELL=$'\xe2\x80\xa6'
hits=$(find . \
    \( -path ./build -o -path ./dist -o -path ./.snapshots -o -path ./shots -o -path ./.git \
       -o -path ./docs/status -o -path ./docs/briefs -o -name wineprefix -o -name .wine \
       -o -name __pycache__ -o -name .vscode -o -name .idea \) -prune -o \
    \( -path './toolchain/*' ! -path ./toolchain/env.sh \) -prune -o \
    \( -path './third_party/*' ! -path ./third_party/BUILD.md ! -path ./third_party/fonts \
       ! -path './third_party/fonts/*' \) -prune -o \
    \( -path './reference/*' ! -path ./reference/README.md \) -prune -o \
    -type f ! -name LICENSE ! -name 'COPYING*' ! -name NOTES.md ! -name WORKER_GUIDE.md \
    ! -name '*.log' ! -name '*.py[co]' ! -name '*.swp' ! -name '*~' ! -name compile_commands.json \
    ! -name CMakeUserPresets.json -print0 |
  xargs -0 grep -I -n -e "$EM" -e "$EN" -e "$ELL" 2>/dev/null || true)
if [ -n "$hits" ]; then
    echo "$hits" | cut -c1-200
    echo "check-no-dashes: $(echo "$hits" | wc -l) line(s) with an em dash, en dash or ellipsis character" >&2
    exit 1
fi
echo "check-no-dashes: no em dash, en dash or ellipsis character in the published texts"
