// SPDX-License-Identifier: GPL-3.0-or-later
#include <QtTest/QtTest>
#include <QtTest/QSignalSpy>

#include "core/network_manager.h"
#include "core/user_manager.h"
#include "fake_plugin.h"
#include "mx/settings_keys.h"
#include "mx/simple_settings.h"

#include <QtCore/QRegularExpression>
#include <QtCore/QTemporaryDir>

using namespace mx;
using mx::test::FakePlugin;

class TestNetwork : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { registerMetaTypes(); }

    void screenNames()
    {
        const QRegularExpression re(QStringLiteral("^Guest\\d{3}_\\d{5}$"));
        for (int i = 0; i < 20; ++i)
            QVERIFY(re.match(NetworkManager::generateScreenName()).hasMatch());
        QCOMPARE(NetworkManager::sanitizeScreenName(QStringLiteral("  bad name/with:chars  ")), QStringLiteral("bad_name_with_chars"));
        QCOMPARE(NetworkManager::sanitizeScreenName(QString(50, QLatin1Char('x'))).size(), 32);
    }

    // [ovl-2] one screen name: what the user sees is what the OctoMX network announces (suffix from the
    // user key), existing profiles included; own rows are recognised by id
    void oneScreenName()
    {
        SimpleSettings settings;
        settings.setValue(QString(keys::identity::ScreenName), QStringLiteral("Guest123_11111"));   // an old profile
        auto *ov = new FakePlugin(Net::Overlay, NetworkPlugin::CanChat);
        auto *ed = new FakePlugin(Net::ED2K, NetworkPlugin::CanSearch);
        const QString key = QString(64, QLatin1Char('a'));
        ov->me = UserRef{Net::Overlay, key, QStringLiteral("Guest123_22222"), QString()};
        NetworkManager net(&settings, {ov, ed});
        QCOMPARE(net.screenName(), QStringLiteral("Guest123_11111"));   // before the identity is known
        QSignalSpy changed(&net, &NetworkManager::screenNameChanged);
        QTemporaryDir dir;
        net.initPlugins(dir.path(), nullptr, nullptr);
        QCOMPARE(net.screenName(), QStringLiteral("Guest123_22222"));   // migrated to the announced name
        QCOMPARE(settings.s(QString(keys::identity::ScreenName)), QStringLiteral("Guest123_22222"));
        QVERIFY(!changed.isEmpty());
        // the Change dialog edits the base; a typed suffix never wins over the key-derived one
        net.setScreenName(QStringLiteral("Alice"));
        QCOMPARE(net.screenName(), QStringLiteral("Alice_22222"));
        net.setScreenName(QStringLiteral("Alice_11111"));
        QCOMPARE(net.screenName(), QStringLiteral("Alice_22222"));
        // whoever writes the setting (Settings page, wizard), it is kept canonical
        settings.setValue(QString(keys::identity::ScreenName), QStringLiteral("Bobby_22222"));
        QCOMPARE(net.screenName(), QStringLiteral("Bobby_22222"));
        QCOMPARE(changed.last().first().toString(), QStringLiteral("Bobby_22222"));
        settings.setValue(QString(keys::identity::ScreenName), QStringLiteral("two words"));
        QCOMPARE(net.screenName(), QStringLiteral("two_words_22222"));
        // own rows by id: not by name, and no Hotlist / ignore entry for ourselves
        UserManager users(&settings, &net, nullptr);
        QVERIFY(users.isSelf(ov->me));
        QVERIFY(users.isSelf(UserRef{Net::Overlay, key.toUpper(), QStringLiteral("whatever"), QString()}));
        QVERIFY(!users.isSelf(UserRef{Net::Overlay, QString(64, QLatin1Char('b')), net.screenName(), QString()}));
        QVERIFY(!users.isSelf(UserRef{Net::ED2K, key, QString(), QString()}));
        users.addToHotlist(ov->me);
        QVERIFY(users.hotlist().isEmpty());
        users.ignore(ov->me, true);
        QVERIFY(!users.isIgnored(ov->me));
        // no network identity at all: the old per-installation suffix
        SimpleSettings s2;
        NetworkManager plain(&s2, {new FakePlugin(Net::ED2K, NetworkPlugin::CanSearch)});
        plain.initPlugins(dir.path(), nullptr, nullptr);
        plain.setScreenName(QStringLiteral("Carol"));
        QVERIFY(QRegularExpression(QStringLiteral("^Carol_\\d{5}$")).match(plain.screenName()).hasMatch());
    }

    // [core-5] counts in core texts: "1 peer", "2 peers", "1 index", "0 nodes"
    void countNounWording()
    {
        QCOMPARE(countNoun(1, QStringLiteral("peer")), QStringLiteral("1 peer"));
        QCOMPARE(countNoun(2, QStringLiteral("peer")), QStringLiteral("2 peers"));
        QCOMPARE(countNoun(0, QStringLiteral("node")), QStringLiteral("0 nodes"));
        QCOMPARE(countNoun(1, QStringLiteral("index"), QStringLiteral("indexes")), QStringLiteral("1 index"));
        QCOMPARE(countNoun(10, QStringLiteral("index"), QStringLiteral("indexes")), QStringLiteral("10 indexes"));
    }

    // [core-5] "Primary Connections": 30 rats-search peers are one link, ten answering web indexes
    // are one, an overlay with 30 librats links of which 2 are verified OctoMX users counts 2;
    // primaryByNet always adds up to primary
    void primaryCountsRealLinksOnce()
    {
        SimpleSettings settings;
        auto *ov = new FakePlugin(Net::Overlay, NetworkPlugin::CanSearch);
        auto *rats = new FakePlugin(Net::Rats, NetworkPlugin::CanSearch);
        auto *ed = new FakePlugin(Net::ED2K, NetworkPlugin::CanSearch);
        auto *web = new FakePlugin(Net::Web, NetworkPlugin::CanSearch);
        web->peersWhenConnected = 10;   // ten index sites answering
        ov->peersWhenConnected = 30;
        ov->st.primaryLinks = 2;
        rats->peersWhenConnected = 30;
        ed->peersWhenConnected = 1;
        NetworkManager net(&settings, {ov, rats, ed, web});
        QTemporaryDir dir;
        net.initPlugins(dir.path(), nullptr, nullptr);
        net.startNetworks();
        const NetworkSummary s = net.summary();
        QCOMPARE(s.primaryByNet.value(Net::Overlay), 2);
        QCOMPARE(s.primaryByNet.value(Net::Rats), 1);
        QCOMPARE(s.primaryByNet.value(Net::ED2K), 1);
        QCOMPARE(s.primaryByNet.value(Net::Web), 1);
        QCOMPARE(s.primary, 5);
        int sum = 0;
        for (int v : s.primaryByNet) sum += v;
        QCOMPARE(sum, s.primary);
        QCOMPARE(s.peersByNet.value(Net::Rats), 30);   // the raw figure stays available
        QCOMPARE(net.titleSummary(), QStringLiteral("Online P=5 S=0"));
    }

    void lifecycle()
    {
        QTemporaryDir dir;
        SimpleSettings settings;
        auto *ed = new FakePlugin(Net::ED2K, NetworkPlugin::CanSearch | NetworkPlugin::CanServe);
        auto *bt = new FakePlugin(Net::BitTorrent, NetworkPlugin::CanSearch);
        auto *gn = new FakePlugin(Net::Gnutella, NetworkPlugin::CanSearch | NetworkPlugin::CanServe);
        auto *rats = new FakePlugin(Net::Rats, NetworkPlugin::CanSearch);
        gn->connectOnStart = false;    // stays "Connecting"
        settings.setValue(keys::gnutella::Enabled, true);
        QPointer<FakePlugin> watch = ed;
        {
            NetworkManager net(&settings, {ed, bt, gn, rats});
            // first run: a WinMX-style screen name was generated and persisted
            QVERIFY(QRegularExpression(QStringLiteral("^Guest\\d{3}_\\d{5}$")).match(net.screenName()).hasMatch());
            QCOMPARE(settings.s(keys::identity::ScreenName), net.screenName());
            // fixed order (by Net), one row per plugin
            QList<NetworkStatus> list = net.networks();
            QCOMPARE(list.size(), 4);
            QCOMPARE(list.at(0).net, Net::BitTorrent);
            QCOMPARE(list.at(1).net, Net::ED2K);
            QCOMPARE(net.overallState(), ConnState::Disconnected);
            QCOMPARE(net.titleSummary(), QStringLiteral("Offline"));

            net.initPlugins(dir.path(), nullptr, nullptr);
            QCOMPARE(ed->initCount, 1);
            QCOMPARE(ed->ctx.dataDir, dir.path() + QStringLiteral("/ed2k"));
            QVERIFY(QFileInfo(dir.path() + QStringLiteral("/bt")).isDir());
            QVERIFY(QFileInfo(dir.path() + QStringLiteral("/gnutella")).isDir());
            QCOMPARE(rats->ctx.dataDir, dir.path() + QStringLiteral("/rats"));
            QVERIFY(QFileInfo(rats->ctx.dataDir).isDir());
            QCOMPARE(ed->ctx.clientName, QStringLiteral("OctoMX"));
            QCOMPARE(ed->ctx.screenName, net.screenName());
            QVERIFY(ed->ctx.settings == &settings);
            net.initPlugins(dir.path(), nullptr, nullptr);   // idempotent
            QCOMPARE(ed->initCount, 1);

            // shared files are pushed to CanServe plugins only
            SharedFile f;
            f.path = QStringLiteral("/x/song.mp3");
            net.setSharedFiles({f});
            QCOMPARE(ed->sharedLists.size(), 1);
            QCOMPARE(gn->sharedLists.size(), 1);
            QCOMPARE(bt->sharedLists.size(), 0);

            QSignalSpy overall(&net, &NetworkService::overallChanged);
            net.startNetworks();   // network/connectOnStart defaults to true
            QCOMPARE(ed->startCount, 1);
            QCOMPARE(bt->startCount, 1);
            QCOMPARE(gn->startCount, 1);
            QCOMPARE(net.overallState(), ConnState::Connected);
            QCOMPARE(net.overallHeadline(), QStringLiteral("Ready"));
            QCOMPARE(net.overallDetail(), QStringLiteral("You can search and download now."));
            // [engine-4] primary = eD2k servers (3) + the BitTorrent DHT counted once (1) + [core-5]
            // the Rats swarm counted once (1); Gnutella is still connecting
            QCOMPARE(net.titleSummary(), QStringLiteral("Online P=5 S=0"));
            net.setSourceCounter([] { return 12; });
            QCOMPARE(net.titleSummary(), QStringLiteral("Online P=5 S=12"));   // S falls back to known sources
            net.setSecondaryCounter([] { return 4; });
            QCOMPARE(net.titleSummary(), QStringLiteral("Online P=5 S=4"));
            QCOMPARE(net.summary().secondary, 4);
            QCOMPARE(net.summary().knownSources, 12);
            QCOMPARE(net.summary().primaryByNet.value(Net::BitTorrent), 1);
            QCOMPARE(net.summary().peersByNet.value(Net::BitTorrent), 3);
            QCOMPARE(net.summary().primaryByNet.value(Net::ED2K), 3);
            QVERIFY(overall.count() >= 1);
            QCOMPARE(net.activePlugins(NetworkPlugin::CanServe).size(), 2);

            // disabling a network stops it and persists the flag
            net.setEnabled(Net::BitTorrent, false);
            QCOMPARE(bt->stopCount, 1);
            QVERIFY(!settings.b(keys::bt::Enabled));
            QCOMPARE(net.status(Net::BitTorrent).state, ConnState::Disabled);
            QVERIFY(!net.isEnabled(Net::BitTorrent));
            net.setEnabled(Net::BitTorrent, true);         // back on while online → restarted
            QCOMPARE(bt->startCount, 2);

            // changing the setting directly (Settings dialog) has the same effect
            settings.setValue(keys::ed2k::Enabled, false);
            QCOMPARE(ed->stopCount, 1);
            settings.setValue(keys::ed2k::Enabled, true);
            QCOMPARE(ed->startCount, 2);

            net.disconnectAll();
            QCOMPARE(net.overallState(), ConnState::Disconnected);
            QCOMPARE(net.overallHeadline(), QStringLiteral("Not connected"));
            net.connectNetwork(Net::Gnutella);
            QCOMPARE(net.overallState(), ConnState::Connecting);
            QCOMPARE(net.overallHeadline(), QStringLiteral("Finding network connections"));
            QCOMPARE(net.titleSummary(), QStringLiteral("Connecting..."));
            gn->setConnState(ConnState::Connected);
            QCOMPARE(net.overallHeadline(), QStringLiteral("Ready"));
            net.disconnectNetwork(Net::Gnutella);
            QVERIFY(!net.isRunning(Net::Gnutella));

            // servers passthrough
            QSignalSpy srv(&net, &NetworkService::serversChanged);
            net.addServer(Net::ED2K, QStringLiteral("1.2.3.4:4661"), QStringLiteral("Test server"));
            QCOMPARE(ed->addedServers, QStringList{QStringLiteral("1.2.3.4:4661")});
            QCOMPARE(srv.count(), 1);
            QCOMPARE(srv.at(0).at(0).value<Net>(), Net::ED2K);
            QCOMPARE(net.servers(Net::ED2K).size(), 1);
            QCOMPARE(net.servers().size(), 1);
            QCOMPARE(net.servers(Net::BitTorrent).size(), 0);
            net.connectServer(Net::ED2K, QStringLiteral("1.2.3.4:4661"));
            QCOMPARE(ed->connectedServers.size(), 1);
            net.removeServer(Net::ED2K, QStringLiteral("1.2.3.4:4661"));
            QCOMPARE(net.servers(Net::ED2K).size(), 0);

            // screen name
            QSignalSpy nameSpy(&net, &NetworkService::screenNameChanged);
            net.setScreenName(QStringLiteral("Quill233 25471"));
            QCOMPARE(net.screenName(), QStringLiteral("Quill233_25471"));
            QCOMPARE(nameSpy.count(), 1);
            net.setScreenName(QStringLiteral("   "));       // ignored
            QCOMPARE(net.screenName(), QStringLiteral("Quill233_25471"));
            net.setScreenName(QStringLiteral("Bob"));        // OctoMX appends _<5-digit id>
            QVERIFY(QRegularExpression(QStringLiteral("^Bob_\\d{5}$")).match(net.screenName()).hasMatch());
            const QString bob = net.screenName();
            net.setScreenName(QStringLiteral("Bob"));        // stable suffix
            QCOMPARE(net.screenName(), bob);
        }
        QVERIFY(watch.isNull());   // plugins are owned and deleted by the manager
    }

    void noAutoConnectAndNoPlugins()
    {
        SimpleSettings settings;
        settings.setValue(keys::network::ConnectOnStart, false);
        settings.setValue(keys::identity::ScreenName, QStringLiteral("Keep_Me"));
        auto *ed = new FakePlugin(Net::ED2K, NetworkPlugin::CanSearch);
        NetworkManager net(&settings, {ed});
        QCOMPARE(net.screenName(), QStringLiteral("Keep_Me"));   // not regenerated
        net.initPlugins(QString(), nullptr, nullptr);
        net.startNetworks();
        QCOMPARE(ed->startCount, 0);
        QCOMPARE(net.overallHeadline(), QStringLiteral("Not connected"));
        net.startNetworks(true);
        QCOMPARE(ed->startCount, 1);

        SimpleSettings s2;
        NetworkManager empty(&s2, {});
        QCOMPARE(empty.overallState(), ConnState::Disabled);
        QCOMPARE(empty.overallDetail(), QStringLiteral("No networks are available."));
        QCOMPARE(empty.status(Net::Kad).state, ConnState::Disabled);
        empty.connectAll();
        QCOMPARE(empty.networks().size(), 0);
    }
};

QTEST_GUILESS_MAIN(TestNetwork)
#include "tst_network.moc"
