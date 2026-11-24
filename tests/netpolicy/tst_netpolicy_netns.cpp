// SPDX-License-Identifier: GPL-3.0-or-later
// [net-2] The chosen adapter going away and coming back, with a real network device: runs inside
// an unprivileged network namespace (netns_adapter.sh: `unshare -rn`) where a dummy device "vpn0"
// (10.77.0.2) carries the default route and "lan0" (10.88.0.2) is the other line. The test removes
// and adds vpn0's address itself (it is root in its own namespace).
//  * eD2k (real plugin, through NetworkManager): connects from 10.77.0.2 only; when the address
//    goes, eD2k stops and says why, and no connection or listener uses another address; when it
//    is back, eD2k starts again by itself and connects from 10.77.0.2.
//  * the OctoMX network (real plugin): with the internet route on vpn0 it runs, and its listener
//    is on 10.77.0.2 only (socket statistics: `ss`).
// Without MX_NETNS=1 (not in the namespace) every case is skipped.
#include "net_test_servers.h"

#include "ed2k_plugin.h"
#include "overlay_plugin.h"

#include "core/network_manager.h"
#include "mx/net_policy.h"
#include "mx/settings_keys.h"
#include "mx/simple_settings.h"

#include <QtCore/QProcess>
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>

using namespace mx;
using nettest::Canary;
namespace np = mx::netpolicy;

namespace {

bool inNetns() { return qgetenv("MX_NETNS") == "1"; }

int run(const QString &prog, const QStringList &args, QByteArray *out = nullptr)
{
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(prog, args);
    if (!p.waitForFinished(10000)) return -1;
    if (out) *out = p.readAll();
    return p.exitCode();
}

// local addresses of this process's TCP sockets (listening and connected) from `ss`
QStringList tcpLocalAddresses(bool listening)
{
    QByteArray o;
    run(QStringLiteral("ss"), {listening ? QStringLiteral("-ltnH") : QStringLiteral("-tnH")}, &o);
    QStringList out;
    for (const QByteArray &line : o.split('\n')) {
        const QList<QByteArray> f = line.simplified().split(' ');
        if (f.size() >= 4) out << QString::fromLatin1(f.at(3));
    }
    return out;
}

PluginContext ctxFor(Settings *s, const QString &profile, const char *net)
{
    PluginContext c;
    c.settings = s;
    c.profileDir = profile;
    c.dataDir = profile + QLatin1Char('/') + QLatin1String(net);
    QDir().mkpath(c.dataDir);
    c.clientName = QStringLiteral("OctoMX");
    c.clientVersion = QStringLiteral(MX_VERSION_STRING);
    c.coreMapsPorts = true;
    return c;
}

} // namespace

class TstNetPolicyNetns : public QObject {
    Q_OBJECT
private slots:
    void adapterDownAndBack();
    void overlayBoundWhenTheRouteIsTheAdapters();
};

void TstNetPolicyNetns::adapterDownAndBack()
{
    if (!inNetns()) QSKIP("needs the network namespace of netns_adapter.sh");
    Canary server;   // the eD2k server, on the other line
    QVERIFY(server.listen(QHostAddress(QStringLiteral("10.88.0.2"))));
    SimpleSettings s;
    s.setValue(QStringLiteral("ed2k/tcpPort"), 0);
    s.setValue(QStringLiteral("ed2k/udpPort"), 0);
    s.setValue(QStringLiteral("ed2k/autoConnect"), true);
    s.setValue(QStringLiteral("ed2k/autoUpdateServerList"), false);
    s.setValue(QStringLiteral("ed2k/allowLan"), true);
    s.setValue(QStringLiteral("kad/enabled"), false);
    for (const char *k : {"bt/enabled", "gnutella/enabled", "http/enabled", "overlay/enabled", "web/enabled", "rats/enabled"})
        s.setValue(QLatin1String(k), false);
    s.setValue(QString(keys::connection::BindInterface), QStringLiteral("vpn0"));
    QTemporaryDir dir;
    NetworkManager nm(&s, createEd2kPlugins());
    nm.policyMonitor()->setIntervalMs(200);
    nm.initPlugins(dir.path(), nullptr, nullptr);
    NetworkPlugin *ed2k = nm.plugin(Net::ED2K);
    QVERIFY(ed2k);
    for (int i = 0; i < 3; ++i) ed2k->addServer(QStringLiteral("10.88.0.2:%1").arg(server.port() + (i ? 1000 + i : 0)), QStringLiteral("s%1").arg(i));
    nm.startNetworks(true);
    QTRY_VERIFY_WITH_TIMEOUT(server.tcpCount() >= 1, 20000);
    QCOMPARE(server.tcpCount(QStringLiteral("10.77.0.2")), server.tcpCount());
    const QString mine = server.endpoint();   // the test's own server is the only other listener
    for (const QString &a : tcpLocalAddresses(true))
        if (a != mine) QVERIFY2(!a.startsWith(QLatin1String("0.0.0.0")) && !a.startsWith(QLatin1String("10.88.")), qPrintable(a));
    QVERIFY(!tcpLocalAddresses(true).filter(QStringLiteral("10.77.0.2:")).isEmpty());   // eD2k listens on the adapter

    // the VPN goes down
    QCOMPARE(run(QStringLiteral("ip"), {QStringLiteral("addr"), QStringLiteral("del"), QStringLiteral("10.77.0.2/24"), QStringLiteral("dev"), QStringLiteral("vpn0")}), 0);
    QTRY_VERIFY_WITH_TIMEOUT(!nm.isRunning(Net::ED2K), 5000);
    QVERIFY(nm.status(Net::ED2K).statusText.contains(QLatin1String("Waiting for the network adapter \"vpn0\"")));
    const int before = server.tcpCount();
    QTest::qWait(4000);   // eD2k would reconnect within this time
    QCOMPARE(server.tcpCount(), before);
    for (const QString &a : tcpLocalAddresses(false))   // connected sockets: none from the other line
        if (!a.startsWith(mine)) QVERIFY2(!a.startsWith(QLatin1String("10.88.")), qPrintable(a));
    QCOMPARE(tcpLocalAddresses(true).filter(QStringLiteral("10.88.")), QStringList{mine});

    // it is back: eD2k starts by itself and connects from the adapter again
    QCOMPARE(run(QStringLiteral("ip"), {QStringLiteral("addr"), QStringLiteral("add"), QStringLiteral("10.77.0.2/24"), QStringLiteral("dev"), QStringLiteral("vpn0")}), 0);
    run(QStringLiteral("ip"), {QStringLiteral("route"), QStringLiteral("replace"), QStringLiteral("default"), QStringLiteral("dev"), QStringLiteral("vpn0")});
    QTRY_VERIFY_WITH_TIMEOUT(nm.isRunning(Net::ED2K), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(server.tcpCount() > before, 20000);
    QCOMPARE(server.tcpCount(QStringLiteral("10.77.0.2")), server.tcpCount());
    nm.stopAll();
}

void TstNetPolicyNetns::overlayBoundWhenTheRouteIsTheAdapters()
{
    if (!inNetns()) QSKIP("needs the network namespace of netns_adapter.sh");
    qputenv("MX_OVERLAY_DISCOVERY", "0");
    SimpleSettings s;
    s.setValue(QString(keys::connection::BindInterface), QStringLiteral("vpn0"));
    s.setValue(QString(keys::connection::TcpPort), 46699);
    QTemporaryDir dir;
    NetworkPlugin *o = createOverlayPlugin();
    o->init(ctxFor(&s, dir.path(), "overlay"));
    o->start();
    QTRY_VERIFY_WITH_TIMEOUT(o->status().state != ConnState::Error && o->status().listenPort > 0, 10000);
    QCOMPARE(o->status().listenPort, 46699);   // connection/tcpPort
    const QStringList l = tcpLocalAddresses(true).filter(QStringLiteral(":%1").arg(o->status().listenPort));
    QVERIFY2(!l.isEmpty(), "no listener");
    for (const QString &a : l) QCOMPARE(a, QStringLiteral("10.77.0.2:%1").arg(o->status().listenPort));
    o->stop();
    // the route moves to the other line: the overlay does not start
    run(QStringLiteral("ip"), {QStringLiteral("route"), QStringLiteral("replace"), QStringLiteral("default"), QStringLiteral("dev"), QStringLiteral("lan0")});
    np::clearAdapterCache();
    o->start();
    QTRY_COMPARE_WITH_TIMEOUT(o->status().state, ConnState::Error, 5000);
    QVERIFY2(o->status().statusText.contains(QLatin1String("internet route")), qPrintable(o->status().statusText));
    o->stop();
    run(QStringLiteral("ip"), {QStringLiteral("route"), QStringLiteral("replace"), QStringLiteral("default"), QStringLiteral("dev"), QStringLiteral("vpn0")});
    delete o;
}

QTEST_GUILESS_MAIN(TstNetPolicyNetns)
#include "tst_netpolicy_netns.moc"
