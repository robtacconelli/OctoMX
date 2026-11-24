// SPDX-License-Identifier: GPL-3.0-or-later
// Net::Overlay: lifecycle, peers, hello, control dispatch, signed envelopes, presence, status.
#include "overlay_plugin_impl.h"

#include "overlay_transfer.h"

#include <QtCore/QDir>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QNetworkInterface>

#include "mx/net_policy.h"
#include "mx/settings_keys.h"

namespace mx::ovl {

namespace {

constexpr int kEnvelopeHeader = 2 + 32 + 64;
constexpr qint64 kMaxClockSkewMs = 10 * 60 * 1000;

QByteArray signedPart(const QString &topic, const QByteArray &body)
{
    // the protocol identifier and a newline: every node verifies this prefix (identifier kept for
    // compatibility, byte for byte)
    QByteArray m = QByteArray(kOverlayProtocol) + '\n';
    m += topic.toUtf8();
    m += '\n';
    m += body;
    return m;
}

// Parse + verify a signed pub-sub envelope. Thread-safe (used by the librats validator).
bool parseEnvelope(const QString &topic, const QByteArray &raw, int maxBytes, bool checkFresh,
                   QString *userId, QJsonObject *body)
{
    if (raw.size() < kEnvelopeHeader + 10 || raw.size() > maxBytes) return false;
    if (raw[0] != 'S' || raw[1] != char(kWireVersion)) return false;
    const QByteArray pub = raw.mid(2, 32);
    const QByteArray sig = raw.mid(34, 64);
    const QByteArray b = raw.mid(kEnvelopeHeader);
    if (!verifySignature(pub, signedPart(topic, b), sig)) return false;
    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(b, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) return false;
    const QJsonObject o = doc.object();
    if (jStr(o, "t", 32).isEmpty() || !isHex(jStr(o, "id", 64))) return false;
    if (checkFresh) {
        const qint64 ts = jInt(o, "ts", 0);
        if (qAbs(nowMs() - ts) > kMaxClockSkewMs) return false;
    }
    if (userId) *userId = QString::fromLatin1(pub.toHex());
    if (body) *body = o;
    return true;
}

int maxBytesForTopic(const QString &topic)
{
    if (topic.startsWith(QLatin1String("mx/room/"))) return 8 * 1024;
    if (topic == QLatin1String(kTopicRelay)) return 16 * 1024;
    return 32 * 1024;
}

ChatEvents *events(QObject *o) { return static_cast<ChatEvents *>(o); }

} // namespace

// Exposed to the other overlay_*.cpp files.
bool overlayParseEnvelope(const QString &topic, const QByteArray &raw, QString *userId, QJsonObject *body)
{
    return parseEnvelope(topic, raw, maxBytesForTopic(topic), false, userId, body);
}

OverlayPlugin::OverlayPlugin(QObject *parent) : OverlayApi(parent)
{
    m_events = new ChatEvents(this);
    m_status.net = Net::Overlay;
    m_status.state = ConnState::Disconnected;
    m_status.statusText = tr("Not connected");
    m_tick.setInterval(250);
    connect(&m_tick, &QTimer::timeout, this, &OverlayPlugin::tick);
    m_readPool = new QThreadPool(this);
    m_readPool->setMaxThreadCount(2);
    chatInit();
}

OverlayPlugin::~OverlayPlugin()
{
    m_readPool->waitForDone();
    stop();
    if (m_node) m_node->waitStopped(3000);
    qDeleteAll(m_uploads);
    m_uploads.clear();
}

int OverlayPlugin::capabilities() const
{
    return CanSearch | CanFindSources | CanFetchRanges | CanServe | CanBrowse | CanChat | CanHashLinks;
}

QVariant OverlayPlugin::setting(const QString &key, const QVariant &def) const
{
    if (!m_ctx.settings) return def;
    const QVariant v = m_ctx.settings->value(key);
    return v.isValid() ? v : def;
}

QString OverlayPlugin::connectionLabel() const
{
    QString c = setting(QString(keys::identity::Connection), QStringLiteral("DSL")).toString();
    // [ui-h] Settings > Queuing: report another connection type while this many uploads run here
    int active = 0;
    for (const UploadSession *u : m_uploads)
        if (u && u->active) ++active;
    c = reportedConnectionType(c, setting(QString(keys::queue::ChangeSpeedWhenBusy), false).toBool(),
                               setting(QString(keys::queue::ChangeSpeedThreshold), 2).toInt(),
                               setting(QString(keys::queue::ChangedSpeed), QStringLiteral("DSL")).toString(), active);
    return cleanText(c, 24);
}

void OverlayPlugin::updateDisplayName()
{
    QString base = cleanName(m_screenName);
    static const QRegularExpression suffix(QStringLiteral("_\\d{5}$"));
    base.remove(suffix);
    if (base.isEmpty()) base = QStringLiteral("Guest");
    m_displayName = base + QLatin1Char('_') + idSuffix(m_userId);
}

void OverlayPlugin::init(const PluginContext &ctx)
{
    m_ctx = ctx;
    m_dataDir = ctx.dataDir.isEmpty() && !ctx.profileDir.isEmpty() ? ctx.profileDir + QStringLiteral("/overlay")
                                                                    : ctx.dataDir;
    m_screenName = ctx.screenName;
    if (m_ctx.settings) {
        const QString s = m_ctx.settings->s(QString(keys::identity::ScreenName));
        if (!s.isEmpty()) m_screenName = s;
        connect(m_ctx.settings, &Settings::changed, this, [this](const QString &key, const QVariant &v) {
            if (key == QLatin1String(keys::identity::ScreenName) && !v.toString().isEmpty()) {
                m_screenName = v.toString();
                updateDisplayName();
                if (m_running) {
                    announcePresence();
                    for (Room &r : m_rooms) if (r.joined) roomHeartbeat(r);
                }
            } else if (key == QLatin1String(keys::identity::Connection) && m_running) {
                announcePresence();
                for (Room &r : m_rooms) if (r.joined) roomHeartbeat(r);   // [set-1] the room lists too
                m_announcedConn = connectionLabel();
            }
        });
    }
    QString err;
    if (!m_dataDir.isEmpty() && !m_id.loadOrCreate(m_dataDir, &err)) {
        qCWarning(lcOverlay) << "identity:" << err;
        Q_EMIT logMessage(QStringLiteral("OctoMX overlay: cannot create identity: %1").arg(err));
    }
    m_userId = m_id.userId();
    updateDisplayName();
    loadHashLinks();
}

void OverlayPlugin::start()
{
    if (m_running) return;
    if (!m_id.isValid()) {
        // no data dir (tests without profile): ephemeral identity in a temp-like dir is not allowed
        // (tests must pass a profile): refuse to start.
        m_status.state = ConnState::Error;
        m_status.statusText = tr("No identity (no profile directory)");
        Q_EMIT statusChanged(m_status);
        return;
    }
    RatsNodeConfig cfg;
    cfg.protocol = m_testOpts.protocol.isEmpty() ? QString::fromLatin1(kOverlayProtocol) : m_testOpts.protocol;
    const bool randomPorts = setting(QString(keys::connection::RandomizePorts), false).toBool();
    cfg.listenPort = m_testOpts.listenPort >= 0 ? static_cast<quint16>(m_testOpts.listenPort)
                     : randomPorts ? 0
                                   : static_cast<quint16>(setting(QString(keys::connection::TcpPort), 6699).toInt());
    {   // [net-2] no OctoMX network through a proxy; with a chosen adapter every socket is bound to it
        // (librats takes an address, not an interface name; IPv6 off: IPv4 only)
        const mx::netpolicy::Verdict v = mx::netpolicy::verdict(Net::Overlay, m_ctx.settings);
        bool ok = true;
        QString why;
        cfg.bindAddress = mx::netpolicy::bindAddressFor(m_ctx.settings, &ok, &why);
        if (!v.allowed || !ok) {
            m_status.state = ConnState::Error;
            m_status.statusText = v.allowed ? why : v.reason;
            Q_EMIT statusChanged(m_status);
            Q_EMIT logMessage(QStringLiteral("OctoMX overlay: %1").arg(m_status.statusText));
            return;
        }
    }
    cfg.dataDir = m_dataDir;
#ifdef MX_TEST_HOOKS   // [release] fault-injection hooks only exist when CMake option MX_TEST_HOOKS is ON
    {   // [engine-3] hidden test hook: corrupt the bytes we serve in "start-end"
        const QStringList r = setting(QString(keys::overlay::TestCorruptUploads), QString()).toString().split(QLatin1Char('-'));
        m_corruptStart = m_corruptEnd = 0;
        if (r.size() == 2 && r[0].toLongLong() >= 0 && r[1].toLongLong() > r[0].toLongLong()) {
            m_corruptStart = r[0].toLongLong();
            m_corruptEnd = r[1].toLongLong();
            qCWarning(lcOverlay) << "TEST HOOK: corrupting served bytes" << m_corruptStart << "-" << m_corruptEnd;
        }
    }
#else
    if (!setting(QString(keys::overlay::TestCorruptUploads), QString()).toString().isEmpty())
        qCWarning(lcOverlay) << "TEST HOOK unavailable: built without MX_TEST_HOOKS, overlay/testCorruptUploads ignored";
#endif
    cfg.maxPeers = setting(QString(keys::overlay::MaxPeers), 50).toInt();
    cfg.peerTarget = qMin(cfg.maxPeers, 30);
    cfg.dht = m_testOpts.dht;
    cfg.mdns = m_testOpts.mdns && setting(QString(keys::connection::BindInterface), QString()).toString().trimmed().isEmpty();
    cfg.pex = m_testOpts.pex;
    cfg.reconnect = m_testOpts.reconnect;
    {   // [ovl-2] instance tests on a shared host: no DHT rendezvous and no LAN discovery, so test
        // nodes find only their bootstrap peers and never meet real users (MX_OVERLAY_DISCOVERY=0)
        const QByteArray disc = qgetenv("MX_OVERLAY_DISCOVERY").trimmed().toLower();
        if (disc == "0" || disc == "off" || disc == "false") {
            cfg.dht = false;
            cfg.mdns = false;
            cfg.pex = false;
            cfg.reconnect = false;
            qCInfo(lcOverlay) << "discovery off (MX_OVERLAY_DISCOVERY): bootstrap peers only";
        }
        // MX_OVERLAY_FIREWALLED=1: behave like a node behind a NAT without a forwarded port (refuse direct
        // inbound links, announce no address, no hole punch): only connect-back and relayed circuits reach it
        if (qgetenv("MX_OVERLAY_FIREWALLED") == "1") {
            m_testOpts.firewalled = true;
            m_testOpts.advertise = false;
            qCInfo(lcOverlay) << "simulating a firewalled node (MX_OVERLAY_FIREWALLED)";
        }
    }
    cfg.relayServe = setting(QString(keys::overlay::Relay), true).toBool();
    // [diag-1] the core's port mapper forwards our ports when it can (PluginContext::coreMapsPorts)
    cfg.portMapping = !m_ctx.coreMapsPorts && setting(QString(keys::connection::Upnp), false).toBool();
    m_wantPort = cfg.listenPort;
    cfg.punch = !m_testOpts.firewalled;   // [ovl-2] the test firewall: a NAT that no punch opens
    cfg.json = false;
    cfg.pubsub = true;
    cfg.channels = {QString::fromLatin1(kCtlChannel), QString::fromLatin1(kDataChannel)};
    if (!m_node) {
        m_node = new RatsNode(this);
        m_node->setMeter(m_ctx.limiter, Net::Overlay, QString::fromLatin1(kDataChannel));   // [glue-3] [bw-2] only file data is charged
        connect(m_node, &RatsNode::peerConnected, this, &OverlayPlugin::onPeerConnected);
        connect(m_node, &RatsNode::peerDisconnected, this, &OverlayPlugin::onPeerDisconnected);
        connect(m_node, &RatsNode::peerRerouted, this, [this](const QString &id) {
            qCDebug(lcOverlay) << "peer" << id.left(8) << "moved to another link";
            sendHello(id);
            // [ovl-2] librats: what was sent on the replaced route may not have arrived. Ask again for
            // what is still unanswered (duplicates are harmless: answers are matched by request id)
            for (auto it = m_browse.begin(); it != m_browse.end(); ++it)
                if (it->nodeId == id && it->got == 0) {
                    QJsonObject q;
                    q[QStringLiteral("t")] = QStringLiteral("browse_req");
                    q[QStringLiteral("v")] = kWireVersion;
                    q[QStringLiteral("rid")] = it->rid;
                    sendRaw(id, q);
                }
            for (const WhoisReq &w : std::as_const(m_whois))
                if (nodeForUser(w.user.id) == id) {
                    QJsonObject q;
                    q[QStringLiteral("t")] = QStringLiteral("whois_req");
                    q[QStringLiteral("v")] = kWireVersion;
                    q[QStringLiteral("rid")] = w.rid;
                    sendRaw(id, q);
                }
            for (auto &src : m_sources)
                if (src && src->nodeId() == id) src->linkMoved();
            // [bw-2] and our own answers to it (the asker drops duplicates)
            for (const SentAnswer &a : std::as_const(m_sentAnswers))
                if (a.nodeId == id && nowMs() - a.ms < 10000) sendRaw(id, a.msg);
        });
        connect(m_node, &RatsNode::bytesReceived, this, &OverlayPlugin::onBytes);
        connect(m_node, &RatsNode::topicMessage, this, &OverlayPlugin::onTopic);
        connect(m_node, &RatsNode::peerWritable, this, [this](const QString &id) {
            for (UploadSession *s : m_uploads)
                if (s->nodeId == id) pumpUpload(s);
        });
    }
    QString err;
    if (!m_node->start(cfg, &err)) {
        m_status.state = ConnState::Error;
        m_status.statusText = tr("Cannot start: %1").arg(err);
        Q_EMIT statusChanged(m_status);
        return;
    }
    m_running = true;
    m_startedAt.start();
    m_onlineSinceMs = nowMs();
    m_sawInbound = false;
    m_lastPresenceMs = m_lastHeartbeatMs = m_lastDirectoryMs = 0;
    m_lastBootstrapMs = 0;
    subscribeTopic(QString::fromLatin1(kTopicPresence), 32 * 1024);
    subscribeTopic(QString::fromLatin1(kTopicRooms), 32 * 1024);
    subscribeTopic(QString::fromLatin1(kTopicSearch), 32 * 1024);
    subscribeTopic(QString::fromLatin1(kTopicRelay), 16 * 1024);
    subscribeTopic(QString::fromLatin1(kTopicHashLinks), 32 * 1024);
    for (Room &r : m_rooms)
        if (r.joined) subscribeTopic(r.topicStr, 8 * 1024);
    m_tick.start();
    m_relayTopics.clear();
    relayRoomTopics();   // [ovl-2] the default rooms at once, announced ones as they come
    dialBootstrap();
    qCInfo(lcOverlay) << "overlay node" << m_node->nodeId().left(8) << "user" << m_displayName << "port"
                      << m_node->listenPort();
    Q_EMIT logMessage(QStringLiteral("OctoMX overlay: %1 listening on port %2").arg(m_displayName).arg(m_node->listenPort()));
    updateStatus(true);
}

void OverlayPlugin::stop()
{
    if (!m_running) return;
    // tell rooms + presence we leave (best effort: the node is torn down right after)
    for (Room &r : m_rooms)
        if (r.joined) publishSigned(r.topicStr, newBody("part"));
    publishSigned(QString::fromLatin1(kTopicPresence), newBody("bye"));
    m_running = false;
    m_tick.stop();
    finishSearches(true);
    for (auto it = m_browse.begin(); it != m_browse.end(); ++it)
        Q_EMIT browseFailed(it.key(), it->user, tr("Disconnected"));
    m_browse.clear();
    m_whois.clear();
    for (auto &list : m_pending)
        for (Pending &p : list)
            if (p.done) p.done(false);
    m_pending.clear();
    const QList<UploadSession *> ups = m_uploads.values();
    for (UploadSession *s : ups) closeUpload(s, QStringLiteral("stopped"), false);
    for (auto &src : m_sources)
        if (src) src->fail(tr("Disconnected"));
    if (m_node) m_node->stop();
    m_peers.clear();
    m_nodeUser.clear();
    for (auto it = m_users.begin(); it != m_users.end(); ++it) {
        if (it->online) {
            it->online = false;
            Q_EMIT events(m_events)->userPresence(userRefFor(it.key()), false, it->files, it->conn);
        }
        it->direct = false;
    }
    for (Room &r : m_rooms) {
        if (!r.members.isEmpty()) {
            r.members.clear();
            Q_EMIT events(m_events)->roomUsersChanged(r.id);
        }
    }
    saveHashLinks();
    updateStatus(true);
    Q_EMIT serversChanged();
}

// ------------------------------------------------------------------------------------- peers
void OverlayPlugin::onPeerConnected(const RatsPeer &p)
{
    qCDebug(lcOverlay) << "peer up" << p.id.left(8) << p.address << (p.inbound ? "in" : "out") << p.transport;
    if (m_testOpts.firewalled && p.inbound && p.transport != QLatin1String("relay")) {
        // [ovl-2] tests: behave like a node behind a NAT without a forwarded port
        qCDebug(lcOverlay) << "firewalled (test): refusing direct inbound link from" << p.id.left(8);
        m_node->disconnectPeer(p.id);
        return;
    }
    PeerState &st = m_peers[p.id];
    st.address = p.address;
    st.inbound = p.inbound;
    st.transport = p.transport;   // [diag-1]
    st.connectedMs = nowMs();
    st.lastRxMs = st.connectedMs;
    st.lastPingMs = 0;
    st.hello = false;
    if (p.inbound) m_sawInbound = true;
    if (p.inbound && p.transport != QLatin1String("relay")) {   // [diag-1]
        if (p.transport == QLatin1String("udp")) m_inboundUdp = true;
        else m_inboundTcp = true;
    }
    sendHello(p.id);
    // [ovl-2] queued messages wait for the peer's verified hello (flushPending in onHello): a link that
    // drops right after it came up (a firewall, a lost duplicate) used to swallow them as "sent"
    Q_EMIT serversChanged();
}

void OverlayPlugin::onPeerDisconnected(const QString &nodeId, const QString &reason)
{
    // a quick restart of the same peer (same node id) may already have a newer live link
    const bool still = m_node && m_node->isConnected(nodeId);
    qCDebug(lcOverlay) << "peer down" << nodeId.left(8) << reason << (still ? "(another link is live)" : "");
    if (still) return;
    dropPeer(nodeId, reason);
}

void OverlayPlugin::dropPeer(const QString &nodeId, const QString &)
{
    if (!m_peers.contains(nodeId)) return;
    m_peers.remove(nodeId);
    m_lastPeerLostMs = nowMs();
    const QString uid = m_nodeUser.take(nodeId);
    if (!uid.isEmpty()) {
        auto it = m_users.find(uid);
        if (it != m_users.end() && it->nodeId == nodeId) {
            it->direct = false;
            // [ovl-2] the last link to a direct peer went: they quit, crashed or lost their connection.
            // Offline within seconds, unless they answer a ping through the mesh (still reachable via
            // other users); older nodes cannot answer, they come back with their next presence.
            if (it->online) {
                if (it->has("ping") && !m_peers.isEmpty()) startPing(uid);
                else userWentOffline(uid, QStringLiteral("connection lost"));
            }
        }
    }
    dropPeerUploads(nodeId);
    for (auto &src : m_sources)
        if (src && src->nodeId() == nodeId) src->peerGone();
    // [ovl-2] a browse that got nothing yet: ask once more (reconnecting, connect-back, relay)
    QList<quint32> again;
    for (auto it = m_browse.begin(); it != m_browse.end(); ++it)
        if (it->nodeId == nodeId && it->got == 0 && !it->resent) {
            it->resent = true;
            again << it.key();
        }
    for (quint32 id : again) sendBrowseRequest(id);
    Q_EMIT serversChanged();
}

QStringList OverlayPlugin::ourAddresses() const
{
    QStringList out;
    if (!m_node || !m_testOpts.advertise) return out;
    const quint16 port = m_node->listenPort();
    if (!m_testOpts.publicFirst.isEmpty()) out << m_testOpts.publicFirst;
    for (const QString &a : m_node->observedAddresses())
        if (!out.contains(a)) out << a;
    QStringList loop;
    for (const QHostAddress &ha : QNetworkInterface::allAddresses()) {
        if (ha.protocol() != QAbstractSocket::IPv4Protocol) continue;
        const QString s = QStringLiteral("%1:%2").arg(ha.toString()).arg(port);
        if (ha.isLoopback()) loop << s;
        else if (!out.contains(s)) out << s;
    }
    out += loop;
    while (out.size() > 8) out.removeLast();
    return out;
}

void OverlayPlugin::rememberAnswer(const QString &nodeId, const QJsonObject &msg)
{
    const qint64 now = nowMs();
    while (!m_sentAnswers.isEmpty() && (now - m_sentAnswers.first().ms >= 10000 || m_sentAnswers.size() >= 64))
        m_sentAnswers.removeFirst();
    m_sentAnswers.append(SentAnswer{nodeId, msg, now});
}

void OverlayPlugin::sendRaw(const QString &nodeId, const QJsonObject &msg)
{
    if (!m_node) return;
    m_node->sendBytes(nodeId, QString::fromLatin1(kCtlChannel), encodeControl(msg));
}

void OverlayPlugin::sendUrgent(const QString &nodeId, const QJsonObject &msg)
{
    if (!m_node) return;
    m_node->sendBytesUrgent(nodeId, QString::fromLatin1(kCtlChannel), encodeControl(msg));
}

void OverlayPlugin::sendHello(const QString &nodeId)
{
    if (!m_node) return;
    QJsonObject h;
    h[QStringLiteral("t")] = QStringLiteral("hello");
    h[QStringLiteral("v")] = kWireVersion;
    const QString node = m_node->nodeId();
    const QString x = QString::fromLatin1(m_id.boxPublicKey().toHex());
    const qint64 ts = nowMs();
    h[QStringLiteral("user")] = m_userId;
    h[QStringLiteral("node")] = node;
    h[QStringLiteral("x")] = x;
    h[QStringLiteral("name")] = m_displayName;
    h[QStringLiteral("ts")] = static_cast<double>(ts);
    h[QStringLiteral("client")] = m_ctx.clientName.isEmpty() ? QStringLiteral("OctoMX") : m_ctx.clientName;
    h[QStringLiteral("ver")] = m_ctx.clientVersion.isEmpty() ? QStringLiteral(MX_VERSION_STRING) : m_ctx.clientVersion;
    h[QStringLiteral("files")] = m_shared.size();
    h[QStringLiteral("conn")] = connectionLabel();
    h[QStringLiteral("since")] = static_cast<double>(m_onlineSinceMs);
    QJsonArray addrs;
    for (const QString &a : ourAddresses()) addrs << a;
    h[QStringLiteral("addrs")] = addrs;
    h[QStringLiteral("caps")] = ourCaps();   // [ovl-2]
    const QByteArray toSign = QStringLiteral("hello\n%1\n%2\n%3\n%4").arg(node, x, m_displayName).arg(ts).toUtf8();
    h[QStringLiteral("sig")] = QString::fromLatin1(m_id.sign(toSign).toHex());
    sendRaw(nodeId, h);
}

void OverlayPlugin::onHello(const QString &nodeId, const QJsonObject &o)
{
    auto pit = m_peers.find(nodeId);
    if (pit == m_peers.end()) return;
    const QString user = jStr(o, "user", 64).toLower();
    const QString node = jStr(o, "node", 64).toLower();
    const QString x = jStr(o, "x", 64).toLower();
    const QString name = cleanText(jStr(o, "name", 48), 48);
    const qint64 ts = jInt(o, "ts", 0);
    const QByteArray sig = QByteArray::fromHex(jStr(o, "sig", 128).toLatin1());
    if (!isHex(user, 64) || !isHex(x, 64) || node != nodeId || name.isEmpty()) {
        qCInfo(lcOverlay) << "bad hello from" << nodeId.left(8);
        m_node->disconnectPeer(nodeId);
        return;
    }
    const QByteArray toSign = QStringLiteral("hello\n%1\n%2\n%3\n%4").arg(node, x, name).arg(ts).toUtf8();
    if (!verifySignature(QByteArray::fromHex(user.toLatin1()), toSign, sig)) {
        qCInfo(lcOverlay) << "hello signature invalid from" << nodeId.left(8);
        m_node->disconnectPeer(nodeId);
        return;
    }
    if (user == m_userId) {   // a second instance with our identity (or ourselves via a loop)
        m_node->disconnectPeer(nodeId);
        return;
    }
    // another connection of the same user replaces the old mapping
    const QString old = m_nodeUser.value(nodeId);
    pit->hello = true;
    pit->userId = user;
    m_nodeUser[nodeId] = user;
    UserInfo &u = m_users[user];
    u.name = name;
    u.nodeId = nodeId;
    u.xpub = QByteArray::fromHex(x.toLatin1());
    u.files = static_cast<int>(qBound(qint64(0), jInt(o, "files", 0), qint64(10000000)));
    u.conn = cleanText(jStr(o, "conn", 24), 24);
    u.client = cleanText(jStr(o, "client", 32) + QLatin1Char(' ') + jStr(o, "ver", 16), 48);
    u.sinceMs = jInt(o, "since", 0);
    u.addrs.clear();
    for (const QJsonValue &v : jArr(o, "addrs", 8))
        if (v.isString()) u.addrs << cleanText(v.toString(), 64);
    if (!pit->address.isEmpty() && !u.addrs.contains(pit->address) && !pit->inbound) u.addrs.prepend(pit->address);
    u.caps.clear();
    for (const QJsonValue &v : jArr(o, "caps", 16))
        if (v.isString()) u.caps << cleanText(v.toString(), 16);
    u.lastSeenMs = nowMs();
    u.direct = true;
    u.pingSentMs = 0;
    setUserOnline(user, true);
    pmRetryFor(user);   // [ovl-2] a fresh direct link: undelivered messages go now
    Q_UNUSED(old)
    // messages that arrived before the hello
    const QList<QByteArray> early = pit->early;
    pit->early.clear();
    for (const QByteArray &b : early) {
        QJsonObject m;
        if (decodeControl(b, &m)) handleControl(nodeId, m);
    }
    flushPending(nodeId);
    flushPending(nodeId, QStringLiteral("u:") + user);   // [ovl-2] queued before its node was known
    Q_EMIT serversChanged();
}

bool OverlayPlugin::sendToNode(const QString &nodeId, const QJsonObject &msg, const QStringList &addrs,
                               std::function<void(bool)> done)
{
    if (!m_node || !m_running || !isHex(nodeId, 64)) {
        if (done) done(false);
        return false;
    }
    if (m_peers.contains(nodeId) && m_peers.value(nodeId).hello) {
        sendRaw(nodeId, msg);
        if (done) done(true);
        return true;
    }
    QList<Pending> &q = m_pending[nodeId];
    if (q.size() >= 64) {
        if (done) done(false);
        return false;
    }
    qCDebug(lcOverlay) << "not connected to" << nodeId.left(8) << ": queueing" << jStr(msg, "t", 16) << "and dialling" << addrs;
    q.append(Pending{encodeControl(msg), nowMs() + m_t.dialWaitMs, std::move(done)});
    if (q.size() == 1) {
        for (const QString &a : addrs) m_node->connectTo(a);
        requestReach(nodeId);   // [ovl-2] connect-back and relay circuit when the dial cannot work
    }
    return true;
}

bool OverlayPlugin::sendToUser(const QString &userId, const QJsonObject &msg, std::function<void(bool)> done)
{
    const UserInfo *u = user(userId);
    if (!u || !m_running || !m_node) {
        if (done) done(false);
        return false;
    }
    if (u->nodeId.isEmpty()) {
        // [ovl-2] seen (in a room) but its node is not known yet (a 4.2 member before its presence): queue
        // by user and ask it through the mesh to connect to us; its hello delivers the queue
        const QString key = QStringLiteral("u:") + userId.toLower();
        QList<Pending> &q = m_pending[key];
        if (q.size() >= 64) {
            if (done) done(false);
            return false;
        }
        q.append(Pending{encodeControl(msg), nowMs() + m_t.dialWaitMs, std::move(done)});
        if (nowMs() - m_reachAskedMs.value(key, 0) > 5000) {
            m_reachAskedMs.insert(key, nowMs());
            QJsonObject x;
            x[QStringLiteral("node")] = m_node->nodeId();
            QJsonArray addrs;
            for (const QString &a : ourAddresses()) addrs << a;
            x[QStringLiteral("addrs")] = addrs;
            publishRelay("reach", userId.toLower(), x);
        }
        return true;
    }
    return sendToNode(u->nodeId, msg, u->addrs, std::move(done));
}

void OverlayPlugin::flushPending(const QString &nodeId, const QString &key)
{
    auto it = m_pending.find(key.isEmpty() ? nodeId : key);
    if (it == m_pending.end()) return;
    const QList<Pending> list = it.value();
    m_pending.erase(it);
    for (const Pending &p : list) {
        m_node->sendBytes(nodeId, QString::fromLatin1(kCtlChannel), p.bytes);
        if (p.done) p.done(true);
    }
}

void OverlayPlugin::expirePending()
{
    const qint64 now = nowMs();
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        QList<Pending> keep;
        QList<Pending> expired;
        for (Pending &p : it.value()) (p.expiresMs < now ? expired : keep).append(p);
        it.value() = keep;
        if (!expired.isEmpty())
            qCDebug(lcOverlay) << expired.size() << "queued message(s) to" << it.key().left(8) << "expired (not connected)";
        for (Pending &p : expired)
            if (p.done) p.done(false);
        if (it.value().isEmpty()) it = m_pending.erase(it);
        else ++it;
    }
}

bool OverlayPlugin::sendDataFrame(const QString &nodeId, const QByteArray &frame)
{
    return m_node && m_node->sendBytes(nodeId, QString::fromLatin1(kDataChannel), frame);
}

bool OverlayPlugin::isWritable(const QString &nodeId) const
{
    return m_node && m_node->isWritable(nodeId);
}

const OverlayPlugin::UserInfo *OverlayPlugin::user(const QString &userId) const
{
    auto it = m_users.constFind(userId.toLower());
    return it == m_users.constEnd() ? nullptr : &it.value();
}

QString OverlayPlugin::nodeForUser(const QString &userId) const
{
    const UserInfo *u = user(userId);
    return u ? u->nodeId : QString();
}

UserRef OverlayPlugin::userRefFor(const QString &userId) const
{
    UserRef r;
    r.net = Net::Overlay;
    r.id = userId;
    const UserInfo *u = user(userId);
    r.name = u && !u->name.isEmpty() ? u->name : QStringLiteral("User_%1").arg(idSuffix(userId));
    if (u && !u->addrs.isEmpty()) r.address = u->addrs.front();
    return r;
}

int OverlayPlugin::connectedUsers() const
{
    int n = 0;
    for (const PeerState &p : m_peers) n += p.hello ? 1 : 0;
    return n;
}

// --------------------------------------------------------------------------------- dispatch
void OverlayPlugin::onBytes(const QString &nodeId, const QString &channel, const QByteArray &bytes)
{
    auto pit = m_peers.find(nodeId);
    if (pit == m_peers.end()) {
        // [ovl-2] bytes from a peer we think is gone: librats still holds a link (a missed or out of
        // order connect / disconnect event). Take it up again instead of dropping everything it says.
        if (!m_node || !m_node->isConnected(nodeId)) return;
        reconcilePeers();
        pit = m_peers.find(nodeId);
        if (pit == m_peers.end()) return;
    }
    pit->lastRxMs = nowMs();
    if (channel == QLatin1String(kDataChannel)) {
        onDataFrame(nodeId, bytes);
        return;
    }
    if (!m_peerGate.allow(nodeId)) {
        if (++pit->dropped == 2000) {
            qCInfo(lcOverlay) << "peer" << nodeId.left(8) << "floods control messages: disconnecting";
            m_node->disconnectPeer(nodeId);
        }
        return;
    }
    if (bytes.size() > 4 * 1024 * 1024) return;
    if (!pit->hello) {
        QJsonObject o;
        if (!decodeControl(bytes, &o, 64 * 1024)) return;
        if (jStr(o, "t", 16) == QLatin1String("hello")) {
            onHello(nodeId, o);
        } else if (pit->early.size() < 64) {
            pit->early << bytes;
        }
        return;
    }
    QJsonObject o;
    if (!decodeControl(bytes, &o)) return;
    handleControl(nodeId, o);
}

void OverlayPlugin::handleControl(const QString &nodeId, const QJsonObject &o)
{
    const QString t = jStr(o, "t", 32);
    if (jInt(o, "v", kWireVersion) > 255) return;
    if (t == QLatin1String("hello")) return onHello(nodeId, o);
    const QString uid = m_nodeUser.value(nodeId);
    if (uid.isEmpty()) {
        qCDebug(lcOverlay) << "dropping" << t << "from" << nodeId.left(8) << "(no hello)";
        return;
    }
    if (t == QLatin1String("pm")) return onPm(uid, o, false, nodeId);
    if (t == QLatin1String("pm_ack")) return onPmAck(uid, jStr(o, "id", 64));   // [ovl-2]
    if (t == QLatin1String("pm_err")) return onPmRefused(uid, jStr(o, "id", 64), jStr(o, "reason", 16));
    if (t == QLatin1String("browse_req")) return onBrowseRequest(nodeId, o);
    if (t == QLatin1String("browse") || t == QLatin1String("browse_err")) return onBrowse(nodeId, o);
    if (t == QLatin1String("whois_req")) return onWhoisRequest(nodeId, o);
    if (t == QLatin1String("whois")) return onWhois(nodeId, o, false);
    if (t == QLatin1String("whois_err")) return onWhois(nodeId, o, true);
    if (t == QLatin1String("hist_req")) return onHistoryRequest(nodeId, o);
    if (t == QLatin1String("hist")) return onHistory(nodeId, o);
    if (t == QLatin1String("hits")) return onHits(nodeId, o);
    if (t == QLatin1String("src")) return onSources(nodeId, o);
    if (t == QLatin1String("hl")) return onHashLinkAnswer(nodeId, o);
    if (t == QLatin1String("get_open")) return onGetOpen(nodeId, o);
    if (t == QLatin1String("get")) return onGet(nodeId, o);
    if (t == QLatin1String("get_cancel")) return onGetClose(nodeId, o, true);
    if (t == QLatin1String("get_close")) return onGetClose(nodeId, o, false);
    if (t == QLatin1String("slot")) return onSlot(nodeId, o);
    if (t == QLatin1String("have")) return onHave(nodeId, o);   // [up-1]
    if (t == QLatin1String("got")) return onGot(nodeId, o);
    if (t == QLatin1String("hs_req")) return onHashSetRequest(nodeId, o);   // [engine-3]
    if (t == QLatin1String("hs")) return onHashSetAnswer(nodeId, o);
    if (t == QLatin1String("probe")) return onProbeRequest(nodeId, o);   // [diag-1]
    if (t == QLatin1String("probed")) return onProbeAnswer(nodeId, o);
    if (t == QLatin1String("ping")) {
        QJsonObject p;
        p[QStringLiteral("t")] = QStringLiteral("pong");
        p[QStringLiteral("v")] = kWireVersion;
        p[QStringLiteral("ts")] = o.value(QStringLiteral("ts"));
        sendRaw(nodeId, p);
        return;
    }
    // unknown types are ignored (forward compatibility)
}

// --------------------------------------------------------------------------------- envelopes
QJsonObject OverlayPlugin::newBody(const char *type) const
{
    QJsonObject b;
    b[QStringLiteral("t")] = QString::fromLatin1(type);
    b[QStringLiteral("v")] = kWireVersion;
    b[QStringLiteral("id")] = randomId();
    b[QStringLiteral("ts")] = static_cast<double>(nowMs());
    b[QStringLiteral("name")] = m_displayName;
    return b;
}

QByteArray OverlayPlugin::makeEnvelope(const QString &topic, const QJsonObject &body) const
{
    const QByteArray b = QJsonDocument(body).toJson(QJsonDocument::Compact);
    QByteArray env;
    env.reserve(kEnvelopeHeader + b.size());
    env.append('S');
    env.append(char(kWireVersion));
    env.append(m_id.publicKey());
    env.append(m_id.sign(signedPart(topic, b)));
    env.append(b);
    return env;
}

void OverlayPlugin::publishSigned(const QString &topic, const QJsonObject &body)
{
    if (!m_node || !m_running) return;
    const QByteArray env = makeEnvelope(topic, body);
    if (env.size() > maxBytesForTopic(topic)) {
        qCWarning(lcOverlay) << "envelope too large for" << topic << env.size();
        return;
    }
    m_seen.insert(jStr(body, "id", 64));
    m_node->publish(topic, env);
}

void OverlayPlugin::subscribeTopic(const QString &topic, int maxBytes)
{
    if (!m_node) return;
    m_node->subscribe(topic, [maxBytes](const QString &tp, const QByteArray &data) {
        // runs on a librats thread: pure + thread-safe. Rejected messages are not relayed.
        return parseEnvelope(tp, data, maxBytes, true, nullptr, nullptr);
    });
}

void OverlayPlugin::onTopic(const QString &topic, const QString &, const QByteArray &data)
{
    QString uid;
    QJsonObject body;
    if (!parseEnvelope(topic, data, maxBytesForTopic(topic), true, &uid, &body)) return;
    if (uid == m_userId) return;
    if (!m_seen.insert(jStr(body, "id", 64))) return;
    if (topic == QLatin1String(kTopicPresence)) {
        const QString t = jStr(body, "t", 16);
        if (t == QLatin1String("presence") || t == QLatin1String("bye")) onPresence(uid, body, t == QLatin1String("bye"));
        return;
    }
    if (topic == QLatin1String(kTopicRooms)) return onDirectory(body);
    if (topic == QLatin1String(kTopicSearch)) return onSearchEnvelope(uid, body);
    if (topic == QLatin1String(kTopicRelay)) return onRelayEnvelope(uid, body);
    if (topic == QLatin1String(kTopicHashLinks)) return onHashLinkPublish(body);
    for (Room &r : m_rooms)
        if (r.joined && r.topicStr == topic) return onRoomEnvelope(r, uid, body, data, false);
}

// --------------------------------------------------------------------------------- presence
void OverlayPlugin::announcePresence()
{
    if (!m_running) return;
    QJsonObject b = newBody("presence");
    b[QStringLiteral("node")] = m_node->nodeId();
    b[QStringLiteral("x")] = QString::fromLatin1(m_id.boxPublicKey().toHex());
    QJsonArray addrs;
    for (const QString &a : ourAddresses()) addrs << a;
    b[QStringLiteral("addrs")] = addrs;
    b[QStringLiteral("files")] = m_shared.size();
    b[QStringLiteral("conn")] = connectionLabel();
    b[QStringLiteral("client")] = m_ctx.clientName.isEmpty() ? QStringLiteral("OctoMX") : m_ctx.clientName;
    b[QStringLiteral("ver")] = m_ctx.clientVersion.isEmpty() ? QStringLiteral(MX_VERSION_STRING) : m_ctx.clientVersion;
    b[QStringLiteral("since")] = static_cast<double>(m_onlineSinceMs);
    b[QStringLiteral("pi")] = qMax(1, m_t.presenceMs / 1000);   // [ovl-2] our announce interval (s)
    b[QStringLiteral("caps")] = ourCaps();
    m_lastPresenceMs = nowMs();
    publishSigned(QString::fromLatin1(kTopicPresence), b);
}

void OverlayPlugin::touchUser(const QString &userId, const QJsonObject &body, const QString &nodeId)
{
    UserInfo &u = m_users[userId];
    const QString name = cleanText(jStr(body, "name", 48), 48);
    if (!name.isEmpty()) u.name = name;
    if (isHex(nodeId, 64) && !u.direct) u.nodeId = nodeId.toLower();
    u.lastSeenMs = nowMs();
}

void OverlayPlugin::onPresence(const QString &userId, const QJsonObject &b, bool bye)
{
    if (bye) {
        // [ovl-2] a bye is signed by the user: honour it also from a direct peer (it used to be ignored,
        // so a user who quit stayed Online until the link timed out)
        auto it = m_users.find(userId);
        if (it != m_users.end()) {
            it->lastSeenMs = 0;
            it->pingSentMs = 0;
            userWentOffline(userId, QStringLiteral("quit"));
        }
        return;
    }
    const QString node = jStr(b, "node", 64).toLower();
    touchUser(userId, b, node);
    UserInfo &u = m_users[userId];
    const QString x = jStr(b, "x", 64);
    if (isHex(x, 64)) u.xpub = QByteArray::fromHex(x.toLatin1());
    const int files = static_cast<int>(qBound(qint64(0), jInt(b, "files", 0), qint64(10000000)));
    const QString conn = cleanText(jStr(b, "conn", 24), 24);
    const bool changed = files != u.files || conn != u.conn;
    u.files = files;
    u.conn = conn;
    u.client = cleanText(jStr(b, "client", 32) + QLatin1Char(' ') + jStr(b, "ver", 16), 48);
    u.sinceMs = jInt(b, "since", 0);
    u.presenceIntervalSec = static_cast<int>(qBound(qint64(0), jInt(b, "pi", 0), qint64(3600)));
    if (b.contains(QStringLiteral("caps"))) {
        u.caps.clear();
        for (const QJsonValue &v : jArr(b, "caps", 16))
            if (v.isString()) u.caps << cleanText(v.toString(), 16);
    }
    u.pingSentMs = 0;
    if (!u.direct) {
        u.addrs.clear();
        for (const QJsonValue &v : jArr(b, "addrs", 8))
            if (v.isString()) u.addrs << cleanText(v.toString(), 64);
    }
    if (!u.online || changed) {
        const bool cameBack = !u.online;
        u.online = false;   // force the signal
        setUserOnline(userId, true);
        if (cameBack) pmRetryFor(userId);
    }
}

void OverlayPlugin::setUserOnline(const QString &userId, bool online)
{
    auto it = m_users.find(userId);
    if (it == m_users.end() || it->online == online) return;
    it->online = online;
    if (online) {
        it->offlineSinceMs = 0;
        it->offlineWhy.clear();
    }
    const int files = it->files;
    const QString conn = it->conn;
    Q_EMIT events(m_events)->userPresence(userRefFor(userId), online, files, conn);
    if (m_pmPartners.contains(userId)) {
        // [ovl-2] an open message window tells it too (System line; the core files it under the user)
        ChatMessage m;
        m.from = userRefFor(userId);
        m.kind = ChatKind::System;
        m.time = QDateTime::currentDateTime();
        const auto u = m_users.constFind(userId);
        const QString why = u == m_users.constEnd() ? QString() : u->offlineWhy;
        m.text = online ? tr("%1 is online.").arg(m.from.name)
                 : why.isEmpty() ? tr("%1 is offline.").arg(m.from.name)
                                 : tr("%1 is offline (%2).").arg(m.from.name, why);
        Q_EMIT events(m_events)->messageReceived(m);
    }
}

// [ovl-2] everything that shows a user as present agrees: presence, room user lists, PM windows
void OverlayPlugin::userWentOffline(const QString &userId, const QString &why)
{
    auto it = m_users.find(userId);
    if (it == m_users.end()) return;
    it->pingSentMs = 0;
    if (!it->online) return;
    it->offlineSinceMs = nowMs();
    it->offlineWhy = why;
    for (Room &r : m_rooms) {
        if (!r.joined || !r.members.contains(userId)) continue;
        const Member mem = r.members.take(userId);
        emitRoomMessage(r, userId, mem.name, tr("%1 has left the room (%2)").arg(mem.name, why), ChatKind::Part);
        Q_EMIT events(m_events)->roomUsersChanged(r.id);
    }
    setUserOnline(userId, false);
}

void OverlayPlugin::startPing(const QString &userId)
{
    auto it = m_users.find(userId);
    if (it == m_users.end()) return;
    it->pingSentMs = nowMs();
    it->pingId = randomId();
    qCDebug(lcOverlay) << "direct link to" << it->name << "lost: pinging through the mesh";
    QJsonObject x;
    x[QStringLiteral("pid")] = it->pingId;
    publishRelay("ping", userId, x);
}

qint64 OverlayPlugin::presenceTimeoutFor(const UserInfo &u) const
{
    if (u.presenceIntervalSec <= 0) return m_t.presenceTimeoutMs;   // older node: 120 s announcements
    return qMin<qint64>(m_t.presenceTimeoutMs, 2LL * u.presenceIntervalSec * 1000 + m_t.presenceGraceMs);
}

QJsonArray OverlayPlugin::ourCaps() const
{
    // [up-1] "part": serves files it is still downloading (slot "have" maps) and fetches from such
    // sources; get_close carries a reason
    return QJsonArray{QStringLiteral("pm_ack"), QStringLiteral("reach"), QStringLiteral("ping"), QStringLiteral("part")};
}

// [ovl-2] liveness of verified links: a quiet link gets a "ping" (every node, 4.2 too, answers "pong");
// a link that stays silent is dropped, so a crashed direct peer is offline in seconds
void OverlayPlugin::checkLinks()
{
    const qint64 now = nowMs();
    QStringList dead;
    for (auto it = m_peers.begin(); it != m_peers.end(); ++it) {
        if (!it->hello) continue;
        const qint64 quiet = now - it->lastRxMs;
        if (quiet > m_t.linkDeadMs) {
            dead << it.key();
        } else if (quiet > m_t.linkIdlePingMs && now - it->lastPingMs > m_t.linkIdlePingMs) {
            it->lastPingMs = now;
            QJsonObject p;
            p[QStringLiteral("t")] = QStringLiteral("ping");
            p[QStringLiteral("v")] = kWireVersion;
            p[QStringLiteral("ts")] = static_cast<double>(now);
            sendRaw(it.key(), p);
        }
    }
    for (const QString &id : dead) {
        qCDebug(lcOverlay) << "peer" << id.left(8) << "silent for" << m_t.linkDeadMs / 1000 << "s: dropping the link";
        if (m_node) m_node->disconnectPeer(id);
        dropPeer(id, QStringLiteral("no answer"));
    }
}

// [ovl-2] librats events are queued; a link can come back (or never be announced) between a disconnect
// and its handling. Bring m_peers in line with what librats really holds.
void OverlayPlugin::reconcilePeers()
{
    if (!m_node || !m_running) return;
    m_lastReconcileMs = nowMs();
    QSet<QString> live;
    for (const RatsPeer &p : m_node->peers()) {
        live.insert(p.id);
        if (!m_peers.contains(p.id)) {
            qCDebug(lcOverlay) << "peer" << p.id.left(8) << "is connected but was not known: taking it up";
            onPeerConnected(p);
        }
    }
    const QStringList known = m_peers.keys();
    for (const QString &id : known)
        if (!live.contains(id) && nowMs() - m_peers.value(id).connectedMs > 5000) dropPeer(id, QStringLiteral("gone"));
}

// [ovl-2] the target is not connected: dialling its addresses may not work (firewalled, NAT). Ask it
// through the mesh to connect back to us, and ask librats for a hole punch / relayed circuit.
void OverlayPlugin::requestReach(const QString &nodeId)
{
    if (!m_node || !m_running) return;
    const qint64 now = nowMs();
    if (now - m_reachAskedMs.value(nodeId, 0) < 5000) return;
    m_reachAskedMs.insert(nodeId, now);
    QString uid;
    for (auto it = m_users.cbegin(); it != m_users.cend(); ++it)
        if (it->nodeId == nodeId) { uid = it.key(); break; }
    if (!uid.isEmpty()) {
        QJsonObject x;
        x[QStringLiteral("node")] = m_node->nodeId();
        QJsonArray addrs;
        for (const QString &a : ourAddresses()) addrs << a;
        x[QStringLiteral("addrs")] = addrs;
        publishRelay("reach", uid, x);
    }
    m_node->reachPeer(nodeId);
}

void OverlayPlugin::onReachRequest(const QString &fromUser, const QJsonObject &b)
{
    const QString node = jStr(b, "node", 64).toLower();
    if (!isHex(node, 64) || !m_node) return;
    if (m_peers.contains(node) && m_peers.value(node).hello) return;   // a verified link exists already
    const qint64 now = nowMs();
    if (now - m_reachServedMs.value(fromUser, 0) < 5000) return;   // one connect-back per 5 s per user
    m_reachServedMs.insert(fromUser, now);
    if (m_reachServedMs.size() > 512) m_reachServedMs.clear();
    UserInfo &u = m_users[fromUser];
    if (u.name.isEmpty()) u.name = cleanText(jStr(b, "name", 48), 48);
    if (!u.direct) u.nodeId = node;
    int dialled = 0;
    for (const QJsonValue &v : jArr(b, "addrs", 8)) {
        if (!v.isString() || dialled >= 6) continue;
        m_node->connectTo(cleanText(v.toString(), 64));
        ++dialled;
    }
    // no relayed circuit from this side: the requester opens one itself, two would race and swap routes
    qCDebug(lcOverlay) << "connect-back for" << u.name << ":" << dialled << "address(es)";
}

UserRef OverlayPlugin::localUser() const
{
    if (m_userId.isEmpty()) return UserRef();   // no identity (no profile directory)
    UserRef r;
    r.net = Net::Overlay;
    r.id = m_userId;
    r.name = m_displayName;
    return r;
}

bool OverlayPlugin::isUserOnline(const UserRef &u) const
{
    const UserInfo *i = user(u.id);
    return i && i->online;
}

void OverlayPlugin::setHotlist(const QList<UserRef> &users)
{
    m_hotlist.clear();
    for (const UserRef &u : users)
        if (u.net == Net::Overlay && isHex(u.id, 64)) m_hotlist.insert(u.id.toLower());
}

void OverlayPlugin::connectPeer(const QString &hostPort)
{
    if (m_node) m_node->connectTo(hostPort);
}

// --------------------------------------------------------------------------------- servers
QList<ServerEntry> OverlayPlugin::servers() const
{
    QList<ServerEntry> out;
    QSet<QString> addrs;
    if (m_node && m_running) {
        for (const RatsPeer &p : m_node->peers()) {
            ServerEntry e;
            e.net = Net::Overlay;
            e.id = p.id;
            const QString uid = m_nodeUser.value(p.id);
            const UserInfo *u = uid.isEmpty() ? nullptr : user(uid);
            e.name = u ? u->name : QStringLiteral("OctoMX peer %1").arg(p.id.left(8));
            e.address = p.address;
            addrs.insert(p.address);
            e.state = u ? ConnState::Connected : ConnState::Connecting;
            if (u) {
                e.files = u->files;
                e.description = QStringLiteral("%1, %2, %3").arg(u->client, u->conn, p.inbound ? QStringLiteral("in") : QStringLiteral("out"));
            }
            out << e;
        }
    }
    for (const QString &b : setting(QString(keys::overlay::BootstrapPeers), QStringList()).toStringList()) {
        if (addrs.contains(b)) continue;
        ServerEntry e;
        e.net = Net::Overlay;
        e.id = b;
        e.name = tr("Bootstrap peer");
        e.address = b;
        e.state = ConnState::Disconnected;
        e.isStatic = true;
        out << e;
    }
    return out;
}

void OverlayPlugin::addServer(const QString &address, const QString &)
{
    const QString a = address.trimmed();
    if (a.isEmpty()) return;
    if (m_ctx.settings) {
        QStringList l = m_ctx.settings->sl(QString(keys::overlay::BootstrapPeers));
        if (!l.contains(a)) {
            l << a;
            m_ctx.settings->setValue(QString(keys::overlay::BootstrapPeers), l);
        }
    }
    if (m_node && m_running) m_node->connectTo(a);
    Q_EMIT serversChanged();
}

void OverlayPlugin::removeServer(const QString &id)
{
    if (m_ctx.settings) {
        QStringList l = m_ctx.settings->sl(QString(keys::overlay::BootstrapPeers));
        if (l.removeAll(id) > 0) m_ctx.settings->setValue(QString(keys::overlay::BootstrapPeers), l);
    }
    if (m_node && isHex(id, 64)) m_node->disconnectPeer(id);
    Q_EMIT serversChanged();
}

void OverlayPlugin::connectServer(const QString &id)
{
    if (m_node && m_running && id.contains(QLatin1Char(':'))) m_node->connectTo(id);
}

void OverlayPlugin::disconnectServer(const QString &id)
{
    if (m_node && isHex(id, 64)) m_node->disconnectPeer(id);
}

void OverlayPlugin::dialBootstrap()
{
    if (!m_node || !m_running) return;
    m_lastBootstrapMs = nowMs();
    for (const QString &b : setting(QString(keys::overlay::BootstrapPeers), QStringList()).toStringList())
        m_node->connectTo(b);
}

// --------------------------------------------------------------------------------- periodic
void OverlayPlugin::tick()
{
    if (!m_running) return;
    ++m_tickCount;
    const qint64 now = nowMs();
    const bool havePeers = !m_peers.isEmpty();
    // presence: soon after the first peer, then every presenceMs
    if (havePeers && (m_lastPresenceMs == 0 ? m_startedAt.elapsed() > 1000 : now - m_lastPresenceMs >= m_t.presenceMs))
        announcePresence();
    // [set-1] the connection type others see changed by itself (Settings > Queuing: another type while
    // this many uploads run): told at once, not at the next announcement a minute later
    if (const QString conn = connectionLabel(); conn != m_announcedConn) {
        const bool first = m_announcedConn.isEmpty();
        m_announcedConn = conn;
        if (!first && havePeers) {
            announcePresence();
            for (Room &r : m_rooms)
                if (r.joined) roomHeartbeat(r);
        }
    }
    if (now - m_lastHeartbeatMs >= m_t.heartbeatMs) {
        m_lastHeartbeatMs = now;
        for (Room &r : m_rooms)
            if (r.joined) roomHeartbeat(r);
    }
    if (now - m_lastDirectoryMs >= m_t.directoryMs) {
        m_lastDirectoryMs = now;
        announceRooms();
    }
    // users whose presence expired, and unanswered pings after a lost direct link
    QStringList gone, pingLost;
    for (auto it = m_users.begin(); it != m_users.end(); ++it) {
        if (!it->online || it->direct) continue;
        if (it->pingSentMs > 0 && now - it->pingSentMs > m_t.pingWaitMs) pingLost << it.key();
        else if (it->pingSentMs == 0 && now - it->lastSeenMs > presenceTimeoutFor(*it)) gone << it.key();
    }
    for (const QString &u : pingLost) userWentOffline(u, QStringLiteral("connection lost"));
    for (const QString &u : gone) userWentOffline(u, QStringLiteral("timed out"));
    if (now - m_lastReconcileMs > 5000) reconcilePeers();
    if (m_tickCount % 4 == 0) checkLinks();
    if (m_tickCount % 8 == 0) relayRoomTopics();
    pmTick();
    expireMembers();
    expirePending();
    expireRequests();
    finishSearches();
    askSources();
    uploadsTick();
    for (auto it = m_hlReqs.begin(); it != m_hlReqs.end();) {
        if (now - it->sentMs > 60000) it = m_hlReqs.erase(it);
        else ++it;
    }
    // alone: dial the bootstrap peers again (5 s after losing the last peer, then every minute)
    if (m_peers.isEmpty() && now - m_lastBootstrapMs > (now - m_lastPeerLostMs < 15000 ? 5000 : 60000)) dialBootstrap();
    if (m_tickCount % 4 == 0) updateStatus();
    if (m_tickCount % 1200 == 0) {   // ~5 min
        saveHashLinks();
        m_peerGate.prune();
        m_floodGate.prune();
    }
}

void OverlayPlugin::updateStatus(bool force)
{
    NetworkStatus st;
    st.net = Net::Overlay;
    if (!m_running) {
        st.state = m_status.state == ConnState::Error ? ConnState::Error : ConnState::Disconnected;
        st.statusText = st.state == ConnState::Error ? m_status.statusText : tr("Not connected");
    } else {
        const int peers = m_node ? m_node->peerCount() : 0;
        int online = 0;
        qint64 files = m_shared.size();
        for (const UserInfo &u : m_users)
            if (u.online) { ++online; files += u.files; }
        st.peers = peers;
        // [core-5] primary connections: OctoMX peers that completed the signed hello, each user once
        // (not librats links still in their handshake, not a second link to the same user)
        {
            QSet<QString> verified;
            for (auto it = m_peers.cbegin(); it != m_peers.cend(); ++it)
                if (it->hello) verified.insert(m_nodeUser.value(it.key(), it.key()));
            st.primaryLinks = int(verified.size());
        }
        st.usersOnline = online + 1;
        st.filesOnline = files;
        st.listenPort = m_node ? m_node->listenPort() : 0;
        const QStringList obs = m_node ? m_node->observedAddresses() : QStringList();
        st.externalAddress = m_node && !m_node->externalAddress().isEmpty() ? m_node->externalAddress()
                             : obs.isEmpty() ? QString() : obs.front();
        const QString fw = setting(QString(keys::connection::Firewalled), QStringLiteral("auto")).toString();
        // [ui-h] "Unable to accept incoming TCP connections" (Settings > Incoming TCP) = firewalled
        const bool noIncoming = !setting(QString(keys::connection::AcceptIncoming), true).toBool();
        st.firewalled = fw == QLatin1String("firewalled") || noIncoming ? true
                        : fw == QLatin1String("open")     ? false
                                                          : (!m_sawInbound && m_startedAt.elapsed() > 120000);
        if (peers == 0) {
            if (m_startedAt.elapsed() < m_t.aloneAfterMs) {
                st.state = ConnState::Connecting;
                st.statusText = tr("Connecting...");
            } else {
                st.state = ConnState::Connected;
                st.statusText = tr("Connected (no other OctoMX users found yet)");
            }
        } else {
            st.state = ConnState::Connected;
            st.statusText = online > 0 ? tr("Connected: %n peer(s), %1 users online", nullptr, peers).arg(online + 1)
                                       : tr("Connected: %n peer(s)", nullptr, peers);
        }
    }
    const bool changed = st.state != m_status.state || st.statusText != m_status.statusText ||
                         st.peers != m_status.peers || st.firewalled != m_status.firewalled ||
                         st.usersOnline != m_status.usersOnline || st.primaryLinks != m_status.primaryLinks;
    m_status = st;
    if (changed || force) Q_EMIT statusChanged(st);
}

} // namespace mx::ovl

mx::NetworkPlugin *mx::createOverlayPlugin(QObject *parent)
{
    return new mx::ovl::OverlayPlugin(parent);
}
