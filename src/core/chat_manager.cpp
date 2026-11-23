// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/chat_manager.h"
#include "core/chat_commands.h"
#include "core/dao.h"
#include "core/log.h"
#include "core/network_manager.h"
#include "core/user_manager.h"

#include "mx/settings_keys.h"

#include <QtCore/QDateTime>
#include <QtCore/QRandomGenerator>
#include <QtCore/QTimer>

namespace mx {

namespace {
const QString kOffline = QStringLiteral("Chat is offline: no chat network is available.");
const QString kNotConnected = QStringLiteral("Not connected to the OctoMX network yet. Please wait or press Connect.");
}

ChatManager::ChatManager(Settings *settings, NetworkManager *network, UserManager *users, UserListsDao *dao,
                         QObject *parent)
    : ChatService(parent)
    , m_settings(settings)
    , m_network(network)
    , m_users(users)
    , m_dao(dao)
{
    if (m_network) {
        for (NetworkPlugin *p : m_network->plugins()) {
            auto *b = dynamic_cast<ChatBackend *>(p);
            if (b && (p->capabilities() & NetworkPlugin::CanChat)) {
                m_backend = b;
                m_backendPlugin = p;
                break;
            }
        }
    }
    if (m_backend) {
        QObject *ev = m_backend->chatEvents();
        if (auto *ce = qobject_cast<ChatEvents *>(ev)) {
            connect(ce, &ChatEvents::roomListChanged, this, &ChatService::roomListChanged);
            connect(ce, &ChatEvents::roomJoined, this, [this](const ChatRoomInfo &r) {
                const bool fresh = !m_joined.contains(r.id);
                m_joined.insert(r.id, r);
                if (fresh) m_joinOrder << r.id;
                emit roomJoined(r);
            });
            connect(ce, &ChatEvents::roomLeft, this, [this](const QString &id) {
                m_joined.remove(id);
                m_joinOrder.removeAll(id);
                m_backlog.remove(id);
                emit roomLeft(id);
            });
            connect(ce, &ChatEvents::roomUpdated, this, [this](const ChatRoomInfo &r) {
                if (m_joined.contains(r.id)) m_joined.insert(r.id, r);
                emit roomUpdated(r);
            });
            connect(ce, &ChatEvents::roomUsersChanged, this, &ChatService::roomUsersChanged);
            connect(ce, &ChatEvents::messageReceived, this, &ChatManager::onIncoming);
            // [ovl-2] delivery receipts of the PMs we sent
            connect(ce, &ChatEvents::privateMessageStatus, this, &ChatService::privateMessageStatus);
            // ChatEvents::userPresence is handled by the UserManager (for every chat plugin)
        } else {
            qCWarning(lcChat) << "chat backend has no ChatEvents object";
        }
        connect(m_backendPlugin.data(), &NetworkPlugin::statusChanged, this, [this](const NetworkStatus &st) {
            const bool connected = st.state == ConnState::Connected;
            if (connected && !m_wasConnected) autoJoin();
            if (!connected && m_wasConnected) {
                // the backend dropped every room
                const QStringList ids = m_joinOrder;
                for (const QString &id : ids) {
                    system(id, QStringLiteral("Disconnected from the chat network."));
                }
            }
            m_wasConnected = connected;
        });
    }
    if (m_users) {
        connect(m_users, &UserService::whoisResult, this, [this](const UserRef &u, const QVariantMap &info) {
            const QString room = m_pendingWhois.take(u.key());
            if (room.isNull()) return;
            system(room, QStringLiteral("Whois %1:").arg(u.name.isEmpty() ? u.id : u.name));
            for (auto it = info.constBegin(); it != info.constEnd(); ++it)
                system(room, QStringLiteral("  %1: %2").arg(it.key(), it.value().toString()));
        });
    }
}

void ChatManager::start()
{
    if (m_dao) m_favourites = m_dao->favouriteRooms();
    if (m_backendPlugin && m_backendPlugin->status().state == ConnState::Connected && !m_wasConnected) {
        m_wasConnected = true;
        autoJoin();
    }
}

void ChatManager::autoJoin()
{
    if (!m_backend) return;
    QStringList rooms;
    if (m_settings->b(keys::chat::AutoJoinFavourites)) rooms = m_favourites;
    const QString def = m_settings->s(keys::chat::DefaultRoom).trimmed();
    if (!def.isEmpty() && !rooms.contains(def)) rooms << def;
    for (const QString &r : std::as_const(rooms)) {
        bool joined = false;
        for (const ChatRoomInfo &j : std::as_const(m_joined))
            if (j.id == r || j.name.compare(r, Qt::CaseInsensitive) == 0) joined = true;
        if (!joined) m_backend->joinRoom(r);
    }
}

bool ChatManager::backendOnline() const
{
    return m_backend && m_backendPlugin && m_network && m_network->isRunning(m_backendPlugin->net());
}

UserRef ChatManager::self() const
{
    // [ovl-2] the chat network's own identity (OctoMX: user key + the name others see), so own lines
    // show the same name as the room list and match our own row by id
    if (m_backendPlugin) {
        const UserRef me = m_backendPlugin->localUser();
        if (me.isValid()) return me;
    }
    UserRef u;
    u.net = m_backendPlugin ? m_backendPlugin->net() : Net::Overlay;
    u.name = m_settings->s(keys::identity::ScreenName);
    u.id = u.name;
    return u;
}

void ChatManager::append(const QString &key, const ChatMessage &m)
{
    QList<ChatMessage> &l = m_backlog[key];
    l << m;
    int cap = qMax(10, m_settings->i(keys::chat::BacklogLines));
    if (!m_settings->b(keys::chat::AllowLargeBackscroll)) cap = qMin(cap, 1000);
    if (l.size() > cap) l.remove(0, l.size() - cap);
}

void ChatManager::system(const QString &roomId, const QString &text, ChatKind kind)
{
    ChatMessage m;
    m.roomId = roomId;
    m.kind = kind;
    m.text = text;
    m.time = QDateTime::currentDateTime();
    if (m_joined.contains(roomId)) append(roomId, m);
    emit messageReceived(m);
}

bool ChatManager::floodCheck(const UserRef &u)
{
    if (!m_settings->b(keys::messages::BlockFloods)) return true;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QList<qint64> &t = m_flood[u.key()];
    t << now;
    while (!t.isEmpty() && now - t.first() > 10000) t.removeFirst();
    return t.size() <= m_settings->i(keys::messages::FloodLimit);
}

bool ChatManager::allowedPrivate(const UserRef &from, bool onHotlist) const
{
    const QString allow = m_settings->s(keys::messages::AllowPrivateMessages);
    if (allow == QLatin1String("nobody")) return false;
    if (allow == QLatin1String("hotlist") && !onHotlist) return false;
    if (m_settings->s(keys::messages::AllowFrom) != QLatin1String("restricted")) return true;
    // original WinMX "Only allow messages from the following sources"
    if (m_settings->b(keys::messages::AllowFromHotlist) && onHotlist) return true;
    if (m_settings->b(keys::messages::AllowFromSameRoom) && m_backend) {
        for (const QString &id : m_joinOrder)
            for (const ChatUser &cu : m_backend->roomUsers(id))
                if (cu.user.key() == from.key()) return true;
    }
    if (m_transferPeer) {
        if (m_settings->b(keys::messages::AllowFromDownloadingFrom) && m_transferPeer(from, true)) return true;
        if (m_settings->b(keys::messages::AllowFromUploadingTo) && m_transferPeer(from, false)) return true;
    }
    return false;
}

void ChatManager::onIncoming(const ChatMessage &in)
{
    ChatMessage m = in;
    if (!m.time.isValid()) m.time = QDateTime::currentDateTime();
    const bool isPrivate = m.roomId.isEmpty() || m.kind == ChatKind::Private;
    const bool fromUser = m.from.isValid() && (m.kind == ChatKind::Message || m.kind == ChatKind::Action
                                               || m.kind == ChatKind::Private);
    if (fromUser) {
        // [ovl-2] an ignored user is not told (their messages vanish, as in the original WinMX); a privacy refusal is
        // passed back to the backend, which tells the sender ("accepts messages only from ...")
        if (m_users && m_users->isIgnored(m.from)) return;
        const bool onHotlist = m_users && m_users->isOnHotlist(m.from);
        auto refuse = [&](const char *why) {
            if (isPrivate && m_backend) m_backend->refusePrivateMessage(m, QString::fromLatin1(why));
        };
        if (m_settings->b(keys::messages::IgnoreHotlistOnly) && !onHotlist) return refuse("hotlist");
        if (isPrivate && !allowedPrivate(m.from, onHotlist)) {
            const QString allow = m_settings->s(keys::messages::AllowPrivateMessages);
            return refuse(allow == QLatin1String("nobody") ? "nobody" : allow == QLatin1String("hotlist") ? "hotlist" : "restricted");
        }
        for (const QString &w : m_settings->sl(keys::messages::FilterWords))
            if (!w.isEmpty() && m.text.contains(w, Qt::CaseInsensitive)) return;
        if (!floodCheck(m.from)) return;
    }
    if (m.kind == ChatKind::Join || m.kind == ChatKind::Part) {
        if (m_users && m.from.isValid() && m_users->isIgnored(m.from)) return;
        if (!m_settings->b(keys::chat::ShowJoinsParts)) return;
    }
    if (isPrivate) {
        if (m.kind == ChatKind::Message) m.kind = ChatKind::Private;
        append(pmKey(m.from), m);
        emit messageReceived(m);
        if (fromUser && m_settings->b(keys::messages::AutoReply) && m_backend) {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            const qint64 last = m_autoReplied.value(m.from.key(), 0);
            if (now - last > 10 * 60 * 1000) {
                m_autoReplied.insert(m.from.key(), now);
                sendPrivateMessage(m.from, m_settings->s(keys::messages::AutoReplyText));
            }
        }
        return;
    }
    if (m.kind == ChatKind::Topic && m_joined.contains(m.roomId)) {
        ChatRoomInfo &r = m_joined[m.roomId];
        if (r.topic != m.text) {
            r.topic = m.text;
            emit roomUpdated(r);
        }
    }
    append(m.roomId, m);
    emit messageReceived(m);
}

// ---------------------------------------------------------------- ChatService
void ChatManager::refreshRoomList()
{
    if (m_backend && backendOnline()) m_backend->refreshRoomList();
    else QTimer::singleShot(0, this, &ChatService::roomListChanged);
}

QList<Net> ChatManager::chatNetworks() const
{
    QList<Net> out;
    if (!m_network) return out;
    for (NetworkPlugin *p : m_network->plugins()) {
        if (!dynamic_cast<ChatBackend *>(p) || !(p->capabilities() & NetworkPlugin::CanChat)) continue;
        if (m_network->isRunning(p->net()) && !out.contains(p->net())) out << p->net();
    }
    return out;
}

void ChatManager::refreshRoomList(Net net)
{
    // One chat backend serves the Chat page today (the first CanChat plugin); asking for another
    // network's list answers with an unchanged list.
    if (m_backendPlugin && m_backendPlugin->net() == net) refreshRoomList();
    else QTimer::singleShot(0, this, &ChatService::roomListChanged);
}

QList<ChatRoomInfo> ChatManager::roomList() const
{
    return m_backend ? m_backend->roomList() : QList<ChatRoomInfo>();
}

void ChatManager::joinRoom(const QString &roomIdOrName)
{
    const QString r = roomIdOrName.trimmed();
    if (r.isEmpty()) return;
    if (!m_backend) {
        system(r, kOffline);
        return;
    }
    if (!backendOnline()) {
        system(r, kNotConnected);
        return;
    }
    m_backend->joinRoom(r);
}

void ChatManager::leaveRoom(const QString &roomId)
{
    if (m_backend && backendOnline()) {
        m_backend->leaveRoom(roomId);
        return;
    }
    // offline: drop it locally
    if (m_joined.remove(roomId)) {
        m_joinOrder.removeAll(roomId);
        m_backlog.remove(roomId);
        emit roomLeft(roomId);
    }
}

QList<ChatRoomInfo> ChatManager::joinedRooms() const
{
    QList<ChatRoomInfo> out;
    for (const QString &id : m_joinOrder) out << m_joined.value(id);
    return out;
}

QList<ChatUser> ChatManager::roomUsers(const QString &roomId) const
{
    return m_backend ? m_backend->roomUsers(roomId) : QList<ChatUser>();
}

QList<ChatMessage> ChatManager::backlog(const QString &roomId) const
{
    return m_backlog.value(roomId);
}

UserRef ChatManager::resolveUser(const QString &name, const QString &roomId) const
{
    if (name.isEmpty()) return UserRef();
    auto match = [&name](const ChatUser &cu) {
        return cu.user.name.compare(name, Qt::CaseInsensitive) == 0 || cu.user.id == name;
    };
    if (m_backend) {
        for (const ChatUser &cu : m_backend->roomUsers(roomId))
            if (match(cu)) return cu.user;
        for (const QString &id : m_joinOrder)
            for (const ChatUser &cu : m_backend->roomUsers(id))
                if (match(cu)) return cu.user;
    }
    if (m_users) {
        const UserRef u = m_users->findByName(name);
        if (u.isValid()) return u;
    }
    return UserRef();
}

void ChatManager::sendMessage(const QString &roomId, const QString &text)
{
    const ChatCommand c = parseChatCommand(text);
    switch (c.type) {
    case ChatCommand::Empty:
        return;
    case ChatCommand::Text:
    case ChatCommand::Me: {
        if (c.type == ChatCommand::Me && c.text.isEmpty()) {
            system(roomId, QStringLiteral("Usage: /me <action>"), ChatKind::Error);
            return;
        }
        if (!m_backend) { system(roomId, kOffline); return; }
        if (!backendOnline()) { system(roomId, kNotConnected); return; }
        const ChatKind kind = c.type == ChatCommand::Me ? ChatKind::Action : ChatKind::Message;
        m_backend->sendRoomMessage(roomId, c.text, kind);
        ChatMessage m;
        m.roomId = roomId;
        m.from = self();
        m.text = c.text;
        m.kind = kind;
        m.time = QDateTime::currentDateTime();
        append(roomId, m);
        emit messageReceived(m);
        return;
    }
    case ChatCommand::Topic:
        if (c.text.isEmpty()) {
            const QString t = m_joined.value(roomId).topic;
            system(roomId, t.isEmpty() ? QStringLiteral("No topic is set.") : QStringLiteral("Topic: ") + t, ChatKind::Topic);
        } else {
            setTopic(roomId, c.text);
        }
        return;
    case ChatCommand::Join:
        if (c.target.isEmpty()) system(roomId, QStringLiteral("Usage: /join <room>"), ChatKind::Error);
        else joinRoom(c.target);
        return;
    case ChatCommand::Part: {
        QString target = roomId;
        if (!c.target.isEmpty()) {
            for (const ChatRoomInfo &r : std::as_const(m_joined))
                if (r.id == c.target || r.name.compare(c.target, Qt::CaseInsensitive) == 0) target = r.id;
        }
        leaveRoom(target);
        return;
    }
    case ChatCommand::Msg: {
        if (c.target.isEmpty() || c.text.isEmpty()) {
            system(roomId, QStringLiteral("Usage: /msg <user> <text>"), ChatKind::Error);
            return;
        }
        const UserRef u = resolveUser(c.target, roomId);
        if (!u.isValid()) {
            system(roomId, QStringLiteral("No such user: %1").arg(c.target), ChatKind::Error);
            return;
        }
        sendPrivateMessage(u, c.text);
        return;
    }
    case ChatCommand::Ignore:
    case ChatCommand::Unignore: {
        if (c.target.isEmpty()) {
            system(roomId, QStringLiteral("Usage: /%1 <user>").arg(c.name), ChatKind::Error);
            return;
        }
        UserRef u = resolveUser(c.target, roomId);
        if (!u.isValid() && m_users) {
            for (const UserRef &i : m_users->ignored())
                if (i.name.compare(c.target, Qt::CaseInsensitive) == 0 || i.id == c.target) u = i;
        }
        if (!u.isValid()) { // by name only
            u.net = m_backendPlugin ? m_backendPlugin->net() : Net::Overlay;
            u.id = c.target;
            u.name = c.target;
        }
        const bool on = c.type == ChatCommand::Ignore;
        if (m_users) m_users->ignore(u, on);
        system(roomId, on ? QStringLiteral("%1 is now ignored.").arg(u.name) : QStringLiteral("%1 is no longer ignored.").arg(u.name));
        return;
    }
    case ChatCommand::Whois: {
        if (c.target.isEmpty()) {
            system(roomId, QStringLiteral("Usage: /whois <user>"), ChatKind::Error);
            return;
        }
        UserRef u = resolveUser(c.target, roomId);
        if (!u.isValid()) {
            u.net = m_backendPlugin ? m_backendPlugin->net() : Net::Overlay;
            u.id = c.target;
            u.name = c.target;
        }
        m_pendingWhois.insert(u.key(), roomId);
        if (m_users) m_users->whois(u);
        return;
    }
    case ChatCommand::Clear:
        m_backlog[roomId].clear();
        emit backlogCleared(roomId);
        return;
    case ChatCommand::Help:
        for (const QString &l : chatHelpLines()) system(roomId, l);
        return;
    case ChatCommand::Motd: {
        const QString motd = m_joined.value(roomId).motd;
        if (motd.isEmpty()) system(roomId, QStringLiteral("No message of the day is set."));
        else for (const QString &l : motd.split(QLatin1Char('\n'))) system(roomId, l, ChatKind::Motd);
        return;
    }
    case ChatCommand::SetMotd:
        if (!m_backend) { system(roomId, kOffline); return; }
        if (!backendOnline()) { system(roomId, kNotConnected); return; }
        m_backend->setMotd(roomId, c.text);
        return;
    case ChatCommand::Unknown:
        system(roomId, QStringLiteral("Unknown command: /%1. Type /help for a list of commands.").arg(c.name), ChatKind::Error);
        return;
    }
}

void ChatManager::sendPrivateMessage(const UserRef &to, const QString &text)
{
    if (text.trimmed().isEmpty() || !to.isValid()) return;
    ChatMessage m;
    m.from = self();
    m.to = to;
    m.kind = ChatKind::Private;
    m.time = QDateTime::currentDateTime();
    if (!m_backend || !backendOnline()) {
        m.kind = ChatKind::Error;
        m.text = m_backend ? kNotConnected : kOffline;
        m.from = UserRef();
        emit messageReceived(m);
        return;
    }
    // [ovl-2] the echo first, with a message id: a backend that reports delivery sends its status
    // (Sending, Delivered, Not delivered...) for this id right after
    m.text = text;
    m.msgId = QString::number(QRandomGenerator::global()->generate64(), 16).rightJustified(16, QLatin1Char('0'));
    append(pmKey(to), m);
    emit messageReceived(m);
    m_backend->sendPrivateMessageWithId(to, text, m.msgId);
}

void ChatManager::setTopic(const QString &roomId, const QString &topic)
{
    if (!m_backend) { system(roomId, kOffline); return; }
    if (!backendOnline()) { system(roomId, kNotConnected); return; }
    m_backend->setTopic(roomId, topic);
}

QStringList ChatManager::favouriteRooms() const
{
    return m_favourites;
}

void ChatManager::setFavouriteRoom(const QString &roomIdOrName, bool fav)
{
    const QString r = roomIdOrName.trimmed();
    if (r.isEmpty() || fav == m_favourites.contains(r)) return;
    if (fav) m_favourites << r;
    else m_favourites.removeAll(r);
    if (m_dao) m_dao->setFavouriteRoom(r, fav);
}

} // namespace mx
