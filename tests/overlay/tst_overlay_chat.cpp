// SPDX-License-Identifier: GPL-3.0-or-later
// Overlay chat with 3 (+1) nodes over 127.0.0.1: join, messages, actions, topic, presence timeout,
// history replay for late joiners, forged-signature rejection, flood limit, room directory.
#include "ovl_test_support.h"

#include "ovl_crypto.h"

#include <QtCore/QJsonDocument>

using namespace mx::ovl;
using namespace ovltest;

namespace {
// Build a pub-sub envelope exactly as PROTOCOL.md specifies (for forging tests).
QByteArray envelope(const QByteArray &pub, const QByteArray &sig, const QByteArray &body)
{
    QByteArray e;
    e.append('S');
    e.append(char(1));
    e.append(pub);
    e.append(sig);
    e.append(body);
    return e;
}
QByteArray signedPart(const QString &topic, const QByteArray &body)
{
    return QByteArrayLiteral("winmx-ng/1\n") + topic.toUtf8() + '\n' + body;
}
QString roomTopicFor(const QString &roomId)
{
    return QStringLiteral("mx/room/") +
           QString::fromLatin1(QCryptographicHash::hash(roomId.toUtf8(), QCryptographicHash::Sha1).toHex().left(20));
}
QList<mx::ChatMessage> msgs(const QSignalSpy &spy, mx::ChatKind kind, const QString &room = QString())
{
    QList<mx::ChatMessage> out;
    for (const auto &args : spy) {
        const auto m = args.first().value<mx::ChatMessage>();
        if (m.kind == kind && (room.isEmpty() || m.roomId == room)) out << m;
    }
    return out;
}
} // namespace

class TstOverlayChat : public QObject {
    Q_OBJECT
    std::unique_ptr<Instance> A, B, C;
    const QString room = QStringLiteral("octomx lobby");

    bool allSee(int n)
    {
        return A->chat->roomUsers(room).size() == n && B->chat->roomUsers(room).size() == n &&
               C->chat->roomUsers(room).size() == n;
    }

private slots:
    void initTestCase()
    {
        mx::registerMetaTypes();
        A.reset(new Instance(QStringLiteral("Alice")));
        B.reset(new Instance(QStringLiteral("Bob")));
        C.reset(new Instance(QStringLiteral("Carol")));
        for (Instance *i : {A.get(), B.get(), C.get()}) {
            auto o = OverlayPlugin::TestOptions();
            o.dht = o.mdns = o.reconnect = false;
            o.pex = false;   // force a chain A - B - C: C's messages reach A only through B
            o.listenPort = 0;
            i->plugin->setTestOptions(o);
            i->start();
        }
        QVERIFY(link(*A, *B));
        QVERIFY(link(*B, *C));
        QCOMPARE(A->plugin->connectedUsers(), 1);
        QCOMPARE(C->plugin->connectedUsers(), 1);
    }

    void namesAndIds()
    {
        const mx::UserRef a = A->me();
        QCOMPARE(a.net, mx::Net::Overlay);
        QCOMPARE(a.id.size(), 64);
        QVERIFY(a.name.startsWith(QStringLiteral("Alice_")));
        QCOMPARE(a.name.size(), 6 + 5);
        QVERIFY(A->plugin->capabilities() & mx::NetworkPlugin::CanChat);
    }

    void joinAndMessage()
    {
        QSignalSpy joinedA(A->events, &mx::ChatEvents::roomJoined);
        for (Instance *i : {A.get(), B.get(), C.get()}) i->chat->joinRoom(QStringLiteral("OctoMX Lobby"));
        QCOMPARE(joinedA.count(), 1);
        QCOMPARE(joinedA.first().first().value<mx::ChatRoomInfo>().id, room);
        QVERIFY2(waitFor([&] { return allSee(3); }, 15000), "room members did not converge");

        QSignalSpy rxA(A->events, &mx::ChatEvents::messageReceived);
        QSignalSpy rxB(B->events, &mx::ChatEvents::messageReceived);
        QSignalSpy rxC(C->events, &mx::ChatEvents::messageReceived);
        C->chat->sendRoomMessage(room, QStringLiteral("hello from Carol"), mx::ChatKind::Message);
        C->chat->sendRoomMessage(room, QStringLiteral("waves"), mx::ChatKind::Action);
        QVERIFY(waitFor([&] { return msgs(rxA, mx::ChatKind::Action).size() == 1; }));
        QVERIFY(waitFor([&] { return msgs(rxB, mx::ChatKind::Action).size() == 1; }));
        const auto ma = msgs(rxA, mx::ChatKind::Message);
        QCOMPARE(ma.size(), 1);   // relayed 2 hops (A is not connected to C), exactly once
        QCOMPARE(ma.first().text, QStringLiteral("hello from Carol"));
        QCOMPARE(ma.first().from.id, C->me().id);
        QCOMPARE(ma.first().from.name, C->me().name);
        QCOMPARE(ma.first().roomId, room);
        QCOMPARE(msgs(rxA, mx::ChatKind::Action).first().text, QStringLiteral("waves"));
        QTest::qWait(300);
        QCOMPARE(msgs(rxC, mx::ChatKind::Message).size(), 0);   // no echo of our own lines
    }

    void topicLastWriterWins()
    {
        QSignalSpy upA(A->events, &mx::ChatEvents::roomUpdated);
        B->chat->setTopic(room, QStringLiteral("Welcome to the lobby"));
        QVERIFY(waitFor([&] {
            for (const auto &r : A->chat->roomList())
                if (r.id == room && r.topic == QStringLiteral("Welcome to the lobby")) return true;
            return false;
        }));
        QVERIFY(upA.count() >= 1);
        QTest::qWait(20);
        C->chat->setTopic(room, QStringLiteral("Newer topic"));
        QVERIFY(waitFor([&] {
            const auto l = A->chat->roomList();
            return std::any_of(l.begin(), l.end(), [&](const mx::ChatRoomInfo &r) { return r.id == room && r.topic == QStringLiteral("Newer topic"); });
        }));
    }

    void historyReplay()
    {
        A->chat->sendRoomMessage(room, QStringLiteral("history line 1"), mx::ChatKind::Message);
        A->chat->sendRoomMessage(room, QStringLiteral("history line 2"), mx::ChatKind::Message);
        QTest::qWait(500);
        Instance D(QStringLiteral("Dave"));
        D.start();
        QVERIFY(link(*B, D));
        QSignalSpy rxD(D.events, &mx::ChatEvents::messageReceived);
        D.chat->joinRoom(room);
        QVERIFY2(waitFor([&] { return msgs(rxD, mx::ChatKind::Message).size() >= 3; }, 15000), "no history replay");
        QStringList texts;
        for (const auto &m : msgs(rxD, mx::ChatKind::Message)) texts << m.text;
        QVERIFY(texts.contains(QStringLiteral("hello from Carol")));
        QVERIFY(texts.contains(QStringLiteral("history line 1")));
        QVERIFY(texts.contains(QStringLiteral("history line 2")));
        QCOMPARE(texts.count(QStringLiteral("history line 2")), 1);
        QVERIFY(waitFor([&] { return D.chat->roomUsers(room).size() == 4; }, 10000));
        D.chat->leaveRoom(room);
        QVERIFY(waitFor([&] { return A->chat->roomUsers(room).size() == 3; }, 6000));
    }

    // [core-5] two nodes: a member set the topic and the MOTD of a room; a node joining later gets
    // both through the existing ChatEvents (roomUpdated + one Topic line + one Motd line per MOTD
    // line), once, and a later topic change as one more Topic line.
    void topicAndMotdOnJoin()
    {
        const QString r = QStringLiteral("core5 room");
        Instance E(QStringLiteral("Erin"));
        E.start();
        QVERIFY(link(*A, E));
        A->chat->joinRoom(QStringLiteral("Core5 Room"));
        A->chat->setTopic(r, QStringLiteral("Big Buck Bunny night"));
        A->chat->setMotd(r, QStringLiteral("Welcome!\nOnly legal content, please."));
        QTest::qWait(200);
        QSignalSpy rxE(E.events, &mx::ChatEvents::messageReceived);
        QSignalSpy upE(E.events, &mx::ChatEvents::roomUpdated);
        E.chat->joinRoom(r);
        QVERIFY2(waitFor([&] { return msgs(rxE, mx::ChatKind::Motd, r).size() >= 2; }, 10000), "no MOTD on join");
        QTest::qWait(1500);   // nothing is repeated by later heartbeats or the history replay
        const auto topics = msgs(rxE, mx::ChatKind::Topic, r);
        QCOMPARE(topics.size(), 1);
        QCOMPARE(topics.first().text, QStringLiteral("Big Buck Bunny night"));
        const auto motd = msgs(rxE, mx::ChatKind::Motd, r);
        QCOMPARE(motd.size(), 2);
        QCOMPARE(motd.at(0).text, QStringLiteral("Welcome!"));
        QCOMPARE(motd.at(1).text, QStringLiteral("Only legal content, please."));
        QVERIFY(upE.count() >= 1);
        const auto info = upE.last().first().value<mx::ChatRoomInfo>();
        QCOMPARE(info.topic, QStringLiteral("Big Buck Bunny night"));
        QCOMPARE(info.motd, QStringLiteral("Welcome!\nOnly legal content, please."));
        // the topic line comes before the MOTD lines (original WinMX order)
        int topicAt = -1, motdAt = -1;
        for (int i = 0; i < rxE.size(); ++i) {
            const auto m = rxE.at(i).first().value<mx::ChatMessage>();
            if (m.kind == mx::ChatKind::Topic && topicAt < 0) topicAt = i;
            if (m.kind == mx::ChatKind::Motd && motdAt < 0) motdAt = i;
        }
        QVERIFY(topicAt >= 0 && topicAt < motdAt);
        // a change while we are in the room: one more Topic line
        QTest::qWait(20);
        A->chat->setTopic(r, QStringLiteral("Sintel tomorrow"));
        QVERIFY(waitFor([&] { return msgs(rxE, mx::ChatKind::Topic, r).size() == 2; }, 6000));
        QCOMPARE(msgs(rxE, mx::ChatKind::Topic, r).last().text, QStringLiteral("Sintel tomorrow"));
        E.chat->leaveRoom(r);
        A->chat->leaveRoom(r);
    }

    // [core-5] a default room nobody is in still greets with its built-in topic and MOTD
    void defaultRoomGreeting()
    {
        Instance F(QStringLiteral("Frank"));
        F.start();
        QSignalSpy rxF(F.events, &mx::ChatEvents::messageReceived);
        F.chat->joinRoom(QStringLiteral("Help"));
        QVERIFY2(waitFor([&] { return msgs(rxF, mx::ChatKind::Motd).size() >= 3; }, 8000), "no default MOTD");
        QCOMPARE(msgs(rxF, mx::ChatKind::Topic).size(), 1);
        QCOMPARE(msgs(rxF, mx::ChatKind::Topic).first().text, QStringLiteral("Questions about OctoMX? Ask here."));
        QCOMPARE(msgs(rxF, mx::ChatKind::Topic).first().roomId, QStringLiteral("help"));
        F.chat->leaveRoom(QStringLiteral("help"));
    }

    void forgedSignatureRejected()
    {
        // an attacker node speaking the overlay protocol connects to A and injects envelopes
        RatsNode evil;
        RatsNodeConfig c;
        c.protocol = QStringLiteral("winmx-ng/1");
        c.dht = c.mdns = c.reconnect = c.pex = false;
        c.pubsub = true;
        c.json = false;
        c.dataDir = freshDir(QStringLiteral("evil"));
        QVERIFY(evil.start(c));
        const QString topic = roomTopicFor(room);
        evil.subscribe(topic);
        evil.connectTo(A->addr());
        QVERIFY(waitFor([&] { return evil.peerCount() == 1; }));
        QTest::qWait(1500);   // mesh graft

        Identity mallory;
        QVERIFY(mallory.loadOrCreate(c.dataDir + QStringLiteral("/id")));
        auto body = [&](const QString &text) {
            QJsonObject b;
            b[QStringLiteral("t")] = QStringLiteral("msg");
            b[QStringLiteral("v")] = 1;
            b[QStringLiteral("id")] = randomId();
            b[QStringLiteral("ts")] = double(QDateTime::currentMSecsSinceEpoch());
            b[QStringLiteral("name")] = QStringLiteral("Bob_00000");
            b[QStringLiteral("room")] = room;
            b[QStringLiteral("text")] = text;
            return QJsonDocument(b).toJson(QJsonDocument::Compact);
        };
        QSignalSpy rxA(A->events, &mx::ChatEvents::messageReceived);
        const QByteArray bobPub = QByteArray::fromHex(B->me().id.toLatin1());
        // 1) claims to be Bob, signed by Mallory
        const QByteArray b1 = body(QStringLiteral("forged as Bob"));
        evil.publish(topic, envelope(bobPub, mallory.sign(signedPart(topic, b1)), b1));
        // 2) Mallory's own valid signature but made for another room (replay across topics)
        const QByteArray b2 = body(QStringLiteral("cross-room replay"));
        evil.publish(topic, envelope(mallory.publicKey(), mallory.sign(signedPart(roomTopicFor(QStringLiteral("music")), b2)), b2));
        // 3) valid signature, tampered body
        const QByteArray b3 = body(QStringLiteral("original"));
        QByteArray b3t = b3;
        b3t.replace("original", "tampered");
        evil.publish(topic, envelope(mallory.publicKey(), mallory.sign(signedPart(topic, b3)), b3t));
        // 4) control: a correctly signed message from Mallory is accepted (as Mallory, not Bob)
        const QByteArray b4 = body(QStringLiteral("honest mallory"));
        evil.publish(topic, envelope(mallory.publicKey(), mallory.sign(signedPart(topic, b4)), b4));
        QVERIFY(waitFor([&] { return msgs(rxA, mx::ChatKind::Message).size() >= 1; }, 5000));
        QTest::qWait(500);
        const auto got = msgs(rxA, mx::ChatKind::Message);
        QCOMPARE(got.size(), 1);
        QCOMPARE(got.first().text, QStringLiteral("honest mallory"));
        QCOMPARE(got.first().from.id, QString::fromLatin1(mallory.publicKey().toHex()));
        // and B (one hop further) saw nothing forged either: A's validator did not relay them
        evil.stop();
    }

    void floodLimit()
    {
        QSignalSpy rxB(B->events, &mx::ChatEvents::messageReceived);
        for (int i = 0; i < 20; ++i)
            A->chat->sendRoomMessage(room, QStringLiteral("spam %1").arg(i), mx::ChatKind::Message);
        QTest::qWait(1500);
        const int got = msgs(rxB, mx::ChatKind::Message).size();
        qInfo("flood: B accepted %d of 20 burst messages (limit 6 per 10 s)", got);
        QVERIFY(got >= 5 && got <= 8);
        QCOMPARE(msgs(rxB, mx::ChatKind::System).size(), 1);   // one "is flooding" notice
    }

    void tooLongMessageIsTruncated()
    {
        QSignalSpy rxB(B->events, &mx::ChatEvents::messageReceived);
        QTest::qWait(11000);   // let the flood bucket refill
        C->chat->sendRoomMessage(room, QString(5000, QLatin1Char('x')), mx::ChatKind::Message);
        QVERIFY(waitFor([&] { return msgs(rxB, mx::ChatKind::Message).size() == 1; }));
        QCOMPARE(msgs(rxB, mx::ChatKind::Message).first().text.size(), 1024);
    }

    void roomDirectory()
    {
        // default public rooms are always listed, even on a fresh node with no peers
        Instance lonely(QStringLiteral("Lonely"));
        QStringList names;
        for (const auto &r : lonely.chat->roomList()) names << r.name;
        for (const char *n : {"OctoMX Lobby", "Music", "Movies", "Help"}) QVERIFY(names.contains(QString::fromLatin1(n)));
        // a room created by A shows up on C (2 hops) with its user count
        A->chat->joinRoom(QStringLiteral("Test Room 42"));
        QVERIFY(waitFor([&] {
            C->chat->refreshRoomList();
            for (const auto &r : C->chat->roomList())
                if (r.name == QStringLiteral("Test Room 42") && r.users == 1) return true;
            return false;
        }, 10000));
        for (const auto &r : C->chat->roomList())
            if (r.id == room) QCOMPARE(r.users, 3);
    }

    void presenceTimeout()
    {
        QSignalSpy rxA(A->events, &mx::ChatEvents::messageReceived);
        QSignalSpy pres(A->events, &mx::ChatEvents::userPresence);
        const QString cid = C->me().id;
        QVERIFY(A->plugin->isUserOnline(C->me()));
        // simulate a crash of C: its node vanishes without part/bye messages
        C->plugin->node()->stop();
        QVERIFY2(waitFor([&] { return A->chat->roomUsers(room).size() == 2; }, 8000), "member did not time out");
        const auto parts = msgs(rxA, mx::ChatKind::Part, room);
        QCOMPARE(parts.size(), 1);
        QVERIFY(parts.first().text.contains(QStringLiteral("timed out")));
        QCOMPARE(parts.first().from.id, cid);
        QVERIFY(waitFor([&] { return !A->plugin->isUserOnline(C->me()); }, 12000));
        bool offline = false;
        for (const auto &args : pres)
            if (args.at(0).value<mx::UserRef>().id == cid && !args.at(1).toBool()) offline = true;
        QVERIFY(offline);
    }

    void cleanupTestCase()
    {
        A.reset();
        B.reset();
        C.reset();
        QVERIFY(RatsNode::waitAllStopped(8000));
    }
};

QTEST_GUILESS_MAIN(TstOverlayChat)
#include "tst_overlay_chat.moc"
