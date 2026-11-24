// SPDX-License-Identifier: GPL-3.0-or-later
// [net-3] transfers/maxUploads ("Max total uploads") is one total over the networks that queue
// uploads: the OctoMX network, eD2k and Gnutella ask one pool (mx/upload_slots.h). Checked on the
// pool (fair between networks, a network that does not take its slot in time does not block the
// others, "Start Transfer" / the small file bypass beyond the total, 0 = none) and between real
// plugins over 127.0.0.1: two slots in total, an OctoMX upload, an eD2k upload and a Gnutella
// upload; per-user limits and the small file bypass keep working; a network's own slot count
// (ed2k/uploadSlots, gnutella/uploadSlots above 0) stays outside the pool.
#include "mx/simple_settings.h"
#include "mx/upload_slots.h"

#include "core/rate_limiter.h"
#include "ed2k_core.h"
#include "ed2k_plugin_impl.h"
#include "ed2k_opcodes.h"
#include "ed2k_util.h"
#include "gnut_plugin_impl.h"
#include "ovl_test_support.h"
#include "mxhash/encoding.h"

#include "../ed2k/test_support.h"
#include "../gnutella/test_support.h"

#include <QtCore/QRandomGenerator>
#include <QtCore/QTemporaryDir>
#include <QtNetwork/QTcpSocket>
#include <QtTest/QSignalSpy>
#include <QtTest/QtTest>

using mx::Net;
using mx::UploadSlots;
using namespace ovltest;

namespace {

// a raw Gnutella HTTP client (as in tst_gnutella_glue3): one request, the head of the answer
struct GnutClient {
    QTcpSocket s;
    bool open(quint16 port)
    {
        s.connectToHost(QHostAddress::LocalHost, port);
        return waitFor([&] { return s.state() == QAbstractSocket::ConnectedState; }, 5000);
    }
    QByteArray get(const QByteArray &sha1, qint64 from, qint64 to, const QByteArray &parq = QByteArray())
    {
        QByteArray req = "GET /uri-res/N2R?urn:sha1:" + mx::hash::toBase32(sha1).toLatin1() + " HTTP/1.1\r\n"
                         "Host: 127.0.0.1\r\nUser-Agent: pool-test/1.0\r\nX-Queue: 1.0\r\nConnection: Keep-Alive\r\n"
                         "Range: bytes=" + QByteArray::number(from) + "-" + QByteArray::number(to) + "\r\n";
        if (!parq.isEmpty()) req += "X-Queued: ID=" + parq + "\r\n";
        req += "\r\n";
        s.readAll();
        s.write(req);
        QByteArray resp;
        waitFor([&] { resp += s.readAll(); return resp.contains("\r\n\r\n") || s.state() != QAbstractSocket::ConnectedState; }, 10000);
        const int end = int(resp.indexOf("\r\n\r\n"));
        return end < 0 ? resp : resp.left(end + 4);
    }
    static QByteArray parqId(const QByteArray &head)
    {
        for (const QByteArray &l : head.split('\n'))
            if (l.toLower().startsWith("x-queued:")) {
                const int i = int(l.indexOf("ID="));
                return i < 0 ? QByteArray() : l.mid(i + 3).split(';').first().trimmed();
            }
        return QByteArray();
    }
};

QString makeFile(const QString &path, qint64 size, quint32 seed)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return QString();
    QRandomGenerator rng(seed);
    QByteArray buf(1 << 20, '\0');
    while (f.size() < size) {
        rng.fillRange(reinterpret_cast<quint32 *>(buf.data()), buf.size() / 4);
        f.write(buf.left(int(qMin<qint64>(buf.size(), size - f.size()))));
    }
    return path;
}

// an OctoMX network node that uploads through the pool
struct PoolOverlay {
    QString profile = freshDir(QStringLiteral("pool-ovl"));
    mx::SimpleSettings settings;
    TestLibrary library;
    mx::ovl::OverlayPlugin *plugin = nullptr;
    QHash<QString, mx::TransferInfo> rows;
    QStringList finished;
    explicit PoolOverlay(UploadSlots *pool)
    {
        settings.setValue(QString(mx::keys::identity::ScreenName), QStringLiteral("PoolOwner"));
        mx::PluginContext ctx;
        ctx.settings = &settings;
        ctx.library = &library;
        ctx.uploadSlots = pool;
        ctx.profileDir = profile;
        ctx.dataDir = profile + QStringLiteral("/overlay");
        ctx.screenName = QStringLiteral("PoolOwner");
        ctx.clientName = QStringLiteral("OctoMX");
        ctx.clientVersion = QStringLiteral("5.4-test");
        plugin = static_cast<mx::ovl::OverlayPlugin *>(mx::createOverlayPlugin());
        mx::ovl::OverlayPlugin::TestOptions o;
        o.dht = o.mdns = o.reconnect = o.pex = false;
        plugin->setTestOptions(o);
        plugin->init(ctx);
        QObject::connect(plugin, &mx::NetworkPlugin::uploadChanged, plugin, [this](const mx::TransferInfo &t) { rows.insert(t.id, t); });
        QObject::connect(plugin, &mx::NetworkPlugin::uploadFinished, plugin, [this](const QString &id) { finished << id; });
        plugin->start();
    }
    ~PoolOverlay()
    {
        plugin->stop();
        delete plugin;
        mx::ovl::RatsNode::waitAllStopped(5000);
        QDir(profile).removeRecursively();
    }
    QString addr() const { return QStringLiteral("127.0.0.1:%1").arg(plugin->listenPort()); }
    mx::SearchSource sourceFor() const
    {
        mx::SearchSource s;
        s.net = Net::Overlay;
        s.user = plugin->localUser();
        s.extra.insert(QStringLiteral("node"), plugin->nodeId());
        return s;
    }
    int transferring() const
    {
        int n = 0;
        for (const mx::TransferInfo &t : rows) n += t.state == mx::TransferState::Transferring && !finished.contains(t.id);
        return n;
    }
};

bool linkTo(PoolOverlay &o, Instance &b)
{
    const int n = o.plugin->connectedUsers();
    b.plugin->connectPeer(o.addr());
    return waitFor([&] { return o.plugin->connectedUsers() > n && b.plugin->connectedUsers() > 0; }, 10000);
}

mx::RangeSource *openOverlay(Instance &from, PoolOverlay &o, const mx::SharedFile &f, const QString &id)
{
    mx::FileHashes h;
    h.sha1 = f.hashes.sha1;
    mx::RangeSource *s = from.plugin->openSource(id, h, f.size, o.sourceFor());
    if (s) s->open();
    return s;
}

void drop(mx::RangeSource *s)
{
    if (!s) return;
    s->close();
    s->deleteLater();
}

// an eD2k node (as in tst_ed2k_glue3), optionally uploading through the pool
struct Ed2kNode {
    mxtest::MemSettings settings;
    mxtest::FileLibrary lib;
    mx::CoreRateLimiter limiter{nullptr};
    QTemporaryDir dir;
    mx::ed2k::Ed2kPlugin *plugin = nullptr;
    QHash<QString, mx::TransferInfo> rows;
    QStringList finished;
    explicit Ed2kNode(UploadSlots *pool, int ownSlots = 0)
    {
        for (const auto &kv : {std::pair<const char *, QVariant>{"ed2k/tcpPort", 0}, {"ed2k/udpPort", 0}, {"ed2k/autoConnect", false},
                               {"ed2k/autoUpdateServerList", false}, {"ed2k/udpPing", false}, {"ed2k/udpSearch", false},
                               {"ed2k/udpSources", false}, {"ed2k/allowLan", true}, {"ed2k/obfuscation", false}})
            settings.setValue(QString::fromLatin1(kv.first), kv.second);
        settings.setValue(QStringLiteral("ed2k/uploadSlots"), ownSlots);
        plugin = new mx::ed2k::Ed2kPlugin();
        QObject::connect(plugin, &mx::NetworkPlugin::uploadChanged, plugin, [this](const mx::TransferInfo &t) { rows.insert(t.id, t); });
        QObject::connect(plugin, &mx::NetworkPlugin::uploadFinished, plugin, [this](const QString &id) { finished << id; });
        mx::PluginContext ctx = mxtest::makeContext(&settings, &lib, &limiter, dir.path());
        ctx.uploadSlots = pool;
        plugin->init(ctx);
        plugin->start();
    }
    ~Ed2kNode()
    {
        plugin->stop();
        delete plugin;
    }
    mx::SearchSource sourceFor() const
    {
        mx::SearchSource ss;
        ss.net = Net::ED2K;
        ss.uri = QStringLiteral("ed2k-peer://127.0.0.1:%1?uid=%2").arg(plugin->core()->tcpPort())
                     .arg(QString::fromLatin1(plugin->core()->userHash().toHex()));
        return ss;
    }
    int transferring() const
    {
        int n = 0;
        for (const mx::TransferInfo &t : rows) n += t.state == mx::TransferState::Transferring && !finished.contains(t.id);
        return n;
    }
};

// a Gnutella node uploading through the pool (or with its own slot count)
struct GnutNode {
    QTemporaryDir dir;
    gnuttest::MemSettings settings;
    gnuttest::FileLibrary lib;
    mx::gnut::GnutellaPlugin *plugin = nullptr;
    explicit GnutNode(UploadSlots *pool, int ownSlots = 0)
    {
        settings.setValue(QStringLiteral("gnutella/enabled"), true);
        settings.setValue(QStringLiteral("gnutella/port"), 0);
        settings.setValue(QStringLiteral("gnutella/bootstrapHosts"), QStringList{QStringLiteral("127.0.0.1:1")});
        settings.setValue(QStringLiteral("gnutella/autoBootstrap"), false);
        settings.setValue(QStringLiteral("gnutella/uploadSlots"), ownSlots);
        settings.setValue(QStringLiteral("gnutella/uploadQueuePollSec"), 2);
        settings.setValue(QStringLiteral("connection/firewalled"), QStringLiteral("open"));
        plugin = new mx::gnut::GnutellaPlugin;
        mx::PluginContext ctx = gnuttest::makeContext(&settings, &lib, nullptr, dir.filePath(QStringLiteral("profile")));
        ctx.uploadSlots = pool;
        plugin->init(ctx);
        plugin->start();
    }
    ~GnutNode()
    {
        plugin->stop();
        delete plugin;
    }
};

} // namespace

class TstUploadPool : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { mx::registerMetaTypes(); }

    // the pool alone: one total, fair between networks, nobody blocks the others for long
    void poolIsOneFairTotal()
    {
        mx::SimpleSettings s;
        s.setValue(QStringLiteral("transfers/maxUploads"), 3);
        UploadSlots p(&s);
        QCOMPARE(p.total(), 3);
        QVERIFY(p.acquire(Net::Overlay, QStringLiteral("o1")));
        QVERIFY(p.acquire(Net::Overlay, QStringLiteral("o2")));
        // eD2k waits and holds none: the last free slot is kept for it
        p.setWaiting(Net::ED2K, 1, 2000);
        QCOMPARE(p.available(Net::Overlay), 0);
        QVERIFY(!p.acquire(Net::Overlay, QStringLiteral("o3")));
        QCOMPARE(p.available(Net::ED2K), 1);
        QVERIFY(p.acquire(Net::ED2K, QStringLiteral("e1")));
        // full: one total, whatever the network
        QVERIFY(!p.acquire(Net::Gnutella, QStringLiteral("g1")));
        QCOMPARE(p.used(), 3);
        // "Start Transfer" / the small file bypass: beyond the total, counted
        QVERIFY(p.acquire(Net::Gnutella, QStringLiteral("g-forced"), true));
        QCOMPARE(p.used(), 4);
        QCOMPARE(p.available(Net::Overlay), 0);
        {
            QSignalSpy freed(&p, &UploadSlots::slotsFreed);
            p.release(QStringLiteral("g-forced"));
            p.release(QStringLiteral("o1"));
            QVERIFY(freed.wait(1000));   // the queues hear it
        }
        // overlay 1, eD2k 1, Gnutella 0 and waiting: the free slot is Gnutella's first
        p.setWaiting(Net::Gnutella, 2, 1000);
        p.setWaiting(Net::Overlay, 5, 1000);
        QTest::qWait(50);
        QSignalSpy freed(&p, &UploadSlots::slotsFreed);
        QCOMPARE(p.available(Net::Overlay), 0);
        QCOMPARE(p.available(Net::ED2K), 0);
        QCOMPARE(p.available(Net::Gnutella), 1);
        // Gnutella does not take it within its grace time: the pool says so and the others may
        QVERIFY(freed.wait(3000));
        QCOMPARE(p.available(Net::Overlay), 1);
        QVERIFY(p.acquire(Net::Overlay, QStringLiteral("o4")));
        // the next free slot: Gnutella first again (a fresh grace), no network starves
        p.release(QStringLiteral("o2"));
        QCOMPARE(p.available(Net::Overlay), 0);
        QCOMPARE(p.available(Net::Gnutella), 1);
        QVERIFY(p.acquire(Net::Gnutella, QStringLiteral("g2")));
        QCOMPARE(p.usedBy(Net::Overlay), 1);
        QCOMPARE(p.usedBy(Net::ED2K), 1);
        QCOMPARE(p.usedBy(Net::Gnutella), 1);
        // nobody waits elsewhere: a network may use every free slot
        p.setWaiting(Net::Gnutella, 0, 1000);
        p.setWaiting(Net::ED2K, 0, 1000);
        s.setValue(QStringLiteral("transfers/maxUploads"), 5);
        QCOMPARE(p.available(Net::Overlay), 2);
        // 0: no uploads on these networks
        s.setValue(QStringLiteral("transfers/maxUploads"), 0);
        QCOMPARE(p.available(Net::Overlay), 0);
        QVERIFY(!p.acquire(Net::Overlay, QStringLiteral("o5")));
    }

    // real plugins: two slots in total over the OctoMX network, eD2k and Gnutella
    void oneTotalOverThreeNetworks()
    {
        mx::SimpleSettings core;
        core.setValue(QStringLiteral("transfers/maxUploads"), 2);
        UploadSlots pool(&core);
        PoolOverlay ovl(&pool);
        ovl.settings.setValue(QStringLiteral("transfers/maxUploadsPerUser"), 1);
        ovl.settings.setValue(QStringLiteral("queue/smallFileBypassKB"), 128);
        const mx::SharedFile o1 = ovl.library.addGenerated(ovl.profile, QStringLiteral("Pool One.bin"), 2 * 1024 * 1024, 1);
        const mx::SharedFile o2 = ovl.library.addGenerated(ovl.profile, QStringLiteral("Pool Two.bin"), 2 * 1024 * 1024, 2);
        const mx::SharedFile small = ovl.library.addGenerated(ovl.profile, QStringLiteral("Pool Small.txt"), 60 * 1024, 3);
        ovl.plugin->setSharedFiles(ovl.library.files);
        Instance a1(QStringLiteral("PoolAsker1")), a2(QStringLiteral("PoolAsker2"));
        a1.start();
        a2.start();
        QVERIFY(linkTo(ovl, a1));
        QVERIFY(linkTo(ovl, a2));

        // per user (transfers/maxUploadsPerUser 1): a second file of the same user waits although
        // the pool has room
        mx::RangeSource *s1 = openOverlay(a1, ovl, o1, QStringLiteral("p1"));
        QVERIFY(waitFor([&] { return s1->state() == mx::RangeSource::Ready; }, 15000));
        mx::RangeSource *s2 = openOverlay(a1, ovl, o2, QStringLiteral("p2"));
        QVERIFY(waitFor([&] { return s2->state() == mx::RangeSource::Queued; }, 15000));
        QCOMPARE(pool.used(), 1);
        drop(s1);
        QVERIFY(waitFor([&] { return s2->state() == mx::RangeSource::Ready; }, 15000));   // its turn now
        drop(s2);
        QVERIFY(waitFor([&] { return pool.used() == 0; }, 10000));

        // Gnutella takes slot 1 (the client keeps the connection)
        GnutNode gn(&pool);
        const QString gpath = makeFile(gn.dir.filePath(QStringLiteral("share/pool gnut.bin")), 400000, 4);
        const mx::SharedFile gf = gn.lib.addFile(gpath);
        gn.plugin->setSharedFiles({gf});
        QVERIFY(waitFor([&] { return gn.plugin->listenPort() != 0; }, 5000));
        GnutClient c1;
        QVERIFY(c1.open(gn.plugin->listenPort()));
        const QByteArray h1 = c1.get(gf.hashes.sha1, 0, 9);
        QVERIFY2(h1.startsWith("HTTP/1.1 206"), h1.left(120).constData());
        QCOMPARE(pool.usedBy(Net::Gnutella), 1);

        // eD2k takes slot 2 (a slow transfer: the uploader sends 200 KB/s)
        Ed2kNode up(&pool), down(nullptr);
        up.limiter.setLimits(0, 200 * 1024);
        up.limiter.setLimitLan(true);
        const QString epath = makeFile(up.dir.filePath(QStringLiteral("pool ed2k.bin")), 3 * mx::ed2k::PARTSIZE, 5);
        const mx::SharedFile ef = up.lib.addFile(epath);
        up.plugin->setSharedFiles(up.lib.sharedFiles());
        mx::RangeSource *ers = down.plugin->openSource(QStringLiteral("pool-e"), ef.hashes, ef.size, up.sourceFor());
        QVERIFY(ers);
        auto *dl = new mxtest::SimpleDownloader(down.dir.path() + QStringLiteral("/out.bin"), ef.size, ef.hashes.ed2k, this);
        dl->addSource(ers);
        QVERIFY(waitFor([&] { return up.transferring() == 1; }, 20000));
        QCOMPARE(pool.used(), 2);

        // the total is reached: the OctoMX request and a second Gnutella client wait
        mx::RangeSource *q1 = openOverlay(a1, ovl, o1, QStringLiteral("q1"));
        QVERIFY(waitFor([&] { return q1->state() == mx::RangeSource::Queued; }, 15000));
        GnutClient c2;
        QVERIFY(c2.open(gn.plugin->listenPort()));
        QByteArray h2 = c2.get(gf.hashes.sha1, 0, 99999);
        QVERIFY2(h2.startsWith("HTTP/1.1 503"), h2.left(120).constData());
        const QByteArray parq = GnutClient::parqId(h2);
        QVERIFY(!parq.isEmpty());
        QCOMPARE(pool.used(), 2);
        QCOMPARE(ovl.transferring() + up.transferring() + pool.usedBy(Net::Gnutella), 2);
        qInfo("full: OctoMX %d, eD2k %d, Gnutella %d of 2", pool.usedBy(Net::Overlay), pool.usedBy(Net::ED2K), pool.usedBy(Net::Gnutella));

        // the small file bypass still goes beyond the total (queue/smallFileBypassKB)
        mx::RangeSource *sm = openOverlay(a2, ovl, small, QStringLiteral("sm"));
        QVERIFY(waitFor([&] { return sm->state() == mx::RangeSource::Ready; }, 15000));
        QCOMPARE(pool.used(), 3);
        drop(sm);
        QVERIFY(waitFor([&] { return pool.used() == 2; }, 10000));

        // Gnutella's first client hangs up: OctoMX and Gnutella both wait with no slot; the OctoMX
        // network, which is told at once, takes it
        c1.s.disconnectFromHost();
        QVERIFY(waitFor([&] { return q1->state() == mx::RangeSource::Ready; }, 15000));
        QCOMPARE(pool.usedBy(Net::Overlay), 1);
        h2 = c2.get(gf.hashes.sha1, 0, 99999, parq);
        QVERIFY2(h2.startsWith("HTTP/1.1 503"), h2.left(120).constData());   // still full

        // fair: a second OctoMX user waits as well; when eD2k's upload ends, the free slot is kept
        // for Gnutella (it waits and holds none, OctoMX holds one) until its client asks again
        mx::RangeSource *q2 = openOverlay(a2, ovl, o2, QStringLiteral("q2"));
        QVERIFY(waitFor([&] { return q2->state() == mx::RangeSource::Queued; }, 15000));
        drop(ers);
        delete dl;
        QVERIFY(waitFor([&] { return pool.usedBy(Net::ED2K) == 0; }, 20000));
        QTest::qWait(1500);
        QCOMPARE(q2->state(), mx::RangeSource::Queued);   // not taken by the OctoMX network
        h2 = c2.get(gf.hashes.sha1, 0, 9, parq);
        QVERIFY2(h2.startsWith("HTTP/1.1 206"), h2.left(120).constData());
        QCOMPARE(pool.usedBy(Net::Gnutella), 1);
        QCOMPARE(pool.usedBy(Net::Overlay), 1);
        QCOMPARE(q2->state(), mx::RangeSource::Queued);
        // the OctoMX user gets the next one
        drop(q1);
        QVERIFY(waitFor([&] { return q2->state() == mx::RangeSource::Ready; }, 15000));
        drop(q2);
        c2.s.disconnectFromHost();
        QVERIFY(waitFor([&] { return pool.used() == 0; }, 15000));
    }

    // a network whose page sets its own slot count keeps it outside the total
    void ownSlotCountsStayOutside()
    {
        mx::SimpleSettings core;
        core.setValue(QStringLiteral("transfers/maxUploads"), 1);
        UploadSlots pool(&core);
        QVERIFY(pool.acquire(Net::Overlay, QStringLiteral("someone")));   // the total is used up
        GnutNode gn(&pool, 2);   // gnutella/uploadSlots 2: its own
        const QString gpath = makeFile(gn.dir.filePath(QStringLiteral("share/own.bin")), 200000, 6);
        const mx::SharedFile gf = gn.lib.addFile(gpath);
        gn.plugin->setSharedFiles({gf});
        QVERIFY(waitFor([&] { return gn.plugin->listenPort() != 0; }, 5000));
        GnutClient a, b, c;
        QVERIFY(a.open(gn.plugin->listenPort()) && b.open(gn.plugin->listenPort()) && c.open(gn.plugin->listenPort()));
        QVERIFY(a.get(gf.hashes.sha1, 0, 9).startsWith("HTTP/1.1 206"));
        QVERIFY(b.get(gf.hashes.sha1, 10, 19).startsWith("HTTP/1.1 206"));
        QVERIFY(c.get(gf.hashes.sha1, 20, 29).startsWith("HTTP/1.1 503"));   // its own 2 are used
        QCOMPARE(pool.used(), 1);   // nothing taken from the total
    }
};

QTEST_GUILESS_MAIN(TstUploadPool)
#include "tst_upload_pool.moc"
