// SPDX-License-Identifier: GPL-3.0-or-later
// [net-2] OctoMX: the binding relay for web requests (see bind_relay.h).
#include "bind_relay.h"

#include "mx/net_policy.h"

#include <QtCore/QAtomicInt>
#include <QtCore/QCoreApplication>
#include <QtCore/QMutex>
#include <QtCore/QPointer>
#include <QtCore/QRandomGenerator>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtCore/QtEndian>
#include <QtNetwork/QHostInfo>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

namespace mx {
namespace netpolicy {
namespace relay {

namespace {

QAtomicInt g_made, g_refused;
constexpr qint64 kCap = 512 * 1024;   // per direction in flight: the relay never buffers more

class Session : public QObject {
public:
    Session(QTcpSocket *client, const QByteArray &relayPass, QObject *parent)
        : QObject(parent), m_client(client), m_relayPass(relayPass)
    {
        m_client->setParent(this);
        m_client->setReadBufferSize(kCap);
        QObject::connect(m_client, &QTcpSocket::readyRead, this, [this] { onClient(); });
        QObject::connect(m_client, &QTcpSocket::bytesWritten, this, [this] { pump(); });
        QObject::connect(m_client, &QTcpSocket::disconnected, this, [this] { m_clientGone = true; pump(); finishIfDone(); });
        QTimer::singleShot(30000, this, [this] { if (!m_up) deleteLater(); });   // handshake time out
    }

private:
    enum Stage { Greeting, Auth, Request, Resolving, Connecting, Piping };

    void reply(char code)
    {
        static const char tail[] = {1, 0, 0, 0, 0, 0, 0};   // ATYP IPv4 0.0.0.0:0
        QByteArray r;
        r.append(char(5)).append(code).append(char(0)).append(tail, sizeof tail);
        m_client->write(r);
    }
    void refuse(char code)
    {
        g_refused.ref();
        reply(code);
        m_client->disconnectFromHost();
        deleteLater();
    }

    void onClient()
    {
        if (m_stage == Piping) { pump(); return; }
        m_buf += m_client->readAll();
        if (m_stage == Greeting) {
            if (m_buf.size() < 2) return;
            const int n = quint8(m_buf[1]);
            if (m_buf.size() < 2 + n) return;
            const bool v5 = quint8(m_buf[0]) == 5;
            const bool userPass = m_buf.mid(2, n).contains(char(2));
            m_buf.remove(0, 2 + n);
            if (!v5 || !userPass) {
                m_client->write(QByteArray("\x05\xff", 2));
                m_client->disconnectFromHost();
                deleteLater();
                return;
            }
            m_client->write(QByteArray("\x05\x02", 2));
            m_stage = Auth;
        }
        if (m_stage == Auth) {
            if (m_buf.size() < 2) return;
            const int ul = quint8(m_buf[1]);
            if (m_buf.size() < 2 + ul + 1) return;
            const int pl = quint8(m_buf[2 + ul]);
            if (m_buf.size() < 3 + ul + pl) return;
            const QByteArray user = m_buf.mid(2, ul), pass = m_buf.mid(3 + ul, pl);
            m_buf.remove(0, 3 + ul + pl);
            if (pass != m_relayPass) {
                m_client->write(QByteArray("\x01\x01", 2));
                m_client->disconnectFromHost();
                deleteLater();
                return;
            }
            // user name = "<6|4>|<adapter>"
            const QString u = QString::fromUtf8(user);
            m_ipv6 = !u.startsWith(QLatin1Char('4'));
            m_bind = u.mid(2);
            m_client->write(QByteArray("\x01\x00", 2));
            m_stage = Request;
        }
        if (m_stage == Request) {
            if (m_buf.size() < 5) return;
            if (quint8(m_buf[0]) != 5 || quint8(m_buf[1]) != 1) { refuse(7); return; }   // CONNECT only
            const int atyp = quint8(m_buf[3]);
            int need = 0;
            if (atyp == 1) need = 4 + 4 + 2;
            else if (atyp == 4) need = 4 + 16 + 2;
            else if (atyp == 3) need = 5 + quint8(m_buf[4]) + 2;
            else { refuse(8); return; }
            if (m_buf.size() < need) return;
            if (atyp == 1) {
                m_target = QHostAddress(qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(m_buf.constData() + 4))).toString();
            } else if (atyp == 4) {
                Q_IPV6ADDR a;
                memcpy(a.c, m_buf.constData() + 4, 16);
                m_target = QHostAddress(a).toString();
            } else {
                m_target = QString::fromUtf8(m_buf.mid(5, quint8(m_buf[4])));
            }
            m_port = qFromBigEndian<quint16>(reinterpret_cast<const uchar *>(m_buf.constData() + need - 2));
            m_buf.remove(0, need);
            resolve();
        }
    }

    void resolve()
    {
        m_stage = Resolving;
        QHostAddress lit;
        if (lit.setAddress(m_target)) {
            connectTo({lit});
            return;
        }
        QPointer<Session> self(this);
        QHostInfo::lookupHost(m_target, this, [self](const QHostInfo &hi) {
            if (!self) return;
            if (hi.error() != QHostInfo::NoError || hi.addresses().isEmpty()) { self->refuse(4); return; }
            self->connectTo(hi.addresses());
        });
    }

    void connectTo(const QList<QHostAddress> &candidates)
    {
        const Adapter a = resolveAdapter(m_bind);
        if (!a.present) { refuse(3); return; }   // network unreachable: never another adapter
        QHostAddress target, local;
        for (const QHostAddress &c : candidates) {
            if (c.protocol() == QAbstractSocket::IPv6Protocol && !m_ipv6) continue;
            const QHostAddress l = m_bind.isEmpty() ? QHostAddress()
                                   : (c.protocol() == QAbstractSocket::IPv6Protocol ? a.v6() : a.v4());
            if (!m_bind.isEmpty() && l.isNull()) continue;
            target = c;
            local = l;
            break;
        }
        if (target.isNull()) { refuse(4); return; }
        m_stage = Connecting;
        m_up = new QTcpSocket(this);
        m_up->setProxy(QNetworkProxy::NoProxy);
        m_up->setReadBufferSize(kCap);
        if (!local.isNull() && !m_up->bind(local, 0)) { refuse(3); return; }
        QObject::connect(m_up, &QTcpSocket::connected, this, [this] {
            g_made.ref();
            reply(0);
            m_stage = Piping;
            if (!m_buf.isEmpty()) { m_up->write(m_buf); m_buf.clear(); }
            pump();
        });
        QObject::connect(m_up, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError e) {
            if (m_stage == Connecting) { refuse(e == QAbstractSocket::ConnectionRefusedError ? 5 : 4); return; }
        });
        QObject::connect(m_up, &QTcpSocket::readyRead, this, [this] { pump(); });
        QObject::connect(m_up, &QTcpSocket::bytesWritten, this, [this] { pump(); });
        QObject::connect(m_up, &QTcpSocket::disconnected, this, [this] { m_upGone = true; pump(); finishIfDone(); });
        m_up->connectToHost(target, m_port);
    }

    void pump()
    {
        if (m_stage != Piping || !m_up) return;
        while (m_client->bytesAvailable() > 0 && m_up->bytesToWrite() < kCap && !m_upGone)
            m_up->write(m_client->read(64 * 1024));
        while (m_up->bytesAvailable() > 0 && m_client->bytesToWrite() < kCap && !m_clientGone)
            m_client->write(m_up->read(64 * 1024));
        finishIfDone();
    }

    void finishIfDone()
    {
        if (m_stage != Piping) {
            if (m_clientGone) deleteLater();
            return;
        }
        if (m_upGone && m_up->bytesAvailable() == 0 && !m_clientGone) {
            m_client->disconnectFromHost();   // flushes what is still to be written
            m_clientGone = m_client->state() == QAbstractSocket::UnconnectedState;
        }
        if (m_clientGone && m_client->bytesAvailable() == 0 && !m_upGone) {
            m_up->disconnectFromHost();
            m_upGone = m_up->state() == QAbstractSocket::UnconnectedState;
        }
        if (m_clientGone && m_upGone) deleteLater();
    }

    QTcpSocket *m_client;
    QTcpSocket *m_up = nullptr;
    QByteArray m_relayPass, m_buf;
    Stage m_stage = Greeting;
    QString m_bind, m_target;
    quint16 m_port = 0;
    bool m_ipv6 = true, m_clientGone = false, m_upGone = false;
};

class Server : public QTcpServer {
public:
    explicit Server(const QByteArray &relayPass) : m_relayPass(relayPass) {}
protected:
    void incomingConnection(qintptr fd) override
    {
        auto *s = new QTcpSocket;
        s->setSocketDescriptor(fd);
        if (s->peerAddress().isLoopback()) new Session(s, m_relayPass, this);
        else delete s;
    }
private:
    QByteArray m_relayPass;
};

struct Host {
    QThread thread;
    Server *server = nullptr;
    quint16 port = 0;
    QByteArray relayPass;
};

QMutex g_hostMutex;
Host *g_host = nullptr;

void stopHost()
{
    QMutexLocker l(&g_hostMutex);
    if (!g_host) return;
    Server *srv = g_host->server;
    if (srv) QMetaObject::invokeMethod(srv, [srv] { delete srv; }, Qt::BlockingQueuedConnection);
    g_host->thread.quit();
    g_host->thread.wait(3000);
    delete g_host;
    g_host = nullptr;
}

Host *host()
{
    QMutexLocker l(&g_hostMutex);
    if (g_host) return g_host;
    auto *h = new Host;
    for (int i = 0; i < 24; ++i) h->relayPass.append(char('a' + QRandomGenerator::system()->bounded(26)));
    h->thread.setObjectName(QStringLiteral("mx.netpolicy.relay"));
    h->thread.start();
    auto *srv = new Server(h->relayPass);
    srv->moveToThread(&h->thread);
    quint16 port = 0;
    QMetaObject::invokeMethod(srv, [srv, &port] {
        if (srv->listen(QHostAddress::LocalHost, 0)) port = srv->serverPort();
    }, Qt::BlockingQueuedConnection);
    h->server = srv;
    h->port = port;
    g_host = h;
    qAddPostRoutine(stopHost);
    return h;
}

} // namespace

QNetworkProxy proxyFor(const QString &bind, bool ipv6)
{
    Host *h = host();
    QNetworkProxy p(QNetworkProxy::Socks5Proxy, QStringLiteral("127.0.0.1"), h->port);
    p.setUser(QStringLiteral("%1|%2").arg(ipv6 ? QLatin1Char('6') : QLatin1Char('4')).arg(bind));
    p.setPassword(QString::fromLatin1(h->relayPass));
    QNetworkProxy::Capabilities caps = p.capabilities();
    caps.setFlag(QNetworkProxy::HostNameLookupCapability, true);   // the relay looks names up (system DNS)
    caps.setFlag(QNetworkProxy::UdpTunnelingCapability, false);
    caps.setFlag(QNetworkProxy::ListeningCapability, false);
    p.setCapabilities(caps);
    return p;
}

int connectionsMade() { return g_made.loadRelaxed(); }
int connectionsRefused() { return g_refused.loadRelaxed(); }

} // namespace relay
} // namespace netpolicy
} // namespace mx
