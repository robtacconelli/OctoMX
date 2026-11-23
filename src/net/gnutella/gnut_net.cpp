// SPDX-License-Identifier: GPL-3.0-or-later
// Gnutella plugin: ultrapeer connections, bootstrap (GWebCache / UDP host caches), message
// dispatch, QRP upload, vendor messages.
#include "gnut_plugin_impl.h"
#include "gnut_huge.h"

#include "mx/settings_keys.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QHostInfo>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QUdpSocket>

namespace mx::gnut {

static const QByteArray kBEAR("BEAR", 4), kLIME("LIME", 4), kGTKG("GTKG", 4), kNull(4, '\0');

QList<UltrapeerConnection *> GnutellaPlugin::connectedUps() const
{
    QList<UltrapeerConnection *> l;
    for (UltrapeerConnection *c : m_conns)
        if (c->state() == UltrapeerConnection::Connected) l << c;
    return l;
}

void GnutellaPlugin::maintain()
{
    if (!m_started) return;
    const qint64 now = nowSecs();
    int connected = 0, connecting = 0;
    QSet<quint64> busy;
    for (UltrapeerConnection *c : m_conns) {
        busy.insert(c->address().key());
        if (c->state() == UltrapeerConnection::Connected) ++connected;
        else ++connecting;
    }
    const int want = maxUltrapeers();
    // too many (e.g. the user lowered the setting): close the youngest non-manual ones
    if (connected > want) {
        QList<UltrapeerConnection *> ups = connectedUps();
        std::sort(ups.begin(), ups.end(), [](UltrapeerConnection *a, UltrapeerConnection *b) { return a->connectedMs() < b->connectedMs(); });
        for (UltrapeerConnection *c : ups) {
            if (connected <= want) break;
            if (m_manualConnect.contains(c->address().key())) continue;
            c->close(QStringLiteral("enough ultrapeers"), 200);
            --connected;
        }
    }
    // parallel attempts: up to 4 more than missing while few are connected
    const int missing = want - connected;
    if (missing > 0) {
        const int parallel = qMin(8, missing + (connected == 0 ? 3 : 1));
        if (connecting < parallel) {
            const QList<HostPort> cands = m_hosts.pick(parallel - connecting, busy, now);
            for (const HostPort &h : cands) startConnection(h);
            if (cands.size() < parallel - connecting || m_hosts.usableCount(now) < 30) bootstrap();
        }
    }
    // expire push waiters / queue entries / recent pushes
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    for (auto it = m_pushWaits.begin(); it != m_pushWaits.end();) {
        if ((!it->src && !it->browse) || it->deadline < nowMs) it = m_pushWaits.erase(it);
        else ++it;
    }
    for (int i = int(m_uploadQueue.size()) - 1; i >= 0; --i)
        if (now - m_uploadQueue[i].lastPoll > 180) m_uploadQueue.removeAt(i);
    for (auto it = m_recentPushes.begin(); it != m_recentPushes.end();) {
        if (now - it.value() > 30) it = m_recentPushes.erase(it);
        else ++it;
    }
    for (auto it = m_udpCbGuids.begin(); it != m_udpCbGuids.end();) {
        if (now - it.value() > 120) it = m_udpCbGuids.erase(it);
        else ++it;
    }
    emitStatus();
}

void GnutellaPlugin::startConnection(const HostPort &h, bool manual)
{
    if (!h.isValid()) return;
    if (h.ip == m_externalIp && h.port == m_port) { m_hosts.markUseless(h); return; }
    HandshakeConfig cfg;
    cfg.userAgent = m_userAgent;
    cfg.listen = selfAddress();
    cfg.browseHost = browseAllowed(HostPort());
    cfg.handshakeTimeoutMs = qMin(connectTimeoutMs(), 10000) + 5000;   // dead hosts are common: fail fast
    cfg.meter = m_ctx.limiter;   // [glue-3] ultrapeer traffic in the bandwidth graph
    // [enc-1] TLS to hosts known to take it (prefer) or to every host (require)
    if (tlsMode() != TlsMode::Off && ensureTls()) {
        cfg.tls = m_tls;
        cfg.tlsPolicy = tlsPolicy(m_hosts.supportsTls(h));
        cfg.advertiseTls = true;
    } else if (tlsMode() == TlsMode::Require) {
        m_hosts.markFailure(h, 600);
        return;                                   // TLS unavailable: no plain links in require mode
    }
    auto *c = new UltrapeerConnection(h, cfg, this);
    m_conns << c;
    if (manual) m_manualConnect.insert(h.key());
    m_hosts.markAttempt(h);
    connect(c, &UltrapeerConnection::handshakeDone, this, [this, c] { onHandshakeDone(c); });
    connect(c, &UltrapeerConnection::handshakeFailed, this,
            [this, c](int code, const QString &reason, const QList<HostPort> &tryHosts, int retry) {
                int added = 0;
                for (const HostPort &t : tryHosts) if (m_hosts.add(t, QStringLiteral("x-try"))) ++added;
                if (code == -1) return;                           // harvest only (successful handshake)
                ++m_stats.handshakesFailed;
                // [enc-1] TLS refused: plain next time (prefer); a plain attempt the host dropped
                // without a word may come from a host that wants TLS (gtk-gnutella retries with TLS too)
                if (code == 0 && reason.startsWith(QLatin1String("TLS handshake failed"))) m_hosts.setTls(c->address(), false);
                else if (code == 0 && !c->tlsTried() && tlsMode() == TlsMode::Prefer && m_tls &&
                         (reason == QLatin1String("closed during handshake") || reason.contains(QLatin1String("reset"), Qt::CaseInsensitive)))
                    m_hosts.setTls(c->address(), true);
                if (code == 0 && reason.contains(QLatin1String("not an ultrapeer"))) m_hosts.markUseless(c->address());
                else if (code == 0 && reason.contains(QLatin1String("not a Gnutella"))) m_hosts.markUseless(c->address());
                else m_hosts.markFailure(c->address(), code == 503 ? qMax(retry, 600) : retry);
                qCDebug(lcGnut).noquote() << "handshake with" << c->id() << "failed:" << code << reason << "(+" << added << "hosts)";
            });
    connect(c, &UltrapeerConnection::messageReceived, this, [this, c](const Message &m) { onMessage(c, m); });
    connect(c, &UltrapeerConnection::closed, this, [this, c](const QString &reason) { onConnectionClosed(c, reason); });
    c->start();
    emit serversChanged();
}

void GnutellaPlugin::learnExternalIp(UltrapeerConnection *c)
{
    bool ok = false;
    const quint32 ip = ipFromString(c->remoteIpSeen(), &ok);
    if (!ok || !ip) return;
    // accept a private address only when the ultrapeer itself is on a private network (tests/LAN)
    if (!isRoutableIp(ip) && isRoutableIp(c->address().ip)) return;
    if (m_externalIp != ip) {
        m_externalIp = ip;
        log(QStringLiteral("external address %1 (reported by %2)").arg(ipToString(ip), c->id()));
    }
}

void GnutellaPlugin::sendVendor(UltrapeerConnection *c, const QByteArray &vendor, quint16 id, quint16 version,
                                const QByteArray &body, const QByteArray &guid)
{
    Message m = Message::make(MsgVendor, buildVendorPayload(vendor, id, version, body), 1,
                              guid.size() == 16 ? guid : QByteArray(16, '\0'));
    c->send(m);
}

void GnutellaPlugin::onHandshakeDone(UltrapeerConnection *c)
{
    ++m_stats.handshakesOk;
    m_hosts.markSuccess(c->address(), c->vendor());
    // [enc-1] what we learned about TLS
    if (c->encrypted() || c->remoteOffersTls()) m_hosts.setTls(c->address(), true);
    else if (c->tlsFellBack()) m_hosts.setTls(c->address(), false);
    learnExternalIp(c);
    QString tlsNote;
    if (c->encrypted()) tlsNote = QStringLiteral(", TLS (%1)").arg(c->tlsInfo());
    else if (c->tlsFellBack()) tlsNote = QStringLiteral(", plain (TLS failed: %1)").arg(c->tlsInfo().left(80));
    else tlsNote = c->remoteOffersTls() ? QStringLiteral(", plain (offers TLS)") : QStringLiteral(", plain");
    log(QStringLiteral("connected to ultrapeer %1 (%2) in %3 ms%4%5").arg(c->id(), c->vendor()).arg(c->handshakeMs())
            .arg(c->compressedIn() ? QStringLiteral(", deflate") : QString(), tlsNote));
    {
        int tls = 0;
        const QList<UltrapeerConnection *> ups = connectedUps();
        for (UltrapeerConnection *u : ups) if (u->encrypted()) ++tls;
        log(QStringLiteral("TLS on %1 of %2 ultrapeer links (encryption %3)").arg(tls).arg(ups.size()).arg(tlsModeName(tlsMode())));
    }
    // vendor messages we understand
    const QList<VendorKey> ours = {
        {kBEAR, 4, 1}, {kBEAR, 7, 1}, {kBEAR, 11, 1}, {kBEAR, 12, 1}, {kLIME, 21, 1}, {kLIME, 22, 2}, {kGTKG, 7, 2},
    };
    sendVendor(c, kNull, 0, 0, buildMessagesSupportedBody(ours));
    sendQrp(c, true);
    // learn our reachability: ask (at most two) ultrapeers to connect back to us
    if (!m_reachable && !c->connectBackSent) {
        int sent = 0;
        for (UltrapeerConnection *o : connectedUps()) if (o->connectBackSent) ++sent;
        if (sent < 2) askConnectBack(c);
    }
    if (firewalled() && !c->pushProxyRequested) {
        sendVendor(c, kLIME, 21, 1, QByteArray(), m_guid);
        c->pushProxyRequested = true;
    }
    c->send(Message::make(MsgPing, QByteArray(), 1));
    // running searches the new ultrapeer has not seen yet (first minute only)
    for (auto it = m_searches.begin(); it != m_searches.end(); ++it)
        if (!it->done && it->started.elapsed() < 60000 && !it->sentTo.contains(c->address().key())) sendSearch(*it, c);
    emitStatus();
    emit serversChanged();
}

void GnutellaPlugin::onConnectionClosed(UltrapeerConnection *c, const QString &reason)
{
    const bool wasConnected = c->connectedMs() > 0;
    m_conns.removeAll(c);
    if (wasConnected) {
        log(QStringLiteral("ultrapeer %1 closed: %2 (after %3 s)").arg(c->id(), reason).arg(c->connectedMs() / 1000));
        // a connection that dropped quickly counts as a soft failure
        if (c->connectedMs() < 60000 && reason != QLatin1String("enough ultrapeers") &&
            reason != QLatin1String("encryption setting changed")) m_hosts.markFailure(c->address(), 300);
    } else if (!reason.startsWith(QLatin1String("handshake rejected"))) {
        m_hosts.markFailure(c->address(), 0);
    }
    m_manualConnect.remove(c->address().key());
    c->deleteLater();
    emitStatus();
    emit serversChanged();
    if (m_started) QTimer::singleShot(500, this, &GnutellaPlugin::maintain);
}

void GnutellaPlugin::sendQrp(UltrapeerConnection *c, bool force)
{
    if (c->state() != UltrapeerConnection::Connected) return;
    if (!force && c->qrpSentDigest == m_qrpDigest) return;
    const QList<QByteArray> msgs = m_qrp.encode(7, 4, true, 4096);
    for (const QByteArray &p : msgs) c->send(Message::make(MsgQrp, p, 1));
    c->qrpSentDigest = m_qrpDigest;
    c->qrpAcked = true;
    ++m_stats.qrpSent;
    log(QStringLiteral("QRP table sent to %1: %2 slots, %3 filled, %4 messages").arg(c->id()).arg(m_qrp.nslots()).arg(m_qrp.filled()).arg(msgs.size()));
}

void GnutellaPlugin::onMessage(UltrapeerConnection *c, const Message &m)
{
    switch (m.type) {
    case MsgPing: {
        // keep-alive / pong request from our ultrapeer: answer with our own pong
        if (m.hops > 1) return;
        HostPort self = selfAddress();
        qint64 kb = 0;
        for (const mx::SharedFile &f : m_shared) kb += f.size / 1024;
        GgepBlock pg;
        if (tlsAdvertised()) pg.add("TLS", QByteArray());   // [enc-1]
        Message p = Message::make(MsgPong, buildPongPayload(self, quint32(m_shared.size()), quint32(qMin<qint64>(kb, 0xffffffffLL)), pg),
                                  quint8(qMax(1, int(m.hops) + 1)), m.guid);
        c->send(p, true);
        return;
    }
    case MsgPong: {
        Pong p;
        if (!parsePong(m.payload, &p)) return;
        const HostPort h{p.ip, p.port};
        if (h.isValid() && isRoutableIp(h.ip) && m.hops > 0) {
            m_hosts.add(h, QStringLiteral("pong"));
            if (p.tls) m_hosts.setTls(h, true);   // [enc-1]
        }
        for (const HostPort &x : p.ipp) if (isRoutableIp(x.ip)) m_hosts.add(x, QStringLiteral("pong"));
        for (const HostPort &x : p.ippTls) m_hosts.setTls(x, true);
        for (const QString &u : p.hostCaches) m_hosts.addGwc(u);
        return;
    }
    case MsgBye: {
        quint16 code = 0;
        QString reason;
        parseBye(m.payload, &code, &reason);
        log(QStringLiteral("BYE from %1: %2 %3").arg(c->id()).arg(code).arg(reason.left(100)));
        c->close(QStringLiteral("BYE %1 %2").arg(code).arg(reason.left(60)));
        return;
    }
    case MsgVendor:
        onVendor(c, m);
        return;
    case MsgPush:
        onPush(c, m);
        return;
    case MsgQuery:
        onQuery(c, m);
        return;
    case MsgQueryHit:
        onQueryHit(c, m);
        return;
    default:
        ++m_stats.droppedMessages;
        return;
    }
}

void GnutellaPlugin::onVendor(UltrapeerConnection *c, const Message &m)
{
    VendorMsg v;
    if (!parseVendor(m.payload, &v)) return;
    if (v.vendor == kNull && v.id == 0) {
        c->setSupportedVendorMessages(parseMessagesSupported(v.body));
        return;
    }
    if (v.vendor == kBEAR && v.id == 4) {              // Hops Flow
        if (!v.body.isEmpty()) c->setHopsFlow(quint8(v.body[0]));
        return;
    }
    if (v.vendor == kBEAR && v.id == 11) {             // Query Status Request (leaf-guided dynamic query)
        answerQueryStatus(c, m.guid);
        return;
    }
    if (v.vendor == kLIME && v.id == 22) {             // Push Proxy Acknowledgement
        Reader r(v.body);
        HostPort pp;
        if (v.version >= 2) { pp.ip = r.ip(); pp.port = r.u16(); }
        else { pp.ip = c->address().ip; pp.port = r.u16(); }
        if (r.ok() && pp.isValid()) {
            c->pushProxyAddr = pp;
            ++m_stats.pushProxyAcks;
            log(QStringLiteral("push proxy acknowledged by %1 (%2)").arg(c->id(), pp.toString()));
            m_browseCache.clear();
        }
        return;
    }
    // everything else (time sync, OOB acks, HEAD pings, crawler pings...) is ignored
}

void GnutellaPlugin::bootstrap()
{
    if (!m_started) return;
    // "gnutella/autoBootstrap" (wanted key, default true): false = only static / remembered hosts
    if (!setting(QStringLiteral("gnutella/autoBootstrap"), true).toBool()) return;
    const qint64 now = nowSecs();
    // UDP host caches: one ping every 5 s
    if (now - m_lastUhc >= 5 && m_udp) {
        if (CacheEntry *u = m_hosts.nextUhc(now, 300)) {
            m_lastUhc = now;
            u->lastAttempt = now;
            pingUhc(u->url);
        }
    }
    // GWebCaches: at most one request in flight, one every 15 s
    const bool needGwc = m_hosts.usableCount(now) < 50 || (connectedCount() == 0 && m_uptime.elapsed() > 20000);
    if (needGwc && m_gwcInFlight == 0 && now - m_lastGwc >= 15) {
        if (CacheEntry *g = m_hosts.nextGwc(now, 1800)) {
            m_lastGwc = now;
            g->lastAttempt = now;
            queryGwc(g->url);
        }
    }
}

void GnutellaPlugin::queryGwc(const QString &url)
{
    QUrl u(url);
    if (!u.isValid() || (u.scheme() != QLatin1String("http") && u.scheme() != QLatin1String("https"))) return;
    if (!url.endsWith(QLatin1String("/list4"))) {
        QUrlQuery q(u);
        q.addQueryItem(QStringLiteral("get"), QStringLiteral("1"));
        q.addQueryItem(QStringLiteral("hostfile"), QStringLiteral("1"));
        q.addQueryItem(QStringLiteral("net"), QStringLiteral("gnutella"));
        q.addQueryItem(QStringLiteral("client"), QString::fromLatin1(VendorCode));
        q.addQueryItem(QStringLiteral("version"), QStringLiteral(MX_VERSION_STRING));
        u.setQuery(q);
    }
    QNetworkRequest req(u);
    req.setHeader(QNetworkRequest::UserAgentHeader, m_userAgent);
    req.setTransferTimeout(20000);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *r = m_nam->get(req);
    ++m_gwcInFlight;
    connect(r, &QNetworkReply::readyRead, r, [r] { if (r->bytesAvailable() > 256 * 1024) r->abort(); });
    connect(r, &QNetworkReply::finished, this, [this, r, url] { --m_gwcInFlight; onGwcReply(r, url); r->deleteLater(); });
}

void GnutellaPlugin::onGwcReply(QNetworkReply *r, const QString &url)
{
    CacheEntry *entry = nullptr;
    for (CacheEntry &c : m_hosts.gwcs()) if (c.url == url) entry = &c;
    const QByteArray body = r->read(256 * 1024);
    int hosts = 0, caches = 0;
    if (r->error() == QNetworkReply::NoError) {
        const QList<QByteArray> lines = body.split('\n');
        for (QByteArray l : lines.mid(0, 1000)) {
            l = l.trimmed();
            if (l.isEmpty()) continue;
            QString s = QString::fromLatin1(l);
            if (s.startsWith(QLatin1String("H|"), Qt::CaseInsensitive)) {
                const HostPort h = HostPort::parse(s.section(QLatin1Char('|'), 1, 1), 0);
                if (h.isValid() && isRoutableIp(h.ip) && m_hosts.add(h, QStringLiteral("gwc"))) ++hosts;
            } else if (s.startsWith(QLatin1String("U|"), Qt::CaseInsensitive)) {
                const QString cu = s.section(QLatin1Char('|'), 1, 1);
                if (cu.startsWith(QLatin1String("http://")) && cu.size() < 200) { m_hosts.addGwc(cu); ++caches; }
            } else if (s.startsWith(QLatin1String("ERROR")) || s.startsWith(QLatin1String("i|"))) {
                continue;
            } else {
                // hostfile / GHC style: "ip:port" or "ip:port,vendor"
                const HostPort h = HostPort::parse(s.section(QLatin1Char(','), 0, 0).section(QLatin1Char(' '), 0, 0), 0);
                if (h.isValid() && isRoutableIp(h.ip) && m_hosts.add(h, QStringLiteral("gwc"))) ++hosts;
            }
        }
    }
    const bool ok = r->error() == QNetworkReply::NoError && hosts + caches > 0;
    if (entry) {
        if (ok) { entry->successes++; entry->failures = 0; entry->lastSuccess = nowSecs(); }
        else entry->failures++;
    }
    m_hosts.touch();
    if (ok) ++m_stats.gwcOk; else ++m_stats.gwcFail;
    log(QStringLiteral("GWebCache %1: %2 (%3 new hosts, %4 caches)").arg(url, ok ? QStringLiteral("ok") : r->errorString().left(60))
            .arg(hosts).arg(caches));
    if (hosts > 0) QTimer::singleShot(0, this, &GnutellaPlugin::maintain);
}

void GnutellaPlugin::pingUhc(const QString &hostPort)
{
    const QString host = hostPort.section(QLatin1Char(':'), 0, 0);
    const quint16 port = quint16(hostPort.section(QLatin1Char(':'), 1, 1).toUInt());
    if (host.isEmpty() || !port) return;
    QPointer<GnutellaPlugin> self(this);
    QHostInfo::lookupHost(host, this, [self, hostPort, port](const QHostInfo &info) {
        if (!self || !self->m_udp) return;
        for (const QHostAddress &a : info.addresses()) {
            if (a.protocol() != QAbstractSocket::IPv4Protocol) continue;
            const quint32 ip = a.toIPv4Address();
            if (!isRoutableIp(ip)) continue;                       // e.g. 0.0.0.0 for a parked name
            Message m = Message::make(MsgPing, buildUhcPingPayload(), 1);
            self->m_uhcPings.insert(m.guid, hostPort);
            if (self->m_uhcPings.size() > 64) self->m_uhcPings.erase(self->m_uhcPings.begin());
            const QByteArray dg = m.encode();
            if (self->m_udp->writeDatagram(dg, QHostAddress(ip), port) == dg.size() && self->m_ctx.limiter)   // [glue-3]
                self->m_ctx.limiter->accountTraffic(mx::RateLimiter::Up, mx::Net::Gnutella, dg.size(), 0);
            qCDebug(lcGnut).noquote() << "UHC ping to" << hostPort << a.toString();
            return;
        }
        for (CacheEntry &c : self->m_hosts.uhcs()) if (c.url == hostPort) c.failures++;
    });
}

// TCP (BEAR 7) and UDP (GTKG 7 v2) connect-back requests to one ultrapeer
void GnutellaPlugin::askConnectBack(UltrapeerConnection *c)
{
    QByteArray body;
    putU16(body, m_port);
    sendVendor(c, kBEAR, 7, 1, body);
    QByteArray ub;
    putU16(ub, m_port);
    const QByteArray g = randomGuid(true);
    m_udpCbGuids.insert(g, nowSecs());
    sendVendor(c, kGTKG, 7, 2, ub, g);
    c->connectBackSent = true;
    ++m_stats.tcpConnectBacksRequested;
    if (!m_cbAskedAt.isValid()) m_cbAskedAt.start();   // [diag-1]
}

// [diag-1] Connection Test
void GnutellaPlugin::recheckConnectivity()
{
    if (!m_started) return;
    int n = 0;
    m_cbAskedAt.invalidate();
    for (UltrapeerConnection *c : connectedUps()) {
        if (n++ >= 2) break;
        askConnectBack(c);
    }
}

mx::PluginDiagnostics GnutellaPlugin::diagnostics() const
{
    mx::PluginDiagnostics d;
    d.valid = true;
    if (!m_started) return d;
    mx::PortBinding tcp;
    tcp.proto = QStringLiteral("TCP");
    tcp.port = m_port ? m_port : setting(QString(keys::gnutella::Port), 6346).toInt();
    tcp.ok = m_port != 0;
    tcp.error = m_port ? QString() : (m_server ? m_server->errorString() : QStringLiteral("not listening"));
    tcp.purpose = QStringLiteral("ultrapeers, uploads, push");
    d.bindings << tcp;
    mx::PortBinding udp;
    udp.proto = QStringLiteral("UDP");
    udp.port = tcp.port;
    udp.ok = m_udp && m_udp->state() == QAbstractSocket::BoundState;
    udp.error = udp.ok ? QString() : (m_udp ? m_udp->errorString() : QStringLiteral("not bound"));
    udp.purpose = QStringLiteral("host caches, UDP connect-back");
    d.bindings << udp;
    const bool asked = m_cbAskedAt.isValid();
    const qint64 waited = asked ? m_cbAskedAt.elapsed() : 0;
    if (m_reachable) {
        d.tcpIn = mx::Reach::Yes;
        d.tcpEvidence = QStringLiteral("incoming connection (%1)").arg(m_reachableHow);
    } else if (asked && waited > 90000) {
        d.tcpIn = mx::Reach::No;
        d.tcpEvidence = QStringLiteral("asked %1 for a TCP connect-back, none arrived").arg(countNoun(m_stats.tcpConnectBacksRequested, QStringLiteral("ultrapeer")));
    } else if (asked) {
        d.tcpEvidence = QStringLiteral("waiting for the connect-back");
    }
    if (m_udpReachable) {
        d.udpIn = mx::Reach::Yes;
        d.udpEvidence = QStringLiteral("UDP connect-back received");
    } else if (asked && waited > 90000) {
        d.udpIn = mx::Reach::No;
        d.udpEvidence = QStringLiteral("no UDP connect-back arrived");
    }
    if (m_externalIp) {
        d.externalAddress = ipToString(m_externalIp);
        d.externalSeenBy = QStringLiteral("Gnutella ultrapeers");
    }
    d.incomingConnections = m_stats.incomingHandshakes + m_stats.incomingHttp + m_stats.connectBacks;
    {   // [enc-1] encryption row
        const TlsMode mode = tlsMode();
        const TlsStats ts = tlsStats();
        const QList<UltrapeerConnection *> ups = connectedUps();
        const int tls = encryptedLinks();
        mx::DiagItem it;
        it.id = QStringLiteral("gnutella.encryption");
        it.check = QStringLiteral("Encryption (TLS)");
        if (mode == TlsMode::Off) {
            it.level = mx::DiagLevel::Off;
            it.result = QStringLiteral("Off");
            it.details = QStringLiteral("plain connections (Settings, Gnutella, Encryption)");
            it.explain = QStringLiteral("Gnutella connections are not encrypted. Choose Prefer encryption to use TLS where the other side can.");
        } else if (!m_tls) {
            it.level = mx::DiagLevel::Broken;
            it.result = QStringLiteral("Not available");
            it.details = m_tlsError;
            it.explain = QStringLiteral("TLS could not be set up, so Gnutella connections are plain%1.")
                             .arg(mode == TlsMode::Require ? QStringLiteral(" (and, with Require encryption, refused)") : QString());
        } else {
            it.result = ups.isEmpty() ? QStringLiteral("No ultrapeer links yet")
                                      : QStringLiteral("TLS on %1 of %2").arg(tls).arg(countNoun(int(ups.size()), QStringLiteral("ultrapeer link")));
            QStringList parts;
            if (ts.outAttempts) parts << QStringLiteral("outgoing TLS %1 of %2 worked").arg(ts.outOk).arg(ts.outAttempts);
            if (ts.outFallback) parts << QStringLiteral("%1 fell back to plain").arg(ts.outFallback);
            if (ts.outFailed) parts << QStringLiteral("%1 refused (not encrypted)").arg(ts.outFailed);
            if (ts.inOk) parts << QStringLiteral("%1 incoming TLS").arg(ts.inOk);
            if (ts.inRefused) parts << QStringLiteral("%1 incoming plain refused").arg(ts.inRefused);
            parts << QStringLiteral("%1 known hosts offer TLS").arg(m_hosts.tlsCount());
            it.details = parts.join(QStringLiteral(", "));
            // yellow only when TLS is required and nothing works; with "prefer" plain links are
            // the expected result where the other side has no TLS
            if (tls > 0 || ts.outOk > 0 || ts.inOk > 0) it.level = mx::DiagLevel::Ok;
            else if (mode == TlsMode::Require && (ts.outAttempts > 0 || m_uptime.elapsed() > 120000)) it.level = mx::DiagLevel::Limited;
            else it.level = ups.isEmpty() ? mx::DiagLevel::Unknown : mx::DiagLevel::Ok;
            it.explain = QStringLiteral("TLS is %1. It hides the content of Gnutella connections from simple filtering by a provider. "
                                        "Peers do not check certificates (nobody on Gnutella has a verifiable identity), it does not "
                                        "hide your IP address and it is not anonymity. UDP (host caches, connect-back) stays plain.%2")
                             .arg(tlsModeName(mode),
                                  mode == TlsMode::Require ? QStringLiteral(" Require encryption finds fewer hosts and sources: most Gnutella peers do not offer TLS.")
                                                           : QString());
        }
        d.items << it;
    }
    for (const QString &u : setting(QString(keys::gnutella::GWebCaches), QStringList()).toStringList()) {
        const QString h = QUrl(u).host();
        if (!h.isEmpty() && !d.hosts.contains(h)) d.hosts << h;
    }
    return d;
}

void GnutellaPlugin::onUdpReadyRead()
{
    while (m_udp && m_udp->hasPendingDatagrams()) {
        QHostAddress from;
        quint16 fromPort = 0;
        const qint64 sz = m_udp->pendingDatagramSize();
        if (sz <= 0 || sz > 65536) { m_udp->readDatagram(nullptr, 0); continue; }
        QByteArray d(int(sz), Qt::Uninitialized);
        m_udp->readDatagram(d.data(), sz, &from, &fromPort);
        if (mx::consumeProbeDatagram(d)) continue;   // [diag-1] overlay port probe
        if (m_ctx.limiter) m_ctx.limiter->accountTraffic(mx::RateLimiter::Down, mx::Net::Gnutella, d.size(), 0);   // [glue-3]
        Framer f;
        f.feed(d);
        Message m;
        if (!f.next(&m)) continue;
        if (m.type == MsgPong) {
            const QString uhc = m_uhcPings.take(m.guid);
            Pong p;
            if (!parsePong(m.payload, &p)) continue;
            int added = 0;
            for (const HostPort &h : p.ipp) if (isRoutableIp(h.ip) && m_hosts.add(h, QStringLiteral("uhc"))) ++added;
            for (const HostPort &h : p.ippTls) m_hosts.setTls(h, true);   // [enc-1] IPP_TLS
            const HostPort ph{p.ip, p.port};
            if (ph.isValid() && isRoutableIp(ph.ip) && p.ultrapeer) {
                m_hosts.add(ph, QStringLiteral("uhc"));
                if (p.tls) m_hosts.setTls(ph, true);
            }
            if (!uhc.isEmpty()) {
                ++m_stats.uhcReplies;
                for (CacheEntry &c : m_hosts.uhcs())
                    if (c.url == uhc) { c.successes++; c.failures = 0; c.lastSuccess = nowSecs(); }
                log(QStringLiteral("UDP host cache %1: %2 hosts (%3 new)").arg(uhc).arg(p.ipp.size()).arg(added));
                if (added) QTimer::singleShot(0, this, &GnutellaPlugin::maintain);
            }
        } else if (m.type == MsgPing) {
            if (m_udpCbGuids.contains(m.guid)) {
                m_udpCbGuids.remove(m.guid);
                if (!m_udpReachable) log(QStringLiteral("UDP connect-back received from %1: UDP is reachable").arg(from.toString()));
                m_udpReachable = true;
            }
        }
    }
}

} // namespace mx::gnut
