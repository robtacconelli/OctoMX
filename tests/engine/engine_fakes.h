// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX engine tests: deterministic fakes: a configurable RangeSource / plugin and an
// in-memory TorrentBackend with real SHA-1 piece hashes (header-only; listed in the test target so
// AUTOMOC sees it).
#pragma once

#include <QtCore/QBitArray>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QHash>
#include <QtCore/QPointer>
#include <QtCore/QRandomGenerator>
#include <QtCore/QSet>
#include <QtCore/QTimer>

#include <functional>
#include <memory>

#include "fake_plugin.h"
#include "mx/plugin.h"
#include "mxhash/bt_pieces.h"
#include "mxhash/encoding.h"

namespace mx::etest {

inline QByteArray randomBytes(qint64 n, quint32 seed)
{
    QByteArray b(int(n), Qt::Uninitialized);
    QRandomGenerator g(seed);
    for (qint64 i = 0; i < n; i += 4) {
        const quint32 v = g.generate();
        for (int k = 0; k < 4 && i + k < n; ++k) b[int(i + k)] = char((v >> (8 * k)) & 0xff);
    }
    return b;
}

struct SourceConfig {
    QByteArray data;                         // the whole file the remote has
    QList<QPair<qint64, qint64>> avail;      // empty = everything
    int    bytesPerSec = 0;                  // 0 = as fast as the event loop
    int    chunk = 16 * 1024;
    int    maxPending = 2;
    qint64 pref = 256 * 1024;
    int    openDelayMs = 0;
    bool   failOpen = false;                 // → Failed after open
    QList<QPair<qint64, qint64>> corrupt;    // file ranges delivered with flipped bytes
    bool   corruptAll = false;
    bool   sequentialOnly = false;           // extra supportsRanges = false; only request(0, ...) works
    bool   unknownLength = false;            // available() empty, no size; serves to EOF
    QVariantMap extra;                       // merged into descriptor().extra once Ready
    int    queuedMs = 0;                     // state Queued for this long first (rank 7)
    bool   misbehave = false;                // first chunk of every request at a wrong offset
    bool   stallAfterBytes = false;          // stop sending after `stallAt` bytes (never finishes)
    qint64 stallAt = 0;
    bool   shortRequests = false;            // finish every request after half of it
    QList<QByteArray> hashSet;               // [engine-3] sent once via hashSetReceived when first Ready
    QString hashSetKind = QStringLiteral("ed2k-parts");
    qint64 hashSetUnit = 9728000;
};

struct SourceStats {                         // outlives the source (the engine deletes closed ones)
    QString uri;
    SourceConfig cfg;
    QList<QPair<qint64, qint64>> requests;   // as received
    int cancelCount = 0;
    qint64 served = 0;
};

class TestSource : public RangeSource {
    Q_OBJECT
public:
    TestSource(const SearchSource &d, const SourceConfig &c, QObject *parent = nullptr)
        : RangeSource(parent), cfg(c), stats(std::make_shared<SourceStats>()), m_desc(d)
    {
        stats->uri = d.uri;
        stats->cfg = c;
        m_tick.setInterval(20);
        connect(&m_tick, &QTimer::timeout, this, &TestSource::pump);
    }
    ~TestSource() override { alive.remove(this); }
    static inline QSet<TestSource *> alive;

    SourceConfig cfg;
    std::shared_ptr<SourceStats> stats;

    SearchSource descriptor() const override
    {
        SearchSource s = m_desc;
        if (m_state == Ready || m_state == Busy)
            for (auto it = cfg.extra.constBegin(); it != cfg.extra.constEnd(); ++it) s.extra.insert(it.key(), it.value());
        if (cfg.sequentialOnly) s.extra.insert(QStringLiteral("supportsRanges"), false);
        if (m_state == Queued) s.extra.insert(QStringLiteral("queueRank"), 7);
        return s;
    }
    State state() const override { return m_state; }
    QString stateText() const override
    {
        switch (m_state) {
        case Queued: return QStringLiteral("Remotely Queued (7)");
        case Connecting: return QStringLiteral("Connecting...");
        case Ready: return QStringLiteral("Ready");
        case Busy: return QStringLiteral("Transferring");
        case Failed: return QStringLiteral("Connection Refused");
        default: return QStringLiteral("Idle");
        }
    }
    QString clientName() const override { return QStringLiteral("TestClient"); }
    QList<QPair<qint64, qint64>> available() const override
    {
        if (cfg.unknownLength) return {};
        if (!cfg.avail.isEmpty()) return cfg.avail;
        return {{0, cfg.data.size()}};
    }
    qint64 preferredRequestSize() const override { return cfg.pref; }
    int maxPendingRequests() const override { return cfg.maxPending; }
    void open() override
    {
        alive.insert(this);
        setState(Connecting);
        QTimer::singleShot(cfg.openDelayMs, this, [this] {
            if (m_state != Connecting) return;
            if (cfg.failOpen) { setState(Failed); return; }
            if (cfg.queuedMs > 0) {
                setState(Queued);
                QTimer::singleShot(cfg.queuedMs, this, [this] { if (m_state == Queued) setState(Ready); });
                return;
            }
            setState(Ready);
        });
    }
    void request(qint64 offset, qint64 length) override
    {
        stats->requests << qMakePair(offset, length);
        Job j;
        j.off = offset;
        j.len = length;
        j.pos = offset;
        if (cfg.sequentialOnly && offset != 0) {
            QTimer::singleShot(0, this, [this, offset, length] { emit requestFinished(offset, length, false, QStringLiteral("no ranges")); });
            return;
        }
        if (offset < 0 || offset >= cfg.data.size()) {
            QTimer::singleShot(0, this, [this, offset, length] { emit requestFinished(offset, length, false, QStringLiteral("range not available")); });
            return;
        }
        m_jobs << j;
        setState(Busy);
        if (!m_tick.isActive()) m_tick.start();
        m_clock.start();
    }
    void cancelRequests() override
    {
        ++stats->cancelCount;
        m_jobs.clear();
        if (m_state == Busy) setState(Ready);
    }
    void close() override
    {
        m_jobs.clear();
        m_tick.stop();
        setState(Closed);
    }

private:
    struct Job { qint64 off = 0, len = 0, pos = 0; bool misbehaved = false; };
    void pump()
    {
        if (m_jobs.isEmpty()) { m_tick.stop(); return; }
        qint64 budget = cfg.bytesPerSec > 0 ? qMax<qint64>(1, qint64(cfg.bytesPerSec) * 20 / 1000) : (qint64(64) << 20);
        while (budget > 0 && !m_jobs.isEmpty()) {
            Job &j = m_jobs.first();
            const qint64 fileEnd = cfg.data.size();
            const qint64 jobEnd = qMin(j.off + j.len, fileEnd);
            const qint64 limitEnd = cfg.shortRequests ? qMin(jobEnd, j.off + qMax<qint64>(1, j.len / 2)) : jobEnd;
            if (cfg.stallAfterBytes && stats->served >= cfg.stallAt) return;
            if (j.pos >= limitEnd) {
                const Job done = m_jobs.takeFirst();
                emit requestFinished(done.off, done.len, true, QString());
                continue;
            }
            const qint64 n = qMin<qint64>(qMin<qint64>(cfg.chunk, budget), limitEnd - j.pos);
            QByteArray part = cfg.data.mid(int(j.pos), int(n));
            for (const auto &c : std::as_const(cfg.corrupt))
                for (qint64 k = qMax(c.first, j.pos); k < qMin(c.second, j.pos + n); ++k) part[int(k - j.pos)] = char(part.at(int(k - j.pos)) ^ 0x5a);
            if (cfg.corruptAll && !part.isEmpty()) part[0] = char(part.at(0) ^ 0x33);
            if (cfg.misbehave && !j.misbehaved) {
                j.misbehaved = true;
                emit data(j.pos + 7777, part);   // wrong offset
                return;
            }
            const qint64 at = j.pos;
            j.pos += n;
            stats->served += n;
            budget -= n;
            QPointer<TestSource> guard(this);
            emit data(at, part);
            if (!guard) return;
        }
        if (m_jobs.isEmpty() && m_state == Busy) setState(Ready);
    }
    void setState(State s)
    {
        if (m_state == s) return;
        m_state = s;
        emit stateChanged();
        if (s == Ready && !m_sentSet && !cfg.hashSet.isEmpty()) {
            m_sentSet = true;
            QTimer::singleShot(0, this, [this] { emit hashSetReceived(cfg.hashSetKind, cfg.hashSetUnit, cfg.hashSet); });
        }
    }
    bool m_sentSet = false;
    SearchSource m_desc;
    State m_state = Idle;
    QList<Job> m_jobs;
    QTimer m_tick;
    QElapsedTimer m_clock;
};

// A plugin whose openSource() builds TestSources from per-URI configurations.
class TestPlugin : public test::FakePlugin {
    Q_OBJECT
public:
    TestPlugin(Net net, int caps, QObject *parent = nullptr) : FakePlugin(net, caps, parent) {}
    QHash<QString, SourceConfig> configs;              // uri → config
    QList<QPointer<TestSource>> sources;
    QList<std::shared_ptr<SourceStats>> stats;         // one per openSource()
    QList<SearchSource> onFind;                        // emitted as sourcesFound on findSources
    QList<QPair<FileHashes, qint64>> findCalls;
    QList<QPair<qint64, QList<QByteArray>>> hashSetOnFind;  // (unit, set) emitted as "ed2k-parts"/"tth-leaves"
    QString hashSetKind = QStringLiteral("ed2k-parts");
    int findDelayMs = 10;
    std::function<bool(const FileHashes &)> findFilter;   // answer only lookups it can use

    SearchSource addSource(const QString &uri, const SourceConfig &c, const QString &user = QString())
    {
        configs.insert(uri, c);
        SearchSource s;
        s.net = net();
        s.uri = uri;
        if (!user.isEmpty()) { s.user.net = net(); s.user.id = user; s.user.name = user; }
        return s;
    }
    SourceStats *source(const QString &uri) const   // latest opening of that uri
    {
        for (int i = stats.size() - 1; i >= 0; --i) if (stats[i]->uri == uri) return stats[i].get();
        return nullptr;
    }
    qint64 served(const QString &uri) const
    {
        qint64 n = 0;
        for (const auto &s : stats) if (s->uri == uri) n += s->served;
        return n;
    }
    void findSources(const QString &downloadId, const FileHashes &h, qint64 size, const QString &name) override
    {
        FakePlugin::findSources(downloadId, h, size, name);
        findCalls << qMakePair(h, size);
        if (findFilter && !findFilter(h)) return;
        QTimer::singleShot(findDelayMs, this, [this, downloadId] {
            for (const auto &hs : std::as_const(hashSetOnFind)) emit hashSetLearned(downloadId, hashSetKind, hs.first, hs.second);
            if (!onFind.isEmpty()) emit sourcesFound(downloadId, onFind);
        });
    }
    RangeSource *openSource(const QString &id, const FileHashes &h, qint64 size, const SearchSource &src) override
    {
        auto it = configs.constFind(src.uri);
        if (it == configs.constEnd()) return FakePlugin::openSource(id, h, size, src);
        auto *s = new TestSource(src, *it);
        sources << s;
        stats << s->stats;
        return s;
    }
    void emitHashLinks(const FileHashes &known, const FileHashes &all, qint64 size) { emit hashLinksFound(known, all, size); }
    void emitHashes(const QString &id, const FileHashes &h) { emit hashesLearned(id, h); }
    void emitHashSet(const QString &id, const QString &kind, qint64 unit, const QList<QByteArray> &set)
    { emit hashSetLearned(id, kind, unit, set); }
    void emitSources(const QString &id, const QList<SearchSource> &s) { emit sourcesFound(id, s); }
};

// ---------------------------------------------------------------- fake torrent backend
struct FakeTorrent {
    QString id, name, savePath;
    QByteArray payload;                 // all files concatenated
    QList<TorrentFile> files;
    int pieceLength = 0;
    bool metadata = false;
    bool paused = true;
    QBitArray have;
    QSet<int> reserved;
    QList<int> wanted;                   // empty = all
    int peerPiecesPerTick = 0;           // simulated BitTorrent peers: pieces/200 ms
    int peers = 0;
    int metaPeers = 0;                   // [eng-1] peers connected while the metadata is missing
    int swarmTotal = -1;                 // [eng-1] TorrentSnapshot::peersTotal
    qint64 uploaded = 0;
    QList<TransferSource> peerRows;      // [engine-4] TorrentSnapshot::sources (connected peers)
    QList<TransferSource> channels;      // [engine-4] TorrentSnapshot::channels (DHT, trackers ...)
    QBitArray downloading;               // [ui-i] the pieces the simulated peers send in the next step
    int numPieces() const { return int((payload.size() + pieceLength - 1) / pieceLength); }
};

class FakeTorrentBackend : public QObject, public TorrentBackend {
    Q_OBJECT
public:
    explicit FakeTorrentBackend(QObject *parent = nullptr) : QObject(parent), events(new TorrentEvents(this))
    {
        m_timer.setInterval(200);
        connect(&m_timer, &QTimer::timeout, this, &FakeTorrentBackend::step);
        m_timer.start();
    }
    TorrentEvents *events;
    QHash<QString, FakeTorrent> known;   // "swarm knowledge": id → torrent (metadata + data)
    QHash<QString, FakeTorrent> active;
    int metadataDelayMs = 100;
    bool checkOnAdd = false;             // [ui-j] like libtorrent: data already at the save path counts once the metadata is known
    QList<QPair<int, bool>> reserveLog;  // (piece, reserve)
    int maxReservedSeen = 0;
    double maxReservedShareSeen = 0;
    QList<int> injectedOk, injectedBad;
    QStringList rechecks;

    // Create a torrent over files (name, bytes). Returns its id.
    QString makeTorrent(const QString &name, const QList<QPair<QString, QByteArray>> &files, int pieceLength)
    {
        FakeTorrent t;
        t.name = name;
        t.pieceLength = pieceLength;
        qint64 off = 0;
        for (const auto &f : files) {
            TorrentFile tf;
            tf.path = files.size() == 1 ? f.first : name + QLatin1Char('/') + f.first;
            tf.size = f.second.size();
            tf.offset = off;
            off += tf.size;
            t.files << tf;
            t.payload += f.second;
        }
        QCryptographicHash h(QCryptographicHash::Sha1);
        h.addData(name.toUtf8());
        h.addData(t.payload);
        t.id = QString::fromLatin1(h.result().toHex());
        t.have = QBitArray(t.numPieces());
        known.insert(t.id, t);
        return t.id;
    }
    QByteArray piece(const FakeTorrent &t, int p) const { return t.payload.mid(p * t.pieceLength, t.pieceLength); }

    QString addTorrent(const QString &uri, const QByteArray &, const QString &saveDir, bool paused,
                       const QList<int> &wantedFiles, QString *error) override
    {
        QString id;
        if (uri.startsWith(QLatin1String("magnet:"))) {
            const int i = uri.indexOf(QLatin1String("btih:"));
            const QString tok = i >= 0 ? uri.mid(i + 5).section(QLatin1Char('&'), 0, 0) : QString();
            if (tok.size() == 40) id = tok.toLower();
            else if (tok.size() == 32) id = QString::fromLatin1(hash::fromBase32(tok).toHex());
        } else if (uri.endsWith(QLatin1String(".torrent"))) {
            id = QFileInfo(uri).completeBaseName();
        }
        if (id.size() != 40) { if (error) *error = QStringLiteral("bad uri"); return QString(); }
        if (active.contains(id)) return id;
        FakeTorrent t;
        if (known.contains(id)) {
            t = known.value(id);
            t.metadata = false;
        } else {
            t.id = id;
        }
        t.savePath = saveDir;
        t.paused = paused;
        t.wanted = wantedFiles;
        t.have = QBitArray(t.pieceLength ? t.numPieces() : 0);
        active.insert(id, t);
        if (known.contains(id)) {
            QTimer::singleShot(metadataDelayMs, this, [this, id] {
                if (!active.contains(id)) return;
                active[id].metadata = true;
                if (checkOnAdd) {   // [ui-j] libtorrent checks the files it finds before it downloads
                    const QStringList before = rechecks;
                    recheck(id);
                    rechecks = before;
                }
                emit events->torrentMetadata(id);
            });
        }
        QTimer::singleShot(0, this, [this, id] { emit events->torrentAdded(id); });
        return id;
    }
    void removeTorrent(const QString &id, bool deleteFiles) override
    {
        if (!active.contains(id)) return;
        if (deleteFiles)
            for (const TorrentFile &f : active.value(id).files) QFile::remove(QDir(active.value(id).savePath).filePath(f.path));
        active.remove(id);
        QTimer::singleShot(0, this, [this, id] { emit events->torrentRemoved(id); });
    }
    void pauseTorrent(const QString &id) override { if (active.contains(id)) active[id].paused = true; }
    void resumeTorrent(const QString &id) override { if (active.contains(id)) active[id].paused = false; }
    void setWantedFiles(const QString &id, const QList<int> &idx) override { if (active.contains(id)) active[id].wanted = idx; }
    void setTorrentPriority(const QString &, int) override {}
    void recheck(const QString &id) override
    {
        if (!active.contains(id)) return;
        rechecks << id;
        FakeTorrent &t = active[id];
        for (int p = 0; p < t.numPieces(); ++p) {
            const QByteArray want = piece(t, p);
            QByteArray got;
            const qint64 s = qint64(p) * t.pieceLength, e = s + want.size();
            for (const TorrentFile &f : std::as_const(t.files)) {
                const qint64 a = qMax(s, f.offset), b = qMin(e, f.offset + f.size);
                if (b <= a) continue;
                QFile file(QDir(t.savePath).filePath(f.path));
                if (!file.open(QIODevice::ReadOnly) || !file.seek(a - f.offset)) { got.clear(); break; }
                got += file.read(b - a);
            }
            t.have.setBit(p, got == want);
        }
    }
    void reannounce(const QString &id) override { reannounces << id; }   // [eng-1]
    QStringList reannounces;
    // [eng-1] a torrent the swarm learns later (peers come back): its metadata arrives now
    void giveMetadata(const QString &id)
    {
        if (!active.contains(id) || !known.contains(id) || active.value(id).metadata) return;
        const FakeTorrent was = active.value(id);
        FakeTorrent t = known.value(id);
        t.savePath = was.savePath;
        t.paused = was.paused;
        t.wanted = was.wanted;
        t.have = QBitArray(t.numPieces());
        t.metadata = true;
        active.insert(id, t);
        emit events->torrentMetadata(id);
    }
    QStringList torrents() const override { return active.keys(); }
    TorrentSnapshot snapshot(const QString &id) const override
    {
        TorrentSnapshot s;
        if (!active.contains(id)) return s;
        const FakeTorrent &t = active.value(id);
        s.id = id;
        s.savePath = t.savePath;
        s.paused = t.paused;
        s.sources = t.peerRows;
        s.channels = t.channels;
        s.peersTotal = t.swarmTotal;   // [eng-1]
        if (!t.metadata) { s.state = TransferState::FindingSources; s.peers = t.metaPeers; return s; }
        s.hasMetadata = true;
        s.name = t.name;
        s.pieceLength = t.pieceLength;
        s.numPieces = t.numPieces();
        s.have = t.have;
        if (!t.paused) s.downloading = t.downloading;   // [ui-i] nothing arrives while paused
        s.files = t.files;
        bool all = true;
        for (int i = 0; i < s.files.size(); ++i) {
            const bool w = t.wanted.isEmpty() || t.wanted.contains(i);
            s.files[i].priority = w ? 4 : 0;
            qint64 done = 0;
            for (int p = 0; p < s.numPieces; ++p) {
                const qint64 a = qMax(qint64(p) * t.pieceLength, s.files[i].offset);
                const qint64 b = qMin(qint64(p + 1) * t.pieceLength, s.files[i].offset + s.files[i].size);
                if (b > a && t.have.testBit(p)) done += b - a;
            }
            s.files[i].done = done;
            if (w) { s.totalSize += s.files[i].size; s.totalDone += done; if (done < s.files[i].size) all = false; }
        }
        s.finished = all;
        s.peers = t.peers;
        s.seeds = t.peers;
        s.rateDownBps = (t.peerPiecesPerTick > 0 && !t.paused && !all) ? t.peerPiecesPerTick * t.pieceLength * 5 : 0;
        s.state = all ? TransferState::Seeding : TransferState::Transferring;
        return s;
    }
    QByteArray pieceHash(const QString &id, int p) const override
    {
        if (!active.contains(id) || !active.value(id).metadata) return {};
        const FakeTorrent &t = active.value(id);
        if (p < 0 || p >= t.numPieces()) return {};
        return hash::BtPieceHasher::hashPiece(piece(t, p));
    }
    // [dl-1] the order flags the engine set, per torrent (last call wins)
    QHash<QString, QPair<bool, bool>> order;
    int orderCalls = 0;
    bool setDownloadOrder(const QString &id, bool sequential, bool firstLast) override
    {
        ++orderCalls;
        order.insert(id, qMakePair(sequential, firstLast));
        return true;
    }
    void reservePieces(const QString &id, const QList<int> &pieces, bool reserve) override
    {
        if (!active.contains(id)) return;
        FakeTorrent &t = active[id];
        for (int p : pieces) {
            reserveLog << qMakePair(p, reserve);
            if (reserve) t.reserved.insert(p); else t.reserved.remove(p);
        }
        maxReservedSeen = qMax(maxReservedSeen, int(t.reserved.size()));
        int missing = 0;
        for (int p = 0; p < t.numPieces(); ++p) if (!t.have.testBit(p)) ++missing;
        if (missing >= 20 && t.peers > 0) maxReservedShareSeen = qMax(maxReservedShareSeen, double(t.reserved.size()) / missing);
    }
    void injectPiece(const QString &id, int p, const QByteArray &data) override
    {
        QTimer::singleShot(5, this, [this, id, p, data] {
            if (!active.contains(id)) return;
            FakeTorrent &t = active[id];
            if (t.have.testBit(p)) { emit events->torrentPieceFinished(id, p); return; }
            if (data != piece(t, p)) {
                injectedBad << p;
                emit events->torrentPieceFailed(id, p);
                return;
            }
            injectedOk << p;
            completePiece(t, p);
        });
    }
    bool seedLocalFile(const QString &, QString *) override { return false; }
    QObject *torrentEvents() override { return events; }

    void completePiece(FakeTorrent &t, int p)
    {
        t.have.setBit(p);
        t.reserved.remove(p);
        writePiece(t, p);
        const QString id = t.id;
        emit events->torrentPieceFinished(id, p);
        emit events->torrentChanged(id);
        if (snapshot(id).finished) emit events->torrentFinished(id);
    }
    void writePiece(const FakeTorrent &t, int p)
    {
        const qint64 s = qint64(p) * t.pieceLength, e = qMin<qint64>(s + t.pieceLength, t.payload.size());
        for (int i = 0; i < t.files.size(); ++i) {
            const TorrentFile &f = t.files.at(i);
            const qint64 a = qMax(s, f.offset), b = qMin(e, f.offset + f.size);
            if (b <= a) continue;
            const QString path = QDir(t.savePath).filePath(f.path);
            QDir().mkpath(QFileInfo(path).absolutePath());
            QFile file(path);
            if (!file.open(QIODevice::ReadWrite)) continue;
            file.seek(a - f.offset);
            file.write(t.payload.constData() + a, b - a);
        }
    }

private:
    void step()
    {
        for (auto it = active.begin(); it != active.end(); ++it) {
            FakeTorrent &t = it.value();
            if (!t.metadata || t.paused || t.peerPiecesPerTick <= 0) continue;
            int n = t.peerPiecesPerTick;
            auto wantedPiece = [&t](int p) {
                if (t.have.testBit(p) || t.reserved.contains(p)) return false;
                bool wanted = t.wanted.isEmpty();
                for (int i = 0; !wanted && i < t.files.size(); ++i) {
                    const TorrentFile &f = t.files.at(i);
                    if (t.wanted.contains(i) && f.offset < qint64(p + 1) * t.pieceLength && f.offset + f.size > qint64(p) * t.pieceLength) wanted = true;
                }
                return wanted;
            };
            for (int p = 0; p < t.numPieces() && n > 0; ++p) {
                if (!wantedPiece(p)) continue;
                completePiece(t, p);
                --n;
            }
            // [ui-i] the download queue: the pieces the next step completes
            t.downloading = QBitArray(t.numPieces());
            n = t.peerPiecesPerTick;
            for (int p = 0; p < t.numPieces() && n > 0; ++p)
                if (wantedPiece(p)) { t.downloading.setBit(p); --n; }
        }
    }
    QTimer m_timer;
};

} // namespace mx::etest
