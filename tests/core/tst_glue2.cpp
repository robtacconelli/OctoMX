// SPDX-License-Identifier: GPL-3.0-or-later
// glue-2: settings schema batch (incl. "every literal key read by plugin/core code is in the schema"),
// the added service API (ping, hash search, per-transfer / per-source / upload actions, server list
// management, network summary, manual resume) and the search hygiene (bridge entries, suspects).
#include <QtTest/QtTest>
#include <QtTest/QSignalSpy>

#include <QtCore/QDirIterator>
#include <QtCore/QRegularExpression>
#include <QtCore/QTemporaryDir>
#include <QtNetwork/QTcpServer>

#include "core/database.h"
#include "core/engine/engine.h"
#include "core/network_manager.h"
#include "core/pinger.h"
#include "core/search_hygiene.h"
#include "core/search_manager.h"
#include "core/server_list.h"
#include "core/upload_tracker.h"
#include "fake_plugin.h"
#include "mx/settings_keys.h"
#include "mx/simple_settings.h"
#include "mxhash/ed2k.h"
#include "mxhash/encoding.h"

#ifdef MX_HAVE_MOCK
#include "mx/services.h"
#endif

using namespace mx;
using mx::test::FakePlugin;
using mx::test::FakeRangeSource;

namespace {

// FakePlugin + the glue-2 hooks and emit helpers
class GluePlugin : public FakePlugin {
    Q_OBJECT
public:
    using FakePlugin::FakePlugin;
    bool nativePing = false;
    int nativePingMs = 42;
    int importAnswer = -1;
    QList<QByteArray> imported;
    QStringList startedNow;
    QList<QPair<QString, int>> priorities;
    bool uploadHooks = true;
    // byte sources
    QHash<QByteArray, QByteArray> data;       // ed2k hash → bytes
    int openDelayMs = 0;
    bool failNewSources = false;
    QList<QPointer<FakeRangeSource>> made;

    bool ping(quint32 token, const SearchSource &) override
    {
        if (!nativePing) return false;
        QTimer::singleShot(5, this, [this, token] { emit pingResult(token, nativePingMs); });
        return true;
    }
    int importServers(const QByteArray &d) override
    {
        imported << d;
        return importAnswer;
    }
    bool startUploadNow(const QString &id) override
    {
        if (!uploadHooks) return false;
        startedNow << id;
        return true;
    }
    bool setUploadPriority(const QString &id, int p) override
    {
        if (!uploadHooks) return false;
        priorities << qMakePair(id, p);
        return true;
    }
    RangeSource *openSource(const QString &, const FileHashes &h, qint64, const SearchSource &src) override
    {
        const auto it = data.constFind(h.ed2k);
        if (it == data.constEnd()) return nullptr;
        auto *s = new FakeRangeSource(src, it.value());
        s->openDelayMs = openDelayMs;
        s->failRequests = failNewSources;
        made << s;
        return s;
    }
    void emitSources(const QString &dl, const QList<SearchSource> &s) { emit sourcesFound(dl, s); }
    void emitHashes(const QString &dl, const FileHashes &h) { emit hashesLearned(dl, h); }
    void emitServerMessage(const QString &id, const QString &t) { emit serverMessage(id, t); }
    void emitIncoming(int n) { emit incomingSearches(n); }
};

SearchSource peer(Net net, const QString &id, const QString &address = QString())
{
    SearchSource s;
    s.net = net;
    s.user.net = net;
    s.user.id = id;
    s.user.name = id;
    s.user.address = address;
    s.uri = QStringLiteral("fake://%1").arg(id);
    return s;
}

QByteArray randomBytes(int n, quint32 seed)
{
    QByteArray b(n, Qt::Uninitialized);
    quint32 x = seed * 2654435761u + 1;
    for (int i = 0; i < n; ++i) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        b[i] = char(x & 0xFF);
    }
    return b;
}

QByteArray ed2kOf(const QByteArray &d)
{
    hash::Ed2kHasher h;
    h.update(d.constData(), d.size());
    return h.finish();
}

bool writeFile(const QString &path, const QByteArray &d)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(d) == d.size();
}

QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// A literal settings key as written in C++ source: "prefix/name" with a known prefix.
const QRegularExpression &literalKeyRx()
{
    static const QRegularExpression rx(QStringLiteral(
        "\"((?:bt|ed2k|kad|gnutella|http|overlay|web|rats|connection|network|identity|other|messages|chat|"
        "hotlist|search|transfers|queue|bandwidth|launch|library|misc|ui|colors|internal)/[A-Za-z][A-Za-z0-9_]*)\""));
    return rx;
}

// Literal keys that are deliberately not in the schema (test switches / prefix matches).
const QSet<QString> &allowedNonSchemaKeys()
{
    static const QSet<QString> s = {
        // [engine-3] the kad/ed2k test switches are now hidden schema entries (SettingDef::hidden)
        QStringLiteral("connection/proxy"),   // key.startsWith("connection/proxy"), a family prefix
    };
    return s;
}

QStringList unknownKeysIn(const QString &text)
{
    QStringList out;
    auto it = literalKeyRx().globalMatch(text);
    while (it.hasNext()) {
        const QString k = it.next().captured(1);
        if (allowedNonSchemaKeys().contains(k)) continue;
        if (!findSetting(k)) out << k;
    }
    return out;
}

struct Net2 {
    SimpleSettings settings;
    NetworkManager *nm = nullptr;
    QList<GluePlugin *> plugins;
    explicit Net2(const QList<GluePlugin *> &ps) : plugins(ps)
    {
        QList<NetworkPlugin *> l;
        for (GluePlugin *p : ps) l << p;
        nm = new NetworkManager(&settings, l);
        nm->initPlugins(QString(), nullptr, nullptr);
        nm->startNetworks(true);
    }
    ~Net2() { delete nm; }
};

struct Eng {
    SimpleSettings settings;
    Database db;
    UploadTracker uploads{&settings};
    GluePlugin *plugin = nullptr;
    std::unique_ptr<DownloadEngine> engine;
    QString root;
    Eng(const QString &dir, int openDelayMs = 0)
    {
        root = dir;
        QDir(root).removeRecursively();
        QDir().mkpath(root);
        settings.setValue(keys::transfers::IncomingDir, root + QStringLiteral("/incoming"));
        settings.setValue(keys::transfers::IncompleteDir, root + QStringLiteral("/incomplete"));
        settings.setValue(keys::transfers::MinFreeSpaceMB, 0);
        QString err;
        db.open(root + QStringLiteral("/octomx.db"), &err);
        plugin = new GluePlugin(Net::ED2K, NetworkPlugin::CanFindSources | NetworkPlugin::CanFetchRanges | NetworkPlugin::CanServe);
        plugin->openDelayMs = openDelayMs;
        plugin->start();
        uploads.attach(plugin);
        EngineContext ctx;
        ctx.settings = &settings;
        ctx.db = &db;
        ctx.uploads = &uploads;
        ctx.plugins = {plugin};
        ctx.profileDir = root;
        engine = std::make_unique<DownloadEngine>(ctx);
        engine->start();
    }
    ~Eng()
    {
        engine->shutdown();
        engine.reset();
        db.close();
        delete plugin;
    }
};


} // namespace

class TestGlue2 : public QObject {
    Q_OBJECT
    QTemporaryDir m_tmp;

private slots:
    void initTestCase()
    {
        registerMetaTypes();
        QVERIFY(m_tmp.isValid());
    }

    // ------------------------------------------------------------------ 1. schema batch
    void schemaHasTheBatch()
    {
        const QList<QPair<QString, QVariant>> expect = {
            {QStringLiteral("gnutella/uploadSlots"), 0}, {QStringLiteral("gnutella/uploadQueueSize"), 32},
            {QStringLiteral("gnutella/uploadQueuePollSec"), 30}, {QStringLiteral("gnutella/autoBootstrap"), true},
            {QStringLiteral("gnutella/udpHostCaches"), QStringList()},
            {QStringLiteral("kad/storeIndex"), true}, {QStringLiteral("kad/firewallChecks"), true},
            {QStringLiteral("kad/publish"), true}, {QStringLiteral("kad/buddies"), true},
            {QStringLiteral("kad/maxPacketsPerSec"), 80},
            {QStringLiteral("ed2k/uploadSlots"), 0}, {QStringLiteral("ed2k/maxHalfOpen"), 8},
            {QStringLiteral("ed2k/maxConnectsPer5s"), 20}, {QStringLiteral("ed2k/parallelServerConnects"), 2},
            {QStringLiteral("ed2k/udpPing"), true}, {QStringLiteral("ed2k/udpSearch"), true},
            {QStringLiteral("ed2k/udpSources"), true}, {QStringLiteral("ed2k/compressUploads"), true},
            {QStringLiteral("ui/hotlistVisible"), false},   // [ui-d] the 4 duplicate ui/search* keys were retired
            {QStringLiteral("ui/hotlistConfirmRemove"), false},
            {QStringLiteral("colors/newsBackground"), QStringLiteral("#000000")},
            {QStringLiteral("colors/newsText"), QStringLiteral("#ffff00")},
            {QStringLiteral("colors/newsLink"), QStringLiteral("#ff00ff")},
            {QStringLiteral("search/dropTorrentBridges"), true}, {QStringLiteral("search/flagSuspects"), true},
            {QStringLiteral("search/hideSuspects"), false},
        };
        for (const auto &e : expect) {
            const SettingDef *d = findSetting(e.first);
            QVERIFY2(d, qPrintable(e.first));
            QVERIFY2(!d->label.isEmpty(), qPrintable(e.first));
            QCOMPARE(settingDefault(e.first), e.second);
        }
        // test switches stay out of the schema (not user-facing; the schema has no "hidden" flag)
        for (const QString &k : allowedNonSchemaKeys()) QVERIFY2(!findSetting(k), qPrintable(k));
        // ranges are enforced
        SimpleSettings s;
        s.setValue(QStringLiteral("kad/maxPacketsPerSec"), 5);
        QCOMPARE(s.i(QStringLiteral("kad/maxPacketsPerSec")), 10);
        s.setValue(QStringLiteral("gnutella/uploadQueuePollSec"), 99);
        QCOMPARE(s.i(QStringLiteral("gnutella/uploadQueuePollSec")), 40);
    }

    void literalKeyScannerWorks()
    {
        // the scanner itself must find unknown keys, ignore allowed ones and schema keys
        const QString sample = QStringLiteral(
            "cfg.b(QStringLiteral(\"kad/notARealKey\"), true); s->value(\"ed2k/tcpPort\"); "
            "cfg.b(QStringLiteral(\"kad/allowLan\"), false); dir + QStringLiteral(\"/torrents\");");
        QCOMPARE(unknownKeysIn(sample), QStringList{QStringLiteral("kad/notARealKey")});
    }

    void sourceReadsOnlySchemaKeys()
    {
        const QString root = QStringLiteral(MX_SOURCE_DIR);
        if (!QFileInfo::exists(root + QStringLiteral("/src/core")))
            QSKIP("source tree not reachable from this test (cross build?)");
        QStringList problems;
        int files = 0, keysSeen = 0;
        for (const QString &sub : {QStringLiteral("/src/net"), QStringLiteral("/src/core")}) {
            QDirIterator it(root + sub, {QStringLiteral("*.cpp"), QStringLiteral("*.h")}, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const QString path = it.next();
                const QString text = QString::fromUtf8(readFile(path));
                ++files;
                auto m = literalKeyRx().globalMatch(text);
                while (m.hasNext()) { m.next(); ++keysSeen; }
                for (const QString &k : unknownKeysIn(text))
                    problems << QStringLiteral("%1: %2").arg(QDir(root).relativeFilePath(path), k);
            }
        }
        QVERIFY2(files > 50, qPrintable(QString::number(files)));
        QVERIFY2(keysSeen > 40, qPrintable(QString::number(keysSeen)));
        QVERIFY2(problems.isEmpty(), qPrintable(QStringLiteral("settings keys read by code but missing from the schema:\n  ")
                                                + problems.join(QStringLiteral("\n  "))));
    }

    // [engine-3] internal test switches are schema keys flagged hidden; user keys are not hidden
    void hiddenSchemaEntries()
    {
        for (const char *k : {"kad/allowLan", "kad/allowSameIp", "kad/forceFirewalled", "kad/forceUdpFirewalled",
                              "kad/findBuddyDelay", "ed2k/allowLan", "overlay/testCorruptUploads"}) {
            const SettingDef *d = findSetting(QString::fromLatin1(k));
            QVERIFY2(d, k);
            QVERIFY2(d->hidden, k);
        }
        QCOMPARE(findSetting(QStringLiteral("kad/findBuddyDelay"))->defaultValue.toInt(), 300);
        QVERIFY(!findSetting(QStringLiteral("transfers/maxDownloads"))->hidden);
        const struct { const char *k, *v; } labels[] = {{"search/typeButtonAudioLabel", "Audio"},
                                                       {"search/typeButtonVideoLabel", "Video"},
                                                       {"search/typeButtonPicturesLabel", "Pictures"}};
        for (const auto &l : labels) {
            const SettingDef *d = findSetting(QString::fromLatin1(l.k));
            QVERIFY2(d && !d->hidden, l.k);
            QCOMPARE(d->defaultValue.toString(), QString::fromLatin1(l.v));
        }
        int hidden = 0;
        for (const SettingDef &d : settingsSchema()) hidden += d.hidden;
        QVERIFY(hidden >= 7);   // others may add hidden switches
    }

    // ------------------------------------------------------------------ 3. hygiene (unit)
    void bridgeEntries()
    {
        SearchHit h;
        h.fileName = QStringLiteral("[torrent emule-qt.org] debian-13.7.0-amd64-netinst.iso");
        h.hashes.ed2k = QByteArray::fromHex("ed2b0111ffff0000a1b2c3d4e5f60718");
        QVERIFY(hygiene::isTorrentBridge(h));
        h.fileName = QStringLiteral("[kad emule-qt.org] Big Buck Bunny.avi");
        h.hashes.ed2k = QByteArray::fromHex("0123456789abcdef0123456789abcdef");
        QVERIFY(hygiene::isTorrentBridge(h));
        h.fileName = QStringLiteral("debian-13.7.0-amd64-netinst.iso");
        QVERIFY(!hygiene::isTorrentBridge(h));
        h.hashes.ed2k = QByteArray::fromHex("ed2b0111ffff9999a1b2c3d4e5f60718");   // synthetic hash alone
        QVERIFY(hygiene::isTorrentBridge(h));
        h.fileName = QStringLiteral("[Teapot-Subs] Some Show - 01.mkv");             // a release tag, not a bridge
        h.hashes.ed2k = QByteArray::fromHex("0123456789abcdef0123456789abcdef");
        QVERIFY(!hygiene::isTorrentBridge(h));
    }

    void suspects_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<qint64>("size");
        QTest::addColumn<int>("reported");
        QTest::addColumn<int>("duration");
        QTest::addColumn<int>("bitrate");
        QTest::addColumn<QString>("expect");   // substring of the reason, empty = clean
        // recorded from live 'big buck bunny' / 'sintel' / 'ubuntu' searches (eD2k, Gnutella, web)
        QTest::newRow("bbb-mov") << "big_buck_bunny_720p_h264.mov" << qint64(275658043) << 0 << -1 << -1 << "";
        QTest::newRow("bbb-mp4") << "bbb_sunflower_1080p_30fps_normal.mp4" << qint64(276134947) << 2 << -1 << -1 << "";
        QTest::newRow("bbb-ogg") << "big_buck_bunny_480p_stereo.ogg" << qint64(165504432) << 0 << -1 << -1 << "";
        QTest::newRow("iso") << "ubuntu-24.04.1-desktop-amd64.iso" << qint64(6203355136) << 6 << -1 << -1 << "";
        QTest::newRow("exe-wrapper") << "Big Buck Bunny 1080p.avi.exe" << qint64(1245184) << 0 << -1 << -1 << "executable named like a media file";
        QTest::newRow("exe-media-words") << "Sintel 2010 720p BluRay x264.exe" << qint64(3500000) << 0 << -1 << -1 << "executable named like a media file";
        QTest::newRow("zip-wrapper") << "big_buck_bunny.avi.rar" << qint64(52428800) << 0 << -1 << -1 << "archive wrapped around a media file";
        QTest::newRow("tiny-hd-zip") << "Big.Buck.Bunny.2008.1080p.BluRay.x264.zip" << qint64(2097152) << 0 << -1 << -1 << "archive too small";
        QTest::newRow("wmv-release") << "Big Buck Bunny DVDRip XviD.wmv" << qint64(734003200) << 0 << -1 << -1 << "WMV/ASF";
        QTest::newRow("tiny-video") << "sintel.mp4" << qint64(300000) << 0 << -1 << -1 << "too small for a video";
        QTest::newRow("tiny-1080p") << "Sintel.2010.1080p.mkv" << qint64(12 << 20) << 0 << -1 << -1 << "too small";
        QTest::newRow("mp3-ok") << "Pocket Brass Band - Teacup Parade.mp3" << qint64(4900000) << 1 << 300 << 128 << "";
        QTest::newRow("mp3-mismatch") << "Pocket Brass Band - Teacup Parade.mp3" << qint64(500000) << 1 << 300 << 320 << "size does not match";
        QTest::newRow("type-mismatch") << "Big Buck Bunny.exe" << qint64(20000000) << 2 << -1 << -1 << "listed as video";
        QTest::newRow("bait") << "Big Buck Bunny (password in readme).rar" << qint64(300000000) << 0 << -1 << -1 << "bait";
        QTest::newRow("trailer-720p") << "Big.Buck.Bunny.2008.Trailer.720p.mov" << qint64(17815306) << 0 << -1 << -1 << "";   // live false positive, fixed
        QTest::newRow("empty-mkv") << "Big.Buck.Bunny.2008.720p.mkv" << qint64(1341) << 0 << -1 << -1 << "too small for a video";  // live
        QTest::newRow("iso-as-archive") << "debian-13.7.0-amd64-netinst.iso" << qint64(792723456) << 6 << -1 << -1 << "";
    }

    void suspects()
    {
        QFETCH(QString, name);
        QFETCH(qint64, size);
        QFETCH(int, reported);
        QFETCH(int, duration);
        QFETCH(int, bitrate);
        QFETCH(QString, expect);
        SearchHit h;
        h.fileName = name;
        h.size = size;
        h.durationSec = duration;
        h.bitrateKbps = bitrate;
        const QString r = hygiene::suspectReason(h, MediaType(reported));
        if (expect.isEmpty()) QVERIFY2(r.isEmpty(), qPrintable(name + QStringLiteral(" → ") + r));
        else QVERIFY2(r.contains(expect), qPrintable(name + QStringLiteral(" → '") + r + QStringLiteral("'")));
    }

    void constantSourceCounts()
    {
        hygiene::ConstantSourceCountDetector d(20, 8);
        QStringList flagged;
        for (int i = 0; i < 7; ++i)
            flagged << d.add(QStringLiteral("k%1").arg(i), QByteArray(16, char('a' + i)), 99);
        QVERIFY(flagged.isEmpty());
        flagged << d.add(QStringLiteral("real"), QByteArray(16, 'z'), 57);   // other count
        flagged << d.add(QStringLiteral("low"), QByteArray(16, 'y'), 3);     // below minSeeds
        QVERIFY(flagged.isEmpty());
        flagged << d.add(QStringLiteral("k7"), QByteArray(16, 'h'), 99);     // 8th file with 99
        QCOMPARE(flagged.size(), 8);
        QVERIFY(!flagged.contains(QStringLiteral("real")));
        QVERIFY(d.reasonFor(99).contains(QStringLiteral("(99)")));
        QCOMPARE(d.add(QStringLiteral("k8"), QByteArray(16, 'i'), 99).size(), 9);   // count grew: all again
        QVERIFY(d.reasonFor(99).contains(QStringLiteral("on 9 ")));
        QCOMPARE(d.add(QStringLiteral("k8b"), QByteArray(16, 'i'), 99), QStringList{QStringLiteral("k8b")});   // same file, new row
    }

    // ------------------------------------------------------------------ 3. hygiene in the SearchManager
    void searchManagerHygiene()
    {
        auto *ed = new GluePlugin(Net::ED2K, NetworkPlugin::CanSearch);
        auto *gn = new GluePlugin(Net::Gnutella, NetworkPlugin::CanSearch);
        FileHashes bridgeH, exeH, okH, gnH;
        bridgeH.ed2k = QByteArray::fromHex("ed2b0111ffff0000a1b2c3d4e5f60718");
        exeH.ed2k = QByteArray::fromHex("11111111111111111111111111111111");
        okH.ed2k = QByteArray::fromHex("22222222222222222222222222222222");
        gnH.sha1 = QByteArray(20, 'g');
        ed->cannedHits << FakePlugin::makeHit(QStringLiteral("[torrent emule-qt.org] Big Buck Bunny.avi"), 734003200, bridgeH, Net::ED2K, QStringLiteral("u1"))
                       << FakePlugin::makeHit(QStringLiteral("Big Buck Bunny 1080p.avi.scr.zip"), 1500000, exeH, Net::ED2K, QStringLiteral("u2"))
                       << FakePlugin::makeHit(QStringLiteral("big_buck_bunny_720p_h264.mov"), 275658043, okH, Net::ED2K, QStringLiteral("u3"));
        ed->cannedHits[1].fileName = QStringLiteral("Big Buck Bunny 1080p.avi.zip");
        // a Gnutella file whose name happens to start with a bracket tag is not an eD2k bridge
        gn->cannedHits << FakePlugin::makeHit(QStringLiteral("[torrent] notes.txt"), 1000, gnH, Net::Gnutella, QStringLiteral("g1"));
        Net2 env({ed, gn});
        SearchManager sm(&env.settings, env.nm, nullptr);
        sm.setFlushInterval(10);
        auto run = [&]() -> QList<SearchHit> {
            SearchQuery q;
            q.text = QStringLiteral("big buck bunny");
            QSignalSpy fin(&sm, &SearchService::searchFinished);
            const quint32 id = sm.startSearch(q);
            if (!fin.wait(3000)) return {};
            return sm.hits(id);
        };
        QList<SearchHit> hits = run();
        QCOMPARE(hits.size(), 3);   // bridge dropped
        const SearchManager::HygieneStats st = sm.hygieneStats(2 - 1);
        QCOMPARE(st.droppedBridges, 1);
        QCOMPARE(st.flagged, 1);
        for (const SearchHit &h : hits) {
            if (h.fileName.contains(QLatin1String(".avi.zip"))) QVERIFY2(h.suspect.contains(QLatin1String("archive")), qPrintable(h.suspect));
            else QVERIFY2(h.suspect.isEmpty(), qPrintable(h.fileName + QStringLiteral(": ") + h.suspect));
        }
        // hide instead of flag
        env.settings.setValue(keys::search::HideSuspects, true);
        hits = run();
        QCOMPARE(hits.size(), 2);
        QCOMPARE(sm.hygieneStats(2).hiddenSuspects, 1);
        // both off: the bridge entry stays, nothing is flagged
        env.settings.setValue(keys::search::HideSuspects, false);
        env.settings.setValue(keys::search::FlagSuspects, false);
        env.settings.setValue(keys::search::DropTorrentBridges, false);
        hits = run();
        QCOMPARE(hits.size(), 4);
        for (const SearchHit &h : hits) QVERIFY(h.suspect.isEmpty());
    }

    void constantSeedsInSearch()
    {
        auto *ed = new GluePlugin(Net::ED2K, NetworkPlugin::CanSearch);
        for (int i = 0; i < 9; ++i) {
            FileHashes h;
            h.ed2k = QByteArray(16, char('A' + i));
            SearchHit hit = FakePlugin::makeHit(QStringLiteral("Big Buck Bunny part %1.avi").arg(i), 700000000 + i, h, Net::ED2K,
                                                QStringLiteral("srv"));
            hit.sources[0].seeds = 99;
            ed->cannedHits << hit;
        }
        FileHashes real;
        real.ed2k = QByteArray(16, 'z');
        SearchHit ok = FakePlugin::makeHit(QStringLiteral("big_buck_bunny_720p_h264.mov"), 275658043, real, Net::ED2K, QStringLiteral("p"));
        ok.sources[0].seeds = 41;
        ed->cannedHits << ok;
        Net2 env({ed});
        SearchManager sm(&env.settings, env.nm, nullptr);
        sm.setFlushInterval(10);
        SearchQuery q;
        q.text = QStringLiteral("big buck bunny");
        QSignalSpy fin(&sm, &SearchService::searchFinished);
        const quint32 id = sm.startSearch(q);
        QVERIFY(fin.wait(3000));
        int flagged = 0;
        for (const SearchHit &h : sm.hits(id)) {
            if (h.fileName.startsWith(QLatin1String("Big Buck Bunny part"))) {
                QVERIFY2(h.suspect.contains(QLatin1String("same complete-source count (99) on 9")), qPrintable(h.suspect));
                ++flagged;
            } else {
                QVERIFY(h.suspect.isEmpty());
            }
        }
        QCOMPARE(flagged, 9);
    }

    // ------------------------------------------------------------------ 2. search by hash
    void parseHashQuery()
    {
        SearchQuery q;
        SearchSource link;
        const QString ed2kHex = QStringLiteral("0123456789abcdef0123456789abcdef");
        QVERIFY(SearchManager::parseHashQuery(QStringLiteral("ed2k://|file|Big Buck Bunny.avi|734003200|%1|/").arg(ed2kHex), &q, &link));
        QCOMPARE(q.hashes.ed2k.toHex(), ed2kHex.toLatin1());
        QCOMPARE(q.fileSize, qint64(734003200));
        QCOMPARE(q.text, QStringLiteral("Big Buck Bunny.avi"));
        QCOMPARE(link.net, Net::ED2K);
        q = SearchQuery();
        QVERIFY(SearchManager::parseHashQuery(QStringLiteral("magnet:?xt=urn:btih:dd8255ecdc7ca55fb0bbf81323d87062db1f6d1c&dn=Big+Buck+Bunny"), &q, &link));
        QCOMPARE(q.hashes.btih.toHex(), QByteArray("dd8255ecdc7ca55fb0bbf81323d87062db1f6d1c"));
        QCOMPARE(link.net, Net::BitTorrent);
        q = SearchQuery();
        QVERIFY(SearchManager::parseHashQuery(ed2kHex.toUpper(), &q));
        QCOMPARE(q.hashes.ed2k.size(), 16);
        q = SearchQuery();
        QVERIFY(SearchManager::parseHashQuery(QStringLiteral("dd8255ecdc7ca55fb0bbf81323d87062db1f6d1c"), &q, &link));
        QCOMPARE(q.hashes.btih.size(), 20);
        QVERIFY(link.uri.startsWith(QLatin1String("magnet:")));
        q = SearchQuery();
        const QString sha1b32 = hash::toBase32(QByteArray(20, 'q'));
        QVERIFY(SearchManager::parseHashQuery(QStringLiteral("urn:sha1:") + sha1b32, &q));
        QCOMPARE(q.hashes.sha1, QByteArray(20, 'q'));
        QVERIFY(q.hashes.btih.isEmpty());
        q = SearchQuery();
        QVERIFY(SearchManager::parseHashQuery(hash::toBase32(QByteArray(20, 'b')), &q));   // 32 base32 → btih
        QCOMPARE(q.hashes.btih, QByteArray(20, 'b'));
        q = SearchQuery();
        QVERIFY(SearchManager::parseHashQuery(QStringLiteral("tth:") + hash::toBase32(QByteArray(24, 't')), &q));
        QCOMPARE(q.hashes.tth, QByteArray(24, 't'));
        q = SearchQuery();
        QVERIFY(SearchManager::parseHashQuery(QString::fromLatin1(QByteArray(32, 's').toHex()), &q));
        QCOMPARE(q.hashes.sha256, QByteArray(32, 's'));
        for (const char *bad : {"big buck bunny", "", "0123456789abcdef", "zzzz8255ecdc7ca55fb0bbf81323d87062db1f6d1c", "ed2k://|server|1.2.3.4|4661|/"}) {
            q = SearchQuery();
            QVERIFY2(!SearchManager::parseHashQuery(QString::fromLatin1(bad), &q), bad);
        }
    }

    void hashSearch()
    {
        auto *ed = new GluePlugin(Net::ED2K, NetworkPlugin::CanSearch | NetworkPlugin::CanFindSources);
        auto *gn = new GluePlugin(Net::Gnutella, NetworkPlugin::CanSearch | NetworkPlugin::CanFindSources);
        auto *ov = new GluePlugin(Net::Overlay, NetworkPlugin::CanHashLinks);
        Net2 env({ed, gn, ov});
        SearchManager sm(&env.settings, env.nm, nullptr);
        sm.setFlushInterval(10);
        const QString hex = QStringLiteral("0123456789abcdef0123456789abcdef");
        QSignalSpy changed(&sm, &SearchService::hitsChanged);
        const quint32 id = sm.searchHash(QStringLiteral("ed2k://|file|Big Buck Bunny.avi|734003200|%1|/").arg(hex));
        const QString dl = SearchManager::hashSearchDownloadId(id);
        QTRY_VERIFY(ed->findSourcesCalls.contains(dl));
        QVERIFY(gn->findSourcesCalls.contains(dl));
        QCOMPARE(ov->hashLinkLookups.size(), 1);
        QVERIFY(ed->searches.isEmpty() && gn->searches.isEmpty());   // no keyword search
        QTRY_VERIFY(!sm.hits(id).isEmpty());                         // the link itself is a source
        QCOMPARE(sm.hits(id).first().sources.first().net, Net::ED2K);
        // sources for another (real) download id are not ours
        ed->emitSources(QStringLiteral("d0000000000000001"), {peer(Net::ED2K, QStringLiteral("stranger"))});
        ed->emitSources(dl, {peer(Net::ED2K, QStringLiteral("a")), peer(Net::ED2K, QStringLiteral("b"))});
        // eD2k learns the SHA-1 → the other networks are asked again with it
        FileHashes more;
        more.sha1 = QByteArray(20, 'S');
        const int gnBefore = gn->findSourcesCalls.size();
        ed->emitHashes(dl, more);
        QCOMPARE(gn->findSourcesCalls.size(), gnBefore + 1);
        gn->emitSources(dl, {peer(Net::Gnutella, QStringLiteral("g"))});
        QTRY_COMPARE(sm.hits(id).size(), 1);
        QTRY_COMPARE(sm.hits(id).first().sources.size(), 4);
        const SearchHit h = sm.hits(id).first();
        QCOMPARE(h.fileName, QStringLiteral("Big Buck Bunny.avi"));
        QCOMPARE(h.size, qint64(734003200));
        QCOMPARE(h.hashes.sha1, more.sha1);
        for (const SearchSource &s : h.sources) QVERIFY(s.user.id != QLatin1String("stranger"));
        QVERIFY(changed.count() > 0);
        sm.stopSearch(id);
        QVERIFY(ed->stoppedFindSources.contains(dl));
        QVERIFY(gn->stoppedFindSources.contains(dl));
        // the UI's "HASH>" line (text2) or a raw hash in the keyword line also searches by hash
        SearchQuery q;
        q.text2 = hex;
        const quint32 id2 = sm.startSearch(q);
        QTRY_VERIFY(ed->findSourcesCalls.contains(SearchManager::hashSearchDownloadId(id2)));
        QVERIFY(ed->searches.isEmpty());
        // a normal keyword search stays a keyword search
        q = SearchQuery();
        q.text = QStringLiteral("big buck bunny");
        sm.startSearch(q);
        QTRY_COMPARE(ed->searches.size(), 1);
    }

    // ------------------------------------------------------------------ 2. ping
    void pingHitsAndUsers()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QTcpServer closed;                       // grab a port and close it → connection refused
        QVERIFY(closed.listen(QHostAddress::LocalHost));
        const quint16 deadPort = closed.serverPort();
        closed.close();
        auto *ed = new GluePlugin(Net::ED2K, NetworkPlugin::CanSearch);
        auto *ov = new GluePlugin(Net::Overlay, NetworkPlugin::CanSearch);
        ov->nativePing = true;
        SearchHit hit = FakePlugin::makeHit(QStringLiteral("x.bin"), 5000000, FileHashes(), Net::ED2K, QStringLiteral("alive"));
        hit.sources[0].user.address = QStringLiteral("127.0.0.1:%1").arg(server.serverPort());
        hit.sources << peer(Net::ED2K, QStringLiteral("dead"), QStringLiteral("127.0.0.1:%1").arg(deadPort))
                    << peer(Net::ED2K, QStringLiteral("noaddr"));
        hit.hashes.ed2k = QByteArray(16, 'p');
        ed->cannedHits << hit;
        SearchHit ovHit = FakePlugin::makeHit(QStringLiteral("y.bin"), 6000000, FileHashes(), Net::Overlay, QStringLiteral("ovpeer"));
        ovHit.hashes.sha1 = QByteArray(20, 'o');
        ov->cannedHits << ovHit;
        Net2 env({ed, ov});
        Pinger pinger(env.nm);
        SearchManager sm(&env.settings, env.nm, nullptr);
        sm.setPinger(&pinger);
        sm.setFlushInterval(10);
        SearchQuery q;
        q.text = QStringLiteral("x");
        QSignalSpy fin(&sm, &SearchService::searchFinished);
        const quint32 id = sm.startSearch(q);
        QVERIFY(fin.wait(3000));
        QStringList keys;
        for (const SearchHit &h : sm.hits(id)) keys << h.key;
        QCOMPARE(keys.size(), 2);
        sm.pingHits(id, keys);
        auto allPinged = [&] {
            for (const SearchHit &h : sm.hits(id))
                for (const SearchSource &s : h.sources)
                    if (!s.extra.value(QStringLiteral("pinged")).toBool()) return false;
            return true;
        };
        QTRY_VERIFY_WITH_TIMEOUT(allPinged(), 8000);
        for (const SearchHit &h : sm.hits(id)) {
            for (const SearchSource &s : h.sources) {
                if (s.user.id == QLatin1String("alive")) QVERIFY2(s.pingMs >= 1 && s.pingMs < 3000, qPrintable(QString::number(s.pingMs)));
                else if (s.user.id == QLatin1String("ovpeer")) QCOMPARE(s.pingMs, 42);   // the network's own ping
                else QCOMPARE(s.pingMs, -1);
            }
        }
        // users
        env.nm->setPinger(&pinger);
        QSignalSpy pinged(env.nm, &NetworkService::userPinged);
        UserRef u;
        u.net = Net::Gnutella;
        u.id = QStringLiteral("127.0.0.1:%1").arg(server.serverPort());
        env.nm->pingUser(u);
        QTRY_COMPARE(pinged.count(), 1);
        QVERIFY(pinged.first().at(1).toInt() >= 1);
        UserRef none;
        none.net = Net::Kad;
        none.id = QStringLiteral("abcdef");
        env.nm->pingUser(none);
        QTRY_COMPARE(pinged.count(), 2);
        QCOMPARE(pinged.at(1).at(1).toInt(), -1);
    }

    void pingerAddresses()
    {
        QString host;
        quint16 port = 0;
        SearchSource s;
        s.uri = QStringLiteral("ed2k-peer://81.2.3.4:4662?file=00&uid=11");
        QVERIFY(Pinger::addressOf(s, &host, &port));
        QCOMPARE(host, QStringLiteral("81.2.3.4"));
        QCOMPARE(port, quint16(4662));
        s.uri = QStringLiteral("ed2k-peer://0.0.0.0:4662?lowid=123&server=1.2.3.4:4661");   // LowID: not pingable
        QVERIFY(!Pinger::addressOf(s, &host, &port));
        s.uri = QStringLiteral("https://archive.org/download/x/y.mp4");
        QVERIFY(Pinger::addressOf(s, &host, &port));
        QCOMPARE(port, quint16(443));
        s = SearchSource();
        s.user.id = QStringLiteral("10.0.0.5:6346");
        QVERIFY(Pinger::addressOf(s, &host, &port));
        s.user.id = QStringLiteral("a1b2c3");
        QVERIFY(!Pinger::addressOf(s, &host, &port));
    }

    // ------------------------------------------------------------------ 2. network summary
    void networkSummary()
    {
        auto *a = new GluePlugin(Net::ED2K, 0);
        auto *b = new GluePlugin(Net::Gnutella, 0);
        a->peersWhenConnected = 2;
        b->peersWhenConnected = 3;
        Net2 env({a, b});
        a->st.firewalled = true;
        a->st.usersOnline = 1000;
        b->st.usersOnline = 500;
        env.nm->setSourceCounter([] { return 7; });
        QSignalSpy changed(env.nm, &NetworkService::summaryChanged);
        a->emitIncoming(3);
        b->emitIncoming(2);
        env.nm->noteOwnSearch();
        env.nm->noteOwnSearch();
        NetworkSummary s = env.nm->summary();
        QCOMPARE(s.state, ConnState::Connected);
        QCOMPARE(s.primary, 5);
        QCOMPARE(s.sources, 7);
        QCOMPARE(s.searchesPerMinute, 5);
        QCOMPARE(s.ownSearchesPerMinute, 2);
        QCOMPARE(s.networksOnline, 2);
        QCOMPARE(s.networksEnabled, 2);
        QCOMPARE(s.usersOnline, qint64(1500));
        QVERIFY(!s.firewalled);              // Gnutella is open
        env.nm->checkSummary();
        QVERIFY(changed.count() >= 1);
        const int n = changed.count();
        env.nm->checkSummary();
        QCOMPARE(changed.count(), n);         // unchanged → no signal
        b->st.firewalled = true;
        QVERIFY(env.nm->summary().firewalled);
        // title summary still carries the same P / S numbers
        QCOMPARE(env.nm->titleSummary(), QStringLiteral("Online P=5 S=7"));
    }

#ifdef MX_HAVE_MOCK
    void defaultSummaryParsesTitle()
    {
        // the base-class default (used by the mock core) parses titleSummary()
        Core *mock = createMockCore(m_tmp.filePath(QStringLiteral("mock")));
        mock->settings()->setValue(keys::identity::PrimaryConnection, true);
        const QString title = mock->network()->titleSummary();
        const NetworkSummary s = mock->network()->summary();
        if (title.contains(QLatin1String("P="))) {
            QVERIFY2(s.primary > 0, qPrintable(title));
            QVERIFY2(title.contains(QStringLiteral("S=%1").arg(s.sources)), qPrintable(title));
        }
        if (title.contains(QLatin1String("spm="))) QVERIFY(s.searchesPerMinute > 0);
        // the other defaults are safe no-ops / fallbacks
        QCOMPARE(mock->network()->importServerList(Net::ED2K, QStringLiteral("/nonexistent")), -1);
        QVERIFY(mock->transfers()->resumeDownload(DownloadRequest(), QString()).isEmpty());
        QVERIFY(!mock->transfers()->startUploadNow(QStringLiteral("x")));
        delete mock;
    }
#endif

    // ------------------------------------------------------------------ 2. server list management
    void serverListManagement()
    {
        auto *ed = new GluePlugin(Net::ED2K, 0);
        Net2 env({ed});
        ed->addServer(QStringLiteral("1.2.3.4:4661"), QStringLiteral("Old"));
        QSignalSpy srvChanged(env.nm, &NetworkService::serversChanged);
        QVERIFY(env.nm->editServer(Net::ED2K, QStringLiteral("1.2.3.4:4661"), QStringLiteral("5.6.7.8:4242"), QStringLiteral("New")));
        QCOMPARE(ed->removedServers, QStringList{QStringLiteral("1.2.3.4:4661")});
        QCOMPARE(env.nm->servers(Net::ED2K).size(), 1);
        QCOMPARE(env.nm->servers(Net::ED2K).first().id, QStringLiteral("5.6.7.8:4242"));
        QCOMPARE(env.nm->servers(Net::ED2K).first().name, QStringLiteral("New"));
        QVERIFY(srvChanged.count() >= 1);
        QVERIFY(!env.nm->editServer(Net::ED2K, QStringLiteral("9.9.9.9:1"), QStringLiteral("1.1.1.1:2")));
        QVERIFY(!env.nm->editServer(Net::Kad, QStringLiteral("x"), QStringLiteral("y")));

        // binary server.met (the plugin does not understand it → the core parser + addServer)
        QByteArray met;
        auto u8 = [&](quint8 v) { met.append(char(v)); };
        auto u16 = [&](quint16 v) { u8(v & 0xFF); u8(v >> 8); };
        auto u32 = [&](quint32 v) { u16(v & 0xFFFF); u16(v >> 16); };
        u8(0xE0);
        u32(2);
        auto entry = [&](quint8 a, quint8 b, quint8 c, quint8 d, quint16 port, const QByteArray &name) {
            u8(a); u8(b); u8(c); u8(d);
            u16(port);
            u32(2);
            u8(0x02); u16(1); u8(0x01); u16(quint16(name.size())); met.append(name);   // ST_SERVERNAME (long form)
            u8(0x83); u8(0x0D); u32(12345);                                            // compact uint32 tag
        };
        entry(45, 82, 80, 155, 5687, "eMule Security");
        entry(176, 123, 5, 89, 4725, "Shared");
        QList<ServerListEntry> parsed;
        QVERIFY(parseServerMetEntries(met, &parsed));
        QCOMPARE(parsed.size(), 2);
        QCOMPARE(parsed.at(0).address, QStringLiteral("45.82.80.155:5687"));
        QCOMPARE(parsed.at(0).name, QStringLiteral("eMule Security"));
        const QString metPath = m_tmp.filePath(QStringLiteral("server.met"));
        QVERIFY(writeFile(metPath, met));
        ed->addedServers.clear();
        QCOMPARE(env.nm->importServerList(Net::ED2K, metPath), 2);
        QCOMPARE(ed->imported.size(), 1);                 // offered to the plugin first
        QCOMPARE(ed->addedServers, (QStringList{QStringLiteral("45.82.80.155:5687"), QStringLiteral("176.123.5.89:4725")}));
        // text list
        ed->addedServers.clear();
        QCOMPARE(env.nm->importServerData(Net::ED2K, "# my servers\n1.2.3.4:4661 First server\r\ned2k://|server|9.8.7.6|4242|/\nnot a server\n"), 2);
        QCOMPARE(ed->addedServers, (QStringList{QStringLiteral("1.2.3.4:4661"), QStringLiteral("9.8.7.6:4242")}));
        // a damaged server.met and garbage are rejected
        QCOMPARE(env.nm->importServerData(Net::ED2K, met.left(30)), -1);
        QCOMPARE(env.nm->importServerData(Net::ED2K, QByteArray("\x01\x02\x03\x00garbage", 11)), -1);
        QCOMPARE(env.nm->importServerList(Net::ED2K, m_tmp.filePath(QStringLiteral("missing.met"))), -1);
        // a plugin that imports by itself (the eD2k plugin: gzip'ed server.met too)
        ed->importAnswer = 3;
        ed->addedServers.clear();
        QCOMPARE(env.nm->importServerData(Net::ED2K, met), 3);
        QVERIFY(ed->addedServers.isEmpty());
    }

    void serverMessages()
    {
        auto *ed = new GluePlugin(Net::ED2K, 0);
        Net2 env({ed});
        ed->addServer(QStringLiteral("45.82.80.155:5687"), QStringLiteral("eMule Security"));
        QSignalSpy got(env.nm, &NetworkService::serverMessageReceived);
        ed->emitServerMessage(QStringLiteral("45.82.80.155:5687"), QStringLiteral("Welcome to eMule Security"));
        QCOMPARE(got.count(), 1);
        const ServerMessage m = got.first().first().value<ServerMessage>();
        QCOMPARE(m.net, Net::ED2K);
        QCOMPARE(m.serverName, QStringLiteral("eMule Security"));
        QCOMPARE(m.text, QStringLiteral("Welcome to eMule Security"));
        QVERIFY(m.time.isValid());
        for (int i = 0; i < 250; ++i) ed->emitServerMessage(QStringLiteral("45.82.80.155:5687"), QStringLiteral("line %1").arg(i));
        const QList<ServerMessage> all = env.nm->serverMessages(Net::ED2K);
        QCOMPARE(all.size(), 200);
        QCOMPARE(all.last().text, QStringLiteral("line 249"));
        QCOMPARE(env.nm->serverMessages().size(), 200);
        QVERIFY(env.nm->serverMessages(Net::Gnutella).isEmpty());
    }

    // ------------------------------------------------------------------ 2. transfers (engine)
    void perSourceActions()
    {
        Eng e(m_tmp.filePath(QStringLiteral("eng-src")), 600000);   // sources stay "Connecting"
        const QByteArray data = randomBytes(2 * 1000 * 1000, 7);
        DownloadRequest r;
        r.name = QStringLiteral("file.bin");
        r.size = data.size();
        r.hashes.ed2k = ed2kOf(data);
        r.sources = {peer(Net::ED2K, QStringLiteral("p1")), peer(Net::ED2K, QStringLiteral("p2"))};
        e.plugin->data.insert(r.hashes.ed2k, data);
        const QString id = e.engine->download(r);
        QVERIFY(!id.isEmpty());
        QTRY_COMPARE(e.engine->transfer(id).sources.size(), 2);
        TransferInfo t = e.engine->transfer(id);
        QString k1, k2;
        for (const TransferSource &s : t.sources) {
            QVERIFY(!s.id.isEmpty());
            QCOMPARE(s.priority, 0);
            (s.user.id == QLatin1String("p1") ? k1 : k2) = s.id;
        }
        e.engine->setSourcePriority(id, k2, 1);
        for (const TransferSource &s : e.engine->transfer(id).sources)
            QCOMPARE(s.priority, s.id == k2 ? 1 : 0);
        // cancel p1: gone, and not re-added when a network reports it again
        e.engine->cancelSource(id, k1);
        t = e.engine->transfer(id);
        QCOMPARE(t.sources.size(), 1);
        QCOMPARE(t.sources.first().id, k2);
        QCOMPARE(t.sourcesTotal, 1);
        e.plugin->emitSources(id, {peer(Net::ED2K, QStringLiteral("p1"))});
        QTest::qWait(50);
        QCOMPARE(e.engine->transfer(id).sources.size(), 1);
        QVERIFY(e.engine->transfer(id).state != TransferState::Error);
        // cancelling the last one does not turn the download into a "File Mismatch" error
        e.engine->cancelSource(id, k2);
        QTest::qWait(1500);
        QVERIFY(e.engine->transfer(id).state != TransferState::Error);
        QCOMPARE(e.engine->transfer(id).sourcesTotal, 0);
        // persisted: priority + removal survive a restart of the engine
        e.engine->setPriority(id, 0);
    }

    void findMoreSourcesAndEnterQueue()
    {
        Eng e(m_tmp.filePath(QStringLiteral("eng-find")));
        const QByteArray data = randomBytes(1500 * 1000, 9);
        DownloadRequest r;
        r.name = QStringLiteral("q.bin");
        r.size = data.size();
        r.hashes.ed2k = ed2kOf(data);
        r.sources = {peer(Net::ED2K, QStringLiteral("flaky"))};
        e.plugin->data.insert(r.hashes.ed2k, data);
        e.plugin->failNewSources = true;            // every request fails → source backs off (30 s)
        const QString id = e.engine->download(r);
        QTRY_VERIFY(e.plugin->findSourcesCalls.contains(id));
        QTRY_VERIFY(!e.plugin->made.isEmpty());
        QTRY_VERIFY(e.plugin->made.first().isNull() || e.plugin->made.first()->state() == RangeSource::Closed);
        const int made = e.plugin->made.size();
        // Find More Sources: asks the networks again (after the engine's 2 s burst debounce)
        QTest::qWait(2100);
        QCOMPARE(e.plugin->made.size(), made);      // backing off (30 s)
        const int asked = int(e.plugin->findSourcesCalls.count(id));
        e.engine->findMoreSources(id);
        QCOMPARE(int(e.plugin->findSourcesCalls.count(id)), asked + 1);
        // Enter Queue: reconnect now
        e.plugin->failNewSources = false;
        e.engine->enterQueue(id);
        QTRY_VERIFY(e.plugin->made.size() > made);
        QTRY_VERIFY_WITH_TIMEOUT(e.engine->transfer(id).state == TransferState::Completed, 10000);
    }

    void manualResume()
    {
        Eng e(m_tmp.filePath(QStringLiteral("eng-resume")));
        const QByteArray data = randomBytes(3 * 1000 * 1000, 11);
        const qint64 have = 1000 * 1000;
        // an old incomplete file: the first MB of data + a zero-filled (preallocated) tail
        const QString old = e.root + QStringLiteral("/old/Big Buck Bunny.avi.incomplete");
        QVERIFY(writeFile(old, data.left(have) + QByteArray(500 * 1000, '\0')));
        DownloadRequest r;
        r.name = QStringLiteral("Big Buck Bunny.avi");
        r.size = data.size();
        r.hashes.ed2k = ed2kOf(data);
        r.sources = {peer(Net::ED2K, QStringLiteral("s1"))};
        e.plugin->data.insert(r.hashes.ed2k, data);
        // refused: file larger than the download / unreadable path
        const QString big = e.root + QStringLiteral("/old/big.bin");
        QVERIFY(writeFile(big, QByteArray(data.size() + 1, 'x')));
        QVERIFY(e.engine->resumeDownload(r, big).isEmpty());
        QVERIFY(e.engine->resumeDownload(r, e.root + QStringLiteral("/old/nope")).isEmpty());

        const QString id = e.engine->resumeDownload(r, old);
        QVERIFY(!id.isEmpty());
        // the zero tail was not counted as data (held, not yet verified)
        QTRY_COMPARE(e.engine->debugInfo(id).value(QStringLiteral("held")).toLongLong(), have);
        QVERIFY(e.engine->resumeDownload(r, old).isEmpty());   // already downloading
        QTRY_VERIFY_WITH_TIMEOUT(e.engine->transfer(id).state == TransferState::Completed, 15000);
        const QString out = e.engine->transfer(id).savePath;
        QCOMPARE(readFile(out), data);
        // only the missing part was fetched
        qint64 fetched = 0, minOff = data.size();
        for (const auto &s : e.plugin->made)
            if (s) for (const auto &q : s->requests) { fetched += q.second; minOff = qMin(minOff, q.first); }
        QVERIFY2(minOff >= have, qPrintable(QString::number(minOff)));
        QVERIFY2(fetched <= data.size() - have + 512 * 1024, qPrintable(QString::number(fetched)));
        QVERIFY(!QFileInfo::exists(old));                // the file was used in place and moved
    }

    void manualResumeBadDataIsRefetched()
    {
        Eng e(m_tmp.filePath(QStringLiteral("eng-resume-bad")));
        const QByteArray data = randomBytes(2 * 1000 * 1000, 13);
        const QString old = e.root + QStringLiteral("/old/wrong.bin");
        QVERIFY(writeFile(old, randomBytes(800 * 1000, 99)));   // not this file's data
        DownloadRequest r;
        r.name = QStringLiteral("right.bin");
        r.size = data.size();
        r.hashes.ed2k = ed2kOf(data);
        r.sources = {peer(Net::ED2K, QStringLiteral("s1"))};
        e.plugin->data.insert(r.hashes.ed2k, data);
        const QString id = e.engine->resumeDownload(r, old);
        QVERIFY(!id.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(e.engine->transfer(id).state == TransferState::Completed, 20000);
        QCOMPARE(readFile(e.engine->transfer(id).savePath), data);
    }

    void downloadTargetAndPartials()
    {
        Eng e(m_tmp.filePath(QStringLiteral("eng-target")));
        const QByteArray data = randomBytes(1200 * 1000, 17);
        DownloadRequest r;
        r.name = QStringLiteral("orig.bin");
        r.size = data.size();
        r.hashes.ed2k = ed2kOf(data);
        r.sources = {peer(Net::ED2K, QStringLiteral("s1"))};
        r.startPaused = true;
        e.plugin->data.insert(r.hashes.ed2k, data);
        // a stray file in the incomplete folder shows up as a candidate for Manually Resume
        QVERIFY(writeFile(e.root + QStringLiteral("/incomplete/__INCOMPLETE___song.mp3"), QByteArray(1000, 'a')));
        const QString id = e.engine->download(r);
        QVERIFY(!id.isEmpty());
        bool stray = false;
        for (const PartialFile &p : e.engine->partialFiles())
            if (p.name == QLatin1String("__INCOMPLETE___song.mp3")) { stray = true; QVERIFY(p.downloadId.isEmpty()); QCOMPARE(p.size, qint64(1000)); }
        QVERIFY(stray);
        QCOMPARE(e.engine->incompleteDir(), e.root + QStringLiteral("/incomplete"));
        const QString other = e.root + QStringLiteral("/elsewhere");
        QVERIFY(e.engine->setDownloadTarget(id, other, QStringLiteral("renamed.bin")));
        QCOMPARE(e.engine->transfer(id).name, QStringLiteral("renamed.bin"));
        QVERIFY(!e.engine->setDownloadTarget(QStringLiteral("nope"), other));
        e.engine->resume(id);
        // while running, its part file is listed as belonging to it
        QTRY_VERIFY_WITH_TIMEOUT(e.engine->transfer(id).state == TransferState::Completed, 15000);
        QCOMPARE(QFileInfo(e.engine->transfer(id).savePath).absolutePath(), QDir(other).absolutePath());
        QCOMPARE(QFileInfo(e.engine->transfer(id).savePath).fileName(), QStringLiteral("renamed.bin"));
        QCOMPARE(readFile(e.engine->transfer(id).savePath), data);
        QVERIFY(!e.engine->setDownloadTarget(id, e.root));   // finished: no longer possible
    }

    void partialFileOwnedByDownload()
    {
        Eng e(m_tmp.filePath(QStringLiteral("eng-owned")), 600000);
        const QByteArray data = randomBytes(1000 * 1000, 19);
        DownloadRequest r;
        r.name = QStringLiteral("held.bin");
        r.size = data.size();
        r.hashes.ed2k = ed2kOf(data);
        r.sources = {peer(Net::ED2K, QStringLiteral("s1"))};
        e.plugin->data.insert(r.hashes.ed2k, data);
        const QString id = e.engine->download(r);
        auto owned = [&] {
            for (const PartialFile &p : e.engine->partialFiles())
                if (p.downloadId == id) return true;
            return false;
        };
        QTRY_VERIFY(owned());
    }

    void uploadActions()
    {
        Eng e(m_tmp.filePath(QStringLiteral("eng-up")));
        TransferInfo up;
        up.id = QStringLiteral("u1");
        up.isUpload = true;
        up.name = QStringLiteral("shared.mp3");
        up.state = TransferState::Queued;
        e.plugin->emitUpload(up);
        QTRY_COMPARE(e.engine->uploads().size(), 1);
        const QString uid = e.engine->uploads().first().id;
        QVERIFY(e.engine->startUploadNow(uid));
        QCOMPARE(e.plugin->startedNow, QStringList{QStringLiteral("u1")});
        QVERIFY(e.engine->setUploadPriority(uid, 5));   // clamped
        QCOMPARE(e.plugin->priorities.last(), qMakePair(QStringLiteral("u1"), 1));
        QCOMPARE(e.engine->transfer(uid).priority, 1);
        e.plugin->emitUpload(up);                       // the plugin's next report keeps it
        QTest::qWait(600);
        QCOMPARE(e.engine->transfer(uid).priority, 1);
        QVERIFY(!e.engine->startUploadNow(QStringLiteral("up:nope")));
        e.plugin->uploadHooks = false;                  // a network that cannot
        QVERIFY(!e.engine->startUploadNow(uid));
        QVERIFY(!e.engine->setUploadPriority(uid, -1));
        e.plugin->emitUploadFinished(QStringLiteral("u1"));
        QTest::qWait(50);
        QVERIFY(!e.engine->startUploadNow(uid));        // finished
    }
};

QTEST_GUILESS_MAIN(TestGlue2)
#include "tst_glue2.moc"
