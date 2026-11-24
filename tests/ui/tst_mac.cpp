// SPDX-License-Identifier: GPL-3.0-or-later
// [mac-1] the macOS parts that can be tested everywhere (they are plain logic over a test seam) and,
// in the macOS build, against the real system: the program and portable folders of an app bundle,
// the package names and the update layout of the macOS package, the LaunchAgent and the Launch
// Services seam of the macOS OsIntegration backend, links and .torrent files from QFileOpenEvent,
// .app players and the trash's name. Never touches the real HOME, LaunchAgents or Launch Services
// (MX_OS_ROOT and folders under the build tree).
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QSettings>
#include <QtCore/QStandardPaths>
#include <QtCore/QTemporaryDir>
#include <QtGui/QFileOpenEvent>
#include <QtGui/QFontMetrics>
#include <QtTest/QtTest>

#include "mx/app_paths.h"
#include "mx/trash.h"
#include "mx/update_install.h"
#include "ui/kit/launcher.h"
#include "ui/kit/list_view.h"
#include "ui/kit/theme.h"
#include "ui/mac_glue.h"
#include "ui/os_integration.h"
#include "ui/tray.h"
#include "ui/dialogs/close_prompt.h"
#include "ui/main_window.h"
#include "uib_helpers.h"
#include <QtWidgets/QMenuBar>
#include <QtGui/QStandardItemModel>

using namespace mx;
using namespace mx::ui;

namespace {
QString readAll(const QString &p)
{
    QFile f(p);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}
bool writeFile(const QString &p, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(p).absolutePath());
    QFile f(p);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}
} // namespace

class TstMac : public QObject {
    Q_OBJECT
private:
    QString m_root;

private slots:
    void initTestCase()
    {
        m_root = QCoreApplication::applicationDirPath() + QStringLiteral("/tst_mac-root");
        QDir(m_root).removeRecursively();
        QVERIFY(QDir().mkpath(m_root));
    }
    void cleanupTestCase() { QDir(m_root).removeRecursively(); }

    void bundlePaths()
    {
        QCOMPARE(paths::bundleOf(QStringLiteral("/Applications/OctoMX.app/Contents/MacOS")), QStringLiteral("/Applications/OctoMX.app"));
        QCOMPARE(paths::bundleOf(QStringLiteral("/Users/u/Desktop/Octo MX copy.app/Contents/MacOS/")), QStringLiteral("/Users/u/Desktop/Octo MX copy.app"));
        QVERIFY(paths::bundleOf(QStringLiteral("/opt/OctoMX/bin")).isEmpty());
        QVERIFY(paths::bundleOf(QStringLiteral("/x/Contents/MacOS")).isEmpty());   // not an .app
        QVERIFY(paths::bundleOf(QString()).isEmpty());
        // the program as the user sees it, and where portable.txt goes
        QCOMPARE(paths::programDir(QStringLiteral("/Applications/OctoMX.app/Contents/MacOS")), QStringLiteral("/Applications/OctoMX.app"));
        QCOMPARE(paths::portableDir(QStringLiteral("/Volumes/Stick/OctoMX/OctoMX.app/Contents/MacOS")), QStringLiteral("/Volumes/Stick/OctoMX"));
        QCOMPARE(paths::programDir(QStringLiteral("C:/Programs/OctoMX")), QStringLiteral("C:/Programs/OctoMX"));
        QCOMPARE(paths::portableDir(QStringLiteral("/opt/OctoMX/bin")), QStringLiteral("/opt/OctoMX/bin"));
#ifdef Q_OS_MACOS
        // this test program is not inside a bundle: everything is its own folder
        QCOMPARE(paths::programDir(), QCoreApplication::applicationDirPath());
#endif
    }

    void standardFolders()
    {
        // the profile and the download folder the program uses by default (main.cpp,
        // QStandardPaths with the application name OctoMX)
        const QString old = QCoreApplication::applicationName();
        QCoreApplication::setApplicationName(QStringLiteral("OctoMX"));
        const QString data = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        const QString dl = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        QCoreApplication::setApplicationName(old);
        qInfo("profile: ...%s, downloads: ...%s", qPrintable(data.right(40)), qPrintable(dl.right(20)));
#ifdef Q_OS_MACOS
        QVERIFY2(data.endsWith(QLatin1String("/Library/Application Support/OctoMX")), qPrintable(data));
        QVERIFY2(dl.endsWith(QLatin1String("/Downloads")), qPrintable(dl));
#else
        QVERIFY(data.endsWith(QLatin1String("OctoMX")));
#endif
    }

    void packageNames()
    {
        const QString tag = QStringLiteral("macos-universal");
        QCOMPARE(upd::archiveName(QStringLiteral("5.2.0"), tag), QStringLiteral("OctoMX-5.2-macos-universal.zip"));
        QCOMPARE(upd::packageDirName(QStringLiteral("5.3.1"), tag), QStringLiteral("OctoMX-5.3-macos-universal"));
        QVERIFY(upd::isPackageArchiveName(QStringLiteral("OctoMX-5.3-macos-universal.zip"), tag));
        QVERIFY(!upd::isPackageArchiveName(QStringLiteral("OctoMX-5.3-macos-universal.tar.xz"), tag));
        QVERIFY(!upd::isPackageArchiveName(QStringLiteral("OctoMX-5.3-win64.zip"), tag));
        QVERIFY(!upd::isPackageArchiveName(QStringLiteral("OctoMX-5.3-macos-universal.zip"), QStringLiteral("win64")));
        QCOMPARE(upd::programFile(tag), QStringLiteral("OctoMX.app/Contents/MacOS/octomx"));
        QVERIFY(upd::launcherFile(tag).isEmpty());
        QVERIFY(upd::legacyStarterFiles(tag).isEmpty());
        QCOMPARE(upd::programDirOf(QStringLiteral("/Applications/x/OctoMX.app/Contents/MacOS"), tag), QStringLiteral("/Applications/x"));
        QVERIFY(upd::programDirOf(QStringLiteral("/opt/OctoMX/bin"), tag).isEmpty());
        // the other platforms are unchanged
        QCOMPARE(upd::archiveName(QStringLiteral("5.2.0"), QStringLiteral("linux-x86_64")), QStringLiteral("OctoMX-5.2-linux-x86_64.tar.xz"));
        QCOMPARE(upd::programFile(QStringLiteral("win64")), QStringLiteral("octomx.exe"));
#ifdef Q_OS_MACOS
        QVERIFY2(upd::platformTag().startsWith(QLatin1String("macos-")), qPrintable(upd::platformTag()));
#endif
    }

    // the macOS package is never installed by the program itself: the layout says why
    void macLayoutOffersDownloadPage()
    {
        const QString pkg = m_root + QStringLiteral("/pkg/OctoMX-5.2-macos-universal");
        const QString exe = pkg + QStringLiteral("/OctoMX.app/Contents/MacOS/octomx");
        QVERIFY(writeFile(exe, QByteArray("\xcf\xfa\xed\xfe", 4) + QByteArray(64, '\0')));
        QVERIFY(writeFile(pkg + QStringLiteral("/MANIFEST.txt"), "version 5.2.0\nplatform macos-universal\n"));
        const upd::Layout l = upd::detectLayout(QFileInfo(exe).absolutePath(), QStringLiteral("macos-universal"));
        QVERIFY(!l.ok);
        QCOMPARE(l.programDir, QDir::cleanPath(pkg));
        QVERIFY2(l.why.contains(QLatin1String("download")), qPrintable(l.why));
    }

    void stagedMacTree()
    {
        // a staged macOS package: the program must be a Mach-O file
        const QString stage = m_root + QStringLiteral("/stage");
        const QString top = stage + QStringLiteral("/OctoMX-5.2-macos-universal");
        const QByteArray macho = QByteArray("\xca\xfe\xba\xbe", 4) + QByteArray(100, 'x');
        QVERIFY(writeFile(top + QStringLiteral("/OctoMX.app/Contents/MacOS/octomx"), macho));
        QFile::setPermissions(top + QStringLiteral("/OctoMX.app/Contents/MacOS/octomx"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        upd::Manifest m;
        m.version = QStringLiteral("5.2.0");
        m.platform = QStringLiteral("macos-universal");
        m.files << upd::ManifestEntry{QStringLiteral("OctoMX.app/Contents/MacOS/octomx"), macho.size(),
                                      upd::sha256OfFile(top + QStringLiteral("/OctoMX.app/Contents/MacOS/octomx"))};
        QVERIFY(writeFile(top + QStringLiteral("/MANIFEST.txt"), upd::manifestText(m)));
        QString err, gotTop;
        QVERIFY2(upd::checkStagedTree(stage, QStringLiteral("5.2.0"), QStringLiteral("macos-universal"), &gotTop, nullptr, &err), qPrintable(err));
        // an ELF in its place is refused
        QVERIFY(writeFile(top + QStringLiteral("/OctoMX.app/Contents/MacOS/octomx"), QByteArray("\x7f" "ELF", 4) + QByteArray(100, 'x')));
        m.files[0].sha256 = upd::sha256OfFile(top + QStringLiteral("/OctoMX.app/Contents/MacOS/octomx"));
        QVERIFY(writeFile(top + QStringLiteral("/MANIFEST.txt"), upd::manifestText(m)));
        QVERIFY(!upd::checkStagedTree(stage, QStringLiteral("5.2.0"), QStringLiteral("macos-universal"), &gotTop, nullptr, &err));
        QVERIFY2(err.contains(QLatin1String("macOS")), qPrintable(err));
    }

    void launchAgent()
    {
        OsIntegration::Options o;
        o.exePath = QStringLiteral("/Applications/OctoMX & Co.app/Contents/MacOS/octomx");
        o.exeArgs = {QStringLiteral("--profile"), QStringLiteral("/Users/u/my <profile>")};
        o.launchAgentsDir = m_root + QStringLiteral("/LaunchAgents");
        o.launchServicesIni = m_root + QStringLiteral("/launchservices.ini");
        std::unique_ptr<OsIntegration> os(OsIntegration::createMac(o));
        QCOMPARE(os->backendName(), QStringLiteral("macos-test"));
        QVERIFY(!os->isAutostart());
        QVERIFY(os->setAutostart(true));
        QVERIFY(os->isAutostart());
        const QString plist = readAll(o.launchAgentsDir + QStringLiteral("/io.github.robtacconelli.octomx.plist"));
        QVERIFY(plist.contains(QLatin1String("<string>io.github.robtacconelli.octomx</string>")));
        QVERIFY(plist.contains(QLatin1String("<string>/Applications/OctoMX &amp; Co.app/Contents/MacOS/octomx</string>")));
        QVERIFY(plist.contains(QLatin1String("<string>/Users/u/my &lt;profile&gt;</string>")));
        QVERIFY(plist.contains(QLatin1String("<string>--minimized</string>")));
        QVERIFY(plist.contains(QLatin1String("<key>RunAtLoad</key>\n\t<true/>")));
        // another copy of the program does not count as "ours"
        OsIntegration::Options other = o;
        other.exePath = QStringLiteral("/Volumes/Old/OctoMX.app/Contents/MacOS/octomx");
        std::unique_ptr<OsIntegration> os2(OsIntegration::createMac(other));
        QVERIFY(!os2->isAutostart());
        QVERIFY(os->setAutostart(false));
        QVERIFY(!QFile::exists(o.launchAgentsDir + QStringLiteral("/io.github.robtacconelli.octomx.plist")));
        QVERIFY(!os->isAutostart());
    }

    void launchServicesSeam()
    {
        OsIntegration::Options o;
        o.exePath = QStringLiteral("/Applications/OctoMX.app/Contents/MacOS/octomx");
        o.launchAgentsDir = m_root + QStringLiteral("/LaunchAgents2");
        o.launchServicesIni = m_root + QStringLiteral("/ls2.ini");
        std::unique_ptr<OsIntegration> os(OsIntegration::createMac(o));
        for (auto a : {OsIntegration::Assoc::Magnet, OsIntegration::Assoc::Ed2k, OsIntegration::Assoc::Torrent}) {
            QVERIFY(!os->isHandler(a));
            QVERIFY(os->setHandler(a, true));
            QVERIFY(os->isHandler(a));
        }
        QSettings s(o.launchServicesIni, QSettings::IniFormat);
        QCOMPARE(s.value(QStringLiteral("handlers/magnet")).toString(), QStringLiteral("io.github.robtacconelli.octomx"));
        QCOMPARE(s.value(QStringLiteral("handlers/torrent")).toString(), QStringLiteral("io.github.robtacconelli.octomx"));
        // macOS cannot give a handler back: switching off keeps the system's choice
        QVERIFY(os->setHandler(OsIntegration::Assoc::Magnet, false));
        QVERIFY(os->isHandler(OsIntegration::Assoc::Magnet));
        // another program took it
        s.setValue(QStringLiteral("handlers/ed2k"), QStringLiteral("org.example.other"));
        s.sync();
        QVERIFY(!os->isHandler(OsIntegration::Assoc::Ed2k));
    }

    void appUsesRootOnMac()
    {
#ifdef Q_OS_MACOS
        const QByteArray old = qgetenv("MX_OS_ROOT");
        qputenv("MX_OS_ROOT", QFile::encodeName(m_root + QStringLiteral("/osroot")));
        std::unique_ptr<OsIntegration> os(OsIntegration::createForApp(false, {}));
        QVERIFY(os);
        QCOMPARE(os->backendName(), QStringLiteral("macos-test"));
        QCOMPARE(os->options.launchAgentsDir, m_root + QStringLiteral("/osroot/LaunchAgents"));
        QVERIFY(os->setAutostart(true));
        QVERIFY(QFile::exists(m_root + QStringLiteral("/osroot/LaunchAgents/io.github.robtacconelli.octomx.plist")));
        QVERIFY(os->setAutostart(false));
        qputenv("MX_OS_ROOT", old);
#else
        QSKIP("the macOS backend is the native one only on macOS");
#endif
    }

    void realLaunchServicesReadOnly()
    {
#ifdef Q_OS_MACOS
        // the real Launch Services, read only: asking who opens magnet: links must work (no
        // handler is set here; the answer depends on the machine)
        OsIntegration::Options o;
        o.launchAgentsDir = m_root + QStringLiteral("/LaunchAgents3");
        std::unique_ptr<OsIntegration> os(OsIntegration::createMac(o));
        QCOMPARE(os->backendName(), QStringLiteral("macos"));
        const bool h = os->isHandler(OsIntegration::Assoc::Magnet);
        qInfo("magnet: handled by OctoMX on this Mac: %d", int(h));
        // this test program is not an app bundle: claiming a handler is refused with a reason
        QVERIFY(!os->setHandler(OsIntegration::Assoc::Magnet, true));
        QVERIFY2(os->lastError().contains(QLatin1String("bundle")), qPrintable(os->lastError()));
#else
        QSKIP("macOS only");
#endif
    }

    void openEvents()
    {
        QCOMPARE(OpenEventFilter::linkOf(QString(), QStringLiteral("magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567&dn=Big+Buck+Bunny")),
                 QStringLiteral("magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567&dn=Big+Buck+Bunny"));
        QCOMPARE(OpenEventFilter::linkOf(QString(), QStringLiteral("ed2k://|file|big_buck_bunny_720p.mp4|61878609|0123456789ABCDEF0123456789ABCDEF|/")),
                 QStringLiteral("ed2k://|file|big_buck_bunny_720p.mp4|61878609|0123456789ABCDEF0123456789ABCDEF|/"));
        QVERIFY(OpenEventFilter::linkOf(QString(), QStringLiteral("https://example.org/")).isEmpty());
        QVERIFY(OpenEventFilter::linkOf(m_root + QStringLiteral("/notes.txt"), QString()).isEmpty());
        const QString t = m_root + QStringLiteral("/Big Buck Bunny.torrent");
        QCOMPARE(OpenEventFilter::linkOf(t, QString()), t);
        // delivered to the application object, as macOS does
        OpenEventFilter f;
        QSignalSpy spy(&f, &OpenEventFilter::linksReceived);
        QFileOpenEvent e1(t);
        QCoreApplication::sendEvent(QCoreApplication::instance(), &e1);
        QFileOpenEvent e2(QUrl(QStringLiteral("magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567")));
        QCoreApplication::sendEvent(QCoreApplication::instance(), &e2);
        QFileOpenEvent e3(m_root + QStringLiteral("/picture.png"));
        QCoreApplication::sendEvent(QCoreApplication::instance(), &e3);
        QCOMPARE(spy.count(), 2);
        QCOMPARE(spy.at(0).at(0).toStringList(), QStringList{t});
        QVERIFY(spy.at(1).at(0).toStringList().value(0).startsWith(QLatin1String("magnet:?xt=urn:btih:")));
    }

    void playersAndTrash()
    {
#ifdef Q_OS_MACOS
        QCOMPARE(trash::placeName(), QStringLiteral("Trash"));
        // QuickTime Player comes with macOS: it is found as an application bundle
        QVERIFY(launcher::isAppBundle(QStringLiteral("/System/Applications/QuickTime Player.app")));
        QVERIFY(!launcher::isAppBundle(QStringLiteral("/bin/ls")));
        bool qt = false;
        for (const launcher::Player &p : launcher::findPlayers()) {
            qInfo("player: %s = %s", qPrintable(p.name), qPrintable(p.path));
            qt = qt || p.path.endsWith(QLatin1String("QuickTime Player.app"));
        }
        QVERIFY(qt);
#else
        QVERIFY(!launcher::isAppBundle(QStringLiteral("/usr/bin")));
        QVERIFY(trash::placeName() != QLatin1String("Trash"));
#endif
    }

    // the UI font lays text out with the advances of its 11-px bitmaps on every platform (on macOS
    // through the macOS copies of the font, patch_tahoma.py --mac-advances): the same text widths
    void fontAdvances()
    {
        installClassicLook(qApp);
        const QFontMetrics fm(normalFont());
        QString all;
        for (char ch = 33; ch < 127; ++ch)
            all += QStringLiteral("%1").arg(fm.horizontalAdvance(QLatin1Char(ch)));
        const QString sentence = QStringLiteral("OctoMX: file sharing with the classic WinMX-style interface on modern peer-to-peer networks.");
        qInfo("font %s, advances 33..126: %s", qPrintable(normalFont().family()), qPrintable(all));
        qInfo("sentence width %d (sum of advances %d)", fm.horizontalAdvance(sentence), [&] {
            int w = 0;
            for (const QChar c : sentence) w += fm.horizontalAdvance(c);
            return w;
        }());
        QCOMPARE(fm.horizontalAdvance(QLatin1Char('C')), 7);
        QCOMPARE(fm.horizontalAdvance(QLatin1Char('h')), 6);
        QCOMPARE(fm.horizontalAdvance(QLatin1Char('m')), 8);
        QCOMPARE(fm.horizontalAdvance(QLatin1Char('o')), 6);
        QCOMPARE(fm.horizontalAdvance(QStringLiteral("mmmmmmmmmm")), 80);
        int sum = 0;
        for (const QChar c : sentence)
            sum += fm.horizontalAdvance(c);
        QCOMPARE(fm.horizontalAdvance(sentence), sum);   // no pair kerning (GDI does not kern it)
    }

    // the native menu bar, the Dock menu and the Quit path of the Mac (menus by their roles; Quit asks
    // as the close prompt does: "Keep running in the tray" hides, "Exit" quits)
    void macGlue()
    {
        uib::App app;
        app.start(m_root + QStringLiteral("/glue-profile"));
        QMenuBar *bar = installMacGlue(app.mw);
#ifndef Q_OS_MACOS
        QVERIFY(!bar);
        app.stop();
#else
        QVERIFY(bar);
        QHash<int, QAction *> byRole;
        for (QMenu *m : bar->findChildren<QMenu *>())
            for (QAction *a : m->actions())
                byRole.insert(int(a->menuRole()), a);
        QVERIFY(byRole.contains(int(QAction::AboutRole)));
        QVERIFY(byRole.contains(int(QAction::PreferencesRole)));
        QVERIFY(byRole.contains(int(QAction::QuitRole)));
        QCOMPARE(byRole.value(int(QAction::PreferencesRole))->shortcut(), QKeySequence(QKeySequence::Preferences));
        QCOMPARE(byRole.value(int(QAction::QuitRole))->shortcut(), QKeySequence(QKeySequence::Quit));
        QVERIFY(app.mw->findChild<QMenu *>(QStringLiteral("dockMenu")));
        TrayController *t = TrayController::of(app.mw);
        QVERIFY(t);
        TrayController::setTrayAvailableOverride(1);
        // Quit with "keep running in the tray": the window hides, nothing quits
        app.core->settings()->setValue(QStringLiteral("misc/closeAction"), QStringLiteral("tray"));
        QSignalSpy quit(app.mw, &MainWindow::quitRequested);
        byRole.value(int(QAction::QuitRole))->trigger();
        QTRY_VERIFY(t->isHiddenToTray());
        QCOMPARE(quit.count(), 0);
        t->restore();
        QTRY_VERIFY(app.mw->isVisible());
        // the prompt answers "Exit": the program quits
        app.core->settings()->setValue(QStringLiteral("misc/closeAction"), QStringLiteral("ask"));
        TrayController::setPromptHook([](bool, bool *) { return int(ClosePrompt::ExitNow); });
        byRole.value(int(QAction::QuitRole))->trigger();
        QTRY_VERIFY(t->isQuitting());
        TrayController::setPromptHook({});
        TrayController::setTrayAvailableOverride(-1);
        app.stop();
#endif
    }

    // the delete key on a list: Del (and on the Mac Backspace, its main delete key) asks to delete,
    // whatever keypad flag the platform puts on the key
    void deleteKeyOnLists()
    {
        struct Probe : QWidget {
            Qt::KeyboardModifiers mods;
            void keyPressEvent(QKeyEvent *e) override { mods = e->modifiers(); }
        } probe;
        QTest::keyClick(&probe, Qt::Key_Delete);
        qInfo("Key_Delete arrives with modifiers 0x%x", unsigned(probe.mods));
        QStandardItemModel model(2, 1);
        MxListView view;
        view.setModel(&model);
        int asked = 0;
        connect(&view, &MxListView::deleteRequested, this, [&asked] { ++asked; });
        view.selectionModel()->select(model.index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        QTest::keyClick(&view, Qt::Key_Delete);
        QCOMPARE(asked, 1);
        QTest::keyClick(&view, Qt::Key_Delete, Qt::KeypadModifier);
        QCOMPARE(asked, 2);
        QTest::keyClick(&view, Qt::Key_Backspace);
#ifdef Q_OS_MACOS
        QCOMPARE(asked, 3);
#else
        QCOMPARE(asked, 2);
#endif
    }

    void caseInsensitiveVolume()
    {
        // APFS (the macOS default) ignores case: a second download "BIG BUCK BUNNY.mp4" next to
        // "big buck bunny.mp4" must get its own name (the engine checks QFileInfo::exists)
        const QString a = m_root + QStringLiteral("/case/big buck bunny.mp4");
        QVERIFY(writeFile(a, "x"));
        const bool insensitive = QFileInfo::exists(m_root + QStringLiteral("/case/BIG BUCK BUNNY.mp4"));
        qInfo("this volume ignores case: %d", int(insensitive));
#ifdef Q_OS_MACOS
        QVERIFY(insensitive);   // the runner's volume is APFS, case-insensitive
#endif
    }
};

QTEST_MAIN(TstMac)
#include "tst_mac.moc"
