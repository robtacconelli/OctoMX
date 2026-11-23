// SPDX-License-Identifier: GPL-3.0-or-later
#include "ed2k_io.h"

#include <QtCore/QStringDecoder>
#include <QtCore/QStringList>
#include <QtCore/QtEndian>
#include <cstring>

namespace mx::ed2k {

quint8 Reader::u8()
{
    if (!need(1)) return 0;
    return quint8(m_d[m_pos++]);
}
quint16 Reader::u16()
{
    if (!need(2)) return 0;
    quint16 v = qFromLittleEndian<quint16>(m_d + m_pos);
    m_pos += 2;
    return v;
}
quint32 Reader::u32()
{
    if (!need(4)) return 0;
    quint32 v = qFromLittleEndian<quint32>(m_d + m_pos);
    m_pos += 4;
    return v;
}
quint64 Reader::u64()
{
    if (!need(8)) return 0;
    quint64 v = qFromLittleEndian<quint64>(m_d + m_pos);
    m_pos += 8;
    return v;
}
float Reader::f32()
{
    quint32 bits = u32();
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}
QByteArray Reader::bytes(qsizetype k)
{
    if (!need(k)) return QByteArray();
    QByteArray b(m_d + m_pos, k);
    m_pos += k;
    return b;
}
QByteArray Reader::raw16(qsizetype maxLen)
{
    quint16 len = u16();
    if (!m_ok || len > maxLen) { fail(); return QByteArray(); }
    return bytes(len);
}
QString Reader::str16(qsizetype maxLen)
{
    return decodeString(raw16(maxLen));
}

Writer &Writer::u16(quint16 v) { char b[2]; qToLittleEndian(v, b); m_b.append(b, 2); return *this; }
Writer &Writer::u32(quint32 v) { char b[4]; qToLittleEndian(v, b); m_b.append(b, 4); return *this; }
Writer &Writer::u64(quint64 v) { char b[8]; qToLittleEndian(v, b); m_b.append(b, 8); return *this; }
Writer &Writer::f32(float v) { quint32 bits; std::memcpy(&bits, &v, 4); return u32(bits); }
Writer &Writer::hash16(const QByteArray &h)
{
    QByteArray x = h.left(16);
    if (x.size() < 16) x.append(QByteArray(16 - x.size(), '\0'));
    m_b.append(x);
    return *this;
}
Writer &Writer::str16(const QString &s) { return raw16(s.toUtf8()); }
Writer &Writer::raw16(const QByteArray &b)
{
    QByteArray x = b.left(0xFFFF);
    u16(quint16(x.size()));
    m_b.append(x);
    return *this;
}
void Writer::put32At(qsizetype at, quint32 v)
{
    if (at < 0 || at + 4 > m_b.size()) return;
    qToLittleEndian(v, m_b.data() + at);
}

QString decodeString(const QByteArray &raw)
{
    QByteArray b = raw;
    if (b.startsWith("\xEF\xBB\xBF")) b.remove(0, 3);
    // strip embedded NULs (some clients pad)
    int nul = b.indexOf('\0');
    if (nul >= 0) b.truncate(nul);
    QStringDecoder dec(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    QString s = dec(b);
    if (dec.hasError()) return QString::fromLatin1(b);
    return s;
}

QString ipToString(quint32 ip)
{
    return QStringLiteral("%1.%2.%3.%4").arg(ip & 0xFF).arg((ip >> 8) & 0xFF)
        .arg((ip >> 16) & 0xFF).arg((ip >> 24) & 0xFF);
}
quint32 ipFromString(const QString &s, bool *ok)
{
    const QStringList parts = s.trimmed().split(QLatin1Char('.'));
    if (parts.size() != 4) { if (ok) *ok = false; return 0; }
    quint32 v = 0;
    for (int i = 0; i < 4; ++i) {
        bool k = false;
        uint o = parts[i].toUInt(&k);
        if (!k || o > 255) { if (ok) *ok = false; return 0; }
        v |= quint32(o) << (8 * i);
    }
    if (ok) *ok = true;
    return v;
}
quint32 ipFromHostOrder(quint32 h) { return qbswap(h); }
quint32 ipToHostOrder(quint32 ip) { return qbswap(ip); }

bool isLoopbackIp(quint32 ip) { return (ip & 0xFF) == 127; }

bool isRoutableIp(quint32 ip)
{
    const quint32 a = ip & 0xFF, b = (ip >> 8) & 0xFF;
    if (a == 0 || a == 10 || a == 127 || a >= 224) return false;
    if (a == 169 && b == 254) return false;
    if (a == 172 && b >= 16 && b <= 31) return false;
    if (a == 192 && b == 168) return false;
    if (a == 100 && b >= 64 && b <= 127) return false; // CGNAT
    return true;
}

QString toHex(const QByteArray &b) { return QString::fromLatin1(b.toHex()); }
QByteArray fromHex(const QString &s, int expectBytes)
{
    const QByteArray in = s.trimmed().toLatin1();
    if (in.size() != expectBytes * 2) return QByteArray();
    for (char c : in)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return QByteArray();
    return QByteArray::fromHex(in);
}

} // namespace mx::ed2k
