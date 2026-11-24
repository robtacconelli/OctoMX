// SPDX-License-Identifier: GPL-3.0-or-later
// [ui-c] OsIntegration: fake backend driven by the settings, XDG backend under a root redirected
// below the build directory (also with xdg-mime / update-desktop-database running sandboxed), the
// registry layout over an INI file. Never touches the real HOME or registry.
// [ren-1] legacy registrations (copies installed before 5.0) that point at this copy move to the
// current names, others stay.
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QSettings>
#include <QtCore/QStandardPaths>
#include <QtTest/QtTest>

#include "mock/mock_core.h"
#include "mx/services.h"
#include "mx/settings_keys.h"
#include "ui/os_integration.h"

using namespace mx;
using namespace mx::ui;

namespace {
// [ren-1] the legacy names a copy installed before 5.0 registered (compatibility literals, the same as
// in src/ui/os_integration.cpp)
const QString kLegacyDesktopId = QStringLiteral("winmx-ng.desktop");
const QString kLegacyRunKey = QStringLiteral("Microsoft/Windows/CurrentVersion/Run/WinMX NG");
const QString kLegacyTorrentClass = QStringLiteral("WinMXNG.torrent");

QString readAll(const QString &p)
{
    QFile f(p);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

QByteArray fileHash(const QString &p)
{
    QFile f(p);
    if (!f.open(QIODevice::ReadOnly))
        return "missing";
    return QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha1);
}

} // namespace

class TstOsIntegration : public QObject {
    Q_OBJECT
private:
    QString m_root;   // below the test binary (= the build tree)

    OsIntegration::Options opts(const QString &sub, bool tools) const
    {
        OsIntegration::Options o;
        o.exePath = QStringLiteral("/opt/OctoMX 5/bin/octomx");
        o.exeArgs = {QStringLiteral("--profile"), QStringLiteral("/data/my profile")};
        o.configHome = m_root + QLatin1Char('/') + sub + QStringLiteral("/config");
        o.dataHome = m_root + QLatin1Char('/') + sub + QStringLiteral("/data");
        o.runXdgTools = tools;
        o.registryIni = m_root + QLatin1Char('/') + sub + QStringLiteral("/registry.ini");
        return o;
    }

private slots:
    void initTestCase()
    {
        m_root = QCoreApplication::applicationDirPath() + QStringLiteral("/tst_os_root");
        // [qa-1] the sandbox lives in the test's own build directory, never directly in $HOME (no machine path)
        QVERIFY(m_root.startsWith(QCoreApplication::applicationDirPath() + QLatin1Char('/')));
        QVERIFY(QDir::cleanPath(QCoreApplication::applicationDirPath()) != QDir::cleanPath(QDir::homePath()));
        QDir(m_root).removeRecursively();
        QVERIFY(QDir().mkpath(m_root));
    }

    void fakeFollowsSettings()
    {
        MockOptions mo;
        mo.frozen = true;
        std::unique_ptr<Core> core(createMockCoreWithOptions(QString(), mo));
        Settings *s = core->settings();
        s->setValue(keys::misc::AssociateEd2k, false);
        auto *os = static_cast<FakeOsIntegration *>(OsIntegration::createFake(this));
        os->attach(s);
        // start-up: associations claimed (checkAssociations on), autostart follows the setting (off)
        QVERIFY(os->isHandler(OsIntegration::Assoc::Magnet));
        QVERIFY(os->isHandler(OsIntegration::Assoc::Torrent));
        QVERIFY(!os->isHandler(OsIntegration::Assoc::Ed2k));
        QVERIFY(!os->isAutostart());
        s->setValue(keys::misc::StartWithOs, true);
        QVERIFY(os->isAutostart());
        s->setValue(keys::misc::AssociateEd2k, true);
        QVERIFY(os->isHandler(OsIntegration::Assoc::Ed2k));
        s->setValue(keys::misc::AssociateMagnet, false);
        QVERIFY(!os->isHandler(OsIntegration::Assoc::Magnet));
        QVERIFY(os->log.contains(QStringLiteral("autostart=1")));
        delete os;
    }

    void sandboxedAppUsesFake()
    {
        // [ui-e] MX_OS_ROOT (toolchain/env.sh) is restored afterwards, whatever happens below
        const QByteArray savedRoot = qgetenv("MX_OS_ROOT");
        struct Restore {
            QByteArray v;
            ~Restore() { v.isEmpty() ? qunsetenv("MX_OS_ROOT") : qputenv("MX_OS_ROOT", v); }
        } restore{savedRoot};
        qunsetenv("MX_OS_ROOT");
        qputenv("MX_OS_INTEGRATION", "");
        std::unique_ptr<OsIntegration> os(OsIntegration::createForApp(true, {}));
        QVERIFY(os);
        QCOMPARE(os->backendName(), QStringLiteral("fake"));
        qputenv("MX_OS_INTEGRATION", "off");
        QVERIFY(!OsIntegration::createForApp(false, {}));
        qunsetenv("MX_OS_INTEGRATION");
        qputenv("MX_OS_ROOT", QFile::encodeName(m_root + QStringLiteral("/approot")));
        std::unique_ptr<OsIntegration> os2(OsIntegration::createForApp(true, {}));
        QVERIFY(os2->backendName().contains(QStringLiteral("redirected")) || os2->backendName().contains(QStringLiteral("registry"))
                || os2->backendName() == QLatin1String("macos-test"));   // [mac-1] LaunchAgent + Launch Services seam
    }

    // [ui-e] with MX_OS_ROOT set, the app's backend writes only below the root and never runs the
    // desktop tools (a fake xdg-mime / update-desktop-database on PATH must stay untouched)
    void osRootRedirectsAndSkipsTools()
    {
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
        QSKIP("XDG tools are Linux only (the macOS backend under MX_OS_ROOT: tst_mac appUsesRootOnMac)");
#else
        const QString bin = m_root + QStringLiteral("/fakebin"), marker = m_root + QStringLiteral("/tool-ran");
        QVERIFY(QDir().mkpath(bin));
        QFile::remove(marker);
        for (const char *tool : {"xdg-mime", "update-desktop-database"}) {
            QFile f(bin + QLatin1Char('/') + QLatin1String(tool));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QStringLiteral("#!/bin/sh\necho \"$0 $*\" >> '%1'\n").arg(marker).toUtf8());
            f.close();
            f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        }
        const QByteArray savedPath = qgetenv("PATH"), savedRoot = qgetenv("MX_OS_ROOT");
        qputenv("PATH", QFile::encodeName(bin) + ':' + savedPath);
        qputenv("MX_OS_ROOT", QFile::encodeName(m_root + QStringLiteral("/approot2")));
        qunsetenv("MX_OS_INTEGRATION");
        {
            std::unique_ptr<OsIntegration> os(OsIntegration::createForApp(false, {}));
            QVERIFY(os);
            QVERIFY(os->backendName().contains(QStringLiteral("redirected")));
            QVERIFY(os->setHandler(OsIntegration::Assoc::Magnet, true));
            QVERIFY(os->isHandler(OsIntegration::Assoc::Magnet));
        }
        qputenv("PATH", savedPath);
        savedRoot.isEmpty() ? qunsetenv("MX_OS_ROOT") : qputenv("MX_OS_ROOT", savedRoot);
        QVERIFY2(!QFile::exists(marker), "a desktop tool was started although MX_OS_ROOT redirects the backend");
        QVERIFY(QFile::exists(m_root + QStringLiteral("/approot2/config/mimeapps.list")));
#endif
    }

    void xdgBackend()
    {
#ifdef Q_OS_WIN
        QSKIP("the XDG backend is not used on Windows");
#endif
        const OsIntegration::Options o = opts(QStringLiteral("xdg"), false);
        std::unique_ptr<OsIntegration> os(OsIntegration::createXdg(o));
        QVERIFY(!os->isAutostart());
        QVERIFY(os->setAutostart(true));
        QVERIFY(os->isAutostart());
        const QString autostart = readAll(o.configHome + QStringLiteral("/autostart/octomx.desktop"));
        QVERIFY(autostart.contains(QStringLiteral("Exec=\"/opt/OctoMX 5/bin/octomx\" --profile \"/data/my profile\" --minimized\n")));
        QVERIFY(os->setAutostart(false));
        QVERIFY(!os->isAutostart());
        QVERIFY(!QFile::exists(o.configHome + QStringLiteral("/autostart/octomx.desktop")));

        // a foreign default for .torrent and an unrelated entry must survive
        QDir().mkpath(o.configHome);
        QFile f(o.configHome + QStringLiteral("/mimeapps.list"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("[Default Applications]\ntext/plain=gedit.desktop\napplication/x-bittorrent=other.desktop\n");
        f.close();
        QVERIFY(os->setHandler(OsIntegration::Assoc::Magnet, true));
        QVERIFY(os->setHandler(OsIntegration::Assoc::Torrent, true));
        QVERIFY(os->isHandler(OsIntegration::Assoc::Magnet));
        QVERIFY(os->isHandler(OsIntegration::Assoc::Torrent));
        QVERIFY(!os->isHandler(OsIntegration::Assoc::Ed2k));
        const QString app = readAll(o.dataHome + QStringLiteral("/applications/octomx.desktop"));
        QVERIFY(app.contains(QStringLiteral("MimeType=x-scheme-handler/magnet;application/x-bittorrent;")));
        QVERIFY(app.contains(QStringLiteral("%U")));
        QString list = readAll(o.configHome + QStringLiteral("/mimeapps.list"));
        QVERIFY(list.contains(QStringLiteral("text/plain=gedit.desktop")));
        QVERIFY(list.contains(QStringLiteral("x-scheme-handler/magnet=octomx.desktop")));
        QVERIFY(list.contains(QStringLiteral("application/x-bittorrent=octomx.desktop")));
        QVERIFY(os->setHandler(OsIntegration::Assoc::Magnet, false));
        QVERIFY(!os->isHandler(OsIntegration::Assoc::Magnet));
        QVERIFY(os->isHandler(OsIntegration::Assoc::Torrent));
        list = readAll(o.configHome + QStringLiteral("/mimeapps.list"));
        QVERIFY(!list.contains(QStringLiteral("x-scheme-handler/magnet=")));
        QVERIFY(list.contains(QStringLiteral("text/plain=gedit.desktop")));
        QVERIFY(os->setHandler(OsIntegration::Assoc::Torrent, false));
        QVERIFY(!QFile::exists(o.dataHome + QStringLiteral("/applications/octomx.desktop")));
    }

    void xdgToolsStaySandboxed()
    {
#ifdef Q_OS_WIN
        QSKIP("the XDG backend is not used on Windows");
#endif
        // [ui-e] a redirected backend writes its files into the root and starts no desktop tool at all
        const QString realList = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/mimeapps.list");
        const QByteArray before = fileHash(realList);
        const OsIntegration::Options o = opts(QStringLiteral("xdgtools"), true);
        std::unique_ptr<OsIntegration> os(OsIntegration::createXdg(o));
        QVERIFY(os->setHandler(OsIntegration::Assoc::Ed2k, true));
        QVERIFY(os->isHandler(OsIntegration::Assoc::Ed2k));
        QCOMPARE(fileHash(realList), before);
        QVERIFY(!QFile::exists(o.dataHome + QStringLiteral("/applications/mimeinfo.cache")));
        QVERIFY(QFile::exists(o.dataHome + QStringLiteral("/applications/octomx.desktop")));
    }

    void registryLayout()
    {
        const OsIntegration::Options o = opts(QStringLiteral("reg"), false);
        std::unique_ptr<OsIntegration> os(OsIntegration::createRegistry(o));
        QVERIFY(os->setAutostart(true));
        QVERIFY(os->isAutostart());
        QSettings ini(o.registryIni, QSettings::IniFormat);
        const QString run = ini.value(QStringLiteral("Microsoft/Windows/CurrentVersion/Run/OctoMX")).toString();
        QCOMPARE(run, QStringLiteral("\"%1\" --profile \"/data/my profile\" --minimized").arg(QDir::toNativeSeparators(o.exePath)));
        for (auto a : {OsIntegration::Assoc::Magnet, OsIntegration::Assoc::Ed2k, OsIntegration::Assoc::Torrent})
            QVERIFY(os->setHandler(a, true));
        ini.sync();
        QCOMPARE(ini.value(QStringLiteral("Classes/magnet/URL Protocol")).toString(), QString());
        QVERIFY(ini.contains(QStringLiteral("Classes/magnet/URL Protocol")));
        QVERIFY(ini.value(QStringLiteral("Classes/ed2k/shell/open/command/.")).toString().endsWith(QStringLiteral(" \"%1\"")));
        QCOMPARE(ini.value(QStringLiteral("Classes/.torrent/.")).toString(), QStringLiteral("OctoMX.torrent"));
        QVERIFY(os->isHandler(OsIntegration::Assoc::Torrent));
        // a handler that belongs to another program is left alone
        ini.setValue(QStringLiteral("Classes/ed2k/shell/open/command/."), QStringLiteral("\"C:\\eMule\\emule.exe\" \"%1\""));
        ini.sync();
        QVERIFY(!os->isHandler(OsIntegration::Assoc::Ed2k));
        QVERIFY(os->setHandler(OsIntegration::Assoc::Ed2k, false));
        QSettings ini2(o.registryIni, QSettings::IniFormat);
        QVERIFY(ini2.value(QStringLiteral("Classes/ed2k/shell/open/command/.")).toString().contains(QStringLiteral("emule")));
        QVERIFY(os->setHandler(OsIntegration::Assoc::Torrent, false));
        QVERIFY(os->setAutostart(false));
        QSettings ini3(o.registryIni, QSettings::IniFormat);
        QVERIFY(!ini3.contains(QStringLiteral("Classes/.torrent/.")));
        QVERIFY(!ini3.contains(QStringLiteral("Microsoft/Windows/CurrentVersion/Run/OctoMX")));
    }

    // [ren-1] Linux: the legacy autostart entry and application / menu entry (Exec into this
    // program's folder) become octomx.desktop; mimeapps.list follows; the icons too
    void xdgLegacyMoved()
    {
#ifdef Q_OS_WIN
        QSKIP("the XDG backend is not used on Windows");
#endif
        OsIntegration::Options o = opts(QStringLiteral("xdglegacy"), false);
        const QString pkg = m_root + QStringLiteral("/xdglegacy/OctoMX-5.0-linux-x86_64");
        o.exePath = pkg + QStringLiteral("/bin/octomx");
        o.exeArgs.clear();
        auto write = [](const QString &path, const QByteArray &text) {
            QDir().mkpath(QFileInfo(path).absolutePath());
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(text);
        };
        // what 4.5 left: its autostart entry (bin/winmx), install-desktop-entry.sh's menu entry (./winmx)
        // with its icon, and the defaults in mimeapps.list; plus the new package's own template
        const QByteArray legacyId = kLegacyDesktopId.toLatin1();
        write(o.configHome + QStringLiteral("/autostart/") + kLegacyDesktopId,
              "[Desktop Entry]\nType=Application\nName=WinMX\nExec=" + QFile::encodeName(pkg) + "/bin/winmx --minimized\n");
        write(o.dataHome + QStringLiteral("/applications/") + kLegacyDesktopId,
              "[Desktop Entry]\nType=Application\nName=WinMX\nExec=" + QFile::encodeName(pkg) +
                  "/winmx %U\nIcon=winmx\nMimeType=x-scheme-handler/magnet;application/x-bittorrent;\n");
        write(o.dataHome + QStringLiteral("/icons/hicolor/48x48/apps/winmx.png"), "old icon");
        write(o.configHome + QStringLiteral("/mimeapps.list"),
              "[Default Applications]\ntext/plain=gedit.desktop\nx-scheme-handler/magnet=" + legacyId + "\n"
              "application/x-bittorrent=" + legacyId + "\n\n[Added Associations]\nx-scheme-handler/magnet=" + legacyId + ";other.desktop;\n");
        write(pkg + QStringLiteral("/share/applications/octomx.desktop"),
              "[Desktop Entry]\nType=Application\nName=OctoMX\nExec=@PREFIX@/octomx %U\nIcon=octomx\n"
              "MimeType=x-scheme-handler/magnet;x-scheme-handler/ed2k;application/x-bittorrent;\n");
        write(pkg + QStringLiteral("/share/icons/hicolor/48x48/apps/octomx.png"), "new icon");
        std::unique_ptr<OsIntegration> os(OsIntegration::createXdg(o));
        const QStringList moved = os->migrateLegacy();
        QCOMPARE(moved.size(), 2);
        QVERIFY(!QFile::exists(o.configHome + QStringLiteral("/autostart/") + kLegacyDesktopId));
        QVERIFY(!QFile::exists(o.dataHome + QStringLiteral("/applications/") + kLegacyDesktopId));
        QVERIFY(os->isAutostart());
        QVERIFY(readAll(o.configHome + QStringLiteral("/autostart/octomx.desktop")).contains(QStringLiteral("Exec=%1/bin/octomx --minimized").arg(pkg)));
        const QString app = readAll(o.dataHome + QStringLiteral("/applications/octomx.desktop"));
        QVERIFY2(app.contains(QStringLiteral("Exec=%1/octomx %U").arg(pkg)), qPrintable(app));
        QVERIFY(app.contains(QStringLiteral("Icon=octomx\n")));
        QCOMPARE(readAll(o.dataHome + QStringLiteral("/icons/hicolor/48x48/apps/octomx.png")), QStringLiteral("new icon"));
        QVERIFY(!QFile::exists(o.dataHome + QStringLiteral("/icons/hicolor/48x48/apps/winmx.png")));
        const QString list = readAll(o.configHome + QStringLiteral("/mimeapps.list"));
        QVERIFY(!list.contains(kLegacyDesktopId));
        QVERIFY(list.contains(QStringLiteral("x-scheme-handler/magnet=octomx.desktop\n")));
        QVERIFY(list.contains(QStringLiteral("x-scheme-handler/magnet=octomx.desktop;other.desktop;")));
        QVERIFY(list.contains(QStringLiteral("text/plain=gedit.desktop")));
        QVERIFY(os->isHandler(OsIntegration::Assoc::Magnet));
        QVERIFY(os->isHandler(OsIntegration::Assoc::Torrent));
        QVERIFY(!os->isHandler(OsIntegration::Assoc::Ed2k));
        // a second start finds nothing left to move
        QVERIFY(os->migrateLegacy().isEmpty());
    }

    // [ren-1] entries of another copy of the program (another folder) are not touched
    void xdgLegacyOtherCopyStays()
    {
#ifdef Q_OS_WIN
        QSKIP("the XDG backend is not used on Windows");
#endif
        OsIntegration::Options o = opts(QStringLiteral("xdgother"), false);
        o.exePath = m_root + QStringLiteral("/xdgother/new/bin/octomx");
        const QString autoFile = o.configHome + QStringLiteral("/autostart/") + kLegacyDesktopId;
        const QString appFile = o.dataHome + QStringLiteral("/applications/") + kLegacyDesktopId;
        for (const QString &f : {autoFile, appFile}) {
            QDir().mkpath(QFileInfo(f).absolutePath());
            QFile out(f);
            QVERIFY(out.open(QIODevice::WriteOnly));
            out.write("[Desktop Entry]\nExec=/home/user/WinMX-4.5-linux-x86_64/winmx %U\nMimeType=x-scheme-handler/magnet;\n");
        }
        std::unique_ptr<OsIntegration> os(OsIntegration::createXdg(o));
        QVERIFY(os->migrateLegacy().isEmpty());
        QVERIFY(QFile::exists(autoFile) && QFile::exists(appFile));
        QVERIFY(!QFile::exists(o.dataHome + QStringLiteral("/applications/octomx.desktop")));
    }

    // [ren-1] Windows layout (INI): the legacy Run value, magnet: / ed2k: commands with winmx.exe and
    // the legacy .torrent class of this folder move; another copy's Run value stays
    void registryLegacyMoved()
    {
        OsIntegration::Options o = opts(QStringLiteral("reglegacy"), false);
        o.exePath = QStringLiteral("C:/Users/demo/OctoMX/octomx.exe");
        o.exeArgs.clear();
        {
            QSettings ini(o.registryIni, QSettings::IniFormat);
            ini.setValue(kLegacyRunKey, QStringLiteral("\"C:\\Users\\demo\\OctoMX\\winmx.exe\" --minimized"));
            ini.setValue(QStringLiteral("Microsoft/Windows/CurrentVersion/Run/Other"), QStringLiteral("\"C:\\Tools\\other.exe\""));
            ini.setValue(QStringLiteral("Classes/magnet/shell/open/command/."), QStringLiteral("\"C:\\Users\\demo\\OctoMX\\winmx.exe\" \"%1\""));
            ini.setValue(QStringLiteral("Classes/ed2k/shell/open/command/."), QStringLiteral("\"C:\\eMule\\emule.exe\" \"%1\""));
            ini.setValue(QStringLiteral("Classes/.torrent/."), kLegacyTorrentClass);
            ini.setValue(QStringLiteral("Classes/") + kLegacyTorrentClass + QStringLiteral("/shell/open/command/."), QStringLiteral("\"C:\\Users\\demo\\OctoMX\\winmx.exe\" \"%1\""));
        }
        std::unique_ptr<OsIntegration> os(OsIntegration::createRegistry(o));
        const QStringList moved = os->migrateLegacy();
        QCOMPARE(moved.size(), 3);
        QSettings ini(o.registryIni, QSettings::IniFormat);
        QVERIFY(!ini.contains(kLegacyRunKey));
        QCOMPARE(ini.value(QStringLiteral("Microsoft/Windows/CurrentVersion/Run/OctoMX")).toString(),
                 QStringLiteral("\"%1\" --minimized").arg(QDir::toNativeSeparators(o.exePath)));
        QVERIFY(ini.contains(QStringLiteral("Microsoft/Windows/CurrentVersion/Run/Other")));
        QVERIFY(ini.value(QStringLiteral("Classes/magnet/shell/open/command/.")).toString().contains(QStringLiteral("octomx.exe")));
        QVERIFY(ini.value(QStringLiteral("Classes/ed2k/shell/open/command/.")).toString().contains(QStringLiteral("emule.exe")));
        QCOMPARE(ini.value(QStringLiteral("Classes/.torrent/.")).toString(), QStringLiteral("OctoMX.torrent"));
        QVERIFY(!ini.contains(QStringLiteral("Classes/") + kLegacyTorrentClass + QStringLiteral("/shell/open/command/.")));
        QVERIFY(os->isAutostart());
        QVERIFY(os->isHandler(OsIntegration::Assoc::Magnet) && os->isHandler(OsIntegration::Assoc::Torrent));
        QVERIFY(!os->isHandler(OsIntegration::Assoc::Ed2k));
        QVERIFY(os->migrateLegacy().isEmpty());
    }

    // [ren-1] attach() moves them first: the start-up check then finds the new ones in place
    void attachMovesLegacyFirst()
    {
        OsIntegration::Options o = opts(QStringLiteral("regattach"), false);
        o.exePath = QStringLiteral("D:/Apps/OctoMX/octomx.exe");
        o.exeArgs.clear();
        {
            QSettings ini(o.registryIni, QSettings::IniFormat);
            ini.setValue(kLegacyRunKey, QStringLiteral("\"D:\\Apps\\OctoMX\\winmx.exe\" --minimized"));
        }
        MockOptions mo;
        mo.frozen = true;
        std::unique_ptr<Core> core(createMockCoreWithOptions(QString(), mo));
        core->settings()->setValue(keys::misc::StartWithOs, true);
        core->settings()->setValue(keys::misc::CheckAssociations, false);
        std::unique_ptr<OsIntegration> os(OsIntegration::createRegistry(o));
        os->attach(core->settings());
        QSettings ini(o.registryIni, QSettings::IniFormat);
        QVERIFY(!ini.contains(kLegacyRunKey));
        QVERIFY(os->isAutostart());
    }

    // The real HKCU\Software layout: only where the registry is disposable (the wine prefix of
    // toolchain/env.sh): opt in with MX_TEST_NATIVE_REGISTRY=1.
    void nativeRegistry()
    {
#ifndef Q_OS_WIN
        QSKIP("Windows only");
#else
        if (qEnvironmentVariable("MX_TEST_NATIVE_REGISTRY") != QLatin1String("1"))
            QSKIP("set MX_TEST_NATIVE_REGISTRY=1 (disposable wine prefix only)");
        OsIntegration::Options o;
        o.exePath = QStringLiteral("C:/OctoMXTest/octomx.exe");
        std::unique_ptr<OsIntegration> os(OsIntegration::createRegistry(o));
        QVERIFY(os->backendName().contains(QStringLiteral("HKCU")));
        QVERIFY(os->setAutostart(true));
        QVERIFY(os->isAutostart());
        QVERIFY(os->setHandler(OsIntegration::Assoc::Magnet, true));
        QVERIFY(os->setHandler(OsIntegration::Assoc::Torrent, true));
        QSettings hkcr(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes"), QSettings::NativeFormat);
        QCOMPARE(hkcr.value(QStringLiteral("magnet/shell/open/command/Default")).toString(),
                 QStringLiteral("\"C:\\OctoMXTest\\octomx.exe\" \"%1\""));
        QCOMPARE(hkcr.value(QStringLiteral(".torrent/Default")).toString(), QStringLiteral("OctoMX.torrent"));
        QVERIFY(os->isHandler(OsIntegration::Assoc::Magnet));
        QVERIFY(os->setHandler(OsIntegration::Assoc::Magnet, false));
        QVERIFY(os->setHandler(OsIntegration::Assoc::Torrent, false));
        QVERIFY(os->setAutostart(false));
        QVERIFY(!os->isAutostart() && !os->isHandler(OsIntegration::Assoc::Magnet));
#endif
    }
};

QTEST_MAIN(TstOsIntegration)
#include "tst_os_integration.moc"
