// SPDX-License-Identifier: GPL-3.0-or-later
// [net-2] Test servers for the connection policy tests (header-only, Qt only):
//  * TestProxy: a SOCKS5 (no auth or user/password) and HTTP CONNECT proxy on 127.0.0.1. It
//    records every target it was asked for and can redirect a target (for example an address of
//    a documentation range that nothing can reach directly, 192.0.2.x) to a local server: a
//    connection that arrives there came through the proxy, a direct attempt cannot arrive.
//    UDP ASSOCIATE is refused (as by many proxies): a network may not then send UDP directly.
//  * Canary: a TCP listener and a UDP socket that record where every connection / datagram came
//    from (the "socket statistics" of the tests: counts per source address).
#pragma once

#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QMutex>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QUrl>
#include <QtCore/QtEndian>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtNetwork/QUdpSocket>

#include <functional>
#include <memory>

namespace nettest {

struct Endpoint {
    QHostAddress addr;
    quint16 port = 0;
};

class TestProxy : public QObject {
public:
    explicit TestProxy(QObject *parent = nullptr) : QObject(parent)
    {
        m_server.setProxy(QNetworkProxy::NoProxy);
        QObject::connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *c = m_server.nextPendingConnection()) start(c);
        });
    }
    bool listen() { return m_server.listen(QHostAddress::LocalHost, 0); }
    quint16 port() const { return m_server.serverPort(); }

    QString user, password;                 // non-empty: SOCKS5 user/password required
    QHash<QString, Endpoint> redirects;     // "host:port" asked for -> where to connect instead
    QStringList targets;                    // every "host:port" asked for, in order
    int udpAssociates = 0;                  // UDP ASSOCIATE requests (always refused)
    int clients = 0;
    int refusedRemote = 0;                  // targets outside this computer (never connected)
    bool stall = false;                     // take the request and never answer (a target that does not answer)
    int openClients = 0, maxOpenClients = 0;   // connections open at the same time
    QList<qint64> closedAfterMs;            // how long each client connection stayed open

    void redirect(const QString &hostPort, const QHostAddress &a, quint16 p) { redirects.insert(hostPort, {a, p}); }
    int countTarget(const QString &prefix) const
    {
        int n = 0;
        for (const QString &t : targets) if (t.startsWith(prefix)) ++n;
        return n;
    }

private:
    struct Conn {
        QPointer<QTcpSocket> c, up;
        QByteArray buf;
        int stage = 0;   // 0 first bytes, 1 socks auth, 2 socks request, 3 piping
        bool http = false;
        bool plainHttp = false;   // a forwarded request, no "200 Connection established"
        QElapsedTimer since;
    };

    void start(QTcpSocket *c)
    {
        ++clients;
        maxOpenClients = qMax(maxOpenClients, ++openClients);
        auto st = std::make_shared<Conn>();
        st->c = c;
        st->since.start();
        c->setParent(this);
        QObject::connect(c, &QTcpSocket::readyRead, this, [this, st] { onData(st); });
        QObject::connect(c, &QTcpSocket::disconnected, this, [this, st] {
            --openClients;
            closedAfterMs << st->since.elapsed();
            if (st->up) st->up->disconnectFromHost();
            if (st->c) st->c->deleteLater();
        });
    }

    void fail(const std::shared_ptr<Conn> &st, const QByteArray &reply)
    {
        if (!st->c) return;
        st->c->write(reply);
        st->c->disconnectFromHost();
    }

    void onData(const std::shared_ptr<Conn> &st)
    {
        if (!st->c) return;
        if (st->stage == 3) {
            if (st->up) st->up->write(st->c->readAll());
            return;
        }
        st->buf += st->c->readAll();
        if (st->stage == 0) {
            if (st->buf.isEmpty()) return;
            if (st->buf.at(0) != char(5)) {   // HTTP CONNECT
                st->http = true;
                const int end = st->buf.indexOf("\r\n\r\n");
                if (end < 0) return;
                const int eol = st->buf.indexOf("\r\n");
                const QList<QByteArray> words = st->buf.left(eol).split(' ');
                if (words.size() >= 3 && words.at(0) != "CONNECT" && words.at(1).startsWith("http://")) {
                    // plain http through an HTTP proxy: "GET http://host:port/path": forward in origin form
                    const QUrl u(QString::fromLatin1(words.at(1)));
                    QByteArray path = u.path(QUrl::FullyEncoded).toLatin1();
                    if (path.isEmpty()) path = "/";
                    if (u.hasQuery()) path += '?' + u.query(QUrl::FullyEncoded).toLatin1();
                    st->buf = words.at(0) + ' ' + path + ' ' + words.at(2) + st->buf.mid(eol);
                    st->http = true;
                    st->plainHttp = true;
                    connectUp(st, u.host(), quint16(u.port(80)));
                    return;
                }
                st->buf.remove(0, end + 4);
                if (words.size() < 2 || words.at(0) != "CONNECT") { fail(st, "HTTP/1.1 405 Method Not Allowed\r\n\r\n"); return; }
                const QString hp = QString::fromLatin1(words.at(1));
                const int colon = hp.lastIndexOf(QLatin1Char(':'));
                connectUp(st, hp.left(colon), quint16(hp.mid(colon + 1).toUInt()));
                return;
            }
            if (st->buf.size() < 2 || st->buf.size() < 2 + quint8(st->buf.at(1))) return;
            const QByteArray methods = st->buf.mid(2, quint8(st->buf.at(1)));
            st->buf.remove(0, 2 + methods.size());
            const char want = user.isEmpty() ? char(0) : char(2);
            if (!methods.contains(want)) { fail(st, QByteArray("\x05\xff", 2)); return; }
            st->c->write(QByteArray("\x05", 1) + QByteArray(1, want));
            st->stage = user.isEmpty() ? 2 : 1;
        }
        if (st->stage == 1) {
            if (st->buf.size() < 2) return;
            const int ul = quint8(st->buf.at(1));
            if (st->buf.size() < 3 + ul) return;
            const int pl = quint8(st->buf.at(2 + ul));
            if (st->buf.size() < 3 + ul + pl) return;
            const bool ok = st->buf.mid(2, ul) == user.toUtf8() && st->buf.mid(3 + ul, pl) == password.toUtf8();
            st->buf.remove(0, 3 + ul + pl);
            if (!ok) { fail(st, QByteArray("\x01\x01", 2)); return; }
            st->c->write(QByteArray("\x01\x00", 2));
            st->stage = 2;
        }
        if (st->stage == 2) {
            if (st->buf.size() < 5) return;
            const int cmd = quint8(st->buf.at(1)), atyp = quint8(st->buf.at(3));
            int need = atyp == 1 ? 10 : atyp == 4 ? 22 : atyp == 3 ? 7 + quint8(st->buf.at(4)) : -1;
            if (need < 0) { fail(st, QByteArray("\x05\x08\x00\x01\0\0\0\0\0\0", 10)); return; }
            if (st->buf.size() < need) return;
            QString host;
            if (atyp == 1) host = QHostAddress(qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(st->buf.constData() + 4))).toString();
            else if (atyp == 4) { Q_IPV6ADDR a; memcpy(a.c, st->buf.constData() + 4, 16); host = QHostAddress(a).toString(); }
            else host = QString::fromUtf8(st->buf.mid(5, quint8(st->buf.at(4))));
            const quint16 port = qFromBigEndian<quint16>(reinterpret_cast<const uchar *>(st->buf.constData() + need - 2));
            st->buf.remove(0, need);
            if (cmd == 3) { ++udpAssociates; fail(st, QByteArray("\x05\x07\x00\x01\0\0\0\0\0\0", 10)); return; }
            if (cmd != 1) { fail(st, QByteArray("\x05\x07\x00\x01\0\0\0\0\0\0", 10)); return; }
            connectUp(st, host, port);
        }
    }

    void connectUp(const std::shared_ptr<Conn> &st, const QString &host, quint16 port)
    {
        const QString key = QStringLiteral("%1:%2").arg(host).arg(port);
        targets << key;
        if (stall) return;
        Endpoint to;
        if (redirects.contains(key)) to = redirects.value(key);
        else { to.addr = QHostAddress(host); to.port = port; }
        // a test never reaches past this computer: anything not redirected must be local
        if (!redirects.contains(key) && !to.addr.isLoopback()) {
            ++refusedRemote;
            fail(st, st->http ? QByteArray("HTTP/1.1 403 Forbidden\r\n\r\n") : QByteArray("\x05\x02\x00\x01\0\0\0\0\0\0", 10));
            return;
        }
        auto *up = new QTcpSocket(this);
        up->setProxy(QNetworkProxy::NoProxy);
        st->up = up;
        QObject::connect(up, &QTcpSocket::connected, this, [st] {
            if (!st->c) return;
            if (!st->plainHttp)
                st->c->write(st->http ? QByteArray("HTTP/1.1 200 Connection established\r\n\r\n")
                                      : QByteArray("\x05\x00\x00\x01\x7f\x00\x00\x01\x00\x00", 10));
            st->stage = 3;
            if (!st->buf.isEmpty()) { st->up->write(st->buf); st->buf.clear(); }
        });
        QObject::connect(up, &QTcpSocket::readyRead, this, [st] { if (st->c && st->up) st->c->write(st->up->readAll()); });
        QObject::connect(up, &QTcpSocket::errorOccurred, this, [this, st](QAbstractSocket::SocketError) {
            if (st->stage != 3) fail(st, st->http ? QByteArray("HTTP/1.1 502 Bad Gateway\r\n\r\n") : QByteArray("\x05\x05\x00\x01\0\0\0\0\0\0", 10));
        });
        QObject::connect(up, &QTcpSocket::disconnected, this, [st] {
            if (st->c) st->c->disconnectFromHost();
            if (st->up) st->up->deleteLater();
        });
        if (to.addr.isNull()) { fail(st, QByteArray("\x05\x04\x00\x01\0\0\0\0\0\0", 10)); return; }
        up->connectToHost(to.addr, to.port);
    }

    QTcpServer m_server;
};

// A listener that records the source of every connection and every datagram.
class Canary : public QObject {
public:
    explicit Canary(QObject *parent = nullptr) : QObject(parent)
    {
        m_tcp.setProxy(QNetworkProxy::NoProxy);
        m_udp.setProxy(QNetworkProxy::NoProxy);
        QObject::connect(&m_tcp, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *s = m_tcp.nextPendingConnection()) {
                tcpFrom << s->peerAddress().toString();
                s->setParent(this);
                QObject::connect(s, &QTcpSocket::readyRead, this, [this, s] {
                    received += s->readAll();
                    if (onData) onData(s);
                });
                QObject::connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
                if (onConnection) onConnection(s);
            }
        });
        QObject::connect(&m_udp, &QUdpSocket::readyRead, this, [this] {
            while (m_udp.hasPendingDatagrams()) {
                QByteArray d(int(qMax<qint64>(0, m_udp.pendingDatagramSize())), Qt::Uninitialized);
                QHostAddress from;
                m_udp.readDatagram(d.data(), d.size(), &from);
                udpFrom << from.toString();
            }
        });
    }
    bool listen(const QHostAddress &on = QHostAddress::LocalHost, quint16 port = 0)
    {
        if (!m_tcp.listen(on, port)) return false;
        return m_udp.bind(on, m_tcp.serverPort());
    }
    quint16 port() const { return m_tcp.serverPort(); }
    QString endpoint() const { return QStringLiteral("%1:%2").arg(m_tcp.serverAddress().toString()).arg(port()); }
    int tcpCount(const QString &from = QString()) const { return from.isEmpty() ? int(tcpFrom.size()) : int(tcpFrom.count(from)); }
    int udpCount(const QString &from = QString()) const { return from.isEmpty() ? int(udpFrom.size()) : int(udpFrom.count(from)); }

    QStringList tcpFrom, udpFrom;
    QByteArray received;
    std::function<void(QTcpSocket *)> onConnection, onData;

private:
    QTcpServer m_tcp;
    QUdpSocket m_udp;
};

} // namespace nettest
