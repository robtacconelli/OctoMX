// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX web module: TorrentRecord → mx::SearchHit, query filtering, de-duplication (private).
#pragma once

#include <QtCore/QHash>
#include <QtCore/QSet>
#include <QtCore/QStringList>

#include "mx/types.h"
#include "search_backend.h"

namespace mx::web {

QStringList defaultAdultKeywords();

// Media type of a record: provider category mapping, else file extension of the name, else the
// largest listed file, else name keywords (1080p/x264 → Video, FLAC/MP3 → Audio, ...), else Other.
MediaType guessType(const TorrentRecord &r);

bool isAdult(const TorrentRecord &r, const QStringList &keywords);

struct FilterOptions {
    bool matchName = true;      // every query word must occur in the name (or a listed file path)
    bool adultFilter = false;
    QStringList adultKeywords;
};
// Does the record satisfy the query? (*why = reason when not; for logs/tests)
bool recordMatches(const TorrentRecord &r, const SearchQuery &q, const FilterOptions &opt, QString *why = nullptr);

// One hit with one BitTorrent source (+ HTTP sources for direct URLs). `magnet` is the source URI.
SearchHit recordToHit(const TorrentRecord &r, quint32 queryId, const QString &providerName, const QString &magnet);

// Per-search de-duplication by info-hash (or by URL for HTTP-only rows).
class HitMerger {
public:
    explicit HitMerger(int maxHits = 2000) : m_max(maxHits) {}
    // Merge a record; returns the index of the (new or updated) hit, or -1 if dropped (limit reached).
    // `trackers` are the configured trackers used for the canonical magnet of a new hit.
    int add(const TorrentRecord &r, quint32 queryId, const QString &providerName, const QStringList &trackers);
    const SearchHit &hit(int i) const { return m_hits.at(i); }
    int size() const { return m_hits.size(); }
    bool isFull() const { return m_hits.size() >= m_max; }
    bool contains(const QString &key) const { return m_index.contains(key); }
    int maxHits() const { return m_max; }
    static QString keyFor(const TorrentRecord &r);

private:
    struct Meta { bool bundleKnown = false; };
    void sortSources(SearchHit &h);
    QList<SearchHit> m_hits;
    QList<Meta> m_meta;
    QHash<QString, int> m_index;
    int m_max;
};

} // namespace mx::web
