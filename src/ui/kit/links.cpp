// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/kit/links.h"

#include <QtCore/QUrl>

namespace mx::ui {

QString ed2kLink(const QString &name, qint64 size, const FileHashes &h)
{
    if (h.ed2k.size() != 16 || size <= 0)
        return QString();
    QString n = name;
    n.replace(QLatin1Char('|'), QLatin1Char('_'));
    return QStringLiteral("ed2k://|file|%1|%2|%3|/")
        .arg(QString::fromUtf8(QUrl::toPercentEncoding(n, " ()[]-_.,'&+!")), QString::number(size),
             QString::fromLatin1(h.ed2k.toHex().toUpper()));
}

QString magnetLink(const QString &name, qint64 size, const FileHashes &h)
{
    return h.isEmpty() ? QString() : h.toMagnet(name, size);
}

QString magnetLinkOf(const SearchHit &h)
{
    for (const SearchSource &s : h.sources)
        if (s.uri.startsWith(QLatin1String("magnet:"), Qt::CaseInsensitive))
            return s.uri;
    return magnetLink(h.fileName, h.size, h.hashes);
}

} // namespace mx::ui
