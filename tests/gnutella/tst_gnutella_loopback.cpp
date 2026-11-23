// SPDX-License-Identifier: GPL-3.0-or-later
// Loopback integration test: two Gnutella plugin instances (leaf A downloads, leaf B shares)
// connected through a tiny in-test ultrapeer stub that speaks the real wire protocol (0.6
// handshake, deflate both ways, QRP routing, query/hit/push routing, TCP connect-back,
// push-proxy ack, query status requests). Covers: search → hit → RangeSource download (plain,
// ranged, remotely queued, via PUSH) → identical bytes; THEX tree verified against mxhash; browse
// host (direct and via push); answering an incoming handshake.
#include "gnut_headers.h"
#include "gnut_message.h"
#include "gnut_plugin_impl.h"
#include "gnut_qrp.h"
#include "gnut_tls.h"
#include "gnut_zlib.h"
#include "gnutella_plugin.h"
#include "test_support.h"

#include "mxhash/encoding.h"
#include "mxhash/tiger_tree.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QTemporaryDir>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtTest/QtTest>
#include <memory>

using namespace mx::gnut;
using namespace gnuttest;

// ---------------------------------------------------------------- the ultrapeer stub
class UpStub : public QObject {
    Q_OBJECT
public:
    struct Peer {
        QTcpSocket *s = nullptr;
        HeaderReader hdr;
        int stage = 0;                       // 0 = wait CONNECT, 1 = wait final 200, 2 = messages
        std::unique_ptr<Inflater> inf;
        std::unique_ptr<Deflater> def;
        Framer framer;
        QrpReceiver qrp;
        QString agent;
        int qrpMessages = 0;
        QList<Message> received;
    };
    QTcpServer server;
    QList<Peer *> peers;
    QHash<QByteArray, Peer *> queryRoutes, pushRoutes;
    int queriesRouted = 0, hitsRouted = 0, pushesRouted = 0, connectBacks = 0, proxyAcks = 0;
    QList<quint16> queryStatusAnswers;
    // [enc-1] TLS: with an engine the stub accepts TLS links and announces "tls/1.0"
    std::shared_ptr<TlsEngine> tls;
    int tlsLinks = 0, plainLinks = 0;

    UpStub()
    {
        server.listen(QHostAddress::LocalHost, 0);
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *s = server.nextPendingConnection()) {
                auto *p = new Peer;
                p->s = s;
                peers << p;
                hook(p);
            }
        });
    }
    void hook(Peer *p)
    {
        QTcpSocket *s = p->s;
        connect(s, &QTcpSocket::readyRead, this, [this, p] { onRead(p); });
        connect(s, &QTcpSocket::disconnected, this, [this, p] { peers.removeAll(p); for (auto *h : {&queryRoutes, &pushRoutes}) for (auto it = h->begin(); it != h->end();) { if (it.value() == p) it = h->erase(it); else ++it; } });
    }
    ~UpStub() override { for (Peer *p : peers) p->s->disconnect(this); qDeleteAll(peers); peers.clear(); }
    quint16 port() const { return server.serverPort(); }
    Peer *peerWithAgentAndQrp() const { for (Peer *p : peers) if (p->qrp.complete() && p->qrp.table()->filled() > 0) return p; return nullptr; }

    void sendTo(Peer *p, const Message &m)
    {
        const QByteArray raw = m.encode();
        p->s->write(p->def ? p->def->compress(raw) : raw);
    }
    void sendQueryStatusRequest(Peer *p, const QByteArray &guid)
    {
        sendTo(p, Message::make(MsgVendor, buildVendorPayload("BEAR", 11, 1), 1, guid));
    }

private:
    void onRead(Peer *p)
    {
        const QByteArray data = p->s->readAll();
        if (p->stage == 0 && tls && looksLikeTlsClientHello(data) && !qobject_cast<TlsSocket *>(p->s)) {
            QTcpSocket *plain = p->s;
            plain->disconnect(this);
            auto *t = new TlsSocket(tls, TlsSocket::TlsOnly, this);
            p->s = t;
            hook(p);
            connect(t, &TlsSocket::encrypted, this, [this] { ++tlsLinks; });
            t->startServer(plain, data);
            return;
        }
        if (p->stage == 0) {
            p->hdr.feed(data);
            if (!p->hdr.complete()) return;
            const HeaderBlock h = p->hdr.block();
            p->agent = h.first(QStringLiteral("User-Agent"));
            QVERIFY(h.firstLine.startsWith(QLatin1String("GNUTELLA CONNECT/0.6")));
            QCOMPARE(h.first(QStringLiteral("X-Ultrapeer")), QStringLiteral("False"));
            QVERIFY(h.value(QStringLiteral("Accept-Encoding")).contains(QLatin1String("deflate")));
            QVERIFY(h.has(QStringLiteral("X-Query-Routing")));
            HeaderBlock r;
            r.firstLine = QStringLiteral("GNUTELLA/0.6 200 OK");
            r.add(QStringLiteral("User-Agent"), QStringLiteral("StubUP/1.0"));
            r.add(QStringLiteral("X-Ultrapeer"), QStringLiteral("True"));
            r.add(QStringLiteral("X-Query-Routing"), QStringLiteral("0.2"));
            r.add(QStringLiteral("X-Ultrapeer-Query-Routing"), QStringLiteral("0.1"));
            r.add(QStringLiteral("Accept-Encoding"), QStringLiteral("deflate"));
            r.add(QStringLiteral("Content-Encoding"), QStringLiteral("deflate"));
            r.add(QStringLiteral("X-Dynamic-Querying"), QStringLiteral("0.1"));
            r.add(QStringLiteral("Remote-IP"), QStringLiteral("127.0.0.1"));
            r.add(QStringLiteral("X-Try-Ultrapeers"), QStringLiteral("127.0.0.1:%1").arg(port()));
            if (tls) r.add(QStringLiteral("X-Features"), QStringLiteral("browse/1.0, tls/1.0"));
            if (!qobject_cast<TlsSocket *>(p->s)) ++plainLinks;
            p->s->write(r.serialize());
            p->stage = 1;
            p->hdr.reset();
            return;
        }
        QByteArray rest = data;
        if (p->stage == 1) {
            p->hdr.feed(data);
            if (!p->hdr.complete()) return;
            const HeaderBlock h = p->hdr.block();
            QCOMPARE(h.code, 200);
            if (h.value(QStringLiteral("Content-Encoding")).contains(QLatin1String("deflate"))) p->inf.reset(new Inflater);
            p->def.reset(new Deflater);          // we announced Content-Encoding: deflate
            p->stage = 2;
            rest = p->hdr.takeRemainder();
        }
        if (p->inf) {
            QByteArray plain;
            QVERIFY(p->inf->feed(rest, &plain, 1 << 22));
            p->framer.feed(plain);
        } else {
            p->framer.feed(rest);
        }
        Message m;
        while (p->framer.next(&m)) handle(p, m);
    }

    static bool qrpMatches(const QrpTable *t, const QueryMsg &q)
    {
        if (!t) return false;
        for (const QByteArray &s : q.sha1s)
            if (t->containsWord(QStringLiteral("urn:sha1:") + mx::hash::toBase32(s))) return true;
        return t->matchesQuery(q.effectiveText());
    }

    void handle(Peer *p, Message m)
    {
        p->received << m;
        switch (m.type) {
        case MsgPing:
            sendTo(p, Message::make(MsgPong, buildPongPayload(HostPort{0x7f000001, port()}, 0, 0), 1, m.guid));
            break;
        case MsgQrp: {
            ++p->qrpMessages;
            QString err;
            QVERIFY2(p->qrp.handle(m.payload, &err), qPrintable(err));
            break;
        }
        case MsgQuery: {
            QueryMsg q;
            QVERIFY(parseQuery(m.payload, &q));
            queryRoutes.insert(m.guid, p);
            m.hops++;
            m.ttl = quint8(qMax(1, m.ttl - 1));
            for (Peer *o : peers)
                if (o != p && o->stage == 2 && qrpMatches(o->qrp.table(), q)) { sendTo(o, m); ++queriesRouted; }
            break;
        }
        case MsgQueryHit: {
            Peer *to = queryRoutes.value(m.guid);
            if (m.payload.size() >= 16) pushRoutes.insert(m.payload.right(16), p);
            if (!to) break;
            m.hops++;
            sendTo(to, m);
            ++hitsRouted;
            break;
        }
        case MsgPush: {
            PushMsg pm;
            QVERIFY(parsePush(m.payload, &pm));
            Peer *to = pushRoutes.value(pm.serventGuid);
            if (!to) break;
            m.hops++;
            sendTo(to, m);
            ++pushesRouted;
            break;
        }
        case MsgVendor: {
            VendorMsg v;
            QVERIFY(parseVendor(m.payload, &v));
            if (v.vendor == "BEAR" && v.id == 7) {                // TCP connect-back
                Reader r(v.body);
                const quint16 port = r.u16();
                auto *s = new QTcpSocket(this);
                connect(s, &QTcpSocket::connected, s, [s, this] { s->write("CONNECT BACK\r\n\r\n"); s->disconnectFromHost(); ++connectBacks; });
                connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
                s->connectToHost(QHostAddress::LocalHost, port);
            } else if (v.vendor == "LIME" && v.id == 21) {         // push-proxy request → ack v2
                QByteArray body;
                putIp(body, 0x7f000001);
                putU16(body, this->port());
                sendTo(p, Message::make(MsgVendor, buildVendorPayload("LIME", 22, 2, body), 1, m.guid));
                ++proxyAcks;
            } else if (v.vendor == "BEAR" && v.id == 12) {         // query status answer
                Reader r(v.body);
                queryStatusAnswers << r.u16();
            }
            break;
        }
        default:
            break;
        }
    }
};

// ---------------------------------------------------------------- a leaf instance
struct Leaf {
    QString profile;
    MemSettings settings;
    FileLibrary lib;
    CountingLimiter limiter;
    GnutellaPlugin *plugin = nullptr;
    QList<mx::SearchHit> hits;
    QList<quint32> done;
    QList<mx::TransferInfo> uploads;
    QStringList uploadsFinished;
    QList<QPair<qint64, QList<QByteArray>>> hashSets;
    QList<mx::BrowseEntry> browsed;
    bool browseFinished = false;
    QString browseError;

    void setup(const QString &dir, quint16 upPort, const QString &fwMode)
    {
        profile = dir;
        settings.setValue(QStringLiteral("gnutella/enabled"), true);
        settings.setValue(QStringLiteral("gnutella/port"), 0);
        settings.setValue(QStringLiteral("gnutella/maxUltrapeers"), 1);
        settings.setValue(QStringLiteral("gnutella/bootstrapHosts"), QStringList{QStringLiteral("127.0.0.1:%1").arg(upPort)});
        settings.setValue(QStringLiteral("gnutella/autoBootstrap"), false);
        settings.setValue(QStringLiteral("gnutella/uploadSlots"), 2);
        settings.setValue(QStringLiteral("gnutella/uploadQueuePollSec"), 2);
        settings.setValue(QStringLiteral("connection/firewalled"), fwMode);
        plugin = new GnutellaPlugin;
        QObject::connect(plugin, &mx::NetworkPlugin::searchHits, plugin, [this](quint32, const QList<mx::SearchHit> &h) { hits += h; });
        QObject::connect(plugin, &mx::NetworkPlugin::searchDone, plugin, [this](quint32 id) { done << id; });
        QObject::connect(plugin, &mx::NetworkPlugin::uploadChanged, plugin, [this](const mx::TransferInfo &t) { uploads << t; });
        QObject::connect(plugin, &mx::NetworkPlugin::uploadFinished, plugin, [this](const QString &id) { uploadsFinished << id; });
        QObject::connect(plugin, &mx::NetworkPlugin::hashSetLearned, plugin,
                         [this](const QString &, const QString &kind, qint64 unit, const QList<QByteArray> &h) {
                             if (kind == QLatin1String("tth-leaves")) hashSets << qMakePair(unit, h);
                         });
        QObject::connect(plugin, &mx::NetworkPlugin::browseResults, plugin,
                         [this](quint32, const mx::UserRef &, const QList<mx::BrowseEntry> &e, bool fin) { browsed += e; if (fin) browseFinished = true; });
        QObject::connect(plugin, &mx::NetworkPlugin::browseFailed, plugin,
                         [this](quint32, const mx::UserRef &, const QString &why) { browseError = why; });
        plugin->init(makeContext(&settings, &lib, &limiter, profile));
    }
    ~Leaf() { delete plugin; }
};

class TstGnutellaLoopback : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void connectAndQrp();
    void incomingHandshakeAnswered();
    void searchAndHit();
    void downloadPlainWithThex();
    void downloadRanged();
    void downloadQueued();
    void browseDirect();
    void downloadViaPush();
    void browseViaPush();
    // [enc-1] TLS
    void tlsTransfersEncrypted();
    void tlsUltrapeerLink();
    void tlsRequireRefusesPlain();
    void tlsOneSidePlain();
    void tlsBothPreferAgain();
    void cleanupTestCase();

private:
    mx::SearchHit searchFor(const QString &text, bool wantFirewalled);
    bool downloadAll(const mx::SearchHit &hit, qint64 from, qint64 to, QByteArray *out, QList<int> *states = nullptr, bool waitThex = false,
                     QString *client = nullptr);

    QTemporaryDir *m_dir = nullptr;
    UpStub *m_up = nullptr;
    Leaf *m_a = nullptr, *m_b = nullptr;
    QByteArray m_data1, m_data2;
    mx::SharedFile m_f1;
    quint32 m_nextQuery = 1;
    int m_downloadNo = 0;
};

void TstGnutellaLoopback::initTestCase()
{
    m_dir = new QTemporaryDir(QDir::currentPath() + QStringLiteral("/gnut-loop-XXXXXX"));
    QVERIFY(m_dir->isValid());
    m_up = new UpStub;
    QVERIFY(m_up->port() != 0);
    m_a = new Leaf;
    m_b = new Leaf;
    m_a->setup(m_dir->filePath(QStringLiteral("a")), m_up->port(), QStringLiteral("auto"));
    m_b->setup(m_dir->filePath(QStringLiteral("b")), m_up->port(), QStringLiteral("open"));
    m_data1 = makeTestData(3 * 1024 * 1024 + 123, 11);
    m_data2 = makeTestData(5000, 12);
    const QString p1 = m_dir->filePath(QStringLiteral("share/loopback testfile alpha.bin"));
    const QString p2 = m_dir->filePath(QStringLiteral("share/another sample beta.txt"));
    QVERIFY(writeFile(p1, m_data1));
    QVERIFY(writeFile(p2, m_data2));
    m_f1 = m_b->lib.addFile(p1);
    const mx::SharedFile f2 = m_b->lib.addFile(p2);
    QCOMPARE(m_f1.hashes.sha1, QCryptographicHash::hash(m_data1, QCryptographicHash::Sha1));
    QCOMPARE(m_f1.hashes.tth.size(), 24);
    m_b->plugin->setSharedFiles({m_f1, f2});
    m_a->plugin->start();
    m_b->plugin->start();
}

void TstGnutellaLoopback::connectAndQrp()
{
    QTRY_COMPARE_WITH_TIMEOUT(m_a->plugin->connectedCount(), 1, 20000);
    QTRY_COMPARE_WITH_TIMEOUT(m_b->plugin->connectedCount(), 1, 20000);
    QCOMPARE(m_a->plugin->status().statusText, QStringLiteral("Connected to 1 ultrapeer"));
    QCOMPARE(m_a->plugin->status().state, mx::ConnState::Connected);
    // B's QRP table reached the ultrapeer and routes our keywords
    QTRY_VERIFY_WITH_TIMEOUT(m_up->peerWithAgentAndQrp() != nullptr, 10000);
    const QrpTable *t = m_up->peerWithAgentAndQrp()->qrp.table();
    QVERIFY(t->matchesQuery(QStringLiteral("testfile alpha")));
    QVERIFY(t->containsWord(QStringLiteral("urn:sha1:") + mx::hash::toBase32(m_f1.hashes.sha1)));
    QVERIFY(!t->matchesQuery(QStringLiteral("zebra")));
    QCOMPARE(m_up->peerWithAgentAndQrp()->agent, QStringLiteral("OctoMX/" MX_VERSION_STRING));
    // A learned it is reachable from the stub's TCP connect-back; push-proxy acks came back
    QTRY_VERIFY_WITH_TIMEOUT(!m_a->plugin->firewalled(), 10000);
    QVERIFY(m_up->connectBacks >= 1);
    QCOMPARE(m_a->plugin->debugStats().value(QStringLiteral("externalIp")).toString(), QStringLiteral("127.0.0.1"));
    // the bootstrap host is remembered with a success
    bool found = false;
    for (const mx::ServerEntry &e : m_a->plugin->servers())
        if (e.id == QStringLiteral("127.0.0.1:%1").arg(m_up->port())) { found = true; QCOMPARE(e.state, mx::ConnState::Connected); QCOMPARE(e.name, QStringLiteral("StubUP/1.0")); }
    QVERIFY(found);
}

void TstGnutellaLoopback::incomingHandshakeAnswered()
{
    QTcpSocket s;
    s.connectToHost(QHostAddress::LocalHost, m_b->plugin->listenPort());
    QVERIFY(s.waitForConnected(5000));
    s.write("GNUTELLA CONNECT/0.6\r\nUser-Agent: TestServent/0.1\r\nX-Ultrapeer: True\r\n\r\n");
    QByteArray resp;
    QTRY_VERIFY_WITH_TIMEOUT((resp += s.readAll()).contains("\r\n\r\n"), 5000);
    QVERIFY2(resp.startsWith("GNUTELLA/0.6 503"), resp.constData());
    QVERIFY(resp.contains("X-Ultrapeer: False"));
    QCOMPARE(m_b->plugin->debugStats().value(QStringLiteral("incomingHandshakes")).toInt(), 1);
}

mx::SearchHit TstGnutellaLoopback::searchFor(const QString &text, bool wantFirewalled)
{
    m_a->hits.clear();
    mx::SearchQuery q;
    q.id = m_nextQuery++;
    q.text = text;
    m_a->plugin->search(q);
    mx::SearchHit found;
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 15000) {
        QTest::qWait(100);
        for (const mx::SearchHit &h : m_a->hits)
            if (h.queryId == q.id && h.sources.first().extra.value(QStringLiteral("firewalled")).toBool() == wantFirewalled) found = h;
        if (!found.fileName.isEmpty()) break;
    }
    m_a->plugin->cancelSearch(q.id);
    return found;
}

void TstGnutellaLoopback::searchAndHit()
{
    const int routedBefore = m_up->queriesRouted;
    const mx::SearchHit h = searchFor(QStringLiteral("testfile alpha"), false);
    QCOMPARE(h.fileName, QStringLiteral("loopback testfile alpha.bin"));
    QCOMPARE(h.size, qint64(m_data1.size()));
    QCOMPARE(h.hashes.sha1, m_f1.hashes.sha1);
    QCOMPARE(h.hashes.tth, m_f1.hashes.tth);
    QCOMPARE(h.sources.size(), 1);
    const mx::SearchSource &s = h.sources.first();
    QCOMPARE(s.net, mx::Net::Gnutella);
    QCOMPARE(s.user.id, guidHex(m_b->plugin->serventGuid()));
    QCOMPARE(s.user.address, QStringLiteral("127.0.0.1:%1").arg(m_b->plugin->listenPort()));
    QCOMPARE(s.extra.value(QStringLiteral("vendor")).toString(), QStringLiteral("OctoMX"));
    QVERIFY(s.extra.value(QStringLiteral("browse")).toBool());
    QCOMPARE(s.freeSlots, 1);
    QVERIFY(s.uri.startsWith(QLatin1String("gnutella://127.0.0.1:")));
    QVERIFY(m_up->queriesRouted > routedBefore);
    QVERIFY(m_b->plugin->debugStats().value(QStringLiteral("queriesAnswered")).toInt() >= 1);
    // a query that does not match B's QRP table is not even routed to it
    const int routed = m_up->queriesRouted;
    mx::SearchQuery q;
    q.id = m_nextQuery++;
    q.text = QStringLiteral("nonexistent zebra");
    m_a->plugin->search(q);
    QTest::qWait(500);
    QCOMPARE(m_up->queriesRouted, routed);
    // leaf-guided dynamic querying: the ultrapeer asks A how many results it kept
    UpStub::Peer *pa = nullptr;
    for (UpStub::Peer *p : m_up->peers) if (p != m_up->peerWithAgentAndQrp()) pa = p;
    QVERIFY(pa);
    QByteArray guid;
    for (const Message &m : pa->received) if (m.type == MsgQuery) guid = m.guid;
    m_up->sendQueryStatusRequest(pa, guid);
    QTRY_VERIFY_WITH_TIMEOUT(!m_up->queryStatusAnswers.isEmpty(), 5000);
    m_a->plugin->cancelSearch(q.id);
}

bool TstGnutellaLoopback::downloadAll(const mx::SearchHit &hit, qint64 from, qint64 to, QByteArray *out, QList<int> *states, bool waitThex,
                                      QString *client)
{
    const QString id = QStringLiteral("dl-%1").arg(++m_downloadNo);
    mx::RangeSource *src = m_a->plugin->openSource(id, hit.hashes, hit.size, hit.sources.first());
    if (!src) return false;
    MemDownloader d(hit.size, from, to);
    src->setParent(&d);
    d.addSource(src);
    QElapsedTimer t;
    t.start();
    while (!d.done() && t.elapsed() < 60000) QTest::qWait(50);
    if (!d.done()) qWarning() << "download log:" << d.log();
    while (waitThex && m_a->hashSets.isEmpty() && t.elapsed() < 60000) QTest::qWait(50);   // tree fetched on the same connection
    *out = d.bytes();
    if (states) *states = d.statesSeen();
    if (client) *client = src->clientName();
    src->close();
    return d.done();
}

void TstGnutellaLoopback::downloadPlainWithThex()
{
    const mx::SearchHit h = searchFor(QStringLiteral("testfile alpha"), false);
    QVERIFY(!h.fileName.isEmpty());
    QByteArray data;
    QVERIFY(downloadAll(h, 0, -1, &data, nullptr, true));
    QCOMPARE(data.size(), m_data1.size());
    QVERIFY(data == m_data1);
    // THEX tree (X-Thex-URI → /uri-res/N2X) delivered as tth-leaves and verified against mxhash
    QTRY_VERIFY_WITH_TIMEOUT(!m_a->hashSets.isEmpty(), 15000);
    const qint64 unit = m_a->hashSets.first().first;
    const QList<QByteArray> leaves = m_a->hashSets.first().second;
    mx::hash::TigerTree tree;
    tree.update(m_data1);
    QCOMPARE(tree.finish(), m_f1.hashes.tth);
    QCOMPARE(leaves, tree.levelForUnit(unit));
    QVERIFY(leaves.size() >= 8);
    qInfo() << "THEX:" << leaves.size() << "nodes of" << unit << "bytes";
    // B reported the upload and finished it
    QTRY_VERIFY_WITH_TIMEOUT(!m_b->uploadsFinished.isEmpty(), 10000);
    bool sawTransfer = false;
    for (const mx::TransferInfo &t : m_b->uploads)
        if (t.isUpload && t.name == QStringLiteral("loopback testfile alpha.bin") && t.state == mx::TransferState::Transferring) sawTransfer = true;
    QVERIFY(sawTransfer);
    QVERIFY(m_b->limiter.total(mx::RateLimiter::Up) >= m_data1.size());
    QVERIFY(m_a->limiter.total(mx::RateLimiter::Down) >= m_data1.size());
}

void TstGnutellaLoopback::downloadRanged()
{
    const mx::SearchHit h = searchFor(QStringLiteral("testfile alpha"), false);
    QVERIFY(!h.fileName.isEmpty());
    QByteArray data;
    const qint64 from = 1000003, to = 1000003 + 700001;
    QVERIFY(downloadAll(h, from, to, &data));
    QVERIFY(data == m_data1.mid(int(from), int(to - from)));
}

void TstGnutellaLoopback::downloadQueued()
{
    m_b->settings.setValue(QStringLiteral("gnutella/uploadSlots"), 1);
    // occupy B's only slot with a raw keep-alive client
    QTcpSocket hog;
    hog.connectToHost(QHostAddress::LocalHost, m_b->plugin->listenPort());
    QVERIFY(hog.waitForConnected(5000));
    hog.write("GET /uri-res/N2R?urn:sha1:" + mx::hash::toBase32(m_f1.hashes.sha1).toLatin1() +
              " HTTP/1.1\r\nRange: bytes=0-9\r\nConnection: Keep-Alive\r\n\r\n");
    QByteArray resp;
    QTRY_VERIFY_WITH_TIMEOUT((resp += hog.readAll()).contains("\r\n\r\n"), 5000);
    QVERIFY2(resp.startsWith("HTTP/1.1 206"), resp.left(60).constData());
    QVERIFY(resp.contains("Content-Range: bytes 0-9/"));
    QVERIFY(resp.contains("X-Thex-URI: /uri-res/N2X?urn:sha1:"));
    QTimer::singleShot(4000, &hog, [&hog] { hog.disconnectFromHost(); });
    const mx::SearchHit h = searchFor(QStringLiteral("testfile alpha"), false);
    QVERIFY(!h.fileName.isEmpty());
    QByteArray data;
    QList<int> states;
    QVERIFY(downloadAll(h, 0, 600000, &data, &states));
    QVERIFY(data == m_data1.left(600000));
    QVERIFY2(states.contains(int(mx::RangeSource::Queued)), "source was never remotely queued");
    m_b->settings.setValue(QStringLiteral("gnutella/uploadSlots"), 2);
}

void TstGnutellaLoopback::browseDirect()
{
    mx::UserRef u;
    u.net = mx::Net::Gnutella;
    u.id = guidHex(m_b->plugin->serventGuid());
    u.address = QStringLiteral("127.0.0.1:%1").arg(m_b->plugin->listenPort());
    m_a->browsed.clear();
    m_a->browseFinished = false;
    m_a->plugin->browse(1, u);
    QTRY_VERIFY_WITH_TIMEOUT(m_a->browseFinished || !m_a->browseError.isEmpty(), 15000);
    QVERIFY2(m_a->browseError.isEmpty(), qPrintable(m_a->browseError));
    QCOMPARE(m_a->browsed.size(), 2);
    QStringList names;
    for (const mx::BrowseEntry &e : m_a->browsed) names << e.path;
    QVERIFY(names.contains(QStringLiteral("loopback testfile alpha.bin")));
    // privacy: browse-host disabled → refused
    m_b->settings.setValue(QStringLiteral("gnutella/browseHost"), false);
    m_a->browseError.clear();
    m_a->browseFinished = false;
    m_a->plugin->browse(2, u);
    QTRY_VERIFY_WITH_TIMEOUT(!m_a->browseError.isEmpty() || m_a->browseFinished, 15000);
    QVERIFY(m_a->browseError.contains(QLatin1String("403")));
    m_b->settings.setValue(QStringLiteral("gnutella/browseHost"), true);
}

void TstGnutellaLoopback::downloadViaPush()
{
    m_b->settings.setValue(QStringLiteral("connection/firewalled"), QStringLiteral("firewalled"));
    const mx::SearchHit h = searchFor(QStringLiteral("testfile alpha"), true);
    QVERIFY2(!h.fileName.isEmpty(), "no firewalled hit");
    QVERIFY(h.sources.first().uri.contains(QLatin1String("fw=1")));
    const int givsBefore = m_a->plugin->debugStats().value(QStringLiteral("givsReceived")).toInt();
    QByteArray data;
    QVERIFY(downloadAll(h, 0, -1, &data));
    QVERIFY(data == m_data1);
    QVERIFY(m_up->pushesRouted >= 1);
    QVERIFY(m_a->plugin->debugStats().value(QStringLiteral("givsReceived")).toInt() > givsBefore);
    QVERIFY(m_b->plugin->debugStats().value(QStringLiteral("givsSent")).toInt() >= 1);
}

void TstGnutellaLoopback::browseViaPush()
{
    mx::UserRef u;
    u.net = mx::Net::Gnutella;
    u.id = guidHex(m_b->plugin->serventGuid());       // known from the firewalled hit → push
    m_a->browsed.clear();
    m_a->browseFinished = false;
    m_a->browseError.clear();
    const int pushes = m_up->pushesRouted;
    m_a->plugin->browse(3, u);
    QTRY_VERIFY_WITH_TIMEOUT(m_a->browseFinished || !m_a->browseError.isEmpty(), 20000);
    QVERIFY2(m_a->browseError.isEmpty(), qPrintable(m_a->browseError));
    QCOMPARE(m_a->browsed.size(), 2);
    QVERIFY(m_up->pushesRouted > pushes);
}

// ---------------------------------------------------------------- [enc-1] TLS
// Both leaves run with the default "enabled" (prefer TLS): B announced TLS in its hits and A in its
// pushes, so the transfers above already ran over TLS.
void TstGnutellaLoopback::tlsTransfersEncrypted()
{
    m_b->settings.setValue(QStringLiteral("connection/firewalled"), QStringLiteral("open"));
    QVERIFY(QFile::exists(m_dir->filePath(QStringLiteral("a/gnutella/tls.pem"))));
    const QVariantMap sa = m_a->plugin->debugStats(), sb = m_b->plugin->debugStats();
    qInfo() << "A tls:" << sa.value("tlsOutOk") << "of" << sa.value("tlsOutAttempts") << "out," << sa.value("tlsInOk") << "in;"
            << "B tls:" << sb.value("tlsInOk") << "in," << sb.value("tlsOutOk") << "out";
    QVERIFY(sa.value(QStringLiteral("tlsOutOk")).toInt() >= 1);    // direct downloads + browse
    QVERIFY(sb.value(QStringLiteral("tlsInOk")).toInt() >= 1);
    QVERIFY(sb.value(QStringLiteral("tlsOutOk")).toInt() >= 1);    // B's connect-back after A's PUSH
    QVERIFY(sa.value(QStringLiteral("tlsInOk")).toInt() >= 1);
    QCOMPARE(sa.value(QStringLiteral("tlsOutFallback")).toInt(), 0);
    // the source row and B's upload row say so
    const mx::SearchHit h = searchFor(QStringLiteral("testfile alpha"), false);
    QVERIFY(!h.fileName.isEmpty());
    QVERIFY2(h.sources.first().uri.contains(QLatin1String("tls=1")), qPrintable(h.sources.first().uri));
    QByteArray data;
    QString client;
    QVERIFY(downloadAll(h, 0, 200000, &data, nullptr, false, &client));
    QVERIFY(data == m_data1.left(200000));
    QVERIFY2(client.endsWith(QLatin1String("[TLS]")), qPrintable(client));
    bool tlsUpload = false;
    for (const mx::TransferInfo &t : m_b->uploads)
        if (!t.sources.isEmpty() && t.sources.first().client.endsWith(QLatin1String("[TLS]"))) tlsUpload = true;
    QVERIFY(tlsUpload);
    // the ultrapeer stub has no TLS: plain links, and the Connection Test row says so
    QCOMPARE(m_a->plugin->encryptedLinks(), 0);
    bool row = false;
    for (const mx::DiagItem &i : m_a->plugin->diagnostics().items)
        if (i.id == QLatin1String("gnutella.encryption")) {
            row = true;
            qInfo() << "row:" << i.result << "|" << i.details;
            QCOMPARE(i.result, QStringLiteral("TLS on 0 of 1 ultrapeer link"));
            QCOMPARE(i.level, mx::DiagLevel::Ok);         // transfers were encrypted
        }
    QVERIFY(row);
}

// the stub learns TLS; "require" replaces A's plain link by a TLS one
void TstGnutellaLoopback::tlsUltrapeerLink()
{
    QString err;
    m_up->tls = TlsEngine::create(m_dir->filePath(QStringLiteral("stub.pem")), &err);
    QVERIFY2(m_up->tls, qPrintable(err));
    m_a->settings.setValue(QStringLiteral("gnutella/encryption"), QStringLiteral("forced"));
    QTRY_COMPARE_WITH_TIMEOUT(m_a->plugin->encryptedLinks(), 1, 20000);
    QCOMPARE(m_a->plugin->connectedCount(), 1);
    QVERIFY(m_up->tlsLinks >= 1);
    QVERIFY(m_a->plugin->connectionLines().first().contains(QLatin1String("TLS TLSv1")));
    for (const mx::DiagItem &i : m_a->plugin->diagnostics().items)
        if (i.id == QLatin1String("gnutella.encryption")) {
            QCOMPARE(i.result, QStringLiteral("TLS on 1 of 1 ultrapeer link"));
            QCOMPARE(i.level, mx::DiagLevel::Ok);
            QVERIFY(i.explain.contains(QLatin1String("not anonymity")));
        }
    // queries and hits still flow over the encrypted link
    const mx::SearchHit h = searchFor(QStringLiteral("testfile alpha"), false);
    QVERIFY(!h.fileName.isEmpty());
}

// A requires TLS, B has it disabled: A's TLS attempt is refused by B, B's plain push connection
// is refused by A, and plain requests to A are dropped
void TstGnutellaLoopback::tlsRequireRefusesPlain()
{
    m_b->settings.setValue(QStringLiteral("gnutella/encryption"), QStringLiteral("disabled"));
    const mx::SearchHit h = searchFor(QStringLiteral("testfile alpha"), false);
    QVERIFY(!h.fileName.isEmpty());
    QVERIFY(!h.sources.first().uri.contains(QLatin1String("tls=1")));
    const int bRefusedBefore = m_b->plugin->debugStats().value(QStringLiteral("tlsInRefused")).toInt();
    const int aFailedBefore = m_a->plugin->debugStats().value(QStringLiteral("tlsOutFailed")).toInt();
    mx::RangeSource *src = m_a->plugin->openSource(QStringLiteral("dl-require"), h.hashes, h.size, h.sources.first());
    QVERIFY(src);
    MemDownloader d(h.size, 0, 100000);
    src->setParent(&d);
    d.addSource(src);
    QTRY_VERIFY_WITH_TIMEOUT(m_a->plugin->debugStats().value(QStringLiteral("tlsOutFailed")).toInt() > aFailedBefore, 15000);
    QTRY_VERIFY_WITH_TIMEOUT(m_b->plugin->debugStats().value(QStringLiteral("tlsInRefused")).toInt() > bRefusedBefore, 5000);
    QVERIFY(d.received() == 0);
    src->close();
    // a plain HTTP request to A gets no answer
    QTcpSocket raw;
    raw.connectToHost(QHostAddress::LocalHost, m_a->plugin->listenPort());
    QVERIFY(raw.waitForConnected(5000));
    raw.write("GET /uri-res/N2R?urn:sha1:AAAA HTTP/1.1\r\n\r\n");
    QTRY_VERIFY_WITH_TIMEOUT(raw.state() == QAbstractSocket::UnconnectedState, 5000);
    QVERIFY(raw.readAll().isEmpty());
}

// prefer on A, disabled on B: plain transfer, no TLS attempt
void TstGnutellaLoopback::tlsOneSidePlain()
{
    m_a->settings.setValue(QStringLiteral("gnutella/encryption"), QStringLiteral("enabled"));
    const mx::SearchHit h = searchFor(QStringLiteral("testfile alpha"), false);
    QVERIFY(!h.fileName.isEmpty());
    const int attempts = m_a->plugin->debugStats().value(QStringLiteral("tlsOutAttempts")).toInt();
    QByteArray data;
    QString client;
    QVERIFY(downloadAll(h, 0, 300000, &data, nullptr, false, &client));
    QVERIFY(data == m_data1.left(300000));
    QVERIFY(!client.contains(QLatin1String("[TLS]")));
    QCOMPARE(m_a->plugin->debugStats().value(QStringLiteral("tlsOutAttempts")).toInt(), attempts);
    // the TLS ultrapeer link stays (prefer accepts both)
    QCOMPARE(m_a->plugin->encryptedLinks(), 1);
}

// both prefer again: the source is encrypted; a host that announces TLS but breaks the
// handshake gets a plain retry (fallback)
void TstGnutellaLoopback::tlsBothPreferAgain()
{
    m_b->settings.setValue(QStringLiteral("gnutella/encryption"), QStringLiteral("enabled"));
    const mx::SearchHit h = searchFor(QStringLiteral("testfile alpha"), false);
    QVERIFY(!h.fileName.isEmpty());
    QByteArray data;
    QString client;
    QVERIFY(downloadAll(h, 0, 300000, &data, nullptr, false, &client));
    QVERIFY(data == m_data1.left(300000));
    QVERIFY(client.endsWith(QLatin1String("[TLS]")));
    // fallback: B claims TLS (tls=1 in the descriptor) but has it disabled now
    m_b->settings.setValue(QStringLiteral("gnutella/encryption"), QStringLiteral("disabled"));
    const int fallbacks = m_a->plugin->debugStats().value(QStringLiteral("tlsOutFallback")).toInt();
    QVERIFY(downloadAll(h, 0, 100000, &data, nullptr, false, &client));
    QVERIFY(data == m_data1.left(100000));
    QVERIFY(!client.contains(QLatin1String("[TLS]")));
    QCOMPARE(m_a->plugin->debugStats().value(QStringLiteral("tlsOutFallback")).toInt(), fallbacks + 1);
    m_b->settings.setValue(QStringLiteral("gnutella/encryption"), QStringLiteral("enabled"));
}

void TstGnutellaLoopback::cleanupTestCase()
{
    m_a->plugin->stop();
    m_b->plugin->stop();
    // host cache persisted
    QVERIFY(QFile::exists(m_dir->filePath(QStringLiteral("a/gnutella/hosts.json"))));
    delete m_a;
    delete m_b;
    delete m_up;
    delete m_dir;
}

QTEST_GUILESS_MAIN(TstGnutellaLoopback)
#include "tst_gnutella_loopback.moc"
