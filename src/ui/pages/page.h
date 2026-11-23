// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: base class of the main-window pages (Networks, Shared Files, Chat, Search,
// Transfers, Bandwidth). See src/ui/README.md "How to add a page".
//
// A page fills the page area below the page-button row (client y = 48 ... status bar). It owns its
// own top margin (the original WinMX pages start their row C 3-5 px below row B) and tells the main window what
// to show in the status bar through statusFields() + statusChanged().
#pragma once

#include <QtCore/QStringList>
#include <QtWidgets/QWidget>

namespace mx {
class Core;
}

namespace mx::ui {

class MainWindow;

class Page : public QWidget {
    Q_OBJECT
public:
    Page(const QString &id, const QString &title, mx::Core *core, MainWindow *mainWindow, QWidget *parent = nullptr);

    QString pageId() const { return m_id; }       // "networks", "shared", "chat", "search", "transfers", "bandwidth"
    QString title() const { return m_title; }     // "Networks" ... (floating window title)
    mx::Core *core() const { return m_core; }
    MainWindow *mainWindow() const { return m_main; }

    // Status-bar fields (drawn left to right, separated like the original WinMX's borderless panes).
    virtual QStringList statusFields() const { return {}; }
    // Called when the page becomes the visible page (also after docking back).
    virtual void activated() {}
    // Persist list columns / splitters etc. (called on close and by MainWindow::saveState()).
    virtual void saveState() {}
    // Sub-tab support for the script driver ("subtab <name>"). Return false if unknown.
    virtual bool selectSubTab(const QString &name) { Q_UNUSED(name); return false; }
    // Page-specific script commands (verb + args). Return true if handled; set *error on failure.
    virtual bool scriptCommand(const QString &verb, const QStringList &args, QString *error)
    {
        Q_UNUSED(verb); Q_UNUSED(args); Q_UNUSED(error);
        return false;
    }

signals:
    void statusChanged();

private:
    QString m_id;
    QString m_title;
    mx::Core *m_core;
    MainWindow *m_main;
};

// A blank face-coloured page (placeholder for pages another worker fills in).
class PlaceholderPage : public Page {
    Q_OBJECT
public:
    PlaceholderPage(const QString &id, const QString &title, mx::Core *core, MainWindow *mw, QWidget *parent = nullptr);
    QStringList statusFields() const override { return m_fields; }
    void setStatusFields(const QStringList &f) { m_fields = f; emit statusChanged(); }

private:
    QStringList m_fields;
};

} // namespace mx::ui
