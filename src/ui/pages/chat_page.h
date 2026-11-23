// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: the Chat page (UI_SPEC §3.3, §8.5).
//
// Row C is the chat tab strip: the "List Channels" command button (pops the network menu), the
// channel-list tab ("OctoMX Peer Network") and one tab per open room (up to 8). Below it either the
// channel list (ChannelListView: Filter / edit / Join / Host / Close, Join Selected Channel /
// Refresh List / Favorites, Channel / Users / Topic list) or a room (ChatRoomView: bold header +
// topic, black output, user list, input line, Commands / Exit Channel).
//
// Everything goes through mx::ChatService / mx::UserService. Client-side commands: /clr, /log
// [file], /channelname; every other line (incl. /me, /topic, /join, /msg ...) is handed to
// ChatService::sendMessage. Open rooms are remembered ("ui/chatOpenRooms") and rejoined after a
// restart once the OctoMX network is up; the room splitter and both lists' columns persist.
#pragma once

#include <QtCore/QHash>
#include <QtCore/QPointer>

#include "mx/types.h"
#include "ui/pages/page.h"

class QFile;
class QLineEdit;
class QMenu;
class QSplitter;
class QStackedWidget;
class QTimer;

namespace mx::ui {

class ChannelModel;
class ChannelProxy;
class ChatInput;
class ChatOutput;
class ChatUserModel;
class ChatUserProxy;
class MxListView;
class PushButton;
class TabStrip;
class Well;

// UI-only settings keys used by the chat page (listed under "Settings keys wanted").
namespace chatkeys {
inline const QString OpenRooms = QStringLiteral("ui/chatOpenRooms");             // StringList
inline const QString HoldOpenAfterPart = QStringLiteral("ui/chatHoldOpenAfterPart"); // Bool, false
inline const QString WarningDisabled = QStringLiteral("ui/chatWarningDisabled"); // Bool, false
}

class ChannelListView : public QWidget {
    Q_OBJECT
public:
    ChannelListView(mx::Core *core, QWidget *parent = nullptr);
    MxListView *list() const { return m_list; }
    ChannelModel *model() const { return m_model; }
    ChannelProxy *proxy() const { return m_proxy; }
    QLineEdit *edit() const { return m_edit; }
    QStringList statusFields() const;
    void reload();
    void showFavoritesMenu();
    void showRowMenu(const QModelIndex &viewIndex, const QPoint &globalPos);
    QString selectedRoomName() const;
    void updateHostVisibility();
    // the network whose rooms are listed (List Channels ▸ <network>); Refresh List asks only it
    void setNetwork(mx::Net n);
    mx::Net network() const { return m_net; }

signals:
    void joinRequested(const QString &roomIdOrName);
    void closeRequested();
    void statusChanged();

protected:
    void resizeEvent(QResizeEvent *) override;

private:
    void applyFilter();
    mx::Core *m_core;
    PushButton *m_filter, *m_join, *m_host, *m_close, *m_joinSel, *m_refresh, *m_favs;
    Well *m_wFilter, *m_wJoin, *m_wHost, *m_wClose, *m_wJoinSel, *m_wRefresh, *m_wFavs;
    QLineEdit *m_edit;
    MxListView *m_list;
    ChannelModel *m_model;
    ChannelProxy *m_proxy;
    mx::Net m_net = mx::Net::Overlay;
};

class ChatRoomView : public QWidget {
    Q_OBJECT
public:
    ChatRoomView(mx::Core *core, MainWindow *mw, const mx::ChatRoomInfo &room, QWidget *parent = nullptr);
    ~ChatRoomView() override;
    QString roomId() const { return m_room.id; }
    mx::ChatRoomInfo room() const { return m_room; }
    bool isJoined() const { return m_joined; }
    void setJoined(bool on);
    void setRoomInfo(const mx::ChatRoomInfo &r);
    void addMessage(const mx::ChatMessage &m);
    void loadBacklog();
    void scheduleUserReload();
    void reloadUsers();
    void clearOutput();
    // a line typed by the user (or the script driver): client commands or ChatService
    void submit(const QString &text);
    void showCommandsMenu();
    void showUserMenu(const QModelIndex &viewIndex, const QPoint &globalPos);
    ChatOutput *output() const { return m_out; }
    ChatInput *input() const { return m_input; }
    MxListView *userList() const { return m_users; }
    ChatUserModel *userModel() const { return m_userModel; }
    QSplitter *splitter() const { return m_split; }
    QString headerName() const;

signals:
    void closeRequested();

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;

private:
    mx::UserRef userAt(const QModelIndex &viewIndex) const;
    void system(const QString &text, bool error = false);
    void applyBackscroll();
    void logLine(const QString &plain);
    bool weAreOp() const;
    mx::Core *m_core;
    MainWindow *m_main;
    mx::ChatRoomInfo m_room;
    bool m_joined = true;
    ChatOutput *m_out;
    MxListView *m_users;
    ChatUserModel *m_userModel;
    ChatUserProxy *m_userProxy;
    QSplitter *m_split;
    ChatInput *m_input;
    PushButton *m_commands;
    PushButton *m_exit;
    QTimer *m_userTimer;
    QFile *m_log = nullptr;
    bool m_sized = false;
};

class ChatPage : public Page {
    Q_OBJECT
public:
    ChatPage(mx::Core *core, MainWindow *mw, QWidget *parent = nullptr);

    QStringList statusFields() const override;
    void activated() override;
    void saveState() override;
    bool selectSubTab(const QString &name) override;
    bool scriptCommand(const QString &verb, const QStringList &args, QString *error) override;

    ChannelListView *channelList() const { return m_list; }
    ChatRoomView *room(const QString &roomId) const;
    ChatRoomView *currentRoom() const;
    QList<ChatRoomView *> rooms() const { return m_rooms; }
    TabStrip *tabs() const { return m_tabs; }
    void showChannelList();                 // open (or re-open) the channel list tab
    void showChannelListFor(mx::Net n);     // List Channels ▸ <network>
    void join(const QString &roomIdOrName); // user-initiated join (switches to the room)
    void closeRoom(ChatRoomView *v);
    // open a PM window for / find a room with `u` (MainWindow::openChat)
    void focusUser(const mx::UserRef &u);
    void showListChannelsMenu();

    static constexpr int kMaxRooms = 8;

protected:
    void resizeEvent(QResizeEvent *) override;

private:
    void onJoined(const mx::ChatRoomInfo &r);
    void onLeft(const QString &roomId);
    void onMessage(const mx::ChatMessage &m);
    void rebuildTabs();
    void setCurrentWidget(QWidget *w);
    void persistOpenRooms();
    void rejoinSaved();
    void maybeWarn();
    void setTransientStatus(const QString &s);

    TabStrip *m_tabs;
    QStackedWidget *m_stack;
    QWidget *m_blank;
    ChannelListView *m_list = nullptr;
    QList<ChatRoomView *> m_rooms;
    QList<QWidget *> m_tabWidgets;          // tab i shows m_tabWidgets[i]
    QStringList m_pendingJoins;             // lower-case names/ids the user asked for
    QString m_prevPage = QStringLiteral("networks");
    QString m_transient;
    bool m_listFetched = false;
    bool m_rejoined = false;
    bool m_quitting = false;
};

} // namespace mx::ui
