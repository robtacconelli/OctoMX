// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: operating-system integration [ui-c]: start with the system, and open magnet: /
// ed2k:// links and .torrent files with OctoMX.
//
//   Windows   HKCU\Software\Microsoft\Windows\CurrentVersion\Run  "OctoMX"
//             HKCU\Software\Classes\magnet | ed2k  (URL protocol, shell\open\command)
//             HKCU\Software\Classes\.torrent -> OctoMX.torrent
//   macOS     ~/Library/LaunchAgents/io.github.robtacconelli.octomx.plist; Launch Services for the
//             links and .torrent files declared in Info.plist ([mac-1], os_integration_mac.cpp)
//   Linux     $XDG_CONFIG_HOME/autostart/octomx.desktop
//             $XDG_DATA_HOME/applications/octomx.desktop (MimeType=...) + default handler in
//             $XDG_CONFIG_HOME/mimeapps.list (through xdg-mime when installed)
//
// Backends: the native one for the platform, a registry backend over any QSettings (an INI file in
// tests, so the Windows logic is tested on Linux too) and a fake one (tests, demo, automation).
// Tests never touch the real HOME or registry: they use the fake backend or roots under build/.
//
// attach(settings) makes the misc/startWithOs and misc/associate* settings drive the backend;
// with misc/checkAssociations the associations are (re)registered at start.
#pragma once

#include <QtCore/QObject>
#include <QtCore/QStringList>

namespace mx {
class Settings;
}

namespace mx::ui {

class OsIntegration : public QObject {
    Q_OBJECT
public:
    enum class Assoc { Magnet, Ed2k, Torrent };

    struct Options {
        QString exePath;            // default: QCoreApplication::applicationFilePath()
        QStringList exeArgs;        // extra arguments, e.g. {"--profile", dir}
        // XDG backend roots (default: $XDG_CONFIG_HOME / $XDG_DATA_HOME or ~/.config, ~/.local/share)
        QString configHome;
        QString dataHome;
        bool runXdgTools = true;    // xdg-mime / update-desktop-database (env redirected to the roots)
        // registry backend: an INI file instead of HKCU\Software (tests / sandboxes)
        QString registryIni;
        // [mac-1] macOS backend: the LaunchAgents folder (default ~/Library/LaunchAgents) and an INI
        // file in place of Launch Services (tests / sandboxes; empty = the real Launch Services)
        QString launchAgentsDir;
        QString launchServicesIni;
    };

    // The native backend of this platform (Windows registry, macOS or XDG).
    static OsIntegration *createNative(const Options &o, QObject *parent = nullptr);
    // Registry layout over HKCU\Software (Windows) or the INI file in o.registryIni (any platform).
    static OsIntegration *createRegistry(const Options &o, QObject *parent = nullptr);
    static OsIntegration *createXdg(const Options &o, QObject *parent = nullptr);
    // [mac-1] LaunchAgent + Launch Services (any platform with o.launchServicesIni, for tests)
    static OsIntegration *createMac(const Options &o, QObject *parent = nullptr);
    static OsIntegration *createFake(QObject *parent = nullptr);
    // What main() uses: env MX_OS_INTEGRATION = native | fake | off, MX_OS_ROOT = redirect root;
    // `sandboxed` (demo / script / screenshot runs) selects the fake backend unless redirected.
    static OsIntegration *createForApp(bool sandboxed, const QStringList &exeArgs, QObject *parent = nullptr);

    explicit OsIntegration(QObject *parent = nullptr) : QObject(parent) {}
    virtual QString backendName() const = 0;
    virtual bool setAutostart(bool on) = 0;
    virtual bool isAutostart() const = 0;
    virtual bool setHandler(Assoc a, bool on) = 0;
    virtual bool isHandler(Assoc a) const = 0;
    // [ren-1] legacy registrations that a copy installed before 5.0 made for this copy of the
    // program (the legacy Run value, .torrent class, program file in the handler commands, autostart
    // and application entries; names in os_integration.cpp) are moved to the current names;
    // registrations of other copies stay. Called by attach() first. Returns one line per moved thing.
    virtual QStringList migrateLegacy() { return {}; }
    // "Shutdown When Xfers Stop": power the computer off (fake: records the call)
    virtual void shutdownComputer() = 0;
    QString lastError() const { return m_error; }

    // follow the settings (misc/startWithOs, misc/associateMagnet/Ed2k/Torrent, misc/checkAssociations)
    void attach(mx::Settings *s);
    // the command line the OS uses to start us ("<exe>" <args> [--minimized] ["%1"])
    QString commandLine(bool minimized, const QString &argPlaceholder) const;
    static QString assocName(Assoc a);

    Options options;

signals:
    void changed();

protected:
    void setError(const QString &e) { m_error = e; }
    QString m_error;

private:
    void apply(const QString &key);
    mx::Settings *m_settings = nullptr;
};

// The in-memory backend (tests). Records every call.
class FakeOsIntegration : public OsIntegration {
    Q_OBJECT
public:
    using OsIntegration::OsIntegration;
    QString backendName() const override { return QStringLiteral("fake"); }
    bool setAutostart(bool on) override { log << QStringLiteral("autostart=%1").arg(on); m_autostart = on; emit changed(); return true; }
    bool isAutostart() const override { return m_autostart; }
    bool setHandler(Assoc a, bool on) override
    {
        log << QStringLiteral("%1=%2").arg(assocName(a)).arg(on);
        m_handlers[int(a)] = on;
        emit changed();
        return true;
    }
    bool isHandler(Assoc a) const override { return m_handlers[int(a)]; }
    void shutdownComputer() override { log << QStringLiteral("shutdown"); ++shutdowns; }
    QStringList log;
    int shutdowns = 0;

private:
    bool m_autostart = false;
    bool m_handlers[3] = {false, false, false};
};

} // namespace mx::ui
