// SPDX-License-Identifier: GPL-3.0-or-later
// Overlay users: private messages (direct, connect-on-demand, relayed end-to-end encrypted),
// PM privacy, browse with paging + privacy refusal, whois, presence lookup.
#include "ovl_test_support.h"

using namespace mx::ovl;
using namespace ovltest;

namespace {
QList<mx::ChatMessage> msgs(const QSignalSpy &spy, mx::ChatKind kind)
{
    QList<mx::ChatMessage> out;
    for (const auto &args : spy) {
        const auto m = args.first().value<mx::ChatMessage>();
        if (m.kind == kind) out << m;
    }
    return out;
}
OverlayPlugin::TestOptions opts(bool advertise)
{
    OverlayPlugin::TestOptions o;
    o.dht = o.mdns = o.reconnect = o.pex = false;
    o.listenPort = 0;
    o.advertise = advertise;
    return o;
}
} // namespace

class TstOverlayUsers : public QObject {
    Q_OBJECT
    std::unique_ptr<Instance> A, B, C;

private slots:
    void initTestCase()
    {
        mx::registerMetaTypes();
        A.reset(new Instance(QStringLiteral("Alice")));
        B.reset(new Instance(QStringLiteral("Bob")));
        C.reset(new Instance(QStringLiteral("Carol")));
        A->plugin->setTestOptions(opts(true));
        B->plugin->setTestOptions(opts(true));
        C->plugin->setTestOptions(opts(false));   // Carol announces no address: only relaying reaches her
        A->start();
        B->start();
        C->start();
        QVERIFY(link(*A, *B));
        QVERIFY(link(*B, *C));
        // presence travels A <-> B <-> C through the mesh
        QVERIFY2(waitFor([&] { return A->plugin->isUserOnline(C->me()) && C->plugin->isUserOnline(A->me()); }, 15000),
                 "presence did not propagate");
    }

    void pmDirect()
    {
        QSignalSpy rxA(A->events, &mx::ChatEvents::messageReceived);
        QSignalSpy rxB(B->events, &mx::ChatEvents::messageReceived);
        A->chat->sendPrivateMessage(B->me(), QStringLiteral("hi Bob, direct"));
        QVERIFY(waitFor([&] { return msgs(rxB, mx::ChatKind::Private).size() == 1; }));
        const auto m = msgs(rxB, mx::ChatKind::Private).first();
        QCOMPARE(m.text, QStringLiteral("hi Bob, direct"));
        QCOMPARE(m.from.id, A->me().id);
        QCOMPARE(m.from.name, A->me().name);
        QVERIFY(!m.to.isValid());   // [ovl-2] only our own echo carries `to` (the UI files it as ours)
        QVERIFY(!m.msgId.isEmpty());
        QVERIFY(m.roomId.isEmpty());
        QCOMPARE(rxA.count(), 0);   // no local echo
    }

    void pmRelayedEncrypted()
    {
        QVERIFY(A->plugin->nodeForUser(C->me().id).size() == 64);
        QVERIFY(!A->plugin->node()->isConnected(C->plugin->nodeId()));
        QSignalSpy rxC(C->events, &mx::ChatEvents::messageReceived);
        QSignalSpy rxB(B->events, &mx::ChatEvents::messageReceived);
        A->chat->sendPrivateMessage(C->me(), QStringLiteral("secret for Carol"));
        QVERIFY2(waitFor([&] { return msgs(rxC, mx::ChatKind::Private).size() == 1; }, 10000), "relayed PM lost");
        const auto m = msgs(rxC, mx::ChatKind::Private).first();
        QCOMPARE(m.text, QStringLiteral("secret for Carol"));
        QCOMPARE(m.from.id, A->me().id);
        QVERIFY(!A->plugin->node()->isConnected(C->plugin->nodeId()));   // it really went through B
        QCOMPARE(msgs(rxB, mx::ChatKind::Private).size(), 0);            // B relayed but cannot read it
        // and back: C -> A (A advertises an address, so C connects on demand)
        QSignalSpy rxA(A->events, &mx::ChatEvents::messageReceived);
        C->chat->sendPrivateMessage(A->me(), QStringLiteral("thanks Alice"));
        QVERIFY(waitFor([&] { return msgs(rxA, mx::ChatKind::Private).size() == 1; }, 10000));
        QVERIFY(waitFor([&] { return C->plugin->node()->isConnected(A->plugin->nodeId()); }, 3000));
    }

    void pmPrivacy()
    {
        B->settings.setValue(QString(mx::keys::messages::AllowPrivateMessages), QStringLiteral("nobody"));
        QSignalSpy rxA(A->events, &mx::ChatEvents::messageReceived);
        QSignalSpy rxB(B->events, &mx::ChatEvents::messageReceived);
        A->chat->sendPrivateMessage(B->me(), QStringLiteral("are you there?"));
        QVERIFY(waitFor([&] { return msgs(rxA, mx::ChatKind::Error).size() == 1; }));
        QVERIFY(msgs(rxA, mx::ChatKind::Error).first().text.contains(QStringLiteral("not accepting")));
        QCOMPARE(msgs(rxB, mx::ChatKind::Private).size(), 0);
        // "hotlist" mode accepts users on the hotlist
        B->settings.setValue(QString(mx::keys::messages::AllowPrivateMessages), QStringLiteral("hotlist"));
        B->plugin->setHotlist({A->me()});
        A->chat->sendPrivateMessage(B->me(), QStringLiteral("now?"));
        QVERIFY(waitFor([&] { return msgs(rxB, mx::ChatKind::Private).size() == 1; }));
        B->settings.setValue(QString(mx::keys::messages::AllowPrivateMessages), QStringLiteral("everyone"));
        // unknown user
        mx::UserRef ghost{mx::Net::Overlay, QString(64, QLatin1Char('a')), QStringLiteral("Ghost"), QString()};
        A->chat->sendPrivateMessage(ghost, QStringLiteral("boo"));
        QVERIFY(waitFor([&] { return msgs(rxA, mx::ChatKind::Error).size() == 2; }));
    }

    void browsePaged()
    {
        for (int i = 0; i < 1000; ++i) {
            mx::SharedFile f;
            f.path = QStringLiteral("/home/bob/My Music/Artist %1 - Song %2.mp3").arg(i / 10).arg(i);
            f.name = QFileInfo(f.path).fileName();
            f.size = 3000000 + i;
            f.type = mx::MediaType::Audio;
            f.bitrateKbps = 192;
            f.durationSec = 200 + i % 60;
            f.sampleRateHz = 44100;
            f.hashes.sha1 = QCryptographicHash::hash(f.name.toUtf8(), QCryptographicHash::Sha1);
            f.hashes.ed2k = QCryptographicHash::hash(f.name.toUtf8(), QCryptographicHash::Md4);
            B->library.files << f;
        }
        B->setShared();
        QSignalSpy res(A->plugin, &mx::NetworkPlugin::browseResults);
        QSignalSpy fail(A->plugin, &mx::NetworkPlugin::browseFailed);
        A->plugin->browse(7, B->me());
        QVERIFY(waitFor([&] { return !res.isEmpty() && res.last().at(3).toBool(); }, 15000));
        QCOMPARE(fail.count(), 0);
        QCOMPARE(res.count(), 3);   // 400 + 400 + 200
        int total = 0;
        for (const auto &args : res) {
            QCOMPARE(args.at(0).toUInt(), 7u);
            QCOMPARE(args.at(1).value<mx::UserRef>().id, B->me().id);
            total += args.at(2).value<QList<mx::BrowseEntry>>().size();
        }
        QCOMPARE(total, 1000);
        const auto first = res.first().at(2).value<QList<mx::BrowseEntry>>().first();
        QCOMPARE(first.path, QStringLiteral("\\My Music\\Artist 0 - Song 0.mp3"));   // no full local path
        QCOMPARE(first.size, 3000000);
        QCOMPARE(first.bitrateKbps, 192);
        QCOMPARE(first.hashes.sha1, B->library.files.first().hashes.sha1);
        QCOMPARE(first.hashes.ed2k, B->library.files.first().hashes.ed2k);
    }

    void browseRefused()
    {
        B->settings.setValue(QString(mx::keys::messages::AllowBrowse), QStringLiteral("nobody"));
        QSignalSpy res(A->plugin, &mx::NetworkPlugin::browseResults);
        QSignalSpy fail(A->plugin, &mx::NetworkPlugin::browseFailed);
        A->plugin->browse(8, B->me());
        QVERIFY(waitFor([&] { return fail.count() == 1; }));
        QCOMPARE(res.count(), 0);
        QVERIFY(fail.first().at(2).toString().contains(QStringLiteral("does not allow")));
        B->settings.setValue(QString(mx::keys::messages::AllowBrowse), QStringLiteral("hotlist"));
        B->plugin->setHotlist({A->me()});
        A->plugin->browse(9, B->me());
        QVERIFY(waitFor([&] { return !res.isEmpty() && res.last().at(3).toBool(); }, 15000));
        B->settings.setValue(QString(mx::keys::messages::AllowBrowse), QStringLiteral("everyone"));
    }

    void whois()
    {
        B->chat->joinRoom(QStringLiteral("Music"));
        QSignalSpy who(A->plugin, &mx::NetworkPlugin::whoisResult);
        A->plugin->whois(B->me());
        QVERIFY(waitFor([&] { return who.count() == 1; }));
        const QVariantMap m = who.first().at(1).toMap();
        QCOMPARE(m.value(QStringLiteral("name")).toString(), B->me().name);
        QCOMPARE(m.value(QStringLiteral("files")).toInt(), 1000);
        QVERIFY(m.value(QStringLiteral("client")).toString().contains(QStringLiteral("OctoMX")));
        QCOMPARE(m.value(QStringLiteral("connection")).toString(), QStringLiteral("DSL"));
        QVERIFY(m.value(QStringLiteral("rooms")).toStringList().contains(QStringLiteral("Music")));
        QVERIFY(m.value(QStringLiteral("reachable")).toBool());
        B->settings.setValue(QString(mx::keys::messages::AllowWhois), false);
        A->plugin->whois(B->me());
        QVERIFY(waitFor([&] { return who.count() == 2; }));
        QVERIFY(who.last().at(1).toMap().contains(QStringLiteral("error")));
        B->settings.setValue(QString(mx::keys::messages::AllowWhois), true);
    }

    void presenceUpdates()
    {
        QSignalSpy pres(A->events, &mx::ChatEvents::userPresence);
        QVERIFY(A->plugin->isUserOnline(B->me()));
        B->settings.setValue(QString(mx::keys::identity::Connection), QStringLiteral("Cable"));
        QVERIFY(waitFor([&] {
            for (const auto &a : pres)
                if (a.at(0).value<mx::UserRef>().id == B->me().id && a.at(3).toString() == QStringLiteral("Cable")) return true;
            return false;
        }, 8000));
        // screen-name change is announced
        B->settings.setValue(QString(mx::keys::identity::ScreenName), QStringLiteral("Robert"));
        QVERIFY(B->me().name.startsWith(QStringLiteral("Robert_")));
        QVERIFY(waitFor([&] { return A->plugin->userRefFor(B->me().id).name == B->me().name; }, 8000));
    }

    void cleanupTestCase()
    {
        A.reset();
        B.reset();
        C.reset();
        QVERIFY(RatsNode::waitAllStopped(8000));
    }
};

QTEST_GUILESS_MAIN(TstOverlayUsers)
#include "tst_overlay_users.moc"
