// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: the Networks page (UI_SPEC §3.1 + §8.2).
//   sub-tab "OctoMX Peer Network": Connect / Disconnect, Connection Status (black indicator with a
//     green / yellow / red lamp), Network Identity (Screen Name + Change...), Connection Type radios.
//   sub-tab "Other Protocol Networks": per-network / per-server black list with the original WinMX
//     server-list buttons and context menu.
#pragma once

#include <QtCore/QHash>

#include <functional>

#include "mx/types.h"
#include "ui/pages/page.h"

class QGroupBox;
class QLabel;
class QRadioButton;
class QStackedWidget;

namespace mx::ui {

class MxListView;
class PushButton;
class ServerModel;
class TabStrip;
class WrapLabel;
class StaticText;
class ServerMessagesWindow;

class StatusIndicator : public QWidget {
    Q_OBJECT
public:
    explicit StatusIndicator(QWidget *parent = nullptr);
    void setState(int s);   // 0 = red (not connected), 1 = yellow (connecting), 2 = green (ready)
    int state() const { return m_state; }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    int m_state = 0;
};

class NetworksPage : public Page {
    Q_OBJECT
public:
    NetworksPage(mx::Core *core, MainWindow *mw, QWidget *parent = nullptr);
    QStringList statusFields() const override;
    bool selectSubTab(const QString &name) override;
    MxListView *serverList() const { return m_list; }
    void showServerMenu(const QModelIndex &viewIndex, const QPoint &globalPos);
    void importList();                       // "Import .MET" -> NetworkService::importServerList
    // "View Server Messages": one floating window per network / server (serverId empty = whole network)
    void viewServerMessages(mx::Net n, const QString &serverId);
    void saveState() override;
    // tests: replaces the file dialog of Import .MET (the result text is kept instead of a message box)
    static std::function<QString(QWidget *)> s_importHook;
    QString lastImportMessage() const { return m_lastImportMessage; }

protected:
    void resizeEvent(QResizeEvent *) override;
    bool eventFilter(QObject *o, QEvent *e) override;

private:
    void buildWpn();
    void buildOther();
    void layoutWpn();
    void layoutOther();
    void refreshStatus();
    void changeScreenName();
    void addServer();
    void editServer();
    void removeServer();
    void exportList();
    void findList();
    QModelIndex currentServerIndex() const;   // source index of the selection

    TabStrip *m_tabs;
    QStackedWidget *m_stack;
    QWidget *m_wpn;
    QWidget *m_other;
    // WPN
    PushButton *m_connect;
    PushButton *m_disconnect;
    PushButton *m_connTest = nullptr;    // [diag-1]
    PushButton *m_connTest2 = nullptr;
    QGroupBox *m_statusBox;
    StatusIndicator *m_lamp;
    QLabel *m_headline;
    StaticText *m_detail;
    QGroupBox *m_identityBox;
    StaticText *m_nameLabel;
    StaticText *m_name;
    PushButton *m_change;
    QGroupBox *m_typeBox;
    QRadioButton *m_secondary;
    QRadioButton *m_primary;
    WrapLabel *m_secondaryText;
    WrapLabel *m_primaryText;
    // Other
    QList<PushButton *> m_otherButtons;
    MxListView *m_list;
    ServerModel *m_model;
    QHash<QString, ServerMessagesWindow *> m_msgWindows;
    QString m_lastImportMessage;
};

} // namespace mx::ui
