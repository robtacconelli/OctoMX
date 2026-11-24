// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI [mac-1]: the macOS backend of OsIntegration (see os_integration.h).
//
//   start at login   ~/Library/LaunchAgents/io.github.robtacconelli.octomx.plist (RunAtLoad, the
//                    program with --minimized), written and removed by the program; launchd reads
//                    it at the next login (macOS 13+ shows a "Background Items Added" notice)
//   magnet: / ed2k:  declared in OctoMX.app/Contents/Info.plist (CFBundleURLTypes); "make OctoMX the
//   .torrent         default" = Launch Services (LSSetDefaultHandlerForURLScheme,
//                    LSSetDefaultRoleHandlerForContentType for org.bittorrent.torrent); macOS has no
//                    way to give a handler back, so switching it off leaves the choice as it is
//                    (another program, or Finder's Get Info > Open with, takes it over)
//   shutdown         System Events through osascript (macOS asks once for the permission)
//
// Test seam: o.launchAgentsDir (MX_OS_ROOT/LaunchAgents) and o.launchServicesIni (an INI file in
// place of Launch Services), so the logic runs and is tested on every platform; the Launch Services
// calls exist only in the macOS build.
#include "ui/os_integration.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtCore/QSaveFile>
#include <QtCore/QSettings>
#include <QtCore/QStandardPaths>

#include "mx/app_paths.h"

#ifdef Q_OS_MACOS
#include <CoreServices/CoreServices.h>
#endif

namespace mx::ui {

namespace {

const char kBundleId[] = "io.github.robtacconelli.octomx";

QString xmlEscape(const QString &s)
{
    QString o = s;
    o.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
    o.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
    o.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
    o.replace(QLatin1Char('"'), QStringLiteral("&quot;"));
    return o;
}

QString scheme(OsIntegration::Assoc a)
{
    return a == OsIntegration::Assoc::Magnet ? QStringLiteral("magnet") : QStringLiteral("ed2k");
}

#ifdef Q_OS_MACOS
const char kTorrentUti[] = "org.bittorrent.torrent";   // imported in Info.plist (UTImportedTypeDeclarations)

// a CFString that releases itself
struct CfStr {
    explicit CfStr(const QString &s) : ref(s.toCFString()) {}
    ~CfStr() { if (ref) CFRelease(ref); }
    CfStr(const CfStr &) = delete;
    CfStr &operator=(const CfStr &) = delete;
    CFStringRef ref;
};

QString takeCf(CFStringRef s)
{
    if (!s)
        return QString();
    const QString out = QString::fromCFString(s);
    CFRelease(s);
    return out;
}

// The Launch Services calls OctoMX needs have no replacement in plain C (their successors are
// Objective-C NSWorkspace methods of macOS 12); they still work and are what other programs use.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
OSStatus lsSetUrlHandler(const QString &sch, const QString &bundleId)
{
    CfStr s(sch), b(bundleId);
    return LSSetDefaultHandlerForURLScheme(s.ref, b.ref);
}
QString lsUrlHandler(const QString &sch)
{
    CfStr s(sch);
    return takeCf(LSCopyDefaultHandlerForURLScheme(s.ref));
}
OSStatus lsSetTypeHandler(const QString &uti, const QString &bundleId)
{
    CfStr u(uti), b(bundleId);
    return LSSetDefaultRoleHandlerForContentType(u.ref, kLSRolesAll, b.ref);
}
QString lsTypeHandler(const QString &uti)
{
    CfStr u(uti);
    return takeCf(LSCopyDefaultRoleHandlerForContentType(u.ref, kLSRolesAll));
}
#pragma clang diagnostic pop

// make sure Launch Services knows this copy of OctoMX.app (it learns a bundle when Finder sees it,
// but a copy started from a fresh folder may not be registered yet)
void lsRegister(const QString &bundle)
{
    if (bundle.isEmpty())
        return;
    const QByteArray p = QFile::encodeName(bundle);
    if (CFURLRef url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(p.constData()),
                                                                CFIndex(p.size()), true)) {
        LSRegisterURL(url, true);
        CFRelease(url);
    }
}
#endif

class MacOsIntegration : public OsIntegration {
public:
    explicit MacOsIntegration(const Options &o, QObject *parent) : OsIntegration(parent) { options = o; }

    QString backendName() const override { return options.launchServicesIni.isEmpty() ? QStringLiteral("macos") : QStringLiteral("macos-test"); }

    QString agentPath() const { return QDir(options.launchAgentsDir).filePath(QLatin1String(kBundleId) + QStringLiteral(".plist")); }

    QString agentText() const
    {
        QString args;
        QStringList all{options.exePath};
        all << options.exeArgs << QStringLiteral("--minimized");
        for (const QString &a : all)
            args += QStringLiteral("\t\t<string>%1</string>\n").arg(xmlEscape(a));
        return QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                              "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                              "<plist version=\"1.0\">\n<dict>\n"
                              "\t<key>Label</key>\n\t<string>%1</string>\n"
                              "\t<key>ProgramArguments</key>\n\t<array>\n%2\t</array>\n"
                              "\t<key>RunAtLoad</key>\n\t<true/>\n"
                              "\t<key>ProcessType</key>\n\t<string>Interactive</string>\n"
                              "\t<key>LimitLoadToSessionType</key>\n\t<string>Aqua</string>\n"
                              "</dict>\n</plist>\n")
            .arg(QLatin1String(kBundleId), args);
    }

    bool setAutostart(bool on) override
    {
        const QString p = agentPath();
        if (!on) {
            if (QFileInfo::exists(p) && !QFile::remove(p)) {
                setError(QStringLiteral("cannot remove %1").arg(p));
                return false;
            }
            emit changed();
            return true;
        }
        QDir().mkpath(options.launchAgentsDir);
        QSaveFile f(p);
        if (!f.open(QIODevice::WriteOnly) || f.write(agentText().toUtf8()) < 0 || !f.commit()) {
            setError(QStringLiteral("cannot write %1").arg(p));
            return false;
        }
        emit changed();
        return true;
    }

    bool isAutostart() const override
    {
        QFile f(agentPath());
        if (!f.open(QIODevice::ReadOnly))
            return false;
        const QString text = QString::fromUtf8(f.readAll());
        return text.contains(QStringLiteral("<string>%1</string>").arg(xmlEscape(options.exePath)));
    }

    QString bundleId() const { return QLatin1String(kBundleId); }

    bool setHandler(Assoc a, bool on) override
    {
        if (!on) {
            // macOS has no "give it back": the choice stays until another program claims it
            emit changed();
            return true;
        }
        if (!options.launchServicesIni.isEmpty()) {
            QSettings s(options.launchServicesIni, QSettings::IniFormat);
            s.setValue(QStringLiteral("handlers/") + key(a), bundleId());
            s.sync();
            emit changed();
            return s.status() == QSettings::NoError;
        }
#ifdef Q_OS_MACOS
        const QString bundle = mx::paths::bundleOf(QFileInfo(options.exePath).absolutePath());
        if (bundle.isEmpty()) {
            setError(QStringLiteral("only OctoMX.app can be the handler (this program is not inside an app bundle)"));
            return false;
        }
        lsRegister(bundle);
        const OSStatus st = a == Assoc::Torrent ? lsSetTypeHandler(QLatin1String(kTorrentUti), bundleId()) : lsSetUrlHandler(scheme(a), bundleId());
        if (st != noErr) {
            setError(QStringLiteral("Launch Services refused (error %1)").arg(int(st)));
            return false;
        }
        emit changed();
        return true;
#else
        setError(QStringLiteral("Launch Services exist only on macOS"));
        return false;
#endif
    }

    bool isHandler(Assoc a) const override
    {
        QString h;
        if (!options.launchServicesIni.isEmpty()) {
            QSettings s(options.launchServicesIni, QSettings::IniFormat);
            h = s.value(QStringLiteral("handlers/") + key(a)).toString();
        } else {
#ifdef Q_OS_MACOS
            h = a == Assoc::Torrent ? lsTypeHandler(QLatin1String(kTorrentUti)) : lsUrlHandler(scheme(a));
#endif
        }
        return h.compare(bundleId(), Qt::CaseInsensitive) == 0;
    }

    void shutdownComputer() override
    {
        QProcess::startDetached(QStringLiteral("/usr/bin/osascript"),
                                {QStringLiteral("-e"), QStringLiteral("tell application \"System Events\" to shut down")});
    }

private:
    static QString key(Assoc a) { return a == Assoc::Torrent ? QStringLiteral("torrent") : scheme(a); }
};

} // namespace

OsIntegration *OsIntegration::createMac(const Options &o, QObject *parent)
{
    Options m = o;
    if (m.exePath.isEmpty())
        m.exePath = QCoreApplication::applicationFilePath();
    if (m.launchAgentsDir.isEmpty())
        m.launchAgentsDir = QDir::homePath() + QStringLiteral("/Library/LaunchAgents");
    return new MacOsIntegration(m, parent);
}

} // namespace mx::ui
