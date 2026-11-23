// SPDX-License-Identifier: GPL-3.0-or-later
#include <QtTest/QtTest>
#include <QtTest/QSignalSpy>

#include "core/chat_commands.h"
#include "core/chat_manager.h"
#include "core/dao.h"
#include "core/database.h"
#include "core/network_manager.h"
#include "core/user_manager.h"
#include "fake_plugin.h"
#include "mx/settings_keys.h"
#include "mx/simple_settings.h"

#include <QtCore/QTemporaryDir>

using namespace mx;
using mx::test::FakeChatPlugin;
using mx::test::FakePlugin;

namespace {
QList<ChatMessage> messages(const QSignalSpy &spy)
{
    QList<ChatMessage> out;
    for (const auto &args : spy) out << args.at(0).value<ChatMessage>();
    return out;
}
}

class TestChat : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { registerMetaTypes(); }

    void parsing()
    {
        QCOMPARE(parseChatCommand(QString()).type, ChatCommand::Empty);
        QCOMPARE(parseChatCommand(QStringLiteral("   ")).type, ChatCommand::Empty);
        ChatCommand c = parseChatCommand(QStringLiteral("hello /world"));
        QCOMPARE(c.type, ChatCommand::Text);
        QCOMPARE(c.text, QStringLiteral("hello /world"));
        c = parseChatCommand(QStringLiteral("//slash at start"));
        QCOMPARE(c.type, ChatCommand::Text);
        QCOMPARE(c.text, QStringLiteral("/slash at start"));
        c = parseChatCommand(QStringLiteral("/me waves hello"));
        QCOMPARE(c.type, ChatCommand::Me);
        QCOMPARE(c.text, QStringLiteral("waves hello"));
        QCOMPARE(parseChatCommand(QStringLiteral("/ACTION dances")).type, ChatCommand::Me);
        c = parseChatCommand(QStringLiteral("/topic New topic here"));
        QCOMPARE(c.type, ChatCommand::Topic);
        QCOMPARE(c.text, QStringLiteral("New topic here"));
        QCOMPARE(parseChatCommand(QStringLiteral("/topic")).text, QString());
        QCOMPARE(parseChatCommand(QStringLiteral("/motd")).type, ChatCommand::Motd);
        c = parseChatCommand(QStringLiteral("/setmotd a | b"));
        QCOMPARE(c.type, ChatCommand::SetMotd);
        QCOMPARE(c.text, QStringLiteral("a\nb"));
        c = parseChatCommand(QStringLiteral("/join   OctoMX Lobby_0A1B2C3D4E5F"));
        QCOMPARE(c.type, ChatCommand::Join);
        QCOMPARE(c.target, QStringLiteral("OctoMX Lobby_0A1B2C3D4E5F"));
        c = parseChatCommand(QStringLiteral("/part"));
        QCOMPARE(c.type, ChatCommand::Part);
        QVERIFY(c.target.isEmpty());
        c = parseChatCommand(QStringLiteral("/msg Quill233_25471 hi there, how are you?"));
        QCOMPARE(c.type, ChatCommand::Msg);
        QCOMPARE(c.target, QStringLiteral("Quill233_25471"));
        QCOMPARE(c.text, QStringLiteral("hi there, how are you?"));
        c = parseChatCommand(QStringLiteral("/msg Bob"));
        QCOMPARE(c.type, ChatCommand::Msg);
        QVERIFY(c.text.isEmpty());
        QCOMPARE(parseChatCommand(QStringLiteral("/ignore Spammer extra")).target, QStringLiteral("Spammer"));
        QCOMPARE(parseChatCommand(QStringLiteral("/unignore Spammer")).type, ChatCommand::Unignore);
        QCOMPARE(parseChatCommand(QStringLiteral("/whois Bob")).type, ChatCommand::Whois);
        QCOMPARE(parseChatCommand(QStringLiteral("/clear")).type, ChatCommand::Clear);
        QCOMPARE(parseChatCommand(QStringLiteral("/help")).type, ChatCommand::Help);
        QCOMPARE(parseChatCommand(QStringLiteral("/?")).type, ChatCommand::Help);
        c = parseChatCommand(QStringLiteral("/frobnicate now"));
        QCOMPARE(c.type, ChatCommand::Unknown);
        QCOMPARE(c.name, QStringLiteral("frobnicate"));
        QVERIFY(chatHelpLines().size() > 5);
    }

    void offlineWithoutBackend()
    {
        SimpleSettings settings;
        auto *ed = new FakePlugin(Net::ED2K, NetworkPlugin::CanSearch);   // no chat
        NetworkManager net(&settings, {ed});
        UserManager users(&settings, &net, nullptr);
        ChatManager chat(&settings, &net, &users, nullptr);
        chat.start();
        QVERIFY(!chat.hasBackend());
        QSignalSpy msgs(&chat, &ChatService::messageReceived);
        QSignalSpy list(&chat, &ChatService::roomListChanged);
        chat.refreshRoomList();
        QVERIFY(list.wait(500));
        QVERIFY(chat.roomList().isEmpty());
        QVERIFY(chat.joinedRooms().isEmpty());
        chat.joinRoom(QStringLiteral("Lobby"));
        QCOMPARE(msgs.count(), 1);
        ChatMessage m = msgs.at(0).at(0).value<ChatMessage>();
        QCOMPARE(m.kind, ChatKind::System);
        QVERIFY(m.text.contains(QStringLiteral("offline")));
        chat.sendMessage(QStringLiteral("Lobby"), QStringLiteral("anyone?"));
        QCOMPARE(msgs.count(), 2);
        chat.sendMessage(QStringLiteral("Lobby"), QStringLiteral("/help"));   // local commands still work
        QVERIFY(msgs.count() > 5);
        chat.setFavouriteRoom(QStringLiteral("Lobby"), true);
        QCOMPARE(chat.favouriteRooms(), QStringList{QStringLiteral("Lobby")});
        // user service without plugins: lists work, browse/whois fail politely
        QSignalSpy failed(&users, &UserService::browseFailed);
        QSignalSpy whois(&users, &UserService::whoisResult);
        UserRef u = FakeChatPlugin::user(QStringLiteral("Bob"));
        users.addToHotlist(u);
        QCOMPARE(users.hotlist().size(), 1);
        const quint32 id = users.browse(u);
        QVERIFY(failed.wait(500));
        QCOMPARE(failed.at(0).at(0).toUInt(), id);
        users.whois(u);
        QVERIFY(whois.wait(500));
        QVERIFY(whois.at(0).at(1).toMap().contains(QStringLiteral("error")));
    }

    void withBackend()
    {
        QTemporaryDir dir;
        Database db;
        QVERIFY(db.open(dir.filePath(QStringLiteral("octomx.db"))));
        UserListsDao dao(&db);
        SimpleSettings settings;
        settings.setValue(keys::identity::ScreenName, QStringLiteral("Me_12345"));
        auto *ov = new FakeChatPlugin;
        const UserRef bob = FakeChatPlugin::user(QStringLiteral("Bob"));
        const UserRef eve = FakeChatPlugin::user(QStringLiteral("Eve"));
        ov->users.insert(QStringLiteral("Lobby"), {ChatUser{bob, 10, QStringLiteral("DSL"), 0},
                                                   ChatUser{eve, 0, QStringLiteral("56K"), 0}});
        NetworkManager net(&settings, {ov});
        UserManager users(&settings, &net, &dao);
        ChatManager chat(&settings, &net, &users, &dao);
        QVERIFY(chat.hasBackend());
        net.initPlugins(dir.path(), nullptr, nullptr);
        users.start();
        QCOMPARE(ov->hotlists.size(), 1);     // pushed at start (empty list)
        QVERIFY(ov->hotlists.at(0).isEmpty());
        chat.start();
        chat.setFavouriteRoom(QStringLiteral("Lobby"), true);
        QSignalSpy joined(&chat, &ChatService::roomJoined);
        QSignalSpy msgs(&chat, &ChatService::messageReceived);
        QSignalSpy cleared(&chat, &ChatService::backlogCleared);

        net.connectAll();                       // backend connects → favourites auto-joined
        QVERIFY(joined.wait(500));
        QCOMPARE(ov->joinCalls, QStringList{QStringLiteral("Lobby")});
        QCOMPARE(chat.joinedRooms().size(), 1);
        QCOMPARE(chat.roomUsers(QStringLiteral("Lobby")).size(), 2);

        // plain text: forwarded + local echo
        chat.sendMessage(QStringLiteral("Lobby"), QStringLiteral("hello all"));
        QCOMPARE(ov->sentRoom.size(), 1);
        QCOMPARE(ov->sentRoom.at(0).kind, ChatKind::Message);
        QCOMPARE(msgs.count(), 1);
        QCOMPARE(messages(msgs).at(0).from.name, QStringLiteral("Me_12345"));
        // /me
        chat.sendMessage(QStringLiteral("Lobby"), QStringLiteral("/me waves"));
        QCOMPARE(ov->sentRoom.at(1).kind, ChatKind::Action);
        QCOMPARE(ov->sentRoom.at(1).text, QStringLiteral("waves"));
        // /topic
        chat.sendMessage(QStringLiteral("Lobby"), QStringLiteral("/topic Welcome!"));
        QCOMPARE(ov->topics.size(), 1);
        QCOMPARE(ov->topics.at(0).second, QStringLiteral("Welcome!"));
        // [core-5] /setmotd (" | " and "\\n" start new lines) and /motd (shows the room's MOTD lines)
        chat.sendMessage(QStringLiteral("Lobby"), QStringLiteral("/setmotd Rule one | Rule two\\nRule three"));
        QCOMPARE(ov->motds.size(), 1);
        QCOMPARE(ov->motds.at(0).second, QStringLiteral("Rule one\nRule two\nRule three"));
        {
            ChatRoomInfo upd = chat.joinedRooms().first();
            upd.motd = QStringLiteral("Rule one\nRule two");
            QSignalSpy updated(&chat, &ChatService::roomUpdated);
            emit ov->events->roomUpdated(upd);
            QCOMPARE(updated.count(), 1);
            QCOMPARE(chat.joinedRooms().first().motd, upd.motd);
            const int before = msgs.count();
            chat.sendMessage(QStringLiteral("Lobby"), QStringLiteral("/motd"));
            QCOMPARE(msgs.count(), before + 2);
            QCOMPARE(messages(msgs).at(before).kind, ChatKind::Motd);
            QCOMPARE(messages(msgs).at(before).text, QStringLiteral("Rule one"));
            QCOMPARE(messages(msgs).at(before + 1).text, QStringLiteral("Rule two"));
            // a Motd line from the network reaches the room's backlog like any other line
            ov->receive(QStringLiteral("Lobby"), UserRef(), QStringLiteral("Rule one"), ChatKind::Motd);
            QCOMPARE(messages(msgs).last().kind, ChatKind::Motd);
            QCOMPARE(chat.backlog(QStringLiteral("Lobby")).last().kind, ChatKind::Motd);
        }
        // /msg resolves the user by name from the room list
        chat.sendMessage(QStringLiteral("Lobby"), QStringLiteral("/msg bob psst"));
        QCOMPARE(ov->sentPrivate.size(), 1);
        QCOMPARE(ov->sentPrivate.at(0).first.id, bob.id);
        QCOMPARE(ov->sentPrivate.at(0).second, QStringLiteral("psst"));
        const ChatMessage echo = messages(msgs).last();
        QCOMPARE(echo.kind, ChatKind::Private);
        QCOMPARE(echo.to.id, bob.id);
        QCOMPARE(chat.backlog(ChatManager::pmKey(bob)).size(), 1);
        chat.sendMessage(QStringLiteral("Lobby"), QStringLiteral("/msg nobody hi"));
        QCOMPARE(messages(msgs).last().kind, ChatKind::Error);
        // incoming message from Bob is shown
        const int before = msgs.count();
        ov->receive(QStringLiteral("Lobby"), bob, QStringLiteral("hi Me"));
        QCOMPARE(msgs.count(), before + 1);
        // /ignore Eve → her messages are dropped, persisted in the DB
        chat.sendMessage(QStringLiteral("Lobby"), QStringLiteral("/ignore Eve"));
        QVERIFY(users.isIgnored(eve));
        QCOMPARE(dao.ignored().size(), 1);
        const int beforeEve = msgs.count();
        ov->receive(QStringLiteral("Lobby"), eve, QStringLiteral("spam spam"));
        QCOMPARE(msgs.count(), beforeEve);
        chat.sendMessage(QStringLiteral("Lobby"), QStringLiteral("/unignore Eve"));
        QVERIFY(!users.isIgnored(eve));
        // name-only ignore of an unknown user matches by screen name
        chat.sendMessage(QStringLiteral("Lobby"), QStringLiteral("/ignore Mallory"));
        UserRef mallory = FakeChatPlugin::user(QStringLiteral("Mallory"));
        QVERIFY(users.isIgnored(mallory));
        // backlog and /clear
        QVERIFY(chat.backlog(QStringLiteral("Lobby")).size() >= 4);
        chat.sendMessage(QStringLiteral("Lobby"), QStringLiteral("/clear"));
        QCOMPARE(cleared.count(), 1);
        QVERIFY(chat.backlog(QStringLiteral("Lobby")).isEmpty());
        // /whois prints the result into the room
        chat.sendMessage(QStringLiteral("Lobby"), QStringLiteral("/whois Bob"));
        QCOMPARE(ov->whoisCalls.size(), 1);
        emit ov->whoisResult(bob, QVariantMap{{QStringLiteral("files"), 10}});
        QVERIFY(messages(msgs).last().text.contains(QStringLiteral("files")));
        // unknown command
        chat.sendMessage(QStringLiteral("Lobby"), QStringLiteral("/bogus"));
        QCOMPARE(messages(msgs).last().kind, ChatKind::Error);
        // backlog cap
        settings.setValue(keys::messages::BlockFloods, false);
        settings.setValue(keys::chat::BacklogLines, 50);
        for (int i = 0; i < 120; ++i) ov->receive(QStringLiteral("Lobby"), bob, QStringLiteral("line %1").arg(i));
        QCOMPARE(chat.backlog(QStringLiteral("Lobby")).size(), 50);
        QCOMPARE(chat.backlog(QStringLiteral("Lobby")).last().text, QStringLiteral("line 119"));

        // private message privacy
        settings.setValue(keys::messages::BlockFloods, false);
        int n = msgs.count();
        ov->receive(QString(), bob, QStringLiteral("pm 1"), ChatKind::Private);
        QCOMPARE(msgs.count(), n + 1);
        settings.setValue(keys::messages::AllowPrivateMessages, QStringLiteral("hotlist"));
        ov->receive(QString(), bob, QStringLiteral("pm 2"), ChatKind::Private);
        QCOMPARE(msgs.count(), n + 1);                    // Bob is not on the hotlist
        users.addToHotlist(bob);
        ov->receive(QString(), bob, QStringLiteral("pm 3"), ChatKind::Private);
        QCOMPARE(msgs.count(), n + 2);
        settings.setValue(keys::messages::AllowPrivateMessages, QStringLiteral("everyone"));
        settings.setValue(keys::messages::AllowFrom, QStringLiteral("restricted"));
        settings.setValue(keys::messages::AllowFromHotlist, false);
        ov->receive(QString(), bob, QStringLiteral("pm 4"), ChatKind::Private);
        QCOMPARE(msgs.count(), n + 3);                    // Bob is in the same room
        UserRef stranger = FakeChatPlugin::user(QStringLiteral("Stranger"));
        ov->receive(QString(), stranger, QStringLiteral("pm 5"), ChatKind::Private);
        QCOMPARE(msgs.count(), n + 3);                    // not allowed
        settings.setValue(keys::messages::AllowFrom, QStringLiteral("any"));
        // auto-reply
        settings.setValue(keys::messages::AutoReply, true);
        const int sentBefore = ov->sentPrivate.size();
        ov->receive(QString(), stranger, QStringLiteral("are you there?"), ChatKind::Private);
        QCOMPARE(ov->sentPrivate.size(), sentBefore + 1);
        ov->receive(QString(), stranger, QStringLiteral("hello??"), ChatKind::Private);
        QCOMPARE(ov->sentPrivate.size(), sentBefore + 1); // only once per 10 minutes
        // flood protection
        settings.setValue(keys::messages::BlockFloods, true);
        settings.setValue(keys::messages::FloodLimit, 3);
        n = msgs.count();
        const UserRef flooder = FakeChatPlugin::user(QStringLiteral("Flooder"));
        for (int i = 0; i < 10; ++i) ov->receive(QStringLiteral("Lobby"), flooder, QStringLiteral("flood"));
        QCOMPARE(msgs.count(), n + 3);

        // the hotlist is pushed to every plugin on change
        QVERIFY(!ov->hotlists.isEmpty());
        QCOMPARE(ov->hotlists.last().size(), 1);
        QCOMPARE(ov->hotlists.last().at(0).id, bob.id);
        // presence (from any chat plugin) updates the hotlist entry
        QSignalSpy hlChanged(&users, &UserService::hotlistChanged);
        emit ov->events->userPresence(bob, true, 42, QStringLiteral("Cable"));
        QCOMPARE(hlChanged.count(), 1);
        QVERIFY(users.hotlist().at(0).online);
        QCOMPARE(users.hotlist().at(0).files, 42);
        QCOMPARE(users.hotlist().at(0).connection, QStringLiteral("Cable"));
        QVERIFY(users.hotlist().at(0).lastSeen.secsTo(QDateTime::currentDateTime()) < 5);
        emit ov->events->userPresence(bob, false, -1, QString());
        QCOMPARE(hlChanged.count(), 2);
        QVERIFY(!users.hotlist().at(0).online);
        QCOMPARE(users.hotlist().at(0).files, 42);              // unknown values keep the old ones
        emit ov->events->userPresence(eve, true, 1, QString()); // not on the hotlist → ignored
        QCOMPARE(hlChanged.count(), 2);
        users.removeFromHotlist(eve);                           // not on it: no push
        const int pushes = ov->hotlists.size();
        users.removeFromHotlist(bob);
        QCOMPARE(ov->hotlists.size(), pushes + 1);
        QVERIFY(ov->hotlists.last().isEmpty());
        users.addToHotlist(bob);
        QCOMPARE(ov->hotlists.last().size(), 1);

        // /part
        QSignalSpy left(&chat, &ChatService::roomLeft);
        chat.sendMessage(QStringLiteral("Lobby"), QStringLiteral("/part"));
        QVERIFY(left.wait(500));
        QVERIFY(chat.joinedRooms().isEmpty());

        // browse goes to the overlay plugin with request ids preserved
        QSignalSpy br(&users, &UserService::browseResults);
        const quint32 bid = users.browse(bob);
        QCOMPARE(ov->browseCalls.size(), 1);
        QCOMPARE(ov->browseCalls.at(0).first, bid);
        emit ov->browseResults(bid, bob, {BrowseEntry{QStringLiteral("\\music\\a.mp3"), 1000, -1, -1, -1, {}}}, true);
        QCOMPARE(br.count(), 1);
        emit ov->browseResults(bid, bob, {}, true);      // after finished: ignored
        QCOMPARE(br.count(), 1);

        // persistence
        UserManager users2(&settings, &net, &dao);
        users2.start();
        QCOMPARE(users2.hotlist().size(), 1);
        QCOMPARE(users2.ignored().size(), 1);           // Mallory
        QCOMPARE(dao.favouriteRooms(), QStringList{QStringLiteral("Lobby")});
    }
};

QTEST_GUILESS_MAIN(TestChat)
#include "tst_chat.moc"
