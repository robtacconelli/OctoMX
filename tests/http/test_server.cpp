// SPDX-License-Identifier: GPL-3.0-or-later
// Test HTTP server. Paths:
//   /f/<name>?<opts>          generated file (deterministic content, seed from <name>)
//   /redir/<n>/<rest>         302 chain of n hops (relative Location) ending at /<rest>
//   /loop/a, /loop/b          redirect loop
//   /status/<code>            empty response with that status
//   /abs?to=<url>             302 to an absolute URL
// Options for /f/:
//   size=N (default 1 MiB)   noranges=1   nohead=1   noetag=1   etag=<value>   lm=1 (Last-Modified)
//   changeafter=K  (content + ETag change after K GETs)    wrongrange=1   rate=<bytes/s>
//   stallonce=B   (first GET stalls after B body bytes)    resetonce=B (first GET: reset after B bytes)
//   chunked=1     nolength=1 (200 without length, close-delimited)
//   busy=N&retryafter=S (first N requests answer 503 / 429 with s429=1)    maxconn=K (503 above K GETs)
//   cd=<percent-encoded Content-Disposition value>  ctype=<type>  link=1  digest=1  torrent=1
#include "test_server.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

namespace mxtest {

static inline quint64 splitmix(quint64 x)
{
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

void generate(quint64 seed, qint64 offset, char *out, qint64 n)
{
    qint64 i = 0;
    while (i < n) {
        const qint64 pos = offset + i;
        const quint64 w = splitmix(seed ^ quint64(pos >> 3));
        int b = int(pos & 7);
        for (; b < 8 && i < n; ++b, ++i) out[i] = char((w >> (8 * b)) & 0xff);
    }
}

QByteArray generated(quint64 seed, qint64 offset, qint64 n)
{
    QByteArray a(int(n), Qt::Uninitialized);
    generate(seed, offset, a.data(), n);
    return a;
}

quint64 seedFor(const QByteArray &name, int version)
{
    return qHash(name, 1234) * 31 + quint64(version) * 0x1000193ULL;
}

static const QByteArray kTorrent = "d8:announce30:udp://tracker.example.org:13374:infod6:lengthi5e4:name5:a.txt12:piece lengthi16384e6:pieces20:aaaaaaaaaaaaaaaaaaaaee";

// ---------------------------------------------------------------- one connection
class Conn : public QObject {
public:
    Conn(QTcpSocket *s, TestServer *srv) : QObject(s), m_s(s), m_srv(srv)
    {
        connect(s, &QTcpSocket::readyRead, this, [this] { onRead(); });
        connect(s, &QTcpSocket::bytesWritten, this, [this] { feed(); });
        connect(s, &QTcpSocket::disconnected, this, [this] { endBody(); m_s->deleteLater(); });
        m_timer.setInterval(50);
        connect(&m_timer, &QTimer::timeout, this, [this] { m_budget += m_rate / 20; feed(); });
    }

private:
    void onRead()
    {
        m_in += m_s->readAll();
        while (!m_inBody) {
            const int end = m_in.indexOf("\r\n\r\n");
            if (end < 0) return;
            const QByteArray head = m_in.left(end);
            m_in.remove(0, end + 4);
            handle(head);
        }
    }

    void sendHead(int code, const QByteArray &reason, const QList<QPair<QByteArray, QByteArray>> &hdrs)
    {
        QByteArray out = "HTTP/1.1 " + QByteArray::number(code) + ' ' + reason + "\r\n";
        for (const auto &h : hdrs) out += h.first + ": " + h.second + "\r\n";
        out += "\r\n";
        m_s->write(out);
    }
    void simple(int code, const QByteArray &reason, QList<QPair<QByteArray, QByteArray>> hdrs = {}, const QByteArray &body = {})
    {
        hdrs << qMakePair(QByteArray("Content-Length"), QByteArray::number(body.size()));
        sendHead(code, reason, hdrs);
        if (!m_head && !body.isEmpty()) m_s->write(body);
    }

    void handle(const QByteArray &head)
    {
        const QList<QByteArray> lines = head.split('\n');
        const QList<QByteArray> rl = lines.value(0).trimmed().split(' ');
        const QByteArray method = rl.value(0);
        const QByteArray target = rl.value(1);
        QHash<QByteArray, QByteArray> h;
        for (int i = 1; i < lines.size(); ++i) {
            const int c = lines[i].indexOf(':');
            if (c > 0) h.insert(lines[i].left(c).trimmed().toLower(), lines[i].mid(c + 1).trimmed());
        }
        m_head = method == "HEAD";
        m_srv->record({method, target, h.value("range"), h.value("if-range"), h.value("authorization"), h.value("user-agent")});
        const QUrl url(QString::fromLatin1("http://x" + target));
        const QByteArray path = url.path(QUrl::FullyEncoded).toLatin1();
        const QUrlQuery q(url);
        const QByteArray query = url.query(QUrl::FullyEncoded).toLatin1();
        const QByteArray qs = query.isEmpty() ? QByteArray() : "?" + query;

        if (path.startsWith("/redir/")) {
            const QList<QByteArray> parts = path.mid(7).split('/');
            const int n = parts.value(0).toInt();
            const QByteArray rest = path.mid(7 + parts.value(0).size() + 1);
            // n > 1: path-absolute relative Location; last hop: absolute URL
            const QByteArray loc = n > 1 ? "/redir/" + QByteArray::number(n - 1) + "/" + rest + qs
                                         : "http://127.0.0.1:" + QByteArray::number(m_srv->port()) + "/" + rest + qs;
            simple(302, "Found", {{"Location", loc}});
            return;
        }
        if (path == "/utf") { simple(302, "Found", {{"Location", QByteArray("/f/datei mit \xc3\xbc.bin") + qs}}); return; }
        if (path == "/loop/a") { simple(302, "Found", {{"Location", "/loop/b"}}); return; }
        if (path == "/loop/b") { simple(301, "Moved", {{"Location", "http://127.0.0.1:" + QByteArray::number(m_srv->port()) + "/loop/a"}}); return; }
        if (path.startsWith("/status/")) {
            const int code = path.mid(8).toInt();
            simple(code, "Status", {}, method == "HEAD" ? QByteArray() : QByteArray("status page"));
            return;
        }
        if (path == "/abs") { simple(302, "Found", {{"Location", q.queryItemValue("to", QUrl::FullyEncoded).toLatin1()}}); return; }
        if (path.startsWith("/f/")) { serveFile(method, path, q, h); return; }
        simple(404, "Not Found");
    }

    void serveFile(const QByteArray &method, const QByteArray &path, const QUrlQuery &q, const QHash<QByteArray, QByteArray> &h)
    {
        auto opt = [&q](const char *k) { return q.queryItemValue(QLatin1String(k)).toLatin1(); };
        auto num = [&](const char *k, qint64 def) { const QByteArray v = opt(k); return v.isEmpty() ? def : v.toLongLong(); };
        const QByteArray key = path + "?" + q.query(QUrl::FullyEncoded).toLatin1();
        const int nthReq = m_srv->bumpPath("all:" + key);
        const qint64 busy = num("busy", 0);
        if (nthReq <= busy) {
            QList<QPair<QByteArray, QByteArray>> hd;
            if (!opt("retryafter").isEmpty()) hd << qMakePair(QByteArray("Retry-After"), opt("retryafter"));
            if (opt("s429") == "1") simple(429, "Too Many Requests", hd, "busy");
            else simple(503, "Service Unavailable", hd, "busy");
            return;
        }
        if (method == "HEAD" && opt("nohead") == "1") { simple(405, "Method Not Allowed"); return; }

        const bool torrent = opt("torrent") == "1";
        const qint64 size = torrent ? kTorrent.size() : num("size", 1 << 20);
        int version = 0;
        int gets = method == "GET" ? m_srv->bumpPath("get:" + key) : m_srv->pathCount("get:" + key);
        const qint64 changeAfter = num("changeafter", 0);
        if (changeAfter > 0 && gets > changeAfter) version = 1;
        const quint64 seed = seedFor(path, version);
        QByteArray etag = opt("etag").isEmpty() ? "\"v" + QByteArray::number(version) + "-" + QByteArray::number(size) + "\"" : opt("etag");
        if (opt("noetag") == "1") etag.clear();
        const bool liar = opt("liar") == "1";            // advertises ranges, ignores them
        const bool ranges = opt("noranges") != "1";

        if (const qint64 maxconn = num("maxconn", 0); maxconn > 0 && method == "GET") {
            if (m_srv->pathCount("active:" + path) >= maxconn) { simple(503, "Service Unavailable", {}, "too many connections"); return; }
        }

        QList<QPair<QByteArray, QByteArray>> hd;
        hd << qMakePair(QByteArray("Content-Type"), opt("ctype").isEmpty() ? QByteArray("application/octet-stream") : opt("ctype"));
        if (!etag.isEmpty()) hd << qMakePair(QByteArray("ETag"), etag);
        if (opt("lm") == "1") hd << qMakePair(QByteArray("Last-Modified"), version ? QByteArray("Tue, 02 Jan 2024 00:00:00 GMT") : QByteArray("Mon, 01 Jan 2024 00:00:00 GMT"));
        if (ranges) hd << qMakePair(QByteArray("Accept-Ranges"), QByteArray("bytes"));
        if (!opt("cd").isEmpty()) hd << qMakePair(QByteArray("Content-Disposition"), QByteArray::fromPercentEncoding(opt("cd")));
        if (opt("link") == "1") {
            const QByteArray b = "http://127.0.0.1:" + QByteArray::number(m_srv->port());
            hd << qMakePair(QByteArray("Link"), "<" + b + "/f/mirror1.bin>; rel=duplicate; pri=1; geo=de, <" + b
                                                    + "/f/x.meta4>; rel=describedby; type=\"application/metalink4+xml\", <" + b
                                                    + "/f/mirror2.bin>; rel=\"duplicate\"; pri=2");
        }
        if (opt("digest") == "1") {
            const QByteArray all = torrent ? kTorrent : generated(seed, 0, size);
            hd << qMakePair(QByteArray("Digest"), "SHA-256=" + QCryptographicHash::hash(all, QCryptographicHash::Sha256).toBase64()
                                                      + ",MD5=" + QCryptographicHash::hash(all, QCryptographicHash::Md5).toBase64());
            hd << qMakePair(QByteArray("X-Checksum-Sha1"), QCryptographicHash::hash(all, QCryptographicHash::Sha1).toHex());
        }

        qint64 first = 0, last = size - 1;
        int code = 200;
        const QByteArray range = h.value("range");
        bool honour = ranges && !liar && range.startsWith("bytes=") && method == "GET";
        if (honour && h.contains("if-range") && h.value("if-range") != etag && !h.value("if-range").contains("GMT")) honour = false;
        if (honour) {
            const QList<QByteArray> se = range.mid(6).split('-');
            first = se.value(0).toLongLong();
            last = se.value(1).isEmpty() ? size - 1 : qMin(size - 1, se.value(1).toLongLong());
            if (first >= size) {
                simple(416, "Range Not Satisfiable", {{"Content-Range", "bytes */" + QByteArray::number(size)}});
                return;
            }
            code = 206;
            qint64 shownFirst = first;
            if (opt("wrongrange") == "1") { first += 1000; shownFirst = first; last = qMin(size - 1, last + 1000); }
            hd << qMakePair(QByteArray("Content-Range"), "bytes " + QByteArray::number(shownFirst) + "-" + QByteArray::number(last) + "/" + QByteArray::number(size));
        }
        const bool chunked = opt("chunked") == "1";
        const bool nolength = opt("nolength") == "1";
        if (chunked) hd << qMakePair(QByteArray("Transfer-Encoding"), QByteArray("chunked"));
        else if (nolength && code == 200) hd << qMakePair(QByteArray("Connection"), QByteArray("close"));
        else hd << qMakePair(QByteArray("Content-Length"), QByteArray::number(last - first + 1));
        sendHead(code, code == 206 ? "Partial Content" : "OK", hd);
        if (method == "HEAD") return;

        m_seed = seed;
        m_torrent = torrent;
        m_pos = first;
        m_end = last + 1;
        m_chunked = chunked;
        m_closeAtEnd = nolength && code == 200 && !chunked;
        m_sent = 0;
        m_stallAt = -1;
        m_resetAt = -1;
        m_activeKey = "active:" + path;
        m_srv->bumpPath(m_activeKey);
        m_srv->activeDelta(1);
        const int getNo = m_srv->pathCount("get:" + key);
        if (num("stallonce", -1) >= 0 && getNo == 1) m_stallAt = num("stallonce", -1);
        if (num("resetonce", -1) >= 0 && getNo == 1) m_resetAt = num("resetonce", -1);
        m_rate = num("rate", 0);
        m_budget = m_rate / 20;
        if (m_rate > 0) m_timer.start();
        m_inBody = true;
        feed();
    }

    void endBody()
    {
        if (!m_inBody) return;
        m_inBody = false;
        m_timer.stop();
        m_srv->bumpPath(m_activeKey + "-done");
        m_srv->activeDelta(-1);
        m_srv->bumpPath(m_activeKey + "#dec");
    }

    void feed()
    {
        if (!m_inBody) return;
        while (m_pos < m_end && m_s->bytesToWrite() < 256 * 1024) {
            qint64 n = qMin<qint64>(64 * 1024, m_end - m_pos);
            if (m_rate > 0) { n = qMin(n, m_budget); if (n <= 0) return; }
            if (m_stallAt >= 0) { n = qMin(n, m_stallAt - m_sent); if (n <= 0) return; }   // stall: just stop sending
            if (m_resetAt >= 0 && m_sent + n >= m_resetAt) {
                n = m_resetAt - m_sent;
                if (n > 0) writeBody(n);
                m_s->flush();
                m_s->abort();
                return;
            }
            writeBody(n);
            if (m_rate > 0) m_budget -= n;
        }
        if (m_pos >= m_end) {
            if (m_chunked) m_s->write("0\r\n\r\n");
            const bool close = m_closeAtEnd;
            endBody();
            if (close) m_s->disconnectFromHost();
            else if (!m_in.isEmpty()) QTimer::singleShot(0, this, [this] { onRead(); });
        }
    }

    void writeBody(qint64 n)
    {
        QByteArray b = m_torrent ? kTorrent.mid(int(m_pos), int(n)) : generated(m_seed, m_pos, n);
        if (m_chunked) m_s->write(QByteArray::number(b.size(), 16) + "\r\n" + b + "\r\n");
        else m_s->write(b);
        m_pos += n;
        m_sent += n;
    }

    QTcpSocket *m_s;
    TestServer *m_srv;
    QByteArray m_in;
    bool m_head = false, m_inBody = false, m_chunked = false, m_closeAtEnd = false, m_torrent = false;
    quint64 m_seed = 0;
    qint64 m_pos = 0, m_end = 0, m_sent = 0, m_stallAt = -1, m_resetAt = -1, m_rate = 0, m_budget = 0;
    QByteArray m_activeKey;
    QTimer m_timer;
};

// ---------------------------------------------------------------- server
TestServer::TestServer()
{
    start();
    m_ready.acquire();
}

TestServer::~TestServer()
{
    quit();
    wait();
}

void TestServer::run()
{
    QTcpServer server;
    server.listen(QHostAddress::LocalHost, 0);
    m_port = server.serverPort();
    QObject::connect(&server, &QTcpServer::newConnection, &server, [this, &server] {
        while (QTcpSocket *s = server.nextPendingConnection()) new Conn(s, this);
    });
    m_ready.release();
    exec();
}

void TestServer::record(const RequestLog &r)
{
    QMutexLocker l(&m_mutex);
    m_log << r;
}

int TestServer::bumpPath(const QByteArray &key)
{
    QMutexLocker l(&m_mutex);
    if (key.endsWith("#dec")) {
        const QByteArray k = key.left(key.size() - 4);
        return --m_counts[k];
    }
    return ++m_counts[key];
}

int TestServer::pathCount(const QByteArray &key) const
{
    QMutexLocker l(&m_mutex);
    return m_counts.value(key);
}

void TestServer::activeDelta(int d)
{
    QMutexLocker l(&m_mutex);
    m_active += d;
    m_maxActive = qMax(m_maxActive, m_active);
}

int TestServer::count(const QByteArray &method, const QByteArray &pathPrefix) const
{
    QMutexLocker l(&m_mutex);
    int n = 0;
    for (const RequestLog &r : m_log)
        if (r.method == method && r.target.startsWith(pathPrefix)) ++n;
    return n;
}

int TestServer::maxConcurrent() const
{
    QMutexLocker l(&m_mutex);
    return m_maxActive;
}

QList<RequestLog> TestServer::log() const
{
    QMutexLocker l(&m_mutex);
    return m_log;
}

void TestServer::reset()
{
    QMutexLocker l(&m_mutex);
    m_log.clear();
    m_counts.clear();
    m_active = m_maxActive = 0;
}

} // namespace mxtest
