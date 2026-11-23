// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/os_integration.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QLibrary>
#include <QtCore/QProcess>
#include <QtCore/QSaveFile>
#include <QtCore/QSettings>
#include <QtCore/QStandardPaths>
#include <QtCore/QTextStream>

#include <memory>

#include "mx/services.h"
#include "mx/settings_keys.h"

namespace mx::ui {

using namespace mx::keys;

namespace {

const char kAppName[] = "OctoMX";
const char kDesktopId[] = "octomx.desktop";
const char kTorrentClass[] = "OctoMX.torrent";
// [ren-1] the legacy names that copies installed before 5.0 registered (identifiers kept for
// compatibility): moved to the names above at start, only where they point at this copy
const char kLegacyRunValue[] = "WinMX NG";            // the Windows Run value
const char kLegacyDesktopId[] = "winmx-ng.desktop";   // autostart and application entry (Linux)
const char kLegacyTorrentClass[] = "WinMXNG.torrent"; // HKCU\Software\Classes
const char kLegacyIconName[] = "winmx";               // hicolor/*/apps/winmx.png of the legacy menu entry
const QStringList kLegacyExeNames = {QStringLiteral("winmx.exe"), QStringLiteral("winmx")};
const QStringList kExeNames = {QStringLiteral("octomx.exe"), QStringLiteral("octomx")};

// the program file of a command line or Exec= value: the first token, quotes removed
QString firstToken(const QString &cmd)
{
    const QString c = cmd.trimmed();
    if (c.startsWith(QLatin1Char('"'))) {
        QString out;
        for (int i = 1; i < c.size(); ++i) {
            if (c[i] == QLatin1Char('\\') && i + 1 < c.size() && (c[i + 1] == QLatin1Char('"') || c[i + 1] == QLatin1Char('\\'))) {
                out += c[++i];
                continue;
            }
            if (c[i] == QLatin1Char('"'))
                break;
            out += c[i];
        }
        return out;
    }
    return c.section(QLatin1Char(' '), 0, 0);
}

// does this command start a program file of ours (old or new name) in this program's folder? The
// folder is the one of exePath, or its parent when that is the "bin" folder of a Linux package
// (whose start script sits at the top)
bool pointsHere(const QString &cmd, const QString &exePath)
{
    // (both separators on every platform: the registry layout is tested on Linux too)
    const QString prog = QString(firstToken(cmd)).replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (prog.isEmpty())
        return false;
    const QFileInfo target(prog);
    const QString name = target.fileName();
    bool ours = false;
    for (const QString &n : kLegacyExeNames + kExeNames)
        ours = ours || name.compare(n, Qt::CaseInsensitive) == 0;
    if (!ours)
        return false;
#ifdef Q_OS_WIN
    const Qt::CaseSensitivity cs = Qt::CaseInsensitive;
#else
    const Qt::CaseSensitivity cs = Qt::CaseSensitive;
#endif
    const QString dir = QDir::cleanPath(target.absolutePath());
    const QString exeDir = QDir::cleanPath(QFileInfo(QString(exePath).replace(QLatin1Char('\\'), QLatin1Char('/'))).absolutePath());
    if (dir.compare(exeDir, cs) == 0)
        return true;
    return QFileInfo(exeDir).fileName() == QLatin1String("bin") && dir.compare(QDir::cleanPath(exeDir + QStringLiteral("/..")), cs) == 0;
}

QString readText(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly | QIODevice::Text) ? QString::fromUtf8(f.readAll()) : QString();
}

QString desktopValue(const QString &text, const QString &key)
{
    for (const QString &line : text.split(QLatin1Char('\n')))
        if (line.startsWith(key + QLatin1Char('=')))
            return line.mid(key.size() + 1).trimmed();
    return QString();
}

QString quoteArg(const QString &a)
{
    if (a.isEmpty() || a.contains(QLatin1Char(' ')) || a.contains(QLatin1Char('\t')) || a.contains(QLatin1Char('"')))
        return QLatin1Char('"') + QString(a).replace(QLatin1Char('"'), QStringLiteral("\\\"")) + QLatin1Char('"');
    return a;
}

QString assocKey(OsIntegration::Assoc a)
{
    switch (a) {
    case OsIntegration::Assoc::Magnet: return QString(misc::AssociateMagnet);
    case OsIntegration::Assoc::Ed2k: return QString(misc::AssociateEd2k);
    case OsIntegration::Assoc::Torrent: return QString(misc::AssociateTorrent);
    }
    return QString();
}

const OsIntegration::Assoc kAllAssoc[] = {OsIntegration::Assoc::Magnet, OsIntegration::Assoc::Ed2k, OsIntegration::Assoc::Torrent};

// ======================================================================================= XDG

class XdgOsIntegration : public OsIntegration {
public:
    explicit XdgOsIntegration(const Options &o, QObject *parent) : OsIntegration(parent)
    {
        options = o;
        m_redirected = !o.configHome.isEmpty() || !o.dataHome.isEmpty();
        if (options.configHome.isEmpty())
            options.configHome = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
        if (options.dataHome.isEmpty())
            options.dataHome = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    }
    QString backendName() const override { return m_redirected ? QStringLiteral("xdg (redirected)") : QStringLiteral("xdg"); }

    QString autostartFile() const { return options.configHome + QStringLiteral("/autostart/") + QLatin1String(kDesktopId); }
    QString appFile() const { return options.dataHome + QStringLiteral("/applications/") + QLatin1String(kDesktopId); }
    QString mimeappsFile() const { return options.configHome + QStringLiteral("/mimeapps.list"); }
    static QString mimeOf(Assoc a)
    {
        switch (a) {
        case Assoc::Magnet: return QStringLiteral("x-scheme-handler/magnet");
        case Assoc::Ed2k: return QStringLiteral("x-scheme-handler/ed2k");
        case Assoc::Torrent: return QStringLiteral("application/x-bittorrent");
        }
        return QString();
    }

    // Exec= value (desktop entry spec: quote args with spaces, escape '%')
    QString desktopExec(bool minimized, bool withUrl) const
    {
        QStringList parts;
        auto q = [](QString s) {
            s.replace(QLatin1Char('%'), QStringLiteral("%%"));
            if (s.contains(QLatin1Char(' ')) || s.contains(QLatin1Char('"')) || s.contains(QLatin1Char('\\')))
                return QLatin1Char('"') + s.replace(QLatin1Char('\\'), QStringLiteral("\\\\")).replace(QLatin1Char('"'), QStringLiteral("\\\"")) + QLatin1Char('"');
            return s;
        };
        parts << q(options.exePath);
        for (const QString &a : options.exeArgs)
            parts << q(a);
        if (minimized)
            parts << QStringLiteral("--minimized");
        if (withUrl)
            parts << QStringLiteral("%U");
        return parts.join(QLatin1Char(' '));
    }

    bool writeFile(const QString &path, const QString &content)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QSaveFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            setError(QStringLiteral("cannot write %1").arg(path));
            return false;
        }
        f.write(content.toUtf8());
        if (!f.commit()) {
            setError(QStringLiteral("cannot write %1").arg(path));
            return false;
        }
        return true;
    }

    bool setAutostart(bool on) override
    {
        const QString f = autostartFile();
        bool ok = true;
        if (on) {
            ok = writeFile(f, QStringLiteral("[Desktop Entry]\nType=Application\nName=%1\nComment=Start %1 hidden in the system tray\n"
                                             "Exec=%2\nTerminal=false\nX-GNOME-Autostart-enabled=true\n")
                                  .arg(QLatin1String(kAppName), desktopExec(true, false)));
        } else if (QFile::exists(f)) {
            ok = QFile::remove(f);
        }
        emit changed();
        return ok;
    }
    bool isAutostart() const override
    {
        QFile f(autostartFile());
        if (!f.open(QIODevice::ReadOnly))
            return false;
        const QString s = QString::fromUtf8(f.readAll());
        return !s.contains(QLatin1String("Hidden=true")) && !s.contains(QLatin1String("X-GNOME-Autostart-enabled=false"));
    }

    QStringList registeredTypes() const
    {
        QFile f(appFile());
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            return {};
        for (const QString &line : QString::fromUtf8(f.readAll()).split(QLatin1Char('\n')))
            if (line.startsWith(QLatin1String("MimeType=")))
                return line.mid(9).split(QLatin1Char(';'), Qt::SkipEmptyParts);
        return {};
    }

    // the [Default Applications] value for a type in mimeapps.list
    QString defaultFor(const QString &type) const
    {
        QFile f(mimeappsFile());
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            return QString();
        bool inDefaults = false;
        for (const QString &raw : QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'))) {
            const QString line = raw.trimmed();
            if (line.startsWith(QLatin1Char('[')))
                inDefaults = line == QLatin1String("[Default Applications]");
            else if (inDefaults && line.startsWith(type + QLatin1Char('=')))
                return line.mid(type.size() + 1).section(QLatin1Char(';'), 0, 0);
        }
        return QString();
    }

    // set (or remove, value empty) a key in a section of mimeapps.list, keeping everything else
    bool editMimeapps(const QString &section, const QString &type, const QString &value)
    {
        QStringList lines;
        {
            QFile f(mimeappsFile());
            if (f.open(QIODevice::ReadOnly | QIODevice::Text))
                lines = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
        }
        while (!lines.isEmpty() && lines.last().isEmpty())
            lines.removeLast();
        int sec = -1;
        for (int i = 0; i < lines.size(); ++i)
            if (lines[i].trimmed() == section)
                sec = i;
        if (sec < 0) {
            if (value.isEmpty())
                return true;
            if (!lines.isEmpty())
                lines << QString();
            lines << section;
            sec = int(lines.size()) - 1;
        }
        int end = sec + 1;
        while (end < lines.size() && !lines[end].trimmed().startsWith(QLatin1Char('[')))
            ++end;
        int at = -1;
        for (int i = sec + 1; i < end; ++i)
            if (lines[i].trimmed().startsWith(type + QLatin1Char('=')))
                at = i;
        if (value.isEmpty()) {
            if (at >= 0)
                lines.removeAt(at);
        } else if (at >= 0) {
            lines[at] = type + QLatin1Char('=') + value;
        } else {
            int ins = end;
            while (ins > sec + 1 && lines[ins - 1].trimmed().isEmpty())
                --ins;
            lines.insert(ins, type + QLatin1Char('=') + value);
        }
        return writeFile(mimeappsFile(), lines.join(QLatin1Char('\n')) + QLatin1Char('\n'));
    }

    void runTool(const QString &program, const QStringList &args)
    {
        // [ui-e] a redirected backend (MX_OS_ROOT, tests, scripted runs) never starts xdg-mime /
        // update-desktop-database: desktop helpers may write to the real home whatever XDG_* says
        if (!options.runXdgTools || m_redirected || !qEnvironmentVariableIsEmpty("MX_OS_ROOT"))
            return;
        const QString exe = QStandardPaths::findExecutable(program);
        if (exe.isEmpty())
            return;
        QProcess p;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("XDG_CONFIG_HOME"), options.configHome);
        env.insert(QStringLiteral("XDG_DATA_HOME"), options.dataHome);
        if (m_redirected) {
            // a sandbox: no desktop-specific tools (they could write to the real home)
            env.insert(QStringLiteral("HOME"), QFileInfo(options.configHome).absolutePath());
            for (const char *v : {"XDG_CURRENT_DESKTOP", "DESKTOP_SESSION", "KDE_FULL_SESSION", "GNOME_DESKTOP_SESSION_ID", "KDE_SESSION_VERSION"})
                env.remove(QString::fromLatin1(v));
        }
        p.setProcessEnvironment(env);
        p.setProcessChannelMode(QProcess::MergedChannels);
        p.start(exe, args);
        if (!p.waitForFinished(5000))
            p.kill();
    }

    bool writeHandlerFile(const QStringList &types)
    {
        if (types.isEmpty()) {
            if (QFile::exists(appFile()))
                QFile::remove(appFile());
            return true;
        }
        return writeFile(appFile(), QStringLiteral("[Desktop Entry]\nType=Application\nName=%1\nGenericName=File Sharing\n"
                                                   "Comment=Download magnet:, ed2k:// links and .torrent files\nExec=%2\n"
                                                   "Terminal=false\nCategories=Network;FileTransfer;P2P;\nMimeType=%3;\n"
                                                   "NoDisplay=false\n")
                                    .arg(QLatin1String(kAppName), desktopExec(false, true), types.join(QLatin1Char(';'))));
    }

    // [ren-1] mimeapps.list: every mention of oldId (defaults, added / removed associations) becomes newId
    bool renameInMimeapps(const QString &oldId, const QString &newId)
    {
        const QString text = readText(mimeappsFile());
        if (text.isEmpty() || !text.contains(oldId))
            return true;
        QStringList lines = text.split(QLatin1Char('\n'));
        for (QString &l : lines) {
            const int eq = l.indexOf(QLatin1Char('='));
            if (eq < 0 || l.trimmed().startsWith(QLatin1Char('[')))
                continue;
            QStringList ids = l.mid(eq + 1).split(QLatin1Char(';'));
            QStringList out;
            for (const QString &id : ids) {
                const QString v = id.trimmed() == oldId ? newId : id;
                if (!v.isEmpty() && out.contains(v))
                    continue;
                out << v;
            }
            l = l.left(eq + 1) + out.join(QLatin1Char(';'));
        }
        return writeFile(mimeappsFile(), lines.join(QLatin1Char('\n')));
    }

    QStringList migrateLegacy() override
    {
        QStringList moved;
        const QString oldId = QLatin1String(kLegacyDesktopId), newId = QLatin1String(kDesktopId);
        // autostart entry
        const QString oldAuto = options.configHome + QStringLiteral("/autostart/") + oldId;
        const QString autoText = readText(oldAuto);
        if (!autoText.isEmpty() && pointsHere(desktopValue(autoText, QStringLiteral("Exec")), options.exePath)) {
            const bool on = !autoText.contains(QLatin1String("Hidden=true")) && !autoText.contains(QLatin1String("X-GNOME-Autostart-enabled=false"));
            QFile::remove(oldAuto);
            if (on)
                setAutostart(true);
            moved << QStringLiteral("legacy autostart entry -> %1").arg(newId);
        }
        // application entry (link handlers, and the menu entry of install-desktop-entry.sh)
        const QString oldApp = options.dataHome + QStringLiteral("/applications/") + oldId;
        const QString appText = readText(oldApp);
        if (!appText.isEmpty() && pointsHere(desktopValue(appText, QStringLiteral("Exec")), options.exePath)) {
            const QStringList types = desktopValue(appText, QStringLiteral("MimeType")).split(QLatin1Char(';'), Qt::SkipEmptyParts);
            const bool menuEntry = !desktopValue(appText, QStringLiteral("Icon")).isEmpty();
            QFile::remove(oldApp);
            // the package's own menu entry with the new start script and icon name, when it has one
            const QString exeDir = QFileInfo(options.exePath).absolutePath();
            const QString pkg = QFileInfo(exeDir).fileName() == QLatin1String("bin") ? QDir::cleanPath(exeDir + QStringLiteral("/..")) : QString();
            const QString tmpl = pkg.isEmpty() ? QString() : readText(pkg + QStringLiteral("/share/applications/") + newId);
            if (menuEntry && !tmpl.isEmpty()) {
                writeFile(appFile(), QString(tmpl).replace(QStringLiteral("@PREFIX@"), pkg));
                for (const char *size : {"48x48", "256x256"}) {
                    const QString icons = options.dataHome + QStringLiteral("/icons/hicolor/%1/apps/").arg(QLatin1String(size));
                    const QString src = pkg + QStringLiteral("/share/icons/hicolor/%1/apps/octomx.png").arg(QLatin1String(size));
                    if (QFile::exists(src)) {
                        QDir().mkpath(icons);
                        QFile::remove(icons + QStringLiteral("octomx.png"));
                        QFile::copy(src, icons + QStringLiteral("octomx.png"));
                    }
                    QFile::remove(icons + QLatin1String(kLegacyIconName) + QStringLiteral(".png"));
                }
            } else {
                writeHandlerFile(types);
            }
            renameInMimeapps(oldId, newId);
            runTool(QStringLiteral("update-desktop-database"), {options.dataHome + QStringLiteral("/applications")});
            emit changed();
            moved << QStringLiteral("legacy application entry -> %1%2").arg(newId, menuEntry ? QStringLiteral(" (menu entry)") : QString());
        }
        return moved;
    }

    bool setHandler(Assoc a, bool on) override
    {
        QStringList types = registeredTypes();
        const QString t = mimeOf(a);
        if (on && !types.contains(t))
            types << t;
        if (!on)
            types.removeAll(t);
        bool ok = writeHandlerFile(types);
        const QString ours = QLatin1String(kDesktopId);
        if (on) {
            ok &= editMimeapps(QStringLiteral("[Default Applications]"), t, ours);
            ok &= editMimeapps(QStringLiteral("[Added Associations]"), t, ours + QLatin1Char(';'));
        } else {
            if (defaultFor(t) == ours)
                ok &= editMimeapps(QStringLiteral("[Default Applications]"), t, QString());
            ok &= editMimeapps(QStringLiteral("[Added Associations]"), t, QString());
        }
        runTool(QStringLiteral("update-desktop-database"), {options.dataHome + QStringLiteral("/applications")});
        if (on)
            runTool(QStringLiteral("xdg-mime"), {QStringLiteral("default"), ours, t});
        emit changed();
        return ok;
    }
    bool isHandler(Assoc a) const override
    {
        const QString t = mimeOf(a);
        return registeredTypes().contains(t) && defaultFor(t) == QLatin1String(kDesktopId);
    }
    void shutdownComputer() override { QProcess::startDetached(QStringLiteral("systemctl"), {QStringLiteral("poweroff")}); }

private:
    bool m_redirected = false;
};

// ================================================================================== registry

class RegistryOsIntegration : public OsIntegration {
public:
    explicit RegistryOsIntegration(const Options &o, QObject *parent) : OsIntegration(parent) { options = o; }
    QString backendName() const override
    {
        return options.registryIni.isEmpty() ? QStringLiteral("registry (HKCU)") : QStringLiteral("registry (%1)").arg(options.registryIni);
    }
    std::unique_ptr<QSettings> root() const
    {
        if (options.registryIni.isEmpty())
            return std::make_unique<QSettings>(QStringLiteral("HKEY_CURRENT_USER\\Software"), QSettings::NativeFormat);
        return std::make_unique<QSettings>(options.registryIni, QSettings::IniFormat);
    }
    static QString runValue() { return QStringLiteral("Microsoft/Windows/CurrentVersion/Run/") + QLatin1String(kAppName); }
    QString exeNative() const { return QDir::toNativeSeparators(options.exePath); }

    bool finish(QSettings &s)
    {
        s.sync();
        if (s.status() != QSettings::NoError) {
            setError(QStringLiteral("registry write failed"));
            return false;
        }
        notifyShell();
        emit changed();
        return true;
    }
    void notifyShell() const
    {
#ifdef Q_OS_WIN
        if (!options.registryIni.isEmpty())
            return;
        // SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr) so Explorer re-reads the associations
        using Fn = void(__stdcall *)(long, unsigned int, const void *, const void *);
        if (auto fn = reinterpret_cast<Fn>(QLibrary::resolve(QStringLiteral("shell32"), "SHChangeNotify")))
            fn(0x08000000L, 0x0000u, nullptr, nullptr);
#endif
    }

    bool setAutostart(bool on) override
    {
        auto s = root();
        if (on)
            s->setValue(runValue(), commandLine(true, QString()));
        else
            s->remove(runValue());
        return finish(*s);
    }
    bool isAutostart() const override
    {
        auto s = root();
        return s->value(runValue()).toString().contains(exeNative(), Qt::CaseInsensitive);
    }

    static QString classKey(Assoc a)
    {
        switch (a) {
        case Assoc::Magnet: return QStringLiteral("Classes/magnet");
        case Assoc::Ed2k: return QStringLiteral("Classes/ed2k");
        case Assoc::Torrent: return QStringLiteral("Classes/") + QLatin1String(kTorrentClass);
        }
        return QString();
    }
    bool setHandler(Assoc a, bool on) override
    {
        auto s = root();
        const QString k = classKey(a);
        if (on) {
            const QString icon = exeNative() + QStringLiteral(",0");
            if (a == Assoc::Torrent) {
                s->setValue(QStringLiteral("Classes/.torrent/."), QLatin1String(kTorrentClass));
                s->setValue(QStringLiteral("Classes/.torrent/Content Type"), QStringLiteral("application/x-bittorrent"));
                s->setValue(k + QStringLiteral("/."), QStringLiteral("BitTorrent File"));
            } else {
                s->setValue(k + QStringLiteral("/."), a == Assoc::Magnet ? QStringLiteral("URL:Magnet link") : QStringLiteral("URL:ed2k link"));
                s->setValue(k + QStringLiteral("/URL Protocol"), QString());
            }
            s->setValue(k + QStringLiteral("/DefaultIcon/."), icon);
            s->setValue(k + QStringLiteral("/shell/."), QStringLiteral("open"));
            s->setValue(k + QStringLiteral("/shell/open/command/."), commandLine(false, QStringLiteral("%1")));
        } else if (isHandler(a)) {
            s->remove(k);
            if (a == Assoc::Torrent && s->value(QStringLiteral("Classes/.torrent/.")).toString() == QLatin1String(kTorrentClass))
                s->remove(QStringLiteral("Classes/.torrent"));
        }
        return finish(*s);
    }
    bool isHandler(Assoc a) const override
    {
        auto s = root();
        const bool cmd = s->value(classKey(a) + QStringLiteral("/shell/open/command/.")).toString().contains(exeNative(), Qt::CaseInsensitive);
        if (a == Assoc::Torrent)
            return cmd && s->value(QStringLiteral("Classes/.torrent/.")).toString() == QLatin1String(kTorrentClass);
        return cmd;
    }
    void shutdownComputer() override
    {
        QProcess::startDetached(QStringLiteral("shutdown"), {QStringLiteral("/s"), QStringLiteral("/t"), QStringLiteral("30")});
    }

    QStringList migrateLegacy() override
    {
        QStringList moved;
        bool autostart = false, handler[3] = {false, false, false};
        {
            auto s = root();
            const QString oldRun = QStringLiteral("Microsoft/Windows/CurrentVersion/Run/") + QLatin1String(kLegacyRunValue);
            if (s->contains(oldRun) && pointsHere(s->value(oldRun).toString(), options.exePath)) {
                s->remove(oldRun);
                autostart = true;
                moved << QStringLiteral("legacy Run value -> \"%1\"").arg(QLatin1String(kAppName));
            }
            // magnet: / ed2k: keep their key; only the command still names the legacy program file
            for (Assoc a : {Assoc::Magnet, Assoc::Ed2k}) {
                const QString cmd = s->value(classKey(a) + QStringLiteral("/shell/open/command/.")).toString();
                if (pointsHere(cmd, options.exePath) && !cmd.contains(exeNative(), Qt::CaseInsensitive)) {
                    handler[int(a)] = true;
                    moved << QStringLiteral("%1: handler command -> %2").arg(assocName(a), QFileInfo(options.exePath).fileName());
                }
            }
            const QString oldClass = QStringLiteral("Classes/") + QLatin1String(kLegacyTorrentClass);
            if (pointsHere(s->value(oldClass + QStringLiteral("/shell/open/command/.")).toString(), options.exePath)) {
                handler[int(Assoc::Torrent)] = s->value(QStringLiteral("Classes/.torrent/.")).toString() == QLatin1String(kLegacyTorrentClass);
                s->remove(oldClass);
                if (handler[int(Assoc::Torrent)])
                    s->remove(QStringLiteral("Classes/.torrent"));
                moved << QStringLiteral(".torrent: legacy class -> %1").arg(QLatin1String(kTorrentClass));
            }
            if (!moved.isEmpty())
                finish(*s);
        }
        if (autostart)
            setAutostart(true);
        for (Assoc a : kAllAssoc)
            if (handler[int(a)])
                setHandler(a, true);
        return moved;
    }
};

OsIntegration::Options withDefaults(OsIntegration::Options o)
{
    if (o.exePath.isEmpty())
        o.exePath = QCoreApplication::applicationFilePath();
    return o;
}

} // namespace

// ================================================================================= factories

OsIntegration *OsIntegration::createXdg(const Options &o, QObject *parent)
{
    return new XdgOsIntegration(withDefaults(o), parent);
}

OsIntegration *OsIntegration::createRegistry(const Options &o, QObject *parent)
{
    return new RegistryOsIntegration(withDefaults(o), parent);
}

OsIntegration *OsIntegration::createFake(QObject *parent)
{
    auto *f = new FakeOsIntegration(parent);
    f->options = withDefaults(Options());
    return f;
}

OsIntegration *OsIntegration::createNative(const Options &o, QObject *parent)
{
#ifdef Q_OS_WIN
    return createRegistry(o, parent);
#else
    return createXdg(o, parent);
#endif
}

OsIntegration *OsIntegration::createForApp(bool sandboxed, const QStringList &exeArgs, QObject *parent)
{
    const QString mode = qEnvironmentVariable("MX_OS_INTEGRATION").toLower();
    const QString root = qEnvironmentVariable("MX_OS_ROOT");
    if (mode == QLatin1String("off"))
        return nullptr;
    Options o;
    o.exeArgs = exeArgs;
    if (!root.isEmpty()) {
        o.configHome = QDir(root).filePath(QStringLiteral("config"));
        o.dataHome = QDir(root).filePath(QStringLiteral("data"));
#ifdef Q_OS_WIN
        if (mode != QLatin1String("native"))
            o.registryIni = QDir(root).filePath(QStringLiteral("registry.ini"));
#endif
        return createNative(o, parent);
    }
    if (mode == QLatin1String("fake") || (sandboxed && mode != QLatin1String("native")))
        return createFake(parent);
    return createNative(o, parent);
}

// ============================================================================== shared logic

QString OsIntegration::assocName(Assoc a)
{
    switch (a) {
    case Assoc::Magnet: return QStringLiteral("magnet");
    case Assoc::Ed2k: return QStringLiteral("ed2k");
    case Assoc::Torrent: return QStringLiteral("torrent");
    }
    return QString();
}

QString OsIntegration::commandLine(bool minimized, const QString &argPlaceholder) const
{
    QStringList parts;
    parts << QLatin1Char('"') + QDir::toNativeSeparators(options.exePath) + QLatin1Char('"');
    for (const QString &a : options.exeArgs)
        parts << quoteArg(a);
    if (minimized)
        parts << QStringLiteral("--minimized");
    if (!argPlaceholder.isEmpty())
        parts << QLatin1Char('"') + argPlaceholder + QLatin1Char('"');
    return parts.join(QLatin1Char(' '));
}

void OsIntegration::attach(mx::Settings *s)
{
    m_settings = s;
    connect(s, &Settings::changed, this, [this](const QString &key, const QVariant &) { apply(key); });
    // [ren-1] legacy registrations (pre-5.0) for this copy of the program move to the current names first
    const QStringList moved = migrateLegacy();
    for (const QString &m : moved)
        qInfo("octomx: OS integration: %s (moved from a copy installed before 5.0)", qPrintable(m));
    // start-up check: the autostart entry follows the setting both ways; associations are only
    // (re)claimed: never removed at start (another program may own them on purpose)
    if (s->b(misc::StartWithOs) != isAutostart())
        setAutostart(s->b(misc::StartWithOs));
    if (s->b(misc::CheckAssociations)) {
        for (Assoc a : kAllAssoc)
            if (s->b(assocKey(a)) && !isHandler(a))
                setHandler(a, true);
    }
}

void OsIntegration::apply(const QString &key)
{
    if (!m_settings)
        return;
    if (key == QLatin1String(misc::StartWithOs)) {
        setAutostart(m_settings->b(misc::StartWithOs));
        return;
    }
    for (Assoc a : kAllAssoc)
        if (key == assocKey(a))
            setHandler(a, m_settings->b(key));
}

} // namespace mx::ui
