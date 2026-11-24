#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# tools/mac/test.sh: run the test suite of a Mac build tree and summarise it.
#
#   tools/mac/test.sh <build-dir> <out-dir> [label] [extra ctest args...]
#
# QT_QPA_PLATFORM defaults to offscreen (set it to cocoa for the real platform). Writes
# <out-dir>/ctest-<label>.log (full), <out-dir>/ctest-<label>.xml (JUnit) and
# <out-dir>/summary-<label>.txt (one line per test, then the totals). The tests that do not
# run on macOS and why are in tools/mac/test-skips.txt (name<TAB>reason); exit 1 if any other fails.
set -uo pipefail
source "$(cd "$(dirname "$0")" && pwd)/env.sh"
[ $# -ge 2 ] || { echo "usage: $0 <build-dir> <out-dir> [label] [ctest args...]" >&2; exit 2; }
B=$(cd "$1" && pwd); OUT=$2; LABEL=${3:-offscreen}; shift 3 2>/dev/null || shift $#
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
export QT_QPA_PLATFORM=${QT_QPA_PLATFORM:-offscreen}
mkdir -p "$MX_OS_ROOT"
SKIPS=$OCTOMX_ROOT/tools/mac/test-skips.txt
EXCL=$(grep -v '^#' "$SKIPS" 2>/dev/null | cut -f1 | grep . | paste -sd'|' -)
ARGS=(--test-dir "$B" --timeout "${MX_TEST_TIMEOUT:-900}" -j "${MX_TEST_JOBS:-3}" --output-junit "$OUT/ctest-$LABEL.xml" --output-on-failure
      --test-output-size-passed 4000000 --test-output-size-failed 4000000)
[ -n "$EXCL" ] && ARGS+=(-E "^($EXCL)\$")
echo "tests ($LABEL, QT_QPA_PLATFORM=$QT_QPA_PLATFORM, $(uname -m)): ctest ${ARGS[*]} $*"
ctest "${ARGS[@]}" "$@" > "$OUT/ctest-$LABEL.log" 2>&1
rc=$?
{
    grep -E '^ *[0-9]+/[0-9]+ Test +#' "$OUT/ctest-$LABEL.log" | sed -E 's/^ *[0-9]+\/[0-9]+ Test +#[0-9]+: //; s/ \.+/ /'
    # QTest totals per executable (function counts) from the full output
    # per suite: the QTest totals and the skipped functions with their reasons (from the JUnit file,
    # which keeps the whole output of every test)
    python3 -I - "$OUT/ctest-$LABEL.xml" <<'PY'
import re, sys, xml.etree.ElementTree as ET
tp = tf = ts = 0
for tc in ET.parse(sys.argv[1]).getroot().iter('testcase'):
    out = tc.findtext('system-out') or ''
    t = re.findall(r'Totals: (\d+) passed, (\d+) failed, (\d+) skipped', out)
    if t:
        p, f, s = map(int, t[-1]); tp += p; tf += f; ts += s
        print('suite %-32s %3d passed %2d failed %2d skipped' % (tc.get('name'), p, f, s))
    else:
        print('suite %-32s (not a QTest program: %s)' % (tc.get('name'), tc.get('status')))
    for m in re.finditer(r'^SKIP   : (\S+) (.*)$', out, re.M):
        print('    skipped %s: %s' % (m.group(1), m.group(2)[:160]))
print('QTest functions: %d passed, %d failed, %d skipped' % (tp, tf, ts))
PY
    grep -E 'tests passed|tests failed' "$OUT/ctest-$LABEL.log"
    [ -n "$EXCL" ] && echo "not run on macOS (tools/mac/test-skips.txt): $(echo "$EXCL" | tr '|' ' ')"
} > "$OUT/summary-$LABEL.txt"
grep -v -E ' Passed |^suite ' "$OUT/summary-$LABEL.txt" | head -n 80
grep -c ' Passed ' "$OUT/summary-$LABEL.txt" | sed 's/^/passed suites: /'
if [ $rc != 0 ]; then
    # the first failing lines of each failed test, to read in the CI log
    grep -E '^FAIL!|^QFATAL|Loc: \[|Totals:.*[1-9][0-9]* failed' "$OUT/ctest-$LABEL.log" | head -n 80 | cut -c1-300
    # timing tests (rate limiter, engine, HTTP) can miss on a busy CI machine: the failed ones run
    # once more, alone; the summary names every test that needed it (MX_TEST_RERUN=0: no second run)
    if [ "${MX_TEST_RERUN:-1}" = 1 ]; then
        FAILED=$(grep -E '^[[:space:]]*[0-9]+ - .* \(' "$OUT/ctest-$LABEL.log" | sed -E 's/^[[:space:]]*[0-9]+ - ([^ ]+) .*/\1/' | tr '\n' ' ')
        ctest --test-dir "$B" --rerun-failed --output-on-failure --timeout "${MX_TEST_TIMEOUT:-900}" > "$OUT/ctest-$LABEL-rerun.log" 2>&1
        rc=$?
        { echo "first run failed: $FAILED"
          echo "second run (alone): $(grep -E 'tests passed|tests failed' "$OUT/ctest-$LABEL-rerun.log")"; } | tee -a "$OUT/summary-$LABEL.txt"
        [ $rc = 0 ] || grep -E '^FAIL!|^QFATAL|Loc: \[' "$OUT/ctest-$LABEL-rerun.log" | head -n 40 | cut -c1-300
    fi
fi
exit $rc
