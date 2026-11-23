// SPDX-License-Identifier: GPL-3.0-or-later
// [enc-1] Gnutella TLS: TlsSocket against local servers (TLS echo, plain-only peers that drop or
// ignore a ClientHello), the fallback / require policies, an anonymous-DH TLS 1.0 client like old
// LimeWire-family servents, the certificate kept in the profile, and the TLS flags on the wire
// (GGEP TLS / IPP_TLS / ALT_TLS / PUSH_TLS, X-Alt "tls=", push GGEP) plus the host cache flag.
#include "gnut_hostcache.h"
#include "gnut_http.h"
#include "gnut_message.h"
#include "gnut_tls.h"

#include <QtCore/QTemporaryDir>
#include <QtNetwork/QTcpServer>
#include <QtTest/QtTest>

#include <openssl/bio.h>
#include <openssl/ssl.h>

using namespace mx::gnut;

// a server on the loopback: TLS echo, or a plain echo that drops / ignores TLS
class EchoServer : public QObject {
    Q_OBJECT
public:
    enum Mode { TlsEcho, PlainDropsTls, PlainIgnoresTls };
    EchoServer(Mode m, std::shared_ptr<TlsEngine> e) : m_mode(m), m_engine(std::move(e))
    {
        QVERIFY(m_srv.listen(QHostAddress::LocalHost, 0));
        connect(&m_srv, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *s = m_srv.nextPendingConnection()) {
                s->setParent(this);
                connect(s, &QTcpSocket::readyRead, this, [this, s] { onFirst(s); });
            }
        });
    }
    quint16 port() const { return m_srv.serverPort(); }
    QList<QPointer<TlsSocket>> tls;
    int plainConnections = 0;
    bool closeAfterEcho = false;

private:
    void onFirst(QTcpSocket *s)
    {
        if (s->property("plain").toBool()) { echo(s); return; }
        const QByteArray data = s->readAll();
        if (looksLikeTlsClientHello(data)) {
            if (m_mode == PlainDropsTls) { s->abort(); return; }
            if (m_mode == PlainIgnoresTls) return;
            s->disconnect(this);
            auto *t = new TlsSocket(m_engine, TlsSocket::TlsOnly, this);
            tls << t;
            connect(t, &QTcpSocket::readyRead, this, [this, t] { echo(t); });
            t->startServer(s, data);
            return;
        }
        ++plainConnections;
        s->setProperty("plain", true);
        s->write(data);
        if (closeAfterEcho) s->disconnectFromHost();
    }
    void echo(QTcpSocket *s)
    {
        s->write(s->readAll());
        if (closeAfterEcho) s->disconnectFromHost();
    }
    Mode m_mode;
    std::shared_ptr<TlsEngine> m_engine;
    QTcpServer m_srv;
};

class TstGnutellaTls : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void certificateKeptInProfile();
    void tlsEchoBothWays();
    void largeTransfer();
    void peerClosesAfterData();
    void preferFallsBackWhenDropped();
    void preferFallsBackWhenIgnored();
    void requireFailsAgainstPlain();
    void refusedConnectIsNoFallback();
    void anonymousDhTls10Client();
    void clientHelloDetection();
    void tlsBitFields();
    void altLocTlsPrefix();
    void pushTlsFlag();
    void pongAndHitTlsFlags();
    void hostCacheKeepsTls();
    void modeNames();

private:
    QByteArray roundTrip(TlsSocket &c, const QByteArray &msg, int timeoutMs = 10000);
    QTemporaryDir *m_dir = nullptr;
    std::shared_ptr<TlsEngine> m_server, m_client;
};

void TstGnutellaTls::initTestCase()
{
    m_dir = new QTemporaryDir(QDir::currentPath() + QStringLiteral("/gnut-tls-XXXXXX"));
    QVERIFY(m_dir->isValid());
    QString err;
    m_server = TlsEngine::create(m_dir->filePath(QStringLiteral("server/tls.pem")), &err);
    QVERIFY2(m_server, qPrintable(err));
    m_client = TlsEngine::create(m_dir->filePath(QStringLiteral("client/tls.pem")), &err);
    QVERIFY2(m_client, qPrintable(err));
}

void TstGnutellaTls::certificateKeptInProfile()
{
    const QString path = m_dir->filePath(QStringLiteral("profile/gnutella/tls.pem"));
    QString err;
    auto a = TlsEngine::create(path, &err);
    QVERIFY2(a, qPrintable(err));
    QVERIFY(a->createdNow());
    QVERIFY(QFile::exists(path));
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QByteArray pem = f.readAll();
    QVERIFY(pem.contains("PRIVATE KEY"));
    QVERIFY(pem.contains("BEGIN CERTIFICATE"));
#ifndef Q_OS_WIN
    QCOMPARE(f.permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther), QFileDevice::Permissions());
#endif
    // restart: the same certificate is loaded, not made again
    auto b = TlsEngine::create(path, &err);
    QVERIFY(b);
    QVERIFY(!b->createdNow());
    QCOMPARE(b->certificateInfo(), a->certificateInfo());
    QFile g(path);
    QVERIFY(g.open(QIODevice::ReadOnly));
    QCOMPARE(g.readAll(), pem);
    // a damaged file is replaced
    QFile w(path);
    QVERIFY(w.open(QIODevice::WriteOnly | QIODevice::Truncate));
    w.write("garbage");
    w.close();
    auto c = TlsEngine::create(path, &err);
    QVERIFY(c);
    QVERIFY(c->createdNow());
}

QByteArray TstGnutellaTls::roundTrip(TlsSocket &c, const QByteArray &msg, int timeoutMs)
{
    c.write(msg);
    QByteArray got;
    QElapsedTimer t;
    t.start();
    while (got.size() < msg.size() && t.elapsed() < timeoutMs) {
        QTest::qWait(5);
        got += c.readAll();
    }
    return got;
}

void TstGnutellaTls::tlsEchoBothWays()
{
    EchoServer srv(EchoServer::TlsEcho, m_server);
    TlsSocket c(m_client, TlsSocket::TlsOnly);
    QSignalSpy connected(&c, &QTcpSocket::connected), enc(&c, &TlsSocket::encrypted);
    c.connectToHost(QHostAddress(QHostAddress::LocalHost), srv.port());
    c.write("early ");                                   // written during the handshake: kept, sent after it
    QTRY_COMPARE_WITH_TIMEOUT(connected.count(), 1, 10000);
    QCOMPARE(enc.count(), 1);
    QVERIFY(c.isEncrypted());
    QCOMPARE(c.state(), QAbstractSocket::ConnectedState);
    QCOMPARE(c.peerAddress(), QHostAddress(QHostAddress::LocalHost));
    QCOMPARE(c.peerPort(), srv.port());
    QVERIFY2(c.cipherName().startsWith(QLatin1String("TLSv1.3")), qPrintable(c.cipherName()));
    c.write("hello Gnutella\n");
    const QByteArray want("early hello Gnutella\n");
    QByteArray got;
    QTRY_VERIFY_WITH_TIMEOUT((got += c.readAll()).size() >= want.size(), 10000);
    QCOMPARE(got, want);
    QTRY_COMPARE_WITH_TIMEOUT(srv.tls.size(), 1, 2000);
    QVERIFY(srv.tls.first()->isEncrypted());
    QCOMPARE(m_server->stats.inOk >= 1, true);
}

void TstGnutellaTls::largeTransfer()
{
    EchoServer srv(EchoServer::TlsEcho, m_server);
    TlsSocket c(m_client, TlsSocket::TlsOnly);
    c.connectToHost(QHostAddress(QHostAddress::LocalHost), srv.port());
    QTRY_VERIFY_WITH_TIMEOUT(c.isEncrypted(), 10000);
    QByteArray big(3 * 1024 * 1024 + 7, Qt::Uninitialized);
    for (int i = 0; i < big.size(); ++i) big[i] = char((i * 131) ^ (i >> 7));
    qint64 written = 0;
    connect(&c, &QTcpSocket::bytesWritten, this, [&written](qint64 n) { written += n; });
    QCOMPARE(roundTrip(c, big, 30000), big);
    QVERIFY(written >= big.size());                      // bytesWritten counts the wire (a bit more)
    QCOMPARE(c.bytesToWrite(), qint64(0));
}

void TstGnutellaTls::peerClosesAfterData()
{
    EchoServer srv(EchoServer::TlsEcho, m_server);
    srv.closeAfterEcho = true;
    TlsSocket c(m_client, TlsSocket::TlsOnly);
    QSignalSpy gone(&c, &QTcpSocket::disconnected);
    c.connectToHost(QHostAddress(QHostAddress::LocalHost), srv.port());
    QTRY_VERIFY_WITH_TIMEOUT(c.isEncrypted(), 10000);
    c.write("last words\n");
    QTRY_COMPARE_WITH_TIMEOUT(gone.count(), 1, 10000);
    QCOMPARE(c.state(), QAbstractSocket::UnconnectedState);
    QCOMPARE(c.readAll(), QByteArray("last words\n"));   // data that came with the close is still readable
}

void TstGnutellaTls::preferFallsBackWhenDropped()
{
    EchoServer srv(EchoServer::PlainDropsTls, nullptr);
    const int before = m_client->stats.outFallback;
    TlsSocket c(m_client, TlsSocket::TryTls);
    QSignalSpy connected(&c, &QTcpSocket::connected), failed(&c, &TlsSocket::tlsFailed), err(&c, &QTcpSocket::errorOccurred);
    c.connectToHost(QHostAddress(QHostAddress::LocalHost), srv.port());
    QTRY_COMPARE_WITH_TIMEOUT(connected.count(), 1, 10000);
    QVERIFY(!c.isEncrypted());
    QVERIFY(c.fellBack());
    QCOMPARE(failed.count(), 1);
    QCOMPARE(err.count(), 0);
    QCOMPARE(roundTrip(c, "plain after all\n"), QByteArray("plain after all\n"));
    QCOMPARE(m_client->stats.outFallback, before + 1);
    QCOMPARE(srv.plainConnections, 1);
}

void TstGnutellaTls::preferFallsBackWhenIgnored()
{
    EchoServer srv(EchoServer::PlainIgnoresTls, nullptr);
    TlsSocket::setHandshakeTimeoutMs(800);
    TlsSocket c(m_client, TlsSocket::TryTls);
    QSignalSpy connected(&c, &QTcpSocket::connected);
    c.connectToHost(QHostAddress(QHostAddress::LocalHost), srv.port());
    QTRY_COMPARE_WITH_TIMEOUT(connected.count(), 1, 10000);
    TlsSocket::setHandshakeTimeoutMs(12000);
    QVERIFY(c.fellBack());
    QVERIFY(c.tlsError().contains(QLatin1String("timeout")));
    QCOMPARE(roundTrip(c, "x\n"), QByteArray("x\n"));
}

void TstGnutellaTls::requireFailsAgainstPlain()
{
    EchoServer srv(EchoServer::PlainDropsTls, nullptr);
    const int before = m_client->stats.outFailed;
    TlsSocket c(m_client, TlsSocket::TlsOnly);
    QSignalSpy connected(&c, &QTcpSocket::connected), err(&c, &QTcpSocket::errorOccurred);
    c.connectToHost(QHostAddress(QHostAddress::LocalHost), srv.port());
    QTRY_COMPARE_WITH_TIMEOUT(err.count(), 1, 10000);
    QCOMPARE(err.first().first().value<QAbstractSocket::SocketError>(), QAbstractSocket::SslHandshakeFailedError);
    QCOMPARE(connected.count(), 0);
    QVERIFY(c.errorString().startsWith(QLatin1String("TLS handshake failed")));
    QCOMPARE(c.state(), QAbstractSocket::UnconnectedState);
    QCOMPARE(m_client->stats.outFailed, before + 1);
    QCOMPARE(srv.plainConnections, 0);
    QTest::qWait(200);
    QCOMPARE(err.count(), 1);                            // reported once
}

void TstGnutellaTls::refusedConnectIsNoFallback()
{
    quint16 port = 0;
    {
        QTcpServer tmp;
        QVERIFY(tmp.listen(QHostAddress::LocalHost, 0));
        port = tmp.serverPort();
    }
    const int fb = m_client->stats.outFallback;
    TlsSocket c(m_client, TlsSocket::TryTls);
    QSignalSpy err(&c, &QTcpSocket::errorOccurred), connected(&c, &QTcpSocket::connected);
    c.connectToHost(QHostAddress(QHostAddress::LocalHost), port);
    // Linux reports the refusal at once; Windows (and wine) retry a refused connect for a while
    const bool refused = QTest::qWaitFor([&] { return err.count() > 0; }, 15000);
    Q_UNUSED(refused)   // no error within the wait is accepted: the checks below still hold
    if (err.count() > 0)
        QCOMPARE(err.first().first().value<QAbstractSocket::SocketError>(), QAbstractSocket::ConnectionRefusedError);
    QCOMPARE(connected.count(), 0);
    QCOMPARE(m_client->stats.outFallback, fb);                  // no TCP connection, so no plain retry
}

// old LimeWire-family servents offer only TLS_DH_anon_WITH_AES_128_CBC_SHA over TLS 1.0
void TstGnutellaTls::anonymousDhTls10Client()
{
    EchoServer srv(EchoServer::TlsEcho, m_server);
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_min_proto_version(ctx, TLS1_VERSION);
    SSL_CTX_set_max_proto_version(ctx, TLS1_VERSION);
    SSL_CTX_set_security_level(ctx, 0);
    QVERIFY(SSL_CTX_set_cipher_list(ctx, "ADH-AES128-SHA:@SECLEVEL=0") == 1);
    SSL *ssl = SSL_new(ctx);
    BIO *rb = BIO_new(BIO_s_mem()), *wb = BIO_new(BIO_s_mem());
    SSL_set_bio(ssl, rb, wb);
    SSL_set_connect_state(ssl);
    QTcpSocket s;
    s.connectToHost(QHostAddress::LocalHost, srv.port());
    QTRY_COMPARE_WITH_TIMEOUT(s.state(), QAbstractSocket::ConnectedState, 5000);
    const auto flush = [&] {
        char buf[16384];
        int n;
        while ((n = BIO_read(wb, buf, sizeof buf)) > 0) s.write(buf, n);
    };
    QElapsedTimer t;
    t.start();
    int r = 0;
    while ((r = SSL_do_handshake(ssl)) != 1 && t.elapsed() < 10000) {
        flush();
        QTest::qWait(5);
        const QByteArray in = s.readAll();
        if (!in.isEmpty()) BIO_write(rb, in.constData(), int(in.size()));
    }
    QCOMPARE(r, 1);
    QCOMPARE(QString::fromLatin1(SSL_get_cipher_name(ssl)), QStringLiteral("ADH-AES128-SHA"));
    QCOMPARE(QString::fromLatin1(SSL_get_version(ssl)), QStringLiteral("TLSv1"));
    SSL_write(ssl, "GNUTELLA CONNECT/0.6\r\n", 22);
    flush();
    QByteArray echoed;
    char buf[256];
    t.restart();
    while (echoed.size() < 22 && t.elapsed() < 5000) {
        QTest::qWait(5);
        const QByteArray in = s.readAll();
        if (!in.isEmpty()) BIO_write(rb, in.constData(), int(in.size()));
        int n;
        while ((n = SSL_read(ssl, buf, sizeof buf)) > 0) echoed.append(buf, n);
    }
    QCOMPARE(echoed, QByteArray("GNUTELLA CONNECT/0.6\r\n"));
    QVERIFY(!srv.tls.isEmpty() && srv.tls.last()->cipherName().contains(QLatin1String("ADH-AES128-SHA")));
    SSL_free(ssl);
    SSL_CTX_free(ctx);
}

void TstGnutellaTls::clientHelloDetection()
{
    QVERIFY(looksLikeTlsClientHello(QByteArray::fromHex("16030100a5010000a1")));
    QVERIFY(looksLikeTlsClientHello(QByteArray::fromHex("16")));          // split first record
    QVERIFY(!looksLikeTlsClientHello("GNUTELLA CONNECT/0.6\r\n"));
    QVERIFY(!looksLikeTlsClientHello("GET / HTTP/1.1\r\n"));
    QVERIFY(!looksLikeTlsClientHello("GIV 1:abc/x\n\n"));
    QVERIFY(!looksLikeTlsClientHello(QByteArray::fromHex("1603010005020000")));   // a ServerHello
    QVERIFY(!looksLikeTlsClientHello(QByteArray()));
}

void TstGnutellaTls::tlsBitFields()
{
    const QList<bool> f{true, false, false, true, false, false, false, false, true};
    const QByteArray b = encodeTlsBits(f);
    QCOMPARE(b, QByteArray::fromHex("9080"));
    QCOMPARE(decodeTlsBits(b, 9), f);
    QCOMPARE(decodeTlsBits(QByteArray::fromHex("80"), 3), (QList<bool>{true, false, false}));   // short field
    QCOMPARE(encodeTlsBits({false, false}), QByteArray());                                     // trailing zeros left out
}

void TstGnutellaTls::altLocTlsPrefix()
{
    QList<HostPort> tls;
    const QList<HostPort> all = parseAltLocs(QStringLiteral("tls=A0, 192.0.2.1:6346, 192.0.2.2:6347, 192.0.2.3"), 100, &tls);
    QCOMPARE(all.size(), 3);
    QCOMPARE(tls.size(), 2);
    QCOMPARE(tls[0].toString(), QStringLiteral("192.0.2.1:6346"));
    QCOMPARE(tls[1].toString(), QStringLiteral("192.0.2.3:6346"));
    QCOMPARE(parseAltLocs(QStringLiteral("192.0.2.1")).size(), 1);
}

void TstGnutellaTls::pushTlsFlag()
{
    PushMsg p;
    p.serventGuid = QByteArray(16, 'g');
    p.index = 7;
    p.ip = 0xC0000201;
    p.port = 6346;
    p.tls = true;
    const QByteArray w = buildPushPayload(p);
    QVERIFY(w.size() > 26);
    PushMsg q;
    QVERIFY(parsePush(w, &q));
    QVERIFY(q.tls);
    QCOMPARE(q.index, 7u);
    p.tls = false;
    const QByteArray plain = buildPushPayload(p);
    QCOMPARE(plain.size(), 26);
    QVERIFY(parsePush(plain, &q));
    QVERIFY(!q.tls);
}

void TstGnutellaTls::pongAndHitTlsFlags()
{
    GgepBlock g;
    g.add("TLS", QByteArray());
    QByteArray ipp;
    for (quint32 ip : {0xC0000201u, 0xC0000202u, 0xC0000203u}) ipp.append(packHost6(HostPort{ip, 6346}));
    g.add("IPP", ipp);
    g.add("IPP_TLS", encodeTlsBits({false, true, true}));
    Pong pong;
    QVERIFY(parsePong(buildPongPayload(HostPort{0xC0000209u, 6346}, 1, 2, g), &pong));
    QVERIFY(pong.tls);
    QCOMPARE(pong.ipp.size(), 3);
    QCOMPARE(pong.ippTls.size(), 2);
    QCOMPARE(pong.ippTls.first().toString(), QStringLiteral("192.0.2.2:6346"));
    {   // gtk-gnutella's own name for the flag
        GgepBlock gg;
        gg.add("GTKG.TLS", QByteArray());
        Pong p2;
        QVERIFY(parsePong(buildPongPayload(HostPort{0xC0000209u, 6346}, 1, 2, gg), &p2));
        QVERIFY(p2.tls);
    }
    // a query hit with GGEP "TLS" in the trailer
    HitResult r;
    r.index = 1;
    r.size = 100;
    r.name = QStringLiteral("Sintel.mkv");
    HitTrailer t;
    t.serventGuid = QByteArray(16, 's');
    t.tls = true;
    QueryHit qh;
    QVERIFY(parseQueryHit(buildQueryHitPayload(HostPort{0xC0000201u, 6346}, 1000, {r}, t), &qh));
    QVERIFY(qh.tls);
    t.tls = false;
    QVERIFY(parseQueryHit(buildQueryHitPayload(HostPort{0xC0000201u, 6346}, 1000, {r}, t), &qh));
    QVERIFY(!qh.tls);
}

void TstGnutellaTls::hostCacheKeepsTls()
{
    const QString path = m_dir->filePath(QStringLiteral("hc/hosts.json"));
    QDir().mkpath(m_dir->filePath(QStringLiteral("hc")));
    const HostPort a{0xC0000201u, 6346}, b{0xC0000202u, 6346};
    {
        HostCache hc;
        hc.setPath(path);
        hc.add(a, QStringLiteral("pong"));
        hc.add(b, QStringLiteral("pong"));
        hc.setTls(a, true);
        QVERIFY(hc.supportsTls(a));
        QVERIFY(!hc.supportsTls(b));
        QCOMPARE(hc.tlsCount(), 1);
        QVERIFY(hc.find(a)->score(nowSecs()) > hc.find(b)->score(nowSecs()));   // TLS hosts are preferred
        QVERIFY(hc.save());
    }
    HostCache again;
    again.setPath(path);
    QVERIFY(again.load());
    QVERIFY(again.supportsTls(a));
    QVERIFY(!again.supportsTls(b));
    again.setTls(a, false);
    QCOMPARE(again.tlsCount(), 0);
}

void TstGnutellaTls::modeNames()
{
    QCOMPARE(tlsModeFromSetting(QStringLiteral("enabled")), TlsMode::Prefer);
    QCOMPARE(tlsModeFromSetting(QStringLiteral("forced")), TlsMode::Require);
    QCOMPARE(tlsModeFromSetting(QStringLiteral("disabled")), TlsMode::Off);
    QCOMPARE(tlsModeFromSetting(QString()), TlsMode::Prefer);   // missing key (older profile): on
    QCOMPARE(tlsModeName(TlsMode::Require), QStringLiteral("required"));
}

QTEST_GUILESS_MAIN(TstGnutellaTls)
#include "tst_gnutella_tls.moc"
