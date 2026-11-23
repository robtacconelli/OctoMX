// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: the Shared Files page (UI_SPEC §3.2.1 / §8.3, references
// web/shared_files_page_columns_353.png, web/shared_files_page_353.png,
// web/menu_shared_files_folder_353.png).
//
// Row C: five thin buttons in the 682-px band, Share Folder / Unshare Folder / Refresh /
// Shared File Types / Scan For Media. Below, the black folder tree (SharedFilesModel): File Path /
// Size / Bitrate / Time / Freq. Status bar: "<n> files shared (<megs> megs)" and the hasher /
// rescan progress. Everything goes through mx::LibraryService, which persists the folders.
#pragma once

#include <QtCore/QSet>

#include "ui/pages/page.h"

class QTimer;

namespace mx::ui {

class MxListView;
class SharedFilesModel;
class SharedProxy;
class ThinButton;

class SharedPage : public Page {
    Q_OBJECT
public:
    SharedPage(mx::Core *core, MainWindow *mw, QWidget *parent = nullptr);

    QStringList statusFields() const override;
    void saveState() override;
    bool scriptCommand(const QString &verb, const QStringList &args, QString *error) override;

    MxListView *list() const { return m_list; }
    SharedFilesModel *model() const { return m_model; }

    void shareFolder();                 // "Share Folder": Browse for Folder dialog
    void showUnshareMenu();             // "Unshare Folder": menu of the shared folders
    void showFileTypes();
    void scanForMedia();
    void showContextMenu(const QModelIndex &viewIndex, const QPoint &globalPos);
    void reload();                      // rebuild the tree from the service now

protected:
    void resizeEvent(QResizeEvent *) override;

private:
    void launch(bool open, const QString &path);   // [ui-e]
    void scheduleReload();
    void unshareFolderKey(const QString &key);
    void activate(const QModelIndex &viewIndex);
    QList<ThinButton *> m_buttons;
    MxListView *m_list;
    SharedFilesModel *m_model;
    SharedProxy *m_proxy;
    QTimer *m_reloadTimer;
    QString m_scan;                     // "Hashing 12 of 340: foo.mp3"
};

} // namespace mx::ui
