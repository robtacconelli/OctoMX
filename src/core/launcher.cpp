// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/launcher.h"

#include "mx/services.h"
#include "mx/settings_keys.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>

namespace mx {

namespace {

QStringList osOpen(const QString &target)
{
#if defined(Q_OS_WIN)
    // rundll32 url.dll,FileProtocolHandler opens with the associated program (like ShellExecute "open")
    return {QStringLiteral("rundll32"), QStringLiteral("url.dll,FileProtocolHandler"), QDir::toNativeSeparators(target)};
#elif defined(Q_OS_MACOS)
    return {QStringLiteral("open"), target};
#else
    return {QStringLiteral("xdg-open"), target};
#endif
}

} // namespace

QStringList launchCommand(Settings *settings, const QString &path)
{
    if (settings && !settings->b(keys::launch::UseOsDefault)) {
        QString cmd;
        switch (mediaTypeForFileName(path)) {
        case MediaType::Audio:    cmd = settings->s(keys::launch::AudioCommand); break;
        case MediaType::Video:    cmd = settings->s(keys::launch::VideoCommand); break;
        case MediaType::Image:    cmd = settings->s(keys::launch::ImageCommand); break;
        case MediaType::Document: cmd = settings->s(keys::launch::DocumentCommand); break;
        default:                  cmd = settings->s(keys::launch::OtherCommand); break;
        }
        cmd = cmd.trimmed();
        if (!cmd.isEmpty()) {
            QStringList args = QProcess::splitCommand(cmd);
            bool substituted = false;
            for (QString &a : args) {
                if (a.contains(QLatin1String("%1"))) {
                    a.replace(QLatin1String("%1"), QDir::toNativeSeparators(path));
                    substituted = true;
                }
            }
            if (!substituted) args << QDir::toNativeSeparators(path);
            if (!args.isEmpty()) return args;
        }
    }
    return osOpen(path);
}

bool launchFile(Settings *settings, const QString &path)
{
    if (!QFileInfo::exists(path)) return false;
    QStringList cmd = launchCommand(settings, path);
    if (cmd.isEmpty()) return false;
    const QString program = cmd.takeFirst();
    return QProcess::startDetached(program, cmd);
}

bool openContainingFolder(const QString &path)
{
    const QFileInfo fi(path);
#if defined(Q_OS_WIN)
    if (fi.exists() && fi.isFile())
        return QProcess::startDetached(QStringLiteral("explorer.exe"),
                                       {QStringLiteral("/select,") + QDir::toNativeSeparators(fi.absoluteFilePath())});
#endif
    const QString dir = fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath();
    if (!QFileInfo(dir).isDir()) return false;
    QStringList cmd = osOpen(dir);
    const QString program = cmd.takeFirst();
    return QProcess::startDetached(program, cmd);
}

} // namespace mx
