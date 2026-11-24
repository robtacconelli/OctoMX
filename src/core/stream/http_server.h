// SPDX-License-Identifier: GPL-3.0-or-later
// [str-1] OctoMX core: a small HTTP/1.1 server for the local streaming API (Qt only, main thread,
// non-blocking). One request at a time per connection (keep-alive, pipelined requests wait), bodies
// by Content-Length only, every response with a Content-Length. A response is either complete at
// once, deferred (polled until it is ready: a stream whose size is not known yet) or a body that is
// pulled while the socket has room (HttpBody: the bytes of a stream as they arrive).
// Limits so that broken or slow clients cannot hang the program: request head and body sizes,
// connections in all and per address, a time to send the head, an idle time between requests and
// a time a client may leave the data we send unread.
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QPair>
#include <QtCore/QPointer>
#include <QtNetwork/QHostAddress>

#include <functional>
#include <memory>

class QTcpServer;
class QTcpSocket;
class QTimer;

namespace mx::stream {

struct HttpRequest {
    QByteArray method;            // "GET"
    QByteArray target;            // "/api/v1/status?x=1"
    QByteArray path;              // "/api/v1/status" (percent-decoded)
    QByteArray query;             // "x=1"
    QByteArray version;           // "HTTP/1.1"
    QHash<QByteArray, QByteArray> headers;   // lower-case names
    QByteArray body;
    QHostAddress peer;
    QHostAddress local;
    QByteArray header(const char *name) const { return headers.value(QByteArray(name)); }
};

// The body of a response, pulled while the socket has room.
class HttpBody {
public:
    virtual ~HttpBody() = default;
    // Append at most `max` bytes to `out`. Returns the bytes appended (> 0), 0 = nothing right now
    // (asked again soon), -1 = the body is complete, -2 = give up (the connection is closed).
    virtual qint64 produce(QByteArray &out, qint64 max) = 0;
};

struct HttpResponse {
    int status = 200;
    QList<QPair<QByteArray, QByteArray>> headers;
    QByteArray body;                         // when no stream body
    std::unique_ptr<HttpBody> stream;        // pulled; its length is streamLength
    qint64 streamLength = 0;
    bool close = false;                      // close the connection after this response
    // not ready yet: polled (about every 100 ms) until it returns true, then the response is sent
    std::function<bool(HttpResponse &)> deferred;
    void setHeader(const QByteArray &name, const QByteArray &value);
};

class HttpServer : public QObject {
    Q_OBJECT
public:
    struct Limits {
        int maxConnections = 32;
        int maxPerAddress = 16;
        int headBytes = 8192;
        int bodyBytes = 64 * 1024;
        int headTimeoutMs = 10000;   // the request head must arrive within this
        int idleTimeoutMs = 30000;   // keep-alive between requests
        int writeStallMs = 300000;   // the client reads nothing of what we send for this long
    };
    using Handler = std::function<void(const HttpRequest &, HttpResponse &)>;

    explicit HttpServer(QObject *parent = nullptr);
    ~HttpServer() override;

    // Listen on every address (one socket each) at `port` (0 = any free port, the same for all).
    // A failure on any address closes the others: *error says why in plain words.
    bool listen(const QList<QHostAddress> &addresses, quint16 port, QString *error, bool *inUse = nullptr);
    void close();                    // stop listening and close every connection
    bool isListening() const { return !m_servers.isEmpty(); }
    quint16 port() const { return m_port; }
    QList<QHostAddress> addresses() const { return m_addresses; }
    int connectionCount() const { return int(m_conns.size()); }
    void setHandler(Handler h) { m_handler = std::move(h); }
    void setLimits(const Limits &l) { m_limits = l; }
    const Limits &limits() const { return m_limits; }
    static QByteArray reasonPhrase(int status);
    static QByteArray percentDecode(const QByteArray &in, bool *ok);

signals:
    void connectionsChanged();

private:
    class Conn;
    void onNewConnection();
    void dropConn(Conn *c);
    QList<QTcpServer *> m_servers;
    QList<QHostAddress> m_addresses;
    quint16 m_port = 0;
    QList<Conn *> m_conns;
    Handler m_handler;
    Limits m_limits;
};

} // namespace mx::stream
