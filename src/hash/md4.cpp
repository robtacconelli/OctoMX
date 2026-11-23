// SPDX-License-Identifier: GPL-3.0-or-later
// RFC 1320 MD4.
#include "mxhash/hasher.h"

#include <cstring>

namespace mx::hash {

namespace {
inline quint32 rol(quint32 x, int s) { return (x << s) | (x >> (32 - s)); }
inline quint32 F(quint32 x, quint32 y, quint32 z) { return (x & y) | (~x & z); }
inline quint32 G(quint32 x, quint32 y, quint32 z) { return (x & y) | (x & z) | (y & z); }
inline quint32 H(quint32 x, quint32 y, quint32 z) { return x ^ y ^ z; }
} // namespace

void Md4::reset()
{
    m_state[0] = 0x67452301u;
    m_state[1] = 0xefcdab89u;
    m_state[2] = 0x98badcfeu;
    m_state[3] = 0x10325476u;
    m_length = 0;
    m_bufLen = 0;
}

void Md4::block(const unsigned char *p)
{
    quint32 x[16];
    for (int i = 0; i < 16; ++i)
        x[i] = quint32(p[i * 4]) | (quint32(p[i * 4 + 1]) << 8) | (quint32(p[i * 4 + 2]) << 16)
             | (quint32(p[i * 4 + 3]) << 24);
    quint32 a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3];

#define R1(a, b, c, d, k, s) a = rol(a + F(b, c, d) + x[k], s)
#define R2(a, b, c, d, k, s) a = rol(a + G(b, c, d) + x[k] + 0x5a827999u, s)
#define R3(a, b, c, d, k, s) a = rol(a + H(b, c, d) + x[k] + 0x6ed9eba1u, s)
    R1(a, b, c, d, 0, 3);  R1(d, a, b, c, 1, 7);  R1(c, d, a, b, 2, 11);  R1(b, c, d, a, 3, 19);
    R1(a, b, c, d, 4, 3);  R1(d, a, b, c, 5, 7);  R1(c, d, a, b, 6, 11);  R1(b, c, d, a, 7, 19);
    R1(a, b, c, d, 8, 3);  R1(d, a, b, c, 9, 7);  R1(c, d, a, b, 10, 11); R1(b, c, d, a, 11, 19);
    R1(a, b, c, d, 12, 3); R1(d, a, b, c, 13, 7); R1(c, d, a, b, 14, 11); R1(b, c, d, a, 15, 19);

    R2(a, b, c, d, 0, 3);  R2(d, a, b, c, 4, 5);  R2(c, d, a, b, 8, 9);   R2(b, c, d, a, 12, 13);
    R2(a, b, c, d, 1, 3);  R2(d, a, b, c, 5, 5);  R2(c, d, a, b, 9, 9);   R2(b, c, d, a, 13, 13);
    R2(a, b, c, d, 2, 3);  R2(d, a, b, c, 6, 5);  R2(c, d, a, b, 10, 9);  R2(b, c, d, a, 14, 13);
    R2(a, b, c, d, 3, 3);  R2(d, a, b, c, 7, 5);  R2(c, d, a, b, 11, 9);  R2(b, c, d, a, 15, 13);

    R3(a, b, c, d, 0, 3);  R3(d, a, b, c, 8, 9);  R3(c, d, a, b, 4, 11);  R3(b, c, d, a, 12, 15);
    R3(a, b, c, d, 2, 3);  R3(d, a, b, c, 10, 9); R3(c, d, a, b, 6, 11);  R3(b, c, d, a, 14, 15);
    R3(a, b, c, d, 1, 3);  R3(d, a, b, c, 9, 9);  R3(c, d, a, b, 5, 11);  R3(b, c, d, a, 13, 15);
    R3(a, b, c, d, 3, 3);  R3(d, a, b, c, 11, 9); R3(c, d, a, b, 7, 11);  R3(b, c, d, a, 15, 15);
#undef R1
#undef R2
#undef R3

    m_state[0] += a;
    m_state[1] += b;
    m_state[2] += c;
    m_state[3] += d;
}

void Md4::update(const char *data, qint64 len)
{
    if (len <= 0) return;
    const unsigned char *p = reinterpret_cast<const unsigned char *>(data);
    m_length += quint64(len);
    if (m_bufLen) {
        const int take = int(qMin<qint64>(64 - m_bufLen, len));
        std::memcpy(m_buf + m_bufLen, p, size_t(take));
        m_bufLen += take;
        p += take;
        len -= take;
        if (m_bufLen < 64) return;
        block(m_buf);
        m_bufLen = 0;
    }
    while (len >= 64) {
        block(p);
        p += 64;
        len -= 64;
    }
    if (len > 0) {
        std::memcpy(m_buf, p, size_t(len));
        m_bufLen = int(len);
    }
}

QByteArray Md4::finish()
{
    const quint64 bits = m_length * 8;
    unsigned char pad[72];
    std::memset(pad, 0, sizeof pad);
    pad[0] = 0x80;
    const int padLen = (m_bufLen < 56) ? (56 - m_bufLen) : (120 - m_bufLen);
    for (int i = 0; i < 8; ++i)
        pad[padLen + i] = static_cast<unsigned char>(bits >> (8 * i));
    const quint64 savedLen = m_length;
    update(reinterpret_cast<const char *>(pad), padLen + 8);
    m_length = savedLen;
    QByteArray out(16, Qt::Uninitialized);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            out[i * 4 + j] = char((m_state[i] >> (8 * j)) & 0xff);
    return out;
}

QByteArray Md4::hash(const QByteArray &data)
{
    Md4 h;
    h.update(data);
    return h.finish();
}

} // namespace mx::hash
