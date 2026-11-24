// SPDX-License-Identifier: GPL-3.0-or-later
// [safe-1] see mx/durable_file.h
#include "mx/durable_file.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QMutex>
#include <QtCore/QPair>
#include <QtCore/QThread>

#ifdef Q_OS_WIN
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace mx::durable {

namespace {

bool syncFd(int fd)
{
    if (fd < 0) return false;
#ifdef Q_OS_WIN
    const HANDLE h = HANDLE(_get_osfhandle(fd));
    return h != INVALID_HANDLE_VALUE && FlushFileBuffers(h);
#elif defined(Q_OS_LINUX)
    return ::fdatasync(fd) == 0;
#else
    return ::fsync(fd) == 0;
#endif
}

struct Journal {
    QMutex mutex;
    QHash<QString, QList<QPair<qint64, qint64>>> unsynced;   // path -> [offset, length]
};

Journal &journal()
{
    static Journal j;
    return j;
}

QString key(const QString &path) { return QDir::cleanPath(QFileInfo(path).absoluteFilePath()); }

} // namespace

bool syncFile(QFileDevice &f)
{
    if (!f.isOpen()) return false;
    f.flush();   // Qt's own buffer (none for Unbuffered files)
    return syncFd(f.handle());
}

bool syncPath(const QString &path)
{
    QFile f(path);
    if (!f.exists()) return true;
#ifdef Q_OS_WIN
    // FlushFileBuffers needs a handle with write access
    if (!f.open(QIODevice::ReadWrite)) return false;
#else
    if (!f.open(QIODevice::ReadOnly)) return false;
#endif
    const bool ok = syncFd(f.handle());
    if (ok) noteSynced(path);
    return ok;
}

bool syncDir(const QString &dir)
{
#ifdef Q_OS_WIN
    Q_UNUSED(dir)
    return true;
#else
    const QByteArray p = QFile::encodeName(dir.isEmpty() ? QStringLiteral(".") : dir);
    const int fd = ::open(p.constData(), O_RDONLY);
    if (fd < 0) return false;
    const bool ok = ::fsync(fd) == 0;
    ::close(fd);
    return ok;
#endif
}

QString copyTempName(const QString &to)
{
    const QFileInfo fi(to);
    return fi.absoluteDir().filePath(QStringLiteral(".") + fi.fileName() + QStringLiteral(".octomx-copy"));
}

bool copyFile(const QString &from, const QString &to, QString *error, const std::atomic<bool> *cancel)
{
    const QString tmp = copyTempName(to);
    QFile::remove(tmp);
    QFile in(from), out(tmp);
    auto fail = [&](const QString &why) {
        if (error) *error = why;
        out.close();
        QFile::remove(tmp);
        return false;
    };
    if (!in.open(QIODevice::ReadOnly)) return fail(in.errorString());
    if (!out.open(QIODevice::WriteOnly)) return fail(out.errorString());
    for (;;) {
        if (cancel && cancel->load()) return fail(QStringLiteral("cancelled"));
        const QByteArray buf = in.read(4 << 20);
        if (buf.isEmpty()) {
            if (!in.atEnd()) return fail(in.errorString());
            break;
        }
        if (const int ms = testWriteDelayMs(); ms > 0) QThread::msleep(ulong(ms));   // test seam: a slow disk
        if (out.write(buf) != buf.size()) return fail(out.errorString());
    }
    if (out.size() != in.size()) return fail(QStringLiteral("the copy is shorter than the file"));
    if (!syncFile(out)) return fail(QStringLiteral("cannot write the copy to the disk"));
    out.close();
    in.close();
    if (QFileInfo::exists(to)) return fail(QStringLiteral("%1 exists").arg(to));
    if (!QFile::rename(tmp, to)) return fail(QStringLiteral("cannot rename the copy to %1").arg(to));
    syncDir(QFileInfo(to).absolutePath());
    return true;
}

int testWriteDelayMs()
{
    static const int ms = qEnvironmentVariableIntValue("MX_TEST_SLOW_WRITE_MS");
    return ms;
}

bool powerCutTest()
{
    static const bool on = qEnvironmentVariableIntValue("MX_TEST_POWERCUT") > 0;
    return on;
}

qint64 journalWrite(QFileDevice &f, const QString &path, qint64 off, const QByteArray &data)
{
    if (const int ms = testWriteDelayMs(); ms > 0) QThread::msleep(ulong(ms));
    if (!powerCutTest()) return f.seek(off) ? f.write(data) : -1;
    Journal &j = journal();
    QMutexLocker l(&j.mutex);
    const qint64 n = f.seek(off) ? f.write(data) : -1;
    if (n > 0) j.unsynced[key(path)].append(qMakePair(off, n));
    return n;
}

void noteWritten(const QString &path, qint64 off, qint64 len)
{
    if (!powerCutTest() || len <= 0) return;
    Journal &j = journal();
    QMutexLocker l(&j.mutex);
    j.unsynced[key(path)].append(qMakePair(off, len));
}

bool powerCutLies()
{
    static const bool on = qEnvironmentVariableIntValue("MX_TEST_POWERCUT_LIE") > 0;
    return on;
}

void noteSynced(const QString &path)
{
    if (!powerCutTest() || powerCutLies()) return;
    Journal &j = journal();
    QMutexLocker l(&j.mutex);
    j.unsynced.remove(key(path));
}

void noteRenamed(const QString &from, const QString &to)
{
    if (!powerCutTest()) return;
    Journal &j = journal();
    QMutexLocker l(&j.mutex);
    const auto v = j.unsynced.take(key(from));
    if (!v.isEmpty()) j.unsynced[key(to)] += v;
}

qint64 simulatePowerCut(QString *report)
{
    Journal &j = journal();
    j.mutex.lock();   // never unlocked: the process is killed right after
    qint64 zeroed = 0;
    QStringList lines;
    for (auto it = j.unsynced.constBegin(); it != j.unsynced.constEnd(); ++it) {
        QFile f(it.key());
        if (!f.open(QIODevice::ReadWrite)) continue;
        qint64 n = 0;
        for (const auto &r : it.value()) {
            const qint64 end = qMin(r.first + r.second, f.size());
            if (end <= r.first || !f.seek(r.first)) continue;
            const QByteArray zeros(int(end - r.first), '\0');
            if (f.write(zeros) == zeros.size()) n += zeros.size();
        }
        f.flush();
        syncFd(f.handle());
        zeroed += n;
        lines << QStringLiteral("%1: %2 bytes").arg(QFileInfo(it.key()).fileName()).arg(n);
    }
    if (report) *report = lines.join(QStringLiteral(", "));
    return zeroed;
}

void saveJournal(const QString &file)
{
    if (!powerCutTest()) return;
    Journal &j = journal();
    QMutexLocker l(&j.mutex);
    QFile f(file);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    for (auto it = j.unsynced.constBegin(); it != j.unsynced.constEnd(); ++it)
        for (const auto &r : it.value()) f.write(QStringLiteral("%1\t%2\t%3\n").arg(r.first).arg(r.second).arg(it.key()).toUtf8());
}

void loadJournal(const QString &file)
{
    if (!powerCutTest()) return;
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return;
    Journal &j = journal();
    QMutexLocker l(&j.mutex);
    while (!f.atEnd()) {
        const QStringList p = QString::fromUtf8(f.readLine()).trimmed().split(QLatin1Char('\t'));
        if (p.size() == 3) j.unsynced[p.at(2)].append(qMakePair(p.at(0).toLongLong(), p.at(1).toLongLong()));
    }
}

} // namespace mx::durable
