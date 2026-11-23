// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/pages/shared_delete.h"

#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFileInfo>
#include <QtGui/QFontMetrics>

#include <algorithm>

#include "mx/trash.h"
#include "ui/kit/format.h"
#include "ui/kit/scale.h"
#include "ui/kit/theme.h"

namespace mx::ui::shdel {

namespace {
#ifdef Q_OS_WIN
constexpr Qt::CaseSensitivity kCase = Qt::CaseInsensitive;
#else
constexpr Qt::CaseSensitivity kCase = Qt::CaseSensitive;
#endif

QString norm(const QString &p)
{
    if (p.isEmpty())
        return p;
    return QDir::cleanPath(QDir::fromNativeSeparators(QFileInfo(p).absoluteFilePath()));
}

// the real place, links resolved (the deepest part that exists, plus the rest)
QString canon(const QString &p)
{
    const QString n = norm(p);
    QString head = n, tail;
    while (!head.isEmpty()) {
        const QString c = QFileInfo(head).canonicalFilePath();
        if (!c.isEmpty())
            return tail.isEmpty() ? c : QDir::cleanPath(c + QLatin1Char('/') + tail);
        const QString up = QFileInfo(head).absolutePath();
        if (up == head)
            break;
        tail = tail.isEmpty() ? QFileInfo(head).fileName() : QFileInfo(head).fileName() + QLatin1Char('/') + tail;
        head = up;
    }
    return n;
}

bool same(const QString &a, const QString &b)
{
    return !a.isEmpty() && !b.isEmpty() && a.compare(b, kCase) == 0;
}

// a == dir or a is below dir
bool under(const QString &a, const QString &dir)
{
    if (a.isEmpty() || dir.isEmpty())
        return false;
    if (same(a, dir))
        return true;
    const QString d = dir.endsWith(QLatin1Char('/')) ? dir : dir + QLatin1Char('/');
    return a.startsWith(d, kCase);
}

// both spellings: as given and with links resolved
bool underAny(const QString &a, const QString &dir)
{
    return under(norm(a), norm(dir)) || under(canon(a), canon(dir));
}

QString shown(const QString &p)
{
    return fmt::elideMiddle(QDir::toNativeSeparators(p), QFontMetrics(normalFont()), S(300));
}

QString q(const QString &p)
{
    return QStringLiteral("\"%1\"").arg(shown(p));
}

bool exists(const QString &p)
{
    const QFileInfo fi(p);
    return fi.exists() || fi.isSymLink();
}

// files of a selection item, for the uploads that are known by name and size only
QList<QPair<QString, qint64>> filesOf(const QString &path)
{
    QList<QPair<QString, qint64>> out;
    const QFileInfo fi(path);
    if (fi.isDir() && !fi.isSymLink()) {
        QDirIterator it(path, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QFileInfo f = it.nextFileInfo();
            out << qMakePair(f.fileName(), f.size());
        }
    } else if (fi.isFile()) {
        out << qMakePair(fi.fileName(), fi.size());
    }
    return out;
}
} // namespace

Plan plan(const QList<Item> &items, const Context &ctx)
{
    Plan p;
    QStringList paths;
    for (const Item &it : items) {
        if (it.path.isEmpty() || !exists(it.path))
            continue;   // gone already: nothing to do for it
        const QString n = norm(it.path);
        QString root;
        for (const QString &r : ctx.roots)
            if (under(n, norm(r)) && (root.isEmpty() || norm(r).size() > root.size()))
                root = norm(r);
        if (root.isEmpty()) {
            p.refusal = QObject::tr("%1 is not inside one of your shared folders, so OctoMX does not delete it.").arg(q(it.path));
            return p;
        }
        // a link on the way (a linked folder inside the share) must not lead the delete out of it
        const QString parent = QFileInfo(n).absolutePath();
        if (!same(n, root) && !under(canon(parent), canon(root))) {
            p.refusal = QObject::tr("%1 is reached through a link that leads outside your shared folders, so OctoMX does not "
                                    "delete it.").arg(q(it.path));
            return p;
        }
        if (!ctx.incoming.isEmpty() && (underAny(ctx.incoming, it.path))) {
            p.refusal = QObject::tr("%1 is your incoming folder, where downloads are saved, or holds it. OctoMX does not delete it; "
                                    "you can delete single files in it.").arg(q(it.path));
            return p;
        }
        const struct { QString dir; const char *why; } guarded[] = {
            {ctx.incomplete, QT_TR_NOOP("%1 is, holds or lies in the folder of unfinished downloads. OctoMX does not delete it.")},
            {ctx.profile, QT_TR_NOOP("%1 is, holds or lies in OctoMX's profile folder (its settings and lists). OctoMX does not delete it.")},
            {ctx.program, QT_TR_NOOP("%1 is, holds or lies in the folder of the OctoMX program. OctoMX does not delete it.")},
        };
        for (const auto &g : guarded)
            if (!g.dir.isEmpty() && (underAny(g.dir, it.path) || underAny(it.path, g.dir))) {
                p.refusal = QObject::tr(g.why).arg(q(it.path));
                return p;
            }
        QList<QPair<QString, qint64>> files;
        bool filesListed = false;
        for (const Busy &b : ctx.busy) {
            bool hit = false;
            if (!b.path.isEmpty()) {
                hit = underAny(b.path, it.path) || underAny(it.path, b.path);
            } else if (b.upload && !b.name.isEmpty()) {
                if (!filesListed) {
                    files = filesOf(it.path);
                    filesListed = true;
                }
                for (const auto &f : std::as_const(files))
                    hit = hit || (same(f.first, b.name) && (b.size < 0 || f.second == b.size));
            }
            if (!hit)
                continue;
            if (b.upload)
                p.refusal = QObject::tr("%1 is being sent to %2 right now. Cancel that upload on the Transfers page, or wait until it "
                                        "has finished, then delete it.")
                                .arg(q(it.path), b.who.isEmpty() ? QObject::tr("another user") : b.who);
            else
                p.refusal = QObject::tr("%1 holds the data of the download \"%2\", which is still on the Transfers page. Remove that "
                                        "download there first: it can delete its file too.")
                                .arg(q(it.path), b.name);
            return p;
        }
        paths << n;
        if (it.folder)
            for (const QString &r : ctx.roots)
                if (same(n, norm(r)) && !p.roots.contains(norm(r)))
                    p.roots << norm(r);
    }
    // nothing below another selected item
    std::sort(paths.begin(), paths.end(), [](const QString &a, const QString &b) { return a.size() < b.size(); });
    for (const QString &x : std::as_const(paths)) {
        bool inside = false;
        for (const QString &y : std::as_const(p.paths))
            inside = inside || under(x, y);
        if (!inside)
            p.paths << x;
    }
    for (const QString &x : std::as_const(p.paths)) {
        int n = 0;
        p.bytes += trash::sizeOnDisk(x, &n);
        p.files += n;
    }
    p.trash = trash::availableForAll(p.paths);
    return p;
}

DeleteTexts texts(const QList<Item> &items, const Plan &p)
{
    DeleteTexts t;
    t.forGood = !p.trash;
    const QString where = trash::placeName();
    const QString effectTrash = QObject::tr("It leaves OctoMX's shared files and the disk: it is moved to the %1, where you can restore it.").arg(where);
    const QString effectGood = QObject::tr("It leaves OctoMX's shared files and the disk: it is deleted for good. It is on a drive without "
                                           "a %1 and cannot be restored.").arg(where);
    const QString amount = p.files == 1 ? QObject::tr("1 file, %1").arg(fmt::size(p.bytes))
                                        : QObject::tr("%1 files, %2").arg(p.files).arg(fmt::size(p.bytes));
    if (p.paths.size() == 1) {
        const QString path = p.paths.first();
        const bool folder = QFileInfo(path).isDir() && !QFileInfo(path).isSymLink();
        const bool root = p.roots.contains(path);
        t.target = shown(path);
        if (!folder) {
            t.title = QObject::tr("Delete File");
            t.question = QObject::tr("Delete the file \"%1\" from your disk?").arg(QFileInfo(path).fileName());
            t.details = fmt::size(p.bytes);
            t.button = QObject::tr("Delete File");
        } else if (root) {
            t.title = QObject::tr("Delete Shared Folder");
            t.question = QObject::tr("Delete the shared folder below, with everything in it, from your disk?");
            t.target = QFileInfo(path).fileName().isEmpty() ? shown(path) : QFileInfo(path).fileName();
            t.boldTarget = true;
            t.details = QObject::tr("In %1. This is one of your shared folders. Deleting it removes the share AND deletes its content "
                                    "(%2) from the disk.").arg(shown(QFileInfo(path).absolutePath()), amount);
            t.hint = QObject::tr("To stop sharing it without deleting anything, use Unshare Folder instead.");
            t.button = QObject::tr("Delete Folder");
        } else {
            t.title = QObject::tr("Delete Folder");
            t.question = QObject::tr("Delete this folder, with everything in it, from your disk?");
            t.target = QFileInfo(path).fileName();
            t.boldTarget = true;
            t.details = QObject::tr("In %1: %2.").arg(shown(QFileInfo(path).absolutePath()), amount);
            t.hint = QObject::tr("To stop sharing it without deleting anything, use Un-share Folder instead.");
            t.button = QObject::tr("Delete Folder");
        }
        t.effect = p.trash ? effectTrash : effectGood;
    } else {
        t.title = QObject::tr("Delete Files");
        t.question = QObject::tr("Delete these %1 items from your disk?").arg(p.paths.size());
        QStringList lines;
        for (int i = 0; i < p.paths.size() && i < 6; ++i) {
            const QFileInfo fi(p.paths.at(i));
            lines << (fi.isDir() && !fi.isSymLink() ? QObject::tr("%1 (folder)").arg(shown(p.paths.at(i))) : shown(p.paths.at(i)));
        }
        if (p.paths.size() > 6)
            lines << QObject::tr("and %1 more").arg(p.paths.size() - 6);
        t.target = lines.join(QLatin1Char('\n'));
        t.details = QObject::tr("%1 in all.").arg(amount);
        t.effect = p.trash ? QObject::tr("They leave OctoMX's shared files and the disk: they are moved to the %1, where you can restore them.").arg(where)
                           : QObject::tr("They leave OctoMX's shared files and the disk: they are deleted for good. Some are on a drive "
                                         "without a %1 and cannot be restored.").arg(where);
        if (!p.roots.isEmpty()) {
            t.boldTarget = true;
            t.warning = p.roots.size() == 1 ? QObject::tr("One of them is a shared folder: its share is removed and its content deleted.")
                                            : QObject::tr("%1 of them are shared folders: their shares are removed and their content deleted.")
                                                  .arg(p.roots.size());
            t.hint = QObject::tr("To stop sharing a folder without deleting anything, use Unshare Folder instead.");
        }
        t.button = QObject::tr("Delete %1 Items").arg(p.paths.size());
    }
    if (t.forGood)
        t.warning = t.warning.isEmpty() ? QObject::tr("Deleted for good: this cannot be undone.")
                                        : t.warning + QLatin1Char(' ') + QObject::tr("Deleted for good: this cannot be undone.");
    Q_UNUSED(items)
    return t;
}

} // namespace mx::ui::shdel
