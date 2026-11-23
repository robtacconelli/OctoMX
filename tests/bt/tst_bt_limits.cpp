// SPDX-License-Identifier: GPL-3.0-or-later
// [bw-1] libtorrent inside the core's one shared budget: a seeder on 127.0.0.1 with the core's
// CoreRateLimiter (LAN peers limited too, bandwidth/limitLan) uploads to a second session.
//   * the upload limit holds (measured as the downloader's progress);
//   * a change applies within a couple of seconds (the plugin polls budgetBps() once per second);
//   * a second, non-BitTorrent uploader in the same limiter (acquire, as eD2k / Gnutella /
//     overlay uploads do): together they stay at the limit and share it;
//   * unlimited is unlimited.
// [bw-2] two real transfers through one download budget: libtorrent (a seeder on 127.0.0.1) and the
// HTTP plugin (a local HTTP server) at once. The limit holds in every second, not only on average.
#include <QtCore/QElapsedTimer>
#include <QtCore/QTimer>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

#include <memory>

#include "bt_test_util.h"
#include "core/rate_limiter.h"
#ifdef MX_HAVE_HTTP
#include "http_plugin.h"
#endif
#include "mx/simple_settings.h"

namespace {
// a plain HTTP/1.1 server for one file: HEAD, GET, Range, keep-alive, as fast as the socket takes it
class FileServer : public QTcpServer {
public:
    explicit FileServer(const QString &path) : m_path(path)
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *s = nextPendingConnection()) serve(s);
        });
    }

private:
    struct Conn { QByteArray in; std::unique_ptr<QFile> f; qint64 left = 0; };
    void serve(QTcpSocket *s)
    {
        auto c = std::make_shared<Conn>();
        auto pump = [s, c] {
            while (c->left > 0 && s->bytesToWrite() < 1024 * 1024) {
                const QByteArray b = c->f->read(qMin<qint64>(c->left, 64 * 1024));
                if (b.isEmpty()) { c->left = 0; break; }
                c->left -= b.size();
                s->write(b);
            }
        };
        connect(s, &QTcpSocket::bytesWritten, s, pump);
        connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
        connect(s, &QTcpSocket::readyRead, s, [this, s, c, pump] {
            c->in += s->readAll();
            for (;;) {
                const int end = c->in.indexOf("\r\n\r\n");
                if (end < 0 || c->left > 0) return;
                const QList<QByteArray> lines = c->in.left(end).split('\n');
                c->in.remove(0, end + 4);
                const bool head = lines.value(0).startsWith("HEAD");
                c->f = std::make_unique<QFile>(m_path);
                if (!c->f->open(QIODevice::ReadOnly)) { s->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n"); continue; }
                const qint64 size = c->f->size();
                qint64 from = 0, to = size - 1;
                bool range = false;
                for (const QByteArray &l : lines)
                    if (l.trimmed().toLower().startsWith("range: bytes=")) {
                        const QList<QByteArray> p = l.trimmed().mid(13).split('-');
                        from = p.value(0).toLongLong();
                        if (!p.value(1).isEmpty()) to = qMin(size - 1, p.value(1).toLongLong());
                        range = true;
                    }
                QByteArray h = range ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n";
                h += "Accept-Ranges: bytes\r\nContent-Type: application/octet-stream\r\nContent-Length: " + QByteArray::number(to - from + 1) + "\r\n";
                if (range) h += "Content-Range: bytes " + QByteArray::number(from) + '-' + QByteArray::number(to) + '/' + QByteArray::number(size) + "\r\n";
                s->write(h + "\r\n");
                if (head) continue;
                c->f->seek(from);
                c->left = to - from + 1;
                pump();
            }
        });
    }
    QString m_path;
};
} // namespace

using namespace bttest;

class TstBtLimits : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { mx::registerMetaTypes(); }
    void seederObeysSharedUploadLimit();
    void bitTorrentPlusHttpEverySecond();   // [bw-2]
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

// [bw-2] A leecher with the core's limiter downloads a torrent from a local seeder while the real
// HTTP plugin, in the same limiter, fetches another file from a local HTTP server (both sources could
// deliver many times the limit). Measured from the limiter's own totals every 100 ms (libtorrent's
// counters arrive every 100 ms, so there are no piece-sized steps in it): the total of EVERY one-
// second window (in 100 ms steps) stays near the limit, and both networks get a fair part.
void TstBtLimits::bitTorrentPlusHttpEverySecond()
{
#ifndef MX_HAVE_HTTP
    QSKIP("built without the HTTP plugin");
#else
    const QString root = freshDir(QStringLiteral("limits-mix"));
    const QString src = root + QStringLiteral("/share/octomx-bw2-bt-64M.bin");
    const QString web = root + QStringLiteral("/web/octomx-bw2-http-64M.bin");
    makeFile(src, 64 * 1024 * 1024, 11);
    makeFile(web, 64 * 1024 * 1024, 12);

    mx::CoreRateLimiter limiter(nullptr);
    limiter.setLimitLan(true);           // 127.0.0.1 is limited like the internet
    const int limit = 400 * 1024;
    limiter.setLimits(limit, 0);

    Node a(root + QStringLiteral("/a")), b(root + QStringLiteral("/b"), /*limitLan*/ true);
    b.limiter = &limiter;
    a.create();
    b.create();
    QVERIFY(a.start());
    QVERIFY(b.start());
    QString err;
    QVERIFY2(a.be->seedLocalFile(src, &err), qPrintable(err));
    QVERIFY(QTest::qWaitFor([&] { return !a.seeded.isEmpty(); }, 60000));
    const QString id = a.seeded.begin().value();
    QVERIFY(QTest::qWaitFor([&] { return a.snap(id).state == mx::TransferState::Seeding; }, 20000));

    FileServer server(web);
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    mx::SimpleSettings hs;
    std::unique_ptr<mx::NetworkPlugin> http(mx::createHttpPlugin());
    mx::PluginContext hctx;
    hctx.settings = &hs;
    hctx.limiter = &limiter;
    hctx.profileDir = root + QStringLiteral("/http");
    hctx.dataDir = hctx.profileDir;
    http->init(hctx);
    http->start();
    mx::SearchSource ss;
    ss.net = mx::Net::Http;
    ss.uri = QStringLiteral("http://127.0.0.1:%1/octomx-bw2-http-64M.bin").arg(server.serverPort());
    const qint64 webSize = QFileInfo(web).size();
    QPointer<mx::RangeSource> rs = http->openSource(QStringLiteral("mix"), {}, webSize, ss);
    QVERIFY(rs);
    qint64 next = 0, httpBytes = 0;
    int pending = 0;
    auto feed = [&] {   // what the engine does: ranges of the preferred size, as many as allowed
        while (rs && rs->state() != mx::RangeSource::Failed && rs->state() != mx::RangeSource::Closed
               && (rs->state() == mx::RangeSource::Ready || rs->state() == mx::RangeSource::Busy)
               && pending < rs->maxPendingRequests() && next < webSize) {
            const qint64 len = qMin(rs->preferredRequestSize(), webSize - next);
            ++pending;
            rs->request(next, len);
            next += len;
        }
    };
    connect(rs, &mx::RangeSource::data, this, [&](qint64, const QByteArray &d) { httpBytes += d.size(); });
    connect(rs, &mx::RangeSource::requestFinished, this, [&](qint64, qint64, bool, const QString &) { --pending; feed(); });
    connect(rs, &mx::RangeSource::stateChanged, this, [&] { feed(); });

    const QString dl = root + QStringLiteral("/b-dl");
    QCOMPARE(b.be->addTorrent(QStringLiteral("magnet:?xt=urn:btih:%1&dn=x").arg(id), {}, dl, false, {}, &err), id);
    b.ctl->addPeer(id, a.endpoint());
    QVERIFY(QTest::qWaitFor([&] { return b.metadata.contains(id); }, 20000));
    b.be->resumeTorrent(id);
    b.ctl->addPeer(id, a.endpoint());
    rs->open();
    QVERIFY(QTest::qWaitFor([&] { return b.snap(id).totalDone > 0 && httpBytes > 0; }, 30000));
    QTest::qWait(6000);   // both running, shares settled, libtorrent's peer out of its slow start

    // the limiter's totals every 100 ms for 14 s
    QList<qint64> all, bt, ht;
    QElapsedTimer t;
    t.start();
    for (int i = 0; i <= 140; ++i) {
        all << limiter.total(mx::RateLimiter::Down);
        bt << limiter.total(mx::RateLimiter::Down, mx::Net::BitTorrent);
        ht << limiter.total(mx::RateLimiter::Down, mx::Net::Http);
        if (i % 10 == 0 && i > 0)
            qInfo("  %2d s: BitTorrent %4lld KiB/s, HTTP %4lld KiB/s, BitTorrent budget %4d KiB/s, share HTTP %4d KiB/s", i / 10,
                  (bt[i] - bt[i - 10]) / 1024, (ht[i] - ht[i - 10]) / 1024, limiter.budgetBps(mx::RateLimiter::Down, mx::Net::BitTorrent) / 1024,
                  limiter.shareBps(mx::RateLimiter::Down, mx::Net::Http) / 1024);
        QTest::qWait(int(qMax<qint64>(1, (i + 1) * 100 - t.elapsed())));
    }
    const double secs = t.elapsed() / 1000.0;
    QList<double> win;   // every one-second window, in 100 ms steps
    for (int i = 0; i + 10 < all.size(); ++i) win << double(all[i + 10] - all[i]) / limit;
    std::sort(win.begin(), win.end());
    const double mean = double(all.last() - all.first()) / secs / limit;
    const double p90 = win[int(win.size() * 0.9)], top = win.last(), mn = win.first();
    const double btShare = double(bt.last() - bt.first()) / secs / limit, httpShare = double(ht.last() - ht.first()) / secs / limit;
    const QString msg = QStringLiteral("limit %1 KiB/s: mean %2 %, one-second windows min %3 % p90 %4 % max %5 %; BitTorrent %6 %, HTTP %7 %")
                            .arg(limit / 1024).arg(mean * 100, 0, 'f', 1).arg(mn * 100, 0, 'f', 1).arg(p90 * 100, 0, 'f', 1)
                            .arg(top * 100, 0, 'f', 1).arg(btShare * 100, 0, 'f', 1).arg(httpShare * 100, 0, 'f', 1);
    qInfo("BitTorrent + HTTP through one budget, %s", qPrintable(msg));
    QVERIFY2(mean > 0.9 && mean < 1.05, qPrintable(msg));
    QVERIFY2(p90 < 1.12 && top < 1.25, qPrintable(msg));   // before bw-2: max about 2.1 (a comb)
    QVERIFY2(btShare > 0.3 && httpShare > 0.3, qPrintable(msg));
    if (rs) rs->close();
    http->stop();
#endif
}

QTEST_MAIN(TstBtLimits)
#include "tst_bt_limits.moc"
