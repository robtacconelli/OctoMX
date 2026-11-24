#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# tools/mac/test-cocoa.sh: the UI test programs once more with the real Cocoa platform.
#
#   tools/mac/test-cocoa.sh <build-dir> <out-dir> [name-regex]
#
# ctest gives every UI test QT_QPA_PLATFORM=offscreen (tests/ui/CMakeLists.txt), so this runs the
# test programs directly with the same environment except the platform (cocoa, as the program
# itself uses on a Mac). Writes <out-dir>/cocoa/<test>.txt and
# <out-dir>/summary-cocoa.txt; exit 1 if any fails. The tests in tools/mac/test-skips.txt and
# tools/mac/cocoa-skips.txt (name<TAB>reason) are not run.
set -uo pipefail
source "$(cd "$(dirname "$0")" && pwd)/env.sh"
[ $# -ge 2 ] || { echo "usage: $0 <build-dir> <out-dir> [name-regex]" >&2; exit 2; }
B=$(cd "$1" && pwd); mkdir -p "$2/cocoa"; OUT=$(cd "$2" && pwd)
RE=${3:-^ui_}
run_limited() { perl -e 'alarm shift; exec @ARGV or die "exec: $!"' "$@"; }
SK=$(cat "$OCTOMX_ROOT/tools/mac/test-skips.txt" "$OCTOMX_ROOT/tools/mac/cocoa-skips.txt" 2>/dev/null | grep -v '^#' | cut -f1)
mkdir -p "$B/os-root-cocoa"
PASS=0; FAIL=0; SKIP=0
: > "$OUT/summary-cocoa.txt"
for t in $(ctest --test-dir "$B" -N -R "$RE" | sed -n 's/^ *Test *#[0-9]*: //p'); do
    if echo "$SK" | grep -qx "$t"; then SKIP=$((SKIP + 1)); echo "$t skipped" >> "$OUT/summary-cocoa.txt"; continue; fi
    exe=$B/bin/${t#ui_}
    [ -x "$exe" ] || { echo "$t: no program $exe" >> "$OUT/summary-cocoa.txt"; continue; }
    # QTEST_DISABLE_CRASH_HANDLER: a crash leaves a macOS crash report (with the stack) instead
    ( cd "$B" && QTEST_DISABLE_CRASH_HANDLER=1 QT_QPA_PLATFORM=cocoa LC_ALL=C.UTF-8 MX_DEMO_FROZEN=1 MX_PORT_MAPPING=0 MX_OS_ROOT="$B/os-root-cocoa" \
        run_limited 300 "$exe" > "$OUT/cocoa/$t.txt" 2>&1 )
    rc=$?
    tot=$(grep -E '^Totals:' "$OUT/cocoa/$t.txt" | tail -n 1)
    if [ $rc = 0 ]; then PASS=$((PASS + 1)); echo "$t Passed  $tot" >> "$OUT/summary-cocoa.txt"
    else FAIL=$((FAIL + 1)); echo "$t FAILED (exit $rc)  $tot" >> "$OUT/summary-cocoa.txt"
         grep -E '^FAIL!|^QFATAL|Loc: \[' "$OUT/cocoa/$t.txt" | head -n 6 | cut -c1-250 | sed 's/^/    /' >> "$OUT/summary-cocoa.txt"; fi
done
echo "cocoa: $PASS passed, $FAIL failed, $SKIP skipped" >> "$OUT/summary-cocoa.txt"
grep -v ' Passed ' "$OUT/summary-cocoa.txt"
[ $FAIL = 0 ]
