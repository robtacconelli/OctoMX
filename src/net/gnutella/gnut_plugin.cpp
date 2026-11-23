// SPDX-License-Identifier: GPL-3.0-or-later
// Gnutella plugin: lifecycle, settings, status, servers list, persistence.
#include "gnut_plugin_impl.h"

#include "mx/settings_keys.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QHostInfo>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QUdpSocket>

namespace mx {
mx::NetworkPlugin *createGnutellaPlugin(QObject *parent) { return new mx::gnut::GnutellaPlugin(parent); }
}

namespace mx::gnut {

QStringList defaultGwcs()
{
    // Checked live 2026-11-20 with get=1&net=gnutella: all returned H| lines.
    return {
        QStringLiteral("http://cache.jayl.de/g2/gwc.php"),
        QStringLiteral("http://skulls.gwc.dyslexicfish.net/skulls.php"),
        QStringLiteral("http://gweb.4octets.co.uk/skulls.php"),
        QStringLiteral("http://gweb4.4octets.co.uk/gwc.php"),
        QStringLiteral("http://scissors.gwc.dyslexicfish.net:3709/"),
        QStringLiteral("http://paper.gwc.dyslexicfish.net:3709/"),
        QStringLiteral("http://rock.gwc.dyslexicfish.net:3709/"),
        QStringLiteral("http://bj.ddns.net/skulls/skulls.php"),
        QStringLiteral("http://bj.ddns.net/beacon/gwc.php"),
        QStringLiteral("http://ghc4.gtkgnutella.com/list4"),   // gtk-gnutella's crawler list ("ip:port,vendor")
    };
}

QStringList defaultUhcs()
{
    // gtk-gnutella's boot list; on 2026-11-20 only 2.uhc.gtk-gnutella.nl replied, the dyslexicfish
    // names resolved to 0.0.0.0 (kept: they may come back, failures back off).
    return {
        QStringLiteral("2.uhc.gtk-gnutella.nl:4876"),
        QStringLiteral("1.uhc.gtk-gnutella.nl:19104"),
        QStringLiteral("useast.gnutella.dyslexicfish.net:3558"),
        QStringLiteral("uswest.gnutella.dyslexicfish.net:3558"),
        QStringLiteral("uk.gnutella.dyslexicfish.net:3558"),
        QStringLiteral("au.gnutella.dyslexicfish.net:3558"),
    };
}

GnutellaPlugin::GnutellaPlugin(QObject *parent) : NetworkPlugin(parent)
{
    m_maintainTimer.setInterval(2000);
    connect(&m_maintainTimer, &QTimer::timeout, this, &GnutellaPlugin::maintain);
    m_saveTimer.setInterval(60000);
    connect(&m_saveTimer, &QTimer::timeout, this, &GnutellaPlugin::saveState);
    m_searchTimer.setInterval(1000);
    connect(&m_searchTimer, &QTimer::timeout, this, &GnutellaPlugin::searchTick);
    m_hitFlushTimer.setInterval(300);
    m_hitFlushTimer.setSingleShot(true);
    connect(&m_hitFlushTimer, &QTimer::timeout, this, &GnutellaPlugin::flushSearchHits);
    m_qrpTimer.setSingleShot(true);
    m_qrpTimer.setInterval(3000);
    connect(&m_qrpTimer, &QTimer::timeout, this, [this] {
        for (UltrapeerConnection *c : connectedUps()) sendQrp(c, false);
    });
}

GnutellaPlugin::~GnutellaPlugin()
{
    *m_alive = false;
    if (m_started) stop();
}

QVariant GnutellaPlugin::setting(const QString &key, const QVariant &fallback) const
{
    if (!m_ctx.settings) return fallback;
    const QVariant v = m_ctx.settings->value(key);
    return v.isValid() && !(v.typeId() == QMetaType::QString && v.toString().isEmpty() && fallback.typeId() != QMetaType::QString)
               ? v : fallback;
}

quint32 GnutellaPlugin::ourSpeedKbps() const
{
    return speedForConnection(setting(QString(keys::identity::Connection), QStringLiteral("DSL")).toString());
}

int GnutellaPlugin::maxUltrapeers() const { return qBound(1, setting(QString(keys::gnutella::MaxUltrapeers), 3).toInt(), 10); }

int GnutellaPlugin::uploadSlots() const
{
    // "gnutella/uploadSlots" (wanted key) → transfers/maxUploads → 4
    int n = setting(QStringLiteral("gnutella/uploadSlots"), 0).toInt();
    if (n <= 0) n = setting(QString(keys::transfers::MaxUploads), 4).toInt();
    return qBound(1, n <= 0 ? 4 : n, 100);
}

int GnutellaPlugin::connectTimeoutMs() const
{
    return qBound(3, setting(QString(keys::connection::ConnectTimeoutSec), 20).toInt(), 120) * 1000;
}

void GnutellaPlugin::log(const QString &s)
{
    qCInfo(lcGnut).noquote() << s;
    emit logMessage(QStringLiteral("Gnutella: ") + s);
}

void GnutellaPlugin::sourceLog(const QString &line) { qCInfo(lcGnut).noquote() << line; }

void GnutellaPlugin::init(const mx::PluginContext &ctx)
{
    m_ctx = ctx;
    m_inited = true;
    const QString client = ctx.clientName.isEmpty() ? QStringLiteral("OctoMX") : ctx.clientName;
    const QString ver = ctx.clientVersion.isEmpty() ? QStringLiteral(MX_VERSION_STRING) : ctx.clientVersion;
    m_userAgent = client + QLatin1Char('/') + ver;
    if (!m_ctx.dataDir.isEmpty()) QDir().mkpath(m_ctx.dataDir);
    // servent GUID: persistent across restarts
    QFile gf(m_ctx.dataDir + QStringLiteral("/servent.guid"));
    if (gf.open(QIODevice::ReadOnly)) m_guid = QByteArray::fromHex(gf.read(64).trimmed());
    gf.close();
    if (m_guid.size() != 16) {
        m_guid = randomGuid(true);
        if (gf.open(QIODevice::WriteOnly)) gf.write(m_guid.toHex());
    }
    m_hosts.setPath(m_ctx.dataDir + QStringLiteral("/hosts.json"));
    m_hosts.load();
    QStringList gwcs = defaultGwcs();
    for (const QString &u : setting(QString(keys::gnutella::GWebCaches), QStringList()).toStringList())
        if (!gwcs.contains(u.trimmed()) && u.startsWith(QLatin1String("http"))) gwcs << u.trimmed();
    QStringList uhcs = defaultUhcs();
    for (const QString &u : setting(QStringLiteral("gnutella/udpHostCaches"), QStringList()).toStringList())
        if (!uhcs.contains(u.trimmed())) uhcs << u.trimmed();
    m_hosts.setBuiltinCaches(gwcs, uhcs);
    for (const QString &h : setting(QString(keys::gnutella::BootstrapHosts), QStringList()).toStringList()) {
        const HostPort hp = HostPort::parse(h, 6346);
        if (hp.isValid()) m_hosts.addStatic(hp, QString());
    }
    if (m_ctx.settings) {
        connect(m_ctx.settings, &mx::Settings::changed, this, [this](const QString &key, const QVariant &) {
            if (key == QLatin1String(keys::gnutella::MaxUltrapeers.data()) && m_started) maintain();
            if (key == QLatin1String(keys::gnutella::Enabled.data()) && !setting(key, true).toBool() && m_started) stop();
            if (key == QLatin1String(keys::gnutella::Encryption.data())) applyTlsMode();   // [enc-1]
        });
    }
    log(QStringLiteral("init: %1 hosts remembered, servent GUID %2").arg(m_hosts.size()).arg(guidHex(m_guid)));
}

void GnutellaPlugin::start()
{
    if (!m_inited || m_started) return;
    if (!setting(QString(keys::gnutella::Enabled), true).toBool()) { emitStatus(); return; }
    m_started = true;
    m_uptime.start();
    if (tlsMode() != TlsMode::Off) ensureTls();   // [enc-1] certificate made on first use
    if (!m_nam) m_nam = new QNetworkAccessManager(this);
    // listener (TCP: handshakes, HTTP uploads, GIV, connect-back) + UDP on the same port
    int port = setting(QString(keys::gnutella::Port), 6346).toInt();
    m_server = new QTcpServer(this);
    connect(m_server, &QTcpServer::newConnection, this, &GnutellaPlugin::onNewConnection);
    bool bound = false;
    if (port <= 0) {
        bound = m_server->listen(QHostAddress::AnyIPv4, 0);
    } else {
        for (int i = 0; i < 10 && !bound; ++i) bound = m_server->listen(QHostAddress::AnyIPv4, quint16(port + i));
        if (!bound) bound = m_server->listen(QHostAddress::AnyIPv4, 0);
    }
    m_port = bound ? m_server->serverPort() : 0;
    m_udp = new QUdpSocket(this);
    if (m_port && !m_udp->bind(QHostAddress::AnyIPv4, m_port)) log(QStringLiteral("UDP bind on %1 failed").arg(m_port));
    connect(m_udp, &QUdpSocket::readyRead, this, &GnutellaPlugin::onUdpReadyRead);
    log(QStringLiteral("listening on port %1 (TCP%2)").arg(m_port).arg(m_udp->state() == QAbstractSocket::BoundState ? "+UDP" : ""));
    m_maintainTimer.start();
    m_saveTimer.start();
    m_searchTimer.start();
    QTimer::singleShot(0, this, &GnutellaPlugin::maintain);
    QTimer::singleShot(100, this, &GnutellaPlugin::bootstrap);   // one UHC ping keeps the cache fresh
    emitStatus();
}

void GnutellaPlugin::stop()
{
    if (!m_started) return;
    m_started = false;
    m_maintainTimer.stop();
    m_saveTimer.stop();
    m_searchTimer.stop();
    const auto conns = m_conns;
    for (UltrapeerConnection *c : conns) {
        c->disconnect(this);
        c->close(QStringLiteral("shutdown"), 200);
        c->deleteLater();
    }
    m_conns.clear();
    for (auto it = m_searches.begin(); it != m_searches.end(); ++it)
        if (!it->done) { it->done = true; emit searchDone(it.key()); }
    const auto sessions = m_sessions;
    for (UploadSession *s : sessions) s->cancel();
    for (const QueueEntry &e : std::as_const(m_uploadQueue))   // [glue-3] drop the Queued rows
        if (!e.uploadId.isEmpty() && !(e.session && e.session->uploadId() == e.uploadId)) reportUploadFinished(e.uploadId);
    m_uploadQueue.clear();
    for (auto &b : m_browses) if (b) b->abort();
    if (m_server) { m_server->close(); m_server->deleteLater(); m_server = nullptr; }
    if (m_udp) { m_udp->close(); m_udp->deleteLater(); m_udp = nullptr; }
    for (auto it = m_sniff.begin(); it != m_sniff.end(); ++it) it.key()->deleteLater();
    m_sniff.clear();
    saveState();
    emitStatus();
    emit serversChanged();
}

// ---------------------------------------------------------------- [enc-1] TLS
TlsMode GnutellaPlugin::tlsMode() const
{
    return tlsModeFromSetting(setting(QString(keys::gnutella::Encryption), QStringLiteral("enabled")).toString());
}

QString GnutellaPlugin::tlsCertificatePath() const
{
    return m_ctx.dataDir.isEmpty() ? QString() : m_ctx.dataDir + QStringLiteral("/tls.pem");
}

bool GnutellaPlugin::ensureTls()
{
    if (m_tls) return true;
    if (!m_tlsError.isEmpty()) return false;     // tried once and failed: do not retry every connection
    QElapsedTimer t;
    t.start();
    m_tls = TlsEngine::create(tlsCertificatePath(), &m_tlsError);
    if (!m_tls) {
        if (m_tlsError.isEmpty()) m_tlsError = QStringLiteral("TLS setup failed");
        log(QStringLiteral("TLS not available: %1").arg(m_tlsError));
        return false;
    }
    log(QStringLiteral("TLS ready (%1%2, %3 ms); peers do not verify certificates")
            .arg(m_tls->certificateInfo(), m_tls->createdNow() ? QStringLiteral(", new certificate") : QString())
            .arg(t.elapsed()));
    return true;
}

bool GnutellaPlugin::tlsAdvertised() const { return tlsMode() != TlsMode::Off && m_tls; }

TlsSocket::Policy GnutellaPlugin::tlsPolicy(bool peerTls) const
{
    switch (tlsMode()) {
    case TlsMode::Off: return TlsSocket::Plain;
    case TlsMode::Require: return TlsSocket::TlsOnly;     // try TLS with everybody, never plain
    default: return peerTls ? TlsSocket::TryTls : TlsSocket::Plain;
    }
}

QTcpSocket *GnutellaPlugin::newSocket(bool peerTls, QObject *parent)
{
    const TlsSocket::Policy p = tlsPolicy(peerTls);
    if (p == TlsSocket::Plain) return new QTcpSocket(parent);
    if (!ensureTls()) {
        // TLS is broken on this system: "require" then means no connection at all
        if (p == TlsSocket::TlsOnly) return new TlsSocket(nullptr, TlsSocket::TlsOnly, parent);
        return new QTcpSocket(parent);
    }
    return new TlsSocket(m_tls, p, parent);
}

int GnutellaPlugin::encryptedLinks() const
{
    int n = 0;
    for (UltrapeerConnection *c : connectedUps()) if (c->encrypted()) ++n;
    return n;
}

void GnutellaPlugin::applyTlsMode()
{
    const TlsMode m = tlsMode();
    log(QStringLiteral("encryption setting: TLS %1").arg(tlsModeName(m)));
    if (m != TlsMode::Off) ensureTls();
    m_browseCache.clear();                     // the TLS flag of our browse hits changes
    if (!m_started) return;
    // links that contradict the new setting are replaced (transfers keep their connection)
    int closed = 0;
    for (UltrapeerConnection *c : connectedUps()) {
        if ((m == TlsMode::Require && !c->encrypted()) || (m == TlsMode::Off && c->encrypted())) {
            c->close(QStringLiteral("encryption setting changed"), 200);
            ++closed;
        }
    }
    if (closed) log(QStringLiteral("closed %1 after the encryption change").arg(countNoun(closed, QStringLiteral("ultrapeer link"))));
    QTimer::singleShot(0, this, &GnutellaPlugin::maintain);
}

void GnutellaPlugin::saveState()
{
    if (m_hosts.dirty() || !m_started) m_hosts.save();
}

int GnutellaPlugin::connectedCount() const { return int(connectedUps().size()); }

mx::NetworkStatus GnutellaPlugin::status() const
{
    mx::NetworkStatus st;
    st.net = mx::Net::Gnutella;
    const int n = connectedCount();
    const bool enabled = setting(QString(keys::gnutella::Enabled), true).toBool();
    if (!m_started) {
        st.state = enabled ? mx::ConnState::Disconnected : mx::ConnState::Disabled;
        st.statusText = enabled ? QStringLiteral("Not connected") : QStringLiteral("Disabled");
    } else if (n > 0) {
        st.state = mx::ConnState::Connected;
        st.statusText = n == 1 ? QStringLiteral("Connected to 1 ultrapeer") : QStringLiteral("Connected to %1 ultrapeers").arg(n);
    } else {
        st.state = mx::ConnState::Connecting;
        st.statusText = QStringLiteral("Connecting... (%1 known)").arg(mx::countNoun(m_hosts.size(), QStringLiteral("host")));
    }
    st.peers = n;
    st.firewalled = m_started ? firewalled() : false;
    st.listenPort = m_port;
    if (m_externalIp) st.externalAddress = ipToString(m_externalIp);
    return st;
}

void GnutellaPlugin::emitStatus()
{
    const mx::NetworkStatus st = status();
    if (st.state != m_lastStatus.state || st.statusText != m_lastStatus.statusText || st.firewalled != m_lastStatus.firewalled ||
        st.peers != m_lastStatus.peers || st.listenPort != m_lastStatus.listenPort || st.externalAddress != m_lastStatus.externalAddress) {
        m_lastStatus = st;
        emit statusChanged(st);
    }
}

QList<mx::ServerEntry> GnutellaPlugin::servers() const
{
    QList<mx::ServerEntry> out;
    QSet<quint64> listed;
    for (UltrapeerConnection *c : m_conns) {
        mx::ServerEntry e;
        e.net = mx::Net::Gnutella;
        e.id = c->id();
        e.address = c->id();
        e.name = c->vendor().isEmpty() ? c->id() : c->vendor();
        e.state = c->state() == UltrapeerConnection::Connected ? mx::ConnState::Connected : mx::ConnState::Connecting;
        e.pingMs = c->pingMs();
        if (c->state() == UltrapeerConnection::Connected)
            e.description = QStringLiteral("Ultrapeer, connected %1 min%2%3%4").arg(c->connectedMs() / 60000)
                                .arg(c->encrypted() ? QStringLiteral(", TLS") : QString())   // [enc-1]
                                .arg(c->compressedIn() ? QStringLiteral(", deflate") : QString())
                                .arg(c->qrpAcked ? QStringLiteral(", QRP sent") : QString());
        else e.description = QStringLiteral("Connecting...");
        const HostEntry *he = m_hosts.find(c->address());
        e.isStatic = he && he->isStatic;
        listed.insert(c->address().key());
        out << e;
    }
    QList<HostEntry> remembered = m_hosts.entries();
    const qint64 now = nowSecs();
    std::sort(remembered.begin(), remembered.end(), [now](const HostEntry &a, const HostEntry &b) { return a.score(now) > b.score(now); });
    int shown = 0;
    for (const HostEntry &h : remembered) {
        if (listed.contains(h.addr.key())) continue;
        if (!h.isStatic && (shown >= 50 || h.lastSuccess == 0)) continue;
        ++shown;
        mx::ServerEntry e;
        e.net = mx::Net::Gnutella;
        e.id = h.addr.toString();
        e.address = h.addr.toString();
        e.name = !h.hostName.isEmpty() ? h.hostName : (!h.vendor.isEmpty() ? h.vendor : e.id);
        e.state = mx::ConnState::Disconnected;
        e.isStatic = h.isStatic;
        QString d = QStringLiteral("%1 ok / %2 failed").arg(h.successes).arg(h.totalFailures);
        if (h.lastSuccess) d += QStringLiteral(", last connected %1").arg(QDateTime::fromSecsSinceEpoch(h.lastSuccess).toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        e.description = d;
        out << e;
    }
    return out;
}

void GnutellaPlugin::addServer(const QString &address, const QString &name)
{
    QString a = address.trimmed();
    if (a.startsWith(QLatin1String("gnutella://"))) a = a.mid(11);
    a = a.section(QLatin1Char('/'), 0, 0);
    const HostPort hp = HostPort::parse(a, 6346);
    if (hp.isValid()) {
        m_hosts.addStatic(hp, name);
        m_hosts.save();
        emit serversChanged();
        return;
    }
    // host name: resolve asynchronously
    const QString host = a.section(QLatin1Char(':'), 0, 0);
    const quint16 port = quint16(a.contains(QLatin1Char(':')) ? a.section(QLatin1Char(':'), 1).toUInt() : 6346);
    if (host.isEmpty() || port == 0) { log(QStringLiteral("cannot add server \"%1\"").arg(address)); return; }
    QPointer<GnutellaPlugin> self(this);
    QHostInfo::lookupHost(host, this, [self, host, port, name](const QHostInfo &info) {
        if (!self) return;
        for (const QHostAddress &ad : info.addresses()) {
            if (ad.protocol() != QAbstractSocket::IPv4Protocol) continue;
            HostPort hp;
            hp.ip = ad.toIPv4Address();
            hp.port = port;
            if (!hp.isValid()) continue;
            self->m_hosts.addStatic(hp, name.isEmpty() ? host : name);
            self->m_hosts.save();
            emit self->serversChanged();
            return;
        }
        self->log(QStringLiteral("cannot resolve %1").arg(host));
    });
}

void GnutellaPlugin::removeServer(const QString &id)
{
    for (UltrapeerConnection *c : m_conns)
        if (c->id() == id) c->close(QStringLiteral("removed by user"), 200);
    m_hosts.remove(id);
    m_hosts.save();
    emit serversChanged();
}

void GnutellaPlugin::connectServer(const QString &id)
{
    const HostPort hp = HostPort::parse(id, 6346);
    if (!hp.isValid() || !m_started) return;
    for (UltrapeerConnection *c : m_conns) if (c->address() == hp) return;
    m_hosts.add(hp, QStringLiteral("static"));
    startConnection(hp, true);
}

void GnutellaPlugin::disconnectServer(const QString &id)
{
    for (UltrapeerConnection *c : m_conns)
        if (c->id() == id) {
            m_manualConnect.remove(c->address().key());
            m_hosts.markFailure(c->address(), 600);   // do not reconnect right away
            c->close(QStringLiteral("disconnected by user"), 200);
        }
}

QVariantMap GnutellaPlugin::debugStats() const
{
    QVariantMap m;
    m[QStringLiteral("connected")] = connectedCount();
    m[QStringLiteral("connecting")] = int(m_conns.size()) - connectedCount();
    m[QStringLiteral("hostsKnown")] = m_hosts.size();
    m[QStringLiteral("handshakesOk")] = m_stats.handshakesOk;
    m[QStringLiteral("handshakesFailed")] = m_stats.handshakesFailed;
    m[QStringLiteral("qrpSent")] = m_stats.qrpSent;
    m[QStringLiteral("qrpSlotsFilled")] = m_qrp.filled();
    m[QStringLiteral("queriesSeen")] = m_stats.queriesSeen;
    m[QStringLiteral("queriesAnswered")] = m_stats.queriesAnswered;
    m[QStringLiteral("hitsSent")] = m_stats.hitsSent;
    m[QStringLiteral("hitsReceived")] = m_stats.hitsReceived;
    m[QStringLiteral("incomingHandshakes")] = m_stats.incomingHandshakes;
    m[QStringLiteral("incomingHttp")] = m_stats.incomingHttp;
    m[QStringLiteral("browsesServed")] = m_stats.browsesServed;
    m[QStringLiteral("pushesReceived")] = m_stats.pushesReceived;
    m[QStringLiteral("givsReceived")] = m_stats.givsReceived;
    m[QStringLiteral("givsSent")] = m_stats.givsSent;
    m[QStringLiteral("connectBacks")] = m_stats.connectBacks;
    m[QStringLiteral("gwcOk")] = m_stats.gwcOk;
    m[QStringLiteral("gwcFail")] = m_stats.gwcFail;
    m[QStringLiteral("uhcReplies")] = m_stats.uhcReplies;
    m[QStringLiteral("pushProxyAcks")] = m_stats.pushProxyAcks;
    m[QStringLiteral("firewalled")] = firewalled();
    m[QStringLiteral("reachableHow")] = m_reachableHow;
    m[QStringLiteral("udpReachable")] = m_udpReachable;
    m[QStringLiteral("externalIp")] = m_externalIp ? ipToString(m_externalIp) : QString();
    m[QStringLiteral("incomingAgents")] = m_stats.incomingAgents;
    m[QStringLiteral("uploadsDone")] = m_uploadsDone;
    {   // [enc-1]
        const TlsStats ts = tlsStats();
        m[QStringLiteral("tlsMode")] = tlsModeName(tlsMode());
        m[QStringLiteral("tlsLinks")] = encryptedLinks();
        m[QStringLiteral("tlsOutAttempts")] = ts.outAttempts;
        m[QStringLiteral("tlsOutOk")] = ts.outOk;
        m[QStringLiteral("tlsOutFallback")] = ts.outFallback;
        m[QStringLiteral("tlsOutFailed")] = ts.outFailed;
        m[QStringLiteral("tlsInOk")] = ts.inOk;
        m[QStringLiteral("tlsInFailed")] = ts.inFailed;
        m[QStringLiteral("tlsInRefused")] = ts.inRefused;
        m[QStringLiteral("tlsHostsKnown")] = m_hosts.tlsCount();
    }
    return m;
}

QStringList GnutellaPlugin::connectionLines() const
{
    QStringList l;
    for (UltrapeerConnection *c : m_conns) {
        if (c->state() != UltrapeerConnection::Connected) continue;
        l << QStringLiteral("%1 %2 [%10, handshake %3 ms, in:%4 out:%5, %6 msgs in, ping %7 ms%8%9]")
                 .arg(c->id(), c->vendor())
                 .arg(c->handshakeMs())
                 .arg(c->compressedIn() ? "deflate" : "plain", c->compressedOut() ? "deflate" : "plain")
                 .arg(c->messagesIn())
                 .arg(c->pingMs())
                 .arg(c->qrpAcked ? QStringLiteral(", QRP sent") : QString(),
                      c->pushProxyAddr.isValid() ? QStringLiteral(", push proxy") : QString())
                 .arg(c->encrypted() ? QStringLiteral("TLS ") + c->tlsInfo() : QStringLiteral("plain"));
    }
    return l;
}

} // namespace mx::gnut
