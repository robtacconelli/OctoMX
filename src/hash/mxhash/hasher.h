// SPDX-License-Identifier: GPL-3.0-or-later
// mxhash: streaming hash primitives (Qt Core only; usable by the core and by network plugins).
// Every hasher follows the same pattern: construct, update() any number of times, finish() once
// (returns the raw digest), reset() to reuse.
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QCryptographicHash>
#include <QtCore/QtGlobal>
#include <cstdint>

namespace mx::hash {

class Hasher {
public:
    virtual ~Hasher() = default;
    virtual void update(const char *data, qint64 len) = 0;
    void update(const QByteArray &b) { update(b.constData(), b.size()); }
    virtual QByteArray finish() = 0;   // the digest (raw bytes); call once
    virtual void reset() = 0;
};

// RFC 1320 MD4 (16 bytes). Building block of the eD2k hash.
class Md4 : public Hasher {
public:
    Md4() { reset(); }
    void update(const char *data, qint64 len) override;
    using Hasher::update;
    QByteArray finish() override;
    void reset() override;
    static QByteArray hash(const QByteArray &data);
private:
    void block(const unsigned char *p);
    quint32 m_state[4];
    quint64 m_length = 0;            // bytes
    unsigned char m_buf[64];
    int m_bufLen = 0;
};

// Tiger/192 (Anderson & Biham, 3 passes, original 0x01 padding, the variant THEX/TTH uses).
class Tiger : public Hasher {
public:
    Tiger() { reset(); }
    void update(const char *data, qint64 len) override;
    using Hasher::update;
    QByteArray finish() override;      // 24 bytes
    void reset() override;
    static QByteArray hash(const QByteArray &data);
    static QByteArray hash(const char *data, qint64 len);
private:
    quint64 m_state[3];
    quint64 m_length = 0;
    unsigned char m_buf[64];
    int m_bufLen = 0;
};

// SHA-1 / SHA-256 / MD5 ... through QCryptographicHash.
class CryptoHasher : public Hasher {
public:
    explicit CryptoHasher(QCryptographicHash::Algorithm a) : m_h(a) {}
    void update(const char *data, qint64 len) override { m_h.addData(QByteArrayView(data, len)); }
    using Hasher::update;
    QByteArray finish() override { return m_h.result(); }
    void reset() override { m_h.reset(); }
private:
    QCryptographicHash m_h;
};

} // namespace mx::hash
