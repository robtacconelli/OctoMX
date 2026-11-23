#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# tools/release/manifest.sh <package dir> <version x.y.z> <platform win64|linux-x86_64>
# [upd-1] Writes <package dir>/MANIFEST.txt: "sha256 size path" of every file of the package (sorted,
# '/' separated, MANIFEST.txt itself not listed). The self-update reads it twice: the manifest of the
# installed package says which files belong to OctoMX (only those are ever moved), the one of the new
# package what must arrive (every size and hash is checked before the swap). Called by
# tools/release.sh after the notices and before the archive is made.
set -euo pipefail
[ $# -eq 3 ] || { echo "usage: $0 <package dir> <version> <platform>" >&2; exit 2; }
P=$(realpath "$1"); V=$2; PL=$3
[ -d "$P" ] || { echo "no package dir $P" >&2; exit 1; }
case "$PL" in win64|linux-x86_64) ;; *) echo "unknown platform $PL" >&2; exit 2 ;; esac
OUT=$P/MANIFEST.txt
rm -f "$OUT"
BAD=$(cd "$P" && find . -mindepth 1 \( -type l -o \( ! -type f ! -type d \) \) | head -n3)
[ -z "$BAD" ] || { echo "symbolic links or special files in the package: $BAD" >&2; exit 1; }
# names the updater never moves (they belong to the user): a package must not contain them
RES=$(cd "$P" && find . -mindepth 1 -maxdepth 1 | sed 's|^\./||' | grep -iE '^(portable\.txt|profile|downloads|incoming|\.old-.*|\.new-.*|\.update-.*)$' || true)
[ -z "$RES" ] || { echo "the package contains reserved names: $RES" >&2; exit 1; }
TMP=$(mktemp)
{
  echo "# OctoMX package manifest: sha256, size and path of every file of this package"
  echo "version $V"
  echo "platform $PL"
  (cd "$P" && find . -type f ! -path ./MANIFEST.txt -printf '%P\n' | LC_ALL=C sort | while IFS= read -r f; do
     printf '%s %s %s\n' "$(sha256sum "$f" | cut -d' ' -f1)" "$(stat -c %s "$f")" "$f"
   done)
} > "$TMP"
mv "$TMP" "$OUT"
chmod 0644 "$OUT"
echo "manifest: $OUT, $(grep -c '^[0-9a-f]\{64\} ' "$OUT") files, version $V, $PL"
