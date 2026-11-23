// SPDX-License-Identifier: GPL-3.0-or-later
#include <QtTest/QtTest>
#include <QtCore/QStandardPaths>
#include <QtTest/QSignalSpy>

#include "core/library.h"
#include "core/log.h"
#include "core/rate_limiter.h"
#include "core/real_core.h"
#include "fake_plugin.h"
#include "mx/settings_keys.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QTemporaryDir>

using namespace mx;
using mx::test::FakeChatPlugin;
using mx::test::FakePlugin;

namespace {
const char *kMagnet = "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567&dn=Big%20Buck%20Bunny.mkv&xl=276134947"
                      "&tr=udp%3A%2F%2Ftracker.example%3A80";
}

class TestRealCore : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    QString m_profile, m_share, m_incoming;
    QString m_screenName;

    static void shutdownAndDelete(Core *core)
    {
        QSignalSpy done(core, &Core::shutdownFinished);
        QElapsedTimer t;
        t.start();
        core->shutdown();
        QVERIFY(done.count() == 1 || done.wait(5000));
        QVERIFY2(t.elapsed() < 5000, qPrintable(QString::number(t.elapsed())));
        delete core;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_profile = m_dir.filePath(QStringLiteral("profile"));
        m_share = m_dir.filePath(QStringLiteral("share"));
        m_incoming = m_dir.filePath(QStringLiteral("incoming"));
        QDir().mkpath(m_share);
        QFile f(m_share + QStringLiteral("/song.mp3"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QByteArray(300000, 'x'));
    }

    void logHandler()
    {
        installLogHandler(m_profile, 64 * 1024, 2, false, 0);
        qCInfo(lcCore) << "hello log";
        for (int i = 0; i < 2000; ++i) qCDebug(lcCore) << "filler line to force rotation" << i;
        QVERIFY(QFile::exists(m_profile + QStringLiteral("/logs/octomx.log")));
        QVERIFY(QFile::exists(m_profile + QStringLiteral("/logs/octomx.1.log")));   // rotated
        QCOMPARE(logFilePath(), m_profile + QStringLiteral("/logs/octomx.log"));
        uninstallLogHandler();
        QFile f(m_profile + QStringLiteral("/logs/octomx.1.log"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QVERIFY(f.readAll().contains("mx.core: filler line"));
    }

    // [core-5] which profiles may use the user's real folders: only a normal start on the default
    // per-user profile, without MX_OS_ROOT / MX_PROFILE_EXPLICIT
    void isolatedProfileRule()
    {
        const QByteArray osRoot = qgetenv("MX_OS_ROOT"), expl = qgetenv("MX_PROFILE_EXPLICIT");
        const QString def = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        qunsetenv("MX_OS_ROOT");
        qunsetenv("MX_PROFILE_EXPLICIT");
        const bool defaultIsolated = RealCore::isolatedProfile(def);
        const bool otherIsolated = RealCore::isolatedProfile(m_profile);
        qputenv("MX_PROFILE_EXPLICIT", "1");
        const bool explicitIsolated = RealCore::isolatedProfile(def);
        qunsetenv("MX_PROFILE_EXPLICIT");
        qputenv("MX_OS_ROOT", "/nonexistent");
        const bool osRootIsolated = RealCore::isolatedProfile(def);
        if (osRoot.isNull()) qunsetenv("MX_OS_ROOT"); else qputenv("MX_OS_ROOT", osRoot);
        if (!expl.isNull()) qputenv("MX_PROFILE_EXPLICIT", expl);
        QVERIFY(!defaultIsolated);   // the user's own start: ~/Downloads/OctoMX (or an existing legacy ~/Downloads/WinMX)
        QVERIFY(otherIsolated);      // any other profile directory
        QVERIFY(explicitIsolated);   // the app was given --profile
        QVERIFY(osRootIsolated);     // redirected OS integration (tests, scripts)
    }

    void firstRun()
    {
        auto *ed = new FakePlugin(Net::ED2K, NetworkPlugin::CanSearch | NetworkPlugin::CanServe | NetworkPlugin::CanFindSources);
        ed->cannedHits << FakePlugin::makeHit(QStringLiteral("song.mp3"), 300000, FileHashes(), Net::ED2K, QStringLiteral("peer"));
        auto *ov = new FakeChatPlugin;
        Core *core = createRealCore(m_profile, {ed, ov});
        QVERIFY(!core->isMock());
        QCOMPARE(core->versionString(), QStringLiteral(MX_VERSION_STRING));   // [post-1] from project() VERSION
        Settings *s = core->settings();
        // runtime defaults
        QCOMPARE(s->s(keys::transfers::IncompleteDir), QDir::cleanPath(QDir(m_profile).absolutePath()) + QStringLiteral("/incomplete"));
        // [core-5] a test / --profile run keeps its downloads inside the profile, never ~/Downloads
        QCOMPARE(s->s(keys::transfers::IncomingDir), QDir::cleanPath(QDir(m_profile).absolutePath()) + QStringLiteral("/incoming"));
        QVERIFY(core->library()->suggestMediaFolders().isEmpty());
        s->setValue(keys::transfers::IncomingDir, m_incoming);
        s->setValue(keys::bandwidth::UpLimitKBs, 33);
        m_screenName = core->network()->screenName();
        QVERIFY(m_screenName.startsWith(QStringLiteral("Guest")));

        QSignalSpy added(core->transfers(), &TransferService::transferAdded);
        core->start();
        QCOMPARE(ed->initCount, 1);
        QCOMPARE(ed->startCount, 1);
        QCOMPARE(ov->startCount, 1);
        QCOMPARE(ed->ctx.dataDir, QDir(m_profile).absolutePath() + QStringLiteral("/ed2k"));
        QVERIFY(ed->ctx.library != nullptr);
        QVERIFY(ed->ctx.limiter != nullptr);
        QCOMPARE(core->network()->overallHeadline(), QStringLiteral("Ready"));
        QVERIFY(core->network()->titleSummary().startsWith(QStringLiteral("Online P=")));

        // library: add a folder, the file gets hashed and pushed to the serving plugin
        core->library()->addFolder(m_share);
        QTRY_COMPARE_WITH_TIMEOUT(core->library()->fileCount(), 1, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!core->library()->isScanning(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!ed->sharedLists.isEmpty() && ed->sharedLists.last().size() == 1, 10000);
        const SharedFile shared = ed->sharedLists.last().first();
        QVERIFY(shared.hashed);
        QCOMPARE(ed->ctx.library->read(shared.hashes, 0, 10), QByteArray(10, 'x'));

        // downloads (engine stub): add, join, pause
        const QString id = core->transfers()->downloadUri(QString::fromLatin1(kMagnet));
        QVERIFY(!id.isEmpty());
        QCOMPARE(added.count(), 1);
        TransferInfo t = core->transfers()->transfer(id);
        QCOMPARE(t.name, QStringLiteral("Big Buck Bunny.mkv"));
        QCOMPARE(t.size, 276134947);
        QVERIFY(!t.statusText.isEmpty());   // engine-specific wording (stub: "Waiting for engine")
        QVERIFY(!t.savePath.isEmpty());     // stub: final path; real engine: part file while incomplete
        QCOMPARE(core->transfers()->downloadUri(QString::fromLatin1(kMagnet)), id);   // joined, not duplicated
        QCOMPARE(core->transfers()->downloads().size(), 1);
        core->transfers()->pause(id);
        QCOMPARE(core->transfers()->transfer(id).state, TransferState::Paused);
        const QString id2 = core->transfers()->downloadUri(QStringLiteral("ed2k://|file|ubuntu.iso|1000000000|31d6cfe0d16ae931b73c59d7e0c089c0|/"));
        QVERIFY(!id2.isEmpty());
        core->transfers()->cancel(id2);
        QCOMPARE(core->transfers()->downloads().size(), 1);
        QVERIFY(core->transfers()->downloadUri(QStringLiteral("not a link")).isEmpty());

        // search flags: the hit matching our shared file is marked as already in the library
        ed->cannedHits.clear();
        ed->cannedHits << FakePlugin::makeHit(QStringLiteral("song.mp3"), 300000, shared.hashes, Net::ED2K, QStringLiteral("peer"));
        QSignalSpy done(core->search(), &SearchService::searchFinished);
        SearchQuery q;
        q.text = QStringLiteral("song");
        const quint32 sid = core->search()->startSearch(q);
        QVERIFY(done.wait(3000));
        QCOMPARE(core->search()->hits(sid).size(), 1);
        QVERIFY(core->search()->hits(sid).at(0).alreadyInLibrary);

        // uploads reported by a plugin show up in the transfers service
        QSignalSpy upAdded(core->transfers(), &TransferService::transferAdded);
        TransferInfo up;
        up.id = QStringLiteral("u1");
        up.name = QStringLiteral("song.mp3");
        up.size = 300000;
        up.done = 1000;
        up.hashes = shared.hashes;
        up.state = TransferState::Transferring;
        up.peer.id = QStringLiteral("10.0.0.1:4662");
        ed->emitUpload(up);
        QCOMPARE(upAdded.count(), 1);
        QVERIFY(upAdded.at(0).at(0).value<TransferInfo>().isUpload);
        QCOMPARE(core->transfers()->uploads().size(), 1);
        const QString upId = core->transfers()->uploads().at(0).id;
        QCOMPARE(upId, QStringLiteral("up:ed2k:u1"));
        up.done = 300000;
        ed->emitUpload(up);
        ed->emitUploadFinished(QStringLiteral("u1"));
        QCOMPARE(core->transfers()->transfer(upId).state, TransferState::Completed);
        QTRY_COMPARE_WITH_TIMEOUT(core->library()->files().at(0).uploadedBytes, 300000, 3000);
        core->transfers()->cancelUpload(upId);
        QCOMPARE(core->transfers()->uploads().size(), 0);

        // users / chat persistence material
        core->users()->addToHotlist(FakeChatPlugin::user(QStringLiteral("Bob")));
        core->chat()->setFavouriteRoom(QStringLiteral("Lobby"), true);

        // bandwidth accounting through the limiter given to plugins
        ed->ctx.limiter->account(RateLimiter::Down, Net::ED2K, 123456);
        QCOMPARE(core->bandwidth()->totalDownloaded(), 123456);
        QCOMPARE(ed->ctx.limiter->limitBps(RateLimiter::Up), 33 * 1024);

        QPointer<FakePlugin> edWatch = ed;
        shutdownAndDelete(core);
        QVERIFY(edWatch.isNull());   // plugins deleted with the core
        QVERIFY(QFile::exists(m_profile + QStringLiteral("/settings.json")));
        QVERIFY(QFile::exists(m_profile + QStringLiteral("/octomx.db")));
    }

    void restartRestoresEverything()
    {
        auto *ed = new FakePlugin(Net::ED2K, NetworkPlugin::CanSearch | NetworkPlugin::CanServe);
        auto *ov = new FakeChatPlugin;
        Core *core = createRealCore(m_profile, {ed, ov});
        QSignalSpy added(core->transfers(), &TransferService::transferAdded);
        core->start();
        Settings *s = core->settings();
        QCOMPARE(s->i(keys::bandwidth::UpLimitKBs), 33);
        QCOMPARE(s->s(keys::transfers::IncomingDir), m_incoming);
        QVERIFY(s->b(keys::internal::FirstRunDone));
        QCOMPARE(core->network()->screenName(), m_screenName);
        // downloads restored (and announced)
        QCOMPARE(core->transfers()->downloads().size(), 1);
        QCOMPARE(added.count(), 1);
        const TransferInfo t = core->transfers()->downloads().at(0);
        QCOMPARE(t.name, QStringLiteral("Big Buck Bunny.mkv"));
        QCOMPARE(t.state, TransferState::Paused);
        QCOMPARE(t.hashes.btih.toHex(), QByteArray("0123456789abcdef0123456789abcdef01234567"));
        // (source counting is engine-specific)
        core->transfers()->resume(t.id);
        QVERIFY(core->transfers()->transfer(t.id).state != TransferState::Paused);
        // lists
        QCOMPARE(core->users()->hotlist().size(), 1);
        QCOMPARE(core->users()->hotlist().at(0).user.name, QStringLiteral("Bob"));
        QCOMPARE(core->chat()->favouriteRooms(), QStringList{QStringLiteral("Lobby")});
        QCOMPARE(core->search()->history(), QStringList{QStringLiteral("song")});
        // the share list is known immediately from the cache (no re-hash)
        QVERIFY(core->library()->folders().size() >= 1);
        QCOMPARE(core->library()->folders().at(0).path, QDir::cleanPath(m_share));
        auto *lib = static_cast<RealCore *>(core)->libraryImpl();
        QCOMPARE(lib->sharedFiles().size(), 1);
        QSignalSpy hashed(lib, &Library::fileHashed);
        QVERIFY(lib->waitIdle(10000));
        QCOMPARE(hashed.count(), 0);
        QCOMPARE(lib->files().at(0).uploadedBytes, 300000);
        // all-time statistics
        QCOMPARE(core->bandwidth()->allTimeDownloaded(), 123456);
        // the overlay plugin is connected → favourite room auto-joined
        QTRY_COMPARE_WITH_TIMEOUT(ov->joinCalls, QStringList{QStringLiteral("Lobby")}, 2000);
        shutdownAndDelete(core);
    }

    void shutdownWithoutStart()
    {
        QTemporaryDir other;
        Core *core = createRealCore(other.filePath(QStringLiteral("p")), {});
        QCOMPARE(core->network()->overallDetail(), QStringLiteral("No networks are available."));
        shutdownAndDelete(core);
        // destroying a started core without shutdown() still persists state
        Core *c2 = createRealCore(other.filePath(QStringLiteral("p")), {});
        c2->start();
        c2->settings()->setValue(keys::misc::Sounds, false);
        delete c2;
        Core *c3 = createRealCore(other.filePath(QStringLiteral("p")), {});
        QCOMPARE(c3->settings()->b(keys::misc::Sounds), false);
        shutdownAndDelete(c3);
    }
};

QTEST_GUILESS_MAIN(TestRealCore)
#include "tst_realcore.moc"
