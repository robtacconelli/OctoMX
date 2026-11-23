// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: manual resume.
//
// FileDownloadDialog: "File Download" (UI_SPEC §5.7, web/dialog_file_download_manual_resume.png),
// opened by Search ▸ "Manually Resume..." / "Manually Download": save to a new file, browse for the
// file to resume from, or resume from one of the incomplete files (TransferService::partialFiles()).
//
// LoadIncompleteDialog: Transfers ▸ "Load Incomplete": the incomplete files on disk that no download
// uses; the chosen one is resumed from a search for its name (or a pasted link of the same file).
#pragma once

#include <QtWidgets/QDialog>

#include <functional>

#include "mx/types.h"

class QListWidget;
class QRadioButton;
class QLineEdit;

namespace mx::ui {

class FileDownloadDialog : public QDialog {
    Q_OBJECT
public:
    FileDownloadDialog(const mx::SearchHit &hit, const QList<mx::PartialFile> &partials, const QString &incompleteDir,
                       QWidget *parent = nullptr);

    // empty = "Save to a new file"; otherwise the local file to resume from
    QString chosenFile() const { return m_chosen; }
    int candidateCount() const;
    // tests: choose radio 0 (new) / 1 (browse) / 2 (list) and a list row
    void choose(int option, int row = 0);
    // tests: replaces the file dialog of "Manually browse..."
    static std::function<QString(QWidget *parent, const QString &dir)> s_browseHook;

    void accept() override;

private:
    QRadioButton *m_new;
    QRadioButton *m_browse;
    QRadioButton *m_fromList;
    QListWidget *m_list;
    QString m_dir;
    QString m_chosen;
};

class LoadIncompleteDialog : public QDialog {
    Q_OBJECT
public:
    LoadIncompleteDialog(const QList<mx::PartialFile> &partials, const QString &incompleteDir, QWidget *parent = nullptr);
    QString chosenFile() const;          // the selected incomplete file
    QString chosenName() const;          // [ui-j] its name (a kept download: the download's name)
    QString link() const;                // optional magnet:/ed2k:// link of the same file
    int rowCount() const;
    void selectRow(int row);
    void setLink(const QString &l);

private:
    QListWidget *m_list;
    QLineEdit *m_link;
};

} // namespace mx::ui
