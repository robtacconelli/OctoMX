// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI [ui-j]: the two questions that may delete a user's files.
//
// RemoveDownloadDialog: a download (or several) leaves the Transfers list: "Remove from the list,
// keep the file" / "Remove from the list and delete the file" / Cancel (the default button), with
// "Do not ask again". It names the file (or "these 5 downloads" with what they have on disk) and
// says what deleting does: to the trash / Recycle Bin, or deleted for good where there is none.
//
// DeleteFilesDialog: Shared Files ▸ Delete File... / Delete Folder...: names the file, or the folder
// with its number of files and their size, says that it leaves the share AND the disk; a shared
// root folder is named in bold. "Delete" / Cancel (the default button).
//
// Both are classic message boxes (icon, text, buttons right-aligned, Esc = Cancel). The texts are
// built by free functions so tests read exactly what the user reads.
#pragma once

#include <functional>

#include <QtWidgets/QDialog>

#include "mx/types.h"

class QCheckBox;
class QPushButton;

namespace mx::ui {

class WrapLabel;

// ------------------------------------------------------------------------------- downloads
struct RemoveItem {
    TransferInfo t;
    DownloadDiskInfo disk;
};

struct RemoveTexts {
    QString title;
    QString question;          // "Remove "x" from the download list?"
    QString list;              // multi-selection: one line per download (first 6, "and 3 more")
    QString details;           // what is on the disk now
    QString keepLine;          // what "keep" does
    QString deleteLine;        // what "delete" does (shown in bold)
    QString warning;           // in-place file / deleted for good (bold), may be empty
    QString keptNote;          // files that stay in every case
    QString keepButton, deleteButton;
    bool canDelete = true;     // something of it is on the disk
    bool forGood = false;      // something would be deleted for good (no trash)
    qint64 bytes = 0;
};
RemoveTexts removeTexts(const QList<RemoveItem> &items);

class RemoveDownloadDialog : public QDialog {
    Q_OBJECT
public:
    enum Choice { Cancel = 0, Keep, Delete };
    RemoveDownloadDialog(const QList<RemoveItem> &items, QWidget *parent = nullptr);
    Choice choice() const { return m_choice; }
    bool dontAskAgain() const;
    const RemoveTexts &texts() const { return m_texts; }
    QPushButton *keepButton() const { return m_keep; }
    QPushButton *deleteButton() const { return m_delete; }
    QPushButton *cancelButton() const { return m_cancel; }
    QCheckBox *dontAskBox() const { return m_dont; }
    // tests: instead of exec(); return the Choice (and tick the box through dontAskBox())
    static std::function<int(RemoveDownloadDialog *)> s_execHook;
    Choice ask();   // exec() or the hook

private:
    RemoveTexts m_texts;
    Choice m_choice = Cancel;
    QPushButton *m_keep = nullptr;
    QPushButton *m_delete = nullptr;
    QPushButton *m_cancel = nullptr;
    QCheckBox *m_dont = nullptr;
};

// ------------------------------------------------------------------------------- shared files
struct DeleteTexts {
    QString title;
    QString question;          // "Delete the file "x" from your disk?"
    QString target;            // the path (bold for a shared root folder)
    QString details;           // "37 files, 1.2 GB"
    QString effect;            // "It leaves OctoMX's shared files and the disk: ..."
    QString warning;           // a shared root: removes the share AND deletes the content; for good
    QString hint;              // "To stop sharing without deleting, use Unshare Folder."
    QString button;            // "Delete File" / "Delete Folder" / "Delete 3 Items"
    bool boldTarget = false;
    bool forGood = false;
};

class DeleteFilesDialog : public QDialog {
    Q_OBJECT
public:
    DeleteFilesDialog(const DeleteTexts &texts, QWidget *parent = nullptr);
    const DeleteTexts &texts() const { return m_texts; }
    QPushButton *deleteButton() const { return m_delete; }
    QPushButton *cancelButton() const { return m_cancel; }
    // tests: instead of exec(); return QDialog::Accepted to delete
    static std::function<int(DeleteFilesDialog *)> s_execHook;
    bool ask();

private:
    DeleteTexts m_texts;
    QPushButton *m_delete = nullptr;
    QPushButton *m_cancel = nullptr;
};

} // namespace mx::ui
