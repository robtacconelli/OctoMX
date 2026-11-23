#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# tools/dev-worktree.sh <name> [<base-ref>]
#
# A second working copy for parallel work: a git worktree <checkout>-wt/<name> on the new branch
# work/<name> (from <base-ref>, default HEAD), wired to this checkout's toolchain, third-party
# prefixes and compiler cache through symbolic links, so nothing is downloaded or built twice.
# Build inside it as usual (tools/configure-linux.sh build/linux, tools/configure-win.sh
# build/win): with the same build directory names in every worktree the compiler cache is shared.
# Remove it with: git worktree remove <path> && git branch -D work/<name>
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
[ $# -ge 1 ] || { echo "usage: $0 <name> [<base-ref>]" >&2; exit 2; }
NAME=$1
BASE=${2:-HEAD}
WT="${ROOT}-wt/$NAME"
[ ! -e "$WT" ] || { echo "$WT exists already" >&2; exit 1; }
mkdir -p "${ROOT}-wt"
git -C "$ROOT" worktree add -q -b "work/$NAME" "$WT" "$BASE"

link() {   # <path relative to the checkout>: link it into the worktree when the checkout has it
    [ -e "$ROOT/$1" ] || return 0
    mkdir -p "$(dirname "$WT/$1")"
    [ -e "$WT/$1" ] || ln -s "$ROOT/$1" "$WT/$1"
}
# toolchain: everything but env.sh (part of the source) and tmp (one per worktree)
for e in "$ROOT"/toolchain/*; do
    b=$(basename "$e")
    case "$b" in env.sh|tmp) ;; *) link "toolchain/$b" ;; esac
done
mkdir -p "$WT/toolchain/tmp"
for d in src linux win; do link "third_party/$d"; done
# local, unpublished material the tests or the people working here use
for e in "$ROOT"/reference/*; do
    b=$(basename "$e")
    [ "$b" = README.md ] || link "reference/$b"
done
for p in docs/briefs docs/status docs/WORKER_GUIDE.md; do link "$p"; done

echo "$WT (branch work/$NAME from $(git -C "$ROOT" rev-parse --short "$BASE"))"
