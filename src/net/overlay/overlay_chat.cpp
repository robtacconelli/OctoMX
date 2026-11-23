// SPDX-License-Identifier: GPL-3.0-or-later
// Net::Overlay: chat rooms (ChatBackend), room directory, history replay, flood control.
#include "overlay_plugin_impl.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonArray>

#include "mx/settings_keys.h"

namespace mx::ovl {

bool overlayParseEnvelope(const QString &topic, const QByteArray &raw, QString *userId, QJsonObject *body);

namespace {
ChatEvents *ev(QObject *o) { return static_cast<ChatEvents *>(o); }
// The default rooms always exist; their built-in topic and message of the day (timestamp 0) are
// what a joining member sees until somebody sets their own (any /topic or /setmotd wins).
struct DefaultRoom { const char *name; const char *topic; };
const DefaultRoom kDefaultRooms[] = {
    {"OctoMX Lobby", "Welcome to the OctoMX Lobby"},
    {"Music", "Music talk: what are you listening to?"},
    {"Movies", "Movies and series, no spoilers without a warning"},
    {"Help", "Questions about OctoMX? Ask here."},
};
const char *const kDefaultMotd =
    "Welcome to the OctoMX network.\n"
    "Be friendly, no flooding and no advertising, please.\n"
    "Type /help for a list of commands.";
const DefaultRoom *defaultRoom(const QString &id)
{
    for (const DefaultRoom &d : kDefaultRooms)
        if (QString::fromLatin1(d.name).toLower() == id) return &d;
    return nullptr;
}
constexpr int kMaxMotd = 1024;
constexpr int kHistoryKeep = 50;
constexpr int kHistorySend = 30;
constexpr int kMaxText = 1024;
} // namespace

// A MOTD keeps its line breaks (at most 12 lines); each line is cleaned like a chat line.
QString OverlayPlugin::cleanMotd(const QString &raw)
{
    QStringList lines;
    for (const QString &l : raw.left(kMaxMotd).split(QLatin1Char('\n'))) {
        if (lines.size() >= 12) break;
        lines << cleanText(l, 256);
    }
    while (!lines.isEmpty() && lines.last().trimmed().isEmpty()) lines.removeLast();
    return lines.join(QLatin1Char('\n'));
}

QString OverlayPlugin::roomTopic(const QString &roomId)
{
    const QByteArray h = QCryptographicHash::hash(roomId.toUtf8(), QCryptographicHash::Sha1).toHex().left(20);
    return QStringLiteral("mx/room/") + QString::fromLatin1(h);
}

void OverlayPlugin::chatInit()
{
    for (const DefaultRoom &r : kDefaultRooms) {
        const QString name = QString::fromLatin1(r.name);
        DirEntry &d = m_directory[normaliseRoomName(name)];
        d.name = name;
        d.topic = QString::fromLatin1(r.topic);
        d.lastMs = -1;   // never expires
    }
}

OverlayPlugin::Room *OverlayPlugin::room(const QString &idOrName)
{
    const QString id = normaliseRoomName(idOrName);
    auto it = m_rooms.find(id);
    return it == m_rooms.end() ? nullptr : &it.value();
}

ChatRoomInfo OverlayPlugin::roomInfo(const Room &r) const
{
    ChatRoomInfo i;
    i.id = r.id;
    i.name = r.name;
    i.topic = r.topic;
    i.motd = r.motd;
    i.users = r.joined ? r.members.size() + 1 : r.members.size();
    i.net = Net::Overlay;
    return i;
}

bool OverlayPlugin::floodOk(const QString &userId)
{
    if (!setting(QString(keys::messages::BlockFloods), true).toBool()) return true;
    const int limit = qBound(2, setting(QString(keys::messages::FloodLimit), 6).toInt(), 100);
    m_floodGate.configure(limit / 10.0, limit);
    if (m_floodGate.allow(userId)) {
        m_floodNotified.remove(userId);
        return true;
    }
    return false;
}

void OverlayPlugin::emitRoomMessage(const Room &r, const QString &userId, const QString &name, const QString &text,
                                    ChatKind kind, qint64 ts)
{
    ChatMessage m;
    m.roomId = r.id;
    if (!userId.isEmpty()) m.from = userRefFor(userId);   // room state lines (topic, MOTD) have no author
    else m.from.net = Net::Overlay;
    if (!name.isEmpty()) m.from.name = name;
    m.text = text;
    m.kind = kind;
    m.time = ts > 0 ? QDateTime::fromMSecsSinceEpoch(ts) : QDateTime::currentDateTime();
    Q_EMIT ev(m_events)->messageReceived(m);
}

// ------------------------------------------------------------------------------- ChatBackend
void OverlayPlugin::refreshRoomList()
{
    if (m_running) announceRooms();
    QTimer::singleShot(0, this, [this] { Q_EMIT ev(m_events)->roomListChanged(); });
}

QList<ChatRoomInfo> OverlayPlugin::roomList() const
{
    QHash<QString, ChatRoomInfo> out;
    const qint64 now = nowMs();
    for (auto it = m_directory.cbegin(); it != m_directory.cend(); ++it) {
        if (it->lastMs >= 0 && now - it->lastMs > m_t.directoryTimeoutMs) continue;
        ChatRoomInfo i;
        i.id = it.key();
        i.name = it->name;
        i.topic = it->topic;
        i.users = it->users;
        out.insert(i.id, i);
    }
    for (const Room &r : m_rooms) {
        if (!r.joined && r.members.isEmpty()) continue;
        ChatRoomInfo i = roomInfo(r);
        auto ex = out.find(r.id);
        if (ex != out.end()) {
            i.users = qMax(i.users, ex->users);
            if (i.topic.isEmpty()) i.topic = ex->topic;
        }
        out.insert(r.id, i);
    }
    QList<ChatRoomInfo> l = out.values();
    std::sort(l.begin(), l.end(), [](const ChatRoomInfo &a, const ChatRoomInfo &b) {
        if (a.users != b.users) return a.users > b.users;
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    return l;
}

void OverlayPlugin::joinRoom(const QString &roomIdOrName)
{
    const QString id = normaliseRoomName(roomIdOrName);
    if (id.isEmpty()) return;
    Room &r = m_rooms[id];
    if (r.id.isEmpty()) {
        r.id = id;
        const auto d = m_directory.constFind(id);
        r.name = d != m_directory.constEnd() && !d->name.isEmpty() ? d->name : cleanText(roomIdOrName, 64).simplified();
        r.topicStr = roomTopic(id);
        if (d != m_directory.constEnd()) r.topic = d->topic;
        if (const DefaultRoom *def = defaultRoom(id)) {
            if (r.topic.isEmpty()) r.topic = QString::fromLatin1(def->topic);
            r.motd = QString::fromLatin1(kDefaultMotd);
        }
    }
    if (r.joined) {
        Q_EMIT ev(m_events)->roomJoined(roomInfo(r));
        return;
    }
    r.joined = true;
    r.joinedMs = nowMs();
    r.historyAsks = 0;
    r.historyDone = false;
    r.greeted = false;
    r.shownTopic.clear();
    r.shownMotd.clear();
    if (m_running) subscribeTopic(r.topicStr, 8 * 1024);
    Q_EMIT ev(m_events)->roomJoined(roomInfo(r));
    Q_EMIT ev(m_events)->roomUsersChanged(id);
    // Give GossipSub a heartbeat to graft us into the room mesh before announcing ourselves.
    QTimer::singleShot(qMin(m_t.historyDelayMs, 1500), this, [this, id] {
        Room *rr = room(id);
        if (!rr || !rr->joined || !m_running) return;
        QJsonObject b = newBody("join");
        b[QStringLiteral("room")] = id;
        b[QStringLiteral("files")] = m_shared.size();
        b[QStringLiteral("conn")] = connectionLabel();
        b[QStringLiteral("node")] = m_node ? m_node->nodeId() : QString();   // [ovl-2] see roomHeartbeat
        b[QStringLiteral("x")] = QString::fromLatin1(m_id.boxPublicKey().toHex());
        publishSigned(rr->topicStr, b);
        roomHeartbeat(*rr);
        // [ovl-2] tell the directory now (not up to 60 s later): nodes that relay rooms subscribe to its
        // topic, and a few early heartbeats reach members whose only path is such a relay
        QJsonObject d = newBody("room");
        d[QStringLiteral("room")] = rr->id;
        d[QStringLiteral("title")] = rr->name;
        d[QStringLiteral("topic")] = rr->topic;
        d[QStringLiteral("users")] = int(rr->members.size()) + 1;
        publishSigned(QString::fromLatin1(kTopicRooms), d);
        for (int ms : {4000, 9000, 16000})
            QTimer::singleShot(ms, this, [this, id] {
                if (Room *x = room(id); x && x->joined && m_running) roomHeartbeat(*x);
            });
    });
    QTimer::singleShot(m_t.historyDelayMs, this, [this, id] { askHistory(id); });
    // Topic and MOTD lines: as soon as a member's heartbeat (answering our join) brought the room
    // state, at the latest shortly after the history request (an empty room: what we know).
    QTimer::singleShot(m_t.historyDelayMs + 1000, this, [this, id] { greetRoom(id); });
}

void OverlayPlugin::greetRoom(const QString &roomId)
{
    Room *r = room(roomId);
    if (!r || !r->joined || r->greeted) return;
    r->greeted = true;
    showRoomState(*r);
}

// The topic / MOTD lines in the room window, once after joining and again whenever they change.
void OverlayPlugin::showRoomState(Room &r)
{
    if (!r.greeted) return;
    const qint64 now = nowMs();
    if (!r.topic.isEmpty() && r.topic != r.shownTopic) {
        r.shownTopic = r.topic;
        emitRoomMessage(r, QString(), QString(), r.topic, ChatKind::Topic, now);
    }
    if (!r.motd.isEmpty() && r.motd != r.shownMotd) {
        r.shownMotd = r.motd;
        for (const QString &line : r.motd.split(QLatin1Char('\n')))
            emitRoomMessage(r, QString(), QString(), line, ChatKind::Motd, now);
    }
}

void OverlayPlugin::leaveRoom(const QString &roomId)
{
    Room *r = room(roomId);
    if (!r || !r->joined) return;
    QJsonObject b = newBody("part");
    b[QStringLiteral("room")] = r->id;
    publishSigned(r->topicStr, b);
    // [ovl-2] a room still in the directory stays subscribed, as a relay for its members
    if (m_node && !m_directory.contains(r->id)) m_node->unsubscribe(r->topicStr);
    else m_relayTopics.insert(r->topicStr);
    r->joined = false;
    r->greeted = false;
    r->members.clear();
    r->history.clear();
    const QString id = r->id;
    Q_EMIT ev(m_events)->roomLeft(id);
}

QList<ChatUser> OverlayPlugin::roomUsers(const QString &roomId) const
{
    QList<ChatUser> out;
    const auto it = m_rooms.constFind(normaliseRoomName(roomId));
    if (it == m_rooms.constEnd()) return out;
    if (it->joined) {
        ChatUser me;
        me.user = localUser();
        me.files = m_shared.size();
        me.connection = connectionLabel();
        out << me;
    }
    for (auto m = it->members.cbegin(); m != it->members.cend(); ++m) {
        ChatUser u;
        u.user = userRefFor(m.key());
        u.user.name = m->name;
        u.files = m->files;
        u.connection = m->conn;
        out << u;
    }
    return out;
}

void OverlayPlugin::sendRoomMessage(const QString &roomId, const QString &text, ChatKind kind)
{
    Room *r = room(roomId);
    if (!r || !r->joined) return;
    const QString t = cleanText(text, kMaxText);
    if (t.trimmed().isEmpty()) return;
    QJsonObject b = newBody("msg");
    b[QStringLiteral("room")] = r->id;
    b[QStringLiteral("text")] = t;
    if (kind == ChatKind::Action) b[QStringLiteral("kind")] = QStringLiteral("action");
    const QByteArray env = makeEnvelope(r->topicStr, b);
    r->history.push_back(env);
    while (static_cast<int>(r->history.size()) > kHistoryKeep) r->history.pop_front();
    if (!m_running) return;
    m_seen.insert(jStr(b, "id", 64));
    m_node->publish(r->topicStr, env);
}

void OverlayPlugin::setTopic(const QString &roomId, const QString &topic)
{
    Room *r = room(roomId);
    if (!r || !r->joined) return;
    QJsonObject b = newBody("topic");
    b[QStringLiteral("room")] = r->id;
    b[QStringLiteral("topic")] = cleanText(topic, 256);
    r->topic = cleanText(topic, 256);
    r->topicTs = jInt(b, "ts", nowMs());
    r->topicBy = m_userId;
    r->shownTopic = r->topic;   // our own change: no echo line
    const QByteArray env = makeEnvelope(r->topicStr, b);
    r->history.push_back(env);
    while (static_cast<int>(r->history.size()) > kHistoryKeep) r->history.pop_front();
    Q_EMIT ev(m_events)->roomUpdated(roomInfo(*r));
    if (!m_running) return;
    m_seen.insert(jStr(b, "id", 64));
    m_node->publish(r->topicStr, env);
}

void OverlayPlugin::setMotd(const QString &roomId, const QString &motd)
{
    Room *r = room(roomId);
    if (!r || !r->joined) return;
    QJsonObject b = newBody("motd");
    b[QStringLiteral("room")] = r->id;
    b[QStringLiteral("motd")] = cleanMotd(motd);
    r->motd = cleanMotd(motd);
    r->motdTs = jInt(b, "ts", nowMs());
    r->motdBy = m_userId;
    r->shownMotd = r->motd;
    const QByteArray env = makeEnvelope(r->topicStr, b);
    r->history.push_back(env);
    while (static_cast<int>(r->history.size()) > kHistoryKeep) r->history.pop_front();
    Q_EMIT ev(m_events)->roomUpdated(roomInfo(*r));
    if (!m_running) return;
    m_seen.insert(jStr(b, "id", 64));
    m_node->publish(r->topicStr, env);
}

// ----------------------------------------------------------------------------------- inbound
void OverlayPlugin::onRoomEnvelope(Room &r, const QString &uid, const QJsonObject &b, const QByteArray &raw,
                                   bool fromHistory)
{
    const QString t = jStr(b, "t", 16);
    const QString room = jStr(b, "room", 64);
    if (!room.isEmpty() && room != r.id) return;
    const QString name = cleanText(jStr(b, "name", 48), 48);
    const qint64 ts = jInt(b, "ts", 0);
    const qint64 now = nowMs();
    auto keepHistory = [&] {
        r.history.push_back(raw);
        while (static_cast<int>(r.history.size()) > kHistoryKeep) r.history.pop_front();
    };
    auto member = [&](bool *added) -> Member & {
        *added = !r.members.contains(uid);
        Member &m = r.members[uid];
        if (!name.isEmpty()) m.name = name;
        m.lastMs = now;
        return m;
    };
    if (!fromHistory) {
        // [ovl-2] a live room message is presence (PROTOCOL.md section 4): the member is online, and a
        // 4.3 join / hb tells their node and box key
        touchUser(uid, b, jStr(b, "node", 64).toLower());
        UserInfo &ui = m_users[uid];
        const QString x = jStr(b, "x", 64);
        if (isHex(x, 64)) ui.xpub = QByteArray::fromHex(x.toLatin1());
        if (t == QLatin1String("join") || t == QLatin1String("hb") || t == QLatin1String("msg")) {
            ui.pingSentMs = 0;
            if (!ui.online) {
                setUserOnline(uid, true);
                pmRetryFor(uid);
            }
        }
    }

    if (t == QLatin1String("msg")) {
        const QString text = cleanText(jStr(b, "text", kMaxText), kMaxText);
        if (text.isEmpty()) return;
        if (!fromHistory) {
            if (!floodOk(uid)) {
                if (!m_floodNotified.contains(uid)) {
                    m_floodNotified.insert(uid);
                    emitRoomMessage(r, uid, name, tr("%1 is flooding the room and has been muted.").arg(name),
                                    ChatKind::System);
                }
                return;
            }
            bool added = false;
            member(&added);
            if (added) Q_EMIT ev(m_events)->roomUsersChanged(r.id);
        }
        keepHistory();
        const bool action = jStr(b, "kind", 16) == QLatin1String("action");
        emitRoomMessage(r, uid, name, text, action ? ChatKind::Action : ChatKind::Message, ts);
        return;
    }
    if (t == QLatin1String("topic")) {
        const QString topic = cleanText(jStr(b, "topic", 256), 256);
        // last writer wins (timestamp, then user id); timestamps from the future are clamped out
        if (ts > now + 5 * 60 * 1000) return;
        if (ts < r.topicTs || (ts == r.topicTs && uid <= r.topicBy)) return;
        r.topic = topic;
        r.topicTs = ts;
        r.topicBy = uid;
        keepHistory();
        Q_EMIT ev(m_events)->roomUpdated(roomInfo(r));
        showRoomState(r);
        return;
    }
    if (t == QLatin1String("motd")) {
        const QString motd = cleanMotd(jStr(b, "motd", kMaxMotd + 64));
        if (ts > now + 5 * 60 * 1000) return;
        if (ts < r.motdTs || (ts == r.motdTs && uid <= r.motdBy)) return;
        r.motd = motd;
        r.motdTs = ts;
        r.motdBy = uid;
        keepHistory();
        Q_EMIT ev(m_events)->roomUpdated(roomInfo(r));
        showRoomState(r);
        return;
    }
    if (fromHistory) return;
    if (t == QLatin1String("join") || t == QLatin1String("hb")) {
        bool added = false;
        Member &m = member(&added);
        m.files = static_cast<int>(qBound(qint64(0), jInt(b, "files", 0), qint64(10000000)));
        m.conn = cleanText(jStr(b, "conn", 24), 24);
        if (t == QLatin1String("join")) {
            emitRoomMessage(r, uid, name, tr("%1 has joined the room").arg(name), ChatKind::Join, ts);
            // answer with our heartbeat right away so the newcomer sees us without waiting
            roomHeartbeat(r);
        } else {
            bool changed = false;
            const QString topic = cleanText(jStr(b, "topic", 256), 256);
            const qint64 tts = jInt(b, "topicTs", 0);
            if (tts > r.topicTs && tts <= now + 5 * 60 * 1000 && !topic.isEmpty()) {
                r.topic = topic;
                r.topicTs = tts;
                r.topicBy = uid;
                changed = true;
            }
            const QString motd = cleanMotd(jStr(b, "motd", kMaxMotd + 64));
            const qint64 mts = jInt(b, "motdTs", 0);
            if (mts > r.motdTs && mts <= now + 5 * 60 * 1000 && !motd.isEmpty()) {
                r.motd = motd;
                r.motdTs = mts;
                r.motdBy = uid;
                changed = true;
            }
            if (changed) Q_EMIT ev(m_events)->roomUpdated(roomInfo(r));
            // a member answered our join with the room state: show it now instead of waiting
            if (!r.greeted) greetRoom(r.id);
            else showRoomState(r);
        }
        if (added || t == QLatin1String("join")) Q_EMIT ev(m_events)->roomUsersChanged(r.id);
        return;
    }
    if (t == QLatin1String("part")) {
        if (r.members.remove(uid) > 0) {
            emitRoomMessage(r, uid, name, tr("%1 has left the room").arg(name), ChatKind::Part, ts);
            Q_EMIT ev(m_events)->roomUsersChanged(r.id);
        }
        return;
    }
}

void OverlayPlugin::roomHeartbeat(Room &r)
{
    if (!m_running || !r.joined) return;
    QJsonObject b = newBody("hb");
    b[QStringLiteral("room")] = r.id;
    b[QStringLiteral("files")] = m_shared.size();
    b[QStringLiteral("conn")] = connectionLabel();
    // [ovl-2] node id and box key: a member seen only in a room (before their presence arrives) can be
    // browsed, whois'd and messaged at once (4.2 members lack them until their next presence)
    b[QStringLiteral("node")] = m_node ? m_node->nodeId() : QString();
    b[QStringLiteral("x")] = QString::fromLatin1(m_id.boxPublicKey().toHex());
    if (!r.topic.isEmpty()) {
        b[QStringLiteral("topic")] = r.topic;
        b[QStringLiteral("topicTs")] = static_cast<double>(r.topicTs);
    }
    if (!r.motd.isEmpty()) {
        b[QStringLiteral("motd")] = r.motd;
        b[QStringLiteral("motdTs")] = static_cast<double>(r.motdTs);
    }
    publishSigned(r.topicStr, b);
}

void OverlayPlugin::expireMembers()
{
    const qint64 now = nowMs();
    for (Room &r : m_rooms) {
        if (!r.joined) continue;
        bool changed = false;
        for (auto it = r.members.begin(); it != r.members.end();) {
            if (now - it->lastMs > m_t.memberTimeoutMs) {
                emitRoomMessage(r, it.key(), it->name, tr("%1 has left the room (timed out)").arg(it->name),
                                ChatKind::Part);
                it = r.members.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }
        if (changed) Q_EMIT ev(m_events)->roomUsersChanged(r.id);
    }
    for (auto it = m_directory.begin(); it != m_directory.end();) {
        if (it->lastMs >= 0 && now - it->lastMs > m_t.directoryTimeoutMs) it = m_directory.erase(it);
        else ++it;
    }
}

// --------------------------------------------------------------------------------- directory
void OverlayPlugin::announceRooms()
{
    if (!m_running) return;
    for (Room &r : m_rooms) {
        if (!r.joined) continue;
        // one announcer per room: the member with the smallest user id (cuts traffic in big rooms)
        bool smallest = true;
        for (auto it = r.members.cbegin(); it != r.members.cend(); ++it)
            if (it.key() < m_userId) { smallest = false; break; }
        DirEntry &d = m_directory[r.id];
        d.name = r.name;
        d.topic = r.topic;
        d.users = r.members.size() + 1;
        if (d.lastMs >= 0) d.lastMs = nowMs();
        if (!smallest) continue;
        QJsonObject b = newBody("room");
        b[QStringLiteral("room")] = r.id;
        b[QStringLiteral("title")] = r.name;
        b[QStringLiteral("topic")] = r.topic;
        b[QStringLiteral("users")] = r.members.size() + 1;
        publishSigned(QString::fromLatin1(kTopicRooms), b);
    }
}

void OverlayPlugin::relayRoomTopics()
{
    if (!m_node || !m_running) return;
    QSet<QString> want;
    for (auto it = m_directory.cbegin(); it != m_directory.cend() && want.size() < 64; ++it) {
        const auto r = m_rooms.constFind(it.key());
        if (r != m_rooms.constEnd() && r->joined) continue;   // subscribed as a member
        want.insert(roomTopic(it.key()));
    }
    for (const QString &t : want)
        if (!m_relayTopics.contains(t)) subscribeTopic(t, 8 * 1024);
    for (const QString &t : std::as_const(m_relayTopics)) {
        if (want.contains(t)) continue;
        bool joined = false;
        for (const Room &r : std::as_const(m_rooms)) joined |= r.joined && r.topicStr == t;
        if (!joined) m_node->unsubscribe(t);
    }
    m_relayTopics = want;
}

void OverlayPlugin::onDirectory(const QJsonObject &b)
{
    if (jStr(b, "t", 16) != QLatin1String("room")) return;
    const QString title = cleanText(jStr(b, "title", 64), 64).simplified();
    const QString id = normaliseRoomName(title);
    if (id.isEmpty() || id != jStr(b, "room", 64)) return;
    DirEntry &d = m_directory[id];
    const bool fresh = d.name.isEmpty();
    d.name = title;
    d.topic = cleanText(jStr(b, "topic", 256), 256);
    d.users = static_cast<int>(qBound(qint64(0), jInt(b, "users", 0), qint64(100000)));
    if (d.lastMs >= 0) d.lastMs = nowMs();
    if (fresh) Q_EMIT ev(m_events)->roomListChanged();
}

// ----------------------------------------------------------------------------------- history
void OverlayPlugin::askHistory(const QString &roomId)
{
    Room *r = room(roomId);
    if (!r || !r->joined || r->historyDone || !m_running) return;
    QStringList cands;
    for (const QString &p : m_node->topicPeers(r->topicStr))
        if (m_peers.contains(p) && m_peers.value(p).hello) cands << p;
    if (cands.isEmpty()) {
        if (++r->historyAsks < 8) QTimer::singleShot(2000, this, [this, roomId] { askHistory(roomId); });
        return;
    }
    QJsonObject q;
    q[QStringLiteral("t")] = QStringLiteral("hist_req");
    q[QStringLiteral("v")] = kWireVersion;
    q[QStringLiteral("room")] = r->id;
    q[QStringLiteral("n")] = kHistorySend;
    for (int i = 0; i < cands.size() && i < 2; ++i) sendRaw(cands[i], q);
    ++r->historyAsks;
}

void OverlayPlugin::onHistoryRequest(const QString &nodeId, const QJsonObject &o)
{
    Room *r = room(jStr(o, "room", 64));
    if (!r || !r->joined) return;
    const int n = static_cast<int>(qBound(qint64(1), jInt(o, "n", kHistorySend), qint64(kHistoryKeep)));
    QJsonArray items;
    const int start = qMax(0, static_cast<int>(r->history.size()) - n);
    for (int i = start; i < static_cast<int>(r->history.size()); ++i)
        items << QString::fromLatin1(r->history[static_cast<size_t>(i)].toBase64());
    QJsonObject a;
    a[QStringLiteral("t")] = QStringLiteral("hist");
    a[QStringLiteral("v")] = kWireVersion;
    a[QStringLiteral("room")] = r->id;
    a[QStringLiteral("items")] = items;
    sendRaw(nodeId, a);
}

void OverlayPlugin::onHistory(const QString &, const QJsonObject &o)
{
    Room *r = room(jStr(o, "room", 64));
    if (!r || !r->joined) return;
    r->historyDone = true;
    const QJsonArray items = jArr(o, "items", kHistoryKeep);
    for (const QJsonValue &v : items) {
        if (!v.isString() || v.toString().size() > 12000) continue;
        const QByteArray raw = QByteArray::fromBase64(v.toString().toLatin1());
        QString uid;
        QJsonObject body;
        // every replayed message carries its author's signature: a peer cannot forge history
        if (!overlayParseEnvelope(r->topicStr, raw, &uid, &body)) continue;
        if (uid == m_userId) continue;   // our own lines: the core keeps its own backlog
        if (!m_seen.insert(jStr(body, "id", 64))) continue;
        if (nowMs() - jInt(body, "ts", 0) > 24LL * 3600 * 1000) continue;   // stale
        onRoomEnvelope(*r, uid, body, raw, true);
    }
}

} // namespace mx::ovl
