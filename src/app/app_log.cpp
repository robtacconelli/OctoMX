// SPDX-License-Identifier: GPL-3.0-or-later
#include "app_log.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QMutex>

#include <cstdio>

namespace mx::app {

namespace {
QFile *g_file = nullptr;
QMutex g_mutex;
QtMessageHandler g_prev = nullptr;

void handler(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
{
    static const char kinds[] = {'D', 'W', 'C', 'F', 'I'};
    const char k = (type >= 0 && type <= 4) ? kinds[type] : '?';
    const QString line = QStringLiteral("%1 %2 %3: %4\n")
                             .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")))
                             .arg(QLatin1Char(k))
                             .arg(QString::fromLatin1(ctx.category ? ctx.category : "default"), msg);
    {
        QMutexLocker lock(&g_mutex);
        if (g_file) {
            g_file->write(line.toUtf8());
            g_file->flush();
        }
    }
    if (g_prev)
        g_prev(type, ctx, msg);
    else
        fprintf(stderr, "%s", line.toLocal8Bit().constData());
}
} // namespace

void installAppLog(const QString &profileDir)
{
    const QString dir = QDir(profileDir).filePath(QStringLiteral("logs"));
    QDir().mkpath(dir);
    const QString path = QDir(dir).filePath(QStringLiteral("octomx.log"));
    if (QFileInfo(path).size() > 2 * 1024 * 1024) {
        QFile::remove(QDir(dir).filePath(QStringLiteral("octomx.1.log")));
        QFile::rename(path, QDir(dir).filePath(QStringLiteral("octomx.1.log")));
    }
    g_file = new QFile(path);
    if (!g_file->open(QIODevice::Append | QIODevice::Text)) {
        delete g_file;
        g_file = nullptr;
    }
    g_prev = qInstallMessageHandler(handler);
}

} // namespace mx::app
