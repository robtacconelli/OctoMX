// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: tray icon, minimise / close behaviour, system menu [ui-c] (UI_SPEC §5.5, §6).
//
// One TrayController per MainWindow (created by MainWindow::setupTray(), a child of the window).
//   * tray icon with the original menu: Show / Hide (bold default) / Exit When Xfers Stop /
//     Shutdown When Xfers Stop / Reset All Windows / Color Theme ▸ ([post-1]) / Speed Limits ▸ ([bw-1]) / Auto - Startup /
//     Exit; click = show, double-click = show / hide; tooltip = window title
//   * minimise = an ordinary minimise to the taskbar, always ([post-1]: misc/minimizeToTray is read
//     but has no effect); close → misc/closeAction: Prompt (the close prompt, with "Exit when
//     downloads have finished" while downloads run and "Do not ask again") / Keep running in the
//     tray / Exit (asks first while downloads run, misc/confirmExit)
//   * the extended system menu (Hide / Exit / Exit When Transfers Finish / Shutdown When
//     Transfers Finish / Auto - Startup / Reset All Windows) on Windows; the same actions are in
//     systemMenu() everywhere (tests, script verb "system-menu")
//   * --minimized start, restore on second-instance activation (restore())
// Without a system tray (some Linux desktops, offscreen tests) the close button exits (never an
// invisible running program) and the explicit "Hide" entries minimise to the taskbar.
#pragma once

#include <QtCore/QObject>
#include <QtCore/QPointer>

#include <functional>

class QAction;
class QMenu;
class QSystemTrayIcon;
class QTimer;

namespace mx::ui {

class MainWindow;
class OsIntegration;

// ui/floatingPages: which pages are floated in their own window (saved at quit / hide, restored at
// start by the TrayController)
void saveFloatingPages(MainWindow *mw);

// [post-1] "Color Theme" submenu: Dark (classic) / Light / Follow system (exclusive, checked = the
// current ui/colorTheme). Used by the tray menu and the Settings button's right-click menu.
QMenu *createColorThemeMenu(MainWindow *mw, QWidget *parent);

class TrayController : public QObject {
    Q_OBJECT
public:
    explicit TrayController(MainWindow *mw);
    ~TrayController() override;
    static TrayController *of(const MainWindow *mw);

    MainWindow *mainWindow() const { return m_mw; }
    QMenu *trayMenu() const { return m_menu; }
    QMenu *systemMenu() const { return m_sysMenu; }
    QSystemTrayIcon *trayIcon() const { return m_icon; }
    // a system tray exists (tests can force it: setTrayAvailableOverride(1))
    static bool trayAvailable();
    static void setTrayAvailableOverride(int v);   // -1 = ask the platform, 0 / 1 = forced

    void setOsIntegration(OsIntegration *os) { m_os = os; }
    OsIntegration *osIntegration() const { return m_os; }

    bool isHiddenToTray() const { return m_hidden; }
    void hideToTray();
    void restore();
    void toggle();
    void startHidden();            // --minimized
    void requestExit();            // tray / system menu "Exit": confirm while downloads run, then quit
    void quitNow();                // save everything and ask the app to shut down
    bool isQuitting() const { return m_quitting; }
    bool handleClose();            // the window's close button; true = quit now
    int activeDownloads() const;
    void setExitWhenDone(bool on);
    bool exitWhenDone() const { return m_exitWhenDone; }
    void setShutdownWhenDone(bool on);
    bool shutdownWhenDone() const { return m_shutdownWhenDone; }
    void resetAllWindows();

    // tests: replace the close prompt (return ClosePrompt::Choice, or -1 = closed) and the
    // "exit while downloading?" question (return true = exit)
    using PromptHook = std::function<int(bool downloadsRunning, bool *dontShowAgain)>;
    static void setPromptHook(const PromptHook &h);
    using ConfirmHook = std::function<bool(const QString &text)>;
    static void setConfirmHook(const ConfirmHook &h);

signals:
    void hiddenChanged(bool hidden);

protected:
    bool eventFilter(QObject *o, QEvent *e) override;

private:
    void buildMenus();
    void updateMenus();
    void checkDone();
    bool confirmExit();
    void installSystemMenu();

    MainWindow *m_mw;
    QPointer<OsIntegration> m_os;
    QSystemTrayIcon *m_icon = nullptr;
    QMenu *m_menu = nullptr;
    QMenu *m_sysMenu = nullptr;
    QAction *m_show = nullptr, *m_hide = nullptr, *m_exitDone = nullptr, *m_shutDone = nullptr, *m_auto = nullptr;
    QAction *m_sysExitDone = nullptr, *m_sysShutDone = nullptr, *m_sysAuto = nullptr;
    QTimer *m_doneTimer = nullptr;
    QList<QPointer<QWidget>> m_hiddenWindows;
    bool m_hidden = false;
    bool m_wasMaximized = false;
    bool m_quitting = false;
    bool m_exitWhenDone = false;
    bool m_shutdownWhenDone = false;
    qint64 m_restoredAt = 0;
    void *m_nativeFilter = nullptr;
};

} // namespace mx::ui
