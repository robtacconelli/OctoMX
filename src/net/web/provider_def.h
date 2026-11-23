// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX web module: data-driven provider definitions (private). Schema: src/net/web/README.md.
#pragma once

#include <QtCore/QHash>
#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtCore/QMap>
#include <QtCore/QRegularExpression>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include "mx/types.h"

namespace mx::web {

// How one output field is obtained from an item.
struct FieldSpec {
    QString path;        // value path inside the item ("a.b.0.c", "../title", "enclosure@url")
    QString tmpl;        // or a template "{path} [{other|urlencode}]"
    QString constant;    // or a constant value
    QRegularExpression regex;  // optional: applied to the text; group "v" or 1 (else whole match)
    QString transform;   // text | int | size | hash | magnet | date | unixtime | html | lower | bool
    QString format;      // date format for transform "date"
    bool valid() const { return !path.isEmpty() || !tmpl.isEmpty() || !constant.isEmpty(); }
};

struct FilesSpec {
    QString items;       // item-relative items path ("details.result[]", "files[]")
    FieldSpec path, size, url;
    QString filterField; // keep only entries whose filterField matches filterRegex
    QRegularExpression filterRegex;
    QRegularExpression exclude;   // drop entries whose path matches
    int maxFiles = 2000;
    bool valid() const { return !items.isEmpty() && path.valid(); }
};

struct DetailsSpec {     // optional second request per item (e.g. Internet Archive file lists)
    QString url;         // template over the item
    QString key = QStringLiteral("details");  // the parsed JSON is stored in the item under this key
    int maxItems = 8;    // only for the first N items of a page
    int timeoutMs = 8000;
    bool valid() const { return !url.isEmpty(); }
};

struct ProviderDef {
    QString id;
    QString name;
    QString homepage;
    QString notes;
    bool    enabled = true;
    QString origin;          // "builtin" or the file path it came from (last one that touched it)
    bool    userDefined = false;   // defined (not only overridden) by a user file

    // request
    QString url;             // template: {query} {page} {offset} {category} {pageSize} {adult_hide}
    QString method = QStringLiteral("GET");
    QString body;            // POST body template; {query_json} = JSON string literal of the query
    QString contentType;
    QMap<QString, QString> headers;
    QString spaceAs = QStringLiteral("%20");   // how spaces in {query} are encoded ("+", "%20", "-")
    QHash<int, QString> categories;           // MediaType → value for {category}
    bool    hasAnyCategory = false;           // categories contains "any"
    bool    strictCategories = false;         // types not in `categories` → provider not asked
    QString probeQuery = QStringLiteral("ubuntu");

    // paging / limits
    int firstPage = 1;
    int maxPages = 1;
    int pageSize = 0;        // results per page (to know whether a next page may exist); 0 = unknown
    int minIntervalMs = 1000;
    int timeoutMs = 10000;
    int maxBytes = 4 * 1024 * 1024;

    // response
    QString format = QStringLiteral("json");  // json | rss | xml | regex
    QString items;           // json: path with [] ; rss/xml: element name ("item"); regex: pattern
    bool    decimalUnits = false;
    QHash<QString, FieldSpec> fields;   // name infoHash size seeders leechers numFiles published category
                                        // magnet torrentUrl httpUrl detailsUrl
    FilesSpec files;
    DetailsSpec details;

    // classification
    QList<QPair<QRegularExpression, MediaType>> categoryTypes;  // matched against `category`
    QList<QRegularExpression> adultCategories;
    MediaType defaultType = MediaType::Any;
    QStringList trackers;    // trackers of this index, added to magnets built for its rows
    bool matchName = true;   // drop rows whose name does not contain every query word
};

QString mediaTypeKey(MediaType t);                 // "any", "audio", "video", ...
MediaType mediaTypeFromKey(const QString &key);    // unknown → Any

// Parse one provider object (already merged). Returns false + *error if unusable.
bool parseProviderDef(const QJsonObject &o, ProviderDef *out, QString *error);

// Provider objects in a document: {"providers":[...]}, a bare array, or a single object.
QList<QJsonObject> providerObjects(const QByteArray &json, QString *error);

// Loads the built-in definitions (Qt resource :/mx/web/providers.json) and then each override file
// in order (missing files are ignored). An override object with the id of an existing provider is
// merged into it key by key (nested objects merged recursively), so {"id":"apibay","enabled":false}
// just disables it. Unknown ids add providers. Malformed providers are skipped with a message.
struct ProviderSet {
    QList<ProviderDef> providers;
    QStringList messages;    // warnings for the log
};
// `profile` selects the default-enabled set from the built-in "defaults" object (see
// providers.json); empty = the build's MX_WEB_DEFAULT_PROFILE ("standard" unless configured).
ProviderSet loadProviders(const QStringList &overrideFiles, const QByteArray &builtinOverride = QByteArray(),
                          const QString &profile = QString());
QString defaultProviderProfile();

QJsonObject mergeJsonObjects(const QJsonObject &base, const QJsonObject &over);

} // namespace mx::web
