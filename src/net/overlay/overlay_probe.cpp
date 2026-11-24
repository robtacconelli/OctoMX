// SPDX-License-Identifier: GPL-3.0-or-later
// [diag-1] OctoMX overlay: Connection Test facts and "probe my ports" (PROTOCOL.md section 11).
//
// Requester: probePorts() sends {"t":"probe","rid","nonce","ports":[{"p":"TCP"|"UDP","n":port,"k":kind}]}
// to at most two hello-verified, directly connected peers.
// Helper: onProbeRequest() tests ONLY the requester's own source address (the address of the live
// link it sent the request over, never an address named in the message), ONLY the listed ports (at
// most 8), at most once per requester per minute and 10 times per 10 minutes overall, and answers
// {"t":"probed","rid","helper","res":[{"p","n","r":"yes"|"no"|"sent"|"untested","e"}]}.
//   TCP: a plain connect, closed at once (yes = connected within 6 s).
//   UDP "dht": a BitTorrent DHT ping (yes = an answer within 6 s).
//   UDP "datagram": three datagrams "WMXNG-PROBE <nonce> <port>" ("sent"); the requester's own
//   socket owner reports it through mx::consumeProbeDatagram(), the requester decides.
#include "overlay_plugin_impl.h"

#include "mx/settings_keys.h"

#include <QtCore/QJsonArray>
#include <QtCore/QRandomGenerator>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpSocket>
#include <QtNetwork/QUdpSocket>

#include "mx/net_policy.h"

#include <memory>

namespace mx::ovl {

namespace {
constexpr int kProbeTimeoutMs = 6000;
constexpr int kMaxProbePorts = 8;

QString hostOfAddress(const QString &addr)
{
    if (addr.startsWith(QLatin1Char('['))) {
        const int e = int(addr.indexOf(QLatin1Char(']')));
        return e > 0 ? addr.mid(1, e - 1) : QString();
    }
    if (addr.count(QLatin1Char(':')) == 1) return addr.section(QLatin1Char(':'), 0, 0);
    return addr;
}
} // namespace

PluginDiagnostics OverlayPlugin::diagnostics() const
{
    PluginDiagnostics d;
    d.valid = true;
    if (!m_running || !m_node) return d;
    const int port = m_node->listenPort();
    for (const char *proto : {"TCP", "UDP"}) {
        PortBinding b;
        b.proto = QLatin1String(proto);
        b.port = port ? port : m_wantPort;
        b.ok = port != 0;
        if (port && m_wantPort && port != m_wantPort) b.error = QStringLiteral("port %1 busy, using %2").arg(m_wantPort).arg(port);
        b.purpose = QStringLiteral("OctoMX peers");
        d.bindings << b;
    }
    const int dht = m_node->dhtPort();
    if (dht > 0 && dht != port) {
        PortBinding b;
        b.proto = QStringLiteral("UDP");
        b.port = dht;
        b.ok = true;
        b.purpose = QStringLiteral("overlay DHT, random port");
        b.mapByCore = false;   // a new random port every start: nothing to forward
        d.bindings << b;
    }
    if (m_inboundTcp) {
        d.tcpIn = Reach::Yes;
        d.tcpEvidence = QStringLiteral("other OctoMX users connected to us over TCP");
    }
    if (m_inboundUdp) {
        d.udpIn = Reach::Yes;
        d.udpEvidence = QStringLiteral("other OctoMX users connected to us over UDP");
    }
    d.externalAddress = m_status.externalAddress.section(QLatin1Char(':'), 0, 0);
    if (!d.externalAddress.isEmpty()) d.externalSeenBy = QStringLiteral("OctoMX peers");
    d.dhtNodes = m_node->dhtNodes();
    int direct = 0, relayed = 0;
    for (auto it = m_peers.cbegin(); it != m_peers.cend(); ++it) {
        if (!it->hello) continue;
        if (it->transport == QLatin1String("relay")) ++relayed;
        else ++direct;
    }
    d.items << DiagItem{QStringLiteral("octomx.users"), QStringLiteral("OctoMX users connected"), direct + relayed > 0 ? DiagLevel::Ok : DiagLevel::Unknown,
                        QString::number(direct + relayed), relayed ? QStringLiteral("%1 through a relay").arg(relayed) : QString(),
                        direct > 0 ? QStringLiteral("They can test your ports from the outside: press Check Ports Online.")
                                   : QStringLiteral("No other OctoMX user is connected right now, so they cannot test your ports.")};
    // [enc-1] librats links always run the Noise protocol; private messages are end-to-end on top
    d.items << DiagItem{QStringLiteral("octomx.encryption"), QStringLiteral("Encryption"), DiagLevel::Ok,
                        QStringLiteral("Always on"),
                        QStringLiteral("%1 encrypted (Noise), private messages end-to-end").arg(countNoun(direct + relayed, QStringLiteral("link"))),
                        QStringLiteral("Every OctoMX network link is encrypted and cannot be switched off. Private messages are also "
                                       "encrypted end to end (X25519, ChaCha20-Poly1305). The DHT that finds peers is plain UDP. "
                                       "It does not hide your IP address from the users you talk to.")};
    return d;
}

bool OverlayPlugin::probePorts(quint32 token, const QList<PortProbe> &ports)
{
    if (!m_running || !m_node || ports.isEmpty()) return false;
    QStringList helpers;
    for (auto it = m_peers.cbegin(); it != m_peers.cend() && helpers.size() < 2; ++it)
        if (it->hello && it->transport != QLatin1String("relay") && !it->address.isEmpty()) helpers << it.key();
    if (helpers.isEmpty()) return false;
    ProbeReq req;
    req.ports = ports.mid(0, kMaxProbePorts);
    for (PortProbe &p : req.ports) {
        p.reach = Reach::Unknown;
        p.helper.clear();
        p.error = QStringLiteral("no answer from the other OctoMX users");
    }
    req.nonce = QString::number(QRandomGenerator::global()->generate64(), 16);
    req.asked = int(helpers.size());
    req.deadlineMs = nowMs() + 16000;
    QJsonArray arr;
    for (const PortProbe &p : std::as_const(req.ports)) {
        QJsonObject x;
        x[QStringLiteral("p")] = p.proto;
        x[QStringLiteral("n")] = p.port;
        x[QStringLiteral("k")] = p.kind;
        arr.append(x);
    }
    QJsonObject msg;
    msg[QStringLiteral("t")] = QStringLiteral("probe");
    msg[QStringLiteral("v")] = kWireVersion;
    msg[QStringLiteral("rid")] = QString::number(token);
    msg[QStringLiteral("nonce")] = req.nonce;
    msg[QStringLiteral("ports")] = arr;
    m_probeReqs.insert(token, req);
    for (const QString &h : std::as_const(helpers)) sendRaw(h, msg);
    QTimer::singleShot(16000, this, [this, token] { finishProbe(token); });
    return true;
}

// [net-2] a probe datagram leaves from the chosen adapter only (or from any when none is chosen)
static bool bindProbeUdp(QUdpSocket *u, const QHostAddress &to, const mx::Settings *s)
{
    const mx::netpolicy::Policy pol = mx::netpolicy::fromSettings(s);
    if (pol.proxy.active()) return false;
    const bool v6 = to.protocol() == QAbstractSocket::IPv6Protocol;
    QHostAddress from = v6 ? QHostAddress(QHostAddress::AnyIPv6) : QHostAddress(QHostAddress::AnyIPv4);
    if (pol.bindWanted()) {
        const mx::netpolicy::Adapter a = mx::netpolicy::resolveAdapter(pol.bind);
        from = v6 ? a.v6() : a.v4();
    }
    u->setProxy(QNetworkProxy::NoProxy);
    return !from.isNull() && u->bind(from, 0);
}

void OverlayPlugin::onProbeRequest(const QString &nodeId, const QJsonObject &o)
{
    const QString rid = jStr(o, "rid", 24);
    const QString nonce = jStr(o, "nonce", 32);
    const QString uid = m_nodeUser.value(nodeId);
    auto reply = [this, nodeId, rid](const QJsonArray &res, const QString &error) {
        QJsonObject a;
        a[QStringLiteral("t")] = QStringLiteral("probed");
        a[QStringLiteral("v")] = kWireVersion;
        a[QStringLiteral("rid")] = rid;
        a[QStringLiteral("helper")] = m_displayName;
        a[QStringLiteral("res")] = res;
        if (!error.isEmpty()) a[QStringLiteral("e")] = error;
        sendRaw(nodeId, a);
    };
    if (rid.isEmpty() || uid.isEmpty()) return;
    if (!setting(QString(keys::diag::AnswerProbes), true).toBool()) return reply({}, QStringLiteral("this user does not test ports"));
    const qint64 now = nowMs();
    if (now - m_probeServedMs.value(uid, -120000) < 60000) return reply({}, QStringLiteral("asked too often, try again in a minute"));
    while (!m_probeServedAll.empty() && now - m_probeServedAll.front() > 600000) m_probeServedAll.pop_front();
    if (m_probeServedAll.size() >= 10) return reply({}, QStringLiteral("busy, try again later"));
    const PeerState ps = m_peers.value(nodeId);
    if (ps.transport == QLatin1String("relay") || ps.address.isEmpty())
        return reply({}, QStringLiteral("connected through a relay, cannot reach your address"));
    const QHostAddress ip(hostOfAddress(ps.address));
    if (ip.isNull()) return reply({}, QStringLiteral("unknown address"));
    m_probeServedMs.insert(uid, now);
    m_probeServedAll.push_back(now);

    struct Job {
        QJsonArray res;
        int left = 0;
    };
    auto job = std::make_shared<Job>();
    const QJsonArray ports = o.value(QStringLiteral("ports")).toArray();
    QList<QJsonObject> todo;
    for (const QJsonValue &v : ports) {
        if (todo.size() >= kMaxProbePorts) break;
        const QJsonObject x = v.toObject();
        const QString proto = x.value(QStringLiteral("p")).toString();
        const int port = x.value(QStringLiteral("n")).toInt();
        if ((proto != QLatin1String("TCP") && proto != QLatin1String("UDP")) || port <= 0 || port > 65535) continue;
        todo << x;
    }
    if (todo.isEmpty()) return reply({}, QStringLiteral("no ports to test"));
    job->left = int(todo.size());
    qCInfo(lcOverlay).noquote() << QStringLiteral("port probe for %1: testing %2 ports at %3").arg(userRefFor(uid).name).arg(todo.size()).arg(ip.toString());
    auto done = [job, reply](const QString &proto, int port, const QString &r, const QString &e) {
        QJsonObject x;
        x[QStringLiteral("p")] = proto;
        x[QStringLiteral("n")] = port;
        x[QStringLiteral("r")] = r;
        if (!e.isEmpty()) x[QStringLiteral("e")] = e.left(120);
        job->res.append(x);
        if (--job->left == 0) reply(job->res, QString());
    };
    for (const QJsonObject &x : std::as_const(todo)) {
        const QString proto = x.value(QStringLiteral("p")).toString();
        const quint16 port = quint16(x.value(QStringLiteral("n")).toInt());
        const QString kind = x.value(QStringLiteral("k")).toString();
        if (proto == QLatin1String("TCP")) {
            auto *s = new QTcpSocket(this);
            auto fired = std::make_shared<bool>(false);
            auto finish = [s, fired, done, port](bool ok, const QString &err) {
                if (*fired) return;
                *fired = true;
                done(QStringLiteral("TCP"), port, ok ? QStringLiteral("yes") : QStringLiteral("no"), err);
                s->abort();
                s->deleteLater();
            };
            connect(s, &QTcpSocket::connected, this, [finish] { finish(true, QString()); });
            connect(s, &QTcpSocket::errorOccurred, this, [s, finish](QAbstractSocket::SocketError) { finish(false, s->errorString()); });
            QTimer::singleShot(kProbeTimeoutMs, s, [finish] { finish(false, QStringLiteral("no answer within 6 s")); });
            QString why;   // [net-2] from the chosen adapter only
            if (!mx::netpolicy::prepareTcp(s, m_ctx.settings, &why, ip.protocol() == QAbstractSocket::IPv6Protocol
                                                                        ? QAbstractSocket::IPv6Protocol : QAbstractSocket::IPv4Protocol)) {
                QTimer::singleShot(0, s, [finish, why] { finish(false, why); });
                continue;
            }
            s->connectToHost(ip, port);
        } else if (kind == QLatin1String("dht")) {
            auto *u = new QUdpSocket(this);
            auto fired = std::make_shared<bool>(false);
            auto finish = [u, fired, done, port](bool ok, const QString &err) {
                if (*fired) return;
                *fired = true;
                done(QStringLiteral("UDP"), port, ok ? QStringLiteral("yes") : QStringLiteral("no"), err);
                u->deleteLater();
            };
            if (!bindProbeUdp(u, ip, m_ctx.settings)) { QTimer::singleShot(0, u, [finish] { finish(false, QStringLiteral("not on the chosen adapter")); }); continue; }
            connect(u, &QUdpSocket::readyRead, this, [u, finish] {
                while (u->hasPendingDatagrams()) {
                    QByteArray d;
                    d.resize(int(qMax<qint64>(0, u->pendingDatagramSize())));
                    u->readDatagram(d.data(), d.size());
                    if (d.startsWith('d') && d.contains("1:t2:mp")) {
                        finish(true, QString());
                        return;
                    }
                }
            });
            QByteArray id(20, 0);
            for (int i = 0; i < 20; ++i) id[i] = char(QRandomGenerator::global()->bounded(256));
            const QByteArray ping = QByteArray("d1:ad2:id20:") + id + QByteArray("e1:q4:ping1:t2:mp1:y1:qe");
            u->writeDatagram(ping, ip, port);
            QTimer::singleShot(2000, u, [u, ping, ip, port] { u->writeDatagram(ping, ip, port); });
            QTimer::singleShot(kProbeTimeoutMs, u, [finish] { finish(false, QStringLiteral("no DHT answer within 6 s")); });
        } else if (kind == QLatin1String("datagram") && !nonce.isEmpty()) {
            auto *u = new QUdpSocket(this);
            if (!bindProbeUdp(u, ip, m_ctx.settings)) { u->deleteLater(); continue; }
            const QByteArray dg = probeDatagram(nonce, port);
            for (int i = 0; i < 3; ++i)
                QTimer::singleShot(i * 400, u, [u, dg, ip, port] { u->writeDatagram(dg, ip, port); });
            QTimer::singleShot(1200, u, [u, done, port] {
                done(QStringLiteral("UDP"), port, QStringLiteral("sent"), QString());
                u->deleteLater();
            });
        } else {
            done(proto, port, QStringLiteral("untested"), QStringLiteral("not testable from the outside"));
        }
    }
}

void OverlayPlugin::onProbeAnswer(const QString &nodeId, const QJsonObject &o)
{
    bool okNum = false;
    const quint32 token = jStr(o, "rid", 24).toUInt(&okNum);
    if (!okNum || !m_probeReqs.contains(token)) return;
    ProbeReq &req = m_probeReqs[token];
    const QString helper = QStringLiteral("OctoMX user %1").arg(jStr(o, "helper", 64).isEmpty() ? userRefFor(m_nodeUser.value(nodeId)).name : jStr(o, "helper", 64));
    const QString err = jStr(o, "e", 120);
    const QJsonArray res = o.value(QStringLiteral("res")).toArray();
    if (res.isEmpty() && !err.isEmpty())
        for (PortProbe &p : req.ports)
            if (p.reach == Reach::Unknown) p.error = QStringLiteral("%1: %2").arg(helper, err);
    bool waitDatagrams = false;
    for (const QJsonValue &v : res) {
        const QJsonObject x = v.toObject();
        const QString proto = x.value(QStringLiteral("p")).toString();
        const int port = x.value(QStringLiteral("n")).toInt();
        const QString r = x.value(QStringLiteral("r")).toString();
        for (PortProbe &p : req.ports) {
            if (p.proto != proto || p.port != port || p.reach == Reach::Yes) continue;
            if (r == QLatin1String("yes")) {
                p.reach = Reach::Yes;
                p.helper = helper;
                p.error.clear();
            } else if (r == QLatin1String("no")) {
                p.reach = Reach::No;
                p.helper = helper;
                p.error = x.value(QStringLiteral("e")).toString().left(120);
            } else if (r == QLatin1String("sent")) {
                p.helper = helper;
                waitDatagrams = true;
            } else {
                p.error = x.value(QStringLiteral("e")).toString().left(120);
            }
        }
    }
    ++req.answers;
    const QString nonce = req.nonce;
    auto settle = [this, token, nonce] {
        auto it = m_probeReqs.find(token);
        if (it == m_probeReqs.end()) return;
        for (PortProbe &p : it->ports) {
            if (p.proto != QLatin1String("UDP") || p.kind != QLatin1String("datagram") || p.reach == Reach::Yes || p.helper.isEmpty()) continue;
            if (probeDatagramSeen(nonce, quint16(p.port))) {
                p.reach = Reach::Yes;
                p.error.clear();
            } else {
                p.reach = Reach::No;
                p.error = QStringLiteral("the test datagrams did not arrive");
            }
        }
        if (it->answers >= it->asked) finishProbe(token);
    };
    if (waitDatagrams) QTimer::singleShot(1000, this, settle);
    else settle();
}

void OverlayPlugin::finishProbe(quint32 token)
{
    auto it = m_probeReqs.find(token);
    if (it == m_probeReqs.end()) return;
    const QList<PortProbe> ports = it->ports;
    m_probeReqs.erase(it);
    Q_EMIT portProbeResult(token, ports);
}

} // namespace mx::ovl
