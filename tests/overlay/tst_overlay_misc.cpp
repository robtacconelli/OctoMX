// SPDX-License-Identifier: GPL-3.0-or-later
// Overlay plugin: persistence across restart (identity, known peers, hash-link LRU), 20× start/stop,
// status texts when alone / connected, servers() + static bootstrap peers, two plugins on the same
// configured port, hostile input (garbage frames, bad hello, flood) without crashing.
#include "ovl_test_support.h"

using namespace mx::ovl;
using namespace ovltest;

namespace {
OverlayPlugin::TestOptions opts(bool reconnect = false, int port = 0)
{
    OverlayPlugin::TestOptions o;
    o.dht = o.mdns = o.pex = false;
    o.reconnect = reconnect;
    o.listenPort = port;
    return o;
}
} // namespace

class TstOverlayMisc : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { mx::registerMetaTypes(); }

    void persistenceAcrossRestart()
    {
        Instance B(QStringLiteral("Bob"));
        B.plugin->setTestOptions(opts(true));
        B.start();
        const QString profileA = freshDir(QStringLiteral("persistA"));
        QString idA;
        mx::FileHashes linked;
        linked.sha1 = QByteArray(20, 'q');
        linked.tth = QByteArray(24, 't');
        {
            Instance A(QStringLiteral("Alice"), profileA);
            A.plugin->setTestOptions(opts(true));
            A.start();
            idA = A.me().id;
            QVERIFY(link(B, A));   // A dials B → B lands in A's peers.json
            A.plugin->publishHashLinks(linked, 4242, QStringLiteral("linked.bin"));
            QTest::qWait(300);
            A.plugin->stop();
        }
        QVERIFY(RatsNode::waitAllStopped(5000));
        QVERIFY(QFile::exists(profileA + QStringLiteral("/overlay/user_ed25519.key")));
        QVERIFY(QFile::exists(profileA + QStringLiteral("/overlay/peers.json")));
        QVERIFY(QFile::exists(profileA + QStringLiteral("/overlay/hashlinks.json")));
        QVERIFY(waitFor([&] { return B.plugin->connectedUsers() == 0; }, 8000));
        Instance A2(QStringLiteral("Alice"), profileA);
        A2.plugin->setTestOptions(opts(true));
        QElapsedTimer t;
        t.start();
        A2.start();
        QCOMPARE(A2.me().id, idA);                       // same identity
        QCOMPARE(A2.me().name, A2.me().name);
        QVERIFY(waitFor([&] { return A2.plugin->connectedUsers() == 1; }, 5000));   // warm reconnect
        qInfo("restarted node re-connected to its known peer in %lld ms", t.elapsed());
        QVERIFY(t.elapsed() < 2000);
        // the hash-link LRU came back from disk: answered locally
        QSignalSpy hl(A2.plugin, &mx::NetworkPlugin::hashLinksFound);
        mx::FileHashes known;
        known.sha1 = linked.sha1;
        A2.plugin->lookupHashLinks(known, 4242);
        QVERIFY(waitFor([&] { return hl.count() >= 1; }, 3000));
        QCOMPARE(hl.first().at(1).value<mx::FileHashes>().tth, linked.tth);
    }

    void startStopCycles()
    {
        Instance A(QStringLiteral("Cycler"));
        Instance peer(QStringLiteral("Peer"));
        peer.plugin->setTestOptions(opts());
        peer.start();
        A.plugin->setTestOptions(opts());
        A.chat->joinRoom(QStringLiteral("Help"));
        QElapsedTimer t;
        t.start();
        for (int i = 0; i < 20; ++i) {
            A.plugin->start();
            QCOMPARE(A.plugin->status().state, mx::ConnState::Connecting);
            if (i % 4 == 0) {
                QVERIFY(waitFor([&] { return peer.plugin->connectedUsers() == 0; }, 8000));
                QVERIFY(link(peer, A));
            }
            QTest::qWait(20);
            A.plugin->stop();
            QCOMPARE(A.plugin->status().state, mx::ConnState::Disconnected);
        }
        QVERIFY(RatsNode::waitAllStopped(8000));
        qInfo("20 plugin start/stop cycles in %lld ms", t.elapsed());
        // still fully functional afterwards
        QVERIFY(waitFor([&] { return peer.plugin->connectedUsers() == 0; }, 8000));
        A.plugin->start();
        QVERIFY(link(peer, A));
        QSignalSpy rx(peer.events, &mx::ChatEvents::messageReceived);
        peer.chat->joinRoom(QStringLiteral("Help"));
        QVERIFY(waitFor([&] { return peer.chat->roomUsers(QStringLiteral("help")).size() == 2; }, 10000));
        A.chat->sendRoomMessage(QStringLiteral("help"), QStringLiteral("still alive"), mx::ChatKind::Message);
        QVERIFY(waitFor([&] { return rx.count() >= 1; }, 5000));
    }

    void statusAloneAndConnected()
    {
        Instance A(QStringLiteral("Solo"));
        A.plugin->setTestOptions(opts());
        mx::ovl::OverlayTimings t;
        t.aloneAfterMs = 1500;
        A.plugin->setTimings(t);
        QSignalSpy st(A.plugin, &mx::NetworkPlugin::statusChanged);
        A.start();
        QCOMPARE(A.plugin->status().statusText, QStringLiteral("Connecting..."));
        QVERIFY(waitFor([&] { return A.plugin->status().state == mx::ConnState::Connected; }, 4000));
        QCOMPARE(A.plugin->status().statusText, QStringLiteral("Connected (no other OctoMX users found yet)"));
        QCOMPARE(A.plugin->status().peers, 0);
        QVERIFY(A.plugin->status().listenPort > 0);
        const int emitted = st.count();
        QTest::qWait(1500);
        QCOMPARE(st.count(), emitted);   // alone: no busy status churn
        Instance B(QStringLiteral("Duo"));
        B.plugin->setTestOptions(opts());
        B.start();
        QVERIFY(link(A, B));
        QVERIFY(waitFor([&] { return A.plugin->status().peers == 1; }, 5000));
        QVERIFY(waitFor([&] { return A.plugin->status().statusText.startsWith(QStringLiteral("Connected: 1 peer")); }, 8000));
        qInfo("status: %s", qPrintable(A.plugin->status().statusText));
        const auto servers = A.plugin->servers();
        QCOMPARE(servers.size(), 1);
        QCOMPARE(servers.first().name, B.me().name);
        QCOMPARE(servers.first().state, mx::ConnState::Connected);
        QCOMPARE(servers.first().net, mx::Net::Overlay);
    }

    void bootstrapPeersFromSettings()
    {
        Instance A(QStringLiteral("Seed"));
        A.plugin->setTestOptions(opts());
        A.start();
        Instance C(QStringLiteral("Newbie"));
        C.plugin->setTestOptions(opts());
        C.settings.setValue(QString(mx::keys::overlay::BootstrapPeers), QStringList{A.addr()});
        bool listed = false;
        for (const auto &s : C.plugin->servers()) listed |= s.isStatic && s.address == A.addr();
        QVERIFY(listed);
        C.start();
        QVERIFY(waitFor([&] { return C.plugin->connectedUsers() == 1; }, 5000));
        // addServer persists to the setting
        C.plugin->addServer(QStringLiteral("127.0.0.1:9"), QString());
        QVERIFY(C.settings.sl(QString(mx::keys::overlay::BootstrapPeers)).contains(QStringLiteral("127.0.0.1:9")));
        C.plugin->removeServer(QStringLiteral("127.0.0.1:9"));
        QVERIFY(!C.settings.sl(QString(mx::keys::overlay::BootstrapPeers)).contains(QStringLiteral("127.0.0.1:9")));
    }

    void samePortTwice()
    {
        Instance A(QStringLiteral("PortA"));
        Instance B(QStringLiteral("PortB"));
        A.plugin->setTestOptions(opts(false, 0));
        A.start();
        B.plugin->setTestOptions(opts(false, A.plugin->listenPort()));
        B.start();
        QVERIFY(B.plugin->listenPort() != A.plugin->listenPort());
        QVERIFY(link(A, B));
    }

    void hostileInput()
    {
        Instance A(QStringLiteral("Target"));
        A.plugin->setTestOptions(opts());
        A.start();
        Instance friendB(QStringLiteral("Friend"));
        friendB.plugin->setTestOptions(opts());
        friendB.start();
        QVERIFY(link(A, friendB));

        // 1) garbage before/without hello, then a hello with a bad signature → disconnected
        {
            FakePeer evil;
            QVERIFY(evil.start(freshDir(QStringLiteral("evil-a"))));
            evil.node.connectTo(A.addr());
            QVERIFY(waitFor([&] { return evil.node.peerCount() == 1; }));
            QRandomGenerator rng(1);
            for (int i = 0; i < 200; ++i) {
                QByteArray junk(int(rng.bounded(1, 3000)), Qt::Uninitialized);
                rng.fillRange(reinterpret_cast<quint32 *>(junk.data()), junk.size() / 4);
                if (i % 3 == 0) junk[0] = 'J';
                if (i % 3 == 1) junk[0] = 'Z';
                evil.sendRaw(A.plugin->nodeId(), i % 2 ? QStringLiteral("mx") : QStringLiteral("mxd"), junk);
            }
            QJsonObject h;
            h[QStringLiteral("t")] = QStringLiteral("hello");
            h[QStringLiteral("user")] = friendB.me().id;   // impersonation attempt
            h[QStringLiteral("node")] = evil.node.nodeId();
            h[QStringLiteral("x")] = QString(64, QLatin1Char('0'));
            h[QStringLiteral("name")] = QStringLiteral("Friend_12345");
            h[QStringLiteral("ts")] = 1.0;
            h[QStringLiteral("sig")] = QString(128, QLatin1Char('1'));
            evil.send(A.plugin->nodeId(), h);
            QVERIFY(waitFor([&] { return evil.node.peerCount() == 0; }, 5000));
            QCOMPARE(A.plugin->userRefFor(friendB.me().id).name, friendB.me().name);   // mapping untouched
        }
        // 2) a valid hello, then wrong-typed fields, oversized + unknown messages, data frames
        //    for unknown sessions, and a control flood → eventually disconnected, A keeps working
        {
            FakePeer evil;
            QVERIFY(evil.start(freshDir(QStringLiteral("evil-b"))));
            evil.node.connectTo(A.addr());
            QVERIFY(waitFor([&] { return evil.node.peerCount() == 1; }));
            evil.hello(A.plugin->nodeId());
            QVERIFY(waitFor([&] { return A.plugin->connectedUsers() == 2; }));
            const QString to = A.plugin->nodeId();
            const char *types[] = {"pm", "browse_req", "whois_req", "hist_req", "hits", "src", "hl", "get_open",
                                   "get", "get_close", "slot", "got", "browse", "whois", "future_type"};
            for (const char *t : types) {
                QJsonObject o;
                o[QStringLiteral("t")] = QString::fromLatin1(t);
                o[QStringLiteral("v")] = 1;
                o[QStringLiteral("rid")] = 12;                      // wrong type
                o[QStringLiteral("sid")] = QStringLiteral("x");     // wrong type
                o[QStringLiteral("off")] = -5;
                o[QStringLiteral("len")] = 1e300;
                o[QStringLiteral("items")] = QJsonArray{1, QStringLiteral("a"), QJsonObject{}};
                o[QStringLiteral("text")] = QString(100000, QLatin1Char('x'));
                o[QStringLiteral("id")] = QStringLiteral("abc");
                evil.send(to, o);
            }
            evil.sendRaw(to, QStringLiteral("mxd"), QByteArray("D\x01") + QByteArray(16, '\0') + QByteArray(100, 'x'));
            QSignalSpy rx(A.events, &mx::ChatEvents::messageReceived);
            QJsonObject pm;
            pm[QStringLiteral("t")] = QStringLiteral("pm");
            pm[QStringLiteral("v")] = 1;
            pm[QStringLiteral("id")] = QStringLiteral("0123456789abcdef");
            pm[QStringLiteral("text")] = QStringLiteral("hello target");
            evil.send(to, pm);
            evil.send(to, pm);   // duplicate id: delivered once
            auto hellos = [&] {
                int n = 0;
                for (const auto &a : rx) n += a.first().value<mx::ChatMessage>().text == QStringLiteral("hello target");
                return n;
            };
            QVERIFY(waitFor([&] { return hellos() >= 1; }, 3000));
            QTest::qWait(200);
            QCOMPARE(hellos(), 1);
            // control flood (rate limit 50/s, burst 200, disconnect after 2000 drops)
            for (int i = 0; i < 4000; ++i) evil.send(to, QJsonObject{{QStringLiteral("t"), QStringLiteral("ping")}});
            QVERIFY2(waitFor([&] { return evil.node.peerCount() == 0; }, 10000), "flooding peer not disconnected");
        }
        // still healthy
        QSignalSpy rxB(friendB.events, &mx::ChatEvents::messageReceived);
        A.chat->sendPrivateMessage(friendB.me(), QStringLiteral("survived"));
        QVERIFY(waitFor([&] { return rxB.count() == 1; }, 5000));
    }

    void cleanupTestCase() { QVERIFY(RatsNode::waitAllStopped(8000)); }
};

QTEST_GUILESS_MAIN(TstOverlayMisc)
#include "tst_overlay_misc.moc"
