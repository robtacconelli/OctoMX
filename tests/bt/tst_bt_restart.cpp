// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX: BitTorrent plugin: persistence across a clean restart and across kill -9.
#include <QtCore/QElapsedTimer>
#include <QtCore/QProcess>
#include <QtCore/QTextStream>
#include <QtCore/QTimer>

#include <cstdio>
#include <cstring>

#include "bt_test_util.h"

using namespace bttest;

namespace {
constexpr int kPiece = 256 * 1024;

QStringList g_btLog;
QtMessageHandler g_prevHandler = nullptr;
void captureHandler(QtMsgType t, const QMessageLogContext &ctx, const QString &msg)
{
    if (ctx.category && qstrcmp(ctx.category, "mx.bt") == 0)
        g_btLog << msg;
    if (g_prevHandler)
        g_prevHandler(t, ctx, msg);
}

QSet<int> toSet(const QList<int> &l) { return QSet<int>(l.cbegin(), l.cend()); }

// child mode: download until killed, print "t=<ms> have=<n>" once per second
int runChild(const QString &profile, const QString &torrentPath, const QString &dl, const QString &peer, int rateBps)
{
    Node n(profile, /*limitLan*/ true);
    n.create();
    if (!n.start())
        return 2;
    QString err;
    const QString id = n.be->addTorrent(torrentPath, {}, dl, false, {}, &err);
    if (id.isEmpty())
        return 3;
    n.ctl->setRateLimits(rateBps, 0);
    n.ctl->addPeer(id, peer);
    QElapsedTimer el;
    el.start();
    QTimer t;
    QObject::connect(&t, &QTimer::timeout, [&] {
        std::printf("t=%lld have=%d\n", el.elapsed(), int(n.done[id].size()));
        std::fflush(stdout);
    });
    t.start(1000);
    return QCoreApplication::exec();
}
} // namespace

class TstBtRestart : public QObject {
    Q_OBJECT
private slots:
    void initTestCase()
    {
        mx::registerMetaTypes();
        g_prevHandler = qInstallMessageHandler(captureHandler);
    }
    void stopMidDownloadAndRestore();
    void crashLosesAtMostOneSaveInterval();
};

void TstBtRestart::stopMidDownloadAndRestore()
{
    const QString root = freshDir(QStringLiteral("restart1"));
    const QString src = root + QStringLiteral("/share/big.bin");
    makeFile(src, 40 * 1024 * 1024 + 555, 31);
    const QByteArray tor = makeTorrent(src, kPiece);
    const QString id = infoHashOf(tor);
    const int numPieces = int((QFileInfo(src).size() + kPiece - 1) / kPiece);
    // a second torrent that stays paused, with its own save path and priority
    makeFile(root + QStringLiteral("/share2/other.bin"), 300 * 1024, 32);
    const QByteArray tor2 = makeTorrent(root + QStringLiteral("/share2/other.bin"), 64 * 1024);
    const QString id2 = infoHashOf(tor2);

    Node a(root + QStringLiteral("/a"));
    a.create();
    QVERIFY(a.start());
    QString err;
    QCOMPARE(a.be->addTorrent(QString(), tor, root + QStringLiteral("/share"), false, {}, &err), id);
    QVERIFY(QTest::qWaitFor([&] { return a.snap(id).finished; }, 30000));

    const QString bProfile = root + QStringLiteral("/b");
    const QString dl = root + QStringLiteral("/b-dl"), dl2 = root + QStringLiteral("/b-dl2");
    QList<int> reserved;
    for (int i = numPieces - 10; i < numPieces; ++i)
        reserved << i;
    int haveAtStop = 0;
    QSet<int> doneBefore;
    {
        Node b(bProfile, /*limitLan*/ true);
        b.create();
        QVERIFY(b.start());
        b.ctl->setRateLimits(4 * 1024 * 1024, 0);
        QCOMPARE(b.be->addTorrent(QString(), tor, dl, false, {}, &err), id);
        QCOMPARE(b.be->addTorrent(QString(), tor2, dl2, true, {}, &err), id2);
        b.be->setTorrentPriority(id2, 1);
        b.be->reservePieces(id, reserved, true);
        b.ctl->addPeer(id, a.endpoint());
        QVERIFY(QTest::qWaitFor([&] { return b.done[id].size() >= numPieces * 4 / 10; }, 60000));
        QVERIFY(b.finished.isEmpty());
        QElapsedTimer el;
        el.start();
        b.plugin->stop();   // saves resume data of every torrent + session state
        qInfo("stop() took %lld ms", el.elapsed());
        QVERIFY(el.elapsed() < 4000);
        doneBefore = toSet(b.done[id]);
        haveAtStop = doneBefore.size();
        for (int p : reserved)
            QVERIFY(!doneBefore.contains(p));
        el.restart();
        b.destroy();
        qInfo("destroy took %lld ms; %d of %d pieces verified at stop", el.elapsed(), haveAtStop, numPieces);
    }
    QVERIFY(QFileInfo::exists(bProfile + QStringLiteral("/bt/session.dat")));
    QVERIFY(QFileInfo::exists(bProfile + QStringLiteral("/bt/torrents/%1.fastresume").arg(id)));
    QVERIFY(QFileInfo::exists(bProfile + QStringLiteral("/bt/torrents/%1.torrent").arg(id)));

    g_btLog.clear();
    Node b2(bProfile, true);
    b2.create();   // init only: state must be there before start()
    QCOMPARE(QSet<QString>(b2.be->torrents().cbegin(), b2.be->torrents().cend()), (QSet<QString>{id, id2}));
    const mx::TorrentSnapshot r1 = b2.snap(id);
    QVERIFY(r1.hasMetadata);
    QCOMPARE(r1.savePath, dl);
    // pieces still in flight at stop() may complete before the session is torn down; they are
    // saved too, so the restored set is a superset of what was signalled before stop()
    QVERIFY(r1.have.count(true) >= haveAtStop);
    for (int p : doneBefore)
        QVERIFY(r1.have.testBit(p));
    QSet<int> restoredSet;
    for (int i = 0; i < r1.numPieces; ++i)
        if (r1.have.testBit(i))
            restoredSet.insert(i);
    haveAtStop = restoredSet.size();
    doneBefore = restoredSet;
    QVERIFY(!r1.paused);
    const mx::TorrentSnapshot r2 = b2.snap(id2);
    QVERIFY(r2.paused);
    // the resume data of a restored torrent is read in the background: on a busy disk the first
    // snapshot can still show it as being checked; it must settle as paused without being started
    if (r2.state != mx::TransferState::Paused)
        qInfo() << "restored paused torrent first seen as" << int(r2.state) << r2.statusText;
    QTRY_COMPARE_WITH_TIMEOUT(b2.snap(id2).state, mx::TransferState::Paused, 15000);
    QCOMPARE(r2.savePath, dl2);
    QCOMPARE(b2.be->pieceHash(id, 3), a.be->pieceHash(id, 3));

    QVERIFY(b2.start());
    b2.ctl->addPeer(id, a.endpoint());
    QVERIFY2(QTest::qWaitFor([&] { return b2.finished.contains(id); }, 60000), qPrintable(b2.snap(id).statusText));
    QTest::qWait(1200);
    qInfo("seeder uploaded %lld bytes in total (torrent %lld)", a.snap(id).uploadedBytes, QFileInfo(src).size());
    // nothing verified before the restart was transferred again (slack: blocks in flight at stop)
    QVERIFY(a.snap(id).uploadedBytes <= QFileInfo(src).size() + 12 * kPiece);
    const QSet<int> doneAfter = toSet(b2.done[id]);
    qInfo("after restart: downloaded %d more pieces, %d were restored", int(doneAfter.size()), haveAtStop);
    QVERIFY(!doneAfter.intersects(doneBefore));               // nothing verified was downloaded again
    QCOMPARE(int(doneAfter.size()) + haveAtStop, numPieces);
    for (int p : reserved)
        QVERIFY(doneAfter.contains(p));                       // reservations were cleared by the restart
    QCOMPARE(sha1File(dl + QStringLiteral("/big.bin")), sha1File(src));
    for (const QString &m : std::as_const(g_btLog))
        QVERIFY2(!m.contains(QLatin1String("fastresume")) && !m.contains(QLatin1String("rejected")), qPrintable(m));
    // paused torrent is still paused once online
    QTest::qWait(1500);
    QVERIFY(b2.snap(id2).paused);
    QCOMPARE(b2.snap(id2).totalDone, 0);
}

void TstBtRestart::crashLosesAtMostOneSaveInterval()
{
    const QString root = freshDir(QStringLiteral("restart2"));
    const QString src = root + QStringLiteral("/share/crash.bin");
    makeFile(src, 40 * 1024 * 1024 + 99, 41);
    const QByteArray tor = makeTorrent(src, kPiece);
    const QString id = infoHashOf(tor);
    const QString tpath = root + QStringLiteral("/crash.torrent");
    {
        QFile f(tpath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(tor);
    }
    Node a(root + QStringLiteral("/a"));
    a.create();
    QVERIFY(a.start());
    QString err;
    QCOMPARE(a.be->addTorrent(QString(), tor, root + QStringLiteral("/share"), false, {}, &err), id);
    QVERIFY(QTest::qWaitFor([&] { return a.snap(id).finished; }, 30000));

    const QString profile = root + QStringLiteral("/c"), dl = root + QStringLiteral("/c-dl");
    QProcess child;
    child.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    child.start(QCoreApplication::applicationFilePath(),
                {QStringLiteral("--child"), profile, tpath, dl, a.endpoint(), QString::number(600 * 1024)});
    QVERIFY(child.waitForStarted(10000));
    QList<QPair<qint64, int>> samples;
    QElapsedTimer el;
    el.start();
    while (el.elapsed() < 37000 && child.state() == QProcess::Running) {
        child.waitForReadyRead(500);
        while (child.canReadLine()) {
            const QString line = QString::fromLatin1(child.readLine()).trimmed();
            const QStringList kv = line.split(QLatin1Char(' '));
            if (kv.size() == 2)
                samples.append({kv[0].mid(2).toLongLong(), kv[1].mid(5).toInt()});
        }
    }
    QCOMPARE(child.state(), QProcess::Running);
    child.kill();   // SIGKILL: no shutdown code runs
    QVERIFY(child.waitForFinished(10000));
    QVERIFY(!samples.isEmpty());
    const int haveAtKill = samples.last().second;
    int haveAt26 = 0;
    for (const auto &s : samples)
        if (s.first <= 26000)
            haveAt26 = s.second;
    qInfo("child killed at t=%lld ms with %d pieces; %d pieces at t<=26 s", samples.last().first, haveAtKill, haveAt26);
    QVERIFY(haveAt26 > 0);

    Node r(profile, true);
    r.create();
    QVERIFY(r.be->torrents().contains(id));
    const int restored = r.snap(id).have.count(true);
    qInfo("restored after kill -9: %d pieces (lost %d pieces ≈ %.1f s at 600 KiB/s)", restored, haveAtKill - restored,
          (haveAtKill - restored) * double(kPiece) / (600 * 1024));
    QVERIFY(restored >= haveAt26);           // the periodic save (every 30 s) made it to disk
    QVERIFY(restored <= haveAtKill);
    QVERIFY((haveAtKill - restored) * double(kPiece) / (600 * 1024) <= 60.0);
    // and it completes from there, after the recheck-free restore
    QVERIFY(r.start());
    r.ctl->addPeer(id, a.endpoint());
    QVERIFY(QTest::qWaitFor([&] { return r.finished.contains(id); }, 60000));
    QCOMPARE(sha1File(dl + QStringLiteral("/crash.bin")), sha1File(src));
}

int main(int argc, char **argv)
{
    if (argc >= 7 && std::strcmp(argv[1], "--child") == 0) {
        QCoreApplication app(argc, argv);
        mx::registerMetaTypes();
        return runChild(QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]), QString::fromLocal8Bit(argv[4]),
                        QString::fromLocal8Bit(argv[5]), std::atoi(argv[6]));
    }
    QCoreApplication app(argc, argv);
    TstBtRestart tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_bt_restart.moc"
