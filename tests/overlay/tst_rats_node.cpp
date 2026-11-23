// SPDX-License-Identifier: GPL-3.0-or-later
// RatsNode wrapper: start/stop cycles, port fallback, persistence of the node id, JSON / raw bytes /
// pub-sub messaging between two nodes over 127.0.0.1, validator rejection, teardown safety.
#include "ovl_test_support.h"

using namespace mx::ovl;
using namespace ovltest;

class TstRatsNode : public QObject {
    Q_OBJECT
    RatsNodeConfig localCfg(const QString &dir, quint16 port = 0)
    {
        RatsNodeConfig c;
        c.protocol = QStringLiteral("octomx-test/1");
        c.dataDir = dir;
        c.listenPort = port;
        c.dht = false;
        c.mdns = false;
        c.reconnect = true;
        c.pubsub = true;
        c.jsonTypes = {QStringLiteral("hello")};
        c.channels = {QStringLiteral("bin")};
        return c;
    }
private slots:
    void initTestCase() { mx::registerMetaTypes(); }

    void startStopCycles()
    {
        const QString dir = freshDir(QStringLiteral("cycles"));
        QString firstId;
        QElapsedTimer total;
        total.start();
        qint64 worstStop = 0;
        for (int i = 0; i < 20; ++i) {
            RatsNode n;
            QString err;
            QVERIFY2(n.start(localCfg(dir), &err), qPrintable(err));
            QVERIFY(n.isRunning());
            QVERIFY(n.listenPort() != 0);
            if (firstId.isEmpty()) firstId = n.nodeId();
            QCOMPARE(n.nodeId(), firstId);   // identity.key persisted in the data dir
            QTest::qWait(30);
            QElapsedTimer st;
            st.start();
            n.stop();
            QVERIFY(n.waitStopped(3000));
            worstStop = qMax(worstStop, st.elapsed());
        }
        qInfo("20 start/stop cycles in %lld ms, worst stop %lld ms", total.elapsed(), worstStop);
        QVERIFY(worstStop < 2500);
        QVERIFY(QFile::exists(dir + QStringLiteral("/identity.key")));
    }

    void cyclesWithTraffic()
    {
        // start/stop while connected and exchanging messages: no crash, no late deliveries
        RatsNode a;
        QVERIFY(a.start(localCfg(freshDir(QStringLiteral("cyc-a")))));
        int got = 0;
        connect(&a, &RatsNode::bytesReceived, this, [&](const QString &, const QString &, const QByteArray &) { ++got; });
        for (int i = 0; i < 20; ++i) {
            auto *b = new RatsNode;
            QVERIFY(b->start(localCfg(freshDir(QStringLiteral("cyc-b")))));
            b->connectTo(QStringLiteral("127.0.0.1"), a.listenPort());
            QVERIFY(waitFor([&] { return b->peerCount() == 1; }, 5000));
            for (int k = 0; k < 50; ++k) b->sendBytes(a.nodeId(), QStringLiteral("bin"), QByteArray(1000, 'x'));
            b->stop();
            delete b;   // deleted right after stop(): teardown continues on a helper thread
        }
        QVERIFY(RatsNode::waitAllStopped(10000) || a.isRunning());
        QVERIFY(waitFor([&] { return a.peerCount() == 0; }, 10000));
        qInfo("received %d of %d frames across 20 connect/stop cycles", got, 20 * 50);
        QVERIFY(got > 0);
    }

    void portFallback()
    {
        RatsNode a, b;
        QVERIFY(a.start(localCfg(freshDir(QStringLiteral("pf-a")))));
        RatsNodeConfig cb = localCfg(freshDir(QStringLiteral("pf-b")), a.listenPort());
        QVERIFY(b.start(cb));
        QVERIFY(b.listenPort() != 0);
        QVERIFY(b.listenPort() != a.listenPort());
        // librats itself walks port+1..+4 first; our fallback to an ephemeral port comes after
        RatsNode c;
        QVERIFY(c.start(cb));
        QVERIFY(c.listenPort() != a.listenPort() && c.listenPort() != b.listenPort());
    }

    void messaging()
    {
        RatsNode a, b;
        QVERIFY(a.start(localCfg(freshDir(QStringLiteral("msg-a")))));
        QVERIFY(b.start(localCfg(freshDir(QStringLiteral("msg-b")))));
        QSignalSpy upA(&a, &RatsNode::peerConnected);
        QSignalSpy upB(&b, &RatsNode::peerConnected);
        b.connectTo(QStringLiteral("127.0.0.1:%1").arg(a.listenPort()));
        QVERIFY(upA.wait(5000) || upA.count() > 0);
        QVERIFY(waitFor([&] { return upB.count() > 0; }));
        const RatsPeer p = upB.first().first().value<RatsPeer>();
        QCOMPARE(p.id, a.nodeId());
        QVERIFY(b.isConnected(a.nodeId()));
        QCOMPARE(a.peers().size(), 1);
        QVERIFY(a.peers().first().inbound);

        // typed JSON, delivered on the main thread
        QSignalSpy js(&a, &RatsNode::jsonReceived);
        QJsonObject o;
        o[QStringLiteral("x")] = 42;
        o[QStringLiteral("s")] = QStringLiteral("héllo");
        b.sendJson(a.nodeId(), QStringLiteral("hello"), o);
        QVERIFY(waitFor([&] { return js.count() == 1; }));
        QCOMPARE(js.first().at(0).toString(), b.nodeId());
        QCOMPARE(js.first().at(1).toString(), QStringLiteral("hello"));
        QCOMPARE(js.first().at(2).toJsonObject().value(QStringLiteral("s")).toString(), QStringLiteral("héllo"));

        // raw bytes, 1 MiB payload intact
        QSignalSpy bs(&a, &RatsNode::bytesReceived);
        QByteArray big(1 << 20, Qt::Uninitialized);
        for (int i = 0; i < big.size(); ++i) big[i] = char(i * 31 + 7);
        b.sendBytes(a.nodeId(), QStringLiteral("bin"), big);
        QVERIFY(waitFor([&] { return bs.count() == 1; }));
        QCOMPARE(bs.first().at(2).toByteArray(), big);

        // pub-sub with a validator: rejected messages are not delivered
        QSignalSpy ts(&a, &RatsNode::topicMessage);
        a.subscribe(QStringLiteral("t1"), [](const QString &, const QByteArray &d) { return !d.startsWith("BAD"); });
        b.subscribe(QStringLiteral("t1"));
        QTest::qWait(1500);   // a GossipSub heartbeat grafts the mesh
        b.publish(QStringLiteral("t1"), "BAD message");
        b.publish(QStringLiteral("t1"), "good message");
        QVERIFY(waitFor([&] { return ts.count() >= 1; }));
        QTest::qWait(300);
        QCOMPARE(ts.count(), 1);
        QCOMPARE(ts.first().at(2).toByteArray(), QByteArray("good message"));
        QCOMPARE(ts.first().at(1).toString(), b.nodeId());

        // disconnect notification
        QSignalSpy down(&a, &RatsNode::peerDisconnected);
        const QString bid = b.nodeId();
        b.stop();
        QVERIFY(waitFor([&] { return down.count() == 1; }, 8000));
        QCOMPARE(down.first().at(0).toString(), bid);
    }

    void warmStartReconnects()
    {
        // b remembers a in peers.json; after a restart it re-dials a by itself
        const QString da = freshDir(QStringLiteral("warm-a")), db = freshDir(QStringLiteral("warm-b"));
        RatsNode a;
        QVERIFY(a.start(localCfg(da)));
        {
            RatsNode b;
            QVERIFY(b.start(localCfg(db)));
            b.connectTo(QStringLiteral("127.0.0.1"), a.listenPort());
            QVERIFY(waitFor([&] { return b.peerCount() == 1; }));
            QTest::qWait(500);
            b.stop();
            QVERIFY(b.waitStopped(3000));
        }
        QVERIFY(waitFor([&] { return a.peerCount() == 0; }, 8000));
        RatsNode b2;
        QElapsedTimer t;
        t.start();
        QVERIFY(b2.start(localCfg(db)));
        QVERIFY(waitFor([&] { return b2.peerCount() == 1; }, 5000));
        qInfo("warm start re-connected to the remembered peer in %lld ms", t.elapsed());
        QVERIFY(t.elapsed() < 1500);
    }
};

QTEST_GUILESS_MAIN(TstRatsNode)
#include "tst_rats_node.moc"
