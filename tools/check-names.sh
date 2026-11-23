#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# tools/check-names.sh: the program is called OctoMX, and that is the only name it uses. Fail when a
# published file has a line with "WinMX" (any case) that tools/names-allowlist.txt does not explain,
# or when a published file or folder has "winmx" in its path.
#
#   tools/check-names.sh [-v] [<source-root>]
#
# Two rules:
#   1. every line with "WinMX" (any case) must match an entry of the allow-list (any class);
#   2. a line with the phrase made of "WinMX" and "NG" (any case, joined by a blank, "-", "_", "." or
#      nothing) or with the word that starts with "former" and ends in "ly" may only match a class L
#      entry: compatibility literals (identifier strings and the tests that exercise them). There is
#      no class L entry for prose.
# The allow-list has one entry per line:   <class> :: <path pattern> :: <line regex> :: <reason>
#   class L  compatibility literals (rule 2), byte for byte: wire and format identifiers, the legacy
#            names that copies installed before 5.0 left on a computer
#   class 2  other identifiers and legacy names with "WinMX" (folder, file and package names)
#   class 3  the original WinMX as the model of the interface (the statement of independence too)
# A hit is allowed when its path matches the pattern (shell pattern, * also matches /) and the line
# matches the regex (POSIX extended, case sensitive). An entry that matches nothing is an error as
# well, so the list cannot rot. The entry lines of the allow-list itself are exempt from rule 2 (they
# name the literals); its comments are not. -v prints every allowed line with its class.
# Checks the published files (same exclusions as tools/check-no-dashes.sh); works in a git checkout
# and in a tools/make-source-tree.sh snapshot. Wired into ctest as "tools_check_names".
set -euo pipefail
VERBOSE=0
if [ "${1:-}" = -v ]; then VERBOSE=1; shift; fi
ROOT=$(cd "${1:-$(dirname "${BASH_SOURCE[0]}")/..}" && pwd)
cd "$ROOT"
LIST=tools/names-allowlist.txt
[ -f "$LIST" ] || { echo "check-names: no $LIST" >&2; exit 1; }
# rule 2: the phrase and the word (written so that this file does not contain them)
PHRASE='[Ww][Ii][Nn][Mm][Xx][ ._-]?[Nn][Gg]|(^|[^[:alnum:]_])[Ff][Oo][Rr][Mm][Ee][Rr][Ll][Yy]([^[:alnum:]_]|$)'

published() {   # NUL-separated list of the published files
    find . \
        \( -path ./build -o -path ./dist -o -path ./.snapshots -o -path ./shots -o -path ./.git \
           -o -path ./docs/status -o -path ./docs/briefs -o -name wineprefix -o -name .wine \
           -o -name __pycache__ -o -name .vscode -o -name .idea \) -prune -o \
        \( -path './toolchain/*' ! -path ./toolchain/env.sh \) -prune -o \
        \( -path './third_party/*' ! -path ./third_party/BUILD.md ! -path ./third_party/fonts \
           ! -path './third_party/fonts/*' \) -prune -o \
        \( -path './reference/*' ! -path ./reference/README.md \) -prune -o \
        -type f ! -name NOTES.md ! -name WORKER_GUIDE.md \
        ! -name '*.log' ! -name '*.py[co]' ! -name '*.swp' ! -name '*~' ! -name compile_commands.json \
        ! -name CMakeUserPresets.json -print0
}

classes=(); globs=(); res=(); reasons=(); used=()
n=0
while IFS= read -r raw || [ -n "$raw" ]; do
    case "$raw" in ''|'#'*) continue ;; esac
    line=$raw
    c=${line%% :: *}; line=${line#* :: }
    g=${line%% :: *}; line=${line#* :: }
    r=${line%% :: *}; why=${line#* :: }
    if [ "$c" = "$raw" ] || [ "$r" = "$why" ] || { [ "$c" != L ] && [ "$c" != 2 ] && [ "$c" != 3 ]; }; then
        echo "check-names: bad allow-list line: $raw" >&2; exit 1
    fi
    classes[n]=$c; globs[n]=$g; res[n]=$r; reasons[n]=$why; used[n]=0
    n=$((n + 1))
done < "$LIST"

bad=0
declare -A perClass=([L]=0 [2]=0 [3]=0)
declare -A perEntries=([L]=0 [2]=0 [3]=0)
# file and folder names
paths=$(published | tr '\0' '\n' | grep -i winmx || true)
if [ -n "$paths" ]; then
    echo "$paths" | sed 's|^\./||; s|$|: the old name in a file or folder name|'
    bad=$((bad + $(echo "$paths" | wc -l)))
fi
# lines
while IFS= read -r hit; do
    [ -n "$hit" ] || continue
    f=${hit%%:*}; rest=${hit#*:}; text=${rest#*:}
    f=${f#./}
    rule2=0
    if [[ $text =~ $PHRASE ]]; then
        rule2=1
        # the entry lines of the allow-list name the literals
        if [ "$f" = "$LIST" ] && [[ $text != '#'* ]]; then rule2=0; fi
    fi
    ok=0
    for ((i = 0; i < n; i++)); do
        [ $rule2 = 1 ] && [ "${classes[i]}" != L ] && continue
        # shellcheck disable=SC2053
        if [[ $f == ${globs[i]} ]] && [[ $text =~ ${res[i]} ]]; then
            ok=1; used[i]=$((used[i] + 1)); perClass[${classes[i]}]=$((perClass[${classes[i]}] + 1))
            [ $VERBOSE = 1 ] && echo "class ${classes[i]}: $f:${rest%%:*}: ${text:0:150}"
            break
        fi
    done
    if [ $ok = 0 ]; then
        if [ $rule2 = 1 ]; then
            echo "$f:${rest%%:*}: (not a listed compatibility literal) ${text:0:160}"
        else
            echo "$f:${rest%%:*}: ${text:0:180}"
        fi
        bad=$((bad + 1))
    fi
done < <(published | xargs -0 grep -I -n -i -E -- "winmx|$PHRASE" 2>/dev/null || true)
stale=0
for ((i = 0; i < n; i++)); do
    perEntries[${classes[i]}]=$((perEntries[${classes[i]}] + 1))
    if [ "${used[i]}" = 0 ]; then
        echo "check-names: allow-list entry matches nothing (remove it): ${classes[i]} :: ${globs[i]} :: ${res[i]}" >&2
        stale=$((stale + 1))
    fi
done
echo "check-names: allowed class L (compatibility literals): ${perClass[L]} line(s), ${perEntries[L]} entries"
echo "check-names: allowed class 2 (legacy folder, file and package names): ${perClass[2]} line(s), ${perEntries[2]} entries"
echo "check-names: allowed class 3 (the original WinMX as the model of the interface): ${perClass[3]} line(s), ${perEntries[3]} entries"
if [ $bad -gt 0 ] || [ $stale -gt 0 ]; then
    echo "check-names: $bad line(s) or path(s) not explained by $LIST, $stale stale entr(ies)" >&2
    exit 1
fi
echo "check-names: every remaining \"WinMX\" in the published tree is explained by $LIST"
