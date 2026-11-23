// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX: BitTorrent plugin: the multi-network hook (reservePieces / injectPiece).
#include "bt_test_util.h"

using namespace bttest;

namespace {
constexpr int kPiece = 256 * 1024;

struct Fixture {
    QString root, src;
    QByteArray torrent;
    QString id;
    int numPieces = 0;
    qint64 size = 0;
    void make(const QString &name, qint64 sz, int pieceSize = kPiece, quint32 seed = 7)
    {
        root = freshDir(name);
        src = root + QStringLiteral("/share/payload.bin");
        size = sz;
        makeFile(src, sz, seed);
        torrent = makeTorrent(src, pieceSize);
        id = infoHashOf(torrent);
        numPieces = int((sz + pieceSize - 1) / pieceSize);
    }
    QByteArray piece(int i, int pieceSize = kPiece) const { return readRange(src, qint64(i) * pieceSize, pieceSize); }
};

QString dumpSnap(const mx::TorrentSnapshot &s)
{
    return QStringLiteral("state=%1 '%2' have=%3/%4 done=%5/%6 peers=%7 seeds=%8 finished=%9")
        .arg(int(s.state)).arg(s.statusText).arg(s.have.count(true)).arg(s.numPieces).arg(s.totalDone).arg(s.totalSize)
        .arg(s.peers).arg(s.seeds).arg(s.finished);
}
} // namespace

class TstBtInject : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { mx::registerMetaTypes(); }
    void injectEverythingWithoutPeers();
    void reserveHalfPeerDeliversRest();
    void unreserveHandsBackToPicker();
    void injectWhilePeerIsMidPiece();
};

void TstBtInject::injectEverythingWithoutPeers()
{
    Fixture fx;
    fx.make(QStringLiteral("inject1"), 24 * 1024 * 1024 + 4321);
    Node c(fx.root + QStringLiteral("/c"));
    c.create();
    QVERIFY(c.start());
    QString err;
    const QString dl = fx.root + QStringLiteral("/c-dl");
    QCOMPARE(c.be->addTorrent(QString(), fx.torrent, dl, /*paused*/ true, {}, &err), fx.id);
    QVERIFY(QTest::qWaitFor([&] { return c.added.contains(fx.id); }, 5000));
    QCOMPARE(c.snap(fx.id).numPieces, fx.numPieces);

    // pieceHash() == SHA-1 of the original data, for every piece
    for (int i = 0; i < fx.numPieces; ++i)
        QCOMPARE(c.be->pieceHash(fx.id, i), QCryptographicHash::hash(fx.piece(i), QCryptographicHash::Sha1));

    // injection works while paused
    for (int i = 0; i < 5; ++i)
        c.be->injectPiece(fx.id, i, fx.piece(i));
    QVERIFY(QTest::qWaitFor([&] { return c.done[fx.id].size() >= 5; }, 10000));
    QCOMPARE(QSet<int>(c.done[fx.id].cbegin(), c.done[fx.id].cend()), (QSet<int>{0, 1, 2, 3, 4}));

    // a corrupted piece → torrentPieceFailed, nothing written
    QByteArray bad = fx.piece(5);
    bad[1000] = char(bad[1000] ^ 0x55);
    c.be->injectPiece(fx.id, 5, bad);
    QVERIFY(QTest::qWaitFor([&] { return c.failed[fx.id].contains(5); }, 10000));
    QVERIFY(!c.done[fx.id].contains(5));
    // wrong size → failed
    c.be->injectPiece(fx.id, 6, fx.piece(6).left(1000));
    QVERIFY(QTest::qWaitFor([&] { return c.failed[fx.id].contains(6); }, 10000));
    // unknown torrent → failed
    c.be->injectPiece(QStringLiteral("00112233445566778899aabbccddeeff00112233"), 0, fx.piece(0));

    // resume (still no peers) and inject the rest, including a correct piece 5
    c.be->resumeTorrent(fx.id);
    for (int i = 5; i < fx.numPieces; ++i) {
        const QByteArray d = fx.piece(i);
        QCOMPARE(d.size(), i == fx.numPieces - 1 ? int(fx.size - qint64(i) * kPiece) : kPiece);
        c.be->injectPiece(fx.id, i, d);
    }
    QVERIFY2(QTest::qWaitFor([&] { return c.finished.contains(fx.id); }, 60000), qPrintable(dumpSnap(c.snap(fx.id))));
    const mx::TorrentSnapshot s = c.snap(fx.id);
    QVERIFY(s.finished);
    QCOMPARE(s.totalDone, fx.size);
    QCOMPARE(s.have.count(true), fx.numPieces);
    QCOMPARE(c.failed[fx.id].size(), 2);
    QCOMPARE(sha1File(dl + QStringLiteral("/payload.bin")), sha1File(fx.src));

    // injecting a piece libtorrent already has: no-op, torrentPieceFinished re-emitted
    const int before = c.done[fx.id].count(0);
    c.be->injectPiece(fx.id, 0, fx.piece(0));
    QVERIFY(QTest::qWaitFor([&] { return c.done[fx.id].count(0) == before + 1; }, 5000));
    QCOMPARE(c.failed[fx.id].size(), 2);
    QTest::qWait(1500);
    QCOMPARE(sha1File(dl + QStringLiteral("/payload.bin")), sha1File(fx.src));
    QVERIFY(c.snap(fx.id).finished);
}

void TstBtInject::reserveHalfPeerDeliversRest()
{
    Fixture fx;
    fx.make(QStringLiteral("inject2"), 20 * 1024 * 1024 + 999, kPiece, 8);
    Node a(fx.root + QStringLiteral("/a")), b(fx.root + QStringLiteral("/b"));
    a.create();
    b.create();
    QVERIFY(a.start());
    QVERIFY(b.start());
    QString err;
    QCOMPARE(a.be->addTorrent(QString(), fx.torrent, fx.root + QStringLiteral("/share"), false, {}, &err), fx.id);
    QVERIFY(QTest::qWaitFor([&] { return a.snap(fx.id).finished; }, 30000));

    const QString dl = fx.root + QStringLiteral("/b-dl");
    QCOMPARE(b.be->addTorrent(QString(), fx.torrent, dl, true, {}, &err), fx.id);
    QList<int> reserved, unreserved;
    qint64 unreservedBytes = 0;
    for (int i = 0; i < fx.numPieces; ++i) {
        if (i % 2 == 0) {
            reserved << i;
        } else {
            unreserved << i;
            unreservedBytes += fx.piece(i).size();
        }
    }
    b.be->reservePieces(fx.id, reserved, true);
    b.be->resumeTorrent(fx.id);
    b.ctl->addPeer(fx.id, a.endpoint());
    QVERIFY2(QTest::qWaitFor([&] { return b.haveCount(fx.id) >= unreserved.size(); }, 60000), qPrintable(dumpSnap(b.snap(fx.id))));
    QTest::qWait(3000);   // give the picker every chance to (wrongly) fetch reserved pieces
    const mx::TorrentSnapshot s = b.snap(fx.id);
    QCOMPARE(s.have.count(true), unreserved.size());
    for (int p : reserved) {
        QVERIFY2(!s.have.testBit(p), qPrintable(QStringLiteral("reserved piece %1 was downloaded").arg(p)));
        QVERIFY(!b.done[fx.id].contains(p));
    }
    QVERIFY(!s.finished);
    QVERIFY(b.finished.isEmpty());
    QCOMPARE(s.totalSize, fx.size);   // reserved pieces still count as wanted
    QVERIFY(s.totalDone < fx.size);
    qInfo("b while reserved: %s", qPrintable(dumpSnap(s)));
    // the seeder uploaded only the unreserved half
    QTest::qWait(1200);
    const qint64 upBefore = a.snap(fx.id).uploadedBytes;
    qInfo("seeder uploaded %lld bytes, unreserved half = %lld bytes", upBefore, unreservedBytes);
    QVERIFY(upBefore >= unreservedBytes);
    QVERIFY(upBefore <= unreservedBytes + kPiece);

    // inject the reserved pieces → finished, identical
    for (int p : reserved)
        b.be->injectPiece(fx.id, p, fx.piece(p));
    QVERIFY2(QTest::qWaitFor([&] { return b.finished.contains(fx.id); }, 60000), qPrintable(dumpSnap(b.snap(fx.id))));
    QCOMPARE(sha1File(dl + QStringLiteral("/payload.bin")), sha1File(fx.src));
    const mx::TorrentSnapshot f = b.snap(fx.id);
    QCOMPARE(f.totalDone, fx.size);
    QCOMPARE(f.have.count(true), fx.numPieces);
    QVERIFY(b.failed[fx.id].isEmpty());
    QTest::qWait(1200);
    QVERIFY(a.snap(fx.id).uploadedBytes <= unreservedBytes + kPiece);
}

void TstBtInject::unreserveHandsBackToPicker()
{
    Fixture fx;
    fx.make(QStringLiteral("inject3"), 12 * 1024 * 1024 + 77, kPiece, 9);
    Node a(fx.root + QStringLiteral("/a")), b(fx.root + QStringLiteral("/b"));
    a.create();
    b.create();
    QVERIFY(a.start());
    QVERIFY(b.start());
    QString err;
    QCOMPARE(a.be->addTorrent(QString(), fx.torrent, fx.root + QStringLiteral("/share"), false, {}, &err), fx.id);
    QVERIFY(QTest::qWaitFor([&] { return a.snap(fx.id).finished; }, 30000));
    const QString dl = fx.root + QStringLiteral("/b-dl");
    QCOMPARE(b.be->addTorrent(QString(), fx.torrent, dl, true, {}, &err), fx.id);
    QList<int> firstHalf;
    for (int i = 0; i < fx.numPieces / 2; ++i)
        firstHalf << i;
    b.be->reservePieces(fx.id, firstHalf, true);
    b.be->resumeTorrent(fx.id);
    b.ctl->addPeer(fx.id, a.endpoint());
    const int rest = fx.numPieces - firstHalf.size();
    QVERIFY2(QTest::qWaitFor([&] { return b.haveCount(fx.id) >= rest; }, 60000), qPrintable(dumpSnap(b.snap(fx.id))));
    QTest::qWait(2500);
    // libtorrent now considers the torrent "finished" (only priority-0 pieces left) and drops the
    // seed; the plugin must not report it finished
    QCOMPARE(b.haveCount(fx.id), rest);
    QVERIFY(!b.snap(fx.id).finished);
    QVERIFY(b.finished.isEmpty());
    qInfo("before unreserve: %s", qPrintable(dumpSnap(b.snap(fx.id))));
    // give them back: the normal picker must fetch them from the peer (no explicit reconnect here)
    b.be->reservePieces(fx.id, firstHalf, false);
    QVERIFY2(QTest::qWaitFor([&] { return b.finished.contains(fx.id); }, 60000), qPrintable(dumpSnap(b.snap(fx.id))));
    QCOMPARE(sha1File(dl + QStringLiteral("/payload.bin")), sha1File(fx.src));
    QCOMPARE(b.snap(fx.id).totalDone, fx.size);
}

void TstBtInject::injectWhilePeerIsMidPiece()
{
    // big pieces + a slow link: when we inject, the peer is in the middle of several pieces
    constexpr int kBig = 1024 * 1024;
    Fixture fx;
    fx.make(QStringLiteral("inject4"), 8 * kBig + 12345, kBig, 10);
    Node a(fx.root + QStringLiteral("/a")), b(fx.root + QStringLiteral("/b"), /*limitLan*/ true);
    a.create();
    b.create();
    QVERIFY(a.start());
    QVERIFY(b.start());
    QString err;
    QCOMPARE(a.be->addTorrent(QString(), fx.torrent, fx.root + QStringLiteral("/share"), false, {}, &err), fx.id);
    QVERIFY(QTest::qWaitFor([&] { return a.snap(fx.id).finished; }, 30000));
    b.ctl->setRateLimits(96 * 1024, 0);
    const QString dl = fx.root + QStringLiteral("/b-dl");
    QCOMPARE(b.be->addTorrent(QString(), fx.torrent, dl, false, {}, &err), fx.id);
    b.ctl->addPeer(fx.id, a.endpoint());
    QVERIFY2(QTest::qWaitFor([&] { return b.snap(fx.id).rateDownBps > 0; }, 20000), qPrintable(dumpSnap(b.snap(fx.id))));
    QTest::qWait(3000);   // ~300 KiB of blocks spread over the first pieces, none complete yet
    const int haveBefore = b.haveCount(fx.id);
    qInfo("peer is mid-way: %s", qPrintable(dumpSnap(b.snap(fx.id))));
    for (int i = 0; i < fx.numPieces; ++i)
        b.be->injectPiece(fx.id, i, readRange(fx.src, qint64(i) * kBig, kBig));
    QVERIFY2(QTest::qWaitFor([&] { return b.finished.contains(fx.id); }, 60000), qPrintable(dumpSnap(b.snap(fx.id))));
    QCOMPARE(sha1File(dl + QStringLiteral("/payload.bin")), sha1File(fx.src));
    QVERIFY(b.failed[fx.id].isEmpty());
    QCOMPARE(b.snap(fx.id).have.count(true), fx.numPieces);
    qInfo("had %d pieces from the peer before injecting; finished with no hash failure", haveBefore);
}

QTEST_GUILESS_MAIN(TstBtInject)
#include "tst_bt_inject.moc"
