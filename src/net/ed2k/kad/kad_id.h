// SPDX-License-Identifier: GPL-3.0-or-later
// 128-bit Kademlia identifiers (KadIDs, keyword/file targets) with the XOR metric.
//
// Representation follows the eMule protocol: four 32-bit chunks, chunk 0 most significant, bit 0
// = most significant bit. On the wire (and in nodes.dat / obfuscation keys) the ID is written as
// the four chunks, each little-endian ("wire" form). File hashes and MD4 keyword hashes enter Kad
// big-endian: the 16 hash bytes are read as one big-endian number ("hash" form), so the wire bytes
// of a file target are the hash bytes with every 4-byte group reversed.
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QHashFunctions>
#include <QtCore/QString>

namespace mx::ed2k::kad {

class KadId {
public:
    KadId() = default;
    KadId(quint32 a, quint32 b, quint32 c, quint32 d) : m_d{a, b, c, d} {}

    static KadId fromWire(const char *p16);
    static KadId fromWire(const QByteArray &b) { return b.size() >= 16 ? fromWire(b.constData()) : KadId(); }
    static KadId fromHash(const QByteArray &be16);       // file hash / MD4 digest bytes
    static KadId random();
    static KadId allOnes() { return KadId(0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu); }
    // first `bits` bits from `prefix`, the rest random (eMule CUInt128(prefix, bits))
    static KadId randomWithPrefix(const KadId &prefix, int bits);

    QByteArray toWire() const;
    QByteArray toHash() const;                           // inverse of fromHash
    QString toHex() const;                               // 32 hex digits, most significant first

    quint32 chunk(int i) const { return m_d[i & 3]; }
    int bit(int i) const { return (i < 0 || i > 127) ? 0 : int((m_d[i / 32] >> (31 - (i % 32))) & 1u); }
    void setBit(int i, int v);
    bool isZero() const { return !(m_d[0] | m_d[1] | m_d[2] | m_d[3]); }
    KadId shiftedLeft(int bits) const;
    KadId plus(quint32 v) const;
    // index of the first set bit (0 = MSB), 128 for zero
    int leadingZeros() const;

    KadId operator^(const KadId &o) const { return KadId(m_d[0] ^ o.m_d[0], m_d[1] ^ o.m_d[1], m_d[2] ^ o.m_d[2], m_d[3] ^ o.m_d[3]); }
    bool operator==(const KadId &o) const { return m_d[0] == o.m_d[0] && m_d[1] == o.m_d[1] && m_d[2] == o.m_d[2] && m_d[3] == o.m_d[3]; }
    bool operator!=(const KadId &o) const { return !(*this == o); }
    bool operator<(const KadId &o) const
    {
        for (int i = 0; i < 4; ++i) if (m_d[i] != o.m_d[i]) return m_d[i] < o.m_d[i];
        return false;
    }
    bool operator>(const KadId &o) const { return o < *this; }
    bool operator<=(const KadId &o) const { return !(o < *this); }

private:
    quint32 m_d[4] = {0, 0, 0, 0};
};

inline size_t qHash(const KadId &k, size_t seed = 0) noexcept
{
    return qHashMulti(seed, k.chunk(0), k.chunk(1), k.chunk(2), k.chunk(3));
}

} // namespace mx::ed2k::kad
