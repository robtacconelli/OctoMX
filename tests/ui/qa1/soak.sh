#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# tests/ui/qa1/soak.sh <build-dir> <out-dir> [iterations]: [qa-1] long scripted soak of the live demo
# core: every page, every window opened/closed, float/dock, settings changes, searches, chat, resizes.
# Use with the ASan build: tests/ui/qa1/soak.sh build/qa1-asan build/qa1-soak 40
set -uo pipefail
B=$(realpath "$1"); OUT=$(realpath -m "$2"); N=${3:-30}
mkdir -p "$OUT"; rm -rf "$OUT/prof"; mkdir -p "$OUT/prof"
echo '{"ui/chatWarningDisabled":true}' > "$OUT/prof/settings.json"
S=$OUT/soak.script; : > "$S"
for i in $(seq 1 "$N"); do
  cat >> "$S" <<EOS
page networks
subtab other
context-menu serverList 2
close-menus
subtab wpn
page shared
page chat
join OctoMX Help_49717657BABE
say soak line $i http://example.org/$i #c4#coloured
click chatCommands
close-menus
page search
type searchText velorine $i
key searchText Return
wait 1500
context-menu searchResults 1
close-menus
sort searchResults 1 desc
click searchOpenNew
type searchText linux
key searchText Return
wait 200
page transfers
context-menu downloadList 0
close-menus
hotlist on
hotlist off
page bandwidth
float bandwidth
wait 50
dock bandwidth
float transfers
dock transfers
open-browse zqvtra662_44120
open-whois Twirl836_24616
open-pm helping_holly_88120
pm-say helping_holly_88120 soak $i
open-ignored
settings connection
set ui/fontDynamic OctoMX Tahoma,11
settings appearance
close-dialogs
wizard
close-dialogs
close-window browseWindow
close-window whoisWindow
close-window privateMessageWindow
close-window ignoredWindow
resize 700 450
resize 1280 900
set search/autoEnterQueue $((i % 50 + 1))
wait 100
EOS
done
echo quit >> "$S"
export QT_QPA_PLATFORM=offscreen LC_ALL=C.UTF-8 MX_OS_INTEGRATION=fake MX_FAKE_TRAY=1
export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0:halt_on_error=0} UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1}
start=$(date +%s)
timeout 3000 "$B/bin/octomx" --profile "$OUT/prof" --demo --no-wizard --size 1024x689 --script "$S" > "$OUT/soak.log" 2>&1
rc=$?
echo "exit=$rc seconds=$(( $(date +%s) - start )) lines=$(wc -l < "$S") asan=$(grep -c 'ERROR: AddressSanitizer' "$OUT/soak.log") ubsan=$(grep -c 'runtime error' "$OUT/soak.log")"
