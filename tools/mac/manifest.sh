#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# tools/mac/manifest.sh <package dir> <version x.y.z> <platform macos-universal|macos-arm64|macos-x86_64>
# [mac-1] Writes <package dir>/MANIFEST.txt in the format of tools/release/manifest.sh ("sha256 size
# path" of every regular file, sorted, MANIFEST.txt itself not listed), with BSD tools (shasum, stat
# -f). An app bundle needs symbolic links (the framework layout): they are listed as comment lines
# "# link <path> -> <target>" (octomx --verify-manifest skips comments; tools/mac/check-manifest.py
# checks them too). The manifest sits beside OctoMX.app, outside the signed bundle: inside, its own
# hash would be part of the signature it lists.
set -euo pipefail
[ $# -eq 3 ] || { echo "usage: $0 <package dir> <version> <platform>" >&2; exit 2; }
P=$(cd "$1" && pwd); V=$2; PL=$3
case "$PL" in macos-universal|macos-arm64|macos-x86_64) ;; *) echo "unknown platform $PL" >&2; exit 2 ;; esac
OUT=$P/MANIFEST.txt
rm -f "$OUT"
BAD=$(cd "$P" && find . -mindepth 1 ! -type f ! -type d ! -type l | head -n3)
[ -z "$BAD" ] || { echo "special files in the package: $BAD" >&2; exit 1; }
RES=$(cd "$P" && find . -mindepth 1 -maxdepth 1 | sed 's|^\./||' | grep -iE '^(portable\.txt|profile|downloads|incoming|\.old-.*|\.new-.*|\.update-.*)$' || true)
[ -z "$RES" ] || { echo "the package contains reserved names: $RES" >&2; exit 1; }
TMP=$(mktemp "${TMPDIR:-/tmp}/octomx-manifest.XXXXXX")
{
  echo "# OctoMX package manifest: sha256, size and path of every file of this package"
  echo "version $V"
  echo "platform $PL"
  (cd "$P" && find . -type f ! -path ./MANIFEST.txt | sed 's|^\./||' | LC_ALL=C sort | while IFS= read -r f; do
     printf '%s %s %s\n' "$(shasum -a 256 "$f" | cut -d' ' -f1)" "$(stat -f %z "$f")" "$f"
   done)
  (cd "$P" && find . -type l | sed 's|^\./||' | LC_ALL=C sort | while IFS= read -r f; do
     printf '# link %s -> %s\n' "$f" "$(readlink "$f")"
   done)
} > "$TMP"
mv "$TMP" "$OUT"
chmod 0644 "$OUT"
echo "manifest: $OUT, $(grep -c '^[0-9a-f]\{64\} ' "$OUT") files, $(grep -c '^# link ' "$OUT") links, version $V, $PL"
