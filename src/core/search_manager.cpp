// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/search_manager.h"
#include "core/dao.h"
#include "core/log.h"
#include "core/network_manager.h"
#include "core/pinger.h"
#include "core/search_hygiene.h"
#include "core/search_merge.h"

#include "mxhash/encoding.h"
#include "mxhash/links.h"

#include "mx/settings_keys.h"

#include <QtCore/QFileInfo>
#include <QtCore/QPointer>
#include <QtCore/QRegularExpression>

namespace mx {

struct SearchManager::Search {
    SearchQuery q;
    std::unique_ptr<SearchMerger> merger;
    QSet<NetworkPlugin *> pending;
    QList<QPointer<NetworkPlugin>> plugins;
    QHash<NetworkPlugin *, int> counts;
    QSet<QString> dirty;              // keys of hits to (re-)emit
    bool finished = false;
    bool stopped = false;
    bool background = false;          // [engine-4] startBackgroundSearch()
    QTimer timeout;
    // [glue-2] search by hash
    bool hashMode = false;
    QString linkText;                          // the pasted link / hash as typed
    SearchHit hashBase;                        // name / size / hashes of the wanted file
    QList<QPointer<NetworkPlugin>> lookups;    // plugins asked with findSources()
    // [glue-2] hygiene
    SearchManager::HygieneStats stats;
    hygiene::ConstantSourceCountDetector constantSeeds;
    int autoFileLists = 0;            // [ui-f] file lists asked for to classify unknown bundles
    int innerFileLists = 0;           // [merge-1] file lists asked for because a plain file may be inside
    QSet<QString> fileListAsked;      // [merge-1] bundle keys asked once
};

namespace {

const QStringList &adultWords()
{
    static const QStringList w = {
        QStringLiteral("xxx"), QStringLiteral("porn"), QStringLiteral("porno"), QStringLiteral("sex"),
        QStringLiteral("nude"), QStringLiteral("naked"), QStringLiteral("hentai"), QStringLiteral("milf"),
        QStringLiteral("fuck"), QStringLiteral("pussy"), QStringLiteral("cock"), QStringLiteral("anal"),
        QStringLiteral("blowjob"), QStringLiteral("hardcore"), QStringLiteral("erotic"), QStringLiteral("nsfw"),
        QStringLiteral("playboy"), QStringLiteral("brazzers"), QStringLiteral("camgirl"),
    };
    return w;
}

QString extensionOf(const QString &name)
{
    return QFileInfo(name).suffix().toLower();
}

} // namespace

SearchManager::SearchManager(Settings *settings, NetworkManager *network, UserListsDao *dao, QObject *parent)
    : SearchService(parent)
    , m_settings(settings)
    , m_network(network)
    , m_dao(dao)
{
    m_flushTimer.setInterval(250);
    connect(&m_flushTimer, &QTimer::timeout, this, &SearchManager::flushNow);
    if (m_network) {
        for (NetworkPlugin *p : m_network->plugins()) {
            connect(p, &NetworkPlugin::searchHits, this,
                    [this, p](quint32 id, const QList<SearchHit> &hits) { onHits(p, id, hits); });
            connect(p, &NetworkPlugin::searchDone, this, [this, p](quint32 id) { onDone(p, id); });
            connect(p, &NetworkPlugin::bundleFiles, this, &SearchManager::onBundleFiles);
            // [glue-2] search by hash: answers for our "search:<id>" pseudo downloads
            connect(p, &NetworkPlugin::sourcesFound, this,
                    [this, p](const QString &dl, const QList<SearchSource> &src) { onSourcesFound(p, dl, src); });
            connect(p, &NetworkPlugin::hashesLearned, this,
                    [this](const QString &dl, const FileHashes &more) { onHashesLearned(dl, more, -1); });
            connect(p, &NetworkPlugin::hashLinksFound, this, [this](const FileHashes &known, const FileHashes &all, qint64 size) {
                for (auto it = m_searches.constBegin(); it != m_searches.constEnd(); ++it) {
                    Search *s = it.value();
                    if (s->hashMode && !s->stopped && s->hashBase.hashes.matches(known) && !s->hashBase.hashes.conflicts(all))
                        onHashesLearned(hashSearchDownloadId(it.key()), all, size);
                }
            });
        }
    }
}

SearchManager::~SearchManager()
{
    qDeleteAll(m_searches);
}

quint32 SearchManager::startSearch(SearchQuery q)
{
    const quint32 id = m_nextId++;
    q.id = id;
    const int maxSetting = m_settings->i(keys::search::MaxResults);
    if (q.maxResults <= 0 || q.maxResults > maxSetting) q.maxResults = maxSetting;
    // [glue-2] a link / raw hash in the second line ("HASH>" in the UI) or alone in the keyword line
    // turns the search into a search by hash
    QString linkText;
    if (q.hashes.isEmpty()) {
        for (const QString &cand : {q.text2, q.text}) {
            SearchQuery t;
            if (cand.trimmed().isEmpty() || !parseHashQuery(cand, &t)) continue;
            q.hashes = t.hashes;
            q.fileSize = t.fileSize;
            q.text = t.text;
            linkText = cand.trimmed();
            break;
        }
    } else if (!q.text2.trimmed().isEmpty()) {
        linkText = q.text2.trimmed();
    }
    if (q.networks.isEmpty()) {
        for (const QString &n : m_settings->sl(keys::search::Networks)) {
            const Net net = netFromName(n);
            if (net != Net::Unknown) q.networks << net;
        }
    }

    auto *s = new Search;
    s->q = q;
    s->background = m_nextIsBackground;
    m_nextIsBackground = false;
    s->merger = std::make_unique<SearchMerger>(m_settings->i64(keys::search::MinCandidateSize),
                                               m_settings->b(keys::search::MergeByNameAndSize));
    m_searches.insert(id, s);
    if (!s->background) emit searchStarted(id);

    // history
    const QString text = (q.text + QLatin1Char(' ') + q.text2).simplified();
    if (!s->background && !text.isEmpty() && m_settings->b(keys::search::SaveHistory)) {
        const int keep = m_settings->i(keys::search::HistorySize);
        if (m_dao) {
            m_dao->addSearchHistory(text, keep);
        } else {
            m_memoryHistory.removeAll(text);
            m_memoryHistory.prepend(text);
            while (m_memoryHistory.size() > keep) m_memoryHistory.removeLast();
        }
    }

    QList<NetworkPlugin *> targets;
    if (!q.hashes.isEmpty()) {
        // [glue-2] search by hash: every network that can look up sources of a known file
        s->hashMode = true;
        s->linkText = linkText;
        if (m_network) {
            for (NetworkPlugin *p : m_network->activePlugins(NetworkPlugin::CanFindSources | NetworkPlugin::CanHashLinks))
                if (q.networks.isEmpty() || q.networks.contains(p->net())) targets << p;
        }
        startHashSearch(s, targets);
        return id;
    }
    if (m_network) {
        for (NetworkPlugin *p : m_network->activePlugins(NetworkPlugin::CanSearch))
            if (q.networks.isEmpty() || q.networks.contains(p->net())) targets << p;
    }
    qCInfo(lcSearch) << "search" << id << q.text << q.text2 << "on" << targets.size() << "networks";
    for (NetworkPlugin *p : std::as_const(targets)) {
        s->pending.insert(p);
        s->plugins << p;
        s->counts.insert(p, 0);
    }
    s->timeout.setSingleShot(true);
    s->timeout.setInterval(m_settings->i(keys::search::TimeoutSec) * 1000);
    connect(&s->timeout, &QTimer::timeout, this, [this, id] { finish(id); });
    if (targets.isEmpty()) {
        QTimer::singleShot(0, this, [this, id] {
            emit searchProgress(id, Net::Unknown, QStringLiteral("No networks are available for searching."));
            finish(id);
        });
        return id;
    }
    s->timeout.start();
    // dispatch after returning so callers can map the id before results arrive
    QTimer::singleShot(0, this, [this, id, targets] {
        Search *s = m_searches.value(id);
        if (!s || s->stopped) return;
        for (NetworkPlugin *p : targets) {
            if (!s->pending.contains(p)) continue;
            p->search(s->q);
            progress(s, p, false);
            s = m_searches.value(id);  // a synchronous plugin may have finished/closed it
            if (!s) return;
        }
    });
    return id;
}

void SearchManager::progress(Search *s, NetworkPlugin *p, bool done)
{
    const int n = s->counts.value(p);
    QString text = QStringLiteral("%1: %2 result%3").arg(netName(p->net())).arg(n).arg(n == 1 ? QLatin1String("") : QLatin1String("s"));
    if (done) text += QStringLiteral(" (done)");
    emit searchProgress(s->q.id, p->net(), text);
}

bool SearchManager::acceptHit(const SearchQuery &q, const SearchHit &h) const
{
    if (h.fileName.isEmpty()) return false;
    if (q.minSize > 0 && h.size > 0 && h.size < q.minSize) return false;
    if (q.maxSize > 0 && h.size > q.maxSize) return false;
    // [ui-f] type, extensions and bitrate: the one rule of mx::hitMatchesFileType() (bundles and
    // extension-less torrent names are classified by category / file list, unknown ones kept)
    if (!hitMatchesFileType(q, h)) return false;
    const QString ext = extensionOf(h.fileName);
    for (const QString &b : m_settings->sl(keys::search::BlockedExtensions))
        if (!b.isEmpty() && ext == b.toLower().remove(QLatin1Char('.'))) return false;
    if (m_settings->b(keys::search::AdultFilter)) {
        const QStringList words = SearchMerger::normalizedName(h.fileName).split(QRegularExpression(QStringLiteral("[ .]")),
                                                                                 Qt::SkipEmptyParts);
        for (const QString &w : words)
            if (adultWords().contains(w)) return false;
    }
    if (m_settings->b(keys::search::FakeFilter)) {
        // "song.mp3.exe", "movie.avi.scr": a media extension followed by an executable one
        static const QRegularExpression doubleExt(
            QStringLiteral("\\.(mp3|avi|mkv|mp4|wmv|mpg|mpeg|jpg|pdf|flac)\\.(exe|scr|com|pif|bat|vbs|cmd)$"),
            QRegularExpression::CaseInsensitiveOption);
        if (doubleExt.match(h.fileName).hasMatch()) return false;
        if (h.size == 0 && !h.isBundle && h.hashes.btih.isEmpty()) return false;
    }
    return true;
}

void SearchManager::onHits(NetworkPlugin *p, quint32 id, const QList<SearchHit> &hits)
{
    Search *s = m_searches.value(id);
    if (!s || s->stopped) return;
    int accepted = 0;
    const bool ed2kNet = p->net() == Net::ED2K || p->net() == Net::Kad;
    const bool dropBridges = m_settings->b(keys::search::DropTorrentBridges);
    const bool flagSuspects = m_settings->b(keys::search::FlagSuspects);
    const bool hideSuspects = flagSuspects && m_settings->b(keys::search::HideSuspects);
    for (SearchHit h : hits) {
        h.queryId = id;
        const MediaType reportedType = h.type;
        if (ed2kNet && dropBridges && hygiene::isTorrentBridge(h)) {
            ++s->stats.droppedBridges;
            continue;
        }
        if (h.type == MediaType::Any && !h.isBundle) h.type = mediaTypeForFileName(h.fileName);
        hygiene::sanitizeMedia(h);
        for (SearchSource &src : h.sources) {
            if (src.net == Net::Unknown) src.net = p->net();
            if (src.user.isValid() || !src.user.id.isEmpty()) {
                if (src.user.net == Net::Unknown) src.user.net = src.net;
            }
        }
        if (h.sources.isEmpty()) {
            SearchSource src;
            src.net = p->net();
            h.sources << src;
        }
        // a file list fetched earlier classifies the bundle before the type filter ([ui-f])
        if (h.isBundle && h.files.isEmpty() && !h.hashes.btih.isEmpty())
            h.files = m_bundleCache.value(h.hashes.btih);
        if (!acceptHit(s->q, h)) continue;
        if (flagSuspects) h.suspect = hygiene::addReason(h.suspect, hygiene::suspectReason(h, reportedType));
        if (hideSuspects && !h.suspect.isEmpty()) {
            ++s->stats.hiddenSuspects;
            continue;
        }
        const bool allowNew = s->merger->size() < s->q.maxResults;
        const SearchMerger::Result r = s->merger->add(h, allowNew);
        if (r.index < 0) continue;
        if (!r.removedKeys.isEmpty()) {
            for (const QString &k : r.removedKeys) s->dirty.remove(k);
            emit hitsRemoved(id, r.removedKeys);   // the survivor follows in the next hitsChanged
        }
        const QString mergedKey = s->merger->hits().at(r.index).key;
        s->dirty.insert(mergedKey);
        for (const QString &k : r.changedKeys) s->dirty.insert(k);   // [merge-1] inner-file links, rebuilt rows
        ++accepted;
        // [merge-1] a folder torrent whose name and size say it may hold this file (or a file of this
        // search): one file-list request, so its inner files can be matched
        for (const QString &bk : s->merger->bundlesWorthFileList(r.index)) {
            if (s->innerFileLists >= 30 || s->fileListAsked.contains(bk)) continue;
            s->fileListAsked.insert(bk);
            ++s->innerFileLists;
            QTimer::singleShot(0, this, [this, id, bk] { requestBundleFiles(id, bk); });
        }
        {
            // [ui-f] a typed search keeps bundles of unknown type; ask for their file list (rats peers
            // answer quickly) so a list that contradicts the type removes the row (onBundleFiles)
            const SearchHit &m = s->merger->hits().at(r.index);
            const bool typed = s->q.type != MediaType::Any || !queryExtensions(s->q).isEmpty();
            if (typed && m.isBundle && m.files.isEmpty() && !m.hashes.btih.isEmpty() && classifyHit(m) == MediaType::Other
                && s->autoFileLists < 20) {
                ++s->autoFileLists;
                QTimer::singleShot(0, this, [this, id, mergedKey] { requestBundleFiles(id, mergedKey); });
            }
        }
        if (flagSuspects && ed2kNet && !h.hashes.ed2k.isEmpty()) {
            // server-side fake farms report one constant complete-source count for many files
            int seeds = -1;
            for (const SearchSource &src : std::as_const(h.sources)) seeds = qMax(seeds, src.seeds);
            for (const QString &k : s->constantSeeds.add(mergedKey, h.hashes.ed2k, seeds)) {
                const int i = s->merger->indexOfKey(k);
                if (i < 0) continue;
                SearchHit &x = s->merger->hitAt(i);
                const QString reason = s->constantSeeds.reasonFor(s->constantSeeds.flaggedValue(k));
                // the count in the reason grows; keep one entry of this kind
                QStringList parts = x.suspect.isEmpty() ? QStringList() : x.suspect.split(QStringLiteral("; "));
                parts.erase(std::remove_if(parts.begin(), parts.end(),
                                           [](const QString &r) { return r.startsWith(QLatin1String("same complete-source count")); }),
                            parts.end());
                parts << reason;
                x.suspect = parts.join(QStringLiteral("; "));
                s->dirty.insert(k);
            }
        }
    }
    s->counts[p] += accepted;
    progress(s, p, !s->pending.contains(p));
    if (!s->dirty.isEmpty() && !m_flushTimer.isActive()) m_flushTimer.start();
}

void SearchManager::onDone(NetworkPlugin *p, quint32 id)
{
    Search *s = m_searches.value(id);
    if (!s || !s->pending.remove(p)) return;
    progress(s, p, true);
    if (s->pending.isEmpty()) finish(id);
}

void SearchManager::flushNow()
{
    m_flushTimer.stop();
    for (auto it = m_searches.begin(); it != m_searches.end(); ++it) {
        Search *s = it.value();
        if (s->dirty.isEmpty()) continue;
        QList<int> idx;
        for (const QString &k : std::as_const(s->dirty)) {
            const int i = s->merger->indexOfKey(k);
            if (i >= 0) idx << i;
        }
        std::sort(idx.begin(), idx.end());
        s->dirty.clear();
        if (idx.isEmpty()) continue;
        QList<SearchHit> out;
        out.reserve(idx.size());
        for (int i : idx) {
            SearchHit h = s->merger->hits().at(i);
            if (m_flagger && !s->background) m_flagger(h);
            fillHitNumbers(h);
            out << h;
        }
        emit hitsChanged(it.key(), out);
    }
}

void SearchManager::finish(quint32 id)
{
    Search *s = m_searches.value(id);
    if (!s || s->finished) return;
    flushNow();
    s->finished = true;
    s->timeout.stop();
    // plugins still pending are NOT cancelled on timeout: late hits keep merging until stop/close
    const HygieneStats hs = hygieneStats(id);
    qCInfo(lcSearch) << "search" << id << "finished with" << s->merger->size() << "hits; hygiene: dropped"
                     << hs.droppedBridges << "bridge entries, flagged" << hs.flagged << "suspects, hid" << hs.hiddenSuspects;
    QStringList perNet;
    for (auto it = s->counts.constBegin(); it != s->counts.constEnd(); ++it)
        if (it.key()) perNet << QStringLiteral("%1=%2").arg(netName(it.key()->net())).arg(it.value());
    std::sort(perNet.begin(), perNet.end());
    qCInfo(lcSearch).noquote() << "search" << id << "accepted per network:" << perNet.join(QLatin1Char(' '));
    int examples = 0;
    for (const SearchHit &h : s->merger->hits()) {
        if (h.suspect.isEmpty() || ++examples > 8) continue;
        qCInfo(lcSearch).noquote() << "  suspect:" << h.fileName << "(" << h.size << "bytes ) -" << h.suspect;
    }
    if (s->hashMode) stopHashLookups(s);
    emit searchFinished(id);
}

void SearchManager::stopSearch(quint32 id)
{
    Search *s = m_searches.value(id);
    if (!s) return;
    flushNow();
    s->stopped = true;
    for (const QPointer<NetworkPlugin> &p : std::as_const(s->plugins))
        if (p && s->pending.contains(p.data())) p->cancelSearch(id);
    s->pending.clear();
    if (s->hashMode) stopHashLookups(s);
    finish(id);
}

void SearchManager::closeSearch(quint32 id)
{
    Search *s = m_searches.value(id);
    if (!s) return;
    s->stopped = true;
    s->finished = true;
    for (const QPointer<NetworkPlugin> &p : std::as_const(s->plugins))
        if (p && s->pending.contains(p.data())) p->cancelSearch(id);
    if (s->hashMode) stopHashLookups(s);
    m_searches.remove(id);
    delete s;
}

void SearchManager::fillHitNumbers(SearchHit &h)
{
    // seeds / leechers: several indexes report the same swarm → the best figure, not a sum;
    // other networks: what each reported (eD2k/Kad source counts) or the sources it named
    int btSeeds = -1, btLeech = -1, otherSeeds = -1;
    QHash<int, int> namedByNet, reportedByNet, listedByNet;
    QSet<QString> users;
    QSet<int> nets;
    QStringList indexes;
    for (const SearchSource &s : std::as_const(h.sources)) {
        nets.insert(int(s.net));
        const QString idx = s.extra.value(QStringLiteral("providerName")).toString();
        if (!idx.isEmpty() && !indexes.contains(idx)) indexes << idx;
        if (s.net == Net::BitTorrent) {
            btSeeds = qMax(btSeeds, s.seeds);
            btLeech = qMax(btLeech, s.peers);
            continue;
        }
        if (s.seeds >= 0) otherSeeds = qMax(0, otherSeeds) + s.seeds;
        ++listedByNet[int(s.net)];
        const QString u = s.user.isValid() ? s.user.key() : (s.user.address.isEmpty() ? QString() : s.user.address);
        if (!u.isEmpty() && !users.contains(u)) { users.insert(u); ++namedByNet[int(s.net)]; }
        // [core-5] what the network reports counts also on a row that names one client (an eD2k
        // server result carries FT_SOURCES and, sometimes, one source address): the row's figure
        // and the Peers column agree
        if (s.seeds >= 0 || s.peers >= 0)
            reportedByNet[int(s.net)] = qMax(reportedByNet.value(int(s.net)), qMax(s.seeds, s.peers));
    }
    int total = -1;
    if (btSeeds >= 0 || btLeech >= 0) total = qMax(0, btSeeds) + qMax(0, btLeech);
    QSet<int> others;
    for (auto it = namedByNet.constBegin(); it != namedByNet.constEnd(); ++it) others.insert(it.key());
    for (auto it = reportedByNet.constBegin(); it != reportedByNet.constEnd(); ++it) others.insert(it.key());
    for (auto it = listedByNet.constBegin(); it != listedByNet.constEnd(); ++it) others.insert(it.key());
    for (int n : std::as_const(others)) {
        // [core-5] never fewer than the sources listed for that network
        const int k = qMax(qMax(namedByNet.value(n), reportedByNet.value(n)), listedByNet.value(n));
        if (k > 0) total = qMax(0, total) + k;
    }
    h.totalPeers = total;
    h.seeds = btSeeds >= 0 ? btSeeds + qMax(0, otherSeeds) : otherSeeds;
    h.leechers = btLeech;
    h.sourceCount = int(h.sources.size());
    QList<int> nl = nets.values();
    std::sort(nl.begin(), nl.end());
    h.networks.clear();
    for (int n : std::as_const(nl)) if (n != int(Net::Unknown)) h.networks << Net(n);
    h.indexes = indexes;
}

quint32 SearchManager::startBackgroundSearch(SearchQuery q)
{
    q.hashes = FileHashes();   // plain keyword search, never a hash search
    q.text2.clear();
    m_nextIsBackground = true;
    return startSearch(q);
}

bool SearchManager::isBackground(quint32 id) const
{
    const Search *s = m_searches.value(id);
    return s && s->background;
}

bool SearchManager::isRunning(quint32 id) const
{
    const Search *s = m_searches.value(id);
    return s && !s->finished && !s->stopped;
}

QList<SearchHit> SearchManager::hits(quint32 id) const
{
    const Search *s = m_searches.value(id);
    if (!s) return {};
    QList<SearchHit> out = s->merger->hits();
    for (SearchHit &h : out) {
        if (m_flagger && !s->background) m_flagger(h);
        fillHitNumbers(h);
    }
    return out;
}

void SearchManager::requestBundleFiles(quint32 id, const QString &hitKey)
{
    Search *s = m_searches.value(id);
    if (!s) return;
    const int i = s->merger->indexOfKey(hitKey);
    if (i < 0) return;
    const SearchHit &h = s->merger->hits().at(i);
    if (!h.isBundle || !h.files.isEmpty()) return;
    if (h.hashes.btih.isEmpty()) {   // [engine-4] nobody could ever answer: say so
        QTimer::singleShot(0, this, [this, id, hitKey] {
            emit bundleFilesUnavailable(id, hitKey, QStringLiteral("The index gave no torrent identity for this entry, so its file list is unknown"));
        });
        return;
    }
    const auto cached = m_bundleCache.constFind(h.hashes.btih);
    if (cached != m_bundleCache.constEnd()) {
        FileHashes key;
        key.btih = h.hashes.btih;
        onBundleFiles(key, cached.value());
        return;
    }
    FileHashes req;
    req.btih = h.hashes.btih;
    // [engine-4] always answer: the file list or, after a while, bundleFilesUnavailable()
    const bool first = !m_pendingBundles.contains(h.hashes.btih);
    QList<QPair<quint32, QString>> &waiting = m_pendingBundles[h.hashes.btih];
    if (!waiting.contains(qMakePair(id, hitKey))) waiting << qMakePair(id, hitKey);
    const QByteArray btih = h.hashes.btih;
    if (first) {
        QTimer::singleShot(m_bundleTimeoutMs, this, [this, btih] {
            const QList<QPair<quint32, QString>> left = m_pendingBundles.take(btih);
            for (const auto &w : left)
                emit bundleFilesUnavailable(w.first, w.second,
                                            QStringLiteral("No network could get the file list of this torrent (no peer answered within %1 s)")
                                                .arg(m_bundleTimeoutMs / 1000));
        });
    }
    if (!m_network) return;
    for (NetworkPlugin *p : m_network->activePlugins()) p->requestBundleFiles(req);
}

void SearchManager::onBundleFiles(const FileHashes &h, const QList<BundleFile> &files)
{
    if (h.btih.isEmpty() || files.isEmpty()) return;
    m_bundleCache.insert(h.btih, files);
    m_pendingBundles.remove(h.btih);   // [engine-4]
    bool any = false;
    for (auto it = m_searches.begin(); it != m_searches.end(); ++it) {
        Search *s = it.value();
        // [merge-1] the merger updates every row of that torrent: a probable single file that is a
        // folder is split again, inner files that are plain-file rows are linked
        const SearchMerger::Update u = s->merger->setBundleFiles(h.btih, files);
        QStringList removed = u.removedKeys;
        QStringList dropped;
        for (const QString &k : u.changedKeys) {
            const int i = s->merger->indexOfKey(k);
            if (i < 0) continue;
            const SearchHit &hit = s->merger->hits().at(i);
            if (!s->hashMode && hit.isBundle && hit.hashes.btih == h.btih && !hitMatchesFileType(s->q, hit)) {   // [ui-f] the list says: not that type
                dropped << k;
                continue;
            }
            s->dirty.insert(k);
            any = true;
        }
        for (const QString &k : std::as_const(dropped)) {
            s->merger->remove(k);
            s->dirty.remove(k);
            removed << k;
        }
        for (const QString &k : std::as_const(removed)) s->dirty.remove(k);
        if (!removed.isEmpty()) emit hitsRemoved(it.key(), removed);
    }
    if (any) flushNow();
}

QStringList SearchManager::history() const
{
    if (m_dao) return m_dao->searchHistory(qMax(1, m_settings->i(keys::search::HistorySize)));
    return m_memoryHistory;
}

void SearchManager::clearHistory()
{
    if (m_dao) m_dao->clearSearchHistory();
    m_memoryHistory.clear();
}

// ============================================================================ [glue-2] search by hash

bool SearchManager::parseHashQuery(const QString &textIn, SearchQuery *q, SearchSource *linkSource)
{
    const QString text = textIn.trimmed();
    if (text.isEmpty() || !q) return false;
    const QString lower = text.toLower();
    if (lower.startsWith(QLatin1String("magnet:")) || lower.startsWith(QLatin1String("ed2k://"))
        || lower.startsWith(QLatin1String("ed2k:|")) || lower.startsWith(QLatin1String("ed2k:%7c"))) {
        hash::ParsedLink l = hash::parseLink(text);
        if (!l.isValid() || l.hashes.isEmpty()) return false;
        q->hashes = l.hashes;
        q->fileSize = l.size;
        q->text = l.name;
        if (linkSource) {
            *linkSource = SearchSource();
            linkSource->uri = text;
            linkSource->net = l.kind == hash::ParsedLink::Magnet && !l.hashes.btih.isEmpty() ? Net::BitTorrent
                              : l.kind == hash::ParsedLink::Ed2k ? Net::ED2K : Net::Unknown;
        }
        return true;
    }
    // raw hash, optionally with a kind prefix ("urn:sha1:...", "ed2k:...", "btih:...", "tree:tiger:...")
    QString v = text;
    QString kind;
    if (v.startsWith(QLatin1String("urn:"), Qt::CaseInsensitive)) v = v.mid(4);
    static const QRegularExpression prefix(QStringLiteral("^(btih|ed2k|ed2khash|sha1|sha-1|tth|tree:tiger|bitprint|sha256|sha-256|md5|aich):"),
                                           QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch pm = prefix.match(v);
    if (pm.hasMatch()) {
        kind = pm.captured(1).toLower().remove(QLatin1Char('-'));
        v = v.mid(pm.capturedLength());
    }
    v = v.trimmed();
    static const QRegularExpression hexRx(QStringLiteral("^[0-9a-fA-F]+$"));
    static const QRegularExpression b32Rx(QStringLiteral("^[A-Za-z2-7]+=*$"));
    const bool isHex = hexRx.match(v).hasMatch();
    const bool isB32 = b32Rx.match(v).hasMatch();
    FileHashes h;
    if (kind == QLatin1String("bitprint")) v = v.section(QLatin1Char('.'), 0, 0), kind = QStringLiteral("sha1");
    if (isHex && v.size() == 32 && (kind.isEmpty() || kind.startsWith(QLatin1String("ed2k")))) h.ed2k = hash::fromHex(v);
    else if (isHex && v.size() == 32 && kind == QLatin1String("md5")) h.md5 = hash::fromHex(v);
    else if (isHex && v.size() == 40 && (kind.isEmpty() || kind == QLatin1String("btih"))) h.btih = hash::fromHex(v);
    else if (isHex && v.size() == 40 && kind == QLatin1String("sha1")) h.sha1 = hash::fromHex(v);
    else if (isHex && v.size() == 40 && kind == QLatin1String("aich")) h.aich = hash::fromHex(v);
    else if (isHex && v.size() == 64 && (kind.isEmpty() || kind == QLatin1String("sha256"))) h.sha256 = hash::fromHex(v);
    else if (isB32 && v.size() == 32 && (kind.isEmpty() || kind == QLatin1String("btih"))) h.btih = hash::fromBase32(v);
    else if (isB32 && v.size() == 32 && kind == QLatin1String("sha1")) h.sha1 = hash::fromBase32(v);
    else if (isB32 && v.size() == 32 && kind == QLatin1String("aich")) h.aich = hash::fromBase32(v);
    else if (isB32 && v.size() == 39 && (kind.isEmpty() || kind == QLatin1String("tth") || kind == QLatin1String("tree:tiger")))
        h.tth = hash::fromBase32(v);
    if (h.isEmpty()) return false;
    q->hashes = h;
    q->fileSize = -1;
    q->text.clear();
    if (linkSource) {
        *linkSource = SearchSource();
        if (!h.btih.isEmpty()) {
            linkSource->net = Net::BitTorrent;
            linkSource->uri = h.toMagnet(QString(), -1);
        }
    }
    return true;
}

quint32 SearchManager::searchHash(const QString &linkOrHash)
{
    SearchQuery q;
    if (!parseHashQuery(linkOrHash, &q)) {
        q = SearchQuery();
        q.text = linkOrHash;
        return startSearch(q);
    }
    q.text2 = linkOrHash.trimmed();   // keeps the link (it is itself a source), see startSearch
    return startSearch(q);
}

static QString hashDisplayName(const FileHashes &h)
{
    if (!h.ed2k.isEmpty()) return QStringLiteral("ed2k:") + QString::fromLatin1(h.ed2k.toHex());
    if (!h.sha1.isEmpty()) return QStringLiteral("sha1:") + hash::toBase32(h.sha1);
    if (!h.btih.isEmpty()) return QStringLiteral("btih:") + QString::fromLatin1(h.btih.toHex());
    if (!h.tth.isEmpty()) return QStringLiteral("tth:") + hash::toBase32(h.tth);
    if (!h.sha256.isEmpty()) return QStringLiteral("sha256:") + QString::fromLatin1(h.sha256.toHex());
    if (!h.md5.isEmpty()) return QStringLiteral("md5:") + QString::fromLatin1(h.md5.toHex());
    return QStringLiteral("unknown file");
}

void SearchManager::startHashSearch(Search *s, const QList<NetworkPlugin *> &targets)
{
    const quint32 id = s->q.id;
    SearchHit &b = s->hashBase;
    b.queryId = id;
    b.hashes = s->q.hashes;
    b.size = s->q.fileSize > 0 ? s->q.fileSize : 0;
    b.fileName = s->q.text.isEmpty() ? hashDisplayName(b.hashes) : s->q.text;
    b.type = mediaTypeForFileName(b.fileName);
    b.isBundle = !b.hashes.btih.isEmpty() && b.hashes.btFileIndex < 0 && b.hashes.ed2k.isEmpty() && b.hashes.sha1.isEmpty()
                 && b.hashes.tth.isEmpty();
    for (NetworkPlugin *p : targets) s->lookups << p;
    s->timeout.setSingleShot(true);
    s->timeout.setInterval(m_settings->i(keys::search::TimeoutSec) * 1000);
    connect(&s->timeout, &QTimer::timeout, this, [this, id] { finish(id); });
    s->timeout.start();
    qCInfo(lcSearch) << "hash search" << id << hashDisplayName(b.hashes) << "on" << targets.size() << "networks";
    QTimer::singleShot(0, this, [this, id] {
        Search *s = m_searches.value(id);
        if (!s || s->stopped) return;
        // the pasted link itself is a source (magnet → BitTorrent swarm, ed2k:// → eD2k)
        SearchQuery tmp;
        SearchSource link;
        if (!s->linkText.isEmpty() && parseHashQuery(s->linkText, &tmp, &link) && link.net != Net::Unknown) {
            SearchHit h = s->hashBase;
            h.sources = {link};
            const SearchMerger::Result r = s->merger->add(h, true);
            if (r.index >= 0) s->dirty.insert(s->merger->hits().at(r.index).key);
        } else if (!s->hashBase.hashes.btih.isEmpty()) {
            SearchHit h = s->hashBase;
            SearchSource src;
            src.net = Net::BitTorrent;
            src.uri = s->hashBase.hashes.toMagnet(s->q.text, s->q.fileSize);
            h.sources = {src};
            const SearchMerger::Result r = s->merger->add(h, true);
            if (r.index >= 0) s->dirty.insert(s->merger->hits().at(r.index).key);
        }
        const QString dl = hashSearchDownloadId(id);
        const qint64 size = s->q.fileSize > 0 ? s->q.fileSize : 0;
        const QList<QPointer<NetworkPlugin>> lookups = s->lookups;
        for (const QPointer<NetworkPlugin> &p : lookups) {
            if (!p) continue;
            if (p->capabilities() & NetworkPlugin::CanFindSources) p->findSources(dl, s->q.hashes, size, s->q.text);
            if (p->capabilities() & NetworkPlugin::CanHashLinks) p->lookupHashLinks(s->q.hashes, size);
            emit searchProgress(id, p->net(), QStringLiteral("%1: looking up sources").arg(netName(p->net())));
            s = m_searches.value(id);
            if (!s) return;
        }
        if (lookups.isEmpty()) emit searchProgress(id, Net::Unknown, QStringLiteral("No networks can look up this hash right now."));
        if (!s->dirty.isEmpty()) flushNow();
    });
}

SearchManager::Search *SearchManager::hashSearchFor(const QString &downloadId) const
{
    if (!downloadId.startsWith(QLatin1String("search:"))) return nullptr;
    bool ok = false;
    const quint32 id = downloadId.mid(7).toUInt(&ok);
    Search *s = ok ? m_searches.value(id) : nullptr;
    return s && s->hashMode && !s->stopped ? s : nullptr;
}

void SearchManager::onSourcesFound(NetworkPlugin *p, const QString &downloadId, const QList<SearchSource> &sources)
{
    Search *s = hashSearchFor(downloadId);
    if (!s || sources.isEmpty()) return;
    SearchHit h = s->hashBase;
    for (SearchSource src : sources) {
        if (src.net == Net::Unknown) src.net = p->net();
        if (src.user.net == Net::Unknown && !src.user.id.isEmpty()) src.user.net = src.net;
        h.sources << src;
    }
    const SearchMerger::Result r = s->merger->add(h, true);
    if (r.index < 0) return;
    for (const QString &k : r.changedKeys) s->dirty.insert(k);
    s->counts[p] += sources.size();
    s->dirty.insert(s->merger->hits().at(r.index).key);
    emit searchProgress(s->q.id, p->net(), QStringLiteral("%1: %2 source%3").arg(netName(p->net())).arg(s->counts.value(p))
                                               .arg(s->counts.value(p) == 1 ? QLatin1String("") : QLatin1String("s")));
    if (!m_flushTimer.isActive()) m_flushTimer.start();
}

void SearchManager::onHashesLearned(const QString &downloadId, const FileHashes &more, qint64 size)
{
    Search *s = hashSearchFor(downloadId);
    if (!s || s->hashBase.hashes.conflicts(more)) return;
    const FileHashes before = s->hashBase.hashes;
    s->hashBase.hashes.mergeFrom(more);
    if (size > 0 && s->hashBase.size <= 0) s->hashBase.size = size;
    const bool grew = !(before.ed2k == s->hashBase.hashes.ed2k && before.sha1 == s->hashBase.hashes.sha1
                        && before.btih == s->hashBase.hashes.btih && before.tth == s->hashBase.hashes.tth
                        && before.aich == s->hashBase.hashes.aich && before.sha256 == s->hashBase.hashes.sha256
                        && before.md5 == s->hashBase.hashes.md5);
    for (int i = 0; i < s->merger->size(); ++i) {
        SearchHit &x = s->merger->hitAt(i);
        if (!x.hashes.matches(before) && !x.hashes.matches(more)) continue;
        x.hashes.mergeFrom(more);
        if (x.size <= 0 && size > 0) x.size = size;
        s->dirty.insert(x.key);
    }
    if (grew) {
        // more hash kinds unlock more networks (e.g. a btih learned from the hash registry)
        const QString dl = hashSearchDownloadId(s->q.id);
        for (const QPointer<NetworkPlugin> &p : std::as_const(s->lookups))
            if (p && (p->capabilities() & NetworkPlugin::CanFindSources))
                p->findSources(dl, s->hashBase.hashes, s->hashBase.size, s->q.text);
    }
    if (!s->dirty.isEmpty() && !m_flushTimer.isActive()) m_flushTimer.start();
}

void SearchManager::stopHashLookups(Search *s)
{
    const QString dl = hashSearchDownloadId(s->q.id);
    for (const QPointer<NetworkPlugin> &p : std::as_const(s->lookups))
        if (p && (p->capabilities() & NetworkPlugin::CanFindSources)) p->stopFindSources(dl);
    s->lookups.clear();
}

// ============================================================================ [glue-2] ping

void SearchManager::pingHits(quint32 id, const QStringList &hitKeys)
{
    Search *s = m_searches.value(id);
    if (!s || !m_pinger) return;
    for (const QString &key : hitKeys) {
        const int i = s->merger->indexOfKey(key);
        if (i < 0) continue;
        const QList<SearchSource> sources = s->merger->hits().at(i).sources;
        for (int j = 0; j < sources.size() && j < 20; ++j) {
            const SearchSource src = sources.at(j);
            m_pinger->ping(src, [this, id, key, src](int ms) {
                Search *s = m_searches.value(id);
                if (!s) return;
                const int i = s->merger->indexOfKey(key);
                if (i < 0) return;
                SearchHit &h = s->merger->hitAt(i);
                for (SearchSource &e : h.sources) {
                    const bool same = e.net == src.net && e.uri == src.uri && e.user.id == src.user.id
                                      && e.user.address == src.user.address;
                    if (!same) continue;
                    e.pingMs = ms;
                    e.extra.insert(QStringLiteral("pinged"), true);
                }
                s->dirty.insert(key);
                if (!m_flushTimer.isActive()) m_flushTimer.start();
            });
        }
    }
}

SearchManager::HygieneStats SearchManager::hygieneStats(quint32 id) const
{
    const Search *s = m_searches.value(id);
    if (!s) return {};
    HygieneStats st = s->stats;
    st.hits = s->merger->size();
    st.flagged = 0;
    for (const SearchHit &h : s->merger->hits())
        if (!h.suspect.isEmpty()) ++st.flagged;
    return st;
}

} // namespace mx
