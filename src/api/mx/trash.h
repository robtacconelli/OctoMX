// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX: moving files to the system trash / Recycle Bin, with a test seam [ui-j].
//
// The only place where OctoMX deletes a user's file (a download's data on request, a shared file or
// folder on request). Everything goes through dispose():
//   * the trash / Recycle Bin where the path's volume has one (so a mistake can be undone);
//   * deleted for good only where it has none AND the caller says the user was told so.
//
// Test seam: when MX_OS_ROOT is set (toolchain/env.sh sets it in every development shell; tests and
// scripted runs set it), the "trash" is the folder $MX_OS_ROOT/trash (FreeDesktop layout: files/ +
// info/<name>.trashinfo with the original path) and the real trash or Recycle Bin is never reached.
// MX_TRASH=none, honoured only together with MX_OS_ROOT, simulates a volume without a trash.
// Without MX_OS_ROOT: Linux uses the FreeDesktop trash through QFile::moveToTrash (none on network
// file systems), Windows the Recycle Bin through QFile::moveToTrash (fixed drives only: network and
// removable drives have no Recycle Bin).
#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>

namespace mx::trash {

// true: the seam under MX_OS_ROOT is used (never the real trash)
bool redirected();
// the seam folder ($MX_OS_ROOT/trash), empty when not redirected
QString seamDir();
// "Recycle Bin" on Windows, "trash" elsewhere (for sentences: "moved to the Recycle Bin")
QString placeName();
// Can `path` go to the trash / Recycle Bin? false = removing it would delete it for good.
bool available(const QString &path);
// true when every path can go to the trash
bool availableForAll(const QStringList &paths);

// Move one file or folder (with everything in it) to the trash. *where = its place in the trash
// (when known). Never deletes for good: false + *error when it cannot be moved.
bool moveToTrash(const QString &path, QString *where = nullptr, QString *error = nullptr);
// Delete one file or folder for good (a folder with everything in it; links are removed, never
// followed). Only for paths whose volume has no trash, after the user was told.
bool removeForGood(const QString &path, QString *error = nullptr);

struct Result {
    QStringList trashed;     // moved to the trash
    QStringList deleted;     // deleted for good
    QStringList failed;      // still there (could not be moved / deleted, or not allowed for good)
    QStringList errors;      // one line per failed path
    bool ok() const { return failed.isEmpty(); }
};
// Each path to the trash where available; where not, deleted for good only when allowForGood
// (the question said "deleted for good"), else it stays and is reported as failed. A path that is
// gone already counts as neither. Refuses "/", drive roots and empty or relative paths.
Result dispose(const QStringList &paths, bool allowForGood);

// Bytes of a file, or of every file below a folder (links not followed); *files = their number.
qint64 sizeOnDisk(const QString &path, int *files = nullptr);

} // namespace mx::trash
