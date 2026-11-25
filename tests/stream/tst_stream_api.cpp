// SPDX-License-Identifier: GPL-3.0-or-later
// [str-1] The streaming API as a door: off by default, on / off / port / LAN at run time, a port in
// use, the access token, the Host check, methods, web pages, size / connection / time limits, and
// every call with its honest errors.
#include <QtNetwork/QNetworkInterface>
#include <QtTest/QTest>

#include "stream_support.h"

using namespace mx;
using namespace mx::stest;

namespace {
bool listens(quint16 port, const QHostAddress &a = QHostAddress::LocalHost)
{
    QTcpSocket s;
    s.connectToHost(a, port);
    (void)QTest::qWaitFor([&] { return s.state() == QAbstractSocket::ConnectedState || s.state() == QAbstractSocket::UnconnectedState; }, 3000);
    return s.state() == QAbstractSocket::ConnectedState;
}

QByteArray get(quint16 port, const QByteArray &path, const QByteArray &headers)
{
    return "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(port) + "\r\n" + headers + "Connection: close\r\n\r\n";
}

QHostAddress privateLanAddress()
{
    for (const QHostAddress &a : QNetworkInterface::allAddresses()) {
        if (a.protocol() != QAbstractSocket::IPv4Protocol || a.isLoopback()) continue;
        const quint32 v = a.toIPv4Address();
        if ((v >> 24) == 10 || (v >> 20) == ((172u << 4) | 1u) || (v >> 16) == ((192u << 8) | 168u)) return a;
    }
    return QHostAddress();
}
} // namespace

class TstStreamApi : public QObject {
    Q_OBJECT
private slots:
    void offByDefault();
    void onOffPortAndLanAtRunTime();
    void portInUse();
    void tokenHostMethodsAndWebPages();
    void limits();
    void callsAndErrors();
    void newTokenEndsOldLinks();
};

void TstStreamApi::offByDefault()
{
    StreamEnv env(QStringLiteral("off"));
    env.port = freePort();
    env.up({}, /*apiOn*/ false);
    // the schema default
    QCOMPARE(settingDefault(QString(keys::api::Enabled)).toBool(), false);
    QCOMPARE(env.core->settings()->value(keys::api::Token).toString(), QString());   // no token before it was switched on
    const StreamApiState st = env.core->streamApiState();
    QVERIFY(st.available);
    QVERIFY(!st.enabled);
    QVERIFY(!st.listening);
    QVERIFY(!listens(env.port));
}

void TstStreamApi::onOffPortAndLanAtRunTime()
{
    StreamEnv env(QStringLiteral("onoff"));
    env.port = freePort();
    env.up({}, false);
    Settings *s = env.core->settings();
    s->setValue(keys::api::Enabled, true);
    QVERIFY(QTest::qWaitFor([&] { return env.core->streamApiState().listening; }, 3000));
    QVERIFY(listens(env.port));
    // a token was made when it was first switched on: 48 hex characters
    env.token = s->s(keys::api::Token).toUtf8();
    QCOMPARE(env.token.size(), 48);
    QCOMPARE(env.api("GET", "/api/v1/status").status, 200);
    // only the loopback addresses
    QStringList addrs = env.core->streamApiState().addresses;
    for (const QString &a : addrs) QVERIFY2(QHostAddress(a).isLoopback(), qPrintable(a));
    const QHostAddress lan = privateLanAddress();
    if (!lan.isNull()) QVERIFY(!listens(env.port, lan));
    // another port at once
    const quint16 p2 = freePort();
    s->setValue(keys::api::Port, int(p2));
    QVERIFY(QTest::qWaitFor([&] { return listens(p2); }, 3000));
    QVERIFY(!listens(env.port));
    env.port = p2;
    QCOMPARE(env.api("GET", "/api/v1/status").status, 200);
    // the LAN switch: the private addresses of this computer too (never a public one)
    s->setValue(keys::api::AllowLan, true);
    addrs = env.core->streamApiState().addresses;
    for (const QString &a : addrs) {
        const QHostAddress h(a);
        QVERIFY2(h.isLoopback() || h.isPrivateUse(), qPrintable(a));
    }
    if (!lan.isNull()) {
        QVERIFY(addrs.contains(lan.toString()));
        QVERIFY(listens(p2, lan));
        // the Host check knows the LAN name too
        const Reply r = rawExchange(p2, "GET /api/v1/status HTTP/1.1\r\nHost: " + lan.toString().toLatin1() + ':' + QByteArray::number(p2)
                                             + "\r\nAuthorization: Bearer " + env.token + "\r\nConnection: close\r\n\r\n", 5000, lan);
        QCOMPARE(r.status, 200);
    } else {
        qInfo("no private LAN address on this machine: the LAN listen itself is not checked");
    }
    s->setValue(keys::api::AllowLan, false);
    if (!lan.isNull()) QVERIFY(QTest::qWaitFor([&] { return !listens(p2, lan); }, 3000));
    // off: nothing listens
    s->setValue(keys::api::Enabled, false);
    QVERIFY(QTest::qWaitFor([&] { return !listens(p2); }, 3000));
    QVERIFY(!env.core->streamApiState().listening);
}

void TstStreamApi::portInUse()
{
    QTcpServer other;
    QVERIFY(other.listen(QHostAddress::LocalHost, 0));
    StreamEnv env(QStringLiteral("inuse"));
    env.port = other.serverPort();
    env.up({}, true);
    const StreamApiState st = env.core->streamApiState();
    QVERIFY(!st.listening);
    QVERIFY2(st.error.contains(QStringLiteral("in use")), qPrintable(st.error));
    QVERIFY(st.error.contains(QString::number(env.port)));
    // no silent fallback to another port
    QCOMPARE(st.port, int(env.port));
    // the other program goes: a new port setting listens
    const quint16 p2 = freePort();
    env.core->settings()->setValue(keys::api::Port, int(p2));
    QVERIFY(QTest::qWaitFor([&] { return env.core->streamApiState().listening; }, 3000));
    QVERIFY(env.core->streamApiState().error.isEmpty());
}

void TstStreamApi::tokenHostMethodsAndWebPages()
{
    StreamEnv env(QStringLiteral("door"));
    env.up();
    const quint16 port = env.port;
    // no token, a wrong token, a token of the wrong length: 401 and nothing else told
    for (const QByteArray &h : {QByteArray(), QByteArray("Authorization: Bearer 00\r\n"),
                                "Authorization: Bearer " + QByteArray(env.token).replace(0, 1, env.token.at(0) == 'a' ? "b" : "a") + "\r\n",
                                "Authorization: Basic " + env.token.toBase64() + "\r\n"}) {
        const Reply r = rawExchange(port, get(port, "/api/v1/status", h));
        QCOMPARE(r.status, 401);
        QCOMPARE(r.json().value(QStringLiteral("message")).toString(), QStringLiteral("Unauthorized."));
        QVERIFY(!r.body.contains("version"));
        const Reply r2 = rawExchange(port, get(port, "/api/v1/streams/abc", h));
        QCOMPARE(r2.status, 401);
    }
    const QByteArray auth = "Authorization: Bearer " + env.token + "\r\n";
    QCOMPARE(rawExchange(port, get(port, "/api/v1/status", auth)).status, 200);
    // DNS rebinding: a Host name that is not ours
    {
        const Reply r = rawExchange(port, "GET /api/v1/status HTTP/1.1\r\nHost: rebind.example:" + QByteArray::number(port) + "\r\n" + auth + "\r\n");
        QCOMPARE(r.status, 403);
        const Reply r2 = rawExchange(port, "GET /api/v1/status HTTP/1.1\r\nHost: 127.0.0.1:1\r\n" + auth + "\r\n");
        QCOMPARE(r2.status, 403);
        const Reply r3 = rawExchange(port, "GET /api/v1/status HTTP/1.1\r\nHost: localhost:" + QByteArray::number(port) + "\r\n" + auth + "Connection: close\r\n\r\n");
        QCOMPARE(r3.status, 200);
    }
    // a web page: Origin present
    {
        const Reply r = rawExchange(port, get(port, "/api/v1/status", auth + "Origin: http://page.example\r\n"));
        QCOMPARE(r.status, 403);
        QVERIFY(r.header("access-control-allow-origin").isEmpty());
    }
    // methods: only GET HEAD POST DELETE; no CORS preflight
    for (const QByteArray &m : {QByteArray("OPTIONS"), QByteArray("PUT"), QByteArray("PATCH"), QByteArray("TRACE")}) {
        const Reply r = rawExchange(port, m + " /api/v1/status HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(port) + "\r\n" + auth
                                                + "Origin: http://page.example\r\nAccess-Control-Request-Method: POST\r\nConnection: close\r\n\r\n");
        QCOMPARE(r.status, 405);
        QVERIFY(r.header("access-control-allow-origin").isEmpty());
        QVERIFY(r.header("access-control-allow-methods").isEmpty());
    }
    // a POST without JSON (what a form of a web page can send) and broken JSON
    {
        const QByteArray body = "text=bunny";
        const Reply r = rawExchange(port, "POST /api/v1/searches HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(port) + "\r\n" + auth
                                              + "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: " + QByteArray::number(body.size())
                                              + "\r\nConnection: close\r\n\r\n" + body);
        QCOMPARE(r.status, 415);
        const QByteArray bad = "{\"text\":";
        const Reply r2 = rawExchange(port, "POST /api/v1/searches HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(port) + "\r\n" + auth
                                               + "Content-Type: application/json\r\nContent-Length: " + QByteArray::number(bad.size())
                                               + "\r\nConnection: close\r\n\r\n" + bad);
        QCOMPARE(r2.status, 400);
    }
    // an unknown version, an unknown call
    QCOMPARE(env.api("GET", "/api/v2/status").status, 404);
    QCOMPARE(env.api("GET", "/api/v1/settings").status, 404);
    QCOMPARE(env.api("GET", "/").status, 404);
    QCOMPARE(env.api("DELETE", "/api/v1/status").status, 405);
    // no path into the file system
    QCOMPARE(env.api("GET", "/stream/../../profile/settings.json").status, 400);
    QCOMPARE(env.api("GET", "/api/v1/../../settings.json").status, 400);
}

void TstStreamApi::limits()
{
    StreamEnv env(QStringLiteral("limits"));
    env.up();
    const quint16 port = env.port;
    const QByteArray auth = "Authorization: Bearer " + env.token + "\r\n";
    // a head larger than 8 KiB
    {
        const Reply r = rawExchange(port, "GET /api/v1/status HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(port) + "\r\nX-Pad: "
                                              + QByteArray(9000, 'a') + "\r\n\r\n");
        QCOMPARE(r.status, 431);
        QCOMPARE(r.header("connection"), QByteArray("close"));
    }
    // a body larger than 64 KiB is not even read
    {
        const Reply r = rawExchange(port, "POST /api/v1/searches HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(port) + "\r\n" + auth
                                              + "Content-Type: application/json\r\nContent-Length: 10000000\r\n\r\n{");
        QCOMPARE(r.status, 413);
    }
    // chunked request bodies are refused (no smuggling); two Content-Length headers too
    {
        const Reply r = rawExchange(port, "POST /api/v1/searches HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(port) + "\r\n" + auth
                                              + "Transfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n");
        QCOMPARE(r.status, 411);
        const Reply r2 = rawExchange(port, "POST /api/v1/searches HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(port) + "\r\n" + auth
                                               + "Content-Length: 2\r\nContent-Length: 3\r\n\r\n{}");
        QCOMPARE(r2.status, 400);
    }
    // garbage is answered and closed, the program goes on
    QCOMPARE(rawExchange(port, "\x16\x03\x01\x02\xfc\x03\x03junk\r\n\r\n").status, 400);
    QCOMPARE(env.api("GET", "/api/v1/status").status, 200);
    // a client that sends half a head and nothing more is closed (time limit shrunk for the test)
    stream::HttpServer::Limits L = env.core->streamService()->server()->limits();
    L.headTimeoutMs = 600;
    L.idleTimeoutMs = 600;
    env.core->streamService()->server()->setLimits(L);
    {
        QTcpSocket s;
        s.connectToHost(QHostAddress::LocalHost, port);
        QVERIFY(QTest::qWaitFor([&] { return s.state() == QAbstractSocket::ConnectedState; }, 3000));
        s.write("GET /api/v1/status HTTP/1.1\r\nHost: 127.0.0.1");
        QElapsedTimer t;
        t.start();
        QVERIFY(QTest::qWaitFor([&] { return s.state() == QAbstractSocket::UnconnectedState; }, 5000));
        QVERIFY(t.elapsed() < 4000);
    }
    // an idle keep-alive connection is closed
    {
        QTcpSocket s;
        s.connectToHost(QHostAddress::LocalHost, port);
        QVERIFY(QTest::qWaitFor([&] { return s.state() == QAbstractSocket::ConnectedState; }, 3000));
        QVERIFY(QTest::qWaitFor([&] { return s.state() == QAbstractSocket::UnconnectedState; }, 5000));
    }
    // connections: at most 16 from one address, 32 in all; the 17th is told and closed
    L.headTimeoutMs = 10000;
    L.idleTimeoutMs = 30000;
    env.core->streamService()->server()->setLimits(L);
    {
        std::vector<std::unique_ptr<QTcpSocket>> socks;
        for (int i = 0; i < 16; ++i) {
            socks.push_back(std::make_unique<QTcpSocket>());
            socks.back()->connectToHost(QHostAddress::LocalHost, port);
        }
        QVERIFY(QTest::qWaitFor([&] { return env.core->streamService()->server()->connectionCount() == 16; }, 5000));
        const Reply r = rawExchange(port, get(port, "/api/v1/status", auth));
        QCOMPARE(r.status, 503);
        socks.clear();
        QVERIFY(QTest::qWaitFor([&] { return env.core->streamService()->server()->connectionCount() == 0; }, 5000));
        QCOMPARE(env.api("GET", "/api/v1/status").status, 200);
    }
}

void TstStreamApi::callsAndErrors()
{
    StreamEnv env(QStringLiteral("calls"));
    TestPlugin *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(3 * 1024 * 1024 + 777, 11);
    const FileHashes h = hashesOf(data, 2);
    SourceConfig c;
    c.data = data;
    c.hashSet = ed2kParts(data);
    const SearchSource src = ed->addSource(QStringLiteral("test-ed2k://calls"), c, QStringLiteral("198.51.100.20:4662"));
    ed->cannedHits << hitOf(QStringLiteral("Big Buck Bunny trailer.mp4"), data, h, {src});
    ed->cannedHits << hitOf(QStringLiteral("Big Buck Bunny poster.txt"), QByteArray(1000, 'x'), hashesOf(QByteArray(1000, 'x'), 2), {src});
    SearchHit nos = hitOf(QStringLiteral("Big Buck Bunny nosource.mkv"), QByteArray(5000, 'y'), hashesOf(QByteArray(5000, 'y'), 2), {});
    ed->cannedHits << nos;
    SearchHit bundle;
    bundle.fileName = QStringLiteral("Big Buck Bunny bundle");
    bundle.size = 5000000;
    bundle.isBundle = true;
    bundle.hashes.btih = QByteArray(20, '\x42');
    SearchSource bs;
    bs.net = Net::BitTorrent;
    bs.uri = QStringLiteral("magnet:?xt=urn:btih:") + QString::fromLatin1(bundle.hashes.btih.toHex());
    bundle.sources << bs;
    ed->cannedHits << bundle;
    ed->bundleAnswers.insert(bundle.hashes.btih, {BundleFile{QStringLiteral("a.mkv"), 2000000, 0}, BundleFile{QStringLiteral("b.mp4"), 3000000, 1}});
    env.up();
    // status
    const Reply st = env.api("GET", "/api/v1/status");
    QCOMPARE(st.status, 200);
    QCOMPARE(st.json().value(QStringLiteral("api")).toString(), QStringLiteral("on"));
    QVERIFY(!st.json().value(QStringLiteral("networks")).toArray().isEmpty());
    QCOMPARE(st.header("content-type"), QByteArray("application/json; charset=utf-8"));
    // a search with an unknown type / network: 400
    QCOMPARE(env.api("POST", "/api/v1/searches", QJsonObject{{QStringLiteral("text"), QStringLiteral("bunny")}, {QStringLiteral("type"), QStringLiteral("films")}}).status, 400);
    QCOMPARE(env.api("POST", "/api/v1/searches", QJsonObject{{QStringLiteral("text"), QStringLiteral("bunny")},
                                                            {QStringLiteral("networks"), QJsonArray{QStringLiteral("Nowhere")}}}).status, 400);
    QCOMPARE(env.api("POST", "/api/v1/searches", QJsonObject{{QStringLiteral("text"), QString()}}).status, 400);
    // a search whose network answers late: "still empty"
    ed->respondDelayMs = 1500;
    const Reply s1 = env.api("POST", "/api/v1/searches", QJsonObject{{QStringLiteral("text"), QStringLiteral("bunny")}});
    QCOMPARE(s1.status, 201);
    const int sid = s1.json().value(QStringLiteral("id")).toInt();
    const Reply early = env.api("GET", "/api/v1/searches/" + QByteArray::number(sid));
    QCOMPARE(early.status, 200);
    QCOMPARE(early.json().value(QStringLiteral("count")).toInt(), 0);
    QVERIFY(early.json().value(QStringLiteral("running")).toBool());
    const Reply selEarly = env.api("POST", "/api/v1/streams", QJsonObject{{QStringLiteral("search"), sid}, {QStringLiteral("result"), QStringLiteral("x")}});
    QCOMPARE(selEarly.status, 409);
    QCOMPARE(selEarly.json().value(QStringLiteral("error")).toString(), QStringLiteral("search_empty"));
    QJsonArray results;
    QVERIFY(QTest::qWaitFor([&] {
        results = env.api("GET", "/api/v1/searches/" + QByteArray::number(sid)).json().value(QStringLiteral("results")).toArray();
        return results.size() == 4;
    }, 10000));
    QHash<QString, QJsonObject> byName;
    for (const QJsonValue &v : results) byName.insert(v.toObject().value(QStringLiteral("name")).toString(), v.toObject());
    const QJsonObject trailer = byName.value(QStringLiteral("Big Buck Bunny trailer.mp4"));
    QCOMPARE(qint64(trailer.value(QStringLiteral("size")).toDouble()), qint64(data.size()));
    QCOMPARE(trailer.value(QStringLiteral("type")).toString(), QStringLiteral("video"));
    QVERIFY(trailer.value(QStringLiteral("networks")).toArray().contains(QStringLiteral("eD2k")));
    QVERIFY(trailer.value(QStringLiteral("streamable")).toBool());
    QVERIFY(!byName.value(QStringLiteral("Big Buck Bunny poster.txt")).value(QStringLiteral("streamable")).toBool());
    // a bundle: its file list comes later (the search may have asked for it already)
    QVERIFY(byName.value(QStringLiteral("Big Buck Bunny bundle")).value(QStringLiteral("bundle")).toBool());
    // the list of searches
    QVERIFY(!env.api("GET", "/api/v1/searches").json().value(QStringLiteral("searches")).toArray().isEmpty());
    auto select = [&](const QString &name, const QJsonObject &more = {}) {
        QJsonObject o{{QStringLiteral("search"), sid}, {QStringLiteral("result"), byName.value(name).value(QStringLiteral("key")).toString()}};
        for (auto it = more.begin(); it != more.end(); ++it) o.insert(it.key(), it.value());
        return env.api("POST", "/api/v1/streams", o);
    };
    // errors of the select call
    {
        const Reply r = env.api("POST", "/api/v1/streams", QJsonObject{{QStringLiteral("search"), sid}, {QStringLiteral("result"), QStringLiteral("0123")}});
        QCOMPARE(r.status, 404);
        QCOMPARE(env.api("POST", "/api/v1/streams", QJsonObject{{QStringLiteral("search"), 999}, {QStringLiteral("result"), QStringLiteral("0")}}).status, 404);
        const Reply txt = select(QStringLiteral("Big Buck Bunny poster.txt"));
        QCOMPARE(txt.status, 415);
        QCOMPARE(txt.json().value(QStringLiteral("error")).toString(), QStringLiteral("not_streamable"));
        const Reply b1 = select(QStringLiteral("Big Buck Bunny bundle"));
        if (b1.status == 409) QCOMPARE(b1.json().value(QStringLiteral("error")).toString(), QStringLiteral("files_unknown"));
        else QCOMPARE(b1.json().value(QStringLiteral("error")).toString(), QStringLiteral("choose_file"));   // the search had it already
        // the file list arrives (asked for by that call); two media files: say which
        QJsonObject bres;
        QVERIFY(QTest::qWaitFor([&] {
            bres = env.api("GET", "/api/v1/searches/" + QByteArray::number(sid) + "/results/"
                                       + byName.value(QStringLiteral("Big Buck Bunny bundle")).value(QStringLiteral("key")).toString().toLatin1()).json();
            return bres.value(QStringLiteral("files")).isArray();
        }, 5000));
        QCOMPARE(bres.value(QStringLiteral("files")).toArray().size(), 2);
        const Reply b2 = select(QStringLiteral("Big Buck Bunny bundle"));
        QCOMPARE(b2.status, 400);
        QCOMPARE(b2.json().value(QStringLiteral("error")).toString(), QStringLiteral("choose_file"));
        QCOMPARE(select(QStringLiteral("Big Buck Bunny bundle"), QJsonObject{{QStringLiteral("file"), 7}}).status, 400);
    }
    // select: a stream id and a link
    const Reply sel = select(QStringLiteral("Big Buck Bunny trailer.mp4"));
    QCOMPARE(sel.status, 201);
    const QJsonObject so = sel.json();
    const QString streamId = so.value(QStringLiteral("id")).toString();
    QVERIFY(!streamId.isEmpty());
    const QString url = so.value(QStringLiteral("url")).toString();
    QVERIFY2(url.startsWith(QStringLiteral("http://127.0.0.1:%1/stream/%2/").arg(env.port).arg(streamId)), qPrintable(url));
    QVERIFY(url.endsWith(QStringLiteral("/Big%20Buck%20Bunny%20trailer.mp4")));
    QVERIFY(!url.contains(QString::fromLatin1(env.token)));   // the link carries a key for this stream only
    // the same selection again: the same stream
    const Reply again = select(QStringLiteral("Big Buck Bunny trailer.mp4"));
    QCOMPARE(again.status, 200);
    QCOMPARE(again.json().value(QStringLiteral("id")).toString(), streamId);
    // the stream link: HEAD knows the size, a range beyond the end is 416, a wrong key 401
    const QByteArray path = StreamEnv::pathOf(so);
    {
        const Reply hd = rawExchange(env.port, "HEAD " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(env.port) + "\r\nConnection: close\r\n\r\n");
        QCOMPARE(hd.status, 200);
        QCOMPARE(hd.header("content-length").toLongLong(), qint64(data.size()));
        QCOMPARE(hd.header("accept-ranges"), QByteArray("bytes"));
        QCOMPARE(hd.header("content-type"), QByteArray("video/mp4"));
        const Reply bad = rawExchange(env.port, "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(env.port) + "\r\nRange: bytes="
                                                    + QByteArray::number(data.size() + 10) + "-\r\nConnection: close\r\n\r\n");
        QCOMPARE(bad.status, 416);
        QCOMPARE(bad.header("content-range"), "bytes */" + QByteArray::number(data.size()));
        QByteArray wrong = path;
        const int k = wrong.indexOf(streamId.toLatin1()) + streamId.size() + 1;
        wrong[k] = wrong.at(k) == '0' ? '1' : '0';
        QCOMPARE(rawExchange(env.port, "GET " + wrong + " HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(env.port) + "\r\nConnection: close\r\n\r\n").status, 401);
        QCOMPARE(rawExchange(env.port, "POST " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(env.port)
                                           + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n").status, 405);
        // a suffix range: the last 1000 bytes
        Player p(env.port, path, data.size() - 1000, data.size());
        QVERIFY(p.waitDone(30000));
        QCOMPARE(p.reply().status, 206);
        QCOMPARE(p.data(), data.right(1000));
    }
    // the stream's status and the list
    QVERIFY(QTest::qWaitFor([&] { return env.status(streamId).value(QStringLiteral("transferStatus")).toString().startsWith(QStringLiteral("Streaming:")); }, 5000));
    const QJsonObject sj = env.status(streamId);
    QCOMPARE(sj.value(QStringLiteral("id")).toString(), streamId);
    QCOMPARE(qint64(sj.value(QStringLiteral("size")).toDouble()), qint64(data.size()));
    QVERIFY(sj.contains(QStringLiteral("players")));
    QVERIFY(sj.value(QStringLiteral("transferStatus")).toString().startsWith(QStringLiteral("Streaming:")));
    QCOMPARE(env.api("GET", "/api/v1/streams").json().value(QStringLiteral("streams")).toArray().size(), 1);
    {   // the caches against the limit, in the list and in the status call
        const QJsonObject cache = env.api("GET", "/api/v1/streams").json().value(QStringLiteral("cache")).toObject();
        QCOMPARE(qint64(cache.value(QStringLiteral("limitBytes")).toDouble()), qint64(4096) << 20);
        QVERIFY(cache.value(QStringLiteral("totalBytes")).toDouble() >= 1000);
        QCOMPARE(cache.value(QStringLiteral("streams")).toArray().size(), 1);
        QVERIFY(cache.value(QStringLiteral("rule")).toString().contains(QStringLiteral("never cut")));
        QVERIFY(env.api("GET", "/api/v1/status").json().contains(QStringLiteral("cache")));
        QVERIFY(sj.contains(QStringLiteral("cacheTotalBytes")) && sj.contains(QStringLiteral("cacheLimitBytes")));
    }
    QCOMPARE(env.api("GET", "/api/v1/streams/nope").status, 404);
    // stop: the link is gone, the status says why
    const Reply stop = env.api("DELETE", "/api/v1/streams/" + streamId.toLatin1());
    QCOMPARE(stop.status, 200);
    QVERIFY(stop.json().value(QStringLiteral("stopped")).toBool());
    QCOMPARE(rawExchange(env.port, "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(env.port) + "\r\nConnection: close\r\n\r\n").status, 410);
    QCOMPARE(env.status(streamId).value(QStringLiteral("state")).toString(), QStringLiteral("ended"));
    // too many searches: busy
    for (int i = 0; i < 20; ++i) {
        const Reply r = env.api("POST", "/api/v1/searches", QJsonObject{{QStringLiteral("text"), QStringLiteral("bunny %1").arg(i)}});
        if (r.status == 429) {
            QCOMPARE(r.json().value(QStringLiteral("error")).toString(), QStringLiteral("busy"));
            QCOMPARE(env.api("DELETE", "/api/v1/searches/" + QByteArray::number(sid)).status, 200);
            QCOMPARE(env.api("GET", "/api/v1/searches/" + QByteArray::number(sid)).status, 404);
            return;
        }
        QCOMPARE(r.status, 201);
    }
    QFAIL("never busy");
}

void TstStreamApi::newTokenEndsOldLinks()
{
    StreamEnv env(QStringLiteral("newtoken"));
    TestPlugin *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(1024 * 1024, 12);
    SourceConfig c;
    c.data = data;
    ed->cannedHits << hitOf(QStringLiteral("Big Buck Bunny 360p.mkv"), data, hashesOf(data, 2),
                            {ed->addSource(QStringLiteral("test-ed2k://tok"), c, QStringLiteral("198.51.100.21:4662"))});
    env.up();
    const QJsonObject so = env.streamOf(QStringLiteral("bunny"), QStringLiteral("Big Buck Bunny 360p.mkv"));
    const QByteArray path = StreamEnv::pathOf(so);
    {
        Player p(env.port, path, 0, 1000);
        QVERIFY(p.waitDone(20000));
        QCOMPARE(p.data(), data.left(1000));
    }
    const QByteArray old = env.token;
    env.core->settings()->setValue(keys::api::Token, stream::StreamService::newToken());
    QCOMPARE(rawExchange(env.port, get(env.port, "/api/v1/status", "Authorization: Bearer " + old + "\r\n")).status, 401);
    QCOMPARE(rawExchange(env.port, "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(env.port) + "\r\nConnection: close\r\n\r\n").status, 401);
}

QTEST_MAIN(TstStreamApi)
#include "tst_stream_api.moc"
