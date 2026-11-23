// SPDX-License-Identifier: GPL-3.0-or-later
// Overlay direct transfer: RangeSource download of a generated 200 MB file (identical SHA-1,
// throughput, bounded memory), upload slots + remote queueing, cancel, missing file, rate limit.
#include "ovl_test_support.h"

#include <QtCore/QMutex>

using namespace mx::ovl;
using namespace ovltest;

namespace {

qint64 rssKb()
{
    QFile f(QStringLiteral("/proc/self/status"));
    if (!f.open(QIODevice::ReadOnly)) return -1;
    for (const QByteArray &l : f.readAll().split('\n'))
        if (l.startsWith("VmRSS:")) return l.mid(6).trimmed().split(' ').first().toLongLong();
    return -1;
}

class TestLimiter : public mx::RateLimiter {
public:
    QMutex m;
    qint64 accounted[2] = {0, 0};
    int upLimit = 0;   // bytes/s, 0 = unlimited
    double tokens = 0;
    QElapsedTimer t;
    qint64 acquire(Dir d, qint64 bytes) override
    {
        QMutexLocker l(&m);
        if (d != Up || upLimit == 0) return bytes;
        if (!t.isValid()) t.start();
        tokens = qMin<double>(upLimit / 4.0, tokens + t.restart() / 1000.0 * upLimit);
        const qint64 n = qMin<qint64>(bytes, static_cast<qint64>(tokens));
        tokens -= n;
        return qMax<qint64>(0, n);
    }
    void account(Dir d, mx::Net, qint64 bytes) override
    {
        QMutexLocker l(&m);
        accounted[d] += bytes;
    }
    // [engine-3] glue-3's split accounting: wire (all bytes on the link, counted by RatsNode) and
    // payload (file data, reported by the transfer code with wire = 0)
    qint64 payload[2] = {0, 0};
    void accountTraffic(Dir d, mx::Net, qint64 wireBytes, qint64 payloadBytes) override
    {
        QMutexLocker l(&m);
        accounted[d] += wireBytes;
        payload[d] += payloadBytes;
    }
    int limitBps(Dir d) const override { return d == Up ? upLimit : 0; }
};

// Drives a RangeSource like the core engine does: sequential chunks, pipelined up to
// maxPendingRequests(), writing to a file and hashing on the fly.
class Fetcher : public QObject {
public:
    mx::RangeSource *src;
    qint64 size, next = 0, done = 0;
    int pending = 0;
    QFile out;
    QCryptographicHash sha1{QCryptographicHash::Sha1};
    qint64 expectOff = 0;
    bool failed = false;
    QString error;
    Fetcher(mx::RangeSource *s, qint64 sz, const QString &path) : src(s), size(sz), out(path)
    {
        (void)out.open(QIODevice::WriteOnly);
        connect(src, &mx::RangeSource::data, this, [this](qint64 off, const QByteArray &b) {
            if (off != expectOff) { failed = true; error = QStringLiteral("gap"); }
            expectOff = off + b.size();
            out.write(b);
            sha1.addData(b);
        });
        connect(src, &mx::RangeSource::requestFinished, this, [this](qint64, qint64 len, bool ok, const QString &e) {
            --pending;
            if (!ok) { failed = true; error = e; return; }
            done += len;
            pump();
        });
        connect(src, &mx::RangeSource::stateChanged, this, [this] { pump(); });
    }
    void pump()
    {
        if (failed) return;
        if (src->state() != mx::RangeSource::Ready && src->state() != mx::RangeSource::Busy) return;
        while (pending < src->maxPendingRequests() && next < size) {
            const qint64 len = qMin(src->preferredRequestSize(), size - next);
            ++pending;
            const qint64 off = next;
            next += len;
            src->request(off, len);
        }
    }
    bool complete() const { return done == size; }
};

OverlayPlugin::TestOptions opts()
{
    OverlayPlugin::TestOptions o;
    o.dht = o.mdns = o.reconnect = o.pex = false;
    o.listenPort = 0;
    return o;
}

mx::SearchSource sourceFor(Instance &uploader)
{
    mx::SearchSource s;
    s.net = mx::Net::Overlay;
    s.user = uploader.me();
    s.extra.insert(QStringLiteral("node"), uploader.plugin->nodeId());
    return s;
}

QByteArray sha1Of(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash h(QCryptographicHash::Sha1);
    h.addData(&f);
    return h.result();
}

} // namespace

class TstOverlayTransfer : public QObject {
    Q_OBJECT
    std::unique_ptr<Instance> A, B, C;
    TestLimiter limA, limB;
    mx::SharedFile big, small;

private slots:
    void initTestCase()
    {
        mx::registerMetaTypes();
        A.reset(new Instance(QStringLiteral("Alice")));
        B.reset(new Instance(QStringLiteral("Bob")));
        C.reset(new Instance(QStringLiteral("Carol")));
        // the limiter is part of the context: re-init with it
        auto withLimiter = [](Instance *i, TestLimiter *l) {
            mx::PluginContext ctx;
            ctx.settings = &i->settings;
            ctx.library = &i->library;
            ctx.limiter = l;
            ctx.profileDir = i->profile;
            ctx.dataDir = i->profile + QStringLiteral("/overlay");
            ctx.screenName = i->settings.s(QString(mx::keys::identity::ScreenName));
            i->plugin->init(ctx);
        };
        withLimiter(A.get(), &limA);
        withLimiter(B.get(), &limB);
        for (Instance *i : {A.get(), B.get(), C.get()}) {
            i->plugin->setTestOptions(opts());
            i->start();
        }
        QElapsedTimer t;
        t.start();
        big = B->library.addGenerated(B->profile, QStringLiteral("big-200MB.bin"), 200LL * 1024 * 1024, 4242);
        small = B->library.addGenerated(B->profile, QStringLiteral("small-3MB.bin"), 3 * 1024 * 1024 + 123, 77);
        qInfo("generated test files in %lld ms", t.elapsed());
        B->setShared();
        QVERIFY(link(*A, *B));
        QVERIFY(link(*C, *B));
    }

    void transfer200MB()
    {
        mx::FileHashes h;
        h.sha1 = big.hashes.sha1;   // the downloader knows one hash; the uploader matches by it
        auto *src = A->plugin->openSource(QStringLiteral("dl-big"), h, big.size, sourceFor(*B));
        QVERIFY(src);
        QSignalSpy upChanged(B->plugin, &mx::NetworkPlugin::uploadChanged);
        QSignalSpy upFinished(B->plugin, &mx::NetworkPlugin::uploadFinished);
        const QString outPath = A->profile + QStringLiteral("/big.out");
        Fetcher f(src, big.size, outPath);
        const qint64 rss0 = rssKb();
        qint64 rssMax = rss0;
        QElapsedTimer t;
        t.start();
        src->open();
        QVERIFY(waitFor([&] {
            rssMax = qMax(rssMax, rssKb());
            return f.complete() || f.failed;
        }, 120000));
        const qint64 ms = t.elapsed();
        QVERIFY2(!f.failed, qPrintable(f.error));
        f.out.close();
        QCOMPARE(f.sha1.result(), big.hashes.sha1);
        QCOMPARE(sha1Of(outPath), big.hashes.sha1);
        const double mbps = big.size / 1048576.0 / (ms / 1000.0);
        qInfo("200 MB in %lld ms = %.1f MB/s; RSS %lld -> max %lld KB (+%lld MB)", ms, mbps, rss0, rssMax,
              (rssMax - rss0) / 1024);
        if (rss0 > 0) QVERIFY2(rssMax - rss0 < 160 * 1024, "memory ballooned during the transfer");
        QVERIFY(upChanged.count() > 0);
        const auto last = upChanged.last().first().value<mx::TransferInfo>();
        QVERIFY(last.isUpload);
        QCOMPARE(last.peer.id, A->me().id);
        QCOMPARE(last.name, big.name);
        // payload exact; the wire total adds framing / control traffic (≤ 1 %)
        QCOMPARE(limB.payload[mx::RateLimiter::Up], big.size);
        QCOMPARE(limA.payload[mx::RateLimiter::Down], big.size);
        QVERIFY(limB.accounted[mx::RateLimiter::Up] >= big.size && limB.accounted[mx::RateLimiter::Up] < big.size + big.size / 100);
        QVERIFY(limA.accounted[mx::RateLimiter::Down] >= big.size && limA.accounted[mx::RateLimiter::Down] < big.size + big.size / 100);
        src->close();
        delete src;
        QVERIFY(waitFor([&] { return upFinished.count() == 1; }, 5000));
        QFile::remove(outPath);
    }

    // [engine-3] slot answers offer "ed2k-parts" when the library has the set; the client fetches
    // it once (hs_req → hs) and hands it to the engine through RangeSource::hashSetReceived.
    void hashSetExchange()
    {
        const int n = int((big.size + 9727999) / 9728000);
        QList<QByteArray> parts;
        for (int i = 0; i < n; ++i) parts << QCryptographicHash::hash(QByteArray::number(i), QCryptographicHash::Md5);
        B->library.partSets.insert(big.hashes.sha1, parts);
        // [up-1] a server offers only a set that reduces to the file's ed2k hash: make the stand-in
        // ed2k hash of the generated file the root of this set
        {
            QCryptographicHash md4(QCryptographicHash::Md4);
            for (const QByteArray &p : std::as_const(parts)) md4.addData(p);
            for (mx::SharedFile &f : B->library.files)
                if (f.hashes.sha1 == big.hashes.sha1) f.hashes.ed2k = md4.result();
            big.hashes.ed2k = md4.result();
            B->setShared();
        }
        mx::FileHashes h;
        h.sha1 = big.hashes.sha1;
        auto *src = A->plugin->openSource(QStringLiteral("dl-hs"), h, big.size, sourceFor(*B));
        QVERIFY(src);
        QSignalSpy sets(src, &mx::RangeSource::hashSetReceived);
        src->open();
        QVERIFY(waitFor([&] { return sets.count() == 1; }, 10000));
        QCOMPARE(sets.first().at(0).toString(), QStringLiteral("ed2k-parts"));
        QCOMPARE(sets.first().at(1).toLongLong(), qint64(9728000));
        QCOMPARE(sets.first().at(2).value<QList<QByteArray>>(), parts);
        QTest::qWait(300);
        QCOMPARE(sets.count(), 1);   // asked once per session
        src->close();
        delete src;
        // a library without the set (= an old server) offers nothing: the client never asks
        B->library.partSets.clear();
        auto *src2 = A->plugin->openSource(QStringLiteral("dl-hs2"), h, big.size, sourceFor(*B));
        QSignalSpy sets2(src2, &mx::RangeSource::hashSetReceived);
        src2->open();
        QVERIFY(waitFor([&] { return src2->state() == mx::RangeSource::Ready; }, 10000));
        QTest::qWait(500);
        QCOMPARE(sets2.count(), 0);
        src2->close();
        delete src2;
    }

    void slotQueueing()
    {
        B->settings.setValue(QString(mx::keys::transfers::MaxUploads), 1);
        B->settings.setValue(QString(mx::keys::queue::SmallFileBypassKB), 0);
        mx::FileHashes h;
        h.sha256 = small.hashes.sha256;
        auto *sa = A->plugin->openSource(QStringLiteral("dl-a"), h, small.size, sourceFor(*B));
        auto *sc = C->plugin->openSource(QStringLiteral("dl-c"), h, small.size, sourceFor(*B));
        QSignalSpy upChanged(B->plugin, &mx::NetworkPlugin::uploadChanged);
        sa->open();
        QVERIFY(waitFor([&] { return sa->state() == mx::RangeSource::Ready; }));
        sc->open();
        QVERIFY(waitFor([&] { return sc->state() == mx::RangeSource::Queued; }));
        QCOMPARE(sc->stateText(), QStringLiteral("Remotely Queued (1)"));
        QVERIFY(sc->available().isEmpty());
        QCOMPARE(sa->available().size(), 1);
        QCOMPARE(sa->available().first().second, small.size);
        bool sawQueued = false;
        for (const auto &a : upChanged) {
            const auto t = a.first().value<mx::TransferInfo>();
            if (t.state == mx::TransferState::Queued && t.peer.id == C->me().id) sawQueued = true;
        }
        QVERIFY(sawQueued);
        // A downloads and closes; C moves up and downloads the identical bytes
        Fetcher fa(sa, small.size, A->profile + QStringLiteral("/small-a.out"));
        fa.pump();
        QVERIFY(waitFor([&] { return fa.complete() || fa.failed; }, 20000));
        QVERIFY(!fa.failed);
        QCOMPARE(fa.sha1.result(), small.hashes.sha1);
        Fetcher fc(sc, small.size, C->profile + QStringLiteral("/small-c.out"));
        QCOMPARE(sc->state(), mx::RangeSource::Queued);
        sa->close();
        delete sa;
        QVERIFY(waitFor([&] { return fc.complete() || fc.failed; }, 20000));
        QVERIFY(!fc.failed);
        QCOMPARE(fc.sha1.result(), small.hashes.sha1);
        sc->close();
        delete sc;
        B->settings.setValue(QString(mx::keys::transfers::MaxUploads), 3);
    }

    void cancelAndMissing()
    {
        mx::FileHashes h;
        h.sha1 = big.hashes.sha1;
        auto *src = A->plugin->openSource(QStringLiteral("dl-x"), h, big.size, sourceFor(*B));
        Fetcher f(src, big.size, A->profile + QStringLiteral("/cancel.out"));
        QSignalSpy upChanged(B->plugin, &mx::NetworkPlugin::uploadChanged);
        QSignalSpy upFinished(B->plugin, &mx::NetworkPlugin::uploadFinished);
        src->open();
        QVERIFY(waitFor([&] { return f.done > 4 * 1024 * 1024; }, 20000));
        const QString uploadId = upChanged.last().first().value<mx::TransferInfo>().id;
        B->plugin->cancelUpload(uploadId);
        QVERIFY(waitFor([&] { return src->state() == mx::RangeSource::Failed; }, 5000));
        bool finished = false;
        for (const auto &a : upFinished) finished |= a.first().toString() == uploadId;
        QVERIFY(finished);
        delete src;

        mx::FileHashes none;
        none.sha1 = QByteArray(20, '\x01');
        auto *m = A->plugin->openSource(QStringLiteral("dl-m"), none, 1000, sourceFor(*B));
        m->open();
        QVERIFY(waitFor([&] { return m->state() == mx::RangeSource::Failed; }, 5000));
        QCOMPARE(m->stateText(), QStringLiteral("File not shared"));
        delete m;
        // a non-overlay source descriptor is refused
        mx::SearchSource http;
        http.net = mx::Net::Http;
        QVERIFY(!A->plugin->openSource(QStringLiteral("dl-h"), h, 10, http));
    }

    void rateLimited()
    {
        limB.upLimit = 2 * 1024 * 1024;   // 2 MB/s
        mx::FileHashes h;
        h.sha1 = small.hashes.sha1;
        auto *src = A->plugin->openSource(QStringLiteral("dl-r"), h, small.size, sourceFor(*B));
        Fetcher f(src, small.size, A->profile + QStringLiteral("/rate.out"));
        QElapsedTimer t;
        t.start();
        src->open();
        QVERIFY(waitFor([&] { return f.complete() || f.failed; }, 20000));
        QVERIFY(!f.failed);
        QCOMPARE(f.sha1.result(), small.hashes.sha1);
        qInfo("3 MB at a 2 MB/s upload limit took %lld ms", t.elapsed());
        QVERIFY(t.elapsed() > 1100);
        limB.upLimit = 0;
        src->close();
        delete src;
    }

    void disconnectFailsSource()
    {
        mx::FileHashes h;
        h.sha1 = big.hashes.sha1;
        auto *src = C->plugin->openSource(QStringLiteral("dl-d"), h, big.size, sourceFor(*B));
        Fetcher f(src, big.size, C->profile + QStringLiteral("/disc.out"));
        src->open();
        QVERIFY(waitFor([&] { return f.done > 2 * 1024 * 1024; }, 20000));
        B->plugin->stop();
        QVERIFY(waitFor([&] { return src->state() == mx::RangeSource::Failed; }, 10000));
        delete src;
    }

    void cleanupTestCase()
    {
        A.reset();
        B.reset();
        C.reset();
        QVERIFY(RatsNode::waitAllStopped(8000));
    }
};

QTEST_GUILESS_MAIN(TstOverlayTransfer)
#include "tst_overlay_transfer.moc"
