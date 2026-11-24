// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX core: the real multi-network DownloadEngine (see engine.h for the design).
// This file: download list, local queue, persistence, plugin-signal routing, partial sharing and
// throttled reporting. One Download (download.cpp) per file / torrent bundle does the real work.
#include "core/engine/engine.h"

#include "core/dao.h"
#include "core/database.h"
#include "core/download_request.h"
#include "core/engine/download.h"
#include "core/launcher.h"
#include "core/library.h"
#include "core/log.h"
#include "core/search_manager.h"
#include "core/search_merge.h"
#include "core/upload_tracker.h"
#include "mx/settings_keys.h"
#include "mx/trash.h"
#include "mx/durable_file.h"   // [safe-1]
#include "mxhash/encoding.h"
#include "mxhash/links.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QSaveFile>
#include <QtCore/QMutex>
#include <QtCore/QPointer>
#include <QtCore/QStorageInfo>
#include <QtCore/QThreadPool>
#include <QtCore/QTimer>

#include <algorithm>

namespace mx {

using engine::Download;

namespace {
bool finishedState(TransferState s)
{
    return s == TransferState::Completed || s == TransferState::Seeding || s == TransferState::Cancelled;
}

// [engine-4] what the Transfers tree shows of a download: an update that changes none of it is not
// sent (idle downloads with many sources do not flood the UI)
quint64 infoSignature(const TransferInfo &t)
{
    QByteArray b;
    b.reserve(512 + 96 * t.sources.size());
    auto add = [&b](const QString &x) { b += x.toUtf8(); b += '\x1f'; };
    auto num = [&b](qint64 v) { b += QByteArray::number(v); b += '\x1f'; };
    add(t.name); add(t.savePath); add(t.statusText); add(t.errorText); add(t.diagnosis);   // [diag-1] diagnosis
    num(t.size); num(t.done); num(t.rateDownBps); num(t.rateUpBps); num(t.etaSec); num(int(t.state));
    num(t.sourcesTotal); num(t.sourcesActive); num(t.priority); num(t.autoFindSourcesMin); num(t.autoEnterQueue);
    for (Net n : t.networks) num(int(n));
    b += t.pieceMap;
    b += t.pieceFill;   // [ui-i]
    b += t.hashes.ed2k + t.hashes.btih + t.hashes.sha1;
    for (const BundleFile &f : t.files) { num(f.done); num(f.wanted ? 1 : 0); }
    for (const TransferSource &s : t.sources) {
        num(int(s.net)); add(s.user.name); add(s.statusText); num(s.bytes); num(s.rateBps); add(s.id); num(s.priority); add(s.client);
    }
    return qHash(b, 0x9e3779b9u);
}

// [ui-j] a download removed with "keep the file": its record as JSON (kept-downloads.json)
QVariantMap recordToVariant(const DownloadRecord &r)
{
    QVariantMap m;
    m.insert(QStringLiteral("id"), r.id);
    m.insert(QStringLiteral("name"), r.name);
    m.insert(QStringLiteral("size"), r.size);
    m.insert(QStringLiteral("hashes"), r.hashes.toVariant());
    m.insert(QStringLiteral("saveDir"), r.saveDir);
    m.insert(QStringLiteral("savePath"), r.savePath);
    m.insert(QStringLiteral("uri"), r.uri);
    QVariantList wanted;
    for (int i : r.wantedFiles) wanted << i;
    m.insert(QStringLiteral("wanted"), wanted);
    m.insert(QStringLiteral("priority"), r.priority);
    m.insert(QStringLiteral("isBundle"), r.isBundle);
    QVariantList files;
    for (const BundleFile &f : r.files)
        files << QVariantMap{{QStringLiteral("path"), f.path}, {QStringLiteral("size"), f.size}, {QStringLiteral("index"), f.index},
                             {QStringLiteral("done"), f.done}, {QStringLiteral("wanted"), f.wanted}};
    m.insert(QStringLiteral("files"), files);
    m.insert(QStringLiteral("done"), r.done);
    m.insert(QStringLiteral("added"), r.added.toString(Qt::ISODate));
    m.insert(QStringLiteral("state"), QString::fromLatin1(r.engineState.toBase64()));
    return m;
}

DownloadRecord recordFromVariant(const QVariantMap &m)
{
    DownloadRecord r;
    r.id = m.value(QStringLiteral("id")).toString();
    r.name = m.value(QStringLiteral("name")).toString();
    r.size = m.value(QStringLiteral("size")).toLongLong();
    r.hashes = FileHashes::fromVariant(m.value(QStringLiteral("hashes")).toMap());
    r.saveDir = m.value(QStringLiteral("saveDir")).toString();
    r.savePath = m.value(QStringLiteral("savePath")).toString();
    r.uri = m.value(QStringLiteral("uri")).toString();
    for (const QVariant &v : m.value(QStringLiteral("wanted")).toList()) r.wantedFiles << v.toInt();
    r.priority = m.value(QStringLiteral("priority")).toInt();
    r.isBundle = m.value(QStringLiteral("isBundle")).toBool();
    for (const QVariant &v : m.value(QStringLiteral("files")).toList()) {
        const QVariantMap f = v.toMap();
        BundleFile b;
        b.path = f.value(QStringLiteral("path")).toString();
        b.size = f.value(QStringLiteral("size")).toLongLong();
        b.index = f.value(QStringLiteral("index")).toInt();
        b.done = f.value(QStringLiteral("done")).toLongLong();
        b.wanted = f.value(QStringLiteral("wanted"), true).toBool();
        r.files << b;
    }
    r.done = m.value(QStringLiteral("done")).toLongLong();
    r.added = QDateTime::fromString(m.value(QStringLiteral("added")).toString(), Qt::ISODate);
    r.engineState = QByteArray::fromBase64(m.value(QStringLiteral("state")).toString().toLatin1());
    return r;
}

bool pathOnDisk(const QString &p)
{
    const QFileInfo fi(p);
    return !p.isEmpty() && (fi.exists() || fi.isSymLink());
}

QString samePath(const QString &p)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(QFileInfo(p).absoluteFilePath()));
}
} // namespace

struct DownloadEngine::Private : public engine::EngineHost {
    DownloadEngine *q = nullptr;
    EngineContext ctx;
    std::unique_ptr<DownloadsDao> daoPtr;
    std::unique_ptr<HashLinksDao> linksDao;
    QThreadPool threadPool;
    engine::Tuning tune;
    QList<Download *> list;                   // display / queue order
    QHash<QString, Download *> byId;
    bool started = false;
    bool stopped = false;
    bool closing = false;      // [safe-1] beginShutdown() ran
    bool closeNow = false;     // [safe-1] "Close now": shutdown() waits for nothing
    bool uncleanStart = false; // [safe-1] the last session was not shut down
    QTimer tickTimer;
    QTimer flushTimer;
    QSet<QString> dirty;
    QHash<QString, qint64> lastEmit;
    qint64 lastTick = 0;
    qint64 lastDiskCheck = 0;
    bool rebalancePending = false;
    QSet<NetworkPlugin *> connectedNets;
    QHash<QString, QString> pendingSeeds;      // seeded local file → btih, waiting for its library hashes
    // [engine-4] downloads started from a search hit follow that hit (sources merged into it later)
    struct HitFollow { quint32 query = 0; QString key; QString downloadId; };
    QList<HitFollow> follows;
    QHash<quint32, QString> nameSearches;      // background size+name search id → download id
    qint64 lastNameSearchMs = 0;
    QString requestWhat;                       // [engine-4] downloadNotStarted(): the hit key being downloaded
    QHash<QString, quint64> lastSig;           // signature of the last transferChanged per download
    // [str-1] the next new download is a stream (setNextStream)
    bool nextStream = false;
    int nextStreamFile = -1;
    QString nextStreamDir;

    // A library file seeded as a torrent (bt/seedLibrary or explicit): link its btih with the
    // library's hashes, keep it in HashLinksDao and publish it, so a magnet-only downloader can
    // find the same file on the other networks through the hash-link registry.
    void linkSeed(const QString &path)
    {
        const QString id = pendingSeeds.value(path);
        if (id.isEmpty() || !ctx.library) return;
        SharedFile f;
        bool found = false;
        for (const SharedFile &x : ctx.library->files())
            if (x.hashed && QFileInfo(x.path).absoluteFilePath() == path) { f = x; found = true; break; }
        if (!found) return;   // fileHashed will call us again
        pendingSeeds.remove(path);
        FileHashes all = f.hashes;
        all.btih = hash::fromHex(id);
        all.btFileIndex = -1;   // seedLocalFile makes single-file torrents: torrent == file (matches magnets)
        if (all.btih.size() != 20) return;
        if (linksDao) linksDao->upsert(all, f.size, f.name, QStringLiteral("library"));
        for (NetworkPlugin *p : std::as_const(ctx.plugins))
            if ((p->capabilities() & NetworkPlugin::CanHashLinks) && pluginUsable(p)) p->publishHashLinks(all, f.size, f.name);
        qCInfo(lcEngine) << "hash link published for seeded file" << f.name << ": btih" << id.left(8) << "↔ ed2k"
                         << f.hashes.ed2k.toHex().left(8) << "sha1" << f.hashes.sha1.toHex().left(8);
    }
    // partial sharing registry (read from plugin threads)
    struct PartialEntry {
        FileHashes hashes;
        qint64 size = 0;
        QString name, path;
        std::function<QByteArray(qint64, qint64)> reader;
        QList<QPair<qint64, qint64>> have;   // [up-1] verified against trusted hashes
        QList<QByteArray> ed2kParts;          // [up-1] the trusted set (empty: none)
        int btFile = -1;                      // [up-1] a single-file torrent: the index of its file
        // [up-1] the same file: by any hash; a single-file torrent also by its info-hash with the
        // file's index or without one (the whole torrent is that file)
        bool same(const FileHashes &h) const
        {
            if (hashes.conflicts(h) && !(btFile >= 0 && h.btih == hashes.btih)) return false;
            if (btFile >= 0 && !h.btih.isEmpty() && h.btih == hashes.btih) return h.btFileIndex < 0 || h.btFileIndex == btFile;
            return hashes.matches(h);
        }
    };
    QSet<QString> lastBusy;                    // [up-1] what the library was told last
    std::shared_ptr<QMutex> partialMutex = std::make_shared<QMutex>();
    std::shared_ptr<QList<PartialEntry>> partials = std::make_shared<QList<PartialEntry>>();

    // ---- EngineHost
    Settings *settings() const override { return ctx.settings; }
    DownloadsDao *dao() const override { return daoPtr.get(); }
    HashLinksDao *links() const override { return linksDao.get(); }
    Library *library() const override { return ctx.library; }
    TorrentBackend *backend() const override { return ctx.torrents; }
    QList<NetworkPlugin *> plugins() const override { return ctx.plugins; }
    bool pluginUsable(NetworkPlugin *p) const override
    {
        if (!p) return false;
        if (ctx.settings) {
            const QString k = keys::enabledKey(p->net());
            if (!k.isEmpty() && !ctx.settings->value(k).toBool() && ctx.settings->value(k).isValid()) return false;
        }
        return p->status().state != ConnState::Disabled || p->status().net == Net::Unknown;
    }
    NetworkPlugin *pluginFor(Net n) const override
    {
        NetworkPlugin *fallback = nullptr;
        for (NetworkPlugin *p : ctx.plugins) {
            if (!(p->capabilities() & NetworkPlugin::CanFetchRanges) || !pluginUsable(p)) continue;
            if (p->net() == n) return p;
            if (n == Net::Kad && p->net() == Net::ED2K) fallback = p;
        }
        return fallback;
    }
    QThreadPool *pool() const override { return const_cast<QThreadPool *>(&threadPool); }
    const engine::Tuning &tuning() const override { return tune; }
    QString incomingDir() const override
    {
        QString d = ctx.settings ? ctx.settings->s(keys::transfers::IncomingDir) : QString();
        if (d.isEmpty()) d = ctx.profileDir + QStringLiteral("/incoming");
        return QDir::cleanPath(d);
    }
    QString incompleteDir() const override
    {
        QString d = ctx.settings ? ctx.settings->s(keys::transfers::IncompleteDir) : QString();
        if (d.isEmpty()) d = ctx.profileDir + QStringLiteral("/incomplete");
        return QDir::cleanPath(d);
    }
    int openSourcesGlobal() const override
    {
        int n = 0;
        for (Download *d : list) n += d->openSources();
        return n;
    }
    qint64 bufferedGlobal() const override
    {
        qint64 n = 0;
        for (Download *d : list) n += d->bufferedBytes();
        return n;
    }
    void changed(Download *d, bool important) override
    {
        if (!started || stopped) return;
        dirty.insert(d->id());
        if (important) {
            const qint64 now = engine::nowMs();
            if (now - lastEmit.value(d->id()) >= 1000) emitNow(d);
        }
    }
    void finished(Download *) override
    {
        requestRebalance();
        requestChecks();   // [safe-1] a check after an unclean stop ended: the next one
        updatePartials();   // [up-1] the finished file is no longer busy: the library may share it now
    }
    void completedFile(Download *d) override
    {
        const TransferInfo t = d->info();
        dirty.remove(d->id());
        emitNow(d, true);
        emit q->downloadCompleted(t);
        if (ctx.settings && ctx.settings->b(keys::transfers::AutoClearCompleted)) {
            const int sec = ctx.settings->i(keys::transfers::ClearCompleteDownloadsSec);
            const QString id = d->id();
            QTimer::singleShot(qMax(0, sec) * 1000, q, [this, id] {
                Download *x = byId.value(id);
                if (x && x->isFinished()) removeDownload(x, FileRemoval::Keep);
            });
        }
    }

    void notice(Download *d, const QString &text) override   // [ver-1]
    {
        if (!started || stopped) return;
        emitNow(d, true);
        emit q->downloadNotice(d->id(), text);
    }
    void sharingChanged() override { updatePartials(); }   // [ver-1]

    // ---- [ver-1] Restart Download: the old data goes (by the rules of removeDownload with delete),
    // the download starts again from zero under the same id, in the same place of the list
    struct Disposal {
        QStringList trashed, deleted, failed, errors;
    };
    void restart(Download *old)
    {
        const QString id = old->id();
        const int idx = int(list.indexOf(old));
        QStringList prune;
        const DownloadDiskInfo di = old->diskInfo(&prune);
        TorrentBackend *be = ctx.torrents;
        const QString tid = old->torrentId();
        // a torrent leaves the session (libtorrent lets go of its files), its files go, then the same
        // torrent is added again (its .torrent or magnet: the info-hash makes it the same metadata)
        const bool inBackend = di.torrent && be && !tid.isEmpty() && be->torrents().contains(tid);
        if (inBackend) removingTorrents.insert(tid);
        old->stopForRestart(false);
        DownloadRecord r = old->restartRecord();
        r.engineState = old->restartBlob(false);
        auto *n = new Download(this, r, q);
        n->restoreState(r.engineState);
        list[idx] = n;
        byId.insert(id, n);
        lastSig.remove(id);
        lastEmit.remove(id);
        dirty.remove(id);
        QList<SearchSource> srcs;
        if (daoPtr)
            for (const SourceRecord &s : daoPtr->sources(id)) srcs << s.source;
        n->addSources(srcs, nullptr, true);
        old->deleteLater();
        qCInfo(lcEngine) << "restart" << id << r.name << ": disposing of" << di.paths << (inBackend ? "(once the torrent left the session)" : "");
        const bool forGood = !di.trash;   // the question said "deleted for good" where there is no trash
        if (inBackend) {
            n->setRestartPending(true);
            pendingDeletes << PendingDelete{id, r.name, tid, di.paths, prune, forGood, true};
            QTimer::singleShot(10000, q, [this, id] { flushPendingDelete(id, QString()); });
        } else {
            n->persist(true);
            const trash::Result res = trash::dispose(di.paths, forGood);
            for (const QString &dir : prune)
                if (QFileInfo(dir).isDir() && !QFileInfo(dir).isSymLink()) QDir().rmdir(dir);
            if (ctx.library && (!res.trashed.isEmpty() || !res.deleted.isEmpty())) ctx.library->forgetPaths(res.trashed + res.deleted);
            restartNotice(n, Disposal{res.trashed, res.deleted, res.failed, res.errors});
        }
        emitNow(n, true);
        updatePartials();
        requestRebalance();
    }
    void restartNotice(Download *n, const Disposal &r)
    {
        const QString name = QStringLiteral("'%1'").arg(n->record().name);
        QString text = QStringLiteral("%1 starts again from zero").arg(name);
        if (!r.trashed.isEmpty()) text += QStringLiteral(": its old data went to the %1").arg(trash::placeName());
        else if (!r.deleted.isEmpty()) text += QStringLiteral(": its old data was deleted for good");
        text += QLatin1Char('.');
        if (!r.failed.isEmpty())
            text += QStringLiteral(" Could not be removed and stays on the disk: %1 (%2).").arg(QDir::toNativeSeparators(r.failed.first()), r.errors.value(0));
        qCInfo(lcEngine).noquote() << n->id() << text;
        notice(n, text);
    }

    // ---- helpers
    void emitNow(Download *d, bool force = false)
    {
        lastEmit.insert(d->id(), engine::nowMs());
        dirty.remove(d->id());
        const TransferInfo t = d->info();
        const quint64 sig = infoSignature(t);
        if (!force && lastSig.value(d->id()) == sig) return;   // [engine-4] nothing visible changed
        lastSig.insert(d->id(), sig);
        emit q->transferChanged(t);
    }
    // [engine-4] hits of searches downloads follow, and the background size+name searches
    void onSearchHits(quint32 id, const QList<SearchHit> &hits)
    {
        if (nameSearches.contains(id)) {
            if (Download *x = byId.value(nameSearches.value(id)); x && !x->isFinished()) {
                x->nameSearchHits(hits);
                // [merge-1] folder torrents that may hold the file: their file lists decide
                if (ctx.search)
                    for (const QString &k : x->takeFileListWants()) ctx.search->requestBundleFiles(id, k);
            }
            return;
        }
        for (int i = 0; i < follows.size();) {
            const HitFollow f = follows.at(i);
            Download *x = byId.value(f.downloadId);
            if (!x || x->isFinished()) { follows.removeAt(i); continue; }
            ++i;
            if (f.query != id) continue;
            for (const SearchHit &h : hits) {
                const bool same = h.key == f.key
                                  || (!h.hashes.isEmpty() && x->hashes().matches(h.hashes) && !x->hashes().conflicts(h.hashes));
                if (same) x->addHitSources(h);
            }
        }
    }
    void onSearchFinished(quint32 id)
    {
        if (!nameSearches.contains(id)) return;
        if (Download *x = byId.value(nameSearches.value(id))) x->nameSearchFinished();
        // late answers still merge for a minute, then the hidden search is dropped
        QTimer::singleShot(60000, q, [this, id] {
            nameSearches.remove(id);
            if (ctx.search && !stopped) ctx.search->closeSearch(id);
        });
    }
    void startNameSearches(qint64 now)
    {
        if (!ctx.search || now - lastNameSearchMs < 10000) return;   // one background search per 10 s
        for (Download *x : std::as_const(list)) {
            if (!x->wantsNameSearch(now)) continue;
            const SearchQuery qq = x->nameSearchQuery();
            if (qq.networks.isEmpty() || qq.text.isEmpty()) continue;   // nobody to ask right now
            const quint32 sid = ctx.search->startBackgroundSearch(qq);
            nameSearches.insert(sid, x->id());
            x->nameSearchStarted(now, qq.networks);
            lastNameSearchMs = now;
            return;
        }
    }
    void flush()
    {
        if (stopped) return;
        const qint64 now = engine::nowMs();
        const QSet<QString> ids = dirty;
        for (const QString &id : ids) {
            Download *d = byId.value(id);
            if (!d) { dirty.remove(id); continue; }
            if (now - lastEmit.value(id) >= 1000) emitNow(d);
        }
    }
    // [safe-1] step D: after an unclean stop every unfinished download that holds data is verified
    // before it runs or is shared, kChecksAtOnce at a time (the others wait, their status says so)
    static constexpr int kChecksAtOnce = 2;
    bool checksPending = false;
    void requestChecks()
    {
        if (checksPending || stopped) return;
        checksPending = true;
        QTimer::singleShot(0, q, [this] { checksPending = false; startChecks(); });
    }
    void startChecks()
    {
        if (!started || stopped || closing) return;
        int running = 0;
        for (Download *d : std::as_const(list))
            if (d->checkFirst() && d->isVerifying()) ++running;
        const QList<Download *> l = list;   // a check that ends at once may change the list order
        for (Download *d : l) {
            if (running >= kChecksAtOnce) break;
            if (!byId.contains(d->id()) || !d->checkFirst() || d->isVerifying()) continue;
            if (d->startCheckFirst() && d->isVerifying()) ++running;
        }
    }
    void requestRebalance()
    {
        if (rebalancePending || stopped) return;
        rebalancePending = true;
        QTimer::singleShot(0, q, [this] { rebalancePending = false; rebalance(); });
    }
    void rebalance()
    {
        if (!started || stopped || closing) return;
        QList<Download *> run;
        // [str-1] a player waits for a stream: it always runs and takes no place of "downloads at once"
        for (Download *d : list) {
            if (!d->wantsToRun()) continue;
            if (d->streaming()) { if (!d->isActive()) d->activate(); continue; }
            run << d;
        }
        std::stable_sort(run.begin(), run.end(), [](Download *a, Download *b) {
            if (a->record().priority != b->record().priority) return a->record().priority > b->record().priority;
            return a->record().sortOrder < b->record().sortOrder;
        });
        const int maxActive = ctx.settings ? qMax(1, ctx.settings->i(keys::transfers::MaxDownloads)) : 40;
        for (int i = 0; i < run.size(); ++i) {
            if (i >= maxActive && run.at(i)->isActive()) run.at(i)->deactivate();
        }
        for (Download *d : list)
            if (!d->wantsToRun() && d->isActive()) d->deactivate();
        for (int i = 0; i < run.size() && i < maxActive; ++i)
            if (!run.at(i)->isActive()) run.at(i)->activate();
        for (int i = maxActive; i < run.size(); ++i) run.at(i)->deactivate();   // status "Locally Queued"
    }
    Download *adopt(const DownloadRecord &rec)
    {
        auto *d = new Download(this, rec, q);
        list << d;
        byId.insert(rec.id, d);
        return d;
    }
    // [ui-j] removing a download: Keep (finished file stays; partial data stays and is remembered
    // so the download can continue from it) or Trash / TrashOrDelete (its own data, nothing else)
    struct KeptDownload {
        DownloadRecord rec;
        QStringList paths;                    // where its data is
        qint64 held = 0;
        QDateTime keptAt;
    };
    QList<KeptDownload> kept;
    struct PendingDelete {
        QString id, name, torrentId;
        QStringList paths, pruneDirs;
        bool forGood = false;
        bool restart = false;                 // [ver-1] Restart Download: the same download takes the torrent up again
    };
    QList<PendingDelete> pendingDeletes;      // torrents: their files go once libtorrent let them go
    QSet<QString> removingTorrents;           // torrents removed with their download, until the backend confirms
    qint64 lastKeptCheck = 0;

    QString keptFile() const
    {
        return ctx.profileDir.isEmpty() ? QString() : QDir(ctx.profileDir).filePath(QStringLiteral("kept-downloads.json"));
    }
    void loadKept()
    {
        kept.clear();
        QFile f(keptFile());
        if (keptFile().isEmpty() || !f.open(QIODevice::ReadOnly)) return;
        const QVariantMap root = QJsonDocument::fromJson(f.readAll()).toVariant().toMap();
        for (const QVariant &v : root.value(QStringLiteral("kept")).toList()) {
            const QVariantMap m = v.toMap();
            KeptDownload k;
            k.rec = recordFromVariant(m.value(QStringLiteral("record")).toMap());
            k.paths = m.value(QStringLiteral("paths")).toStringList();
            k.held = m.value(QStringLiteral("held")).toLongLong();
            k.keptAt = QDateTime::fromString(m.value(QStringLiteral("keptAt")).toString(), Qt::ISODate);
            if (!k.rec.id.isEmpty() && !k.paths.isEmpty()) kept << k;
        }
        pruneKept();
    }
    void saveKept()
    {
        if (keptFile().isEmpty()) return;
        if (kept.isEmpty()) {
            QFile::remove(keptFile());
            return;
        }
        QVariantList l;
        for (const KeptDownload &k : std::as_const(kept))
            l << QVariantMap{{QStringLiteral("record"), recordToVariant(k.rec)}, {QStringLiteral("paths"), k.paths},
                             {QStringLiteral("held"), k.held}, {QStringLiteral("keptAt"), k.keptAt.toString(Qt::ISODate)}};
        QSaveFile f(keptFile());
        if (f.open(QIODevice::WriteOnly)) {
            f.write(QJsonDocument::fromVariant(QVariantMap{{QStringLiteral("v"), 1}, {QStringLiteral("kept"), l}}).toJson(QJsonDocument::Indented));
            if (!f.commit()) qCWarning(lcEngine) << "cannot write" << keptFile();
        }
    }
    // kept data that is gone from the disk (deleted by the user, resumed elsewhere) is forgotten
    void pruneKept()
    {
        bool changedAny = false;
        for (int i = int(kept.size()) - 1; i >= 0; --i) {
            bool any = false;
            for (const QString &p : std::as_const(kept.at(i).paths)) any = any || pathOnDisk(p);
            if (!any) {
                qCInfo(lcEngine) << "kept partial download" << kept.at(i).rec.id << kept.at(i).rec.name << "is gone from the disk";
                kept.removeAt(i);
                changedAny = true;
            }
        }
        while (kept.size() > 500) { kept.removeFirst(); changedAny = true; }
        if (changedAny) saveKept();
    }
    int keptForRequest(const DownloadRequest &req) const
    {
        const QString uri = req.uri.trimmed();
        for (int i = 0; i < kept.size(); ++i) {
            const DownloadRecord &r = kept.at(i).rec;
            if (!req.hashes.isEmpty() && r.hashes.matches(req.hashes)) return i;
            if (req.hashes.isEmpty() && !uri.isEmpty() && r.uri.trimmed() == uri) return i;
        }
        // the rule of the "matches incomplete local file" mark (RealCore::flagHit): same name and size
        if (req.size >= 1024 * 1024 && !req.name.isEmpty()) {
            const QString norm = SearchMerger::normalizedName(req.name);
            for (int i = 0; i < kept.size(); ++i) {
                const DownloadRecord &r = kept.at(i).rec;
                if (r.size == req.size && !r.hashes.conflicts(req.hashes) && SearchMerger::normalizedName(r.name) == norm) return i;
            }
        }
        return -1;
    }
    int keptForPath(const QString &path) const
    {
        const QString p = samePath(path);
        for (int i = 0; i < kept.size(); ++i)
            for (const QString &k : kept.at(i).paths)
                if (samePath(k) == p) return i;
        return -1;
    }
    // the kept download comes back to the list and continues from its data
    Download *resurrect(int i, const DownloadRequest *req)
    {
        KeptDownload k = kept.takeAt(i);
        saveKept();
        DownloadRecord r = k.rec;
        if (byId.contains(r.id)) r.id = newDownloadId();
        const bool paused = (req && req->startPaused) || (ctx.settings && ctx.settings->b(keys::transfers::StartPaused));
        r.state = int(paused ? TransferState::Paused : TransferState::Queued);
        r.statusText = transferStateText(TransferState(r.state));
        r.errorText.clear();
        r.sortOrder = list.isEmpty() ? 0 : list.last()->record().sortOrder + 1;
        Download *x = adopt(r);
        x->restoreState(r.engineState);
        if (x->isPausedByUser() && !paused) x->setUserPaused(false);
        if (req) {
            x->mergeRequest(*req);
            x->addSources(req->sources, nullptr, true);
        }
        x->persist(true);
        qCInfo(lcEngine) << "resumed kept download" << r.id << r.name << "from" << k.paths << "with" << k.held << "bytes";
        emit q->transferAdded(x->info());
        rebalance();
        return x;
    }
    void removeDownload(Download *d, FileRemoval mode)
    {
        // [str-1] a stream's cache never goes to the trash and is never kept: the stream service deletes it
        if (d->streamRestricts()) {
            q->dropStream(d->id());
            return;
        }
        const QString id = d->id();
        const QString name = d->record().name;
        const bool finished = d->isFinished();
        QStringList prune;
        const DownloadDiskInfo di = d->diskInfo(&prune);
        const bool keep = mode == FileRemoval::Keep;
        const QString torrentId = d->torrentId();
        TorrentBackend *be = ctx.torrents;
        const bool inBackend = be && !torrentId.isEmpty() && be->torrents().contains(torrentId);
        const bool waitTorrent = !keep && inBackend;
        if (inBackend) removingTorrents.insert(torrentId);
        d->stopForRemoval(keep);
        if (keep && !finished && !di.paths.isEmpty()) {
            KeptDownload k;
            k.rec = d->record();
            k.rec.engineState = d->saveState();
            k.paths = di.paths;
            k.held = di.bytes;
            k.keptAt = QDateTime::currentDateTime();
            for (int i = int(kept.size()) - 1; i >= 0; --i)
                if (kept.at(i).rec.id == k.rec.id) kept.removeAt(i);
            kept << k;
            saveKept();
            qCInfo(lcEngine) << "removed" << id << name << "from the list, kept its partial data:" << di.paths;
        }
        list.removeAll(d);
        byId.remove(id);
        dirty.remove(id);
        lastEmit.remove(id);
        lastSig.remove(id);
        if (daoPtr) daoPtr->remove(id);
        for (int i = int(follows.size()) - 1; i >= 0; --i)
            if (follows.at(i).downloadId == id) follows.removeAt(i);
        d->deleteLater();
        emit q->transferRemoved(id, false);
        updatePartials();
        requestRebalance();
        if (keep) return;
        PendingDelete p{id, name, torrentId, di.paths, prune, mode == FileRemoval::TrashOrDelete};
        if (waitTorrent) {
            // libtorrent closes the files when the torrent has left the session (torrentRemoved)
            pendingDeletes << p;
            QTimer::singleShot(10000, q, [this, id] { flushPendingDelete(id, QString()); });
        } else {
            QTimer::singleShot(0, q, [this, p] { runDelete(p); });
        }
    }
    void flushPendingDelete(const QString &id, const QString &torrentId)
    {
        for (int i = 0; i < pendingDeletes.size(); ++i) {
            const PendingDelete &p = pendingDeletes.at(i);
            if ((!id.isEmpty() && p.id == id) || (!torrentId.isEmpty() && p.torrentId == torrentId)) {
                const PendingDelete x = pendingDeletes.takeAt(i);
                // a moment for the disk thread to close its handles
                QTimer::singleShot(300, q, [this, x] { runDelete(x); });
                return;
            }
        }
    }
    void runDelete(const PendingDelete &p)
    {
        if (p.restart) {   // [ver-1] the old files of a restarted torrent download: gone, then it starts
            Download *n = byId.value(p.id);
            if (!n || !n->restartPending()) return;
            const trash::Result r = trash::dispose(p.paths, p.forGood);
            for (const QString &dir : p.pruneDirs)
                if (QFileInfo(dir).isDir() && !QFileInfo(dir).isSymLink()) QDir().rmdir(dir);
            if (ctx.library && (!r.trashed.isEmpty() || !r.deleted.isEmpty())) ctx.library->forgetPaths(r.trashed + r.deleted);
            n->setRestartPending(false);
            restartNotice(n, Disposal{r.trashed, r.deleted, r.failed, r.errors});
            requestRebalance();
            return;
        }
        // the same torrent was taken up again by a download meanwhile: its files are that download's now
        if (!p.torrentId.isEmpty() && findByTorrent(p.torrentId)) {
            qCWarning(lcEngine) << "not deleting the data of" << p.id << ": the torrent is downloading again";
            emit q->downloadFilesDeleted(p.id, p.name, {}, {}, p.paths,
                                         QStringList(p.paths.size(), QStringLiteral("it is used by a download again")));
            return;
        }
        const trash::Result r = trash::dispose(p.paths, p.forGood);
        for (const QString &dir : p.pruneDirs)
            if (QFileInfo(dir).isDir() && !QFileInfo(dir).isSymLink()) QDir().rmdir(dir);   // only when empty
        if (ctx.library && (!r.trashed.isEmpty() || !r.deleted.isEmpty())) ctx.library->forgetPaths(r.trashed + r.deleted);
        qCInfo(lcEngine) << "deleted the data of" << p.id << p.name << ": trash" << r.trashed << "for good" << r.deleted
                         << "failed" << r.failed << r.errors;
        emit q->downloadFilesDeleted(p.id, p.name, r.trashed, r.deleted, r.failed, r.errors);
    }
    Download *findByTorrent(const QString &id) const
    {
        for (Download *d : list) if (d->torrentId() == id) return d;
        return nullptr;
    }
    Download *findByPromote(const QString &id) const
    {
        for (Download *d : list) if (!d->promoteTorrentId().isEmpty() && d->promoteTorrentId() == id) return d;
        return nullptr;
    }
    void tick()
    {
        if (stopped) return;
        const qint64 now = engine::nowMs();
        if (now - lastTick >= 950) {
            lastTick = now;
            for (Download *d : std::as_const(list)) d->tick(now);
            startChecks();   // [safe-1]
            startNameSearches(now);
            updatePartials();
            if (now - lastKeptCheck > 30000) {   // [ui-j]
                lastKeptCheck = now;
                pruneKept();
            }
            if (now - lastDiskCheck > 10000) {
                lastDiskCheck = now;
                checkDiskSpace();
            }
        }
        flush();
    }
    void checkDiskSpace()
    {
        if (!ctx.settings) return;
        const qint64 minFree = qint64(ctx.settings->i(keys::transfers::MinFreeSpaceMB)) << 20;
        if (minFree <= 0) return;
        const QStorageInfo si(incompleteDir());
        if (!si.isValid() || si.bytesAvailable() < 0 || si.bytesAvailable() >= minFree) return;
        for (Download *d : std::as_const(list)) {
            if (d->isActive() && d->partStore()) {
                qCWarning(lcEngine) << d->id() << "low disk space:" << si.bytesAvailable() / (1 << 20) << "MB free";
                d->record().errorText = QStringLiteral("Low disk space");
                d->setUserPaused(true);
                d->record().errorText = QStringLiteral("Low disk space");
                d->persist(true);
                changed(d, true);
            }
        }
        requestRebalance();
    }
    void updatePartials()
    {
        QList<PartialEntry> l;
        QSet<QString> busy;
        const bool on = !ctx.settings || ctx.settings->b(keys::library::ShareIncomplete);
        for (Download *d : std::as_const(list)) {
            // [up-1] files a running download writes are never hashed or shared as complete files
            for (const QString &p : d->busyPaths()) busy.insert(p);
            if (!on) continue;
            // [up-1] part files and single-file torrents: only what trusted hashes verified
            engine::Download::ServeView v = d->serveView();
            if (!v.ok) continue;
            PartialEntry e;
            e.hashes = v.hashes;
            e.size = v.size;
            e.name = v.name;
            e.path = v.path;
            e.reader = std::move(v.reader);
            for (const engine::Range &r : v.verified.ranges()) e.have << qMakePair(r.first, r.second);
            e.ed2kParts = v.ed2kParts;
            e.btFile = v.btFile;
            if (e.btFile >= 0) e.hashes.btFileIndex = -1;   // the torrent is this one file
            l << e;
        }
        {
            QMutexLocker lock(partialMutex.get());
            *partials = l;
        }
        if (ctx.library && busy != lastBusy) {
            lastBusy = busy;
            ctx.library->setBusyPaths(busy);
        }
    }
    void installPartialProvider()
    {
        if (!ctx.library) return;
        auto mtx = partialMutex;
        auto parts = partials;
        ctx.library->setPartialProvider(
            [mtx, parts](const FileHashes &h, SharedFile *out) {
                QMutexLocker l(mtx.get());
                for (const PartialEntry &e : std::as_const(*parts)) {
                    if (!e.same(h)) continue;
                    if (out) {
                        out->path = e.path;
                        out->name = e.name;
                        out->size = e.size;
                        out->hashes = e.hashes;
                        out->hashed = true;
                        out->type = mediaTypeForFileName(e.name);
                    }
                    return true;
                }
                return false;
            },
            [mtx, parts](const FileHashes &h, qint64 off, qint64 len) -> QByteArray {
                std::function<QByteArray(qint64, qint64)> reader;
                {
                    QMutexLocker l(mtx.get());
                    for (const PartialEntry &e : std::as_const(*parts))
                        if (e.same(h)) { reader = e.reader; break; }
                }
                return reader ? reader(off, len) : QByteArray();
            });
        ctx.library->setPartialDetails(   // [up-1]
            [mtx, parts](const FileHashes &h, QList<QPair<qint64, qint64>> *have, QList<QByteArray> *ed2kParts) {
                QMutexLocker l(mtx.get());
                for (const PartialEntry &e : std::as_const(*parts)) {
                    if (!e.same(h)) continue;
                    if (have) *have = e.have;
                    if (ed2kParts) *ed2kParts = e.ed2kParts;
                    return true;
                }
                return false;
            });
    }
    void connectPlugins()
    {
        for (NetworkPlugin *p : std::as_const(ctx.plugins)) {
            QPointer<NetworkPlugin> pp(p);
            QObject::connect(p, &NetworkPlugin::sourcesFound, q, [this, pp](const QString &id, const QList<SearchSource> &s) {
                if (Download *d = byId.value(id); d && !d->isFinished()) d->addSources(s, pp, false);
            });
            QObject::connect(p, &NetworkPlugin::hashesLearned, q, [this, pp](const QString &id, const FileHashes &h) {
                // [core-5] Gnutella hashes come from one servent's hit or THEX tree and nobody vouches
                // for them: provisional (a hint, its tree kept aside) until the whole-file check
                // against the trusted kinds confirms or refutes them; a liar is banned then, the
                // download goes on. Other networks' hashes are bound to the hash they were asked by.
                const bool provisional = pp && pp->net() == Net::Gnutella;
                if (Download *d = byId.value(id); d && !d->isFinished())
                    d->learnHashes(h, !provisional, pp ? netName(pp->net()) : QStringLiteral("plugin"));
            });
            QObject::connect(p, &NetworkPlugin::hashSetLearned, q,
                             [this](const QString &id, const QString &kind, qint64 unit, const QList<QByteArray> &hs) {
                                 if (Download *d = byId.value(id); d && !d->isFinished()) d->learnHashSet(kind, unit, hs);
                             });
            QObject::connect(p, &NetworkPlugin::statusChanged, q, [this, pp](const NetworkStatus &st) {
                if (!pp) return;
                // "up" = connected AND talking to somebody (an overlay node alone is "Connected" too)
                const bool up = st.state == ConnState::Connected && (st.peers > 0 || pp->net() == Net::Http);
                const bool was = connectedNets.contains(pp.data());
                if (up && !was) {
                    connectedNets.insert(pp.data());
                    for (Download *d : std::as_const(list)) d->networkCameUp(pp);
                } else if (!up && was) {
                    connectedNets.remove(pp.data());
                }
            });
            if (p->status().state == ConnState::Connected && p->status().peers > 0) connectedNets.insert(p);
            QObject::connect(p, &NetworkPlugin::hashLinksFound, q, [this](const FileHashes &known, const FileHashes &all, qint64 size) {
                for (Download *d : std::as_const(list))
                    if (!d->isFinished() && d->matchesLink(known, size)) d->hashLinksFound(all, size);
            });
        }
        if (ctx.torrents) {
            QObject *ev = ctx.torrents->torrentEvents();
            auto *te = qobject_cast<TorrentEvents *>(ev);
            if (te) {
                QObject::connect(te, &TorrentEvents::torrentMetadata, q, [this](const QString &id) {
                    if (Download *d = findByTorrent(id)) d->onTorrentMetadata();
                    else if (Download *p = findByPromote(id)) p->onTorrentMetadata();
                });
                QObject::connect(te, &TorrentEvents::torrentChanged, q, [this](const QString &id) {
                    if (Download *d = findByTorrent(id)) d->onTorrentChanged();
                    else if (Download *p = findByPromote(id)) p->onTorrentMetadata();
                });
                QObject::connect(te, &TorrentEvents::torrentPieceFinished, q, [this](const QString &id, int piece) {
                    if (Download *d = findByTorrent(id)) d->onTorrentPieceFinished(piece);
                });
                QObject::connect(te, &TorrentEvents::torrentPieceFailed, q, [this](const QString &id, int piece) {
                    if (Download *d = findByTorrent(id)) d->onTorrentPieceFailed(piece);
                });
                QObject::connect(te, &TorrentEvents::torrentFinished, q, [this](const QString &id) {
                    if (Download *d = findByTorrent(id)) d->onTorrentFinished();
                });
                QObject::connect(te, &TorrentEvents::torrentError, q, [this](const QString &id, const QString &text) {
                    if (Download *d = findByTorrent(id)) d->onTorrentError(text);
                    for (Download *d : std::as_const(list)) d->onUrlTorrentFailed(id, text);   // id = url for fetch errors
                });
                QObject::connect(te, &TorrentEvents::torrentStorageMoved, q,   // [engine-3]
                                 [this](const QString &id, const QString &path, bool ok, const QString &err) {
                    if (Download *d = findByTorrent(id)) d->onTorrentStorageMoved(path, ok, err);
                });
                QObject::connect(te, &TorrentEvents::torrentRemoved, q, [this](const QString &id) {
                    // [ui-j] a torrent we removed with its download: its files may go now; a download
                    // that took the same torrent again meanwhile adds it anew
                    if (removingTorrents.remove(id)) {
                        flushPendingDelete(QString(), id);
                        if (Download *d = findByTorrent(id)) d->torrentReAdd();
                        return;
                    }
                    if (Download *d = findByTorrent(id); d && !d->isFinished()) d->onTorrentRemoved();
                });
            }
            // BitTorrentEvents extras (declared in the bt module; connect by name)
            if (ev && ev->metaObject()->indexOfSignal("urlTorrentAdded(QString,QString)") >= 0) {
                q->connect(ev, SIGNAL(urlTorrentAdded(QString,QString)), q, SLOT(onUrlTorrentAdded(QString,QString)));
                q->connect(ev, SIGNAL(urlTorrentFailed(QString,QString)), q, SLOT(onUrlTorrentFailed(QString,QString)));
            }
            if (ev && ev->metaObject()->indexOfSignal("seedReady(QString,QString)") >= 0)
                q->connect(ev, SIGNAL(seedReady(QString,QString)), q, SLOT(onSeedReady(QString,QString)));
        }
        if (ctx.library) {
            QObject::connect(ctx.library, &Library::fileHashed, q, [this](const SharedFile &f) {
                const QString p = QFileInfo(f.path).absoluteFilePath();
                for (Download *d : std::as_const(list)) if (d->awaitsHashOf(p)) d->onFileHashed(f);
                if (pendingSeeds.contains(p)) linkSeed(p);
            });
        }
    }
};

// ============================================================================

DownloadEngine::DownloadEngine(const EngineContext &ctx, QObject *parent)
    : TransferServiceBase(ctx.uploads, parent), d(std::make_unique<Private>())
{
    d->q = this;
    d->ctx = ctx;
    if (ctx.db) {
        d->daoPtr = std::make_unique<DownloadsDao>(ctx.db);
        d->linksDao = std::make_unique<HashLinksDao>(ctx.db);
    }
    d->threadPool.setMaxThreadCount(4);
    d->threadPool.setExpiryTimeout(30000);
    d->tickTimer.setInterval(250);
    connect(&d->tickTimer, &QTimer::timeout, this, [this] { d->tick(); });
    // a changed "maximum downloads at once" applies now, not at the next finished download
    if (ctx.settings)
        connect(ctx.settings, &Settings::changed, this, [this](const QString &key, const QVariant &) {
            if (key == keys::transfers::MaxDownloads) d->requestRebalance();
        });
}

DownloadEngine::~DownloadEngine()
{
    d->stopped = true;
    d->tickTimer.stop();
    if (d->ctx.library) {
        d->ctx.library->setPartialProvider({}, {});
        d->ctx.library->setPartialDetails({});
    }
    // kill-style teardown when shutdown() was not called: no persisting here (state written
    // incrementally is what survives, exactly as after a crash)
    for (Download *x : std::as_const(d->list)) delete x;
    d->list.clear();
    // removed downloads still waiting for deleteLater: destroy them while the pool exists
    qDeleteAll(findChildren<Download *>(Qt::FindDirectChildrenOnly));
    d->threadPool.waitForDone(2000);
}

void DownloadEngine::setTuning(const engine::Tuning &t) { d->tune = t; }
engine::Tuning DownloadEngine::tuning() const { return d->tune; }
QString DownloadEngine::incompleteDir() const { return d->incompleteDir(); }

void DownloadEngine::start()
{
    if (d->started) return;
    d->started = true;
    QDir().mkpath(d->incompleteDir());
    // [dl-1] preview links / copies of the last session (one still open in a player stays)
    QDir(engine::Download::previewDirOf(d->incompleteDir())).removeRecursively();
    d->connectPlugins();
    d->installPartialProvider();
    d->loadKept();   // [ui-j]
    if (d->ctx.search) {   // [engine-4]
        connect(d->ctx.search, &SearchService::hitsChanged, this, [this](quint32 id, const QList<SearchHit> &h) { d->onSearchHits(id, h); });
        connect(d->ctx.search, &SearchService::searchFinished, this, [this](quint32 id) { d->onSearchFinished(id); });
    }
    // [safe-1] the running mark: still there = the last session ended without shutdown()
    const QString mark = runningMarkPath(d->ctx.profileDir);
    if (!mark.isEmpty()) {
        d->uncleanStart = QFileInfo::exists(mark);
        QFile f(mark);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(QStringLiteral("OctoMX is running (pid %1) since %2\n").arg(QCoreApplication::applicationPid())
                        .arg(QDateTime::currentDateTime().toString(Qt::ISODate)).toUtf8());
            durable::syncFile(f);
        }
        if (d->uncleanStart) qCWarning(lcEngine) << "the last session was not closed properly: downloads with data are checked first";
    }
    if (d->daoPtr) {
        const QList<DownloadRecord> recs = d->daoPtr->all();
        for (DownloadRecord r : recs) {
            if (r.id.isEmpty()) continue;
            Download *x = d->adopt(r);
            x->restoreState(r.engineState);
            x->resolveInterruptedMove();   // [safe-1]
            if (d->uncleanStart && !x->isFinished()) x->setCheckFirst(true);   // one with nothing to check is cleared at once
            QList<SearchSource> srcs;
            for (const SourceRecord &s : d->daoPtr->sources(r.id)) srcs << s.source;
            x->addSources(srcs, nullptr, true);
        }
    }
    // [safe-1] markers of moves to the target folder whose download is gone (finished ones are
    // cleared by resolveInterruptedMove)
    const QFileInfoList markers = QDir(d->incompleteDir()).entryInfoList({QStringLiteral("*.part.moving")}, QDir::Files);
    for (const QFileInfo &fi : markers) {
        const QString mid = fi.fileName().chopped(int(qstrlen(".part.moving")));
        if (!d->byId.contains(mid)) QFile::remove(fi.absoluteFilePath());
    }
    qCInfo(lcEngine) << "engine: restored" << d->list.size() << "downloads";
    // [ver-1] a restart that was waiting for the old files of its torrent: done now
    for (Download *x : std::as_const(d->list)) {
        if (!x->restartPending()) continue;
        QStringList prune;
        const DownloadDiskInfo di = x->diskInfo(&prune);
        const QString tid = x->torrentId();
        Private::PendingDelete p{x->id(), x->record().name, tid, di.paths, prune, !di.trash, true};
        if (d->ctx.torrents && !tid.isEmpty() && d->ctx.torrents->torrents().contains(tid)) {
            d->removingTorrents.insert(tid);
            d->ctx.torrents->removeTorrent(tid, false);
            d->pendingDeletes << p;
            QTimer::singleShot(10000, this, [this, id = x->id()] { d->flushPendingDelete(id, QString()); });
        } else {
            QTimer::singleShot(0, this, [this, p] { d->runDelete(p); });
        }
    }
    // [safe-1] torrents to be checked are paused in the backend until their check (nothing of them is
    // sent to peers before it); the check runs a paused torrent and pauses it again
    if (TorrentBackend *be = d->ctx.torrents)
        for (Download *x : std::as_const(d->list))
            if (x->checkFirst() && !x->torrentId().isEmpty() && be->torrents().contains(x->torrentId())) be->pauseTorrent(x->torrentId());
    for (Download *x : std::as_const(d->list)) emit transferAdded(x->info());
    d->tickTimer.start();
    d->rebalance();
    d->startChecks();
    d->updatePartials();
}

void DownloadEngine::beginShutdown()
{
    if (d->stopped || d->closing) return;
    d->closing = true;
    d->tickTimer.stop();
    for (Download *x : std::as_const(d->list)) x->beginShutdown();
}

qint64 DownloadEngine::unsavedBytes() const
{
    qint64 n = 0;
    for (Download *x : d->list) n += x->unsavedBytes();
    return n;
}

void DownloadEngine::closeNow()
{
    d->closeNow = true;
}

bool DownloadEngine::uncleanStart() const { return d->uncleanStart; }

QString DownloadEngine::runningMarkPath(const QString &profileDir)
{
    return profileDir.isEmpty() ? QString() : QDir(profileDir).filePath(QStringLiteral("octomx.running"));
}

void DownloadEngine::shutdown()
{
    if (d->stopped) return;
    QElapsedTimer t;
    t.start();
    beginShutdown();
    // [safe-1] every part file written and synced, as long as it takes (a hung disk: the last resort
    // cap), then the records, which claim only what is synced
    bool saved = true;
    for (Download *x : std::as_const(d->list))
        saved &= x->shutdown(d->closeNow ? 0 : int(qMax<qint64>(0, kShutdownCapMs - t.elapsed())));
    d->stopped = true;
    if (d->ctx.library) {
        d->ctx.library->setPartialProvider({}, {});
        d->ctx.library->setPartialDetails({});
    }
    if (d->ctx.db) d->ctx.db->flush();
    // the records are stored: this session ended properly (torrents: their resume data is saved and
    // synced by the plugin's stop(), which runs after this)
    if (const QString mark = runningMarkPath(d->ctx.profileDir); !mark.isEmpty()) QFile::remove(mark);
    qCInfo(lcEngine) << "engine shut down in" << t.elapsed() << "ms:"
                     << (saved ? "every download is saved" : "not everything was saved in time, it is downloaded again");
}

int DownloadEngine::knownSourceCount() const
{
    int n = 0;
    for (Download *x : d->list) if (!x->isFinished()) n += x->knownSources();
    return n;
}

int DownloadEngine::connectedSourceCount() const
{
    int n = 0;
    for (Download *x : d->list) n += x->connectedSources();
    return n;
}

QList<TransferInfo> DownloadEngine::downloadsBrief() const
{
    QList<TransferInfo> out;
    out.reserve(d->list.size());
    for (Download *x : d->list) out << x->briefInfo();
    return out;
}

void DownloadEngine::setAutoFindSources(const QString &id, int minutes)
{
    if (Download *x = d->byId.value(id)) {
        x->setAutoFindSources(minutes);
        d->changed(x, true);
    }
}

// [dl-1] download order and preview
void DownloadEngine::setSequentialDownload(const QString &id, bool on)
{
    if (Download *x = d->byId.value(id)) {
        if (x->isFinished()) return;
        x->setOrder(on, x->firstLast());
        x->persist(false);
    }
}

void DownloadEngine::setFirstLastPiecesFirst(const QString &id, bool on)
{
    if (Download *x = d->byId.value(id)) {
        if (x->isFinished()) return;
        x->setOrder(x->sequential(), on);
        x->persist(false);
    }
}

PreviewInfo DownloadEngine::previewInfo(const QString &id) const
{
    if (Download *x = d->byId.value(id)) return x->previewInfo();
    return {};
}

QString DownloadEngine::preparePreview(const QString &id, int fileIndex, QString *error)
{
    Download *x = d->byId.value(id);
    if (!x) {
        if (error) *error = QStringLiteral("The download is gone.");
        return QString();
    }
    return x->preparePreview(fileIndex, error);
}

// [ver-1] Verify File / Restart Download
bool DownloadEngine::canVerifyDownload(const QString &id, QString *why) const
{
    Download *x = d->byId.value(id);
    if (!x) {
        if (why) *why = QStringLiteral("The download is gone.");
        return false;
    }
    return x->canVerify(why);
}

bool DownloadEngine::verifyDownload(const QString &id)
{
    Download *x = d->byId.value(id);
    if (!x || !x->startVerify()) return false;
    d->emitNow(x, true);
    d->requestRebalance();
    return true;
}

bool DownloadEngine::canRestartDownload(const QString &id, QString *why) const
{
    Download *x = d->byId.value(id);
    if (!x) {
        if (why) *why = QStringLiteral("The download is gone.");
        return false;
    }
    return x->canRestart(why);
}

bool DownloadEngine::restartDownload(const QString &id)
{
    Download *x = d->byId.value(id);
    if (!x || !x->canRestart(nullptr)) return false;
    d->restart(x);
    return true;
}

void DownloadEngine::setAutoEnterQueue(const QString &id, int n)
{
    if (Download *x = d->byId.value(id)) {
        x->setAutoEnterQueue(n);
        d->changed(x, true);
    }
}

QString DownloadEngine::download(const DownloadRequest &reqIn)
{
    DownloadRequest req = reqIn;
    QString err;
    if (!resolveDownloadRequest(req, &err)) {
        qCWarning(lcEngine) << "cannot download:" << err;
        // [engine-4] never silently: the UI learns why nothing appeared
        QString what = d->requestWhat;
        if (what.isEmpty()) what = req.uri.isEmpty() ? req.name : req.uri;
        QString why = err;
        if (err == QLatin1String("Nothing to download"))
            why = QStringLiteral("Nothing to download: no link, hash or reachable source is known for \"%1\"").arg(req.name.isEmpty() ? what : req.name);
        emit downloadNotStarted(what, why);
        return QString();
    }
    // join a download of the same file
    for (Download *x : std::as_const(d->list)) {
        const bool sameHash = !req.hashes.isEmpty() && x->hashes().matches(req.hashes);
        const bool sameUri = req.hashes.isEmpty() && !req.uri.isEmpty() && x->originalUri() == req.uri.trimmed();
        if (!sameHash && !sameUri) continue;
        if (x->isFinished()) {
            qCInfo(lcEngine) << "download of" << req.name << "already complete:" << x->id();
            return x->id();
        }
        qCInfo(lcEngine) << "joining" << req.name << "into running download" << x->id();
        x->mergeRequest(req);
        d->requestRebalance();
        return x->id();
    }
    // [ui-j] removed earlier with "keep the file": it continues from its data
    if (const int k = d->keptForRequest(req); k >= 0) return d->resurrect(k, &req)->id();
    // already in the library?
    if (d->ctx.library && !req.hashes.isEmpty()) {
        SharedFile f;
        if (d->ctx.library->findByHash(req.hashes, &f) && QFileInfo::exists(f.path) && !f.path.endsWith(QLatin1String(".part"))) {
            DownloadRecord r;
            r.id = newDownloadId();
            r.name = f.name.isEmpty() ? req.name : f.name;
            r.size = f.size;
            r.hashes = req.hashes;
            r.hashes.mergeFrom(f.hashes);
            r.state = int(TransferState::Completed);
            r.statusText = QStringLiteral("Complete (already in your library)");
            r.savePath = f.path;
            r.uri = req.uri;
            r.done = f.size;
            r.added = r.completed = QDateTime::currentDateTime();
            r.sortOrder = d->list.isEmpty() ? 0 : d->list.last()->record().sortOrder + 1;
            Download *x = d->adopt(r);
            x->setNotOwned(true);   // [ui-j] removing this row never deletes the library's file
            x->persist(true);
            emit transferAdded(x->info());
            qCInfo(lcEngine) << req.name << "is already in the library:" << f.path;
            return r.id;
        }
    }
    DownloadRecord r;
    r.id = newDownloadId();
    r.name = req.name;
    r.size = req.size > 0 ? req.size : 0;
    r.hashes = req.hashes;
    r.uri = req.uri.trimmed();
    r.saveDir = req.saveDir;
    r.wantedFiles = req.wantedFiles;
    r.added = QDateTime::currentDateTime();
    r.sortOrder = d->list.isEmpty() ? 0 : d->list.last()->record().sortOrder + 1;
    const bool stream = d->nextStream;   // [str-1]
    d->nextStream = false;
    const QString streamDir = stream ? QDir(d->nextStreamDir).filePath(r.id) : QString();
    if (stream) r.saveDir = streamDir;
    const bool paused = !stream && (req.startPaused || (d->ctx.settings && d->ctx.settings->b(keys::transfers::StartPaused)));
    r.state = int(paused ? TransferState::Paused : TransferState::Queued);
    r.statusText = transferStateText(TransferState(r.state));
    Download *x = d->adopt(r);
    if (stream) {
        QDir().mkpath(streamDir);
        x->setStream(true, true, d->nextStreamFile, QDir(streamDir).filePath(r.id + QStringLiteral(".part")));
    } else if (d->ctx.settings)   // [dl-1] the defaults of the order flags (Settings, File Transfers)
        x->setOrder(d->ctx.settings->b(keys::transfers::SequentialDefault), d->ctx.settings->b(keys::transfers::FirstLastDefault), false);
    x->persist(true);
    x->addSources(req.sources, nullptr, true);
    qCInfo(lcEngine) << "new download" << r.id << r.name << r.size << "bytes," << req.sources.size() << "sources";
    emit transferAdded(x->info());
    d->rebalance();
    return r.id;
}

QString DownloadEngine::downloadHit(const SearchHit &hit, const QString &saveDir)
{
    d->requestWhat = hit.key.isEmpty() ? hit.fileName : hit.key;
    const QString id = downloadHitRequest(hit, saveDir);
    d->requestWhat.clear();
    // [engine-4] the hit as the search knows it NOW (the UI's copy may be older or one user row of
    // a group) carries every source of every network; the download keeps following it
    if (!id.isEmpty() && d->ctx.search && hit.queryId) {
        Download *x = d->byId.value(id);
        if (x && !x->isFinished()) {
            for (const SearchHit &h : d->ctx.search->hits(hit.queryId))
                if (h.key == hit.key) { x->addHitSources(h); break; }
            bool known = false;
            for (const Private::HitFollow &f : std::as_const(d->follows))
                if (f.query == hit.queryId && f.key == hit.key && f.downloadId == id) known = true;
            if (!known) d->follows << Private::HitFollow{hit.queryId, hit.key, id};
        }
    }
    return id;
}

QString DownloadEngine::downloadHitRequest(const SearchHit &hit, const QString &saveDir)
{
    DownloadRequest r = requestFromHit(hit, saveDir);
    // the hit's first URI may be a network-internal source descriptor ("winmx:<node>...", "fake://..."):
    // only real links are a download URI; the sources keep their descriptors
    const QString u = r.uri.trimmed().toLower();
    const bool link = u.startsWith(QLatin1String("magnet:")) || u.startsWith(QLatin1String("ed2k:"))
                      || u.startsWith(QLatin1String("http://")) || u.startsWith(QLatin1String("https://"))
                      || u.startsWith(QLatin1String("ftp://"));
    if (!link) {
        r.uri.clear();
        for (const SearchSource &s : hit.sources) {
            const QString su = s.uri.trimmed().toLower();
            if (s.extra.value(QStringLiteral("candidate")).toBool()) continue;
            if (su.startsWith(QLatin1String("magnet:")) || su.startsWith(QLatin1String("ed2k:"))) { r.uri = s.uri; break; }
        }
    }
    return download(r);
}

QString DownloadEngine::downloadUri(const QString &uri, const QString &saveDir)
{
    DownloadRequest req;
    req.uri = uri;
    req.saveDir = saveDir;
    return download(req);
}

QList<TransferInfo> DownloadEngine::downloads() const
{
    QList<TransferInfo> out;
    for (Download *x : d->list) out << x->info();
    return out;
}

TransferInfo DownloadEngine::transfer(const QString &id) const
{
    if (Download *x = d->byId.value(id)) return x->info();
    TransferInfo t;
    uploadTransfer(id, &t);
    return t;
}

void DownloadEngine::pause(const QString &id)
{
    if (Download *x = d->byId.value(id)) {
        x->setUserPaused(true);
        d->emitNow(x);
        d->requestRebalance();
    }
}

void DownloadEngine::resume(const QString &id)
{
    if (Download *x = d->byId.value(id)) {
        x->setUserPaused(false);
        d->rebalance();
        d->emitNow(x);
    }
}

void DownloadEngine::pauseAll()
{
    for (Download *x : std::as_const(d->list)) if (!x->isFinished()) { x->setUserPaused(true); d->changed(x, true); }
    d->requestRebalance();
}

void DownloadEngine::resumeAll()
{
    for (Download *x : std::as_const(d->list)) if (!x->isFinished()) { x->setUserPaused(false); d->changed(x, true); }
    d->rebalance();
}

void DownloadEngine::cancel(const QString &id, bool deleteData)
{
    // the earlier meaning: deleteData drops the partial data of an unfinished download; a finished
    // file always stayed. [ui-j] the data goes to the trash where there is one
    if (Download *x = d->byId.value(id))
        d->removeDownload(x, deleteData && !x->isFinished() ? FileRemoval::TrashOrDelete : FileRemoval::Keep);
}

// [ui-j]
DownloadDiskInfo DownloadEngine::diskInfo(const QString &id) const
{
    if (Download *x = d->byId.value(id)) return x->diskInfo();
    return {};
}

void DownloadEngine::removeDownload(const QString &id, FileRemoval mode)
{
    if (Download *x = d->byId.value(id)) d->removeDownload(x, mode);
}

bool DownloadEngine::matchesKept(const SearchHit &h) const
{
    DownloadRequest r;
    r.name = h.fileName;
    r.size = h.size;
    r.hashes = h.hashes;
    return d->keptForRequest(r) >= 0;
}

QList<TransferInfo> DownloadEngine::keptDownloads() const
{
    QList<TransferInfo> out;
    for (const Private::KeptDownload &k : std::as_const(d->kept)) {
        TransferInfo t;
        t.id = k.rec.id;
        t.name = k.rec.name;
        t.size = k.rec.size;
        t.done = k.held;
        t.hashes = k.rec.hashes;
        t.isBundle = k.rec.isBundle;
        t.savePath = k.paths.value(0);
        t.state = TransferState::Paused;
        out << t;
    }
    return out;
}

void DownloadEngine::retry(const QString &id)
{
    if (Download *x = d->byId.value(id)) {
        x->retry();
        d->rebalance();
        d->emitNow(x);
    }
}

void DownloadEngine::setPriority(const QString &id, int priority)
{
    if (Download *x = d->byId.value(id)) {
        x->setPriority(priority);
        d->rebalance();
    }
}

void DownloadEngine::clearCompleted()
{
    const QList<Download *> l = d->list;
    for (Download *x : l)
        if (finishedState(TransferState(x->record().state))) d->removeDownload(x, FileRemoval::Keep);
    if (m_uploads) m_uploads->clearFinished();
}

void DownloadEngine::setWantedFiles(const QString &id, const QList<int> &indexes)
{
    if (Download *x = d->byId.value(id)) x->setWantedFiles(indexes);
}

void DownloadEngine::openFile(const QString &id)
{
    if (Download *x = d->byId.value(id)) {
        const QString p = x->info().savePath;
        if (QFileInfo::exists(p)) launchFile(d->ctx.settings, p);
    }
}

void DownloadEngine::openFolder(const QString &id)
{
    if (Download *x = d->byId.value(id)) {
        const QString p = x->info().savePath;
        openContainingFolder(QFileInfo::exists(p) ? p : QFileInfo(p).absolutePath());
    }
}

QString DownloadEngine::incomingDir() const { return d->incomingDir(); }

QVariantMap DownloadEngine::debugInfo(const QString &id) const
{
    if (Download *x = d->byId.value(id)) return x->debugInfo();
    return {};
}

// [diag-1]
QList<diag::DownloadFacts> DownloadEngine::diagFacts() const
{
    QList<diag::DownloadFacts> out;
    for (Download *x : std::as_const(d->list))
        if (!x->isFinished()) out << x->diagFacts();
    return out;
}

void DownloadEngine::setDiagnosis(const QString &id, const QString &text, int level)
{
    Download *x = d->byId.value(id);
    if (x && x->setDiagnosis(text, level)) d->emitNow(x);
}

int DownloadEngine::activeDownloadCount() const
{
    int n = 0;
    for (Download *x : std::as_const(d->list))
        if (x->isActive() && !x->isFinished()) ++n;
    return n;
}

void DownloadEngine::onUrlTorrentAdded(const QString &url, const QString &id)
{
    for (Download *x : std::as_const(d->list)) if (x->waitsForUrl(url)) x->onUrlTorrentAdded(url, id);
}

void DownloadEngine::onSeedReady(const QString &path, const QString &id)
{
    const QString p = QFileInfo(path).absoluteFilePath();
    d->pendingSeeds.insert(p, id);
    d->linkSeed(p);
    if (d->pendingSeeds.contains(p) && d->ctx.library) d->ctx.library->requestHash(p);   // not hashed yet
}

void DownloadEngine::onUrlTorrentFailed(const QString &url, const QString &error)
{
    for (Download *x : std::as_const(d->list)) if (x->waitsForUrl(url)) x->onUrlTorrentFailed(url, error);
}

// ============================================================================ [glue-2]

void DownloadEngine::findMoreSources(const QString &id)
{
    if (Download *x = d->byId.value(id)) {
        x->findMoreSources();
        d->emitNow(x);
    }
}

void DownloadEngine::enterQueue(const QString &id)
{
    if (Download *x = d->byId.value(id)) {
        x->enterQueue();
        d->rebalance();
        d->emitNow(x);
    }
}

void DownloadEngine::cancelSource(const QString &id, const QString &sourceId)
{
    if (Download *x = d->byId.value(id); x && x->removeSource(sourceId)) d->emitNow(x);
}

void DownloadEngine::setSourcePriority(const QString &id, const QString &sourceId, int priority)
{
    if (Download *x = d->byId.value(id); x && x->setSourcePriority(sourceId, priority)) d->emitNow(x);
}

bool DownloadEngine::setDownloadTarget(const QString &id, const QString &dir, const QString &name)
{
    Download *x = d->byId.value(id);
    if (!x || !x->setTarget(dir, name)) return false;
    d->emitNow(x);
    return true;
}

QString DownloadEngine::resumeDownload(const DownloadRequest &reqIn, const QString &existingFile)
{
    DownloadRequest req = reqIn;
    QString err;
    if (!resolveDownloadRequest(req, &err)) {
        qCWarning(lcEngine) << "resume: unusable request:" << err;
        emit downloadNotStarted(req.name.isEmpty() ? existingFile : req.name, err);
        return QString();
    }
    const QFileInfo fi(existingFile);
    if (!fi.isFile() || !fi.isReadable() || !fi.isWritable()) {
        qCWarning(lcEngine) << "resume: cannot use" << existingFile;
        return QString();
    }
    const QString path = fi.absoluteFilePath();
    // [ui-j] the kept data of a download removed from the list: it comes back with all it knew
    if (const int k = d->keptForPath(path); k >= 0) {
        if (!req.hashes.isEmpty() && d->kept.at(k).rec.hashes.conflicts(req.hashes)) {
            emit downloadNotStarted(req.name.isEmpty() ? existingFile : req.name,
                                    QStringLiteral("The link is of another file than this partial download"));
            return QString();
        }
        return d->resurrect(k, &req)->id();
    }
    for (Download *x : std::as_const(d->list)) {
        const bool same = (!req.hashes.isEmpty() && x->hashes().matches(req.hashes))
                          || QFileInfo(x->info().savePath).absoluteFilePath() == path;
        if (same && !x->isFinished()) {
            qCWarning(lcEngine) << "resume: already downloading as" << x->id();
            return QString();
        }
    }
    // a torrent identity: libtorrent checks existing data itself when the file sits at its place
    if (!req.hashes.btih.isEmpty()) {
        if (req.name.isEmpty()) return QString();
        const QString dir = req.saveDir.isEmpty() ? d->incomingDir() : req.saveDir;
        const QString target = QDir(dir).filePath(req.name);
        if (QFileInfo(target).absoluteFilePath() != path) {
            if (QFileInfo::exists(target) || !QDir().mkpath(dir) || !QFile::rename(path, target)) {
                qCWarning(lcEngine) << "resume: cannot place" << path << "at" << target;
                return QString();
            }
        }
        return download(req);
    }
    if (req.size <= 0 || fi.size() > req.size) {
        qCWarning(lcEngine) << "resume:" << path << "is" << fi.size() << "bytes, the download" << req.size;
        return QString();
    }
    // data = the file up to its last non-zero byte (a preallocated tail is not data); only the
    // last 16 MiB are scanned so this stays cheap on the GUI thread
    qint64 held = fi.size();
    {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) {
            const qint64 block = 1 << 20;
            qint64 scanned = 0;
            while (held > 0 && scanned < (16LL << 20)) {
                const qint64 from = qMax<qint64>(0, held - block);
                f.seek(from);
                const QByteArray b = f.read(held - from);
                int last = int(b.size()) - 1;
                while (last >= 0 && b.at(last) == 0) --last;
                if (last >= 0) { held = from + last + 1; break; }
                held = from;
                scanned += b.size();
            }
        }
    }
    DownloadRecord r;
    r.id = newDownloadId();
    r.name = req.name.isEmpty() ? fi.fileName() : req.name;
    r.size = req.size;
    r.hashes = req.hashes;
    r.uri = req.uri.trimmed();
    r.saveDir = req.saveDir;
    r.added = QDateTime::currentDateTime();
    r.sortOrder = d->list.isEmpty() ? 0 : d->list.last()->record().sortOrder + 1;
    r.state = int(req.startPaused ? TransferState::Paused : TransferState::Queued);
    r.statusText = transferStateText(TransferState(r.state));
    r.done = held;
    r.engineState = engine::Download::stateForExistingFile(path, held);
    Download *x = d->adopt(r);
    x->restoreState(r.engineState);
    if (d->ctx.settings)   // [dl-1]
        x->setOrder(d->ctx.settings->b(keys::transfers::SequentialDefault), d->ctx.settings->b(keys::transfers::FirstLastDefault), false);
    x->persist(true);
    x->addSources(req.sources, nullptr, true);
    qCInfo(lcEngine) << "resumed download" << r.id << r.name << "from" << path << "with" << held << "of" << r.size << "bytes";
    emit transferAdded(x->info());
    d->rebalance();
    return r.id;
}

QList<PartialFile> DownloadEngine::partialFiles() const
{
    QList<PartialFile> out;
    QHash<QString, QString> owner;   // absolute path → download id
    for (Download *x : std::as_const(d->list)) {
        if (x->isFinished()) continue;
        const QString p = x->info().savePath;
        if (!p.isEmpty()) owner.insert(QFileInfo(p).absoluteFilePath(), x->id());
    }
    const QDir dir(d->incompleteDir());
    for (const QFileInfo &fi : dir.entryInfoList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Time)) {
        PartialFile pf;
        pf.path = fi.absoluteFilePath();
        pf.name = fi.fileName();
        pf.size = fi.size();
        pf.modified = fi.lastModified();
        pf.downloadId = owner.value(pf.path);
        out << pf;
    }
    // [ui-j] downloads removed with "keep the file": their data (a part file above, or the files of a
    // torrent in its save folder) with the name and the link that continue it
    for (const Private::KeptDownload &k : std::as_const(d->kept)) {
        const QString first = k.paths.value(0);
        if (!pathOnDisk(first)) continue;
        PartialFile *pf = nullptr;
        for (PartialFile &x : out)
            if (samePath(x.path) == samePath(first)) pf = &x;
        if (!pf) {
            PartialFile x;
            x.path = QFileInfo(first).absoluteFilePath();
            const QFileInfo fi(first);
            x.size = fi.isDir() ? trash::sizeOnDisk(first) : fi.size();
            x.modified = fi.lastModified();
            out << x;
            pf = &out.last();
        }
        pf->kept = true;
        pf->name = k.rec.name;
        pf->held = k.held;
        pf->total = k.rec.size;
        const QString uri = k.rec.uri.trimmed();
        if (uri.startsWith(QLatin1String("magnet:"), Qt::CaseInsensitive) || uri.startsWith(QLatin1String("ed2k:"), Qt::CaseInsensitive))
            pf->link = uri;
        else if (!k.rec.hashes.btih.isEmpty() || k.rec.hashes.ed2k.isEmpty())
            pf->link = k.rec.hashes.isEmpty() ? QString() : k.rec.hashes.toMagnet(k.rec.name, k.rec.isBundle ? -1 : k.rec.size);
        else
            pf->link = hash::formatEd2kLink(k.rec.name, k.rec.size, k.rec.hashes);
    }
    return out;
}

// ============================================================================ [str-1] streaming

engine::Download *DownloadEngine::downloadObject(const QString &id) const { return d->byId.value(id); }

void DownloadEngine::setNextStream(int fileIndex, const QString &cacheDir)
{
    d->nextStream = true;
    d->nextStreamFile = fileIndex;
    d->nextStreamDir = cacheDir;
}

void DownloadEngine::clearNextStream() { d->nextStream = false; }

QString DownloadEngine::streamCacheRoot() const { return QDir(d->incompleteDir()).filePath(QStringLiteral("Streams")); }

void DownloadEngine::dropStream(const QString &id)
{
    Download *x = d->byId.value(id);
    if (!x) return;
    x->stopForRemoval(false);
    d->list.removeAll(x);
    d->byId.remove(id);
    d->dirty.remove(id);
    d->lastEmit.remove(id);
    d->lastSig.remove(id);
    if (d->daoPtr) d->daoPtr->remove(id);
    for (int i = int(d->follows.size()) - 1; i >= 0; --i)
        if (d->follows.at(i).downloadId == id) d->follows.removeAt(i);
    x->deleteLater();
    qCInfo(lcEngine) << "stream" << id << "removed from the list";
    emit transferRemoved(id, false);
    d->updatePartials();
    d->requestRebalance();
}

} // namespace mx
