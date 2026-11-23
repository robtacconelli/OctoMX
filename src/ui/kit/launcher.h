// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: open a downloaded / shared file, or show it in the file manager. [ui-e]
//
//   launcher::openFile(parent, settings, path)   the OS handler; executables / archives first get the
//                                               classic dangerous-file warning (unless suppressed)
//   launcher::showInFolder(path)                Explorer with the file selected (Windows), the desktop
//                                               file manager via org.freedesktop.FileManager1 (Linux),
//                                               else the containing folder; a folder opens itself
//
// Tests replace the OS side with setHook(); MX_LAUNCH_LOG=<file> records the requests instead of
// launching anything (scripted / headless runs).
#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVariantMap>

#include <functional>

class QWidget;

namespace mx {
class Settings;
}

namespace mx::ui::launcher {

// file types OctoMX warns about (executables, scripts, archives that may hide them)
bool isDangerousFile(const QString &name);

// action: "open" or "show"; return false = could not be done (the caller says why)
using Hook = std::function<bool(const QString &action, const QString &path)>;
void setHook(const Hook &h);
// the dangerous-file question; return true = go ahead (tests replace it)
using AskHook = std::function<bool(QWidget *parent, const QString &path)>;
void setAskHook(const AskHook &h);

// [qa-2] the process / URL the launcher is about to start, after every path conversion: program
// "explorer.exe" with native arguments `/select,"C:\dir\file.ext"` (Windows), "gdbus" with the
// FileManager1 call (Linux), or "url" with the file:// URL handed to the desktop. A command hook
// replaces only that last step (the real conversions run), so tests can check the exact command
// line without opening a window. Return false = the start failed.
using CommandHook = std::function<bool(const QString &program, const QStringList &args, const QString &nativeArgs)>;
void setCommandHook(const CommandHook &h);
// Explorer's arguments for selecting `path` (made native and quoted: names with spaces, commas, "&"
// or non-ASCII letters select the right file; the path must not be split at a comma)
QString explorerSelectArguments(const QString &path);
// the FileManager1.ShowItems URI list for `path` (a GVariant string array; a quote in the name is
// percent-encoded so it cannot end the string)
QString fileManagerItems(const QString &path);

// false + *why when the file is gone or nothing could open it; false without a reason when the user
// said No to the warning
bool openFile(QWidget *parent, mx::Settings *settings, const QString &path, QString *why = nullptr);
bool showInFolder(const QString &path, QString *why = nullptr);

// ---- [ui-h] Settings > File Launching / Launch Options
// The program set for the file's type when "Use the system default programs" is off (launch/
// audioCommand ... otherCommand, %1 = the file, else it is appended): program + arguments, or empty
// (= the system's handler). openFile() uses it after the dangerous-file warning.
QStringList userCommand(mx::Settings *settings, const QString &path);
// The File Launching entries (launch/entries: title, ext, action, command, params, workdir) that
// belong in the menu of `fileName`: ext "*" or a list of extensions ("mp3 ogg", ".mp3;.ogg"); an
// entry without a command is a shell verb ("play", "enqueue" ...), offered on Windows only and only
// when the file's type has that verb (else it would be Open File again), "open" never.
QList<QVariantMap> menuEntries(mx::Settings *settings, const QString &fileName);
// Runs an entry for `path`: its command with params (%path% / %1 = the native path) in workdir, or
// on Windows the shell verb. Hook / MX_LAUNCH_LOG record "entry <title>" instead.
bool runEntry(QWidget *parent, mx::Settings *settings, const QVariantMap &entry, const QString &path, QString *why = nullptr);
// the program and arguments runEntry starts for a command entry (tests)
QStringList entryCommand(const QVariantMap &entry, const QString &path);

// ---- [dl-1] preview players
//   launcher::previewFile(path, choice, program)   plays an audio / video file (also a partial one):
//                                                  choice "system" = the OS handler of its type, "program" =
//                                                  that player with the file as its only argument. Refuses
//                                                  anything that is not audio / video by the mxapi table, and a
//                                                  program that is the file itself or a media file: preview
//                                                  never starts the file as a program.
struct Player {
    QString name;       // "VLC media player", "mpv" ...
    QString path;       // absolute path of the executable
};
// VLC, mpv, MPC-HC, MPC-BE, Windows Media Player in the usual places (Windows); vlc, mpv, celluloid,
// totem in PATH (Linux). Tests replace it with setPlayersHook().
QList<Player> findPlayers();
using PlayersHook = std::function<QList<Player>()>;
void setPlayersHook(const PlayersHook &h);
// the command a player is started with: program + arguments (Linux: {file}, passed as one argv
// entry, no shell) and, on Windows, the native command line after the program ("C:\dir\a b.mkv"
// in quotes; a Windows path cannot contain a quote)
QStringList playerArguments(const QString &file);
QString playerNativeArguments(const QString &file);
bool previewFile(QWidget *parent, const QString &path, const QString &choice, const QString &program, QString *why = nullptr);

} // namespace mx::ui::launcher
