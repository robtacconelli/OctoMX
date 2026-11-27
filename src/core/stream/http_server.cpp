// SPDX-License-Identifier: GPL-3.0-or-later
// [str-1] OctoMX core: the small HTTP/1.1 server of the streaming API (see http_server.h).
#include "core/stream/http_server.h"

#include "core/log.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QTimer>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

namespace mx::stream {

void HttpResponse::setHeader(const QByteArray &name, const QByteArray &value)
{
    for (auto &h : headers)
        if (h.first.compare(name, Qt::CaseInsensitive) == 0) { h.second = value; return; }
    headers.append(qMakePair(name, value));
}

QByteArray HttpServer::reasonPhrase(int status)
{
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 410: return "Gone";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 415: return "Unsupported Media Type";
    case 416: return "Range Not Satisfiable";
    case 422: return "Unprocessable Content";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    default: return "Status";
    }
}

QByteArray HttpServer::percentDecode(const QByteArray &in, bool *ok)
{
    QByteArray out;
    out.reserve(in.size());
    *ok = true;
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (int i = 0; i < in.size(); ++i) {
        const char c = in.at(i);
        if (c != '%') { out += c; continue; }
        if (i + 2 >= in.size() || hex(in.at(i + 1)) < 0 || hex(in.at(i + 2)) < 0) { *ok = false; return QByteArray(); }
        const char v = char(hex(in.at(i + 1)) * 16 + hex(in.at(i + 2)));
        if (v == 0) { *ok = false; return QByteArray(); }
        out += v;
        i += 2;
    }
    return out;
}

// ------------------------------------------------------------------------------------------ Conn

class HttpServer::Conn : public QObject {
public:
    Conn(HttpServer *srv, QTcpSocket *s) : QObject(srv), m_srv(srv), m_sock(s)
    {
        m_peer = s->peerAddress();
        m_timer = new QTimer(this);
        m_timer->setInterval(50);
        connect(m_timer, &QTimer::timeout, this, &Conn::onTimer);
        connect(s, &QTcpSocket::readyRead, this, &Conn::onRead);
        connect(s, &QTcpSocket::bytesWritten, this, [this](qint64) {
            m_lastWriteMs = m_clock.elapsed();
            pump();
        });
        connect(s, &QTcpSocket::disconnected, this, [this] { m_srv->dropConn(this); });
        connect(s, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
            if (m_sock->state() != QAbstractSocket::ConnectedState) m_srv->dropConn(this);
        });
        m_clock.start();
        m_headStartMs = 0;
        m_idleSinceMs = 0;
        m_timer->start();
        onRead();
    }
    ~Conn() override
    {
        m_resp.reset();
        if (m_sock) {
            m_sock->disconnect(this);
            m_sock->abort();
            m_sock->deleteLater();
        }
    }
    QHostAddress peer() const { return m_peer; }
    void abortNow()
    {
        if (m_sock) m_sock->abort();
    }

private:
    enum Phase { ReadHead, ReadBody, Deferred, Sending, Closing };

    void onRead()
    {
        if (!m_sock) return;
        if (m_phase == Closing) {   // what still arrives is read and dropped (no reset before our answer is read)
            m_sock->readAll();
            return;
        }
        if (m_phase == ReadHead || m_phase == ReadBody) {
            m_in += m_sock->readAll();
        } else {
            // a pipelined request waits; but nothing unbounded is kept
            if (m_in.size() < m_srv->m_limits.headBytes + m_srv->m_limits.bodyBytes) m_in += m_sock->read(16384);
            return;
        }
        parse();
    }

    void parse()
    {
        if (m_phase == ReadHead) {
            if (m_in.isEmpty()) return;
            if (m_headStartMs == 0) m_headStartMs = m_clock.elapsed() + 1;
            int end = m_in.indexOf("\r\n\r\n");
            int sep = 4;
            if (end < 0) {
                end = m_in.indexOf("\n\n");
                sep = 2;
            }
            if (end < 0) {
                if (m_in.size() > m_srv->m_limits.headBytes) fail(431, "The request head is too large.");
                return;
            }
            if (end > m_srv->m_limits.headBytes) { fail(431, "The request head is too large."); return; }
            const QByteArray head = m_in.left(end);
            m_in.remove(0, end + sep);
            if (!parseHead(head)) return;
            const QByteArray te = m_req.header("transfer-encoding");
            if (!te.isEmpty()) { fail(411, "Chunked request bodies are not accepted: send a Content-Length."); return; }
            const QByteArray cl = m_req.header("content-length");
            m_bodyLen = 0;
            if (!cl.isEmpty()) {
                bool ok = false;
                m_bodyLen = cl.trimmed().toLongLong(&ok);
                if (!ok || m_bodyLen < 0) { fail(400, "Bad Content-Length."); return; }
                if (m_bodyLen > m_srv->m_limits.bodyBytes) { fail(413, "The request body is too large."); return; }
            }
            m_phase = ReadBody;
        }
        if (m_phase == ReadBody) {
            if (m_in.size() < m_bodyLen) return;
            m_req.body = m_in.left(int(m_bodyLen));
            m_in.remove(0, int(m_bodyLen));
            dispatch();
        }
    }

    bool parseHead(const QByteArray &head)
    {
        m_req = HttpRequest();
        m_req.peer = m_peer;
        m_req.local = m_sock->localAddress();
        QList<QByteArray> lines = head.split('\n');
        for (QByteArray &l : lines)
            if (l.endsWith('\r')) l.chop(1);
        const QList<QByteArray> rl = lines.value(0).split(' ');
        if (rl.size() != 3 || rl.at(0).isEmpty() || rl.at(1).isEmpty()) { fail(400, "Bad request line."); return false; }
        m_req.method = rl.at(0);
        m_req.target = rl.at(1);
        m_req.version = rl.at(2);
        if (m_req.version != "HTTP/1.1" && m_req.version != "HTTP/1.0") { fail(505, "Only HTTP/1.1 and HTTP/1.0."); return false; }
        if (lines.size() > 101) { fail(431, "Too many header lines."); return false; }
        for (int i = 1; i < lines.size(); ++i) {
            const QByteArray &l = lines.at(i);
            if (l.isEmpty()) continue;
            const int c = l.indexOf(':');
            if (c <= 0 || l.at(0) == ' ' || l.at(0) == '\t') { fail(400, "Bad header line."); return false; }
            const QByteArray name = l.left(c).trimmed().toLower();
            const QByteArray value = l.mid(c + 1).trimmed();
            if (name.isEmpty() || name.contains(' ')) { fail(400, "Bad header name."); return false; }
            if (m_req.headers.contains(name)) {
                // a second Host or Content-Length is a request smuggling attempt
                if (name == "host" || name == "content-length") { fail(400, "Repeated header."); return false; }
                m_req.headers[name] += ", " + value;
            } else {
                m_req.headers.insert(name, value);
            }
        }
        if (!m_req.target.startsWith('/')) { fail(400, "Only origin-form request targets."); return false; }
        const int q = m_req.target.indexOf('?');
        bool ok = true;
        m_req.path = percentDecode(q >= 0 ? m_req.target.left(q) : m_req.target, &ok);
        m_req.query = q >= 0 ? m_req.target.mid(q + 1) : QByteArray();
        if (!ok) { fail(400, "Bad percent encoding."); return false; }
        if (m_req.path.contains("/../") || m_req.path.endsWith("/..")) { fail(400, "Bad path."); return false; }
        return true;
    }

    void dispatch()
    {
        m_headStartMs = 0;
        const QByteArray conn = m_req.header("connection").toLower();
        m_keepAlive = m_req.version == "HTTP/1.1" ? !conn.contains("close") : conn.contains("keep-alive");
        m_resp = std::make_unique<HttpResponse>();
        if (m_srv->m_handler) m_srv->m_handler(m_req, *m_resp);
        else m_resp->status = 503;
        if (m_resp->deferred) {
            m_phase = Deferred;
            if (m_resp->deferred(*m_resp)) {
                m_resp->deferred = nullptr;
                send();
            }
            return;
        }
        send();
    }

    void send()
    {
        HttpResponse &r = *m_resp;
        if (r.close) m_keepAlive = false;
        const bool head = m_req.method == "HEAD";
        QByteArray out = (m_req.version.isEmpty() ? QByteArray("HTTP/1.1") : m_req.version) + ' ' + QByteArray::number(r.status) + ' ' + HttpServer::reasonPhrase(r.status) + "\r\n";
        const qint64 len = r.stream ? r.streamLength : r.body.size();
        bool hasLen = false;
        for (const auto &h : std::as_const(r.headers)) {
            if (h.first.compare("content-length", Qt::CaseInsensitive) == 0) hasLen = true;
            out += h.first + ": " + h.second + "\r\n";
        }
        if (!hasLen) out += "Content-Length: " + QByteArray::number(len) + "\r\n";
        out += m_keepAlive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
        out += "\r\n";
        if (!head && !r.stream) out += r.body;
        m_sock->write(out);
        m_lastWriteMs = m_clock.elapsed();
        if (head || !r.stream || r.streamLength <= 0) {
            r.stream.reset();
            finishResponse();
            return;
        }
        m_left = r.streamLength;
        m_phase = Sending;
        pump();
    }

    void pump()
    {
        if (m_phase != Sending || !m_resp || !m_resp->stream || !m_sock) return;
        constexpr qint64 kHigh = 1 << 20, kChunk = 256 * 1024;
        int guard = 0;
        while (m_left > 0 && m_sock->bytesToWrite() < kHigh && ++guard < 16) {
            QByteArray buf;
            const qint64 n = m_resp->stream->produce(buf, qMin(kChunk, m_left));
            if (n == -2 || (n == -1 && m_left > 0) || !m_resp) {
                m_keepAlive = false;
                closeSoon();
                return;
            }
            if (n <= 0) return;   // nothing right now: the timer asks again
            if (buf.size() > m_left) buf.truncate(int(m_left));
            m_left -= buf.size();
            m_sock->write(buf);
        }
        if (m_left <= 0) {
            m_resp->stream.reset();
            finishResponse();
        }
    }

    void finishResponse()
    {
        m_resp.reset();
        if (!m_keepAlive) {
            closeSoon();
            return;
        }
        m_phase = ReadHead;
        m_idleSinceMs = m_clock.elapsed() + 1;
        if (m_sock->bytesAvailable() > 0) m_in += m_sock->readAll();
        if (!m_in.isEmpty()) QTimer::singleShot(0, this, [this] { parse(); });
    }

    // close once our answer is out, a moment later: what the client still sends is read meanwhile
    void closeSoon()
    {
        m_phase = Closing;
        m_closeAtMs = m_clock.elapsed() + 300;
    }

    void fail(int status, const char *text)
    {
        m_keepAlive = false;
        m_resp = std::make_unique<HttpResponse>();
        m_resp->status = status;
        m_resp->setHeader("Content-Type", "application/json; charset=utf-8");
        m_resp->body = QByteArray("{\"error\":\"bad_request\",\"message\":\"") + text + "\"}\n";
        m_resp->close = true;
        m_in.clear();
        send();
    }

    void onTimer()
    {
        if (!m_sock) return;
        const qint64 now = m_clock.elapsed();
        const Limits &L = m_srv->m_limits;
        switch (m_phase) {
        case ReadHead:
        case ReadBody:
            if (m_headStartMs && now - (m_headStartMs - 1) > L.headTimeoutMs) {
                fail(408, "The request did not arrive in time.");
                return;
            }
            if (!m_headStartMs && m_in.isEmpty() && now - (m_idleSinceMs ? m_idleSinceMs - 1 : 0) > L.idleTimeoutMs) {
                m_phase = Closing;
                m_closeAtMs = now;
            }
            break;
        case Deferred:
            if (m_resp && m_resp->deferred && m_resp->deferred(*m_resp)) {
                m_resp->deferred = nullptr;
                send();
            }
            break;
        case Sending:
            if (m_sock->bytesToWrite() > 0 && now - m_lastWriteMs > L.writeStallMs) {
                qCInfo(lcCore) << "streaming API: a client read nothing for" << L.writeStallMs / 1000 << "s, closed";
                m_srv->dropConn(this);
                return;
            }
            pump();
            break;
        case Closing:
            m_sock->readAll();
            if (m_sock->state() == QAbstractSocket::UnconnectedState || now - m_lastWriteMs > 10000) {
                m_srv->dropConn(this);
                return;
            }
            if (!m_disconnecting && m_sock->bytesToWrite() == 0 && now >= m_closeAtMs) {
                m_disconnecting = true;
                m_sock->disconnectFromHost();
            }
            break;
        }
    }

    HttpServer *m_srv;
    QPointer<QTcpSocket> m_sock;
    QHostAddress m_peer;
    QTimer *m_timer = nullptr;
    QElapsedTimer m_clock;
    Phase m_phase = ReadHead;
    QByteArray m_in;
    qint64 m_bodyLen = 0;
    qint64 m_headStartMs = 0;    // +1 (0 = no head started)
    qint64 m_idleSinceMs = 0;    // +1
    qint64 m_lastWriteMs = 0;
    qint64 m_left = 0;
    qint64 m_closeAtMs = 0;
    bool m_disconnecting = false;
    bool m_keepAlive = true;
    HttpRequest m_req;
    std::unique_ptr<HttpResponse> m_resp;
};

// ------------------------------------------------------------------------------------------ server

HttpServer::HttpServer(QObject *parent) : QObject(parent) {}

HttpServer::~HttpServer() { close(); }

bool HttpServer::listen(const QList<QHostAddress> &addresses, quint16 port, QString *error, bool *inUse)
{
    close();
    if (inUse) *inUse = false;
    quint16 p = port;
    for (const QHostAddress &a : addresses) {
        auto *s = new QTcpServer(this);
        s->setMaxPendingConnections(16);
        if (!s->listen(a, p)) {
            const bool busy = s->serverError() == QAbstractSocket::AddressInUseError;
            // no IPv6 on this machine: ::1 is skipped, not an error
            if (!busy && a.protocol() == QAbstractSocket::IPv6Protocol && a.isLoopback()) {
                delete s;
                continue;
            }
            if (error)
                *error = busy ? QStringLiteral("Port %1 is in use by another program on %2.").arg(p).arg(a.toString())
                              : QStringLiteral("Cannot listen on %1 port %2: %3").arg(a.toString()).arg(p).arg(s->errorString());
            if (inUse) *inUse = busy;
            delete s;
            close();
            return false;
        }
        if (p == 0) p = s->serverPort();
        connect(s, &QTcpServer::newConnection, this, [this] { onNewConnection(); });
        m_servers << s;
        m_addresses << a;
    }
    if (m_servers.isEmpty()) {
        if (error) *error = QStringLiteral("No address to listen on.");
        return false;
    }
    m_port = p;
    return true;
}

void HttpServer::close()
{
    for (QTcpServer *s : std::as_const(m_servers)) {
        s->close();
        s->deleteLater();
    }
    m_servers.clear();
    m_addresses.clear();
    m_port = 0;
    const QList<Conn *> conns = m_conns;
    m_conns.clear();
    for (Conn *c : conns) delete c;
    if (!conns.isEmpty()) emit connectionsChanged();
}

void HttpServer::onNewConnection()
{
    auto *srv = qobject_cast<QTcpServer *>(sender());
    if (!srv) return;
    while (QTcpSocket *s = srv->nextPendingConnection()) {
        int same = 0;
        for (Conn *c : std::as_const(m_conns))
            if (c->peer().isEqual(s->peerAddress())) ++same;
        if (int(m_conns.size()) >= m_limits.maxConnections || same >= m_limits.maxPerAddress) {
            const QByteArray body = "{\"error\":\"busy\",\"message\":\"Too many connections to the OctoMX API at once.\"}\n";
            s->write("HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json; charset=utf-8\r\nConnection: close\r\n"
                     "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
            connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
            connect(s, &QTcpSocket::readyRead, s, [s] { s->readAll(); });   // read and dropped: no reset before the answer
            QTimer::singleShot(300, s, [s] { s->disconnectFromHost(); });
            QTimer::singleShot(5000, s, [s] { s->abort(); s->deleteLater(); });
            continue;
        }
        s->setParent(nullptr);
        auto *c = new Conn(this, s);
        m_conns << c;
        emit connectionsChanged();
    }
}

void HttpServer::dropConn(Conn *c)
{
    if (!m_conns.removeOne(c)) return;
    c->deleteLater();
    emit connectionsChanged();
}

} // namespace mx::stream
