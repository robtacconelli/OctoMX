// SPDX-License-Identifier: GPL-3.0-or-later
// [net-3] The eD2k, Kad and Gnutella settings that had only a reader proof: each test sets the key
// through the plugin's Settings (as the program does) and observes what the network does: packets
// that arrive at local fake servers and caches, the sources a download is handed, concurrent uploads
// and queue answers on the wire, the counters and index of real Kad nodes over 127.0.0.1.
// Nothing leaves this computer: built-in server lists, caches and nodes.dat addresses are kept out
// (a server.met of local servers only, no server list download, eD2k bound to 127.0.0.1, Gnutella
// caches marked as failed in the profile, a local nodes.dat).
#include "net_test_servers.h"

#include "ed2k_core.h"
#include "ed2k_opcodes.h"
#include "ed2k_plugin_impl.h"
#include "ed2k_server.h"
#include "ed2k_servermet.h"
#include "ed2k_upload.h"
#include "ed2k_util.h"
#include "kad/kad_index.h"
#include "kad/kad_node.h"
#include "kad/kad_plugin.h"
#include "kad/kad_routing.h"
#include "kad/kad_search.h"
#include "kad/kad_wire.h"
#include "gnut_hostcache.h"
#include "gnut_plugin_impl.h"

#include "../ed2k/test_support.h"
#include "../gnutella/test_support.h"

#include "core/rate_limiter.h"
#include "mxhash/encoding.h"

#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRandomGenerator>
#include <QtCore/QTemporaryDir>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtNetwork/QUdpSocket>
#include <QtTest/QtTest>

#include <functional>
#include <memory>
#include <vector>

using namespace mx;
using namespace mx::ed2k;
using namespace mx::ed2k::kad;

namespace {

bool waitUntil(const std::function<bool()> &f, int ms)
{
    QElapsedTimer t;
    t.start();
    while (!f()) {
        if (t.elapsed() > ms) return false;
        QTest::qWait(50);
    }
    return true;
}

// ------------------------------------------------------------------------------------- eD2k helpers
QByteArray le32(quint32 v)
{
    QByteArray b;
    for (int i = 0; i < 4; ++i) b.append(char((v >> (8 * i)) & 0xFF));
    return b;
}
QByteArray le16(quint16 v)
{
    QByteArray b;
    b.append(char(v & 0xFF)).append(char(v >> 8));
    return b;
}
QByteArray ed2kPacket(quint8 op, const QByteArray &payload)
{
    QByteArray p;
    p.append(char(PROT_EDONKEY));
    p.append(le32(quint32(payload.size() + 1)));
    p.append(char(op));
    p.append(payload);
    return p;
}

// a minimal eD2k server: answers the login with a high ID and every OP_GETSOURCES with `sources`
// sources on 127.0.0.1 (ports 30001 and up, nothing listens there)
struct FakeServer {
    ~FakeServer() { for (QTcpSocket *s : tcp.findChildren<QTcpSocket *>()) s->disconnect(); }
    QTcpServer tcp;
    int sources = 0;
    int getSources = 0;
    QHash<QTcpSocket *, QByteArray> buf;
    bool listen()
    {
        if (!tcp.listen(QHostAddress::LocalHost, 0)) return false;
        QObject::connect(&tcp, &QTcpServer::newConnection, &tcp, [this] {
            while (QTcpSocket *s = tcp.nextPendingConnection()) {
                s->setParent(&tcp);
                QObject::connect(s, &QTcpSocket::readyRead, s, [this, s] { onData(s); });
            }
        });
        return true;
    }
    quint16 port() const { return tcp.serverPort(); }
    void onData(QTcpSocket *s)
    {
        QByteArray &b = buf[s];
        b += s->readAll();
        while (b.size() >= 6) {
            const quint32 len = quint8(b[1]) | (quint8(b[2]) << 8) | (quint8(b[3]) << 16) | (quint32(quint8(b[4])) << 24);
            if (len == 0 || len > 4 * 1024 * 1024) { b.clear(); return; }
            if (quint32(b.size()) < 5 + len) return;
            const quint8 proto = quint8(b[0]), op = quint8(b[5]);
            const QByteArray payload = b.mid(6, int(len) - 1);
            b.remove(0, int(5 + len));
            if (proto != PROT_EDONKEY) continue;
            if (op == OP_LOGINREQUEST) {
                s->write(ed2kPacket(OP_IDCHANGE, QByteArray("\x0d\x0c\x0b\x0a\x00\x00\x00\x00", 8)));   // high ID, no flags
            } else if (op == OP_GETSOURCES && payload.size() >= 16) {
                ++getSources;
                QByteArray a = payload.left(16);
                a.append(char(quint8(sources)));
                for (int i = 0; i < sources; ++i) {
                    a.append(QByteArray("\x7f\x00\x00\x01", 4));   // 127.0.0.1 as an eD2k ID (high)
                    a.append(le16(quint16(30001 + i)));
                }
                s->write(ed2kPacket(OP_FOUNDSOURCES, a));
            }
        }
    }
};

// the UDP side of a second server of the list (on 127.0.0.2): answers the status ping, counts what
// it is asked
struct FakeServerUdp {
    QUdpSocket udp;
    QList<quint8> ops;
    int pingsAnswered = 0;
    bool listen()
    {
        for (int i = 0; i < 20; ++i) {
            if (!udp.bind(QHostAddress(QStringLiteral("127.0.0.2")), 0)) return false;
            if (udp.localPort() > 1100) break;
            udp.close();
        }
        QObject::connect(&udp, &QUdpSocket::readyRead, &udp, [this] {
            while (udp.hasPendingDatagrams()) {
                QByteArray d(int(udp.pendingDatagramSize()), Qt::Uninitialized);
                QHostAddress from;
                quint16 fromPort = 0;
                udp.readDatagram(d.data(), d.size(), &from, &fromPort);
                if (d.size() < 2 || quint8(d[0]) != PROT_EDONKEY) continue;
                const quint8 op = quint8(d[1]);
                ops << op;
                if (op == OP_GLOBSERVSTATREQ && d.size() >= 6) {
                    QByteArray a;
                    a.append(char(PROT_EDONKEY)).append(char(OP_GLOBSERVSTATRES));
                    a.append(d.mid(2, 4));     // the challenge
                    a.append(le32(5));         // users
                    a.append(le32(50));        // files
                    udp.writeDatagram(a, from, fromPort);
                    ++pingsAnswered;
                }
            }
        });
        return true;
    }
    // UDP = TCP + 4; another address than the connected server (UDP source requests skip its address)
    QString serverAddress() const { return QStringLiteral("127.0.0.2:%1").arg(udp.localPort() - 4); }
    int count(std::initializer_list<quint8> which) const
    {
        int n = 0;
        for (quint8 o : ops)
            for (quint8 w : which) n += o == w;
        return n;
    }
};

// the profile's server list: only these (local) servers, so the built-in list of internet servers
// is never merged in (ServerManager::load takes it only when server.met is empty)
void writeServerList(const QString &profile, const QList<QPair<QString, quint32>> &servers)
{
    QList<ServerInfo> list;
    for (const auto &a : servers) {
        ServerInfo si;
        si.ip = ipFromString(a.first.section(QLatin1Char(':'), 0, 0));
        si.port = quint16(a.first.section(QLatin1Char(':'), 1).toUInt());
        si.name = QStringLiteral("local %1").arg(si.port);
        si.isStatic = true;           // a loopback server is kept only as a static one
        si.preference = list.isEmpty() ? 1 : 0;   // the first one is dialled first
        si.ping = a.second;           // answered a ping before (eMule keeps it in server.met)
        if (si.ping) si.lastPingTime = quint32(QDateTime::currentSecsSinceEpoch());
        list << si;
    }
    const QString dir = QDir(profile).filePath(QStringLiteral("ed2k"));
    QDir().mkpath(dir);
    QFile f(QDir(dir).filePath(QStringLiteral("server.met")));
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(writeServerMet(list));
}

void ed2kBase(mxtest::MemSettings &s)
{
    s.setValue(QStringLiteral("connection/bindInterface"), QStringLiteral("127.0.0.1"));   // nothing can leave this computer
    s.setValue(QStringLiteral("ed2k/tcpPort"), 0);
    s.setValue(QStringLiteral("ed2k/udpPort"), 0);
    s.setValue(QStringLiteral("ed2k/autoUpdateServerList"), false);   // the server.met of the internet: never
    s.setValue(QStringLiteral("ed2k/allowLan"), true);
    s.setValue(QStringLiteral("ed2k/obfuscation"), false);
    s.setValue(QStringLiteral("ed2k/addServersFromServer"), false);
}

// an eD2k uploader with its own slot count (ed2k/uploadSlots) and a core rate limiter
struct Ed2kNode {
    mxtest::MemSettings settings;
    mxtest::FileLibrary lib;
    mx::CoreRateLimiter limiter{nullptr};
    QTemporaryDir dir;
    Ed2kPlugin *plugin = nullptr;
    QHash<QString, mx::TransferInfo> rows;
    QStringList finished;
    explicit Ed2kNode(int nSlots)
    {
        ed2kBase(settings);
        settings.setValue(QStringLiteral("ed2k/autoConnect"), false);
        settings.setValue(QStringLiteral("ed2k/udpPing"), false);
        settings.setValue(QStringLiteral("ed2k/udpSearch"), false);
        settings.setValue(QStringLiteral("ed2k/udpSources"), false);
        if (nSlots > 0) settings.setValue(QStringLiteral("ed2k/uploadSlots"), nSlots);
        writeServerList(dir.path(), {{QStringLiteral("127.0.0.1:1"), 0}});
        plugin = new Ed2kPlugin();
        QObject::connect(plugin, &mx::NetworkPlugin::uploadChanged, plugin, [this](const mx::TransferInfo &t) { rows.insert(t.id, t); });
        QObject::connect(plugin, &mx::NetworkPlugin::uploadFinished, plugin, [this](const QString &id) { finished << id; });
        plugin->init(mxtest::makeContext(&settings, &lib, &limiter, dir.path()));
        plugin->start();
    }
    ~Ed2kNode() { plugin->stop(); delete plugin; }
    QString uri() const
    {
        return QStringLiteral("ed2k-peer://127.0.0.1:%1?uid=%2")
            .arg(plugin->core()->tcpPort())
            .arg(QString::fromLatin1(plugin->core()->userHash().toHex()));
    }
    int inState(mx::TransferState st) const
    {
        int n = 0;
        for (const mx::TransferInfo &t : rows) n += t.state == st && !finished.contains(t.id);
        return n;
    }
};

QString makeRandomFile(const QString &path, qint64 size)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return QString();
    QRandomGenerator rng{quint32(size)};
    QByteArray buf(1 << 20, '\0');
    while (f.size() < size) {
        for (auto &c : buf) c = char(rng.bounded(256));
        f.write(buf.left(int(qMin<qint64>(buf.size(), size - f.size()))));
    }
    return path;
}

// -------------------------------------------------------------------------------------- Kad helpers
// a real Kad plugin over 127.0.0.1; `extra` sets the keys under test before start()
struct KadInst {
    mxtest::MemSettings settings;
    mxtest::FileLibrary lib;
    mxtest::NullLimiter limiter;
    QTemporaryDir dir;
    std::shared_ptr<Ed2kCore> core = makeSharedCore();
    std::unique_ptr<KadPlugin> kad;
    explicit KadInst(const std::function<void(mxtest::MemSettings &)> &extra = {})
    {
        settings.setValue(QStringLiteral("ed2k/tcpPort"), 0);
        settings.setValue(QStringLiteral("ed2k/udpPort"), 0);
        settings.setValue(QStringLiteral("ed2k/allowLan"), true);
        settings.setValue(QStringLiteral("ed2k/autoConnect"), false);
        settings.setValue(QStringLiteral("ed2k/autoUpdateServerList"), false);
        settings.setValue(QStringLiteral("ed2k/udpPing"), false);
        settings.setValue(QStringLiteral("kad/allowLan"), true);
        settings.setValue(QStringLiteral("kad/allowSameIp"), true);
        settings.setValue(QStringLiteral("kad/firewallChecks"), false);
        settings.setValue(QStringLiteral("kad/autoBootstrap"), false);   // no nodes.dat from the internet
        settings.setValue(QStringLiteral("kad/findBuddyDelay"), 5);
        if (extra) extra(settings);
        kad = std::make_unique<KadPlugin>(core);
        mx::PluginContext ctx = mxtest::makeContext(&settings, &lib, &limiter, dir.path());
        ctx.dataDir = QDir(dir.path()).filePath(QStringLiteral("kad"));
        kad->init(ctx);
        kad->start();
    }
    ~KadInst() { kad->stop(); }
    quint16 udp() const { return core->udpLocalPort(); }
    KadNode *node() const { return kad->node(); }
};

// --------------------------------------------------------------------------------- Gnutella helpers
// a leaf's ultrapeer as far as the 0.6 handshake goes: answers "200 OK, I am an ultrapeer", then
// keeps the link open and ignores what follows
struct UltrapeerStub {
    ~UltrapeerStub() { for (QTcpSocket *s : server.findChildren<QTcpSocket *>()) s->disconnect(); }
    QTcpServer server;
    int open = 0;
    QHash<QTcpSocket *, int> stage;
    QHash<QTcpSocket *, QByteArray> buf;
    bool listen()
    {
        if (!server.listen(QHostAddress::LocalHost, 0)) return false;
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
            while (QTcpSocket *s = server.nextPendingConnection()) {
                s->setParent(&server);
                stage.insert(s, 0);
                QObject::connect(s, &QTcpSocket::readyRead, s, [this, s] { onData(s); });
                QObject::connect(s, &QTcpSocket::disconnected, s, [this, s] {
                    if (stage.value(s) == 2) --open;
                    stage.remove(s);
                    buf.remove(s);
                    s->deleteLater();
                });
            }
        });
        return true;
    }
    QString address() const { return QStringLiteral("127.0.0.1:%1").arg(server.serverPort()); }
    void onData(QTcpSocket *s)
    {
        QByteArray &b = buf[s];
        b += s->readAll();
        if (stage.value(s) == 0 && b.contains("\r\n\r\n")) {
            b = b.mid(b.indexOf("\r\n\r\n") + 4);
            s->write("GNUTELLA/0.6 200 OK\r\nUser-Agent: StubUP/1.0\r\nX-Ultrapeer: True\r\n"
                     "X-Query-Routing: 0.2\r\nX-Ultrapeer-Query-Routing: 0.1\r\nRemote-IP: 127.0.0.1\r\n\r\n");
            stage[s] = 1;
        }
        if (stage.value(s) == 1 && b.contains("\r\n\r\n")) {
            stage[s] = 2;
            ++open;
            b.clear();
        }
        if (stage.value(s) == 2) b.clear();
    }
};

// one HTTP client of a Gnutella uploader (GET by sha1 with X-Queue, as in tst_gnutella_glue3)
struct GnutClient {
    QTcpSocket s;
    bool open(quint16 port)
    {
        s.connectToHost(QHostAddress::LocalHost, port);
        return waitUntil([this] { return s.state() == QAbstractSocket::ConnectedState; }, 5000);
    }
    QByteArray get(const QByteArray &sha1, qint64 from, qint64 to)
    {
        QByteArray req = "GET /uri-res/N2R?urn:sha1:" + mx::hash::toBase32(sha1).toLatin1() + " HTTP/1.1\r\n"
                         "Host: 127.0.0.1\r\nUser-Agent: p2p-settings-test/1.0\r\nX-Queue: 1.0\r\nConnection: Keep-Alive\r\n"
                         "Range: bytes=" + QByteArray::number(from) + "-" + QByteArray::number(to) + "\r\n\r\n";
        s.write(req);
        QByteArray resp;
        QElapsedTimer t;
        t.start();
        while (!resp.contains("\r\n\r\n") && t.elapsed() < 10000) {
            QTest::qWait(10);
            resp += s.readAll();
            if (s.state() != QAbstractSocket::ConnectedState && !s.bytesAvailable()) break;
        }
        const int end = int(resp.indexOf("\r\n\r\n"));
        if (end < 0) return resp;
        const QByteArray head = resp.left(end + 4);
        QByteArray body = resp.mid(end + 4);
        if (head.startsWith("HTTP/1.1 2"))
            while (body.size() < to - from + 1 && t.elapsed() < 10000) {
                QTest::qWait(10);
                body += s.readAll();
            }
        return head;
    }
    static QByteArray header(const QByteArray &head, const QByteArray &name)
    {
        for (const QByteArray &l : head.split('\n'))
            if (l.toLower().startsWith(name.toLower() + ':')) return l.mid(name.size() + 1).trimmed();
        return QByteArray();
    }
};

// the built-in GWebCaches and UDP host caches (internet addresses) marked as failed three times just
// now in the profile: HostCache::nextCache skips them for hours, only the user's caches are asked
void retireBuiltinCaches(const QString &dataDir)
{
    QDir().mkpath(dataDir);
    const qint64 now = gnut::nowSecs();
    auto entries = [now](const QStringList &urls) {
        QJsonArray a;
        for (const QString &u : urls) {
            QJsonObject o;
            o[QStringLiteral("url")] = u;
            o[QStringLiteral("attempt")] = double(now);
            o[QStringLiteral("success")] = 0;
            o[QStringLiteral("ok")] = 0;
            o[QStringLiteral("fail")] = 3;
            a << o;
        }
        return a;
    };
    QJsonObject root;
    root[QStringLiteral("hosts")] = QJsonArray();
    root[QStringLiteral("gwcs")] = entries(gnut::defaultGwcs());
    root[QStringLiteral("uhcs")] = entries(gnut::defaultUhcs());
    QFile f(dataDir + QStringLiteral("/hosts.json"));
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(QJsonDocument(root).toJson());
}

} // namespace

class TstNetSettingsP2p : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { mx::registerMetaTypes(); }
    void ed2kSourcesPerDownload();
    void ed2kServerUdpSwitches();
    void ed2kOwnUploadSlots();
    void kadNodesDatFromTheUrl();
    void kadPacketRate();
    void kadPublishStoreFirewallBuddies();
    void kadStoreIndexOff();
    void gnutellaUltrapeerCount();
    void gnutellaUserCaches();
    void gnutellaSlotsQueueAndPoll();
};

// ---------------------------------------------------------------------------------------------- eD2k
// ed2k/maxSourcesPerFile: the server offers 40 sources; a download is handed at most that many
void TstNetSettingsP2p::ed2kSourcesPerDownload()
{
    auto handed = [](const QVariant &max) {
        FakeServer server;
        server.sources = 40;
        if (!server.listen()) return -1;
        QTemporaryDir dir;
        mxtest::MemSettings s;
        ed2kBase(s);
        s.setValue(QStringLiteral("ed2k/autoConnect"), true);
        s.setValue(QStringLiteral("ed2k/udpPing"), false);
        s.setValue(QStringLiteral("ed2k/udpSearch"), false);
        s.setValue(QStringLiteral("ed2k/udpSources"), false);
        if (max.isValid()) s.setValue(QStringLiteral("ed2k/maxSourcesPerFile"), max);
        writeServerList(dir.path(), {{QStringLiteral("127.0.0.1:%1").arg(server.port()), 0}});
        mxtest::FileLibrary lib;
        mxtest::NullLimiter lim;
        Ed2kPlugin p;
        int total = 0;
        QObject::connect(&p, &mx::NetworkPlugin::sourcesFound, &p, [&total](const QString &id, const QList<mx::SearchSource> &l) {
            if (id == QLatin1String("dl1")) total += int(l.size());
        });
        p.init(mxtest::makeContext(&s, &lib, &lim, dir.path()));
        p.start();
        if (!waitUntil([&] { return p.core()->servers()->current() != nullptr; }, 15000)) { p.stop(); return -2; }
        mx::FileHashes h;
        h.ed2k = QByteArray::fromHex("0123456789abcdef0123456789abcdef");
        p.findSources(QStringLiteral("dl1"), h, 5000000, QStringLiteral("sources test.bin"));
        waitUntil([&] { return server.getSources > 0 && total > 0; }, 20000);
        QTest::qWait(500);
        p.stop();
        return total;
    };
    QCOMPARE(handed(15), 15);
    QCOMPARE(handed(QVariant()), 40);   // the default (400) takes all of them
}

// ed2k/udpSearch, ed2k/udpSources: what a second server of the list (not the one we are connected
// to, known from server.met to answer UDP) receives over UDP. ed2k/udpPing: the servers of the list
// are pinged over UDP (OP_GLOBSERVSTATREQ; a LAN server too with ed2k/allowLan) or not
void TstNetSettingsP2p::ed2kServerUdpSwitches()
{
    struct Got { int searches, sources, pings; };
    auto run = [](bool search, bool sources, bool ping = false) {
        Got g{-1, -1, -1};
        FakeServer s1;
        FakeServerUdp s2;
        if (!s1.listen() || !s2.listen()) return g;
        QTemporaryDir dir;
        writeServerList(dir.path(), {{QStringLiteral("127.0.0.1:%1").arg(s1.port()), 0}, {s2.serverAddress(), 30}});
        mxtest::MemSettings s;
        ed2kBase(s);
        s.setValue(QStringLiteral("ed2k/autoConnect"), true);
        s.setValue(QStringLiteral("ed2k/parallelServerConnects"), 1);   // the second server is never dialled (a refused
                                                                          // one would be left out of the UDP requests for a while)
        s.setValue(QStringLiteral("ed2k/udpPing"), ping);
        s.setValue(QStringLiteral("ed2k/udpSearch"), search);
        s.setValue(QStringLiteral("ed2k/udpSources"), sources);
        mxtest::FileLibrary lib;
        mxtest::NullLimiter lim;
        Ed2kPlugin p;
        p.init(mxtest::makeContext(&s, &lib, &lim, dir.path()));
        p.start();
        if (!waitUntil([&] { return p.core()->servers()->current() != nullptr; }, 20000)) { p.stop(); return g; }
        if (p.core()->servers()->current()->info().port != s1.port()) { p.stop(); return g; }
        mx::SearchQuery q;
        q.id = 77;
        q.text = QStringLiteral("big buck bunny");
        p.search(q);
        mx::FileHashes h;
        h.ed2k = QByteArray::fromHex("00112233445566778899aabbccddeeff");
        p.findSources(QStringLiteral("dl1"), h, 5000000, QStringLiteral("udp sources test.bin"));
        waitUntil([&] { return s1.getSources > 0; }, 10000);   // the TCP request: the download is known
        QTest::qWait(4000);
        g.searches = s2.count({OP_GLOBSEARCHREQ, OP_GLOBSEARCHREQ2, OP_GLOBSEARCHREQ3});
        g.sources = s2.count({OP_GLOBGETSOURCES, OP_GLOBGETSOURCES2});
        g.pings = s2.count({OP_GLOBSERVSTATREQ});
        qInfo("udpSearch %d udpSources %d: %d UDP searches, %d UDP source requests at the other server (%d asked over TCP)",
              search, sources, g.searches, g.sources, s1.getSources);
        p.stop();
        return g;
    };
    Got g = run(true, true);
    QVERIFY(g.searches >= 1);
    QVERIFY(g.sources >= 1);
    g = run(false, true);
    QCOMPARE(g.searches, 0);
    QVERIFY(g.sources >= 1);
    g = run(true, false);
    QVERIFY(g.searches >= 1);
    QCOMPARE(g.sources, 0);
    QCOMPARE(g.pings, 0);   // ed2k/udpPing off
    g = run(false, false, true);
    QVERIFY(g.pings >= 1);  // on: the other server is pinged
}

// ed2k/uploadSlots (above 0: eD2k's own count): three clients ask for a file; that many get a slot,
// the others wait in the queue
void TstNetSettingsP2p::ed2kOwnUploadSlots()
{
    for (const int nSlots : {1, 2}) {
        Ed2kNode a(nSlots), b1(0), b2(0), b3(0);
        a.limiter.setLimits(0, 1024 * 1024);   // the sessions last long enough to be seen together
        a.limiter.setLimitLan(true);
        const QString src = makeRandomFile(a.dir.path() + QStringLiteral("/slots test file.bin"), 3 * PARTSIZE + 777);
        QVERIFY(!src.isEmpty());
        const mx::SharedFile sf = a.lib.addFile(src);
        a.plugin->setSharedFiles(a.lib.sharedFiles());
        QList<mx::RangeSource *> rss;
        QList<mxtest::SimpleDownloader *> dls;
        for (Ed2kNode *n : {&b1, &b2, &b3}) {
            mx::SearchSource ss;
            ss.net = mx::Net::ED2K;
            ss.uri = a.uri();
            mx::RangeSource *rs = n->plugin->openSource(QStringLiteral("slots"), sf.hashes, sf.size, ss);
            QVERIFY(rs);
            auto *dl = new mxtest::SimpleDownloader(n->dir.path() + QStringLiteral("/out.bin"), sf.size, sf.hashes.ed2k, this);
            dl->addSource(rs);
            rss << rs;
            dls << dl;
        }
        QVERIFY2(waitUntil([&] { return a.inState(mx::TransferState::Transferring) == nSlots
                                        && a.inState(mx::TransferState::Queued) == 3 - nSlots; }, 30000),
                 qPrintable(QStringLiteral("%1 uploading, %2 queued").arg(a.inState(mx::TransferState::Transferring))
                                .arg(a.inState(mx::TransferState::Queued))));
        QTest::qWait(2000);   // and it stays so
        QCOMPARE(a.inState(mx::TransferState::Transferring), nSlots);
        QCOMPARE(a.plugin->core()->uploads()->activeUploads(), nSlots);
        for (mx::RangeSource *rs : rss) { rs->close(); delete rs; }
        qDeleteAll(dls);
    }
}

// ----------------------------------------------------------------------------------------------- Kad
// kad/nodesDatUrl: the nodes.dat is fetched from the address given (a local web server) and Kad
// joins through the contact it lists
void TstNetSettingsP2p::kadNodesDatFromTheUrl()
{
    QUdpSocket contact;
    QVERIFY(contact.bind(QHostAddress::LocalHost, 0));
    int contactPackets = 0;
    connect(&contact, &QUdpSocket::readyRead, this, [&] {
        while (contact.hasPendingDatagrams()) {
            QByteArray d(int(contact.pendingDatagramSize()), Qt::Uninitialized);
            contact.readDatagram(d.data(), d.size());
            ++contactPackets;
        }
    });
    NodesDatEntry e;
    e.peer.id = KadId::random();
    e.peer.ip = 0x7F000001u;
    e.peer.udpPort = contact.localPort();
    e.peer.tcpPort = 4662;
    e.peer.version = 9;
    const QByteArray nodesDat = writeNodesDat({e});
    NodesDat check;
    QVERIFY(parseNodesDat(nodesDat, &check));
    QCOMPARE(check.entries.size(), 1);
    nettest::Canary web;
    web.onData = [nodesDat](QTcpSocket *s) {
        if (s->property("answered").toBool()) return;
        s->setProperty("answered", true);
        s->write("HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: " + QByteArray::number(nodesDat.size())
                 + "\r\nConnection: close\r\n\r\n" + nodesDat);
        s->disconnectFromHost();
    };
    QVERIFY(web.listen());
    KadInst k([&web](mxtest::MemSettings &s) {
        s.setValue(QStringLiteral("kad/autoBootstrap"), true);
        s.setValue(QStringLiteral("kad/nodesDatUrl"), QStringLiteral("http://127.0.0.1:%1/kad/nodes.dat").arg(web.port()));
    });
    QVERIFY2(waitUntil([&] { return web.received.contains("GET /kad/nodes.dat"); }, 10000), web.received.left(200).constData());
    QVERIFY(waitUntil([&] { return contactPackets > 0; }, 15000));   // Kad talks to the contact of that nodes.dat
    QCOMPARE(web.tcpCount(), 1);
    QCOMPARE(k.kad->nodesDatFetches(), 1);   // one download, no other address tried
}

// kad/maxPacketsPerSec: 100 contacts to bootstrap from at once; at most that many packets go out
// (the bucket starts with one second's worth), with a high limit every one of them
void TstNetSettingsP2p::kadPacketRate()
{
    std::vector<std::unique_ptr<QUdpSocket>> sinks;
    int got = 0;
    for (int i = 0; i < 100; ++i) {
        auto u = std::make_unique<QUdpSocket>();
        QVERIFY(u->bind(QHostAddress::LocalHost, 0));
        QUdpSocket *raw = u.get();
        connect(raw, &QUdpSocket::readyRead, this, [raw, &got] {
            while (raw->hasPendingDatagrams()) {
                QByteArray d(int(raw->pendingDatagramSize()), Qt::Uninitialized);
                raw->readDatagram(d.data(), d.size());
                ++got;
            }
        });
        sinks.push_back(std::move(u));
    }
    auto burst = [&](int rate) {
        KadInst k([rate](mxtest::MemSettings &s) { s.setValue(QStringLiteral("kad/maxPacketsPerSec"), rate); });
        QTest::qWait(300);
        got = 0;
        for (const auto &u : sinks) k.kad->addServer(QStringLiteral("127.0.0.1:%1").arg(u->localPort()), QString());
        QTest::qWait(500);
        const int n = got;
        qInfo("kad/maxPacketsPerSec %d: %d of 100 bootstrap requests sent in the first 0.5 s, %llu dropped", rate, n,
              k.node()->counters().droppedRate);
        return n;
    };
    const int limited = burst(10);
    QVERIFY2(limited >= 5 && limited <= 20, qPrintable(QString::number(limited)));
    const int fast = burst(1000);
    QVERIFY2(fast >= 95, qPrintable(QString::number(fast)));
}

// kad/publish, kad/storeIndex (on), kad/firewallChecks, kad/buddies between real Kad nodes:
// * a sharer with kad/publish on publishes (its keyword is stored on the others), one with it off
//   publishes nothing (its keyword is nowhere);
// * a node with kad/storeIndex on keeps what it is given (off: kadStoreIndexOff);
// * with kad/firewallChecks off nobody asks the others whether it is reachable; a node that joins with
//   it on does;
// * a firewalled node with kad/buddies on finds a buddy, one with it off never looks for one
void TstNetSettingsP2p::kadPublishStoreFirewallBuddies()
{
    auto publish = [](bool on) { return [on](mxtest::MemSettings &s) { s.setValue(QStringLiteral("kad/publish"), on); }; };
    auto store = [](bool on) { return [on](mxtest::MemSettings &s) { s.setValue(QStringLiteral("kad/storeIndex"), on); }; };
    auto firewalled = [](bool buddies) {
        return [buddies](mxtest::MemSettings &s) {
            s.setValue(QStringLiteral("kad/forceFirewalled"), true);
            s.setValue(QStringLiteral("kad/forceUdpFirewalled"), true);
            s.setValue(QStringLiteral("kad/buddies"), buddies);
        };
    };
    std::vector<std::unique_ptr<KadInst>> n;
    n.push_back(std::make_unique<KadInst>());                     // 0 hub
    n.push_back(std::make_unique<KadInst>(store(true)));          // 1
    n.push_back(std::make_unique<KadInst>(store(true)));          // 2
    n.push_back(std::make_unique<KadInst>(publish(true)));        // 3 sharer
    n.push_back(std::make_unique<KadInst>(publish(false)));       // 4 quiet sharer
    n.push_back(std::make_unique<KadInst>(firewalled(true)));     // 5 looks for a buddy
    n.push_back(std::make_unique<KadInst>(firewalled(false)));    // 6 does not
    for (size_t i = 1; i < n.size(); ++i) n[i]->kad->addServer(QStringLiteral("127.0.0.1:%1").arg(n[0]->udp()), QString());
    n[0]->kad->addServer(QStringLiteral("127.0.0.1:%1").arg(n[1]->udp()), QString());
    QVERIFY(waitUntil([&] {
        for (auto &x : n) if (!x->node()->connected() || x->node()->contacts() < 3) return false;
        return true;
    }, 60000));
    // the two sharers share one file each, with a word of its own
    for (int i : {3, 4}) {
        const QString word = i == 3 ? QStringLiteral("zorblaxpublished") : QStringLiteral("quextarunpublished");
        const QString path = makeRandomFile(QDir(n[size_t(i)]->dir.path()).filePath(word + QStringLiteral(" sample.bin")), 300000 + i);
        n[size_t(i)]->lib.addFile(path);
        n[size_t(i)]->kad->setSharedFiles(n[size_t(i)]->lib.sharedFiles());
    }
    KadNode *sharer = n[3]->node(), *quiet = n[4]->node();
    QVERIFY2(waitUntil([&] { return sharer->counters().keywordsPublished >= 1 && sharer->counters().filesPublished >= 1; }, 150000),
             qPrintable(sharer->statusText()));
    QVERIFY(waitUntil([&] { return n[5]->node()->hasBuddy(); }, 90000));   // also meanwhile
    QTest::qWait(3000);
    const qint64 now = n[0]->kad->kadNowSecs();
    auto storedOn = [&](const QString &word) {
        int c = 0;
        for (auto &x : n) c += !x->node()->index().keywordResults(kadKeywordHash(word), QByteArray(), 0, 10, now).isEmpty();
        return c;
    };
    qInfo("publish: %d nodes keep the published word, %d the unpublished one",
          storedOn(QStringLiteral("zorblaxpublished")), storedOn(QStringLiteral("quextarunpublished")));
    // kad/publish
    QVERIFY(storedOn(QStringLiteral("zorblaxpublished")) >= 1);
    QCOMPARE(storedOn(QStringLiteral("quextarunpublished")), 0);
    QCOMPARE(quiet->counters().keywordsPublished, quint64(0));
    QCOMPARE(quiet->counters().filesPublished, quint64(0));
    QCOMPARE(quiet->counters().keywordPublishes + quiet->counters().sourcePublishes, quint64(0));
    // kad/storeIndex on: every node that answered a publish keeps what it was given
    for (auto &x : n)
        if (x->node()->counters().answeredPublish > 0)
            QVERIFY(x->node()->index().keywordEntries() + x->node()->index().sourceEntries() > 0);
    // kad/buddies
    QVERIFY(n[5]->node()->counters().buddyRequests > 0);
    QCOMPARE(n[6]->node()->counters().buddyRequests, quint64(0));
    QVERIFY(!n[6]->node()->hasBuddy());
    // kad/firewallChecks: nobody asked so far; a node that joins with the checks on asks
    auto firewallAnswers = [&] {
        quint64 a = 0;
        for (auto &x : n) a += x->node()->counters().answeredFirewall;
        return a;
    };
    QCOMPARE(firewallAnswers(), quint64(0));
    n.push_back(std::make_unique<KadInst>([](mxtest::MemSettings &s) { s.setValue(QStringLiteral("kad/firewallChecks"), true); }));
    n.back()->kad->addServer(QStringLiteral("127.0.0.1:%1").arg(n[0]->udp()), QString());
    QVERIFY(waitUntil([&] { return firewallAnswers() > 0; }, 40000));
}

// kad/storeIndex off on every node but the sharer: they answer its publish requests (it gets its
// PUBLISH_RES) and keep nothing
void TstNetSettingsP2p::kadStoreIndexOff()
{
    auto off = [](mxtest::MemSettings &s) { s.setValue(QStringLiteral("kad/storeIndex"), false); };
    std::vector<std::unique_ptr<KadInst>> n;
    for (int i = 0; i < 4; ++i) n.push_back(std::make_unique<KadInst>(off));
    n.push_back(std::make_unique<KadInst>());   // 4: the sharer
    for (size_t i = 1; i < n.size(); ++i) n[i]->kad->addServer(QStringLiteral("127.0.0.1:%1").arg(n[0]->udp()), QString());
    n[0]->kad->addServer(QStringLiteral("127.0.0.1:%1").arg(n[1]->udp()), QString());
    QVERIFY(waitUntil([&] {
        for (auto &x : n) if (!x->node()->connected() || x->node()->contacts() < 2) return false;
        return true;
    }, 60000));
    const QString path = makeRandomFile(QDir(n[4]->dir.path()).filePath(QStringLiteral("glimmerstored sample.bin")), 200003);
    n[4]->lib.addFile(path);
    n[4]->kad->setSharedFiles(n[4]->lib.sharedFiles());
    KadNode *sharer = n[4]->node();
    QVERIFY(waitUntil([&] { return sharer->counters().keywordPublishes > 0 && sharer->counters().sourcePublishes > 0; }, 120000));
    QTest::qWait(2000);
    quint64 answered = 0;
    for (int i = 0; i < 4; ++i) {
        answered += n[size_t(i)]->node()->counters().answeredPublish;
        QCOMPARE(n[size_t(i)]->node()->index().keywordEntries(), 0);
        QCOMPARE(n[size_t(i)]->node()->index().sourceEntries(), 0);
    }
    qInfo("kad/storeIndex off: %llu publish requests answered, nothing kept", answered);
    QVERIFY(answered > 0);
}

// ------------------------------------------------------------------------------------------ Gnutella
// gnutella/maxUltrapeers: five ultrapeers known; the leaf keeps exactly that many, also when the
// setting changes while it runs
void TstNetSettingsP2p::gnutellaUltrapeerCount()
{
    std::vector<std::unique_ptr<UltrapeerStub>> ups;
    QStringList hosts;
    for (int i = 0; i < 5; ++i) {
        ups.push_back(std::make_unique<UltrapeerStub>());
        QVERIFY(ups.back()->listen());
        hosts << ups.back()->address();
    }
    auto openLinks = [&] {
        int c = 0;
        for (auto &u : ups) c += u->open;
        return c;
    };
    QTemporaryDir dir(QDir::currentPath() + QStringLiteral("/p2p-ultra-XXXXXX"));
    QVERIFY(dir.isValid());
    gnuttest::MemSettings s;
    gnuttest::FileLibrary lib;
    gnuttest::CountingLimiter lim;
    s.setValue(QStringLiteral("gnutella/enabled"), true);
    s.setValue(QStringLiteral("gnutella/port"), 0);
    s.setValue(QStringLiteral("gnutella/autoBootstrap"), false);
    s.setValue(QStringLiteral("gnutella/bootstrapHosts"), hosts);
    s.setValue(QStringLiteral("gnutella/maxUltrapeers"), 2);
    s.setValue(QStringLiteral("connection/firewalled"), QStringLiteral("open"));
    auto *p = new gnut::GnutellaPlugin;
    p->init(gnuttest::makeContext(&s, &lib, &lim, dir.filePath(QStringLiteral("profile"))));
    p->start();
    QVERIFY(waitUntil([&] { return p->connectedCount() == 2; }, 20000));
    QTest::qWait(3000);   // and no third one
    QCOMPARE(p->connectedCount(), 2);
    QCOMPARE(openLinks(), 2);
    s.setValue(QStringLiteral("gnutella/maxUltrapeers"), 4);   // at once: two more
    QVERIFY(waitUntil([&] { return p->connectedCount() == 4 && openLinks() == 4; }, 20000));
    s.setValue(QStringLiteral("gnutella/maxUltrapeers"), 1);   // at once: the extra ones are closed
    QVERIFY(waitUntil([&] { return p->connectedCount() == 1 && openLinks() == 1; }, 20000));
    QTest::qWait(2000);
    QCOMPARE(p->connectedCount(), 1);
    p->stop();
    delete p;
}

// gnutella/gwebcaches: the user's web cache is asked for hosts over HTTP (on the wire, a local web
// server). gnutella/udpHostCaches: the user's UDP host cache is tried (its attempt is recorded in
// the profile's host cache); the ping itself is not sent to a loopback or LAN address
// [net-3] and pinged over UDP (a cache on the own network is asked too). Without the keys neither
// is used.
void TstNetSettingsP2p::gnutellaUserCaches()
{
    struct Seen { int gwcGets = -1; qint64 uhcAttempt = -1; int uhcFailures = -1; int uhcPings = -1; };
    auto run = [](bool withKeys, Seen *seen) {
        nettest::Canary gwc;
        gwc.onData = [](QTcpSocket *s) {
            s->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            s->disconnectFromHost();
        };
        QVERIFY(gwc.listen());
        const QString uhc = QStringLiteral("127.0.0.1:%1").arg(gwc.port());
        QTemporaryDir dir(QDir::currentPath() + QStringLiteral("/p2p-caches-XXXXXX"));
        QVERIFY(dir.isValid());
        const QString profile = dir.filePath(QStringLiteral("profile"));
        retireBuiltinCaches(profile + QStringLiteral("/gnutella"));
        gnuttest::MemSettings s;
        gnuttest::FileLibrary lib;
        gnuttest::CountingLimiter lim;
        s.setValue(QStringLiteral("gnutella/enabled"), true);
        s.setValue(QStringLiteral("gnutella/port"), 0);
        s.setValue(QStringLiteral("gnutella/autoBootstrap"), true);
        s.setValue(QStringLiteral("gnutella/bootstrapHosts"), QStringList());
        s.setValue(QStringLiteral("gnutella/gwebcaches"),
                   withKeys ? QStringList{QStringLiteral("http://127.0.0.1:%1/cache/gwc.php").arg(gwc.port())} : QStringList());
        s.setValue(QStringLiteral("gnutella/udpHostCaches"), withKeys ? QStringList{uhc} : QStringList());
        auto *p = new gnut::GnutellaPlugin;
        p->init(gnuttest::makeContext(&s, &lib, &lim, profile));
        p->start();
        if (withKeys) waitUntil([&] { return gwc.received.contains("GET /cache/gwc.php") && gwc.udpCount() > 0; }, 25000);
        QTest::qWait(withKeys ? 2000 : 8000);
        p->stop();   // writes the host cache
        delete p;
        seen->gwcGets = int(gwc.received.count("GET /cache/gwc.php"));
        seen->uhcPings = gwc.udpCount();
        if (withKeys) QVERIFY2(gwc.received.contains("hostfile=1"), gwc.received.left(300).constData());
        QFile f(profile + QStringLiteral("/gnutella/hosts.json"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QJsonArray uhcs = QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("uhcs")).toArray();
        for (const QJsonValue &v : uhcs) {
            const QJsonObject o = v.toObject();
            if (o.value(QStringLiteral("url")).toString() != uhc) continue;
            seen->uhcAttempt = qint64(o.value(QStringLiteral("attempt")).toDouble());
            seen->uhcFailures = o.value(QStringLiteral("fail")).toInt();
        }
    };
    Seen with, without;
    run(true, &with);
    QVERIFY(with.gwcGets >= 1);
    QVERIFY2(with.uhcAttempt > 0, qPrintable(QString::number(with.uhcAttempt)));   // tried
    QVERIFY(with.uhcPings >= 1);      // the ping arrived (no pong from the canary: a failure)
    run(false, &without);
    QCOMPARE(without.gwcGets, 0);
    QCOMPARE(without.uhcPings, 0);
    QCOMPARE(without.uhcAttempt, qint64(-1));   // not in the cache list at all
}

// gnutella/uploadSlots (above 0: Gnutella's own count), gnutella/uploadQueueSize,
// gnutella/uploadQueuePollSec: answers on the wire to HTTP clients of the uploader
void TstNetSettingsP2p::gnutellaSlotsQueueAndPoll()
{
    QTemporaryDir dir(QDir::currentPath() + QStringLiteral("/p2p-gnutup-XXXXXX"));
    QVERIFY(dir.isValid());
    gnuttest::MemSettings s;
    gnuttest::FileLibrary lib;
    gnuttest::CountingLimiter lim;
    s.setValue(QStringLiteral("gnutella/enabled"), true);
    s.setValue(QStringLiteral("gnutella/port"), 0);
    s.setValue(QStringLiteral("gnutella/bootstrapHosts"), QStringList{QStringLiteral("127.0.0.1:1")});
    s.setValue(QStringLiteral("gnutella/autoBootstrap"), false);
    s.setValue(QStringLiteral("gnutella/uploadSlots"), 2);
    s.setValue(QStringLiteral("gnutella/uploadQueueSize"), 1);
    s.setValue(QStringLiteral("gnutella/uploadQueuePollSec"), 7);
    s.setValue(QStringLiteral("connection/firewalled"), QStringLiteral("open"));
    const QByteArray data = gnuttest::makeTestData(400000, 31);
    QVERIFY(gnuttest::writeFile(dir.filePath(QStringLiteral("share/slots.bin")), data));
    const mx::SharedFile f = lib.addFile(dir.filePath(QStringLiteral("share/slots.bin")));
    QVERIFY(gnuttest::writeFile(dir.filePath(QStringLiteral("share/other.bin")), gnuttest::makeTestData(300000, 32)));
    const mx::SharedFile f2 = lib.addFile(dir.filePath(QStringLiteral("share/other.bin")));
    auto *p = new gnut::GnutellaPlugin;
    p->init(gnuttest::makeContext(&s, &lib, &lim, dir.filePath(QStringLiteral("profile"))));
    p->start();
    p->setSharedFiles({f, f2});
    QVERIFY(waitUntil([&] { return p->listenPort() != 0; }, 5000));
    const quint16 port = p->listenPort();
    // two slots: two clients get data and keep their connections (and slots)
    GnutClient c1, c2, c3, c4;
    QVERIFY(c1.open(port) && c2.open(port) && c3.open(port) && c4.open(port));
    QByteArray h = c1.get(f.hashes.sha1, 0, 9);
    QVERIFY2(h.startsWith("HTTP/1.1 206"), h.left(120).constData());
    h = c2.get(f.hashes.sha1, 10, 19);
    QVERIFY2(h.startsWith("HTTP/1.1 206"), h.left(120).constData());
    // the third waits in the queue and is told to come back every 7 s (gnutella/uploadQueuePollSec)
    h = c3.get(f.hashes.sha1, 0, 9);
    QVERIFY2(h.startsWith("HTTP/1.1 503") && h.contains("X-Queued"), h.constData());
    QCOMPARE(GnutClient::header(h, "Retry-After"), QByteArray("7"));
    QVERIFY2(GnutClient::header(h, "X-Queue").contains("pollMin=7"), h.constData());
    QCOMPARE(p->queuedUploadIds().size(), 1);
    // the queue holds one (gnutella/uploadQueueSize): the fourth (another file, so another queue
    // place: one place is "address / file") is turned away without a place
    h = c4.get(f2.hashes.sha1, 0, 9);
    QVERIFY2(h.startsWith("HTTP/1.1 503") && !h.contains("X-Queued"), h.constData());
    QCOMPARE(p->queuedUploadIds().size(), 1);
    // one more slot: the waiting client gets data at its next request
    s.setValue(QStringLiteral("gnutella/uploadSlots"), 3);
    h = c3.get(f.hashes.sha1, 0, 9);
    QVERIFY2(h.startsWith("HTTP/1.1 206"), h.constData());
    QVERIFY(p->queuedUploadIds().isEmpty());
    p->stop();
    delete p;
}

QTEST_GUILESS_MAIN(TstNetSettingsP2p)
#include "tst_netsettings_p2p.moc"
