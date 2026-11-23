// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX core: SearchService implementation ("SearchManager").
// Fans a SearchQuery out to every active plugin with CanSearch (restricted to q.networks, or the
// search/networks setting, or all), merges hits with SearchMerger, filters (size / type / extension
// / bitrate / blocked extensions / adult + fake filters), sets local flags, and emits hitsChanged
// in batches (every 250 ms). Rows absorbed by a retro-merge are announced with hitsRemoved().
// Bundle file lists are fetched lazily (requestBundleFiles) and cached per info-hash. A search finishes when every plugin sent searchDone or after
// search/timeoutSec; late hits are still merged while the search is open (not stopped/closed).
#pragma once

#include <QtCore/QHash>
#include <QtCore/QSet>
#include <QtCore/QTimer>

#include <functional>
#include <memory>

#include "mx/plugin.h"
#include "mx/services.h"

namespace mx {

class NetworkManager;
class UserListsDao;
class SearchMerger;
class Pinger;

class SearchManager : public SearchService {
    Q_OBJECT
public:
    // dao may be null (no history persistence).
    SearchManager(Settings *settings, NetworkManager *network, UserListsDao *dao, QObject *parent = nullptr);
    ~SearchManager() override;

    // Sets the original WinMX legend flags (incomplete / can join / downloading / in library) on a hit.
    using LocalFlagger = std::function<void(SearchHit &)>;
    void setLocalFlagger(LocalFlagger f) { m_flagger = std::move(f); }
    void setFlushInterval(int ms) { m_flushTimer.setInterval(ms); }
    void flushNow();                                   // emit pending hitsChanged immediately

    quint32 startSearch(SearchQuery q) override;
    void stopSearch(quint32 id) override;
    void closeSearch(quint32 id) override;
    bool isRunning(quint32 id) const override;
    QList<SearchHit> hits(quint32 id) const override;
    QStringList history() const override;
    void clearHistory() override;
    // Asks every active plugin (NetworkPlugin::requestBundleFiles); answers are cached per btih.
    void requestBundleFiles(quint32 id, const QString &hitKey) override;

    // [engine-4] A search the user does not see (the download engine's background size+name
    // search for more copies of a file): no history, not counted as an own search, no local flags.
    // Results arrive through hitsChanged / searchFinished like any other search.
    quint32 startBackgroundSearch(SearchQuery q);
    bool isBackground(quint32 id) const;
    // [engine-4] SearchHit::totalPeers / seeds / leechers / sourceCount / networks / indexes from its sources
    static void fillHitNumbers(SearchHit &h);
    void setBundleTimeout(int ms) { m_bundleTimeoutMs = ms; }   // tests

    // Exposed for tests: would this hit pass the query + settings filters?
    bool acceptHit(const SearchQuery &q, const SearchHit &h) const;

    // [glue-2] search by hash (magnet / ed2k link / raw hash); pings
    quint32 searchHash(const QString &linkOrHash) override;
    void pingHits(quint32 id, const QStringList &hitKeys) override;
    void setPinger(Pinger *p) { m_pinger = p; }
    // Parses a pasted link / hash into a hash query (hashes, fileSize, text = name). *linkSource
    // (optional) receives a source for the link itself (magnet → BitTorrent, ed2k:// → eD2k) when
    // it can be downloaded from directly. false if `text` is no link or hash.
    static bool parseHashQuery(const QString &text, SearchQuery *q, SearchSource *linkSource = nullptr);
    static QString hashSearchDownloadId(quint32 id) { return QStringLiteral("search:%1").arg(id); }

    // [glue-2] search hygiene counters of one search (see core/search_hygiene.h)
    struct HygieneStats { int droppedBridges = 0; int hiddenSuspects = 0; int flagged = 0; int hits = 0; };
    HygieneStats hygieneStats(quint32 id) const;

signals:
    void searchStarted(quint32 id);   // [glue-2] (core-internal: the SPM figure)

private:
    struct Search;
    void onHits(NetworkPlugin *p, quint32 id, const QList<SearchHit> &hits);
    void onDone(NetworkPlugin *p, quint32 id);
    void onBundleFiles(const FileHashes &h, const QList<BundleFile> &files);
    void finish(quint32 id);
    void progress(Search *s, NetworkPlugin *p, bool done);
    void startHashSearch(Search *s, const QList<NetworkPlugin *> &targets);
    void onSourcesFound(NetworkPlugin *p, const QString &downloadId, const QList<SearchSource> &sources);
    void onHashesLearned(const QString &downloadId, const FileHashes &more, qint64 size);
    void stopHashLookups(Search *s);
    Search *hashSearchFor(const QString &downloadId) const;

    Settings *m_settings;
    NetworkManager *m_network;
    UserListsDao *m_dao;
    LocalFlagger m_flagger;
    quint32 m_nextId = 1;
    QHash<quint32, Search *> m_searches;
    QTimer m_flushTimer;
    QStringList m_memoryHistory;       // used when no DAO
    QHash<QByteArray, QList<BundleFile>> m_bundleCache;   // btih → files
    Pinger *m_pinger = nullptr;
    bool m_nextIsBackground = false;   // [engine-4]
    QHash<QByteArray, QList<QPair<quint32, QString>>> m_pendingBundles;   // btih → (search, hit key) waiting
    int m_bundleTimeoutMs = 20000;
};

} // namespace mx
