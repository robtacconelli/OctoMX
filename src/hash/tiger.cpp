// SPDX-License-Identifier: GPL-3.0-or-later
// Tiger/192, 3 passes (Ross Anderson & Eli Biham, 1996).
//
// The four 256-entry S-boxes are not stored as a 1024-constant table: they are regenerated once
// (thread-safe function-local static) with the generation procedure published with the reference
// implementation ("sboxes.c"/gen()): start from t[i] = i replicated in all 8 bytes, then 5 passes of
// byte-column swaps driven by the Tiger compression function (using the table being generated)
// applied to the 64-byte key "Tiger - A Fast New Hash Function, by Ross Anderson and Eli Biham",
// with the standard Tiger IV as state. The result is verified by the NESSIE test vectors in
// tests/hash (e.g. T1[0] = 0x02AAB17CF7E90C5E, Tiger("") = 3293AC63...).
#include "mxhash/hasher.h"

#include <cstring>

namespace mx::hash {

namespace {

struct Tables {
    quint64 t[1024];
    Tables();
};

inline quint64 loadLe64(const unsigned char *p)
{
    quint64 v = 0;
    for (int j = 0; j < 8; ++j)
        v |= quint64(p[j]) << (8 * j);
    return v;
}

void compress(const quint64 *T, const quint64 *xin, quint64 st[3])
{
    quint64 a = st[0], b = st[1], c = st[2];
    const quint64 aa = a, bb = b, cc = c;
    quint64 x[8];
    std::memcpy(x, xin, sizeof x);

    auto round = [T](quint64 &ra, quint64 &rb, quint64 &rc, quint64 xv, quint64 mul) {
        rc ^= xv;
        ra -= T[rc & 0xFF] ^ T[256 + ((rc >> 16) & 0xFF)] ^ T[512 + ((rc >> 32) & 0xFF)]
            ^ T[768 + ((rc >> 48) & 0xFF)];
        rb += T[768 + ((rc >> 8) & 0xFF)] ^ T[512 + ((rc >> 24) & 0xFF)]
            ^ T[256 + ((rc >> 40) & 0xFF)] ^ T[(rc >> 56) & 0xFF];
        rb *= mul;
    };
    auto pass = [&](quint64 &pa, quint64 &pb, quint64 &pc, quint64 mul) {
        round(pa, pb, pc, x[0], mul);
        round(pb, pc, pa, x[1], mul);
        round(pc, pa, pb, x[2], mul);
        round(pa, pb, pc, x[3], mul);
        round(pb, pc, pa, x[4], mul);
        round(pc, pa, pb, x[5], mul);
        round(pa, pb, pc, x[6], mul);
        round(pb, pc, pa, x[7], mul);
    };
    auto keySchedule = [&x]() {
        x[0] -= x[7] ^ 0xA5A5A5A5A5A5A5A5ULL;
        x[1] ^= x[0];
        x[2] += x[1];
        x[3] -= x[2] ^ ((~x[1]) << 19);
        x[4] ^= x[3];
        x[5] += x[4];
        x[6] -= x[5] ^ ((~x[4]) >> 23);
        x[7] ^= x[6];
        x[0] += x[7];
        x[1] -= x[0] ^ ((~x[7]) << 19);
        x[2] ^= x[1];
        x[3] += x[2];
        x[4] -= x[3] ^ ((~x[2]) >> 23);
        x[5] ^= x[4];
        x[6] += x[5];
        x[7] -= x[6] ^ 0x0123456789ABCDEFULL;
    };

    pass(a, b, c, 5);
    keySchedule();
    pass(c, a, b, 7);
    keySchedule();
    pass(b, c, a, 9);

    st[0] = a ^ aa;
    st[1] = b - bb;
    st[2] = c + cc;
}

inline unsigned byteOf(quint64 v, int col) { return unsigned((v >> (8 * col)) & 0xFF); }
inline void setByte(quint64 &v, int col, unsigned b)
{
    v &= ~(0xFFULL << (8 * col));
    v |= quint64(b & 0xFF) << (8 * col);
}

Tables::Tables()
{
    static const char seed[] = "Tiger - A Fast New Hash Function, by Ross Anderson and Eli Biham";
    static_assert(sizeof(seed) == 65, "seed must be 64 bytes");
    quint64 key[8];
    for (int i = 0; i < 8; ++i)
        key[i] = loadLe64(reinterpret_cast<const unsigned char *>(seed) + i * 8);
    quint64 st[3] = {0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL, 0xF096A5B4C3B2E187ULL};
    for (int i = 0; i < 1024; ++i) {
        t[i] = 0;
        for (int col = 0; col < 8; ++col)
            setByte(t[i], col, unsigned(i & 255));
    }
    int abc = 2;
    for (int cnt = 0; cnt < 5; ++cnt) {
        for (int i = 0; i < 256; ++i) {
            for (int sb = 0; sb < 1024; sb += 256) {
                if (++abc == 3) {
                    abc = 0;
                    compress(t, key, st);
                }
                for (int col = 0; col < 8; ++col) {
                    const int j = int(byteOf(st[abc], col));
                    const unsigned tmp = byteOf(t[sb + i], col);
                    setByte(t[sb + i], col, byteOf(t[sb + j], col));
                    setByte(t[sb + j], col, tmp);
                }
            }
        }
    }
}

const quint64 *tables()
{
    static const Tables tbl;
    return tbl.t;
}

} // namespace

void Tiger::reset()
{
    m_state[0] = 0x0123456789ABCDEFULL;
    m_state[1] = 0xFEDCBA9876543210ULL;
    m_state[2] = 0xF096A5B4C3B2E187ULL;
    m_length = 0;
    m_bufLen = 0;
}

void Tiger::update(const char *data, qint64 len)
{
    if (len <= 0) return;
    const quint64 *T = tables();
    const unsigned char *p = reinterpret_cast<const unsigned char *>(data);
    m_length += quint64(len);
    quint64 x[8];
    if (m_bufLen) {
        const int take = int(qMin<qint64>(64 - m_bufLen, len));
        std::memcpy(m_buf + m_bufLen, p, size_t(take));
        m_bufLen += take;
        p += take;
        len -= take;
        if (m_bufLen < 64) return;
        for (int i = 0; i < 8; ++i) x[i] = loadLe64(m_buf + i * 8);
        compress(T, x, m_state);
        m_bufLen = 0;
    }
    while (len >= 64) {
        for (int i = 0; i < 8; ++i) x[i] = loadLe64(p + i * 8);
        compress(T, x, m_state);
        p += 64;
        len -= 64;
    }
    if (len > 0) {
        std::memcpy(m_buf, p, size_t(len));
        m_bufLen = int(len);
    }
}

QByteArray Tiger::finish()
{
    const quint64 bits = m_length * 8;
    unsigned char pad[72];
    std::memset(pad, 0, sizeof pad);
    pad[0] = 0x01;
    const int padLen = (m_bufLen < 56) ? (56 - m_bufLen) : (120 - m_bufLen);
    for (int i = 0; i < 8; ++i)
        pad[padLen + i] = static_cast<unsigned char>(bits >> (8 * i));
    const quint64 saved = m_length;
    update(reinterpret_cast<const char *>(pad), padLen + 8);
    m_length = saved;
    QByteArray out(24, Qt::Uninitialized);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 8; ++j)
            out[i * 8 + j] = char(byteOf(m_state[i], j));
    return out;
}

QByteArray Tiger::hash(const char *data, qint64 len)
{
    Tiger t;
    t.update(data, len);
    return t.finish();
}

QByteArray Tiger::hash(const QByteArray &data)
{
    return hash(data.constData(), data.size());
}

} // namespace mx::hash
