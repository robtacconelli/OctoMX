#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# tools/check-content.sh: no names of real commercial works in the published tree. Owner's rule:
# "never upload any copyrighted content (search, transfers, etc)": no content is shipped, and the
# demo data, tests, tools, documents and comments must not show the names of real recordings, films,
# games, artists or studios as if they were shared files either. They use invented names, public-domain
# works or freely licensed ones (the Blender open films, Linux distributions).
#
#   tools/check-content.sh [-v] [<source-root>]
#
# Fails when a published text file has a line with an entry of tools/content-denylist.txt (matched
# case-insensitively as a whole word, see that file) that tools/content-allowlist.txt does not
# explain. The allow-list has one entry per line:   <path pattern> :: <line regex> :: <reason>
# A hit is allowed when its path matches the pattern (shell pattern, * also matches /) and the line
# matches the regex (POSIX extended, case sensitive). An entry that matches nothing is an error as
# well, so the list cannot rot. The two lists themselves are not checked. -v prints every allowed
# line. Pictures and other binary files are not looked at: read the screenshots yourself.
# Checks the published files (same exclusions as tools/check-no-dashes.sh); works in a git checkout
# and in a tools/make-source-tree.sh snapshot. Wired into ctest as "tools_check_content".
set -euo pipefail
VERBOSE=0
if [ "${1:-}" = -v ]; then VERBOSE=1; shift; fi
ROOT=$(cd "${1:-$(dirname "${BASH_SOURCE[0]}")/..}" && pwd)
cd "$ROOT"
WORDS=tools/content-denylist.txt
LIST=tools/content-allowlist.txt
[ -f "$WORDS" ] || { echo "check-content: no $WORDS" >&2; exit 1; }
[ -f "$LIST" ] || { echo "check-content: no $LIST" >&2; exit 1; }

published() {   # NUL-separated list of the published files
    find . \
        \( -path ./build -o -path ./dist -o -path ./.snapshots -o -path ./shots -o -path ./.git \
           -o -path ./docs/status -o -path ./docs/briefs -o -name wineprefix -o -name .wine \
           -o -name __pycache__ -o -name .vscode -o -name .idea \) -prune -o \
        \( -path './toolchain/*' ! -path ./toolchain/env.sh \) -prune -o \
        \( -path './third_party/*' ! -path ./third_party/BUILD.md ! -path ./third_party/fonts \
           ! -path './third_party/fonts/*' \) -prune -o \
        \( -path './reference/*' ! -path ./reference/README.md \) -prune -o \
        -type f ! -path "./$WORDS" ! -path "./$LIST" ! -name NOTES.md ! -name WORKER_GUIDE.md \
        ! -name '*.log' ! -name '*.py[co]' ! -name '*.swp' ! -name '*~' ! -name compile_commands.json \
        ! -name CMakeUserPresets.json -print0
}

# the entries as regular expressions: literal characters, separators inside an entry match any run
# of blanks, dots, dashes and underscores (or none)
names=(); pats=()
while IFS= read -r raw || [ -n "$raw" ]; do
    line=$(printf '%s' "$raw" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')
    case "$line" in ''|'#'*) continue ;; esac
    p=$(printf '%s' "$line" | sed 's/[][\\^$*+?(){}|/]/\\&/g; s/[[:space:]._-][[:space:]._-]*/[[:space:]._-]*/g')
    names+=("$line"); pats+=("$p")
done < "$WORDS"
[ ${#pats[@]} -gt 0 ] || { echo "check-content: $WORDS has no entries" >&2; exit 1; }
alt=$(IFS='|'; echo "${pats[*]}")
B1='(^|[^[:alnum:]])'; B2='([^[:alnum:]]|$)'
RE="$B1($alt)$B2"

globs=(); res=(); reasons=(); used=()
n=0
while IFS= read -r raw || [ -n "$raw" ]; do
    case "$raw" in ''|'#'*) continue ;; esac
    line=$raw
    g=${line%% :: *}; line=${line#* :: }
    r=${line%% :: *}; why=${line#* :: }
    if [ "$g" = "$raw" ] || [ "$r" = "$why" ] || [ -z "$why" ]; then
        echo "check-content: bad allow-list line: $raw" >&2; exit 1
    fi
    globs[n]=$g; res[n]=$r; reasons[n]=$why; used[n]=0
    n=$((n + 1))
done < "$LIST"

bad=0; allowed=0
shopt -s nocasematch
while IFS= read -r hit; do
    [ -n "$hit" ] || continue
    f=${hit%%:*}; rest=${hit#*:}; text=${rest#*:}
    f=${f#./}
    # which entry (for the message)
    which=""
    for ((k = 0; k < ${#pats[@]}; k++)); do
        if [[ $text =~ $B1(${pats[k]})$B2 ]]; then which=${names[k]}; break; fi
    done
    ok=0
    shopt -u nocasematch
    for ((i = 0; i < n; i++)); do
        # shellcheck disable=SC2053
        if [[ $f == ${globs[i]} ]] && [[ $text =~ ${res[i]} ]]; then
            ok=1; used[i]=$((used[i] + 1)); allowed=$((allowed + 1))
            [ $VERBOSE = 1 ] && echo "allowed: $f:${rest%%:*}: [$which] ${text:0:150}"
            break
        fi
    done
    shopt -s nocasematch
    if [ $ok = 0 ]; then
        echo "$f:${rest%%:*}: [$which] ${text:0:180}"
        bad=$((bad + 1))
    fi
done < <(published | xargs -0 grep -I -n -i -E -- "$RE" 2>/dev/null || true)
shopt -u nocasematch
stale=0
for ((i = 0; i < n; i++)); do
    if [ "${used[i]}" = 0 ]; then
        echo "check-content: allow-list entry matches nothing (remove it): ${globs[i]} :: ${res[i]}" >&2
        stale=$((stale + 1))
    fi
done
echo "check-content: ${#names[@]} names in $WORDS, $allowed line(s) allowed by $n entr(ies) of $LIST"
if [ $bad -gt 0 ] || [ $stale -gt 0 ]; then
    echo "check-content: $bad line(s) with the name of a real work or maker not explained by $LIST, $stale stale entr(ies)" >&2
    exit 1
fi
echo "check-content: no name of a real commercial work in the published texts"
