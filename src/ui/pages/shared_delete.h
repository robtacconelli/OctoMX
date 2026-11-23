// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI [ui-j]: Shared Files ▸ Delete File... / Delete Folder...: what may be deleted, and the
// question. Pure functions over a Context (the page fills it from the core), so every refusal is
// tested without a window.
//
// Never deleted (the whole selection is refused, with the reason):
//   * anything outside the shared folders, or reached through a link that leads out of them;
//   * the incoming folder itself (files in it may go), a folder that contains it; the incomplete
//     folder, anything in it or a folder that contains it;
//   * the profile folder or the program folder, anything in them or a folder that contains them;
//   * the data of a download that is still in the Transfers list and not finished, or a torrent
//     that is seeding (remove it on the Transfers page: it can delete its file too);
//   * a file being uploaded right now (cancel that upload first, or wait until it ends).
#pragma once

#include <QtCore/QList>
#include <QtCore/QStringList>

#include "ui/dialogs/remove_dialogs.h"

namespace mx::ui::shdel {

struct Item {
    QString path;
    bool folder = false;
};

struct Busy {               // data a download or an upload holds right now
    QString path;           // a file or folder (empty: matched by name and size)
    QString name;           // for the message: the download's name / the uploaded file
    qint64 size = -1;       // uploads without a path: the file's size
    QString who;            // uploads: the user
    bool upload = false;
};

struct Context {
    QStringList roots;          // the shared folders (the ones added with Share Folder)
    QString incoming, incomplete, profile, program;
    QList<Busy> busy;           // unfinished downloads, seeding torrents, active uploads
};

struct Plan {
    QStringList paths;          // what goes (nothing below another one of them)
    QStringList roots;          // the shared roots among them: their share is removed too
    int files = 0;
    qint64 bytes = 0;
    bool trash = true;          // every path can go to the trash / Recycle Bin
    QString refusal;            // not empty: nothing is deleted, this says why
};

Plan plan(const QList<Item> &items, const Context &ctx);
DeleteTexts texts(const QList<Item> &items, const Plan &p);

} // namespace mx::ui::shdel
