// SPDX-License-Identifier: GPL-3.0-or-later
// [net-3] The OctoMX network and Rats settings, each changed and observed between real nodes on
// 127.0.0.1 (explicit dials, no DHT, no LAN discovery): whether the network runs at all, how many
// links others may open to us, relaying for firewalled users, hash links, the Rats listen port,
// safe search, the result cap, the bootstrap peers and the peer cap. The Rats side is answered by a
// small fake rats-search peer (a librats node speaking the rats-search JSON messages).
#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonArray>
#include <QtCore/QRandomGenerator>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QUdpSocket>
#include <QtTest/QSignalSpy>
#include <QtTest/QtTest>

#include "core/network_manager.h"
#include "ovl_test_support.h"
#include "rats_search_plugin.h"

using namespace mx::ovl;
using namespace ovltest;

namespace {

OverlayTimings fastTimings()
{
    OverlayTimings t;
    t.presenceMs = 3000;
    t.presenceTimeoutMs = 8000;
    t.presenceGraceMs = 2000;
    t.pingWaitMs = 2000;
    t.heartbeatMs = 1000;
    t.memberTimeoutMs = 3500;
    t.directoryMs = 1000;
    t.historyDelayMs = 1200;
    t.searchQuietMs = 2500;
    t.searchMaxMs = 8000;
    t.sourcesRepeatMs = 3000;
    t.dialWaitMs = 6000;
    return t;
}

// an OctoMX network node; firewalled: refuses direct inbound links and announces no address, so
// only connect-back and relayed circuits reach it
std::unique_ptr<Instance> node(const QString &name, bool firewalled = false, bool relay = true)
{
    auto i = std::make_unique<Instance>(name);
    OverlayPlugin::TestOptions to;
    to.dht = to.mdns = to.reconnect = to.pex = false;
    to.listenPort = 0;
    to.firewalled = firewalled;
    to.advertise = !firewalled;
    i->plugin->setTestOptions(to);
    i->plugin->setTimings(fastTimings());
    i->settings.setValue(QString(mx::keys::overlay::Relay), relay);
    return i;
}

// a port free for TCP and UDP on 127.0.0.1 (librats listens on both). [api-2] Taken from 20000 to
// 32767, below the ephemeral ranges of Linux (32768 and up), Windows and macOS (49152 and up): a port
// the system hands out (listen on 0) can go to an outgoing connection of another test running at the
// same time before the node here binds it seconds later, and the node then takes another port (seen
// in full ctest runs with -j3: "listenPort == ratsPort" and "fake.peers.size() == 1" failed).
quint16 freePort()
{
    for (int i = 0; i < 200; ++i) {
        const quint16 p = quint16(20000 + QRandomGenerator::global()->bounded(12768));
        QTcpServer t;
        if (!t.listen(QHostAddress::LocalHost, p)) continue;
        QUdpSocket u;
        if (u.bind(QHostAddress::AnyIPv4, p)) return p;
    }
    return 0;
}

QString hex40(int i) { return QString::fromLatin1(QCryptographicHash::hash(QByteArray::number(i), QCryptographicHash::Sha1).toHex()); }

// a rats-search peer as the real ones: answers searchTorrent with one searchTorrent_response per hit
struct FakeRats {
    RatsNode node;
    QString dir = freshDir(QStringLiteral("fake-rats"));
    QList<QJsonObject> queries;   // searchTorrent requests received
    QList<QJsonObject> answers;   // what it answers to each of them
    QSet<QString> peers;
    bool start(quint16 port = 0)
    {
        RatsNodeConfig c;
        c.protocol = QString::fromLatin1(kDefaultRatsProtocol);
        c.listenPort = port;
        c.dht = c.mdns = c.pex = c.reconnect = false;
        c.holePunch = false;
        c.json = true;
        c.jsonTypes = {QStringLiteral("searchTorrent"), QStringLiteral("searchFiles"), QStringLiteral("client_info")};
        c.dataDir = dir;
        QObject::connect(&node, &RatsNode::jsonReceived, &node, [this](const QString &from, const QString &type, const QJsonObject &o) {
            if (type != QLatin1String("searchTorrent")) return;
            queries << o;
            for (const QJsonObject &a : std::as_const(answers)) node.sendJson(from, QStringLiteral("searchTorrent_response"), a);
        });
        QObject::connect(&node, &RatsNode::peerConnected, &node, [this](const RatsPeer &p) { peers.insert(p.id); });
        QObject::connect(&node, &RatsNode::peerDisconnected, &node, [this](const QString &id, const QString &) { peers.remove(id); });
        return node.start(c);
    }
    ~FakeRats()
    {
        node.stop();
        node.waitStopped(3000);
        QDir(dir).removeRecursively();
    }
    QString addr() const { return QStringLiteral("127.0.0.1:%1").arg(node.listenPort()); }
};

QJsonObject torrent(int i, const QString &name, const QString &category = QString())
{
    QJsonObject o;
    o[QStringLiteral("hash")] = hex40(i);
    o[QStringLiteral("name")] = name;
    o[QStringLiteral("size")] = 276134947.0 + i;
    o[QStringLiteral("files")] = 1;
    o[QStringLiteral("contentType")] = QStringLiteral("video");
    if (!category.isEmpty()) o[QStringLiteral("contentCategory")] = category;
    o[QStringLiteral("seeders")] = 10;
    o[QStringLiteral("leechers")] = 2;
    return o;
}

// the Rats plugin as OctoMX runs it, with its own profile and settings
struct RatsClient {
    QString dir = freshDir(QStringLiteral("rats-client"));
    mx::SimpleSettings settings;
    RatsSearchPlugin *plugin = nullptr;
    RatsClient()
    {
        settings.setValue(QStringLiteral("rats/listenPort"), 0);   // a free one (4444 may be busy on a shared host)
        plugin = static_cast<RatsSearchPlugin *>(mx::createRatsSearchPlugin());
    }
    void start()
    {
        mx::PluginContext ctx;
        ctx.settings = &settings;
        ctx.profileDir = dir;
        ctx.dataDir = dir + QStringLiteral("/rats");
        ctx.clientName = QStringLiteral("OctoMX");
        ctx.clientVersion = QStringLiteral("5.4-test");
        plugin->init(ctx);
        plugin->start();
    }
    ~RatsClient()
    {
        plugin->stop();
        delete plugin;
        RatsNode::waitAllStopped(5000);
        QDir(dir).removeRecursively();
    }
    int peers() const { return plugin->node() ? plugin->node()->peerCount() : 0; }
};

// hits of one search, by file name
QStringList hitNames(const QSignalSpy &spy, quint32 id)
{
    QStringList out;
    for (const auto &a : spy)
        if (a.at(0).toUInt() == id)
            for (const mx::SearchHit &h : a.at(1).value<QList<mx::SearchHit>>()) out << h.fileName;
    out.sort();
    return out;
}

mx::SearchQuery bunnyQuery(quint32 id)
{
    mx::SearchQuery q;
    q.id = id;
    q.text = QStringLiteral("big buck bunny");
    q.maxResults = 2000;
    return q;
}

mx::FileHashes ed2kOnly(const mx::FileHashes &all)
{
    mx::FileHashes h;
    h.ed2k = all.ed2k;
    return h;
}

} // namespace

class TstNetSettingsOvl : public QObject {
    Q_OBJECT
private slots:
    void initTestCase()
    {
        mx::registerMetaTypes();
        // local only, also when run by hand: no DHT, no LAN discovery, no peer exchange
        qputenv("MX_OVERLAY_DISCOVERY", "0");
        qunsetenv("MX_RATS_DHT_ROUTERS");
        qunsetenv("MX_OVERLAY_FIREWALLED");
    }
    void cleanup() { RatsNode::waitAllStopped(5000); }

    void enabledSwitchesStartAndStop();
    void overlayMaxPeersCapsInboundLinks();
    void overlayRelayServesCircuits();
    void overlayShareHashLinksAnswersAndGossip();
    void ratsListenPortAndBootstrapPeers();
    void ratsSafeSearchAndMaxResults();
    void ratsMaxPeersCapsInboundLinks();
    void ratsDhtPortIsTheDhtSource();
};

// overlay/enabled and rats/enabled: the core's NetworkManager starts and stops the real plugins as
// the switches say. Off: nothing listens on the network's port, a peer that dials it gets nowhere;
// on: it runs and the peer links; off again at run time: the link goes.
void TstNetSettingsOvl::enabledSwitchesStartAndStop()
{
    const quint16 ovlPort = freePort(), ratsPort = freePort();
    QVERIFY(ovlPort && ratsPort && ovlPort != ratsPort);
    const QString profile = freshDir(QStringLiteral("netmgr"));
    mx::SimpleSettings settings;
    settings.setValue(QString(mx::keys::overlay::Enabled), false);
    settings.setValue(QString(mx::keys::rats::Enabled), false);
    settings.setValue(QStringLiteral("rats/listenPort"), int(ratsPort));
    auto *ov = static_cast<OverlayPlugin *>(mx::createOverlayPlugin());
    OverlayPlugin::TestOptions to;
    to.dht = to.mdns = to.reconnect = to.pex = false;
    to.listenPort = ovlPort;
    ov->setTestOptions(to);
    ov->setTimings(fastTimings());
    auto *rats = static_cast<RatsSearchPlugin *>(mx::createRatsSearchPlugin());
    {
        mx::NetworkManager net(&settings, {ov, rats});
        net.initPlugins(profile, nullptr, nullptr);
        net.startNetworks(true);
        auto peer = node(QStringLiteral("Visitor"));
        peer->start();
        FakeRats fake;
        QVERIFY(fake.start());
        // both off: nothing runs, nothing answers on their ports
        QVERIFY(!net.isRunning(mx::Net::Overlay));
        QVERIFY(!net.isRunning(mx::Net::Rats));
        QCOMPARE(ov->listenPort(), quint16(0));
        QVERIFY(!rats->node() || !rats->node()->isRunning());
        peer->plugin->connectPeer(QStringLiteral("127.0.0.1:%1").arg(ovlPort));
        fake.node.connectTo(QStringLiteral("127.0.0.1:%1").arg(ratsPort));
        QTest::qWait(2500);
        QCOMPARE(peer->plugin->connectedUsers(), 0);
        QVERIFY(fake.peers.isEmpty());
        // on: each runs on its port and the peers reach it
        settings.setValue(QString(mx::keys::overlay::Enabled), true);
        settings.setValue(QString(mx::keys::rats::Enabled), true);
        QVERIFY(waitFor([&] { return ov->listenPort() == ovlPort; }, 5000));
        QVERIFY(waitFor([&] { return rats->status().listenPort == ratsPort; }, 5000));
        QVERIFY(net.isRunning(mx::Net::Overlay) && net.isRunning(mx::Net::Rats));
        peer->plugin->connectPeer(QStringLiteral("127.0.0.1:%1").arg(ovlPort));
        QVERIFY(waitFor([&] { return peer->plugin->connectedUsers() == 1 && ov->connectedUsers() == 1; }, 10000));
        QVERIFY(fake.node.connectTo(QStringLiteral("127.0.0.1:%1").arg(ratsPort)));
        QVERIFY(waitFor([&] { return fake.peers.size() == 1 && rats->node()->peerCount() == 1; }, 10000));
        // off at run time: the network stops, its links go
        settings.setValue(QString(mx::keys::overlay::Enabled), false);
        settings.setValue(QString(mx::keys::rats::Enabled), false);
        QVERIFY(waitFor([&] { return peer->plugin->connectedUsers() == 0; }, 10000));
        QVERIFY(waitFor([&] { return fake.peers.isEmpty(); }, 10000));
        QVERIFY(!net.isRunning(mx::Net::Overlay) && !net.isRunning(mx::Net::Rats));
        QCOMPARE(net.status(mx::Net::Overlay).state, mx::ConnState::Disabled);
        QCOMPARE(net.status(mx::Net::Rats).state, mx::ConnState::Disabled);
    }   // the manager deletes both plugins
    RatsNode::waitAllStopped(5000);
    QDir(profile).removeRecursively();
}

// overlay/maxPeers: the links other users open to this computer, at most that many (the schema's
// lowest value, 4, against six dialers); the default 50 takes all six
void TstNetSettingsOvl::overlayMaxPeersCapsInboundLinks()
{
    auto capped = node(QStringLiteral("Capped"));
    capped->settings.setValue(QString(mx::keys::overlay::MaxPeers), 4);
    auto open = node(QStringLiteral("Open"));   // overlay/maxPeers not set: 50
    capped->start();
    open->start();
    std::vector<std::unique_ptr<Instance>> dialers;
    for (int i = 0; i < 6; ++i) {
        dialers.push_back(node(QStringLiteral("Dialer%1").arg(i)));
        dialers.back()->start();
    }
    for (auto &d : dialers) {
        d->plugin->connectPeer(capped->addr());
        d->plugin->connectPeer(open->addr());
        QTest::qWait(150);
    }
    QVERIFY(waitFor([&] { return open->plugin->node()->peerCount() == 6; }, 15000));
    QTest::qWait(2000);   // late refusals settle
    const int cappedPeers = capped->plugin->node()->peerCount();
    int reachedCapped = 0;
    for (auto &d : dialers) reachedCapped += d->plugin->node()->isConnected(capped->plugin->nodeId()) ? 1 : 0;
    qInfo("overlay/maxPeers 4: %d links in (%d dialers linked); default: %d", cappedPeers, reachedCapped,
          open->plugin->node()->peerCount());
    QCOMPARE(cappedPeers, 4);
    QCOMPARE(reachedCapped, 4);
    QVERIFY(waitFor([&] { return capped->plugin->connectedUsers() == 4 && open->plugin->connectedUsers() == 6; }, 5000));
}

// overlay/relay: two firewalled users that both reach a third one can browse each other only when
// the third one relays (a relayed circuit); with "Act as a relay" off the browse fails, and it says why
void TstNetSettingsOvl::overlayRelayServesCircuits()
{
    for (const bool relay : {true, false}) {
        auto A = node(QStringLiteral("Alice"), true), B = node(QStringLiteral("Bob"), true);
        auto C = node(QStringLiteral("Carol"), false, relay);   // overlay/relay of the middle node
        A->start();
        B->start();
        C->start();
        QVERIFY(link(*C, *A));
        QVERIFY(link(*C, *B));
        QVERIFY(waitFor([&] { return A->plugin->isUserOnline(B->me()); }, 15000));
        B->library.addGenerated(B->profile, QStringLiteral("Big Buck Bunny.avi"), 64 * 1024, 7);
        B->setShared();
        QSignalSpy res(A->plugin, &mx::NetworkPlugin::browseResults);
        QSignalSpy fail(A->plugin, &mx::NetworkPlugin::browseFailed);
        A->plugin->browse(relay ? 51 : 52, B->me());
        QVERIFY2(waitFor([&] { return (!res.isEmpty() && res.last().at(3).toBool()) || !fail.isEmpty(); }, fastTimings().dialWaitMs + 10000),
                 "no answer");
        if (relay) {
            QVERIFY2(fail.isEmpty(), fail.isEmpty() ? "" : qPrintable(fail.first().at(2).toString()));
            QCOMPARE(res.last().at(2).value<QList<mx::BrowseEntry>>().size(), 1);
            QCOMPARE(A->plugin->node()->peerTransport(B->plugin->nodeId()), QStringLiteral("relay"));
        } else {
            QVERIFY(res.isEmpty());
            const QString why = fail.first().at(2).toString();
            qInfo("no relay: %s", qPrintable(why));
            QVERIFY(why.contains(QStringLiteral("firewalled")));
            QVERIFY(A->plugin->node()->peerTransport(B->plugin->nodeId()) != QLatin1String("relay"));
        }
    }
}

// overlay/shareHashLinks: on, a node answers a lookup with the other hashes of a file it shares and
// keeps the sets others publish; off, it neither answers nor keeps them, and publishes none itself
void TstNetSettingsOvl::overlayShareHashLinksAnswersAndGossip()
{
    auto A = node(QStringLiteral("Asker")), B = node(QStringLiteral("Holder"));
    A->start();
    B->start();
    QVERIFY(link(*B, *A));
    QTest::qWait(1500);   // the gossip topics meet their subscribers
    const mx::SharedFile f = B->library.addGenerated(B->profile, QStringLiteral("Big Buck Bunny.mp4"), 96 * 1024, 11);
    B->setShared();
    auto lookup = [&](const mx::FileHashes &known, qint64 size, int waitMs) {
        QSignalSpy hl(A->plugin, &mx::NetworkPlugin::hashLinksFound);
        A->plugin->lookupHashLinks(known, size);
        (void)waitFor([&] { return hl.count() >= 1; }, waitMs);
        return hl.isEmpty() ? mx::FileHashes() : hl.first().at(1).value<mx::FileHashes>();
    };
    // the asker keeps no gossip of its own (off), so every answer below comes from the holder; an
    // answer it got is remembered, so each "no answer" is checked before the "answer"
    A->settings.setValue(QString(mx::keys::overlay::ShareHashLinks), false);
    // the holder's own file: not answered while off, answered while on
    B->settings.setValue(QString(mx::keys::overlay::ShareHashLinks), false);
    QVERIFY(lookup(ed2kOnly(f.hashes), f.size, 4000).sha1.isEmpty());
    B->settings.setValue(QString(mx::keys::overlay::ShareHashLinks), true);
    QCOMPARE(lookup(ed2kOnly(f.hashes), f.size, 8000).sha1, f.hashes.sha1);
    // a set another user publishes and then goes offline (it keeps its own copy, so only what the
    // holder kept can answer): kept and given on only while both share hash links
    mx::FileHashes pub;
    pub.btih = QByteArray::fromHex("dd8255ecdc7ca55fb0bbf81323d87062db1f6d1c");
    pub.btFileIndex = 0;
    pub.sha1 = QByteArray(20, 'S');
    pub.ed2k = QByteArray(16, 'E');
    const qint64 size = 276134947;
    auto publish = [&](const QString &name, bool publisherOn) {
        auto P = node(name);
        P->start();
        QVERIFY(link(*B, *P));
        QTest::qWait(1500);
        P->settings.setValue(QString(mx::keys::overlay::ShareHashLinks), publisherOn);
        P->plugin->publishHashLinks(pub, size, QStringLiteral("Big Buck Bunny.mp4"));
        QTest::qWait(1500);
        P->plugin->stop();
    };
    B->settings.setValue(QString(mx::keys::overlay::ShareHashLinks), false);
    publish(QStringLiteral("Publisher1"), true);   // the holder (off) does not keep it
    B->settings.setValue(QString(mx::keys::overlay::ShareHashLinks), true);
    QVERIFY(lookup(ed2kOnly(pub), size, 4000).btih.isEmpty());
    publish(QStringLiteral("Publisher2"), false);   // the publisher (off) sends none
    QVERIFY(lookup(ed2kOnly(pub), size, 4000).btih.isEmpty());
    publish(QStringLiteral("Publisher3"), true);    // both on: kept by the holder, answered
    QCOMPARE(lookup(ed2kOnly(pub), size, 8000).btih, pub.btih);
}

// rats/listenPort: the Rats node listens on the port set, a peer that dials it links; a busy port
// is replaced by a free one. rats/bootstrapPeers: the peers listed are dialled at start, none
// without the list
void TstNetSettingsOvl::ratsListenPortAndBootstrapPeers()
{
    FakeRats fake;
    QVERIFY(fake.start());
    const quint16 port = freePort();
    QVERIFY(port);
    {
        RatsClient c;   // no bootstrap peers: nobody is dialled
        c.settings.setValue(QStringLiteral("rats/listenPort"), int(port));
        c.start();
        QCOMPARE(c.plugin->status().listenPort, int(port));
        QCOMPARE(c.plugin->node()->listenPort(), port);
        QTest::qWait(2000);
        QVERIFY(fake.peers.isEmpty());
        QVERIFY(fake.node.connectTo(QStringLiteral("127.0.0.1:%1").arg(port)));   // others reach it there
        QVERIFY(waitFor([&] { return fake.peers.size() == 1 && c.peers() == 1; }, 10000));
        // the same port taken by someone else: the next node takes a free one
        RatsClient busy;
        busy.settings.setValue(QStringLiteral("rats/listenPort"), int(port));
        busy.start();
        QVERIFY(busy.plugin->status().listenPort != 0);
        QVERIFY(busy.plugin->status().listenPort != int(port));
    }
    QVERIFY(waitFor([&] { return fake.peers.isEmpty(); }, 10000));
    {
        RatsClient c;
        c.settings.setValue(QStringLiteral("rats/bootstrapPeers"), QStringList{fake.addr(), QStringLiteral("not an address")});
        c.start();
        QVERIFY(waitFor([&] { return fake.peers.size() == 1 && c.peers() == 1; }, 10000));
    }
}

// rats/safeSearch: the query asks the peers for safe results (safeSearch in the request) and the
// adult results a peer sends anyway are left out; off, both kinds come through.
// rats/maxResults: at most that many results of one search are taken
void TstNetSettingsOvl::ratsSafeSearchAndMaxResults()
{
    FakeRats fake;
    for (int i = 0; i < 30; ++i) fake.answers << torrent(i, QStringLiteral("Big Buck Bunny 1080p part %1").arg(i, 2, 10, QLatin1Char('0')));
    fake.answers << torrent(100, QStringLiteral("Big Buck Bunny extra cut"), QStringLiteral("xxx"));
    QVERIFY(fake.start());
    RatsClient c;
    c.settings.setValue(QStringLiteral("rats/bootstrapPeers"), QStringList{fake.addr()});
    c.start();
    QVERIFY(waitFor([&] { return c.peers() == 1; }, 10000));
    QTest::qWait(500);   // the plugin has seen the link
    QSignalSpy hits(c.plugin, &mx::NetworkPlugin::searchHits);
    QSignalSpy done(c.plugin, &mx::NetworkPlugin::searchDone);
    auto run = [&](quint32 id) {
        const int before = int(fake.queries.size());
        c.plugin->search(bunnyQuery(id));
        QVERIFY(waitFor([&] { return int(fake.queries.size()) > before; }, 5000));
        QTest::qWait(2500);   // every answer handled and flushed
    };
    // safe search on (the default): asked for, and the adult result is dropped
    c.settings.setValue(QStringLiteral("rats/safeSearch"), true);
    run(1);
    QCOMPARE(fake.queries.last().value(QStringLiteral("safeSearch")).toBool(), true);
    QStringList names = hitNames(hits, 1);
    QCOMPARE(names.size(), 30);
    QVERIFY(!names.contains(QStringLiteral("Big Buck Bunny extra cut")));
    // off: the request says so and every result is kept
    c.settings.setValue(QStringLiteral("rats/safeSearch"), false);
    run(2);
    QCOMPARE(fake.queries.last().value(QStringLiteral("safeSearch")).toBool(), false);
    names = hitNames(hits, 2);
    QCOMPARE(names.size(), 31);
    QVERIFY(names.contains(QStringLiteral("Big Buck Bunny extra cut")));
    // rats/maxResults 10 (the schema's lowest): ten results, then the search is done
    c.settings.setValue(QStringLiteral("rats/maxResults"), 10);
    run(3);
    QCOMPARE(hitNames(hits, 3).size(), 10);
    bool finished = false;
    for (const auto &a : done) finished |= a.at(0).toUInt() == 3;
    QVERIFY(finished);
    // a cap above what the peer has: all of them
    c.settings.setValue(QStringLiteral("rats/maxResults"), 500);
    run(4);
    QCOMPARE(hitNames(hits, 4).size(), 31);
}

// rats/maxPeers: rats-search peers that dial us, at most that many; the default 30 takes all four
void TstNetSettingsOvl::ratsMaxPeersCapsInboundLinks()
{
    RatsClient capped, open;
    capped.settings.setValue(QStringLiteral("rats/maxPeers"), 2);
    capped.start();
    open.start();
    std::vector<std::unique_ptr<FakeRats>> fakes;
    for (int i = 0; i < 4; ++i) {
        fakes.push_back(std::make_unique<FakeRats>());
        QVERIFY(fakes.back()->start());
    }
    for (auto &f : fakes) {
        QVERIFY(f->node.connectTo(QStringLiteral("127.0.0.1:%1").arg(capped.plugin->status().listenPort)));
        QVERIFY(f->node.connectTo(QStringLiteral("127.0.0.1:%1").arg(open.plugin->status().listenPort)));
        QTest::qWait(150);
    }
    QVERIFY(waitFor([&] { return open.peers() == 4; }, 10000));
    QTest::qWait(2000);
    int reached = 0;
    for (auto &f : fakes) reached += f->node.isConnected(capped.plugin->node()->nodeId()) ? 1 : 0;
    qInfo("rats/maxPeers 2: %d links in (%d dialers linked); default: %d", capped.peers(), reached, open.peers());
    QCOMPARE(capped.peers(), 2);
    QCOMPARE(reached, 2);
    QCOMPARE(open.peers(), 4);
}

// [net-3] rats/dhtPort: the DHT talks from that UDP port. The DHT runs here with only a local router
// (MX_RATS_DHT_ROUTERS, no public STUN servers): its datagrams arrive there from the port set.
void TstNetSettingsOvl::ratsDhtPortIsTheDhtSource()
{
    QUdpSocket router;
    QVERIFY(router.bind(QHostAddress::LocalHost, 0));
    QList<quint16> sources;
    connect(&router, &QUdpSocket::readyRead, this, [&] {
        while (router.hasPendingDatagrams()) {
            QByteArray d(int(qMax<qint64>(0, router.pendingDatagramSize())), Qt::Uninitialized);
            QHostAddress from;
            quint16 fromPort = 0;
            router.readDatagram(d.data(), d.size(), &from, &fromPort);
            sources << fromPort;
        }
    });
    qputenv("MX_RATS_DHT_ROUTERS", QStringLiteral("127.0.0.1:%1").arg(router.localPort()).toLatin1());
    QUdpSocket probe;   // a free UDP port for the setting
    QVERIFY(probe.bind(QHostAddress::LocalHost, 0));
    const quint16 dht = probe.localPort();
    probe.close();
    {
        RatsClient c;
        c.settings.setValue(QStringLiteral("rats/dhtPort"), int(dht));
        c.start();
        const bool got = waitFor([&] { return !sources.isEmpty(); }, 15000);
        qunsetenv("MX_RATS_DHT_ROUTERS");
        QVERIFY2(got, "no DHT datagram reached the local router");
        for (quint16 p : std::as_const(sources)) QCOMPARE(p, dht);
        QCOMPARE(c.plugin->node()->dhtPort(), dht);
    }
    qunsetenv("MX_RATS_DHT_ROUTERS");
}

QTEST_GUILESS_MAIN(TstNetSettingsOvl)
#include "tst_netsettings_ovl.moc"
