#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# tools/mac/shots.sh: screenshots of the real program (demo core, frozen data) for the look check.
#
#   tools/mac/shots.sh <octomx program> <out-dir>
#
# The program is OctoMX.app/Contents/MacOS/octomx (or bin/octomx on Linux, for the same shots to
# compare). On a Mac the real Cocoa platform is used (QT_QPA_PLATFORM unset); set QT_SCALE_FACTOR=2
# for the 2x (Retina-like) shots. The same scripts on Linux with QT_QPA_PLATFORM=offscreen give the
# reference images. Every run uses its own throwaway profile under <out-dir>/.work and MX_OS_ROOT
# there (no change to the user's settings, autostart or link handlers). Bash 3.2, no GNU tools.
set -uo pipefail
[ $# -eq 2 ] || { echo "usage: $0 <octomx program> <out-dir>" >&2; exit 2; }
EXE=$1
mkdir -p "$2"; OUT=$(cd "$2" && pwd)
WORK=$OUT/.work
rm -rf "$WORK"; mkdir -p "$WORK"
export LC_ALL=en_US.UTF-8 MX_DIGIT_GROUPING=space MX_OS_ROOT=$WORK/os-root MX_SOUND_OFF=1
# a process that does not end in time is killed (macOS has no timeout(1))
run_limited() { perl -e 'alarm shift; exec @ARGV or die "exec: $!"' "$@"; }
FAILED=0

# shot <name> <WxH> <settings-json-or-empty> <script lines...>
shot() {
    local name=$1 size=${MX_SHOTS_SIZE:-$2} settings=$3; shift 3   # MX_SHOTS_SIZE: one size for all (2x shots fit the screen)
    local prof=$WORK/prof-$name script=$WORK/$name.script
    mkdir -p "$prof"
    [ -n "$settings" ] && echo "$settings" > "$prof/settings.json"
    : > "$script"
    for l in "$@"; do echo "$l" >> "$script"; done
    printf 'shot %s\nquit\n' "$OUT/$name.png" >> "$script"
    if run_limited 90 "$EXE" --profile "$prof" --demo-frozen --no-wizard --size "$size" --script "$script" > "$WORK/$name.log" 2>&1 \
       && [ -s "$OUT/$name.png" ]; then
        echo "ok: $name.png $(ls "$OUT/$name".*.png 2>/dev/null | sed 's|.*/||' | tr '\n' ' ')"
    else
        echo "FAILED: $name"; tail -n 5 "$WORK/$name.log"; FAILED=$((FAILED + 1))
    fi
}

shot networks       1024x689 "" "page networks" "subtab wpn" "wait 300"
shot networks_menu  1024x689 "" "page networks" "subtab other" "context-menu serverList 2" "wait 300"
shot search_bbb     1024x714 "" "page search" "type searchText big buck bunny" "key searchText Return" "wait 900"
shot search_menu    1024x714 "" "page search" "type searchText big buck bunny" "key searchText Return" "wait 900" \
                                "context-menu searchResults 0" "wait 300"
shot transfers      1024x689 "" "page transfers" "wait 400"
shot transfers_menu 1024x689 "" "page transfers" "context-menu downloadList 1" "wait 300"
shot shared         1024x689 "" "page shared" "wait 400"
shot chat           1024x689 "" "page chat" "wait 400"
shot bandwidth      1024x689 "" "page bandwidth" "wait 600"
shot help_menu      1024x689 "" "page networks" "help-menu" "wait 300"
shot about          1024x689 "" "page networks" "click btnHelp" "wait 300" "menu-pick About OctoMX..." "wait 500"
shot settings       1024x689 "" "settings misc/system" "wait 400"
shot settings_launch 1024x689 "" "settings launching/preview" "wait 400"
echo "shots in $OUT: $FAILED failed"
[ $FAILED = 0 ]
