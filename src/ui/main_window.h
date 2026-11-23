// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: the main window (UI_SPEC §1.4-§1.8): row A (news bar + Ignored / Settings / Help),
// row B (the seven page buttons), the page area with the docked Hotlist pane at the right, and the
// status bar with the Float tab. Geometry is absolute, exactly as measured on the references:
// rows A and B are a fixed 682-px band that does not stretch.
//
// Follow-up workers: the stubs marked [ui-b] / [ui-c] below are where the remaining windows plug in.
#pragma once

#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QSet>
#include <QtCore/QPointer>
#include <QtWidgets/QWidget>

#include <functional>

#include "mx/types.h"
#include "ui/kit/icons.h"

class QMenu;
class QMessageBox;
class QSplitter;
class QStackedWidget;
class QTimer;

namespace mx {
class Core;
}

namespace mx::ui {

class HotlistPane;
class NewsBar;
class Page;
class StatusBar;
class ThinButton;
class FloatWindow;

class MainWindow : public QWidget {
    Q_OBJECT
public:
    explicit MainWindow(mx::Core *core, QWidget *parent = nullptr);
    ~MainWindow() override;

    mx::Core *core() const { return m_core; }

    // ---- pages
    static QStringList pageIds();              // networks shared chat search transfers bandwidth
    Page *page(const QString &id) const;
    QString currentPageId() const { return m_current; }
    void showPage(const QString &id);          // also accepts "hotlist" (toggles the pane on)
    ThinButton *pageButton(const QString &id) const;

    // ---- hotlist pane (a docked side pane, not a page)
    void setHotlistVisible(bool on);
    bool isHotlistVisible() const;
    HotlistPane *hotlistPane() const { return m_hotlist; }

    // ---- float / dock
    void floatPage(const QString &id);
    void dockPage(const QString &id);
    bool isFloating(const QString &id) const { return m_floats.contains(id); }

    // ---- chrome access
    StatusBar *statusBar() const { return m_status; }
    NewsBar *newsBar() const { return m_news; }
    QString titleText() const;
    QString stateText() const;                 // the "<Online P=5 S=12 spm=4284>" part (NetworkService::summary)
    // Ping a user (hotlist / chat / transfer source "Ping"); the answer shows in the status bar.
    void pingUser(const mx::UserRef &u);
    // A transient status-bar note (8 s), after the current page's fields.
    void showNotice(const QString &text);
    // [qa-1] a download action that produced no Transfers row: status-bar notice + an information box
    // (objectName "downloadProblem", lines collected while it is open)
    void reportDownloadProblem(const QString &what, const QString &reason);
    // [ui-e] a download completed: status-bar note + tray notification (ui/notifyDownloadDone)
    void notifyDownloadDone(const mx::TransferInfo &t);
    // [ui-g] a Hotlist user came online: status-bar note + tray notification (hotlist/notifyOnline)
    void notifyHotlistOnline(const QString &name);
    static std::function<void(const QString &name)> s_notifyHook;   // tests: replaces the tray message
    // [ui-h] flash the taskbar button (misc/flashTaskbar) unless OctoMX is the active window
    void flashTaskbar();
    void applyAlwaysOnTop();                                         // [ui-h] misc/alwaysOnTop
    static std::function<void()> s_flashHook;                        // tests: replaces QApplication::alert
    QString notice() const { return m_notice; }

    // ---- actions used by pages (stubs where another worker will plug in a window)
    void startSearch(const QString &text, const QString &text2 = QString());
    void addLinks(const QStringList &uris);    // magnet:/ed2k:/http(s):/.torrent -> downloads
    void openHelp();                           // About / help window
    // [post-1] the Help button's menu (About OctoMX... / Check for Updates...), the Settings button's
    // right-click menu (Settings... / Color Theme ▸) and Help ▸ Check for Updates...
    QMenu *helpMenu();
    QMenu *settingsButtonMenu();
    void checkForUpdates();
    virtual void openSettings(const QString &settingsPage = QString());  // [ui-c] Settings dialog
    virtual void openIgnored();                                          // [ui-b] Ignored Users window
    virtual void openBrowse(const mx::UserRef &u);                       // [ui-b] Browse window
    virtual void openWhois(const mx::UserRef &u);                        // [ui-b] Whois window
    virtual void openPrivateMessage(const mx::UserRef &u);               // [ui-b] private message window
    virtual void openChat(const mx::UserRef &u);                         // [ui-b] chat page / room with user
    virtual void setupTray();                                            // [ui-c] tray icon + menu
    // Called on the window's close button. [ui-c] replaces with the close prompt / tray logic.
    virtual bool handleCloseRequest();

    // persist geometry / page / splitters now (also done on close)
    void saveState();

signals:
    void pageChanged(const QString &id);
    void quitRequested();                      // the app should shut down (core->shutdown())
    void linksAdded(const QStringList &uris, const QStringList &ids);

protected:
    void resizeEvent(QResizeEvent *) override;
    void closeEvent(QCloseEvent *) override;
    void dragEnterEvent(QDragEnterEvent *) override;
    void dropEvent(QDropEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    bool event(QEvent *) override;   // [ui-g] status tips of hovered menu entries go to the status bar

private:
    void createChrome();
    void createPages();
    void addPage(Page *p, IconId icon, const QString &buttonText);
    void relayout();
    void updateTitle();
    void updateStatus();
    void updateNews();
    void updatePageButtons();
    void restoreState();

    mx::Core *m_core;
    NewsBar *m_news = nullptr;
    ThinButton *m_ignoredBtn = nullptr;
    ThinButton *m_settingsBtn = nullptr;
    ThinButton *m_helpBtn = nullptr;
    QMenu *m_helpMenu = nullptr;          // [post-1]
    QMenu *m_settingsMenu = nullptr;      // [post-1]
    QList<ThinButton *> m_pageBtnList;
    QHash<QString, ThinButton *> m_pageBtns;
    QHash<QString, Page *> m_pages;
    QHash<QString, QWidget *> m_slots;          // stack slot per page (holds the page unless floating)
    QHash<QString, FloatWindow *> m_floats;
    QSplitter *m_split = nullptr;
    QStackedWidget *m_stack = nullptr;
    HotlistPane *m_hotlist = nullptr;
    StatusBar *m_status = nullptr;
    QString m_current;
    QTimer *m_titleTimer = nullptr;
    int m_hotlistWidth = 200;
    bool m_restoring = false;
    QString m_notice;
    QPointer<QMessageBox> m_problemBox;   // [qa-1] reportDownloadProblem
    QStringList m_problemLines;
    QSet<QString> m_completedIds;               // [ui-e] downloads already announced as complete
    QElapsedTimer m_startedAt;                  // [ui-h] downloads restored at start do not switch pages
    QSet<QString> m_pinging;                    // users we asked NetworkService::pingUser about
    mutable QElapsedTimer m_onlineSince;        // secondary connection: "<Online m:ss>"
};

// A page floated into its own top-level window (Float tab -> "Dock").
class FloatWindow : public QWidget {
    Q_OBJECT
public:
    FloatWindow(Page *page, QWidget *parent = nullptr);
    Page *page() const { return m_page; }
    Page *takePage();
    StatusBar *statusBar() const { return m_status; }
    // [ui-d] position and size persist per page (ui/dialogs/float_<page>); false = none stored
    bool restorePlacement();
    void savePlacement();

signals:
    void dockRequested();

protected:
    void closeEvent(QCloseEvent *) override;
    void moveEvent(QMoveEvent *) override;
    void resizeEvent(QResizeEvent *) override;

private:
    void updateStatus();
    Page *m_page;
    StatusBar *m_status;
    QString m_pageId;
    bool m_placed = false;
};

} // namespace mx::ui
