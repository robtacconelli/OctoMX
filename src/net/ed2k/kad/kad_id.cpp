// SPDX-License-Identifier: GPL-3.0-or-later
#include "kad_id.h"

#include <QtCore/QRandomGenerator>

namespace mx::ed2k::kad {

static quint32 le32(const char *p)
{
    return quint32(quint8(p[0])) | (quint32(quint8(p[1])) << 8) | (quint32(quint8(p[2])) << 16) | (quint32(quint8(p[3])) << 24);
}
static quint32 be32(const char *p)
{
    return (quint32(quint8(p[0])) << 24) | (quint32(quint8(p[1])) << 16) | (quint32(quint8(p[2])) << 8) | quint32(quint8(p[3]));
}

KadId KadId::fromWire(const char *p)
{
    return KadId(le32(p), le32(p + 4), le32(p + 8), le32(p + 12));
}

KadId KadId::fromHash(const QByteArray &b)
{
    if (b.size() < 16) return KadId();
    const char *p = b.constData();
    return KadId(be32(p), be32(p + 4), be32(p + 8), be32(p + 12));
}

KadId KadId::random()
{
    auto *g = QRandomGenerator::global();
    return KadId(g->generate(), g->generate(), g->generate(), g->generate());
}

KadId KadId::randomWithPrefix(const KadId &prefix, int bits)
{
    KadId r = random();
    for (int i = 0; i < bits && i < 128; ++i) r.setBit(i, prefix.bit(i));
    return r;
}

QByteArray KadId::toWire() const
{
    QByteArray b(16, Qt::Uninitialized);
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k < 4; ++k) b[i * 4 + k] = char((m_d[i] >> (8 * k)) & 0xFF);
    return b;
}

QByteArray KadId::toHash() const
{
    QByteArray b(16, Qt::Uninitialized);
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k < 4; ++k) b[i * 4 + k] = char((m_d[i] >> (8 * (3 - k))) & 0xFF);
    return b;
}

QString KadId::toHex() const
{
    return QString::asprintf("%08X%08X%08X%08X", m_d[0], m_d[1], m_d[2], m_d[3]);
}

void KadId::setBit(int i, int v)
{
    if (i < 0 || i > 127) return;
    const quint32 mask = 1u << (31 - (i % 32));
    if (v) m_d[i / 32] |= mask; else m_d[i / 32] &= ~mask;
}

KadId KadId::shiftedLeft(int bits) const
{
    if (bits <= 0) return *this;
    if (bits > 127) return KadId();
    KadId r;
    for (int i = 0; i + bits < 128; ++i) r.setBit(i, bit(i + bits));
    return r;
}

KadId KadId::plus(quint32 v) const
{
    KadId r = *this;
    quint64 carry = v;
    for (int i = 3; i >= 0 && carry; --i) {
        const quint64 s = quint64(r.m_d[i]) + carry;
        r.m_d[i] = quint32(s);
        carry = s >> 32;
    }
    return r;
}

int KadId::leadingZeros() const
{
    for (int i = 0; i < 4; ++i) {
        if (!m_d[i]) continue;
        int n = 0;
        quint32 v = m_d[i];
        while (!(v & 0x80000000u)) { v <<= 1; ++n; }
        return i * 32 + n;
    }
    return 128;
}

} // namespace mx::ed2k::kad
