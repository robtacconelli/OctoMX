#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copy exactly what would be published (every file git would add under the rules in .gitignore)
# into <dest>, so it can be inspected and test-built without a git repository.
#
#   tools/make-source-tree.sh [--list] <dest>
#
# Uses git itself to apply .gitignore (a throwaway git dir in <dest>.gitdir, removed afterwards;
# the user's/system git configuration and global excludes are ignored). <dest> must not exist or
# be empty, and must not be inside a published path. --list only prints the file list.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
LIST=0
if [ "${1:-}" = --list ]; then LIST=1; shift; fi
[ $# -eq 1 ] || { echo "usage: $0 [--list] <dest>" >&2; exit 2; }
DEST=$(realpath -m -- "$1")
[ -f "$ROOT/.gitignore" ] || { echo "no .gitignore in $ROOT" >&2; exit 1; }
if [ -e "$DEST" ] && [ -n "$(ls -A "$DEST" 2>/dev/null)" ]; then echo "$DEST exists and is not empty" >&2; exit 1; fi

GD=$DEST.gitdir
rm -rf "$GD"
export GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null
git init -q --bare "$GD"
LISTFILE=$GD/files.lst
# (from the checkout root: ls-files lists relative to the current directory) [upd-1]
(cd "$ROOT" && git --git-dir="$GD" --work-tree="$ROOT" -c core.excludesFile=/dev/null \
    ls-files --others --exclude-standard -z) > "$LISTFILE"
# a destination inside the tree must itself be ignored, or the copy would include itself
case "$DEST/" in "$ROOT"/*)
  rel=${DEST#"$ROOT"/}
  if tr '\0' '\n' < "$LISTFILE" | grep -q "^$rel/"; then echo "$DEST is inside a published path" >&2; rm -rf "$GD"; exit 1; fi ;;
esac

# [upd-1] the release signing key must never be published: refuse keys/, *.pem and any file that
# holds a private key, whatever .gitignore says
LEAK=$(tr '\0' '\n' < "$LISTFILE" | grep -E '^keys/|\.pem$' | head -n3 || true)
[ -z "$LEAK" ] || { echo "refusing: private key material would be published: $LEAK" >&2; rm -rf "$GD"; exit 1; }
LEAK=$(cd "$ROOT" && xargs -0 grep -lIE -- '-----BEGIN ([A-Z]+ )?PRIVATE KEY-----' < "$LISTFILE" 2>/dev/null | head -n3 || true)
[ -z "$LEAK" ] || { echo "refusing: a private key would be published: $LEAK" >&2; rm -rf "$GD"; exit 1; }

n=$(tr -cd '\0' < "$LISTFILE" | wc -c)
if [ "$LIST" = 1 ]; then tr '\0' '\n' < "$LISTFILE"; rm -rf "$GD"; exit 0; fi
mkdir -p "$DEST"
(cd "$ROOT" && tar --no-recursion --null -T "$LISTFILE" -cf - ) | tar -C "$DEST" -xf -
rm -rf "$GD"
echo "$n files, $(du -sh "$DEST" | cut -f1) -> $DEST"
