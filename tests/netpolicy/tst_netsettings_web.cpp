// SPDX-License-Identifier: GPL-3.0-or-later
// [net-3] The HTTP and web index settings, each changed and observed at a local server: what
// arrives (requests, their user agent, how many at the same time, when a retry comes), what the
// plugin makes of the answer (a time out, a self-signed certificate refused or accepted, an https
// to http redirect refused or followed, the number of results, adult words, the trackers of the
// magnet links) and what happens at start (the index checks). The */enabled keys with the real
// plugins inside the core: off, nothing reaches the server.
// The HTTPS server is made here: a certificate and key created with OpenSSL at run time (nothing in
// the tree), TLS over memory BIOs on a QTcpSocket, so it works with any Qt TLS backend on the client.
#include "http_plugin.h"
#include "http_util.h"
#include "test_server.h"
#include "web_plugin.h"
#include "web_search_plugin.h"

#include "core/network_manager.h"
#include "mx/services.h"
#include "mx/settings_keys.h"
#include "mx/simple_settings.h"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QPointer>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtNetwork/QSslSocket>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtTest/QSignalSpy>
#include <QtTest/QtTest>

#include <functional>
#include <memory>

using namespace mx;

namespace {

class NoLimiter : public RateLimiter {
public:
    qint64 acquire(Dir, qint64 bytes) override { return bytes; }
    void account(Dir, Net, qint64) override {}
    int limitBps(Dir) const override { return 0; }
};

QByteArray body(qint64 off, qint64 n)
{
    QByteArray b(int(n), Qt::Uninitialized);
    for (qint64 i = 0; i < n; ++i) b[int(i)] = char(((off + i) * 7 + 3) & 0xFF);
    return b;
}

// one HTTP request as a server saw it
struct Seen {
    QByteArray method, path, query, head;
    qint64 atMs = 0;
    QByteArray header(const QByteArray &name) const
    {
        for (const QByteArray &l : head.split('\n'))
            if (l.toLower().startsWith(name.toLower() + ':')) return l.mid(name.size() + 1).trimmed();
        return {};
    }
};

// takes one request head from a byte stream (GET / HEAD only, no bodies)
bool takeRequest(QByteArray *buf, Seen *out, qint64 now)
{
    const int end = buf->indexOf("\r\n\r\n");
    if (end < 0) return false;
    out->head = buf->left(end);
    buf->remove(0, end + 4);
    const QList<QByteArray> first = out->head.split('\n').first().trimmed().split(' ');
    out->method = first.value(0);
    const QByteArray target = first.value(1);
    const int q = target.indexOf('?');
    out->path = q < 0 ? target : target.left(q);
    out->query = q < 0 ? QByteArray() : target.mid(q + 1);
    out->atMs = now;
    return true;
}

QByteArray response(int status, const QList<QPair<QByteArray, QByteArray>> &headers, const QByteArray &content, bool head = false)
{
    QByteArray r = "HTTP/1.1 " + QByteArray::number(status) + " X\r\n";
    bool haveLength = false;
    for (const auto &h : headers) {
        r += h.first + ": " + h.second + "\r\n";
        haveLength |= h.first.toLower() == "content-length";
    }
    if (!haveLength) r += "Content-Length: " + QByteArray::number(content.size()) + "\r\n";
    r += "Connection: close\r\n\r\n";
    if (!head) r += content;
    return r;
}

// a file of `size` bytes with ranges: HEAD, GET, GET with "Range: bytes=a-b"
QByteArray fileAnswer(const Seen &s, qint64 size)
{
    QList<QPair<QByteArray, QByteArray>> h{{"Content-Type", "application/octet-stream"}, {"Accept-Ranges", "bytes"}, {"ETag", "\"n3-file\""}};
    const QByteArray range = s.header("Range");
    if (range.startsWith("bytes=") && s.method == "GET") {
        const QByteArray spec = range.mid(6);
        const int dash = spec.indexOf('-');
        const qint64 a = spec.left(dash).toLongLong();
        const QByteArray last = spec.mid(dash + 1);
        const qint64 b = qMin(size - 1, last.isEmpty() ? size - 1 : last.toLongLong());
        h << qMakePair(QByteArray("Content-Range"), "bytes " + QByteArray::number(a) + "-" + QByteArray::number(b) + "/" + QByteArray::number(size));
        return response(206, h, body(a, b - a + 1));
    }
    h << qMakePair(QByteArray("Content-Length"), QByteArray::number(size));
    const bool head = s.method == "HEAD";
    return response(200, h, head ? QByteArray() : body(0, size), head);
}

// A plain HTTP server whose answers the test scripts: `answer` returns the bytes to send (after
// delayMs), or nothing: no answer at all (the connection stays open and silent).
class ScriptServer : public QObject {
public:
    struct Reply { QByteArray bytes; int delayMs = 0; };
    std::function<Reply(const Seen &)> answer;
    QList<Seen> seen;

    ScriptServer()
    {
        m_clock.start();
        m_srv.listen(QHostAddress::LocalHost, 0);
        connect(&m_srv, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *s = m_srv.nextPendingConnection()) {
                auto buf = std::make_shared<QByteArray>();
                connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
                connect(s, &QTcpSocket::readyRead, s, [this, s, buf] {
                    buf->append(s->readAll());
                    Seen r;
                    while (takeRequest(buf.get(), &r, m_clock.elapsed())) {
                        seen << r;
                        const Reply rep = answer ? answer(r) : Reply{response(404, {}, "none"), 0};
                        if (rep.bytes.isEmpty()) continue;
                        QPointer<QTcpSocket> ps(s);
                        QTimer::singleShot(rep.delayMs, s, [ps, rep] {
                            if (!ps) return;
                            ps->write(rep.bytes);
                            ps->disconnectFromHost();
                        });
                    }
                });
            }
        });
    }
    quint16 port() const { return m_srv.serverPort(); }
    QString url(const QString &pathAndQuery) const { return QStringLiteral("http://127.0.0.1:%1%2").arg(port()).arg(pathAndQuery); }
    QList<Seen> of(const QByteArray &method, const QByteArray &path) const
    {
        QList<Seen> out;
        for (const Seen &s : seen)
            if ((method.isEmpty() || s.method == method) && s.path == path) out << s;
        return out;
    }

private:
    QTcpServer m_srv;
    QElapsedTimer m_clock;
};

// ------------------------------------------------------------------------------- HTTPS test server
// a self-signed certificate for 127.0.0.1 made now (EC P-256), TLS over memory BIOs
class TlsServer : public QObject {
public:
    std::function<QByteArray(const Seen &)> answer;
    QList<Seen> seen;
    int handshakes = 0;          // TLS handshakes completed
    int handshakeFailures = 0;   // refused by the client (our certificate) or broken off

    TlsServer()
    {
        m_srv.listen(QHostAddress::LocalHost, 0);
        EVP_PKEY *pkey = EVP_EC_gen("P-256");
        X509 *x = X509_new();
        if (pkey && x) {
            X509_set_version(x, 2);
            ASN1_INTEGER_set(X509_get_serialNumber(x), 3);
            X509_gmtime_adj(X509_getm_notBefore(x), -3600L);
            X509_gmtime_adj(X509_getm_notAfter(x), 86400L);
            X509_set_pubkey(x, pkey);
            X509_NAME *n = X509_get_subject_name(x);
            X509_NAME_add_entry_by_txt(n, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char *>("127.0.0.1"), -1, -1, 0);
            X509_set_issuer_name(x, n);
            if (X509_sign(x, pkey, EVP_sha256()) > 0) {
                m_ctx = SSL_CTX_new(TLS_server_method());
                if (m_ctx && SSL_CTX_use_certificate(m_ctx, x) == 1 && SSL_CTX_use_PrivateKey(m_ctx, pkey) == 1) m_ok = true;
            }
        }
        X509_free(x);
        EVP_PKEY_free(pkey);
        connect(&m_srv, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *s = m_srv.nextPendingConnection()) accept(s);
        });
    }
    ~TlsServer() override
    {
        for (auto &c : m_conns) SSL_free(c->ssl);   // frees its BIOs too
        m_conns.clear();
        if (m_ctx) SSL_CTX_free(m_ctx);
    }
    bool ok() const { return m_ok && m_srv.isListening(); }
    quint16 port() const { return m_srv.serverPort(); }
    QString url(const QString &path) const { return QStringLiteral("https://127.0.0.1:%1%2").arg(port()).arg(path); }

private:
    struct Conn {
        QPointer<QTcpSocket> sock;
        SSL *ssl = nullptr;
        BIO *in = nullptr, *out = nullptr;
        QByteArray plain;
        bool counted = false, closed = false;
    };
    void accept(QTcpSocket *s)
    {
        auto c = std::make_shared<Conn>();
        c->sock = s;
        c->ssl = SSL_new(m_ctx);
        c->in = BIO_new(BIO_s_mem());
        c->out = BIO_new(BIO_s_mem());
        SSL_set_bio(c->ssl, c->in, c->out);
        SSL_set_accept_state(c->ssl);
        m_conns << c;
        connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
        connect(s, &QTcpSocket::readyRead, this, [this, c] { pump(c); });
    }
    void flush(const std::shared_ptr<Conn> &c)
    {
        char buf[16384];
        int n;
        while ((n = BIO_read(c->out, buf, int(sizeof buf))) > 0)
            if (c->sock) c->sock->write(buf, n);
    }
    void pump(const std::shared_ptr<Conn> &c)
    {
        if (!c->sock || c->closed) return;
        const QByteArray raw = c->sock->readAll();
        BIO_write(c->in, raw.constData(), int(raw.size()));
        if (!SSL_is_init_finished(c->ssl)) {
            const int r = SSL_do_handshake(c->ssl);
            flush(c);
            if (r <= 0) {
                const int e = SSL_get_error(c->ssl, r);
                ERR_clear_error();
                if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) return;
                ++handshakeFailures;
                c->closed = true;
                c->sock->disconnectFromHost();
                return;
            }
        }
        if (!c->counted) {
            c->counted = true;
            ++handshakes;
        }
        char buf[16384];
        int n;
        while ((n = SSL_read(c->ssl, buf, int(sizeof buf))) > 0) c->plain.append(buf, n);
        ERR_clear_error();
        Seen r;
        if (!takeRequest(&c->plain, &r, 0)) {
            flush(c);
            return;
        }
        seen << r;
        const QByteArray rep = answer ? answer(r) : response(404, {}, "none");
        SSL_write(c->ssl, rep.constData(), int(rep.size()));
        SSL_shutdown(c->ssl);
        flush(c);
        c->closed = true;
        c->sock->disconnectFromHost();
    }

    QTcpServer m_srv;
    SSL_CTX *m_ctx = nullptr;
    bool m_ok = false;
    QList<std::shared_ptr<Conn>> m_conns;
};

// the HTTP plugin as the engine uses it, with the test's settings
struct HttpEnv {
    SimpleSettings settings;
    NoLimiter limiter;
    std::unique_ptr<NetworkPlugin> plugin;
    HttpEnv()
    {
        settings.setValue(keys::http::MaxConnectionsPerServer, 16);
        plugin.reset(createHttpPlugin());
        PluginContext ctx;
        ctx.settings = &settings;
        ctx.limiter = &limiter;
        plugin->init(ctx);
        plugin->start();
    }
    std::unique_ptr<RangeSource> open(const QString &url, qint64 size)
    {
        SearchSource ss;
        ss.net = Net::Http;
        ss.uri = url;
        std::unique_ptr<RangeSource> s(plugin->openSource(QStringLiteral("n3"), {}, size, ss));
        if (s) s->open();
        return s;
    }
};

bool settled(RangeSource *s) { return s->state() == RangeSource::Ready || s->state() == RangeSource::Failed; }

struct Finished {
    explicit Finished(RangeSource *s)
    {
        QObject::connect(s, &RangeSource::data, s, [this](qint64, const QByteArray &b) { data += b; });
        QObject::connect(s, &RangeSource::requestFinished, s, [this](qint64, qint64, bool ok, const QString &err) {
            (ok ? okCount : failCount)++;
            errors << err;
        });
    }
    QByteArray data;
    int okCount = 0, failCount = 0;
    QStringList errors;
};

// ------------------------------------------------------------------------------- web index fixture
QByteArray hashOf(const QByteArray &seed) { return QCryptographicHash::hash(seed, QCryptographicHash::Sha1).toHex(); }

// one JSON index at the script server, nothing else (no index on the internet)
QByteArray providerDefs(const ScriptServer &srv, int timeoutMs = 60000)
{
    return QStringLiteral(R"({"providers":[{"id":"n3idx","name":"Test index","request":{"url":"%1/json?q={query}","probeQuery":"debian"},)"
                          R"("limits":{"minIntervalMs":0,"timeoutMs":%2},)"
                          R"("response":{"format":"json","fields":{"name":"name","infoHash":"info_hash","seeders":"seeders","size":"size"}}}]})")
        .arg(srv.url(QString()))
        .arg(timeoutMs)
        .toUtf8();
}

QByteArray records(const QStringList &names)
{
    QByteArray out = "[";
    for (int i = 0; i < names.size(); ++i) {
        if (i) out += ',';
        out += "{\"name\":\"" + names[i].toUtf8() + "\",\"info_hash\":\"" + hashOf(names[i].toUtf8()) + "\",\"seeders\":\""
               + QByteArray::number(100 - i) + "\",\"size\":\"1000\"}";
    }
    return out + "]";
}

ScriptServer::Reply jsonReply(const QByteArray &json, int delayMs = 0)
{
    return ScriptServer::Reply{response(200, {{"Content-Type", "application/json"}}, json), delayMs};
}

struct WebEnv {
    QTemporaryDir dir;
    SimpleSettings settings;
    web::WebSearchPlugin plugin;
    explicit WebEnv(const ScriptServer &srv, bool start = true)
    {
        settings.setValue(web::wkeys::ProbeOnStart, false);
        settings.setValue(keys::search::TimeoutSec, 120);
        plugin.setBuiltinProviders(providerDefs(srv));
        PluginContext ctx;
        ctx.settings = &settings;
        ctx.dataDir = dir.path();
        plugin.init(ctx);
        if (start) plugin.start();
    }
};

struct SearchRun {
    QList<SearchHit> hits;
    bool done = false;
    qint64 ms = 0;
};

SearchRun runSearch(NetworkPlugin *p, quint32 id, const QString &text, int waitMs = 15000)
{
    SearchRun r;
    QElapsedTimer t;
    t.start();
    QObject guard;
    QObject::connect(p, &NetworkPlugin::searchHits, &guard, [&r, id](quint32 q, const QList<SearchHit> &h) {
        if (q == id) r.hits << h;
    });
    QObject::connect(p, &NetworkPlugin::searchDone, &guard, [&r, id, &t](quint32 q) {
        if (q == id && !r.done) {
            r.done = true;
            r.ms = t.elapsed();
        }
    });
    SearchQuery q;
    q.id = id;
    q.text = text;
    p->search(q);
    (void)QTest::qWaitFor([&] { return r.done; }, waitMs);
    return r;
}

QStringList hitNames(const QList<SearchHit> &hits)
{
    QStringList n;
    for (const SearchHit &h : hits)
        if (!n.contains(h.fileName)) n << h.fileName;
    n.sort();
    return n;
}

QString magnetOf(const SearchHit &h)
{
    for (const SearchSource &s : h.sources)
        if (s.uri.startsWith(QLatin1String("magnet:"))) return s.uri;
    return QString();
}

QString enc(const QString &s) { return QString::fromLatin1(QUrl::toPercentEncoding(s)); }

} // namespace

class TstNetSettingsWeb : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { registerMetaTypes(); }
    void httpEnabledInTheCore();
    void httpTimeoutAndRetryDelay();
    void httpPartsPerSource();
    void httpCertificatesAndRedirects();
    void webEnabledInTheNetworkManager();
    void webResultsAdultWordsAndTrackers();
    void webUserAgentAndSearchTimeout();
    void webIndexCheckAtStart();
};

// ------------------------------------------------------------------------------------------- HTTP
// http/enabled: the core's download engine uses the HTTP plugin only while it is on. Off: a pasted
// web address gets no request at all; on: the same core downloads it.
void TstNetSettingsWeb::httpEnabledInTheCore()
{
    ScriptServer srv;
    srv.answer = [](const Seen &s) { return ScriptServer::Reply{fileAnswer(s, 20000), 0}; };
    QTemporaryDir dir;
    const QString profile = dir.filePath(QStringLiteral("core"));
    NetworkPlugin *http = createHttpPlugin();
    Core *core = createRealCore(profile, {http});
    Settings *s = core->settings();
    s->setValue(keys::connection::Upnp, false);
    s->setValue(keys::connection::NatPmp, false);
    s->setValue(keys::library::WatchFolders, false);
    s->setValue(QString(keys::transfers::IncomingDir), profile + QStringLiteral("/incoming"));
    s->setValue(QString(keys::http::Enabled), false);   // "http/enabled"
    core->start();
    DownloadRequest off;
    off.uri = srv.url(QStringLiteral("/off/bunny-trailer.bin"));
    const QString idOff = core->transfers()->download(off);
    QVERIFY(!idOff.isEmpty());
    QTest::qWait(4000);
    QCOMPARE(srv.of(QByteArray(), "/off/bunny-trailer.bin").size(), 0);   // nothing asked while off
    QVERIFY(core->transfers()->transfer(idOff).state != TransferState::Completed);

    s->setValue(QString(keys::http::Enabled), true);
    QTRY_COMPARE_WITH_TIMEOUT(http->status().state, ConnState::Connected, 5000);
    DownloadRequest on;
    on.uri = srv.url(QStringLiteral("/on/bunny-poster.bin"));
    const QString idOn = core->transfers()->download(on);
    QTRY_COMPARE_WITH_TIMEOUT(core->transfers()->transfer(idOn).state, TransferState::Completed, 30000);
    QVERIFY(!srv.of(QByteArray(), "/on/bunny-poster.bin").isEmpty());
    QFile f(core->transfers()->transfer(idOn).savePath);
    QVERIFY2(f.open(QIODevice::ReadOnly), qPrintable(f.fileName()));
    QCOMPARE(f.readAll(), body(0, 20000));
    f.close();
    // off again: the plugin is stopped
    s->setValue(QString(keys::http::Enabled), false);
    QTRY_COMPARE_WITH_TIMEOUT(http->status().state, ConnState::Disconnected, 5000);
    core->shutdown();
    delete core;
}

// http/timeoutSec: a server that never answers a range request is given up on (and asked again)
// after that many seconds; http/retryBaseMs: the wait before the first retry
void TstNetSettingsWeb::httpTimeoutAndRetryDelay()
{
    ScriptServer srv;
    srv.answer = [](const Seen &s) -> ScriptServer::Reply {
        if (s.method == "HEAD") return {fileAnswer(s, 100000), 0};
        if (s.path == "/silent.bin") return {QByteArray(), 0};   // the headers never come
        return {response(500, {}, "broken"), 0};                 // /broken.bin: an error every time
    };
    auto getsOf = [&](const QByteArray &path) {
        QList<qint64> t;
        for (const Seen &x : srv.of("GET", path)) t << x.atMs;
        return t;
    };
    HttpEnv env;
    env.settings.setValue(QLatin1String(httpkeys::MaxRetries), 1);
    // 5 s (the smallest value of the page): the request is asked again after 5 s and the retry delay
    env.settings.setValue(keys::http::TimeoutSec, 5);                      // "http/timeoutSec"
    env.settings.setValue(QLatin1String(httpkeys::RetryBaseMs), 200);      // "http/retryBaseMs"
    {
        auto src = env.open(srv.url(QStringLiteral("/silent.bin")), 100000);
        QVERIFY(src);
        QTRY_VERIFY_WITH_TIMEOUT(settled(src.get()), 5000);
        QCOMPARE(src->state(), RangeSource::Ready);
        const int before = getsOf("/silent.bin").size();   // a GET of the probe, if any
        Finished fin(src.get());
        src->request(0, 50000);
        QTRY_COMPARE_WITH_TIMEOUT(getsOf("/silent.bin").size(), before + 2, 12000);
        const QList<qint64> g = getsOf("/silent.bin");
        const qint64 gap = g.last() - g.at(g.size() - 2);
        qInfo("timeoutSec 5: asked again after %lld ms", gap);
        QVERIFY2(gap >= 4700 && gap <= 9000, qPrintable(QString::number(gap)));
        QTRY_COMPARE_WITH_TIMEOUT(fin.failCount, 1, 9000);   // silent again: given up (maxRetries 1)
        QVERIFY2(fin.errors.first().contains(QLatin1String("within 5 s")), qPrintable(fin.errors.first()));
        src->close();
    }
    // 12 s: after 8 s of silence the request is still waiting, nothing asked again
    env.settings.setValue(keys::http::TimeoutSec, 12);
    {
        auto src = env.open(srv.url(QStringLiteral("/silent.bin?b=2")), 100000);
        QTRY_VERIFY_WITH_TIMEOUT(settled(src.get()), 5000);
        const int before = getsOf("/silent.bin").size();
        Finished fin(src.get());
        src->request(0, 50000);
        QTRY_COMPARE_WITH_TIMEOUT(getsOf("/silent.bin").size(), before + 1, 3000);
        QTest::qWait(8000);
        QCOMPARE(getsOf("/silent.bin").size(), before + 1);
        QCOMPARE(fin.failCount, 0);
        src->close();
    }
    // the retry delay: a request answered with an error is asked again after retryBaseMs
    env.settings.setValue(keys::http::TimeoutSec, 5);
    for (const int base : {300, 2500}) {
        env.settings.setValue(QLatin1String(httpkeys::RetryBaseMs), base);
        srv.seen.clear();
        auto src = env.open(srv.url(QStringLiteral("/broken.bin")), 100000);
        QTRY_VERIFY_WITH_TIMEOUT(settled(src.get()), 5000);
        QCOMPARE(src->state(), RangeSource::Ready);
        const int before = getsOf("/broken.bin").size();
        Finished fin(src.get());
        src->request(0, 50000);
        QTRY_COMPARE_WITH_TIMEOUT(getsOf("/broken.bin").size(), before + 2, base + 5000);
        const QList<qint64> g = getsOf("/broken.bin");
        const qint64 gap = g.last() - g.at(g.size() - 2);
        qInfo("retryBaseMs %d: asked again after %lld ms", base, gap);
        QVERIFY2(gap >= base - 50 && gap <= base + 2000, qPrintable(QString::number(gap)));
        QTRY_COMPARE_WITH_TIMEOUT(fin.failCount, 1, 5000);
        src->close();
    }
}

// http/connectionsPerSource: parts of one file fetched from one web source at the same time,
// counted at the server
void TstNetSettingsWeb::httpPartsPerSource()
{
    mxtest::TestServer srv;
    HttpEnv env;
    for (const int n : {2, 5}) {
        srv.reset();
        env.settings.setValue(QLatin1String(httpkeys::ConnectionsPerSource), n);   // "http/connectionsPerSource"
        const QString u = QString::fromLatin1(srv.base()) + QStringLiteral("/f/parts%1.bin?size=3000000&rate=600000").arg(n);
        auto src = env.open(u, 3000000);
        QTRY_VERIFY_WITH_TIMEOUT(settled(src.get()), 5000);
        QCOMPARE(src->state(), RangeSource::Ready);
        QCOMPARE(src->maxPendingRequests(), n);
        Finished fin(src.get());
        for (int i = 0; i < 5; ++i) src->request(qint64(i) * 600000, 600000);
        QTRY_COMPARE_WITH_TIMEOUT(fin.okCount + fin.failCount, 5, 30000);
        QCOMPARE(fin.okCount, n);          // the parts beyond the limit are refused at once
        QCOMPARE(srv.maxConcurrent(), n);  // and the server saw exactly that many at the same time
        if (n < 5) QVERIFY(fin.errors.join(QLatin1Char(' ')).contains(QStringLiteral("max %1").arg(n)));
        src->close();
    }
}

// http/verifyTls: an https server with a self-signed certificate is refused while it is on, used
// while it is off; http/allowInsecureRedirect: an https address that sends us on to plain http
void TstNetSettingsWeb::httpCertificatesAndRedirects()
{
    if (!QSslSocket::supportsSsl()) QSKIP("no TLS support in this Qt build: the https checks need it");
    TlsServer tls;
    QVERIFY(tls.ok());
    ScriptServer plain;
    plain.answer = [](const Seen &s) { return ScriptServer::Reply{fileAnswer(s, 4000), 0}; };
    const QString plainUrl = plain.url(QStringLiteral("/plain/after-redirect.bin"));
    tls.answer = [plainUrl](const Seen &s) {
        if (s.path == "/redirect.bin") return response(302, {{"Location", plainUrl.toUtf8()}}, QByteArray(), s.method == "HEAD");
        return fileAnswer(s, 4000);
    };
    HttpEnv env;
    env.settings.setValue(QLatin1String(httpkeys::RetryBaseMs), 100);
    env.settings.setValue(QLatin1String(httpkeys::MaxRetries), 0);
    // on (the default): the handshake is refused, no request is sent, the source fails
    env.settings.setValue(keys::http::VerifyTls, true);   // "http/verifyTls"
    {
        auto src = env.open(tls.url(QStringLiteral("/selfsigned.bin")), 4000);
        QTRY_VERIFY_WITH_TIMEOUT(src->state() == RangeSource::Failed, 15000);
        qInfo("verifyTls on: %s", qPrintable(src->stateText()));
        QVERIFY(tls.seen.isEmpty());   // no request reached it
        // [net-3] Qt's OpenSSL backend aborts in the handshake; the Windows backend (Schannel, also under
        // wine) finishes it and then refuses the certificate: no request either way
        QVERIFY(tls.handshakes <= 1);
    }
    // off: the same server is used, the data arrives
    env.settings.setValue(keys::http::VerifyTls, false);
    {
        auto src = env.open(tls.url(QStringLiteral("/selfsigned.bin")), 4000);
        QTRY_VERIFY_WITH_TIMEOUT(settled(src.get()), 15000);
        QVERIFY2(src->state() == RangeSource::Ready, qPrintable(src->stateText()));
        Finished fin(src.get());
        src->request(0, 4000);
        QTRY_COMPARE_WITH_TIMEOUT(fin.okCount, 1, 10000);
        QCOMPARE(fin.data, body(0, 4000));
        QVERIFY(tls.handshakes >= 1);
        QVERIFY(!tls.seen.isEmpty());
        src->close();
    }
    // https to http refused (the default): the plain server is never asked
    env.settings.setValue(QLatin1String(httpkeys::AllowInsecureRedirect), false);   // "http/allowInsecureRedirect"
    {
        auto src = env.open(tls.url(QStringLiteral("/redirect.bin")), 4000);
        QTRY_VERIFY_WITH_TIMEOUT(src->state() == RangeSource::Failed, 15000);
        QVERIFY2(src->stateText().contains(QLatin1String("insecure"), Qt::CaseInsensitive), qPrintable(src->stateText()));
        QVERIFY(plain.seen.isEmpty());
    }
    // allowed: followed, the file comes from the plain server
    env.settings.setValue(QLatin1String(httpkeys::AllowInsecureRedirect), true);
    {
        auto src = env.open(tls.url(QStringLiteral("/redirect.bin")), 4000);
        QTRY_VERIFY_WITH_TIMEOUT(settled(src.get()), 15000);
        QVERIFY2(src->state() == RangeSource::Ready, qPrintable(src->stateText()));
        Finished fin(src.get());
        src->request(0, 4000);
        QTRY_COMPARE_WITH_TIMEOUT(fin.okCount, 1, 10000);
        QCOMPARE(fin.data, body(0, 4000));
        QVERIFY(!plain.of(QByteArray(), "/plain/after-redirect.bin").isEmpty());
        src->close();
    }
}

// ------------------------------------------------------------------------------------------- web
// web/enabled: the NetworkManager runs the web plugin only while it is on; off, a search asks no
// index at all
void TstNetSettingsWeb::webEnabledInTheNetworkManager()
{
    ScriptServer srv;
    srv.answer = [](const Seen &) { return jsonReply(records({QStringLiteral("Big Buck Bunny 1080p")})); };
    QTemporaryDir dir;
    SimpleSettings settings;
    settings.setValue(web::wkeys::ProbeOnStart, false);
    settings.setValue(keys::search::TimeoutSec, 60);
    settings.setValue(keys::web::Enabled, false);   // "web/enabled"
    auto *plugin = new web::WebSearchPlugin;
    plugin->setBuiltinProviders(providerDefs(srv));
    NetworkManager net(&settings, {plugin});
    net.initPlugins(dir.path(), nullptr, nullptr);
    net.startNetworks(true);
    QCOMPARE(plugin->status().state, ConnState::Disconnected);
    SearchRun off = runSearch(plugin, 1, QStringLiteral("big buck bunny"), 3000);
    QVERIFY(off.done);
    QVERIFY(off.hits.isEmpty());
    QVERIFY(srv.seen.isEmpty());
    settings.setValue(keys::web::Enabled, true);
    QTRY_VERIFY_WITH_TIMEOUT(plugin->status().state != ConnState::Disconnected, 3000);
    SearchRun on = runSearch(plugin, 2, QStringLiteral("big buck bunny"));
    QVERIFY(on.done);
    QCOMPARE(hitNames(on.hits), QStringList{QStringLiteral("Big Buck Bunny 1080p")});
    QCOMPARE(srv.of("GET", "/json").size(), 1);
    settings.setValue(keys::web::Enabled, false);
    QTRY_COMPARE_WITH_TIMEOUT(plugin->status().state, ConnState::Disconnected, 3000);
    srv.seen.clear();
    SearchRun again = runSearch(plugin, 3, QStringLiteral("big buck bunny"), 3000);
    QVERIFY(again.hits.isEmpty());
    QVERIFY(srv.seen.isEmpty());
}

// web/maxResults: results per search at most; web/adultKeywords: the words "Filter adult content"
// drops (empty: the built-in list); web/trackers: the trackers of the results' magnet links (empty:
// bt/extraTrackers)
void TstNetSettingsWeb::webResultsAdultWordsAndTrackers()
{
    QStringList many;
    for (int i = 1; i <= 30; ++i) many << QStringLiteral("Big Buck Bunny part %1").arg(i);
    ScriptServer srv;
    QByteArray current;
    srv.answer = [&current](const Seen &) { return jsonReply(current); };
    WebEnv env(srv);
    quint32 id = 10;
    // maxResults
    current = records(many);
    env.settings.setValue(keys::web::MaxResults, 10);   // "web/maxResults"
    SearchRun r = runSearch(&env.plugin, ++id, QStringLiteral("big buck bunny"));
    QVERIFY(r.done);
    QCOMPARE(hitNames(r.hits).size(), 10);
    env.settings.setValue(keys::web::MaxResults, 200);
    r = runSearch(&env.plugin, ++id, QStringLiteral("big buck bunny"));
    QCOMPARE(hitNames(r.hits).size(), 30);
    // adult words, with "Filter adult content" on
    current = records({QStringLiteral("Big Buck Bunny 1080p"), QStringLiteral("Big Buck Bunny XXX"), QStringLiteral("Big Buck Bunny grownup cut")});
    env.settings.setValue(keys::search::AdultFilter, true);
    env.settings.setValue(web::wkeys::AdultKeywords, QStringList());   // "web/adultKeywords" empty: the built-in list
    r = runSearch(&env.plugin, ++id, QStringLiteral("big buck bunny"));
    QCOMPARE(hitNames(r.hits), (QStringList{QStringLiteral("Big Buck Bunny 1080p"), QStringLiteral("Big Buck Bunny grownup cut")}));
    env.settings.setValue(web::wkeys::AdultKeywords, QStringList{QStringLiteral("grownup")});   // the user's words instead
    r = runSearch(&env.plugin, ++id, QStringLiteral("big buck bunny"));
    QCOMPARE(hitNames(r.hits), (QStringList{QStringLiteral("Big Buck Bunny 1080p"), QStringLiteral("Big Buck Bunny XXX")}));
    env.settings.setValue(keys::search::AdultFilter, false);
    r = runSearch(&env.plugin, ++id, QStringLiteral("big buck bunny"));
    QCOMPARE(hitNames(r.hits).size(), 3);
    // trackers of the magnet links
    current = records({QStringLiteral("Big Buck Bunny 1080p")});
    const QString own = QStringLiteral("udp://198.51.100.7:6969/announce");
    const QString extra = QStringLiteral("udp://192.0.2.9:1337/announce");
    env.settings.setValue(keys::bt::ExtraTrackers, QStringList{extra});
    env.settings.setValue(web::wkeys::Trackers, QStringList{own});   // "web/trackers"
    r = runSearch(&env.plugin, ++id, QStringLiteral("big buck bunny"));
    QVERIFY(!r.hits.isEmpty());
    QString m = magnetOf(r.hits.first());
    QVERIFY2(m.contains(QStringLiteral("&tr=") + enc(own)), qPrintable(m));
    QVERIFY2(!m.contains(enc(extra)), qPrintable(m));
    env.settings.setValue(web::wkeys::Trackers, QStringList());   // empty: the BitTorrent page's extra trackers
    r = runSearch(&env.plugin, ++id, QStringLiteral("big buck bunny"));
    QVERIFY(!r.hits.isEmpty());
    m = magnetOf(r.hits.first());
    QVERIFY2(m.contains(QStringLiteral("&tr=") + enc(extra)), qPrintable(m));
    QVERIFY2(!m.contains(enc(own)), qPrintable(m));
}

// web/userAgent: the program name the index sees (empty: the built-in browser one);
// web/timeoutSec: how long a search waits for a slow index
void TstNetSettingsWeb::webUserAgentAndSearchTimeout()
{
    ScriptServer srv;
    int delay = 0;
    srv.answer = [&delay](const Seen &) { return jsonReply(records({QStringLiteral("Big Buck Bunny 1080p")}), delay); };
    WebEnv env(srv);
    env.settings.setValue(web::wkeys::UserAgent, QStringLiteral("OctoMXTestAgent/3"));   // "web/userAgent"
    SearchRun r = runSearch(&env.plugin, 31, QStringLiteral("big buck bunny"));
    QVERIFY(r.done);
    QVERIFY(!srv.seen.isEmpty());
    QCOMPARE(srv.seen.last().header("User-Agent"), QByteArray("OctoMXTestAgent/3"));
    env.settings.setValue(web::wkeys::UserAgent, QString());
    r = runSearch(&env.plugin, 32, QStringLiteral("big buck bunny"));
    QCOMPARE(srv.seen.last().header("User-Agent"), web::defaultUserAgent().toUtf8());
    // an index that answers after 6 s: with 3 s the search ends at 3 s without it, with 10 s it waits
    delay = 6000;
    env.settings.setValue(web::wkeys::TimeoutSec, 3);   // "web/timeoutSec"
    r = runSearch(&env.plugin, 33, QStringLiteral("big buck bunny"));
    QVERIFY(r.done);
    qInfo("web/timeoutSec 3: the search ended after %lld ms", r.ms);
    QVERIFY2(r.ms >= 2700 && r.ms < 5500, qPrintable(QString::number(r.ms)));
    QVERIFY(r.hits.isEmpty());
    env.settings.setValue(web::wkeys::TimeoutSec, 10);
    r = runSearch(&env.plugin, 34, QStringLiteral("big buck bunny"), 15000);
    QVERIFY(r.done);
    qInfo("web/timeoutSec 10: the search ended after %lld ms", r.ms);
    QVERIFY2(r.ms >= 5600 && r.ms < 9800, qPrintable(QString::number(r.ms)));
    QCOMPARE(hitNames(r.hits), QStringList{QStringLiteral("Big Buck Bunny 1080p")});
}

// web/probeOnStart: at start every index is checked once (its probe search); off: nothing is
// asked until the user searches
void TstNetSettingsWeb::webIndexCheckAtStart()
{
    for (const bool on : {true, false}) {
        ScriptServer srv;
        srv.answer = [](const Seen &) { return jsonReply(records({QStringLiteral("Debian 13 netinst iso")})); };
        WebEnv env(srv, false);
        env.settings.setValue(web::wkeys::ProbeOnStart, on);   // "web/probeOnStart"
        env.plugin.start();
        if (on) {
            QTRY_VERIFY_WITH_TIMEOUT(!srv.of("GET", "/json").isEmpty(), 8000);
            QCOMPARE(srv.of("GET", "/json").first().query, QByteArray("q=debian"));
            QTRY_COMPARE_WITH_TIMEOUT(env.plugin.status().state, ConnState::Connected, 5000);
        } else {
            QTest::qWait(5000);
            QVERIFY(srv.seen.isEmpty());
            QVERIFY(env.plugin.status().state != ConnState::Connected);
        }
        env.plugin.stop();
    }
}

QTEST_GUILESS_MAIN(TstNetSettingsWeb)
#include "tst_netsettings_web.moc"
