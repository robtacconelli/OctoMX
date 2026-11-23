#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Add "SPDX-License-Identifier: GPL-3.0-or-later" as the first comment line of every own source
# file that is published (C/C++ sources and headers, shell/Python scripts, CMake files).
# Run it ONCE on a quiet tree (nobody else editing), then review the diff.
#
#   tools/add-spdx-headers.sh            # dry run: list the files that would change
#   tools/add-spdx-headers.sh --apply    # change them
#
# Idempotent: a file that already has an SPDX-License-Identifier in its first 5 lines is skipped.
# Scripts keep their "#!" line first. Third-party texts (tools/release/files/licenses/, fonts) and
# data files (.json, .md, .txt, ...) are not touched.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
APPLY=0; [ "${1:-}" = --apply ] && APPLY=1
TAG="SPDX-License-Identifier: GPL-3.0-or-later"
WORK=$ROOT/build/spdx-headers; rm -rf "$WORK"; mkdir -p "$WORK"
"$ROOT/tools/make-source-tree.sh" --list "$WORK/list" > "$WORK/files.txt"

n=0; skipped=0
while IFS= read -r f; do
  case "$f" in
    tools/release/files/licenses/*|third_party/*|reference/*) continue ;;
    *.cpp|*.cc|*.c|*.h|*.hpp|*.rc) style=c ;;
    *.sh|*.py|*.cmake|*/CMakeLists.txt|CMakeLists.txt) style=hash ;;
    *) continue ;;
  esac
  p=$ROOT/$f
  if head -n 5 "$p" | grep -q "SPDX-License-Identifier"; then skipped=$((skipped + 1)); continue; fi
  n=$((n + 1))
  if [ "$APPLY" = 0 ]; then echo "would add: $f"; continue; fi
  if [ "$style" = c ]; then line="// $TAG"; else line="# $TAG"; fi
  tmp=$WORK/tmp
  if [ "$style" = hash ] && head -n 1 "$p" | grep -q '^#!'; then
    { head -n 1 "$p"; echo "$line"; tail -n +2 "$p"; } > "$tmp"
  else
    { echo "$line"; cat "$p"; } > "$tmp"
  fi
  cat "$tmp" > "$p"      # keep the file's mode and inode
  echo "added: $f"
done < "$WORK/files.txt"
rm -rf "$WORK"
if [ "$APPLY" = 1 ]; then echo "$n files changed, $skipped already had a header"
else echo "$n files would change, $skipped already have a header (dry run; --apply to change)"; fi
