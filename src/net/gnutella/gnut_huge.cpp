// SPDX-License-Identifier: GPL-3.0-or-later
#include "gnut_huge.h"

#include "mxhash/encoding.h"

namespace mx::gnut {

using mx::hash::fromBase32;
using mx::hash::toBase32;

static QByteArray b32(const QString &s, int bytes)
{
    bool ok = false;
    const QByteArray raw = fromBase32(s.trimmed(), &ok);
    return ok && raw.size() == bytes ? raw : QByteArray();
}

bool parseUrn(const QString &urnIn, FileHashes *h)
{
    QString u = urnIn.trimmed();
    if (u.size() > 200) return false;
    const QString l = u.toLower();
    if (l.startsWith(QLatin1String("urn:sha1:")) || l.startsWith(QLatin1String("sha1:"))) {
        const QString v = u.mid(u.indexOf(QLatin1Char(':'), l.startsWith(QLatin1String("urn:")) ? 4 : 0) + 1);
        QByteArray raw = b32(v, 20);
        if (raw.isEmpty() && v.size() == 40) raw = mx::hash::decodeHash(v, 20);   // hex variant
        if (raw.isEmpty()) return false;
        if (h->sha1.isEmpty()) h->sha1 = raw;
        return true;
    }
    if (l.startsWith(QLatin1String("urn:bitprint:")) || l.startsWith(QLatin1String("bitprint:"))) {
        const QString v = u.mid(u.indexOf(QLatin1Char(':'), l.startsWith(QLatin1String("urn:")) ? 4 : 0) + 1);
        const int dot = int(v.indexOf(QLatin1Char('.')));
        if (dot < 0) return false;
        const QByteArray s = b32(v.left(dot), 20);
        const QByteArray t = b32(v.mid(dot + 1), 24);
        if (s.isEmpty() || t.isEmpty()) return false;
        if (h->sha1.isEmpty()) h->sha1 = s;
        if (h->tth.isEmpty()) h->tth = t;
        return true;
    }
    for (const char *pfx : {"urn:tree:tiger/:", "urn:tree:tiger:", "urn:ttroot:", "urn:tree:tiger/1024:"}) {
        if (l.startsWith(QLatin1String(pfx))) {
            const QByteArray t = b32(u.mid(int(qstrlen(pfx))), 24);
            if (t.isEmpty()) return false;
            if (h->tth.isEmpty()) h->tth = t;
            return true;
        }
    }
    return false;
}

QString sha1Urn(const QByteArray &sha1) { return QStringLiteral("urn:sha1:") + toBase32(sha1); }
QString tthUrn(const QByteArray &tth) { return QStringLiteral("urn:tree:tiger/:") + toBase32(tth); }
QString bitprintUrn(const QByteArray &sha1, const QByteArray &tth)
{
    return QStringLiteral("urn:bitprint:") + toBase32(sha1) + QLatin1Char('.') + toBase32(tth);
}

bool parseGgepH(const QByteArray &d, FileHashes *h)
{
    if (d.isEmpty()) return false;
    switch (quint8(d[0])) {
    case 1:
        if (d.size() < 21) return false;
        if (h->sha1.isEmpty()) h->sha1 = d.mid(1, 20);
        return true;
    case 2:
        if (d.size() < 45) {
            if (d.size() >= 21 && h->sha1.isEmpty()) { h->sha1 = d.mid(1, 20); return true; }
            return false;
        }
        if (h->sha1.isEmpty()) h->sha1 = d.mid(1, 20);
        if (h->tth.isEmpty()) h->tth = d.mid(21, 24);
        return true;
    case 3:
        if (d.size() < 17) return false;
        if (h->md5.isEmpty()) h->md5 = d.mid(1, 16);
        return true;
    default:
        return false;
    }
}

QByteArray encodeGgepH(const QByteArray &sha1, const QByteArray &tth)
{
    QByteArray b;
    if (sha1.size() != 20) return b;
    if (tth.size() == 24) {
        b.append(char(2));
        b.append(sha1);
        b.append(tth);
    } else {
        b.append(char(1));
        b.append(sha1);
    }
    return b;
}

} // namespace mx::gnut
