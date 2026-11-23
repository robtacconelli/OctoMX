// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX engine: TorrentStore tests with an in-memory TorrentBackend (real SHA-1 piece hashes).
#include <QtTest/QTest>

#include "engine_env.h"
#include "mxhash/encoding.h"

using namespace mx;
using namespace mx::etest;

namespace {
QString magnetFor(const QString &id, const QString &name)
{
    return QStringLiteral("magnet:?xt=urn:btih:%1&dn=%2").arg(id, name);
}

DownloadRequest torrentReq(const QString &id, const QString &name, qint64 size, const QList<SearchSource> &extra)
{
    DownloadRequest r;
    r.name = name;
    r.size = size;
    r.hashes.btih = QByteArray::fromHex(id.toLatin1());
    r.uri = magnetFor(id, name);
    SearchSource bt;
    bt.net = Net::BitTorrent;
    bt.uri = r.uri;
    r.sources << bt << extra;
    return r;
}

int reservedTrueCount(const FakeTorrentBackend &be, int piece)
{
    int n = 0;
    for (const auto &e : be.reserveLog) if (e.first == piece && e.second) ++n;
    return n;
}
} // namespace

class TstEngineTorrent : public QObject {
    Q_OBJECT
private slots:
    void metadataAndMirroring();
    void foreignInjectionWithoutPeers();
    void mixedNetworksRespectShare();
    void preverifyCatchesBadSource();
    void reservationTimesOut();
    void boundaryPiecePolicy();
    void promotionPartToTorrent();
    void pauseResumeCancelTorrent();
    void metadataTimeoutFallsBackToPartFile();
    void metadataTimeoutSaysStillLooking();
    void indexTorrentSaysFetching();
    void metadataTimeoutUnknownSize();   // [engine-3]
};

void TstEngineTorrent::metadataAndMirroring()
{
    Env env(QStringLiteral("t-meta"));
    FakeTorrentBackend be;
    be.metadataDelayMs = 600;
    const QByteArray data = randomBytes(4 * 1024 * 1024 + 5, 31);
    const QString tid = be.makeTorrent(QStringLiteral("meta.bin"), {{QStringLiteral("meta.bin"), data}}, 256 * 1024);
    be.known[tid].peerPiecesPerTick = 1;
    be.known[tid].peers = 3;
    env.engineUp(&be);
    const QString id = env.engine->downloadUri(magnetFor(tid, QStringLiteral("meta.bin")));
    QVERIFY(!id.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(env.info(id).statusText.startsWith(QLatin1String("Finding sources... looking for peers")), 2000);   // [engine-4]
    QTRY_VERIFY_WITH_TIMEOUT(env.info(id).state == TransferState::Transferring, 5000);
    const TransferInfo t = env.info(id);
    QCOMPARE(t.size, qint64(data.size()));
    QVERIFY(!t.pieceMap.isEmpty() && t.pieceMap.size() <= 512);
    QVERIFY(t.networks.contains(Net::BitTorrent));
    QVERIFY(t.rateDownBps > 0);
    QVERIFY2(env.waitDone(id, 30000), qPrintable(env.info(id).statusText));
    QCOMPARE(env.info(id).done, qint64(data.size()));
    QCOMPARE(readFile(env.info(id).savePath), data);
    QCOMPARE(env.completed->size(), 1);
}

void TstEngineTorrent::foreignInjectionWithoutPeers()
{
    Env env(QStringLiteral("t-inject"));
    FakeTorrentBackend be;
    auto *http = env.addPlugin(Net::Http, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(6 * 1024 * 1024 + 77, 32);
    const QString tid = be.makeTorrent(QStringLiteral("inj.bin"), {{QStringLiteral("inj.bin"), data}}, 256 * 1024);
    SourceConfig c;
    c.data = data;
    c.bytesPerSec = 4 << 20;
    c.maxPending = 4;
    const SearchSource s = http->addSource(QStringLiteral("https://mirror/inj.bin"), c);
    env.engineUp(&be);
    const QString id = env.engine->download(torrentReq(tid, QStringLiteral("inj.bin"), data.size(), {s}));
    QVERIFY2(env.waitDone(id, 40000), qPrintable(env.info(id).statusText));
    QCOMPARE(readFile(env.info(id).savePath), data);
    const int pieces = int((data.size() + 256 * 1024 - 1) / (256 * 1024));
    QCOMPARE(be.injectedOk.size(), pieces);
    QVERIFY(be.injectedBad.isEmpty());
    const QVariantMap dbg = env.engine->debugInfo(id);
    QCOMPARE(dbg.value(QStringLiteral("injectedPieces")).toInt(), pieces);
    QCOMPARE(dbg.value(QStringLiteral("verifiedByNet")).toMap().value(QStringLiteral("HTTP")).toLongLong(), qint64(data.size()));
}

void TstEngineTorrent::mixedNetworksRespectShare()
{
    Env env(QStringLiteral("t-mixed"));
    FakeTorrentBackend be;
    auto *http = env.addPlugin(Net::Http, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(8 * 1024 * 1024, 33);
    const QString tid = be.makeTorrent(QStringLiteral("mix.bin"), {{QStringLiteral("mix.bin"), data}}, 64 * 1024);
    be.known[tid].peerPiecesPerTick = 2;
    be.known[tid].peers = 2;
    SourceConfig c;
    c.data = data;
    c.bytesPerSec = 1 << 20;
    const SearchSource s = http->addSource(QStringLiteral("https://mirror/mix.bin"), c);
    env.engineUp(&be);
    const QString id = env.engine->download(torrentReq(tid, QStringLiteral("mix.bin"), data.size(), {s}));
    QTRY_VERIFY_WITH_TIMEOUT(env.info(id).networks.size() == 2, 10000);   // BitTorrent + HTTP at once
    QVERIFY2(env.waitDone(id, 60000), qPrintable(env.info(id).statusText));
    QCOMPARE(readFile(env.info(id).savePath), data);
    const int pieces = 128;
    qInfo("injected %lld pieces, BitTorrent %lld; max reserved share %.2f", qint64(be.injectedOk.size()),
          qint64(pieces - be.injectedOk.size()), be.maxReservedShareSeen);
    QVERIFY(be.injectedOk.size() > 0);
    QVERIFY(be.injectedOk.size() < pieces);
    QVERIFY2(be.maxReservedShareSeen <= 0.36, "reserved more than the libtorrent share allows");
}

void TstEngineTorrent::preverifyCatchesBadSource()
{
    Env env(QStringLiteral("t-bad"));
    FakeTorrentBackend be;
    auto *http = env.addPlugin(Net::Http, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(3 * 1024 * 1024, 34);
    const QString tid = be.makeTorrent(QStringLiteral("bad.bin"), {{QStringLiteral("bad.bin"), data}}, 128 * 1024);
    SourceConfig good, bad;
    good.data = bad.data = data;
    good.bytesPerSec = 1 << 20;
    bad.bytesPerSec = 4 << 20;
    bad.corruptAll = true;
    const SearchSource sg = http->addSource(QStringLiteral("https://good/bad.bin"), good);
    const SearchSource sb = http->addSource(QStringLiteral("https://evil/bad.bin"), bad);
    env.engineUp(&be);
    const QString id = env.engine->download(torrentReq(tid, QStringLiteral("bad.bin"), data.size(), {sb, sg}));
    QVERIFY2(env.waitDone(id, 60000), qPrintable(env.info(id).statusText));
    QCOMPARE(readFile(env.info(id).savePath), data);
    QVERIFY2(be.injectedBad.isEmpty(), "a bad piece reached the backend");
    const QVariantMap dbg = env.engine->debugInfo(id);
    QVERIFY(dbg.value(QStringLiteral("piecePreverifyFailures")).toInt() >= 1);
    bool evilBanned = false;
    for (const QVariant &v : dbg.value(QStringLiteral("sources")).toList())
        if (v.toMap().value(QStringLiteral("key")).toString().contains(QLatin1String("evil")))
            evilBanned = v.toMap().value(QStringLiteral("banned")).toBool();
    QVERIFY(evilBanned);
}

void TstEngineTorrent::reservationTimesOut()
{
    Env env(QStringLiteral("t-timeout"));
    env.tuning.requestTimeoutMs = 60000;   // let the reservation timer act, not the request timer
    FakeTorrentBackend be;
    auto *http = env.addPlugin(Net::Http, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(2 * 1024 * 1024, 35);
    const QString tid = be.makeTorrent(QStringLiteral("to.bin"), {{QStringLiteral("to.bin"), data}}, 64 * 1024);
    be.known[tid].peerPiecesPerTick = 1;
    be.known[tid].peers = 1;
    SourceConfig stall;
    stall.data = data;
    stall.stallAfterBytes = true;
    stall.stallAt = 100 * 1024;
    const SearchSource s = http->addSource(QStringLiteral("https://stall/to.bin"), stall);
    env.engineUp(&be);
    const QString id = env.engine->download(torrentReq(tid, QStringLiteral("to.bin"), data.size(), {s}));
    bool returned = false;
    QVERIFY(QTest::qWaitFor([&] {
        for (const auto &e : be.reserveLog) if (!e.second) returned = true;
        return returned;
    }, 15000));
    QVERIFY2(env.waitDone(id, 60000), qPrintable(env.info(id).statusText));
    QCOMPARE(readFile(env.info(id).savePath), data);
}

void TstEngineTorrent::boundaryPiecePolicy()
{
    Env env(QStringLiteral("t-boundary"));
    FakeTorrentBackend be;
    auto *http = env.addPlugin(Net::Http, NetworkPlugin::CanFetchRanges);
    const QByteArray a = randomBytes(1000000, 36), b = randomBytes(1000000, 37);
    const int L = 65536;
    const QString tid = be.makeTorrent(QStringLiteral("Bundle"), {{QStringLiteral("A.bin"), a}, {QStringLiteral("sub/B.bin"), b}}, L);
    be.known[tid].peerPiecesPerTick = 0;   // BitTorrent joins only after the foreign part is done
    be.known[tid].peers = 0;
    SourceConfig c;
    c.data = a;
    c.bytesPerSec = 2 << 20;
    SearchSource s = http->addSource(QStringLiteral("https://archive/A.bin"), c);
    s.extra.insert(QStringLiteral("bundlePath"), QStringLiteral("A.bin"));
    env.engineUp(&be);
    DownloadRequest r = torrentReq(tid, QStringLiteral("Bundle"), a.size() + b.size(), {s});
    const QString id = env.engine->download(r);
    const int boundary = 1000000 / L;   // spans A and B
    QVERIFY(QTest::qWaitFor([&] {
        for (int p = 0; p < boundary; ++p) if (!be.active.value(tid).have.testBit(p)) return false;
        return true;
    }, 30000));
    QTest::qWait(1500);
    QVERIFY(!be.active.value(tid).have.testBit(boundary));   // nobody could deliver B's bytes
    be.active[tid].peerPiecesPerTick = 2;
    be.active[tid].peers = 1;
    QVERIFY2(env.waitDone(id, 60000), qPrintable(env.info(id).statusText));
    QCOMPARE(reservedTrueCount(be, boundary), 0);
    for (int p = 0; p < boundary; ++p) QVERIFY2(reservedTrueCount(be, p) > 0, qPrintable(QString::number(p)));
    for (int p = boundary + 1; p < 2000000 / L; ++p) QCOMPARE(reservedTrueCount(be, p), 0);
    const TransferInfo t = env.info(id);
    QVERIFY(t.isBundle);
    QCOMPARE(t.files.size(), 2);
    QCOMPARE(readFile(env.root + QStringLiteral("/incoming/Bundle/A.bin")), a);
    QCOMPARE(readFile(env.root + QStringLiteral("/incoming/Bundle/sub/B.bin")), b);
}

void TstEngineTorrent::promotionPartToTorrent()
{
    Env env(QStringLiteral("t-promote"));
    FakeTorrentBackend be;
    auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges | NetworkPlugin::CanFindSources);
    const QByteArray data = randomBytes(5 * 1000 * 1000, 38);
    const QString tid = be.makeTorrent(QStringLiteral("promo.bin"), {{QStringLiteral("promo.bin"), data}}, 128 * 1024);
    be.known[tid].peerPiecesPerTick = 1;
    be.known[tid].peers = 2;
    SourceConfig c;
    c.data = data;
    c.bytesPerSec = 400 * 1024;
    const SearchSource s = ed->addSource(QStringLiteral("ed2k://promo"), c, QStringLiteral("20.0.0.1:4662"));
    env.engineUp(&be);
    DownloadRequest r;
    r.name = QStringLiteral("promo.bin");
    r.size = data.size();
    r.hashes = hashesOf(data, 2);
    r.sources = {s};
    const QString id = env.engine->download(r);
    QTRY_VERIFY_WITH_TIMEOUT(env.info(id).done > 0 || env.engine->debugInfo(id).value(QStringLiteral("held")).toLongLong() > 600 * 1024, 10000);
    QCOMPARE(env.engine->debugInfo(id).value(QStringLiteral("store")).toString(), QStringLiteral("part"));
    FileHashes more;
    more.btih = QByteArray::fromHex(tid.toLatin1());
    ed->emitHashes(id, more);   // e.g. learned through a hash link
    QTRY_COMPARE_WITH_TIMEOUT(env.engine->debugInfo(id).value(QStringLiteral("store")).toString(), QStringLiteral("torrent"), 10000);
    QVERIFY(be.rechecks.contains(tid));
    QVERIFY2(env.waitDone(id, 60000), qPrintable(env.info(id).statusText));
    QCOMPARE(readFile(env.root + QStringLiteral("/incoming/promo.bin")), data);
    QVERIFY(!QFile::exists(env.engine->incompleteDir() + QLatin1Char('/') + id + QStringLiteral(".part")));
}

void TstEngineTorrent::pauseResumeCancelTorrent()
{
    Env env(QStringLiteral("t-ctl"));
    FakeTorrentBackend be;
    const QByteArray data = randomBytes(2 * 1024 * 1024, 39);
    const QString tid = be.makeTorrent(QStringLiteral("ctl.bin"), {{QStringLiteral("ctl.bin"), data}}, 64 * 1024);
    be.known[tid].peerPiecesPerTick = 1;
    be.known[tid].peers = 1;
    env.engineUp(&be);
    const QString id = env.engine->downloadUri(magnetFor(tid, QStringLiteral("ctl.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(env.info(id).done > 0, 10000);
    env.engine->pause(id);
    QVERIFY(be.active.value(tid).paused);
    QCOMPARE(env.info(id).state, TransferState::Paused);
    env.engineDown(true);
    env.engineUp(&be);
    QCOMPARE(env.info(id).state, TransferState::Paused);
    env.engine->resume(id);
    QTRY_VERIFY_WITH_TIMEOUT(!be.active.value(tid).paused, 3000);
    env.engine->cancel(id, true);
    QVERIFY(!be.active.contains(tid));
    QVERIFY(env.engine->downloads().isEmpty());
}

void TstEngineTorrent::metadataTimeoutFallsBackToPartFile()
{
    Env env(QStringLiteral("t-meta-fallback"));
    env.settings.setValue(QString(keys::transfers::MetadataTimeoutSec), 1);
    FakeTorrentBackend be;
    be.metadataDelayMs = 3500;   // the swarm answers late
    auto *http = env.addPlugin(Net::Http, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(4 * 1024 * 1024, 40);
    const QString tid = be.makeTorrent(QStringLiteral("late.bin"), {{QStringLiteral("late.bin"), data}}, 128 * 1024);
    be.known[tid].peerPiecesPerTick = 1;
    be.known[tid].peers = 1;
    SourceConfig c;
    c.data = data;
    c.bytesPerSec = 600 * 1024;
    const SearchSource s = http->addSource(QStringLiteral("https://mirror/late.bin"), c);
    env.engineUp(&be);
    DownloadRequest r = torrentReq(tid, QStringLiteral("late.bin"), data.size(), {s});
    r.hashes.sha1 = QCryptographicHash::hash(data, QCryptographicHash::Sha1);
    const QString id = env.engine->download(r);
    // after the timeout: a part-file download from the HTTP source ...
    QTRY_COMPARE_WITH_TIMEOUT(env.engine->debugInfo(id).value(QStringLiteral("store")).toString(), QStringLiteral("part"), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(env.engine->debugInfo(id).value(QStringLiteral("held")).toLongLong() > 0, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(env.info(id).state, TransferState::Transferring, 3000);
    // ... promoted back to the torrent when the metadata arrives
    QTRY_COMPARE_WITH_TIMEOUT(env.engine->debugInfo(id).value(QStringLiteral("store")).toString(), QStringLiteral("torrent"), 15000);
    QVERIFY(be.rechecks.contains(tid));
    QVERIFY2(env.waitDone(id, 60000), qPrintable(env.info(id).statusText));
    QCOMPARE(readFile(env.info(id).savePath), data);
}

// [engine-3] the same without a known size (bare magnet + an HTTP mirror): the part file takes the
// size from the HTTP source, and the promotion finds the torrent file by that size
void TstEngineTorrent::metadataTimeoutUnknownSize()
{
    Env env(QStringLiteral("t-meta-nosize"));
    env.settings.setValue(QString(keys::transfers::MetadataTimeoutSec), 1);
    FakeTorrentBackend be;
    be.metadataDelayMs = 3500;
    auto *http = env.addPlugin(Net::Http, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(3 * 1024 * 1024 + 17, 41);
    const QString tid = be.makeTorrent(QStringLiteral("nosize.bin"), {{QStringLiteral("nosize.bin"), data}}, 128 * 1024);
    be.known[tid].peerPiecesPerTick = 1;
    be.known[tid].peers = 1;
    SourceConfig c;
    c.data = data;
    c.bytesPerSec = 500 * 1024;
    const SearchSource s = http->addSource(QStringLiteral("https://mirror/nosize.bin"), c);
    env.engineUp(&be);
    const QString id = env.engine->download(torrentReq(tid, QStringLiteral("nosize.bin"), 0, {s}));
    QTRY_COMPARE_WITH_TIMEOUT(env.engine->debugInfo(id).value(QStringLiteral("store")).toString(), QStringLiteral("part"), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(env.engine->debugInfo(id).value(QStringLiteral("held")).toLongLong() > 0, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(env.engine->debugInfo(id).value(QStringLiteral("store")).toString(), QStringLiteral("torrent"), 15000);
    QVERIFY2(env.waitDone(id, 60000), qPrintable(env.info(id).statusText));
    QCOMPARE(readFile(env.info(id).savePath), data);
}

void TstEngineTorrent::metadataTimeoutSaysStillLooking()
{
    Env env(QStringLiteral("t-meta-stalled"));
    env.settings.setValue(QString(keys::transfers::MetadataTimeoutSec), 1);
    FakeTorrentBackend be;   // knows nothing: no metadata ever
    env.engineUp(&be);
    const QString id = env.engine->downloadUri(magnetFor(QStringLiteral("0123456789abcdef0123456789abcdef01234567"), QStringLiteral("nobody.iso")));
    QTRY_VERIFY_WITH_TIMEOUT(env.info(id).statusText.startsWith(QLatin1String("Finding sources...")), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(env.info(id).statusText.startsWith(QStringLiteral("No sources found yet, still looking")), 5000);
    QCOMPARE(env.info(id).state, TransferState::FindingSources);
    QCOMPARE(env.engine->debugInfo(id).value(QStringLiteral("store")).toString(), QStringLiteral("torrent"));
}

QTEST_GUILESS_MAIN(TstEngineTorrent)
#include "tst_engine_torrent.moc"

// [core-5] a torrent listed by an index whose metadata is not there yet reads, in the original WinMX style,
// "Fetching torrent from <index>..." (not "Connecting..." or a libtorrent detail)
void TstEngineTorrent::indexTorrentSaysFetching()
{
    Env env(QStringLiteral("t-index-fetch"));
    FakeTorrentBackend be;   // knows nothing: the metadata never comes
    env.engineUp(&be);
    const QString tid = QStringLiteral("89abcdef0123456789abcdef0123456789abcdef");
    DownloadRequest r = torrentReq(tid, QStringLiteral("Sintel.2010.1080p.mkv"), 0, {});
    r.sources.first().connection = QStringLiteral("LimeTorrents");
    r.sources.first().seeds = 12;
    r.sources.first().extra.insert(QStringLiteral("providerName"), QStringLiteral("LimeTorrents"));
    const QString id = env.engine->download(r);
    QVERIFY(!id.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(env.info(id).statusText, QStringLiteral("Fetching torrent from LimeTorrents..."), 3000);
    QCOMPARE(env.info(id).state, TransferState::FindingSources);
}
