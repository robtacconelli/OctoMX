// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: [dl-1] preview of a partly downloaded audio / video file.
//
//   PreviewWithDialog   the first-time player choice ("Preview With"): the system's default player or a
//                       program (players found on this computer offered, Browse...), "Remember my choice"
//                       (launch/previewPlayer + launch/previewProgram; changeable in Settings, File
//                       Launching, Preview Player)
//   PreviewFileDialog   a multi-file torrent: which media file to preview, with its progress
//   previewui::menuEntry / run   what the Transfers menu shows for a download, and the whole flow
//                       (chooser, TransferService::preparePreview, player choice, launcher::previewFile)
#pragma once

#include <QtWidgets/QDialog>

#include <functional>

#include "mx/types.h"

class QCheckBox;
class QComboBox;
class QRadioButton;
class QTreeWidget;

namespace mx {
class Core;
class Settings;
}

namespace mx::ui {

class PreviewWithDialog : public QDialog {
    Q_OBJECT
public:
    PreviewWithDialog(const QString &fileName, const QString &choice, const QString &program, QWidget *parent = nullptr);
    QString choice() const;          // "system" | "program"
    QString program() const;
    bool remember() const;
    void setChoice(const QString &choice, const QString &program = QString());   // tests
    void setRemember(bool on);
    void accept() override;
    // tests: replace exec() (return QDialog::Accepted / Rejected after setting the widgets) and the
    // file dialog of Browse...
    static std::function<int(PreviewWithDialog *)> s_execHook;
    static std::function<QString(QWidget *parent)> s_browseHook;

private:
    QRadioButton *m_system;
    QRadioButton *m_program;
    QComboBox *m_path;
    QCheckBox *m_remember;
};

class PreviewFileDialog : public QDialog {
    Q_OBJECT
public:
    PreviewFileDialog(const QString &bundleName, const QList<mx::PreviewFile> &files, QWidget *parent = nullptr);
    int chosenIndex() const;         // PreviewFile::index of the selected row, -2 = none
    int rowCount() const;
    void selectRow(int row);         // tests
    static std::function<int(PreviewFileDialog *)> s_execHook;

private:
    QTreeWidget *m_tree;
    QList<mx::PreviewFile> m_files;
};

namespace previewui {

struct MenuEntry {
    bool    show = false;        // a media download: the entry is there
    bool    enabled = false;
    bool    ready = false;       // Preview can start now (else the entry offers the order flags)
    QString text;                // "Preview", "Preview...", "Preview: needs the beginning of the file"
    QString tip;
};
MenuEntry menuEntry(mx::Core *core, const mx::TransferInfo &t);

// Ask for the player when none was chosen yet (or `forceAsk`). false = cancelled.
bool choosePlayer(QWidget *parent, mx::Settings *settings, const QString &fileName, QString *choice, QString *program,
                  bool forceAsk = false);
// The whole flow for one download (fileIndex -2 = let the user choose in a bundle). false + *why
// when nothing was started for a reason the user should read; false + empty *why = cancelled.
bool run(QWidget *parent, mx::Core *core, const mx::TransferInfo &t, int fileIndex, QString *why);

} // namespace previewui
} // namespace mx::ui
