// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX core: ChatService implementation.
// Forwards to the first plugin that also implements mx::ChatBackend (found with dynamic_cast) and
// adds: favourite rooms (DB, auto-join on connect), per-room backlog in memory (chat/backlogLines),
// the ignore list + privacy settings + flood/word filters applied to incoming messages, auto-reply,
// slash-command parsing in sendMessage() (see core/chat_commands.h), and local echo of our own
// messages (backends must not echo them). Private-message history is kept under backlog("pm:<user key>").
// Without a chat backend everything still works: empty lists and a System message saying chat is offline.
#pragma once

#include <QtCore/QHash>
#include <QtCore/QPointer>

#include <functional>

#include "mx/plugin.h"
#include "mx/services.h"

namespace mx {

class NetworkManager;
class UserManager;
class UserListsDao;

class ChatManager : public ChatService {
    Q_OBJECT
public:
    ChatManager(Settings *settings, NetworkManager *network, UserManager *users, UserListsDao *dao,
                QObject *parent = nullptr);

    void start();                         // load favourites; auto-join when the backend connects
    bool hasBackend() const { return m_backend != nullptr; }
    static QString pmKey(const UserRef &u) { return QStringLiteral("pm:") + u.key(); }
    // For the original WinMX privacy checks "Users you are downloading from / uploading to".
    using TransferPeerCheck = std::function<bool(const UserRef &u, bool downloadingFrom)>;
    void setTransferPeerCheck(TransferPeerCheck fn) { m_transferPeer = std::move(fn); }

    void refreshRoomList() override;
    QList<Net> chatNetworks() const override;          // [glue-3] running ChatBackend plugins
    void refreshRoomList(Net net) override;            // [glue-3]
    QList<ChatRoomInfo> roomList() const override;
    void joinRoom(const QString &roomIdOrName) override;
    void leaveRoom(const QString &roomId) override;
    QList<ChatRoomInfo> joinedRooms() const override;
    QList<ChatUser> roomUsers(const QString &roomId) const override;
    QList<ChatMessage> backlog(const QString &roomId) const override;
    void sendMessage(const QString &roomId, const QString &text) override;
    void sendPrivateMessage(const UserRef &to, const QString &text) override;
    void setTopic(const QString &roomId, const QString &topic) override;
    QStringList favouriteRooms() const override;
    void setFavouriteRoom(const QString &roomIdOrName, bool fav) override;

private:
    bool backendOnline() const;
    UserRef self() const;
    UserRef resolveUser(const QString &name, const QString &roomId) const;
    void system(const QString &roomId, const QString &text, ChatKind kind = ChatKind::System);
    void append(const QString &key, const ChatMessage &m);
    void onIncoming(const ChatMessage &m);
    void autoJoin();
    bool floodCheck(const UserRef &u);

    Settings *m_settings;
    NetworkManager *m_network;
    UserManager *m_users;
    UserListsDao *m_dao;
    ChatBackend *m_backend = nullptr;
    QPointer<NetworkPlugin> m_backendPlugin;
    QHash<QString, ChatRoomInfo> m_joined;
    QStringList m_joinOrder;
    QHash<QString, QList<ChatMessage>> m_backlog;
    QStringList m_favourites;
    QHash<QString, QList<qint64>> m_flood;          // user key → recent message times (ms)
    QHash<QString, qint64> m_autoReplied;           // user key → last auto-reply (ms)
    QHash<QString, QString> m_pendingWhois;         // user key → room that asked
    bool m_wasConnected = false;
    TransferPeerCheck m_transferPeer;
    bool allowedPrivate(const UserRef &from, bool onHotlist) const;
};

} // namespace mx
