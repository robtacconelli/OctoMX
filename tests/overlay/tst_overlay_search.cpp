// SPDX-License-Identifier: GPL-3.0-or-later
// Overlay search (hash-rich WinMX-style hits), find-sources (+ hashesLearned), hash-link registry
// (library answers, gossiped LRU answers, conflicting answers dropped), hostile responders.
#include "ovl_test_support.h"

using namespace mx::ovl;
using namespace ovltest;

namespace {
mx::SharedFile fakeFile(const QString &name, qint64 size, bool withBt = false)
{
    mx::SharedFile f;
    f.path = QStringLiteral("/srv/share/Stuff/") + name;
    f.name = name;
    f.size = size;
    f.type = mx::mediaTypeForFileName(name);
    f.hashes.sha1 = QCryptographicHash::hash(name.toUtf8() + "1", QCryptographicHash::Sha1);
    f.hashes.sha256 = QCryptographicHash::hash(name.toUtf8() + "2", QCryptographicHash::Sha256);
    f.hashes.md5 = QCryptographicHash::hash(name.toUtf8() + "3", QCryptographicHash::Md5);
    f.hashes.ed2k = QCryptographicHash::hash(name.toUtf8() + "4", QCryptographicHash::Md4);
    f.hashes.tth = QCryptographicHash::hash(name.toUtf8() + "5", QCryptographicHash::Sha3_224).left(24);
    if (withBt) f.hashes.btih = QCryptographicHash::hash(name.toUtf8() + "6", QCryptographicHash::Sha1);
    f.bitrateKbps = 320;
    f.durationSec = 245;
    f.sampleRateHz = 44100;
    f.hashed = true;
    return f;
}
OverlayPlugin::TestOptions opts()
{
    OverlayPlugin::TestOptions o;
    o.dht = o.mdns = o.reconnect = o.pex = false;
    o.listenPort = 0;
    return o;
}
QList<mx::SearchHit> allHits(const QSignalSpy &spy, quint32 id)
{
    QList<mx::SearchHit> out;
    for (const auto &a : spy)
        if (a.at(0).toUInt() == id) out += a.at(1).value<QList<mx::SearchHit>>();
    return out;
}
} // namespace

class TstOverlaySearch : public QObject {
    Q_OBJECT
    std::unique_ptr<Instance> A, B, C;

private slots:
    void initTestCase()
    {
        mx::registerMetaTypes();
        A.reset(new Instance(QStringLiteral("Alice")));
        B.reset(new Instance(QStringLiteral("Bob")));
        C.reset(new Instance(QStringLiteral("Carol")));
        for (Instance *i : {A.get(), B.get(), C.get()}) {
            i->plugin->setTestOptions(opts());
            i->start();
        }
        B->library.files << fakeFile(QStringLiteral("Moss Lantern - Around the Pond.mp3"), 5123456, true)
                         << fakeFile(QStringLiteral("Moss Lantern - One More Lap.mp3"), 6000000)
                         << fakeFile(QStringLiteral("holiday video.avi"), 700000000);
        C->library.files << fakeFile(QStringLiteral("moss_lantern-paper_boats.flac"), 30000000);
        B->setShared();
        C->setShared();
        QVERIFY(link(*A, *B));
        QVERIFY(link(*B, *C));   // C is two hops from A
        QTest::qWait(1500);      // GossipSub mesh on mx/search
    }

    void keywordSearch()
    {
        QSignalSpy hits(A->plugin, &mx::NetworkPlugin::searchHits);
        QSignalSpy done(A->plugin, &mx::NetworkPlugin::searchDone);
        mx::SearchQuery q;
        q.id = 11;
        q.text = QStringLiteral("moss lantern");
        A->plugin->search(q);
        QVERIFY(waitFor([&] { return done.count() == 1; }, 15000));
        const auto h = allHits(hits, 11);
        QStringList names;
        for (const auto &x : h) names << x.fileName;
        qInfo("hits: %s", qPrintable(names.join(QStringLiteral(" | "))));
        QCOMPARE(h.size(), 3);   // 2 from Bob, 1 from Carol (2 hops: Carol connects to Alice on demand)
        const mx::SearchHit *around = nullptr;
        for (const auto &x : h)
            if (x.fileName.contains(QStringLiteral("Around"))) around = &x;
        QVERIFY(around);
        const mx::SharedFile &src = B->library.files.first();
        QCOMPARE(around->size, src.size);
        QCOMPARE(around->hashes.sha1, src.hashes.sha1);
        QCOMPARE(around->hashes.sha256, src.hashes.sha256);
        QCOMPARE(around->hashes.md5, src.hashes.md5);
        QCOMPARE(around->hashes.ed2k, src.hashes.ed2k);
        QCOMPARE(around->hashes.tth, src.hashes.tth);
        QCOMPARE(around->hashes.btih, src.hashes.btih);
        QCOMPARE(around->bitrateKbps, 320);
        QCOMPARE(around->durationSec, 245);
        QCOMPARE(around->sampleRateHz, 44100);
        QCOMPARE(around->type, mx::MediaType::Audio);
        QCOMPARE(around->path, QStringLiteral("\\Stuff\\Moss Lantern - Around the Pond.mp3"));
        QCOMPARE(around->sources.size(), 1);
        const mx::SearchSource &s = around->sources.first();
        QCOMPARE(s.net, mx::Net::Overlay);
        QCOMPARE(s.user.id, B->me().id);
        QCOMPARE(s.user.name, B->me().name);
        QCOMPARE(s.connection, QStringLiteral("DSL"));
        QCOMPARE(s.totalSlots, B->plugin->totalSlots());   // transfers/maxUploads
        QVERIFY(s.totalSlots > 0);
        QCOMPARE(s.freeSlots, s.totalSlots);
        bool fromCarol = false;
        for (const auto &x : h) fromCarol |= x.sources.first().user.id == C->me().id;
        QVERIFY(fromCarol);
    }

    void filters()
    {
        QSignalSpy hits(A->plugin, &mx::NetworkPlugin::searchHits);
        QSignalSpy done(A->plugin, &mx::NetworkPlugin::searchDone);
        mx::SearchQuery q;
        q.id = 12;
        q.text = QStringLiteral("moss");
        q.extension = QStringLiteral("flac");
        A->plugin->search(q);
        mx::SearchQuery v;
        v.id = 13;
        v.text = QStringLiteral("holiday");
        v.type = mx::MediaType::Audio;   // it is a video: no hit
        A->plugin->search(v);
        mx::SearchQuery m;
        m.id = 14;
        m.text = QStringLiteral("moss lantern");
        m.maxResults = 1;
        A->plugin->search(m);
        // [ui-f] several extensions go as "exts" (Lossless audio), the video under Video
        mx::SearchQuery l;
        l.id = 15;
        l.text = QStringLiteral("moss");
        mx::FileTypeFilter::fromCode(QStringLiteral("lossless")).applyTo(l);
        QCOMPARE(l.extensions.size() > 1, true);
        A->plugin->search(l);
        mx::SearchQuery vv;
        vv.id = 16;
        vv.text = QStringLiteral("holiday");
        mx::FileTypeFilter::fromCode(QStringLiteral("video")).applyTo(vv);
        A->plugin->search(vv);
        QVERIFY(waitFor([&] { return done.count() == 5; }, 15000));
        QCOMPARE(allHits(hits, 12).size(), 1);
        QVERIFY(allHits(hits, 12).first().fileName.endsWith(QStringLiteral(".flac")));
        QCOMPARE(allHits(hits, 13).size(), 0);
        QCOMPARE(allHits(hits, 14).size(), 1);
        QCOMPARE(allHits(hits, 15).size(), 1);
        QVERIFY(allHits(hits, 15).first().fileName.endsWith(QStringLiteral(".flac")));
        QCOMPARE(allHits(hits, 16).size(), 1);
    }

    void aloneSearchFinishesQuickly()
    {
        Instance lonely(QStringLiteral("Lonely"));
        lonely.plugin->setTestOptions(opts());
        lonely.start();
        QSignalSpy done(lonely.plugin, &mx::NetworkPlugin::searchDone);
        mx::SearchQuery q;
        q.id = 1;
        q.text = QStringLiteral("anything");
        QElapsedTimer t;
        t.start();
        lonely.plugin->search(q);
        QVERIFY(waitFor([&] { return done.count() == 1; }, 5000));
        QVERIFY(t.elapsed() < 3500);
    }

    void hostileResponder()
    {
        // a peer answering with hits for a query that does not match, unknown qids and junk types
        FakePeer evil;
        QVERIFY(evil.start(freshDir(QStringLiteral("evil"))));
        evil.node.connectTo(A->addr());
        QVERIFY(waitFor([&] { return evil.node.peerCount() == 1; }));
        evil.hello(A->plugin->nodeId());
        QVERIFY(waitFor([&] { return A->plugin->connectedUsers() == 2; }));
        evil.node.subscribe(QStringLiteral("mx/search"));
        QTest::qWait(1500);
        QSignalSpy hits(A->plugin, &mx::NetworkPlugin::searchHits);
        mx::SearchQuery q;
        q.id = 21;
        q.text = QStringLiteral("around pond");
        A->plugin->search(q);
        // learn the qid from the published query (evil subscribes to mx/search)
        QString qid;
        connect(&evil.node, &RatsNode::topicMessage, this, [&](const QString &, const QString &, const QByteArray &d) {
            const QJsonObject b = QJsonDocument::fromJson(d.mid(2 + 32 + 64)).object();
            if (b.value(QStringLiteral("t")).toString() == QLatin1String("q")) qid = b.value(QStringLiteral("qid")).toString();
        });
        QVERIFY(waitFor([&] { return !qid.isEmpty(); }, 5000));
        auto item = [](const QString &n, QJsonValue size) {
            QJsonObject f;
            f[QStringLiteral("n")] = n;
            f[QStringLiteral("s")] = size;
            return f;
        };
        QJsonObject a;
        a[QStringLiteral("t")] = QStringLiteral("hits");
        a[QStringLiteral("v")] = 1;
        a[QStringLiteral("qid")] = qid;
        a[QStringLiteral("items")] = QJsonArray{item(QStringLiteral("stray result.mp3"), 100),
                                                item(QStringLiteral("around the pond (fake).mp3"), QStringLiteral("big")),
                                                QStringLiteral("not an object"),
                                                item(QStringLiteral("around the pond (evil).mp3"), 1234)};
        evil.send(A->plugin->nodeId(), a);
        QJsonObject wrong = a;
        wrong[QStringLiteral("qid")] = QStringLiteral("00000000deadbeef");
        evil.send(A->plugin->nodeId(), wrong);
        evil.sendRaw(A->plugin->nodeId(), QStringLiteral("mx"), QByteArray("Z\xff\xff\xff\xffjunk", 9));
        evil.sendRaw(A->plugin->nodeId(), QStringLiteral("mx"), QByteArray("J{not json"));
        QVERIFY(waitFor([&] {
            for (const auto &x : allHits(hits, 21)) if (x.sources.first().user.id == evil.userId()) return true;
            return false;
        }, 5000));
        int fromEvil = 0;
        for (const auto &x : allHits(hits, 21))
            if (x.sources.first().user.id == evil.userId()) {
                ++fromEvil;
                QCOMPARE(x.fileName, QStringLiteral("around the pond (evil).mp3"));
            }
        QCOMPARE(fromEvil, 1);   // stray / malformed items dropped; A still alive
        evil.node.stop();
    }

    void findSources()
    {
        const mx::SharedFile &f = B->library.files.at(1);
        mx::FileHashes known;
        known.sha1 = f.hashes.sha1;
        QSignalSpy found(A->plugin, &mx::NetworkPlugin::sourcesFound);
        QSignalSpy learned(A->plugin, &mx::NetworkPlugin::hashesLearned);
        A->plugin->findSources(QStringLiteral("dl-1"), known, f.size, f.name);
        QVERIFY(waitFor([&] { return found.count() >= 1; }, 10000));
        const auto srcs = found.first().at(1).value<QList<mx::SearchSource>>();
        QCOMPARE(found.first().at(0).toString(), QStringLiteral("dl-1"));
        QCOMPARE(srcs.size(), 1);
        QCOMPARE(srcs.first().net, mx::Net::Overlay);
        QCOMPARE(srcs.first().user.id, B->me().id);
        QVERIFY(learned.count() >= 1);
        const auto more = learned.first().at(1).value<mx::FileHashes>();
        QCOMPARE(more.sha256, f.hashes.sha256);
        QCOMPARE(more.ed2k, f.hashes.ed2k);
        // wrong size: nobody answers
        QSignalSpy found2(A->plugin, &mx::NetworkPlugin::sourcesFound);
        A->plugin->findSources(QStringLiteral("dl-2"), known, f.size + 1, f.name);
        QTest::qWait(2500);
        QCOMPARE(found2.count(), 0);
        A->plugin->stopFindSources(QStringLiteral("dl-1"));
        A->plugin->stopFindSources(QStringLiteral("dl-2"));
    }

    void hashLinksFromLibrary()
    {
        const mx::SharedFile &f = C->library.files.first();   // Carol's file, 2 hops
        mx::FileHashes known;
        known.ed2k = f.hashes.ed2k;
        QSignalSpy hl(A->plugin, &mx::NetworkPlugin::hashLinksFound);
        A->plugin->lookupHashLinks(known, f.size);
        QVERIFY(waitFor([&] { return hl.count() >= 1; }, 10000));
        const auto all = hl.first().at(1).value<mx::FileHashes>();
        QCOMPARE(hl.first().at(0).value<mx::FileHashes>().ed2k, known.ed2k);
        QCOMPARE(all.sha1, f.hashes.sha1);
        QCOMPARE(all.sha256, f.hashes.sha256);
        QCOMPARE(all.tth, f.hashes.tth);
        QCOMPARE(hl.first().at(2).toLongLong(), f.size);
    }

    void hashLinksGossiped()
    {
        // B publishes the hash set of a file it completed (and does not share); C caches it from
        // the gossip, so A's lookup is answered although no library holds the file.
        mx::FileHashes all;
        all.btih = QByteArray::fromHex("dd8255ecdc7ca55fb0bbf81323d87062db1f6d1c");
        all.btFileIndex = 1;
        all.sha1 = QByteArray(20, 'S');
        all.ed2k = QByteArray(16, 'E');
        const qint64 size = 263600000;
        B->plugin->publishHashLinks(all, size, QStringLiteral("Big Buck Bunny.avi"));
        QTest::qWait(800);
        B->plugin->stop();   // B cannot answer any more; only C's LRU can
        QVERIFY(waitFor([&] { return A->plugin->connectedUsers() == 0 || A->plugin->connectedUsers() == 1; }, 5000));
        // A <-> C directly (they met during the search test)
        if (!A->plugin->node()->isConnected(C->plugin->nodeId())) QVERIFY(link(*C, *A));
        QTest::qWait(1500);
        mx::FileHashes known;
        known.ed2k = all.ed2k;
        QSignalSpy hl(A->plugin, &mx::NetworkPlugin::hashLinksFound);
        A->plugin->lookupHashLinks(known, size);
        QVERIFY(waitFor([&] { return hl.count() >= 1; }, 10000));
        const auto got = hl.first().at(1).value<mx::FileHashes>();
        QCOMPARE(got.btih, all.btih);
        QCOMPARE(got.sha1, all.sha1);
        // size mismatch: dropped by the answerer and by the asker
        QSignalSpy hl2(A->plugin, &mx::NetworkPlugin::hashLinksFound);
        A->plugin->lookupHashLinks(known, size + 5);
        QTest::qWait(2000);
        bool wrong = false;
        for (const auto &a : hl2) wrong |= a.at(2).toLongLong() == size;
        QVERIFY(!wrong);
        // persisted LRU (C): survives a restart
        C->plugin->stop();
        QVERIFY(QFile::exists(C->profile + QStringLiteral("/overlay/hashlinks.json")));
    }

    void conflictingAnswerDropped()
    {
        Instance X(QStringLiteral("Xavier"));
        X.plugin->setTestOptions(opts());
        X.start();
        FakePeer evil;
        QVERIFY(evil.start(freshDir(QStringLiteral("evil2"))));
        evil.node.subscribe(QStringLiteral("mx/search"));
        evil.node.connectTo(X.addr());
        QVERIFY(waitFor([&] { return evil.node.peerCount() == 1; }));
        evil.hello(X.plugin->nodeId());
        QVERIFY(waitFor([&] { return X.plugin->connectedUsers() == 1; }));
        QTest::qWait(1500);
        QString qid;
        connect(&evil.node, &RatsNode::topicMessage, this, [&](const QString &, const QString &, const QByteArray &d) {
            const QJsonObject b = QJsonDocument::fromJson(d.mid(98)).object();
            if (b.value(QStringLiteral("t")).toString() == QLatin1String("hl_q")) qid = b.value(QStringLiteral("qid")).toString();
        });
        mx::FileHashes known;
        known.sha1 = QByteArray(20, 'A');
        known.ed2k = QByteArray(16, 'B');
        QSignalSpy hl(X.plugin, &mx::NetworkPlugin::hashLinksFound);
        X.plugin->lookupHashLinks(known, 1000);
        QVERIFY(waitFor([&] { return !qid.isEmpty(); }, 5000));
        auto answer = [&](const mx::FileHashes &h, qint64 size) {
            QJsonObject a;
            a[QStringLiteral("t")] = QStringLiteral("hl");
            a[QStringLiteral("v")] = 1;
            a[QStringLiteral("qid")] = qid;
            a[QStringLiteral("h")] = hashesToJson(h);
            a[QStringLiteral("size")] = double(size);
            evil.send(X.plugin->nodeId(), a);
        };
        mx::FileHashes conflicting = known;
        conflicting.ed2k = QByteArray(16, 'Z');   // same sha1, different ed2k → contradiction
        conflicting.btih = QByteArray(20, 'T');
        answer(conflicting, 1000);
        mx::FileHashes wrongSize = known;
        wrongSize.btih = QByteArray(20, 'T');
        answer(wrongSize, 999);
        mx::FileHashes good = known;
        good.btih = QByteArray(20, 'G');
        answer(good, 1000);
        QVERIFY(waitFor([&] { return hl.count() >= 1; }, 5000));
        QTest::qWait(300);
        QCOMPARE(hl.count(), 1);
        QCOMPARE(hl.first().at(1).value<mx::FileHashes>().btih, QByteArray(20, 'G'));
        evil.node.stop();
    }

    void cleanupTestCase()
    {
        A.reset();
        B.reset();
        C.reset();
        QVERIFY(RatsNode::waitAllStopped(8000));
    }
};

QTEST_GUILESS_MAIN(TstOverlaySearch)
#include "tst_overlay_search.moc"
