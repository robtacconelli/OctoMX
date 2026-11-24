// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: the Browse window (UI_SPEC §5.2, reference web/browse_window_354b4.png): another
// user's shared files, fetched with mx::UserService::browse().
//
//   [ Refresh ][ Send Message ][ Whois ][ Add To Hotlist ][ Show Full Paths ]
//   [ Download ][ Manually Resume... ][Auto Find Sources - 15m][Auto Enter Queue - 10]
//   legend (same three colours as Search)
//   black list: folders with [+] (or full paths), File / Bytes / Time / Bitrate / Freq
//   status: "Downloading list..." / "Finished downloading list> 2320 files (665,937 megs) total"
//
// One window per user (MainWindow::openBrowse raises an existing one). Deleted on close; an
// unfinished request is cancelled. Geometry and columns persist.
#pragma once

#include <functional>

#include <QtCore/QSet>
#include <QtWidgets/QWidget>

#include "mx/types.h"

class QTimer;

namespace mx {
class Core;
}

namespace mx::ui {

class BrowseModel;
class BrowseProxy;
class LegendItem;
class MainWindow;
class MenuButton;
class MxListView;
class PushButton;
class StatusBar;

class BrowseWindow : public QWidget {
    Q_OBJECT
public:
    BrowseWindow(mx::Core *core, MainWindow *mw, const mx::UserRef &user, QWidget *parent = nullptr);
    ~BrowseWindow() override;

    mx::UserRef user() const { return m_user; }
    void refresh();                         // (re)request the list
    void downloadSelected();
    void downloadEntries(const QList<int> &entries) { download(entries); }   // [set-1] entries of model()
    bool isLoading() const { return m_request != 0; }
    MxListView *list() const { return m_list; }
    BrowseModel *model() const { return m_model; }
    StatusBar *statusBar() const { return m_status; }
    void showContextMenu(const QModelIndex &viewIndex, const QPoint &globalPos);
    static QString windowTitleFor(const mx::UserRef &u);
    // build the SearchHit a download of `e` hands to TransferService::downloadHit
    static mx::SearchHit hitFor(const mx::UserRef &u, const mx::BrowseEntry &e);
    // [set-1] the dangerous file type question (tests answer it): QMessageBox::Yes downloads them
    static std::function<int(const QString &text)> s_askHook;

protected:
    void resizeEvent(QResizeEvent *) override;
    void closeEvent(QCloseEvent *) override;

private:
    void onResults(quint32 id, const mx::UserRef &u, const QList<mx::BrowseEntry> &files, bool finished);
    void onFailed(quint32 id, const mx::UserRef &u, const QString &reason);
    void updateStatus();
    void rebuildLocalIndex();
    void download(const QList<int> &entries);

    mx::Core *m_core;
    MainWindow *m_main;
    mx::UserRef m_user;
    quint32 m_request = 0;
    QString m_failure;
    QList<PushButton *> m_row1, m_row2;
    MenuButton *m_afs, *m_aeq;
    QList<LegendItem *> m_legend;
    MxListView *m_list;
    BrowseModel *m_model;
    BrowseProxy *m_proxy;
    StatusBar *m_status;
    QTimer *m_localTimer;
    QSet<QString> m_activeKeys, m_incompleteKeys;
};

} // namespace mx::ui
