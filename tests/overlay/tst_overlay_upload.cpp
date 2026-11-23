// SPDX-License-Identifier: GPL-3.0-or-later
// [up-1] Uploads between OctoMX users, two nodes over 127.0.0.1:
//  * a file still downloading is served by its verified ranges only, with a "have" map in the slot,
//    "have" updates as it grows and "full" when complete; a request outside the map is "not
//    available" (no error for the downloader); a 5.2 client (no "part") gets "missing" for it;
//  * a part hash set is offered only when it reduces to the file's ed2k hash;
//  * get_close carries why the downloader stopped; the upload row ends with that ("Stopped by the
//    other side: ...", "Cancelled by remote ...", "Finished"), a 5.2 downloader's close (no reason)
//    reads "Finished"; the uploader stops sending at once; a vanished downloader times out.
#include "ovl_test_support.h"

using namespace mx::ovl;
using namespace ovltest;

namespace {

class CountingLimiter : public mx::RateLimiter {
public:
    QMutex m;
    qint64 payload[2] = {0, 0};
    int upLimit = 0;
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
    void account(Dir, mx::Net, qint64) override {}
    void accountTraffic(Dir d, mx::Net, qint64, qint64 payloadBytes) override
    {
        QMutexLocker l(&m);
        payload[d] += payloadBytes;
    }
    qint64 sentUp()
    {
        QMutexLocker l(&m);
        return payload[Up];
    }
    int limitBps(Dir d) const override { return d == Up ? upLimit : 0; }
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

// one request, waits for its end: (ok, error, bytes)
struct Got { bool ok = false; QString err; QByteArray bytes; bool finished = false; };
Got fetch(mx::RangeSource *src, qint64 off, qint64 len, int ms = 10000)
{
    Got g;
    QMetaObject::Connection c1 = QObject::connect(src, &mx::RangeSource::data, [&](qint64, const QByteArray &b) { g.bytes += b; });
    QMetaObject::Connection c2 = QObject::connect(src, &mx::RangeSource::requestFinished, [&](qint64 o, qint64, bool ok, const QString &e) {
        if (o != off) return;
        g.ok = ok;
        g.err = e;
        g.finished = true;
    });
    src->request(off, len);
    waitFor([&] { return g.finished; }, ms);
    QObject::disconnect(c1);
    QObject::disconnect(c2);
    return g;
}

QByteArray fileBytes(const QString &path, qint64 off, qint64 len)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || !f.seek(off)) return {};
    return f.read(len);
}

// the last report of the upload row of `name` to `peer` (other sessions may still report or end)
mx::TransferInfo lastUpload(const QSignalSpy &spy, const QString &name = QString(), const QString &peer = QString())
{
    for (int i = int(spy.size()) - 1; i >= 0; --i) {
        const mx::TransferInfo t = spy.at(i).first().value<mx::TransferInfo>();
        if ((name.isEmpty() || t.name == name) && (peer.isEmpty() || t.peer.name.startsWith(peer))) return t;
    }
    return {};
}
bool finished(const QSignalSpy &spy, const QString &id)
{
    for (const auto &a : spy) if (a.first().toString() == id) return true;
    return false;
}

QJsonObject lastOf(const QList<QJsonObject> &l, const QString &t)
{
    for (int i = l.size() - 1; i >= 0; --i)
        if (l[i].value(QStringLiteral("t")).toString() == t) return l[i];
    return {};
}

} // namespace

class TstOverlayUpload : public QObject {
    Q_OBJECT
    std::unique_ptr<Instance> A, B;
    CountingLimiter limB;
    mx::SharedFile part, whole, wrongSet;
    const qint64 MiB = 1024 * 1024;

private slots:
    void initTestCase()
    {
        mx::registerMetaTypes();
        A.reset(new Instance(QStringLiteral("Guest100_10001")));
        B.reset(new Instance(QStringLiteral("Guest200_20002")));
        mx::PluginContext ctx;   // B with a counting limiter
        ctx.settings = &B->settings;
        ctx.library = &B->library;
        ctx.limiter = &limB;
        ctx.profileDir = B->profile;
        ctx.dataDir = B->profile + QStringLiteral("/overlay");
        ctx.screenName = B->settings.s(QString(mx::keys::identity::ScreenName));
        B->plugin->init(ctx);
        for (Instance *i : {A.get(), B.get()}) {
            i->plugin->setTestOptions(opts());
            i->start();
        }
        part = B->library.addGenerated(B->profile, QStringLiteral("still-downloading-24MB.bin"), 24 * MiB + 5, 9001);
        whole = B->library.addGenerated(B->profile, QStringLiteral("complete-12MB.bin"), 12 * MiB + 7, 9002);
        wrongSet = B->library.addGenerated(B->profile, QStringLiteral("wrong-set-12MB.bin"), 12 * MiB + 9, 9003);
        B->library.setPartial(part.hashes.sha1, {{0, 4 * MiB}, {8 * MiB, 10 * MiB}});
        B->plugin->setSharedFiles(B->library.completeFiles());
        QVERIFY(link(*A, *B));
    }

    // a partial file: the slot carries the map, available() follows it, data inside is right,
    // a request outside is refused by the client itself ("Range not available")
    void partialServedWithHaveMap()
    {
        mx::FileHashes h;
        h.sha1 = part.hashes.sha1;
        std::unique_ptr<mx::RangeSource> src(A->plugin->openSource(QStringLiteral("dl-p"), h, part.size, sourceFor(*B)));
        QVERIFY(src);
        src->open();
        QVERIFY(waitFor([&] { return src->state() == mx::RangeSource::Ready; }));
        const QList<QPair<qint64, qint64>> av = src->available();
        QCOMPARE(av.size(), 2);
        QCOMPARE(av.at(0), qMakePair(qint64(0), 4 * MiB));
        QCOMPARE(av.at(1), qMakePair(8 * MiB, 10 * MiB));
        Got g = fetch(src.get(), 8 * MiB + 100, MiB);
        QVERIFY2(g.ok, qPrintable(g.err));
        QCOMPARE(g.bytes, fileBytes(part.path, 8 * MiB + 100, MiB));
        const qint64 sentBefore = limB.sentUp();
        g = fetch(src.get(), 5 * MiB, MiB);
        QVERIFY(!g.ok);
        QCOMPARE(g.err, mx::RangeSource::rangeNotAvailableError());
        QCOMPARE(limB.sentUp(), sentBefore);   // nothing sent for it
        src->close();
    }

    // the map grows ("have" within ~5 s), the file completes ("full"); a stale map is answered
    // "not available" by the server and the client forgets that range
    void haveGrowsThenFullAndStaleMap()
    {
        B->library.setPartial(part.hashes.sha1, {{0, 4 * MiB}});
        mx::FileHashes h;
        h.sha1 = part.hashes.sha1;
        std::unique_ptr<mx::RangeSource> src(A->plugin->openSource(QStringLiteral("dl-g"), h, part.size, sourceFor(*B)));
        QSignalSpy avail(src.get(), &mx::RangeSource::availabilityChanged);
        src->open();
        QVERIFY(waitFor([&] { return src->state() == mx::RangeSource::Ready; }));
        QCOMPARE(src->available().size(), 1);
        B->library.setPartial(part.hashes.sha1, {{0, 6 * MiB}, {12 * MiB, 14 * MiB}});
        QElapsedTimer t;
        t.start();
        QVERIFY(waitFor([&] { return src->available().size() == 2; }, 10000));
        qInfo("the grown map arrived after %lld ms", t.elapsed());
        qInfo("map now: [%lld, %lld) [%lld, %lld)", src->available().at(0).first, src->available().at(0).second,
              src->available().at(1).first, src->available().at(1).second);
        QCOMPARE(src->available().at(0), qMakePair(qint64(0), 6 * MiB));
        // stale: the uploader holds less than it said (its own provisional set was dropped)
        B->library.setPartial(part.hashes.sha1, {{0, 1 * MiB}});
        Got g = fetch(src.get(), 2 * MiB, MiB);
        QVERIFY(!g.ok);
        QCOMPARE(g.err, mx::RangeSource::rangeNotAvailableError());
        for (const auto &r : src->available()) QVERIFY(!(r.first <= 2 * MiB && 3 * MiB <= r.second));
        // complete now: a library file
        B->library.setComplete(part.hashes.sha1);
        B->plugin->setSharedFiles(B->library.completeFiles());
        QVERIFY(waitFor([&] { return src->available() == QList<QPair<qint64, qint64>>{{0, part.size}}; }, 10000));
        g = fetch(src.get(), 20 * MiB, MiB);
        QVERIFY2(g.ok, qPrintable(g.err));
        QCOMPARE(g.bytes, fileBytes(part.path, 20 * MiB, MiB));
        src->close();
        B->library.setPartial(part.hashes.sha1, {{0, 4 * MiB}, {8 * MiB, 10 * MiB}});
        B->plugin->setSharedFiles(B->library.completeFiles());
    }

    // a 5.2 client asks without "part": a partial file is "missing" for it (it would ask for ranges
    // we do not hold); a complete file is served as before, with no map
    void oldClientAndPartialFile()
    {
        FakePeer old;
        old.name = QStringLiteral("Guest520_52052");
        QVERIFY(old.start(freshDir(QStringLiteral("old52"))));
        old.node.connectTo(B->addr());
        QString bNode;
        QVERIFY(waitFor([&] {
            for (const auto &p : old.node.peers()) bNode = p.id;
            return !bNode.isEmpty();
        }));
        old.hello(bNode);
        QVERIFY(waitFor([&] { return !lastOf(old.received, QStringLiteral("hello")).isEmpty(); }));
        auto open = [&](quint32 sid, const mx::SharedFile &f) {
            QJsonObject o{{QStringLiteral("t"), QStringLiteral("get_open")}, {QStringLiteral("v"), 1}, {QStringLiteral("sid"), double(sid)},
                          {QStringLiteral("h"), QJsonObject{{QStringLiteral("sha1"), QString::fromLatin1(f.hashes.sha1.toHex())}}},
                          {QStringLiteral("size"), double(f.size)}};
            old.send(bNode, o);
        };
        open(11, part);
        QVERIFY(waitFor([&] { return lastOf(old.received, QStringLiteral("slot")).value(QStringLiteral("sid")).toInt() == 11; }));
        QCOMPARE(lastOf(old.received, QStringLiteral("slot")).value(QStringLiteral("st")).toString(), QStringLiteral("missing"));
        open(12, whole);
        QVERIFY(waitFor([&] { return lastOf(old.received, QStringLiteral("slot")).value(QStringLiteral("sid")).toInt() == 12; }));
        const QJsonObject s = lastOf(old.received, QStringLiteral("slot"));
        QCOMPARE(s.value(QStringLiteral("st")).toString(), QStringLiteral("ready"));
        QVERIFY(!s.contains(QStringLiteral("have")));
        // its close carries no reason: the row reads "Finished", never "Complete" for a part
        QSignalSpy up(B->plugin, &mx::NetworkPlugin::uploadChanged);
        old.send(bNode, QJsonObject{{QStringLiteral("t"), QStringLiteral("get")}, {QStringLiteral("v"), 1}, {QStringLiteral("sid"), 12.0},
                                    {QStringLiteral("rq"), 1.0}, {QStringLiteral("off"), 0.0}, {QStringLiteral("len"), double(MiB)}});
        QVERIFY(waitFor([&] { return lastUpload(up, whole.name, old.name).done >= MiB; }));
        old.send(bNode, QJsonObject{{QStringLiteral("t"), QStringLiteral("get_close")}, {QStringLiteral("v"), 1}, {QStringLiteral("sid"), 12.0}});
        QVERIFY(waitFor([&] { return lastUpload(up, whole.name, old.name).state == mx::TransferState::Completed; }));
        QCOMPARE(lastUpload(up, whole.name, old.name).statusText, QStringLiteral("Finished"));
        QCOMPARE(lastUpload(up, whole.name, old.name).rateUpBps, 0);
        old.node.stop();
    }

    // sets: offered only when they reduce to the file's ed2k hash
    void hashSetOnlyWhenVerified()
    {
        auto setOf = [](const QByteArray &seed, int n) {
            QList<QByteArray> parts;
            for (int i = 0; i < n; ++i) parts << QCryptographicHash::hash(seed + QByteArray::number(i), QCryptographicHash::Md5);
            return parts;
        };
        const int n = int((whole.size + 9727999) / 9728000);
        const QList<QByteArray> good = setOf("good", n), bad = setOf("bad", n);
        QCryptographicHash md4(QCryptographicHash::Md4);
        for (const QByteArray &p : good) md4.addData(p);
        for (mx::SharedFile &f : B->library.files) {
            if (f.hashes.sha1 == whole.hashes.sha1 || f.hashes.sha1 == wrongSet.hashes.sha1) f.hashes.ed2k = md4.result();
        }
        B->library.partSets.insert(whole.hashes.sha1, good);
        B->library.partSets.insert(wrongSet.hashes.sha1, bad);   // computed over other bytes: must never go out
        B->plugin->setSharedFiles(B->library.completeFiles());
        for (const mx::SharedFile &f : {whole, wrongSet}) {
            mx::FileHashes h;
            h.sha1 = f.hashes.sha1;
            std::unique_ptr<mx::RangeSource> src(A->plugin->openSource(QStringLiteral("dl-hs"), h, f.size, sourceFor(*B)));
            QSignalSpy sets(src.get(), &mx::RangeSource::hashSetReceived);
            src->open();
            QVERIFY(waitFor([&] { return src->state() == mx::RangeSource::Ready; }));
            QTest::qWait(600);
            if (f.hashes.sha1 == whole.hashes.sha1) {
                QCOMPARE(sets.count(), 1);
                QCOMPARE(sets.first().at(2).value<QList<QByteArray>>(), good);
            } else {
                QCOMPARE(sets.count(), 0);
            }
            src->close();
        }
        B->library.partSets.clear();
    }

    // the downloader bans us: it says so, the row ends with it at once and no more bytes go out
    void bannedCloseStopsAtOnce()
    {
        limB.upLimit = 512 * 1024;   // a slow uploader: requests pile up on its side
        QTest::qWait(500);           // the end of the sessions of the test before
        mx::FileHashes h;
        h.sha1 = whole.hashes.sha1;
        QSignalSpy up(B->plugin, &mx::NetworkPlugin::uploadChanged);
        QSignalSpy fin(B->plugin, &mx::NetworkPlugin::uploadFinished);
        std::unique_ptr<mx::RangeSource> src(A->plugin->openSource(QStringLiteral("dl-b"), h, whole.size, sourceFor(*B)));
        src->open();
        QVERIFY(waitFor([&] { return src->state() == mx::RangeSource::Ready; }));
        for (int i = 0; i < src->maxPendingRequests(); ++i) src->request(qint64(i) * MiB, MiB);
        QVERIFY(waitFor([&] { return lastUpload(up, whole.name).done > 256 * 1024; }));
        const QString id = lastUpload(up, whole.name).id;
        src->setCloseReason(QStringLiteral("banned"), QStringLiteral("File Mismatch: a block failed the check"));
        src->close();
        const qint64 atClose = limB.sentUp();
        QElapsedTimer t;
        t.start();
        QVERIFY(waitFor([&] { return finished(fin, id); }, 3000));
        const qint64 ms = t.elapsed();
        QTest::qWait(1500);
        const qint64 after = limB.sentUp() - atClose;
        const mx::TransferInfo last = lastUpload(up, whole.name);
        qInfo("closed after %lld ms; %lld bytes handed out after the close; row: %s, %lld bytes", ms, after,
              qPrintable(last.statusText), last.done);
        QCOMPARE(last.state, mx::TransferState::Error);
        QCOMPARE(last.statusText, QStringLiteral("Stopped by the other side: File Mismatch: a block failed the check"));
        QCOMPARE(last.rateUpBps, 0);
        QVERIFY(ms < 2000);
        QVERIFY2(after <= 320 * 1024, "the uploader went on sending after the close");
        limB.upLimit = 0;
    }

    void closeReasonsBecomeRowTexts()
    {
        const QList<QPair<QString, QString>> cases{{QStringLiteral("done"), QStringLiteral("Finished")},
                                                   {QStringLiteral("paused"), QStringLiteral("Cancelled by remote (download paused)")},
                                                   {QStringLiteral("cancelled"), QStringLiteral("Cancelled by remote (download cancelled)")},
                                                   {QStringLiteral("quit"), QStringLiteral("Cancelled by remote (program closed)")}};
        for (const auto &c : cases) {
            mx::FileHashes h;
            h.sha1 = whole.hashes.sha1;
            QTest::qWait(300);
            QSignalSpy up(B->plugin, &mx::NetworkPlugin::uploadChanged);
            std::unique_ptr<mx::RangeSource> src(A->plugin->openSource(QStringLiteral("dl-c"), h, whole.size, sourceFor(*B)));
            src->open();
            QVERIFY(waitFor([&] { return src->state() == mx::RangeSource::Ready; }));
            Got g = fetch(src.get(), 0, 256 * 1024);
            QVERIFY(g.ok);
            src->setCloseReason(c.first, QString());
            src->close();
            QVERIFY2(waitFor([&] { return lastUpload(up, whole.name).statusText == c.second; }, 3000), qPrintable(c.first));
        }
    }

    // a 5.2 uploader: its slot has no map (a complete file), it never sends "have"; our get_close
    // carries the reason (it ignores the fields) and arrives twice (live + reliable copy)
    void toOldUploader()
    {
        FakePeer old;
        old.name = QStringLiteral("Guest521_52152");
        QVERIFY(old.start(freshDir(QStringLiteral("old52up"))));
        old.node.connectTo(A->addr());
        QString aNode;
        QVERIFY(waitFor([&] {
            for (const auto &p : old.node.peers()) aNode = p.id;
            return !aNode.isEmpty();
        }));
        old.hello(aNode);
        QVERIFY(waitFor([&] { return A->plugin->connectedUsers() >= 2; }));
        mx::SearchSource src;
        src.net = mx::Net::Overlay;
        src.user = mx::UserRef{mx::Net::Overlay, old.userId(), old.name, QString()};
        src.extra.insert(QStringLiteral("node"), old.node.nodeId());
        mx::FileHashes h;
        h.sha1 = whole.hashes.sha1;
        std::unique_ptr<mx::RangeSource> rs(A->plugin->openSource(QStringLiteral("dl-old"), h, whole.size, src));
        QVERIFY(rs);
        rs->open();
        QVERIFY(waitFor([&] { return !lastOf(old.received, QStringLiteral("get_open")).isEmpty(); }));
        const QJsonObject go = lastOf(old.received, QStringLiteral("get_open"));
        QVERIFY(go.value(QStringLiteral("part")).toBool());   // an extra field it does not know
        old.send(aNode, QJsonObject{{QStringLiteral("t"), QStringLiteral("slot")}, {QStringLiteral("v"), 1},
                                    {QStringLiteral("sid"), go.value(QStringLiteral("sid"))}, {QStringLiteral("st"), QStringLiteral("ready")}});
        QVERIFY(waitFor([&] { return rs->state() == mx::RangeSource::Ready; }));
        QCOMPARE(rs->available(), (QList<QPair<qint64, qint64>>{{0, whole.size}}));
        rs->setCloseReason(QStringLiteral("banned"), QStringLiteral("File Mismatch: a block failed the check"));
        rs->close();
        QVERIFY(waitFor([&] {
            int n = 0;
            for (const auto &o : old.received) n += o.value(QStringLiteral("t")).toString() == QLatin1String("get_close");
            return n == 2;
        }, 5000));
        const QJsonObject gc = lastOf(old.received, QStringLiteral("get_close"));
        QCOMPARE(gc.value(QStringLiteral("reason")).toString(), QStringLiteral("banned"));
        QCOMPARE(gc.value(QStringLiteral("why")).toString(), QStringLiteral("File Mismatch: a block failed the check"));
        old.node.stop();
    }

    // the downloader vanishes (crash, cable): the uploader notices by the link time-out
    void vanishedDownloaderTimesOut()
    {
        FakePeer gone;
        gone.name = QStringLiteral("Guest300_30003");
        gone.answerPings = false;
        QVERIFY(gone.start(freshDir(QStringLiteral("gone"))));
        gone.node.connectTo(B->addr());
        QString bNode;
        QVERIFY(waitFor([&] {
            for (const auto &p : gone.node.peers()) bNode = p.id;
            return !bNode.isEmpty();
        }));
        gone.hello(bNode);
        QVERIFY(waitFor([&] { return !lastOf(gone.received, QStringLiteral("hello")).isEmpty(); }));
        QSignalSpy up(B->plugin, &mx::NetworkPlugin::uploadChanged);
        gone.send(bNode, QJsonObject{{QStringLiteral("t"), QStringLiteral("get_open")}, {QStringLiteral("v"), 1}, {QStringLiteral("sid"), 21.0},
                                     {QStringLiteral("h"), QJsonObject{{QStringLiteral("sha1"), QString::fromLatin1(whole.hashes.sha1.toHex())}}},
                                     {QStringLiteral("size"), double(whole.size)}});
        QVERIFY(waitFor([&] { return lastOf(gone.received, QStringLiteral("slot")).value(QStringLiteral("st")).toString() == QLatin1String("ready"); }));
        gone.send(bNode, QJsonObject{{QStringLiteral("t"), QStringLiteral("get")}, {QStringLiteral("v"), 1}, {QStringLiteral("sid"), 21.0},
                                     {QStringLiteral("rq"), 1.0}, {QStringLiteral("off"), 0.0}, {QStringLiteral("len"), double(MiB)}});
        QVERIFY(waitFor([&] { return lastUpload(up, whole.name, gone.name).done > 0; }));
        QElapsedTimer t;
        t.start();
        // silent from now on (a crash, a pulled cable): no close, no bye, no answer to pings
        QVERIFY(waitFor([&] { return lastUpload(up, whole.name, gone.name).state == mx::TransferState::Error; }, 30000));
        qInfo("vanished downloader noticed after %lld ms: %s", t.elapsed(), qPrintable(lastUpload(up, whole.name, gone.name).statusText));
        QCOMPARE(lastUpload(up, whole.name, gone.name).statusText, QStringLiteral("Disconnected"));
        QVERIFY(t.elapsed() < 20000);
        gone.node.stop();
    }

    void cleanupTestCase()
    {
        A.reset();
        B.reset();
    }
};

QTEST_MAIN(TstOverlayUpload)
#include "tst_overlay_upload.moc"
