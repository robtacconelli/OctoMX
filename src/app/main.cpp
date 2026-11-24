// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX: the executable. See docs/ARCHITECTURE.md "Command line".
//
//   octomx [--profile <dir>] [--demo] [--demo-frozen] [--page <name>] [--size WxH]
//         [--screenshot <out.png>] [--screenshot-delay <ms>] [--script <file>] [--minimized]
//         [--no-wizard] [<magnet:|ed2k://|http(s)://|file.torrent> ...]
//   octomx --export-icons <dir>      (writes the app icon as PNG/ICO; used to make src/app/res)
//   octomx --verify-update <dir> [--verify-report <file>]       [upd-1] SHA256SUMS.sig against the
//         built-in release keys, then every archive of SHA256SUMS in <dir> (release verify scripts)
//   octomx --verify-manifest <package dir> [--verify-report <file>]   [upd-1] MANIFEST.txt: every file

#include <QtCore/QCommandLineParser>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtGui/QScreen>
#include <QtWidgets/QAbstractButton>

#include <memory>
#include <QtGui/QFontMetrics>
#include <QtGui/QIcon>
#include <QtGui/QImageWriter>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QUdpSocket>
#include <QtWidgets/QApplication>

#include "app_log.h"
#include "mock/mock_core.h"
#include "mx/app_paths.h"
#include "mx/legacy_names.h"
#include "mx/net_policy.h"
#include "mx/services.h"
#include "mx/settings_keys.h"
#include "mx/update_check.h"
#include "single_instance.h"
#include "ui/kit/colors.h"
#include "ui/kit/icons.h"
#include "ui/kit/session.h"
#include "ui/kit/scale.h"
#include "ui/kit/theme.h"
#include "ui/dialogs/closing_window.h"
#include "ui/dialogs/legal_notice.h"
#include "ui/dialogs/setup_wizard.h"
#include "ui/mac_glue.h"
#include "ui/main_window.h"
#include "ui/os_integration.h"
#include "ui/script_driver.h"
#include "ui/tray.h"
#include "ui/update_ui.h"
#include "ui/kit/translation.h"
#include "mx/update_install.h"
#include "mx/quit_signals.h"
#include <QtCore/QDirIterator>
#include <QtGui/QSessionManager>
#include <QtWidgets/QMessageBox>

#ifdef MX_HAVE_CORE
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QTcpSocket>
#include "core/diag/active_checks.h"
#include "core/diag/port_mapper.h"
#include "core/log.h"
#include "core/real_core.h"
#include "mx/plugin.h"
#endif
#ifdef MX_HAVE_BT
#include "bt_plugin.h"
#endif
#ifdef MX_HAVE_ED2K
#include "ed2k_plugin.h"
#endif
#ifdef MX_HAVE_WEB
#include "web_plugin.h"
#endif
#ifdef MX_HAVE_GNUTELLA
#include "gnutella_plugin.h"
#endif
#ifdef MX_HAVE_HTTP
#include "http_plugin.h"
#endif
#ifdef MX_HAVE_OVERLAY
#include "overlay_plugin.h"
#endif

#ifdef Q_OS_WIN
#include <windows.h>
#include <cstdio>
#endif
#ifdef Q_OS_MACOS
#include <CoreFoundation/CoreFoundation.h>
#include <objc/message.h>
#include <objc/runtime.h>
#endif

namespace {

QString defaultProfileDir()
{
    // [mac-1] beside the executable; on macOS beside the app bundle
    const QString exeDir = mx::paths::portableDir();
    if (QFileInfo::exists(QDir(exeDir).filePath(QStringLiteral("portable.txt"))))
        return QDir(exeDir).filePath(QStringLiteral("profile"));
    // [ren-1] <AppData>/OctoMX; the legacy profile folder <AppData>/WinMX of a copy installed before
    // 5.0 (with its settings.json) is used as it is while the new folder has no settings, at every
    // start: nothing is moved or copied
    return mx::legacy::chooseProfileDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
}

// [ui-e] the process DPI awareness (Windows): "per-monitor v2" expected (manifest + Qt)
const char *dpiAwarenessText()
{
#ifdef Q_OS_WIN
    using GetCtx = void *(WINAPI *)();
    using Equal = BOOL(WINAPI *)(void *, void *);
    HMODULE u = GetModuleHandleW(L"user32.dll");
    auto getCtx = u ? reinterpret_cast<GetCtx>(GetProcAddress(u, "GetThreadDpiAwarenessContext")) : nullptr;
    auto equal = u ? reinterpret_cast<Equal>(GetProcAddress(u, "AreDpiAwarenessContextsEqual")) : nullptr;
    if (getCtx && equal) {
        void *c = getCtx();
        if (equal(c, reinterpret_cast<void *>(intptr_t(-4))))   // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
            return "per-monitor v2";
        if (equal(c, reinterpret_cast<void *>(intptr_t(-3))))   // PER_MONITOR_AWARE
            return "per-monitor";
        if (equal(c, reinterpret_cast<void *>(intptr_t(-2))))   // SYSTEM_AWARE
            return "system";
        if (equal(c, reinterpret_cast<void *>(intptr_t(-1))))   // UNAWARE
            return "unaware";
    }
    return "unknown";
#else
    return "n/a (not Windows)";
#endif
}

// [mac-1] App Nap: macOS slows down the timers and the network of a program whose windows are hidden
// or covered; a file-sharing client keeps transferring and serving in the background, as on the
// other systems. [NSProcessInfo beginActivityWithOptions:reason:] through the Objective-C runtime
// (NSActivityUserInitiatedAllowingIdleSystemSleep: the Mac may still go to sleep when idle).
void keepRunningInBackground()
{
#ifdef Q_OS_MACOS
    Class cls = objc_getClass("NSProcessInfo");
    if (!cls)
        return;
    using GetInfo = id (*)(Class, SEL);
    using Begin = id (*)(id, SEL, unsigned long long, id);
    id info = reinterpret_cast<GetInfo>(objc_msgSend)(cls, sel_registerName("processInfo"));
    if (!info)
        return;
    const unsigned long long userInitiatedAllowingIdleSleep = 0x00FFFFFFULL & ~(1ULL << 20);
    CFStringRef reason = CFSTR("OctoMX transfers and shares files in the background");
    id activity = reinterpret_cast<Begin>(objc_msgSend)(info, sel_registerName("beginActivityWithOptions:reason:"),
                                                         userInitiatedAllowingIdleSleep, (id)reason);
    if (activity)
        CFRetain(reinterpret_cast<CFTypeRef>(activity));   // kept for the life of the process
#endif
}

bool exportIcons(const QString &dir)
{
    QDir().mkpath(dir);
    bool ok = true;
    for (int s : {16, 20, 24, 32, 40, 48, 64, 128, 256, 512, 1024})   // [logo-1] the .ico sizes (tools/make-ico.py), [mac-1] 512 and 1024 for the .icns (tools/make-icns.py)
        ok &= mx::ui::iconImage(mx::ui::IconId::App, s).save(QDir(dir).filePath(QStringLiteral("octomx-%1.png").arg(s)));
    // multi-size .ico through Qt's ico writer (one image per call is all it supports): write the
    // 48 px one; tools/make-ico (python) can merge the PNGs if a multi-size file is wanted.
    QImageWriter w(QDir(dir).filePath(QStringLiteral("octomx-48.ico")), "ico");
    ok &= w.write(mx::ui::iconImage(mx::ui::IconId::App, 48));
    // [ui-e] every UI icon at the interface sizes (review of the pixel art at 100 ... 300 %)
    QDir().mkpath(QDir(dir).filePath(QStringLiteral("ui")));
    for (mx::ui::IconId id : mx::ui::allIcons())
        for (int s : {16, 20, 24, 28, 32, 40, 48})
            ok &= mx::ui::iconImage(id, s).save(QDir(dir).filePath(QStringLiteral("ui/%1-%2.png").arg(mx::ui::iconName(id)).arg(s)));
    return ok;
}

mx::Core *createCore(const QString &profile, bool demo, bool frozen)
{
    if (!demo) {
#ifdef MX_HAVE_CORE
        QList<mx::NetworkPlugin *> plugins;
#ifdef MX_HAVE_BT
        plugins << mx::createBitTorrentPlugin();
#endif
#ifdef MX_HAVE_ED2K
        plugins << mx::createEd2kPlugins();
#endif
#ifdef MX_HAVE_WEB
        plugins << mx::createWebSearchPlugin();
#endif
#ifdef MX_HAVE_GNUTELLA
        plugins << mx::createGnutellaPlugin();
#endif
#ifdef MX_HAVE_HTTP
        plugins << mx::createHttpPlugin();
#endif
#ifdef MX_HAVE_OVERLAY
        plugins << mx::createOverlayPlugin();
        plugins << mx::createRatsSearchPlugin();   // rats-search network (also in overlay_plugin.h)
#endif
        plugins.removeAll(nullptr);
        return mx::createRealCore(profile, plugins);
#else
        qWarning("octomx: built without the real core (MX_UI_ONLY); running the demo core");
#endif
    }
    mx::MockOptions o;
    o.frozen = frozen;
    return mx::createMockCoreWithOptions(profile, o);
}

// Setup Wizard port test [ui-c]: can this computer listen on the port? (Reachability from the
// internet needs an outside peer; the wizard says so.)
mx::ui::SetupWizard::PortTest testPort(mx::Settings *s, int port, bool udp)
{
    mx::ui::SetupWizard::PortTest r;
    if (port < 1 || port > 65535) {
        r.message = QObject::tr("%1 is not a valid port number (1 - 65535).").arg(port);
        return r;
    }
    if (udp) {
        QUdpSocket sock;
        r.ok = sock.bind(QHostAddress::AnyIPv4, quint16(port));
    } else {
        QTcpServer srv;
        r.ok = srv.listen(QHostAddress::AnyIPv4, quint16(port));
    }
    const int current = s->i(udp ? mx::keys::connection::UdpPort : mx::keys::connection::TcpPort);
    if (r.ok)
        r.message = QObject::tr("Port %1 is free: OctoMX can use it.").arg(port);
    else if (port == current)
        r.message = QObject::tr("Port %1 is in use (by OctoMX itself when it is connected).").arg(port);
    else
        r.message = QObject::tr("Port %1 is in use by another program. Choose another port.").arg(port);
    if (port == current && !r.ok)
        r.ok = true;
    return r;
}

#ifdef MX_HAVE_CORE
// [diag-1] the wizard's real port test: the free-port check first (testPort), then the router is
// asked to forward the port (the shared PortMapper) and, for TCP, the public port checker
// (diag/portCheckUrl) whether it can be reached; the port is held open meanwhile and the test
// mapping removed at the end. Only runs when the user presses Test Port.
void realPortTest(mx::Settings *s, int port, bool udp, bool mapPorts, QObject *ctx,
                  const mx::ui::SetupWizard::PortTestReport &report)
{
    using PT = mx::ui::SetupWizard::PortTest;
    const PT free = testPort(s, port, udp);
    if (!free.ok) {
        report(free, true);
        return;
    }
    // [net-2] behind a proxy nobody can reach this computer, and the router is not asked
    if (mx::netpolicy::fromSettings(s).proxy.active()) {
        report(PT{true, free.message + QObject::tr(" A proxy is set: OctoMX does not listen and maps no ports then, "
                                                   "incoming connections cannot arrive through a proxy.")}, true);
        return;
    }
    // hold the port while the router and the checker test it (busy = OctoMX itself uses it: done)
    auto *holder = new QObject(ctx);
    bool held = false;
    if (udp) {
        auto *u = new QUdpSocket(holder);
        held = u->bind(QHostAddress::AnyIPv4, quint16(port));
    } else {
        auto *t = new QTcpServer(holder);
        held = t->listen(QHostAddress::AnyIPv4, quint16(port));
        QObject::connect(t, &QTcpServer::newConnection, t, [t] { while (QTcpSocket *c = t->nextPendingConnection()) c->deleteLater(); });
    }
    if (!held) {
        delete holder;
        report(free, true);
        return;
    }
    const QString proto = udp ? QStringLiteral("UDP") : QStringLiteral("TCP");
    auto state = std::make_shared<QString>(free.message);
    auto done = std::make_shared<bool>(false);
    auto *mapper = new mx::diag::PortMapper(holder);
    mapper->setAdapter(mx::netpolicy::fromSettings(s).bind);   // [net-2] the chosen adapter only
    auto finish = [holder, mapper, report, state, done](bool ok, const QString &more) {
        if (*done) return;
        *done = true;
        report(PT{ok, *state + QLatin1Char(' ') + more}, true);
        mapper->shutdown(1500);
        holder->deleteLater();
    };
    auto checkOnline = [s, port, udp, holder, finish, state](const QString &mapped) {
        const QString tmpl = s->s(mx::keys::diag::PortCheckUrl).trimmed();
        if (udp || tmpl.isEmpty() || !tmpl.contains(QLatin1String("%1"))) {
            finish(true, mapped + (udp ? QObject::tr(" UDP cannot be tested from the outside here: the Connection Test checks it once OctoMX is connected.") : QString()));
            return;
        }
        const QString host = QUrl(QString(tmpl).replace(QLatin1String("%1"), QString::number(port))).host();
        *state += QLatin1Char(' ') + mapped;
        auto *nam = new QNetworkAccessManager(holder);
        mx::netpolicy::applyTo(nam, s);   // [net-2]
        mx::diag::publicPortCheck(holder, nam, tmpl, port, 15000, [finish, host](const mx::diag::CheckResult &r) {
            if (!r.answered) finish(true, QObject::tr("%1 gave no answer (%2), so reachability is unknown.").arg(host, r.error));
            else if (r.ok) finish(true, QObject::tr("%1 reached the port: incoming connections work.").arg(host));
            else finish(false, QObject::tr("%1 could not reach the port: forward it in your router (or choose \"Unable to accept incoming TCP connections\").").arg(host));
        });
    };
    if (!mapPorts) {
        *state += QObject::tr(" Automatic port mapping is off.");
        checkOnline(QString());
        return;
    }
    report(PT{true, *state + QObject::tr(" Asking the router to forward it...")}, false);
    QObject::connect(mapper, &mx::diag::PortMapper::mappingChanged, holder, [mapper, checkOnline, done](const mx::PortMapping &m) {
        if (*done || m.result == mx::PortMapping::Pending) return;
        const mx::diag::RouterInfo r = mapper->router();
        const QString name = r.friendlyName.isEmpty() ? r.address : QStringLiteral("%1 (%2)").arg(r.friendlyName, r.address);
        if (m.result == mx::PortMapping::Mapped)
            checkOnline(m.externalPort == m.internalPort ? QObject::tr("The router %1 forwards it.").arg(name)
                                                         : QObject::tr("The router %1 forwards it as external port %2.").arg(name).arg(m.externalPort));
        else
            checkOnline(QObject::tr("The router did not forward it (%1).").arg(m.error));
    });
    mapper->setEnabled(s->b(mx::keys::connection::Upnp) || !s->b(mx::keys::connection::NatPmp), s->b(mx::keys::connection::NatPmp));
    mx::diag::PortMapper::Want w;
    w.proto = proto;
    w.port = port;
    w.label = QStringLiteral("Setup Wizard test");
    mapper->setWanted({w});
    QTimer::singleShot(30000, holder, [finish] { finish(true, QObject::tr("The router did not answer in time.")); });
}
#endif

// [upd-1] --verify-update / --verify-manifest: the release scripts check a release with the
// program's own verifier (no window, no profile, no network). -1 = not such a command line.
int verifyCommand(int argc, char **argv)
{
    QString updateDir, manifestDir, report;
    for (int i = 1; i + 1 < argc; ++i) {
        const QByteArray a(argv[i]);
        if (a == "--verify-update")
            updateDir = QString::fromLocal8Bit(argv[i + 1]);
        else if (a == "--verify-manifest")
            manifestDir = QString::fromLocal8Bit(argv[i + 1]);
        else if (a == "--verify-report")
            report = QString::fromLocal8Bit(argv[i + 1]);
    }
    if (updateDir.isEmpty() && manifestDir.isEmpty())
        return -1;
    QCoreApplication app(argc, argv);
    QStringList out;
    bool ok = true;
    auto line = [&](bool good, const QString &t) {
        out << (good ? QStringLiteral("OK   ") : QStringLiteral("FAIL ")) + t;
        ok = ok && good;
    };
    if (!updateDir.isEmpty()) {
        QFile sums(QDir(updateDir).filePath(QStringLiteral("SHA256SUMS"))), sig(QDir(updateDir).filePath(QStringLiteral("SHA256SUMS.sig")));
        if (!sums.open(QIODevice::ReadOnly)) {
            line(false, QStringLiteral("no SHA256SUMS in %1").arg(updateDir));
        } else if (!sig.open(QIODevice::ReadOnly)) {
            line(false, QStringLiteral("no SHA256SUMS.sig in %1: the release is not signed").arg(updateDir));
        } else {
            const QByteArray sumsText = sums.readAll();
            const mx::upd::SigCheck sc = mx::upd::verifySums(sumsText, sig.readAll());
            line(sc.ok, sc.ok ? QStringLiteral("SHA256SUMS signature: Ed25519, key %1 (built into this program)").arg(sc.keyId)
                              : QStringLiteral("SHA256SUMS signature: %1").arg(sc.error));
            const QHash<QString, QByteArray> list = mx::upd::parseSums(sumsText);
            int present = 0;
            for (auto it = list.constBegin(); it != list.constEnd(); ++it) {
                const QString f = QDir(updateDir).filePath(it.key());
                if (!QFileInfo::exists(f)) {
                    out << QStringLiteral("INFO %1 is listed but not in %2").arg(it.key(), updateDir);
                    continue;
                }
                ++present;
                const QByteArray h = mx::upd::sha256OfFile(f);
                line(h == it.value(), QStringLiteral("%1: SHA-256 %2 the signed list").arg(it.key(), h == it.value() ? QStringLiteral("matches") : QStringLiteral("does NOT match")));
            }
            line(present > 0 && !list.isEmpty(), QStringLiteral("%1 of %2 listed archives checked").arg(present).arg(list.size()));
        }
    }
    if (!manifestDir.isEmpty()) {
        mx::upd::Manifest m;
        QString why;
        if (!mx::upd::readManifest(manifestDir, &m, &why)) {
            line(false, QStringLiteral("MANIFEST.txt: %1").arg(why));
        } else {
            const bool tree = mx::upd::verifyTree(manifestDir, m, &why);
            line(tree, tree ? QStringLiteral("MANIFEST.txt: version %1, platform %2, %3 files, every size and SHA-256 matches")
                                  .arg(m.version, m.platform).arg(m.files.size())
                            : QStringLiteral("MANIFEST.txt: %1").arg(why));
            QStringList extra;
            // [mac-1] symbolic links are not files of the manifest (the framework layout of a macOS app
            // bundle has them; the other packages have none, tools/release/manifest.sh refuses them)
            QDirIterator it(manifestDir, QDir::Files | QDir::Hidden | QDir::System | QDir::NoSymLinks, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const QString rel = QDir(manifestDir).relativeFilePath(it.next());
                if (rel != QLatin1String(mx::upd::kManifestName) && !m.contains(rel))
                    extra << rel;
            }
            line(extra.isEmpty(), extra.isEmpty() ? QStringLiteral("MANIFEST.txt lists every file of the package")
                                                  : QStringLiteral("files not in MANIFEST.txt: %1").arg(extra.join(QLatin1Char(' ')).left(400)));
        }
    }
    out << (ok ? QStringLiteral("RESULT: verified") : QStringLiteral("RESULT: FAILED"));
    const QByteArray text = out.join(QLatin1Char('\n')).toUtf8() + '\n';
    fwrite(text.constData(), 1, size_t(text.size()), stdout);
    fflush(stdout);
    if (!report.isEmpty()) {
        QFile r(report);
        if (r.open(QIODevice::WriteOnly | QIODevice::Truncate))
            r.write(text);
    }
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char **argv)
{
#ifdef Q_OS_WIN
    // GUI subsystem exe: still print to the console when started from one (--screenshot etc.)
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
#endif
    if (const int rc = verifyCommand(argc, argv); rc >= 0)   // [upd-1]
        return rc;
    mx::ui::preApplicationSetup();
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral(MX_APP_NAME));
    QCoreApplication::setApplicationVersion(QStringLiteral(MX_VERSION_STRING));
    QGuiApplication::setApplicationDisplayName(QString());
    QGuiApplication::setQuitOnLastWindowClosed(false);
    mx::registerMetaTypes();
    keepRunningInBackground();   // [mac-1]

    QCommandLineParser cl;
    cl.setApplicationDescription(QStringLiteral("OctoMX %1: a file-sharing client with the classic WinMX-style interface.")
                                     .arg(QStringLiteral(MX_VERSION_STRING)));
    cl.addHelpOption();
    cl.addVersionOption();
    const QCommandLineOption oProfile(QStringLiteral("profile"), QStringLiteral("Profile directory."), QStringLiteral("dir"));
    const QCommandLineOption oDemo(QStringLiteral("demo"), QStringLiteral("Use the demo core (no network)."));
    const QCommandLineOption oFrozen(QStringLiteral("demo-frozen"), QStringLiteral("Demo core without time-based changes."));
    const QCommandLineOption oPage(QStringLiteral("page"), QStringLiteral("Start page."), QStringLiteral("name"));
    const QCommandLineOption oSize(QStringLiteral("size"), QStringLiteral("Window client size."), QStringLiteral("WxH"));
    const QCommandLineOption oShot(QStringLiteral("screenshot"), QStringLiteral("Grab the window and exit."), QStringLiteral("png"));
    const QCommandLineOption oDelay(QStringLiteral("screenshot-delay"), QStringLiteral("Delay before the grab."), QStringLiteral("ms"), QStringLiteral("800"));
    const QCommandLineOption oScript(QStringLiteral("script"), QStringLiteral("Run a UI script."), QStringLiteral("file"));
    const QCommandLineOption oMin(QStringLiteral("minimized"), QStringLiteral("Start minimized."));
    const QCommandLineOption oIcons(QStringLiteral("export-icons"), QStringLiteral("Write the app icon files."), QStringLiteral("dir"));
    const QCommandLineOption oNoWizard(QStringLiteral("no-wizard"), QStringLiteral("Do not show the Setup Wizard on the first start."));
    for (const auto *o : {&oProfile, &oDemo, &oFrozen, &oPage, &oSize, &oShot, &oDelay, &oScript, &oMin, &oIcons, &oNoWizard})
        cl.addOption(*o);
    cl.addPositionalArgument(QStringLiteral("links"), QStringLiteral("magnet:, ed2k://, http(s):// links or .torrent files."));
    cl.process(app);

    if (cl.isSet(oIcons))
        return exportIcons(cl.value(oIcons)) ? 0 : 1;

    const QString profile = QDir(cl.isSet(oProfile) ? cl.value(oProfile) : defaultProfileDir()).absolutePath();
    QDir().mkpath(profile);
    // An explicit --profile keeps every default folder (incoming files, shared folders) inside that
    // profile, so a test or portable run never writes into the user's own Downloads folder.
    if (cl.isSet(oProfile))
        qputenv("MX_PROFILE_EXPLICIT", "1");
    const QStringList links = cl.positionalArguments();
    const bool automation = cl.isSet(oShot) || cl.isSet(oScript);

    // [upd-1] after an update restart: wait until the previous process has ended (it held the
    // single-instance server and the program files); the restarted program gets the same options
    {
        QStringList again;
        QStringList seen;
        for (const QString &n : cl.optionNames()) {
            if (seen.contains(n))
                continue;
            seen << n;
            const QStringList vals = cl.values(n);
            if (vals.isEmpty())
                again << QStringLiteral("--") + n;
            for (const QString &v : vals)
                again << QStringLiteral("--") + n << v;
        }
        mx::ui::UpdateController::setRestartArguments(again);
        if (!mx::ui::UpdateController::waitForPreviousProcess())   // [safe-1] never two on one profile
            return 0;
    }

    // single instance (not for screenshots / scripts, which use their own profiles)
    mx::app::SingleInstance single(profile);
    if (!automation) {
        if (single.forwardToRunning(links))
            return 0;
        single.listen();
    }
    // [qa-2] links a second instance hands over before the main window exists (first start: the Setup
    // Wizard is open) were dropped while the sender was told "ok"; they wait here instead
    QStringList earlyLinks;
    QMetaObject::Connection earlyConn = QObject::connect(&single, &mx::app::SingleInstance::linksReceived, &app,
                                                         [&earlyLinks](const QStringList &l) { earlyLinks << l; });
    // [mac-1] links and .torrent files the system hands over (macOS: QFileOpenEvent), also at start
    mx::ui::OpenEventFilter openEvents(&app);
    QMetaObject::Connection earlyOpenConn = QObject::connect(&openEvents, &mx::ui::OpenEventFilter::linksReceived, &app,
                                                             [&earlyLinks](const QStringList &l) { earlyLinks << l; });

#ifdef MX_HAVE_CORE
    if (!cl.isSet(oDemo))
        mx::installLogHandler(profile);
    else
        mx::app::installAppLog(profile);
#else
    mx::app::installAppLog(profile);
#endif
    // [upd-1] finish or undo an interrupted update of the program folder, remove the old files of
    // a finished one (logged in the profile); true = files were moved under this process: the
    // program now in place starts again
    if (mx::ui::UpdateController::startupRecovery())
        return 0;

    const bool demo = cl.isSet(oDemo) || cl.isSet(oFrozen);
    mx::Core *core = createCore(profile, demo, cl.isSet(oFrozen));
    mx::ui::Colors::instance()->attach(core->settings());
    if (cl.isSet(oFrozen))
        core->settings()->setValue(mx::keys::ui::Geometry, QByteArray());   // reproducible placement
    // [ui-d] retired private UI keys -> canonical keys (the session-only warning switch is in memory)
    mx::ui::migrateLegacySettings(core->settings());
    // [ui-g] the user's translation (misc/languageFile, a Qt .qm) before any window builds its texts
    mx::ui::translation::follow(core->settings());
    // [ui-e] interface size (ui/scale, Automatic = the screen's scale setting) before any window exists
    mx::ui::applyUiScale(core->settings());
    mx::ui::installClassicLook(&app);   // style, palette and fonts at the chosen scale
    if (QScreen *scr = QGuiApplication::primaryScreen())
        qInfo("octomx: interface scale %d %% (setting %s), screen %dx%d at %.0f dpi, DPI awareness: %s",
              qRound(mx::ui::uiScale() * 100), qPrintable(core->settings()->value(mx::keys::ui::Scale).toString()),
              scr->geometry().width(), scr->geometry().height(), scr->logicalDotsPerInch(), dpiAwarenessText());
    if (qEnvironmentVariableIsSet("MX_FONT_DEBUG")) {
        const QFontMetrics fm(mx::ui::normalFont());
        qInfo("scale %.2f font %s px=%d height=%d ascent=%d descent=%d leading=%d lineSpacing=%d width(Ignored)=%d",
              mx::ui::uiScale(), qPrintable(mx::ui::normalFont().family()), mx::ui::normalFont().pixelSize(), fm.height(), fm.ascent(),
              fm.descent(), fm.leading(), fm.lineSpacing(), fm.horizontalAdvance(QStringLiteral("Ignored")));
    }
    mx::ui::SetupWizard::setPortTester([core](int port, bool udp) { return testPort(core->settings(), port, udp); });
#ifdef MX_HAVE_CORE
    if (!demo)   // [diag-1] the real test (router + public checker); the demo keeps the local check
        mx::ui::SetupWizard::setRealPortTester([core](int port, bool udp, bool map, QObject *ctx, const mx::ui::SetupWizard::PortTestReport &r) {
            realPortTest(core->settings(), port, udp, map, ctx, r);
        });
#endif

    // [ui-c] OS integration (autostart, magnet:/ed2k:/.torrent handlers) follows the settings (also the
    // wizard's). Demo / script / screenshot runs use the fake backend unless MX_OS_ROOT redirects it.
    QStringList osArgs;
    if (cl.isSet(oProfile))
        osArgs << QStringLiteral("--profile") << profile;
    mx::ui::OsIntegration *os = mx::ui::OsIntegration::createForApp(demo || automation, osArgs, &app);
    if (os) {
        os->attach(core->settings());
        qInfo("octomx: OS integration: %s", qPrintable(os->backendName()));
    }

    // The terms of use, accepted once per profile before anything else is shown and before any
    // network starts. Not asked in demo mode (no network) and in screenshot / script runs;
    // MX_ACCEPT_TERMS=1 answers for a scripted start without --script (nothing is stored).
    if (!demo && !automation && qEnvironmentVariableIsEmpty("MX_ACCEPT_TERMS") &&
        !mx::ui::LegalNotice::ensureAccepted(core->settings())) {
        qInfo("octomx: the terms of use were not accepted, exiting");
        return 0;
    }

    // [ui-c] first start: the Setup Wizard, before the networks start (ports apply at once)
    std::unique_ptr<mx::ui::SetupWizard> wizard;
    // [ui-d] MX_SCRIPT_WIZARD=<png>: also with --script, show the first-run wizard, save it to <png> and
    // take the one-click path (Finish on the first page): used by the real-core walk-through
    const QString scriptWizard = qEnvironmentVariable("MX_SCRIPT_WIZARD");
    if (!demo && (!automation || !scriptWizard.isEmpty()) && !cl.isSet(oNoWizard) &&
        !core->settings()->b(mx::keys::internal::WizardDone)) {
        wizard = std::make_unique<mx::ui::SetupWizard>(core);
        if (!scriptWizard.isEmpty()) {
            mx::ui::SetupWizard *wz = wizard.get();
            QTimer::singleShot(800, wz, [wz, scriptWizard] {
                wz->grab().save(scriptWizard);
                if (QAbstractButton *fin = wz->button(QWizard::FinishButton))
                    fin->click();
            });
        }
        if (wizard->exec() != QDialog::Accepted)
            wizard.reset();
    }

    // [ui-e] the wizard's first page offers the interface size: the main window is built after it, at
    // the chosen scale
    mx::ui::applyUiScale(core->settings());
    mx::ui::installClassicLook(&app);

    // [post-1] update check policy: never in demo mode; under MX_OS_ROOT (tests and scripted runs)
    // only when updates/url was pointed at a server on purpose. A user's own --profile checks
    // like the default profile does.
    {
        using UC = mx::ui::UpdateController;
        const bool scripted = !qEnvironmentVariableIsEmpty("MX_OS_ROOT");
        UC::setDemoMode(demo);
        UC::setPolicy(demo ? UC::Policy::Off : scripted ? UC::Policy::IfUrlSet : UC::Policy::On);
    }
    auto *mw = new mx::ui::MainWindow(core);
    mx::ui::TrayController *tray = mx::ui::TrayController::of(mw);

    if (os && tray)
        tray->setOsIntegration(os);
    mx::ui::installMacGlue(mw);   // [mac-1] menu bar, Dock, red button (macOS only)
    if (cl.isSet(oSize)) {
        const QStringList wh = cl.value(oSize).split(QLatin1Char('x'));
        if (wh.size() == 2)
            mw->resize(wh[0].toInt(), wh[1].toInt());
    }
    if (cl.isSet(oPage))
        mw->showPage(cl.value(oPage));
    if (automation)
        mw->move(0, 0);

    // [safe-1] the safe quit (menu, window close, tray, update install, signals, session end): hide the
    // UI, stop the transfers, wait until every download is written and synced (a small "Closing:
    // saving downloads..." window when it takes long), then core->shutdown() and exit
    bool shuttingDown = false;
    int exitCode = 0;
    auto shutdown = [&](int code) {
        if (shuttingDown)
            return;
        shuttingDown = true;
        exitCode = code;
        mw->saveState();
        mx::ui::saveFloatingPages(mw);   // [ui-c]
        mw->hide();
        for (QWidget *w : QApplication::topLevelWidgets())
            w->hide();
        mx::ui::waitForSafeQuit(core);
        QObject::connect(core, &mx::Core::shutdownFinished, &app, [&] { app.exit(exitCode); });
        core->shutdown();
    };
    QObject::connect(mw, &mx::ui::MainWindow::quitRequested, &app, [&] { shutdown(0); });
    // [safe-1] SIGTERM / SIGINT / SIGHUP (Linux, macOS), console Ctrl+C / close / logoff / shutdown
    // (Windows): the same safe quit
    mx::QuitSignals *quitSignals = mx::QuitSignals::install(&app);
    QObject::connect(quitSignals, &mx::QuitSignals::requested, &app, [&](const QString &why) {
        qInfo("octomx: %s: closing safely", qPrintable(why));
        shutdown(0);
        quitSignals->finished();
    });
    // [safe-1] the session ends (logout, system shutdown): the desktop asks first (commitDataRequest:
    // the transfers stop and saving starts at once; on Windows the reason shows on the shutdown
    // screen), then the application is told to quit (aboutToQuit; on Windows the process is ended
    // right after): everything is saved synchronously there if no quit ran yet
    QObject::connect(&app, &QGuiApplication::commitDataRequest, &app, [&](QSessionManager &) {
        qInfo("octomx: the session ends: saving downloads");
        core->beginShutdown();
#ifdef Q_OS_WIN
        using Create = BOOL(WINAPI *)(HWND, LPCWSTR);
        if (auto create = reinterpret_cast<Create>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "ShutdownBlockReasonCreate")))
            create(reinterpret_cast<HWND>(mw->winId()), L"OctoMX is saving downloads...");
#endif
    });
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, [&] {
        if (shuttingDown)
            return;
        shuttingDown = true;
        qInfo("octomx: quitting without the quit command (session end): saving downloads first");
        mw->saveState();
        core->shutdown();
    });
    QObject::disconnect(earlyConn);
    QObject::disconnect(earlyOpenConn);
    QObject::connect(&openEvents, &mx::ui::OpenEventFilter::linksReceived, mw, [mw](const QStringList &l) { mw->addLinks(l); });
    QObject::connect(&single, &mx::app::SingleInstance::linksReceived, mw, [mw](const QStringList &l) {
        qInfo("octomx: %d link(s) forwarded by a second instance", int(l.size()));
        mw->addLinks(l);
    });
    QObject::connect(&single, &mx::app::SingleInstance::activateRequested, mw, [mw, tray] {
        if (tray) {
            tray->restore();
        } else {
            mw->showNormal();
            mw->raise();
            mw->activateWindow();
        }
    });

    core->start();
    if (wizard)
        wizard->applyFolders();   // the library needs a started core
    wizard.reset();
    // --minimized (the autostart command line, "hidden in the tray") starts in the tray; [post-1]
    // "Always start minimized" is an ordinary minimise to the taskbar, like the minimise button
    const bool startHidden = cl.isSet(oMin);
    if (startHidden && tray)
        tray->startHidden();
    else if (startHidden || core->settings()->b(mx::keys::misc::StartMinimized))
        mw->showMinimized();
    else
        mw->show();
    // [safe-1] the last session was not closed properly (a crash, a kill, a power cut): said once
    if (core->lastStopWasUnclean()) {
        const QString text = QObject::tr("OctoMX was not closed properly the last time; the downloads are being checked.");
        qInfo("octomx: %s", qPrintable(text));
        if (!automation || qEnvironmentVariableIsSet("MX_SCRIPT_UNCLEAN_NOTICE")) {
            auto *box = new QMessageBox(QMessageBox::Information, QObject::tr("OctoMX"), text, QMessageBox::Ok, mw);
            box->setObjectName(QStringLiteral("uncleanStopNotice"));
            box->setTextFormat(Qt::PlainText);
            box->setAttribute(Qt::WA_DeleteOnClose);
            if (QAbstractButton *ok = box->button(QMessageBox::Ok)) {
                ok->setObjectName(QStringLiteral("uncleanStopOk"));
                ok->setToolTip(QObject::tr("Close this notice. The check runs on its own."));
            }
            box->open();
        }
    }
    if (!earlyLinks.isEmpty())
        qInfo("octomx: %d link(s) forwarded by a second instance during start-up", int(earlyLinks.size()));
    if (!links.isEmpty() || !earlyLinks.isEmpty())
        QTimer::singleShot(0, mw, [mw, all = links + earlyLinks] { mw->addLinks(all); });

    mx::ui::ScriptDriver *driver = nullptr;
    if (cl.isSet(oScript)) {
        driver = new mx::ui::ScriptDriver(mw, mw);
        QString err;
        if (!driver->loadFile(cl.value(oScript), &err)) {
            qCritical("%s", qPrintable(err));
            return 2;
        }
        driver->setBaseDir(QDir::currentPath());
        QObject::connect(driver, &mx::ui::ScriptDriver::finished, &app, [&](int code) {
            if (cl.isSet(oShot)) {
                mx::ui::ScriptDriver::shot(mw, cl.value(oShot));
            }
            shutdown(code);
        });
        driver->start(cl.value(oDelay).toInt() > 0 ? 300 : 0);
    } else if (cl.isSet(oShot)) {
        QTimer::singleShot(cl.value(oDelay).toInt(), mw, [&] {
            QString err;
            const bool ok = mx::ui::ScriptDriver::shot(mw, cl.value(oShot), &err);
            if (!ok)
                qCritical("%s", qPrintable(err));
            shutdown(ok ? 0 : 1);
        });
    }

    const int rc = app.exec();
    delete mw;
    delete core;
    // [upd-1] a ready update is installed now that the core has shut down (Restart Now: the new
    // version is started and waits for this process to end)
    mx::ui::UpdateController::finishOnExit();
    return rc;
}
