// SPDX-License-Identifier: GPL-3.0-or-later
// [ui-i] The map of the file for the Transfers bar (TransferInfo::pieceMap / pieceFill): the condenser,
// the torrent map of a snapshot, and through the engine with fake plugins: "complete" only for
// verified bytes, "in progress" only for what is requested from a source right now, nothing in
// progress while paused, and a restored download shows its map before it is resumed.
#include <QtTest/QTest>

#include "core/engine/store.h"
#include "core/engine/torrent_store.h"
#include "engine_env.h"
#include "mxhash/ed2k.h"

using namespace mx;
using namespace mx::etest;
using mx::engine::Range;
using mx::engine::RangeSet;

namespace {

constexpr qint64 P = hash::Ed2kHasher::PartSize;

RangeSet rangesOf(const QVariant &v)
{
    return RangeSet::fromVariant(v.toList());
}

struct Cells {
    QByteArray state, fill;
};

Cells condense(int cells, const QList<Range> &spans, const RangeSet &complete, const RangeSet &progress)
{
    Cells c;
    engine::condensePieceMap(cells, spans, complete, progress, &c.state, &c.fill);
    return c;
}

// the byte range of cell i of a single-span map over [0, total)
Range cellRange(qint64 total, int n, int i)
{
    return Range(total * i / n, total * (i + 1) / n);
}

DownloadRequest req(const QString &name, const QByteArray &data, const QList<SearchSource> &src)
{
    DownloadRequest r;
    r.name = name;
    r.size = data.size();
    r.hashes = hashesOf(data, 2);
    r.sources = src;
    return r;
}

// One sample of a running part-file download: what the map says against the ranges the store and
// the scheduler hold at the same instant (info() and debugInfo() run back to back, no event in between).
struct Check {
    int complete = 0, moving = 0;
    QString error;
};

Check checkPartSample(const TransferInfo &t, const QVariantMap &dbg)
{
    Check r;
    const QByteArray &st = t.pieceMap;
    const QByteArray &fill = t.pieceFill;
    const RangeSet verified = rangesOf(dbg.value(QStringLiteral("verifiedRanges")));
    RangeSet arriving = rangesOf(dbg.value(QStringLiteral("inFlightRanges")));
    const RangeSet inFlight = arriving;
    RangeSet pending = rangesOf(dbg.value(QStringLiteral("heldRanges")));
    pending.remove(verified);
    arriving.add(pending);   // received for a unit that is still arriving / being checked
    if (st.isEmpty() || fill.size() != st.size()) {
        r.error = QStringLiteral("no map (%1 / %2 cells)").arg(st.size()).arg(fill.size());
        return r;
    }
    const int n = int(st.size());
    for (int i = 0; i < n; ++i) {
        const Range c = cellRange(t.size, n, i);
        const char s = st.at(i);
        const int f = int(quint8(fill.at(i)));
        if (s == 2) {
            ++r.complete;
            if (!verified.contains(c.first, c.second))
                r.error = QStringLiteral("cell %1 [%2, %3) drawn complete but not verified").arg(i).arg(c.first).arg(c.second);
            if (f != 255) r.error = QStringLiteral("complete cell %1 has fill %2").arg(i).arg(f);
        } else {
            if (f == 255) r.error = QStringLiteral("cell %1 not complete but fill 255").arg(i);
            if ((f > 0) != verified.intersects(c.first, c.second))
                r.error = QStringLiteral("cell %1 fill %2 disagrees with the verified ranges").arg(i).arg(f);
        }
        if (s == 1) {
            ++r.moving;
            if (!arriving.intersects(c.first, c.second))
                r.error = QStringLiteral("cell %1 drawn in progress, nothing requested or arriving there").arg(i);
            // [safe-1] a unit being checked is in progress too (PartStore::pieceMap); its check can wait
            // behind a sync of the part file while no request is out (seen under wine)
            if (inFlight.isEmpty() && dbg.value(QStringLiteral("verifyingUnits")).toInt() == 0)
                r.error = QStringLiteral("cell %1 drawn in progress while no request is out and no unit is checked").arg(i);
        }
    }
    return r;
}

} // namespace

class TstEngineMap : public QObject {
    Q_OBJECT
private slots:
    void condenseCells();
    void condenseSpans();
    void torrentMapOfSnapshot();
    void partDownloadInFlightOnly();
    void partDownloadRestoredMap();
    void torrentDownloadQueueAndPause();
};

void TstEngineMap::condenseCells()
{
    // 10 cells of 100 bytes
    RangeSet complete(0, 100);
    complete.add(150, 160);                 // 10 % of cell 1
    complete.add(300, 399);                 // cell 3 lacks one byte
    complete.add(500, 501);                 // one byte of cell 5
    RangeSet progress(250, 251);            // one byte of cell 2 in flight
    progress.add(50, 60);                   // in flight inside a complete cell: complete wins
    progress.add(395, 400);                 // the missing byte of cell 3 arrives
    const Cells c = condense(10, {Range(0, 1000)}, complete, progress);
    QCOMPARE(c.state.size(), 10);
    QCOMPARE(c.fill.size(), 10);
    QCOMPARE(int(c.state[0]), 2);
    QCOMPARE(int(quint8(c.fill[0])), 255);
    QCOMPARE(int(c.state[1]), 0);
    QCOMPARE(int(quint8(c.fill[1])), 26);   // 10 of 100 bytes
    QCOMPARE(int(c.state[2]), 1);
    QCOMPARE(int(quint8(c.fill[2])), 0);
    QCOMPARE(int(c.state[3]), 1);
    QCOMPARE(int(quint8(c.fill[3])), 252);  // 99 of 100 bytes
    QCOMPARE(int(c.state[5]), 0);
    QCOMPARE(int(quint8(c.fill[5])), 3);    // round(255 / 100)
    for (int i : {4, 6, 7, 8, 9}) {
        QCOMPARE(int(c.state[i]), 0);
        QCOMPARE(int(quint8(c.fill[i])), 0);
    }
    // one verified byte of a huge cell still shows (fill 1), one missing byte keeps it below 255
    const Cells big = condense(1, {Range(0, 4000000000LL)}, RangeSet(0, 1), RangeSet());
    QCOMPARE(int(quint8(big.fill[0])), 1);
    const Cells most = condense(1, {Range(0, 4000000000LL)}, RangeSet(1, 4000000000LL), RangeSet());
    QCOMPARE(int(quint8(most.fill[0])), 254);
    QCOMPARE(int(most.state[0]), 0);
    // fewer bytes than cells: one cell per byte; nothing for an empty file
    QCOMPARE(condense(512, {Range(0, 7)}, RangeSet(0, 7), RangeSet()).state, QByteArray(7, char(2)));
    QVERIFY(condense(512, {Range(0, 0)}, RangeSet(), RangeSet()).state.isEmpty());
    QVERIFY(condense(0, {Range(0, 100)}, RangeSet(), RangeSet()).state.isEmpty());
    // the 512-cell maps the engine reports
    QCOMPARE(condense(512, {Range(0, 3 * P)}, RangeSet(), RangeSet()).state.size(), 512);
}

void TstEngineMap::condenseSpans()
{
    // a bundle: wanted files [0, 100) and [300, 400), the unwanted one between them is not on the map
    const QList<Range> spans{Range(0, 100), Range(300, 400)};
    RangeSet complete(0, 100);
    complete.add(100, 300);                 // bytes of the unwanted file never count
    const Cells c = condense(2, spans, complete, RangeSet(350, 351));
    QCOMPARE(c.state.size(), 2);
    QCOMPARE(int(c.state[0]), 2);
    QCOMPARE(int(c.state[1]), 1);
    QCOMPARE(int(quint8(c.fill[1])), 0);
    // 4 cells over the 200 bytes of the map: axis [100, 150) = bytes [300, 350)
    const Cells x = condense(4, spans, RangeSet(300, 350), RangeSet(150, 200));
    QCOMPARE(int(x.state.size()), 4);
    QVERIFY(!x.state.contains(char(1)));    // the progress at [150, 200) is outside the map
    QCOMPARE(int(x.state[1]), 0);
    QCOMPARE(int(x.state[2]), 2);           // axis [100, 150) = bytes [300, 350)
    QCOMPARE(int(x.state[3]), 0);
}

void TstEngineMap::torrentMapOfSnapshot()
{
    TorrentSnapshot sn;
    sn.hasMetadata = true;
    sn.pieceLength = 100;
    sn.numPieces = 10;
    sn.have = QBitArray(10);
    sn.downloading = QBitArray(10);
    // files: a [0, 400) wanted, .pad/0 [400, 450), b [450, 700) not wanted, c [700, 1000) wanted
    sn.files = {TorrentFile{QStringLiteral("x/a.bin"), 400, 0, 4, 0}, TorrentFile{QStringLiteral(".pad/0"), 50, 400, 0, 0},
                TorrentFile{QStringLiteral("x/b.bin"), 250, 450, 0, 0}, TorrentFile{QStringLiteral("x/c.bin"), 300, 700, 4, 0}};
    sn.have.setBit(0);
    sn.have.setBit(1);
    sn.have.setBit(5);                       // inside the unwanted file: not on the map
    sn.have.setBit(9);
    sn.downloading.setBit(2);
    QByteArray st, fill;
    engine::TorrentStore::mapOf(sn, {}, 7, &st, &fill);   // 700 wanted bytes, 100 per cell
    QCOMPARE(st.size(), 7);
    QCOMPARE(QString::fromLatin1(st.toHex()), QStringLiteral("02020100000002"));
    // a piece fetched from another network right now
    engine::TorrentStore::mapOf(sn, {7}, 7, &st, &fill);   // piece 7 = bytes [700, 800) = axis [400, 500) = cell 4
    QCOMPARE(int(st[4]), 1);
    QCOMPARE(int(st[5]), 0);
    engine::TorrentStore::mapOf(sn, {8}, 7, &st, &fill);
    QCOMPARE(int(st[4]), 0);
    QCOMPARE(int(st[5]), 1);
    // nothing arriving: the reported queue is empty (paused)
    sn.downloading = QBitArray();
    engine::TorrentStore::mapOf(sn, {}, 7, &st, &fill);
    QVERIFY(!st.contains(char(1)));
    // no metadata: no map
    sn.hasMetadata = false;
    engine::TorrentStore::mapOf(sn, {}, 7, &st, &fill);
    QVERIFY(st.isEmpty());
}

void TstEngineMap::partDownloadInFlightOnly()
{
    Env env(QStringLiteral("map-part"));
    auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges | NetworkPlugin::CanFindSources);
    const QByteArray data = randomBytes(3 * P + 12345, 41);
    ed->hashSetOnFind << qMakePair(P, ed2kParts(data));
    SourceConfig c;
    c.data = data;
    c.bytesPerSec = 3 << 20;
    c.maxPending = 1;
    c.pref = 256 * 1024;
    const SearchSource s = ed->addSource(QStringLiteral("ed2k://m"), c, QStringLiteral("5.5.5.5:4662"));
    env.engineUp();
    const QString id = env.engine->download(req(QStringLiteral("m.bin"), data, {s}));
    QVERIFY(QTest::qWaitFor([&] { return env.info(id).state == TransferState::Transferring; }, 20000));
    int samples = 0, moving = 0, complete = 0;
    while (env.info(id).done < 2 * P && samples < 400) {
        QTest::qWait(30);
        const TransferInfo t = env.info(id);
        const QVariantMap dbg = env.engine->debugInfo(id);
        if (t.state != TransferState::Transferring) continue;
        const Check k = checkPartSample(t, dbg);
        QVERIFY2(k.error.isEmpty(), qPrintable(k.error));
        moving += k.moving > 0;
        complete += k.complete > 0;
        ++samples;
    }
    QVERIFY2(moving > 0, "no sample showed a part in progress");
    QVERIFY2(complete > 0, "no sample showed a verified part");
    // paused: what is there stays, nothing is in progress
    const QByteArray before = env.info(id).pieceMap;
    env.engine->pause(id);
    QVERIFY(env.waitState(id, TransferState::Paused, 5000));
    const TransferInfo p = env.info(id);
    QVERIFY(!p.pieceMap.isEmpty());
    QVERIFY2(!p.pieceMap.contains(char(1)), "a paused download shows parts in progress");
    for (int i = 0; i < before.size(); ++i)
        if (before.at(i) == 2) QCOMPARE(int(p.pieceMap.at(i)), 2);
    QCOMPARE(env.engine->debugInfo(id).value(QStringLiteral("inFlightRanges")).toList().size(), 0);
    env.engine->resume(id);
    QVERIFY2(env.waitDone(id, 30000), qPrintable(env.info(id).statusText));
    QVERIFY(env.info(id).pieceMap.isEmpty());   // finished: the row says Complete
}

void TstEngineMap::partDownloadRestoredMap()
{
    Env env(QStringLiteral("map-restore"));
    auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges | NetworkPlugin::CanFindSources);
    const QByteArray data = randomBytes(4 * P + 777, 42);
    ed->hashSetOnFind << qMakePair(P, ed2kParts(data));
    SourceConfig c;
    c.data = data;
    c.bytesPerSec = 6 << 20;
    c.pref = 512 * 1024;
    const SearchSource s = ed->addSource(QStringLiteral("ed2k://rm"), c, QStringLiteral("6.6.6.6:4662"));
    env.engineUp();
    const QString id = env.engine->download(req(QStringLiteral("rm.bin"), data, {s}));
    QVERIFY(QTest::qWaitFor([&] { return env.info(id).done >= 2 * P; }, 30000));
    env.engine->pause(id);
    QVERIFY(env.waitState(id, TransferState::Paused, 5000));
    const TransferInfo before = env.info(id);
    const RangeSet verified = rangesOf(env.engine->debugInfo(id).value(QStringLiteral("verifiedRanges")));
    QVERIFY(verified.total() >= 2 * P);
    env.engineDown(true);
    env.engineUp();
    const TransferInfo t = env.info(id);
    QCOMPARE(t.state, TransferState::Paused);
    QCOMPARE(env.engine->debugInfo(id).value(QStringLiteral("store")).toString(), QStringLiteral("none"));   // not resumed
    QVERIFY2(!t.pieceMap.isEmpty(), "a restored, paused download has no map");
    QCOMPARE(t.pieceMap, before.pieceMap);
    QCOMPARE(t.pieceFill, before.pieceFill);
    QVERIFY(!t.pieceMap.contains(char(1)));
    for (int i = 0; i < t.pieceMap.size(); ++i) {
        const Range r = cellRange(t.size, int(t.pieceMap.size()), i);
        QCOMPARE(t.pieceMap.at(i) == 2, verified.contains(r.first, r.second));
    }
    env.engine->resume(id);
    QVERIFY2(env.waitDone(id, 30000), qPrintable(env.info(id).statusText));
    QCOMPARE(readFile(env.info(id).savePath), data);
}

void TstEngineMap::torrentDownloadQueueAndPause()
{
    Env env(QStringLiteral("map-torrent"));
    FakeTorrentBackend be;
    const QByteArray a = randomBytes(1024 * 1024, 43), b = randomBytes(512 * 1024 + 99, 44);
    const QString tid = be.makeTorrent(QStringLiteral("two"), {{QStringLiteral("two/a.bin"), a}, {QStringLiteral("two/b.bin"), b}}, 64 * 1024);
    be.known[tid].peerPiecesPerTick = 1;
    be.known[tid].peers = 2;
    // [api-2] the test makes the simulated peers' steps (no timer): the fake reports a finished piece
    // (torrentChanged) before it fills the next step's download queue, so a map read against a timer saw
    // the queue only between the engine's 1 s refresh and the next step, and missed it under load
    be.setManualSteps(true);
    env.engineUp(&be);
    const QString id = env.engine->downloadUri(QStringLiteral("magnet:?xt=urn:btih:%1&dn=two").arg(tid));
    QVERIFY(!id.isEmpty());
    // one step of the peers, then the snapshot as the backend reports it now
    auto step = [&] {
        be.stepOnce();
        emit be.events->torrentChanged(tid);
        QCoreApplication::processEvents();
    };
    QVERIFY(QTest::qWaitFor([&] {
        if (env.info(id).state == TransferState::Transferring) step();
        return env.info(id).state == TransferState::Transferring && env.info(id).done > 0;
    }, 10000));
    // every cell in progress is a piece in the (fake) download queue, every complete cell a piece it has
    int moving = 0;
    for (int k = 0; k < 6 && env.info(id).state == TransferState::Transferring; ++k) {
        step();
        const TransferInfo t = env.info(id);
        const QVariantMap dbg = env.engine->debugInfo(id);
        if (t.pieceMap.isEmpty()) continue;
        const qint64 L = dbg.value(QStringLiteral("pieceLength")).toLongLong();
        QVERIFY(L > 0);
        RangeSet have = rangesOf(dbg.value(QStringLiteral("heldRanges")));
        RangeSet queue;
        for (const QVariant &v : dbg.value(QStringLiteral("downloadingPieces")).toList())
            queue.add(v.toLongLong() * L, (v.toLongLong() + 1) * L);
        const int n = int(t.pieceMap.size());
        QCOMPARE(t.size, qint64(a.size() + b.size()));
        for (int i = 0; i < n; ++i) {
            const Range r = cellRange(t.size, n, i);
            if (t.pieceMap.at(i) == 2) QVERIFY2(have.contains(r.first, r.second), "complete cell without the pieces");
            if (t.pieceMap.at(i) == 1) {
                ++moving;
                QVERIFY2(queue.intersects(r.first, r.second), "cell in progress outside the download queue");
            }
        }
        // a piece of the queue that is not held yet shows as a cell in progress in this very read
        RangeSet arriving = queue;
        arriving.remove(have);
        if (!arriving.isEmpty()) QVERIFY2(t.pieceMap.contains(char(1)), "the download queue has a piece, the map shows none");
    }
    QVERIFY(moving > 0);
    // paused (and restored paused): what libtorrent has, nothing arriving
    env.engine->pause(id);
    QVERIFY(env.waitState(id, TransferState::Paused, 5000));
    step();   // the peers send nothing to a paused torrent; its snapshot is read again
    const TransferInfo p = env.info(id);
    QVERIFY(!p.pieceMap.isEmpty());
    QVERIFY2(!p.pieceMap.contains(char(1)), "a paused torrent shows parts in progress");
    env.engineDown(true);
    env.engineUp(&be);
    const TransferInfo r = env.info(id);
    QCOMPARE(r.state, TransferState::Paused);
    QVERIFY2(!r.pieceMap.isEmpty(), "a restored, paused torrent has no map");
    QVERIFY(!r.pieceMap.contains(char(1)));
    QCOMPARE(r.pieceMap.count(char(2)), p.pieceMap.count(char(2)));
    env.engine->resume(id);
    QVERIFY2(QTest::qWaitFor([&] {
        step();
        return env.info(id).state == TransferState::Completed;
    }, 30000), qPrintable(env.info(id).statusText));
}

QTEST_GUILESS_MAIN(TstEngineMap)
#include "tst_engine_map.moc"
