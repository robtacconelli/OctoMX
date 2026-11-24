// SPDX-License-Identifier: GPL-3.0-or-later
// [net-2] The connection settings on every network, observed on the wire: a local SOCKS5 / HTTP
// proxy that records what it was asked for and redirects documentation-range addresses (that
// nothing reaches directly) to local servers, canaries that count connections and datagrams per
// source address, and a second loopback address (127.0.0.2) as the "chosen adapter".
//   proxy:   every TCP connection of eD2k, Gnutella, HTTP, the web indexes and the update check
//            arrives through the proxy; eD2k / Gnutella open no UDP socket and do not listen;
//            Kad, the OctoMX network and Rats do not start and say why
//   adapter: every connection and datagram comes from 127.0.0.2, none from 127.0.0.1; a missing
//            adapter: nothing is sent at all
//   manager: the networks stop when the adapter goes, say why, and start again when it is back
#include "net_test_servers.h"
#include "test_support.h"   // tests/ed2k: FileLibrary, SimpleDownloader

#include "ed2k_core.h"
#include "ed2k_plugin.h"
#include "ed2k_plugin_impl.h"
#include "ed2k_upload.h"
#include "gnut_plugin_impl.h"
#include "http_plugin.h"
#include "overlay_plugin.h"
#include "web_plugin.h"

#include "core/network_manager.h"
#include "fake_plugin.h"
#include "mx/net_policy.h"
#include "mx/plugin.h"
#include "mx/settings_keys.h"
#include "mx/simple_settings.h"
#include "mx/update_check.h"

#include <QtCore/QTemporaryDir>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QUdpSocket>
#include <QtTest/QtTest>

using namespace mx;
using nettest::Canary;
using nettest::TestProxy;
namespace np = mx::netpolicy;

namespace {

const QString kAdapter = QStringLiteral("127.0.0.2");   // the "VPN" of these tests
const QString kMissing = QStringLiteral("192.0.2.77");  // an address this computer does not have

bool haveSecondLoopback()
{
    QUdpSocket u;
    return u.bind(QHostAddress(kAdapter), 0);
}

void useProxy(Settings &s, const TestProxy &p, const QString &type = QStringLiteral("socks5"))
{
    s.setValue(QString(keys::connection::ProxyType), type);
    s.setValue(QString(keys::connection::ProxyHost), QStringLiteral("127.0.0.1"));
    s.setValue(QString(keys::connection::ProxyPort), int(p.port()));
}

PluginContext contextFor(Settings *s, const QString &profile, const char *net)
{
    PluginContext ctx;
    ctx.settings = s;
    ctx.profileDir = profile;
    ctx.dataDir = profile + QLatin1Char('/') + QLatin1String(net);
    QDir().mkpath(ctx.dataDir);
    ctx.screenName = QStringLiteral("NetPolicy_12345");
    ctx.clientName = QStringLiteral("OctoMX");
    ctx.clientVersion = QStringLiteral(MX_VERSION_STRING);
    ctx.coreMapsPorts = true;
    return ctx;
}

// a small HTTP responder on a canary
void serveHello(Canary &c)
{
    c.onData = [](QTcpSocket *s) {
        s->write("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello");
        s->disconnectFromHost();
    };
}

void ed2kSettings(SimpleSettings &s)
{
    s.setValue(QStringLiteral("ed2k/tcpPort"), 0);
    s.setValue(QStringLiteral("ed2k/udpPort"), 0);
    s.setValue(QStringLiteral("ed2k/autoConnect"), true);
    s.setValue(QStringLiteral("ed2k/autoUpdateServerList"), false);
    s.setValue(QStringLiteral("ed2k/allowLan"), true);
    s.setValue(QStringLiteral("ed2k/parallelServerConnects"), 4);
}

void gnutSettings(SimpleSettings &s, const QStringList &hosts)
{
    s.setValue(QStringLiteral("gnutella/enabled"), true);
    s.setValue(QStringLiteral("gnutella/port"), 0);
    s.setValue(QStringLiteral("gnutella/bootstrapHosts"), hosts);
    s.setValue(QStringLiteral("gnutella/autoBootstrap"), false);
    s.setValue(QStringLiteral("gnutella/encryption"), QStringLiteral("disabled"));
}

} // namespace

class TstNetPolicy : public QObject {
    Q_OBJECT
private slots:
    void init() { np::setAdapterResolver({}); }
    void cleanup() { np::setAdapterResolver({}); }

    void policyFromSettings();
    void verdictsPerNetwork();
    void relayBindsWebRequests();
    void ed2kThroughProxy();
    void ed2kOnAdapter();
    void gnutellaThroughProxy();
    void gnutellaOnAdapter();
    void kadOffWithProxyAndBoundToAdapter();
    void httpThroughProxyAndAdapter();
    void webAndUpdateCheckThroughProxy();
    void overlayAndRatsWithProxyOrAdapter();
    void managerStopsAndRestartsWithTheAdapter();
    void ed2kObfuscationChoices_data();
    void ed2kObfuscationChoices();
    void proxyNamesAndCredentials();
    void ed2kConnectionPacing();
    void ed2kCompressUploads();
};

// ------------------------------------------------------------------------------------------------
void TstNetPolicy::policyFromSettings()
{
    SimpleSettings s;
    np::Policy p = np::fromSettings(&s);
    QVERIFY(!p.proxy.active());
    QVERIFY(!p.bindWanted());
    QVERIFY(p.ipv6);
    // SOCKS4 is used as SOCKS5 (Qt has no SOCKS4 client; nothing goes direct instead)
    s.setValue(QString(keys::connection::ProxyType), QStringLiteral("socks4"));
    s.setValue(QString(keys::connection::ProxyHost), QStringLiteral("10.1.2.3"));
    s.setValue(QString(keys::connection::ProxyPort), 1081);
    p = np::fromSettings(&s);
    QCOMPARE(p.proxy.kind, np::Proxy::Socks5);
    QCOMPARE(p.proxy.port, quint16(1081));
    QCOMPARE(np::qtProxy(p.proxy).type(), QNetworkProxy::Socks5Proxy);
    // names to the proxy unless the user unticks it
    QVERIFY(np::qtProxy(p.proxy).capabilities() & QNetworkProxy::HostNameLookupCapability);
    s.setValue(QString(keys::connection::ProxyHostnames), false);
    QVERIFY(!(np::qtProxy(np::fromSettings(&s).proxy).capabilities() & QNetworkProxy::HostNameLookupCapability));
    QVERIFY(!np::proxyResolvesNames(&s));
    // credentials only with "requires authentication"
    s.setValue(QString(keys::connection::ProxyUser), QStringLiteral("u"));
    s.setValue(QString(keys::connection::ProxyPassword), QStringLiteral("p"));
    QVERIFY(np::fromSettings(&s).proxy.user.isEmpty());
    s.setValue(QString(keys::connection::ProxyAuth), true);
    QCOMPARE(np::qtProxy(np::fromSettings(&s).proxy).user(), QStringLiteral("u"));
    // a proxy without an address: every network stays off (never direct)
    s.setValue(QString(keys::connection::ProxyHost), QString());
    for (Net n : {Net::BitTorrent, Net::ED2K, Net::Gnutella, Net::Http, Net::Web, Net::Unknown}) {
        const np::Verdict v = np::verdict(n, &s);
        QVERIFY2(!v.allowed, qPrintable(netName(n)));
        QVERIFY(v.reason.contains(QLatin1String("address is empty")));
    }
    // no UDP and no listener while a proxy is set
    QVERIFY(!np::udpAllowed(&s));
    bool ok = true;
    np::listenAddress(&s, &ok);
    QVERIFY(!ok);
}

void TstNetPolicy::verdictsPerNetwork()
{
    np::Policy p;
    np::Adapter a;
    np::Routes r;
    // nothing chosen: everything runs
    for (Net n : {Net::BitTorrent, Net::ED2K, Net::Kad, Net::Gnutella, Net::Http, Net::Overlay, Net::Web, Net::Rats})
        QVERIFY(np::verdictFor(n, p, a, r).allowed);
    // proxy: the UDP / librats networks are off, the others run through it
    p.proxy.kind = np::Proxy::Socks5;
    p.proxy.host = QStringLiteral("10.0.0.1");
    p.proxy.port = 1080;
    QVERIFY(np::verdictFor(Net::Kad, p, a, r).reason.contains(QLatin1String("Kad works over UDP only")));
    QVERIFY(np::verdictFor(Net::Overlay, p, a, r).reason.contains(QLatin1String("cannot run through a proxy")));
    QVERIFY(!np::verdictFor(Net::Rats, p, a, r).allowed);
    for (Net n : {Net::BitTorrent, Net::ED2K, Net::Gnutella, Net::Http, Net::Web, Net::Unknown})
        QVERIFY(np::verdictFor(n, p, a, r).allowed);
    // adapter missing: everything off, the reason names it
    p = np::Policy();
    p.bind = QStringLiteral("tun7");
    a.wanted = p.bind;
    a.present = false;
    for (Net n : {Net::BitTorrent, Net::ED2K, Net::Kad, Net::Gnutella, Net::Http, Net::Overlay, Net::Web, Net::Rats, Net::Unknown}) {
        const np::Verdict v = np::verdictFor(n, p, a, r);
        QVERIFY(!v.allowed);
        QVERIFY(v.reason.contains(QLatin1String("Waiting for the network adapter \"tun7\"")));
    }
    // there with IPv6 only: the IPv4-only networks are off
    a.present = true;
    a.addresses = {QHostAddress(QStringLiteral("2001:db8::5"))};
    QVERIFY(!np::verdictFor(Net::ED2K, p, a, r).allowed);
    QVERIFY(!np::verdictFor(Net::Gnutella, p, a, r).allowed);
    QVERIFY(np::verdictFor(Net::BitTorrent, p, a, r).allowed);
    // the librats networks: only while the internet route is the adapter's
    a.addresses = {QHostAddress(QStringLiteral("10.8.0.2"))};
    r.internetViaAdapter = false;
    QVERIFY(np::verdictFor(Net::Overlay, p, a, r).reason.contains(QLatin1String("internet route")));
    QVERIFY(!np::verdictFor(Net::Rats, p, a, r).allowed);
    QVERIFY(np::verdictFor(Net::ED2K, p, a, r).allowed);   // its sockets are bound themselves
    r.internetViaAdapter = true;
    QVERIFY(np::verdictFor(Net::Overlay, p, a, r).allowed);
    // proxy and adapter: the proxy must be reached through the adapter
    p.proxy.kind = np::Proxy::Http;
    p.proxy.host = QStringLiteral("203.0.113.9");
    p.proxy.port = 3128;
    r.proxyViaAdapter = false;
    r.proxyProblem = QStringLiteral("the route to it uses another adapter");
    for (Net n : {Net::BitTorrent, Net::ED2K, Net::Http, Net::Unknown})
        QVERIFY(np::verdictFor(n, p, a, r).reason.contains(QLatin1String("not reached through the adapter")));
    r.proxyViaAdapter = true;
    QVERIFY(np::verdictFor(Net::ED2K, p, a, r).allowed);

    // the real route check: a proxy on this computer is fine with any adapter; one behind the
    // default route is not reached through 127.0.0.2
    if (haveSecondLoopback()) {
        SimpleSettings s;
        s.setValue(QString(keys::connection::BindInterface), kAdapter);
        s.setValue(QString(keys::connection::ProxyType), QStringLiteral("socks5"));
        s.setValue(QString(keys::connection::ProxyHost), QStringLiteral("127.0.0.1"));
        QVERIFY(np::verdict(Net::ED2K, &s).allowed);
        s.setValue(QString(keys::connection::ProxyHost), QStringLiteral("198.51.100.7"));
        QVERIFY(!np::verdict(Net::ED2K, &s).allowed);
    }
}

// QNetworkAccessManager cannot bind: the relay does it, and refuses for a missing adapter
void TstNetPolicy::relayBindsWebRequests()
{
    if (!haveSecondLoopback()) QSKIP("no second loopback address on this system");
    Canary web;
    serveHello(web);
    QVERIFY(web.listen());
    SimpleSettings s;
    s.setValue(QString(keys::connection::BindInterface), kAdapter);
    QNetworkAccessManager nam;
    np::applyTo(&nam, &s);
    QCOMPARE(nam.proxy().hostName(), QStringLiteral("127.0.0.1"));
    auto get = [&nam](const QString &url) {
        QNetworkReply *r = nam.get(QNetworkRequest(QUrl(url)));
        QSignalSpy done(r, &QNetworkReply::finished);
        done.wait(8000);
        const QByteArray body = r->readAll();
        const bool ok = r->error() == QNetworkReply::NoError;
        r->deleteLater();
        return ok ? body : QByteArray();
    };
    QCOMPARE(get(QStringLiteral("http://127.0.0.1:%1/a").arg(web.port())), QByteArray("hello"));
    QCOMPARE(web.tcpCount(), 1);
    QCOMPARE(web.tcpCount(kAdapter), 1);   // from the adapter, not from 127.0.0.1
    // by name too (the relay looks it up): still from the adapter
    QCOMPARE(get(QStringLiteral("http://localhost:%1/b").arg(web.port())), QByteArray("hello"));
    QCOMPARE(web.tcpCount(), web.tcpCount(kAdapter));
    // adapter gone: the request fails, nothing arrives
    const int before = web.tcpCount();
    s.setValue(QString(keys::connection::BindInterface), kMissing);
    np::clearAdapterCache();
    np::applyTo(&nam, &s);
    QVERIFY(get(QStringLiteral("http://127.0.0.1:%1/c").arg(web.port())).isEmpty());
    QTest::qWait(200);
    QCOMPARE(web.tcpCount(), before);
    // and a raw socket the policy refused: pointed at the closed door, it never connects
    QTcpSocket raw;
    QString why;
    QVERIFY(!np::prepareTcp(&raw, &s, &why));
    QVERIFY(why.contains(kMissing));
    raw.connectToHost(QHostAddress::LocalHost, web.port());
    QVERIFY(!raw.waitForConnected(3000) || raw.state() != QAbstractSocket::ConnectedState);
    QTest::qWait(200);
    QCOMPARE(web.tcpCount(), before);
}

// ------------------------------------------------------------------------------------------------ eD2k
void TstNetPolicy::ed2kThroughProxy()
{
    QTemporaryDir dir;
    Canary server;   // the "eD2k server" (only the login matters)
    QVERIFY(server.listen());
    TestProxy proxy;
    QVERIFY(proxy.listen());
    // the server list holds addresses nothing reaches directly; the proxy knows where they are
    const QStringList fake = {QStringLiteral("192.0.2.10:4661"), QStringLiteral("192.0.2.11:4661"), QStringLiteral("192.0.2.12:4661")};
    for (const QString &f : fake) proxy.redirect(f, QHostAddress::LocalHost, server.port());
    SimpleSettings s;
    ed2kSettings(s);
    useProxy(s, proxy);
    auto *p = new ed2k::Ed2kPlugin;
    p->init(contextFor(&s, dir.path(), "ed2k"));
    for (const QString &f : fake) p->addServer(f, QStringLiteral("policy test"));
    p->start();
    QTRY_VERIFY_WITH_TIMEOUT(server.tcpCount() >= 1, 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!server.received.isEmpty(), 10000);   // the login request came through
    QVERIFY(proxy.countTarget(QStringLiteral("192.0.2.1")) >= 1);
    QCOMPARE(server.tcpCount(), server.tcpCount(QStringLiteral("127.0.0.1")));   // only the proxy's own link
    QVERIFY(server.tcpCount() <= proxy.targets.size());
    // no UDP socket bound, no listener: nothing but the proxy connection
    QCOMPARE(p->core()->udpLocalPort(), quint16(0));
    QVERIFY(!p->core()->listening());
    for (QUdpSocket *u : p->findChildren<QUdpSocket *>()) QCOMPARE(u->state(), QAbstractSocket::UnconnectedState);
    for (QUdpSocket *u : p->core()->findChildren<QUdpSocket *>()) QCOMPARE(u->state(), QAbstractSocket::UnconnectedState);
    p->stop();
    delete p;
}

void TstNetPolicy::ed2kOnAdapter()
{
    if (!haveSecondLoopback()) QSKIP("no second loopback address on this system");
    QTemporaryDir dir;
    Canary server;
    QVERIFY(server.listen());
    SimpleSettings s;
    ed2kSettings(s);
    s.setValue(QString(keys::connection::BindInterface), kAdapter);
    auto *p = new ed2k::Ed2kPlugin;
    p->init(contextFor(&s, dir.path(), "ed2k"));
    p->addServer(QStringLiteral("127.0.0.1:%1").arg(server.port()), QStringLiteral("local"));
    p->addServer(QStringLiteral("127.0.0.1:%1").arg(server.port() == 65535 ? 1 : server.port() + 1), QStringLiteral("x"));
    p->addServer(QStringLiteral("127.0.0.1:1"), QStringLiteral("y"));
    p->start();
    QTRY_VERIFY_WITH_TIMEOUT(server.tcpCount() >= 1, 15000);
    QCOMPARE(server.tcpCount(QStringLiteral("127.0.0.1")), 0);   // nothing from the other address
    QCOMPARE(server.tcpCount(kAdapter), server.tcpCount());
    // the listener and the UDP socket are on the adapter only
    QVERIFY(p->core()->listening());
    QTcpServer *ls = p->core()->findChild<QTcpServer *>();
    QVERIFY(ls);
    QCOMPARE(ls->serverAddress().toString(), kAdapter);
    QVERIFY(p->core()->udpLocalPort() != 0);
    bool udpOnAdapter = false;
    for (QUdpSocket *u : p->core()->findChildren<QUdpSocket *>())
        if (u->state() == QAbstractSocket::BoundState) udpOnAdapter = u->localAddress().toString() == kAdapter;
    QVERIFY(udpOnAdapter);
    p->stop();
    delete p;

    // the adapter is missing: no listener, no UDP, no connection at all
    Canary server2;
    QVERIFY(server2.listen());
    SimpleSettings s2;
    ed2kSettings(s2);
    s2.setValue(QString(keys::connection::BindInterface), kMissing);
    QTemporaryDir dir2;
    auto *q = new ed2k::Ed2kPlugin;
    q->init(contextFor(&s2, dir2.path(), "ed2k"));
    q->addServer(QStringLiteral("127.0.0.1:%1").arg(server2.port()), QStringLiteral("local"));
    q->addServer(QStringLiteral("127.0.0.1:2"), QStringLiteral("x"));
    q->addServer(QStringLiteral("127.0.0.1:3"), QStringLiteral("y"));
    q->start();
    QTest::qWait(3000);
    QCOMPARE(server2.tcpCount(), 0);
    QVERIFY(!q->core()->listening());
    QCOMPARE(q->core()->udpLocalPort(), quint16(0));
    q->stop();
    delete q;
}

// ------------------------------------------------------------------------------------------------ Gnutella
void TstNetPolicy::gnutellaThroughProxy()
{
    QTemporaryDir dir;
    Canary up;   // the "ultrapeer": records the handshake
    QVERIFY(up.listen());
    TestProxy proxy;
    QVERIFY(proxy.listen());
    proxy.redirect(QStringLiteral("192.0.2.20:6346"), QHostAddress::LocalHost, up.port());
    SimpleSettings s;
    gnutSettings(s, {QStringLiteral("192.0.2.20:6346")});
    useProxy(s, proxy, QStringLiteral("http"));   // HTTP CONNECT this time
    auto *p = new gnut::GnutellaPlugin;
    p->init(contextFor(&s, dir.path(), "gnutella"));
    p->start();
    QTRY_VERIFY_WITH_TIMEOUT(up.received.contains("GNUTELLA CONNECT/0.6"), 15000);
    QCOMPARE(proxy.countTarget(QStringLiteral("192.0.2.20:6346")), up.tcpCount());
    QCOMPARE(p->listenPort(), quint16(0));             // no listener behind a proxy
    QVERIFY(p->findChildren<QUdpSocket *>().isEmpty());   // and no UDP socket at all
    p->stop();
    delete p;
}

void TstNetPolicy::gnutellaOnAdapter()
{
    if (!haveSecondLoopback()) QSKIP("no second loopback address on this system");
    QTemporaryDir dir;
    Canary up;
    QVERIFY(up.listen());
    SimpleSettings s;
    gnutSettings(s, {QStringLiteral("192.0.2.21:6346")});
    s.setValue(QString(keys::connection::BindInterface), kAdapter);
    // reach the "ultrapeer" through a SOCKS5 proxy on this computer, which is allowed with any adapter,
    // and also directly (a plain local ultrapeer): both must come from the adapter
    auto *p = new gnut::GnutellaPlugin;
    p->init(contextFor(&s, dir.path(), "gnutella"));
    p->start();
    QTRY_VERIFY_WITH_TIMEOUT(p->listenPort() != 0, 5000);
    QTcpServer *ls = p->findChild<QTcpServer *>();
    QVERIFY(ls);
    QCOMPARE(ls->serverAddress().toString(), kAdapter);
    QList<QUdpSocket *> udps = p->findChildren<QUdpSocket *>();
    QCOMPARE(udps.size(), 1);
    QCOMPARE(udps.first()->localAddress().toString(), kAdapter);
    // an outgoing connection (push-proxy request path: GnutellaPlugin::newSocket) to the canary
    QTcpSocket *sock = p->newSocket(false, p);
    sock->connectToHost(QHostAddress::LocalHost, up.port());
    QTRY_VERIFY_WITH_TIMEOUT(up.tcpCount() == 1, 5000);
    QCOMPARE(up.tcpCount(kAdapter), 1);
    sock->abort();
    p->stop();
    delete p;
}

// ------------------------------------------------------------------------------------------------ Kad
void TstNetPolicy::kadOffWithProxyAndBoundToAdapter()
{
    Canary node;   // a "Kad contact": counts datagrams per source address
    QVERIFY(node.listen());
    auto run = [&node](SimpleSettings &s, const QString &profile, QStringList *log) {
        ed2kSettings(s);
        s.setValue(QStringLiteral("kad/allowLan"), true);
        s.setValue(QStringLiteral("kad/autoBootstrap"), false);
        s.setValue(QStringLiteral("kad/bootstrapNodes"), QStringList{QStringLiteral("127.0.0.1:%1").arg(node.port())});
        const QList<NetworkPlugin *> pl = createEd2kPlugins();
        NetworkPlugin *kad = nullptr;
        for (NetworkPlugin *x : pl) if (x->net() == Net::Kad) kad = x;
        for (NetworkPlugin *x : pl) x->init(contextFor(&s, profile, x->net() == Net::Kad ? "kad" : "ed2k"));
        QObject::connect(kad, &NetworkPlugin::logMessage, kad, [log](const QString &t) { *log << t; });
        kad->start();
        QTest::qWait(2500);
        const NetworkStatus st = kad->status();
        kad->stop();
        qDeleteAll(pl);
        return st;
    };
    TestProxy proxy;
    QVERIFY(proxy.listen());
    {
        QTemporaryDir dir;
        SimpleSettings s;
        useProxy(s, proxy);
        QStringList log;
        run(s, dir.path(), &log);
        QCOMPARE(node.udpCount(), 0);   // not one datagram
        QVERIFY(log.join(QLatin1Char('\n')).contains(QLatin1String("Off while a proxy is set")));
        QCOMPARE(proxy.udpAssociates, 0);
    }
    if (!haveSecondLoopback()) QSKIP("no second loopback address on this system");
    {
        QTemporaryDir dir;
        SimpleSettings s;
        s.setValue(QString(keys::connection::BindInterface), kAdapter);
        QStringList log;
        run(s, dir.path(), &log);
        QVERIFY2(node.udpCount() >= 1, qPrintable(log.join(QLatin1Char('\n'))));
        QCOMPARE(node.udpCount(QStringLiteral("127.0.0.1")), 0);
        QCOMPARE(node.udpCount(kAdapter), node.udpCount());
    }
}

// ------------------------------------------------------------------------------------------------ HTTP
void TstNetPolicy::httpThroughProxyAndAdapter()
{
    Canary web;
    serveHello(web);
    QVERIFY(web.listen());
    TestProxy proxy;
    QVERIFY(proxy.listen());
    proxy.redirect(QStringLiteral("192.0.2.30:8080"), QHostAddress::LocalHost, web.port());
    auto fetch = [](Settings *s, const QString &url) {
        HttpProbe probe(s);
        QByteArray got;
        bool done = false;
        QObject::connect(&probe, &HttpProbe::fetched, &probe, [&](const HttpProbeResult &r, const QByteArray &d) {
            got = d;
            done = true;
            if (!r.ok) qInfo() << "fetch" << url << "failed:" << r.error;
        });
        probe.fetchSmall(url);
        (void)QTest::qWaitFor([&] { return done; }, 10000);
        return got;
    };
    for (const QString &type : {QStringLiteral("socks5"), QStringLiteral("http")}) {
        SimpleSettings s;
        useProxy(s, proxy, type);
        QCOMPARE(fetch(&s, QStringLiteral("http://192.0.2.30:8080/file.bin")), QByteArray("hello"));
    }
    QCOMPARE(proxy.countTarget(QStringLiteral("192.0.2.30:8080")), 2);
    QCOMPARE(web.tcpCount(), 2);
    if (!haveSecondLoopback()) QSKIP("no second loopback address on this system");
    SimpleSettings b;
    b.setValue(QString(keys::connection::BindInterface), kAdapter);
    QCOMPARE(fetch(&b, QStringLiteral("http://127.0.0.1:%1/file.bin").arg(web.port())), QByteArray("hello"));
    QCOMPARE(web.tcpCount(kAdapter), 1);
    b.setValue(QString(keys::connection::BindInterface), kMissing);
    np::clearAdapterCache();
    QVERIFY(fetch(&b, QStringLiteral("http://127.0.0.1:%1/file.bin").arg(web.port())).isEmpty());
    QCOMPARE(web.tcpCount(), 3);
    // IPv6 off: the relay keeps HTTP to IPv4 ("localhost" has ::1 too)
    SimpleSettings v4;
    v4.setValue(QString(keys::connection::Ipv6), false);
    QCOMPARE(fetch(&v4, QStringLiteral("http://localhost:%1/x").arg(web.port())), QByteArray("hello"));
    QCOMPARE(web.tcpCount(QStringLiteral("127.0.0.1")), 3);
}

// ------------------------------------------------------------------------------------------------ web indexes, update check
void TstNetPolicy::webAndUpdateCheckThroughProxy()
{
    Canary web;
    serveHello(web);
    QVERIFY(web.listen());
    TestProxy proxy;
    QVERIFY(proxy.listen());
    proxy.redirect(QStringLiteral("192.0.2.40:80"), QHostAddress::LocalHost, web.port());
    SimpleSettings s;
    s.setValue(QStringLiteral("web/probeOnStart"), false);
    useProxy(s, proxy);
    auto get = [](QNetworkAccessManager *nam, const QString &url) {
        QNetworkReply *r = nam->get(QNetworkRequest(QUrl(url)));
        QSignalSpy done(r, &QNetworkReply::finished);
        done.wait(8000);
        const QByteArray b = r->readAll();
        r->deleteLater();
        return b;
    };
    // the web plugin's own manager
    QTemporaryDir dir;
    auto *w = createWebSearchPlugin();
    w->init(contextFor(&s, dir.path(), "web"));
    auto *nam = w->findChild<QNetworkAccessManager *>();
    QVERIFY(nam);
    QCOMPARE(get(nam, QStringLiteral("http://192.0.2.40/index")), QByteArray("hello"));
    // the update check's
    QNetworkAccessManager un;
    applyUpdateProxy(&un, &s);
    QCOMPARE(get(&un, QStringLiteral("http://192.0.2.40/releases")), QByteArray("hello"));
    QCOMPARE(proxy.countTarget(QStringLiteral("192.0.2.40:80")), 2);
    // a proxy chosen without an address: both fail, nothing goes direct
    s.setValue(QString(keys::connection::ProxyHost), QString());
    applyUpdateProxy(&un, &s);
    QVERIFY(get(&un, QStringLiteral("http://127.0.0.1:%1/direct").arg(web.port())).isEmpty());
    QVERIFY(get(nam, QStringLiteral("http://127.0.0.1:%1/direct").arg(web.port())).isEmpty());
    QCOMPARE(web.tcpCount(), 2);
    delete w;
}

// ------------------------------------------------------------------------------------------------ overlay, Rats
void TstNetPolicy::overlayAndRatsWithProxyOrAdapter()
{
    qputenv("MX_OVERLAY_DISCOVERY", "0");
    TestProxy proxy;
    QVERIFY(proxy.listen());
    auto run = [](SimpleSettings &s, NetworkPlugin *p, const QString &profile, const char *net) {
        s.setValue(QString(keys::connection::TcpPort), 0);
        s.setValue(QStringLiteral("rats/listenPort"), 0);
        p->init(contextFor(&s, profile, net));
        p->start();
        QTest::qWait(300);
        const NetworkStatus st = p->status();
        p->stop();
        delete p;
        return st;
    };
    {
        QTemporaryDir dir;
        SimpleSettings s;
        useProxy(s, proxy);
        const NetworkStatus o = run(s, createOverlayPlugin(), dir.path(), "overlay");
        QCOMPARE(o.state, ConnState::Error);
        QVERIFY2(o.statusText.contains(QLatin1String("cannot run through a proxy")), qPrintable(o.statusText));
        const NetworkStatus r = run(s, createRatsSearchPlugin(), dir.path(), "rats");
        QCOMPARE(r.state, ConnState::Error);
        QVERIFY2(r.statusText.contains(QLatin1String("cannot run through a proxy")), qPrintable(r.statusText));
        QCOMPARE(proxy.clients, 0);
    }
    {   // an adapter that does not carry the internet route: librats cannot bind its outgoing links
        QTemporaryDir dir;
        SimpleSettings s;
        s.setValue(QString(keys::connection::BindInterface), haveSecondLoopback() ? kAdapter : QStringLiteral("127.0.0.1"));
        const NetworkStatus o = run(s, createOverlayPlugin(), dir.path(), "overlay");
        if (np::localAddressTowards(QHostAddress(QStringLiteral("198.51.100.1"))).isNull())
            QSKIP("no internet route on this system: the route rule cannot be shown");
        QCOMPARE(o.state, ConnState::Error);
        QVERIFY2(o.statusText.contains(QLatin1String("internet route")), qPrintable(o.statusText));
        s.setValue(QString(keys::connection::BindInterface), kMissing);
        np::clearAdapterCache();
        const NetworkStatus r = run(s, createRatsSearchPlugin(), dir.path(), "rats");
        QCOMPARE(r.state, ConnState::Error);
        QVERIFY2(r.statusText.contains(QLatin1String("Waiting for the network adapter")), qPrintable(r.statusText));
    }
}

// ------------------------------------------------------------------------------------------------ NetworkManager
void TstNetPolicy::managerStopsAndRestartsWithTheAdapter()
{
    bool there = true;
    np::setAdapterResolver([&there](const QString &w) {
        np::Adapter a;
        a.wanted = w;
        a.name = w;
        a.present = there;
        if (there) a.addresses << QHostAddress(QStringLiteral("10.77.0.2"));
        return a;
    });
    SimpleSettings s;
    s.setValue(QString(keys::connection::BindInterface), QStringLiteral("vpn0"));
    auto *ed2k = new test::FakePlugin(Net::ED2K, NetworkPlugin::CanSearch);
    auto *bt = new test::FakePlugin(Net::BitTorrent, NetworkPlugin::CanSearch);
    auto *kad = new test::FakePlugin(Net::Kad, NetworkPlugin::CanSearch);
    NetworkManager nm(&s, {ed2k, bt, kad});
    nm.policyMonitor()->setIntervalMs(100);
    QTemporaryDir dir;
    nm.initPlugins(dir.path(), nullptr, nullptr);
    nm.startNetworks(true);
    QCOMPARE(ed2k->startCount, 1);
    QCOMPARE(bt->startCount, 1);
    // the VPN goes down: every network stops, the status says why
    there = false;
    QTRY_COMPARE_WITH_TIMEOUT(ed2k->stopCount, 1, 3000);
    QCOMPARE(bt->stopCount, 1);
    QCOMPARE(kad->stopCount, 1);
    QVERIFY(!nm.isRunning(Net::ED2K));
    QCOMPARE(nm.status(Net::ED2K).state, ConnState::Error);
    QVERIFY(nm.status(Net::ED2K).statusText.contains(QLatin1String("Waiting for the network adapter \"vpn0\"")));
    QVERIFY(nm.overallDetail().contains(QLatin1String("vpn0")));
    // Connect does not start them either
    nm.connectNetwork(Net::ED2K);
    QCOMPARE(ed2k->startCount, 1);
    // it is back: they start by themselves
    there = true;
    QTRY_COMPARE_WITH_TIMEOUT(ed2k->startCount, 2, 3000);
    QCOMPARE(bt->startCount, 2);
    QVERIFY(nm.isRunning(Net::ED2K));
    QVERIFY(nm.blockedReason(Net::ED2K).isEmpty());
    // a proxy is set at run time: Kad stops, the others restart on the new policy
    s.setValue(QString(keys::connection::BindInterface), QString());
    s.setValue(QString(keys::connection::ProxyType), QStringLiteral("socks5"));
    s.setValue(QString(keys::connection::ProxyHost), QStringLiteral("127.0.0.1"));
    s.setValue(QString(keys::connection::ProxyPort), 1080);
    QTRY_VERIFY_WITH_TIMEOUT(!nm.isRunning(Net::Kad), 3000);
    QVERIFY(nm.status(Net::Kad).statusText.contains(QLatin1String("Off while a proxy is set")));
    QTRY_VERIFY_WITH_TIMEOUT(ed2k->startCount >= 3, 3000);
    QVERIFY(nm.isRunning(Net::ED2K));
    QVERIFY(nm.isRunning(Net::BitTorrent));
    // the proxy is removed: Kad starts again
    s.setValue(QString(keys::connection::ProxyType), QStringLiteral("none"));
    QTRY_VERIFY_WITH_TIMEOUT(nm.isRunning(Net::Kad), 3000);
    // nothing chosen (the default): no timer runs, nothing restarts
    const int starts = ed2k->startCount;
    QTest::qWait(500);
    QCOMPARE(ed2k->startCount, starts);
}

// ------------------------------------------------------------------------------------------------ eD2k obfuscation
// ed2k/obfuscation (supported and asked for) and ed2k/requireObfuscation against a peer that
// offers only the other kind: the download works, is obfuscated, or does not happen at all
void TstNetPolicy::ed2kObfuscationChoices_data()
{
    QTest::addColumn<bool>("seedObf");
    QTest::addColumn<bool>("seedRequire");
    QTest::addColumn<bool>("dlObf");
    QTest::addColumn<bool>("dlRequire");
    QTest::addColumn<int>("expect");   // 0 refused, 1 plain, 2 obfuscated
    QTest::newRow("prefer vs plain-only peer: plain") << false << false << true << false << 1;
    QTest::newRow("require vs plain-only peer: refused") << false << false << true << true << 0;
    QTest::newRow("plain vs peer that requires: refused") << true << true << false << false << 0;
    QTest::newRow("require vs prefer: obfuscated") << true << false << true << true << 2;
    QTest::newRow("off vs off: plain") << false << false << false << false << 1;
}

void TstNetPolicy::ed2kObfuscationChoices()
{
    QFETCH(bool, seedObf);
    QFETCH(bool, seedRequire);
    QFETCH(bool, dlObf);
    QFETCH(bool, dlRequire);
    QFETCH(int, expect);
    struct Peer {
        SimpleSettings settings;
        mxtest::FileLibrary lib;
        QTemporaryDir dir;
        ed2k::Ed2kPlugin *plugin = nullptr;
        Peer(bool obf, bool req)
        {
            ed2kSettings(settings);
            settings.setValue(QStringLiteral("ed2k/autoConnect"), false);
            settings.setValue(QStringLiteral("ed2k/udpPing"), false);
            settings.setValue(QStringLiteral("ed2k/udpSearch"), false);
            settings.setValue(QStringLiteral("ed2k/udpSources"), false);
            settings.setValue(QStringLiteral("ed2k/obfuscation"), obf);
            settings.setValue(QStringLiteral("ed2k/requireObfuscation"), req);
            plugin = new ed2k::Ed2kPlugin;
            plugin->init(mxtest::makeContext(&settings, &lib, nullptr, dir.path()));
            plugin->start();
        }
        ~Peer() { plugin->stop(); delete plugin; }
    };
    Peer seed(seedObf, seedRequire), dl(dlObf, dlRequire);
    QVERIFY(seed.plugin->core()->tcpPort() != 0);
    const QString src = seed.dir.path() + QStringLiteral("/obf.bin");
    {
        QFile f(src);
        QVERIFY(f.open(QIODevice::WriteOnly));
        QRandomGenerator g(3);
        QByteArray b(1500 * 1000, Qt::Uninitialized);
        for (auto &c : b) c = char(g.bounded(256));
        f.write(b);
    }
    const SharedFile sf = seed.lib.addFile(src);
    seed.plugin->setSharedFiles(seed.lib.sharedFiles());
    SearchSource ss;
    ss.net = Net::ED2K;
    // the peer's own connect options, as a server or Kad would report them (1 supports, 2 asks, 4 requires)
    const int crypt = (seedObf ? 1 : 0) | (seedObf ? 2 : 0) | (seedRequire ? 4 : 0);
    ss.uri = QStringLiteral("ed2k-peer://127.0.0.1:%1?uid=%2&crypt=%3").arg(seed.plugin->core()->tcpPort())
                 .arg(QString::fromLatin1(seed.plugin->core()->userHash().toHex())).arg(crypt);
    RangeSource *rs = dl.plugin->openSource(QStringLiteral("dl"), sf.hashes, sf.size, ss);
    QVERIFY(rs);
    const QString out = dl.dir.path() + QStringLiteral("/out.bin");
    mxtest::SimpleDownloader d(out, sf.size, sf.hashes.ed2k);
    d.addSource(rs);
    if (expect == 0) {
        QTest::qWait(5000);
        QVERIFY(!d.done());
        QCOMPARE(d.receivedBytes(), qint64(0));
    } else {
        QTRY_VERIFY_WITH_TIMEOUT(d.done(), 30000);
        QCOMPARE(rs->descriptor().extra.value(QStringLiteral("obfuscated")).toBool(), expect == 2);
    }
    rs->close();
    delete rs;
}

// ------------------------------------------------------------------------------------------------ proxy names, login
// connection/proxyHostnames: the name goes to the proxy (no lookup here) or is looked up here;
// connection/proxyAuth / proxyUser / proxyPassword: the login the proxy asks for
void TstNetPolicy::proxyNamesAndCredentials()
{
    Canary web;
    serveHello(web);
    QVERIFY(web.listen());
    TestProxy proxy;
    proxy.user = QStringLiteral("alice");
    proxy.password = QStringLiteral("s3cret");
    QVERIFY(proxy.listen());
    proxy.redirect(QStringLiteral("files.policy.invalid:8080"), QHostAddress::LocalHost, web.port());
    auto fetch = [](Settings *s, const QString &url) {
        HttpProbe probe(s);
        QByteArray got;
        bool done = false;
        QObject::connect(&probe, &HttpProbe::fetched, &probe, [&](const HttpProbeResult &, const QByteArray &d) { got = d; done = true; });
        probe.fetchSmall(url);
        (void)QTest::qWaitFor([&] { return done; }, 10000);
        return got;
    };
    SimpleSettings s;
    useProxy(s, proxy);
    s.setValue(QString(keys::connection::ProxyAuth), true);
    s.setValue(QString(keys::connection::ProxyUser), QStringLiteral("alice"));
    s.setValue(QString(keys::connection::ProxyPassword), QStringLiteral("s3cret"));
    s.setValue(QString(keys::connection::ProxyHostnames), true);
    // a name only the proxy knows (.invalid never resolves here): it reaches the proxy as a name
    QCOMPARE(fetch(&s, QStringLiteral("http://files.policy.invalid:8080/a")), QByteArray("hello"));
    QCOMPARE(proxy.countTarget(QStringLiteral("files.policy.invalid:8080")), 1);
    // names looked up here: the lookup fails, the proxy is never asked for that name
    s.setValue(QString(keys::connection::ProxyHostnames), false);
    QVERIFY(fetch(&s, QStringLiteral("http://files.policy.invalid:8080/b")).isEmpty());
    QCOMPARE(proxy.countTarget(QStringLiteral("files.policy.invalid")), 1);
    // a wrong password: the proxy refuses, nothing is fetched (and nothing goes direct)
    s.setValue(QString(keys::connection::ProxyHostnames), true);
    s.setValue(QString(keys::connection::ProxyPassword), QStringLiteral("wrong"));
    QVERIFY(fetch(&s, QStringLiteral("http://files.policy.invalid:8080/c")).isEmpty());
    // no login at all against a proxy that wants one: refused too
    s.setValue(QString(keys::connection::ProxyAuth), false);
    QVERIFY(fetch(&s, QStringLiteral("http://files.policy.invalid:8080/d")).isEmpty());
    QCOMPARE(web.tcpCount(), 1);
}

// ------------------------------------------------------------------------------------------------ eD2k pacing
// ed2k/maxHalfOpen, ed2k/maxConnectsPer5s, ed2k/maxConnections and connection/connectTimeoutSec,
// seen at a proxy that takes every connection and never answers (peers that do not answer)
void TstNetPolicy::ed2kConnectionPacing()
{
    auto run = [](int halfOpen, int per5s, int maxConn, int timeoutSec, TestProxy &proxy, int waitMs) {
        QTemporaryDir dir;
        SimpleSettings s;
        ed2kSettings(s);
        s.setValue(QStringLiteral("ed2k/autoConnect"), false);
        s.setValue(QStringLiteral("ed2k/maxHalfOpen"), halfOpen);
        s.setValue(QStringLiteral("ed2k/maxConnectsPer5s"), per5s);
        s.setValue(QStringLiteral("ed2k/maxConnections"), maxConn);
        s.setValue(QString(keys::connection::ConnectTimeoutSec), timeoutSec);
        useProxy(s, proxy);
        auto *p = new ed2k::Ed2kPlugin;
        p->init(contextFor(&s, dir.path(), "ed2k"));
        p->start();
        int made = 0;
        for (int i = 0; i < 30; ++i) {
            ed2k::PeerDescriptor d;
            d.ip = quint32(0x0A000000u + quint32(i + 1));   // 1.0.0.10 + i in eD2k order: never reached
            d.port = 4662;
            if (p->core()->connectPeer(d, false)) ++made;
        }
        QTest::qWait(waitMs);
        p->stop();
        delete p;
        return made;
    };
    {   // at most 3 attempts at once
        TestProxy proxy;
        proxy.stall = true;
        QVERIFY(proxy.listen());
        QCOMPARE(run(3, 200, 2000, 300, proxy, 2500), 30);
        QCOMPARE(proxy.maxOpenClients, 3);
    }
    {   // at most 4 new connections in five seconds
        TestProxy proxy;
        proxy.stall = true;
        QVERIFY(proxy.listen());
        run(100, 4, 2000, 300, proxy, 3000);
        QCOMPARE(proxy.clients, 4);
    }
    {   // the connection limit (10 at least): no more peers are taken
        TestProxy proxy;
        proxy.stall = true;
        QVERIFY(proxy.listen());
        QCOMPARE(run(100, 200, 10, 300, proxy, 500), 10);
    }
    {   // a 5 s time out closes the attempts after about 5 s; 30 s keeps them
        TestProxy fast, slow;
        fast.stall = slow.stall = true;
        QVERIFY(fast.listen());
        QVERIFY(slow.listen());
        run(2, 200, 2000, 5, fast, 9000);
        QVERIFY(!fast.closedAfterMs.isEmpty());
        QVERIFY2(fast.closedAfterMs.first() < 8500, qPrintable(QString::number(fast.closedAfterMs.first())));
        QVERIFY(fast.clients >= 3);   // the slots were freed for the next ones
        run(2, 200, 2000, 30, slow, 9000);
        QCOMPARE(slow.clients, 2);   // still waiting for the first two when stopped
    }
}

// ed2k/compressUploads: compressible data is sent packed, or not
void TstNetPolicy::ed2kCompressUploads()
{
    for (const bool on : {true, false}) {
        SimpleSettings ss, ds;
        mxtest::FileLibrary lib, dlib;
        QTemporaryDir sdir, ddir;
        for (SimpleSettings *s : {&ss, &ds}) {
            ed2kSettings(*s);
            s->setValue(QStringLiteral("ed2k/autoConnect"), false);
            s->setValue(QStringLiteral("ed2k/compressUploads"), on);
        }
        ed2k::Ed2kPlugin seed, dl;
        seed.init(mxtest::makeContext(&ss, &lib, nullptr, sdir.path()));
        dl.init(mxtest::makeContext(&ds, &dlib, nullptr, ddir.path()));
        seed.start();
        dl.start();
        const QString src = sdir.path() + QStringLiteral("/text.txt");
        {
            QFile f(src);
            QVERIFY(f.open(QIODevice::WriteOnly));
            for (int i = 0; i < 40000; ++i) f.write("line of very compressible text for the eD2k packer\n");
        }
        const SharedFile sf = lib.addFile(src);
        seed.setSharedFiles(lib.sharedFiles());
        SearchSource src2;
        src2.net = Net::ED2K;
        src2.uri = QStringLiteral("ed2k-peer://127.0.0.1:%1?uid=%2&crypt=3").arg(seed.core()->tcpPort())
                       .arg(QString::fromLatin1(seed.core()->userHash().toHex()));
        RangeSource *rs = dl.openSource(QStringLiteral("dl"), sf.hashes, sf.size, src2);
        QVERIFY(rs);
        mxtest::SimpleDownloader d(ddir.path() + QStringLiteral("/out.txt"), sf.size, sf.hashes.ed2k);
        d.addSource(rs);
        QTRY_VERIFY_WITH_TIMEOUT(d.done(), 30000);
        if (on) QVERIFY(seed.core()->uploads()->packedBlocks() > 0);
        else QCOMPARE(seed.core()->uploads()->packedBlocks(), 0);
        rs->close();
        delete rs;
        seed.stop();
        dl.stop();
    }
}

QTEST_GUILESS_MAIN(TstNetPolicy)
#include "tst_netpolicy.moc"
