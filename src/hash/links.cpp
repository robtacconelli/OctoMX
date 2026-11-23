// SPDX-License-Identifier: GPL-3.0-or-later
#include "mxhash/links.h"

#include "mxhash/encoding.h"

#include <QtCore/QUrl>

namespace mx::hash {

namespace {

QString decodeComponent(const QString &v, bool plusIsSpace)
{
    QString s = v;
    if (plusIsSpace) s.replace(QLatin1Char('+'), QLatin1Char(' '));
    return QUrl::fromPercentEncoding(s.toUtf8());
}

QString encodeComponent(const QString &v)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(v));
}

QList<int> parseSelectOnly(const QString &v, bool *ok)
{
    QList<int> out;
    *ok = true;
    for (const QString &item : v.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const int dash = item.indexOf(QLatin1Char('-'));
        bool a = false, b = true;
        if (dash > 0) {
            const int from = item.left(dash).trimmed().toInt(&a);
            const int to = item.mid(dash + 1).trimmed().toInt(&b);
            if (a && b && from <= to && to - from < 100000)
                for (int i = from; i <= to; ++i) out.append(i);
            else
                *ok = false;
        } else {
            const int i = item.trimmed().toInt(&a);
            if (a) out.append(i);
            else *ok = false;
        }
    }
    return out;
}

QString formatSelectOnly(const QList<int> &so)
{
    QStringList parts;
    int i = 0;
    while (i < so.size()) {
        int j = i;
        while (j + 1 < so.size() && so.at(j + 1) == so.at(j) + 1) ++j;
        if (j > i)
            parts << QStringLiteral("%1-%2").arg(so.at(i)).arg(so.at(j));
        else
            parts << QString::number(so.at(i));
        i = j + 1;
    }
    return parts.join(QLatin1Char(','));
}

void appendExtra(QVariantMap &extras, const QString &key, const QString &value)
{
    if (!extras.contains(key)) {
        extras.insert(key, value);
        return;
    }
    QVariant cur = extras.value(key);
    QStringList l = cur.typeId() == QMetaType::QStringList ? cur.toStringList() : QStringList{cur.toString()};
    l << value;
    extras.insert(key, l);
}

bool startsWithCi(const QString &s, const char *prefix)
{
    return s.startsWith(QLatin1String(prefix), Qt::CaseInsensitive);
}

// Handles one xt value. Returns false if it was a known kind with an undecodable hash.
bool applyXt(ParsedLink &l, const QString &xt)
{
    auto tail = [&xt](const char *prefix) { return xt.mid(int(qstrlen(prefix))); };
    QByteArray h;
    if (startsWithCi(xt, "urn:btih:")) {
        h = decodeHash(tail("urn:btih:"), 20);
        if (h.isEmpty()) return false;
        l.hashes.btih = h;
    } else if (startsWithCi(xt, "urn:btmh:")) {
        appendExtra(l.extras, QStringLiteral("btmh"), tail("urn:btmh:"));
    } else if (startsWithCi(xt, "urn:ed2khash:")) {
        h = decodeHash(tail("urn:ed2khash:"), 16);
        if (h.isEmpty()) return false;
        l.hashes.ed2k = h;
    } else if (startsWithCi(xt, "urn:ed2k:")) {
        h = decodeHash(tail("urn:ed2k:"), 16);
        if (h.isEmpty()) return false;
        l.hashes.ed2k = h;
    } else if (startsWithCi(xt, "urn:sha1:")) {
        h = decodeHash(tail("urn:sha1:"), 20);
        if (h.isEmpty()) return false;
        l.hashes.sha1 = h;
    } else if (startsWithCi(xt, "urn:bitprint:")) {
        const QStringList p = tail("urn:bitprint:").split(QLatin1Char('.'));
        if (p.size() != 2) return false;
        const QByteArray s = decodeHash(p.at(0), 20);
        const QByteArray t = decodeHash(p.at(1), 24);
        if (s.isEmpty() || t.isEmpty()) return false;
        l.hashes.sha1 = s;
        l.hashes.tth = t;
    } else if (startsWithCi(xt, "urn:tree:tiger")) {
        // urn:tree:tiger:X, urn:tree:tiger/:X, urn:tree:tiger/1024:X
        const int colon = xt.indexOf(QLatin1Char(':'), int(qstrlen("urn:tree:tiger")));
        if (colon < 0) return false;
        h = decodeHash(xt.mid(colon + 1), 24);
        if (h.isEmpty()) return false;
        l.hashes.tth = h;
    } else if (startsWithCi(xt, "urn:aich:")) {
        h = decodeHash(tail("urn:aich:"), 20);
        if (h.isEmpty()) return false;
        l.hashes.aich = h;
    } else if (startsWithCi(xt, "urn:md5:")) {
        h = decodeHash(tail("urn:md5:"), 16);
        if (h.isEmpty()) return false;
        l.hashes.md5 = h;
    } else {
        appendExtra(l.extras, QStringLiteral("xt"), xt);
    }
    return true;
}

} // namespace

ParsedLink parseMagnet(const QString &uri)
{
    ParsedLink l;
    QString s = uri.trimmed();
    if (!startsWithCi(s, "magnet:")) {
        l.error = QStringLiteral("not a magnet link");
        return l;
    }
    s = s.mid(7);
    if (s.startsWith(QLatin1Char('?'))) s = s.mid(1);
    QStringList badXt;
    for (const QString &pair : s.split(QLatin1Char('&'), Qt::SkipEmptyParts)) {
        const int eq = pair.indexOf(QLatin1Char('='));
        QString key = (eq < 0 ? pair : pair.left(eq)).toLower();
        const QString raw = eq < 0 ? QString() : pair.mid(eq + 1);
        // numbered variants: xt.1, tr.2 ...
        const int dot = key.lastIndexOf(QLatin1Char('.'));
        if (dot > 0) {
            bool num = false;
            key.mid(dot + 1).toInt(&num);
            if (num) key = key.left(dot);
        }
        if (key == QLatin1String("xt")) {
            const QString v = decodeComponent(raw, false);
            if (!applyXt(l, v)) badXt << v;
        } else if (key == QLatin1String("dn")) {
            l.name = decodeComponent(raw, true);
        } else if (key == QLatin1String("xl")) {
            bool ok = false;
            const qint64 v = decodeComponent(raw, false).toLongLong(&ok);
            if (ok && v >= 0) l.size = v;
        } else if (key == QLatin1String("tr")) {
            l.trackers << decodeComponent(raw, false);
        } else if (key == QLatin1String("ws")) {
            l.webSeeds << decodeComponent(raw, false);
        } else if (key == QLatin1String("as")) {
            l.acceptableSources << decodeComponent(raw, false);
        } else if (key == QLatin1String("xs")) {
            l.exactSources << decodeComponent(raw, false);
        } else if (key == QLatin1String("so")) {
            bool ok = true;
            l.selectOnly += parseSelectOnly(decodeComponent(raw, false), &ok);
        } else if (key == QLatin1String("x.pe")) {
            l.sources << decodeComponent(raw, false);
        } else {
            appendExtra(l.extras, key, decodeComponent(raw, false));
        }
    }
    if (!badXt.isEmpty())
        l.extras.insert(QStringLiteral("invalidXt"), badXt);
    if (l.hashes.isEmpty() && l.exactSources.isEmpty() && l.acceptableSources.isEmpty() && l.webSeeds.isEmpty()) {
        l.error = badXt.isEmpty() ? QStringLiteral("magnet link has no usable hash or source")
                                  : QStringLiteral("magnet link has an invalid hash: %1").arg(badXt.first());
        return l;
    }
    l.kind = ParsedLink::Magnet;
    return l;
}

ParsedLink parseEd2kLink(const QString &uri)
{
    ParsedLink l;
    QString s = uri.trimmed();
    if (!startsWithCi(s, "ed2k://")) {
        l.error = QStringLiteral("not an ed2k link");
        return l;
    }
    s = s.mid(7);
    if (!s.contains(QLatin1Char('|')))
        s.replace(QStringLiteral("%7c"), QStringLiteral("|"), Qt::CaseInsensitive);
    const QStringList p = s.split(QLatin1Char('|'));
    if (p.size() < 5 || p.at(1).compare(QLatin1String("file"), Qt::CaseInsensitive) != 0) {
        l.error = QStringLiteral("not an ed2k file link");
        return l;
    }
    l.name = decodeComponent(p.at(2), false);
    bool ok = false;
    l.size = p.at(3).toLongLong(&ok);
    if (!ok || l.size < 0) {
        l.error = QStringLiteral("invalid size in ed2k link");
        l.size = -1;
        return l;
    }
    l.hashes.ed2k = fromHex(p.at(4), &ok);
    if (!ok || l.hashes.ed2k.size() != 16) {
        l.hashes.ed2k.clear();
        l.error = QStringLiteral("invalid hash in ed2k link");
        return l;
    }
    for (int i = 5; i < p.size(); ++i) {
        const QString t = p.at(i);
        if (t.isEmpty() || t == QLatin1String("/")) continue;
        if (startsWithCi(t, "h=")) {
            const QByteArray a = decodeHash(t.mid(2), 20);
            if (!a.isEmpty()) l.hashes.aich = a;
            else appendExtra(l.extras, QStringLiteral("invalidAich"), t.mid(2));
        } else if (startsWithCi(t, "p=")) {
            l.ed2kHashsetParts = t.mid(2).split(QLatin1Char(':'), Qt::SkipEmptyParts);
        } else if (startsWithCi(t, "s=")) {
            l.webSeeds << decodeComponent(t.mid(2), false);
        } else if (startsWithCi(t, "sources,")) {
            for (const QString &src : t.mid(8).split(QLatin1Char(','), Qt::SkipEmptyParts))
                l.sources << src.trimmed();
        } else {
            appendExtra(l.extras, QStringLiteral("unknown"), t);
        }
    }
    l.kind = ParsedLink::Ed2k;
    return l;
}

ParsedLink parseLink(const QString &uri)
{
    const QString s = uri.trimmed();
    if (startsWithCi(s, "magnet:")) return parseMagnet(s);
    if (startsWithCi(s, "ed2k:")) return parseEd2kLink(s);
    ParsedLink l;
    l.error = QStringLiteral("unsupported link");
    return l;
}

QString formatMagnet(const ParsedLink &l)
{
    QStringList parts;
    const FileHashes &h = l.hashes;
    if (!h.btih.isEmpty()) parts << QStringLiteral("xt=urn:btih:") + toHex(h.btih);
    if (!h.ed2k.isEmpty()) parts << QStringLiteral("xt=urn:ed2k:") + toHex(h.ed2k);
    if (!h.sha1.isEmpty() && !h.tth.isEmpty())
        parts << QStringLiteral("xt=urn:bitprint:") + toBase32(h.sha1) + QLatin1Char('.') + toBase32(h.tth);
    else if (!h.sha1.isEmpty())
        parts << QStringLiteral("xt=urn:sha1:") + toBase32(h.sha1);
    else if (!h.tth.isEmpty())
        parts << QStringLiteral("xt=urn:tree:tiger:") + toBase32(h.tth);
    if (!h.aich.isEmpty()) parts << QStringLiteral("xt=urn:aich:") + toBase32(h.aich);
    if (!h.md5.isEmpty()) parts << QStringLiteral("xt=urn:md5:") + toHex(h.md5);
    if (l.size >= 0) parts << QStringLiteral("xl=%1").arg(l.size);
    if (!l.name.isEmpty()) parts << QStringLiteral("dn=") + encodeComponent(l.name);
    for (const QString &t : l.trackers) parts << QStringLiteral("tr=") + encodeComponent(t);
    for (const QString &t : l.webSeeds) parts << QStringLiteral("ws=") + encodeComponent(t);
    for (const QString &t : l.acceptableSources) parts << QStringLiteral("as=") + encodeComponent(t);
    for (const QString &t : l.exactSources) parts << QStringLiteral("xs=") + encodeComponent(t);
    if (!l.selectOnly.isEmpty()) parts << QStringLiteral("so=") + formatSelectOnly(l.selectOnly);
    else if (!h.btih.isEmpty() && h.btFileIndex >= 0) parts << QStringLiteral("so=%1").arg(h.btFileIndex);
    for (const QString &t : l.sources) parts << QStringLiteral("x.pe=") + encodeComponent(t);
    return QStringLiteral("magnet:?") + parts.join(QLatin1Char('&'));
}

QString formatEd2kLink(const QString &name, qint64 size, const mx::FileHashes &h)
{
    // Percent-encode what would break the link ('|', '%', control chars, spaces, non-ASCII as UTF-8).
    const QString enc = QString::fromLatin1(QUrl::toPercentEncoding(name, "()[]{}-_.!~*'&+,;=@$#^`"));
    QString out = QStringLiteral("ed2k://|file|") + enc + QLatin1Char('|') + QString::number(size)
                + QLatin1Char('|') + toHex(h.ed2k).toUpper() + QLatin1Char('|');
    if (!h.aich.isEmpty())
        out += QStringLiteral("h=") + toBase32(h.aich) + QLatin1Char('|');
    out += QLatin1Char('/');
    return out;
}

} // namespace mx::hash
