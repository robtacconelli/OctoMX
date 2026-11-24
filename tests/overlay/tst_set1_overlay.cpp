// SPDX-License-Identifier: GPL-3.0-or-later
// [set-1] Settings checked between real OctoMX Peer Network nodes (127.0.0.1): what another user can
// find of my files by search, by browsing and by asking for a hash when the core's Library leaves a
// file out; the connection type others see; the upload queue rules; what the log file keeps of a
// private message.
#include <QtCore/QTemporaryDir>
#include <QtTest/QSignalSpy>
#include <QtTest/QtTest>

#include "core/database.h"
#include "core/library.h"
#include "core/log.h"
#include "ovl_test_support.h"

using namespace mx::ovl;
using namespace ovltest;

namespace {

// production timings: presence once a minute, room heartbeat every 30 s
OverlayTimings slowTimings()
{
    OverlayTimings t;
    t.presenceMs = 60000;
    t.heartbeatMs = 30000;
    t.memberTimeoutMs = 95000;
    t.directoryMs = 1000;
    t.historyDelayMs = 1200;
    t.searchQuietMs = 2500;
    t.searchMaxMs = 8000;
    t.dialWaitMs = 4000;
    return t;
}

std::unique_ptr<Instance> node(const QString &name, const OverlayTimings &t)
{
    auto i = std::make_unique<Instance>(name);
    OverlayPlugin::TestOptions to;
    to.dht = to.mdns = to.reconnect = to.pex = false;
    i->plugin->setTestOptions(to);
    i->plugin->setTimings(t);
    return i;
}

// an overlay node whose files come from the core's Library (as in OctoMX), not from a test list
struct CoreNode {
    QString profile = freshDir(QStringLiteral("set1-core"));
    mx::SimpleSettings settings;
    mx::Database db;
    std::unique_ptr<mx::Library> lib;
    OverlayPlugin *plugin = nullptr;
    explicit CoreNode(const QString &name)
    {
        settings.setValue(QString(mx::keys::identity::ScreenName), name);
        settings.setValue(QString(mx::keys::library::WatchFolders), false);
        QVERIFY(db.open(profile + QStringLiteral("/octomx.db")));
        lib = std::make_unique<mx::Library>(&settings, &db);
        mx::PluginContext ctx;
        ctx.settings = &settings;
        ctx.library = lib.get();
        ctx.profileDir = profile;
        ctx.dataDir = profile + QStringLiteral("/overlay");
        ctx.screenName = name;
        ctx.clientName = QStringLiteral("OctoMX");
        ctx.clientVersion = QStringLiteral("5.4-test");
        plugin = static_cast<OverlayPlugin *>(mx::createOverlayPlugin());
        OverlayPlugin::TestOptions to;
        to.dht = to.mdns = to.reconnect = to.pex = false;
        plugin->setTestOptions(to);
        OverlayTimings t = slowTimings();
        t.heartbeatMs = 1000;
        t.memberTimeoutMs = 3500;
        plugin->setTimings(t);
        plugin->init(ctx);
        // as the core's NetworkManager does
        QObject::connect(lib.get(), &mx::Library::sharedFilesUpdated, plugin, [this](const QList<mx::SharedFile> &f) { plugin->setSharedFiles(f); });
    }
    ~CoreNode()
    {
        plugin->stop();
        delete plugin;
        lib->stop();
        lib.reset();
        db.close();
        RatsNode::waitAllStopped(5000);
        QDir(profile).removeRecursively();
    }
    QString addr() const { return QStringLiteral("127.0.0.1:%1").arg(plugin->listenPort()); }
    mx::UserRef me() const { return plugin->localUser(); }
};

bool linkTo(CoreNode &a, Instance &b)
{
    const int na = a.plugin->connectedUsers(), nb = b.plugin->connectedUsers();
    b.plugin->connectPeer(a.addr());
    return waitFor([&] { return a.plugin->connectedUsers() > na && b.plugin->connectedUsers() > nb; }, 10000);
}

mx::SearchSource sourceOf(const mx::UserRef &u, const QString &nodeId)
{
    mx::SearchSource s;
    s.net = mx::Net::Overlay;
    s.user = u;
    s.extra.insert(QStringLiteral("node"), nodeId);
    return s;
}

void writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(data);
}

QStringList hitNames(const QSignalSpy &spy, quint32 id)
{
    QStringList out;
    for (const auto &a : spy)
        if (a.at(0).toUInt() == id)
            for (const mx::SearchHit &h : a.at(1).value<QList<mx::SearchHit>>()) out << h.fileName;
    out.sort();
    out.removeDuplicates();
    return out;
}

QString memberConn(Instance &i, const QString &room, const QString &userId)
{
    for (const auto &u : i.chat->roomUsers(room))
        if (u.user.id == userId) return u.connection;
    return QString();
}

bool sawPresence(const QSignalSpy &spy, const QString &userId, const QString &conn)
{
    for (const auto &a : spy)
        if (a.at(0).value<mx::UserRef>().id == userId && a.at(3).toString() == conn) return true;
    return false;
}

} // namespace

class TstSet1Overlay : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { mx::registerMetaTypes(); }

    // library/excludePatterns, library/shareHidden, library/sharedTypes: a file the Library leaves out
    // cannot be found by another user by search, by browsing, nor fetched by asking for its hash
    void leftOutFilesStayOutOfTheNetwork()
    {
        CoreNode A(QStringLiteral("Alice"));
        auto B = node(QStringLiteral("Bob"), slowTimings());
        const QString share = A.profile + QStringLiteral("/share");
        writeFile(share + QStringLiteral("/bunny public.mkv"), QByteArray(200000, 'p'));
        writeFile(share + QStringLiteral("/bunny confid.mkv"), QByteArray(210000, 's'));
        writeFile(share + QStringLiteral("/.bunny hidden.mkv"), QByteArray(220000, 'h'));
        writeFile(share + QStringLiteral("/bunny tool.exe"), QByteArray(230000, 'e'));
        A.settings.setValue(QString(mx::keys::library::ShareHidden), true);
        A.lib->addFolder(share, true);
        A.lib->start();
        QVERIFY(A.lib->waitIdle(30000));
        QCOMPARE(A.lib->sharedFiles().size(), 4);
        A.plugin->start();
        B->start();
        QVERIFY(linkTo(A, *B));
        A.plugin->setSharedFiles(A.lib->sharedFiles());
        QHash<QString, mx::SharedFile> byName;
        for (const mx::SharedFile &f : A.lib->sharedFiles()) byName.insert(f.name, f);

        QSignalSpy hits(B->plugin, &mx::NetworkPlugin::searchHits), done(B->plugin, &mx::NetworkPlugin::searchDone);
        auto search = [&](quint32 id) {
            mx::SearchQuery q;
            q.id = id;
            q.text = QStringLiteral("bunny");
            const int before = done.count();
            B->plugin->search(q);
            return waitFor([&] { return done.count() > before; }, 15000) ? hitNames(hits, id) : QStringList{QStringLiteral("timeout")};
        };
        QCOMPARE(search(1).size(), 4);   // everything first: the search works

        // the rules: a pattern, hidden files off, Software unticked
        A.settings.setValue(QString(mx::keys::library::ExcludePatterns), QStringList{QStringLiteral("*confid*")});
        A.settings.setValue(QString(mx::keys::library::ShareHidden), false);
        QStringList types = mx::sharedTypeIds();
        types.removeAll(QStringLiteral("software"));
        A.settings.setValue(QString(mx::keys::library::SharedTypes), types);
        QVERIFY(A.lib->waitIdle(30000));
        QTRY_COMPARE_WITH_TIMEOUT(A.lib->sharedFiles().size(), 1, 10000);
        QSignalSpy pushed(A.lib.get(), &mx::Library::sharedFilesUpdated);
        QVERIFY(pushed.wait(5000));   // the plugin got the new list
        QCOMPARE(search(2), QStringList{QStringLiteral("bunny public.mkv")});

        QSignalSpy res(B->plugin, &mx::NetworkPlugin::browseResults);
        B->plugin->browse(3, A.me());
        QVERIFY(waitFor([&] { return !res.isEmpty() && res.last().at(3).toBool(); }, 15000));
        QStringList browsed;
        for (const auto &a : res)
            for (const mx::BrowseEntry &e : a.at(2).value<QList<mx::BrowseEntry>>()) browsed << QFileInfo(QString(e.path).replace(QLatin1Char('\\'), QLatin1Char('/'))).fileName();
        QCOMPARE(browsed, QStringList{QStringLiteral("bunny public.mkv")});

        // asked by hash (a link someone kept): refused for the three, served for the public one
        for (const QString &n : {QStringLiteral("bunny confid.mkv"), QStringLiteral(".bunny hidden.mkv"), QStringLiteral("bunny tool.exe"),
                                 QStringLiteral("bunny public.mkv")}) {
            const mx::SharedFile f = byName.value(n);
            mx::FileHashes h;
            h.sha1 = f.hashes.sha1;
            h.ed2k = f.hashes.ed2k;
            std::unique_ptr<mx::RangeSource> src(B->plugin->openSource(QStringLiteral("dl-") + n, h, f.size, sourceOf(A.me(), A.plugin->nodeId())));
            QVERIFY(src);
            src->open();
            const bool shared = n == QLatin1String("bunny public.mkv");
            QVERIFY2(waitFor([&] { return src->state() == (shared ? mx::RangeSource::Ready : mx::RangeSource::Failed); }, 10000),
                     qPrintable(n + QStringLiteral(": ") + src->stateText()));
            src->close();
        }
    }

    // identity/connection, queue/changeSpeedWhenBusy, queue/changeSpeedThreshold, queue/changedSpeed,
    // identity/screenName: what other users see changes at once, in the room lists and the Hotlist
    // presence (with the production timings: presence every minute, room heartbeat every 30 s)
    void othersSeeTheConnectionTypeAtOnce()
    {
        auto A = node(QStringLiteral("Alice"), slowTimings()), B = node(QStringLiteral("Bob"), slowTimings());
        A->start();
        B->start();
        QVERIFY(link(*A, *B));
        const QString room = QStringLiteral("Big Buck Bunny");
        A->chat->joinRoom(room);
        B->chat->joinRoom(room);
        QVERIFY(waitFor([&] { return !memberConn(*A, room, B->me().id).isEmpty(); }, 10000));
        QCOMPARE(memberConn(*A, room, B->me().id), QStringLiteral("DSL"));
        QSignalSpy pres(A->events, &mx::ChatEvents::userPresence);
        B->settings.setValue(QString(mx::keys::identity::Connection), QStringLiteral("FTTH"));
        QVERIFY(waitFor([&] { return memberConn(*A, room, B->me().id) == QLatin1String("FTTH"); }, 4000));
        QVERIFY(waitFor([&] { return sawPresence(pres, B->me().id, QStringLiteral("FTTH")); }, 4000));

        // busy: another type while this many uploads run
        const mx::SharedFile f = B->library.addGenerated(B->profile, QStringLiteral("busy.bin"), 400000, 21);
        B->setShared();
        B->settings.setValue(QString(mx::keys::queue::ChangeSpeedWhenBusy), true);
        B->settings.setValue(QString(mx::keys::queue::ChangeSpeedThreshold), 1);
        B->settings.setValue(QString(mx::keys::queue::ChangedSpeed), QStringLiteral("4G"));
        mx::FileHashes h;
        h.sha1 = f.hashes.sha1;
        std::unique_ptr<mx::RangeSource> src(A->plugin->openSource(QStringLiteral("busy"), h, f.size, sourceOf(B->me(), B->plugin->nodeId())));
        src->open();
        QVERIFY(waitFor([&] { return src->state() == mx::RangeSource::Ready; }, 10000));
        QVERIFY(waitFor([&] { return memberConn(*A, room, B->me().id) == QLatin1String("4G"); }, 4000));
        QVERIFY(waitFor([&] { return sawPresence(pres, B->me().id, QStringLiteral("4G")); }, 4000));
        src->close();
        src.reset();
        QVERIFY(waitFor([&] { return memberConn(*A, room, B->me().id) == QLatin1String("FTTH"); }, 6000));
        // two uploads needed: one is not enough
        B->settings.setValue(QString(mx::keys::queue::ChangeSpeedThreshold), 2);
        std::unique_ptr<mx::RangeSource> one(A->plugin->openSource(QStringLiteral("busy2"), h, f.size, sourceOf(B->me(), B->plugin->nodeId())));
        one->open();
        QVERIFY(waitFor([&] { return one->state() == mx::RangeSource::Ready; }, 10000));
        QTest::qWait(2500);
        QCOMPARE(memberConn(*A, room, B->me().id), QStringLiteral("FTTH"));
        one->close();

        // a new screen name reaches the room at once too
        B->settings.setValue(QString(mx::keys::identity::ScreenName), QStringLiteral("Robert"));
        QVERIFY(waitFor([&] {
            for (const auto &u : A->chat->roomUsers(room))
                if (u.user.id == B->me().id) return u.user.name.startsWith(QLatin1String("Robert_"));
            return false;
        }, 4000));
    }

    // transfers/maxUploads, transfers/maxUploadsPerUser, queue/enabled, queue/maxLength,
    // queue/maxPerUser, queue/sendPosition, queue/smallFileBypassKB
    void uploadSlotsAndQueue()
    {
        OverlayTimings t = slowTimings();
        auto A = node(QStringLiteral("Alice"), t), B = node(QStringLiteral("Bob"), t), C = node(QStringLiteral("Carol"), t);
        for (Instance *i : {A.get(), B.get(), C.get()}) i->start();
        QVERIFY(link(*A, *B));
        QVERIFY(link(*C, *B));
        const mx::SharedFile big1 = B->library.addGenerated(B->profile, QStringLiteral("big1.bin"), 600000, 31);
        const mx::SharedFile big2 = B->library.addGenerated(B->profile, QStringLiteral("big2.bin"), 600000, 32);
        const mx::SharedFile big3 = B->library.addGenerated(B->profile, QStringLiteral("big3.bin"), 600000, 33);
        const mx::SharedFile small = B->library.addGenerated(B->profile, QStringLiteral("small.bin"), 50000, 34);
        B->setShared();
        B->settings.setValue(QString(mx::keys::transfers::MaxUploads), 1);
        B->settings.setValue(QString(mx::keys::transfers::MaxUploadsPerUser), 1);
        B->settings.setValue(QString(mx::keys::queue::SmallFileBypassKB), 0);
        int n = 0;
        auto open = [&](Instance &from, const mx::SharedFile &f) {
            mx::FileHashes h;
            h.sha1 = f.hashes.sha1;
            auto *s = from.plugin->openSource(QStringLiteral("q%1").arg(++n), h, f.size, sourceOf(B->me(), B->plugin->nodeId()));
            s->open();
            return std::unique_ptr<mx::RangeSource>(s);
        };
        auto settled = [](mx::RangeSource *s) {
            return waitFor([&] { return s->state() == mx::RangeSource::Ready || s->state() == mx::RangeSource::Queued
                                        || s->state() == mx::RangeSource::Failed; }, 10000);
        };
        auto a1 = open(*A, big1);
        QVERIFY(settled(a1.get()));
        QCOMPARE(a1->state(), mx::RangeSource::Ready);          // the one slot
        auto c1 = open(*C, big2);
        QVERIFY(settled(c1.get()));
        QCOMPARE(c1->state(), mx::RangeSource::Queued);         // Max total uploads 1
        QCOMPARE(c1->stateText(), QStringLiteral("Remotely Queued (1)"));
        // Files below this skip the queue
        B->settings.setValue(QString(mx::keys::queue::SmallFileBypassKB), 100);
        auto c2 = open(*C, small);
        QVERIFY(settled(c2.get()));
        QCOMPARE(c2->state(), mx::RangeSource::Ready);
        c2->close();
        B->settings.setValue(QString(mx::keys::queue::SmallFileBypassKB), 0);
        // Tell users their queue position: off
        B->settings.setValue(QString(mx::keys::queue::SendPosition), false);
        auto a2 = open(*A, big3);
        QVERIFY(settled(a2.get()));
        QCOMPARE(a2->state(), mx::RangeSource::Queued);
        QCOMPARE(a2->stateText(), QStringLiteral("Remotely Queued"));
        a2->close();
        a2.reset();
        B->settings.setValue(QString(mx::keys::queue::SendPosition), true);
        // Maximum queued files per user: Carol has one in the queue
        B->settings.setValue(QString(mx::keys::queue::MaxPerUser), 1);
        auto c3 = open(*C, big3);
        QVERIFY(settled(c3.get()));
        QCOMPARE(c3->state(), mx::RangeSource::Failed);
        c3->close();
        B->settings.setValue(QString(mx::keys::queue::MaxPerUser), 10);
        // Maximum queue length: one queued already
        B->settings.setValue(QString(mx::keys::queue::MaxLength), 1);
        auto a3 = open(*A, big3);
        QVERIFY(settled(a3.get()));
        QCOMPARE(a3->state(), mx::RangeSource::Failed);
        a3->close();
        B->settings.setValue(QString(mx::keys::queue::MaxLength), 200);
        // Queue requests when all upload slots are busy: off = refused
        B->settings.setValue(QString(mx::keys::queue::Enabled), false);
        auto a4 = open(*A, big3);
        QVERIFY(settled(a4.get()));
        QCOMPARE(a4->state(), mx::RangeSource::Failed);
        a4->close();
        B->settings.setValue(QString(mx::keys::queue::Enabled), true);
        // more slots: Carol's queued file starts; Max uploads per user keeps Alice to one at a time
        B->settings.setValue(QString(mx::keys::transfers::MaxUploads), 5);
        auto a5 = open(*A, big2);
        QVERIFY(settled(a5.get()));
        QCOMPARE(a5->state(), mx::RangeSource::Queued);          // Alice has one running already
        B->settings.setValue(QString(mx::keys::transfers::MaxUploadsPerUser), 30);   // above the old cut at 20
        a5->close();
        a5.reset();
        auto a6 = open(*A, big2);
        QVERIFY(settled(a6.get()));
        QCOMPARE(a6->state(), mx::RangeSource::Ready);
        for (auto *s : {a1.get(), c1.get(), a6.get()}) s->close();
    }

    // queue/timeoutMin: a queued user who stops asking loses his place after the time set (it was cut
    // at 5 minutes); MX_TEST_MINUTE_MS makes a minute 1 s here
    void queuedUserDroppedAfterTheTimeSet()
    {
        qputenv("MX_TEST_MINUTE_MS", "1000");
        auto B = node(QStringLiteral("Bob"), slowTimings()), C = node(QStringLiteral("Carol"), slowTimings());
        B->start();
        C->start();
        QVERIFY(link(*C, *B));
        const mx::SharedFile f = B->library.addGenerated(B->profile, QStringLiteral("slot.bin"), 600000, 41);
        B->setShared();
        B->settings.setValue(QString(mx::keys::transfers::MaxUploads), 1);
        B->settings.setValue(QString(mx::keys::queue::SmallFileBypassKB), 0);
        B->settings.setValue(QString(mx::keys::queue::TimeoutMin), 3);
        mx::FileHashes h;
        h.sha1 = f.hashes.sha1;
        std::unique_ptr<mx::RangeSource> holder(C->plugin->openSource(QStringLiteral("hold"), h, f.size, sourceOf(B->me(), B->plugin->nodeId())));
        holder->open();
        QVERIFY(waitFor([&] { return holder->state() == mx::RangeSource::Ready; }, 10000));
        // a peer that asks once and then says nothing
        FakePeer mute;
        QVERIFY(mute.start(freshDir(QStringLiteral("set1-mute"))));
        mute.node.connectTo(B->addr());
        QVERIFY(waitFor([&] { return mute.node.peerCount() == 1; }));
        mute.hello(B->plugin->nodeId());
        QVERIFY(waitFor([&] { return B->plugin->helloVerified(mute.node.nodeId()); }, 5000));
        QSignalSpy changed(B->plugin, &mx::NetworkPlugin::uploadChanged), finished(B->plugin, &mx::NetworkPlugin::uploadFinished);
        mute.send(B->plugin->nodeId(), QJsonObject{{QStringLiteral("t"), QStringLiteral("get_open")}, {QStringLiteral("v"), 1},
                                                  {QStringLiteral("sid"), 77}, {QStringLiteral("h"), hashesToJson(h)},
                                                  {QStringLiteral("size"), double(f.size)}});
        QString queuedId;
        QVERIFY(waitFor([&] {
            for (const auto &a : changed) {
                const auto t = a.at(0).value<mx::TransferInfo>();
                if (t.state == mx::TransferState::Queued) queuedId = t.id;
            }
            return !queuedId.isEmpty();
        }, 5000));
        QElapsedTimer since;
        since.start();
        auto gone = [&] { for (const auto &a : finished) if (a.at(0).toString() == queuedId) return true; return false; };
        QTest::qWait(2000);
        QVERIFY(!gone());                                  // 2 "minutes": still waiting
        QVERIFY(waitFor(gone, 5000));                       // dropped after 3
        QVERIFY(since.elapsed() >= 2900);
        holder->close();
        qunsetenv("MX_TEST_MINUTE_MS");
    }

    // misc/logLevel = debug: the log file has the private message's delivery, never its text
    void logKeepsNoMessageText()
    {
        const QString profile = freshDir(QStringLiteral("set1-log"));
        mx::installLogHandler(profile, 5 * 1024 * 1024, 5, false, 0);
        mx::setLogLevel(QStringLiteral("debug"));
        {
            auto A = node(QStringLiteral("Alice"), slowTimings()), B = node(QStringLiteral("Bob"), slowTimings());
            A->start();
            B->start();
            QVERIFY(link(*A, *B));
            QSignalSpy rx(B->events, &mx::ChatEvents::messageReceived);
            QSignalSpy st(A->events, &mx::ChatEvents::privateMessageStatus);
            const QString confid = QStringLiteral("my bank pin is 4711 and the door code 0815");
            QVERIFY(A->chat->sendPrivateMessageWithId(B->me(), confid, QStringLiteral("00000000000000f1")));
            QVERIFY(waitFor([&] {
                for (const auto &a : rx)
                    if (a.at(0).value<mx::ChatMessage>().text == confid) return true;
                return false;
            }, 8000));
            QVERIFY(waitFor([&] { for (const auto &a : st) if (a.at(2).toInt() == int(mx::PmDelivery::Delivered)) return true; return false; }, 8000));
            const QString roomLine = QStringLiteral("a room line about the 4711 pin");
            A->chat->joinRoom(QStringLiteral("Debian"));
            QTest::qWait(500);
            A->chat->sendRoomMessage(QStringLiteral("Debian"), roomLine, mx::ChatKind::Message);
            QTest::qWait(500);
        }
        mx::uninstallLogHandler();
        QFile f(profile + QStringLiteral("/logs/octomx.log"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString log = QString::fromUtf8(f.readAll());
        QVERIFY2(log.contains(QStringLiteral(" D mx.overlay")), "debug lines were written");
        QVERIFY(log.contains(QStringLiteral("pm ")));
        QVERIFY(!log.contains(QStringLiteral("4711")));
        QVERIFY(!log.contains(QStringLiteral("door code")));
        QDir(profile).removeRecursively();
    }
};

QTEST_MAIN(TstSet1Overlay)
#include "tst_set1_overlay.moc"
