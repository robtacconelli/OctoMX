// SPDX-License-Identifier: GPL-3.0-or-later
#include "mxhash/encoding.h"

namespace mx::hash {

QString toBase32(const QByteArray &raw)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    QString out;
    out.reserve((raw.size() * 8 + 4) / 5);
    quint32 buffer = 0;
    int bits = 0;
    for (char ch : raw) {
        buffer = (buffer << 8) | quint8(ch);
        bits += 8;
        while (bits >= 5) {
            out.append(QLatin1Char(alphabet[(buffer >> (bits - 5)) & 31]));
            bits -= 5;
        }
        buffer &= (1u << bits) - 1;
    }
    if (bits > 0)
        out.append(QLatin1Char(alphabet[(buffer << (5 - bits)) & 31]));
    return out;
}

QByteArray fromBase32(const QString &s, bool *ok)
{
    QByteArray out;
    quint32 buffer = 0;
    int bits = 0;
    bool good = true;
    for (QChar qc : s) {
        const ushort c = qc.unicode();
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a';
        else if (c >= '2' && c <= '7') v = c - '2' + 26;
        else if (c == '=') continue;
        else { good = false; break; }
        buffer = (buffer << 5) | quint32(v);
        bits += 5;
        if (bits >= 8) {
            out.append(char((buffer >> (bits - 8)) & 0xff));
            bits -= 8;
        }
        buffer &= (1u << bits) - 1;
    }
    if (ok) *ok = good;
    return good ? out : QByteArray();
}

QString toHex(const QByteArray &raw)
{
    return QString::fromLatin1(raw.toHex());
}

QByteArray fromHex(const QString &s, bool *ok)
{
    bool good = (s.size() % 2) == 0;
    for (QChar c : s) {
        if (!good) break;
        const ushort u = c.unicode();
        good = (u >= '0' && u <= '9') || (u >= 'a' && u <= 'f') || (u >= 'A' && u <= 'F');
    }
    if (ok) *ok = good;
    return good ? QByteArray::fromHex(s.toLatin1()) : QByteArray();
}

QByteArray decodeHash(const QString &s, int bytes)
{
    const QString t = s.trimmed();
    bool ok = false;
    if (t.size() == bytes * 2) {
        const QByteArray h = fromHex(t, &ok);
        if (ok && h.size() == bytes) return h;
    }
    if (t.size() == (bytes * 8 + 4) / 5) {
        const QByteArray b = fromBase32(t, &ok);
        if (ok && b.size() == bytes) return b;
    }
    return QByteArray();
}

} // namespace mx::hash
