// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/network_manager.h"
#include "core/log.h"
#include "core/pinger.h"
#include "core/server_list.h"

#include "mx/net_policy.h"
#include "mx/settings_keys.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QRandomGenerator>
#include <QtCore/QRegularExpression>

namespace mx {

NetworkManager::NetworkManager(Settings *settings, const QList<NetworkPlugin *> &plugins, QObject *parent)
    : NetworkService(parent)
    , m_settings(settings)
{
    for (NetworkPlugin *p : plugins) {
        if (!p) continue;
        p->setParent(this);
        m_plugins << p;
        m_running.insert(p, false);
        connect(p, &NetworkPlugin::statusChanged, this, [this, p](const NetworkStatus &) { onPluginStatus(p); });
        connect(p, &NetworkPlugin::serversChanged, this, [this, p] { emit serversChanged(p->net()); });
        connect(p, &NetworkPlugin::logMessage, this, [this, p](const QString &t) {
            qCInfo(lcNet).noquote() << netName(p->net()) + QLatin1String(": ") + t;
            emit logMessage(p->net(), t);
        });
        // [glue-2] server messages + incoming search counter
        connect(p, &NetworkPlugin::serverMessage, this, [this, p](const QString &serverId, const QString &text) {
            ServerMessage m;
            m.net = p->net();
            m.serverId = serverId;
            m.text = text;
            m.time = QDateTime::currentDateTime();
            for (const ServerEntry &e : p->servers())
                if (e.id == serverId) { m.serverName = e.name; break; }
            QList<ServerMessage> &l = m_messages[int(m.net)];
            l << m;
            while (l.size() > 200) l.removeFirst();
            emit serverMessageReceived(m);
        });
        connect(p, &NetworkPlugin::incomingSearches, this, [this](int n) {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            for (int i = 0; i < qBound(0, n, 1000); ++i) m_incoming.push_back(now);
            while (m_incoming.size() > 100000) m_incoming.pop_front();
        });
    }
    // [net-2] the proxy / adapter policy: a network that may not run under it is not started
    // (or is stopped), and starts again when it may; a change of the policy restarts the networks
    m_policy = new netpolicy::Monitor(m_settings, this);
    m_policyPrint = m_policy->policy().fingerprint() + m_policy->adapter().fingerprint();
    m_policyTimer.setSingleShot(true);
    m_policyTimer.setInterval(500);   // a few settings in a row (the proxy page) restart once
    connect(&m_policyTimer, &QTimer::timeout, this, &NetworkManager::applyPolicy);
    connect(m_policy, &netpolicy::Monitor::changed, this, [this] { m_policyTimer.start(); });
    m_summaryTimer.setInterval(1000);
    connect(&m_summaryTimer, &QTimer::timeout, this, &NetworkManager::checkSummary);
    m_summaryTimer.start();
    std::stable_sort(m_plugins.begin(), m_plugins.end(),
                     [](NetworkPlugin *a, NetworkPlugin *b) { return int(a->net()) < int(b->net()); });

    // first run: WinMX-style default screen name
    if (m_settings->s(keys::identity::ScreenName).trimmed().isEmpty())
        m_settings->setValue(keys::identity::ScreenName, generateScreenName());

    connect(m_settings, &Settings::changed, this, [this](const QString &key, const QVariant &v) {
        if (key == keys::identity::ScreenName) {
            // [ovl-2] whoever wrote it (Settings page, wizard, an old profile): keep the one name
            if (m_inited) {
                const QString c = canonicalScreenName(v.toString());
                if (!c.isEmpty() && c != v.toString()) {
                    m_settings->setValue(keys::identity::ScreenName, c);
                    return;
                }
            }
            emit screenNameChanged(v.toString());
            return;
        }
        if (!key.endsWith(QLatin1String("/enabled"))) return;
        for (NetworkPlugin *p : std::as_const(m_plugins)) {
            if (keys::enabledKey(p->net()) != key) continue;
            if (!v.toBool()) stopPlugin(p);
            else if (m_online && m_inited) startPlugin(p);
            emit networkChanged(status(p->net()));
            emitOverallIfChanged();
        }
    });
}

NetworkManager::~NetworkManager()
{
    stopAll();
    qDeleteAll(m_plugins);
    m_plugins.clear();
}

QString NetworkManager::generateScreenName()
{
    auto *rng = QRandomGenerator::global();
    return QStringLiteral("Guest%1_%2").arg(rng->bounded(100, 1000)).arg(rng->bounded(10000, 100000));
}

QString NetworkManager::sanitizeScreenName(const QString &name)
{
    QString s = name.trimmed();
    static const QRegularExpression bad(QStringLiteral("[\\s\\\\/:*?\"<>|@%]+"));
    s.replace(bad, QStringLiteral("_"));
    return s.left(32);
}

void NetworkManager::initPlugins(const QString &profileDir, LibraryAccess *library, RateLimiter *limiter)
{
    if (m_inited) return;
    m_inited = true;
    for (NetworkPlugin *p : std::as_const(m_plugins)) {
        PluginContext ctx;
        ctx.settings = m_settings;
        ctx.library = library;
        ctx.limiter = limiter;
        ctx.uploadSlots = m_uploadSlots;   // [net-3] one total over the queued-upload networks
        ctx.profileDir = profileDir;
        const QString prefix = settingsPrefixForNet(p->net());
        ctx.dataDir = profileDir + QLatin1Char('/') + (prefix.isEmpty() ? QStringLiteral("net") : prefix);
        QDir().mkpath(ctx.dataDir);
        ctx.screenName = screenName();
        ctx.clientName = QStringLiteral("OctoMX");
        ctx.clientVersion = QStringLiteral(MX_VERSION_STRING);
        ctx.coreMapsPorts = true;   // [diag-1] the DiagnosticsManager's PortMapper forwards every listening port
        qCInfo(lcNet) << "init plugin" << netName(p->net());
        p->init(ctx);
    }
    if (m_haveShared) setSharedFiles(m_lastShared);
    // [ovl-2] existing profiles: the stored name had a random suffix, the OctoMX network announced the
    // key-derived one; from now on both are the same
    const QString c = canonicalScreenName(screenName());
    if (!c.isEmpty() && c != screenName()) {
        qCInfo(lcNet) << "screen name" << screenName() << "is now" << c << "(the name the OctoMX network announces)";
        m_settings->setValue(keys::identity::ScreenName, c);
    }
}

QString NetworkManager::identitySuffix() const
{
    for (NetworkPlugin *p : m_plugins) {
        const QString n = p->localUser().name;
        const int us = n.lastIndexOf(QLatin1Char('_'));
        if (us > 0 && n.size() - us == 6) return n.mid(us + 1);
    }
    return QString();
}

QString NetworkManager::canonicalScreenName(const QString &raw) const
{
    QString s = sanitizeScreenName(raw);
    if (s.isEmpty()) return QString();
    static const QRegularExpression suffix(QStringLiteral("_\\d{5}$"));
    const bool hadSuffix = suffix.match(s).hasMatch();
    QString base = hadSuffix ? s.left(s.size() - 6) : s;
    base = base.left(26);
    if (base.isEmpty()) base = QStringLiteral("Guest");
    QString sfx = identitySuffix();
    if (sfx.isEmpty()) {   // no OctoMX identity (overlay off / not initialised): the old per-install suffix
        if (hadSuffix) return base + s.right(6);
        sfx = QString::number(10000 + qHash(m_settings->s(keys::internal::InstallId)) % 90000);
    }
    return base + QLatin1Char('_') + sfx;
}

void NetworkManager::startNetworks(bool force)
{
    if (!force && !m_settings->b(keys::network::ConnectOnStart)) {
        emitOverallIfChanged();
        return;
    }
    connectAll();
}

void NetworkManager::stopAll()
{
    for (NetworkPlugin *p : std::as_const(m_plugins)) stopPlugin(p);
    m_online = false;
}

NetworkPlugin *NetworkManager::plugin(Net n) const
{
    for (NetworkPlugin *p : m_plugins)
        if (p->net() == n) return p;
    return nullptr;
}

bool NetworkManager::isRunning(Net n) const
{
    NetworkPlugin *p = plugin(n);
    return p && m_running.value(p);
}

QList<NetworkPlugin *> NetworkManager::activePlugins(int capability) const
{
    QList<NetworkPlugin *> out;
    for (NetworkPlugin *p : m_plugins) {
        if (!m_running.value(p) || !isEnabled(p->net())) continue;
        if (capability && !(p->capabilities() & capability)) continue;
        out << p;
    }
    return out;
}

void NetworkManager::setSharedFiles(const QList<SharedFile> &files)
{
    m_lastShared = files;
    m_haveShared = true;
    if (!m_inited) return;
    for (NetworkPlugin *p : std::as_const(m_plugins))
        if (p->capabilities() & NetworkPlugin::CanServe) p->setSharedFiles(files);
}

void NetworkManager::startPlugin(NetworkPlugin *p)
{
    if (!m_inited || m_running.value(p) || !isEnabled(p->net())) return;
    const netpolicy::Verdict v = m_policy->verdict(p->net());
    if (!v.allowed) {
        if (m_blocked.value(p) != v.reason) {
            m_blocked.insert(p, v.reason);
            qCInfo(lcNet).noquote() << netName(p->net()) << "not started:" << v.reason;
            emit logMessage(p->net(), v.reason);
        }
        emit networkChanged(status(p->net()));
        return;
    }
    m_blocked.remove(p);
    qCInfo(lcNet) << "starting" << netName(p->net());
    m_running[p] = true;
    p->start();
    emit networkChanged(status(p->net()));
}

void NetworkManager::stopPlugin(NetworkPlugin *p)
{
    if (!m_running.value(p)) return;
    qCInfo(lcNet) << "stopping" << netName(p->net());
    m_running[p] = false;
    p->stop();
    emit networkChanged(status(p->net()));
}

QString NetworkManager::blockedReason(Net n) const
{
    NetworkPlugin *p = plugin(n);
    return p ? m_blocked.value(p) : QString();
}

void NetworkManager::applyPolicy()
{
    m_policy->recheck();
    const QString print = m_policy->policy().fingerprint() + m_policy->adapter().fingerprint();
    const bool restart = print != m_policyPrint;
    m_policyPrint = print;
    for (NetworkPlugin *p : std::as_const(m_plugins)) {
        const netpolicy::Verdict v = m_policy->verdict(p->net());
        if (!v.allowed) {
            const bool was = m_running.value(p);
            if (was) stopPlugin(p);
            const bool news = m_blocked.value(p) != v.reason;
            if (was || m_online || m_blocked.contains(p)) m_blocked.insert(p, v.reason);
            if (news && (was || m_online)) {
                qCInfo(lcNet).noquote() << netName(p->net()) << (was ? "stopped:" : "not started:") << v.reason;
                emit logMessage(p->net(), v.reason);
            }
            emit networkChanged(status(p->net()));
            continue;
        }
        const bool wasBlocked = m_blocked.remove(p) > 0;
        if (m_running.value(p) && restart) {
            qCInfo(lcNet) << "restarting" << netName(p->net()) << "for the new connection settings";
            stopPlugin(p);
            startPlugin(p);
        } else if (!m_running.value(p) && wasBlocked && m_online) {
            startPlugin(p);
        } else if (wasBlocked) {
            emit networkChanged(status(p->net()));
        }
    }
    emitOverallIfChanged();
}

void NetworkManager::onPluginStatus(NetworkPlugin *p)
{
    emit networkChanged(status(p->net()));
    emitOverallIfChanged();
}

void NetworkManager::emitOverallIfChanged()
{
    const QString key = QString::number(int(overallState())) + overallHeadline() + overallDetail() + titleSummary();
    if (key == m_lastOverall) return;
    m_lastOverall = key;
    emit overallChanged();
}

// ---------------------------------------------------------------- NetworkService
QList<NetworkStatus> NetworkManager::networks() const
{
    QList<NetworkStatus> out;
    for (NetworkPlugin *p : m_plugins) out << status(p->net());
    return out;
}

NetworkStatus NetworkManager::status(Net n) const
{
    NetworkPlugin *p = plugin(n);
    if (!p) {
        NetworkStatus st;
        st.net = n;
        st.state = ConnState::Disabled;
        st.statusText = QStringLiteral("Not available");
        return st;
    }
    NetworkStatus st = p->status();
    st.net = n;
    if (!isEnabled(n)) {
        st.state = ConnState::Disabled;
        st.statusText = QStringLiteral("Disabled");
    } else if (!m_running.value(p) && m_blocked.contains(p)) {
        st.state = ConnState::Error;   // [net-2] the proxy / adapter policy keeps it off
        st.statusText = m_blocked.value(p);
    } else if (!m_running.value(p) && st.state != ConnState::Error) {
        st.state = ConnState::Disconnected;
        if (st.statusText.isEmpty() || st.statusText.startsWith(QLatin1String("Connect")))
            st.statusText = QStringLiteral("Not connected");
    }
    return st;
}

ConnState NetworkManager::overallState() const
{
    bool anyEnabled = false, connecting = false, error = false;
    for (NetworkPlugin *p : m_plugins) {
        const NetworkStatus st = status(p->net());
        if (st.state == ConnState::Disabled) continue;
        anyEnabled = true;
        if (st.state == ConnState::Connected) return ConnState::Connected;
        if (st.state == ConnState::Connecting) connecting = true;
        if (st.state == ConnState::Error) error = true;
    }
    if (!anyEnabled) return ConnState::Disabled;
    if (connecting) return ConnState::Connecting;
    if (error && m_online) return ConnState::Error;
    return ConnState::Disconnected;
}

QString NetworkManager::overallHeadline() const
{
    // original WinMX wording (docs/UI_SPEC.md §3.1.1): green / yellow / red lamp
    switch (overallState()) {
    case ConnState::Connected:    return QStringLiteral("Ready");
    case ConnState::Connecting:   return QStringLiteral("Finding network connections");
    case ConnState::Error:
    case ConnState::Disconnected:
    case ConnState::Disabled:     return QStringLiteral("Not connected");
    }
    return QString();
}

QString NetworkManager::overallDetail() const
{
    switch (overallState()) {
    case ConnState::Connected:
        return QStringLiteral("You can search and download now.");
    case ConnState::Connecting:
        return QStringLiteral("Connecting: searching and downloading work in a moment...");
    case ConnState::Error: {
        // [net-2] the adapter is missing / the proxy is not reached through it: say so
        const netpolicy::Verdict v = m_policy->verdict(Net::Unknown);
        if (!v.allowed) return v.reason + QStringLiteral(". Transfers continue when it is back.");
        return QStringLiteral("OctoMX could not connect to any network. It will keep trying.");
    }
    case ConnState::Disconnected:
        return QStringLiteral("Click Connect to join the network.");
    case ConnState::Disabled:
        return m_plugins.isEmpty() ? QStringLiteral("No networks are available.")
                                   : QStringLiteral("All networks are disabled. Enable one in Settings.");
    }
    return QString();
}

void NetworkManager::connectAll()
{
    m_online = true;
    for (NetworkPlugin *p : std::as_const(m_plugins)) startPlugin(p);
    emitOverallIfChanged();
}

void NetworkManager::disconnectAll()
{
    for (NetworkPlugin *p : std::as_const(m_plugins)) stopPlugin(p);
    m_online = false;
    m_blocked.clear();   // [net-2] offline by choice now
    emitOverallIfChanged();
}

void NetworkManager::setEnabled(Net n, bool on)
{
    const QString key = keys::enabledKey(n);
    if (key.isEmpty()) return;
    m_settings->setValue(key, on);   // the changed() handler starts/stops the plugin
}

bool NetworkManager::isEnabled(Net n) const
{
    const QString key = keys::enabledKey(n);
    return !key.isEmpty() && m_settings->b(key);
}

void NetworkManager::connectNetwork(Net n)
{
    NetworkPlugin *p = plugin(n);
    if (!p) return;
    if (!isEnabled(n)) m_settings->setValue(keys::enabledKey(n), true);
    m_online = true;
    startPlugin(p);
    emitOverallIfChanged();
}

void NetworkManager::disconnectNetwork(Net n)
{
    if (NetworkPlugin *p = plugin(n)) {
        stopPlugin(p);
        m_blocked.remove(p);
    }
    emitOverallIfChanged();
}

QList<ServerEntry> NetworkManager::servers(Net n) const
{
    QList<ServerEntry> out;
    for (NetworkPlugin *p : m_plugins) {
        if (n != Net::Unknown && p->net() != n) continue;
        for (ServerEntry e : p->servers()) {
            if (e.net == Net::Unknown) e.net = p->net();
            out << e;
        }
    }
    return out;
}

void NetworkManager::addServer(Net n, const QString &address, const QString &name)
{
    if (NetworkPlugin *p = plugin(n)) p->addServer(address, name);
}

void NetworkManager::removeServer(Net n, const QString &id)
{
    if (NetworkPlugin *p = plugin(n)) p->removeServer(id);
}

void NetworkManager::connectServer(Net n, const QString &id)
{
    if (NetworkPlugin *p = plugin(n)) p->connectServer(id);
}

void NetworkManager::disconnectServer(Net n, const QString &id)
{
    if (NetworkPlugin *p = plugin(n)) p->disconnectServer(id);
}

QString NetworkManager::screenName() const
{
    return m_settings->s(keys::identity::ScreenName);
}

void NetworkManager::setScreenName(const QString &name)
{
    // like the original WinMX, every screen name ends in "_<5 digits>"; [ovl-2] the digits are the ones the OctoMX
    // network derives from the user key (the name others see), a typed suffix is replaced
    const QString s = canonicalScreenName(name);
    if (s.isEmpty()) return;
    if (s == screenName()) return;
    m_settings->setValue(keys::identity::ScreenName, s);  // emits screenNameChanged via changed()
}

QString NetworkManager::titleSummary() const
{
    if (overallState() != ConnState::Connected) {
        return overallState() == ConnState::Connecting ? QStringLiteral("Connecting...") : QStringLiteral("Offline");
    }
    // [engine-4] P = primary connections (hubs we search through), S = secondary (peers we exchange
    // data / queue slots with): the numbers the original WinMX showed; see NetworkSummary
    int primary = 0;
    for (NetworkPlugin *p : m_plugins) {
        const NetworkStatus st = status(p->net());
        if (st.state == ConnState::Connected) primary += primaryOf(st);
    }
    const int secondary = m_secondaryCounter ? m_secondaryCounter() : (m_sourceCounter ? m_sourceCounter() : 0);
    return QStringLiteral("Online P=%1 S=%2").arg(primary).arg(secondary);
}

int NetworkManager::primaryOf(const NetworkStatus &st)
{
    if (st.state != ConnState::Connected) return 0;
    if (st.primaryLinks >= 0) return st.primaryLinks;   // [core-5] the plugin knows its real links
    switch (st.net) {
    case Net::ED2K:        // connected servers
    case Net::Gnutella:    // ultrapeers we are a leaf of
    case Net::Overlay:     // OctoMX overlay peers
        return qMax(0, st.peers);
    case Net::Kad:         // a DHT is one "connection", not its routing table
    case Net::BitTorrent:
    case Net::Rats:        // [core-5] the rats-search swarm is one link, its peers are index nodes
    case Net::Web:         // [core-5] the torrent index sites: asked per search over HTTP, no link
        return st.peers > 0 ? 1 : 0;
    default:               // HTTP: no hub
        return 0;
    }
}

// ---------------------------------------------------------------- [glue-2]
namespace {
int countSince(std::deque<qint64> &q, qint64 since)
{
    while (!q.empty() && q.front() < since) q.pop_front();
    return int(q.size());
}
} // namespace

void NetworkManager::noteOwnSearch()
{
    m_ownSearches.push_back(QDateTime::currentMSecsSinceEpoch());
    while (m_ownSearches.size() > 10000) m_ownSearches.pop_front();
}

NetworkSummary NetworkManager::summary() const
{
    NetworkSummary out;
    out.state = overallState();
    bool anyOpen = false;
    int connected = 0;
    for (NetworkPlugin *p : m_plugins) {
        const NetworkStatus st = status(p->net());
        if (st.state != ConnState::Disabled) ++out.networksEnabled;
        if (st.state != ConnState::Connected) continue;
        ++out.networksOnline;
        ++connected;
        const int prim = primaryOf(st);
        out.primary += prim;
        out.primaryByNet.insert(p->net(), prim);
        out.peersByNet.insert(p->net(), st.peers);
        if (!st.firewalled) anyOpen = true;
        if (st.usersOnline >= 0) out.usersOnline = qMax<qint64>(0, out.usersOnline) + st.usersOnline;
        if (st.filesOnline >= 0) out.filesOnline = qMax<qint64>(0, out.filesOnline) + st.filesOnline;
    }
    out.firewalled = connected > 0 && !anyOpen;
    out.sources = m_sourceCounter ? m_sourceCounter() : 0;
    out.knownSources = out.sources;
    out.secondary = m_secondaryCounter ? m_secondaryCounter() : 0;
    const qint64 since = QDateTime::currentMSecsSinceEpoch() - 60000;
    auto *self = const_cast<NetworkManager *>(this);   // trimming the windows is not observable state
    out.searchesPerMinute = countSince(self->m_incoming, since);
    out.ownSearchesPerMinute = countSince(self->m_ownSearches, since);
    return out;
}

void NetworkManager::checkSummary()
{
    const NetworkSummary s = summary();
    const QString key = QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8|%9")
                            .arg(int(s.state)).arg(s.primary).arg(s.sources).arg(s.searchesPerMinute)
                            .arg(s.ownSearchesPerMinute).arg(s.networksOnline).arg(s.networksEnabled)
                            .arg(s.usersOnline).arg(s.firewalled ? 1 : 0)
                            + QStringLiteral("|%1").arg(s.secondary);
    if (key == m_lastSummary) return;
    m_lastSummary = key;
    emit summaryChanged();
}

bool NetworkManager::editServer(Net n, const QString &id, const QString &address, const QString &name)
{
    NetworkPlugin *p = plugin(n);
    if (!p) return false;
    bool known = false;
    for (const ServerEntry &e : p->servers())
        if (e.id == id) { known = true; break; }
    if (!known) return false;
    return p->editServer(id, address.trimmed().isEmpty() ? id : address.trimmed(), name);
}

int NetworkManager::importServerData(Net n, const QByteArray &data)
{
    NetworkPlugin *p = plugin(n);
    if (!p || data.isEmpty()) return -1;
    const int own = p->importServers(data);
    if (own >= 0) return own;
    QString err;
    const QList<ServerListEntry> list = parseServerListFile(data, &err);
    if (list.isEmpty()) {
        qCInfo(lcNet) << "server list import for" << netName(n) << "failed:" << err;
        return -1;
    }
    for (const ServerListEntry &e : list) p->addServer(e.address, e.name);
    return int(list.size());
}

int NetworkManager::importServerList(Net n, const QString &filePath)
{
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) return -1;
    const QByteArray data = f.read(32 << 20);
    const int r = importServerData(n, data);
    qCInfo(lcNet) << "imported" << r << "servers for" << netName(n) << "from" << filePath;
    return r;
}

QList<ServerMessage> NetworkManager::serverMessages(Net n) const
{
    if (n != Net::Unknown) return m_messages.value(int(n));
    QList<ServerMessage> all;
    for (auto it = m_messages.constBegin(); it != m_messages.constEnd(); ++it) all << it.value();
    std::stable_sort(all.begin(), all.end(), [](const ServerMessage &a, const ServerMessage &b) { return a.time < b.time; });
    return all;
}

void NetworkManager::pingUser(const UserRef &u)
{
    if (!m_pinger) {
        QTimer::singleShot(0, this, [this, u] { emit userPinged(u, -1); });
        return;
    }
    SearchSource src;
    src.net = u.net;
    src.user = u;
    m_pinger->ping(src, [this, u](int ms) { emit userPinged(u, ms); });
}

} // namespace mx
