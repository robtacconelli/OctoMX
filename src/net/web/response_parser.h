// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX web module: turns a provider's HTTP response into TorrentRecords (private).
// Pure functions: safe to run in a worker thread.
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QList>
#include <QtCore/QVariant>

#include "provider_def.h"
#include "search_backend.h"

namespace mx::web {

// The item stack an item's fields are resolved against: [root, ..., outer item, item].
using ItemContext = QVariantList;

struct ParseResult {
    bool ok = false;               // the document could be read (possibly with 0 items)
    QString error;
    int rawItems = 0;              // items found in the document
    int skipped = 0;               // items dropped (malformed / no hash / no name)
    QList<TorrentRecord> records;
    QList<ItemContext> contexts;   // parallel to records (for a details request)
};

ParseResult parseResponse(const ProviderDef &def, const QByteArray &body);

// Field extraction on one item (exposed for details re-extraction and tests).
bool extractRecord(const ProviderDef &def, const ItemContext &ctx, TorrentRecord *out);

// Resolve "a.b.0.c" / "../x" against the stack (innermost first, then outward for plain paths).
// `vars` holds values already extracted for the record ("@infoHash", "@name"); paths starting with "@".
QVariant resolvePath(const ItemContext &ctx, const QString &path, const QVariantMap *vars = nullptr);
// "{path|filter}" expansion over the item (filters: urlencode, urlpath, lower, json, html, trim).
QString expandTemplate(const QString &tmpl, const ItemContext &ctx, const QVariantMap *vars = nullptr);
QString variantText(const QVariant &v);

// The JSON document as a QVariant (map/list) or invalid + *error. Rejects documents > 4 MB.
QVariant parseJsonVariant(const QByteArray &body, QString *error);

// Add the details document to the innermost item of ctx under `key` and re-extract the record.
bool applyDetails(const ProviderDef &def, ItemContext ctx, const QByteArray &body, TorrentRecord *rec);

} // namespace mx::web
