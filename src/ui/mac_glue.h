// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI [mac-1]: what a Mac expects around the classic window.
//
//   * OpenEventFilter (every platform, only macOS sends the event): links and files the system
//     hands to the running program (a magnet: / ed2k: link clicked in a browser, a .torrent file
//     opened in Finder or dropped on the Dock icon) arrive as QFileOpenEvent; they become
//     linksReceived(), the same path as links forwarded by a second instance.
//   * installMacGlue() (macOS only; elsewhere it does nothing):
//       - the native menu bar with the application menu: About OctoMX, Check for Updates...,
//         Settings... (Cmd+,), Quit OctoMX (Cmd+Q, asks as the close prompt does), and a Window
//         menu (Minimize Cmd+M, Show OctoMX); the in-window classic buttons stay as they are
//       - the Dock icon's menu (Show OctoMX, Hide) and a click on the Dock icon brings a hidden
//         window back
//       - the red close button hides the window (the program keeps running, Dock and menu bar
//         icons stay); Quit from the Dock or the system goes through the close prompt
#pragma once

#include <QtCore/QObject>
#include <QtCore/QStringList>

class QMenuBar;

namespace mx::ui {

class MainWindow;

class OpenEventFilter : public QObject {
    Q_OBJECT
public:
    explicit OpenEventFilter(QObject *parent = nullptr);
    // the link or file of one QFileOpenEvent, as addLinks() takes it (empty = not ours)
    static QString linkOf(const QString &file, const QString &url);

signals:
    void linksReceived(const QStringList &links);

protected:
    bool eventFilter(QObject *o, QEvent *e) override;
};

// macOS: menu bar, Dock menu, Dock click, red button and Quit handling for this window. Returns the
// menu bar (nullptr on other platforms). Safe to call once per main window.
QMenuBar *installMacGlue(MainWindow *mw);

} // namespace mx::ui
