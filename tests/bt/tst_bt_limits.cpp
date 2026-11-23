// SPDX-License-Identifier: GPL-3.0-or-later
// [bw-1] libtorrent inside the core's one shared budget: a seeder on 127.0.0.1 with the core's
// CoreRateLimiter (LAN peers limited too, bandwidth/limitLan) uploads to a second session.
//   * the upload limit holds (measured as the downloader's progress);
//   * a change applies within a couple of seconds (the plugin polls budgetBps() once per second);
//   * a second, non-BitTorrent uploader in the same limiter (acquire, as eD2k / Gnutella /
//     overlay uploads do): together they stay at the limit and share it;
//   * unlimited is unlimited.
#include <QtCore/QElapsedTimer>
#include <QtCore/QTimer>

#include "bt_test_util.h"
#include "core/rate_limiter.h"

using namespace bttest;

class TstBtLimits : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { mx::registerMetaTypes(); }
    void seederObeysSharedUploadLimit();
};

void TstBtLimits::seederObeysSharedUploadLimit()
{
    const QString root = freshDir(QStringLiteral("limits"));
    const QString src = root + QStringLiteral("/share/octomx-bw1-48M.bin");
    makeFile(src, 48 * 1024 * 1024, 7);

    mx::CoreRateLimiter limiter(nullptr);
    QTimer tick;                       // the BandwidthService's 1 Hz tick
    tick.setInterval(1000);
    connect(&tick, &QTimer::timeout, &limiter, &mx::CoreRateLimiter::tick);
    tick.start();
    const int limit = 300 * 1024;
    limiter.setLimits(0, limit);

    Node a(root + QStringLiteral("/a"), /*limitLan*/ true), b(root + QStringLiteral("/b"), /*limitLan*/ true);
    a.limiter = &limiter;
    a.create();
    b.create();
    QVERIFY(a.start());
    QVERIFY(b.start());
    QString err;
    QVERIFY2(a.be->seedLocalFile(src, &err), qPrintable(err));
    QVERIFY(QTest::qWaitFor([&] { return !a.seeded.isEmpty(); }, 60000));
    const QString id = a.seeded.begin().value();
    QVERIFY(QTest::qWaitFor([&] { return a.snap(id).state == mx::TransferState::Seeding; }, 20000));

    const QString dl = root + QStringLiteral("/b-dl");
    QCOMPARE(b.be->addTorrent(QStringLiteral("magnet:?xt=urn:btih:%1&dn=x").arg(id), {}, dl, false, {}, &err), id);
    b.ctl->addPeer(id, a.endpoint());
    QVERIFY(QTest::qWaitFor([&] { return b.metadata.contains(id); }, 20000));
    b.be->resumeTorrent(id);
    b.ctl->addPeer(id, a.endpoint());
    QVERIFY(QTest::qWaitFor([&] { return b.snap(id).totalDone > 0; }, 20000));

    // the downloader's progress over `ms` = the seeder's upload rate
    auto rate = [&](int ms) {
        QTest::qWait(1000);   // let a change settle (budget polled once per second)
        const qint64 d0 = b.snap(id).totalDone;
        QElapsedTimer t;
        t.start();
        QTest::qWait(ms);
        return double(b.snap(id).totalDone - d0) * 1000.0 / double(t.elapsed());
    };
    // 16 s: the downloader's progress moves in whole pieces, over 8 s one piece more or less was a
    // fifth of the expected amount and the check sat on its own edge
    const double r1 = rate(16000);
    qInfo("BitTorrent upload, limit %d B/s: %.0f B/s", limit, r1);
    QVERIFY2(r1 > limit * 0.8 && r1 < limit * 1.15, qPrintable(QString::number(r1)));

    // a change applies while it runs
    limiter.setLimits(0, 2 * limit);
    QTest::qWait(1000);
    const double r2 = rate(10000);
    qInfo("BitTorrent upload after a change to %d B/s: %.0f B/s", 2 * limit, r2);
    QVERIFY2(r2 > 2 * limit * 0.8 && r2 < 2 * limit * 1.15, qPrintable(QString::number(r2)));

    // another network uploads through the same budget (greedy, like an eD2k upload): the total
    // stays at the limit and BitTorrent keeps about half of it
    limiter.setLimits(0, limit);
    qint64 other = 0;
    QTimer otherUp;
    otherUp.setInterval(10);
    connect(&otherUp, &QTimer::timeout, this, [&] {
        const qint64 n = limiter.acquire(mx::RateLimiter::Up, 16 * 1024);
        if (n > 0) {
            limiter.accountTraffic(mx::RateLimiter::Up, mx::Net::ED2K, n, n);
            other += n;
        }
    });
    otherUp.start();
    QTest::qWait(6000);   // libtorrent gives way: a fifth of the limit per second, down to half
    const qint64 o0 = other;
    QElapsedTimer t;
    t.start();
    const double bt = rate(8000);
    const double ed = double(other - o0) * 1000.0 / double(t.elapsed());
    otherUp.stop();
    qInfo("shared upload budget %d B/s: BitTorrent %.0f B/s + other network %.0f B/s = %.0f B/s", limit, bt, ed, bt + ed);
    QVERIFY2(bt + ed < limit * 1.15 && bt + ed > limit * 0.8, qPrintable(QStringLiteral("%1 + %2").arg(bt).arg(ed)));
    QVERIFY2(bt > limit * 0.35 && ed > limit * 0.25, qPrintable(QStringLiteral("%1 + %2").arg(bt).arg(ed)));

    // unlimited
    limiter.setLimits(0, 0);
    const double r3 = rate(3000);
    qInfo("BitTorrent upload unlimited: %.0f B/s", r3);
    QVERIFY2(r3 > 8 * limit || b.snap(id).finished, qPrintable(QString::number(r3)));
}

QTEST_MAIN(TstBtLimits)
#include "tst_bt_limits.moc"
