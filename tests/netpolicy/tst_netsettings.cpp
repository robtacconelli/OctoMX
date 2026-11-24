// SPDX-License-Identifier: GPL-3.0-or-later
// [net-2] The long tail of the network settings: each check changes a key and observes what the
// network does (libtorrent's settings, a listening port, connections and requests that arrive at
// local canaries), never only that something reads the key.
#include "net_test_servers.h"

#include "bt_common.h"
#include "ed2k_core.h"
#include "ed2k_plugin_impl.h"
#include "ed2k_server.h"
#include "gnut_plugin_impl.h"

#include "mx/net_policy.h"
#include "mx/settings_keys.h"
#include "mx/simple_settings.h"

#include <libtorrent/settings_pack.hpp>

#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QUdpSocket>
#include <QtTest/QtTest>

using namespace mx;
using nettest::Canary;
using sp = lt::settings_pack;

namespace {

PluginContext contextFor(Settings *s, const QString &profile, const char *net)
{
    PluginContext ctx;
    ctx.settings = s;
    ctx.profileDir = profile;
    ctx.dataDir = profile + QLatin1Char('/') + QLatin1String(net);
    QDir().mkpath(ctx.dataDir);
    ctx.clientName = QStringLiteral("OctoMX");
    ctx.clientVersion = QStringLiteral(MX_VERSION_STRING);
    ctx.coreMapsPorts = true;
    return ctx;
}

quint16 freePort()
{
    QTcpServer t;
    t.listen(QHostAddress::LocalHost, 0);
    const quint16 p = t.serverPort();
    QUdpSocket u;
    if (!u.bind(QHostAddress::AnyIPv4, p)) return freePort();
    return p;
}

lt::settings_pack packFor(SimpleSettings &s, bool online = true)
{
    const bt::Config c = bt::Config::fromSettings(&s);
    return bt::makeSettingsPack(c, online, c.listenPort, {}, 0, 0);
}

void ed2kBase(SimpleSettings &s)
{
    s.setValue(QStringLiteral("ed2k/tcpPort"), 0);
    s.setValue(QStringLiteral("ed2k/udpPort"), 0);
    s.setValue(QStringLiteral("ed2k/autoUpdateServerList"), false);
    s.setValue(QStringLiteral("ed2k/allowLan"), true);
    s.setValue(QStringLiteral("ed2k/udpPing"), false);
    s.setValue(QStringLiteral("ed2k/udpSearch"), false);
    s.setValue(QStringLiteral("ed2k/udpSources"), false);
}

} // namespace

class TstNetSettings : public QObject {
    Q_OBJECT
private slots:
    void btSessionSettingsReachLibtorrent();
    void ed2kPortsAndServerChoices();
    void ed2kServerListDownload();
    void ed2kServerAnnouncementsAndReconnect();
    void gnutellaPortUltrapeersAndCaches();
};

// ------------------------------------------------------------------------------------------------ BitTorrent
// what libtorrent gets for each key (the session settings the plugin applies, live and at start)
void TstNetSettings::btSessionSettingsReachLibtorrent()
{
    SimpleSettings s;
    s.setValue(QStringLiteral("bt/listenPort"), 51413);
    s.setValue(QStringLiteral("connection/ipv6"), false);
    lt::settings_pack p = packFor(s);
    QCOMPARE(QString::fromStdString(p.get_str(sp::listen_interfaces)), QStringLiteral("0.0.0.0:51413"));
    s.setValue(QStringLiteral("connection/ipv6"), true);
    QCOMPARE(QString::fromStdString(packFor(s).get_str(sp::listen_interfaces)), QStringLiteral("0.0.0.0:51413,[::]:51413"));
    s.setValue(QStringLiteral("connection/bindInterface"), QStringLiteral("127.0.0.1"));
    p = packFor(s);
    QCOMPARE(QString::fromStdString(p.get_str(sp::listen_interfaces)), QStringLiteral("127.0.0.1:51413"));
    QCOMPARE(QString::fromStdString(p.get_str(sp::outgoing_interfaces)), QStringLiteral("127.0.0.1"));
    s.setValue(QStringLiteral("connection/bindInterface"), QStringLiteral("192.0.2.77"));   // missing: offline
    mx::netpolicy::clearAdapterCache();
    p = packFor(s);
    QVERIFY(p.get_str(sp::listen_interfaces).empty());
    QVERIFY(!p.get_bool(sp::enable_dht));
    s.setValue(QStringLiteral("connection/bindInterface"), QString());
    // discovery
    s.setValue(QStringLiteral("bt/dht"), false);
    s.setValue(QStringLiteral("bt/lsd"), false);
    p = packFor(s);
    QVERIFY(!p.get_bool(sp::enable_dht));
    QVERIFY(!p.get_bool(sp::enable_lsd));
    s.setValue(QStringLiteral("bt/dht"), true);
    s.setValue(QStringLiteral("bt/lsd"), true);
    p = packFor(s);
    QVERIFY(p.get_bool(sp::enable_dht));
    QVERIFY(p.get_bool(sp::enable_lsd));
    QVERIFY(!packFor(s, false).get_bool(sp::enable_dht));   // offline: nothing
    s.setValue(QStringLiteral("bt/utp"), false);
    p = packFor(s);
    QVERIFY(!p.get_bool(sp::enable_incoming_utp));
    QVERIFY(!p.get_bool(sp::enable_outgoing_utp));
    s.setValue(QStringLiteral("bt/dhtBootstrapNodes"), QStringList{QStringLiteral("dht.example.org:6881"), QStringLiteral("[2001:db8::1]:7000")});
    QCOMPARE(QString::fromStdString(packFor(s).get_str(sp::dht_bootstrap_nodes)), QStringLiteral("dht.example.org:6881,2001:db8::1:7000"));
    // encryption: prefer / require (RC4 only) / disabled
    s.setValue(QStringLiteral("bt/encryption"), QStringLiteral("forced"));
    p = packFor(s);
    QCOMPARE(p.get_int(sp::out_enc_policy), int(sp::pe_forced));
    QCOMPARE(p.get_int(sp::in_enc_policy), int(sp::pe_forced));
    QCOMPARE(p.get_int(sp::allowed_enc_level), int(sp::pe_rc4));
    s.setValue(QStringLiteral("bt/encryption"), QStringLiteral("disabled"));
    QCOMPARE(packFor(s).get_int(sp::out_enc_policy), int(sp::pe_disabled));
    s.setValue(QStringLiteral("bt/encryption"), QStringLiteral("enabled"));
    QCOMPARE(packFor(s).get_int(sp::allowed_enc_level), int(sp::pe_both));
    // limits
    s.setValue(QStringLiteral("bt/maxConnections"), 77);
    s.setValue(QStringLiteral("bt/maxUploads"), 9);
    s.setValue(QStringLiteral("bt/maxActiveDownloads"), 3);
    s.setValue(QStringLiteral("bt/maxActiveSeeds"), 4);
    p = packFor(s);
    QCOMPARE(p.get_int(sp::connections_limit), 77);
    QCOMPARE(p.get_int(sp::unchoke_slots_limit), 9);
    QCOMPARE(p.get_int(sp::active_downloads), 3);
    QCOMPARE(p.get_int(sp::active_seeds), 4);
    // trackers and identity
    s.setValue(QStringLiteral("bt/announceToAllTrackers"), false);
    s.setValue(QStringLiteral("bt/anonymousMode"), true);
    s.setValue(QStringLiteral("bt/allowMultipleConnectionsPerIp"), true);
    s.setValue(QStringLiteral("bt/validateHttpsTrackers"), false);
    p = packFor(s);
    QVERIFY(!p.get_bool(sp::announce_to_all_trackers));
    QVERIFY(!p.get_bool(sp::announce_to_all_tiers));
    QVERIFY(p.get_bool(sp::anonymous_mode));
    QVERIFY(p.get_bool(sp::allow_multiple_connections_per_ip));
    QVERIFY(!p.get_bool(sp::validate_https_trackers));
    // port mapping: libtorrent's own only when the core does not map (it does in the program), never with a proxy
    bt::Config c = bt::Config::fromSettings(&s);
    s.setValue(QStringLiteral("connection/upnp"), true);
    s.setValue(QStringLiteral("connection/natpmp"), false);
    c = bt::Config::fromSettings(&s);
    p = bt::makeSettingsPack(c, true, 6881, {}, 0, 0);
    QVERIFY(p.get_bool(sp::enable_upnp));
    QVERIFY(!p.get_bool(sp::enable_natpmp));
    c.coreMapsPorts = true;
    QVERIFY(!bt::makeSettingsPack(c, true, 6881, {}, 0, 0).get_bool(sp::enable_upnp));
    // the proxy: every peer through it, names as chosen; SOCKS4 as SOCKS5; no LSD, no mapping
    s.setValue(QStringLiteral("connection/proxyType"), QStringLiteral("socks4"));
    s.setValue(QStringLiteral("connection/proxyHost"), QStringLiteral("10.9.8.7"));
    s.setValue(QStringLiteral("connection/proxyPort"), 1081);
    s.setValue(QStringLiteral("connection/proxyHostnames"), false);
    s.setValue(QStringLiteral("connection/proxyAuth"), true);
    s.setValue(QStringLiteral("connection/proxyUser"), QStringLiteral("me"));
    s.setValue(QStringLiteral("connection/proxyPassword"), QStringLiteral("pw"));
    c = bt::Config::fromSettings(&s);
    p = bt::makeSettingsPack(c, true, 6881, {}, 0, 0);
    QCOMPARE(p.get_int(sp::proxy_type), int(sp::socks5_pw));
    // behind a proxy no DHT router name is handed to libtorrent (it would look it up itself)
    QCOMPARE(QString::fromStdString(p.get_str(sp::dht_bootstrap_nodes)), QStringLiteral("2001:db8::1:7000"));
    QCOMPARE(QString::fromStdString(p.get_str(sp::proxy_hostname)), QStringLiteral("10.9.8.7"));
    QCOMPARE(p.get_int(sp::proxy_port), 1081);
    QCOMPARE(QString::fromStdString(p.get_str(sp::proxy_username)), QStringLiteral("me"));
    QVERIFY(p.get_bool(sp::proxy_peer_connections));
    QVERIFY(!p.get_bool(sp::proxy_hostnames));
    QVERIFY(!p.get_bool(sp::enable_lsd));
    QVERIFY(!p.get_bool(sp::enable_upnp));
    s.setValue(QStringLiteral("connection/proxyType"), QStringLiteral("http"));
    s.setValue(QStringLiteral("connection/proxyAuth"), false);
    QCOMPARE(packFor(s).get_int(sp::proxy_type), int(sp::http));
    QVERIFY(!packFor(s).get_bool(sp::enable_dht));            // an HTTP proxy carries no UDP
    QVERIFY(!packFor(s).get_bool(sp::enable_outgoing_utp));
    s.setValue(QStringLiteral("connection/proxyHost"), QString());   // chosen without an address: offline
    QVERIFY(packFor(s).get_str(sp::listen_interfaces).empty());
}

// ------------------------------------------------------------------------------------------------ eD2k
void TstNetSettings::ed2kPortsAndServerChoices()
{
    // ports: the listener and the UDP socket (shared with Kad) on exactly these
    const quint16 tcp = freePort(), udp = freePort();
    {
        QTemporaryDir dir;
        SimpleSettings s;
        ed2kBase(s);
        s.setValue(QStringLiteral("ed2k/tcpPort"), int(tcp));
        s.setValue(QStringLiteral("ed2k/udpPort"), int(udp));
        s.setValue(QStringLiteral("ed2k/autoConnect"), false);
        ed2k::Ed2kPlugin p;
        p.init(contextFor(&s, dir.path(), "ed2k"));
        p.start();
        QCOMPARE(p.core()->tcpPort(), tcp);
        QCOMPARE(p.core()->udpLocalPort(), udp);
        p.stop();
    }
    // ed2k/autoConnect off: no server connection at all; on: one (see tst_netpolicy for the proxy)
    // ed2k/parallelServerConnects: that many servers are tried at the same time
    // ed2k/staticServersOnly: only the static ones. The servers have addresses of a documentation
    // range (a loopback server that is not static is never tried); the test proxy brings them here
    Canary a, b, c, d;
    for (Canary *x : {&a, &b, &c, &d}) QVERIFY(x->listen());   // they accept and never answer the login
    nettest::TestProxy proxy;
    QVERIFY(proxy.listen());
    QList<Canary *> all = {&a, &b, &c, &d};
    for (int i = 0; i < 4; ++i) proxy.redirect(QStringLiteral("192.0.2.%1:4661").arg(31 + i), QHostAddress::LocalHost, all[i]->port());
    auto connectsWith = [&](bool autoConnect, int parallel, bool staticOnly) {
        QTemporaryDir dir;
        SimpleSettings s;
        ed2kBase(s);
        s.setValue(QStringLiteral("ed2k/autoConnect"), autoConnect);
        s.setValue(QStringLiteral("ed2k/parallelServerConnects"), parallel);
        s.setValue(QStringLiteral("ed2k/staticServersOnly"), staticOnly);
        s.setValue(QStringLiteral("ed2k/obfuscation"), false);
        s.setValue(QStringLiteral("connection/proxyType"), QStringLiteral("socks5"));
        s.setValue(QStringLiteral("connection/proxyHost"), QStringLiteral("127.0.0.1"));
        s.setValue(QStringLiteral("connection/proxyPort"), int(proxy.port()));
        // servers are dialled by address; the server.met download (by name) is not a server
        const int before = int(proxy.targets.size());
        auto servers = [&proxy, before] {   // distinct servers dialled since `before`
            QSet<QString> n;
            for (const QString &t : proxy.targets.mid(before))
                if (!QHostAddress(t.section(QLatin1Char(':'), 0, 0)).isNull()) n.insert(t);
            return int(n.size());
        };
        ed2k::Ed2kPlugin p;
        p.init(contextFor(&s, dir.path(), "ed2k"));
        for (int i = 0; i < 4; ++i) {
            const QString addr = QStringLiteral("192.0.2.%1:4661").arg(31 + i);
            if (i < 2) p.addServer(addr, QStringLiteral("static %1").arg(i));
            else p.core()->servers()->addServer(addr, QStringLiteral("not static %1").arg(i), false);
        }
        p.start();
        QTest::qWait(400);   // the first round only (refused servers are replaced later)
        const int n = servers();   // servers tried (the built-in list too)
        p.stop();
        return n;
    };
    QCOMPARE(connectsWith(false, 4, false), 0);
    QCOMPARE(connectsWith(true, 1, false), 1);
    QCOMPARE(connectsWith(true, 3, false), 3);
    QCOMPARE(connectsWith(true, 4, false), 4);
    const int cd = c.tcpCount() + d.tcpCount();
    QVERIFY(connectsWith(true, 4, true) >= 2);
    QCOMPARE(c.tcpCount() + d.tcpCount(), cd);   // the two that are not static: never tried
}

// ed2k/serverListUrl and ed2k/autoUpdateServerList: the list is fetched from the address given
void TstNetSettings::ed2kServerListDownload()
{
    Canary web;
    web.onData = [](QTcpSocket *s) {
        s->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        s->disconnectFromHost();
    };
    QVERIFY(web.listen());
    auto run = [&](bool autoUpdate) {
        QTemporaryDir dir;
        SimpleSettings s;
        ed2kBase(s);
        s.setValue(QStringLiteral("ed2k/autoConnect"), false);
        s.setValue(QStringLiteral("ed2k/autoUpdateServerList"), autoUpdate);
        s.setValue(QStringLiteral("ed2k/serverListUrl"), QStringLiteral("http://127.0.0.1:%1/server.met").arg(web.port()));
        // through the test proxy, so the built-in fallback lists (host names on the internet) are never
        // fetched: the proxy reaches only addresses, here the local web server
        nettest::TestProxy proxy;
        proxy.listen();
        s.setValue(QStringLiteral("connection/proxyType"), QStringLiteral("socks5"));
        s.setValue(QStringLiteral("connection/proxyHost"), QStringLiteral("127.0.0.1"));
        s.setValue(QStringLiteral("connection/proxyPort"), int(proxy.port()));
        s.setValue(QStringLiteral("connection/proxyHostnames"), true);
        ed2k::Ed2kPlugin p;
        p.init(contextFor(&s, dir.path(), "ed2k"));
        for (int i = 1; i <= 3; ++i) p.addServer(QStringLiteral("192.0.2.%1:4661").arg(i), QStringLiteral("x"));
        const int before = web.tcpCount();
        p.start();
        QTest::qWait(1500);
        p.stop();
        return web.tcpCount() - before;
    };
    QCOMPARE(run(false), 0);
    QVERIFY(run(true) >= 1);
    QVERIFY(web.received.contains("GET /server.met"));
}

// a minimal eD2k server: answers the login with an ID and announces two other servers
namespace {
QByteArray ed2kPacket(quint8 op, const QByteArray &payload)
{
    QByteArray p;
    p.append(char(0xE3));
    const quint32 n = quint32(payload.size() + 1);
    for (int i = 0; i < 4; ++i) p.append(char((n >> (8 * i)) & 0xFF));
    p.append(char(op));
    p.append(payload);
    return p;
}
void fakeEd2kServer(Canary &c, bool closeAfter)
{
    c.onData = [closeAfter](QTcpSocket *s) {
        if (s->property("answered").toBool()) return;
        s->setProperty("answered", true);
        QByteArray id("\x0d\x0c\x0b\x0a\x00\x00\x00\x00", 8);   // high ID, no TCP flags
        QByteArray list;
        list.append(char(2));
        list.append(QByteArray("\xc6\x33\x64\x05\x35\x12", 6));   // 198.51.100.5:4661
        list.append(QByteArray("\xc6\x33\x64\x06\x35\x12", 6));   // 198.51.100.6:4661
        s->write(ed2kPacket(0x40, id));      // OP_IDCHANGE
        s->write(ed2kPacket(0x32, list));    // OP_SERVERLIST
        if (closeAfter) QTimer::singleShot(500, s, [s] { s->disconnectFromHost(); });
    };
}
} // namespace

// ed2k/addServersFromServer: the announced servers join the list or not; ed2k/reconnect: after the
// server closes, OctoMX connects again (after its 10 s pause) or stays off
void TstNetSettings::ed2kServerAnnouncementsAndReconnect()
{
    for (const bool on : {true, false}) {
        Canary server;
        fakeEd2kServer(server, true);
        QVERIFY(server.listen());
        QTemporaryDir dir;
        SimpleSettings s;
        ed2kBase(s);
        s.setValue(QStringLiteral("ed2k/autoConnect"), true);
        s.setValue(QStringLiteral("ed2k/addServersFromServer"), on);
        s.setValue(QStringLiteral("ed2k/reconnect"), on);
        s.setValue(QStringLiteral("ed2k/obfuscation"), false);
        ed2k::Ed2kPlugin p;
        p.init(contextFor(&s, dir.path(), "ed2k"));
        p.addServer(QStringLiteral("127.0.0.1:%1").arg(server.port()), QStringLiteral("fake"));
        p.addServer(QStringLiteral("127.0.0.1:1"), QStringLiteral("dead 1"));
        p.addServer(QStringLiteral("127.0.0.1:2"), QStringLiteral("dead 2"));
        const int before = int(p.servers().size());
        p.start();
        QTRY_VERIFY_WITH_TIMEOUT(server.tcpCount() >= 1, 10000);
        if (on) QTRY_COMPARE_WITH_TIMEOUT(int(p.servers().size()), before + 2, 5000);
        else { QTest::qWait(1500); QCOMPARE(int(p.servers().size()), before); }
        // the server closed after half a second; with reconnect OctoMX connects again
        const int first = server.tcpCount();
        const bool again = QTest::qWaitFor([&] { return server.tcpCount() > first; }, on ? 25000 : 14000);
        QCOMPARE(again, on);
        p.stop();
    }
}

// ------------------------------------------------------------------------------------------------ Gnutella
void TstNetSettings::gnutellaPortUltrapeersAndCaches()
{
    // gnutella/port: the listener (and UDP) on it
    const quint16 port = freePort();
    {
        QTemporaryDir dir;
        SimpleSettings s;
        s.setValue(QStringLiteral("gnutella/port"), int(port));
        s.setValue(QStringLiteral("gnutella/autoBootstrap"), false);
        s.setValue(QStringLiteral("gnutella/bootstrapHosts"), QStringList{QStringLiteral("192.0.2.1:1")});
        auto *p = new gnut::GnutellaPlugin;
        p->init(contextFor(&s, dir.path(), "gnutella"));
        p->start();
        QTRY_VERIFY_WITH_TIMEOUT(p->listenPort() != 0, 5000);
        QCOMPARE(p->listenPort(), port);
        QCOMPARE(p->findChild<QUdpSocket *>()->localPort(), port);
        // connection/firewalled and connection/acceptIncoming: what Gnutella tells others about us
        s.setValue(QStringLiteral("connection/firewalled"), QStringLiteral("open"));
        QVERIFY(!p->status().firewalled);
        s.setValue(QStringLiteral("connection/acceptIncoming"), false);
        QVERIFY(p->status().firewalled);
        s.setValue(QStringLiteral("connection/acceptIncoming"), true);
        s.setValue(QStringLiteral("connection/firewalled"), QStringLiteral("firewalled"));
        QVERIFY(p->status().firewalled);
        p->stop();
        delete p;
    }
    // gnutella/autoBootstrap: on, the web caches are asked (here through the test proxy, which
    // refuses everything outside this computer: the built-in caches on the internet come first);
    // off, none is asked
    nettest::TestProxy proxy;
    QVERIFY(proxy.listen());
    auto run = [&](bool autoBootstrap) {
        QTemporaryDir dir;
        SimpleSettings s;
        s.setValue(QStringLiteral("gnutella/port"), 0);
        s.setValue(QStringLiteral("gnutella/autoBootstrap"), autoBootstrap);
        s.setValue(QStringLiteral("gnutella/bootstrapHosts"), QStringList());
        s.setValue(QStringLiteral("connection/proxyType"), QStringLiteral("socks5"));
        s.setValue(QStringLiteral("connection/proxyHost"), QStringLiteral("127.0.0.1"));
        s.setValue(QStringLiteral("connection/proxyPort"), int(proxy.port()));
        const int before = int(proxy.targets.size());
        auto *p = new gnut::GnutellaPlugin;
        p->init(contextFor(&s, dir.path(), "gnutella"));
        p->start();
        if (autoBootstrap) (void)QTest::qWaitFor([&] { return int(proxy.targets.size()) > before; }, 10000);
        else QTest::qWait(3000);
        p->stop();
        delete p;
        int caches = 0;
        for (const QString &t : proxy.targets.mid(before)) if (t.endsWith(QLatin1String(":80")) || t.contains(QLatin1String(":3709"))) ++caches;
        return caches;
    };
    QCOMPARE(run(false), 0);
    QVERIFY(run(true) >= 1);
}

QTEST_GUILESS_MAIN(TstNetSettings)
#include "tst_netsettings.moc"
