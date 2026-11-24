// SPDX-License-Identifier: GPL-3.0-or-later
// [net-2] BitTorrent under the connection policy, observed on the wire:
//  * proxy: the tracker and the peers are asked for through the proxy (documentation-range
//    addresses that only the proxy can reach), the DHT sends no datagram directly (the test
//    proxy refuses UDP ASSOCIATE, so libtorrent keeps UDP off), nothing listens;
//  * adapter: peers and DHT from 127.0.0.2 only;
//  * "Require encryption": a peer that offers only an encrypted handshake with a plain stream
//    (libtorrent pe_plaintext) or no encryption at all is refused; an RC4 peer works.
#include "net_test_servers.h"
#include "bt_test_util.h"

#include "mx/net_policy.h"

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_info.hpp>

#include <QtNetwork/QUdpSocket>
#include <QtTest/QtTest>

using namespace mx;
using nettest::Canary;
using nettest::TestProxy;
namespace k = mx::keys;

namespace {

const QString kAdapter = QStringLiteral("127.0.0.2");

bool haveSecondLoopback()
{
    QUdpSocket u;
    return u.bind(QHostAddress(kAdapter), 0);
}

QString magnetFor(const QString &hash, const QString &tracker)
{
    return QStringLiteral("magnet:?xt=urn:btih:%1&dn=policy-test&tr=%2").arg(hash, QString::fromLatin1(QUrl::toPercentEncoding(tracker)));
}

// a raw libtorrent seed with its own encryption choice
struct Seed {
    std::unique_ptr<lt::session> ses;
    int port = 0;
    void start(const QString &dir, const QByteArray &torrent, int encLevel, int policy)
    {
        lt::settings_pack p;
        p.set_str(lt::settings_pack::listen_interfaces, "127.0.0.1:0");
        p.set_bool(lt::settings_pack::enable_dht, false);
        p.set_bool(lt::settings_pack::enable_lsd, false);
        p.set_bool(lt::settings_pack::enable_upnp, false);
        p.set_bool(lt::settings_pack::enable_natpmp, false);
        p.set_bool(lt::settings_pack::allow_multiple_connections_per_ip, true);
        p.set_int(lt::settings_pack::in_enc_policy, policy);
        p.set_int(lt::settings_pack::out_enc_policy, policy);
        p.set_int(lt::settings_pack::allowed_enc_level, encLevel);
        p.set_int(lt::settings_pack::alert_mask, 0);
        ses = std::make_unique<lt::session>(p);
        lt::add_torrent_params atp;
        lt::error_code ec;
        atp.ti = std::make_shared<lt::torrent_info>(lt::span<char const>(torrent.constData(), torrent.size()), ec, lt::from_span);
        atp.save_path = QDir::toNativeSeparators(dir).toStdString();
        atp.flags |= lt::torrent_flags::seed_mode;
        ses->add_torrent(atp, ec);
        for (int i = 0; i < 50 && ses->listen_port() == 0; ++i) QTest::qWait(50);
        port = ses->listen_port();
    }
};

} // namespace

class TstNetPolicyBt : public QObject {
    Q_OBJECT
private slots:
    void proxyCarriesTrackerAndPeersAndNoDirectUdp();
    void adapterBindsPeersAndDht();
    void requiredEncryptionRefusesHeaderOnlyPeers();
    void randomPortsAndTorrentLimits();
};

void TstNetPolicyBt::proxyCarriesTrackerAndPeersAndNoDirectUdp()
{
    Canary tracker, peer, dht;
    QVERIFY(tracker.listen());
    QVERIFY(peer.listen());
    QVERIFY(dht.listen());
    TestProxy proxy;
    QVERIFY(proxy.listen());
    proxy.redirect(QStringLiteral("192.0.2.50:6969"), QHostAddress::LocalHost, tracker.port());
    proxy.redirect(QStringLiteral("192.0.2.51:6881"), QHostAddress::LocalHost, peer.port());
    bttest::Node n(bttest::freshDir(QStringLiteral("np-proxy")));
    n.create();
    n.settings.setValue(QString(k::connection::BindInterface), QString());
    n.settings.setValue(QString(k::bt::Dht), true);
    n.settings.setValue(QString(k::bt::DhtBootstrapNodes), QStringList{QStringLiteral("127.0.0.1:%1").arg(dht.port())});
    n.settings.setValue(QString(k::connection::ProxyType), QStringLiteral("socks5"));
    n.settings.setValue(QString(k::connection::ProxyHost), QStringLiteral("127.0.0.1"));
    n.settings.setValue(QString(k::connection::ProxyPort), int(proxy.port()));
    n.plugin->start();
    const QString hash = QStringLiteral("0123456789abcdef0123456789abcdef01234567");
    QString err;
    const QString id = n.be->addTorrent(magnetFor(hash, QStringLiteral("http://192.0.2.50:6969/announce")), QByteArray(),
                                        bttest::freshDir(QStringLiteral("np-proxy-dl")), false, {}, &err);
    QVERIFY2(!id.isEmpty(), qPrintable(err));
    n.ctl->addPeer(id, QStringLiteral("192.0.2.51:6881"));
    QTRY_VERIFY_WITH_TIMEOUT(proxy.countTarget(QStringLiteral("192.0.2.50:6969")) >= 1, 20000);
    QTRY_VERIFY_WITH_TIMEOUT(proxy.countTarget(QStringLiteral("192.0.2.51:6881")) >= 1, 20000);
    QTRY_VERIFY_WITH_TIMEOUT(peer.tcpCount() >= 1 && !peer.received.isEmpty(), 10000);   // the handshake came through
    QTRY_VERIFY_WITH_TIMEOUT(proxy.udpAssociates >= 1, 10000);   // the DHT asked for UDP through the proxy ...
    QTest::qWait(2000);
    QCOMPARE(dht.udpCount(), 0);   // ... and, refused, sent nothing directly
    QCOMPARE(n.plugin->status().listenPort, 0);   // nothing listens behind a proxy
}

void TstNetPolicyBt::adapterBindsPeersAndDht()
{
    if (!haveSecondLoopback()) QSKIP("no second loopback address on this system");
    Canary peer, dht;
    QVERIFY(peer.listen());
    QVERIFY(dht.listen());
    bttest::Node n(bttest::freshDir(QStringLiteral("np-bind")));
    n.create();
    n.settings.setValue(QString(k::connection::BindInterface), kAdapter);
    n.settings.setValue(QString(k::bt::Dht), true);
    n.settings.setValue(QString(k::bt::DhtBootstrapNodes), QStringList{QStringLiteral("127.0.0.1:%1").arg(dht.port())});
    QVERIFY(n.start());
    const QString hash = QStringLiteral("89abcdef0123456789abcdef0123456789abcdef");
    QString err;
    const QString id = n.be->addTorrent(QStringLiteral("magnet:?xt=urn:btih:%1&dn=bind-test").arg(hash), QByteArray(),
                                        bttest::freshDir(QStringLiteral("np-bind-dl")), false, {}, &err);
    QVERIFY2(!id.isEmpty(), qPrintable(err));
    n.ctl->addPeer(id, QStringLiteral("127.0.0.1:%1").arg(peer.port()));
    QTRY_VERIFY_WITH_TIMEOUT(peer.tcpCount() >= 1, 20000);
    QTRY_VERIFY_WITH_TIMEOUT(dht.udpCount() >= 1, 20000);
    QCOMPARE(peer.tcpCount(QStringLiteral("127.0.0.1")), 0);
    QCOMPARE(peer.tcpCount(kAdapter), peer.tcpCount());
    QCOMPARE(dht.udpCount(QStringLiteral("127.0.0.1")), 0);
    QCOMPARE(dht.udpCount(kAdapter), dht.udpCount());
}

void TstNetPolicyBt::requiredEncryptionRefusesHeaderOnlyPeers()
{
    const QString src = bttest::freshDir(QStringLiteral("np-enc-src"));
    bttest::makeFile(src + QStringLiteral("/enc.bin"), 256 * 1024, 7);
    const QByteArray torrent = bttest::makeTorrent(src + QStringLiteral("/enc.bin"), 32 * 1024);
    QVERIFY(!torrent.isEmpty());
    // levels: pe_plaintext = handshake encrypted, stream plain ("header only"); pe_rc4 = all
    struct Case { const char *name; int level; int policy; const char *mode; bool works; };
    const Case cases[] = {
        {"forced vs header-only", lt::settings_pack::pe_plaintext, lt::settings_pack::pe_enabled, "forced", false},
        {"forced vs no encryption", lt::settings_pack::pe_both, lt::settings_pack::pe_disabled, "forced", false},
        {"forced vs rc4", lt::settings_pack::pe_rc4, lt::settings_pack::pe_enabled, "forced", true},
        {"prefer vs header-only", lt::settings_pack::pe_plaintext, lt::settings_pack::pe_enabled, "enabled", true},
        {"disabled vs rc4 only", lt::settings_pack::pe_rc4, lt::settings_pack::pe_forced, "disabled", false},
    };
    int i = 0;
    for (const Case &c : cases) {
        Seed seed;
        seed.start(src, torrent, c.level, c.policy);
        QVERIFY(seed.port > 0);
        bttest::Node n(bttest::freshDir(QStringLiteral("np-enc-%1").arg(i)));
        n.create();
        n.settings.setValue(QString(k::bt::Encryption), QString::fromLatin1(c.mode));
        n.settings.setValue(QStringLiteral("bt/utp"), false);
        QVERIFY(n.start());
        QString err;
        const QString id = n.be->addTorrent(QString(), torrent, bttest::freshDir(QStringLiteral("np-enc-dl-%1").arg(i)), false, {}, &err);
        QVERIFY2(!id.isEmpty(), qPrintable(err));
        n.ctl->addPeer(id, QStringLiteral("127.0.0.1:%1").arg(seed.port));
        const bool got = QTest::qWaitFor([&] { return n.haveCount(id) > 0; }, c.works ? 20000 : 6000);
        QVERIFY2(got == c.works, c.name);
        ++i;
    }
}

// connection/randomizePorts: a random port instead of bt/listenPort; bt/maxConnectionsPerTorrent:
// no more peers of one torrent than that (four seeds offered, two taken); bt/maxActiveDownloads:
// the second torrent waits in the queue
void TstNetPolicyBt::randomPortsAndTorrentLimits()
{
    {
        bttest::Node n(bttest::freshDir(QStringLiteral("np-rand")));
        n.create();
        n.settings.setValue(QString(k::bt::ListenPort), 46881);
        n.settings.setValue(QString(k::connection::RandomizePorts), true);
        QVERIFY(n.start());
        const int port = n.plugin->status().listenPort;
        QVERIFY2(port != 46881 && port >= 20000 && port < 60000, qPrintable(QString::number(port)));
    }
    const QString src = bttest::freshDir(QStringLiteral("np-lim-src"));
    bttest::makeFile(src + QStringLiteral("/lim.bin"), 4 * 1024 * 1024, 11);
    const QByteArray torrent = bttest::makeTorrent(src + QStringLiteral("/lim.bin"), 16 * 1024);
    bttest::makeFile(src + QStringLiteral("/other.bin"), 64 * 1024, 12);
    const QByteArray other = bttest::makeTorrent(src + QStringLiteral("/other.bin"), 16 * 1024);
    Seed seeds[4];
    for (Seed &sd : seeds) {
        sd.start(src, torrent, lt::settings_pack::pe_both, lt::settings_pack::pe_enabled);
        QVERIFY(sd.port > 0);
    }
    bttest::Node n(bttest::freshDir(QStringLiteral("np-lim")));
    n.create();
    n.settings.setValue(QString(k::bt::MaxConnectionsPerTorrent), 2);
    n.settings.setValue(QString(k::bt::MaxActiveDownloads), 1);
    QVERIFY(n.start());
    QString err;
    const QString id = n.be->addTorrent(QString(), torrent, bttest::freshDir(QStringLiteral("np-lim-dl")), false, {}, &err);
    QVERIFY2(!id.isEmpty(), qPrintable(err));
    // the second torrent first: the first one (no peer yet) holds the only active slot
    const QString id2 = n.be->addTorrent(QString(), other, bttest::freshDir(QStringLiteral("np-lim-dl2")), false, {}, &err);
    QVERIFY(!id2.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(n.be->torrentDiagnostics(id2).queued, 10000);
    QVERIFY(!n.be->torrentDiagnostics(id).queued);
    for (const Seed &sd : seeds) n.ctl->addPeer(id, QStringLiteral("127.0.0.1:%1").arg(sd.port));
    int most = 0;
    for (int i = 0; i < 400 && !(n.snap(id).numPieces > 0 && n.haveCount(id) == n.snap(id).numPieces); ++i) {
        most = qMax(most, n.be->torrentDiagnostics(id).connectedNow);
        QTest::qWait(50);
    }
    QVERIFY2(n.haveCount(id) > 0, "nothing downloaded");
    QVERIFY2(most <= 2, qPrintable(QString::number(most)));   // four seeds offered, at most two taken
}

QTEST_GUILESS_MAIN(TstNetPolicyBt)
#include "tst_netpolicy_bt.moc"
