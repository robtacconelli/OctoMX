// SPDX-License-Identifier: GPL-3.0-or-later
#include "ovl_crypto.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QMessageAuthenticationCode>
#include <QtCore/QSaveFile>

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <memory>

namespace mx::ovl {

// the key-derivation label of private messages: a wire identifier kept for compatibility
static const QByteArray kPmKeyLabel = QByteArrayLiteral("winmx-ng/1 pm");

namespace {

struct PkeyDel { void operator()(EVP_PKEY *p) const { EVP_PKEY_free(p); } };
struct CtxDel { void operator()(EVP_PKEY_CTX *p) const { EVP_PKEY_CTX_free(p); } };
struct MdDel { void operator()(EVP_MD_CTX *p) const { EVP_MD_CTX_free(p); } };
struct CipherDel { void operator()(EVP_CIPHER_CTX *p) const { EVP_CIPHER_CTX_free(p); } };
using Pkey = std::unique_ptr<EVP_PKEY, PkeyDel>;

QByteArray randomBytes(int n)
{
    QByteArray b(n, Qt::Uninitialized);
    if (RAND_bytes(reinterpret_cast<unsigned char *>(b.data()), n) != 1) return QByteArray();
    return b;
}

QByteArray rawPub(EVP_PKEY *k)
{
    size_t len = 32;
    QByteArray out(32, Qt::Uninitialized);
    if (EVP_PKEY_get_raw_public_key(k, reinterpret_cast<unsigned char *>(out.data()), &len) != 1 || len != 32)
        return QByteArray();
    return out;
}

Pkey privKey(int type, const QByteArray &raw)
{
    return Pkey(EVP_PKEY_new_raw_private_key(type, nullptr, reinterpret_cast<const unsigned char *>(raw.constData()),
                                             static_cast<size_t>(raw.size())));
}

bool loadKey(const QString &path, int type, QByteArray *priv, QByteArray *pub, QString *error)
{
    QFile f(path);
    if (f.exists()) {
        if (!f.open(QIODevice::ReadOnly)) { if (error) *error = f.errorString(); return false; }
        QByteArray raw = f.readAll();
        if (raw.size() == 32) {
            Pkey k = privKey(type, raw);
            if (k) {
                *priv = raw;
                *pub = rawPub(k.get());
                return pub->size() == 32;
            }
        }
        // corrupt: regenerate below (keep the bad one aside for inspection)
        f.close();
        QFile::remove(path + QStringLiteral(".bad"));
        QFile::rename(path, path + QStringLiteral(".bad"));
    }
    QByteArray raw = randomBytes(32);
    Pkey k = raw.size() == 32 ? privKey(type, raw) : Pkey();
    if (!k) { if (error) *error = QStringLiteral("key generation failed"); return false; }
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly) || out.write(raw) != 32 || !out.commit()) {
        if (error) *error = QStringLiteral("cannot write %1").arg(path);
        return false;
    }
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    *priv = raw;
    *pub = rawPub(k.get());
    return pub->size() == 32;
}

QByteArray hkdf(const QByteArray &ikm, const QByteArray &salt, const QByteArray &info, int len)
{
    const QByteArray prk = QMessageAuthenticationCode::hash(ikm, salt, QCryptographicHash::Sha256);
    QByteArray okm, t;
    for (char i = 1; okm.size() < len; ++i) {
        t = QMessageAuthenticationCode::hash(t + info + QByteArray(1, i), prk, QCryptographicHash::Sha256);
        okm += t;
    }
    return okm.left(len);
}

QByteArray x25519(const QByteArray &priv, const QByteArray &peerPub)
{
    Pkey me = privKey(EVP_PKEY_X25519, priv);
    Pkey other(EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr,
                                           reinterpret_cast<const unsigned char *>(peerPub.constData()), 32));
    if (!me || !other) return QByteArray();
    std::unique_ptr<EVP_PKEY_CTX, CtxDel> ctx(EVP_PKEY_CTX_new(me.get(), nullptr));
    if (!ctx || EVP_PKEY_derive_init(ctx.get()) != 1 || EVP_PKEY_derive_set_peer(ctx.get(), other.get()) != 1)
        return QByteArray();
    size_t len = 32;
    QByteArray out(32, Qt::Uninitialized);
    if (EVP_PKEY_derive(ctx.get(), reinterpret_cast<unsigned char *>(out.data()), &len) != 1 || len != 32)
        return QByteArray();
    if (out == QByteArray(32, '\0')) return QByteArray();   // low-order point
    return out;
}

bool aead(bool enc, const QByteArray &key, const QByteArray &nonce, const QByteArray &aad,
          const QByteArray &in, QByteArray *out, QByteArray *tag)
{
    std::unique_ptr<EVP_CIPHER_CTX, CipherDel> c(EVP_CIPHER_CTX_new());
    if (!c) return false;
    const auto *k = reinterpret_cast<const unsigned char *>(key.constData());
    const auto *n = reinterpret_cast<const unsigned char *>(nonce.constData());
    if (EVP_CipherInit_ex(c.get(), EVP_chacha20_poly1305(), nullptr, nullptr, nullptr, enc ? 1 : 0) != 1) return false;
    if (EVP_CIPHER_CTX_ctrl(c.get(), EVP_CTRL_AEAD_SET_IVLEN, 12, nullptr) != 1) return false;
    if (EVP_CipherInit_ex(c.get(), nullptr, nullptr, k, n, enc ? 1 : 0) != 1) return false;
    int len = 0;
    if (!aad.isEmpty() &&
        EVP_CipherUpdate(c.get(), nullptr, &len, reinterpret_cast<const unsigned char *>(aad.constData()), aad.size()) != 1)
        return false;
    out->resize(in.size());
    if (!in.isEmpty() &&
        EVP_CipherUpdate(c.get(), reinterpret_cast<unsigned char *>(out->data()), &len,
                         reinterpret_cast<const unsigned char *>(in.constData()), in.size()) != 1)
        return false;
    if (!enc && EVP_CIPHER_CTX_ctrl(c.get(), EVP_CTRL_AEAD_SET_TAG, 16, tag->data()) != 1) return false;
    int fin = 0;
    unsigned char dummy[16];
    if (EVP_CipherFinal_ex(c.get(), dummy, &fin) != 1) return false;
    if (enc) {
        tag->resize(16);
        if (EVP_CIPHER_CTX_ctrl(c.get(), EVP_CTRL_AEAD_GET_TAG, 16, tag->data()) != 1) return false;
    }
    return true;
}

} // namespace

bool Identity::loadOrCreate(const QString &dir, QString *error)
{
    QDir().mkpath(dir);
    return loadKey(dir + QStringLiteral("/user_ed25519.key"), EVP_PKEY_ED25519, &m_edPriv, &m_edPub, error) &&
           loadKey(dir + QStringLiteral("/user_x25519.key"), EVP_PKEY_X25519, &m_xPriv, &m_xPub, error);
}

QByteArray Identity::sign(const QByteArray &msg) const
{
    Pkey k = privKey(EVP_PKEY_ED25519, m_edPriv);
    if (!k) return QByteArray();
    std::unique_ptr<EVP_MD_CTX, MdDel> md(EVP_MD_CTX_new());
    if (!md || EVP_DigestSignInit(md.get(), nullptr, nullptr, nullptr, k.get()) != 1) return QByteArray();
    size_t len = 64;
    QByteArray sig(64, Qt::Uninitialized);
    if (EVP_DigestSign(md.get(), reinterpret_cast<unsigned char *>(sig.data()), &len,
                       reinterpret_cast<const unsigned char *>(msg.constData()), static_cast<size_t>(msg.size())) != 1 ||
        len != 64)
        return QByteArray();
    return sig;
}

bool verifySignature(const QByteArray &edPub, const QByteArray &msg, const QByteArray &sig)
{
    if (edPub.size() != 32 || sig.size() != 64) return false;
    Pkey k(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                                       reinterpret_cast<const unsigned char *>(edPub.constData()), 32));
    if (!k) return false;
    std::unique_ptr<EVP_MD_CTX, MdDel> md(EVP_MD_CTX_new());
    if (!md || EVP_DigestVerifyInit(md.get(), nullptr, nullptr, nullptr, k.get()) != 1) return false;
    return EVP_DigestVerify(md.get(), reinterpret_cast<const unsigned char *>(sig.constData()), 64,
                            reinterpret_cast<const unsigned char *>(msg.constData()),
                            static_cast<size_t>(msg.size())) == 1;
}

QByteArray sealTo(const QByteArray &recipientXPub, const QByteArray &plain, const QByteArray &aad)
{
    if (recipientXPub.size() != 32) return QByteArray();
    QByteArray ephPriv = randomBytes(32);
    Pkey eph = privKey(EVP_PKEY_X25519, ephPriv);
    if (!eph) return QByteArray();
    const QByteArray ephPub = rawPub(eph.get());
    const QByteArray shared = x25519(ephPriv, recipientXPub);
    if (shared.isEmpty() || ephPub.size() != 32) return QByteArray();
    const QByteArray key = hkdf(shared, ephPub + recipientXPub, kPmKeyLabel, 32);
    const QByteArray nonce = randomBytes(12);
    QByteArray ct, tag;
    if (!aead(true, key, nonce, aad, plain, &ct, &tag)) return QByteArray();
    return ephPub + nonce + ct + tag;
}

bool Identity::open(const QByteArray &box, const QByteArray &aad, QByteArray *plain) const
{
    if (box.size() < 32 + 12 + 16 || m_xPriv.size() != 32) return false;
    const QByteArray ephPub = box.left(32);
    const QByteArray nonce = box.mid(32, 12);
    const QByteArray ct = box.mid(44, box.size() - 44 - 16);
    QByteArray tag = box.right(16);
    const QByteArray shared = x25519(m_xPriv, ephPub);
    if (shared.isEmpty()) return false;
    const QByteArray key = hkdf(shared, ephPub + m_xPub, kPmKeyLabel, 32);
    return aead(false, key, nonce, aad, ct, plain, &tag);
}

} // namespace mx::ovl
