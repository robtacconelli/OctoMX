// SPDX-License-Identifier: GPL-3.0-or-later
// Overlay identity + crypto (OpenSSL 3): Ed25519 signatures, X25519 + HKDF-SHA256 +
// ChaCha20-Poly1305 sealed boxes for end-to-end encrypted relayed private messages. Private.
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>

namespace mx::ovl {

class Identity {
public:
    // Load <dir>/user_ed25519.key + <dir>/user_x25519.key (32 raw bytes each), creating them on
    // first use. Returns false (and *error) if the keys cannot be created or read.
    bool loadOrCreate(const QString &dir, QString *error = nullptr);
    bool isValid() const { return m_edPub.size() == 32 && m_xPub.size() == 32; }

    QByteArray publicKey() const { return m_edPub; }     // Ed25519, 32 bytes
    QString    userId() const { return QString::fromLatin1(m_edPub.toHex()); }  // 64 hex
    QByteArray boxPublicKey() const { return m_xPub; }   // X25519, 32 bytes

    QByteArray sign(const QByteArray &msg) const;        // 64 bytes (empty on failure)
    // Open a box sealed to our X25519 key; empty + false on failure.
    bool open(const QByteArray &box, const QByteArray &aad, QByteArray *plain) const;

private:
    QByteArray m_edPriv, m_edPub, m_xPriv, m_xPub;
};

bool verifySignature(const QByteArray &edPub, const QByteArray &msg, const QByteArray &sig);
// box = eph_pub(32) || nonce(12) || ciphertext || tag(16); key = HKDF(X25519(eph, recipient))
QByteArray sealTo(const QByteArray &recipientXPub, const QByteArray &plain, const QByteArray &aad);

} // namespace mx::ovl
