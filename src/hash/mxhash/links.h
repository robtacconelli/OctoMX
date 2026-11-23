// SPDX-License-Identifier: GPL-3.0-or-later
// Parsing / formatting of magnet: and ed2k:// file links.
#pragma once

#include "mx/types.h"

#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVariantMap>

namespace mx::hash {

struct ParsedLink {
    enum Kind { Invalid = 0, Magnet, Ed2k };
    Kind kind = Invalid;
    mx::FileHashes hashes;            // btih (hex or base32), ed2k, sha1, tth, aich, md5 (bitprint -> sha1+tth)
    QString name;                     // dn / ed2k name (percent-decoded)
    qint64 size = -1;                 // xl / ed2k size
    QStringList trackers;             // tr
    QStringList webSeeds;             // ws (and ed2k "s=" web sources)
    QStringList acceptableSources;    // as
    QStringList exactSources;         // xs (as given)
    QList<int> selectOnly;            // so=0,2,4-6 expanded
    QStringList sources;              // ed2k |sources,ip:port,...| ; magnet x.pe peers
    QStringList ed2kHashsetParts;     // ed2k p=hash:hash:... (hex part hashes)
    QVariantMap extras;               // anything else (unknown params, kt=, btmh, x.* ...)
    QString error;                    // when Invalid
    bool isValid() const { return kind != Invalid; }
};

ParsedLink parseMagnet(const QString &uri);
// ed2k://|file|name|size|md4hex|[h=AICHBASE32|][p=hash:hash..|][s=url|]/[|sources,ip:port,...|/]
// ("%7C" instead of '|' accepted). Server links (ed2k://|server|ip|port|/) are Invalid here.
ParsedLink parseEd2kLink(const QString &uri);
ParsedLink parseLink(const QString &uri);       // dispatches on scheme; Invalid otherwise
QString formatMagnet(const ParsedLink &l);      // xt for every hash kind known, dn, xl, tr, ws, as, xs, so
QString formatEd2kLink(const QString &name, qint64 size, const mx::FileHashes &h); // |h=AICH| when known

} // namespace mx::hash
