// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX engine tests: [engine-4] every known source is a row, discovery after one click (the
// search hit's sources and later merges, background size+name search, provisional candidates),
// per-download Auto Find Sources / Auto Enter Queue.
#include <QtCore/QJsonDocument>
#include <QtTest/QtTest>

#include "core/network_manager.h"
#include "core/search_manager.h"
#include "engine_env.h"
#include "mxhash/tiger_tree.h"

#include <QtCore/QRandomGenerator>

using namespace mx;
using namespace mx::etest;

namespace {

DownloadRequest req(const QString &name, const QByteArray &data, const FileHashes &h, const QList<SearchSource> &src)
{
    DownloadRequest r;
    r.name = name;
    r.size = data.size();
    r.hashes = h;
    r.sources = src;
    return r;
}

const TransferSource *row(const TransferInfo &t, const QString &userName)
{
    for (const TransferSource &s : t.sources)
        if (s.user.name == userName) return &s;
    return nullptr;
}

QString rowStatus(const TransferInfo &t, const QString &userName)
{
    const TransferSource *s = row(t, userName);
    return s ? s->statusText : QStringLiteral("<no row>");
}

QString dump(const TransferInfo &t)
{
    QStringList l;
    for (const TransferSource &s : t.sources) l << QStringLiteral("[%1|%2|%3]").arg(netShortName(s.net), s.user.name, s.statusText);
    return t.statusText + QStringLiteral(" :: ") + l.join(QLatin1Char(' '));
}

SearchSource candidate(SearchSource s, const FileHashes &claim)
{
    s.extra.insert(QStringLiteral("candidate"), true);
    s.extra.insert(QStringLiteral("hashes"), claim.toVariant());
    return s;
}

// A profile with a NetworkManager + SearchManager in front of the engine (as in RealCore).
struct SearchEnv {
    explicit SearchEnv(const QString &name)
    {
        root = testRoot() + QLatin1Char('/') + name;
        QDir(root).removeRecursively();
        QDir().mkpath(root);
        settings.setValue(QString(keys::transfers::IncomingDir), root + QStringLiteral("/incoming"));
        settings.setValue(QString(keys::transfers::IncompleteDir), root + QStringLiteral("/incomplete"));
        settings.setValue(QString(keys::transfers::MinFreeSpaceMB), 0);
        QString err;
        db.open(root + QStringLiteral("/octomx.db"), &err);
        tuning = fastTuning();
    }
    ~SearchEnv()
    {
        if (engine) engine->shutdown();
        engine.reset();
        search.reset();
        nm.reset();   // deletes the plugins
        db.close();
    }
    TestPlugin *add(Net n, int caps)
    {
        auto *p = new TestPlugin(n, caps);
        plugins << p;
        return p;
    }
    void up()
    {
        nm = std::make_unique<NetworkManager>(&settings, plugins);
        nm->initPlugins(root, nullptr, nullptr);
        nm->startNetworks();
        search = std::make_unique<SearchManager>(&settings, nm.get(), nullptr);
        search->setFlushInterval(50);
        EngineContext ctx;
        ctx.settings = &settings;
        ctx.db = &db;
        ctx.uploads = &uploads;
        ctx.plugins = nm->plugins();
        ctx.profileDir = root;
        ctx.search = search.get();
        engine = std::make_unique<DownloadEngine>(ctx);
        engine->setTuning(tuning);
        engine->start();
    }
    TransferInfo info(const QString &id) const { return engine->transfer(id); }

    QString root;
    SimpleSettings settings;
    Database db;
    UploadTracker uploads{&settings};
    QList<NetworkPlugin *> plugins;
    std::unique_ptr<NetworkManager> nm;
    std::unique_ptr<SearchManager> search;
    std::unique_ptr<DownloadEngine> engine;
    engine::Tuning tuning;
};

} // namespace

QList<QByteArray> fakeTthLeaves(qint64 size, qint64 unit, quint32 seed)
{
    QList<QByteArray> l;
    QRandomGenerator g(seed);
    for (qint64 i = 0; i < (size + unit - 1) / unit; ++i) {
        QByteArray h(24, Qt::Uninitialized);
        for (char &c : h) c = char(g.bounded(256));
        l << h;
    }
    return l;
}

class TstEngineSources : public QObject {
    Q_OBJECT
private slots:
    void everyKnownSourceListed();
    void manySourcesSummarised();
    void idleDownloadDoesNotFloodTheUi();
    void torrentTopPeersAndChannels();
    void hitSourcesFollowed();
    void provisionalCandidateWrongFileBanned();
    void nameSearchFindsCandidates();
    void autoFindSourcesPerDownload();
    void autoEnterQueuePerDownload();
    void hitNumbers();
    void provisionalNeedsWholeFileHash();
    void indexOnlyHitsAlwaysAnswered();
    void gnutellaLyingTreeHashBanned();
};

// One download, four sources in four states + a Kad search hit without an address: each source is a
// row with a truthful status, the Kad hit is a "network" row (not a source anyone can connect to).
void TstEngineSources::everyKnownSourceListed()
{
    Env env(QStringLiteral("rows"));
    auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges | NetworkPlugin::CanFindSources);
    auto *gn = env.addPlugin(Net::Gnutella, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(12 * 1000 * 1000, 401);
    const QByteArray other = randomBytes(12 * 1000 * 1000, 402);
    SourceConfig fast, queued, refused, cand;
    fast.data = queued.data = refused.data = data;
    cand.data = other;
    fast.bytesPerSec = 200 * 1024;
    queued.queuedMs = 600000;
    refused.failOpen = true;
    const SearchSource a = ed->addSource(QStringLiteral("ed2k-peer://1.0.0.1:4662"), fast, QStringLiteral("alice"));
    const SearchSource b = ed->addSource(QStringLiteral("ed2k-peer://1.0.0.2:4662"), queued, QStringLiteral("bob"));
    const SearchSource c = ed->addSource(QStringLiteral("ed2k-peer://1.0.0.3:4662"), refused, QStringLiteral("carol"));
    const SearchSource d = candidate(gn->addSource(QStringLiteral("gnut://1.0.0.4/f"), cand, QStringLiteral("dave")), hashesOf(other, 1));
    SearchSource kad;   // a Kad keyword hit: the network knows 2 sources, no address yet
    kad.net = Net::Kad;
    kad.uri = QStringLiteral("ed2k://|file|rows.bin|12000000|%1|/").arg(QString::fromLatin1(hashesOf(data, 2).ed2k.toHex()));
    kad.seeds = 2;
    env.engineUp();
    const QString id = env.engine->download(req(QStringLiteral("rows.bin"), data, hashesOf(data, 2), {a, b, c, d, kad}));
    QTRY_VERIFY2_WITH_TIMEOUT(rowStatus(env.info(id), QStringLiteral("alice")) == QLatin1String("Transferring"), qPrintable(dump(env.info(id))), 5000);
    QTRY_VERIFY2_WITH_TIMEOUT(rowStatus(env.info(id), QStringLiteral("bob")) == QLatin1String("Remotely Queued 7"), qPrintable(dump(env.info(id))), 5000);
    QTRY_VERIFY2_WITH_TIMEOUT(rowStatus(env.info(id), QStringLiteral("carol")).contains(QLatin1String("Connection Refused")),
                              qPrintable(dump(env.info(id))), 5000);
    const TransferInfo t = env.info(id);
    qInfo("rows: %s", qPrintable(dump(t)));
    QCOMPARE(t.sourcesTotal, 4);                      // the Kad hit is no source
    QCOMPARE(rowStatus(t, QStringLiteral("dave")), QStringLiteral("Not used yet: unverified (needs a part hash set)"));
    QCOMPARE(rowStatus(t, QStringLiteral("Kad network")), QStringLiteral("Search hit (2 sources): asking for addresses"));
    QVERIFY(rowStatus(t, QStringLiteral("carol")).startsWith(QLatin1String("File/User Unavailable (Connection Refused), retry in"))
            || rowStatus(t, QStringLiteral("carol")).startsWith(QLatin1String("Connecting")));
    for (const QString &u : {QStringLiteral("alice"), QStringLiteral("bob"), QStringLiteral("carol"), QStringLiteral("dave")}) {
        const TransferSource *s = row(t, u);
        QVERIFY(s);
        QVERIFY(!s->id.isEmpty());                    // addressable (cancelSource / priority)
    }
    QCOMPARE(row(t, QStringLiteral("dave"))->net, Net::Gnutella);
    QVERIFY(row(t, QStringLiteral("alice"))->rateBps > 0);
    QCOMPARE(t.sources.first().user.name, QStringLiteral("alice"));   // transferring rows first
    QCOMPARE(t.autoFindSourcesMin, 10);                // global defaults reported per download
    QCOMPARE(t.autoEnterQueue, 100);
    QVERIFY(t.networks.contains(Net::ED2K));
    // [core-5] the parent row's Network column also names networks that only have known sources
    // (dave's Gnutella copy is not used yet), so a multi-source download never shows a blank cell
    QVERIFY2(t.networks.contains(Net::Gnutella), qPrintable(QString::number(t.networks.size())));
    QVERIFY(!t.networks.contains(Net::Unknown));
    // a source the user removes is no row any more
    env.engine->cancelSource(id, row(t, QStringLiteral("dave"))->id);
    QVERIFY(!row(env.info(id), QStringLiteral("dave")));
    QVERIFY(!env.info(id).networks.contains(Net::Gnutella));   // and no longer a network of the download
}

void TstEngineSources::manySourcesSummarised()
{
    Env env(QStringLiteral("many"));
    auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(2 * 1000 * 1000, 403);
    SourceConfig q;
    q.data = data;
    q.queuedMs = 600000;
    QList<SearchSource> srcs;
    for (int i = 0; i < 80; ++i)
        srcs << ed->addSource(QStringLiteral("ed2k-peer://2.0.%1.%2:4662").arg(i / 200).arg(i % 200), q, QStringLiteral("peer%1").arg(i));
    env.engineUp();
    const QString id = env.engine->download(req(QStringLiteral("many.bin"), data, hashesOf(data, 2), srcs));
    QTRY_VERIFY_WITH_TIMEOUT(env.info(id).state == TransferState::RemotelyQueued, 5000);
    const TransferInfo t = env.info(id);
    QCOMPARE(t.sourcesTotal, 80);
    QCOMPARE(t.sources.size(), 61);                    // 60 rows + one summary row
    QCOMPARE(t.sources.last().user.name, QStringLiteral("+ 20 more sources"));
    QVERIFY2(t.sources.last().statusText.contains(QLatin1String("20 ")), qPrintable(t.sources.last().statusText));
    QVERIFY(t.sources.last().id.isEmpty());            // not addressable
    QCOMPARE(t.sources.last().net, Net::ED2K);
}

// A paused download with sources sends no transferChanged while nothing visible changes.
void TstEngineSources::idleDownloadDoesNotFloodTheUi()
{
    Env env(QStringLiteral("idle"));
    auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(2 * 1000 * 1000, 404);
    SourceConfig q;
    q.data = data;
    q.queuedMs = 600000;
    QList<SearchSource> srcs;
    for (int i = 0; i < 30; ++i) srcs << ed->addSource(QStringLiteral("ed2k-peer://3.0.0.%1:4662").arg(i + 1), q, QStringLiteral("q%1").arg(i));
    env.engineUp();
    const QString id = env.engine->download(req(QStringLiteral("idle.bin"), data, hashesOf(data, 2), srcs));
    QTRY_VERIFY_WITH_TIMEOUT(env.info(id).state == TransferState::RemotelyQueued, 5000);
    QSignalSpy busy(env.engine.get(), &TransferService::transferChanged);
    QTest::qWait(2500);
    const int whileQueued = int(busy.count());
    env.engine->pause(id);
    QTest::qWait(1500);
    QSignalSpy changed(env.engine.get(), &TransferService::transferChanged);
    QTest::qWait(3000);
    qInfo("transferChanged: %d in 2.5 s while 30 sources wait in queues, %d in 3 s paused", whileQueued, int(changed.count()));
    QVERIFY(whileQueued <= 2);
    QCOMPARE(changed.count(), 0);
}

// BitTorrent: the discovery channels (DHT, tracker), web seeds one by one, the top 8 peers by speed
// and one "+ N more peers" row.
void TstEngineSources::torrentTopPeersAndChannels()
{
    Env env(QStringLiteral("bt-rows"));
    FakeTorrentBackend be;
    const QByteArray data = randomBytes(2 * 1024 * 1024, 405);
    const QString tid = be.makeTorrent(QStringLiteral("swarm.bin"), {{QStringLiteral("swarm.bin"), data}}, 65536);
    env.engineUp(&be);
    DownloadRequest r;
    r.uri = QStringLiteral("magnet:?xt=urn:btih:%1&dn=swarm.bin").arg(tid);
    const QString id2 = env.engine->download(r);
    QVERIFY(!id2.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(be.active.contains(tid) && be.active.value(tid).metadata, 3000);
    FakeTorrent &t = be.active[tid];
    t.peers = 30;
    for (int i = 0; i < 30; ++i) {
        TransferSource p;
        p.net = Net::BitTorrent;
        p.user.name = QStringLiteral("peer%1").arg(i);
        p.rateBps = i < 12 ? (i + 1) * 1000 : 0;
        p.bytes = 1000 * i;
        p.statusText = p.rateBps ? QStringLiteral("Transferring") : QStringLiteral("Connected");
        t.peerRows << p;
    }
    TransferSource web;
    web.net = Net::Http;
    web.user.name = QStringLiteral("Web seed 5.0.0.1:80");
    web.rateBps = 500;
    web.statusText = QStringLiteral("Transferring");
    t.peerRows << web;
    TransferSource dht, tracker;
    dht.user.name = QStringLiteral("DHT");
    dht.statusText = QStringLiteral("17 peers connected");
    tracker.user.name = QStringLiteral("tracker.example.org");
    tracker.statusText = QStringLiteral("Working (12 seeds, 40 peers)");
    t.channels << dht << tracker;
    QTRY_VERIFY_WITH_TIMEOUT(row(env.info(id2), QStringLiteral("+ 22 more peers")), 4000);
    const TransferInfo ti = env.info(id2);
    qInfo("rows: %s", qPrintable(dump(ti)));
    // [eng-1] first one line that says what BitTorrent does, then the channels
    QCOMPARE(ti.sources.value(0).user.name, QStringLiteral("BitTorrent swarm"));
    QCOMPARE(ti.sources.value(0).net, Net::BitTorrent);
    QVERIFY2(ti.sources.value(0).statusText.contains(QLatin1String("connected")), qPrintable(ti.sources.value(0).statusText));
    QCOMPARE(ti.sources.value(1).user.name, QStringLiteral("DHT"));
    QCOMPARE(ti.sources.value(1).net, Net::BitTorrent);
    QCOMPARE(ti.sources.value(2).statusText, QStringLiteral("Working (12 seeds, 40 peers)"));
    QCOMPARE(ti.sources.value(3).net, Net::Http);                     // the web seed
    QCOMPARE(ti.sources.value(4).user.name, QStringLiteral("peer11")); // fastest peer first
    int peerRows = 0;
    for (const TransferSource &s : ti.sources) if (s.user.name.startsWith(QLatin1String("peer"))) ++peerRows;
    QCOMPARE(peerRows, 8);
    const TransferSource *more = row(ti, QStringLiteral("+ 22 more peers"));
    QVERIFY(more);
    QCOMPARE(more->rateBps, 1000 + 2000 + 3000 + 4000);               // peers 0..3 are in the summary
    QVERIFY2(more->statusText.isEmpty(), qPrintable(more->statusText));   // [core-5] some send: the speed says it
    // [core-5] nobody in the summary sends: one status in the original WinMX wording, never raw peer texts
    for (TransferSource &p : t.peerRows) if (p.net == Net::BitTorrent && p.rateBps > 0 && p.rateBps <= 4000) p.rateBps = 0;
    QTRY_COMPARE_WITH_TIMEOUT(row(env.info(id2), QStringLiteral("+ 22 more peers"))->statusText, QStringLiteral("Connected, Negotiating..."), 4000);
    for (TransferSource &p : t.peerRows) if (p.net == Net::BitTorrent && p.rateBps == 0) p.statusText = QStringLiteral("Remotely Queued");
    QTRY_COMPARE_WITH_TIMEOUT(row(env.info(id2), QStringLiteral("+ 22 more peers"))->statusText, QStringLiteral("Queued"), 4000);
    QVERIFY(ti.networks.contains(Net::BitTorrent));
}

// The UI hands over an old copy of the hit (one source); the search merges a Gnutella copy (a
// size+name candidate) into the hit later. The download uses the hit's current sources, follows
// it, and, the only eD2k peer refusing every connection, so no part hash set ever arrives, uses
// the candidate provisionally; the whole-file check against the trusted ed2k hash decides.
void TstEngineSources::hitSourcesFollowed()
{
    SearchEnv env(QStringLiteral("follow"));
    env.tuning.provisionalGraceMs = 1500;
    env.tuning.nameSearchDelayMs = 600000;   // not this test
    auto *ed = env.add(Net::ED2K, NetworkPlugin::CanSearch | NetworkPlugin::CanFetchRanges | NetworkPlugin::CanFindSources);
    auto *gn = env.add(Net::Gnutella, NetworkPlugin::CanSearch | NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(10 * 1000 * 1000, 406);
    const QString name = QStringLiteral("Big_Buck_Bunny_640x360.m4v");
    SourceConfig refused, good;
    refused.failOpen = true;
    refused.data = data;
    good.data = data;
    good.bytesPerSec = 4 << 20;
    SearchHit eh = test::FakePlugin::makeHit(name, data.size(), hashesOf(data, 2), Net::ED2K, QStringLiteral("edpeer"));
    eh.sources.first() = ed->addSource(QStringLiteral("ed2k-peer://9.9.9.9:4662"), refused, QStringLiteral("edpeer"));
    eh.sources.first().user.address = QStringLiteral("9.9.9.9:4662");
    ed->cannedHits << eh;
    SearchHit gh = test::FakePlugin::makeHit(name, data.size(), hashesOf(data, 1), Net::Gnutella, QStringLiteral("gnutuser"));
    gh.sources.first() = gn->addSource(QStringLiteral("gnut://8.8.8.8/f"), good, QStringLiteral("gnutuser"));
    gh.sources.first().user.address = QStringLiteral("8.8.8.8:6346");
    gn->cannedHits << gh;
    gn->respondDelayMs = 1500;               // the Gnutella copy merges after the double-click
    env.up();
    SearchQuery q;
    q.text = QStringLiteral("big buck bunny");
    const quint32 sid = env.search->startSearch(q);
    QTRY_VERIFY_WITH_TIMEOUT(!env.search->hits(sid).isEmpty(), 2000);
    SearchHit stale = env.search->hits(sid).first();
    QCOMPARE(stale.sources.size(), 1);
    const QString id = env.engine->downloadHit(stale);
    QVERIFY(!id.isEmpty());
    QTRY_VERIFY2_WITH_TIMEOUT(row(env.info(id), QStringLiteral("gnutuser")), qPrintable(dump(env.info(id))), 5000);
    QVERIFY(row(env.info(id), QStringLiteral("edpeer")));
    QTRY_VERIFY2_WITH_TIMEOUT(env.info(id).state == TransferState::Completed, qPrintable(dump(env.info(id))), 20000);
    const TransferInfo t = env.info(id);
    QFile f(t.savePath);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), data);
    const QVariantMap byNet = env.engine->debugInfo(id).value(QStringLiteral("verifiedByNet")).toMap();
    qInfo("verified by network: %s", qPrintable(QString::fromUtf8(QJsonDocument::fromVariant(byNet).toJson(QJsonDocument::Compact))));
    QCOMPARE(byNet.value(QStringLiteral("Gnutella")).toLongLong(), qint64(data.size()));
    QCOMPARE(t.hashes.sha1, hashesOf(data, 1).sha1);   // the candidate's claim is now part of the identity
}

// A candidate with the wrong file (same size and name) is tried provisionally, fails the whole-file
// check, is banned and its claim rejected; a correct candidate found later completes the file.
void TstEngineSources::provisionalCandidateWrongFileBanned()
{
    Env env(QStringLiteral("prov-wrong"));
    env.tuning.provisionalGraceMs = 1000;
    auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges | NetworkPlugin::CanFindSources);
    auto *gn = env.addPlugin(Net::Gnutella, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(10 * 1000 * 1000, 407);
    const QByteArray fake = randomBytes(10 * 1000 * 1000, 408);
    SourceConfig refused, bad, good;
    refused.failOpen = true;
    refused.data = data;
    bad.data = fake;
    good.data = data;
    const SearchSource e = ed->addSource(QStringLiteral("ed2k-peer://7.0.0.1:4662"), refused, QStringLiteral("edpeer"));
    const SearchSource wrong = candidate(gn->addSource(QStringLiteral("gnut://7.0.0.2/f"), bad, QStringLiteral("liar")), hashesOf(fake, 1));
    env.engineUp();
    const QString id = env.engine->download(req(QStringLiteral("prov.bin"), data, hashesOf(data, 2), {e, wrong}));
    QTRY_VERIFY2_WITH_TIMEOUT(rowStatus(env.info(id), QStringLiteral("liar")).startsWith(QLatin1String("Banned (File Mismatch")),
                              qPrintable(dump(env.info(id))), 20000);
    QVERIFY(env.info(id).state != TransferState::Error);
    QVERIFY(env.info(id).state != TransferState::Completed);
    gn->emitSources(id, {wrong});                       // reported again: stays banned
    gn->emitSources(id, {candidate(gn->addSource(QStringLiteral("gnut://7.0.0.3/f"), good, QStringLiteral("honest")), hashesOf(data, 1))});
    QTRY_VERIFY2_WITH_TIMEOUT(env.info(id).state == TransferState::Completed, qPrintable(dump(env.info(id))), 20000);
    QCOMPARE(readFile(env.info(id).savePath), data);
    QCOMPARE(env.engine->debugInfo(id).value(QStringLiteral("wholeFileFailures")).toInt(), 1);
}

// No usable source and no search hit: the background size+name search asks the keyword networks
// (not eD2k/Kad: the trusted ed2k hash already finds every copy there), finds a Gnutella copy with
// another name spelling, and the file completes from it (verified against the ed2k hash).
void TstEngineSources::nameSearchFindsCandidates()
{
    SearchEnv env(QStringLiteral("namesearch"));
    env.tuning.provisionalGraceMs = 1500;
    env.tuning.nameSearchDelayMs = 300;
    auto *ed = env.add(Net::ED2K, NetworkPlugin::CanSearch | NetworkPlugin::CanFetchRanges | NetworkPlugin::CanFindSources);
    auto *gn = env.add(Net::Gnutella, NetworkPlugin::CanSearch | NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(10 * 1000 * 1000, 409);
    SourceConfig refused, good;
    refused.failOpen = true;
    refused.data = data;
    good.data = data;
    SearchHit gh = test::FakePlugin::makeHit(QStringLiteral("Big Buck Bunny 640x360 iPodTouch iPhone.m4v"), data.size(), hashesOf(data, 1),
                                             Net::Gnutella, QStringLiteral("gnutuser"));
    gh.sources.first() = gn->addSource(QStringLiteral("gnut://6.6.6.6/f"), good, QStringLiteral("gnutuser"));
    gn->cannedHits << gh;
    SearchHit other = test::FakePlugin::makeHit(QStringLiteral("Big Buck Bunny 640x360 iPodTouch iPhone.m4v"), data.size() + 1, FileHashes(),
                                                Net::Gnutella, QStringLiteral("x"));
    gn->cannedHits << other;                            // another size: no candidate
    env.up();
    const SearchSource e = ed->addSource(QStringLiteral("ed2k-peer://5.0.0.1:4662"), refused, QStringLiteral("edpeer"));
    const QString id = env.engine->download(req(QStringLiteral("Big_Buck_Bunny_640x360_IPodTouch_IPhone.m4v"), data, hashesOf(data, 2), {e}));
    QTRY_VERIFY_WITH_TIMEOUT(!gn->searches.isEmpty(), 5000);
    const SearchQuery q = gn->searches.first();
    qInfo("background query: \"%s\" ext %s size %lld", qPrintable(q.text), qPrintable(q.extension), q.minSize);
    QCOMPARE(q.text, QStringLiteral("big buck bunny 640x360 ipodtouch iphone"));
    QCOMPARE(q.extension, QStringLiteral("m4v"));
    QCOMPARE(q.minSize, qint64(data.size()));
    // [merge-1] a folder torrent holding the file is a little larger: the plain hits are filtered by size
    QCOMPARE(q.maxSize, qint64(data.size()) + data.size() / 2 + (64LL << 20));
    QVERIFY(ed->searches.isEmpty());                   // the trusted ed2k hash covers eD2k/Kad
    QVERIFY(env.search->history().isEmpty());          // a hidden search
    QTRY_VERIFY2_WITH_TIMEOUT(row(env.info(id), QStringLiteral("gnutuser")), qPrintable(dump(env.info(id))), 5000);
    QVERIFY(!row(env.info(id), QStringLiteral("x")));
    QTRY_VERIFY2_WITH_TIMEOUT(env.info(id).state == TransferState::Completed, qPrintable(dump(env.info(id))), 20000);
    QFile f(env.info(id).savePath);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), data);
}

void TstEngineSources::autoFindSourcesPerDownload()
{
    Env env(QStringLiteral("afs"));
    env.tuning.findSourcesBaseMs = 0;     // the AFS setting decides
    env.tuning.afsMinuteMs = 200;         // one AFS "minute" = 200 ms
    auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges | NetworkPlugin::CanFindSources);
    const QByteArray data = randomBytes(1000 * 1000, 410);
    env.engineUp();
    const QString id = env.engine->download(req(QStringLiteral("afs.bin"), data, hashesOf(data, 2), {}));
    env.engine->setAutoFindSources(id, 0);   // Never
    QCOMPARE(env.info(id).autoFindSourcesMin, 0);
    const int start = int(ed->findCalls.size());
    QVERIFY(start >= 1);                      // asked once when it started
    QTest::qWait(1500);
    QCOMPARE(int(ed->findCalls.size()), start);
    QVERIFY2(env.info(id).statusText.contains(QLatin1String("Auto Find Sources off")), qPrintable(env.info(id).statusText));
    env.engine->setAutoFindSources(id, 2);   // every 400 ms (checked on the 1 Hz engine tick)
    QTest::qWait(3300);
    const int asked = int(ed->findCalls.size()) - start;
    qInfo("AFS 2 \"minutes\" of 200 ms: asked %d times in 3.3 s", asked);
    QVERIFY(asked >= 2 && asked <= 5);
    // persisted per download
    env.engineDown(true);
    env.engineUp();
    QCOMPARE(env.info(id).autoFindSourcesMin, 2);
    env.settings.setValue(QString(keys::search::AutoFindSourcesMin), 7);
    env.engine->setAutoFindSources(id, -1);  // back to the global setting
    QCOMPARE(env.info(id).autoFindSourcesMin, 7);
    env.engineDown(true);
    env.engineUp();
    QCOMPARE(env.info(id).autoFindSourcesMin, 7);
}

void TstEngineSources::autoEnterQueuePerDownload()
{
    Env env(QStringLiteral("aeq"));
    auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(12 * 1000 * 1000, 411);
    SourceConfig fast, queued;
    fast.data = queued.data = data;
    fast.bytesPerSec = 100 * 1024;
    queued.queuedMs = 600000;                 // rank 7, forever
    const SearchSource f = ed->addSource(QStringLiteral("ed2k-peer://4.0.0.1:4662"), fast, QStringLiteral("fast"));
    const SearchSource q = ed->addSource(QStringLiteral("ed2k-peer://4.0.0.2:4662"), queued, QStringLiteral("queue"));
    env.engineUp();
    const QString id = env.engine->download(req(QStringLiteral("aeq.bin"), data, hashesOf(data, 2), {f, q}));
    QTRY_VERIFY_WITH_TIMEOUT(rowStatus(env.info(id), QStringLiteral("queue")) == QLatin1String("Remotely Queued 7"), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(rowStatus(env.info(id), QStringLiteral("fast")) == QLatin1String("Transferring"), 5000);
    // AEQ 5 < rank 7 while another source transfers: the queue is left
    env.engine->setAutoEnterQueue(id, 5);
    QCOMPARE(env.info(id).autoEnterQueue, 5);
    QTRY_VERIFY2_WITH_TIMEOUT(rowStatus(env.info(id), QStringLiteral("queue")).startsWith(QLatin1String("Remote queue too long (7 > 5), retry in")), qPrintable(dump(env.info(id))),
                              3000);
    // a looser limit takes it back
    env.engine->setAutoEnterQueue(id, 10);
    QTRY_VERIFY2_WITH_TIMEOUT(rowStatus(env.info(id), QStringLiteral("queue")) == QLatin1String("Remotely Queued 7"), qPrintable(dump(env.info(id))), 3000);
    // Never: no remote queue by itself ...
    env.engine->setAutoEnterQueue(id, 0);
    QTRY_VERIFY2_WITH_TIMEOUT(rowStatus(env.info(id), QStringLiteral("queue")).startsWith(QLatin1String("Not entering remote queues")),
                              qPrintable(dump(env.info(id))), 3000);
    // ... but "Enter Queue" does, and it stays
    env.engine->enterQueue(id);
    QTRY_VERIFY2_WITH_TIMEOUT(rowStatus(env.info(id), QStringLiteral("queue")) == QLatin1String("Remotely Queued 7"), qPrintable(dump(env.info(id))), 3000);
    QTest::qWait(800);
    QCOMPARE(rowStatus(env.info(id), QStringLiteral("queue")), QStringLiteral("Remotely Queued 7"));
    env.engineDown(true);
    env.engineUp();
    QCOMPARE(env.info(id).autoEnterQueue, 0);
}

// SearchHit's plain numbers for sortable columns
void TstEngineSources::hitNumbers()
{
    SearchHit h;
    SearchSource lime, knaben, ed, kad;
    lime.net = knaben.net = Net::BitTorrent;
    lime.seeds = 6; lime.peers = 4; lime.extra.insert(QStringLiteral("providerName"), QStringLiteral("LimeTorrents"));
    knaben.seeds = 9; knaben.peers = 2; knaben.extra.insert(QStringLiteral("providerName"), QStringLiteral("Knaben"));
    ed.net = Net::ED2K; ed.user.net = Net::ED2K; ed.user.id = QStringLiteral("abc"); ed.seeds = 1;
    kad.net = Net::Kad; kad.seeds = 3;                // a Kad hit: 3 sources reported, none named
    h.sources = {lime, knaben, ed, kad};
    SearchManager::fillHitNumbers(h);
    QCOMPARE(h.seeds, 9 + 1 + 3);                     // the swarm's best figure + eD2k/Kad complete sources
    QCOMPARE(h.leechers, 4);
    QCOMPARE(h.totalPeers, 9 + 4 + 1 + 3);
    QCOMPARE(h.sourceCount, 4);
    QCOMPARE(h.networks, (QList<Net>{Net::BitTorrent, Net::ED2K, Net::Kad}));
    QCOMPARE(h.indexes, (QStringList{QStringLiteral("LimeTorrents"), QStringLiteral("Knaben")}));
    // [core-5] an eD2k server row that names one client but reports 25 sources (12 complete), a
    // second server row reporting 7, and two Gnutella sources: Peers = 25 + 2, never below the
    // sources listed (3 eD2k rows here: 25 >= 3)
    SearchHit e;
    SearchSource s1, s2, s3, g1, g2;
    s1.net = s2.net = s3.net = Net::ED2K;
    s1.user.net = Net::ED2K; s1.user.id = s1.user.address = QStringLiteral("81.0.0.1:4662");
    s1.peers = 25; s1.seeds = 12;
    s2.peers = 7; s2.seeds = 2;
    g1.net = g2.net = Net::Gnutella;
    g1.user.net = g2.user.net = Net::Gnutella;
    g1.user.id = QStringLiteral("g1"); g2.user.id = QStringLiteral("g2");
    e.sources = {s1, s2, s3, g1, g2};
    SearchManager::fillHitNumbers(e);
    QCOMPARE(e.totalPeers, 25 + 2);
    QVERIFY(e.totalPeers >= e.sourceCount);
    // only listed rows without any figure: the listed count
    SearchHit plain;
    plain.sources = {s3, s3, s3};
    SearchManager::fillHitNumbers(plain);
    QCOMPARE(plain.totalPeers, 3);
    SearchHit empty;
    SearchManager::fillHitNumbers(empty);
    QCOMPARE(empty.totalPeers, -1);
    QCOMPARE(empty.seeds, -1);
}

// The class of the owner's "LimeTorrents" report: a hit an index alone knows. Its file list
// request is always answered (list or bundleFilesUnavailable), downloading it creates a visible
// download at once, and a hit nothing can be fetched from says why (downloadNotStarted).
void TstEngineSources::indexOnlyHitsAlwaysAnswered()
{
    SearchEnv env(QStringLiteral("indexonly"));
    auto *web = env.add(Net::Web, NetworkPlugin::CanSearch);
    SearchHit bundle = test::FakePlugin::makeHit(QStringLiteral("Some Free Album 2020"), 45686456, FileHashes(), Net::BitTorrent);
    bundle.hashes.btih = QByteArray::fromHex("dd8255ecdc7ca55fb0bbf81323d87062db1f6d1c");
    bundle.isBundle = true;
    bundle.sources.first().uri = QStringLiteral("magnet:?xt=urn:btih:dd8255ecdc7ca55fb0bbf81323d87062db1f6d1c&dn=Some%20Free%20Album%202020");
    bundle.sources.first().seeds = 6;
    bundle.sources.first().peers = 4;
    bundle.sources.first().extra.insert(QStringLiteral("providerName"), QStringLiteral("LimeTorrents"));
    SearchHit nothing = test::FakePlugin::makeHit(QStringLiteral("Page Only Entry 1999.zip"), 1234567, FileHashes(), Net::Web);
    nothing.sources.first().uri.clear();
    web->cannedHits << bundle << nothing;
    env.up();
    env.search->setBundleTimeout(500);
    SearchQuery q;
    q.text = QStringLiteral("free");
    const quint32 sid = env.search->startSearch(q);
    QTRY_COMPARE_WITH_TIMEOUT(env.search->hits(sid).size(), 2, 2000);
    SearchHit b, n;
    for (const SearchHit &h : env.search->hits(sid)) (h.isBundle ? b : n) = h;
    QCOMPARE(b.indexes, QStringList{QStringLiteral("LimeTorrents")});
    QCOMPARE(b.totalPeers, 10);
    QSignalSpy unavailable(env.search.get(), &SearchService::bundleFilesUnavailable);
    env.search->requestBundleFiles(sid, b.key);
    QTRY_COMPARE_WITH_TIMEOUT(unavailable.count(), 1, 3000);
    QCOMPARE(unavailable.first().at(1).toString(), b.key);
    qInfo("bundleFilesUnavailable: %s", qPrintable(unavailable.first().at(2).toString()));
    // the whole bundle: a visible download at once, saying what it waits for
    QSignalSpy added(env.engine.get(), &TransferService::transferAdded);
    const QString id = env.engine->downloadHit(b);
    QVERIFY(!id.isEmpty());
    QCOMPARE(added.count(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(env.info(id).statusText.startsWith(QLatin1String("Finding sources")), 3000);
    qInfo("index-only torrent: %s", qPrintable(env.info(id).statusText));
    // an entry with neither link nor hash: no download, but a reason
    QSignalSpy refused(env.engine.get(), &TransferService::downloadNotStarted);
    QVERIFY(env.engine->downloadHit(n).isEmpty());
    QCOMPARE(refused.count(), 1);
    QCOMPARE(refused.first().at(0).toString(), n.key);
    QVERIFY2(refused.first().at(1).toString().startsWith(QLatin1String("Nothing to download")), qPrintable(refused.first().at(1).toString()));
}

// A torrent identity alone cannot check a part file: size+name candidates are never used
// provisionally then (they wait for the torrent's file list / a hash that can decide).
void TstEngineSources::provisionalNeedsWholeFileHash()
{
    Env env(QStringLiteral("prov-btih"));
    env.tuning.provisionalGraceMs = 500;
    auto *gn = env.addPlugin(Net::Gnutella, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(10 * 1000 * 1000, 412);
    SourceConfig c;
    c.data = data;
    const SearchSource cand = candidate(gn->addSource(QStringLiteral("gnut://8.0.0.1/f"), c, QStringLiteral("cand")), hashesOf(data, 1));
    env.engineUp();   // no torrent backend: a part file with a torrent identity only
    FileHashes h;
    h.btih = QByteArray::fromHex("a9d4f3a1409fc3c7eaf4703edd55677b60842770");
    const QString id = env.engine->download(req(QStringLiteral("btih-only.mp4"), data, h, {cand}));
    QTest::qWait(2500);
    QVERIFY(gn->stats.isEmpty());   // never opened
    QVERIFY2(env.info(id).statusText.contains(QLatin1String("wait for the torrent's file list")), qPrintable(env.info(id).statusText));
    QCOMPARE(rowStatus(env.info(id), QStringLiteral("cand")), QStringLiteral("Not used yet: unverified (waiting for the torrent's file list to check it)"));
}

// [core-5] A Gnutella servent names a wrong TTH for the file (its hit and a THEX tree that fits the
// lie). The hashes a Gnutella source teaches are provisional: the tree is kept aside instead of
// judging every honest unit as bad (which ended in "File Mismatch" for the whole download), the
// whole-file check against the trusted SHA-1 decides, the liar is banned and the download completes.
void TstEngineSources::gnutellaLyingTreeHashBanned()
{
    Env env(QStringLiteral("gnut-liar"));
    auto *gn = env.addPlugin(Net::Gnutella, NetworkPlugin::CanFetchRanges | NetworkPlugin::CanFindSources);
    const QByteArray data = randomBytes(3 * 1000 * 1000, 409);
    SourceConfig honest, liar;
    honest.data = data;
    honest.bytesPerSec = 1 << 20;
    liar.data = data;
    liar.bytesPerSec = 1 << 20;
    const qint64 unit = 65536;
    const QList<QByteArray> lieLeaves = fakeTthLeaves(data.size(), unit, 77);
    const QByteArray lieRoot = hash::TigerTree::rootFromLevel(lieLeaves);
    FileHashes lieClaim = hashesOf(data, 1);
    lieClaim.tth = lieRoot;
    SearchSource h = gn->addSource(QStringLiteral("gnut://6.0.0.1/f"), honest, QStringLiteral("honest"));
    SearchSource l = gn->addSource(QStringLiteral("gnut://6.0.0.2/f"), liar, QStringLiteral("liar"));
    l.extra.insert(QStringLiteral("hashes"), lieClaim.toVariant());   // what its query hit said
    env.engineUp();
    const QString id = env.engine->download(req(QStringLiteral("Sintel.2010.720p.mkv"), data, hashesOf(data, 1), {h, l}));
    QVERIFY(!id.isEmpty());
    // the plugin passes on what the liar told it (hit TTH, then its THEX tree)
    FileHashes more;
    more.tth = lieRoot;
    gn->emitHashes(id, more);
    gn->emitHashSet(id, QStringLiteral("tth-leaves"), unit, lieLeaves);
    QCOMPARE(env.engine->debugInfo(id).value(QStringLiteral("hinted")).toMap().value(QStringLiteral("tth")).toByteArray(), lieRoot.toHex());
    QTRY_VERIFY2_WITH_TIMEOUT(env.info(id).state == TransferState::Completed, qPrintable(dump(env.info(id))), 30000);
    const TransferInfo t = env.info(id);
    QCOMPARE(readFile(t.savePath), data);
    hash::TigerTree real;
    real.update(data);
    QCOMPARE(t.hashes.tth, real.finish());                       // the real TTH, not the lie
    const QVariantMap dbg = env.engine->debugInfo(id);
    QCOMPARE(dbg.value(QStringLiteral("unitFailures")).toInt(), 0);
    QCOMPARE(dbg.value(QStringLiteral("wholeFileFailures")).toInt(), 0);
    QVERIFY(dbg.value(QStringLiteral("hinted")).toMap().isEmpty());
    bool liarBanned = false, honestBanned = true;
    for (const QVariant &v : dbg.value(QStringLiteral("sources")).toList()) {
        const QVariantMap m = v.toMap();
        if (m.value(QStringLiteral("key")).toString().endsWith(QLatin1String("|liar"))) liarBanned = m.value(QStringLiteral("banned")).toBool();
        if (m.value(QStringLiteral("key")).toString().endsWith(QLatin1String("|honest"))) honestBanned = m.value(QStringLiteral("banned")).toBool();
    }
    QVERIFY(liarBanned);
    QVERIFY(!honestBanned);
}

QTEST_GUILESS_MAIN(TstEngineSources)
#include "tst_engine_sources.moc"
