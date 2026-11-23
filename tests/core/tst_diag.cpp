// SPDX-License-Identifier: GPL-3.0-or-later
// [diag-1] Connection Test: address masking and classification, the per-download sentence rules,
// NAT / summary verdicts, the plain-text report, the shared PortMapper against a fake UPnP router
// (conflict re-claim, foreign conflict, permanent-only leases, clean removal) and a fake NAT-PMP
// gateway, and the DiagnosticsManager report of a real core with a fake plugin.
#include <QtCore/QTemporaryDir>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtNetwork/QUdpSocket>
#include <QtTest/QtTest>

#include "core/diag/diag_rules.h"
#include "core/diag/diagnostics_manager.h"
#include "core/diag/port_mapper.h"
#include "core/network_manager.h"
#include "core/real_core.h"
#include "fake_plugin.h"
#include "mx/diagnostics.h"
#include "mx/settings_keys.h"

using namespace mx;
using namespace mx::diag;

namespace {

// ------------------------------------------------------------------ a fake UPnP internet gateway
class FakeIgd : public QObject {
public:
    struct Entry { QString client; int internal = 0; int lease = 0; };
    QHash<QString, Entry> table;          // "TCP/6881" -> mapping
    QStringList calls;                    // "AddPortMapping TCP 6881", ...
    bool permanentOnly = false;
    QString wan = QStringLiteral("203.0.113.9");

    FakeIgd()
    {
        m_srv.listen(QHostAddress::LocalHost, 0);
        connect(&m_srv, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *s = m_srv.nextPendingConnection()) {
                auto *buf = new QByteArray;
                connect(s, &QTcpSocket::readyRead, s, [this, s, buf] {
                    *buf += s->readAll();
                    const int he = int(buf->indexOf("\r\n\r\n"));
                    if (he < 0) return;
                    const QByteArray head = buf->left(he);
                    int len = 0;
                    for (const QByteArray &l : head.split('\n'))
                        if (l.toLower().startsWith("content-length:")) len = l.mid(15).trimmed().toInt();
                    if (buf->size() < he + 4 + len) return;
                    const QByteArray body = buf->mid(he + 4, len);
                    answer(s, head, body);
                    delete buf;
                });
            }
        });
    }
    QUrl descriptionUrl() const { return QUrl(QStringLiteral("http://127.0.0.1:%1/desc.xml").arg(m_srv.serverPort())); }

private:
    static QString arg(const QByteArray &body, const char *name)
    {
        const QByteArray open = QByteArray("<") + name + ">", close = QByteArray("</") + name + ">";
        const int a = int(body.indexOf(open)), b = int(body.indexOf(close));
        return a >= 0 && b > a ? QString::fromUtf8(body.mid(a + open.size(), b - a - open.size())) : QString();
    }
    void reply(QTcpSocket *s, int status, const QByteArray &body)
    {
        const QByteArray reason = status == 200 ? "OK" : "Internal Server Error";
        s->write("HTTP/1.1 " + QByteArray::number(status) + " " + reason + "\r\nContent-Type: text/xml\r\nContent-Length: "
                 + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        s->disconnectFromHost();
    }
    void fault(QTcpSocket *s, int code, const char *desc)
    {
        reply(s, 500, QByteArray("<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body><s:Fault>"
                                 "<faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring><detail><UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\">"
                                 "<errorCode>") + QByteArray::number(code) + "</errorCode><errorDescription>" + desc
                         + "</errorDescription></UPnPError></detail></s:Fault></s:Body></s:Envelope>");
    }
    void ok(QTcpSocket *s, const QString &action, const QString &inner = QString())
    {
        reply(s, 200, QStringLiteral("<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body>"
                                     "<u:%1Response xmlns:u=\"urn:schemas-upnp-org:service:WANIPConnection:1\">%2</u:%1Response></s:Body></s:Envelope>")
                          .arg(action, inner).toUtf8());
    }
    void answer(QTcpSocket *s, const QByteArray &head, const QByteArray &body)
    {
        if (head.startsWith("GET")) {
            reply(s, 200, "<?xml version=\"1.0\"?><root xmlns=\"urn:schemas-upnp-org:device-1-0\"><device>"
                          "<deviceType>urn:schemas-upnp-org:device:InternetGatewayDevice:1</deviceType><friendlyName>Fake Router</friendlyName>"
                          "<manufacturer>Example</manufacturer><modelName>FR-1</modelName><deviceList><device><deviceList><device>"
                          "<serviceList><service><serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>"
                          "<controlURL>/ctl</controlURL></service></serviceList></device></deviceList></device></deviceList></device></root>");
            return;
        }
        QString action;
        for (const QByteArray &l : head.split('\n'))
            if (l.toLower().startsWith("soapaction:")) action = QString::fromLatin1(l.mid(l.indexOf('#') + 1)).remove(QLatin1Char('"')).trimmed();
        const QString proto = arg(body, "NewProtocol");
        const QString ext = arg(body, "NewExternalPort");
        const QString key = proto + QLatin1Char('/') + ext;
        calls << QStringLiteral("%1 %2 %3").arg(action, proto, ext).trimmed();
        if (action == QLatin1String("GetExternalIPAddress")) return ok(s, action, QStringLiteral("<NewExternalIPAddress>%1</NewExternalIPAddress>").arg(wan));
        if (action == QLatin1String("AddPortMapping")) {
            const int lease = arg(body, "NewLeaseDuration").toInt();
            if (permanentOnly && lease != 0) return fault(s, 725, "OnlyPermanentLeasesSupported");
            if (table.contains(key)) return fault(s, 718, "ConflictInMappingEntry");
            table.insert(key, Entry{arg(body, "NewInternalClient"), arg(body, "NewInternalPort").toInt(), lease});
            return ok(s, action);
        }
        if (action == QLatin1String("GetSpecificPortMappingEntry")) {
            if (!table.contains(key)) return fault(s, 714, "NoSuchEntryInArray");
            const Entry e = table.value(key);
            return ok(s, action, QStringLiteral("<NewInternalPort>%1</NewInternalPort><NewInternalClient>%2</NewInternalClient><NewEnabled>1</NewEnabled>"
                                                "<NewLeaseDuration>%3</NewLeaseDuration>").arg(e.internal).arg(e.client).arg(e.lease));
        }
        if (action == QLatin1String("DeletePortMapping")) {
            if (!table.remove(key)) return fault(s, 714, "NoSuchEntryInArray");
            return ok(s, action);
        }
        fault(s, 401, "Invalid Action");
    }
    QTcpServer m_srv;
};

PortMapping mappingOf(const PortMapper &m, const QString &proto, int port)
{
    for (const PortMapping &x : m.mappings())
        if (x.proto == proto && x.internalPort == port) return x;
    return {};
}

DiagnosticsManager *diagOf(Core *c) { return static_cast<RealCore *>(c)->diagnosticsImpl(); }

// an eD2k-like plugin that reports a LowID and two listening ports
class DiagPlugin : public mx::test::FakePlugin {
public:
    DiagPlugin() : FakePlugin(Net::ED2K, NetworkPlugin::CanSearch) {}
    int rechecks = 0;
    QList<PortMapping> told;
    PluginDiagnostics diagnostics() const override
    {
        PluginDiagnostics d;
        d.valid = true;
        PortBinding t;
        t.proto = QStringLiteral("TCP");
        t.port = 4662;
        t.ok = true;
        t.purpose = QStringLiteral("eD2k peers");
        PortBinding u;
        u.proto = QStringLiteral("UDP");
        u.port = 4672;
        u.ok = false;
        u.error = QStringLiteral("The bound address is already in use");
        d.bindings << t << u;
        d.tcpIn = Reach::No;
        d.tcpEvidence = QStringLiteral("server assigned a LowID");
        d.externalAddress = QStringLiteral("203.0.113.77");
        d.externalSeenBy = QStringLiteral("eD2k server Test");
        return d;
    }
    void recheckConnectivity() override { ++rechecks; }
    void portMappingChanged(const PortMapping &m) override { told << m; }
};

} // namespace

class TstDiag : public QObject {
    Q_OBJECT
private slots:
    void addressClassification();
    void masking();
    void reportText();
    void torrentSentences_data();
    void torrentSentences();
    void rangeSentences();
    void failureBuckets();
    void natVerdicts();
    void summaryVerdicts();
    void virtualAdapters();
    void mapperReclaimsOwnStaleMapping();
    void mapperForeignConflictPicksOtherPort();
    void mapperPermanentLeaseAndRemoval();
    void mapperNatPmp();
    void mapperOff();
    void managerReport();
    // [diag-1 follow-up] discovery
    void searchAnswerOnSecondAdapter();
    void passiveAnnouncement();
    void cacheShortcut();
    void nothingAnsweredText();
    void summaryReasons();
    void shutdownDuringSearchRemovesMappings();
};

void TstDiag::addressClassification()
{
    QCOMPARE(classifyAddress(QStringLiteral("192.168.1.23")), AddressKind::Private);
    QCOMPARE(classifyAddress(QStringLiteral("10.8.0.2")), AddressKind::Private);
    QCOMPARE(classifyAddress(QStringLiteral("172.20.1.1:6881")), AddressKind::Private);
    QCOMPARE(classifyAddress(QStringLiteral("100.70.1.2")), AddressKind::CarrierNat);
    QCOMPARE(classifyAddress(QStringLiteral("127.0.0.1")), AddressKind::Loopback);
    QCOMPARE(classifyAddress(QStringLiteral("169.254.3.4")), AddressKind::LinkLocal);
    QCOMPARE(classifyAddress(QStringLiteral("203.0.113.77")), AddressKind::Public);
    QCOMPARE(classifyAddress(QStringLiteral("[2001:db8::1]:51413")), AddressKind::Public);
    QCOMPARE(classifyAddress(QStringLiteral("fd00::5")), AddressKind::Private);
    QCOMPARE(classifyAddress(QStringLiteral("fe80::1%eth0")), AddressKind::LinkLocal);
    QCOMPARE(classifyAddress(QStringLiteral("1.2")), AddressKind::Invalid);
    QCOMPARE(classifyAddress(QStringLiteral("router.bittorrent.com")), AddressKind::Invalid);
}

void TstDiag::masking()
{
    AddressMasker m;
    const QString in = QStringLiteral("seen as 203.0.113.77 by DHT, 203.0.113.77:4662 by eD2k, peer 198.51.100.148:34682, "
                                      "LAN 192.168.1.70, CGNAT 100.64.0.9, v6 2001:db8:1::7 and fe80::1, version 4.0.1.2x, 1.2.3, "
                                      "file ubuntu-24.04.5.1-desktop-amd64.iso");
    const QString out = m.mask(in);
    QVERIFY2(!out.contains(QLatin1String("203.0.113.77")), qPrintable(out));
    QVERIFY(!out.contains(QLatin1String("198.51.100.148")));
    QVERIFY(!out.contains(QLatin1String("2001:db8")));
    QVERIFY(out.contains(QLatin1String("seen as ext-1 by DHT, ext-1:4662 by eD2k, peer ext-2:34682")));
    QVERIFY(out.contains(QLatin1String("LAN 192.168.1.70")));
    QVERIFY(out.contains(QLatin1String("CGNAT 100.64.0.9")));
    QVERIFY(out.contains(QLatin1String("v6 ext-3 and fe80::1")));
    QVERIFY(out.contains(QLatin1String("1.2.3")));
    QVERIFY(out.contains(QLatin1String("ubuntu-24.04.5.1-desktop-amd64.iso")));   // a version in a name is no address
    QCOMPARE(m.count(), 3);
    // consistent across calls of the same masker
    QCOMPARE(m.mask(QStringLiteral("198.51.100.148")), QStringLiteral("ext-2"));
}

void TstDiag::reportText()
{
    DiagReport r;
    r.version = QStringLiteral("4.0");
    r.os = QStringLiteral("Test OS");
    r.generated = QDateTime(QDate(2026, 11, 21), QTime(12, 0));
    r.level = DiagLevel::Limited;
    r.summary = QStringLiteral("Works, but you cannot be reached from the internet on eD2k (slower, fewer sources).");
    DiagGroup g;
    g.id = QStringLiteral("net.eD2k");
    g.title = QStringLiteral("eD2k");
    g.net = Net::ED2K;
    g.level = DiagLevel::Limited;
    g.result = QStringLiteral("Works, not reachable from the internet");
    DiagItem i;
    i.id = QStringLiteral("ed2k.reach.tcp");
    i.check = QStringLiteral("Reachable from the internet (TCP)");
    i.level = DiagLevel::Limited;
    i.result = QStringLiteral("No");
    i.details = QStringLiteral("server assigned a LowID, seen as 203.0.113.77");
    i.explain = QStringLiteral("In your router, forward TCP port 4662 to 192.168.1.70.");
    g.items << i;
    r.groups << g;
    DiagGroup d;
    d.id = diagGroupIdForDownload(QStringLiteral("abc"));
    d.downloadId = QStringLiteral("abc");
    d.title = QStringLiteral("ubuntu.iso");
    d.level = DiagLevel::Broken;
    d.result = QStringLiteral("No peer answered: 43 addresses tried, all timed out.");
    r.groups << d;
    r.logLines << QStringLiteral("2026-11-21 12:00:00.000 W mx.diag: port mapping failed for 203.0.113.77");
    const QString hidden = diagReportText(r, true);
    QVERIFY(hidden.contains(QLatin1String("Summary: [LIMITED] Works, but you cannot be reached")));
    QVERIFY(hidden.contains(QLatin1String("== eD2k: [LIMITED] Works, not reachable from the internet")));
    QVERIFY(hidden.contains(QLatin1String("[LIMITED] Reachable from the internet (TCP): No (server assigned a LowID, seen as ext-1)")));
    QVERIFY(hidden.contains(QLatin1String("forward TCP port 4662 to 192.168.1.70")));
    QVERIFY(hidden.contains(QLatin1String("==== Downloads ====")));
    QVERIFY(hidden.contains(QLatin1String("== ubuntu.iso: [BROKEN] No peer answered")));
    QVERIFY(hidden.contains(QLatin1String("port mapping failed for ext-1")));
    QVERIFY(!hidden.contains(QLatin1String("203.0.113.77")));
    QVERIFY(!hidden.contains(QChar(0x2014)));   // no em dash anywhere
    QVERIFY(diagReportText(r, false).contains(QLatin1String("203.0.113.77")));
}

void TstDiag::torrentSentences_data()
{
    QTest::addColumn<int>("scenario");
    QTest::addColumn<int>("level");
    QTest::addColumn<QString>("expect");
    QTest::newRow("all timed out") << 0 << int(DiagLevel::Broken) << QStringLiteral("No peer answered: 43 addresses tried, all timed out. Outgoing connections may be blocked");
    QTest::newRow("stale index") << 1 << int(DiagLevel::Limited) << QStringLiteral("The index (BitSearch) reports 192 seeds but neither the trackers nor the DHT know this torrent: the index numbers are probably out of date.");
    QTest::newRow("dht dead") << 2 << int(DiagLevel::Broken) << QStringLiteral("DHT is not answering (0 nodes)");
    QTest::newRow("few accept") << 3 << int(DiagLevel::Limited) << QStringLiteral("Only 2 of 80 peers accept connections, both slow.");
    QTest::newRow("bt queue") << 4 << int(DiagLevel::Limited) << QStringLiteral("Queued by BitTorrent: 5 torrent downloads may run at once");
    QTest::newRow("slot") << 5 << int(DiagLevel::Limited) << QStringLiteral("Waiting for a free download slot: 40 downloads are active and the limit is 40");
    QTest::newRow("healthy") << 6 << int(DiagLevel::Ok) << QString();
    QTest::newRow("dns") << 7 << int(DiagLevel::Broken) << QStringLiteral("None of the 3 tracker host names resolve");
    QTest::newRow("choked") << 8 << int(DiagLevel::Limited) << QStringLiteral("Connected to 4 peers, but all of them keep us waiting (choked)");
    QTest::newRow("throttle") << 9 << int(DiagLevel::Limited) << QStringLiteral("Your download limit of 100 KB/s is in force");
    QTest::newRow("handshake") << 10 << int(DiagLevel::Broken) << QStringLiteral("Peers close the connection during the handshake (20 of 30 tried)");
    QTest::newRow("paused") << 11 << int(DiagLevel::Unknown) << QString();
}

void TstDiag::torrentSentences()
{
    QFETCH(int, scenario);
    QFETCH(int, level);
    QFETCH(QString, expect);
    DownloadFacts f;
    f.id = QStringLiteral("t1");
    f.active = true;
    f.isTorrent = true;
    f.torrentId = QStringLiteral("aa");
    TorrentDiag t;
    t.valid = true;
    t.hasMetadata = true;
    t.activeSec = 300;
    t.dhtNodes = 300;
    t.knownPeers = 0;
    DownloadContext c;
    c.maxDownloads = 40;
    c.activeDownloads = 3;
    c.btActiveLimit = 5;
    TrackerDiag tr;
    tr.host = QStringLiteral("tracker.example");
    tr.contacted = true;
    switch (scenario) {
    case 0:
        t.knownPeers = 60;
        t.connectAttempts = 43;
        t.failedTimeout = 43;
        break;
    case 1:
        f.indexSeeds = 192;
        f.indexPeers = 295;
        f.indexName = QStringLiteral("BitSearch");
        t.hasMetadata = false;
        t.dhtLookups = 3;
        tr.working = true;
        tr.peersReturned = 0;
        t.trackers << tr << tr << tr;
        break;
    case 2:
        t.dhtNodes = 0;
        t.hasMetadata = false;
        tr.fails = 2;
        tr.error = QStringLiteral("timed out");
        t.trackers << tr;
        break;
    case 3:
        t.knownPeers = 80;
        t.connectAttempts = 75;
        t.connectedNow = 2;
        t.interesting = 2;
        t.slowPeers = 2;
        t.downBps = 3000;
        t.failedTimeout = 60;
        break;
    case 4:
        t.queued = true;
        break;
    case 5:
        f.active = false;
        c.activeDownloads = 40;
        break;
    case 6:
        t.knownPeers = 74;
        t.connectedNow = 56;
        t.downBps = 12 * 1024 * 1024;
        break;
    case 7: {
        TrackerDiag d = tr;
        d.dnsFailed = true;
        d.error = QStringLiteral("name does not resolve");
        t.trackers << d << d << d;
        t.dhtNodes = 200;
        t.dhtLookups = 0;
        break;
    }
    case 8:
        t.knownPeers = 10;
        t.connectAttempts = 4;
        t.connectedNow = 4;
        t.interesting = 4;
        t.chokingUs = 4;
        t.downBps = 0;
        break;
    case 9:
        c.downLimitKBs = 100;
        t.knownPeers = 50;
        t.connectedNow = 20;
        t.downBps = 99 * 1024;
        break;
    case 10:
        t.knownPeers = 40;
        t.connectAttempts = 30;
        t.failedHandshake = 20;
        t.failedRefused = 5;
        break;
    case 11:
        f.paused = true;
        t.paused = true;
        break;
    }
    const Verdict v = downloadVerdict(f, t, c);
    QCOMPARE(int(v.level), level);
    if (expect.isEmpty()) QVERIFY2(v.sentence.isEmpty(), qPrintable(v.sentence));
    else QVERIFY2(v.sentence.contains(expect), qPrintable(v.sentence));
    QVERIFY(!v.sentence.contains(QChar(0x2014)));
}

void TstDiag::rangeSentences()
{
    DownloadFacts f;
    f.active = true;
    f.activeSec = 120;
    f.sourcesKnown = 6;
    f.sourcesQueued = 5;
    f.bestQueueRank = 12;
    DownloadContext c;
    Verdict v = downloadVerdict(f, TorrentDiag(), c);
    QCOMPARE(v.level, DiagLevel::Limited);
    QVERIFY2(v.sentence.contains(QLatin1String("5 of 6 sources have us in their upload queue (best place 12)")), qPrintable(v.sentence));
    f.sourcesQueued = 0;
    f.sourcesFailed = 5;
    f.failReasons.insert(QStringLiteral("Timed Out (Waiting for Network Reply)"), 5);
    c.weFirewalledEd2k = true;
    v = downloadVerdict(f, TorrentDiag(), c);
    QVERIFY2(v.sentence.contains(QLatin1String("5 of 6 sources could not be reached: they did not answer, and many may be firewalled like you")), qPrintable(v.sentence));
    f.sourcesTransferring = 1;
    QCOMPARE(downloadVerdict(f, TorrentDiag(), c).level, DiagLevel::Ok);
}

void TstDiag::failureBuckets()
{
    QCOMPARE(failureBucket(QStringLiteral("Connection timed out")), QStringLiteral("timed out"));
    QCOMPARE(failureBucket(QStringLiteral("Connection refused")), QStringLiteral("refused"));
    QCOMPARE(failureBucket(QStringLiteral("encryption handshake failed")), QStringLiteral("handshake"));
    QCOMPARE(failureBucket(QStringLiteral("peer banned")), QStringLiteral("banned"));
    QCOMPARE(failureBucket(QStringLiteral("whatever")), QStringLiteral("other"));
}

void TstDiag::natVerdicts()
{
    NatVerdict v = natVerdict(QStringLiteral("100.72.1.9"), {QStringLiteral("81.1.2.3")});
    QCOMPARE(v.kind, QStringLiteral("carrier-grade NAT"));
    QVERIFY(v.text.contains(QLatin1String("Port forwarding cannot work there")));
    v = natVerdict(QStringLiteral("192.168.0.2"), {});
    QCOMPARE(v.kind, QStringLiteral("double NAT"));
    v = natVerdict(QStringLiteral("203.0.113.77"), {QStringLiteral("203.0.113.77")});
    QCOMPARE(v.level, DiagLevel::Ok);
    v = natVerdict(QStringLiteral("203.0.113.77"), {QStringLiteral("81.1.2.3")});
    QCOMPARE(v.kind, QStringLiteral("address mismatch"));
    v = natVerdict(QStringLiteral("0.0.0.0"), {QStringLiteral("81.1.2.3")});
    QCOMPARE(v.kind, QStringLiteral("router without internet address"));
    v = natVerdict(QString(), {});
    QCOMPARE(v.level, DiagLevel::Unknown);
}

void TstDiag::summaryVerdicts()
{
    auto group = [](const QString &id, Net n, DiagLevel lv, const QList<DiagItem> &items) {
        DiagGroup g;
        g.id = id;
        g.title = n == Net::Unknown ? QStringLiteral("General") : netName(n);
        g.net = n;
        g.level = lv;
        g.items = items;
        return g;
    };
    auto item = [](const QString &id, DiagLevel lv, const QString &result) {
        DiagItem i;
        i.id = id;
        i.level = lv;
        i.result = result;
        return i;
    };
    QList<DiagGroup> gs;
    gs << group(QStringLiteral("general"), Net::Unknown, DiagLevel::Ok, {item(QStringLiteral("general.router"), DiagLevel::Ok, QStringLiteral("Found"))});
    gs << group(QStringLiteral("net.BitTorrent"), Net::BitTorrent, DiagLevel::Ok, {item(QStringLiteral("bittorrent.reach.tcp"), DiagLevel::Ok, QStringLiteral("Yes"))});
    QCOMPARE(reportSummary(gs).sentence, QStringLiteral("Everything works."));
    gs << group(QStringLiteral("net.eD2k"), Net::ED2K, DiagLevel::Limited, {item(QStringLiteral("ed2k.reach.tcp"), DiagLevel::Limited, QStringLiteral("No"))});
    QVERIFY(reportSummary(gs).sentence.contains(QLatin1String("cannot be reached from the internet on eD2k")));
    gs[0].items[0].level = DiagLevel::Limited;   // the UPnP router is not the gateway
    gs[0].level = DiagLevel::Limited;
    QVERIFY(reportSummary(gs).sentence.contains(QLatin1String("not your internet router")));
    gs << group(QStringLiteral("net.BitTorrent2"), Net::BitTorrent, DiagLevel::Broken, {item(QStringLiteral("bittorrent.dht"), DiagLevel::Broken, QStringLiteral("0 nodes"))});
    QVERIFY(reportSummary(gs).sentence.startsWith(QLatin1String("BitTorrent cannot reach the DHT")));
    QList<DiagGroup> off;
    off << group(QStringLiteral("net.BitTorrent"), Net::BitTorrent, DiagLevel::Off, {});
    QCOMPARE(reportSummary(off).level, DiagLevel::Off);
}

void TstDiag::virtualAdapters()
{
    QVERIFY(looksVirtualAdapter(QStringLiteral("wg0"), QStringLiteral("wg0")));
    QVERIFY(looksVirtualAdapter(QStringLiteral("tun0"), QString()));
    QVERIFY(looksVirtualAdapter(QStringLiteral("{1234}"), QStringLiteral("TAP-Windows Adapter V9")));
    QVERIFY(looksVirtualAdapter(QStringLiteral("{1234}"), QStringLiteral("WireGuard Tunnel")));
    QVERIFY(looksVirtualAdapter(QStringLiteral("{1234}"), QStringLiteral("vEthernet (WSL)")));
    QVERIFY(looksVirtualAdapter(QStringLiteral("docker0"), QString()));
    QVERIFY(!looksVirtualAdapter(QStringLiteral("eth0"), QStringLiteral("eth0")));
    QVERIFY(!looksVirtualAdapter(QStringLiteral("{1234}"), QStringLiteral("Wi-Fi")));
    QVERIFY(!looksVirtualAdapter(QStringLiteral("br0"), QStringLiteral("br0")));
    QVERIFY(!looksVirtualAdapter(QStringLiteral("{1234}"), QStringLiteral("Ethernet 2")));
}

// a stale mapping of this computer (an earlier run whose delete failed) is re-claimed: same external port
void TstDiag::mapperReclaimsOwnStaleMapping()
{
    FakeIgd igd;
    igd.table.insert(QStringLiteral("TCP/6881"), FakeIgd::Entry{QStringLiteral("192.168.1.70"), 6881, 3600});
    PortMapper m;
    m.setTestDescriptionUrl(igd.descriptionUrl());
    m.setTestInternalClient(QStringLiteral("192.168.1.70"));
    m.setTestRetryScale(0.01);
    QSignalSpy changed(&m, &PortMapper::mappingChanged);
    m.setEnabled(true, false);
    PortMapper::Want w;
    w.net = Net::BitTorrent;
    w.proto = QStringLiteral("TCP");
    w.port = 6881;
    m.setWanted({w});
    QTRY_COMPARE_WITH_TIMEOUT(int(mappingOf(m, QStringLiteral("TCP"), 6881).result), int(PortMapping::Mapped), 8000);
    QCOMPARE(mappingOf(m, QStringLiteral("TCP"), 6881).externalPort, 6881);
    QCOMPARE(m.router().friendlyName, QStringLiteral("Fake Router"));
    QCOMPARE(m.router().upnpWan, QStringLiteral("203.0.113.9"));
    QVERIFY2(igd.calls.contains(QStringLiteral("GetSpecificPortMappingEntry TCP 6881")), qPrintable(igd.calls.join(QLatin1Char(','))));
    QVERIFY(igd.calls.contains(QStringLiteral("DeletePortMapping TCP 6881")));
    QCOMPARE(igd.calls.count(QStringLiteral("AddPortMapping TCP 6881")), 2);
    QCOMPARE(igd.table.value(QStringLiteral("TCP/6881")).lease, 3600);
}

// another computer holds the port: another external port is used and reported
void TstDiag::mapperForeignConflictPicksOtherPort()
{
    FakeIgd igd;
    igd.table.insert(QStringLiteral("TCP/4662"), FakeIgd::Entry{QStringLiteral("192.168.1.50"), 4662, 0});
    PortMapper m;
    m.setTestDescriptionUrl(igd.descriptionUrl());
    m.setTestInternalClient(QStringLiteral("192.168.1.70"));
    m.setTestRetryScale(0.01);
    m.setEnabled(true, false);
    PortMapper::Want w;
    w.net = Net::ED2K;
    w.proto = QStringLiteral("TCP");
    w.port = 4662;
    m.setWanted({w});
    QTRY_COMPARE_WITH_TIMEOUT(int(mappingOf(m, QStringLiteral("TCP"), 4662).result), int(PortMapping::Mapped), 8000);
    const PortMapping pm = mappingOf(m, QStringLiteral("TCP"), 4662);
    QVERIFY(pm.externalPort != 4662);
    QVERIFY(pm.externalPort >= 20000);
    QCOMPARE(igd.table.value(QStringLiteral("TCP/4662")).client, QStringLiteral("192.168.1.50"));   // untouched
    QCOMPARE(igd.table.value(QStringLiteral("TCP/%1").arg(pm.externalPort)).internal, 4662);
}

void TstDiag::mapperPermanentLeaseAndRemoval()
{
    FakeIgd igd;
    igd.permanentOnly = true;
    PortMapper m;
    m.setTestDescriptionUrl(igd.descriptionUrl());
    m.setTestInternalClient(QStringLiteral("192.168.1.70"));
    m.setTestRetryScale(0.01);
    m.setEnabled(true, false);
    QList<PortMapper::Want> wants;
    for (const auto &p : {qMakePair(QStringLiteral("TCP"), 6346), qMakePair(QStringLiteral("UDP"), 6346), qMakePair(QStringLiteral("UDP"), 4672)}) {
        PortMapper::Want w;
        w.net = Net::Gnutella;
        w.proto = p.first;
        w.port = p.second;
        wants << w;
    }
    m.setWanted(wants);
    QTRY_COMPARE_WITH_TIMEOUT(int(igd.table.size()), 3, 8000);
    QTRY_COMPARE(int(mappingOf(m, QStringLiteral("UDP"), 4672).result), int(PortMapping::Mapped));
    QCOMPARE(igd.table.value(QStringLiteral("UDP/6346")).lease, 0);
    QVERIFY(!mappingOf(m, QStringLiteral("UDP"), 6346).expires.isValid());
    // a port no longer wanted is deleted on the router
    wants.removeLast();
    m.setWanted(wants);
    QTRY_COMPARE_WITH_TIMEOUT(int(igd.table.size()), 2, 8000);
    QCOMPARE(int(m.mappings().size()), 2);
    // shutdown removes the rest; one already gone (714) still counts as removed
    igd.table.remove(QStringLiteral("UDP/6346"));
    m.shutdown(3000);
    QCOMPARE(int(igd.table.size()), 0);
    QVERIFY(igd.calls.contains(QStringLiteral("DeletePortMapping UDP 6346")));
}

void TstDiag::mapperNatPmp()
{
    QUdpSocket gw;
    QVERIFY(gw.bind(QHostAddress::LocalHost, 0));
    int maps = 0;
    connect(&gw, &QUdpSocket::readyRead, &gw, [&] {
        while (gw.hasPendingDatagrams()) {
            QByteArray d(int(gw.pendingDatagramSize()), 0);
            QHostAddress from;
            quint16 port = 0;
            gw.readDatagram(d.data(), d.size(), &from, &port);
            if (d.size() == 2) {   // external address: 198.51.100.7
                gw.writeDatagram(QByteArray::fromHex("0080000000000001c6336407"), from, port);
            } else if (d.size() == 12) {
                ++maps;
                QByteArray r = QByteArray::fromHex("00000000000000020000000000000e10");
                r[1] = char(128 + d[1]);
                r[8] = d[4];
                r[9] = d[5];
                r[10] = d[4];   // external = internal
                r[11] = d[5];
                gw.writeDatagram(r, from, port);
            }
        }
    });
    PortMapper m;
    m.setTestNatPmp(QHostAddress::LocalHost, gw.localPort());
    m.setTestRetryScale(0.05);
    m.setEnabled(false, true);
    PortMapper::Want w;
    w.net = Net::Kad;
    w.proto = QStringLiteral("UDP");
    w.port = 4672;
    m.setWanted({w});
    QTRY_COMPARE_WITH_TIMEOUT(int(mappingOf(m, QStringLiteral("UDP"), 4672).result), int(PortMapping::Mapped), 8000);
    QCOMPARE(mappingOf(m, QStringLiteral("UDP"), 4672).method, QStringLiteral("NAT-PMP"));
    QCOMPARE(mappingOf(m, QStringLiteral("UDP"), 4672).externalPort, 4672);
    QCOMPARE(m.router().natPmpWan, QStringLiteral("198.51.100.7"));
    QCOMPARE(m.router().method(), QStringLiteral("NAT-PMP"));
    QVERIFY(maps >= 1);
}

void TstDiag::mapperOff()
{
    PortMapper m;
    m.setEnabled(false, false);
    PortMapper::Want w;
    w.proto = QStringLiteral("TCP");
    w.port = 6699;
    m.setWanted({w});
    QCOMPARE(int(mappingOf(m, QStringLiteral("TCP"), 6699).result), int(PortMapping::Off));
    QVERIFY(!m.busy());
}

// a real core with a fake plugin: the plugin's facts end up in the report and in the text
void TstDiag::managerReport()
{
    QTemporaryDir dir;
    auto *fp = new DiagPlugin;
    Core *c = createRealCore(dir.path(), QList<NetworkPlugin *>{fp});
    c->settings()->setValue(QString(keys::connection::Upnp), false);
    c->settings()->setValue(QString(keys::connection::NatPmp), false);
    c->start();
    QVERIFY(c->diagnostics());
    DiagReport r = c->diagnostics()->report();
    QVERIFY(!r.groups.isEmpty());
    QCOMPARE(r.groups.first().id, QStringLiteral("general"));
    const DiagGroup *ed = nullptr;
    for (const DiagGroup &g : r.groups)
        if (g.net == Net::ED2K) ed = &g;
    QVERIFY(ed);
    auto find = [ed](const QString &id) -> DiagItem {
        for (const DiagItem &i : ed->items)
            if (i.id == id) return i;
        return {};
    };
    QCOMPARE(find(QStringLiteral("ed2k.port.tcp.4662")).level, DiagLevel::Ok);
    QCOMPARE(find(QStringLiteral("ed2k.port.udp.4672")).level, DiagLevel::Broken);
    QVERIFY(find(QStringLiteral("ed2k.port.udp.4672")).explain.contains(QLatin1String("already in use")));
    QCOMPARE(find(QStringLiteral("ed2k.reach.tcp")).result, QStringLiteral("No"));
    QVERIFY(find(QStringLiteral("ed2k.reach.tcp")).explain.contains(QLatin1String("forward TCP port 4662")));
    QCOMPARE(find(QStringLiteral("ed2k.map.tcp.4662")).level, DiagLevel::Off);   // mapping switched off
    QCOMPARE(ed->level, DiagLevel::Broken);
    QVERIFY(r.summary.startsWith(QLatin1String("Problem with eD2k")));
    const QString text = diagReportText(r, true);
    QVERIFY(text.contains(QLatin1String("OctoMX connection report")));
    QVERIFY(text.contains(QLatin1String("== General")));
    // the test runs to the end (nothing to look up, no router) and reports progress
    QSignalSpy prog(c->diagnostics(), &DiagnosticsService::testProgress);
    QSignalSpy fin(c->diagnostics(), &DiagnosticsService::testFinished);
    diagOf(c)->setTestTimings(500, 1500);
    c->diagnostics()->startTest();
    QVERIFY(c->diagnostics()->isTesting());
    QVERIFY(fin.wait(30000));
    QCOMPARE(fin.first().first().toBool(), false);
    QVERIFY(prog.size() >= 3);
    QCOMPARE(prog.last().first().toInt(), 100);
    QVERIFY(c->diagnostics()->report().lastTest.isValid());
    QCOMPARE(fp->rechecks, 1);
    // cancel works
    c->diagnostics()->startTest();
    c->diagnostics()->cancelTest();
    QVERIFY(!c->diagnostics()->isTesting());
    c->shutdown();
    delete c;
}

// ------------------------------------------------------------------ [diag-1 follow-up] discovery
namespace {
// answers M-SEARCHes, but only those that come from `onlyFrom`
struct FakeSsdp {
    QUdpSocket sock;
    QHostAddress onlyFrom;
    QUrl location;
    int seen = 0, answered = 0;
    explicit FakeSsdp(const QUrl &loc, const QHostAddress &from) : onlyFrom(from), location(loc)
    {
        sock.bind(QHostAddress::LocalHost, 0);
        QObject::connect(&sock, &QUdpSocket::readyRead, &sock, [this] {
            while (sock.hasPendingDatagrams()) {
                QByteArray d(int(sock.pendingDatagramSize()), 0);
                QHostAddress src;
                quint16 port = 0;
                sock.readDatagram(d.data(), d.size(), &src, &port);
                ++seen;
                if (!d.startsWith("M-SEARCH") || QHostAddress(src.toIPv4Address()) != onlyFrom) continue;
                if (!d.contains("ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1")) continue;
                ++answered;
                sock.writeDatagram("HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=120\r\nST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n"
                                   "USN: uuid:fake::urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\nLOCATION: "
                                   + location.toString().toLatin1() + "\r\n\r\n", src, port);
            }
        });
    }
    quint16 port() const { return sock.localPort(); }
};

quint16 deadUdpPort()
{
    QUdpSocket s;
    s.bind(QHostAddress::LocalHost, 0);
    return s.localPort();
}

PortMapper::Want want(const QString &proto, int port)
{
    PortMapper::Want w;
    w.net = Net::Gnutella;
    w.proto = proto;
    w.port = port;
    return w;
}
} // namespace

// the router answers only the search sent from the second adapter (on Windows the first adapters
// are often virtual ones): it is still found, and the report knows on which adapter
void TstDiag::searchAnswerOnSecondAdapter()
{
    FakeIgd igd;
    FakeSsdp ssdp(igd.descriptionUrl(), QHostAddress(QStringLiteral("127.0.0.2")));
    PortMapper m;
    m.setTestSsdp({{QStringLiteral("VMnet1"), QHostAddress::LocalHost}, {QStringLiteral("WiFi"), QHostAddress(QStringLiteral("127.0.0.2"))}},
                  QHostAddress::LocalHost, ssdp.port());
    m.setTestNatPmp(QHostAddress::LocalHost, deadUdpPort());   // gateway = 127.0.0.1 (the fake IGD), NAT-PMP off below
    m.setTestInternalClient(QStringLiteral("192.168.1.20"));
    m.setTestRetryScale(0.1);
    QSignalSpy cached(&m, &PortMapper::cacheUpdated);
    m.setEnabled(true, false);
    m.setWanted({want(QStringLiteral("TCP"), 6346)});
    QTRY_COMPARE_WITH_TIMEOUT(int(mappingOf(m, QStringLiteral("TCP"), 6346).result), int(PortMapping::Mapped), 8000);
    const RouterInfo r = m.router();
    QVERIFY(r.upnpFound);
    QCOMPARE(r.answeredOn, QStringLiteral("WiFi 127.0.0.2"));
    QCOMPARE(r.foundBy, QStringLiteral("search"));
    QCOMPARE(r.adapters, QStringList({QStringLiteral("VMnet1 127.0.0.1"), QStringLiteral("WiFi 127.0.0.2")}));
    QCOMPARE(r.adapterRouter, QList<bool>({false, true}));
    QCOMPARE(r.adapterAnswers.value(0), 0);
    QVERIFY(r.adapterAnswers.value(1) >= 1);
    QVERIFY(ssdp.seen >= 12);   // 6 targets x 2 adapters, repeated
    QVERIFY(cached.size() >= 1);
    QCOMPARE(m.cache().value(QStringLiteral("127.0.0.1")).toMap().value(QStringLiteral("location")).toString(), igd.descriptionUrl().toString());
}

// nobody answers the search; the router's own announcement (NOTIFY ssdp:alive) heals it
void TstDiag::passiveAnnouncement()
{
    FakeIgd igd;
    const quint16 notifyPort = deadUdpPort();
    PortMapper m;
    m.setTestSsdp({{QStringLiteral("WiFi"), QHostAddress::LocalHost}}, QHostAddress::LocalHost, deadUdpPort());
    m.setTestNotifyPort(notifyPort);
    m.setTestNatPmp(QHostAddress::LocalHost, deadUdpPort());
    m.setTestInternalClient(QStringLiteral("192.168.1.20"));
    m.setTestRetryScale(0.1);
    m.setEnabled(true, false);
    m.setWanted({want(QStringLiteral("UDP"), 6346)});
    QTRY_COMPARE_WITH_TIMEOUT(int(mappingOf(m, QStringLiteral("UDP"), 6346).result), int(PortMapping::Failed), 8000);
    QVERIFY(mappingOf(m, QStringLiteral("UDP"), 6346).error.startsWith(QLatin1String("No router answered;")));
    QVERIFY(m.router().notifyListening);
    QUdpSocket tx;
    tx.writeDatagram("NOTIFY * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nNT: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n"
                     "NTS: ssdp:alive\r\nUSN: uuid:fake::urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\nLOCATION: "
                     + igd.descriptionUrl().toString().toLatin1() + "\r\n\r\n", QHostAddress::LocalHost, notifyPort);
    QTRY_COMPARE_WITH_TIMEOUT(int(mappingOf(m, QStringLiteral("UDP"), 6346).result), int(PortMapping::Mapped), 8000);
    QCOMPARE(m.router().foundBy, QStringLiteral("announcement"));
}

// the description remembered from the last start is tried first and ends the search early
void TstDiag::cacheShortcut()
{
    FakeIgd igd;
    PortMapper m;
    m.setTestSsdp({{QStringLiteral("WiFi"), QHostAddress::LocalHost}}, QHostAddress::LocalHost, deadUdpPort());
    m.setTestNatPmp(QHostAddress::LocalHost, deadUdpPort());
    m.setTestInternalClient(QStringLiteral("192.168.1.20"));
    m.setTestRetryScale(1.0);   // a 3 s search: the shortcut must be faster
    QVariantMap entry;
    entry.insert(QStringLiteral("location"), igd.descriptionUrl().toString());
    QVariantMap cache;
    cache.insert(QStringLiteral("127.0.0.1"), entry);
    m.setCache(cache);
    QElapsedTimer t;
    t.start();
    m.setEnabled(true, false);
    m.setWanted({want(QStringLiteral("TCP"), 6699)});
    QTRY_COMPARE_WITH_TIMEOUT(int(mappingOf(m, QStringLiteral("TCP"), 6699).result), int(PortMapping::Mapped), 8000);
    QVERIFY2(t.elapsed() < 2500, qPrintable(QString::number(t.elapsed())));
    QVERIFY(m.router().cacheTried);
    QVERIFY(m.router().cacheWorked);
    QCOMPARE(m.router().foundBy, QStringLiteral("cache"));
}

void TstDiag::nothingAnsweredText()
{
    PortMapper m;
    m.setTestSsdp({{QStringLiteral("WiFi"), QHostAddress::LocalHost}}, QHostAddress::LocalHost, deadUdpPort());
    m.setTestNatPmp(QHostAddress::LocalHost, deadUdpPort());
    m.setTestRetryScale(0.1);
    m.setTestWindowsFallback(false);   // as on Windows when the system knows no router either
    m.setEnabled(true, true);
    m.setWanted({want(QStringLiteral("TCP"), 6346), want(QStringLiteral("UDP"), 6346)});
    QTRY_COMPARE_WITH_TIMEOUT(int(mappingOf(m, QStringLiteral("TCP"), 6346).result), int(PortMapping::Failed), 8000);
    const RouterInfo r = m.router();
    QVERIFY(r.winTried);
    QCOMPARE(r.winResult, QStringLiteral("Windows found no UPnP router either"));
    const QString err = mappingOf(m, QStringLiteral("TCP"), 6346).error;
    QVERIFY2(err.startsWith(QLatin1String("No router answered; UPnP: no UPnP router answered; NAT-PMP: no answer")), qPrintable(err));
    QVERIFY(!err.contains(QLatin1String("((")));
    DiscoveryFacts f;
    f.adapters = r.adapters;
    f.answers = r.adapterAnswers;
    f.gateway = QStringLiteral("192.168.1.1");
    f.unicastTried = true;
    f.notifyListening = true;
    f.natPmpResult = QStringLiteral("no answer from 192.168.1.1");
    f.winTried = true;
    f.winFoundNone = true;
    f.winResult = r.winResult;
    QString t = discoveryFailureText(f);
    QVERIFY2(t.startsWith(QLatin1String("OctoMX searched for a UPnP router on 1 adapter (WiFi 127.0.0.1), asked the gateway 192.168.1.1 directly, "
                                        "listened for router announcements, asked Windows (Windows found no UPnP router either), asked NAT-PMP")), qPrintable(t));
    QVERIFY(t.contains(QLatin1String("Likely reason: Windows itself found no UPnP router either")));
    QVERIFY(t.contains(QLatin1String("What to do: enable UPnP in your router (192.168.1.1)")));
    // a Public network without an OctoMX rule: that is named
    f.firewallChecked = f.firewallOn = true;
    f.firewallProfiles = QStringLiteral("Public");
    t = discoveryFailureText(f);
    QVERIFY(t.contains(QLatin1String("Windows treats this network as Public")));
    // with an allow rule the firewall is not blamed
    f.firewallAllowRules = 1;
    t = discoveryFailureText(f);
    QVERIFY(!t.contains(QLatin1String("firewall"), Qt::CaseInsensitive));
    // other devices answered: the search works, UPnP is off in the router
    f.answers = {3};
    f.winFoundNone = false;
    QVERIFY(discoveryFailureText(f).contains(QLatin1String("Other devices answered")));
    QVERIFY(!discoveryFailureText(f).contains(QChar(0x2014)));
}

void TstDiag::summaryReasons()
{
    auto item = [](const QString &id, DiagLevel lv, const QString &result) {
        DiagItem i;
        i.id = id;
        i.level = lv;
        i.result = result;
        return i;
    };
    DiagGroup gen;
    gen.id = QStringLiteral("general");
    gen.title = QStringLiteral("General");
    gen.level = DiagLevel::Limited;
    gen.items << item(QStringLiteral("general.router"), DiagLevel::Limited, QStringLiteral("Not found"));
    DiagGroup ed;
    ed.id = QStringLiteral("net.eD2k");
    ed.title = QStringLiteral("eD2k");
    ed.net = Net::ED2K;
    ed.level = DiagLevel::Limited;
    ed.items << item(QStringLiteral("ed2k.reach.tcp"), DiagLevel::Limited, QStringLiteral("No"));
    QString s = reportSummary({gen, ed}).sentence;
    QVERIFY2(s.contains(QLatin1String("on eD2k (slower, fewer sources): no router answered the port mapping requests, see General.")), qPrintable(s));
    QVERIFY(!s.contains(QLatin1String("not your internet router")));
    gen.items[0].result = QStringLiteral("Home Router (at 192.168.1.1)");
    gen.items[0].level = DiagLevel::Ok;
    gen.level = DiagLevel::Ok;
    ed.items << item(QStringLiteral("ed2k.blocked"), DiagLevel::Limited, QStringLiteral("Blocked before the router"));
    s = reportSummary({gen, ed}).sentence;
    QVERIFY2(s.contains(QLatin1String("the router forwards the ports, but something before it blocks them")), qPrintable(s));
    QVERIFY(looksVirtualAdapter(QStringLiteral("{x}"), QStringLiteral("Office Wintun Userspace Tunnel")));
    QVERIFY(looksVirtualAdapter(QStringLiteral("{x}"), QStringLiteral("VMware Network Adapter VMnet8")));
    QVERIFY(!looksVirtualAdapter(QStringLiteral("{x}"), QStringLiteral("HOME_PC")));   // unknown: not guessed
}

// a new search clears the router info; mappings made before must still be removed at shutdown
void TstDiag::shutdownDuringSearchRemovesMappings()
{
    FakeIgd igd;
    PortMapper m;
    m.setTestDescriptionUrl(igd.descriptionUrl());
    m.setTestInternalClient(QStringLiteral("192.168.1.20"));
    m.setTestRetryScale(0.01);
    m.setEnabled(true, false);
    m.setWanted({want(QStringLiteral("TCP"), 51000), want(QStringLiteral("UDP"), 51000)});
    QTRY_COMPARE_WITH_TIMEOUT(int(igd.table.size()), 2, 8000);
    QTRY_COMPARE(int(mappingOf(m, QStringLiteral("UDP"), 51000).result), int(PortMapping::Mapped));
    m.refresh();               // searching again (as every 30 s on a network change, or Test Now)
    QVERIFY(m.router().searching);
    m.shutdown(3000);
    QCOMPARE(int(igd.table.size()), 0);
}

QTEST_MAIN(TstDiag)
#include "tst_diag.moc"
