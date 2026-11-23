// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX engine tests [eng-1]: a plain-file download (eD2k identity, part file) joined with torrents
// that hold the same file (matched by name and size). Which torrent is tried first (the swarm the
// search reported), a torrent that never gives its file list (announced again, set aside, the next
// candidate), metadata that arrives late, a torrent whose file is another one (dropped), a restart
// while the file list was still missing (tried again), and the BitTorrent line of the Transfers row
// in every one of these states.
#include <QtTest/QtTest>

#include "engine_env.h"

using namespace mx;
using namespace mx::etest;

namespace {

QString dump(const TransferInfo &t)
{
    QStringList l;
    for (const TransferSource &s : t.sources) l << QStringLiteral("[%1|%2|%3]").arg(netShortName(s.net), s.user.name, s.statusText);
    return t.statusText + QStringLiteral(" :: ") + l.join(QLatin1Char(' '));
}

// the BitTorrent line of the row (the first source row of that network)
QString btLine(const TransferInfo &t)
{
    for (const TransferSource &s : t.sources)
        if (s.net == Net::BitTorrent) return s.statusText;
    return QStringLiteral("<no BitTorrent row>");
}

SearchSource btCandidate(const QString &tid, const QString &name, int seeds, int peers, const QString &folder = QString())
{
    SearchSource bt;
    bt.net = Net::BitTorrent;
    bt.uri = QStringLiteral("magnet:?xt=urn:btih:%1&dn=%2").arg(tid, name);
    bt.seeds = seeds;
    bt.peers = peers;
    FileHashes h;
    h.btih = QByteArray::fromHex(tid.toLatin1());
    bt.extra.insert(QStringLiteral("candidate"), true);
    bt.extra.insert(QStringLiteral("hashes"), h.toVariant());
    if (!folder.isEmpty()) bt.extra.insert(QStringLiteral("insideBundle"), folder);
    return bt;
}

QString str(const QVariantMap &m, const char *k) { return m.value(QLatin1String(k)).toString(); }

} // namespace

class TstEngineJoin : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { registerMetaTypes(); }
    void largerSwarmFirst();
    void neverGivesFileListNextCandidate();
    void metadataLateSingleCandidate();
    void onlyCandidateIsAnotherFile();
    void restartTriesAgain();
    void deadIdentityTorrentGivesWayQuickly();
};

// The live case of 23 Nov: the row's own identity is the dead single-file torrent (row key btih), the
// eD2k copy and the folder torrent are candidates of it. With a better torrent known, the dead one
// gets a third of the metadata timeout, the part file runs on the eD2k copy's hashes, and the folder
// torrent is asked at once (not after another full timeout).
void TstEngineJoin::deadIdentityTorrentGivesWayQuickly()
{
    FakeTorrentBackend be;
    Env env(QStringLiteral("j-deadid"));
    env.settings.setValue(QString(keys::transfers::MetadataTimeoutSec), 9);
    auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(4 * 1000 * 1000, 609);
    const QString dead = be.makeTorrent(QStringLiteral("deadid.mp4"), {{QStringLiteral("deadid.mp4"), randomBytes(4 * 1000 * 1000, 610)}}, 128 * 1024);
    be.known.remove(dead);
    const QString good = be.makeTorrent(QStringLiteral("Folder"), {{QStringLiteral("deadid.mp4"), data}}, 128 * 1024);
    be.known[good].peers = 2;
    be.known[good].peerPiecesPerTick = 1;
    SourceConfig c;
    c.data = data;
    c.bytesPerSec = 60 * 1024;
    SearchSource e = ed->addSource(QStringLiteral("ed2k-peer://6.6.0.1:4662"), c, QStringLiteral("edpeer"));
    e.extra.insert(QStringLiteral("candidate"), true);
    e.extra.insert(QStringLiteral("hashes"), hashesOf(data, 2).toVariant());
    SearchSource own;
    own.net = Net::BitTorrent;
    own.uri = QStringLiteral("magnet:?xt=urn:btih:%1&dn=deadid.mp4").arg(dead);
    own.seeds = own.peers = 0;
    env.engineUp(&be);
    DownloadRequest r;
    r.name = QStringLiteral("deadid.mp4");
    r.size = data.size();
    r.hashes.btih = QByteArray::fromHex(dead.toLatin1());
    r.uri = own.uri;
    r.sources = {own, e, btCandidate(good, QStringLiteral("deadid.mp4"), 418, 97, QStringLiteral("Folder"))};
    QElapsedTimer t;
    t.start();
    const QString id = env.engine->download(r);
    QTRY_VERIFY2_WITH_TIMEOUT(btLine(env.info(id)).startsWith(QLatin1String("No peers for this torrent yet, trying another torrent in")),
                              qPrintable(dump(env.info(id))), 3000);
    QTRY_VERIFY2_WITH_TIMEOUT(str(env.engine->debugInfo(id), "promoteTorrent") == good || str(env.engine->debugInfo(id), "torrentId") == good,
                              qPrintable(dump(env.info(id))), 15000);
    QVERIFY2(t.elapsed() < 7000, qPrintable(QString::number(t.elapsed())));   // 9 s / 3 + a tick, not 9 + 9
    QTRY_COMPARE_WITH_TIMEOUT(str(env.engine->debugInfo(id), "torrentId"), good, 20000);
    QVERIFY2(env.waitDone(id, 60000), qPrintable(dump(env.info(id))));
    QCOMPARE(readFile(env.info(id).savePath), data);
}

// The owner's row "Big Buck Bunny.mp4": a single-file torrent that an index reported with no seeds
// comes first in the row, the folder torrent holding the same file with 500 peers second. The
// folder is asked first; the dead one is never needed. The row has a BitTorrent line and the
// Network column says BitTorrent from the start.
void TstEngineJoin::largerSwarmFirst()
{
    FakeTorrentBackend be;   // outlives the engine
    Env env(QStringLiteral("j-swarm"));
    auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(4 * 1000 * 1000, 601);
    const QString dead = be.makeTorrent(QStringLiteral("swarm.mp4"), {{QStringLiteral("swarm.mp4"), randomBytes(4 * 1000 * 1000, 602)}}, 128 * 1024);
    be.known.remove(dead);   // nobody has it
    const QString good = be.makeTorrent(QStringLiteral("Folder"), {{QStringLiteral("swarm.mp4"), data}}, 128 * 1024);
    be.known[good].peers = 3;
    be.known[good].peerPiecesPerTick = 1;
    be.metadataDelayMs = 1500;
    SourceConfig c;
    c.data = data;
    c.bytesPerSec = 100 * 1024;
    const SearchSource s = ed->addSource(QStringLiteral("ed2k-peer://6.1.0.1:4662"), c, QStringLiteral("edpeer"));
    env.engineUp(&be);
    DownloadRequest r;
    r.name = QStringLiteral("swarm.mp4");
    r.size = data.size();
    r.hashes = hashesOf(data, 2);
    r.sources = {s, btCandidate(dead, QStringLiteral("swarm.mp4"), 0, 0), btCandidate(good, QStringLiteral("swarm.mp4"), 418, 97, QStringLiteral("Folder"))};
    const QString id = env.engine->download(r);
    QTRY_VERIFY2_WITH_TIMEOUT(str(env.engine->debugInfo(id), "promoteTorrent") == good, qPrintable(dump(env.info(id))), 5000);
    const TransferInfo t = env.info(id);
    QVERIFY2(t.networks.contains(Net::BitTorrent), qPrintable(dump(t)));
    QVERIFY2(btLine(t).startsWith(QLatin1String("Getting the torrent's file list")) || btLine(t).startsWith(QLatin1String("No peers")), qPrintable(dump(t)));
    QVERIFY2(dump(t).contains(QLatin1String("Torrent \"Folder\"")), qPrintable(dump(t)));
    QTRY_COMPARE_WITH_TIMEOUT(str(env.engine->debugInfo(id), "torrentId"), good, 20000);
    QVERIFY(!be.active.contains(dead));
    QVERIFY2(env.waitDone(id, 60000), qPrintable(dump(env.info(id))));
    QCOMPARE(readFile(env.info(id).savePath), data);
}

// The torrent tried first never gives its file list (the index's figure was wrong): it is announced
// again, the row says so, after transfers/metadataTimeoutSec it is set aside and the second
// candidate takes over; the eD2k copy delivers all along.
void TstEngineJoin::neverGivesFileListNextCandidate()
{
    FakeTorrentBackend be;
    Env env(QStringLiteral("j-never"));
    env.settings.setValue(QString(keys::transfers::MetadataTimeoutSec), 3);
    env.tuning.afsMinuteMs = 500;   // announce again every second
    auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(4 * 1000 * 1000, 603);
    const QString dead = be.makeTorrent(QStringLiteral("never.mp4"), {{QStringLiteral("never.mp4"), randomBytes(4 * 1000 * 1000, 604)}}, 128 * 1024);
    be.known.remove(dead);
    const QString good = be.makeTorrent(QStringLiteral("never.mp4 "), {{QStringLiteral("never.mp4"), data}}, 128 * 1024);
    be.known[good].peers = 2;
    be.known[good].peerPiecesPerTick = 1;
    SourceConfig c;
    c.data = data;
    c.bytesPerSec = 60 * 1024;
    const SearchSource s = ed->addSource(QStringLiteral("ed2k-peer://6.2.0.1:4662"), c, QStringLiteral("edpeer"));
    env.engineUp(&be);
    DownloadRequest r;
    r.name = QStringLiteral("never.mp4");
    r.size = data.size();
    r.hashes = hashesOf(data, 2);
    r.sources = {s, btCandidate(dead, QStringLiteral("never.mp4"), 50, 10), btCandidate(good, QStringLiteral("never.mp4"), -1, -1)};
    const QString id = env.engine->download(r);
    QTRY_VERIFY2_WITH_TIMEOUT(str(env.engine->debugInfo(id), "promoteTorrent") == dead, qPrintable(dump(env.info(id))), 5000);
    QTRY_VERIFY2_WITH_TIMEOUT(btLine(env.info(id)).startsWith(QLatin1String("No peers for this torrent yet, trying another torrent in")),
                              qPrintable(dump(env.info(id))), 3000);
    QTRY_VERIFY2_WITH_TIMEOUT(be.reannounces.contains(dead), qPrintable(be.reannounces.join(QLatin1Char(','))), 5000);
    QTRY_VERIFY2_WITH_TIMEOUT(str(env.engine->debugInfo(id), "promoteTorrent") == good || str(env.engine->debugInfo(id), "torrentId") == good,
                              qPrintable(dump(env.info(id))), 10000);
    QVERIFY(!be.active.contains(dead));   // set aside: removed from the backend meanwhile
    QVERIFY(env.engine->debugInfo(id).value(QStringLiteral("btCandidateList")).toStringList().contains(dead));
    QTRY_COMPARE_WITH_TIMEOUT(str(env.engine->debugInfo(id), "torrentId"), good, 20000);
    QVERIFY2(env.waitDone(id, 60000), qPrintable(dump(env.info(id))));
    QCOMPARE(readFile(env.info(id).savePath), data);
}

// The only torrent gives its file list late: it is kept (nothing else to try), announced again,
// the row says when; the part file runs on eD2k meanwhile; then it joins.
void TstEngineJoin::metadataLateSingleCandidate()
{
    FakeTorrentBackend be;
    Env env(QStringLiteral("j-late"));
    env.settings.setValue(QString(keys::transfers::MetadataTimeoutSec), 1);
    env.tuning.afsMinuteMs = 500;
    auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(4 * 1000 * 1000, 605);
    const QString tid = be.makeTorrent(QStringLiteral("late.mp4"), {{QStringLiteral("late.mp4"), data}}, 128 * 1024);
    const FakeTorrent keep = be.known.take(tid);   // the swarm answers only later
    SourceConfig c;
    c.data = data;
    c.bytesPerSec = 60 * 1024;
    const SearchSource s = ed->addSource(QStringLiteral("ed2k-peer://6.3.0.1:4662"), c, QStringLiteral("edpeer"));
    env.engineUp(&be);
    DownloadRequest r;
    r.name = QStringLiteral("late.mp4");
    r.size = data.size();
    r.hashes = hashesOf(data, 2);
    r.sources = {s, btCandidate(tid, QStringLiteral("late.mp4"), 5, 5)};
    const QString id = env.engine->download(r);
    QTRY_VERIFY2_WITH_TIMEOUT(str(env.engine->debugInfo(id), "promoteTorrent") == tid, qPrintable(dump(env.info(id))), 5000);
    QTRY_VERIFY2_WITH_TIMEOUT(btLine(env.info(id)).startsWith(QLatin1String("No peers for this torrent yet, trying again in")),
                              qPrintable(dump(env.info(id))), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(be.reannounces.contains(tid), 5000);
    QTest::qWait(1500);
    QCOMPARE(str(env.engine->debugInfo(id), "promoteTorrent"), tid);   // nothing else to try: kept
    QVERIFY(env.engine->debugInfo(id).value(QStringLiteral("held")).toLongLong() > 0);   // eD2k delivers meanwhile
    be.known.insert(tid, keep);
    be.known[tid].peers = 2;
    be.known[tid].peerPiecesPerTick = 1;
    be.giveMetadata(tid);
    QTRY_COMPARE_WITH_TIMEOUT(str(env.engine->debugInfo(id), "torrentId"), tid, 20000);
    QTRY_VERIFY2_WITH_TIMEOUT(btLine(env.info(id)).contains(QLatin1String("connected")), qPrintable(dump(env.info(id))), 5000);
    QVERIFY2(env.waitDone(id, 60000), qPrintable(dump(env.info(id))));
    QCOMPARE(readFile(env.info(id).savePath), data);
}

// The only torrent holds another file of that name and size: a held piece does not match, it is
// dropped, the row says so in plain words and BitTorrent leaves the Network column.
void TstEngineJoin::onlyCandidateIsAnotherFile()
{
    FakeTorrentBackend be;
    Env env(QStringLiteral("j-wrong"));
    auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(3 * 1000 * 1000, 606);
    const QString tid = be.makeTorrent(QStringLiteral("wrong.mp4"), {{QStringLiteral("wrong.mp4"), randomBytes(3 * 1000 * 1000, 607)}}, 128 * 1024);
    be.metadataDelayMs = 2500;   // the part file holds a whole piece by then
    SourceConfig c;
    c.data = data;
    c.bytesPerSec = 300 * 1024;
    const SearchSource s = ed->addSource(QStringLiteral("ed2k-peer://6.4.0.1:4662"), c, QStringLiteral("edpeer"));
    env.engineUp(&be);
    DownloadRequest r;
    r.name = QStringLiteral("wrong.mp4");
    r.size = data.size();
    r.hashes = hashesOf(data, 2);
    r.sources = {s, btCandidate(tid, QStringLiteral("wrong.mp4"), 3, 3)};
    const QString id = env.engine->download(r);
    QTRY_VERIFY2_WITH_TIMEOUT(dump(env.info(id)).contains(QLatin1String("different file: dropped")), qPrintable(dump(env.info(id))), 15000);
    QVERIFY2(!env.info(id).networks.contains(Net::BitTorrent), qPrintable(dump(env.info(id))));
    QVERIFY(!be.active.contains(tid));
    QVERIFY2(env.waitDone(id, 60000), qPrintable(dump(env.info(id))));
    QCOMPARE(readFile(env.info(id).savePath), data);
}

// The program is closed while the torrent has given no file list yet; after the restart the
// download asks for it again at once (the backend restored the torrent: before, the download took
// it for someone else's and never asked again), and it joins once the swarm answers.
void TstEngineJoin::restartTriesAgain()
{
    FakeTorrentBackend be;
    Env env(QStringLiteral("j-restart"));
    auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(4 * 1000 * 1000, 608);
    const QString tid = be.makeTorrent(QStringLiteral("again.mp4"), {{QStringLiteral("again.mp4"), data}}, 128 * 1024);
    const FakeTorrent keep = be.known.take(tid);
    SourceConfig c;
    c.data = data;
    c.bytesPerSec = 40 * 1024;
    const SearchSource s = ed->addSource(QStringLiteral("ed2k-peer://6.5.0.1:4662"), c, QStringLiteral("edpeer"));
    env.engineUp(&be);
    DownloadRequest r;
    r.name = QStringLiteral("again.mp4");
    r.size = data.size();
    r.hashes = hashesOf(data, 2);
    r.sources = {s, btCandidate(tid, QStringLiteral("again.mp4"), 4, 4)};
    const QString id = env.engine->download(r);
    QTRY_VERIFY2_WITH_TIMEOUT(str(env.engine->debugInfo(id), "promoteTorrent") == tid, qPrintable(dump(env.info(id))), 5000);
    QTest::qWait(800);   // persisted
    env.engineDown(true);
    QVERIFY(be.active.contains(tid));   // the backend keeps (restores) its torrent
    be.reannounces.clear();
    env.engineUp(&be);
    QTRY_VERIFY2_WITH_TIMEOUT(str(env.engine->debugInfo(id), "promoteTorrent") == tid, qPrintable(dump(env.info(id))), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(be.reannounces.contains(tid), 3000);
    QVERIFY2(env.info(id).networks.contains(Net::BitTorrent), qPrintable(dump(env.info(id))));
    be.known.insert(tid, keep);
    be.known[tid].peers = 2;
    be.known[tid].peerPiecesPerTick = 1;
    be.giveMetadata(tid);
    QTRY_COMPARE_WITH_TIMEOUT(str(env.engine->debugInfo(id), "torrentId"), tid, 20000);
    QVERIFY2(env.waitDone(id, 60000), qPrintable(dump(env.info(id))));
    QCOMPARE(readFile(env.info(id).savePath), data);
}

QTEST_GUILESS_MAIN(TstEngineJoin)
#include "tst_engine_join.moc"
