// SPDX-License-Identifier: GPL-3.0-or-later
// [ren-1] copies installed before 5.0: an existing user's profile folder, database, download folder
// and stored defaults keep working where they are; a new install only sees the current names. Everything
// below runs in the test's own directories (HOME and XDG point into the build tree, see CMakeLists).
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QStandardPaths>
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>

#include "core/dao.h"
#include "core/database.h"
#include "core/real_core.h"
#include "mx/legacy_names.h"
#include "mx/settings_keys.h"
#include "mx/simple_settings.h"

using namespace mx;

namespace {
void touch(const QString &path, const QByteArray &text = "{}")
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(text);
}
} // namespace

class TstLegacyNames : public QObject {
    Q_OBJECT
    QTemporaryDir m_tmp{QDir::tempPath() + QStringLiteral("/legacy-XXXXXX")};

private slots:
    // the legacy folder is the sibling QStandardPaths gives the legacy application name
    void legacyFolderIsTheLegacyAppDataFolder()
    {
        const QString saved = QCoreApplication::applicationName();
        QCoreApplication::setApplicationName(QLatin1String(legacy::kLegacyAppName));
        const QString old = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QCoreApplication::setApplicationName(QStringLiteral(MX_APP_NAME));
        const QString fresh = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QCoreApplication::setApplicationName(saved);
        QVERIFY(fresh.endsWith(QStringLiteral("/OctoMX")));
        QCOMPARE(legacy::legacyProfileDir(fresh), QDir::cleanPath(old));
        QVERIFY(legacy::isStandardProfileDir(old, fresh));
        QVERIFY(legacy::isStandardProfileDir(fresh, fresh));
        QVERIFY(!legacy::isStandardProfileDir(m_tmp.path(), fresh));
    }

    void profileChoice()
    {
        const QString base = m_tmp.filePath(QStringLiteral("appdata"));
        const QString fresh = base + QStringLiteral("/OctoMX"), old = base + QStringLiteral("/WinMX");
        // clean system: the new name
        QCOMPARE(legacy::chooseProfileDir(fresh), fresh);
        // a legacy folder without settings (never really used): still the new name
        QVERIFY(QDir().mkpath(old));
        QCOMPARE(legacy::chooseProfileDir(fresh), fresh);
        // a legacy profile: used as it is, every start, nothing moved
        touch(old + QStringLiteral("/settings.json"));
        QCOMPARE(legacy::chooseProfileDir(fresh), old);
        QCOMPARE(legacy::chooseProfileDir(fresh), old);
        QVERIFY(!QFileInfo::exists(fresh));
        QVERIFY(QFileInfo::exists(old + QStringLiteral("/settings.json")));
        // an empty new folder does not hide it either
        QVERIFY(QDir().mkpath(fresh));
        QCOMPARE(legacy::chooseProfileDir(fresh), old);
        // once the new folder holds a profile, it wins
        touch(fresh + QStringLiteral("/settings.json"));
        QCOMPARE(legacy::chooseProfileDir(fresh), fresh);
    }

    // the real core treats the legacy folder like the default one (the user's own Downloads folder)
    void legacyFolderIsNotIsolated()
    {
        const QByteArray osRoot = qgetenv("MX_OS_ROOT"), expl = qgetenv("MX_PROFILE_EXPLICIT");
        qunsetenv("MX_OS_ROOT");
        qunsetenv("MX_PROFILE_EXPLICIT");
        const QString def = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        const bool oldIsolated = RealCore::isolatedProfile(legacy::legacyProfileDir(def));
        const bool otherIsolated = RealCore::isolatedProfile(m_tmp.filePath(QStringLiteral("WinMX")));
        if (!osRoot.isNull()) qputenv("MX_OS_ROOT", osRoot);
        if (!expl.isNull()) qputenv("MX_PROFILE_EXPLICIT", expl);
        QVERIFY(!oldIsolated);
        QVERIFY(otherIsolated);   // only the one next to the default folder
    }

    void databaseName()
    {
        const QString p = m_tmp.filePath(QStringLiteral("db"));
        QVERIFY(QDir().mkpath(p));
        QCOMPARE(QFileInfo(legacy::databaseFile(p)).fileName(), QStringLiteral("octomx.db"));
        touch(p + QStringLiteral("/winmx.db"), QByteArray());
        QCOMPARE(QFileInfo(legacy::databaseFile(p)).fileName(), QStringLiteral("winmx.db"));
        touch(p + QStringLiteral("/octomx.db"), QByteArray());
        QCOMPARE(QFileInfo(legacy::databaseFile(p)).fileName(), QStringLiteral("octomx.db"));
    }

    // a legacy profile keeps its winmx.db (with its -wal side file): the core opens it there
    // and never creates octomx.db; a new profile gets octomx.db
    void realCoreDatabase()
    {
        const QString old = m_tmp.filePath(QStringLiteral("core-old"));
        {
            Database db;
            QVERIFY(db.open(old + QStringLiteral("/winmx.db")));
            UserListsDao dao(&db);
            QVERIFY(dao.setFavouriteRoom(QStringLiteral("Music"), true));
            db.close();
        }
        touch(old + QStringLiteral("/settings.json"), "{\"internal/lastVersion\": \"4.5\", \"internal/firstRunDone\": true}");
        {
            Core *core = createRealCore(old, {});
            core->start();
            QCOMPARE(core->chat()->favouriteRooms(), QStringList{QStringLiteral("Music")});
            QCOMPARE(core->settings()->s(keys::internal::LastVersion), QStringLiteral(MX_VERSION_STRING));
            QSignalSpy done(core, &Core::shutdownFinished);
            core->shutdown();
            QVERIFY(done.wait(5000));
            delete core;
        }
        QVERIFY(QFileInfo::exists(old + QStringLiteral("/winmx.db")));
        QVERIFY(!QFileInfo::exists(old + QStringLiteral("/octomx.db")));
        const QString fresh = m_tmp.filePath(QStringLiteral("core-new"));
        {
            Core *core = createRealCore(fresh, {});
            delete core;
        }
        QVERIFY(QFileInfo::exists(fresh + QStringLiteral("/octomx.db")));
        QVERIFY(!QFileInfo::exists(fresh + QStringLiteral("/winmx.db")));
    }

    void downloadFolderDefault()
    {
        const QString dl = m_tmp.filePath(QStringLiteral("Downloads"));
        QVERIFY(QDir().mkpath(dl));
        QCOMPARE(legacy::defaultIncomingDir(dl), dl + QStringLiteral("/OctoMX"));
        QVERIFY(QDir().mkpath(dl + QStringLiteral("/WinMX")));
        QCOMPARE(legacy::defaultIncomingDir(dl), dl + QStringLiteral("/WinMX"));
        QVERIFY(QDir().mkpath(dl + QStringLiteral("/OctoMX")));
        QCOMPARE(legacy::defaultIncomingDir(dl), dl + QStringLiteral("/OctoMX"));
    }

    // both ways through the real core (MX_DOWNLOADS_DIR stands for the Downloads folder); a stored
    // folder stays whatever exists
    void realCoreDownloadFolder()
    {
        const QString dl = m_tmp.filePath(QStringLiteral("Downloads2"));
        QVERIFY(QDir().mkpath(dl + QStringLiteral("/WinMX")));
        qputenv("MX_DOWNLOADS_DIR", QFile::encodeName(dl));
        auto incoming = [](const QString &profile) {
            Core *core = createRealCore(profile, {});
            const QString v = core->settings()->s(keys::transfers::IncomingDir);
            delete core;
            return v;
        };
        QCOMPARE(incoming(m_tmp.filePath(QStringLiteral("p-old"))), dl + QStringLiteral("/WinMX"));
        QVERIFY(QDir(dl + QStringLiteral("/WinMX")).removeRecursively());
        QCOMPARE(incoming(m_tmp.filePath(QStringLiteral("p-new"))), dl + QStringLiteral("/OctoMX"));
        touch(m_tmp.filePath(QStringLiteral("p-set/settings.json")), "{\"transfers/incomingDir\": \"/srv/files\"}");
        QCOMPARE(incoming(m_tmp.filePath(QStringLiteral("p-set"))), QStringLiteral("/srv/files"));
        qunsetenv("MX_DOWNLOADS_DIR");
    }

    void storedDefaultsFollow()
    {
        SimpleSettings s;
        s.setValue(QString(keys::updates::Url), QString::fromLatin1(legacy::kLegacyUpdateUrl));
        s.setValue(QString(keys::http::UserAgent), QLatin1String(legacy::kLegacyUserAgentPrefix) + QStringLiteral("4.5"));
        s.setValue(QString(keys::search::Networks), QStringList{QStringLiteral("eD2k"), QStringLiteral("WinMX")});
        s.setValue(QString(keys::ui::FontNormal), QStringLiteral("WinMX Tahoma,9"));
        const QStringList changed = legacy::migrateSettings(&s);
        QCOMPARE(changed.size(), 4);
        QCOMPARE(s.s(keys::updates::Url), QStringLiteral("https://api.github.com/repos/robtacconelli/OctoMX/releases/latest"));
        QCOMPARE(s.s(keys::http::UserAgent), QStringLiteral("OctoMX/" MX_VERSION_STRING));
        QCOMPARE(s.sl(keys::search::Networks), (QStringList{QStringLiteral("eD2k"), QStringLiteral("OctoMX")}));
        QCOMPARE(netFromName(QStringLiteral("WinMX")), Net::Overlay);
        QCOMPARE(s.s(keys::ui::FontNormal), QStringLiteral("Tahoma,9"));
        // the user's own values stay
        SimpleSettings u;
        u.setValue(QString(keys::updates::Url), QStringLiteral("https://mirror.example.org/latest"));
        const QString own = QLatin1String(legacy::kLegacyUserAgentPrefix) + QStringLiteral("4.5 (my build)");
        u.setValue(QString(keys::http::UserAgent), own);
        QVERIFY(legacy::migrateSettings(&u).isEmpty());
        QCOMPARE(u.s(keys::updates::Url), QStringLiteral("https://mirror.example.org/latest"));
        QCOMPARE(u.s(keys::http::UserAgent), own);
    }
};

QTEST_MAIN(TstLegacyNames)
#include "tst_legacy_names.moc"
