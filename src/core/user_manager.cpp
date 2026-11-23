// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/user_manager.h"
#include "core/dao.h"
#include "core/network_manager.h"

#include <QtCore/QDateTime>
#include <QtCore/QTimer>

#include <algorithm>

namespace mx {

UserManager::UserManager(Settings *settings, NetworkManager *network, UserListsDao *dao, QObject *parent)
    : UserService(parent)
    , m_settings(settings)
    , m_network(network)
    , m_dao(dao)
{
    if (!m_network) return;
    for (NetworkPlugin *p : m_network->plugins()) {
        // presence from every chat-capable plugin updates the hotlist
        if (auto *backend = dynamic_cast<ChatBackend *>(p)) {
            if (auto *ce = qobject_cast<ChatEvents *>(backend->chatEvents())) {
                connect(ce, &ChatEvents::userPresence, this,
                        [this](const UserRef &u, bool online, int files, const QString &conn) {
                            setPresence(u, online, files, conn);
                        });
            }
        }
        connect(p, &NetworkPlugin::browseResults, this,
                [this](quint32 id, const UserRef &u, const QList<BrowseEntry> &files, bool finished) {
                    if (!m_browses.contains(id)) return;     // cancelled / unknown
                    if (finished) m_browses.remove(id);
                    emit browseResults(id, u, files, finished);
                });
        connect(p, &NetworkPlugin::browseFailed, this, [this](quint32 id, const UserRef &u, const QString &reason) {
            if (!m_browses.remove(id)) return;
            emit browseFailed(id, u, reason);
        });
        connect(p, &NetworkPlugin::whoisResult, this, [this](const UserRef &u, const QVariantMap &info) {
            const int i = indexOf(u);
            if (i >= 0) {
                HotlistEntry &e = m_hotlist[i];
                if (info.contains(QStringLiteral("files"))) e.files = info.value(QStringLiteral("files")).toInt();
                if (info.contains(QStringLiteral("connection")))
                    e.connection = info.value(QStringLiteral("connection")).toString();
                // [ui-g] the answer tells whether the user is online now (presence is only announced every
                // two minutes, so a user added while online looked offline until then): the plugin's own
                // "online" / "reachable", else an answer without error came from the user
                bool known = true, online = false;
                if (info.contains(QStringLiteral("online")))
                    online = info.value(QStringLiteral("online")).toBool() || info.value(QStringLiteral("reachable")).toBool();
                else if (info.contains(QStringLiteral("reachable")))
                    online = info.value(QStringLiteral("reachable")).toBool();
                else if (!info.contains(QStringLiteral("error")))
                    online = true;
                else
                    known = false;
                if (known) {
                    e.online = online;
                    if (online) e.lastSeen = QDateTime::currentDateTime();
                }
                if (m_dao) m_dao->updateHotlist(e);
                emit hotlistChanged();
            }
            emit whoisResult(u, info);
            answerWhois(u, info);
        });
    }
}

void UserManager::start()
{
    if (!m_dao) {
        pushHotlist();
        return;
    }
    m_hotlist = m_dao->hotlist();
    m_ignored = m_dao->ignored();
    emit hotlistChanged();
    emit ignoredChanged();
    pushHotlist();
}

void UserManager::pushHotlist()
{
    if (!m_network) return;
    QList<UserRef> users;
    for (const HotlistEntry &e : std::as_const(m_hotlist)) users << e.user;
    for (NetworkPlugin *p : m_network->plugins()) p->setHotlist(users);
}

int UserManager::indexOf(const UserRef &u) const
{
    for (int i = 0; i < m_hotlist.size(); ++i)
        if (m_hotlist.at(i).user.key() == u.key()) return i;
    return -1;
}

bool UserManager::isOnHotlist(const UserRef &u) const
{
    if (indexOf(u) >= 0) return true;
    for (const HotlistEntry &e : m_hotlist)
        if (!u.name.isEmpty() && e.user.net == u.net && e.user.name.compare(u.name, Qt::CaseInsensitive) == 0) return true;
    return false;
}

UserRef UserManager::findByName(const QString &name) const
{
    for (const HotlistEntry &e : m_hotlist)
        if (e.user.name.compare(name, Qt::CaseInsensitive) == 0 || e.user.id == name) return e.user;
    for (const UserRef &u : m_ignored)
        if (u.name.compare(name, Qt::CaseInsensitive) == 0 || u.id == name) return u;
    return UserRef();
}

void UserManager::setPresence(const UserRef &u, bool online, int files, const QString &connection)
{
    const int i = indexOf(u);
    if (i < 0) return;
    HotlistEntry &e = m_hotlist[i];
    if (e.online == online && (files < 0 || e.files == files) && (connection.isEmpty() || e.connection == connection)
        && e.lastSeen.isValid() && e.lastSeen.secsTo(QDateTime::currentDateTime()) < 60)
        return;   // nothing new (lastSeen refreshed at most once a minute)
    e.online = online;
    if (files >= 0) e.files = files;
    if (!connection.isEmpty()) e.connection = connection;
    if (!u.name.isEmpty()) e.user.name = u.name;
    if (!u.address.isEmpty()) e.user.address = u.address;
    e.lastSeen = QDateTime::currentDateTime();
    if (m_dao) m_dao->updateHotlist(e);
    emit hotlistChanged();
}

QList<HotlistEntry> UserManager::hotlist() const
{
    return m_hotlist;
}

bool UserManager::isSelf(const UserRef &u) const
{
    if (!m_network || u.id.isEmpty()) return false;
    for (NetworkPlugin *p : m_network->plugins()) {
        if (p->net() != u.net) continue;
        const UserRef me = p->localUser();
        if (me.isValid() && me.id.compare(u.id, Qt::CaseInsensitive) == 0) return true;
    }
    return false;
}

void UserManager::addToHotlist(const UserRef &u)
{
    if (!u.isValid() || indexOf(u) >= 0) return;
    if (isSelf(u)) return;   // [ovl-2] not ourselves (the UI disables the entry)
    HotlistEntry e;
    e.user = u;
    if (e.user.name.isEmpty()) e.user.name = u.id;
    m_hotlist << e;
    if (m_dao) m_dao->addHotlist(e);
    emit hotlistChanged();
    pushHotlist();
    // [ui-g] learn at once whether the new entry is online: a whois answer marks it (see above)
    if (pluginFor(u, NetworkPlugin::CanBrowse) || pluginFor(u, NetworkPlugin::CanChat))
        QTimer::singleShot(0, this, [this, u] { whois(u); });
}

void UserManager::removeFromHotlist(const UserRef &u)
{
    const int i = indexOf(u);
    if (i < 0) return;
    m_hotlist.removeAt(i);
    if (m_dao) m_dao->removeHotlist(u);
    emit hotlistChanged();
    pushHotlist();
}

void UserManager::setNote(const UserRef &u, const QString &note)
{
    const int i = indexOf(u);
    if (i < 0) return;
    m_hotlist[i].note = note;
    if (m_dao) m_dao->updateHotlist(m_hotlist[i]);
    emit hotlistChanged();
}

QList<UserRef> UserManager::ignored() const
{
    return m_ignored;
}

void UserManager::ignore(const UserRef &u, bool on)
{
    if (u.id.isEmpty() && u.name.isEmpty()) return;
    if (on && isSelf(u)) return;   // [ovl-2] ignoring ourselves would hide our own lines
    UserRef r = u;
    if (r.id.isEmpty()) r.id = r.name;
    if (r.name.isEmpty()) r.name = r.id;
    int idx = -1;
    for (int i = 0; i < m_ignored.size(); ++i)
        if (m_ignored.at(i).key() == r.key()) idx = i;
    if (on == (idx >= 0)) return;
    if (on) m_ignored << r;
    else m_ignored.removeAt(idx);
    if (m_dao) m_dao->setIgnored(r, on);
    emit ignoredChanged();
}

bool UserManager::isIgnored(const UserRef &u) const
{
    for (const UserRef &i : m_ignored) {
        if (i.net != u.net && i.net != Net::Unknown) continue;
        if (!u.id.isEmpty() && i.id == u.id) return true;
        // an entry added by name only (/ignore Bob) matches that screen name
        if (i.id == i.name && !u.name.isEmpty() && i.name.compare(u.name, Qt::CaseInsensitive) == 0) return true;
    }
    return false;
}

NetworkPlugin *UserManager::pluginFor(const UserRef &u, int capability) const
{
    if (!m_network) return nullptr;
    const QList<NetworkPlugin *> active = m_network->activePlugins(capability);
    for (NetworkPlugin *p : active)
        if (p->net() == u.net) return p;
    for (NetworkPlugin *p : active)
        if (p->net() == Net::Overlay) return p;
    return nullptr;
}

quint32 UserManager::browse(const UserRef &u)
{
    const quint32 id = m_nextBrowse++;
    NetworkPlugin *p = pluginFor(u, NetworkPlugin::CanBrowse);
    if (!p) {
        QTimer::singleShot(0, this, [this, id, u] {
            emit browseFailed(id, u, QStringLiteral("Browsing is not available for %1 users right now.").arg(netName(u.net)));
        });
        return id;
    }
    m_browses.insert(id, p);
    p->browse(id, u);
    return id;
}

void UserManager::cancelBrowse(quint32 requestId)
{
    m_browses.remove(requestId);
}

void UserManager::whois(const UserRef &u)
{
    NetworkPlugin *p = pluginFor(u, NetworkPlugin::CanBrowse);
    if (!p) p = pluginFor(u, NetworkPlugin::CanChat);
    if (!p) {
        QTimer::singleShot(0, this, [this, u] {
            QVariantMap info;
            info.insert(QStringLiteral("name"), u.name.isEmpty() ? u.id : u.name);
            info.insert(QStringLiteral("network"), netName(u.net));
            info.insert(QStringLiteral("error"), QStringLiteral("No whois service is available for this user."));
            emit whoisResult(u, info);
            answerWhois(u, info);
        });
        return;
    }
    p->whois(u);
}

// ---------------------------------------------------------------- [glue-3] whois with request ids
quint32 UserManager::requestWhois(const UserRef &u)
{
    const quint32 id = m_nextWhois++;
    if (m_nextWhois == 0) m_nextWhois = 1;
    m_whois.insert(id, PendingWhois{u, QDateTime::currentMSecsSinceEpoch() + m_whoisTimeoutMs});
    if (!m_whoisTimer) {
        m_whoisTimer = new QTimer(this);
        m_whoisTimer->setInterval(qBound(10, m_whoisTimeoutMs / 4, 1000));
        connect(m_whoisTimer, &QTimer::timeout, this, &UserManager::expireWhois);
    }
    if (!m_whoisTimer->isActive()) m_whoisTimer->start();
    whois(u);
    return id;
}

void UserManager::answerWhois(const UserRef &u, const QVariantMap &info)
{
    // Every pending request for this user gets this answer (once); requests for other users never do.
    QList<quint32> ids;
    for (auto it = m_whois.constBegin(); it != m_whois.constEnd(); ++it)
        if (it->user.net == u.net && it->user.id == u.id) ids << it.key();
    std::sort(ids.begin(), ids.end());
    for (quint32 id : ids) {
        m_whois.remove(id);
        QVariantMap m = info;
        m.insert(QStringLiteral("requestId"), id);
        emit whoisReply(id, u, m);
    }
    if (m_whois.isEmpty() && m_whoisTimer) m_whoisTimer->stop();
}

void UserManager::expireWhois()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QList<quint32> ids;
    for (auto it = m_whois.constBegin(); it != m_whois.constEnd(); ++it)
        if (it->deadlineMs <= now) ids << it.key();
    std::sort(ids.begin(), ids.end());
    for (quint32 id : ids) {
        const UserRef u = m_whois.take(id).user;
        QVariantMap info;
        info.insert(QStringLiteral("name"), u.name.isEmpty() ? u.id : u.name);
        info.insert(QStringLiteral("network"), netName(u.net));
        info.insert(QStringLiteral("error"), QStringLiteral("The user did not answer."));
        info.insert(QStringLiteral("requestId"), id);
        emit whoisReply(id, u, info);
    }
    if (m_whois.isEmpty() && m_whoisTimer) m_whoisTimer->stop();
}

} // namespace mx
