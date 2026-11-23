// SPDX-License-Identifier: GPL-3.0-or-later
#include "ed2k_core.h"
#include "ed2k_io.h"
#include "ed2k_opcodes.h"
#include "ed2k_peer.h"
#include "ed2k_search.h"
#include "ed2k_server.h"
#include "ed2k_source.h"
#include "ed2k_upload.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkDatagram>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QUdpSocket>

namespace mx::ed2k {

static constexpr qint64 kSxReaskMs = SOURCEEXCHANGE_REASK_MS;

Ed2kCore::Ed2kCore(QObject *parent) : QObject(parent)
{
    m_clock.start();
    m_tick.setInterval(1000);
    connect(&m_tick, &QTimer::timeout, this, &Ed2kCore::tick);
}

Ed2kCore::~Ed2kCore()
{
    if (m_running) stopBase();
}

void Ed2kCore::init(const mx::PluginContext &ctx)
{
    if (m_inited) return;
    m_inited = true;
    m_ctx = ctx;
    m_cfg.s = ctx.settings;
    m_dataDir = ctx.dataDir.isEmpty() ? QDir(ctx.profileDir).filePath(QStringLiteral("ed2k")) : ctx.dataDir;
    QDir().mkpath(m_dataDir);
    // user hash: persistent identity
    // persistent identity: ed2k/userHash (schema, hex) mirrored in <dataDir>/userhash.dat
    m_userHash = fromHex(m_cfg.str(QStringLiteral("ed2k/userHash"), QString()), 16);
    QFile f(QDir(m_dataDir).filePath(QStringLiteral("userhash.dat")));
    if (m_userHash.size() != 16 && f.open(QIODevice::ReadOnly)) { m_userHash = f.read(16); f.close(); }
    if (m_userHash.size() != 16) m_userHash = generateUserHash();
    if (!f.exists()) {
        QSaveFile out(f.fileName());
        if (out.open(QIODevice::WriteOnly)) { out.write(m_userHash); out.commit(); }
    }
    if (ctx.settings && fromHex(ctx.settings->value(QStringLiteral("ed2k/userHash")).toString(), 16) != m_userHash)
        ctx.settings->setValue(QStringLiteral("ed2k/userHash"), toHex(m_userHash));
    if (!m_servers) {
        m_servers = new ServerManager(this);
        connect(m_servers, &ServerManager::changed, this, &Ed2kCore::serversChanged);
        connect(m_servers, &ServerManager::connectedChanged, this, [this] {
            // new server: ask it for sources of every running download right away
            for (DownloadCtx &d : m_downloads) d.nextServerAskMs = 0;
            emit statusChanged();
        });
        connect(m_servers, &ServerManager::searchHits, this, &Ed2kCore::searchHits);
        connect(m_servers, &ServerManager::searchDone, this, &Ed2kCore::searchDone);
        connect(m_servers, &ServerManager::sourcesFound, this,
                [this](const QByteArray &h, const QList<PeerDescriptor> &l, const QString &o) { addSources(h, l, o); });
        connect(m_servers, &ServerManager::callbackRequested, this, &Ed2kCore::onCallbackRequested);
        connect(m_servers, &ServerManager::log, this, &Ed2kCore::log);
        connect(m_servers, &ServerManager::serverMessage, this, &Ed2kCore::serverMessage);   // [glue-2]
    }
    if (!m_uploads) m_uploads = new UploadManager(this);
    m_servers->load();
    loadState();
}

QString Ed2kCore::nick() const
{
    QString n = m_cfg.str(QStringLiteral("other/defaultLogin"), QString());
    if (n.isEmpty()) n = m_ctx.screenName;
    if (n.isEmpty()) n = QStringLiteral("OctoMX user");
    return n.left(50);
}

QString Ed2kCore::clientName() const
{
    const QString name = m_ctx.clientName.isEmpty() ? QStringLiteral("OctoMX") : m_ctx.clientName;
    const QString ver = m_ctx.clientVersion.isEmpty() ? QStringLiteral(MX_VERSION_STRING) : m_ctx.clientVersion;
    return name + QLatin1Char(' ') + ver;
}

quint32 Ed2kCore::emuleVersionTag() const
{
    // Compatible-client byte 0x4D ('M'): not one of the known eMule-family ids, so other clients show
    // us as "eMule Compat" plus our CT_MOD_VERSION string "OctoMX <MX_VERSION_STRING>" (e.g. 5.1).
    const QStringList v = (m_ctx.clientVersion.isEmpty() ? QStringLiteral(MX_VERSION_STRING) : m_ctx.clientVersion)
                              .split(QLatin1Char('.'));
    const quint32 maj = v.value(0).toUInt() & 0x7F, min = v.value(1).toUInt() & 0x7F, upd = v.value(2).toUInt() & 0x07;
    return (0x4Du << 24) | (maj << 17) | (min << 10) | (upd << 7);
}

int Ed2kCore::uploadSlots() const
{
    // ed2k/uploadSlots (schema default 0 = use the global transfers/maxUploads) [glue-2]
    const int global = m_cfg.i(QStringLiteral("transfers/maxUploads"), 3);
    const int own = m_cfg.i(QStringLiteral("ed2k/uploadSlots"), 0);
    return qBound(1, own > 0 ? own : (global > 0 ? global : 3), 50);
}

QNetworkAccessManager *Ed2kCore::nam()
{
    if (!m_nam) m_nam = new QNetworkAccessManager(this);
    return m_nam;
}

bool Ed2kCore::bindListener()
{
    if (!m_listener) {
        m_listener = new QTcpServer(this);
        m_listener->setMaxPendingConnections(50);
        connect(m_listener, &QTcpServer::newConnection, this, &Ed2kCore::onNewConnection);
    }
    // schema key ed2k/tcpPort (mx/settings_keys.h) [glue-2: the old ed2k/port alias was dropped]
    const quint16 want = quint16(qBound(0, m_cfg.i(QStringLiteral("ed2k/tcpPort"), 4662), 65535));
    for (int k = 0; k < 10; ++k) {
        const quint16 p = want ? quint16(want + k) : 0;
        if (m_listener->listen(QHostAddress::AnyIPv4, p)) {
            m_tcpPort = m_listener->serverPort();
            m_tcpError = k > 0 ? QStringLiteral("port %1 busy, using %2").arg(want).arg(m_tcpPort) : QString();
            if (k > 0) log(QStringLiteral("TCP port %1 busy, listening on %2").arg(want).arg(m_tcpPort));
            return true;
        }
    }
    log(QStringLiteral("cannot listen for TCP: %1").arg(m_listener->errorString()));
    m_tcpError = m_listener->errorString();
    m_tcpPort = want ? want : 4662;
    return false;
}

void Ed2kCore::acquire(int facade)
{
    if (!m_inited) return;
    const int before = m_facades;
    m_facades |= facade;
    if (!m_running) startBase();
    if ((facade & FacadeEd2k) && !(before & FacadeEd2k)) {
        m_servers->start();
        emit statusChanged();
    }
}

void Ed2kCore::release(int facade)
{
    const int before = m_facades;
    m_facades &= ~facade;
    if ((before & FacadeEd2k) && !(m_facades & FacadeEd2k) && m_servers) m_servers->stop();
    if (m_facades == 0 && m_running) stopBase();
    else emit statusChanged();
}

void Ed2kCore::startBase()
{
    if (m_running) return;
    m_running = true;
    // schema: ed2k/obfuscation (supported + requested), ed2k/requireObfuscation
    m_cryptSupported = m_cfg.b(QStringLiteral("ed2k/obfuscation"), true);
    m_cryptRequested = m_cryptSupported;
    m_cryptRequired = m_cryptSupported && m_cfg.b(QStringLiteral("ed2k/requireObfuscation"), false);
    m_allowLan = m_cfg.b(QStringLiteral("ed2k/allowLan"), false);
    m_listening = bindListener();
    if (!m_udp) {
        m_udp = new QUdpSocket(this);
        connect(m_udp, &QUdpSocket::readyRead, this, &Ed2kCore::onUdpReadyRead);
    }
    if (m_udp->state() != QAbstractSocket::BoundState) {
        // one UDP socket for eD2k client UDP and Kad (as eMule): ed2k/udpPort, else kad/udpPort
        const int kadUdp = m_cfg.i(QStringLiteral("kad/udpPort"), 4672);
        const quint16 up = quint16(qBound(0, m_cfg.i(QStringLiteral("ed2k/udpPort"), kadUdp), 65535));
        m_wantUdp = up;
        m_udpError.clear();
        if (!m_udp->bind(QHostAddress::AnyIPv4, up)) {
            m_udpError = QStringLiteral("port %1: %2").arg(up).arg(m_udp->errorString());
            if (!m_udp->bind(QHostAddress::AnyIPv4, 0))
                log(QStringLiteral("cannot bind UDP: %1").arg(m_udp->errorString()));
        }
        m_udpPort = m_udp->localPort();
    }
    // we answer eMule client UDP (OP_REASKFILEPING) from now on: announce UDP version 4
    m_caps.udpPort = m_udp->state() == QAbstractSocket::BoundState ? m_udp->localPort() : 0;
    m_tick.start();
    log(QStringLiteral("eD2k engine started: TCP %1%2, UDP %3, user hash %4").arg(m_tcpPort)
            .arg(m_listening ? QString() : QStringLiteral(" (not listening)")).arg(m_udpPort).arg(toHex(m_userHash)));
    emit statusChanged();
}

void Ed2kCore::stopBase()
{
    if (!m_running) return;
    m_running = false;
    m_tick.stop();
    m_servers->stop();
    m_uploads->stop();
    for (auto &p : m_pendingConnects) if (p) p->close(QStringLiteral("stopping"));
    m_pendingConnects.clear();
    const auto peers = m_peers;
    for (auto &p : peers) if (p) p->close(QStringLiteral("stopping"));
    if (m_listener) m_listener->close();
    if (m_udp) m_udp->close();
    m_caps.udpPort = 0;
    m_listening = false;
    saveState();
    emit statusChanged();
}

quint32 Ed2kCore::clientId() const { auto *c = m_servers ? m_servers->current() : nullptr; return c ? c->clientId() : 0; }
bool Ed2kCore::lowId() const { const quint32 id = clientId(); return id == 0 || isLowId(id); }
quint32 Ed2kCore::serverIp() const { auto *c = m_servers ? m_servers->current() : nullptr; return c ? c->info().ip : 0; }
quint16 Ed2kCore::serverPort() const { auto *c = m_servers ? m_servers->current() : nullptr; return c ? c->info().port : 0; }
quint32 Ed2kCore::publicIp() const { auto *c = m_servers ? m_servers->current() : nullptr; return c ? c->reportedIp() : 0; }

void Ed2kCore::log(const QString &text)
{
    qCInfo(lcEd2k).noquote() << text;
    emit logMessage(QStringLiteral("eD2k: ") + text);
}

// ---------------------------------------------------------------- UDP
bool Ed2kCore::sendUdp(quint32 ip, quint16 port, const QByteArray &d)
{
    if (!m_udp || m_udp->state() != QAbstractSocket::BoundState) return false;
    const bool ok = m_udp->writeDatagram(d, QHostAddress(ipToHostOrder(ip)), port) == d.size();
    if (ok) accountUdp(mx::RateLimiter::Up, d);
    return ok;
}

quint16 Ed2kCore::udpLocalPort() const { return m_udp ? m_udp->localPort() : 0; }

void Ed2kCore::onUdpReadyRead()
{
    for (int n = 0; n < 128 && m_udp->hasPendingDatagrams(); ++n) {
        const QNetworkDatagram dg = m_udp->receiveDatagram(65536);
        const QByteArray d = dg.data();
        accountUdp(mx::RateLimiter::Down, d);
        if (mx::consumeProbeDatagram(d)) continue;   // [diag-1] overlay port probe
        if (d.size() < 2) continue;
        const quint32 ip = ipFromHostOrder(dg.senderAddress().toIPv4Address());
        const quint16 port = quint16(dg.senderPort());
        const quint8 b0 = quint8(d[0]);
        if (b0 == PROT_EDONKEY) { m_servers->handleUdp(ip, port, d); continue; }
        emit udpDatagram(ip, port, d);
        if (b0 == PROT_EMULE) { handleClientUdp(ip, port, d, false); continue; }
        if (b0 == PROT_KAD || b0 == PROT_KADPACK) { emit kadDatagram(ip, port, d, false, 0, 0); continue; }
        // obfuscated: eD2k client UDP keyed by our user hash, or Kad keyed by our KadID / verify key
        const kad::Deobfuscated r = deobfuscator ? deobfuscator(ip, d) : kad::deobfuscate(d, nullptr, m_userHash, ip, 0);
        if (!r.obfuscated || r.plain.size() < 2) continue;   // junk, or server-obfuscated UDP (not supported)
        const quint8 p0 = quint8(r.plain[0]);
        if (p0 == PROT_EMULE && !r.kad) handleClientUdp(ip, port, r.plain, true);
        else if ((p0 == PROT_KAD || p0 == PROT_KADPACK) && r.kad) emit kadDatagram(ip, port, r.plain, true, r.receiverVerifyKey, r.senderVerifyKey);
    }
}

void Ed2kCore::accountUdp(int dir, const QByteArray &d)
{
    // [glue-3] UDP payload bytes for the bandwidth graph; Kad packets by their protocol
    // byte; obfuscated ones (mostly Kad) count as Kad while the Kad facade runs
    if (!m_ctx.limiter || d.isEmpty()) return;
    const quint8 b0 = quint8(d[0]);
    mx::Net net = mx::Net::ED2K;
    if (b0 == PROT_KAD || b0 == PROT_KADPACK) net = mx::Net::Kad;
    else if (b0 != PROT_EDONKEY && b0 != PROT_EMULE) net = facadeActive(FacadeKad) ? mx::Net::Kad : mx::Net::ED2K;
    // [bw-2] UDP: server searches and source queries, Kad, reasks: never charged to the transfer limits
    m_ctx.limiter->accountExempt(mx::RateLimiter::Dir(dir), net, d.size(), 0);
}

bool Ed2kCore::sendClientUdp(quint32 ip, quint16 port, quint8 opcode, const QByteArray &payload, const QByteArray &uh)
{
    QByteArray d;
    d.reserve(payload.size() + 2);
    d.append(char(PROT_EMULE)).append(char(opcode)).append(payload);
    const quint32 pub = m_kad.ip ? m_kad.ip : publicIp();
    if (uh.size() == 16 && pub && m_cryptSupported) {
        const QByteArray o = kad::obfuscateEd2k(d, uh, pub);
        if (!o.isEmpty()) d = o;
    }
    return sendUdp(ip, port, d);
}

void Ed2kCore::handleClientUdp(quint32 ip, quint16 port, const QByteArray &plain, bool obfuscated)
{
    if (plain.size() < 2 || plain.size() > 4096) return;
    const quint8 op = quint8(plain[1]);
    const QByteArray payload = plain.mid(2);
    switch (op) {
    case OP_REASKFILEPING:
        if (m_uploads) m_uploads->handleUdpReask(ip, port, payload, obfuscated);
        return;
    case OP_REASKACK:
    case OP_FILENOTFOUND:
    case OP_QUEUEFULL:
        for (const auto &s : std::as_const(m_sources))
            if (s && s->awaitingUdpAnswer(ip, port)) { s->udpAnswer(op, payload); return; }
        return;
    case OP_DIRECTCALLBACKREQ: {
        // a downloader wants us (firewalled, UDP open) to connect to it
        if (!m_kad.running || !m_kad.tcpFirewalled) return;
        const qint64 now = nowMs();
        if (now - m_directCallbackLog.value(ip, -3600000) < 60000) return;   // ≤ 1 per minute and IP
        m_directCallbackLog.insert(ip, now);
        if (m_directCallbackLog.size() > 2000) m_directCallbackLog.clear();
        Reader r(payload);
        PeerDescriptor d;
        d.ip = ip;
        d.port = r.u16();
        d.userHash = r.hash16();
        d.cryptOptions = r.u8() & 0x07;
        if (!r.ok() || !d.port || !acceptableSourceIp(ip)) return;
        log(QStringLiteral("direct callback request from %1:%2").arg(ipToString(ip)).arg(d.port));
        connectPeer(d);
        return;
    }
    default:
        return;   // OP_REASKCALLBACKUDP (we never act as buddy for UDP re-asks), OP_PORTTEST, unknown
    }
}

quint32 Ed2kCore::helloClientId() const
{
    if (m_kad.connected && !m_kad.tcpFirewalled && m_kad.ip) return m_kad.ip;   // trust Kad above eD2k (as aMule)
    if (const quint32 id = clientId()) return id;
    if (m_kad.connected) return 1;                                             // firewalled Kad client
    return 0;
}

bool Ed2kCore::canAcceptIncoming() const
{
    if (!m_listening) return false;
    if (clientId() && !lowId()) return true;
    return m_kad.connected && !m_kad.tcpFirewalled;
}

bool Ed2kCore::sendDirectCallback(const PeerDescriptor &peer)
{
    if (!peer.directCallback || !peer.ip || !peer.kadPort || !canAcceptIncoming()) return false;
    Writer w(19);
    w.u16(m_tcpPort).hash16(m_userHash);
    w.u8(quint8((m_cryptSupported ? 1 : 0) | (m_cryptRequested ? 2 : 0) | (m_cryptRequired ? 4 : 0)));
    return sendClientUdp(peer.ip, peer.kadPort, OP_DIRECTCALLBACKREQ, w.data(), (peer.cryptOptions & 1) ? peer.userHash : QByteArray());
}

// ---------------------------------------------------------------- peers
bool Ed2kCore::isOwnAddress(quint32 ip, quint16 port) const
{
    if (port != m_tcpPort) return false;
    if (ip == publicIp() && ip) return true;
    return isLoopbackIp(ip) && !m_allowLan;
}

bool Ed2kCore::acceptableSourceIp(quint32 ip) const
{
    if (!ip) return false;
    if (m_allowLan) return (ip & 0xFF) != 0 && (ip & 0xFF) < 224;
    return isRoutableIp(ip);
}

PeerConnection *Ed2kCore::connectPeer(const PeerDescriptor &d, bool allowObf)
{
    const int maxConn = qBound(10, m_cfg.i(QStringLiteral("ed2k/maxConnections"), 100), 2000);
    if (m_peers.size() >= maxConn) return nullptr;
    auto *conn = new Ed2kConnection();
    auto *pc = new PeerConnection(this, conn, true, this);
    pc->setProperty("mxAllowObf", allowObf);
    pc->setProperty("mxIp", d.ip);
    pc->setProperty("mxPort", d.port);
    pc->setProperty("mxHash", d.userHash);
    pc->setProperty("mxCrypt", d.cryptOptions);
    adoptPeer(pc);
    m_pendingConnects.append(pc);
    QMetaObject::invokeMethod(this, [this] { startPendingConnects(); }, Qt::QueuedConnection);
    return pc;
}

void Ed2kCore::startPendingConnects()
{
    const int maxHalfOpen = qBound(1, m_cfg.i(QStringLiteral("ed2k/maxHalfOpen"), 8), 100);
    const int maxPer5s = qBound(1, m_cfg.i(QStringLiteral("ed2k/maxConnectsPer5s"), 20), 200);
    int halfOpen = 0;
    for (const auto &p : m_peers)
        if (p && p->outgoing() && p->started() && !p->handshakeDone() && p->isOpen()) ++halfOpen;
    const qint64 now = nowMs();
    if (now - m_lastHalfOpenWindowMs > 5000) { m_lastHalfOpenWindowMs = now; m_connectsInWindow = 0; }
    while (!m_pendingConnects.isEmpty() && halfOpen < maxHalfOpen && m_connectsInWindow < maxPer5s) {
        QPointer<PeerConnection> pc = m_pendingConnects.takeFirst();
        if (!pc) continue;
        PeerDescriptor d;
        d.ip = pc->property("mxIp").toUInt();
        d.port = quint16(pc->property("mxPort").toUInt());
        d.userHash = pc->property("mxHash").toByteArray();
        d.cryptOptions = quint8(pc->property("mxCrypt").toUInt());
        pc->startOutgoing(d, pc->property("mxAllowObf").toBool());
        ++halfOpen;
        ++m_connectsInWindow;
    }
}

void Ed2kCore::adoptPeer(PeerConnection *pc)
{
    m_peers.append(pc);
    connect(pc, &PeerConnection::handshakeComplete, this, &Ed2kCore::onPeerHandshake);
    connect(pc, &PeerConnection::closed, this, [this](PeerConnection *p, const QString &) {
        if (p->conn()) { m_closedIn += p->conn()->bytesIn(); m_closedOut += p->conn()->bytesOut(); }
        m_peers.removeAll(p);
        m_pendingConnects.removeAll(p);
        m_uploads->peerClosed(p);
        p->deleteLater();
        QMetaObject::invokeMethod(this, [this] { startPendingConnects(); }, Qt::QueuedConnection);
    });
}

void Ed2kCore::onNewConnection()
{
    while (m_listener->hasPendingConnections()) {
        QTcpSocket *s = m_listener->nextPendingConnection();
        const int maxConn = qBound(10, m_cfg.i(QStringLiteral("ed2k/maxConnections"), 100), 2000);
        if (!m_running || m_peers.size() >= maxConn + 20) { s->abort(); s->deleteLater(); continue; }
        auto *conn = new Ed2kConnection(s);
        conn->useIncomingDetection(m_userHash, m_cryptRequired);
        auto *pc = new PeerConnection(this, conn, false, this);
        adoptPeer(pc);
        pc->startIncoming();
    }
}

void Ed2kCore::onPeerHandshake(PeerConnection *pc)
{
    if (!pc->downloadSink()) {
        // a peer connected to us (slot offer, LowID callback answer) or we connected on a
        // callback request: give it to the source waiting for that peer
        for (const auto &s : std::as_const(m_sources)) {
            if (s && s->wantsConnection() && s->matchesIncoming(pc->info(), pc->ip())) {
                s->attach(pc);
                break;
            }
        }
    }
    m_uploads->peerConnected(pc);
    emit peerHandshake(pc);
}

void Ed2kCore::onCallbackRequested(quint32 ip, quint16 port, quint8 crypt, const QByteArray &uh)
{
    if (!acceptableSourceIp(ip)) return;
    PeerDescriptor d;
    d.ip = ip;
    d.port = port;
    d.cryptOptions = crypt;
    if (uh.size() == 16) d.userHash = uh;
    log(QStringLiteral("server callback request: connecting to %1:%2").arg(ipToString(ip)).arg(port));
    connectPeer(d);
}

void Ed2kCore::registerSource(Ed2kRangeSource *s) { m_sources.append(s); }
void Ed2kCore::unregisterSource(Ed2kRangeSource *s)
{
    for (int i = int(m_sources.size()) - 1; i >= 0; --i)
        if (!m_sources[i] || m_sources[i] == s) m_sources.removeAt(i);
}

// ---------------------------------------------------------------- re-ask registry
static QString askKey(const QString &peer, const QByteArray &file) { return peer + QLatin1Char('|') + toHex(file); }

qint64 Ed2kCore::msSinceAsk(const QString &peerKey, const QByteArray &fileHash) const
{
    auto it = m_askLog.constFind(askKey(peerKey, fileHash));
    if (it == m_askLog.cend()) return -1;
    return QDateTime::currentMSecsSinceEpoch() - it.value();
}
void Ed2kCore::noteAsk(const QString &peerKey, const QByteArray &fileHash)
{
    m_askLog.insert(askKey(peerKey, fileHash), QDateTime::currentMSecsSinceEpoch());
}
bool Ed2kCore::sourceExchangeAllowed(const QString &peerKey, const QByteArray &fileHash) const
{
    auto it = m_sxLog.constFind(askKey(peerKey, fileHash));
    return it == m_sxLog.cend() || QDateTime::currentMSecsSinceEpoch() - it.value() > kSxReaskMs;
}
void Ed2kCore::noteSourceExchange(const QString &peerKey, const QByteArray &fileHash)
{
    m_sxLog.insert(askKey(peerKey, fileHash), QDateTime::currentMSecsSinceEpoch());
}

void Ed2kCore::loadState()
{
    QFile f(QDir(m_dataDir).filePath(QStringLiteral("state.json")));
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonObject o = QJsonDocument::fromJson(f.read(4 * 1024 * 1024)).object();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QJsonObject asks = o.value(QStringLiteral("asks")).toObject();
    for (auto it = asks.begin(); it != asks.end(); ++it) {
        const qint64 t = qint64(it.value().toDouble());
        if (now - t < 2LL * 3600 * 1000) m_askLog.insert(it.key(), t);
    }
    const QJsonObject sx = o.value(QStringLiteral("sx")).toObject();
    for (auto it = sx.begin(); it != sx.end(); ++it) {
        const qint64 t = qint64(it.value().toDouble());
        if (now - t < kSxReaskMs) m_sxLog.insert(it.key(), t);
    }
}

void Ed2kCore::saveState()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QJsonObject asks, sx;
    for (auto it = m_askLog.cbegin(); it != m_askLog.cend(); ++it)
        if (now - it.value() < 2LL * 3600 * 1000) asks.insert(it.key(), double(it.value()));
    for (auto it = m_sxLog.cbegin(); it != m_sxLog.cend(); ++it)
        if (now - it.value() < kSxReaskMs) sx.insert(it.key(), double(it.value()));
    QJsonObject o;
    o.insert(QStringLiteral("asks"), asks);
    o.insert(QStringLiteral("sx"), sx);
    QDir().mkpath(m_dataDir);
    QSaveFile f(QDir(m_dataDir).filePath(QStringLiteral("state.json")));
    if (f.open(QIODevice::WriteOnly)) { f.write(QJsonDocument(o).toJson(QJsonDocument::Compact)); f.commit(); }
}

// ---------------------------------------------------------------- downloads
DownloadCtx *Ed2kCore::ensureDownload(const QString &id, const mx::FileHashes &h, qint64 size, const QString &name)
{
    auto it = m_downloads.find(id);
    if (it == m_downloads.end()) {
        DownloadCtx d;
        d.id = id;
        d.hashes = h;
        d.size = size;
        d.name = name;
        it = m_downloads.insert(id, d);
        if (h.ed2k.size() == 16 && size > 0 && size < PARTSIZE) {
            // one-part file: the file hash is the only part hash
            it->hashsetKnown = true;
            const QString did = id;
            const QByteArray fh = h.ed2k;
            QMetaObject::invokeMethod(this, [this, did, fh] {
                emit hashSetLearned(did, QStringLiteral("ed2k-parts"), PARTSIZE, {fh});
            }, Qt::QueuedConnection);
        }
    } else {
        it->hashes.mergeFrom(h);
        if (size > 0) it->size = size;
        if (!name.isEmpty()) it->name = name;
    }
    return &it.value();
}

DownloadCtx *Ed2kCore::download(const QString &id)
{
    auto it = m_downloads.find(id);
    return it == m_downloads.end() ? nullptr : &it.value();
}

QList<DownloadCtx *> Ed2kCore::downloadsByHash(const QByteArray &ed2k)
{
    QList<DownloadCtx *> out;
    for (auto it = m_downloads.begin(); it != m_downloads.end(); ++it)
        if (it->hashes.ed2k == ed2k) out.append(&it.value());
    return out;
}

bool Ed2kCore::hashsetKnown(const QByteArray &fileHash) const
{
    for (const DownloadCtx &d : m_downloads)
        if (d.hashes.ed2k == fileHash && d.hashsetKnown) return true;
    return false;
}

void Ed2kCore::findSources(const QString &id, const mx::FileHashes &h, qint64 size, const QString &name)
{
    if (h.ed2k.size() != 16 || size <= 0) return;
    DownloadCtx *d = ensureDownload(id, h, size, name);
    d->findingSources = true;
    d->nextServerAskMs = 0;
    QMetaObject::invokeMethod(this, [this] { tick(); }, Qt::QueuedConnection);
}

void Ed2kCore::stopFindSources(const QString &id)
{
    if (DownloadCtx *d = download(id)) d->findingSources = false;
}

void Ed2kCore::learnHashset(const QByteArray &fileHash, const QList<QByteArray> &parts)
{
    for (DownloadCtx *d : downloadsByHash(fileHash)) {
        if (d->partHashes.isEmpty()) d->partHashes = parts;
        if (d->hashsetKnown) continue;
        d->hashsetKnown = true;
        log(QStringLiteral("hashset for %1 verified (%2 part hashes)").arg(d->name.isEmpty() ? toHex(fileHash) : d->name).arg(parts.size()));
        emit hashSetLearned(d->id, QStringLiteral("ed2k-parts"), PARTSIZE, parts);
    }
}

void Ed2kCore::learnAich(const QByteArray &fileHash, const QByteArray &aich, quint32 fromIp)
{
    if (aich.size() != 20) return;
    for (DownloadCtx *d : downloadsByHash(fileHash)) {
        if (d->aichKnown) continue;
        QSet<quint32> &votes = d->aichVotes[aich];
        votes.insert(fromIp);
        // two independent peers must agree before we trust an AICH root
        if (votes.size() >= 2) {
            d->aichKnown = true;
            mx::FileHashes more;
            more.aich = aich;
            d->hashes.aich = aich;
            emit hashesLearned(d->id, more);
        }
    }
}

void Ed2kCore::addSources(const QByteArray &fileHash, const QList<PeerDescriptor> &list, const QString &origin, mx::Net net)
{
    const QList<DownloadCtx *> dls = downloadsByHash(fileHash);
    if (dls.isEmpty()) return;
    const bool weLow = lowId();
    for (DownloadCtx *d : dls) {
        QList<mx::SearchSource> out;
        for (PeerDescriptor p : list) {
            if (!p.isValid()) continue;
            if (p.needsKadCallback()) {
                if (!canAcceptIncoming() || p.userHash.size() != 16) continue;   // they must connect to us
            } else if (p.isLowId()) {
                if (weLow || !p.serverIp) continue;   // LowID↔LowID cannot connect
            } else {
                if (!acceptableSourceIp(p.ip) || isOwnAddress(p.ip, p.port)) continue;
            }
            p.fileHash = fileHash;
            const QString key = p.key();
            if (d->knownSources.contains(key)) continue;
            if (d->knownSources.size() >= qBound(10, m_cfg.i(QStringLiteral("ed2k/maxSourcesPerFile"), 400), 5000)) break;
            d->knownSources.insert(key);
            mx::SearchSource s;
            s.net = net;
            s.uri = formatPeerUri(p);
            s.connection = net == mx::Net::Kad ? QStringLiteral("Kad") : QStringLiteral("eD2k");
            s.user.net = net;
            s.user.id = p.userHash.size() == 16 ? toHex(p.userHash) : key;
            s.user.name = key;
            if (!p.isLowId() && p.ip) s.user.address = QStringLiteral("%1:%2").arg(ipToString(p.ip)).arg(p.port);
            s.extra.insert(QStringLiteral("origin"), origin);
            if (p.isLowId() || p.needsKadCallback()) s.extra.insert(QStringLiteral("lowId"), true);
            out.append(s);
        }
        if (!out.isEmpty()) {
            log(QStringLiteral("%1 new source(s) via %2 for %3").arg(out.size()).arg(origin, d->name.isEmpty() ? toHex(fileHash) : d->name));
            emit sourcesFound(d->id, out);
        }
    }
}

// ---------------------------------------------------------------- shared files
void Ed2kCore::setSharedFiles(const QList<mx::SharedFile> &files)
{
    m_shared.clear();
    for (const mx::SharedFile &f : files)
        if (f.hashes.ed2k.size() == 16 && f.size > 0) m_shared.insert(f.hashes.ed2k, f);
    if (m_servers) m_servers->publishFiles(m_shared.values());
}

bool Ed2kCore::sharedFile(const QByteArray &ed2k, mx::SharedFile *out) const
{
    auto it = m_shared.constFind(ed2k);
    if (it != m_shared.cend()) { if (out) *out = it.value(); return true; }
    if (m_ctx.library) {
        mx::FileHashes h;
        h.ed2k = ed2k;
        mx::SharedFile f;
        if (m_ctx.library->findByHash(h, &f) && f.hashes.ed2k == ed2k) { if (out) *out = f; return true; }
    }
    return false;
}

bool Ed2kCore::isPartialFile(const QByteArray &ed2k) const
{
    if (m_shared.contains(ed2k)) return false;
    for (const DownloadCtx &d : m_downloads) if (d.hashes.ed2k == ed2k) return true;
    // [up-1] any running download the engine serves (a torrent with a known ed2k hash, too)
    if (m_ctx.library) {
        mx::FileHashes h;
        h.ed2k = ed2k;
        if (m_ctx.library->partialRanges(h, nullptr)) return true;
    }
    return false;
}

QList<bool> Ed2kCore::availableParts(const QByteArray &ed2k, qint64 size)
{
    const int n = dataPartCount(size);
    if (!isPartialFile(ed2k) || !m_ctx.library || n <= 0 || n > 30000) return {};
    PartCache &c = m_partCache[ed2k];
    if (c.atMs >= 0 && nowMs() - c.atMs < 30000 && c.parts.size() == n) return c.parts;
    mx::FileHashes h;
    h.ed2k = ed2k;
    c.parts.clear();
    for (int i = 0; i < n; ++i) {
        // the library only returns verified data; parts are verified as a whole
        const qint64 a = qint64(i) * PARTSIZE, b = qMin(size, a + PARTSIZE) - 1;
        c.parts.append(!m_ctx.library->read(h, a, 1).isEmpty() && !m_ctx.library->read(h, b, 1).isEmpty());
    }
    c.atMs = nowMs();
    if (m_partCache.size() > 64) m_partCache.clear();
    return c.parts;
}

QByteArray Ed2kCore::partStatus(const QByteArray &ed2k, qint64 size)
{
    const QList<bool> parts = availableParts(ed2k, size);
    Writer w;
    if (!parts.contains(true)) { w.u16(0); return w.take(); }
    const int count = ed2kPartCount(size);
    w.u16(quint16(count));
    QByteArray bits((count + 7) / 8, '\0');
    for (int i = 0; i < parts.size() && i < count; ++i) if (parts[i]) bits[i / 8] = char(quint8(bits[i / 8]) | (1u << (i % 8)));
    w.bytes(bits);
    return w.take();
}

QList<QByteArray> Ed2kCore::downloadPartHashes(const QByteArray &ed2k) const
{
    for (const DownloadCtx &d : m_downloads) if (d.hashes.ed2k == ed2k && !d.partHashes.isEmpty()) return d.partHashes;
    return {};
}

// ---------------------------------------------------------------- periodic
void Ed2kCore::tick()
{
    if (!m_running) return;
    startPendingConnects();
    const qint64 now = nowMs();
    for (DownloadCtx &d : m_downloads) {
        if (!d.findingSources) continue;
        if (now >= d.nextServerAskMs && m_servers->current()) {
            m_servers->requestSources(d.hashes.ed2k, d.size, d.nextServerAskMs == 0);
            d.nextServerAskMs = now + SERVERREASKTIME_MS;
        }
    }
    // transfer rates over all peer connections
    qint64 in = m_closedIn, out = m_closedOut;
    for (const auto &p : m_peers) if (p && p->conn()) { in += p->conn()->bytesIn(); out += p->conn()->bytesOut(); }
    m_rateDown = int(qMax<qint64>(0, in - m_prevIn));
    m_rateUp = int(qMax<qint64>(0, out - m_prevOut));
    m_prevIn = in;
    m_prevOut = out;
    ++m_tickCount;
    if (m_tickCount % 5 == 0) emit statusChanged();
    if (m_tickCount % 300 == 0) saveState();
}


// [enc-1] how many open (handshaked) peer connections are obfuscated
void Ed2kCore::obfuscationCounts(int *obfuscated, int *all) const
{
    int o = 0, a = 0;
    for (const QPointer<PeerConnection> &p : m_peers) {
        if (!p || !p->isOpen() || !p->handshakeDone()) continue;
        ++a;
        if (p->obfuscated()) ++o;
    }
    if (obfuscated) *obfuscated = o;
    if (all) *all = a;
}

} // namespace mx::ed2k
