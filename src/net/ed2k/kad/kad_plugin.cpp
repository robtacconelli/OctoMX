// SPDX-License-Identifier: GPL-3.0-or-later
#include "kad_plugin.h"

#include <QtCore/QTimer>

#include "../ed2k_io.h"
#include "../ed2k_opcodes.h"
#include "../ed2k_peer.h"
#include "../ed2k_plugin_impl.h"
#include "../ed2k_server.h"
#include "../ed2k_upload.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QRegularExpression>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QHostInfo>

#include "mx/net_policy.h"
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>

namespace mx::ed2k::kad {

static const char *const kDefaultNodesUrls[] = {
    "http://upd.emule-security.org/nodes.dat",   // checked live 2026-11-20 (v2, 175 contacts)
    "http://www.alldivx.de/nodes/nodes.dat",     // checked live 2026-11-20 (v2, 200 contacts)
};

KadPlugin::KadPlugin(std::shared_ptr<Ed2kCore> core, QObject *parent) : mx::NetworkPlugin(parent), m_core(std::move(core))
{
    m_clock.start();
    m_tick.setInterval(1000);
    connect(&m_tick, &QTimer::timeout, this, &KadPlugin::onTick);
    Ed2kCore *c = m_core.get();
    connect(c, &Ed2kCore::kadDatagram, this, &KadPlugin::onKadDatagram);
    connect(c, &Ed2kCore::kadTcpPacket, this, &KadPlugin::onKadTcp);
    connect(c, &Ed2kCore::peerHandshake, this, &KadPlugin::onPeerHandshake);
    connect(c, &Ed2kCore::sourcesFound, this, [this](const QString &id, const QList<mx::SearchSource> &list) {
        QList<mx::SearchSource> mine;
        for (const mx::SearchSource &s : list) if (s.net == mx::Net::Kad) mine << s;
        if (!mine.isEmpty()) emit sourcesFound(id, mine);
    });
    // engine-wide signals while the ED2K facade is not running
    connect(c, &Ed2kCore::hashesLearned, this, [this](const QString &id, const mx::FileHashes &h) { if (primary()) emit hashesLearned(id, h); });
    connect(c, &Ed2kCore::hashSetLearned, this, [this](const QString &id, const QString &k, qint64 u, const QList<QByteArray> &hs) {
        if (primary()) emit hashSetLearned(id, k, u, hs);
    });
    connect(c, &Ed2kCore::uploadChanged, this, [this](const mx::TransferInfo &t) { if (primary()) emit uploadChanged(t); });
    connect(c, &Ed2kCore::uploadFinished, this, [this](const QString &id) { if (primary()) emit uploadFinished(id); });
    connect(c, &Ed2kCore::logMessage, this, [this](const QString &t) { if (primary()) emit logMessage(t); });
}

KadPlugin::~KadPlugin()
{
    if (m_started) stop();
}

bool KadPlugin::primary() const
{
    return m_core->facadeActive(Ed2kCore::FacadeKad) && !m_core->facadeActive(Ed2kCore::FacadeEd2k);
}

qint64 KadPlugin::kadNowSecs() const { return QDateTime::currentSecsSinceEpoch(); }

void KadPlugin::init(const mx::PluginContext &ctx)
{
    m_ctx = ctx;
    if (!m_core->initialized()) {
        // the engine keeps its state (user hash, re-ask registry, server.met) in the eD2k data dir
        mx::PluginContext c = ctx;
        c.dataDir = !ctx.profileDir.isEmpty() ? QDir(ctx.profileDir).filePath(QStringLiteral("ed2k"))
                                              : QDir(ctx.dataDir).filePath(QStringLiteral("ed2k"));
        m_core->init(c);
    }
    m_cfg.dataDir = !ctx.dataDir.isEmpty() ? ctx.dataDir : QDir(ctx.profileDir).filePath(QStringLiteral("kad"));
    QDir().mkpath(m_cfg.dataDir);
    m_inited = true;
}

QStringList KadPlugin::bootstrapSetting() const
{
    const QVariant v = m_core->cfg().get(QStringLiteral("kad/bootstrapNodes"), QStringList());
    QStringList l = v.toStringList();
    if (l.isEmpty() && v.typeId() == QMetaType::QString) l = v.toString().split(QRegularExpression(QStringLiteral("[,;\\s]+")), Qt::SkipEmptyParts);
    return l;
}

void KadPlugin::start()
{
    if (!m_inited || m_started) return;
    {   // [net-2] Kad is UDP only: off while a proxy is set, and without the chosen adapter
        const mx::netpolicy::Verdict v = mx::netpolicy::verdict(mx::Net::Kad, m_core->ctx().settings);
        if (!v.allowed) {
            emit logMessage(v.reason);
            return;
        }
    }
    m_started = true;
    const Config &cfg = m_core->cfg();
    // [engine-3] both keys are (hidden) schema entries now, so a nested fallback would never apply
    m_cfg.allowLan = cfg.b(QStringLiteral("kad/allowLan"), false) || cfg.b(QStringLiteral("ed2k/allowLan"), false);
    m_cfg.allowSameIp = cfg.b(QStringLiteral("kad/allowSameIp"), false);
    m_cfg.storeIndex = cfg.b(QStringLiteral("kad/storeIndex"), true);
    m_cfg.firewallChecks = cfg.b(QStringLiteral("kad/firewallChecks"), true);
    m_cfg.forceTcpFirewalled = cfg.b(QStringLiteral("kad/forceFirewalled"), false);
    m_cfg.forceUdpFirewalled = cfg.b(QStringLiteral("kad/forceUdpFirewalled"), false);
    m_cfg.publish = cfg.b(QStringLiteral("kad/publish"), true);
    m_cfg.buddies = cfg.b(QStringLiteral("kad/buddies"), true);
    m_cfg.findBuddyDelaySecs = qBound(1, cfg.i(QStringLiteral("kad/findBuddyDelay"), 300), 3600);
    m_cfg.maxPacketsPerSec = qBound(10, cfg.i(QStringLiteral("kad/maxPacketsPerSec"), 80), 1000);
    m_core->acquire(Ed2kCore::FacadeKad);
    if (!m_node) {
        m_node = std::make_unique<KadNode>(this, m_cfg);
        connect(m_node.get(), &KadNode::searchHits, this, &NetworkPlugin::searchHits);
        connect(m_node.get(), &KadNode::searchDone, this, &NetworkPlugin::searchDone);
        connect(m_node.get(), &KadNode::sourcesFound, this, &KadPlugin::onSources);
        connect(m_node.get(), &KadNode::logMessage, this, &NetworkPlugin::logMessage);
        connect(m_node.get(), &KadNode::incomingSearches, this, &NetworkPlugin::incomingSearches);   // [engine-3]
        connect(m_node.get(), &KadNode::statusChanged, this, [this] { syncState(); emit statusChanged(status()); });
    }
    m_core->caps().kadPort = m_core->udpLocalPort();
    m_core->caps().kadVersion = KADEMLIA_VERSION;
    m_core->deobfuscator = [this](quint32 ip, const QByteArray &raw) { return m_node->deobfuscate(ip, raw); };
    m_core->kadBuddyCallback = [this](const PeerDescriptor &p, const QByteArray &fileHash) {
        return m_node && m_node->sendCallbackRequest(p.buddyIp, p.buddyPort, p.buddyId, fileHash);
    };
    m_node->start();
    for (const QString &hp : bootstrapSetting()) addServer(hp, QString());
    m_fetchedThisSession = false;
    if (m_node->contacts() < 20 && cfg.b(QStringLiteral("kad/autoBootstrap"), true)) fetchNodesDat();
    m_tick.start();
    syncState();
    emit statusChanged(status());
}

void KadPlugin::stop()
{
    if (!m_started) return;
    m_started = false;
    m_tick.stop();
    if (m_node) m_node->stop();
    if (m_buddyPc) m_buddyPc->close(QStringLiteral("Kad stopped"));
    if (m_servedBuddyPc) m_servedBuddyPc->close(QStringLiteral("Kad stopped"));
    m_buddyConnecting = false;
    m_core->deobfuscator = nullptr;
    m_core->kadBuddyCallback = nullptr;
    LocalCaps &caps = m_core->caps();
    caps.kadPort = 0;
    caps.kadVersion = 0;
    caps.buddyIp = 0;
    caps.buddyPort = 0;
    caps.directCallback = false;
    m_core->kadState() = KadState();
    m_core->release(Ed2kCore::FacadeKad);
    emit statusChanged(status());
}

void KadPlugin::syncState()
{
    if (!m_node) return;
    KadState &k = m_core->kadState();
    k.running = m_started && m_node->running();
    k.connected = m_node->connected();
    k.tcpFirewalled = m_node->firewalled();
    k.ip = m_node->kadIp() ? kadIpToEd2k(m_node->kadIp()) : 0;
    {   // [diag-1] the eD2k facade reports the shared UDP socket's reachability from this
        const KadNode::FirewallInfo f = m_node->firewallInfo();
        k.udpChecked = f.checksOn && (f.udpVerified || f.udpTimedOut || f.udpFinished >= f.udpNeeded);
        k.udpOpen = f.udpVerified || (k.udpChecked && !m_node->udpFirewalled());
    }
    LocalCaps &caps = m_core->caps();
    const bool useBuddy = k.tcpFirewalled && m_node->hasBuddy();
    caps.buddyIp = useBuddy ? m_node->buddyIp() : 0;
    caps.buddyPort = useBuddy ? m_node->buddyPort() : 0;
    caps.directCallback = m_node->directCallbackPossible();
}

void KadPlugin::onTick()
{
    if (!m_started || !m_node) return;
    m_node->tick();
    ++m_tickCount;
    const qint64 now = m_clock.elapsed();
    if (m_buddyPc && m_buddyPc->isOpen() && now >= m_buddyPingAtMs) {
        m_buddyPc->send(PROT_EMULE, OP_BUDDYPING);
        m_buddyPingAtMs = now + 10 * 60 * 1000;
    }
    if (!m_expectBuddyHash.isEmpty() && now > m_expectBuddyUntilMs) m_expectBuddyHash.clear();
    // still not connected a minute after start: try a downloaded nodes.dat (once per session)
    if (m_tickCount % 30 == 0 && !m_node->connected() && !m_fetchedThisSession && !m_fetching
        && m_core->cfg().b(QStringLiteral("kad/autoBootstrap"), true) && m_tickCount >= 60)
        fetchNodesDat();
    syncState();
    const QString text = status().statusText;
    if (m_tickCount % 5 == 0 || text != m_lastStatusText) {
        m_lastStatusText = text;
        emit statusChanged(status());
    }
}

void KadPlugin::onKadDatagram(quint32 ip, quint16 port, const QByteArray &plain, bool obfuscated, quint32 rk, quint32 sk)
{
    if (!m_started || !m_node) return;
    const bool valid = obfuscated && rk != 0 && rk == m_node->verifyKeyFor(ip);
    m_node->handleDatagram(ip, port, plain, obfuscated, valid, sk);
}

void KadPlugin::kadSendDatagram(quint32 ed2kIp, quint16 port, const QByteArray &bytes)
{
    m_core->sendUdp(ed2kIp, port, bytes);
}

quint8 KadPlugin::kadCryptOptions() const
{
    return quint8((m_core->cryptSupported() ? 1 : 0) | (m_core->cryptRequested() ? 2 : 0) | (m_core->cryptRequired() ? 4 : 0));
}

// ---------------------------------------------------------------- nodes.dat download
void KadPlugin::fetchNodesDat()
{
    if (m_fetching) return;
    m_fetchQueue.clear();
    const Config &cfg = m_core->cfg();
    QString url = cfg.str(QStringLiteral("kad/nodesDatUrl"), QString()).trimmed();
    if (!url.isEmpty()) m_fetchQueue << url;
    for (const char *u : kDefaultNodesUrls) if (!m_fetchQueue.contains(QLatin1String(u))) m_fetchQueue << QLatin1String(u);
    m_fetchedThisSession = true;
    fetchNext();
}

void KadPlugin::fetchNext()
{
    if (!m_started || m_fetchQueue.isEmpty()) { m_fetching = false; return; }
    m_fetching = true;
    const QString url = m_fetchQueue.takeFirst();
    QNetworkRequest req{QUrl(url)};
    req.setTransferTimeout(20000);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("OctoMX"));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *r = m_core->nam()->get(req);
    connect(r, &QNetworkReply::downloadProgress, r, [r](qint64 got, qint64) { if (got > 2 * 1024 * 1024) r->abort(); });
    connect(r, &QNetworkReply::finished, this, [this, r, url] {
        r->deleteLater();
        int n = 0;
        if (r->error() == QNetworkReply::NoError && m_node && m_started) {
            const QByteArray data = r->readAll();
            n = m_node->loadNodesDat(data, url);
        } else {
            emit logMessage(QStringLiteral("nodes.dat download from %1 failed: %2").arg(url, r->errorString()));
        }
        ++m_nodesFetches;
        if (n > 0) { m_fetching = false; m_fetchQueue.clear(); return; }
        fetchNext();
    });
}

// ---------------------------------------------------------------- [diag-1] Connection Test
mx::PluginDiagnostics KadPlugin::diagnostics() const
{
    mx::PluginDiagnostics d;
    d.valid = true;
    if (!m_started || !m_node || !m_node->running()) return d;
    mx::PortBinding udp;
    udp.proto = QStringLiteral("UDP");
    udp.port = m_core->udpLocalPort() ? m_core->udpLocalPort() : m_core->wantedUdpPort();
    udp.ok = m_core->udpLocalPort() != 0;
    udp.error = m_core->udpBindError();
    udp.purpose = QStringLiteral("Kad and eD2k client UDP");
    d.bindings << udp;
    mx::PortBinding tcp;
    tcp.proto = QStringLiteral("TCP");
    tcp.port = m_core->tcpPort();
    tcp.ok = m_core->listening();
    tcp.error = m_core->tcpBindError();
    tcp.purpose = QStringLiteral("Kad firewall check, eD2k peers");
    d.bindings << tcp;
    const KadNode::FirewallInfo f = m_node->firewallInfo();
    if (!f.checksOn) {
        d.tcpEvidence = d.udpEvidence = QStringLiteral("Kad firewall checks are switched off");
    } else {
        if (f.tcpAcks >= 2) {
            d.tcpIn = mx::Reach::Yes;
            d.tcpEvidence = QStringLiteral("Kad firewall check: %1 nodes connected back").arg(f.tcpAcks);
        } else if (f.tcpChecked) {
            d.tcpIn = m_node->firewalled() ? mx::Reach::No : mx::Reach::Yes;
            d.tcpEvidence = m_node->firewalled() ? QStringLiteral("Kad firewall check: %1 nodes answered, none could connect back").arg(f.tcpAnswers)
                                                 : QStringLiteral("Kad firewall check passed");
        } else {
            d.tcpEvidence = QStringLiteral("Kad firewall check running (%1 of %2 answers)").arg(f.tcpAnswers).arg(f.tcpNeeded);
        }
        if (f.udpVerified) {
            d.udpIn = mx::Reach::Yes;
            d.udpEvidence = QStringLiteral("Kad UDP firewall test packet arrived");
        } else if (f.udpTimedOut || f.udpFinished >= f.udpNeeded) {
            d.udpIn = m_node->udpFirewalled() ? mx::Reach::No : mx::Reach::Yes;
            d.udpEvidence = f.udpTimedOut ? QStringLiteral("Kad UDP firewall test timed out") : QStringLiteral("Kad UDP firewall test finished");
        } else {
            d.udpEvidence = QStringLiteral("Kad UDP firewall test running (%1 of %2)").arg(f.udpFinished).arg(f.udpNeeded);
        }
    }
    if (m_node->kadIp()) {
        d.externalAddress = kadIpToString(m_node->kadIp());
        d.externalSeenBy = QStringLiteral("Kad nodes");
    }
    d.externalUdpPort = m_node->externalPort();
    if (d.externalUdpPort > 0 && d.externalUdpPort != udp.port)
        d.items << mx::DiagItem{QStringLiteral("kad.extport"), QStringLiteral("External UDP port"), mx::DiagLevel::Limited,
                                QString::number(d.externalUdpPort), QStringLiteral("internal %1").arg(udp.port),
                                QStringLiteral("Kad nodes see your UDP port %1 as %2: the router translates the port, so it is not forwarded. "
                                               "Forward UDP port %1 to this computer (or enable UPnP).").arg(udp.port).arg(d.externalUdpPort)};
    d.dhtNodes = m_node->contacts();
    d.dhtReplies = int(qMin<quint64>(m_node->counters().packetsIn, 0x7fffffff));
    d.items << mx::DiagItem{QStringLiteral("kad.buddy"), QStringLiteral("Buddy"), mx::DiagLevel::Ok,
                            m_node->hasBuddy() ? QStringLiteral("Yes") : QStringLiteral("No"),
                            m_node->firewalled() ? QStringLiteral("firewalled users need one to be reached") : QStringLiteral("not needed"), QString()};
    {   // [enc-1] Kad 2 packet obfuscation (contacts of version 6 and later; always on, as in eMule)
        const KadCounters &k = m_node->counters();
        const quint64 inAll = k.obfuscatedIn + k.plainIn;
        mx::DiagItem e{QStringLiteral("kad.encryption"), QStringLiteral("Obfuscation"),
                       k.packetsOut > 0 ? mx::DiagLevel::Ok : mx::DiagLevel::Unknown,
                       QStringLiteral("%1 of %2 packets sent obfuscated").arg(k.obfuscatedOut).arg(k.packetsOut),
                       QStringLiteral("%1 of %2 received").arg(k.obfuscatedIn).arg(inAll),
                       QStringLiteral("Kad packets to contacts whose Kad ID is known are obfuscated (eMule Kad 2); bootstrap and "
                                      "first contact packets are plain. This hides the content from simple filtering, not your IP address.")};
        d.items << e;
    }
    return d;
}

void KadPlugin::recheckConnectivity()
{
    if (m_node && m_node->running()) m_node->recheckFirewall();
}

void KadPlugin::portMappingChanged(const mx::PortMapping &m)
{
    if (m.result != mx::PortMapping::Mapped || m_recheckedForMapping || !m_node || !m_node->running()) return;
    const bool ours = (m.proto == QLatin1String("UDP") && m.internalPort == m_core->udpLocalPort())
                      || (m.proto == QLatin1String("TCP") && m.internalPort == m_core->tcpPort());
    if (!ours || (!m_node->firewalled() && !m_node->udpFirewalled())) return;
    m_recheckedForMapping = true;
    QTimer::singleShot(3000, this, [this] { if (m_node && m_node->running()) m_node->recheckFirewall(); });
}

// ---------------------------------------------------------------- status / nodes list
mx::NetworkStatus KadPlugin::status() const
{
    mx::NetworkStatus st;
    st.net = mx::Net::Kad;
    st.listenPort = m_core->udpLocalPort();
    if (!m_started || !m_node || !m_node->running()) {
        st.state = mx::ConnState::Disconnected;
        st.statusText = QStringLiteral("Offline");
        return st;
    }
    const KadNode *n = m_node.get();
    st.state = n->connected() ? mx::ConnState::Connected : mx::ConnState::Connecting;
    st.peers = n->contacts();
    st.firewalled = n->firewalled();
    if (n->kadIp()) st.externalAddress = kadIpToString(n->kadIp());
    QString text = n->statusText();
    if (n->connected()) {
        const quint32 users = n->estimatedUsers();
        if (users) {
            st.usersOnline = users;
            // eMule: files ≈ users × average files per user (server statistics, at least 108)
            qint64 avg = 108;
            if (ServerConnection *c = m_core->servers() ? m_core->servers()->current() : nullptr)
                if (c->info().users > 0) avg = qMax<qint64>(108, c->info().files / c->info().users);
            st.filesOnline = qint64(users) * avg;
        }
        const KadCounters &k = n->counters();
        const quint64 answered = k.answeredHello + k.answeredReq + k.answeredBootstrap + k.answeredPing + k.answeredSearch
                                 + k.answeredPublish + k.answeredFirewall + k.answeredBuddy;
        text += QStringLiteral(", %1 answered").arg(mx::countNoun(qint64(answered), QStringLiteral("request")));
        if (n->publishedKeywordCount())
            text += QStringLiteral(", published %1 keyword(s), %2 file(s)").arg(k.keywordsPublished).arg(k.filesPublished);
    }
    st.statusText = text;
    st.rateDownBps = m_core->rateDownBps();
    st.rateUpBps = m_core->rateUpBps();
    return st;
}

QList<mx::ServerEntry> KadPlugin::servers() const
{
    QList<mx::ServerEntry> out;
    for (const QString &hp : bootstrapSetting()) {
        mx::ServerEntry e;
        e.net = mx::Net::Kad;
        e.id = hp;
        e.address = hp;
        e.name = QStringLiteral("Bootstrap node");
        e.isStatic = true;
        e.state = mx::ConnState::Disconnected;
        out << e;
    }
    if (m_node && m_node->routing()) {
        const auto contacts = m_node->routing()->closestContacts(m_node->id(), 4, 40, false);
        for (const Contact *c : contacts) {
            mx::ServerEntry e;
            e.net = mx::Net::Kad;
            e.address = QStringLiteral("%1:%2").arg(kadIpToString(c->ip)).arg(c->udpPort);
            e.id = e.address;
            e.name = QStringLiteral("Kad %1").arg(c->id.toHex().left(8));
            e.state = c->type <= 2 ? mx::ConnState::Connected : (c->type == 4 ? mx::ConnState::Error : mx::ConnState::Connecting);
            e.description = QStringLiteral("Kad v%1, type %2%3, TCP %4").arg(c->version).arg(c->type)
                                .arg(c->ipVerified ? QStringLiteral(", verified") : QString()).arg(c->tcpPort);
            out << e;
        }
    }
    return out;
}

static bool parseHostPortStr(const QString &s, QString *host, quint16 *port)
{
    const int c = int(s.lastIndexOf(QLatin1Char(':')));
    if (c <= 0) return false;
    bool ok = false;
    const uint p = s.mid(c + 1).toUInt(&ok);
    if (!ok || !p || p > 65535) return false;
    *host = s.left(c).trimmed();
    *port = quint16(p);
    return !host->isEmpty();
}

void KadPlugin::addServer(const QString &address, const QString &name)
{
    Q_UNUSED(name)
    QString host;
    quint16 port = 0;
    if (!parseHostPortStr(address.trimmed(), &host, &port)) return;
    const QString key = QStringLiteral("%1:%2").arg(host).arg(port);
    QStringList list = bootstrapSetting();
    if (!list.contains(key) && m_core->cfg().s) {
        list << key;
        m_core->cfg().s->setValue(QStringLiteral("kad/bootstrapNodes"), list);
        emit serversChanged();
    }
    if (!m_started || !m_node) return;
    QHostAddress a;
    if (a.setAddress(host) && a.protocol() == QAbstractSocket::IPv4Protocol) {
        m_node->addBootstrapNode(a.toIPv4Address(), port, true);
        return;
    }
    QPointer<KadPlugin> self(this);
    QHostInfo::lookupHost(host, this, [self, port](const QHostInfo &hi) {
        if (!self || !self->m_node) return;
        for (const QHostAddress &x : hi.addresses())
            if (x.protocol() == QAbstractSocket::IPv4Protocol) { self->m_node->addBootstrapNode(x.toIPv4Address(), port, true); break; }
    });
}

void KadPlugin::removeServer(const QString &id)
{
    QStringList list = bootstrapSetting();
    if (list.removeAll(id) && m_core->cfg().s) {
        m_core->cfg().s->setValue(QStringLiteral("kad/bootstrapNodes"), list);
        emit serversChanged();
    }
}

void KadPlugin::connectServer(const QString &id) { addServer(id, QString()); }

// ---------------------------------------------------------------- searches / sources
void KadPlugin::search(const mx::SearchQuery &q)
{
    QString err;
    if (!m_started || !m_node || !m_node->search(q, &err)) {
        if (m_started && !err.isEmpty()) emit logMessage(QStringLiteral("search \"%1\" not started: %2").arg(q.text, err));
        QMetaObject::invokeMethod(this, [this, id = q.id] { emit searchDone(id); }, Qt::QueuedConnection);
    }
}

void KadPlugin::cancelSearch(quint32 queryId) { if (m_node) m_node->cancelSearch(queryId); }

void KadPlugin::findSources(const QString &downloadId, const mx::FileHashes &h, qint64 size, const QString &name)
{
    if (h.ed2k.size() != 16 || size <= 0) return;
    m_core->ensureDownload(downloadId, h, size, name);
    if (m_node) m_node->findSources(downloadId, h.ed2k, size, name);
}

void KadPlugin::stopFindSources(const QString &downloadId) { if (m_node) m_node->stopFindSources(downloadId); }

mx::RangeSource *KadPlugin::openSource(const QString &downloadId, const mx::FileHashes &h, qint64 size, const mx::SearchSource &src)
{
    if (src.net != mx::Net::Kad && src.net != mx::Net::ED2K && src.net != mx::Net::Unknown) return nullptr;
    return openEd2kSource(m_core.get(), downloadId, h, size, src);
}

bool KadPlugin::startUploadNow(const QString &uploadId)
{
    return primary() && m_core->uploads() && m_core->uploads()->startUploadNow(uploadId);
}

bool KadPlugin::setUploadPriority(const QString &uploadId, int priority)
{
    return primary() && m_core->uploads() && m_core->uploads()->setUploadPriority(uploadId, priority);
}

void KadPlugin::cancelUpload(const QString &uploadId)
{
    if (primary() && m_core->uploads()) m_core->uploads()->cancelUpload(uploadId);
}

void KadPlugin::setSharedFiles(const QList<mx::SharedFile> &files)
{
    m_core->setSharedFiles(files);
    if (m_node) m_node->setSharedFiles(files);
}

void KadPlugin::onSources(const QString &downloadId, const QByteArray &fileHash, const QList<KadSource> &sources)
{
    Q_UNUSED(downloadId)
    QList<PeerDescriptor> list;
    for (const KadSource &s : sources) {
        PeerDescriptor d;
        d.port = s.tcpPort;
        d.userHash = s.userHash;
        d.cryptOptions = s.cryptOptions & 0x07;
        d.kadPort = s.udpPort;
        d.fileHash = fileHash;
        if (s.type == 3 || s.type == 5) {
            d.lowId = 1;
            d.buddyIp = s.buddyIp;
            d.buddyPort = s.buddyPort;
            d.buddyId = s.buddyId;
        } else {
            d.ip = s.ip;
            d.directCallback = s.type == 6;
        }
        list << d;
    }
    if (!list.isEmpty()) m_core->addSources(fileHash, list, QStringLiteral("Kad"), mx::Net::Kad);
}

// ---------------------------------------------------------------- TCP side
static PeerDescriptor descriptor(quint32 ip, quint16 port, const QByteArray &uh, quint8 crypt)
{
    PeerDescriptor d;
    d.ip = ip;
    d.port = port;
    if (uh.size() == 16) d.userHash = uh;
    d.cryptOptions = crypt & 0x07;
    return d;
}

bool KadPlugin::kadTcpFirewallCheck(quint32 ed2kIp, quint16 tcpPort, const QByteArray &userHash, quint8 cryptOptions)
{
    if (m_fwChecksRunning >= 4 || !tcpPort || !m_core->acceptableSourceIp(ed2kIp)) return false;
    PeerConnection *pc = m_core->connectPeer(descriptor(ed2kIp, tcpPort, userHash, cryptOptions));
    if (!pc) return false;
    ++m_fwChecksRunning;
    connect(pc, &PeerConnection::handshakeComplete, this, [](PeerConnection *p) {
        // the eMule way to say "your TCP port is reachable": an ACK over the connection we opened
        p->send(PROT_EMULE, OP_KAD_FWTCPCHECK_ACK);
        QPointer<PeerConnection> g(p);
        QTimer::singleShot(5000, p, [g] { if (g) g->close(QStringLiteral("Kad firewall check done")); });
    });
    connect(pc, &PeerConnection::closed, this, [this](PeerConnection *, const QString &) { m_fwChecksRunning = qMax(0, m_fwChecksRunning - 1); });
    return true;
}

bool KadPlugin::kadUdpFirewallCheck(quint32 ed2kIp, quint16 tcpPort, quint16 internPort, quint16 externPort, quint32 senderKey)
{
    if (!tcpPort || !m_core->acceptableSourceIp(ed2kIp)) return false;
    PeerConnection *pc = m_core->connectPeer(descriptor(ed2kIp, tcpPort, QByteArray(), 0));
    if (!pc) return false;
    auto sent = std::make_shared<bool>(false);
    connect(pc, &PeerConnection::handshakeComplete, this, [sent, internPort, externPort, senderKey](PeerConnection *p) {
        Writer w(8);
        w.u16(internPort).u16(externPort).u32(senderKey);
        p->send(PROT_EMULE, OP_FWCHECKUDPREQ, w.data());
        *sent = true;
        QPointer<PeerConnection> g(p);
        QTimer::singleShot(20000, p, [g] { if (g) g->close(QStringLiteral("Kad UDP firewall check sent")); });
    });
    connect(pc, &PeerConnection::closed, this, [this, sent, ed2kIp](PeerConnection *, const QString &) {
        if (!*sent && m_node) m_node->onUdpFirewallCheckFailed(ed2kIp);
    });
    return true;
}

bool KadPlugin::kadRequestBuddy(quint32 ed2kIp, quint16 tcpPort, quint16 udpPort, const QByteArray &userHash, quint8 cryptOptions)
{
    if (m_buddyPc || m_buddyConnecting || !tcpPort || !m_core->acceptableSourceIp(ed2kIp)) return false;
    PeerConnection *pc = m_core->connectPeer(descriptor(ed2kIp, tcpPort, userHash, cryptOptions));
    if (!pc) return false;
    m_buddyConnecting = true;
    m_buddyUdpPort = udpPort;
    connect(pc, &PeerConnection::handshakeComplete, this, [this](PeerConnection *p) {
        m_buddyConnecting = false;
        if (m_buddyPc || !m_node) { p->close(QStringLiteral("buddy not needed")); return; }
        m_buddyPc = p;
        p->setIdleTimeoutMs(30 * 60 * 1000);
        m_buddyPingAtMs = m_clock.elapsed() + 10 * 60 * 1000;
        m_node->buddyConnected(p->ip(), m_buddyUdpPort);
        syncState();
    });
    connect(pc, &PeerConnection::closed, this, [this](PeerConnection *p, const QString &) {
        if (p == m_buddyPc || !m_buddyPc) {
            m_buddyConnecting = false;
            if (p == m_buddyPc.data() && m_node) m_node->buddyLost();
            syncState();
        }
    });
    return true;
}

bool KadPlugin::kadIncomingBuddy(quint32 ed2kIp, quint16 tcpPort, const QByteArray &userHash)
{
    Q_UNUSED(tcpPort)
    if (kadHasServedBuddy() || userHash.size() != 16) return false;
    m_expectBuddyHash = userHash;
    m_expectBuddyIp = ed2kIp;
    m_expectBuddyUntilMs = m_clock.elapsed() + 2 * 60 * 1000;
    return true;
}

bool KadPlugin::kadHasServedBuddy() const { return m_servedBuddyPc && m_servedBuddyPc->isOpen(); }

bool KadPlugin::kadRelayCallback(const QByteArray &checkWire, const QByteArray &fileWire, quint32 requesterHostIp, quint16 requesterTcpPort)
{
    if (!kadHasServedBuddy() || checkWire.size() != 16 || fileWire.size() != 16) return false;
    Writer w(38);
    w.bytes(checkWire).bytes(fileWire).u32(requesterHostIp).u16(requesterTcpPort);
    m_servedBuddyPc->send(PROT_EMULE, OP_CALLBACK, w.data());
    return true;
}

void KadPlugin::onPeerHandshake(PeerConnection *pc)
{
    if (!m_started || !m_node) return;
    const PeerInfo &i = pc->info();
    if (!m_expectBuddyHash.isEmpty() && !pc->outgoing() && i.userHash == m_expectBuddyHash
        && m_clock.elapsed() <= m_expectBuddyUntilMs && !kadHasServedBuddy()) {
        m_servedBuddyPc = pc;
        m_expectBuddyHash.clear();
        pc->setIdleTimeoutMs(30 * 60 * 1000);
        emit logMessage(QStringLiteral("we are now the Kad buddy of %1").arg(pc->describe()));
    }
    if (i.kadPort && i.kadVersion > 1) m_node->bootstrapFromPeer(pc->ip(), i.kadPort, i.kadVersion);
}

void KadPlugin::onKadTcp(PeerConnection *pc, quint8 opcode, const QByteArray &payload)
{
    if (!m_started || !m_node) return;
    Reader r(payload);
    switch (opcode) {
    case OP_KAD_FWTCPCHECK_ACK:
        m_node->onTcpFirewallAck(pc->ip());
        return;
    case OP_FWCHECKUDPREQ: {
        const quint16 intern = r.u16(), ext = r.u16();
        const quint32 key = r.u32();
        if (!r.ok()) return;
        const bool known = pc->uploadRole || pc->downloadSink() != nullptr;
        m_node->answerUdpFirewallCheck(pc->ip(), intern, ext, key, known);
        return;
    }
    case OP_CALLBACK: {
        if (pc != m_buddyPc) return;   // only our buddy may tell us to connect somewhere
        const QByteArray check = r.bytes(16), file = r.bytes(16);
        const quint32 ip = r.u32();
        const quint16 tcp = r.u16();
        if (!r.ok() || !m_node->checkCallback(check, file)) return;
        const quint32 ed2kIp = kadIpToEd2k(ip);
        if (!m_core->acceptableSourceIp(ed2kIp) || !tcp) return;
        emit logMessage(QStringLiteral("Kad callback via buddy: connecting to %1:%2").arg(ipToString(ed2kIp)).arg(tcp));
        m_core->connectPeer(descriptor(ed2kIp, tcp, QByteArray(), 0));
        return;
    }
    case OP_BUDDYPING:
        if (pc == m_servedBuddyPc && m_clock.elapsed() >= m_servedPongAtMs) {
            pc->send(PROT_EMULE, OP_BUDDYPONG);
            m_servedPongAtMs = m_clock.elapsed() + 3 * 60 * 1000;
        }
        return;
    default:
        return;   // OP_BUDDYPONG, OP_REASKCALLBACKTCP
    }
}

} // namespace mx::ed2k::kad
