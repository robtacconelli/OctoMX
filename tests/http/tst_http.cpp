// SPDX-License-Identifier: GPL-3.0-or-later
// Tests of the HTTP plugin (mxnet_http) against an in-process HTTP server (test_server.cpp).
// [net-2] settings proven here (tests/ui/settings_registry.h): "http/maxRetries" (transient5xxGivesUp:
// 2 retries, then the source fails), "http/stallTimeoutSec" (stallRetry: 2 s instead of 30 s, the
// stalled request is restarted where it stopped).
#include <QtCore/QCryptographicHash>
#include <QtCore/QElapsedTimer>
#include <QtCore/QMutex>
#include <QtCore/QTemporaryFile>
#include <QtNetwork/QNetworkReply>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>
#include <memory>

#include "http_plugin.h"
#include "http_source.h"
#include "http_util.h"
#include "mini_scheduler.h"
#include "mx/simple_settings.h"
#include "test_server.h"

using namespace mx;
using mxtest::TestServer;

namespace {

// Token bucket with the same semantics as the core's CoreRateLimiter.
class TestLimiter : public RateLimiter {
public:
    qint64 acquire(Dir d, qint64 bytes) override
    {
        QMutexLocker l(&m);
        if (d != Down || rate <= 0) return bytes;
        const qint64 now = clock.nsecsElapsed();
        const double cap = qMax<double>(rate, 4096.0);
        tokens = qMin(cap, tokens + double(rate) * double(now - last) / 1e9);
        last = now;
        const qint64 n = qMin<qint64>(bytes, qint64(tokens));
        tokens -= n;
        return n;
    }
    void account(Dir d, Net net, qint64 bytes) override
    {
        QMutexLocker l(&m);
        if (d == Down && net == Net::Http) accounted += bytes;
    }
    int limitBps(Dir) const override { return int(rate); }
    void setRate(qint64 r)
    {
        QMutexLocker l(&m);
        rate = r;
        tokens = 0;
        clock.start();
        last = 0;
    }
    QMutex m;
    qint64 rate = 0;
    double tokens = 0;
    qint64 last = 0;
    QElapsedTimer clock;
    qint64 accounted = 0;
};

struct Collector {
    explicit Collector(RangeSource *s)
    {
        QObject::connect(s, &RangeSource::data, s, [this](qint64 off, const QByteArray &b) {
            if (!data.isEmpty() && off != nextPos) ++gaps;
            if (data.isEmpty()) firstOffset = off;
            data += b;
            nextPos = off + b.size();
            ++chunks;
            maxChunk = qMax<qint64>(maxChunk, b.size());
        });
        QObject::connect(s, &RangeSource::requestFinished, s, [this](qint64 off, qint64 len, bool ok, const QString &err) {
            finished << Fin{off, len, ok, err};
        });
        QObject::connect(s, &RangeSource::stateChanged, s, [this, s] {
            states << s->state();
            texts << s->stateText();
        });
    }
    struct Fin { qint64 off, len; bool ok; QString err; };
    QByteArray data;
    qint64 firstOffset = -1, nextPos = 0, maxChunk = 0;
    int gaps = 0, chunks = 0;
    QList<Fin> finished;
    QList<RangeSource::State> states;
    QStringList texts;
};

QByteArray expected(const QByteArray &path, qint64 off, qint64 n, int version = 0)
{
    return mxtest::generated(mxtest::seedFor(path, version), off, n);
}

} // namespace

class TstHttp : public QObject {
    Q_OBJECT
private:
    std::unique_ptr<TestServer> srv;
    std::unique_ptr<SimpleSettings> settings;
    TestLimiter limiter;
    NetworkPlugin *plugin = nullptr;

    QString url(const QByteArray &pathAndQuery) const { return QString::fromLatin1(srv->base() + pathAndQuery); }

    RangeSource *open(const QString &u, qint64 size, FileHashes h = {}, QVariantMap extra = {})
    {
        SearchSource ss;
        ss.net = Net::Http;
        ss.uri = u;
        ss.extra = extra;
        RangeSource *s = plugin->openSource(QStringLiteral("dl1"), h, size, ss);
        if (s) s->open();
        return s;
    }
    static bool settled(RangeSource *s)
    {
        const auto st = s->state();
        return st == RangeSource::Ready || st == RangeSource::Failed;
    }

private slots:
    void initTestCase()
    {
        srv = std::make_unique<TestServer>();
        settings = std::make_unique<SimpleSettings>();
        settings->setValue(QLatin1String(httpkeys::RetryBaseMs), 100);
        settings->setValue(QLatin1String(httpkeys::StallTimeoutSec), 2);
        settings->setValue(keys::http::TimeoutSec, 5);
        plugin = createHttpPlugin(this);
        PluginContext ctx;
        ctx.settings = settings.get();
        ctx.limiter = &limiter;
        plugin->init(ctx);
        plugin->start();
        QCOMPARE(plugin->net(), Net::Http);
        QCOMPARE(plugin->capabilities(), int(NetworkPlugin::CanFetchRanges));
        QCOMPARE(plugin->status().state, ConnState::Connected);
        QCOMPARE(plugin->status().statusText, QStringLiteral("Ready"));
        QVERIFY(plugin->servers().isEmpty());
    }
    void init()
    {
        // what the test before left behind (a cancelled request's body) is over before this one counts
        if (!QTest::qWaitFor([this] { return srv->active() == 0; }, 5000))
            qWarning("the test server still sends %d bodies of the test before:\n%s", srv->active(), srv->timeline().constData());
        srv->reset();
        limiter.setRate(0);
        settings->setValue(QLatin1String(httpkeys::ConnectionsPerSource), 4);
        settings->setValue(keys::http::MaxConnectionsPerServer, 4);
    }

    // ------------------------------------------------------------ parsers
    void contentRange()
    {
        auto cr = parseContentRange("bytes 0-99/1000");
        QVERIFY(cr.valid); QCOMPARE(cr.first, 0); QCOMPARE(cr.last, 99); QCOMPARE(cr.total, 1000);
        cr = parseContentRange("bytes 5-9/*");
        QVERIFY(cr.valid); QCOMPARE(cr.total, -1);
        cr = parseContentRange("bytes */1234");
        QVERIFY(cr.valid && cr.unsatisfied); QCOMPARE(cr.total, 1234);
        QVERIFY(!parseContentRange("bytes 9-5/100").valid);
        QVERIFY(!parseContentRange("bytes 0-100/100").valid);
        QVERIFY(!parseContentRange("garbage").valid);
    }
    void contentDisposition_data()
    {
        QTest::addColumn<QByteArray>("header");
        QTest::addColumn<QString>("name");
        QTest::newRow("quoted") << QByteArray("attachment; filename=\"debian 13.iso\"") << QStringLiteral("debian 13.iso");
        QTest::newRow("token") << QByteArray("attachment; filename=file.bin") << QStringLiteral("file.bin");
        QTest::newRow("rfc5987") << QByteArray("attachment; filename*=UTF-8''%e2%82%ac%20rates.txt") << QStringLiteral("€ rates.txt");
        QTest::newRow("both, ext wins") << QByteArray("attachment; filename=\"EURO rates.txt\"; filename*=utf-8''%e2%82%ac%20rates.txt")
                                        << QStringLiteral("€ rates.txt");
        QTest::newRow("latin1") << QByteArray("inline; filename*=iso-8859-1'en'%A3%20rates.txt") << QStringLiteral("£ rates.txt");
        QTest::newRow("escaped quote") << QByteArray("attachment; filename=\"a\\\"b.txt\"") << QStringLiteral("a_b.txt");
        QTest::newRow("semicolon in quotes") << QByteArray("attachment; filename=\"a;b.txt\"; size=3") << QStringLiteral("a;b.txt");
        QTest::newRow("traversal") << QByteArray("attachment; filename=\"../../etc/passwd\"") << QStringLiteral("passwd");
        QTest::newRow("windows path") << QByteArray("attachment; filename=\"C:\\\\x\\\\y.exe\"") << QStringLiteral("y.exe");
        QTest::newRow("raw utf8") << QByteArray("attachment; filename=\"m\xc3\xbcsli.txt\"") << QStringLiteral("müsli.txt");
        QTest::newRow("none") << QByteArray("inline") << QString();
    }
    void contentDisposition()
    {
        QFETCH(QByteArray, header);
        QFETCH(QString, name);
        QCOMPARE(parseContentDisposition(header), name);
    }
    void retryAfterLinkDigest()
    {
        QCOMPARE(parseRetryAfter("30"), 30);
        QCOMPARE(parseRetryAfter(""), -1);
        const QDateTime now = QDateTime(QDate(2015, 10, 21), QTime(7, 27, 0), QTimeZone::UTC);
        QCOMPARE(parseRetryAfter("Wed, 21 Oct 2015 07:28:00 GMT", now), 60);
        const QStringList l = parseDuplicateLinks(
            "<http://a.example/x.iso>; rel=duplicate; pri=1, <x.meta4>; rel=describedby, <//b.example/x.iso>; rel=\"duplicate\"",
            QUrl(QStringLiteral("https://main.example/dir/x.iso")));
        QCOMPARE(l, QStringList({QStringLiteral("http://a.example/x.iso"), QStringLiteral("https://b.example/x.iso")}));
        QVERIFY(looksLikeTorrentData("d8:announce35:udp://tracker"));
        QVERIFY(looksLikeTorrentData("d4:infod6:length"));
        QVERIFY(looksLikeTorrentData("d13:creation datei1e"));
        QVERIFY(!looksLikeTorrentData("<html>"));
        QVERIFY(!looksLikeTorrentData("dump of things"));
    }
    void urls()
    {
        const QUrl u = HttpProbe::normalizeUrl(QStringLiteral("http://bücher.example/a b/ä.iso"));
        QCOMPARE(u.toString(QUrl::FullyEncoded), QStringLiteral("http://xn--bcher-kva.example/a%20b/%C3%A4.iso"));
        QCOMPARE(fileNameFromUrl(u), QStringLiteral("ä.iso"));
        QCOMPARE(fileNameFromUrl(QUrl(QStringLiteral("https://x.org/dl/file%20name%2Bplus.zip?x=1"))), QStringLiteral("file name+plus.zip"));
        QCOMPARE(HttpProbe::normalizeUrl(QStringLiteral("www.example.org/x.iso")).toString(), QStringLiteral("http://www.example.org/x.iso"));
        QVERIFY(!HttpProbe::normalizeUrl(QStringLiteral("ftp://example.org/x")).isValid());
        QVERIFY(!HttpProbe::normalizeUrl(QStringLiteral("magnet:?xt=urn:btih:abc")).isValid());
        QUrl c = QUrl(QStringLiteral("https://us%40r:p%3Ass@host.example/f"));
        const HttpCredentials cred = HttpCredentials::take(&c);
        QCOMPARE(c.toString(), QStringLiteral("https://host.example/f"));
        QCOMPARE(cred.basic, QByteArray("Basic ") + QByteArray("us@r:p:ss").toBase64());
        QVERIFY(cred.appliesTo(QUrl(QStringLiteral("https://host.example/other"))));
        QVERIFY(!cred.appliesTo(QUrl(QStringLiteral("https://evil.example/f"))));
        QVERIFY(!cred.appliesTo(QUrl(QStringLiteral("http://host.example/f"))));
        // redirect rules
        HttpConfig cfg;
        RedirectTracker t;
        QString err;
        QVERIFY(!t.next(QUrl(QStringLiteral("https://a.example/x")), QByteArray("http://b.example/y"), cfg, &err).isValid());
        QVERIFY2(err.contains(QLatin1String("insecure")), qPrintable(err));
        cfg.allowInsecureRedirect = true;
        t.reset();
        QCOMPARE(t.next(QUrl(QStringLiteral("https://a.example/x")), QByteArray("http://b.example/y"), cfg, &err).toString(),
                 QStringLiteral("http://b.example/y"));
        t.reset();
        QCOMPARE(t.next(QUrl(QStringLiteral("https://a.example/d/x")), QByteArray("../y z"), cfg, &err).toString(QUrl::FullyEncoded),
                 QStringLiteral("https://a.example/y%20z"));
        QVERIFY(!t.next(QUrl(QStringLiteral("https://a.example/y%20z")), QByteArray("ftp://x/y"), cfg, &err).isValid());
    }

    // ------------------------------------------------------------ HttpProbe
    void probeBasic()
    {
        HttpProbe p(settings.get());
        QSignalSpy spy(&p, &HttpProbe::finished);
        p.probe(url("/f/basic.iso?size=1000&lm=1"));
        QVERIFY(spy.wait(5000));
        const auto r = spy.at(0).at(0).value<HttpProbeResult>();
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.size, 1000);
        QVERIFY(r.supportsRanges);
        QCOMPARE(r.fileName, QStringLiteral("basic.iso"));
        QCOMPARE(r.contentType, QStringLiteral("application/octet-stream"));
        QCOMPARE(r.etag, QStringLiteral("\"v0-1000\""));
        QCOMPARE(r.lastModified, QStringLiteral("Mon, 01 Jan 2024 00:00:00 GMT"));
        QVERIFY(!r.isTorrentFile);
        QCOMPARE(srv->count("HEAD", "/f/basic.iso"), 1);
        // user agent from the settings
        settings->setValue(keys::http::UserAgent, QStringLiteral("TestAgent/1"));
        HttpProbe p2(settings.get());
        QSignalSpy spy2(&p2, &HttpProbe::finished);
        p2.probe(url("/f/ua.bin?size=10"));
        QVERIFY(spy2.wait(5000));
        settings->setValue(keys::http::UserAgent, QStringLiteral("OctoMX/" MX_VERSION_STRING));
        QCOMPARE(srv->log().last().userAgent, QByteArray("TestAgent/1"));
    }
    void probeHeadRefused()
    {
        HttpProbe p(settings.get());
        QSignalSpy spy(&p, &HttpProbe::finished);
        p.probe(url("/f/nohead.bin?size=5000&nohead=1"));
        QVERIFY(spy.wait(5000));
        const auto r = spy.at(0).at(0).value<HttpProbeResult>();
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.size, 5000);
        QVERIFY(r.supportsRanges);
        QCOMPARE(srv->count("GET", "/f/nohead.bin"), 1);
        QVERIFY(srv->log().last().range.startsWith("bytes=0-"));
    }
    void probeNoRanges()
    {
        HttpProbe p(settings.get());
        QSignalSpy spy(&p, &HttpProbe::finished);
        p.probe(url("/f/nr.bin?size=300000&noranges=1"));
        QVERIFY(spy.wait(5000));
        const auto r = spy.at(0).at(0).value<HttpProbeResult>();
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.size, 300000);
        QVERIFY(!r.supportsRanges);
    }
    void probeTorrentSniff()
    {
        HttpProbe p(settings.get());
        QSignalSpy spy(&p, &HttpProbe::finished);
        p.probe(url("/f/download.php?torrent=1"));
        QVERIFY(spy.wait(5000));
        auto r = spy.at(0).at(0).value<HttpProbeResult>();
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(r.isTorrentFile);
        // by content type / by suffix
        p.probe(url("/f/x.bin?size=10&ctype=application/x-bittorrent"));
        QVERIFY(spy.wait(5000));
        QVERIFY(spy.at(1).at(0).value<HttpProbeResult>().isTorrentFile);
        p.probe(url("/f/x.torrent?size=10"));
        QVERIFY(spy.wait(5000));
        QVERIFY(spy.at(2).at(0).value<HttpProbeResult>().isTorrentFile);
    }
    void probeContentDispositionServer()
    {
        HttpProbe p(settings.get());
        QSignalSpy spy(&p, &HttpProbe::finished);
        p.probe(url("/f/get?size=10&cd=attachment%3B%20filename%2A%3DUTF-8%27%27%25e2%2582%25ac%2520rates.txt"));
        QVERIFY(spy.wait(5000));
        QCOMPARE(spy.at(0).at(0).value<HttpProbeResult>().fileName, QStringLiteral("€ rates.txt"));
    }
    void probeErrorsAndRedirects()
    {
        HttpProbe p(settings.get());
        QSignalSpy spy(&p, &HttpProbe::finished);
        p.probe(url("/status/404"));
        QVERIFY(spy.wait(5000));
        auto r = spy.takeFirst().at(0).value<HttpProbeResult>();
        QVERIFY(!r.ok);
        QCOMPARE(r.httpStatus, 404);
        QVERIFY2(r.error.contains(QLatin1String("404")), qPrintable(r.error));

        p.probe(url("/redir/3/f/target.bin?size=77"));
        QVERIFY(spy.wait(5000));
        r = spy.takeFirst().at(0).value<HttpProbeResult>();
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.size, 77);
        QCOMPARE(r.finalUrl.path(), QStringLiteral("/f/target.bin"));
        QCOMPARE(r.fileName, QStringLiteral("target.bin"));

        p.probe(url("/loop/a"));
        QVERIFY(spy.wait(5000));
        r = spy.takeFirst().at(0).value<HttpProbeResult>();
        QVERIFY(!r.ok);
        QVERIFY2(r.error.contains(QLatin1String("loop")), qPrintable(r.error));

        p.probe(url("/redir/12/f/far.bin"));
        QVERIFY(spy.wait(5000));
        r = spy.takeFirst().at(0).value<HttpProbeResult>();
        QVERIFY(!r.ok);
        QVERIFY2(r.error.contains(QLatin1String("Too many redirects")), qPrintable(r.error));

        p.probe(url("/utf?size=9"));          // Location with a raw space and raw UTF-8
        QVERIFY(spy.wait(5000));
        r = spy.takeFirst().at(0).value<HttpProbeResult>();
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.finalUrl.path(QUrl::FullyEncoded), QStringLiteral("/f/datei%20mit%20%C3%BC.bin"));
        QCOMPARE(r.fileName, QStringLiteral("datei mit ü.bin"));

        p.probe(url("/f/busy.bin?size=9&busy=5&retryafter=42"));
        QVERIFY(spy.wait(5000));
        r = spy.takeFirst().at(0).value<HttpProbeResult>();
        QVERIFY(!r.ok);
        QCOMPARE(r.httpStatus, 503);
        QCOMPARE(r.retryAfterSec, 42);
    }
    void probeHints()
    {
        HttpProbe p(settings.get());
        QSignalSpy spy(&p, &HttpProbe::finished);
        p.probe(url("/f/hint.bin?size=4096&link=1&digest=1"));
        QVERIFY(spy.wait(5000));
        const auto r = spy.at(0).at(0).value<HttpProbeResult>();
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.mirrors.size(), 2);
        QVERIFY(r.mirrors.at(0).endsWith(QLatin1String("/f/mirror1.bin")));
        const QByteArray all = expected("/f/hint.bin", 0, 4096);
        QCOMPARE(r.hashes.sha256, QCryptographicHash::hash(all, QCryptographicHash::Sha256));
        QCOMPARE(r.hashes.md5, QCryptographicHash::hash(all, QCryptographicHash::Md5));
        QCOMPARE(r.hashes.sha1, QCryptographicHash::hash(all, QCryptographicHash::Sha1));
        // the plugin reports them for the download id when a source opens
        QSignalSpy found(plugin, &NetworkPlugin::sourcesFound);
        QSignalSpy learned(plugin, &NetworkPlugin::hashesLearned);
        std::unique_ptr<RangeSource> s(open(url("/f/hint.bin?size=4096&link=1&digest=1"), 4096));
        QTRY_VERIFY_WITH_TIMEOUT(settled(s.get()), 5000);
        QCOMPARE(s->state(), RangeSource::Ready);
        QCOMPARE(found.size(), 1);
        QCOMPARE(found.at(0).at(0).toString(), QStringLiteral("dl1"));
        QCOMPARE(found.at(0).at(1).value<QList<SearchSource>>().size(), 2);
        QCOMPARE(learned.size(), 1);
        QCOMPARE(learned.at(0).at(1).value<FileHashes>().sha256, r.hashes.sha256);
        // a conflicting known hash → the source refuses the file
        FileHashes wrong;
        wrong.sha256 = QByteArray(32, 'x');
        std::unique_ptr<RangeSource> s2(open(url("/f/hint.bin?size=4096&link=1&digest=1"), 4096, wrong));
        QTRY_VERIFY_WITH_TIMEOUT(settled(s2.get()), 5000);
        QCOMPARE(s2->state(), RangeSource::Failed);
        QVERIFY2(s2->stateText().contains(QLatin1String("different hash")), qPrintable(s2->stateText()));
    }
    void fetchSmall()
    {
        HttpProbe p(settings.get());
        QSignalSpy spy(&p, &HttpProbe::fetched);
        p.fetchSmall(url("/redir/2/f/file.torrent?torrent=1"), 1 << 20);
        QVERIFY(spy.wait(5000));
        auto r = spy.at(0).at(0).value<HttpProbeResult>();
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(r.isTorrentFile);
        QVERIFY(spy.at(0).at(1).toByteArray().startsWith("d8:announce"));
        QCOMPARE(qint64(spy.at(0).at(1).toByteArray().size()), r.size);
        p.fetchSmall(url("/f/big.bin?size=100000"), 1000);
        QVERIFY(spy.wait(5000));
        r = spy.at(1).at(0).value<HttpProbeResult>();
        QVERIFY(!r.ok);
        QVERIFY2(r.error.contains(QLatin1String("too large")), qPrintable(r.error));
        QVERIFY(spy.at(1).at(1).toByteArray().isEmpty());
        p.fetchSmall(url("/f/big2.bin?size=100000&chunked=1"), 1000);   // no Content-Length: cap while reading
        QVERIFY(spy.wait(5000));
        QVERIFY(!spy.at(2).at(0).value<HttpProbeResult>().ok);
    }

    // ------------------------------------------------------------ RangeSource
    void rangeBasic()
    {
        const qint64 size = 1 << 20;
        std::unique_ptr<RangeSource> s(open(url("/f/basic.bin?size=1048576"), size));
        QVERIFY(s);
        QCOMPARE(s->state(), RangeSource::Connecting);
        Collector c(s.get());
        QTRY_VERIFY_WITH_TIMEOUT(settled(s.get()), 5000);
        QCOMPARE(s->state(), RangeSource::Ready);
        QCOMPARE(s->stateText(), QStringLiteral("Ready"));
        QCOMPARE(s->available(), (QList<QPair<qint64, qint64>>{{0, size}}));
        QCOMPARE(s->maxPendingRequests(), 4);
        QCOMPARE(s->preferredRequestSize(), 4 * 1024 * 1024);
        auto *info = qobject_cast<HttpSourceInfo *>(s.get());
        QVERIFY(info);
        QVERIFY(info->supportsRanges());
        QCOMPARE(info->remoteSize(), size);
        QCOMPARE(s->descriptor().extra.value(QStringLiteral("size")).toLongLong(), size);
        QCOMPARE(s->descriptor().extra.value(QStringLiteral("etag")).toString(), QStringLiteral("\"v0-1048576\""));
        s->request(500000, 300000);
        QCOMPARE(s->state(), RangeSource::Busy);
        QTRY_COMPARE_WITH_TIMEOUT(c.finished.size(), 1, 5000);
        QVERIFY2(c.finished[0].ok, qPrintable(c.finished[0].err));
        QCOMPARE(c.finished[0].off, 500000);
        QCOMPARE(c.finished[0].len, 300000);
        QCOMPARE(c.firstOffset, 500000);
        QCOMPARE(c.gaps, 0);
        QVERIFY(c.data == expected("/f/basic.bin", 500000, 300000));
        QCOMPARE(s->state(), RangeSource::Ready);
        QCOMPARE(limiter.accounted, 300000);
        const auto lg = srv->log();
        QCOMPARE(lg.last().range, QByteArray("bytes=500000-799999"));
        QCOMPARE(lg.last().ifRange, QByteArray("\"v0-1048576\""));
        // invalid requests fail fast (asynchronously)
        s->request(size - 10, 100);
        QTRY_COMPARE_WITH_TIMEOUT(c.finished.size(), 2, 2000);
        QVERIFY(!c.finished[1].ok);
        s->close();
        QCOMPARE(s->state(), RangeSource::Closed);
    }
    void parallel50MB()
    {
        const qint64 size = 50LL * 1024 * 1024;
        std::unique_ptr<RangeSource> s(open(url("/f/big50.bin?size=52428800"), size));
        QTRY_VERIFY_WITH_TIMEOUT(settled(s.get()), 5000);
        QCOMPARE(s->state(), RangeSource::Ready);
        QTemporaryFile f;
        QVERIFY(f.open());
        mxtest::MiniScheduler sched(s.get(), &f, size);
        QSignalSpy done(&sched, &mxtest::MiniScheduler::finished);
        sched.start();
        QVERIFY(done.wait(120000));
        QVERIFY2(sched.ok, qPrintable(sched.error));
        QCOMPARE(sched.maxInFlight, 4);
        QCOMPARE(sched.outOfOrder, 0);
        QVERIFY(sched.maxChunk <= 256 * 1024);
        QVERIFY(srv->maxConcurrent() >= 3);
        auto *hs = dynamic_cast<HttpRangeSource *>(s.get());
        QVERIFY(hs);
        qInfo("50 MB in %lld ms, peak buffered %lld bytes, max chunk %lld, connections %d", sched.elapsedMs(),
              hs->peakBufferedBytes(), hs->maxChunkEmitted(), hs->connectionsOpened());
        QVERIFY(hs->peakBufferedBytes() <= 4 * HttpRangeSource::kReadBuffer + 256 * 1024);
        QVERIFY(hs->connectionsOpened() >= 13);    // 13 chunks of 4 MiB
        // hash of what we assembled == hash of the generated file
        f.seek(0);
        QCryptographicHash got(QCryptographicHash::Sha256), want(QCryptographicHash::Sha256);
        QVERIFY(got.addData(&f));
        QCOMPARE(f.size(), size);
        const quint64 seed = mxtest::seedFor("/f/big50.bin");
        QByteArray buf(1 << 20, Qt::Uninitialized);
        for (qint64 off = 0; off < size; off += buf.size()) {
            mxtest::generate(seed, off, buf.data(), buf.size());
            want.addData(buf);
        }
        QCOMPARE(got.result().toHex(), want.result().toHex());
    }
    void noRangeServer()
    {
        const qint64 size = 300000;
        std::unique_ptr<RangeSource> s(open(url("/f/nr.bin?size=300000&noranges=1"), size));
        Collector c(s.get());
        QTRY_VERIFY_WITH_TIMEOUT(settled(s.get()), 5000);
        QCOMPARE(s->state(), RangeSource::Ready);
        QCOMPARE(s->stateText(), QStringLiteral("Ready (no range support)"));
        QVERIFY(!qobject_cast<HttpSourceInfo *>(s.get())->supportsRanges());
        QCOMPARE(s->descriptor().extra.value(QStringLiteral("supportsRanges")).toBool(), false);
        QCOMPARE(s->available(), (QList<QPair<qint64, qint64>>{{0, size}}));
        QCOMPARE(s->maxPendingRequests(), 1);
        QCOMPARE(s->preferredRequestSize(), size);
        s->request(1000, 5000);
        QCOMPARE(c.finished.size(), 0);           // never re-entrant
        QTRY_COMPARE_WITH_TIMEOUT(c.finished.size(), 1, 2000);
        QVERIFY(!c.finished[0].ok);
        QVERIFY(c.finished[0].err.startsWith(QLatin1String(kHttpErrorNoRanges)));
        // a prefix from offset 0 works (connection dropped after `length` bytes)
        s->request(0, 100000);
        QTRY_COMPARE_WITH_TIMEOUT(c.finished.size(), 2, 5000);
        QVERIFY2(c.finished[1].ok, qPrintable(c.finished[1].err));
        QVERIFY(c.data == expected("/f/nr.bin", 0, 100000));
        QCOMPARE(srv->log().last().range, QByteArray());
    }
    void rangeIgnoredMidway()
    {
        // HEAD claims "Accept-Ranges: bytes", but GETs ignore Range → becomes a no-range source.
        const qint64 size = 200000;
        std::unique_ptr<RangeSource> s(open(url("/f/liar.bin?size=200000&liar=1"), size));
        Collector c(s.get());
        QTRY_VERIFY_WITH_TIMEOUT(settled(s.get()), 5000);
        QVERIFY(qobject_cast<HttpSourceInfo *>(s.get())->supportsRanges());
        QSignalSpy avail(s.get(), &RangeSource::availabilityChanged);
        s->request(1000, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(c.finished.size(), 1, 5000);
        QVERIFY(!c.finished[0].ok);
        QVERIFY2(c.finished[0].err.startsWith(QLatin1String(kHttpErrorNoRanges)), qPrintable(c.finished[0].err));
        QCOMPARE(c.data.size(), 0);
        QVERIFY(!qobject_cast<HttpSourceInfo *>(s.get())->supportsRanges());
        QCOMPARE(s->descriptor().extra.value(QStringLiteral("supportsRanges")).toBool(), false);
        QCOMPARE(s->maxPendingRequests(), 1);
        QCOMPARE(avail.size(), 1);
        s->request(0, size);
        QTRY_COMPARE_WITH_TIMEOUT(c.finished.size(), 2, 5000);
        QVERIFY2(c.finished[1].ok, qPrintable(c.finished[1].err));
        QVERIFY(c.data == expected("/f/liar.bin", 0, size));
    }
    void trickle()
    {
        std::unique_ptr<RangeSource> s(open(url("/f/slow.bin?size=300000&rate=200000"), 300000));
        Collector c(s.get());
        QTRY_VERIFY_WITH_TIMEOUT(settled(s.get()), 5000);
        QElapsedTimer t;
        t.start();
        s->request(0, 300000);
        QTRY_COMPARE_WITH_TIMEOUT(c.finished.size(), 1, 10000);
        QVERIFY2(c.finished[0].ok, qPrintable(c.finished[0].err));
        QVERIFY(c.data == expected("/f/slow.bin", 0, 300000));
        QVERIFY(t.elapsed() >= 1000);
        QVERIFY(c.chunks > 10);                 // streamed as it arrived
    }
    void stallRetry()
    {
        std::unique_ptr<RangeSource> s(open(url("/f/stall.bin?size=600000&stallonce=100000"), 600000));
        Collector c(s.get());
        QTRY_VERIFY_WITH_TIMEOUT(settled(s.get()), 5000);
        s->request(0, 600000);
        QTRY_COMPARE_WITH_TIMEOUT(c.finished.size(), 1, 15000);
        QVERIFY2(c.finished[0].ok, qPrintable(c.finished[0].err));
        QCOMPARE(c.gaps, 0);
        QVERIFY(c.data == expected("/f/stall.bin", 0, 600000));
        QVERIFY(dynamic_cast<HttpRangeSource *>(s.get())->retriesDone() >= 1);
        QCOMPARE(srv->log().last().range, QByteArray("bytes=100000-599999"));   // resumed where it stalled
    }
    void resetMidBody()
    {
        std::unique_ptr<RangeSource> s(open(url("/f/reset.bin?size=900000&resetonce=300000"), 900000));
        Collector c(s.get());
        QTRY_VERIFY_WITH_TIMEOUT(settled(s.get()), 5000);
        s->request(0, 900000);
        QTRY_COMPARE_WITH_TIMEOUT(c.finished.size(), 1, 15000);
        QVERIFY2(c.finished[0].ok, qPrintable(c.finished[0].err));
        QVERIFY(c.data == expected("/f/reset.bin", 0, 900000));
        QVERIFY(dynamic_cast<HttpRangeSource *>(s.get())->retriesDone() >= 1);
        QVERIFY(srv->log().last().range.startsWith("bytes="));
        QVERIFY(srv->log().last().range != "bytes=0-899999");
    }
    void wrongContentRange()
    {
        std::unique_ptr<RangeSource> s(open(url("/f/wrong.bin?size=100000&wrongrange=1"), 100000));
        Collector c(s.get());
        QTRY_VERIFY_WITH_TIMEOUT(settled(s.get()), 5000);
        QCOMPARE(s->state(), RangeSource::Ready);
        s->request(0, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(c.finished.size(), 1, 5000);
        QVERIFY(!c.finished[0].ok);
        QCOMPARE(c.data.size(), 0);
        QCOMPARE(s->state(), RangeSource::Failed);
        QVERIFY2(s->stateText().contains(QLatin1String("wrong range")), qPrintable(s->stateText()));
    }
    void etagChanged()
    {
        std::unique_ptr<RangeSource> s(open(url("/f/chg.bin?size=200000&changeafter=1"), 200000));
        Collector c(s.get());
        QTRY_VERIFY_WITH_TIMEOUT(settled(s.get()), 5000);
        s->request(0, 100000);
        QTRY_COMPARE_WITH_TIMEOUT(c.finished.size(), 1, 5000);
        QVERIFY(c.finished[0].ok);
        s->request(100000, 100000);              // the file changed in between
        QTRY_COMPARE_WITH_TIMEOUT(c.finished.size(), 2, 5000);
        QVERIFY(!c.finished[1].ok);
        QCOMPARE(c.data.size(), 100000);         // nothing of the new version was delivered
        QCOMPARE(s->state(), RangeSource::Failed);
        QVERIFY2(s->stateText().contains(QLatin1String("File changed on the server")), qPrintable(s->stateText()));
        QCOMPARE(srv->log().last().ifRange, QByteArray("\"v0-200000\""));
        // re-opening with the persisted ETag also detects the change
        QVariantMap extra;
        extra.insert(QStringLiteral("etag"), QStringLiteral("\"v0-200000\""));
        std::unique_ptr<RangeSource> s2(open(url("/f/chg.bin?size=200000&changeafter=1"), 200000, {}, extra));
        QTRY_VERIFY_WITH_TIMEOUT(settled(s2.get()), 5000);
        QCOMPARE(s2->state(), RangeSource::Failed);
        QVERIFY2(s2->stateText().contains(QLatin1String("changed")), qPrintable(s2->stateText()));
    }
    void lastModifiedChanged()
    {
        std::unique_ptr<RangeSource> s(open(url("/f/lmchg.bin?size=200000&changeafter=1&noetag=1&lm=1"), 200000));
        Collector c(s.get());
        QTRY_VERIFY_WITH_TIMEOUT(settled(s.get()), 5000);
        s->request(0, 100000);
        QTRY_COMPARE_WITH_TIMEOUT(c.finished.size(), 1, 5000);
        s->request(100000, 100000);
        QTRY_COMPARE_WITH_TIMEOUT(c.finished.size(), 2, 5000);
        QVERIFY(!c.finished[1].ok);
        QCOMPARE(s->state(), RangeSource::Failed);
        QCOMPARE(srv->log().last().ifRange, QByteArray("Mon, 01 Jan 2024 00:00:00 GMT"));
    }
    void redirects()
    {
        std::unique_ptr<RangeSource> s(open(url("/redir/3/f/rd.bin?size=100000"), 100000));
        Collector c(s.get());
        QTRY_VERIFY_WITH_TIMEOUT(settled(s.get()), 5000);
        QCOMPARE(s->state(), RangeSource::Ready);
        QCOMPARE(qobject_cast<HttpSourceInfo *>(s.get())->finalUrl().path(), QStringLiteral("/f/rd.bin"));
        s->request(10, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(c.finished.size(), 1, 5000);
        QVERIFY(c.finished[0].ok);
        QVERIFY(c.data == expected("/f/rd.bin", 10, 1000));
        QVERIFY(srv->log().last().target.startsWith("/f/rd.bin"));     // ranges go straight to the final URL

        std::unique_ptr<RangeSource> loop(open(url("/loop/a"), 100));
        QTRY_VERIFY_WITH_TIMEOUT(settled(loop.get()), 5000);
        QCOMPARE(loop->state(), RangeSource::Failed);
        QVERIFY2(loop->stateText().contains(QLatin1String("loop")), qPrintable(loop->stateText()));
    }
    void busyRetryAfter()
    {
        std::unique_ptr<RangeSource> s(open(url("/f/busy.bin?size=50000&busy=2&retryafter=1"), 50000));
        Collector c(s.get());
        QTRY_VERIFY_WITH_TIMEOUT(s->state() == RangeSource::Queued, 3000);
        QVERIFY2(QRegularExpression(QStringLiteral("^Server busy, retry in \\d+ s$")).match(s->stateText()).hasMatch(),
                 qPrintable(s->stateText()));
        QTRY_VERIFY_WITH_TIMEOUT(settled(s.get()), 8000);
        QCOMPARE(s->state(), RangeSource::Ready);
        QVERIFY(c.states.contains(RangeSource::Queued));
        s->request(0, 50000);
        QTRY_COMPARE_WITH_TIMEOUT(c.finished.size(), 1, 5000);
        QVERIFY(c.finished[0].ok);
        QVERIFY(c.data == expected("/f/busy.bin", 0, 50000));
    }
    void busyDuringRequest()
    {
        // The server answers 503 when more than 2 GETs run at once: the source keeps the working
        // connections, retries the refused ones later and lowers maxPendingRequests() to 2.
        std::unique_ptr<RangeSource> m(open(url("/f/cap.bin?size=4000000&maxconn=2&rate=2000000"), 4000000));
        QTRY_VERIFY_WITH_TIMEOUT(settled(m.get()), 5000);
        QTemporaryFile f;
        QVERIFY(f.open());
        mxtest::MiniScheduler sched(m.get(), &f, 4000000, 500000);
        QSignalSpy done(&sched, &mxtest::MiniScheduler::finished);
        sched.start();
        QVERIFY(done.wait(30000));
        QVERIFY2(sched.ok, qPrintable(sched.error));
#ifdef Q_OS_MACOS
        // [mac-1] the test server may still count a connection the source has finished with (macOS
        // reports its end later): the source then adapts to 1, which is the limit it was shown
        QVERIFY2(m->maxPendingRequests() >= 1 && m->maxPendingRequests() <= 2, qPrintable(QString::number(m->maxPendingRequests())));
#else
        QCOMPARE(m->maxPendingRequests(), 2);       // adapted to the server's limit
#endif
        f.seek(0);
        QVERIFY(f.readAll() == expected("/f/cap.bin", 0, 4000000));
    }
    void permanentErrors_data()
    {
        QTest::addColumn<QByteArray>("path");
        QTest::addColumn<QString>("text");
        QTest::newRow("404") << QByteArray("/status/404") << QStringLiteral("404");
        QTest::newRow("410") << QByteArray("/status/410") << QStringLiteral("410");
        QTest::newRow("403") << QByteArray("/status/403") << QStringLiteral("403");
    }
    void permanentErrors()
    {
        QFETCH(QByteArray, path);
        QFETCH(QString, text);
        std::unique_ptr<RangeSource> s(open(url(path), 100));
        QTRY_VERIFY_WITH_TIMEOUT(settled(s.get()), 5000);
        QCOMPARE(s->state(), RangeSource::Failed);
        QVERIFY2(s->stateText().contains(text), qPrintable(s->stateText()));
        QCOMPARE(srv->log().size(), path == "/status/403" ? 2 : 1);  // 403 on HEAD → GET fallback
    }
    void transient5xxGivesUp()
    {
        settings->setValue(QLatin1String(httpkeys::MaxRetries), 2);
        std::unique_ptr<RangeSource> s(open(url("/status/502"), 100));
        QTRY_VERIFY_WITH_TIMEOUT(settled(s.get()), 8000);
        settings->setValue(QLatin1String(httpkeys::MaxRetries), 5);
        QCOMPARE(s->state(), RangeSource::Failed);
        QVERIFY2(s->stateText().contains(QLatin1String("502")), qPrintable(s->stateText()));
        QCOMPARE(srv->count("HEAD", "/status/502"), 3);
    }
    void chunked()
    {
        std::unique_ptr<RangeSource> s(open(url("/f/chunk.bin?size=300000&chunked=1"), 300000));
        Collector c(s.get());
        QTRY_VERIFY_WITH_TIMEOUT(settled(s.get()), 5000);
        QCOMPARE(s->state(), RangeSource::Ready);
        s->request(1000, 250000);
        QTRY_COMPARE_WITH_TIMEOUT(c.finished.size(), 1, 5000);
        QVERIFY2(c.finished[0].ok, qPrintable(c.finished[0].err));
        QVERIFY(c.data == expected("/f/chunk.bin", 1000, 250000));
    }
    void unknownSize()
    {
        std::unique_ptr<RangeSource> s(open(url("/f/stream.bin?size=123457&nolength=1&noranges=1&cd=attachment%3B%20filename%3D%22My%20Stream.bin%22"), 0));
        Collector c(s.get());
        QTRY_VERIFY_WITH_TIMEOUT(settled(s.get()), 5000);
        QCOMPARE(s->state(), RangeSource::Ready);
        QVERIFY(s->available().isEmpty());
        const QVariantMap x = s->descriptor().extra;
        QCOMPARE(x.value(QStringLiteral("size")).toLongLong(), -1);
        QCOMPARE(x.value(QStringLiteral("fileName")).toString(), QStringLiteral("My Stream.bin"));
        QCOMPARE(x.value(QStringLiteral("supportsRanges")).toBool(), false);
        QCOMPARE(x.value(QStringLiteral("isTorrentFile")).toBool(), false);
        QVERIFY(x.value(QStringLiteral("finalUrl")).toString().contains(QLatin1String("/f/stream.bin")));
        s->request(0, qint64(1) << 40);
        QTRY_COMPARE_WITH_TIMEOUT(c.finished.size(), 1, 5000);
        QVERIFY2(c.finished[0].ok, qPrintable(c.finished[0].err));
        QCOMPARE(c.finished[0].len, 123457);
        QVERIFY(c.data == expected("/f/stream.bin", 0, 123457));
        QCOMPARE(qobject_cast<HttpSourceInfo *>(s.get())->remoteSize(), 123457);

        // size unknown to the engine, but known to the server
        std::unique_ptr<RangeSource> s2(open(url("/f/known.bin?size=4444"), 0));
        QTRY_VERIFY_WITH_TIMEOUT(settled(s2.get()), 5000);
        QCOMPARE(s2->available(), (QList<QPair<qint64, qint64>>{{0, 4444}}));
        QCOMPARE(s2->descriptor().extra.value(QStringLiteral("size")).toLongLong(), 4444);
        QCOMPARE(s2->descriptor().extra.value(QStringLiteral("supportsRanges")).toBool(), true);

        // a pasted link that is a .torrent
        std::unique_ptr<RangeSource> s3(open(url("/f/dl?torrent=1"), 0));
        QTRY_VERIFY_WITH_TIMEOUT(settled(s3.get()), 5000);
        QCOMPARE(s3->descriptor().extra.value(QStringLiteral("isTorrentFile")).toBool(), true);
    }
    void sizeMismatch()
    {
        std::unique_ptr<RangeSource> s(open(url("/f/sz.bin?size=1000"), 999));
        QTRY_VERIFY_WITH_TIMEOUT(settled(s.get()), 5000);
        QCOMPARE(s->state(), RangeSource::Failed);
        QCOMPARE(s->stateText(), QStringLiteral("Size mismatch: the server's file is 1000 bytes, expected 999"));
    }
    void cancelSemantics()
    {
        std::unique_ptr<RangeSource> s(open(url("/f/cancel.bin?size=2000000&rate=400000"), 2000000));
        Collector c(s.get());
        QTRY_VERIFY_WITH_TIMEOUT(settled(s.get()), 5000);
        s->request(0, 1000000);
        s->request(1000000, 1000000);
        QTRY_VERIFY_WITH_TIMEOUT(c.chunks > 0, 5000);
        s->cancelRequests();
        QCOMPARE(s->state(), RangeSource::Ready);
        const int chunks = c.chunks;
        QTest::qWait(700);
        QCOMPARE(c.chunks, chunks);              // nothing after cancelRequests()
        QCOMPARE(c.finished.size(), 0);          // cancelled silently
        // and the source is still usable
        Collector c2(s.get());
        s->request(500, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(c2.finished.size(), 1, 5000);
        QVERIFY(c2.finished[0].ok);
        QVERIFY(c2.data == expected("/f/cancel.bin", 500, 1000));
        // cancel from inside the data() slot
        Collector c3(s.get());
        connect(s.get(), &RangeSource::data, s.get(), [&s] { s->cancelRequests(); });
        s->request(0, 1000000);
        QTRY_VERIFY_WITH_TIMEOUT(c3.chunks > 0, 5000);
        QTest::qWait(300);
        QCOMPARE(c3.chunks, 1);
        QCOMPARE(c3.finished.size(), 0);
        s->disconnect(s.get());
    }
    // a request cancelled in the middle of its body closes its connection: the server sees it go, and the
    // connections Qt keeps per host (6) do not fill up with cancelled requests
    void cancelClosesTheConnection()
    {
        std::unique_ptr<RangeSource> s(open(url("/f/drop.bin?size=40000000&rate=2000000"), 40000000));
        QTRY_VERIFY_WITH_TIMEOUT(settled(s.get()), 5000);
        Collector c(s.get());
        for (int i = 0; i < 8; ++i) {
            const int before = c.chunks;
            s->request(qint64(i) * 4000000, 4000000);
            QTRY_VERIFY2_WITH_TIMEOUT(c.chunks > before, qPrintable(QStringLiteral("request %1 got no data; the server saw:\n%2").arg(i).arg(QString::fromLatin1(srv->timeline()))), 8000);
            s->cancelRequests();
            QTRY_VERIFY2_WITH_TIMEOUT(srv->active() == 0, qPrintable(QStringLiteral("after cancel %1 the server still sends:\n%2").arg(i).arg(QString::fromLatin1(srv->timeline()))), 3000);
        }
        QCOMPARE(c.finished.size(), 0);   // a cancelled request reports nothing
        s->request(0, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(c.finished.size(), 1, 5000);
        QVERIFY2(c.finished[0].ok, qPrintable(c.finished[0].err));
        s->disconnect(s.get());
    }
    void limiterThrottle()
    {
        const qint64 size = 2 * 1024 * 1024;
        const qint64 rate = 512 * 1024;
        std::unique_ptr<RangeSource> s(open(url("/f/lim.bin?size=2097152"), size));
        QTRY_VERIFY_WITH_TIMEOUT(settled(s.get()), 5000);
        QTemporaryFile f;
        QVERIFY(f.open());
        limiter.setRate(rate);
        limiter.accounted = 0;
        mxtest::MiniScheduler sched(s.get(), &f, size, 512 * 1024);
        QSignalSpy done(&sched, &mxtest::MiniScheduler::finished);
        sched.start();
        QVERIFY(done.wait(20000));
        limiter.setRate(0);
        QVERIFY2(sched.ok, qPrintable(sched.error));
        const double secs = sched.elapsedMs() / 1000.0;
        const double achieved = size / secs;
        qInfo("limited to %lld B/s: %.0f B/s over %.2f s (%.1f %%)", rate, achieved, secs, 100.0 * achieved / rate);
        QVERIFY2(achieved >= rate * 0.8 && achieved <= rate * 1.2, qPrintable(QString::number(achieved)));
        QCOMPARE(limiter.accounted, size);
        f.seek(0);
        QVERIFY(f.readAll() == expected("/f/lim.bin", 0, size));
    }
    void perHostCap()
    {
        settings->setValue(keys::http::MaxConnectionsPerServer, 2);
        std::unique_ptr<RangeSource> a(open(url("/f/hostA.bin?size=1000000&rate=1000000"), 1000000));
        std::unique_ptr<RangeSource> b(open(url("/f/hostB.bin?size=1000000&rate=1000000"), 1000000));
        QTRY_VERIFY_WITH_TIMEOUT(settled(a.get()) && settled(b.get()), 5000);
        QCOMPARE(a->maxPendingRequests(), 2);
        Collector ca(a.get()), cb(b.get());
        a->request(0, 400000);
        a->request(400000, 400000);
        b->request(0, 400000);
        b->request(400000, 400000);
        QElapsedTimer t;
        t.start();
        QTRY_VERIFY_WITH_TIMEOUT(ca.finished.size() == 2 && cb.finished.size() == 2, 15000);
        for (const auto &x : ca.finished + cb.finished) QVERIFY2(x.ok, qPrintable(x.err));
        qInfo("four requests done in %lld ms; the server saw:\n%s", t.elapsed(), srv->timeline().constData());
        QCOMPARE(srv->maxConcurrent(), 2);
    }
    void credentials()
    {
        const QByteArray b = srv->base();
        const QString u = QStringLiteral("http://user:secret@") + QString::fromLatin1(b.mid(7)) + QStringLiteral("/redir/1/f/auth.bin?size=10");
        std::unique_ptr<RangeSource> s(open(u, 10));
        QTRY_VERIFY_WITH_TIMEOUT(settled(s.get()), 5000);
        const auto lg = srv->log();
        QVERIFY(lg.size() >= 2);
        QCOMPARE(lg[0].auth, QByteArray("Basic ") + QByteArray("user:secret").toBase64());
        QVERIFY(!qobject_cast<HttpSourceInfo *>(s.get())->finalUrl().toString().contains(QLatin1String("secret")));
        QVERIFY(!s->descriptor().extra.value(QStringLiteral("finalUrl")).toString().contains(QLatin1String("secret")));
    }
    void openSourceRejects()
    {
        SearchSource ss;
        ss.net = Net::ED2K;
        ss.uri = url("/f/x");
        QVERIFY(!plugin->openSource(QStringLiteral("d"), {}, 1, ss));
        ss.net = Net::Http;
        ss.uri = QStringLiteral("ftp://example.org/x");
        QVERIFY(!plugin->openSource(QStringLiteral("d"), {}, 1, ss));
    }
};

QTEST_GUILESS_MAIN(TstHttp)
#include "tst_http.moc"
