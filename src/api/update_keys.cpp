// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX [upd-1]: the public Ed25519 keys a release's SHA256SUMS.sig is checked against.
//
// The private half of the current key lives only with the release owner
// (keys/release_ed25519.pem, outside the published source, see docs/RELEASE.md). To rotate: make a
// new key, add its public half here (keep the old line), ship one release signed with the OLD key
// that carries both, then sign with the new key and drop the old line in a later release. If the
// old private key is lost, versions that only know it cannot verify anything newer: they offer the
// download page, and the user installs the next version by hand once.
//
// Public key in hex = `openssl pkey -in <key>.pem -pubout -outform DER | tail -c 32 | xxd -p -c 32`;
// the id is the first 16 hex digits of its SHA-256 (tools/release/sign.sh prints both).
#include "mx/update_install.h"

namespace mx::upd {

struct BuiltinKey {
    const char *id;
    const char *hex;
};

// 2026-11-22, OctoMX release key 1
static const BuiltinKey kBuiltinKeys[] = {
    {"6b4bb49f391b4521", "848a9eadbdf3db78102a9f21ec7aff0263829ce7083821219a539cb98abcdd37"},
};

QList<PublicKey> builtinKeys()
{
    QList<PublicKey> out;
    for (const BuiltinKey &k : kBuiltinKeys)
        out << PublicKey{QString::fromLatin1(k.id), QByteArray::fromHex(k.hex)};
    return out;
}

} // namespace mx::upd
