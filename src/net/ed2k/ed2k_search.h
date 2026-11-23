// SPDX-License-Identifier: GPL-3.0-or-later
// Keyword search: mx::SearchQuery -> eD2k search tree (OP_SEARCHREQUEST payload), and the
// result list (OP_SEARCHRESULT / OP_GLOBSEARCHRES) -> mx::SearchHit.
//
// Query syntax (both text lines are ANDed): words, "quoted phrases", AND / OR / NOT (upper case),
// a leading '-' on a word = NOT, parentheses. Precedence NOT > AND > OR as in eMule. Adjacent
// plain words are sent as one string (the server tokenises it).
#pragma once

#include "ed2k_tags.h"
#include "mx/types.h"

#include <QtCore/QList>
#include <memory>

namespace mx::ed2k {

struct SearchNode {
    enum Kind { Keyword, And, Or, Not } kind = Keyword;
    QString text;
    std::shared_ptr<SearchNode> a, b;
};
using SearchNodePtr = std::shared_ptr<SearchNode>;

// nullptr + *error on syntax error / empty query
SearchNodePtr parseSearchExpression(const QString &text, QString *error = nullptr);
QString searchNodeToString(const SearchNodePtr &n);   // debug / tests: "(a AND (b OR c))"

struct SearchEncodeOptions {
    bool largeFiles = true;      // server supports 64-bit values (SRV_TCPFLG_LARGEFILES)
    bool typeTagInteger = false; // unused in requests (strings are universal)
};
// Search tree bytes for `expr` (may be null: constraints only) ANDed with the type / size /
// [extension] / [bitrate] constraints of `q`. Empty if there is nothing to encode. Also used for the
// restrictive part of Kad keyword searches (same encoding).
QByteArray encodeSearchTree(const SearchNodePtr &expr, const mx::SearchQuery &q, const SearchEncodeOptions &opt,
                            bool withExtension, bool withBitrate);
// Builds the OP_SEARCHREQUEST payload; empty + *error if the query has no usable keyword.
QByteArray encodeSearchRequest(const mx::SearchQuery &q, const SearchEncodeOptions &opt = {},
                               QString *error = nullptr);

struct SearchResultEntry {
    QByteArray hash;            // 16 bytes ed2k
    quint32 clientId = 0;
    quint16 clientPort = 0;
    QString name;
    qint64  size = 0;
    QString typeText;           // "Audio", "Video", "Image", "Doc", "Pro", "Arc", "Iso", ...
    QString format;             // extension tag if present
    int     sources = -1;
    int     completeSources = -1;
    int     lengthSec = -1;
    int     bitrateKbps = -1;
    QString codec, artist, album, title;
};
// <count 4>(<hash 16><id 4><port 2><tags>)[count] [<more 1>]. Bounded; returns false on garbage.
bool parseSearchResults(const QByteArray &payload, QList<SearchResultEntry> *out, bool *moreAvailable);
// one entry (for UDP results, which carry one entry per packet)
bool parseSearchResultEntry(Reader &r, SearchResultEntry *out);

QString formatEd2kFileLink(const QString &name, qint64 size, const QByteArray &hash16);
mx::MediaType mediaTypeFromEd2k(const QString &typeText, const QString &fileName);
mx::SearchHit toSearchHit(const SearchResultEntry &e, quint32 queryId);
QString ed2kTypeForMedia(mx::MediaType t);    // "" for Any/Other

} // namespace mx::ed2k
