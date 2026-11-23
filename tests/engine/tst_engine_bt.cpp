// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX engine: integration with the REAL BitTorrent plugin (libtorrent), offline on 127.0.0.1:
// node A seeds a file; the engine (node B) downloads the same torrent from A AND from a fake HTTP
// mirror at the same time; pieces from the mirror go through reservePieces + injectPiece.
#include <QtTest/QTest>

#include "bt_plugin.h"
#include "engine_env.h"
#include "mxhash/ed2k.h"
#include "mx/download_order.h"

using namespace mx;
using namespace mx::etest;

namespace {
struct BtNode {
    SimpleSettings settings;
    std::unique_ptr<NetworkPlugin> plugin;
    TorrentBackend *be = nullptr;
    BitTorrentControl *ctl = nullptr;
    BitTorrentEvents *ev = nullptr;
    QString profile;

    explicit BtNode(const QString &prof, bool limitLan = false) : profile(prof)
    {
        namespace k = keys;
        settings.setValue(QString(k::bt::Dht), false);
        settings.setValue(QString(k::bt::Lsd), false);
        settings.setValue(QString(k::connection::Upnp), false);
        settings.setValue(QString(k::connection::NatPmp), false);
        settings.setValue(QString(k::connection::Ipv6), false);
        settings.setValue(QString(k::connection::BindInterface), QStringLiteral("127.0.0.1"));
        settings.setValue(QString(k::bt::ListenPort), 0);
        settings.setValue(QString(k::bt::ExtraTrackers), QStringList());
        settings.setValue(QString(k::bt::SeedRatioLimit), 0.0);
        settings.setValue(QString(k::bandwidth::LimitLan), limitLan);
        settings.setValue(QStringLiteral("bt/allowMultipleConnectionsPerIp"), true);
        plugin.reset(createBitTorrentPlugin());
        be = dynamic_cast<TorrentBackend *>(plugin.get());
        ctl = dynamic_cast<BitTorrentControl *>(plugin.get());
        ev = qobject_cast<BitTorrentEvents *>(be->torrentEvents());
        PluginContext ctx;
        ctx.settings = &settings;
        ctx.profileDir = profile;
        ctx.dataDir = profile + QStringLiteral("/bt");
        ctx.clientName = QStringLiteral("OctoMX");
        ctx.clientVersion = QStringLiteral(MX_VERSION_STRING);
        plugin->init(ctx);
    }
    bool start()
    {
        plugin->start();
        return QTest::qWaitFor([this] { return plugin->status().listenPort > 0; }, 10000);
    }
    QString endpoint() const { return QStringLiteral("127.0.0.1:%1").arg(plugin->status().listenPort); }
};

void writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
}
} // namespace

class TstEngineBt : public QObject {
    Q_OBJECT
private slots:
    void btPlusHttpMirror();
    void httpOnlyInjection();
    void promotionWithRealLibtorrent();
    void moveTorrentStorage();   // [engine-3]
    void sequentialWithRealLibtorrent();   // [dl-1]
    void firstLastWithRealLibtorrent();    // [dl-1]

private:
    void run(bool withPeer);
    void order(bool sequential);
};

void TstEngineBt::run(bool withPeer)
{
    const QString name = withPeer ? QStringLiteral("bt-mixed") : QStringLiteral("bt-inject");
    Env env(name);
    const QByteArray data = randomBytes(24 * 1024 * 1024 + 4321, withPeer ? 51 : 52);
    const QString src = env.root + QStringLiteral("/seed/payload.bin");
    writeFile(src, data);

    BtNode a(env.root + QStringLiteral("/nodeA"));
    QVERIFY(a.start());
    QSignalSpy ready(a.ev, &BitTorrentEvents::seedReady);
    QString err;
    QVERIFY2(a.be->seedLocalFile(src, &err), qPrintable(err));
    QVERIFY(QTest::qWaitFor([&] { return !ready.isEmpty(); }, 60000));
    const QString tid = ready.first().at(1).toString();
    const QString torrentFile = env.root + QStringLiteral("/nodeA/bt/torrents/") + tid + QStringLiteral(".torrent");
    QVERIFY2(QTest::qWaitFor([&] { return QFile::exists(torrentFile) && QFileInfo(torrentFile).size() > 0; }, 10000), qPrintable(torrentFile));

    BtNode b(env.root + QStringLiteral("/nodeB"), /*limitLan*/ true);
    QVERIFY(b.start());
    if (withPeer) b.ctl->setRateLimits(1536 * 1024, 0);   // slow BitTorrent peer, so the mirror matters
    auto *http = env.addPlugin(Net::Http, NetworkPlugin::CanFetchRanges);
    SourceConfig mirror;
    mirror.data = data;
    mirror.bytesPerSec = withPeer ? (2 << 20) : (16 << 20);
    mirror.maxPending = 4;
    mirror.pref = 256 * 1024;
    const SearchSource ms = http->addSource(QStringLiteral("https://mirror.example/payload.bin"), mirror);
    env.engineUp(b.be, b.plugin.get());
    struct Down { Env &e; ~Down() { e.engineDown(true); } } down{env};   // before the nodes go away

    DownloadRequest r;
    r.uri = torrentFile;
    r.sources = {ms};
    const QString id = env.engine->download(r);
    QVERIFY(!id.isEmpty());
    if (withPeer) {
        QVERIFY(QTest::qWaitFor([&] { return !b.be->snapshot(tid).id.isEmpty(); }, 10000));
        b.ctl->addPeer(tid, a.endpoint());
    }
    QVERIFY2(env.waitDone(id, 180000), qPrintable(env.info(id).statusText));
    const QVariantMap dbg = env.engine->debugInfo(id);
    const TorrentSnapshot s = b.be->snapshot(tid);
    const int injected = dbg.value(QStringLiteral("injectedPieces")).toInt();
    qInfo("%s: %d pieces of %d bytes; injected from HTTP: %d pieces (%lld bytes); from BitTorrent: %d pieces; preverify failures %d",
          qPrintable(name), s.numPieces, s.pieceLength, injected, dbg.value(QStringLiteral("injectedBytes")).toLongLong(),
          s.numPieces - injected, dbg.value(QStringLiteral("piecePreverifyFailures")).toInt());
    QCOMPARE(readFile(env.info(id).savePath), data);
    QVERIFY(injected > 0);
    if (withPeer) {
        QVERIFY2(injected < s.numPieces, "nothing came from the BitTorrent peer");
        QVERIFY(a.be->snapshot(tid).uploadedBytes > 0);
    } else {
        QCOMPARE(injected, s.numPieces);
    }
}

void TstEngineBt::btPlusHttpMirror() { run(true); }
void TstEngineBt::httpOnlyInjection() { run(false); }

// part-file download (eD2k-like source + part hashes) learns the btih from a hash-link plugin
// mid-way, promotes itself to a real libtorrent torrent and finishes from the BT peer
void TstEngineBt::promotionWithRealLibtorrent()
{
    constexpr qint64 P = hash::Ed2kHasher::PartSize;
    Env env(QStringLiteral("bt-promote"));
    const QByteArray data = randomBytes(3 * P + 777, 53);
    const QString src = env.root + QStringLiteral("/seed/promo.bin");
    writeFile(src, data);
    BtNode a(env.root + QStringLiteral("/nodeA"));
    QVERIFY(a.start());
    QSignalSpy ready(a.ev, &BitTorrentEvents::seedReady);
    QString err;
    QVERIFY2(a.be->seedLocalFile(src, &err), qPrintable(err));
    QVERIFY(QTest::qWaitFor([&] { return !ready.isEmpty(); }, 60000));
    const QString tid = ready.first().at(1).toString();

    BtNode b(env.root + QStringLiteral("/nodeB"));
    QVERIFY(b.start());
    auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges | NetworkPlugin::CanFindSources);
    auto *links = env.addPlugin(Net::Overlay, NetworkPlugin::CanHashLinks);
    ed->hashSetOnFind << qMakePair(P, ed2kParts(data));
    SourceConfig c;
    c.data = data;
    c.bytesPerSec = 3 << 20;
    c.stallAfterBytes = true;            // goes silent after the first part: the BT peer must finish
    c.stallAt = P + 2 * 1024 * 1024;
    const SearchSource s = ed->addSource(QStringLiteral("ed2k://promo"), c, QStringLiteral("30.0.0.1:4662"));
    env.engineUp(b.be, b.plugin.get());
    struct Down { Env &e; ~Down() { e.engineDown(true); } } down{env};

    DownloadRequest r;
    r.name = QStringLiteral("promo.bin");
    r.size = data.size();
    r.hashes = hashesOf(data, 2);
    r.sources = {s};
    const QString id = env.engine->download(r);
    QVERIFY(QTest::qWaitFor([&] { return env.info(id).done >= P; }, 30000));
    QCOMPARE(env.engine->debugInfo(id).value(QStringLiteral("store")).toString(), QStringLiteral("part"));
    const qint64 verifiedBefore = env.info(id).done;
    // the hash-link registry tells us the torrent of this file (a hint: checked against a piece)
    FileHashes all = r.hashes;
    all.btih = QByteArray::fromHex(tid.toLatin1());
    links->emitHashLinks(r.hashes, all, data.size());
    QVERIFY(QTest::qWaitFor([&] { return b.be->torrents().contains(tid); }, 10000));
    b.ctl->addPeer(tid, a.endpoint());
    QVERIFY(QTest::qWaitFor([&] { return env.engine->debugInfo(id).value(QStringLiteral("store")).toString() == QLatin1String("torrent"); }, 30000));
    QVERIFY2(env.waitDone(id, 120000), qPrintable(env.info(id).statusText));
    {
        const QByteArray got = readFile(env.info(id).savePath);
        qint64 diff = -1;
        for (qint64 i = 0; i < qMin(got.size(), data.size()); ++i)
            if (got.at(i) != data.at(i)) { diff = i; break; }
        QVERIFY2(got == data, qPrintable(QStringLiteral("%1 of %2 bytes, first difference at %3").arg(got.size()).arg(data.size()).arg(diff)));
    }
    QTest::qWait(1500);
    const qint64 fromPeer = a.be->snapshot(tid).uploadedBytes;
    const int L = b.be->snapshot(tid).pieceLength;
    qInfo("promoted after %lld verified bytes; BT peer uploaded %lld of %lld bytes; injected %d pieces",
          verifiedBefore, fromPeer, qint64(data.size()), env.engine->debugInfo(id).value(QStringLiteral("injectedPieces")).toInt());
    QVERIFY2(fromPeer > 0, "nothing came from the BitTorrent peer");
    QVERIFY2(fromPeer <= data.size() - verifiedBefore + 4LL * L, "verified data was downloaded again");
    QVERIFY(QFileInfo(env.info(id).savePath).absolutePath() == QFileInfo(env.root + QStringLiteral("/incoming/x")).absolutePath());
}

// [engine-3] TransferService::setDownloadTarget on a running torrent: libtorrent move_storage with a
// real session. A target that already holds a file of that name is refused by libtorrent (nothing
// overwritten, the download carries on where it was); a free target moves the data mid-download
// and the torrent completes there.
void TstEngineBt::moveTorrentStorage()
{
    Env env(QStringLiteral("bt-move"));
    const QByteArray data = randomBytes(12 * 1024 * 1024 + 99, 54);
    const QString src = env.root + QStringLiteral("/seed/move.bin");
    writeFile(src, data);
    BtNode a(env.root + QStringLiteral("/nodeA"));
    QVERIFY(a.start());
    QSignalSpy ready(a.ev, &BitTorrentEvents::seedReady);
    QString err;
    QVERIFY2(a.be->seedLocalFile(src, &err), qPrintable(err));
    QVERIFY(QTest::qWaitFor([&] { return !ready.isEmpty(); }, 60000));
    const QString tid = ready.first().at(1).toString();
    const QString torrentFile = env.root + QStringLiteral("/nodeA/bt/torrents/") + tid + QStringLiteral(".torrent");
    QVERIFY(QTest::qWaitFor([&] { return QFile::exists(torrentFile) && QFileInfo(torrentFile).size() > 0; }, 10000));

    BtNode b(env.root + QStringLiteral("/nodeB"), /*limitLan*/ true);
    QVERIFY(b.start());
    b.ctl->setRateLimits(1024 * 1024, 0);   // ~12 s: time to move in the middle
    env.engineUp(b.be, b.plugin.get());
    struct Down { Env &e; ~Down() { e.engineDown(true); } } down{env};
    QSignalSpy moved(b.ev, &TorrentEvents::torrentStorageMoved);
    DownloadRequest r;
    r.uri = torrentFile;
    const QString id = env.engine->download(r);
    QVERIFY(QTest::qWaitFor([&] { return !b.be->snapshot(tid).id.isEmpty(); }, 10000));
    b.ctl->addPeer(tid, a.endpoint());
    QVERIFY(QTest::qWaitFor([&] { return b.be->snapshot(tid).totalDone > 2 * 1024 * 1024; }, 30000));

    // 1) the target already has a "move.bin": refused, nothing overwritten
    const QString busy = env.root + QStringLiteral("/busy");
    writeFile(busy + QStringLiteral("/move.bin"), QByteArrayLiteral("keep me"));
    QVERIFY(env.engine->setDownloadTarget(id, busy, QString()));
    QVERIFY(QTest::qWaitFor([&] { return moved.count() == 1; }, 20000));
    QCOMPARE(moved.first().at(2).toBool(), false);
    QCOMPARE(readFile(busy + QStringLiteral("/move.bin")), QByteArrayLiteral("keep me"));
    const QVariantMap d1 = env.engine->debugInfo(id);
    qInfo("refused move: %s", qPrintable(d1.value(QStringLiteral("moveError")).toString()));
    QVERIFY(!d1.value(QStringLiteral("moveError")).toString().isEmpty());
    // a torrent keeps its own file names
    QVERIFY(!env.engine->setDownloadTarget(id, busy, QStringLiteral("other-name.bin")));

    // 2) a free target, mid-download
    const qint64 doneAtMove = b.be->snapshot(tid).totalDone;
    const QString target = env.root + QStringLiteral("/moved here");
    QVERIFY(env.engine->setDownloadTarget(id, target, QString()));
    QVERIFY(QTest::qWaitFor([&] { return moved.count() == 2; }, 30000));
    QCOMPARE(moved.at(1).at(2).toBool(), true);
    QCOMPARE(QDir::cleanPath(moved.at(1).at(1).toString()), QDir::cleanPath(target));
    QCOMPARE(QDir::cleanPath(env.engine->debugInfo(id).value(QStringLiteral("saveDir")).toString()), QDir::cleanPath(target));
    QVERIFY2(env.waitDone(id, 120000), qPrintable(env.info(id).statusText));
    const QString path = env.info(id).savePath;
    qInfo("moved at %lld of %lld bytes; completed at %s", doneAtMove, qint64(data.size()), qPrintable(path));
    QVERIFY(doneAtMove < data.size());
    QCOMPARE(QDir::cleanPath(QFileInfo(path).absolutePath()), QDir::cleanPath(target));
    QCOMPARE(readFile(path), data);
    QVERIFY(!QFile::exists(env.root + QStringLiteral("/incoming/move.bin")));
    QCOMPARE(b.be->snapshot(tid).savePath.isEmpty() ? target : QDir::cleanPath(b.be->snapshot(tid).savePath), QDir::cleanPath(target));
}

// [dl-1] libtorrent follows the engine's order flags: one peer seeds, node B downloads slowly; the
// engine's block map (debugInfo "heldRanges", complete pieces) shows the order
namespace {
QList<QPair<qint64, qint64>> heldRanges(DownloadEngine *e, const QString &id)
{
    QList<QPair<qint64, qint64>> out;
    for (const QVariant &v : e->debugInfo(id).value(QStringLiteral("heldRanges")).toList())
        out << qMakePair(v.toList().value(0).toLongLong(), v.toList().value(1).toLongLong());
    return out;
}
qint64 covered(const QList<QPair<qint64, qint64>> &r, qint64 s, qint64 e)
{
    qint64 n = 0;
    for (const auto &x : r) n += qMax<qint64>(0, qMin(e, x.second) - qMax(s, x.first));
    return n;
}
} // namespace

void TstEngineBt::order(bool sequential)
{
    const QString name = sequential ? QStringLiteral("bt-seq") : QStringLiteral("bt-fl");
    Env env(name);
    env.settings.setValue(QString(keys::transfers::SequentialDefault), sequential);
    env.settings.setValue(QString(keys::transfers::FirstLastDefault), !sequential);
    // [eng-1] first and last: libtorrent's whole-piece preference (whole_pieces_threshold, 20 s) makes
    // a peer that is fast enough request runs of contiguous pieces, payload rate x 20 s capped at 8 MiB,
    // and the expansion ignores priorities: a band piece near a band edge drags up to 8 MiB of its
    // neighbours along (seen: held 0 to 5.75 and 17.81 to 24 MB when both 4 MiB bands were complete).
    // With 24 MiB the middle (16 MiB) was no larger than that spill on both edges, so whether the middle
    // stayed under its bound was luck (about 1 run in 30 failed, with or without the eng-1 change).
    // 96 MiB keeps the bands at 4 MiB and makes the middle (88 MiB) 5.5 times the largest spill.
    const qint64 size = (sequential ? 24 : 96) * 1024 * 1024;
    const QByteArray data = randomBytes(size, sequential ? 61 : 62);
    const QString src = env.root + QStringLiteral("/seed/clip.mkv");
    writeFile(src, data);
    BtNode a(env.root + QStringLiteral("/nodeA"));
    QVERIFY(a.start());
    QSignalSpy ready(a.ev, &BitTorrentEvents::seedReady);
    QString err;
    QVERIFY2(a.be->seedLocalFile(src, &err), qPrintable(err));
    QVERIFY(QTest::qWaitFor([&] { return !ready.isEmpty(); }, 60000));
    const QString tid = ready.first().at(1).toString();
    const QString torrentFile = env.root + QStringLiteral("/nodeA/bt/torrents/") + tid + QStringLiteral(".torrent");
    QVERIFY(QTest::qWaitFor([&] { return QFile::exists(torrentFile) && QFileInfo(torrentFile).size() > 0; }, 10000));
    BtNode b(env.root + QStringLiteral("/nodeB"), /*limitLan*/ true);
    QVERIFY(b.start());
    b.ctl->setRateLimits((sequential ? 1 : 2) * 1024 * 1024, 0);
    env.engineUp(b.be, b.plugin.get());
    struct Down { Env &e; ~Down() { e.engineDown(true); } } down{env};
    DownloadRequest r;
    r.uri = torrentFile;
    const QString id = env.engine->download(r);
    QVERIFY(!id.isEmpty());
    QVERIFY(QTest::qWaitFor([&] { return !b.be->snapshot(tid).id.isEmpty(); }, 10000));
    b.ctl->addPeer(tid, a.endpoint());
    const qint64 band = order::headBandBytes(size);   // 4 MiB
    if (sequential) {
        QVERIFY(QTest::qWaitFor([&] { return covered(heldRanges(env.engine.get(), id), 0, size) >= 6 * 1024 * 1024; }, 60000));
        const auto h = heldRanges(env.engine.get(), id);
        const qint64 have = covered(h, 0, size);
        const qint64 L = b.be->snapshot(tid).pieceLength;
        qInfo("libtorrent sequential: %lld bytes in %d ranges, first [%lld, %lld), piece %lld", have, int(h.size()),
              h.value(0).first, h.value(0).second, L);
        QCOMPARE(h.value(0).first, qint64(0));
        // complete pieces form a prefix, up to the pieces libtorrent has in flight
        QVERIFY2(h.last().second - have <= 24 * L, "pieces far ahead of the prefix");
    } else {
        // [eng-1] when the order reached libtorrent (bytes already there) and when the bands were done
        QElapsedTimer clock;
        clock.start();
        qint64 orderMs = -1, doneAtOrder = -1, lastSample = -1000;
        QStringList series;   // every second: head / tail / middle MB held
        QVERIFY(QTest::qWaitFor([&] {
            if (orderMs < 0 && env.engine->debugInfo(id).value(QStringLiteral("orderBackend")).toBool()) {
                orderMs = clock.elapsed();
                doneAtOrder = b.be->snapshot(tid).totalDone;
            }
            const auto h = heldRanges(env.engine.get(), id);
            if (clock.elapsed() - lastSample >= 1000) {
                lastSample = clock.elapsed();
                series << QStringLiteral("%1s %2/%3/%4").arg(lastSample / 1000)
                              .arg(covered(h, 0, band) / 1048576.0, 0, 'f', 1).arg(covered(h, size - band, size) / 1048576.0, 0, 'f', 1)
                              .arg(covered(h, band, size - band) / 1048576.0, 0, 'f', 1);
            }
            return covered(h, 0, band) == band && covered(h, size - band, size) == band;
        }, 60000));
        qInfo("head/tail/middle MB: %s", qPrintable(series.join(QStringLiteral("  "))));
        const auto h = heldRanges(env.engine.get(), id);
        const qint64 middle = covered(h, band, size - band);
        QStringList rs;   // where the held ranges are (MB): the spill is contiguous with the bands
        for (const auto &x : h) rs << QStringLiteral("%1-%2").arg(x.first / 1048576.0, 0, 'f', 2).arg(x.second / 1048576.0, 0, 'f', 2);
        qInfo("held ranges MB: %s", qPrintable(rs.join(QLatin1Char(' '))));
        QStringList conns;   // the connections to the seed: transport and bytes received on each
        for (const TransferSource &p : b.be->snapshot(tid).sources) conns << QStringLiteral("%1 %2").arg(p.connection).arg(p.bytes);
        qInfo("libtorrent first and last: head and tail bands complete, middle %lld of %lld bytes; order in force after %lld ms"
              " with %lld bytes done, bands complete after %lld ms; connections: %s",
              middle, size - 2 * band, orderMs, doneAtOrder, clock.elapsed(), qPrintable(conns.join(QStringLiteral(", "))));
        // libtorrent's picker honours the priorities but still fetches some other pieces (allowed-fast
        // pieces of a choking seed, partial pieces, the contiguous expansion above, at most 8 MiB per
        // picked run): the bands are complete well before the middle. Without the order the bands would
        // complete only near the end, with almost all of the middle there.
        QVERIFY2(middle <= (size - 2 * band) * 4 / 10, "the middle came before the head and tail bands");
    }
    QVERIFY(env.engine->debugInfo(id).value(QStringLiteral("orderBackend")).toBool());
    env.engine->cancel(id, true);
}

void TstEngineBt::sequentialWithRealLibtorrent() { order(true); }
void TstEngineBt::firstLastWithRealLibtorrent() { order(false); }

QTEST_GUILESS_MAIN(TstEngineBt)
#include "tst_engine_bt.moc"
