// SPDX-License-Identifier: GPL-3.0-or-later
// [set-1] Behaviour tests of the settings the core reads: each test changes a setting the way the
// Settings dialog does (Settings::setValue) and checks what a user would see: which files every
// network is offered, which messages arrive, which results a search shows, what the log file holds.
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtTest/QSignalSpy>
#include <QtTest/QtTest>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

#include "core/chat_manager.h"
#include "core/dao.h"
#include "core/database.h"
#include "core/json_settings.h"
#include "core/library.h"
#include "core/log.h"
#include "core/network_manager.h"
#include "core/real_core.h"
#include "core/search_manager.h"
#include "core/upload_tracker.h"
#include "core/user_manager.h"
#include "fake_plugin.h"
#include "mx/services.h"
#include "mx/settings_keys.h"
#include "mx/simple_settings.h"

using namespace mx;
using mx::test::FakeChatPlugin;
using mx::test::FakePlugin;

namespace {

constexpr qint64 MB = 1024 * 1024;

void writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(data);
}

QStringList names(const QList<SharedFile> &l)
{
    QStringList out;
    for (const SharedFile &f : l) out << f.name;
    out.sort();
    return out;
}

SharedFile byName(const QList<SharedFile> &l, const QString &n)
{
    for (const SharedFile &f : l)
        if (f.name == n) return f;
    return {};
}

FileHashes ed2kHash(char c)
{
    FileHashes h;
    h.ed2k = QByteArray(16, c);
    return h;
}

// a chat network that records what OctoMX tells a refused sender
class RecordingChat : public FakeChatPlugin {
public:
    QList<QPair<QString, QString>> refused;   // sender name, reason
    void refusePrivateMessage(const ChatMessage &m, const QString &reason) override { refused << qMakePair(m.from.name, reason); }
};

QList<ChatMessage> msgs(const QSignalSpy &spy)
{
    QList<ChatMessage> out;
    for (const auto &a : spy) out << a.at(0).value<ChatMessage>();
    return out;
}

// a network with one TCP port open (what the Connection Test asks the public checker about)
class PortPlugin : public FakePlugin {
public:
    PortPlugin() : FakePlugin(Net::ED2K, NetworkPlugin::CanSearch) {}
    PluginDiagnostics diagnostics() const override
    {
        PluginDiagnostics d;
        d.valid = true;
        PortBinding b;
        b.proto = QStringLiteral("TCP");
        b.port = 4662;
        b.ok = true;
        d.bindings << b;
        return d;
    }
};

QString readAll(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

} // namespace

class TstSet1Core : public QObject {
    Q_OBJECT
    QTemporaryDir m_tmp;

private slots:
    void initTestCase()
    {
        registerMetaTypes();
        QVERIFY(m_tmp.isValid());
    }

    // ------------------------------------------------------------------ what is shared
    // library/excludePatterns, library/shareHidden, library/sharedTypes, library/shareUnlistedTypes,
    // transfers/shareCompleted, transfers/incomingDir: the files the networks are offered (the
    // list every plugin gets, and what they can find by hash and read) follow every rule, also for
    // a completed download hashed on request and for the hash cache read at start
    void sharingRulesOnEveryPath()
    {
        const QString root = m_tmp.filePath(QStringLiteral("share-rules"));
        const QString share = root + QStringLiteral("/share"), incoming = root + QStringLiteral("/incoming"),
                      incoming2 = root + QStringLiteral("/incoming2");
        writeFile(share + QStringLiteral("/keep.mp3"), QByteArray(4000, 'k'));
        writeFile(share + QStringLiteral("/skip.part"), QByteArray(4100, 'p'));            // default pattern *.part
        writeFile(share + QStringLiteral("/Thumbs.db"), QByteArray(4200, 't'));            // default pattern
        writeFile(share + QStringLiteral("/.dotfile.mp3"), QByteArray(4300, 'd'));         // hidden
        writeFile(share + QStringLiteral("/.hiddendir/inner.mp3"), QByteArray(4400, 'i')); // in a hidden folder
        writeFile(share + QStringLiteral("/private/confid.mp3"), QByteArray(4500, 's'));   // unshared folder
        writeFile(share + QStringLiteral("/prog.exe"), QByteArray(4600, 'e'));             // Software (unticked)
        writeFile(share + QStringLiteral("/notes.xyz"), QByteArray(4700, 'x'));            // an unlisted type
        writeFile(incoming + QStringLiteral("/done.mp3"), QByteArray(4800, 'c'));
        writeFile(incoming2 + QStringLiteral("/done2.mp3"), QByteArray(4900, 'C'));
        SimpleSettings settings;
        settings.setValue(keys::library::WatchFolders, false);
        QStringList types = sharedTypeIds();
        types.removeAll(QStringLiteral("software"));
        settings.setValue(keys::library::SharedTypes, types);
        settings.setValue(keys::library::ShareUnlistedTypes, false);
        settings.setValue(keys::transfers::IncomingDir, incoming);
        settings.setValue(keys::transfers::ShareCompleted, false);
        Database db;
        QVERIFY(db.open(root + QStringLiteral("/octomx.db")));
        auto lib = std::make_unique<Library>(&settings, &db);
        lib->addFolder(share, true);
        lib->excludeFolder(share + QStringLiteral("/private"), true);
        lib->start();
        QVERIFY(lib->waitIdle(30000));
        QCOMPARE(names(lib->sharedFiles()), QStringList{QStringLiteral("keep.mp3")});

        // Share hidden files: the dot file and the file in the hidden folder come in, and go again
        settings.setValue(keys::library::ShareHidden, true);
        QVERIFY(lib->waitIdle(30000));
        QCOMPARE(names(lib->sharedFiles()), (QStringList{QStringLiteral(".dotfile.mp3"), QStringLiteral("inner.mp3"), QStringLiteral("keep.mp3")}));
        const SharedFile dot = byName(lib->sharedFiles(), QStringLiteral(".dotfile.mp3"));
        settings.setValue(keys::library::ShareHidden, false);
        QVERIFY(lib->waitIdle(30000));
        QVERIFY(!lib->findByHash(dot.hashes, nullptr));
        QVERIFY(lib->read(dot.hashes, 0, 10).isEmpty());

        // an unlisted type is shared with "Share files of types not listed"
        settings.setValue(keys::library::ShareUnlistedTypes, true);
        QVERIFY(lib->waitIdle(30000));
        QVERIFY(names(lib->sharedFiles()).contains(QStringLiteral("notes.xyz")));
        QVERIFY(!names(lib->sharedFiles()).contains(QStringLiteral("prog.exe")));   // Software stays unticked
        settings.setValue(keys::library::ShareUnlistedTypes, false);
        QVERIFY(lib->waitIdle(30000));
        QVERIFY(!names(lib->sharedFiles()).contains(QStringLiteral("notes.xyz")));

        // Share completed downloads: the Incoming Files Folder, and only the one set now
        settings.setValue(keys::transfers::ShareCompleted, true);
        QVERIFY(lib->waitIdle(30000));
        QVERIFY(names(lib->sharedFiles()).contains(QStringLiteral("done.mp3")));
        settings.setValue(keys::transfers::IncomingDir, incoming2);
        QVERIFY(lib->waitIdle(30000));
        QVERIFY(names(lib->sharedFiles()).contains(QStringLiteral("done2.mp3")));
        QVERIFY(!names(lib->sharedFiles()).contains(QStringLiteral("done.mp3")));
        settings.setValue(keys::transfers::ShareCompleted, false);
        QVERIFY(lib->waitIdle(30000));
        QVERIFY(!names(lib->sharedFiles()).contains(QStringLiteral("done2.mp3")));

        // Never share files matching: a new pattern takes a shared file out of every answer
        const SharedFile keep = byName(lib->sharedFiles(), QStringLiteral("keep.mp3"));
        QVERIFY(keep.hashed);
        QVERIFY(!lib->read(keep.hashes, 0, 10).isEmpty());
        QSignalSpy pushed(lib.get(), &Library::sharedFilesUpdated);
        settings.setValue(keys::library::ExcludePatterns, settings.sl(keys::library::ExcludePatterns) << QStringLiteral("KEEP*"));
        QVERIFY(lib->waitIdle(30000));
        QVERIFY(lib->sharedFiles().isEmpty());
        QVERIFY(!lib->findByHash(keep.hashes, nullptr));
        QVERIFY(lib->read(keep.hashes, 0, 10).isEmpty());
        QVERIFY(pushed.wait(5000));
        QVERIFY(pushed.last().at(0).value<QList<SharedFile>>().isEmpty());   // what the plugins get

        // a file that arrives by itself (a completed download is hashed on request) obeys the same
        // rules: a pattern, a hidden name, a hidden folder
        settings.setValue(keys::transfers::ShareCompleted, true);
        writeFile(incoming2 + QStringLiteral("/late.tmp"), QByteArray(5000, 'l'));          // pattern *.tmp
        writeFile(incoming2 + QStringLiteral("/.late.mp3"), QByteArray(5100, 'h'));
        writeFile(incoming2 + QStringLiteral("/.hid/late2.mp3"), QByteArray(5200, 'H'));
        writeFile(incoming2 + QStringLiteral("/late3.mp3"), QByteArray(5300, 'L'));
        QVERIFY(lib->waitIdle(30000));
        QSignalSpy hashed(lib.get(), &Library::fileHashed);
        for (const char *n : {"/late.tmp", "/.late.mp3", "/.hid/late2.mp3", "/late3.mp3"})
            lib->requestHash(incoming2 + QLatin1String(n));
        QVERIFY(lib->waitIdle(30000));
        QTRY_VERIFY_WITH_TIMEOUT(hashed.count() >= 4, 10000);
        QCOMPARE(names(lib->sharedFiles()), (QStringList{QStringLiteral("done2.mp3"), QStringLiteral("late3.mp3")}));
        for (const auto &a : hashed) {
            const SharedFile f = a.at(0).value<SharedFile>();
            if (f.name != QLatin1String("late3.mp3"))
                QVERIFY2(!lib->findByHash(f.hashes, nullptr), qPrintable(f.name));
        }

        // the hash cache at the next start: a file excluded meanwhile is not offered for a moment
        // (it used to be until the first scan ended)
        lib->stop();
        lib.reset();
        settings.setValue(keys::library::ExcludePatterns, settings.sl(keys::library::ExcludePatterns) << QStringLiteral("done2*"));
        Library again(&settings, &db);
        again.start();
        QVERIFY(!names(again.sharedFiles()).contains(QStringLiteral("done2.mp3")));
        QVERIFY(names(again.sharedFiles()).contains(QStringLiteral("late3.mp3")));   // the cache is used
        QVERIFY(again.waitIdle(30000));
        QCOMPARE(names(again.sharedFiles()), QStringList{QStringLiteral("late3.mp3")});
        again.stop();
        db.close();
    }

    // every network gets the same list, and only through the core's library: an excluded file is
    // in no plugin's list and none can find it by hash or read it
    void everyNetworkGetsTheSameShareList()
    {
        const QString profile = m_tmp.filePath(QStringLiteral("share-core"));
        const QString share = m_tmp.filePath(QStringLiteral("share-core-files"));
        writeFile(share + QStringLiteral("/public.mp3"), QByteArray(6000, 'u'));
        writeFile(share + QStringLiteral("/hidden-by-rule.mp3"), QByteArray(6100, 'r'));
        QList<FakePlugin *> nets;
        for (Net n : {Net::ED2K, Net::Gnutella, Net::BitTorrent, Net::Http, Net::Overlay})
            nets << new FakePlugin(n, NetworkPlugin::CanSearch | NetworkPlugin::CanServe);
        QList<NetworkPlugin *> all;
        for (FakePlugin *p : nets) all << p;
        Core *core = createRealCore(profile, all);
        core->settings()->setValue(keys::library::WatchFolders, false);
        core->start();
        core->library()->addFolder(share, true);
        auto lastList = [](FakePlugin *p) { return p->sharedLists.isEmpty() ? QStringList{QStringLiteral("-")} : names(p->sharedLists.last()); };
        for (FakePlugin *p : nets)
            QTRY_COMPARE_WITH_TIMEOUT(lastList(p), (QStringList{QStringLiteral("hidden-by-rule.mp3"), QStringLiteral("public.mp3")}), 15000);
        const SharedFile confid = byName(nets.first()->sharedLists.last(), QStringLiteral("hidden-by-rule.mp3"));
        QVERIFY(!nets.first()->ctx.library->read(confid.hashes, 0, 10).isEmpty());
        core->settings()->setValue(keys::library::ExcludePatterns, QStringList{QStringLiteral("hidden-by-*")});
        for (FakePlugin *p : nets) {
            QTRY_COMPARE_WITH_TIMEOUT(lastList(p), QStringList{QStringLiteral("public.mp3")}, 15000);
            QVERIFY(!p->ctx.library->findByHash(confid.hashes, nullptr));
            QVERIFY(p->ctx.library->read(confid.hashes, 0, 10).isEmpty());
        }
        core->shutdown();
        delete core;
    }

    // library/watchFolders, library/rescanIntervalMin: a new file in a shared folder is offered at
    // once while the folders are watched, else at the next rescan (MX_TEST_MINUTE_MS shortens the
    // minute for this test only)
    void watchAndRescan()
    {
        const QString root = m_tmp.filePath(QStringLiteral("watch"));
        const QString share = root + QStringLiteral("/share");
        writeFile(share + QStringLiteral("/first.mp3"), QByteArray(3000, 'f'));
        SimpleSettings settings;
        settings.setValue(keys::library::WatchFolders, true);
        settings.setValue(keys::library::RescanIntervalMin, 0);
        Database db;
        QVERIFY(db.open(root + QStringLiteral("/octomx.db")));
        Library lib(&settings, &db);
        lib.addFolder(share, true);
        lib.start();
        QVERIFY(lib.waitIdle(30000));
        QCOMPARE(lib.fileCount(), 1);
        writeFile(share + QStringLiteral("/watched.mp3"), QByteArray(3100, 'w'));
        QTRY_COMPARE_WITH_TIMEOUT(lib.fileCount(), 2, 15000);   // the watcher (3 s debounce)

        settings.setValue(keys::library::WatchFolders, false);
        QVERIFY(lib.waitIdle(30000));
        writeFile(share + QStringLiteral("/unwatched.mp3"), QByteArray(3200, 'u'));
        QTest::qWait(5000);
        QCOMPARE(lib.fileCount(), 2);                            // nobody looks
        qputenv("MX_TEST_MINUTE_MS", "1500");
        settings.setValue(keys::library::RescanIntervalMin, 1);  // "every minute" = 1.5 s here
        QTRY_COMPARE_WITH_TIMEOUT(lib.fileCount(), 3, 15000);
        settings.setValue(keys::library::RescanIntervalMin, 0);   // 0 = never
        QVERIFY(lib.waitIdle(30000));
        writeFile(share + QStringLiteral("/never.mp3"), QByteArray(3300, 'n'));
        QTest::qWait(5000);
        QCOMPARE(lib.fileCount(), 3);
        qunsetenv("MX_TEST_MINUTE_MS");
        lib.stop();
        db.close();
    }

    // library/hashThreads: how many files are hashed at the same time
    void hashThreads()
    {
        const QString root = m_tmp.filePath(QStringLiteral("hashthreads"));
        for (int i = 0; i < 12; ++i)
            writeFile(root + QStringLiteral("/a/f%1.bin").arg(i), QByteArray(int(3 * MB), char('a' + i)));
        for (int i = 0; i < 12; ++i)
            writeFile(root + QStringLiteral("/b/g%1.bin").arg(i), QByteArray(int(3 * MB), char('A' + i)));
        SimpleSettings settings;
        settings.setValue(keys::library::WatchFolders, false);
        settings.setValue(keys::library::HashThreads, 1);
        Database db;
        QVERIFY(db.open(root + QStringLiteral("/octomx.db")));
        Library lib(&settings, &db);
        lib.addFolder(root + QStringLiteral("/a"), true);
        lib.start();
        QVERIFY(lib.waitIdle(60000));
        QCOMPARE(lib.fileCount(), 12);
        QCOMPARE(lib.peakParallelHashes(), 1);
        settings.setValue(keys::library::HashThreads, 4);
        lib.addFolder(root + QStringLiteral("/b"), true);
        QVERIFY(lib.waitIdle(60000));
        QCOMPARE(lib.fileCount(), 24);
        QVERIFY2(lib.peakParallelHashes() > 1, qPrintable(QString::number(lib.peakParallelHashes())));
        QVERIFY(lib.peakParallelHashes() <= 4);
        lib.stop();
        db.close();
    }

    // ------------------------------------------------------------------ who may write to me
    // messages/allowFrom + its four sources, messages/allowPrivateMessages, messages/ignoreNonHotlist,
    // messages/filterWords, messages/autoReplyText, the ignore list, chat/showJoinsParts: what the
    // user sees, and what the refused sender is told
    void messagePrivacy()
    {
        QTemporaryDir dir;
        Database db;
        QVERIFY(db.open(dir.filePath(QStringLiteral("octomx.db"))));
        UserListsDao dao(&db);
        SimpleSettings settings;
        settings.setValue(keys::identity::ScreenName, QStringLiteral("Me_12345"));
        settings.setValue(keys::messages::BlockFloods, false);
        auto *ov = new RecordingChat;
        const UserRef bob = FakeChatPlugin::user(QStringLiteral("Bob")), dan = FakeChatPlugin::user(QStringLiteral("Dan")),
                      ulla = FakeChatPlugin::user(QStringLiteral("Ulla")), hank = FakeChatPlugin::user(QStringLiteral("Hank")),
                      sam = FakeChatPlugin::user(QStringLiteral("Sam")), eve = FakeChatPlugin::user(QStringLiteral("Eve"));
        ov->users.insert(QStringLiteral("Lobby"), {ChatUser{sam, 1, QStringLiteral("DSL"), 0}, ChatUser{eve, 1, QStringLiteral("DSL"), 0}});
        NetworkManager net(&settings, {ov});
        UserManager users(&settings, &net, &dao);
        ChatManager chat(&settings, &net, &users, &dao);
        chat.setTransferPeerCheck([&](const UserRef &u, bool downloadingFrom) {
            return downloadingFrom ? u.key() == dan.key() : u.key() == ulla.key();
        });
        net.initPlugins(dir.path(), nullptr, nullptr);
        users.start();
        chat.start();
        users.addToHotlist(hank);
        QSignalSpy joined(&chat, &ChatService::roomJoined);
        net.connectAll();
        chat.joinRoom(QStringLiteral("Lobby"));
        QVERIFY(joined.wait(1000));
        QSignalSpy got(&chat, &ChatService::messageReceived);
        auto shown = [&](const QString &text) {
            for (const ChatMessage &m : msgs(got))
                if (m.text == text) return true;
            return false;
        };
        auto pm = [&](const UserRef &from, const QString &text) { ov->receive(QString(), from, text, ChatKind::Private); return shown(text); };

        // "Only allow messages from the following sources": each box lets its kind of user through
        settings.setValue(keys::messages::AllowFrom, QStringLiteral("restricted"));
        QVERIFY(pm(hank, QStringLiteral("from the hotlist")));
        QVERIFY(pm(dan, QStringLiteral("from my source")));
        QVERIFY(pm(ulla, QStringLiteral("from my downloader")));
        QVERIFY(pm(sam, QStringLiteral("from the room")));
        QVERIFY(!pm(bob, QStringLiteral("from a stranger")));
        QCOMPARE(ov->refused.last(), qMakePair(QStringLiteral("Bob"), QStringLiteral("restricted")));
        const struct { QLatin1StringView key; UserRef who; } boxes[] = {
            {keys::messages::AllowFromHotlist, hank}, {keys::messages::AllowFromDownloadingFrom, dan},
            {keys::messages::AllowFromUploadingTo, ulla}, {keys::messages::AllowFromSameRoom, sam}};
        int n = 0;
        for (const auto &b : boxes) {
            settings.setValue(b.key, false);
            const QString t = QStringLiteral("box off %1").arg(++n);
            QVERIFY2(!pm(b.who, t), qPrintable(QString(b.key)));
            QCOMPARE(ov->refused.last().first, b.who.name);
            settings.setValue(b.key, true);
            QVERIFY2(pm(b.who, t + QStringLiteral(" on")), qPrintable(QString(b.key)));
        }
        settings.setValue(keys::messages::AllowFrom, QStringLiteral("any"));
        QVERIFY(pm(bob, QStringLiteral("any source")));

        // Accept private messages from: Hotlist / Nobody (the sender is told why)
        settings.setValue(keys::messages::AllowPrivateMessages, QStringLiteral("hotlist"));
        QVERIFY(!pm(bob, QStringLiteral("not on the hotlist")));
        QCOMPARE(ov->refused.last().second, QStringLiteral("hotlist"));
        QVERIFY(pm(hank, QStringLiteral("on the hotlist")));
        settings.setValue(keys::messages::AllowPrivateMessages, QStringLiteral("nobody"));
        QVERIFY(!pm(hank, QStringLiteral("nobody at all")));
        QCOMPARE(ov->refused.last().second, QStringLiteral("nobody"));
        settings.setValue(keys::messages::AllowPrivateMessages, QStringLiteral("everyone"));

        // Ignore messages from users not on my Hotlist: private and chat room lines
        settings.setValue(keys::messages::IgnoreHotlistOnly, true);
        QVERIFY(!pm(bob, QStringLiteral("hotlist-only pm")));
        ov->receive(QStringLiteral("Lobby"), sam, QStringLiteral("hotlist-only room line"));
        QVERIFY(!shown(QStringLiteral("hotlist-only room line")));
        QVERIFY(pm(hank, QStringLiteral("hotlist-only pm from hank")));
        settings.setValue(keys::messages::IgnoreHotlistOnly, false);
        ov->receive(QStringLiteral("Lobby"), sam, QStringLiteral("room line again"));
        QVERIFY(shown(QStringLiteral("room line again")));

        // Filter messages containing: in rooms and private, any case
        settings.setValue(keys::messages::FilterWords, QStringList{QStringLiteral("spoiler"), QStringLiteral("buy now")});
        QVERIFY(!pm(bob, QStringLiteral("big SPOILER ahead")));
        ov->receive(QStringLiteral("Lobby"), sam, QStringLiteral("Buy Now cheap"));
        QVERIFY(!shown(QStringLiteral("Buy Now cheap")));
        QVERIFY(pm(bob, QStringLiteral("a harmless line")));
        settings.setValue(keys::messages::FilterWords, QStringList());
        QVERIFY(pm(bob, QStringLiteral("no spoiler filter now")));

        // the automatic reply sends the text set, once per sender in 10 minutes
        settings.setValue(keys::messages::AutoReply, true);
        settings.setValue(keys::messages::AutoReplyText, QStringLiteral("Out at the lake, back on Monday."));
        const int sent = ov->sentPrivate.size();
        QVERIFY(pm(dan, QStringLiteral("are you there")));
        QCOMPARE(ov->sentPrivate.size(), sent + 1);
        QCOMPARE(ov->sentPrivate.last().first.key(), dan.key());
        QCOMPARE(ov->sentPrivate.last().second, QStringLiteral("Out at the lake, back on Monday."));
        settings.setValue(keys::messages::AutoReply, false);
        QVERIFY(pm(sam, QStringLiteral("no reply wanted")));
        QCOMPARE(ov->sentPrivate.size(), sent + 1);

        // an ignored user: no message, no room line, no join line, no automatic reply, not told
        settings.setValue(keys::messages::AutoReply, true);
        users.ignore(eve, true);
        const int refusedBefore = ov->refused.size(), sentBefore = ov->sentPrivate.size();
        QVERIFY(!pm(eve, QStringLiteral("eve writes")));
        ov->receive(QStringLiteral("Lobby"), eve, QStringLiteral("eve in the room"));
        QVERIFY(!shown(QStringLiteral("eve in the room")));
        ov->receive(QStringLiteral("Lobby"), eve, QStringLiteral("Eve joined"), ChatKind::Join);
        QVERIFY(!shown(QStringLiteral("Eve joined")));
        QCOMPARE(ov->refused.size(), refusedBefore);
        QCOMPARE(ov->sentPrivate.size(), sentBefore);
        users.ignore(eve, false);
        QVERIFY(pm(eve, QStringLiteral("eve again")));
        settings.setValue(keys::messages::AutoReply, false);

        // Show joins and parts
        ov->receive(QStringLiteral("Lobby"), sam, QStringLiteral("Sam joined"), ChatKind::Join);
        QVERIFY(shown(QStringLiteral("Sam joined")));
        settings.setValue(keys::chat::ShowJoinsParts, false);
        ov->receive(QStringLiteral("Lobby"), sam, QStringLiteral("Sam left"), ChatKind::Part);
        QVERIFY(!shown(QStringLiteral("Sam left")));
    }

    // chat/backlogLines with chat/allowLargeBackscroll, chat/autoJoinFavourites, chat/defaultRoom
    void roomsAndBackscroll()
    {
        QTemporaryDir dir;
        Database db;
        QVERIFY(db.open(dir.filePath(QStringLiteral("octomx.db"))));
        UserListsDao dao(&db);
        SimpleSettings settings;
        settings.setValue(keys::messages::BlockFloods, false);
        settings.setValue(keys::chat::AutoJoinFavourites, false);
        settings.setValue(keys::chat::DefaultRoom, QStringLiteral("Big Buck Bunny fans"));
        auto *ov = new FakeChatPlugin;
        const UserRef bob = FakeChatPlugin::user(QStringLiteral("Bob"));
        NetworkManager net(&settings, {ov});
        UserManager users(&settings, &net, &dao);
        ChatManager chat(&settings, &net, &users, &dao);
        net.initPlugins(dir.path(), nullptr, nullptr);
        users.start();
        chat.start();
        chat.setFavouriteRoom(QStringLiteral("Debian"), true);
        QSignalSpy joined(&chat, &ChatService::roomJoined);
        net.connectAll();
        QVERIFY(joined.wait(1000));
        QCOMPARE(ov->joinCalls, QStringList{QStringLiteral("Big Buck Bunny fans")});   // favourites not joined
        // a new connect with "Join favourite rooms on connect"
        net.disconnectAll();
        settings.setValue(keys::chat::AutoJoinFavourites, true);
        settings.setValue(keys::chat::DefaultRoom, QString());
        net.connectAll();
        QTRY_VERIFY_WITH_TIMEOUT(ov->joinCalls.contains(QStringLiteral("Debian")), 3000);
        QCOMPARE(ov->joinCalls.count(QStringLiteral("Big Buck Bunny fans")), 1);   // no default room any more

        const QString room = QStringLiteral("Big Buck Bunny fans");
        settings.setValue(keys::chat::BacklogLines, 3000);
        settings.setValue(keys::chat::AllowLargeBackscroll, false);
        for (int i = 0; i < 1500; ++i) ov->receive(room, bob, QStringLiteral("line %1").arg(i));
        QCOMPARE(chat.backlog(room).size(), 1000);   // above 1000 only with the box
        settings.setValue(keys::chat::AllowLargeBackscroll, true);
        for (int i = 0; i < 2500; ++i) ov->receive(room, bob, QStringLiteral("more %1").arg(i));
        QCOMPARE(chat.backlog(room).size(), 3000);
        settings.setValue(keys::chat::BacklogLines, 200);
        ov->receive(room, bob, QStringLiteral("last"));
        QCOMPARE(chat.backlog(room).size(), 200);
        QCOMPARE(chat.backlog(room).last().text, QStringLiteral("last"));
    }

    // ------------------------------------------------------------------ what a search shows
    // search/maxResults, search/networks, search/fakeFilter, search/blockedExtensions,
    // search/adultFilter (with the word list of the Web Search page for every network),
    // search/saveHistory, search/historySize, search/mergeByNameAndSize, search/minCandidateSize
    void searchResults()
    {
        QTemporaryDir dir;
        Database db;
        QVERIFY(db.open(dir.filePath(QStringLiteral("octomx.db"))));
        UserListsDao dao(&db);
        SimpleSettings settings;
        auto *ed = new FakePlugin(Net::ED2K, NetworkPlugin::CanSearch);
        auto *gn = new FakePlugin(Net::Gnutella, NetworkPlugin::CanSearch);
        for (int i = 0; i < 90; ++i)
            ed->cannedHits << FakePlugin::makeHit(QStringLiteral("ubuntu part %1.iso").arg(i), 700 * MB + i, ed2kHash(char(i + 1)), Net::ED2K, QStringLiteral("u"));
        gn->cannedHits << FakePlugin::makeHit(QStringLiteral("debian netinst.iso"), 600 * MB, FileHashes(), Net::Gnutella, QStringLiteral("g"));
        NetworkManager net(&settings, {ed, gn});
        net.initPlugins(dir.path(), nullptr, nullptr);
        net.connectAll();
        SearchManager sm(&settings, &net, &dao);
        QSignalSpy done(&sm, &SearchService::searchFinished);
        auto run = [&](const QString &text) {
            SearchQuery q;
            q.text = text;
            const quint32 id = sm.startSearch(q);
            const int before = done.count();
            [&] { QTRY_VERIFY_WITH_TIMEOUT(done.count() > before, 5000); }();
            return id;
        };
        settings.setValue(keys::search::MaxResults, 60);
        quint32 id = run(QStringLiteral("ubuntu"));
        QCOMPARE(sm.hits(id).size(), 60);
        settings.setValue(keys::search::MaxResults, 2000);
        id = run(QStringLiteral("ubuntu"));
        QCOMPARE(sm.hits(id).size(), 91);

        // Networks to search: by the names the page lists
        const int edBefore = ed->searches.size(), gnBefore = gn->searches.size();
        settings.setValue(keys::search::Networks, QStringList{QStringLiteral("Gnutella")});
        id = run(QStringLiteral("debian"));
        QCOMPARE(ed->searches.size(), edBefore);
        QCOMPARE(gn->searches.size(), gnBefore + 1);
        settings.setValue(keys::search::Networks, QStringList());
        for (const char *name : {"Overlay", "BitTorrent", "eD2k", "Kad", "Gnutella", "HTTP", "Web", "Rats"})
            QVERIFY2(netFromName(QString::fromLatin1(name)) != Net::Unknown, name);   // the page's list of names

        // the result filters
        SearchQuery any;
        settings.setValue(keys::search::BlockedExtensions, QStringList());   // .exe is in the default list
        const SearchHit fake = FakePlugin::makeHit(QStringLiteral("song.mp3.exe"), 3 * MB);
        QVERIFY(!sm.acceptHit(any, fake));
        settings.setValue(keys::search::FakeFilter, false);
        QVERIFY(sm.acceptHit(any, fake));
        settings.setValue(keys::search::FakeFilter, true);
        const SearchHit vbs = FakePlugin::makeHit(QStringLiteral("readme.vbs"), 3 * MB);
        QVERIFY(sm.acceptHit(any, vbs));
        settings.setValue(keys::search::BlockedExtensions, QStringList{QStringLiteral(".VBS")});
        QVERIFY(!sm.acceptHit(any, vbs));
        const SearchHit adult = FakePlugin::makeHit(QStringLiteral("hot xxx clip.avi"), 3 * MB);
        const SearchHit custom = FakePlugin::makeHit(QStringLiteral("bunny sauna night.avi"), 3 * MB);
        QVERIFY(sm.acceptHit(any, adult));
        settings.setValue(keys::search::AdultFilter, true);
        QVERIFY(!sm.acceptHit(any, adult));
        QVERIFY(sm.acceptHit(any, custom));
        settings.setValue(keys::web::AdultKeywords, QStringList{QStringLiteral("Sauna")});   // Web Search page list
        QVERIFY(!sm.acceptHit(any, custom));
        QVERIFY(sm.acceptHit(any, adult));                       // the list replaces the built-in one
        settings.setValue(keys::search::AdultFilter, false);
        QVERIFY(sm.acceptHit(any, custom));

        // history: remembered, how many, and not at all
        settings.setValue(keys::search::HistorySize, 3);
        for (const char *t : {"one", "two", "three", "four"}) run(QString::fromLatin1(t));
        QCOMPARE(sm.history(), (QStringList{QStringLiteral("four"), QStringLiteral("three"), QStringLiteral("two")}));
        SearchManager again(&settings, &net, &dao);   // also after a restart
        QCOMPARE(again.history().first(), QStringLiteral("four"));
        settings.setValue(keys::search::SaveHistory, false);
        run(QStringLiteral("five"));
        QCOMPARE(sm.history().first(), QStringLiteral("four"));

        // merging by name and size: two networks, no common hash. On: nearly the same name is
        // enough; off: only exactly the same name (the classic rule); never below the minimum size
        auto twoHits = [&](const QString &second) {
            ed->cannedHits = {FakePlugin::makeHit(QStringLiteral("Big Buck Bunny 1080p.mkv"), 276 * MB, ed2kHash('B'), Net::ED2K, QStringLiteral("e"))};
            gn->cannedHits = {FakePlugin::makeHit(second, 276 * MB, FileHashes(), Net::Gnutella, QStringLiteral("g"))};
            return int(sm.hits(run(QStringLiteral("bunny"))).size());
        };
        settings.setValue(keys::search::MergeByNameAndSize, true);
        QCOMPARE(twoHits(QStringLiteral("Big Buck Bunny 1080p.mkv")), 1);
        QCOMPARE(twoHits(QStringLiteral("Big.Buck.Bunny.1080p.x264.mkv")), 1);
        settings.setValue(keys::search::MergeByNameAndSize, false);
        QCOMPARE(twoHits(QStringLiteral("Big Buck Bunny 1080p.mkv")), 1);
        QCOMPARE(twoHits(QStringLiteral("Big.Buck.Bunny.1080p.x264.mkv")), 2);
        settings.setValue(keys::search::MinCandidateSize, 300 * MB);   // above the file: never by name
        QCOMPARE(twoHits(QStringLiteral("Big Buck Bunny 1080p.mkv")), 2);
        settings.setValue(keys::search::MergeByNameAndSize, true);
        QCOMPARE(twoHits(QStringLiteral("Big Buck Bunny 1080p.mkv")), 2);
    }

    // ------------------------------------------------------------------ the log file
    // misc/logLevel (what each level writes), misc/logMaxSizeMB, misc/logKeepFiles
    void logFile()
    {
        const QString profile = m_tmp.filePath(QStringLiteral("log-core"));
        QDir().mkpath(profile);
        installLogHandler(profile, 5 * MB, 5, false, 1);
        Core *core = createRealCore(profile, {});
        Settings *s = core->settings();
        const QString path = profile + QStringLiteral("/logs/octomx.log");
        auto lines = [&](const char *marker) { qCDebug(lcCore, "%s debug", marker); qCInfo(lcCore, "%s info", marker); qCWarning(lcCore, "%s warning", marker); };
        s->setValue(keys::misc::LogLevel, QStringLiteral("warning"));
        lines("m-warn");
        s->setValue(keys::misc::LogLevel, QStringLiteral("info"));
        lines("m-info");
        s->setValue(keys::misc::LogLevel, QStringLiteral("debug"));
        lines("m-debug");
        const QString log = readAll(path);
        QVERIFY(!log.contains(QStringLiteral("m-warn debug")) && !log.contains(QStringLiteral("m-warn info")));
        QVERIFY(log.contains(QStringLiteral("m-warn warning")));
        QVERIFY(!log.contains(QStringLiteral("m-info debug")) && log.contains(QStringLiteral("m-info info")));
        QVERIFY(log.contains(QStringLiteral("m-debug debug")) && log.contains(QStringLiteral("m-debug info")));

        // size limit and old files kept
        s->setValue(keys::misc::LogLevel, QStringLiteral("info"));
        s->setValue(keys::misc::LogKeepFiles, 2);
        s->setValue(keys::misc::LogMaxSizeMB, 1);
        const QByteArray filler(1000, 'z');
        for (int i = 0; i < 4500; ++i) qCInfo(lcCore, "fill %d %s", i, filler.constData());
        const QString base = profile + QStringLiteral("/logs/octomx");
        QVERIFY(QFileInfo(path).size() <= 1 * MB + 2000);
        QVERIFY(QFile::exists(base + QStringLiteral(".1.log")));
        QVERIFY(QFile::exists(base + QStringLiteral(".2.log")));
        QVERIFY(!QFile::exists(base + QStringLiteral(".3.log")));   // only two old files kept
        QVERIFY(QFileInfo(base + QStringLiteral(".1.log")).size() > 900 * 1024);
        uninstallLogHandler();
        delete core;
    }

    // diag/portCheckUrl: the address the Connection Test's "Check ... online" asks (and nothing else
    // asks it); empty = no request at all
    void portCheckAddress()
    {
        QTcpServer checker;
        QVERIFY(checker.listen(QHostAddress::LocalHost, 0));
        QStringList asked;
        connect(&checker, &QTcpServer::newConnection, this, [&] {
            while (QTcpSocket *sock = checker.nextPendingConnection()) {
                connect(sock, &QTcpSocket::readyRead, sock, [&asked, sock] {
                    const QByteArray req = sock->readAll();
                    asked << QString::fromLatin1(req.left(req.indexOf('\r')));
                    sock->write("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 1\r\nConnection: close\r\n\r\n1");
                    sock->disconnectFromHost();
                });
            }
        });
        const QString profile = m_tmp.filePath(QStringLiteral("portcheck"));
        auto *p = new PortPlugin;
        Core *core = createRealCore(profile, {p});
        core->settings()->setValue(keys::connection::Upnp, false);
        core->settings()->setValue(keys::connection::NatPmp, false);
        core->settings()->setValue(keys::diag::PortCheckUrl, QStringLiteral("http://127.0.0.1:%1/check/%2").arg(checker.serverPort()).arg(QStringLiteral("%1")));
        core->start();
        QTest::qWait(500);
        QVERIFY(asked.isEmpty());                                  // nothing at start
        QCOMPARE(core->diagnostics()->portCheckService(), QStringLiteral("127.0.0.1"));
        core->diagnostics()->checkPortsOnline();
        QTRY_VERIFY_WITH_TIMEOUT(!asked.isEmpty(), 10000);
        QCOMPARE(asked.first(), QStringLiteral("GET /check/4662 HTTP/1.1"));
        asked.clear();
        core->settings()->setValue(keys::diag::PortCheckUrl, QString());
        QVERIFY(core->diagnostics()->portCheckService().isEmpty());
        QTest::qWait(16000);                                       // the first check is over
        core->diagnostics()->checkPortsOnline();
        QTest::qWait(2000);
        QVERIFY(asked.isEmpty());
        core->shutdown();
        delete core;
    }

    // transfers/autoClearUploads, transfers/clearCompleteUploadsSec: a finished upload leaves the list
    void finishedUploadsCleared()
    {
        SimpleSettings settings;
        settings.setValue(keys::transfers::AutoClearUploads, true);
        settings.setValue(keys::transfers::ClearCompleteUploadsSec, 1);
        UploadTracker up(&settings);
        auto *p = new FakePlugin(Net::Overlay, NetworkPlugin::CanServe);
        up.attach(p);
        auto send = [&](const QString &id, TransferState st) {
            TransferInfo t;
            t.id = id;
            t.name = QStringLiteral("Big Buck Bunny.mkv");
            t.isUpload = true;
            t.size = 1000;
            t.done = st == TransferState::Completed ? 1000 : 500;
            t.state = st;
            p->emitUpload(t);
        };
        auto listed = [&](const QString &id) {
            for (const TransferInfo &t : up.uploads())
                if (t.id.endsWith(id)) return true;
            return false;
        };
        send(QStringLiteral("u1"), TransferState::Transferring);
        send(QStringLiteral("u1"), TransferState::Completed);
        p->emitUploadFinished(QStringLiteral("u1"));
        QVERIFY(listed(QStringLiteral("u1")));
        QTRY_VERIFY_WITH_TIMEOUT(!listed(QStringLiteral("u1")), 4000);
        settings.setValue(keys::transfers::AutoClearUploads, false);
        send(QStringLiteral("u2"), TransferState::Transferring);
        send(QStringLiteral("u2"), TransferState::Completed);
        p->emitUploadFinished(QStringLiteral("u2"));
        QTest::qWait(2500);
        QVERIFY(listed(QStringLiteral("u2")));
        delete p;
    }

    // ------------------------------------------------------------------ the settings file
    // a damaged settings file keeps every entry that can still be read (it used to start from the
    // defaults), and the last good file is kept as a backup
    void damagedSettingsFile()
    {
        const QString dir = m_tmp.filePath(QStringLiteral("damaged"));
        QDir().mkpath(dir);
        const QString path = dir + QStringLiteral("/settings.json");
        {
            JsonSettings s(path);
            QVERIFY(s.load());
            s.setValue(keys::identity::ScreenName, QStringLiteral("Bunny_12345"));
            s.setValue(keys::transfers::MaxUploads, 7);
            s.setValue(keys::library::ExcludePatterns, QStringList{QStringLiteral("*.confid"), QStringLiteral("diary*")});
            s.setValue(keys::colors::ListBackground, QStringLiteral("#102030"));
            s.setValue(keys::messages::AllowPrivateMessages, QStringLiteral("hotlist"));
            s.sync();
        }
        {   // a second start keeps a backup of the good file
            JsonSettings s(path);
            QVERIFY(s.load());
        }
        QVERIFY(QFile::exists(path + QStringLiteral(".bak")));
        // break one entry in the middle and cut the end off
        QString text = readAll(path);
        QVERIFY(text.contains(QStringLiteral("\"transfers/maxUploads\": 7")));
        text.replace(QStringLiteral("\"transfers/maxUploads\": 7"), QStringLiteral("\"transfers/maxUploads\": 7}}{"));
        text.chop(text.size() - text.lastIndexOf(QLatin1Char(',')) - 1);
        {
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(text.toUtf8());
        }
        JsonSettings s(path);
        QString error;
        QVERIFY(!s.load(&error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(s.s(keys::identity::ScreenName), QStringLiteral("Bunny_12345"));
        QCOMPARE(s.sl(keys::library::ExcludePatterns), (QStringList{QStringLiteral("*.confid"), QStringLiteral("diary*")}));
        QCOMPARE(s.s(keys::colors::ListBackground), QStringLiteral("#102030"));
        QCOMPARE(s.s(keys::messages::AllowPrivateMessages), QStringLiteral("hotlist"));
        QCOMPARE(s.i(keys::transfers::MaxUploads), 7);                   // from the backup
        QVERIFY(QFile::exists(path + QStringLiteral(".bad")));            // the damaged file is kept
    }
};

QTEST_GUILESS_MAIN(TstSet1Core)
#include "tst_set1_core.moc"
