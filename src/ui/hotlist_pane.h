// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: the Hotlist side pane (UI_SPEC §3.5), docked at the right of the page area and
// toggled by the Hotlist button. "Hotlist Options" push button on top, black list below.
#pragma once

#include <QtCore/QPointer>
#include <QtWidgets/QWidget>

namespace mx {
class Core;
}

class QLabel;

namespace mx::ui {

class AutoRespondersWindow;
class HotlistModel;
class MainWindow;
class MenuButton;
class MxListView;
class PushButton;

class HotlistPane : public QWidget {
    Q_OBJECT
public:
    HotlistPane(mx::Core *core, MainWindow *mw, QWidget *parent = nullptr);
    MxListView *list() const { return m_list; }
    HotlistModel *model() const { return m_model; }
    // pop the menu for a row (script driver / right click)
    void showContextMenu(int row, const QPoint &globalPos);
    void showOptionsMenu();
    void openAutoResponders();
    void removeSelected();       // [qa-1] Remove / Del on the selected entries
    void removeRows(const QList<int> &rows);   // [qa-1] the Auto Responders tool window

protected:
    void resizeEvent(QResizeEvent *) override;

private:
    void activate(int row);
    mx::Core *m_core;
    MainWindow *m_main;
    PushButton *m_options;
    MxListView *m_list;
    HotlistModel *m_model;
    QPointer<AutoRespondersWindow> m_autoResponders;
    QLabel *m_hint = nullptr;    // [ui-g] one line of help while the list is empty
    void updateHint();
};

} // namespace mx::ui
