// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX core: UserService implementation: hotlist + ignore list (persisted in the DB; work with
// or without any plugin), browse / whois routed to the plugin of the user's network that has
// CanBrowse (falling back to the overlay plugin), presence (ChatEvents::userPresence of every
// ChatBackend plugin) → HotlistEntry online/files/connection/lastSeen, and the hotlist pushed to
// every plugin (NetworkPlugin::setHotlist) at start and whenever it changes.
#pragma once

#include <QtCore/QHash>
#include <QtCore/QPointer>
#include <QtCore/QTimer>

#include "mx/plugin.h"
#include "mx/services.h"

namespace mx {

class NetworkManager;
class UserListsDao;

class UserManager : public UserService {
    Q_OBJECT
public:
    // network and dao may be null (tests / no DB).
    UserManager(Settings *settings, NetworkManager *network, UserListsDao *dao, QObject *parent = nullptr);

    void start();                              // load lists from the DB
    // Presence from the chat backend or a plugin; updates the hotlist entry if the user is on it.
    void setPresence(const UserRef &u, bool online, int files, const QString &connection);
    // Find a known user by screen name (hotlist, ignore list); invalid UserRef if unknown.
    UserRef findByName(const QString &name) const;
    bool isOnHotlist(const UserRef &u) const;
    bool isSelf(const UserRef &u) const override;   // [ovl-2] by user id (NetworkPlugin::localUser)

    QList<HotlistEntry> hotlist() const override;
    void addToHotlist(const UserRef &u) override;
    void removeFromHotlist(const UserRef &u) override;
    void setNote(const UserRef &u, const QString &note) override;
    QList<UserRef> ignored() const override;
    void ignore(const UserRef &u, bool on) override;
    bool isIgnored(const UserRef &u) const override;
    quint32 browse(const UserRef &u) override;
    void cancelBrowse(quint32 requestId) override;
    void whois(const UserRef &u) override;
    quint32 requestWhois(const UserRef &u) override;   // [glue-3] → whoisReply(id, ...) exactly once
    void setWhoisTimeoutMs(int ms) { m_whoisTimeoutMs = ms; }   // default 30 s (tests: shorter)

private:
    NetworkPlugin *pluginFor(const UserRef &u, int capability) const;
    int indexOf(const UserRef &u) const;
    void pushHotlist();                        // NetworkPlugin::setHotlist on every plugin
    void pushIgnored();                        // [net-3] NetworkPlugin::setIgnored on every plugin

    Settings *m_settings;
    NetworkManager *m_network;
    UserListsDao *m_dao;
    QList<HotlistEntry> m_hotlist;
    QList<UserRef> m_ignored;
    quint32 m_nextBrowse = 1;
    QHash<quint32, QPointer<NetworkPlugin>> m_browses;
    struct PendingWhois { UserRef user; qint64 deadlineMs = 0; };
    QHash<quint32, PendingWhois> m_whois;      // [glue-3] request id → user
    quint32 m_nextWhois = 1;
    int m_whoisTimeoutMs = 30000;
    QTimer *m_whoisTimer = nullptr;
    void answerWhois(const UserRef &u, const QVariantMap &info);
    void expireWhois();
};

} // namespace mx
