#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# tools/release/sign.sh <SHA256SUMS> [<private key .pem>]
# [upd-1] Signs the exact bytes of SHA256SUMS with the Ed25519 release key and writes
# SHA256SUMS.sig next to it (the format src/api/update_install.cpp reads; the header line, written
# below, is a format identifier kept for compatibility, so every installed copy reads it):
#     <header>
#     key <first 16 hex digits of SHA-256 of the raw public key>
#     ed25519 <base64 signature>
# Key: the argument, else $MX_RELEASE_KEY, else <checkout>/keys/release_ed25519.pem. The private key
# is only read by openssl; it is never printed, copied or logged. Exit 3 = no key (the caller says
# so loudly), 1 = any other failure. The public key and its id are printed, so they can be compared
# with the table in src/api/update_keys.cpp.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
SUMS=$(realpath "$1")
KEY=${2:-${MX_RELEASE_KEY:-$ROOT/keys/release_ed25519.pem}}
[ -f "$SUMS" ] || { echo "no $SUMS" >&2; exit 1; }
[ -r "$KEY" ] || { echo "no release key at $KEY" >&2; exit 3; }
PERM=$(stat -c %a "$KEY")
case "$PERM" in 600|400) ;; *) echo "WARNING: $KEY has mode $PERM, should be 600" >&2 ;; esac
OUT=$SUMS.sig
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
openssl pkeyutl -sign -inkey "$KEY" -rawin -in "$SUMS" -out "$TMP/sig.bin" 2>"$TMP/err" \
  || { echo "openssl could not sign: $(head -c 300 "$TMP/err")" >&2; exit 1; }
openssl pkey -in "$KEY" -pubout -outform DER -out "$TMP/pub.der" 2>/dev/null || { echo "cannot read the public key" >&2; exit 1; }
PUBHEX=$(tail -c 32 "$TMP/pub.der" | xxd -p -c 32)
KEYID=$(tail -c 32 "$TMP/pub.der" | sha256sum | cut -c1-16)
[ "$(stat -c %s "$TMP/sig.bin")" = 64 ] || { echo "unexpected signature length" >&2; exit 1; }
# check with openssl before anything is published
openssl pkeyutl -verify -pubin -inkey "$TMP/pub.der" -keyform DER -rawin -in "$SUMS" -sigfile "$TMP/sig.bin" >/dev/null 2>&1 \
  || { echo "the new signature does not verify" >&2; exit 1; }
{ echo "winmx-ng-signature 1"; echo "key $KEYID"; echo "ed25519 $(base64 -w0 "$TMP/sig.bin")"; } > "$OUT"
grep -q "\"$KEYID\", \"$PUBHEX\"" "$ROOT/src/api/update_keys.cpp" \
  && echo "signed $(basename "$SUMS") -> $(basename "$OUT"): key $KEYID (public $PUBHEX), built into src/api/update_keys.cpp" \
  || echo "signed $(basename "$SUMS") -> $(basename "$OUT"): key $KEYID (public $PUBHEX) is NOT in src/api/update_keys.cpp: programs will refuse it"
