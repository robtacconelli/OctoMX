// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/log.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QMutex>

#include <atomic>
#include <cstdio>

Q_LOGGING_CATEGORY(lcCore, "mx.core")
Q_LOGGING_CATEGORY(lcDb, "mx.db")
Q_LOGGING_CATEGORY(lcLibrary, "mx.library")
Q_LOGGING_CATEGORY(lcSearch, "mx.search")
Q_LOGGING_CATEGORY(lcNet, "mx.net")
Q_LOGGING_CATEGORY(lcChat, "mx.chat")
Q_LOGGING_CATEGORY(lcEngine, "mx.engine")

namespace mx {
namespace {

struct LogState {
    QMutex mutex;
    QFile file;
    QString path;
    qint64 maxBytes = 5 * 1024 * 1024;
    int keep = 5;
    bool alsoStderr = true;
    std::atomic<int> minLevel{1};
    QtMessageHandler previous = nullptr;
    bool installed = false;
};

LogState &state()
{
    static LogState s;
    return s;
}

void rotateLocked(LogState &s)
{
    s.file.close();
    const QString base = s.path.left(s.path.size() - 4); // strip ".log"
    QFile::remove(base + QStringLiteral(".%1.log").arg(s.keep));
    for (int i = s.keep - 1; i >= 1; --i)
        QFile::rename(base + QStringLiteral(".%1.log").arg(i), base + QStringLiteral(".%1.log").arg(i + 1));
    QFile::rename(s.path, base + QStringLiteral(".1.log"));
    s.file.setFileName(s.path);
    if (!s.file.open(QIODevice::Append | QIODevice::Text)) return;
}

void handler(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
{
    LogState &s = state();
    int level = 0;
    char c = 'D';
    switch (type) {
    case QtDebugMsg:    level = 0; c = 'D'; break;
    case QtInfoMsg:     level = 1; c = 'I'; break;
    case QtWarningMsg:  level = 2; c = 'W'; break;
    case QtCriticalMsg: level = 3; c = 'C'; break;
    case QtFatalMsg:    level = 4; c = 'F'; break;
    }
    if (level >= s.minLevel.load() || level >= 2) {
        const QByteArray line = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")).toUtf8()
            + ' ' + c + ' ' + (ctx.category ? QByteArray(ctx.category) : QByteArray("default")) + ": "
            + msg.toUtf8() + '\n';
        QMutexLocker l(&s.mutex);
        if (s.file.isOpen()) {
            s.file.write(line);
            s.file.flush();
            if (s.file.size() > s.maxBytes) rotateLocked(s);
        }
    }
    if (s.alsoStderr && s.previous)
        s.previous(type, ctx, msg);
}

} // namespace

void installLogHandler(const QString &profileDir, qint64 maxBytes, int keep, bool alsoStderr, int minLevel)
{
    LogState &s = state();
    {
        QMutexLocker l(&s.mutex);
        QDir().mkpath(profileDir + QStringLiteral("/logs"));
        s.path = profileDir + QStringLiteral("/logs/octomx.log");
        s.maxBytes = qMax<qint64>(64 * 1024, maxBytes);
        s.keep = qMax(1, keep);
        s.alsoStderr = alsoStderr;
        s.minLevel = minLevel;
        if (s.file.isOpen()) s.file.close();
        s.file.setFileName(s.path);
        if (!s.file.open(QIODevice::Append | QIODevice::Text))
            fprintf(stderr, "octomx: cannot open log file %s\n", qPrintable(s.path));
        if (s.file.size() > s.maxBytes) rotateLocked(s);
    }
    if (!s.installed) {
        s.previous = qInstallMessageHandler(handler);
        s.installed = true;
    }
    QLoggingCategory::setFilterRules(minLevel <= 0 ? QStringLiteral("mx.*.debug=true")
                                                   : QStringLiteral("mx.*.debug=false"));
}

void setLogLimits(qint64 maxBytes, int keep)
{
    LogState &s = state();
    QMutexLocker l(&s.mutex);
    s.maxBytes = qMax<qint64>(64 * 1024, maxBytes);
    s.keep = qMax(1, keep);
    if (s.file.isOpen() && s.file.size() > s.maxBytes) rotateLocked(s);
}

qint64 logMaxBytes()
{
    LogState &s = state();
    QMutexLocker l(&s.mutex);
    return s.maxBytes;
}

int logKeepFiles()
{
    LogState &s = state();
    QMutexLocker l(&s.mutex);
    return s.keep;
}

void setLogLevel(const QString &level)
{
    const QString l = level.toLower();
    const int v = l == QLatin1String("debug") ? 0 : l == QLatin1String("warning") ? 2 : 1;
    state().minLevel = v;
    QLoggingCategory::setFilterRules(v <= 0 ? QStringLiteral("mx.*.debug=true")
                                            : QStringLiteral("mx.*.debug=false"));
}

void uninstallLogHandler()
{
    LogState &s = state();
    if (!s.installed) return;
    qInstallMessageHandler(s.previous);
    s.installed = false;
    QMutexLocker l(&s.mutex);
    s.file.close();
    s.path.clear();
}

QString logFilePath()
{
    LogState &s = state();
    QMutexLocker l(&s.mutex);
    return s.path;
}

} // namespace mx
