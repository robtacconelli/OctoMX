// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX: the trash / Recycle Bin and its test seam [ui-j] (see mx/trash.h).
#include "mx/trash.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSaveFile>
#include <QtCore/QStorageInfo>
#include <QtCore/QUrl>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace mx::trash {

namespace {
QString osRoot()
{
    return qEnvironmentVariable("MX_OS_ROOT");
}

bool isRootLike(const QString &abs)
{
    const QString c = QDir::cleanPath(QDir::fromNativeSeparators(abs));
    if (c.isEmpty() || c == QLatin1String("/"))
        return true;
    // "C:" / "C:/" and "//server/share"
    if (c.size() <= 3 && c.size() >= 2 && c.at(1) == QLatin1Char(':'))
        return true;
    if (c.startsWith(QLatin1String("//")) && c.mid(2).count(QLatin1Char('/')) <= 1)
        return true;
    return false;
}

bool exists(const QFileInfo &fi)
{
    return fi.exists() || fi.isSymLink();
}

// a name inside dir that is not taken yet: "name", "name (1)", ... (the extension stays last)
QString freeName(const QDir &dir, const QString &name)
{
    if (!QFileInfo::exists(dir.filePath(name)) && !QFileInfo(dir.filePath(name)).isSymLink())
        return name;
    const QFileInfo fi(name);
    const QString base = fi.completeBaseName().isEmpty() ? name : fi.completeBaseName();
    const QString ext = fi.completeBaseName().isEmpty() || fi.suffix().isEmpty() ? QString() : QLatin1Char('.') + fi.suffix();
    for (int n = 1; n < 100000; ++n) {
        const QString c = QStringLiteral("%1 (%2)%3").arg(base).arg(n).arg(ext);
        if (!QFileInfo::exists(dir.filePath(c)) && !QFileInfo(dir.filePath(c)).isSymLink())
            return c;
    }
    return QString();
}

// copy a file (same contents and modification time); folders are not copied
bool copyFile(const QString &from, const QString &to)
{
    if (!QFile::copy(from, to))
        return false;
    QFile f(to);
    if (f.open(QIODevice::ReadWrite))
        f.setFileTime(QFileInfo(from).lastModified(), QFileDevice::FileModificationTime);
    return true;
}

// the seam: $MX_OS_ROOT/trash/files/<name> + $MX_OS_ROOT/trash/info/<name>.trashinfo
bool seamMove(const QString &path, QString *where, QString *error)
{
    const QDir root(seamDir());
    const QDir files(root.filePath(QStringLiteral("files")));
    const QDir info(root.filePath(QStringLiteral("info")));
    if (!QDir().mkpath(files.path()) || !QDir().mkpath(info.path())) {
        if (error) *error = QStringLiteral("cannot create the trash folder %1").arg(QDir::toNativeSeparators(root.path()));
        return false;
    }
    const QFileInfo fi(path);
    const QString name = freeName(files, fi.fileName().isEmpty() ? QStringLiteral("unnamed") : fi.fileName());
    if (name.isEmpty()) {
        if (error) *error = QStringLiteral("no free name in the trash");
        return false;
    }
    const QString dest = files.filePath(name);
    // the info file first (the FreeDesktop order), removed again when the move fails
    const QString infoPath = info.filePath(name + QStringLiteral(".trashinfo"));
    {
        QSaveFile f(infoPath);
        if (!f.open(QIODevice::WriteOnly)) {
            if (error) *error = QStringLiteral("cannot write %1").arg(QDir::toNativeSeparators(infoPath));
            return false;
        }
        const QByteArray body = "[Trash Info]\nPath=" + QUrl::toPercentEncoding(fi.absoluteFilePath(), "/") +
                                "\nDeletionDate=" + QDateTime::currentDateTime().toString(Qt::ISODate).toUtf8() + "\n";
        f.write(body);
        if (!f.commit()) {
            if (error) *error = QStringLiteral("cannot write %1").arg(QDir::toNativeSeparators(infoPath));
            return false;
        }
    }
    bool ok = QDir().rename(fi.absoluteFilePath(), dest);
    if (!ok && fi.isFile() && !fi.isSymLink()) {   // another volume: copy, then remove the original
        ok = copyFile(fi.absoluteFilePath(), dest);
        if (ok && !QFile::remove(fi.absoluteFilePath())) {
            QFile::remove(dest);
            ok = false;
        }
    }
    if (!ok) {
        QFile::remove(infoPath);
        if (error) *error = QStringLiteral("cannot move %1 to the trash").arg(QDir::toNativeSeparators(fi.absoluteFilePath()));
        return false;
    }
    if (where) *where = dest;
    return true;
}

#if !defined(Q_OS_WIN)
bool networkFs(const QByteArray &type)
{
    static const char *const kinds[] = {"nfs", "nfs4", "cifs", "smb3", "smbfs", "fuse.sshfs", "sshfs", "9p", "afs",
                                        "ceph", "glusterfs", "fuse.glusterfs", "davfs", "fuse.rclone", "fuse.davfs2"};
    for (const char *k : kinds)
        if (type == k)
            return true;
    return false;
}

QString existingAncestor(QString p)
{
    p = QDir::cleanPath(p);
    while (!p.isEmpty() && !QFileInfo::exists(p)) {
        const QString up = QFileInfo(p).absolutePath();
        if (up == p)
            break;
        p = up;
    }
    return p;
}
#endif
} // namespace

bool redirected()
{
    return !osRoot().isEmpty();
}

QString seamDir()
{
    const QString r = osRoot();
    return r.isEmpty() ? QString() : QDir(QDir::fromNativeSeparators(r)).filePath(QStringLiteral("trash"));
}

QString placeName()
{
#ifdef Q_OS_WIN
    return QStringLiteral("Recycle Bin");
#else
    return QStringLiteral("trash");
#endif
}

bool available(const QString &path)
{
    if (path.isEmpty())
        return false;
    if (redirected())
        return qEnvironmentVariable("MX_TRASH").toLower() != QLatin1String("none");
#ifdef Q_OS_WIN
    // the Recycle Bin exists on fixed drives only (network shares and removable drives have none)
    const QString abs = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    if (abs.startsWith(QLatin1String("\\\\")))
        return false;
    QString root = QStorageInfo(QFileInfo(path).absolutePath()).rootPath();
    if (root.isEmpty())
        root = abs.left(3);
    root = QDir::toNativeSeparators(root);
    if (!root.endsWith(QLatin1Char('\\')))
        root += QLatin1Char('\\');
    return GetDriveTypeW(reinterpret_cast<LPCWSTR>(root.utf16())) == DRIVE_FIXED;
#else
    const QString abs = QFileInfo(path).absoluteFilePath();
    const QStorageInfo st(existingAncestor(QFileInfo(abs).absolutePath()));
    if (!st.isValid())
        return false;
    if (networkFs(st.fileSystemType()))
        return false;
#if defined(Q_OS_MACOS)
    return true;
#else
    // the FreeDesktop home trash covers the volume of $XDG_DATA_HOME; another volume needs its own
    // $topdir/.Trash/<uid> or $topdir/.Trash-<uid> (QFile::moveToTrash creates the latter)
    QString dataHome = qEnvironmentVariable("XDG_DATA_HOME");
    if (dataHome.isEmpty())
        dataHome = QDir::homePath() + QStringLiteral("/.local/share");
    const QStorageInfo home(existingAncestor(dataHome));
    if (home.isValid() && home.device() == st.device() && home.rootPath() == st.rootPath())
        return true;
    const QString top = st.rootPath();
    const QString uid = QString::number(::getuid());
    const QFileInfo shared(top + QStringLiteral("/.Trash/") + uid);
    if (shared.isDir() && shared.isWritable())
        return true;
    const QFileInfo own(top + QStringLiteral("/.Trash-") + uid);
    if (own.exists())
        return own.isDir() && own.isWritable() && !own.isSymLink();
    return QFileInfo(top).isWritable();
#endif
#endif
}

bool availableForAll(const QStringList &paths)
{
    for (const QString &p : paths)
        if (!available(p))
            return false;
    return true;
}

bool moveToTrash(const QString &path, QString *where, QString *error)
{
    const QFileInfo fi(path);
    if (path.isEmpty() || fi.isRelative() || isRootLike(fi.absoluteFilePath())) {
        if (error) *error = QStringLiteral("refused: %1").arg(path);
        return false;
    }
    if (!exists(fi)) {
        if (error) *error = QStringLiteral("%1 does not exist").arg(QDir::toNativeSeparators(path));
        return false;
    }
    if (redirected())
        return seamMove(fi.absoluteFilePath(), where, error);
    QString at;
    QFile f(fi.absoluteFilePath());
    if (!f.moveToTrash()) {
        if (error) *error = f.errorString().isEmpty() ? QStringLiteral("cannot move %1 to the %2").arg(QDir::toNativeSeparators(path), placeName())
                                                      : f.errorString();
        return false;
    }
    if (where) *where = f.fileName();
    return true;
}

bool removeForGood(const QString &path, QString *error)
{
    const QFileInfo fi(path);
    if (path.isEmpty() || fi.isRelative() || isRootLike(fi.absoluteFilePath())) {
        if (error) *error = QStringLiteral("refused: %1").arg(path);
        return false;
    }
    if (!exists(fi))
        return true;
    bool ok;
    if (fi.isDir() && !fi.isSymLink())
        ok = QDir(fi.absoluteFilePath()).removeRecursively();   // links inside are removed, never followed
    else
        ok = QFile::remove(fi.absoluteFilePath());
    if (!ok && error)
        *error = QStringLiteral("cannot delete %1").arg(QDir::toNativeSeparators(path));
    return ok;
}

Result dispose(const QStringList &paths, bool allowForGood)
{
    Result r;
    for (const QString &p : paths) {
        const QFileInfo fi(p);
        if (p.isEmpty() || fi.isRelative() || isRootLike(fi.absoluteFilePath())) {
            r.failed << p;
            r.errors << QStringLiteral("refused: %1").arg(p);
            continue;
        }
        if (!exists(fi))
            continue;
        QString err;
        if (available(p)) {
            if (moveToTrash(p, nullptr, &err))
                r.trashed << p;
            else {
                r.failed << p;   // never "for good" when the user was told "trash"
                r.errors << err;
            }
        } else if (allowForGood) {
            if (removeForGood(p, &err))
                r.deleted << p;
            else {
                r.failed << p;
                r.errors << err;
            }
        } else {
            r.failed << p;
            r.errors << QStringLiteral("%1 cannot go to the %2 and was not deleted").arg(QDir::toNativeSeparators(p), placeName());
        }
    }
    return r;
}

qint64 sizeOnDisk(const QString &path, int *files)
{
    const QFileInfo fi(path);
    int n = 0;
    qint64 total = 0;
    if (fi.isSymLink()) {
        n = 1;
    } else if (fi.isDir()) {
        QDirIterator it(fi.absoluteFilePath(), QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QFileInfo f = it.nextFileInfo();
            ++n;
            if (!f.isSymLink())
                total += f.size();
        }
    } else if (fi.exists()) {
        n = 1;
        total = fi.size();
    }
    if (files) *files = n;
    return total;
}

} // namespace mx::trash
