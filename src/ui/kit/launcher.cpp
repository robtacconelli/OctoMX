// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/kit/launcher.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QRegularExpression>
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtCore/QStandardPaths>
#include <QtCore/QStringList>
#include <QtCore/QTextStream>
#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>
#include <QtWidgets/QMessageBox>

#include "mx/download_order.h"
#include "mx/services.h"
#include "mx/settings_keys.h"
#include "ui/kit/session.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#endif

namespace mx::ui::launcher {

namespace {
Hook &hook()
{
    static Hook h;
    return h;
}
AskHook &askHook()
{
    static AskHook h;
    return h;
}

// MX_LAUNCH_LOG: record instead of launching
bool logged(const QString &action, const QString &path)
{
    const QString log = qEnvironmentVariable("MX_LAUNCH_LOG");
    if (log.isEmpty())
        return false;
    QFile f(log);
    if (f.open(QIODevice::Append | QIODevice::Text))
        QTextStream(&f) << action << ' ' << QDir::toNativeSeparators(path) << '\n';
    return true;
}

CommandHook &commandHook()
{
    static CommandHook h;
    return h;
}

// [qa-2] the last step of every launch: start the process / open the URL, or hand it to the test hook
// [dl-1] detached = a player: started and left running (never waited for)
bool startCommand(const QString &program, const QStringList &args, const QString &nativeArgs = QString(), bool detached = false)
{
    if (commandHook())
        return commandHook()(program, args, nativeArgs);
    if (program == QLatin1String("url"))
        return QDesktopServices::openUrl(QUrl::fromEncoded(args.value(0).toUtf8()));
    if (detached) {
        QProcess p;
        p.setProgram(program);
#ifdef Q_OS_WIN
        p.setNativeArguments(nativeArgs);
#else
        p.setArguments(args);
#endif
        p.setStandardInputFile(QProcess::nullDevice());
        p.setStandardOutputFile(QProcess::nullDevice());
        p.setStandardErrorFile(QProcess::nullDevice());
        return p.startDetached();
    }
#ifdef Q_OS_WIN
    if (!nativeArgs.isEmpty()) {
        // QProcess would quote the whole "/select,<path>" argument; Explorer wants /select,"<path>"
        QProcess p;
        p.setProgram(program);
        p.setNativeArguments(nativeArgs);
        return p.startDetached();
    }
#endif
    QProcess p;
    p.start(program, args);
    if (p.waitForFinished(3000) && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0)
        return true;
    p.kill();
    p.waitForFinished(500);
    return false;
}
} // namespace

void setCommandHook(const CommandHook &h)
{
    commandHook() = h;
}

QString explorerSelectArguments(const QString &path)
{
    // Explorer splits its command line at commas outside quotes: "/select,C:\a, b.mkv" would select
    // "C:\a"; the quoted form keeps the whole name (a Windows path cannot contain a quote itself)
    return QStringLiteral("/select,\"%1\"").arg(QDir::toNativeSeparators(path));
}

QString fileManagerItems(const QString &path)
{
    QString uri = QString::fromUtf8(QUrl::fromLocalFile(path).toEncoded());
    uri.replace(QLatin1Char('\''), QStringLiteral("%27")).replace(QLatin1Char('\\'), QStringLiteral("%5C"));
    return QStringLiteral("['%1']").arg(uri);
}

bool isDangerousFile(const QString &name)
{
    static const QStringList ext = {QStringLiteral("exe"), QStringLiteral("scr"), QStringLiteral("com"), QStringLiteral("bat"),
                                    QStringLiteral("pif"), QStringLiteral("vbs"), QStringLiteral("cmd"), QStringLiteral("msi"),
                                    QStringLiteral("zip"), QStringLiteral("rar"), QStringLiteral("js"), QStringLiteral("ps1"),
                                    QStringLiteral("lnk"), QStringLiteral("jar"), QStringLiteral("sh")};
    return ext.contains(QFileInfo(name).suffix().toLower());
}

void setHook(const Hook &h)
{
    hook() = h;
}

void setAskHook(const AskHook &h)
{
    askHook() = h;
}

bool openFile(QWidget *parent, mx::Settings *settings, const QString &path, QString *why)
{
    const QFileInfo fi(path);
    if (path.isEmpty() || !fi.exists()) {
        if (why)
            *why = QObject::tr("The file \"%1\" is not there any more (moved or deleted).").arg(QDir::toNativeSeparators(path));
        return false;
    }
    const bool suppressed = Session::instance()->suppressDangerousWarning() ||
                            (settings && settings->b(keys::transfers::SuppressDangerousWarning));
    if (fi.isFile() && isDangerousFile(fi.fileName()) && !suppressed) {
        bool go = false;
        if (askHook()) {
            go = askHook()(parent, path);
        } else {
            QMessageBox box(QMessageBox::Warning, QObject::tr("OctoMX"),
                            QObject::tr("You are about to open\n\n    %1\n\nThis is a program or script, or a file that can hide "
                                        "one. Such files may carry a virus or other harmful code.\n\n"
                                        "Open it all the same?\n\n(This warning can be switched off for the rest of the "
                                        "session in the program settings.)")
                                .arg(fi.fileName()),
                            QMessageBox::Yes | QMessageBox::No, parent);
            box.setObjectName(QStringLiteral("openWarning"));
            box.setDefaultButton(QMessageBox::No);
            go = box.exec() == QMessageBox::Yes;
        }
        if (!go)
            return false;
    }
    // [ui-h] Launch Options: the program set for this type when the system default is off
    const QStringList user = fi.isFile() ? userCommand(settings, fi.absoluteFilePath()) : QStringList();
    if (!user.isEmpty()) {
        if (hook())
            return hook()(QStringLiteral("command"), user.join(QLatin1Char(' ')));
        if (logged(QStringLiteral("command"), user.join(QLatin1Char(' '))))
            return true;
        if (!startCommand(user.first(), user.mid(1), QString(), true)) {
            if (why)
                *why = QObject::tr("The program \"%1\" set in Settings, Launch Options could not be started.").arg(user.first());
            return false;
        }
        return true;
    }
    if (hook())
        return hook()(QStringLiteral("open"), fi.absoluteFilePath());
    if (logged(QStringLiteral("open"), fi.absoluteFilePath()))
        return true;
    if (!startCommand(QStringLiteral("url"), {QUrl::fromLocalFile(fi.absoluteFilePath()).toString(QUrl::FullyEncoded)})) {
        if (why)
            *why = QObject::tr("No program is set up to open \"%1\".").arg(fi.fileName());
        return false;
    }
    return true;
}

namespace {
bool noFileManager(const QString &dir, QString *why)
{
    if (why)
        *why = QObject::tr("No file manager could be started for \"%1\".").arg(QDir::toNativeSeparators(dir));
    return false;
}
} // namespace

bool showInFolder(const QString &path, QString *why)
{
    QFileInfo fi(path);
    if (path.isEmpty() || !fi.exists()) {
        // a file that is gone: at least its folder, if that still exists
        const QFileInfo dir(fi.absolutePath());
        if (path.isEmpty() || !dir.isDir()) {
            if (why)
                *why = QObject::tr("The folder of \"%1\" does not exist any more.").arg(QDir::toNativeSeparators(path));
            return false;
        }
        fi = dir;
    }
    const QString abs = fi.absoluteFilePath();
    if (hook())
        return hook()(QStringLiteral("show"), abs);
    if (logged(QStringLiteral("show"), abs))
        return true;
    if (fi.isDir())   // a torrent bundle: its folder
        return startCommand(QStringLiteral("url"), {QUrl::fromLocalFile(abs).toString(QUrl::FullyEncoded)}) || noFileManager(fi.absoluteFilePath(), why);
#ifdef Q_OS_WIN
    // Explorer opens the folder with the file selected
    if (startCommand(QStringLiteral("explorer.exe"), {}, explorerSelectArguments(abs)))
        return true;
#else
    // the desktop file manager selects the file (Nautilus, Dolphin, Nemo, Thunar, Caja ...)
    const QString gdbus = QStandardPaths::findExecutable(QStringLiteral("gdbus"));
    if (!gdbus.isEmpty() && qEnvironmentVariableIsSet("DBUS_SESSION_BUS_ADDRESS") &&
        startCommand(gdbus, {QStringLiteral("call"), QStringLiteral("--session"), QStringLiteral("--dest"),
                             QStringLiteral("org.freedesktop.FileManager1"), QStringLiteral("--object-path"),
                             QStringLiteral("/org/freedesktop/FileManager1"), QStringLiteral("--method"),
                             QStringLiteral("org.freedesktop.FileManager1.ShowItems"), fileManagerItems(abs), QString()}))
        return true;
#endif
    if (!startCommand(QStringLiteral("url"), {QUrl::fromLocalFile(fi.absolutePath()).toString(QUrl::FullyEncoded)}))
        return noFileManager(fi.absolutePath(), why);
    return true;
}

// ------------------------------------------------------------------------------------ [dl-1] preview

namespace {
PlayersHook &playersHook()
{
    static PlayersHook h;
    return h;
}
} // namespace

void setPlayersHook(const PlayersHook &h)
{
    playersHook() = h;
}

QList<Player> findPlayers()
{
    if (playersHook())
        return playersHook()();
    QList<Player> out;
    auto add = [&out](const QString &name, const QString &path) {
        if (path.isEmpty() || !QFileInfo(path).isFile())
            return;
        for (const Player &p : std::as_const(out))
            if (QFileInfo(p.path).canonicalFilePath() == QFileInfo(path).canonicalFilePath())
                return;
        out << Player{name, QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath())};
    };
#ifdef Q_OS_WIN
    QStringList roots;
    for (const char *v : {"ProgramFiles", "ProgramW6432", "ProgramFiles(x86)", "LOCALAPPDATA"}) {
        const QString r = qEnvironmentVariable(v);
        if (!r.isEmpty() && !roots.contains(r, Qt::CaseInsensitive))
            roots << r;
    }
    const struct { const char *name; const char *rel; } known[] = {
        {"VLC media player", "VideoLAN/VLC/vlc.exe"},
        {"mpv", "mpv/mpv.exe"},
        {"MPC-HC", "MPC-HC/mpc-hc64.exe"},
        {"MPC-HC", "MPC-HC/mpc-hc.exe"},
        {"MPC-HC", "K-Lite Codec Pack/MPC-HC64/mpc-hc64.exe"},
        {"MPC-BE", "MPC-BE x64/mpc-be64.exe"},
        {"MPC-BE", "MPC-BE/mpc-be.exe"},
        {"Windows Media Player", "Windows Media Player/wmplayer.exe"},
    };
    for (const auto &k : known)
        for (const QString &r : std::as_const(roots))
            add(QString::fromLatin1(k.name), QDir(r).filePath(QString::fromLatin1(k.rel)));
    add(QStringLiteral("mpv"), QStandardPaths::findExecutable(QStringLiteral("mpv")));
    add(QStringLiteral("VLC media player"), QStandardPaths::findExecutable(QStringLiteral("vlc")));
#else
    const struct { const char *name; const char *exe; } known[] = {
        {"VLC media player", "vlc"}, {"mpv", "mpv"}, {"Celluloid", "celluloid"}, {"Videos (Totem)", "totem"}};
    for (const auto &k : known)
        add(QString::fromLatin1(k.name), QStandardPaths::findExecutable(QString::fromLatin1(k.exe)));
#endif
    return out;
}

QStringList playerArguments(const QString &file)
{
    return {QDir::toNativeSeparators(QFileInfo(file).absoluteFilePath())};
}

QString playerNativeArguments(const QString &file)
{
    return QStringLiteral("\"%1\"").arg(QDir::toNativeSeparators(QFileInfo(file).absoluteFilePath()));
}

bool previewFile(QWidget *parent, const QString &path, const QString &choice, const QString &program, QString *why)
{
    Q_UNUSED(parent)
    auto fail = [why](const QString &t) {
        if (why)
            *why = t;
        return false;
    };
    const QFileInfo fi(path);
    if (path.isEmpty() || !fi.isFile())
        return fail(QObject::tr("The file \"%1\" is not there any more (moved or deleted).").arg(QDir::toNativeSeparators(path)));
    // never anything but audio / video: the dangerous-file rule of openFile() cannot be bypassed here
    if (!mx::preview::isMediaFile(fi.fileName()) || isDangerousFile(fi.fileName()))
        return fail(QObject::tr("Only audio and video files can be previewed."));
    const QString abs = fi.absoluteFilePath();
    if (choice == QLatin1String("program")) {
        const QFileInfo pi(program);
        if (program.trimmed().isEmpty() || !pi.isFile())
            return fail(QObject::tr("The preview player \"%1\" was not found. Choose another one in Settings, File Launching, "
                                    "Preview Player.").arg(QDir::toNativeSeparators(program)));
        if (pi.absoluteFilePath() == abs || mx::preview::isMediaFile(pi.fileName()) || !pi.isExecutable())
            return fail(QObject::tr("\"%1\" is not a program that can play files.").arg(QDir::toNativeSeparators(program)));
        if (hook())
            return hook()(QStringLiteral("preview-with:") + QDir::toNativeSeparators(pi.absoluteFilePath()), abs);
        if (logged(QStringLiteral("preview-with:") + QDir::toNativeSeparators(pi.absoluteFilePath()), abs))
            return true;
        if (!startCommand(pi.absoluteFilePath(), playerArguments(abs), playerNativeArguments(abs), true))
            return fail(QObject::tr("\"%1\" could not be started.").arg(QDir::toNativeSeparators(program)));
        return true;
    }
    if (hook())
        return hook()(QStringLiteral("preview"), abs);
    if (logged(QStringLiteral("preview"), abs))
        return true;
    if (!startCommand(QStringLiteral("url"), {QUrl::fromLocalFile(abs).toString(QUrl::FullyEncoded)}))
        return fail(QObject::tr("No program is set up to play \"%1\". Choose a player in Settings, File Launching, Preview Player.")
                        .arg(fi.fileName()));
    return true;
}

// ------------------------------------------------------------------ [ui-h] launch settings

QStringList userCommand(mx::Settings *settings, const QString &path)
{
    if (!settings || settings->b(keys::launch::UseOsDefault))
        return {};
    QString cmd;
    switch (mediaTypeForFileName(path)) {
    case MediaType::Audio:    cmd = settings->s(keys::launch::AudioCommand); break;
    case MediaType::Video:    cmd = settings->s(keys::launch::VideoCommand); break;
    case MediaType::Image:    cmd = settings->s(keys::launch::ImageCommand); break;
    case MediaType::Document: cmd = settings->s(keys::launch::DocumentCommand); break;
    default:                  cmd = settings->s(keys::launch::OtherCommand); break;
    }
    QStringList args = QProcess::splitCommand(cmd.trimmed());
    if (args.isEmpty())
        return {};
    bool substituted = false;
    for (int i = 1; i < args.size(); ++i) {
        if (args[i].contains(QLatin1String("%1"))) {
            args[i].replace(QLatin1String("%1"), QDir::toNativeSeparators(path));
            substituted = true;
        }
    }
    if (!substituted)
        args << QDir::toNativeSeparators(path);
    return args;
}

QList<QVariantMap> menuEntries(mx::Settings *settings, const QString &fileName)
{
    QList<QVariantMap> out;
    if (!settings)
        return out;
    const QString suffix = QFileInfo(fileName).suffix().toLower();
    const QVariantList all = settings->value(keys::launch::Entries).toMap().value(QStringLiteral("entries")).toList();
    for (const QVariant &v : all) {
        const QVariantMap e = v.toMap();
        const QString title = e.value(QStringLiteral("title")).toString().trimmed();
        if (title.isEmpty())
            continue;
        const QString ext = e.value(QStringLiteral("ext")).toString().trimmed();
        bool match = ext.isEmpty() || ext == QLatin1String("*");
        if (!match) {
            static const QRegularExpression sep(QStringLiteral("[\\s,;]+"));
            for (QString x : ext.toLower().split(sep, Qt::SkipEmptyParts)) {
                x.remove(QLatin1String("*")).remove(QLatin1Char('.'));
                if (x == suffix)
                    match = true;
            }
        }
        if (!match)
            continue;
        const bool hasCommand = !e.value(QStringLiteral("command")).toString().trimmed().isEmpty();
        const QString verb = e.value(QStringLiteral("action")).toString().trimmed().toLower();
#ifdef Q_OS_WIN
        if (!hasCommand && (verb.isEmpty() || verb == QLatin1String("open")))
            continue;
        // a verb is offered only when the file's type has it (Winamp's "Enqueue", a player's "Play"):
        // otherwise it would only be Open File a second time (qa-2: no duplicate entries)
        if (!hasCommand) {
            const QString dotExt = QLatin1Char('.') + suffix;
            wchar_t buf[1024];
            DWORD n = 1024;
            if (suffix.isEmpty() || AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_COMMAND, reinterpret_cast<LPCWSTR>(dotExt.utf16()),
                                                      reinterpret_cast<LPCWSTR>(verb.utf16()), buf, &n) != S_OK)
                continue;
        }
#else
        if (!hasCommand)
            continue;
        Q_UNUSED(verb)
#endif
        out << e;
    }
    return out;
}

QStringList entryCommand(const QVariantMap &e, const QString &path)
{
    const QString command = e.value(QStringLiteral("command")).toString().trimmed();
    if (command.isEmpty())
        return {};
    QString params = e.value(QStringLiteral("params")).toString();
    if (params.trimmed().isEmpty())
        params = QStringLiteral("%path%");
    const QString native = QDir::toNativeSeparators(path);
    QStringList args = QProcess::splitCommand(params);
    for (QString &a : args)
        a.replace(QLatin1String("%path%"), native, Qt::CaseInsensitive).replace(QLatin1String("%1"), native);
    QStringList prog = QProcess::splitCommand(command);
    if (prog.isEmpty())
        prog << command;
    return prog + args;
}

bool runEntry(QWidget *parent, mx::Settings *settings, const QVariantMap &e, const QString &path, QString *why)
{
    const QString title = e.value(QStringLiteral("title")).toString();
    const QFileInfo fi(path);
    if (!fi.exists()) {
        if (why)
            *why = QObject::tr("The file \"%1\" is not there any more (moved or deleted).").arg(QDir::toNativeSeparators(path));
        return false;
    }
    const QStringList cmd = entryCommand(e, fi.absoluteFilePath());
    if (hook())
        return hook()(QStringLiteral("entry ") + title, cmd.isEmpty() ? fi.absoluteFilePath() : cmd.join(QLatin1Char(' ')));
    if (logged(QStringLiteral("entry ") + title, cmd.isEmpty() ? fi.absoluteFilePath() : cmd.join(QLatin1Char(' '))))
        return true;
    if (!cmd.isEmpty()) {
        QProcess p;
        p.setProgram(cmd.first());
        p.setArguments(cmd.mid(1));
        const QString dir = e.value(QStringLiteral("workdir")).toString().trimmed();
        if (!dir.isEmpty())
            p.setWorkingDirectory(dir);
        p.setStandardInputFile(QProcess::nullDevice());
        p.setStandardOutputFile(QProcess::nullDevice());
        p.setStandardErrorFile(QProcess::nullDevice());
        if (commandHook())
            return commandHook()(cmd.first(), cmd.mid(1), QString());
        if (p.startDetached())
            return true;
        if (why)
            *why = QObject::tr("\"%1\" could not start \"%2\".").arg(title, cmd.first());
        return false;
    }
#ifdef Q_OS_WIN
    const QString verb = e.value(QStringLiteral("action")).toString().trimmed();
    const QString native = QDir::toNativeSeparators(fi.absoluteFilePath());
    const auto r = reinterpret_cast<qintptr>(ShellExecuteW(nullptr, reinterpret_cast<LPCWSTR>(verb.utf16()),
                                                           reinterpret_cast<LPCWSTR>(native.utf16()), nullptr, nullptr, SW_SHOWNORMAL));
    if (r > 32)
        return true;
#endif
    return openFile(parent, settings, fi.absoluteFilePath(), why);
}

} // namespace mx::ui::launcher
