// SPDX-License-Identifier: GPL-3.0-or-later
// glue-3: bandwidth accounting (all traffic of every plugin, per network, payload split, history),
// the Shared File Types filter / sub-folder exclusion / media folder suggestions, chat networks,
// whois request ids, the UI schema keys and Queued upload rows in the UploadTracker.
#include <QtTest/QtTest>
#include <QtTest/QSignalSpy>

#include <QtCore/QStandardPaths>
#include <QtCore/QTemporaryDir>

#include "core/bandwidth.h"
#include "core/chat_manager.h"
#include "core/database.h"
#include "core/library.h"
#include "core/network_manager.h"
#include "core/rate_limiter.h"
#include "core/real_core.h"
#include "core/upload_tracker.h"
#include "core/user_manager.h"
#include "fake_plugin.h"
#include "mx/settings_keys.h"
#include "mx/simple_settings.h"

using namespace mx;
using mx::test::FakeChatPlugin;
using mx::test::FakePlugin;

namespace {

void writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
}

QStringList names(const QList<SharedFile> &files)
{
    QStringList out;
    for (const SharedFile &f : files) out << f.name;
    out.sort();
    return out;
}

UserRef user(Net n, const QString &id, const QString &name)
{
    UserRef u;
    u.net = n;
    u.id = id;
    u.name = name;
    return u;
}

// A plugin that moves "traffic" through ctx.limiter like the real ones do.
class TrafficPlugin : public FakePlugin {
    Q_OBJECT
public:
    using FakePlugin::FakePlugin;
    void wire(RateLimiter::Dir d, qint64 wireBytes, qint64 payload) { ctx.limiter->accountTraffic(d, net(), wireBytes, payload); }
    void legacy(RateLimiter::Dir d, qint64 bytes) { ctx.limiter->account(d, net(), bytes); }
};

} // namespace

class TestGlue3 : public QObject {
    Q_OBJECT
    QTemporaryDir m_tmp;

private slots:
    void initTestCase()
    {
        registerMetaTypes();
        QVERIFY(m_tmp.isValid());
    }

    // ------------------------------------------------------------------ 1. bandwidth
    void limiterPayloadAndNets()
    {
        CoreRateLimiter lim(nullptr);
        lim.accountTraffic(RateLimiter::Down, Net::BitTorrent, 1000, 800);
        lim.accountTraffic(RateLimiter::Down, Net::Kad, 300, 0);
        lim.accountTraffic(RateLimiter::Down, Net::ED2K, 0, 50);   // payload of bytes accounted before
        lim.account(RateLimiter::Down, Net::ED2K, 200);              // unknown kind
        lim.accountTraffic(RateLimiter::Up, Net::Rats, 70, 0);       // Rats (= 8) has its own counter
        lim.accountTraffic(RateLimiter::Up, Net::Web, 30, 0);
        QCOMPARE(lim.total(RateLimiter::Down), qint64(1500));
        QCOMPARE(lim.payloadTotal(RateLimiter::Down), qint64(850));
        QCOMPARE(lim.total(RateLimiter::Down, Net::ED2K), qint64(200));
        QCOMPARE(lim.payloadTotal(RateLimiter::Down, Net::ED2K), qint64(50));
        QCOMPARE(lim.total(RateLimiter::Up, Net::Rats), qint64(70));
        QCOMPARE(lim.total(RateLimiter::Up, Net::Web), qint64(30));
        // the base-class default (plugins built against an older limiter) still counts the wire bytes
        struct Old : RateLimiter {
            qint64 got = 0;
            qint64 acquire(Dir, qint64 b) override { return b; }
            void account(Dir, Net, qint64 b) override { got += b; }
            int limitBps(Dir) const override { return 0; }
        } old;
        old.accountTraffic(RateLimiter::Down, Net::Gnutella, 123, 100);
        QCOMPARE(old.got, qint64(123));
    }

    void bandwidthSamplesEveryPlugin()
    {
        // six fake plugins of different networks account traffic the way the real ones do
        SimpleSettings settings;
        CoreRateLimiter lim(&settings);
        const QList<Net> nets = {Net::BitTorrent, Net::ED2K, Net::Kad, Net::Gnutella, Net::Overlay, Net::Rats, Net::Web, Net::Http};
        QList<TrafficPlugin *> ps;
        QList<NetworkPlugin *> l;
        for (Net n : nets) {
            auto *p = new TrafficPlugin(n, NetworkPlugin::CanSearch);
            ps << p;
            l << p;
        }
        NetworkManager nm(&settings, l);
        nm.initPlugins(m_tmp.filePath(QStringLiteral("bwprofile")), nullptr, &lim);
        for (TrafficPlugin *p : ps) QVERIFY(p->ctx.limiter == &lim);
        CoreBandwidth bw(&lim, nullptr);
        QSignalSpy samples(&bw, &BandwidthService::sample);
        bw.start();
        // one "second": BT downloads payload + DHT overhead, eD2k uploads with protocol, Kad UDP only,
        // Gnutella / overlay / rats / web / http protocol-only or payload
        ps[0]->wire(RateLimiter::Down, 100000, 90000);
        ps[0]->wire(RateLimiter::Up, 5000, 0);
        ps[1]->wire(RateLimiter::Up, 52000, 50000);
        ps[1]->wire(RateLimiter::Down, 2000, 0);
        ps[2]->wire(RateLimiter::Down, 3000, 0);
        ps[2]->wire(RateLimiter::Up, 3000, 0);
        ps[3]->wire(RateLimiter::Down, 4000, 0);
        ps[4]->wire(RateLimiter::Down, 1000, 1000);
        ps[5]->wire(RateLimiter::Down, 700, 0);
        ps[6]->legacy(RateLimiter::Down, 600);
        ps[7]->wire(RateLimiter::Down, 20000, 20000);
        QTest::qWait(1000);
        bw.tickNow();
        QCOMPARE(samples.count(), 1);
        const BandwidthSample s = samples.at(0).at(0).value<BandwidthSample>();
        const qint64 dt = 1;   // ≈ 1 s: compare ratios / sums, not absolute rates
        Q_UNUSED(dt)
        QVERIFY2(s.downBps > 100000 && s.downBps <= 131300, qPrintable(QString::number(s.downBps)));
        // every network is in the per-network map; the parts add up to the total
        int sumDown = 0, sumUp = 0;
        for (auto it = s.netDownBps.constBegin(); it != s.netDownBps.constEnd(); ++it) sumDown += it.value();
        for (auto it = s.netUpBps.constBegin(); it != s.netUpBps.constEnd(); ++it) sumUp += it.value();
        QVERIFY(qAbs(sumDown - s.downBps) <= nets.size());
        QVERIFY(qAbs(sumUp - s.upBps) <= nets.size());
        for (Net n : nets) QVERIFY2(s.netDownBps.value(n) > 0 || s.netUpBps.value(n) > 0, qPrintable(netName(n)));
        QVERIFY(s.netDownBps.value(Net::Web) > 0 && s.netDownBps.value(Net::Rats) > s.netDownBps.value(Net::Web));   // 700 vs 600: separate counters
        QVERIFY(!s.netUpBps.contains(Net::Gnutella));   // no traffic that way → not in the map
        // payload: BT 90000 + overlay 1000 + http 20000 of 131300 down; eD2k 50000 of 60000 up
        const double payDownShare = double(s.payloadDownBps) / s.downBps;
        QVERIFY2(qAbs(payDownShare - 111000.0 / 131300.0) < 0.01, qPrintable(QString::number(payDownShare)));
        const double payUpShare = double(s.payloadUpBps) / s.upBps;
        QVERIFY2(qAbs(payUpShare - 50000.0 / 60000.0) < 0.01, qPrintable(QString::number(payUpShare)));
        QCOMPARE(bw.totalDownloaded(), qint64(131300));
        QCOMPARE(bw.totalUploaded(), qint64(60000));
        // a quiet second: zero, maps empty
        QTest::qWait(50);
        bw.tickNow();
        const BandwidthSample q = bw.current();
        QCOMPARE(q.downBps, 0);
        QVERIFY(q.netDownBps.isEmpty());
        bw.shutdown();
    }

    void bandwidthHistoryBacklog()
    {
        CoreRateLimiter lim(nullptr);
        CoreBandwidth bw(&lim, nullptr);
        bw.start();
        for (int i = 0; i < 3700; ++i) {
            lim.accountTraffic(RateLimiter::Down, Net::Gnutella, 1000 + i, 0);
            bw.tickNow();
        }
        // one hour is kept (the UI draws one pixel per second and asks for a backlog on page open)
        QCOMPARE(bw.history(0).size(), 3600);
        QCOMPARE(bw.history(600).size(), 600);
        QCOMPARE(bw.history(5000).size(), 3600);
        const QList<BandwidthSample> h = bw.history(10);
        for (int i = 1; i < h.size(); ++i) QVERIFY(h[i].timeMs >= h[i - 1].timeMs);
        QCOMPARE(h.last().timeMs, bw.current().timeMs);
        QVERIFY(h.last().netDownBps.value(Net::Gnutella) > 0);
        bw.shutdown();
    }

    void bandwidthAllTimePersisted()
    {
        const QString dir = m_tmp.filePath(QStringLiteral("bwdb"));
        QDir().mkpath(dir);
        Database db;
        QVERIFY(db.open(dir + QStringLiteral("/octomx.db")));
        {
            CoreRateLimiter lim(nullptr);
            CoreBandwidth bw(&lim, &db);
            bw.start();
            lim.accountTraffic(RateLimiter::Down, Net::BitTorrent, 5000, 4000);
            lim.accountTraffic(RateLimiter::Up, Net::ED2K, 700, 600);
            bw.tickNow();
            bw.shutdown();
        }
        db.flush();
        CoreRateLimiter lim(nullptr);
        CoreBandwidth bw(&lim, &db);
        bw.start();
        QCOMPARE(bw.allTimeDownloaded(), qint64(5000));
        QCOMPARE(bw.allTimeUploaded(), qint64(700));
        bw.shutdown();
        db.close();
    }

    // ------------------------------------------------------------------ 2. library
    void sharedTypeRules()
    {
        // [ui-f] the groups of the type table (were the eight WinMX 3.53 groups)
        QCOMPARE(sharedTypeIds().size(), 9);
        const QStringList all = sharedTypeIds();
        QVERIFY(sharedTypeExtensions(QStringLiteral("video")).contains(QStringLiteral("mkv")));
        QVERIFY(sharedTypeExtensions(QStringLiteral("VIDEO")).contains(QStringLiteral("avi")));
        QVERIFY(sharedTypeExtensions(QStringLiteral("nope")).isEmpty());
        QVERIFY(fileMatchesSharedTypes(QStringLiteral("a.MP3"), all, QString(), false));
        QVERIFY(!fileMatchesSharedTypes(QStringLiteral("a.mp3"), {QStringLiteral("video")}, QString(), true));
        QVERIFY(fileMatchesSharedTypes(QStringLiteral("/x/y/a.flac"), {QStringLiteral("audio")}, QString(), false));
        QVERIFY(fileMatchesSharedTypes(QStringLiteral("book.pdf"), all, QString(), false));   // documents are a group now
        QStringList noDocs = all;
        noDocs.removeAll(QStringLiteral("documents"));
        QVERIFY(!fileMatchesSharedTypes(QStringLiteral("book.pdf"), noDocs, QString(), true));
        QVERIFY(!fileMatchesSharedTypes(QStringLiteral("notes.xyz"), all, QString(), false));   // unlisted, strict
        QVERIFY(fileMatchesSharedTypes(QStringLiteral("notes.xyz"), all, QString(), true));
        QVERIFY(fileMatchesSharedTypes(QStringLiteral("book.pdf"), {}, QStringLiteral(".epub, *.PDF"), false));
        QVERIFY(fileMatchesSharedTypes(QStringLiteral("song.mp3"), {}, QStringLiteral("mp3"), false));   // other wins
        QVERIFY(!fileMatchesSharedTypes(QStringLiteral("README"), all, QStringLiteral("txt"), false));
        QVERIFY(fileMatchesSharedTypes(QStringLiteral("README"), {}, QString(), true));
        // WinMX 3.53 ids of older profiles keep sharing what they shared (flac was "otheraudio", mkv
        // "video", pdf an unlisted type)
        QVERIFY(fileMatchesSharedTypes(QStringLiteral("C:\\music\\a.ogg"), {QStringLiteral("ogg")}, QString(), false));
        const QStringList old = {QStringLiteral("mp3"), QStringLiteral("ogg"), QStringLiteral("video"), QStringLiteral("pictures"),
                                 QStringLiteral("servers"), QStringLiteral("clr"), QStringLiteral("otheraudio"), QStringLiteral("archive")};
        QVERIFY(isLegacySharedTypeList(old));
        QVERIFY(!isLegacySharedTypeList(all));
        QCOMPARE(normalizeSharedTypeIds(old, true), all);
        QCOMPARE(normalizeSharedTypeIds(old, false), (QStringList{QStringLiteral("audio"), QStringLiteral("video"),
                                                                 QStringLiteral("pictures"), QStringLiteral("software"),
                                                                 QStringLiteral("archives"), QStringLiteral("servers"),
                                                                 QStringLiteral("clr")}));
        for (const char *f : {"a.flac", "b.mkv", "c.pdf", "d.iso", "e.7z", "f.torrent", "g.m4a"})
            QVERIFY2(fileMatchesSharedTypes(QString::fromLatin1(f), old, QString(), true), f);
        QCOMPARE(normalizeSharedTypeIds({QStringLiteral("mp3"), QStringLiteral("bogus")}, false), QStringList{QStringLiteral("audio")});
        QCOMPARE(normalizeSharedTypeIds(all, false), all);
    }

    void schemaKeys()
    {
        const QList<QPair<QString, QVariant>> expect = {
            {QStringLiteral("ui/chatOpenRooms"), QStringList()},
            {QStringLiteral("ui/chatHoldOpenAfterPart"), false},
            {QStringLiteral("ui/chatWarningDisabled"), false},
            {QStringLiteral("ui/lastShareDir"), QString()},
            {QStringLiteral("library/sharedTypes"), sharedTypeIds()},
            {QStringLiteral("library/otherExtensions"), QString()},
            {QStringLiteral("library/shareUnlistedTypes"), true},
        };
        for (const auto &e : expect) {
            const SettingDef *d = findSetting(e.first);
            QVERIFY2(d, qPrintable(e.first));
            QCOMPARE(settingDefault(e.first), e.second);
        }
        QCOMPARE(findSetting(QStringLiteral("ui/chatOpenRooms"))->type, SettingType::StringList);
        QCOMPARE(findSetting(QStringLiteral("library/sharedTypes"))->type, SettingType::StringList);
    }

    void libraryTypeFilter()
    {
        const QString root = m_tmp.filePath(QStringLiteral("lib1"));
        const QString share = root + QStringLiteral("/share");
        writeFile(share + QStringLiteral("/song.mp3"), QByteArray(5000, 'm'));
        writeFile(share + QStringLiteral("/clip.mkv"), QByteArray(6000, 'v'));
        writeFile(share + QStringLiteral("/pic.jpg"), QByteArray(700, 'p'));
        writeFile(share + QStringLiteral("/notes.xyz"), QByteArray(800, 'd'));   // [ui-f] an unlisted type
        writeFile(share + QStringLiteral("/sub/tune.ogg"), QByteArray(900, 'o'));
        writeFile(share + QStringLiteral("/sub/deeper/arc.zip"), QByteArray(1000, 'z'));
        SimpleSettings settings;
        settings.setValue(keys::library::WatchFolders, false);
        Database db;
        QVERIFY(db.open(root + QStringLiteral("/octomx.db")));
        Library lib(&settings, &db);
        lib.addFolder(share, true);
        lib.start();
        QVERIFY(lib.waitIdle(30000));
        QCOMPARE(lib.fileCount(), 6);
        QCOMPARE(lib.sharedTypes(), sharedTypeIds());

        // uncheck Video and Pictures: those files are no longer listed, pushed to plugins, found or read
        SharedFile clip;
        for (const SharedFile &f : lib.sharedFiles()) if (f.name == QLatin1String("clip.mkv")) clip = f;
        QVERIFY(clip.hashed);
        QVERIFY(!lib.read(clip.hashes, 0, 10).isEmpty());
        QSignalSpy pushed(&lib, &Library::sharedFilesUpdated);
        QStringList types = sharedTypeIds();
        types.removeAll(QStringLiteral("video"));
        types.removeAll(QStringLiteral("pictures"));
        lib.setSharedTypes(types + QStringList{QStringLiteral("bogus")});
        QCOMPARE(lib.sharedTypes(), types);                                  // unknown ids dropped
        QCOMPARE(settings.sl(keys::library::SharedTypes), types);            // persisted through the setting
        QVERIFY(lib.waitIdle(30000));
        QCOMPARE(names(lib.files()), (QStringList{QStringLiteral("arc.zip"), QStringLiteral("notes.xyz"),
                                                  QStringLiteral("song.mp3"), QStringLiteral("tune.ogg")}));
        QVERIFY(!lib.findByHash(clip.hashes, nullptr));
        QVERIFY(lib.read(clip.hashes, 0, 10).isEmpty());
        QVERIFY(pushed.wait(5000));
        QCOMPARE(names(pushed.last().at(0).value<QList<SharedFile>>()), names(lib.files()));

        // strict original WinMX mode: unlisted types (.xyz) only via "Other file extensions"
        settings.setValue(keys::library::ShareUnlistedTypes, false);
        QVERIFY(lib.waitIdle(30000));
        QVERIFY(!names(lib.files()).contains(QStringLiteral("notes.xyz")));
        lib.setOtherExtensions(QStringLiteral(" .xyz   mkv "));
        QCOMPARE(lib.otherExtensions(), QStringLiteral(".xyz mkv"));
        QVERIFY(lib.waitIdle(30000));
        QVERIFY(names(lib.files()).contains(QStringLiteral("notes.xyz")));
        QVERIFY(names(lib.files()).contains(QStringLiteral("clip.mkv")));   // other extensions beat the groups
        QVERIFY(!names(lib.files()).contains(QStringLiteral("pic.jpg")));

        // a file hashed on request (completed download) under a shared folder honours the filter too
        writeFile(share + QStringLiteral("/later.png"), QByteArray(300, 'q'));
        lib.requestHash(share + QStringLiteral("/later.png"));
        QVERIFY(lib.waitIdle(30000));
        QVERIFY(!names(lib.files()).contains(QStringLiteral("later.png")));
        lib.stop();
        db.close();
    }

    void libraryFolderExclusion()
    {
        const QString root = m_tmp.filePath(QStringLiteral("lib2"));
        const QString share = root + QStringLiteral("/share");
        writeFile(share + QStringLiteral("/top.mp3"), QByteArray(1000, 't'));
        writeFile(share + QStringLiteral("/a/one.mp3"), QByteArray(1100, '1'));
        writeFile(share + QStringLiteral("/a/b/two.mp3"), QByteArray(1200, '2'));
        writeFile(share + QStringLiteral("/c/three.mp3"), QByteArray(1300, '3'));
        writeFile(root + QStringLiteral("/flat/x.mp3"), QByteArray(1400, 'x'));
        writeFile(root + QStringLiteral("/flat/inner/y.mp3"), QByteArray(1500, 'y'));
        SimpleSettings settings;
        settings.setValue(keys::library::WatchFolders, false);
        Database db;
        QVERIFY(db.open(root + QStringLiteral("/octomx.db")));
        {
            Library lib(&settings, &db);
            lib.addFolder(share, true);
            lib.addFolder(root + QStringLiteral("/flat"), false);   // "share without sub folders"
            lib.start();
            QVERIFY(lib.waitIdle(30000));
            QCOMPARE(names(lib.files()), (QStringList{QStringLiteral("one.mp3"), QStringLiteral("three.mp3"),
                                                      QStringLiteral("top.mp3"), QStringLiteral("two.mp3"), QStringLiteral("x.mp3")}));
            QSignalSpy folders(&lib, &LibraryService::foldersChanged);
            lib.excludeFolder(share + QStringLiteral("/a"), true);
            QCOMPARE(folders.count(), 1);
            QCOMPARE(names(lib.files()), (QStringList{QStringLiteral("three.mp3"), QStringLiteral("top.mp3"), QStringLiteral("x.mp3")}));
            QCOMPARE(lib.excludedFolders(), QStringList{share + QStringLiteral("/a")});
            lib.rescan();
            QVERIFY(lib.waitIdle(30000));
            QCOMPARE(lib.fileCount(), 3);
            lib.unshareFile(share + QStringLiteral("/top.mp3"));    // a single file is not a folder exclusion
            QCOMPARE(lib.excludedFolders(), QStringList{share + QStringLiteral("/a")});
            lib.stop();
        }
        {
            Library lib(&settings, &db);   // persisted
            lib.start();
            QVERIFY(lib.waitIdle(30000));
            QCOMPARE(names(lib.files()), (QStringList{QStringLiteral("three.mp3"), QStringLiteral("x.mp3")}));
            QCOMPARE(lib.excludedFolders(), QStringList{share + QStringLiteral("/a")});
            bool flatNonRecursive = false;
            for (const SharedFolder &f : lib.folders())
                if (f.path == root + QStringLiteral("/flat")) flatNonRecursive = !f.recursive;
            QVERIFY(flatNonRecursive);
            lib.excludeFolder(share + QStringLiteral("/a"), false);
            QVERIFY(lib.waitIdle(30000));
            QCOMPARE(names(lib.files()), (QStringList{QStringLiteral("one.mp3"), QStringLiteral("three.mp3"),
                                                      QStringLiteral("two.mp3"), QStringLiteral("x.mp3")}));
            QVERIFY(lib.excludedFolders().isEmpty());
            lib.stop();
        }
        db.close();
    }

    void mediaFolderSuggestions()
    {
        const QString root = m_tmp.filePath(QStringLiteral("lib3"));
#ifdef Q_OS_LINUX
        // point the XDG user dirs at our temp tree (never the real $HOME)
        const QString cfg = root + QStringLiteral("/config");
        writeFile(cfg + QStringLiteral("/user-dirs.dirs"),
                  QStringLiteral("XDG_MUSIC_DIR=\"%1/Music\"\nXDG_VIDEOS_DIR=\"%1/Videos\"\nXDG_PICTURES_DIR=\"%1/Pictures\"\n"
                                 "XDG_DOWNLOAD_DIR=\"%1/Downloads\"\n").arg(root).toUtf8());
        const QByteArray oldCfg = qgetenv("XDG_CONFIG_HOME");
        qputenv("XDG_CONFIG_HOME", cfg.toUtf8());
        QDir().mkpath(root + QStringLiteral("/Music"));
        QDir().mkpath(root + QStringLiteral("/Videos"));   // Pictures + Downloads do not exist
#endif
        SimpleSettings settings;
        settings.setValue(keys::library::WatchFolders, false);
        Database db;
        QVERIFY(db.open(root + QStringLiteral("/octomx.db")));
        Library lib(&settings, &db);
        lib.start();
        QVERIFY(lib.waitIdle(10000));
        const QStringList s = lib.suggestMediaFolders();
        for (const QString &p : s) {
            QVERIFY2(QFileInfo(p).isDir(), qPrintable(p));
            QVERIFY(Library::standardMediaFolders().contains(p));
            QVERIFY(p != QDir::cleanPath(QDir::homePath()));
        }
#ifdef Q_OS_LINUX
        QCOMPARE(s, (QStringList{root + QStringLiteral("/Music"), root + QStringLiteral("/Videos")}));
        lib.addFolder(root + QStringLiteral("/Music"), true);   // already shared → not suggested
        QCOMPARE(lib.suggestMediaFolders(), QStringList{root + QStringLiteral("/Videos")});
        if (oldCfg.isNull()) qunsetenv("XDG_CONFIG_HOME");
        else qputenv("XDG_CONFIG_HOME", oldCfg);
#endif
        lib.stop();
        db.close();
    }

    void libraryPersistsThroughRealCore()
    {
        const QString profile = m_tmp.filePath(QStringLiteral("rc"));
        const QString share = profile + QStringLiteral("-share");
        writeFile(share + QStringLiteral("/keep/k.mp3"), QByteArray(1000, 'k'));
        writeFile(share + QStringLiteral("/skip/s.mp3"), QByteArray(1000, 's'));
        {
            RealCore core(profile, {});
            core.settings()->setValue(keys::library::WatchFolders, false);
            core.start();
            core.library()->addFolder(share, true);
            core.library()->setSharedTypes({QStringLiteral("mp3"), QStringLiteral("video")});
            core.library()->setOtherExtensions(QStringLiteral("epub"));
            QVERIFY(core.libraryImpl()->waitIdle(30000));
            core.library()->excludeFolder(share + QStringLiteral("/skip"), true);
            core.shutdown();
        }
        RealCore core(profile, {});
        // [ui-f] the WinMX 3.53 ids are stored as the table's groups (unlisted types are shared by
        // default, so documents and torrents stay shared too)
        QCOMPARE(core.library()->sharedTypes(), (QStringList{QStringLiteral("audio"), QStringLiteral("video"),
                                                             QStringLiteral("documents"), QStringLiteral("torrents")}));
        QCOMPARE(core.library()->otherExtensions(), QStringLiteral("epub"));
        core.start();
        QVERIFY(core.libraryImpl()->waitIdle(30000));
        QCOMPARE(core.library()->excludedFolders(), QStringList{share + QStringLiteral("/skip")});
        QCOMPARE(names(core.library()->files()), QStringList{QStringLiteral("k.mp3")});
        core.shutdown();
    }

    // ------------------------------------------------------------------ 3. chat + whois
    void chatNetworksAndRefresh()
    {
        SimpleSettings settings;
        auto *chatPlugin = new FakeChatPlugin;
        auto *ed = new FakePlugin(Net::ED2K, NetworkPlugin::CanSearch);
        NetworkManager net(&settings, {ed, chatPlugin});
        net.initPlugins(m_tmp.filePath(QStringLiteral("chatprofile")), nullptr, nullptr);
        UserManager users(&settings, &net, nullptr);
        ChatManager chat(&settings, &net, &users, nullptr);
        chat.start();
        QVERIFY(chat.chatNetworks().isEmpty());          // nothing running yet
        net.startNetworks(true);
        QTRY_COMPARE(chat.chatNetworks(), QList<Net>{Net::Overlay});
        QSignalSpy list(&chat, &ChatService::roomListChanged);
        chat.refreshRoomList(Net::Overlay);
        QCOMPARE(chatPlugin->refreshCount, 1);
        QVERIFY(list.count() >= 1);
        const int before = list.count();
        chat.refreshRoomList(Net::Gnutella);              // no rooms there: answered, backend not asked
        QTRY_VERIFY(list.count() > before);
        QCOMPARE(chatPlugin->refreshCount, 1);
        // the mock / base default
        struct Base : ChatService {
            int refreshes = 0;
            void refreshRoomList() override { ++refreshes; }
            QList<ChatRoomInfo> roomList() const override { return {}; }
            void joinRoom(const QString &) override {}
            void leaveRoom(const QString &) override {}
            QList<ChatRoomInfo> joinedRooms() const override { return {}; }
            QList<ChatUser> roomUsers(const QString &) const override { return {}; }
            QList<ChatMessage> backlog(const QString &) const override { return {}; }
            void sendMessage(const QString &, const QString &) override {}
            void sendPrivateMessage(const UserRef &, const QString &) override {}
            void setTopic(const QString &, const QString &) override {}
            QStringList favouriteRooms() const override { return {}; }
            void setFavouriteRoom(const QString &, bool) override {}
        } base;
        QCOMPARE(base.chatNetworks(), QList<Net>{Net::Overlay});
        static_cast<ChatService &>(base).refreshRoomList(Net::Overlay);
        QCOMPARE(base.refreshes, 1);
        net.stopAll();
    }

    void whoisRequestIds()
    {
        SimpleSettings settings;
        auto *chatPlugin = new FakeChatPlugin;
        NetworkManager net(&settings, {chatPlugin});
        net.initPlugins(m_tmp.filePath(QStringLiteral("whoisprofile")), nullptr, nullptr);
        net.startNetworks(true);
        UserManager users(&settings, &net, nullptr);
        users.setWhoisTimeoutMs(300);
        QSignalSpy replies(&users, &UserService::whoisReply);
        QSignalSpy old(&users, &UserService::whoisResult);
        const UserRef alice = user(Net::Overlay, QStringLiteral("aa11"), QStringLiteral("alice_12345"));
        const UserRef bob = user(Net::Overlay, QStringLiteral("bb22"), QStringLiteral("bob_54321"));
        const quint32 r1 = users.requestWhois(alice);   // window 1
        const quint32 r2 = users.requestWhois(bob);     // window 2
        const quint32 r3 = users.requestWhois(alice);   // window 3, same user
        QVERIFY(r1 && r2 && r3 && r1 != r2 && r2 != r3 && r1 != r3);
        QCOMPARE(chatPlugin->whoisCalls.size(), 3);
        // bob answers first: only r2 gets it
        emit chatPlugin->whoisResult(bob, QVariantMap{{QStringLiteral("files"), 7}});
        QCOMPARE(replies.count(), 1);
        QCOMPARE(replies.at(0).at(0).toUInt(), r2);
        QCOMPARE(replies.at(0).at(1).value<UserRef>().id, bob.id);
        QCOMPARE(replies.at(0).at(2).toMap().value(QStringLiteral("files")).toInt(), 7);
        QCOMPARE(replies.at(0).at(2).toMap().value(QStringLiteral("requestId")).toUInt(), r2);
        QCOMPARE(old.count(), 1);
        // alice: both of her requests, once each; a second answer does not repeat them
        emit chatPlugin->whoisResult(alice, QVariantMap{{QStringLiteral("files"), 3}});
        QCOMPARE(replies.count(), 3);
        QCOMPARE(replies.at(1).at(0).toUInt(), r1);
        QCOMPARE(replies.at(2).at(0).toUInt(), r3);
        emit chatPlugin->whoisResult(alice, QVariantMap{{QStringLiteral("files"), 4}});
        QCOMPARE(replies.count(), 3);
        QCOMPARE(old.count(), 3);
        // no answer → exactly one error reply after the timeout
        const quint32 r4 = users.requestWhois(user(Net::Overlay, QStringLiteral("cc33"), QStringLiteral("carol")));
        QTRY_COMPARE_WITH_TIMEOUT(replies.count(), 4, 3000);
        QCOMPARE(replies.at(3).at(0).toUInt(), r4);
        QVERIFY(!replies.at(3).at(2).toMap().value(QStringLiteral("error")).toString().isEmpty());
        QTest::qWait(500);
        QCOMPARE(replies.count(), 4);
        // a network without whois answers at once with an error, still with the id
        const quint32 r5 = users.requestWhois(user(Net::Gnutella, QStringLiteral("1.2.3.4:6346"), QString()));
        QTRY_COMPARE(replies.count(), 5);
        QCOMPARE(replies.at(4).at(0).toUInt(), r5);
        QVERIFY(replies.at(4).at(2).toMap().contains(QStringLiteral("error")));
        net.stopAll();
    }

    // ------------------------------------------------------------------ 5. queued uploads in the tracker
    void queuedUploadRows()
    {
        SimpleSettings settings;
        settings.setValue(keys::transfers::AutoClearUploads, false);
        UploadTracker tracker(&settings);
        FakePlugin p(Net::ED2K, NetworkPlugin::CanServe);
        tracker.attach(&p);
        QSignalSpy removed(&tracker, &UploadTracker::uploadRemoved);
        TransferInfo q;
        q.id = QStringLiteral("ed2k-up-1");
        q.name = QStringLiteral("file.mp3");
        q.state = TransferState::Queued;
        q.queuePosition = 2;
        p.emitUpload(q);
        QCOMPARE(tracker.uploads().size(), 1);
        QCOMPARE(tracker.uploads().first().queuePosition, 2);
        QCOMPARE(tracker.uploads().first().state, TransferState::Queued);
        // left the queue without receiving anything → the row goes away (no "Complete")
        p.emitUploadFinished(q.id);
        QCOMPARE(removed.count(), 1);
        QVERIFY(tracker.uploads().isEmpty());
        // queued → slot granted (same id) → finished = a Complete row
        TransferInfo t = q;
        t.id = QStringLiteral("ed2k-up-2");
        p.emitUpload(t);
        t.state = TransferState::Transferring;
        t.done = 1000;
        t.queuePosition = 0;
        p.emitUpload(t);
        p.emitUploadFinished(t.id);
        QCOMPARE(tracker.uploads().size(), 1);
        QCOMPARE(tracker.uploads().first().state, TransferState::Completed);
        QCOMPARE(tracker.uploads().first().id, QStringLiteral("up:ed2k:ed2k-up-2"));
        // [core-5] an upload slot that ended without sending a byte leaves no "Complete 0 of" row
        TransferInfo z = t;
        z.id = QStringLiteral("ed2k-up-3");
        z.done = 0;
        p.emitUpload(z);
        QCOMPARE(tracker.uploads().size(), 2);
        p.emitUploadFinished(z.id);
        QCOMPARE(tracker.uploads().size(), 1);
        // a plugin's own end state (a peer that left halfway) is kept with its byte count
        TransferInfo h = t;
        h.id = QStringLiteral("bt-up-4");
        h.size = 5000;
        h.done = 1200;
        p.emitUpload(h);
        h.state = TransferState::Cancelled;
        h.statusText = QStringLiteral("Disconnected");
        p.emitUpload(h);
        p.emitUploadFinished(h.id);
        TransferInfo got;
        QVERIFY(tracker.upload(QStringLiteral("up:ed2k:bt-up-4"), &got));
        QCOMPARE(got.state, TransferState::Cancelled);
        QCOMPARE(got.statusText, QStringLiteral("Disconnected"));
        QCOMPARE(got.done, qint64(1200));
    }
};

QTEST_MAIN(TestGlue3)
#include "tst_glue3.moc"
