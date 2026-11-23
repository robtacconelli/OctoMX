// SPDX-License-Identifier: GPL-3.0-or-later
// Parts of this file are adapted from eMule 0.50a (Copyright (C) the eMule team,
// https://www.emule-project.net; EncryptedStreamSocket.cpp), GPL-2.0-or-later;
// this file is distributed under GPL-3.0-or-later. See NOTICE.
#include "ed2k_crypto.h"
#include "ed2k_io.h"
#include "ed2k_opcodes.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QRandomGenerator>

namespace mx::ed2k {

// ---------------------------------------------------------------- RC4
void Rc4::setKey(const QByteArray &key, bool discard)
{
    for (int i = 0; i < 256; ++i) m_s[i] = quint8(i);
    m_x = m_y = 0;
    if (key.isEmpty()) { m_valid = false; return; }
    quint8 j = 0;
    const int n = int(key.size());
    for (int i = 0; i < 256; ++i) {
        j = quint8(j + m_s[i] + quint8(key[i % n]));
        std::swap(m_s[i], m_s[j]);
    }
    m_valid = true;
    if (discard) {
        char junk[1024] = {};
        process(junk, sizeof(junk));
    }
}

void Rc4::process(char *d, qsizetype n)
{
    quint8 x = m_x, y = m_y;
    for (qsizetype i = 0; i < n; ++i) {
        x = quint8(x + 1);
        y = quint8(y + m_s[x]);
        std::swap(m_s[x], m_s[y]);
        d[i] = char(quint8(d[i]) ^ m_s[quint8(m_s[x] + m_s[y])]);
    }
    m_x = x; m_y = y;
}

// ---------------------------------------------------------------- BigUInt
void BigUInt::trim() { while (!m_l.isEmpty() && m_l.last() == 0) m_l.removeLast(); }

BigUInt BigUInt::fromBigEndian(const QByteArray &b)
{
    BigUInt r;
    const int n = int(b.size());
    r.m_l.resize((n + 3) / 4);
    for (int i = 0; i < n; ++i) {
        const int bytePos = n - 1 - i;           // significance
        r.m_l[bytePos / 4] |= quint32(quint8(b[i])) << (8 * (bytePos % 4));
    }
    r.trim();
    return r;
}

QByteArray BigUInt::toBigEndian(int bytes) const
{
    QByteArray out(bytes, '\0');
    for (int i = 0; i < bytes; ++i) {
        const int limb = i / 4;
        if (limb >= m_l.size()) break;
        out[bytes - 1 - i] = char((m_l[limb] >> (8 * (i % 4))) & 0xFF);
    }
    return out;
}

int BigUInt::bitLength() const
{
    if (m_l.isEmpty()) return 0;
    quint32 top = m_l.last();
    int bits = 0;
    while (top) { ++bits; top >>= 1; }
    return (int(m_l.size()) - 1) * 32 + bits;
}
bool BigUInt::bit(int i) const
{
    const int limb = i / 32;
    if (limb >= m_l.size()) return false;
    return (m_l[limb] >> (i % 32)) & 1;
}
int BigUInt::cmp(const BigUInt &a, const BigUInt &b)
{
    if (a.m_l.size() != b.m_l.size()) return a.m_l.size() < b.m_l.size() ? -1 : 1;
    for (int i = int(a.m_l.size()) - 1; i >= 0; --i)
        if (a.m_l[i] != b.m_l[i]) return a.m_l[i] < b.m_l[i] ? -1 : 1;
    return 0;
}

BigUInt BigUInt::mulMod(const BigUInt &a, const BigUInt &b, const BigUInt &m)
{
    // full product
    QVector<quint32> prod(a.m_l.size() + b.m_l.size() + 1, 0);
    for (int i = 0; i < a.m_l.size(); ++i) {
        quint64 carry = 0;
        for (int j = 0; j < b.m_l.size(); ++j) {
            quint64 t = quint64(a.m_l[i]) * b.m_l[j] + prod[i + j] + carry;
            prod[i + j] = quint32(t);
            carry = t >> 32;
        }
        int k = i + int(b.m_l.size());
        while (carry) { quint64 t = quint64(prod[k]) + carry; prod[k] = quint32(t); carry = t >> 32; ++k; }
    }
    BigUInt p; p.m_l = prod; p.trim();
    // reduce: binary long division remainder
    BigUInt r;
    r.m_l.reserve(m.m_l.size() + 1);
    for (int i = p.bitLength() - 1; i >= 0; --i) {
        // r = r*2 + bit
        quint32 carry = p.bit(i) ? 1 : 0;
        for (int k = 0; k < r.m_l.size(); ++k) {
            quint32 nc = r.m_l[k] >> 31;
            r.m_l[k] = (r.m_l[k] << 1) | carry;
            carry = nc;
        }
        if (carry) r.m_l.append(carry);
        if (cmp(r, m) >= 0) {
            // r -= m
            qint64 borrow = 0;
            for (int k = 0; k < r.m_l.size(); ++k) {
                qint64 t = qint64(r.m_l[k]) - (k < m.m_l.size() ? qint64(m.m_l[k]) : 0) - borrow;
                borrow = t < 0 ? 1 : 0;
                r.m_l[k] = quint32(t + (borrow ? (qint64(1) << 32) : 0));
            }
            r.trim();
        }
    }
    return r;
}

BigUInt BigUInt::powMod(const BigUInt &base, const BigUInt &exp, const BigUInt &mod)
{
    BigUInt result = fromU32(1);
    BigUInt b = mulMod(base, fromU32(1), mod);
    for (int i = exp.bitLength() - 1; i >= 0; --i) {
        result = mulMod(result, result, mod);
        if (exp.bit(i)) result = mulMod(result, b, mod);
    }
    return result;
}

QByteArray ed2kDhPrime()
{
    static const unsigned char p[96] = {
        0xF2,0xBF,0x52,0xC5,0x5F,0x58,0x7A,0xDD,0x53,0x71,0xA9,0x36,0xE8,0x86,0xEB,0x3C,
        0x62,0x17,0xA3,0x3E,0xC3,0x4C,0xB4,0x0D,0xC7,0x3A,0x41,0xA6,0x43,0xAF,0xFC,0xE7,
        0x21,0xFC,0x28,0x63,0x66,0x53,0x5B,0xDB,0xCE,0x25,0x9F,0x22,0x86,0xDA,0x4A,0x91,
        0xB2,0x07,0xCB,0xAA,0x52,0x55,0xD4,0xF6,0x1C,0xCE,0xAE,0xD4,0x5A,0xD5,0xE0,0x74,
        0x7D,0xF7,0x78,0x18,0x28,0x10,0x5F,0x34,0x0F,0x76,0x23,0x87,0xF8,0x8B,0x28,0x91,
        0x42,0xFB,0x42,0x68,0x8F,0x05,0x15,0x0F,0x54,0x8B,0x5F,0x43,0x6A,0xF7,0x0D,0xF3,
    };
    return QByteArray(reinterpret_cast<const char *>(p), 96);
}

// ---------------------------------------------------------------- StreamCrypt
static QByteArray md5(const QByteArray &b) { return QCryptographicHash::hash(b, QCryptographicHash::Md5); }

quint8 StreamCrypt::randomMarker()
{
    for (int i = 0; i < 128; ++i) {
        quint8 m = quint8(QRandomGenerator::system()->bounded(256));
        if (m != PROT_EDONKEY && m != PROT_EMULE && m != PROT_PACKED) return m;
    }
    return 0x01;
}

QByteArray StreamCrypt::padding(int maxLen)
{
    const int n = maxLen > 0 ? int(QRandomGenerator::system()->bounded(maxLen + 1)) : 0;
    QByteArray p(n, '\0');
    for (int i = 0; i < n; ++i) p[i] = char(QRandomGenerator::system()->bounded(256));
    return p;
}

void StreamCrypt::fail(const QString &why)
{
    m_state = Failed;
    m_error = why;
    m_in.clear();
    m_pending.clear();
}

void StreamCrypt::setupClientKeys(const QByteArray &hash, quint32 rnd, bool weAreA)
{
    Writer kd;
    kd.hash16(hash).u8(MAGICVALUE_REQUESTER).u32(rnd);
    QByteArray keyReq = kd.data();
    QByteArray keySrv = keyReq;
    keySrv[16] = char(MAGICVALUE_SERVER);
    if (weAreA) { m_send.setKey(md5(keyReq)); m_recv.setKey(md5(keySrv)); }
    else        { m_recv.setKey(md5(keyReq)); m_send.setKey(md5(keySrv)); }
}

void StreamCrypt::startClient(const QByteArray &target, int maxPadding, quint32 rnd, bool fixedRandom)
{
    if (!fixedRandom) rnd = QRandomGenerator::system()->generate();
    m_maxPad = qBound(0, maxPadding, 255);
    setupClientKeys(target, rnd, true);
    Writer head;
    head.u8(randomMarker()).u32(rnd);
    Writer enc;
    const QByteArray pad = padding(m_maxPad);
    enc.u32(MAGICVALUE_SYNC).u8(ENM_OBFUSCATION).u8(ENM_OBFUSCATION).u8(quint8(pad.size())).bytes(pad);
    QByteArray e = enc.take();
    m_send.process(e.data(), e.size());
    m_out.append(head.data());
    m_out.append(e);
    m_state = Negotiating;
    m_step = A_WaitMagic;
    m_want = 4;
}

void StreamCrypt::startServer()
{
    // a: 128 random bits
    QByteArray a(16, '\0');
    for (int i = 0; i < 16; ++i) a[i] = char(QRandomGenerator::system()->bounded(256));
    a[0] = char(quint8(a[0]) | 0x80);
    m_dhA = a;
    const BigUInt p = BigUInt::fromBigEndian(ed2kDhPrime());
    const BigUInt ga = BigUInt::powMod(BigUInt::fromU32(2), BigUInt::fromBigEndian(a), p);
    m_out.append(char(randomMarker()));
    m_out.append(ga.toBigEndian(96));
    // eMule appends 0..15 unencrypted random bytes here; we send none (always valid).
    m_state = Negotiating;
    m_step = S_WaitDh;
    m_want = 96;
}

void StreamCrypt::startIncoming(const QByteArray &ourHash, bool require, int maxPadding)
{
    m_ourHash = ourHash;
    m_requireObf = require;
    m_maxPad = qBound(0, maxPadding, 255);
    m_state = Detecting;
    m_step = None;
}

void StreamCrypt::becomeReady()
{
    m_state = Encrypting;
    m_step = None;
    if (!m_pending.isEmpty()) {
        QByteArray p; p.swap(m_pending);
        m_send.process(p.data(), p.size());
        m_out.append(p);
    }
}

QByteArray StreamCrypt::send(const QByteArray &plain)
{
    switch (m_state) {
    case Off: return plain;
    case Encrypting: { QByteArray b = plain; m_send.process(b.data(), b.size()); return b; }
    case Detecting:
    case Negotiating:
        if (m_pending.size() + plain.size() < 4 * 1024 * 1024) m_pending.append(plain);
        return QByteArray();
    case Failed: return QByteArray();
    }
    return QByteArray();
}

QByteArray StreamCrypt::receive(const QByteArray &raw)
{
    if (m_state == Off) return raw;
    if (m_state == Failed) return QByteArray();
    if (m_state == Encrypting) { QByteArray b = raw; m_recv.process(b.data(), b.size()); return b; }

    QByteArray data = raw;
    if (m_state == Detecting) {
        if (data.isEmpty()) return QByteArray();
        const quint8 first = quint8(data[0]);
        if (first == PROT_EDONKEY || first == PROT_EMULE || first == PROT_PACKED) {
            if (m_requireObf) { fail(QStringLiteral("peer is not obfuscated but we require it")); return QByteArray(); }
            m_state = Off;
            m_detectedPlain = true;
            if (!m_pending.isEmpty()) { m_out.append(m_pending); m_pending.clear(); }
            return data;
        }
        data.remove(0, 1);
        m_state = Negotiating;
        m_step = B_WaitRandom;
        m_want = 4;
    }

    // Negotiating: consume exactly the handshake bytes, decrypting the encrypted parts.
    qsizetype pos = 0;
    while (m_state == Negotiating && pos < data.size()) {
        const qsizetype take = qMin<qsizetype>(m_want - m_in.size(), data.size() - pos);
        QByteArray chunk = data.mid(pos, take);
        pos += take;
        const bool encrypted = (m_step != B_WaitRandom && m_step != S_WaitDh);
        if (encrypted) m_recv.process(chunk.data(), chunk.size());
        m_in.append(chunk);
        if (m_in.size() < m_want) break;
        Reader r(m_in);
        switch (m_step) {
        case A_WaitMagic:
            if (r.u32() != MAGICVALUE_SYNC) { fail(QStringLiteral("obfuscation: bad magic from peer")); break; }
            m_step = A_WaitMethodPad; m_want = 2; break;
        case A_WaitMethodPad: {
            const quint8 method = r.u8();
            const quint8 padLen = r.u8();
            if (method != ENM_OBFUSCATION) { fail(QStringLiteral("obfuscation: unsupported method")); break; }
            if (padLen == 0) { becomeReady(); break; }
            m_step = A_WaitPadding; m_want = padLen; break;
        }
        case A_WaitPadding: becomeReady(); break;
        case B_WaitRandom: {
            const quint32 rnd = r.u32();
            setupClientKeys(m_ourHash, rnd, false);
            m_step = B_WaitMagic; m_want = 4; break;
        }
        case B_WaitMagic:
            if (r.u32() != MAGICVALUE_SYNC) { fail(QStringLiteral("obfuscation: bad magic (wrong user hash?)")); break; }
            m_step = B_WaitMethods; m_want = 3; break;
        case B_WaitMethods: {
            r.u8(); r.u8();
            const quint8 padLen = r.u8();
            if (padLen > 0) { m_step = B_WaitPadding; m_want = padLen; break; }
        }
            Q_FALLTHROUGH();
        case B_WaitPadding: {
            Writer resp;
            const QByteArray pad = padding(qMin(m_maxPad, 16));
            resp.u32(MAGICVALUE_SYNC).u8(ENM_OBFUSCATION).u8(quint8(pad.size())).bytes(pad);
            QByteArray e = resp.take();
            m_send.process(e.data(), e.size());
            m_out.append(e);
            becomeReady();
            break;
        }
        case S_WaitDh: {
            const BigUInt p = BigUInt::fromBigEndian(ed2kDhPrime());
            const BigUInt gb = BigUInt::fromBigEndian(m_in.left(96));
            const BigUInt s = BigUInt::powMod(gb, BigUInt::fromBigEndian(m_dhA), p);
            m_dhA.clear();
            QByteArray key = s.toBigEndian(96);
            key.append(char(MAGICVALUE_REQUESTER));
            m_send.setKey(md5(key));
            key[96] = char(MAGICVALUE_SERVER);
            m_recv.setKey(md5(key));
            m_step = S_WaitMagic; m_want = 4; break;
        }
        case S_WaitMagic:
            if (r.u32() != MAGICVALUE_SYNC) { fail(QStringLiteral("server obfuscation: bad magic")); break; }
            m_step = S_WaitMethods; m_want = 3; break;
        case S_WaitMethods: {
            r.u8(); r.u8();
            const quint8 padLen = r.u8();
            if (padLen > 16) { fail(QStringLiteral("server obfuscation: padding too long")); break; }
            if (padLen > 0) { m_step = S_WaitPadding; m_want = padLen; break; }
        }
            Q_FALLTHROUGH();
        case S_WaitPadding: {
            Writer resp;
            resp.u32(MAGICVALUE_SYNC).u8(ENM_OBFUSCATION).u8(0);
            QByteArray e = resp.take();
            m_send.process(e.data(), e.size());
            m_out.append(e);
            becomeReady();
            break;
        }
        case None: break;
        }
        m_in.clear();
    }
    if (m_state == Encrypting && pos < data.size()) {
        QByteArray rest = data.mid(pos);
        m_recv.process(rest.data(), rest.size());
        return rest;
    }
    return QByteArray();
}

} // namespace mx::ed2k
