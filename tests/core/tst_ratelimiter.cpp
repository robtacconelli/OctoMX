// SPDX-License-Identifier: GPL-3.0-or-later
#include <QtTest/QtTest>
#include <QtTest/QSignalSpy>

#include "core/bandwidth.h"
#include "core/database.h"
#include "core/rate_limiter.h"
#include "mx/settings_keys.h"
#include "mx/simple_settings.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QTemporaryDir>
#include <QtNetwork/QHostAddress>

#include <thread>
#include <vector>

using namespace mx;

class TestRateLimiter : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { registerMetaTypes(); }

    void unlimited()
    {
        CoreRateLimiter l(nullptr);
        QCOMPARE(l.limitBps(RateLimiter::Down), 0);
        QCOMPARE(l.acquire(RateLimiter::Down, 1 << 30), qint64(1) << 30);
        QCOMPARE(l.acquire(RateLimiter::Up, 0), qint64(0));
        QCOMPARE(l.budgetBps(RateLimiter::Down, Net::BitTorrent), 0);
    }

    void tokenBucket()
    {
        CoreRateLimiter l(nullptr);
        l.setLimits(100000, 50000);   // 100 kB/s down
        QCOMPARE(l.limitBps(RateLimiter::Down), 100000);
        // the bucket starts empty and holds at most 100 ms of the limit ([bw-2], was one second):
        // after ~200 ms about 10 kB are available
        QTest::qWait(200);
        const qint64 got = l.acquire(RateLimiter::Down, 1000000);
        QVERIFY2(got > 5000 && got <= 10500, qPrintable(QString::number(got)));
        QCOMPARE(l.acquire(RateLimiter::Down, 1000000) < 2000, true);   // drained
        // sustained rate over ~1 s stays near the limit
        QElapsedTimer t;
        t.start();
        qint64 total = 0;
        while (t.elapsed() < 1000) {
            total += l.acquire(RateLimiter::Down, 4096);
            QThread::msleep(5);
        }
        QVERIFY2(total > 70000 && total < 130000, qPrintable(QString::number(total)));
        // burst is capped at a tenth of a second worth
        QTest::qWait(1500);
        const qint64 burst = l.acquire(RateLimiter::Down, 10000000);
        QVERIFY2(burst <= 10500, qPrintable(QString::number(burst)));
    }

    // [bw-2] the refill is smooth: a greedy consumer that asks every few ms gets the limit in every
    // 100 ms (not a second's worth at once and then nothing), and after a pause no more than 100 ms
    // of it at once
    void smoothRefill()
    {
        CoreRateLimiter l(nullptr);
        const int limit = 200000;
        l.setLimits(limit, 0);
        QTest::qWait(150);
        QElapsedTimer t;
        t.start();
        QList<qint64> slices;   // bytes per 100 ms
        qint64 cur = 0, sliceEnd = 100, total = 0;
        while (t.elapsed() < 3000) {
            const qint64 n = l.acquireFor(RateLimiter::Down, Net::Http, 64 * 1024);
            cur += n;
            total += n;
            if (t.elapsed() >= sliceEnd) {
                slices << cur;
                cur = 0;
                sliceEnd += 100;
            }
            QThread::msleep(3);
        }
        qint64 lo = 1 << 30, hi = 0;
        for (int i = 1; i < slices.size(); ++i) {   // the first holds the full bucket
            lo = qMin(lo, slices[i]);
            hi = qMax(hi, slices[i]);
        }
        const QString msg = QStringLiteral("100 ms slices %1 to %2 B, %3 B/s").arg(lo).arg(hi).arg(total / 3.0);
        qInfo("smooth refill, limit %d B/s: %s", limit, qPrintable(msg));
        QVERIFY2(lo >= limit / 10 * 0.6 && hi <= limit / 10 * 1.5, qPrintable(msg));
        QVERIFY2(total / 3.0 > limit * 0.9 && total / 3.0 < limit * 1.1, qPrintable(msg));
        QTest::qWait(1000);   // a pause: at most 100 ms of the limit comes at once
        const qint64 burst = l.acquireFor(RateLimiter::Down, Net::Http, 1 << 20);
        QVERIFY2(burst <= limit / 10 + 500, qPrintable(QString::number(burst)));
    }

    // [bw-2] fairness: BitTorrent (libtorrent follows budgetBps(), polled every 200 ms like the plugin,
    // its traffic accounted every 100 ms), then HTTP joins, then eD2k, then HTTP leaves. Each network
    // that wants more gets an equal share within about a second, the total stays at the limit in
    // every second, and the one that leaves gives its share back.
    void fairnessJoinLeave()
    {
        CoreRateLimiter l(nullptr);
        const int limit = 300 * 1024;
        l.setLimits(limit, 0);
        QElapsedTimer t;
        t.start();
        qint64 lastBt = 0, lastPoll = 0;
        int btBudget = l.budgetBps(RateLimiter::Down, Net::BitTorrent);
        // per 100 ms slice: bytes of each network
        struct Slice { qint64 bt = 0, http = 0, ed2k = 0; };
        QList<Slice> slices;
        Slice cur;
        qint64 sliceEnd = 100;
        qint64 httpFirstFair = -1;
        while (t.elapsed() < 12000) {
            const qint64 now = t.elapsed();
            const bool http = now >= 2000 && now < 9000, ed2k = now >= 5000;
            if (now - lastBt >= 100) {   // libtorrent: its budget, evenly, reported every 100 ms
                const qint64 bytes = qint64(btBudget) * (now - lastBt) / 1000;
                l.accountTraffic(RateLimiter::Down, Net::BitTorrent, bytes, bytes);
                cur.bt += bytes;
                lastBt = now;
            }
            if (now - lastPoll >= 200) {
                btBudget = l.budgetBps(RateLimiter::Down, Net::BitTorrent);
                lastPoll = now;
            }
            // the greedy ones, in turns (who asks first changes)
            for (int k = 0; k < 2; ++k) {
                const bool first = ((now / 7) % 2) == k;
                if (first && http) {
                    const qint64 n = l.acquireFor(RateLimiter::Down, Net::Http, 32 * 1024);
                    if (n > 0) l.accountTraffic(RateLimiter::Down, Net::Http, n, n);
                    cur.http += n;
                }
                if (!first && ed2k) {
                    const qint64 n = l.acquireFor(RateLimiter::Down, Net::ED2K, 32 * 1024);
                    if (n > 0) l.accountTraffic(RateLimiter::Down, Net::ED2K, n, n);
                    cur.ed2k += n;
                }
            }
            if (now >= sliceEnd) {
                slices << cur;
                cur = Slice();
                sliceEnd += 100;
            }
            QThread::msleep(2);
        }
        auto rate = [&](qint64 Slice::*f, int fromMs, int toMs) {
            qint64 sum = 0;
            for (int i = fromMs / 100; i < toMs / 100 && i < slices.size(); ++i) sum += slices[i].*f;
            return double(sum) * 1000.0 / (toMs - fromMs);
        };
        // when HTTP first had 80 % of its fair half over a second
        for (int ms = 2000; ms + 1000 <= 5000 && httpFirstFair < 0; ms += 100)
            if (rate(&Slice::http, ms, ms + 1000) >= limit * 0.5 * 0.8) httpFirstFair = ms + 1000 - 2000;
        // the total of every one-second window (in 100 ms steps) after the first second
        double worst = 0;
        int worstMs = 0;
        for (int ms = 1000; ms + 1000 <= 12000; ms += 100) {
            const double tot = rate(&Slice::bt, ms, ms + 1000) + rate(&Slice::http, ms, ms + 1000) + rate(&Slice::ed2k, ms, ms + 1000);
            if (tot > worst) { worst = tot; worstMs = ms; }
        }
        const double btA = rate(&Slice::bt, 1000, 2000);
        const double btB = rate(&Slice::bt, 3500, 5000), httpB = rate(&Slice::http, 3500, 5000);
        const double btC = rate(&Slice::bt, 6500, 9000), httpC = rate(&Slice::http, 6500, 9000), edC = rate(&Slice::ed2k, 6500, 9000);
        const double btD = rate(&Slice::bt, 10500, 12000), edD = rate(&Slice::ed2k, 10500, 12000);
        const QString msg = QStringLiteral("alone bt %1 | +http bt %2 http %3 (fair after %4 ms) | +ed2k bt %5 http %6 ed2k %7 | -http bt %8 ed2k %9 | worst 1 s total %10 at %11 ms (KiB/s)")
                                .arg(btA / 1024, 0, 'f', 0).arg(btB / 1024, 0, 'f', 0).arg(httpB / 1024, 0, 'f', 0).arg(httpFirstFair)
                                .arg(btC / 1024, 0, 'f', 0).arg(httpC / 1024, 0, 'f', 0).arg(edC / 1024, 0, 'f', 0)
                                .arg(btD / 1024, 0, 'f', 0).arg(edD / 1024, 0, 'f', 0).arg(worst / 1024, 0, 'f', 0).arg(worstMs);
        qInfo("fairness, limit 300 KiB/s: %s", qPrintable(msg));
        QStringList around;   // the worst second, slice by slice (KiB per 100 ms: bt/http/ed2k)
        for (int i = worstMs / 100; i < worstMs / 100 + 10 && i < slices.size(); ++i)
            around << QStringLiteral("%1/%2/%3").arg(slices[i].bt / 1024).arg(slices[i].http / 1024).arg(slices[i].ed2k / 1024);
        qInfo("worst second: %s", qPrintable(around.join(QLatin1Char(' '))));
        QVERIFY2(btA > limit * 0.9, qPrintable(msg));                                   // alone: all of it
        QVERIFY2(httpFirstFair >= 0 && httpFirstFair <= 1500, qPrintable(msg));         // a joiner: its share within 1.5 s
        auto near = [](double v, double want) { return v > want * 0.75 && v < want * 1.25; };
        QVERIFY2(near(btB, limit / 2.0) && near(httpB, limit / 2.0), qPrintable(msg));
        QVERIFY2(near(btC, limit / 3.0) && near(httpC, limit / 3.0) && near(edC, limit / 3.0), qPrintable(msg));
        QVERIFY2(near(btD, limit / 2.0) && near(edD, limit / 2.0), qPrintable(msg));     // HTTP's share given back
        QVERIFY2(btC + httpC + edC > limit * 0.95, qPrintable(msg));                    // nothing lost
        // every second, joins and leaves included: the limit plus at most one bucket (100 ms of it,
        // tokens nobody took while a network left) and the reporting step of libtorrent; the rest of
        // the margin is for timers that run late when the machine is busy (seen: 112.7 % in a
        // parallel test run, 103 to 108 % alone)
        QVERIFY2(worst < limit * 1.15, qPrintable(msg));
    }

    void threadSafeAccounting()
    {
        CoreRateLimiter l(nullptr);
        std::vector<std::thread> threads;
        for (int t = 0; t < 8; ++t)
            threads.emplace_back([&l, t] {
                for (int i = 0; i < 10000; ++i) {
                    l.account(RateLimiter::Down, t % 2 ? Net::ED2K : Net::Gnutella, 10);
                    l.acquire(RateLimiter::Up, 5);
                }
            });
        for (auto &th : threads) th.join();
        QCOMPARE(l.total(RateLimiter::Down), qint64(8 * 10000 * 10));
        QCOMPARE(l.total(RateLimiter::Down, Net::ED2K), qint64(4 * 10000 * 10));
        QCOMPARE(l.total(RateLimiter::Up), qint64(0));
    }

    // [bw-2] libtorrent's budget follows the others every 100 ms: all of the limit while nobody else
    // moves, what a satisfied network leaves, a newcomer's half while another one wants more
    void budgetForLibtorrent()
    {
        CoreRateLimiter l(nullptr);
        l.setLimits(100000, 0);
        QCOMPARE(l.budgetBps(RateLimiter::Down, Net::BitTorrent), 100000);
        QCOMPARE(l.budgetBps(RateLimiter::Down, Net::ED2K), 100000);
        auto run = [&l](int ms, qint64 ask) {   // eD2k asks for `ask` bytes every 10 ms
            QElapsedTimer t;
            t.start();
            qint64 next = 0;
            while (t.elapsed() < ms) {
                const qint64 n = l.acquireFor(RateLimiter::Down, Net::ED2K, ask);
                if (n > 0) l.accountTraffic(RateLimiter::Down, Net::ED2K, n, n);
                // [mac-1] every 10 ms on average: a plain msleep(10) lasts longer on macOS (timer slack)
                next += 10;
                const qint64 wait = next - t.elapsed();
                if (wait > 0) QThread::msleep(quint64(wait));
            }
        };
        run(1500, 300);   // satisfied at ~30 kB/s: libtorrent gets what it leaves (minus a margin)
        const int b1 = l.budgetBps(RateLimiter::Down, Net::BitTorrent);
        QVERIFY2(b1 >= 55000 && b1 <= 70000, qPrintable(QString::number(b1)));
        run(1500, 64 * 1024);   // greedy: it wants more than it gets, libtorrent (idle) keeps half
        const int b2 = l.budgetBps(RateLimiter::Down, Net::BitTorrent);
        QVERIFY2(b2 >= 45000 && b2 <= 55000, qPrintable(QString::number(b2)));
        QTest::qWait(1500);   // nothing moves any more: (nearly) all of it again
        // [mac-1] the limiter's 1 s tick may land a little later on a busy CI Mac: up to one more second
        QTRY_VERIFY2_WITH_TIMEOUT(l.budgetBps(RateLimiter::Down, Net::BitTorrent) >= 90000,
                                  qPrintable(QString::number(l.budgetBps(RateLimiter::Down, Net::BitTorrent))), 1000);
        QTest::qWait(1000);
        QCOMPARE(l.budgetBps(RateLimiter::Down, Net::BitTorrent), 100000);
        QCOMPARE(l.shareBps(RateLimiter::Down, Net::ED2K), 0);
    }

    void settingsAndSchedule()
    {
        SimpleSettings s;
        CoreRateLimiter l(&s);
        QSignalSpy spy(&l, &CoreRateLimiter::limitsChanged);
        s.setValue(keys::bandwidth::DownLimitKBs, 50);
        QCOMPARE(l.limitBps(RateLimiter::Down), 50 * 1024);
        s.setValue(keys::bandwidth::UpLimitKBs, 10);
        QCOMPARE(l.limitBps(RateLimiter::Up), 10 * 1024);
        QVERIFY(spy.count() >= 2);

        // schedule window logic (Mon=1 ... Sun=7 / mask bit 0 ... 6)
        const QString a = QStringLiteral("08:00"), b = QStringLiteral("18:00");
        QVERIFY(CoreRateLimiter::scheduleActive(QTime(9, 0), 1, a, b, 31));
        QVERIFY(!CoreRateLimiter::scheduleActive(QTime(19, 0), 1, a, b, 31));
        QVERIFY(!CoreRateLimiter::scheduleActive(QTime(9, 0), 6, a, b, 31));     // Saturday not in Mon-Fri
        // overnight 22:00-06:00 on Fridays only: Friday 23:00 yes, Saturday 03:00 yes (started Friday)
        QVERIFY(CoreRateLimiter::scheduleActive(QTime(23, 0), 5, QStringLiteral("22:00"), QStringLiteral("06:00"), 16));
        QVERIFY(CoreRateLimiter::scheduleActive(QTime(3, 0), 6, QStringLiteral("22:00"), QStringLiteral("06:00"), 16));
        QVERIFY(!CoreRateLimiter::scheduleActive(QTime(3, 0), 5, QStringLiteral("22:00"), QStringLiteral("06:00"), 16));
        QVERIFY(!CoreRateLimiter::scheduleActive(QTime(3, 0), 5, QStringLiteral("bad"), b, 127));

        // an always-active schedule switches to the alternative limits
        s.setValue(keys::bandwidth::ScheduleStart, QStringLiteral("00:00"));
        s.setValue(keys::bandwidth::ScheduleEnd, QStringLiteral("23:59"));
        s.setValue(keys::bandwidth::ScheduleDays, 127);
        s.setValue(keys::bandwidth::ScheduleDownKBs, 7);
        s.setValue(keys::bandwidth::ScheduleUpKBs, 30);
        s.setValue(keys::bandwidth::ScheduleEnabled, true);
        if (QTime::currentTime() < QTime(23, 59)) {
            QCOMPARE(l.limitBps(RateLimiter::Down), 7 * 1024);
            QCOMPARE(l.limitBps(RateLimiter::Up), 30 * 1024);
        }
        s.setValue(keys::bandwidth::ScheduleEnabled, false);
        QCOMPARE(l.limitBps(RateLimiter::Down), 50 * 1024);
    }

    // ---------------------------------------------------------------- [bw-1] one shared budget
    void unthrottledTrafficIsCharged()
    {
        CoreRateLimiter l(nullptr);
        l.setLimits(100000, 0);
        QTest::qWait(300);                                   // ~30 kB of tokens
        l.accountTraffic(RateLimiter::Down, Net::ED2K, 60000, 0);   // protocol bytes of a transfer: not acquired, charged
        // in debt now: [eng-1] only the small grant that keeps protocol messages moving
        QVERIFY(l.acquire(RateLimiter::Down, 50000) <= CoreRateLimiter::kTrickleBytes);
        QTest::qWait(400);                                   // pays back ~40 kB: still ~0
        QVERIFY(l.acquire(RateLimiter::Down, 50000) < 15000);
        // acquired bytes are not charged twice when they are accounted
        CoreRateLimiter m(nullptr);
        m.setLimits(100000, 0);
        QTest::qWait(500);
        const qint64 got = m.acquire(RateLimiter::Down, 8000);   // [bw-2] within the 100 ms bucket
        QCOMPARE(got, qint64(8000));
        m.accountTraffic(RateLimiter::Down, Net::ED2K, got, got);
        QCOMPARE(m.chargedTotal(RateLimiter::Down), got);
        // unlimited: nothing is charged, everything is granted
        m.setLimits(0, 0);
        m.accountTraffic(RateLimiter::Down, Net::ED2K, 1 << 20, 0);
        QCOMPARE(m.acquire(RateLimiter::Down, 1 << 30), qint64(1) << 30);
    }

    void bitTorrentChargedWithinItsBudget()
    {
        QElapsedTimer waited;
        waited.start();
        CoreRateLimiter l(nullptr);
        l.setLimits(100000, 0);
        QCOMPARE(l.budgetBps(RateLimiter::Down, Net::BitTorrent), 100000);
        QTest::qWait(1000);
        // libtorrent moved 300 kB in a second: 100 kB within its budget, the rest is its unlimited
        // local (LAN) class or DHT; only the budget part is charged
        l.accountTraffic(RateLimiter::Down, Net::BitTorrent, 300000, 300000);
        const qint64 c = l.chargedTotal(RateLimiter::Down);
        // [mac-1] the budget of the time that really passed (qWait(1000) lasted up to 1.07 s on a CI Mac)
        const qint64 upper = qMax<qint64>(105000, 100000 * waited.elapsed() / 1000 + 5000);
        QVERIFY2(c >= 90000 && c <= upper, qPrintable(QStringLiteral("%1 (at most %2)").arg(c).arg(upper)));
        QVERIFY(l.acquire(RateLimiter::Down, 50000) < 2000);   // the others wait
    }

    // A simulated mix: a greedy eD2k-like consumer (acquire, then account), Kad packets (only
    // accounted), libtorrent (polls budgetBps once per second, moves what it was given) and the
    // 1 Hz tick. The total of all networks stays at the limit, and BitTorrent, which had all of it
    // first, gives way to about half.
    void sharedBudgetAcrossNetworks()
    {
        CoreRateLimiter l(nullptr);
        const int limit = 200000;
        l.setLimits(limit, 0);
        QElapsedTimer t;
        t.start();
        qint64 lastTick = 0, lastBt = 0;
        int btBudget = l.budgetBps(RateLimiter::Down, Net::BitTorrent);
        qint64 start = 0, startBt = 0, startEd = 0;
        bool measuring = false;
        while (t.elapsed() < 6000) {
            const qint64 now = t.elapsed();
            if (!measuring && now >= 2000) {   // BitTorrent started with all of it: let it give way
                measuring = true;
                start = l.total(RateLimiter::Down);
                startBt = l.total(RateLimiter::Down, Net::BitTorrent);
                startEd = l.total(RateLimiter::Down, Net::ED2K);
            }
            const qint64 got = l.acquire(RateLimiter::Down, 8192);
            if (got > 0) l.accountTraffic(RateLimiter::Down, Net::ED2K, got, got);
            if (now - lastBt >= 50) {          // libtorrent's rate limiter: its budget, evenly
                const qint64 bytes = qint64(btBudget) * (now - lastBt) / 1000;
                l.accountTraffic(RateLimiter::Down, Net::BitTorrent, bytes, bytes);
                l.accountTraffic(RateLimiter::Down, Net::Kad, 500, 0);   // ~10 kB/s of Kad packets ([bw-2] counted, not charged)
                lastBt = now;
            }
            if (now - lastTick >= 1000) {
                l.tick();
                btBudget = l.budgetBps(RateLimiter::Down, Net::BitTorrent);
                lastTick = now;
            }
            QThread::msleep(2);
        }
        const double secs = (t.elapsed() - 2000) / 1000.0;
        const double total = (l.total(RateLimiter::Down) - start) / secs;
        const double bt = (l.total(RateLimiter::Down, Net::BitTorrent) - startBt) / secs;
        const double ed = (l.total(RateLimiter::Down, Net::ED2K) - startEd) / secs;
        const QString msg = QStringLiteral("total %1 bt %2 ed2k %3").arg(total).arg(bt).arg(ed);
        qInfo("shared budget, limit %d B/s: %s", limit, qPrintable(msg));
        QVERIFY2(total <= limit * 1.10 && total >= limit * 0.85, qPrintable(msg));
        QVERIFY2(bt >= limit * 0.4 && ed >= limit * 0.2, qPrintable(msg));
    }

    // [eng-1] BitTorrent has used the whole limit for seconds (the bucket is empty, its budget is the
    // full limit); a network with no traffic yet asks for its first bytes (a hello, a file list).
    // It gets some at once (the small grant), a fair share within a few seconds, and the total stays
    // at the limit.
    void quietNetworkGetsAllowance()
    {
        CoreRateLimiter l(nullptr);
        const int limit = 100000;
        l.setLimits(limit, 0);
        QElapsedTimer t;
        t.start();
        qint64 lastTick = 0, lastBt = 0, firstGrantMs = -1, newcomer = 0, start = 0;
        int btBudget = l.budgetBps(RateLimiter::Down, Net::BitTorrent);
        while (t.elapsed() < 6000) {
            const qint64 now = t.elapsed();
            if (now - lastBt >= 50) {   // libtorrent moves its budget, evenly
                const qint64 bytes = qint64(btBudget) * (now - lastBt) / 1000;
                l.accountTraffic(RateLimiter::Down, Net::BitTorrent, bytes, bytes);
                lastBt = now;
            }
            if (now - lastTick >= 1000) {
                l.tick();
                btBudget = l.budgetBps(RateLimiter::Down, Net::BitTorrent);
                lastTick = now;
            }
            if (now >= 2500) {          // the newcomer: zero history so far
                if (!start) start = l.total(RateLimiter::Down);
                const qint64 got = l.acquire(RateLimiter::Down, 4096);
                if (got > 0) {
                    if (firstGrantMs < 0) firstGrantMs = now - 2500;
                    newcomer += got;
                    l.accountTraffic(RateLimiter::Down, Net::ED2K, got, got);
                }
            }
            QThread::msleep(2);
        }
        const double secs = (t.elapsed() - 2500) / 1000.0;
        const double total = (l.total(RateLimiter::Down) - start) / secs;
        const QString msg = QStringLiteral("first grant after %1 ms, newcomer %2 B/s, total %3 B/s").arg(firstGrantMs).arg(newcomer / secs).arg(total);
        qInfo("%s", qPrintable(msg));
        QVERIFY2(firstGrantMs >= 0 && firstGrantMs <= CoreRateLimiter::kTrickleMs + 50, qPrintable(msg));
        QVERIFY2(newcomer / secs >= limit * 0.2, qPrintable(msg));
        QVERIFY2(total <= limit * 1.15, qPrintable(msg));
    }

    // [eng-1] Protocol overhead is never refused: charged traffic (Kad packets, protocol messages)
    // only goes into debt, however deep; and while in debt a waiting connection still gets a small
    // grant every kTrickleMs, so its control messages are read.
    // [bw-2] the limits are for file transfers: search and discovery traffic (Kad, web indexes, Rats,
    // and what the plugins pass as exempt: servers, DHT, trackers, queries, overlay control) is counted
    // for the graph but never charged, so a search never takes allowance away from a transfer
    void searchTrafficIsNeverCharged()
    {
        CoreRateLimiter l(nullptr);
        const int limit = 100000;
        l.setLimits(limit, limit);
        QElapsedTimer t;
        t.start();
        qint64 got = 0;
        int i = 0;
        qint64 next = 0;
        while (t.elapsed() < 2000) {
            // 5x the limit of search answers next to a greedy transfer
            if (i++ % 2 == 0) {
                l.accountTraffic(RateLimiter::Down, Net::Kad, 1000, 0);
                l.accountTraffic(RateLimiter::Down, Net::Web, 1000, 0);
                l.accountTraffic(RateLimiter::Down, Net::Rats, 1000, 0);
                l.accountExempt(RateLimiter::Down, Net::BitTorrent, 1000, 0);   // DHT / tracker
                l.accountExempt(RateLimiter::Down, Net::ED2K, 1000, 0);         // server connection
            }
            const qint64 n = l.acquireFor(RateLimiter::Down, Net::Http, 8192);
            if (n > 0) l.accountTraffic(RateLimiter::Down, Net::Http, n, n);
            got += n;
            // [mac-1] every 2 ms on average (macOS sleeps longer than asked)
            next += 2;
            const qint64 wait = next - t.elapsed();
            if (wait > 0) QThread::msleep(quint64(wait));
        }
        const double rate = got * 1000.0 / t.elapsed();
        const qint64 search = l.total(RateLimiter::Down, Net::Kad) + l.total(RateLimiter::Down, Net::Web) + l.total(RateLimiter::Down, Net::Rats);
        qInfo("transfer %.0f B/s at a limit of %d B/s next to %.0f B/s of search traffic", rate, limit, search * 1000.0 / t.elapsed());
        QVERIFY2(rate > limit * 0.9 && rate < limit * 1.1, qPrintable(QString::number(rate)));
        QVERIFY(search > 3 * limit);                                              // counted
        QCOMPARE(l.chargedTotal(RateLimiter::Down), got);                          // only the transfer was charged
        QCOMPARE(l.budgetBps(RateLimiter::Down, Net::BitTorrent) > 0, true);
    }

    void overheadNeverBlocked()
    {
        CoreRateLimiter l(nullptr);
        l.setLimits(20000, 10240);
        for (int i = 0; i < 50; ++i) l.accountTraffic(RateLimiter::Down, Net::ED2K, 10000, 0);   // 25x the limit
        QCOMPARE(l.total(RateLimiter::Down, Net::ED2K), qint64(500000));                      // all of it counted
        QElapsedTimer t;
        t.start();
        int grants = 0;
        while (t.elapsed() < 1100) {
            if (l.acquire(RateLimiter::Down, 1000) > 0) ++grants;
            QThread::msleep(5);
        }
        QVERIFY2(grants >= 3 && grants <= 6, qPrintable(QString::number(grants)));   // one per 250 ms
        // upload at its minimum: a reply still goes out at once
        l.accountTraffic(RateLimiter::Up, Net::ED2K, 100000, 0);
        QVERIFY(l.acquire(RateLimiter::Up, 600) > 0 || (QTest::qWait(CoreRateLimiter::kTrickleMs + 20), l.acquire(RateLimiter::Up, 600) > 0));
    }

    void liveChangeAndUnlimited()
    {
        CoreRateLimiter l(nullptr);
        auto measure = [&l](int ms) {
            QElapsedTimer t;
            t.start();
            qint64 n = 0;
            while (t.elapsed() < ms) {
                n += l.acquire(RateLimiter::Up, 4096);
                QThread::msleep(2);
            }
            return double(n) * 1000.0 / ms;
        };
        l.setLimits(0, 50000);
        measure(300);                                       // drain the start
        const double a = measure(1000);
        l.setLimits(0, 200000);                             // takes effect at once
        const double b = measure(1000);
        l.setLimits(0, 0);
        QCOMPARE(l.acquire(RateLimiter::Up, 1 << 30), qint64(1) << 30);
        QVERIFY2(a > 40000 && a < 60000, qPrintable(QString::number(a)));
        QVERIFY2(b > 160000 && b < 240000, qPrintable(QString::number(b)));
    }

    void lanPeersExempt()
    {
        CoreRateLimiter l(nullptr);
        l.setLimits(100000, 100000);
        QVERIFY(l.exempt(QHostAddress(QStringLiteral("192.168.1.20"))));
        QVERIFY(l.exempt(QHostAddress(QStringLiteral("10.1.2.3"))));
        QVERIFY(l.exempt(QHostAddress(QStringLiteral("172.20.0.9"))));
        QVERIFY(l.exempt(QHostAddress(QStringLiteral("127.0.0.1"))));
        QVERIFY(l.exempt(QHostAddress(QStringLiteral("::1"))));
        QVERIFY(l.exempt(QHostAddress(QStringLiteral("fe80::1"))));
        QVERIFY(l.exempt(QHostAddress(QStringLiteral("fd00::5"))));
        QVERIFY(l.exempt(QHostAddress(QStringLiteral("::ffff:192.168.0.4"))));
        QVERIFY(!l.exempt(QHostAddress(QStringLiteral("203.0.113.7"))));      // documentation range
        QVERIFY(!l.exempt(QHostAddress(QStringLiteral("172.32.0.1"))));
        QVERIFY(!l.exempt(QHostAddress(QStringLiteral("2001:db8::1"))));
        QVERIFY(!l.exempt(QHostAddress()));
        l.accountExempt(RateLimiter::Up, Net::Gnutella, 500000, 500000);   // counted, not charged
        QCOMPARE(l.chargedTotal(RateLimiter::Up), qint64(0));
        QCOMPARE(l.total(RateLimiter::Up, Net::Gnutella), qint64(500000));
        QCOMPARE(l.exemptTotal(RateLimiter::Up), qint64(500000));
        l.setLimitLan(true);                               // "Apply limits to LAN peers"
        QVERIFY(!l.exempt(QHostAddress(QStringLiteral("192.168.1.20"))));
        // with settings
        SimpleSettings s;
        CoreRateLimiter m(&s);
        QVERIFY(m.exempt(QHostAddress(QStringLiteral("192.168.1.20"))));
        s.setValue(keys::bandwidth::LimitLan, true);
        QVERIFY(!m.exempt(QHostAddress(QStringLiteral("192.168.1.20"))));
    }

    void uploadMinimum()
    {
        SimpleSettings s;
        CoreRateLimiter l(&s);
        s.setValue(keys::bandwidth::UpLimitKBs, 3);
        QCOMPARE(l.limitBps(RateLimiter::Up), bwlimit::kMinUploadKBs * 1024);
        s.setValue(keys::bandwidth::UpLimitKBs, 0);
        QCOMPARE(l.limitBps(RateLimiter::Up), 0);
        s.setValue(keys::bandwidth::UpLimitKBs, 40);
        QCOMPARE(l.limitBps(RateLimiter::Up), 40 * 1024);
    }

    void scheduleWindows()
    {
        const QString a = QStringLiteral("08:00"), b = QStringLiteral("18:00");
        const QDateTime mon0900(QDate(2026, 11, 23), QTime(9, 0));       // a Monday
        QCOMPARE(bwlimit::windowEnd(mon0900, a, b), QDateTime(QDate(2026, 11, 23), QTime(18, 0)));
        QCOMPARE(bwlimit::nextStart(mon0900, a, b, 31), QDateTime(QDate(2026, 11, 24), QTime(8, 0)));
        const QDateTime fri1900(QDate(2026, 11, 27), QTime(19, 0));
        QCOMPARE(bwlimit::nextStart(fri1900, a, b, 31), QDateTime(QDate(2026, 11, 30), QTime(8, 0)));   // Mon-Fri
        QVERIFY(!bwlimit::nextStart(fri1900, a, b, 0).isValid());
        // overnight 22:00-06:00: at 23:00 the window ends tomorrow 06:00, at 03:00 today 06:00
        const QString n1 = QStringLiteral("22:00"), n2 = QStringLiteral("06:00");
        QCOMPARE(bwlimit::windowEnd(QDateTime(QDate(2026, 11, 23), QTime(23, 0)), n1, n2), QDateTime(QDate(2026, 11, 24), QTime(6, 0)));
        QCOMPARE(bwlimit::windowEnd(QDateTime(QDate(2026, 11, 24), QTime(3, 0)), n1, n2), QDateTime(QDate(2026, 11, 24), QTime(6, 0)));
    }

    // who wins: the schedule inside its window; a normal limit changed during the window wins until
    // the window ends; changing the schedule ends that at once
    void scheduleAndManualDoNotFight()
    {
        if (QTime::currentTime() >= QTime(23, 58))
            QSKIP("too close to midnight for an always-on window");
        SimpleSettings s;
        CoreRateLimiter l(&s);
        s.setValue(keys::bandwidth::DownLimitKBs, 50);
        s.setValue(keys::bandwidth::ScheduleStart, QStringLiteral("00:00"));
        s.setValue(keys::bandwidth::ScheduleEnd, QStringLiteral("23:59"));
        s.setValue(keys::bandwidth::ScheduleDays, 127);
        s.setValue(keys::bandwidth::ScheduleDownKBs, 7);
        s.setValue(keys::bandwidth::ScheduleUpKBs, 0);
        s.setValue(keys::bandwidth::ScheduleEnabled, true);
        QCOMPARE(l.limitBps(RateLimiter::Down), 7 * 1024);
        QCOMPARE(l.inForce().source, bwlimit::InForce::Schedule);
        QCOMPARE(l.limitBps(RateLimiter::Up), 0);                        // 0 = no scheduled limit
        // the user picks 300 KB/s during the window (tray, Bandwidth page, Settings)
        s.setValue(keys::bandwidth::DownLimitKBs, 300);
        QCOMPARE(l.limitBps(RateLimiter::Down), 300 * 1024);
        QCOMPARE(l.inForce().source, bwlimit::InForce::Manual);
        QCOMPARE(l.inForce().until.time(), QTime(23, 59));
        QVERIFY(!s.s(keys::bandwidth::ScheduleOverrideUntil).isEmpty());
        // an override that has ended: the schedule again
        s.setValue(keys::bandwidth::ScheduleOverrideUntil, QDateTime::currentDateTime().addSecs(-5).toString(Qt::ISODate));
        QCOMPARE(l.limitBps(RateLimiter::Down), 7 * 1024);
        s.setValue(keys::bandwidth::DownLimitKBs, 400);
        QCOMPARE(l.limitBps(RateLimiter::Down), 400 * 1024);
        // editing the schedule hands control back to it
        s.setValue(keys::bandwidth::ScheduleDownKBs, 8);
        QCOMPARE(s.s(keys::bandwidth::ScheduleOverrideUntil), QString());
        QCOMPARE(l.limitBps(RateLimiter::Down), 8 * 1024);
        s.setValue(keys::bandwidth::ScheduleEnabled, false);
        QCOMPARE(l.limitBps(RateLimiter::Down), 400 * 1024);
        QCOMPARE(l.inForce().source, bwlimit::InForce::Normal);
    }

    // a window that starts in a moment switches the limits at that moment (not up to 30 s later)
    void scheduleSwitchesOnTime()
    {
        // the window starts at the next full minute (at most a minute of waiting)
        const QDateTime now = QDateTime::currentDateTime();
        const QTime t = now.time();
        const QTime start = QTime(t.hour(), t.minute()).addSecs(60), end = start.addSecs(60);
        if (start < t || end < start)
            QSKIP("too close to midnight");
        SimpleSettings s;
        CoreRateLimiter l(&s);
        s.setValue(keys::bandwidth::ScheduleDays, 127);
        s.setValue(keys::bandwidth::ScheduleDownKBs, 11);
        s.setValue(keys::bandwidth::ScheduleStart, start.toString(QStringLiteral("HH:mm")));
        s.setValue(keys::bandwidth::ScheduleEnd, end.toString(QStringLiteral("HH:mm")));
        s.setValue(keys::bandwidth::ScheduleEnabled, true);
        QCOMPARE(l.limitBps(RateLimiter::Down), 0);
        QTRY_COMPARE_WITH_TIMEOUT(l.limitBps(RateLimiter::Down), 11 * 1024, 62000);
        const qint64 late = QDateTime(now.date(), start).msecsTo(QDateTime::currentDateTime());
        QVERIFY2(late < 1500, qPrintable(QString::number(late)));   // within a second of the time
    }

    void bandwidthService()
    {
        QTemporaryDir dir;
        Database db;
        QVERIFY(db.open(dir.filePath(QStringLiteral("octomx.db"))));
        CoreRateLimiter l(nullptr);
        {
            CoreBandwidth bw(&l, &db);
            bw.start();
            QSignalSpy spy(&bw, &BandwidthService::sample);
            l.account(RateLimiter::Down, Net::BitTorrent, 1000000);
            l.account(RateLimiter::Up, Net::ED2K, 300000);
            QVERIFY(spy.wait(2500));                       // 1 Hz timer
            const BandwidthSample s = spy.at(0).at(0).value<BandwidthSample>();
            QVERIFY(s.downBps > 300000);                   // ~1 MB in ≤ ~1 s
            QVERIFY(s.upBps > 100000);
            QCOMPARE(bw.totalDownloaded(), 1000000);
            QCOMPARE(bw.totalUploaded(), 300000);
            QCOMPARE(bw.allTimeDownloaded(), 1000000);
            for (int i = 0; i < 5; ++i) bw.tickNow();
            QCOMPARE(bw.history(3).size(), 3);
            QVERIFY(bw.history(600).size() >= 6);
            QCOMPARE(bw.current().downBps, 0);             // idle now
            bw.shutdown();
        }
        {
            CoreBandwidth bw(&l, &db);                    // all-time totals survive a restart
            bw.start();
            QCOMPARE(bw.totalDownloaded(), 0);
            QCOMPARE(bw.allTimeDownloaded(), 1000000);
            QCOMPARE(bw.allTimeUploaded(), 300000);
            l.account(RateLimiter::Down, Net::Http, 500);
            QCOMPARE(bw.allTimeDownloaded(), 1000500);
            bw.shutdown();
        }
    }
};

QTEST_GUILESS_MAIN(TestRateLimiter)
#include "tst_ratelimiter.moc"
