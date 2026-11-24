// SPDX-License-Identifier: GPL-3.0-or-later
// [net-3] A file that stops being shared stops being offered on every network at once. The uploader
// side is fed by the core's Library (a real folder, as in OctoMX); the file leaves the shares by an
// exclusion rule, by its type, by being removed from Shared Files or by its folder being unshared.
// A second instance then asks by search, browse and hash, and a transfer that was already running
// when the file left stops, with the reason on the uploader's row:
//   OctoMX network: search, browse, request by hash, running upload, queued request
//   eD2k: the server's offer list (offered again without it), Kad publishing, request by hash,
//         running upload, queued client
//   Gnutella: browse, request by hash, running upload, queue place
//   BitTorrent: a library file seeded as a torrent (bt/seedLibrary) and a finished download that
//         was shared (bt/seedCompleted): seeding stops, and starts again when it is shared again
#include "net_test_servers.h"

#include "core/database.h"
#include "core/library.h"
#include "core/rate_limiter.h"
#include "ed2k_core.h"
#include "ed2k_opcodes.h"
#include "ed2k_plugin_impl.h"
#include "gnut_plugin_impl.h"
#include "kad/kad_node.h"
#include "kad/kad_plugin.h"
#include "bt_test_util.h"
#include "ovl_test_support.h"

#include "../ed2k/test_support.h"
#include "../gnutella/test_support.h"

#include "mxhash/encoding.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QRandomGenerator>
#include <QtCore/QTemporaryDir>
#include <QtNetwork/QTcpSocket>
#include <QtTest/QSignalSpy>
#include <QtTest/QtTest>

using namespace ovltest;
using mx::Net;

namespace {

void writeRandom(const QString &path, qint64 size, quint32 seed)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    QRandomGenerator rng(seed);
    QByteArray buf(1 << 20, '\0');
    while (f.size() < size) {
        rng.fillRange(reinterpret_cast<quint32 *>(buf.data()), buf.size() / 4);
        f.write(buf.left(int(qMin<qint64>(buf.size(), size - f.size()))));
    }
}

// the core's Library over a folder of its own; `settings` is the one the networks read as well
struct Lib {
    QString profile;
    QString share;
    mx::Settings *settings;
    mx::Database db;
    std::unique_ptr<mx::Library> lib;
    Lib(const QString &tag, mx::Settings *s) : profile(freshDir(tag)), share(profile + QStringLiteral("/share")), settings(s)
    {
        QDir().mkpath(share);
        settings->setValue(QString(mx::keys::library::WatchFolders), false);
        QVERIFY(db.open(profile + QStringLiteral("/octomx.db")));
        lib = std::make_unique<mx::Library>(settings, &db);
    }
    ~Lib()
    {
        lib->stop();
        lib.reset();
        db.close();
        QDir(profile).removeRecursively();
    }
    void start()
    {
        lib->addFolder(share, true);
        lib->start();
        QVERIFY(lib->waitIdle(60000));
    }
    mx::SharedFile file(const QString &name) const
    {
        for (const mx::SharedFile &f : lib->sharedFiles())
            if (f.name == name) return f;
        return {};
    }
    // the networks hear it as in OctoMX (RealCore wires Library::sharedFilesUpdated to every plugin)
    void feed(mx::NetworkPlugin *p)
    {
        QObject::connect(lib.get(), &mx::Library::sharedFilesUpdated, p, [p](const QList<mx::SharedFile> &f) { p->setSharedFiles(f); });
        p->setSharedFiles(lib->sharedFiles());
    }
};

// how long the change took to reach a network, for the log
struct Stopwatch {
    QElapsedTimer t;
    Stopwatch() { t.start(); }
    qint64 ms() const { return t.elapsed(); }
};

void drop(mx::RangeSource *s)
{
    if (!s) return;
    s->close();
    s->deleteLater();
}

// eD2k packets a fake server got: opcode + payload
QList<QPair<quint8, QByteArray>> ed2kPackets(const QByteArray &stream)
{
    QList<QPair<quint8, QByteArray>> out;
    int i = 0;
    while (i + 6 <= stream.size()) {
        if (quint8(stream[i]) != 0xE3) { ++i; continue; }
        const quint32 len = quint32(quint8(stream[i + 1])) | quint32(quint8(stream[i + 2])) << 8 | quint32(quint8(stream[i + 3])) << 16
                            | quint32(quint8(stream[i + 4])) << 24;
        if (len == 0 || i + 5 + int(len) > stream.size()) break;
        out << qMakePair(quint8(stream[i + 5]), stream.mid(i + 6, int(len) - 1));
        i += 5 + int(len);
    }
    return out;
}

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

// a raw Gnutella HTTP client
struct GnutClient {
    QTcpSocket s;
    QByteArray body;
    bool open(quint16 port)
    {
        s.connectToHost(QHostAddress::LocalHost, port);
        return waitFor([&] { return s.state() == QAbstractSocket::ConnectedState; }, 5000);
    }
    QByteArray get(const QByteArray &sha1, qint64 from, qint64 to)
    {
        const QByteArray req = "GET /uri-res/N2R?urn:sha1:" + mx::hash::toBase32(sha1).toLatin1() + " HTTP/1.1\r\n"
                               "Host: 127.0.0.1\r\nUser-Agent: unshare-test/1.0\r\nX-Queue: 1.0\r\nConnection: Keep-Alive\r\n"
                               "Range: bytes=" + QByteArray::number(from) + "-" + QByteArray::number(to) + "\r\n\r\n";
        s.readAll();
        s.write(req);
        QByteArray resp;
        waitFor([&] { resp += s.readAll(); return resp.contains("\r\n\r\n") || s.state() != QAbstractSocket::ConnectedState; }, 10000);
        const int end = int(resp.indexOf("\r\n\r\n"));
        if (end < 0) return resp;
        body = resp.mid(end + 4);
        return resp.left(end + 4);
    }
};

} // namespace

class TstUnshare : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { mx::registerMetaTypes(); }
    void overlayStopsOfferingAtOnce();
    void ed2kAndKadStopOfferingAtOnce();
    void gnutellaStopsOfferingAtOnce();
    void bitTorrentStopsSeedingAtOnce();
};

// ------------------------------------------------------------------------------------ OctoMX network
void TstUnshare::overlayStopsOfferingAtOnce()
{
    Instance asker(QStringLiteral("UnshareAsker"));
    mx::SimpleSettings settings;
    settings.setValue(QString(mx::keys::identity::ScreenName), QStringLiteral("UnshareOwner"));
    settings.setValue(QStringLiteral("transfers/maxUploads"), 1);
    settings.setValue(QStringLiteral("queue/smallFileBypassKB"), 0);
    Lib L(QStringLiteral("unshare-ovl"), &settings);
    writeRandom(L.share + QStringLiteral("/Unshare Lantern.bin"), 4 * 1024 * 1024, 1);
    writeRandom(L.share + QStringLiteral("/Keep Lantern.bin"), 300 * 1024, 2);
    L.start();
    const mx::SharedFile gone = L.file(QStringLiteral("Unshare Lantern.bin")), kept = L.file(QStringLiteral("Keep Lantern.bin"));
    QVERIFY(gone.size > 0 && kept.size > 0);
    mx::PluginContext ctx;
    ctx.settings = &settings;
    ctx.library = L.lib.get();
    ctx.profileDir = L.profile;
    ctx.dataDir = L.profile + QStringLiteral("/overlay");
    ctx.screenName = QStringLiteral("UnshareOwner");
    ctx.clientName = QStringLiteral("OctoMX");
    ctx.clientVersion = QStringLiteral("5.4-test");
    auto *owner = static_cast<mx::ovl::OverlayPlugin *>(mx::createOverlayPlugin());
    mx::ovl::OverlayPlugin::TestOptions o;
    o.dht = o.mdns = o.reconnect = o.pex = false;
    owner->setTestOptions(o);
    owner->init(ctx);
    QHash<QString, mx::TransferInfo> rows;
    QStringList finished;
    connect(owner, &mx::NetworkPlugin::uploadChanged, this, [&](const mx::TransferInfo &t) { rows.insert(t.id, t); });
    connect(owner, &mx::NetworkPlugin::uploadFinished, this, [&](const QString &id) { finished << id; });
    owner->start();
    L.feed(owner);
    asker.start();
    {
        const int n = owner->connectedUsers();
        asker.plugin->connectPeer(QStringLiteral("127.0.0.1:%1").arg(owner->listenPort()));
        QVERIFY(waitFor([&] { return owner->connectedUsers() > n && asker.plugin->connectedUsers() > 0; }, 10000));
    }
    QTest::qWait(1500);   // the search mesh
    mx::SearchSource src;
    src.net = Net::Overlay;
    src.user = owner->localUser();
    src.extra.insert(QStringLiteral("node"), owner->nodeId());
    auto names = [&](quint32 id) {
        QSignalSpy hits(asker.plugin, &mx::NetworkPlugin::searchHits);
        QSignalSpy done(asker.plugin, &mx::NetworkPlugin::searchDone);
        mx::SearchQuery q;
        q.id = id;
        q.text = QStringLiteral("lantern");
        asker.plugin->search(q);
        waitFor([&] { return done.count() == 1; }, 15000);
        QStringList out;
        for (const auto &a : hits)
            for (const mx::SearchHit &h : a.at(1).value<QList<mx::SearchHit>>()) out << h.fileName;
        out.sort();
        out.removeDuplicates();
        return out;
    };
    auto browsed = [&](quint32 id) {
        QSignalSpy res(asker.plugin, &mx::NetworkPlugin::browseResults);
        asker.plugin->browse(id, owner->localUser());
        waitFor([&] { return !res.isEmpty() && res.last().at(3).toBool(); }, 15000);
        QStringList out;
        for (const auto &a : res)
            for (const mx::BrowseEntry &e : a.at(2).value<QList<mx::BrowseEntry>>()) out << QFileInfo(QString(e.path).replace(QLatin1Char('\\'), QLatin1Char('/'))).fileName();
        out.sort();
        return out;
    };
    auto open = [&](const mx::SharedFile &f, const QString &id) {
        mx::FileHashes h;
        h.sha1 = f.hashes.sha1;
        mx::RangeSource *s = asker.plugin->openSource(id, h, f.size, src);
        if (s) s->open();
        return s;
    };
    QCOMPARE(names(1), (QStringList{QStringLiteral("Keep Lantern.bin"), QStringLiteral("Unshare Lantern.bin")}));
    QCOMPARE(browsed(2), (QStringList{QStringLiteral("Keep Lantern.bin"), QStringLiteral("Unshare Lantern.bin")}));
    // a running upload of the file, and a request of it waiting in the queue (one slot)
    mx::RangeSource *running = open(gone, QStringLiteral("run"));
    QVERIFY(waitFor([&] { return running->state() == mx::RangeSource::Ready; }, 15000));
    running->request(0, 64 * 1024);
    QVERIFY(waitFor([&] {
        for (const mx::TransferInfo &t : rows) if (t.state == mx::TransferState::Transferring) return true;
        return false;
    }, 10000));
    Instance second(QStringLiteral("UnshareQueued"));
    second.start();
    {
        const int n = owner->connectedUsers();
        second.plugin->connectPeer(QStringLiteral("127.0.0.1:%1").arg(owner->listenPort()));
        QVERIFY(waitFor([&] { return owner->connectedUsers() > n; }, 10000));
    }
    mx::FileHashes gh;
    gh.sha1 = gone.hashes.sha1;
    mx::RangeSource *queued = second.plugin->openSource(QStringLiteral("q"), gh, gone.size, src);
    queued->open();
    QVERIFY(waitFor([&] { return queued->state() == mx::RangeSource::Queued; }, 15000));

    // the file leaves the shares by an exclusion rule
    const Stopwatch sw;
    settings.setValue(QString(mx::keys::library::ExcludePatterns), QStringList{QStringLiteral("unshare*")});
    QVERIFY(waitFor([&] { return running->state() == mx::RangeSource::Failed; }, 15000));
    qInfo("OctoMX network: the running upload stopped %lld ms after the rule was set", sw.ms());
    QVERIFY2(running->stateText().contains(QStringLiteral("not shared")), qPrintable(running->stateText()));
    QVERIFY(waitFor([&] { return queued->state() == mx::RangeSource::Failed; }, 15000));
    bool sawReason = false;
    for (const mx::TransferInfo &t : rows)
        if (t.statusText == QStringLiteral("Stopped: the file is no longer shared") && t.state == mx::TransferState::Error) sawReason = true;
    QVERIFY(sawReason);
    QCOMPARE(names(3), QStringList{QStringLiteral("Keep Lantern.bin")});
    QCOMPARE(browsed(4), QStringList{QStringLiteral("Keep Lantern.bin")});
    mx::RangeSource *again = open(gone, QStringLiteral("again"));
    QVERIFY(waitFor([&] { return again->state() == mx::RangeSource::Failed; }, 15000));
    QVERIFY2(again->stateText().contains(QStringLiteral("not shared")), qPrintable(again->stateText()));
    mx::RangeSource *keep = open(kept, QStringLiteral("keep"));
    QVERIFY(waitFor([&] { return keep->state() == mx::RangeSource::Ready; }, 15000));
    for (mx::RangeSource *s : {running, queued, again, keep}) drop(s);
    QTest::qWait(300);
    owner->stop();
    delete owner;
}

// ------------------------------------------------------------------------------------ eD2k + Kad
void TstUnshare::ed2kAndKadStopOfferingAtOnce()
{
    // a fake server: answers the login with a high ID, records what is offered
    nettest::Canary server;
    server.onData = [](QTcpSocket *s) {
        if (s->property("answered").toBool()) return;
        s->setProperty("answered", true);
        s->write(ed2kPacket(mx::ed2k::OP_IDCHANGE, QByteArray("\x0d\x0c\x0b\x0a\x00\x00\x00\x00", 8)));
    };
    QVERIFY(server.listen());
    mxtest::MemSettings settings;
    for (const auto &kv : {std::pair<const char *, QVariant>{"ed2k/tcpPort", 0}, {"ed2k/udpPort", 0}, {"ed2k/autoConnect", true},
                           {"ed2k/autoUpdateServerList", false}, {"ed2k/udpPing", false}, {"ed2k/udpSearch", false},
                           {"ed2k/udpSources", false}, {"ed2k/allowLan", true}, {"ed2k/obfuscation", false},
                           {"ed2k/uploadSlots", 1}, {"ed2k/staticServersOnly", true}, {"kad/allowLan", true}, {"kad/autoBootstrap", false},
                           {"kad/firewallChecks", false}})
        settings.setValue(QString::fromLatin1(kv.first), kv.second);
    Lib L(QStringLiteral("unshare-ed2k"), &settings);
    writeRandom(L.share + QStringLiteral("/Unshare Harbour.bin"), 3 * mx::ed2k::PARTSIZE / 2, 3);
    writeRandom(L.share + QStringLiteral("/Keep Harbour.bin"), 200 * 1024, 4);
    L.start();
    const mx::SharedFile gone = L.file(QStringLiteral("Unshare Harbour.bin")), kept = L.file(QStringLiteral("Keep Harbour.bin"));
    QVERIFY(gone.hashes.ed2k.size() == 16 && kept.hashes.ed2k.size() == 16);
    mx::CoreRateLimiter limiter(nullptr);
    limiter.setLimits(0, 150 * 1024);   // the running upload lasts
    limiter.setLimitLan(true);
    auto core = mx::ed2k::makeSharedCore();
    auto *ed = new mx::ed2k::Ed2kPlugin(core);
    auto *kad = new mx::ed2k::kad::KadPlugin(core);
    QHash<QString, mx::TransferInfo> rows;
    connect(ed, &mx::NetworkPlugin::uploadChanged, this, [&](const mx::TransferInfo &t) { rows.insert(t.id, t); });
    mx::PluginContext ctx = mxtest::makeContext(&settings, L.lib.get(), &limiter, L.profile);
    ed->init(ctx);
    mx::PluginContext kctx = ctx;
    kctx.dataDir = L.profile + QStringLiteral("/kad");
    kad->init(kctx);
    ed->addServer(QStringLiteral("127.0.0.1:%1").arg(server.port()), QStringLiteral("fake"));
    ed->start();
    kad->start();
    L.feed(ed);
    L.feed(kad);
    QVERIFY(kad->node());
    const int keywordsBefore = kad->node()->publishedKeywordCount();   // "unshare", "harbour", "keep"
    QCOMPARE(keywordsBefore, 3);
    // the server got the list with both files
    auto offers = [&] {
        QList<QByteArray> out;
        for (const auto &p : ed2kPackets(server.received))
            if (p.first == mx::ed2k::OP_OFFERFILES) out << p.second;
        return out;
    };
    QVERIFY(waitFor([&] { const auto o = offers(); return !o.isEmpty() && o.last().contains(gone.hashes.ed2k) && o.last().contains(kept.hashes.ed2k); }, 20000));
    const int offersBefore = int(offers().size());

    // a running upload of the file, and a second client queued for it (ed2k/uploadSlots 1)
    struct Down {
        mxtest::MemSettings s;
        mxtest::FileLibrary lib;
        QTemporaryDir dir;
        mx::ed2k::Ed2kPlugin *p = nullptr;
        Down()
        {
            for (const auto &kv : {std::pair<const char *, QVariant>{"ed2k/tcpPort", 0}, {"ed2k/udpPort", 0}, {"ed2k/autoConnect", false},
                                   {"ed2k/autoUpdateServerList", false}, {"ed2k/udpPing", false}, {"ed2k/allowLan", true},
                                   {"ed2k/obfuscation", false}})
                s.setValue(QString::fromLatin1(kv.first), kv.second);
            p = new mx::ed2k::Ed2kPlugin();
            p->init(mxtest::makeContext(&s, &lib, nullptr, dir.path()));
            p->start();
        }
        ~Down() { p->stop(); delete p; }
    } d1, d2;
    mx::SearchSource ss;
    ss.net = Net::ED2K;
    ss.uri = QStringLiteral("ed2k-peer://127.0.0.1:%1?uid=%2").arg(core->tcpPort()).arg(QString::fromLatin1(core->userHash().toHex()));
    mx::RangeSource *r1 = d1.p->openSource(QStringLiteral("u1"), gone.hashes, gone.size, ss);
    auto *dl1 = new mxtest::SimpleDownloader(d1.dir.path() + QStringLiteral("/out.bin"), gone.size, gone.hashes.ed2k, this);
    dl1->addSource(r1);
    QVERIFY(waitFor([&] { return dl1->receivedBytes() > 0; }, 20000));
    mx::RangeSource *r2 = d2.p->openSource(QStringLiteral("u2"), gone.hashes, gone.size, ss);
    auto *dl2 = new mxtest::SimpleDownloader(d2.dir.path() + QStringLiteral("/out.bin"), gone.size, gone.hashes.ed2k, this);
    dl2->addSource(r2);
    QVERIFY(waitFor([&] {
        for (const mx::TransferInfo &t : rows) if (t.state == mx::TransferState::Queued) return true;
        return false;
    }, 20000));

    // the file is removed from Shared Files
    const int offersMid = offersBefore;
    const mx::SharedFile keptNow = kept;
    const Stopwatch sw;
    L.lib->unshareFile(gone.path);   // Shared Files > remove from sharing
    // the running upload stops with the reason, the queued client is dropped, both are told "no such file"
    QVERIFY(waitFor([&] { return r1->state() == mx::RangeSource::Failed; }, 10000));
    qInfo("eD2k: the running upload stopped %lld ms after the file was removed", sw.ms());
    QVERIFY2(r1->stateText().contains(QStringLiteral("not shared")), qPrintable(r1->stateText()));
    QVERIFY(waitFor([&] { return r2->state() == mx::RangeSource::Failed; }, 10000));
    bool sawReason = false;
    for (const mx::TransferInfo &t : rows)
        if (t.state == mx::TransferState::Error && t.statusText == QStringLiteral("Stopped: the file is no longer shared")) sawReason = true;
    QVERIFY(sawReason);
    // the server is offered the list again, without it
    QVERIFY(waitFor([&] { return offers().size() > offersMid; }, 10000));
    QVERIFY(!offers().last().contains(gone.hashes.ed2k));
    QVERIFY(offers().last().contains(keptNow.hashes.ed2k));
    // Kad no longer publishes its keywords ("unshare"; "harbour" stays for the other file)
    QCOMPARE(kad->node()->publishedKeywordCount(), 2);
    // asked by hash again: no such file
    Down d3;
    mx::RangeSource *r3 = d3.p->openSource(QStringLiteral("u3"), gone.hashes, gone.size, ss);
    auto *dl3 = new mxtest::SimpleDownloader(d3.dir.path() + QStringLiteral("/out.bin"), gone.size, gone.hashes.ed2k, this);
    dl3->addSource(r3);
    QVERIFY(waitFor([&] { return r3->state() == mx::RangeSource::Failed; }, 15000));
    QVERIFY2(r3->stateText().contains(QStringLiteral("not shared")), qPrintable(r3->stateText()));
    for (mx::RangeSource *r : {r1, r2, r3}) {   // as the engine does in tst_ed2k_glue3
        r->close();
        delete r;
    }
    delete dl1;
    delete dl2;
    delete dl3;
    kad->stop();
    ed->stop();
    delete kad;
    delete ed;
}

// ------------------------------------------------------------------------------------ Gnutella
void TstUnshare::gnutellaStopsOfferingAtOnce()
{
    gnuttest::MemSettings settings;
    settings.setValue(QStringLiteral("gnutella/enabled"), true);
    settings.setValue(QStringLiteral("gnutella/port"), 0);
    settings.setValue(QStringLiteral("gnutella/bootstrapHosts"), QStringList{QStringLiteral("127.0.0.1:1")});
    settings.setValue(QStringLiteral("gnutella/autoBootstrap"), false);
    settings.setValue(QStringLiteral("gnutella/uploadSlots"), 1);
    settings.setValue(QStringLiteral("gnutella/uploadQueuePollSec"), 2);
    settings.setValue(QStringLiteral("connection/firewalled"), QStringLiteral("open"));
    Lib L(QStringLiteral("unshare-gnut"), &settings);
    QDir().mkpath(L.share + QStringLiteral("/sub"));
    writeRandom(L.share + QStringLiteral("/sub/Unshare Meadow.bin"), 6 * 1024 * 1024, 5);
    writeRandom(L.share + QStringLiteral("/Keep Meadow.bin"), 100 * 1024, 6);
    L.start();
    const mx::SharedFile gone = L.file(QStringLiteral("Unshare Meadow.bin")), kept = L.file(QStringLiteral("Keep Meadow.bin"));
    QVERIFY(gone.hashes.sha1.size() == 20 && kept.hashes.sha1.size() == 20);
    mx::CoreRateLimiter limiter(nullptr);
    limiter.setLimits(0, 200 * 1024);
    limiter.setLimitLan(true);
    auto *owner = new mx::gnut::GnutellaPlugin;
    QHash<QString, mx::TransferInfo> rows;
    QStringList finished;
    connect(owner, &mx::NetworkPlugin::uploadChanged, this, [&](const mx::TransferInfo &t) { rows.insert(t.id, t); });
    connect(owner, &mx::NetworkPlugin::uploadFinished, this, [&](const QString &id) { finished << id; });
    owner->init(gnuttest::makeContext(&settings, L.lib.get(), &limiter, L.profile + QStringLiteral("/gnutella")));
    owner->start();
    L.feed(owner);
    QVERIFY(waitFor([&] { return owner->listenPort() != 0; }, 5000));
    const quint16 port = owner->listenPort();
    // a second Gnutella instance browses it directly
    gnuttest::MemSettings bs;
    bs.setValue(QStringLiteral("gnutella/port"), 0);
    bs.setValue(QStringLiteral("gnutella/autoBootstrap"), false);
    bs.setValue(QStringLiteral("gnutella/bootstrapHosts"), QStringList{QStringLiteral("127.0.0.1:1")});
    gnuttest::FileLibrary blib;
    QTemporaryDir bdir;
    auto *browser = new mx::gnut::GnutellaPlugin;
    browser->init(gnuttest::makeContext(&bs, &blib, nullptr, bdir.path()));
    browser->start();
    auto browsed = [&](quint32 id) {
        QSignalSpy res(browser, &mx::NetworkPlugin::browseResults);
        QSignalSpy fail(browser, &mx::NetworkPlugin::browseFailed);
        mx::UserRef u;
        u.net = Net::Gnutella;
        u.id = mx::gnut::guidHex(owner->serventGuid());
        u.address = QStringLiteral("127.0.0.1:%1").arg(port);
        browser->browse(id, u);
        waitFor([&] { return !fail.isEmpty() || (!res.isEmpty() && res.last().at(3).toBool()); }, 15000);
        QStringList out;
        for (const auto &a : res)
            for (const mx::BrowseEntry &e : a.at(2).value<QList<mx::BrowseEntry>>()) out << QFileInfo(e.path).fileName();
        out.sort();
        return out;
    };
    QCOMPARE(browsed(1), (QStringList{QStringLiteral("Keep Meadow.bin"), QStringLiteral("Unshare Meadow.bin")}));
    // a running upload (the whole file, at 200 KB/s) and a client waiting in the queue
    GnutClient run, wait;
    QVERIFY(run.open(port));
    const QByteArray h1 = run.get(gone.hashes.sha1, 0, gone.size - 1);
    QVERIFY2(h1.startsWith("HTTP/1.1 2"), h1.left(100).constData());
    QVERIFY(wait.open(port));
    const QByteArray h2 = wait.get(gone.hashes.sha1, 0, 99);
    QVERIFY2(h2.startsWith("HTTP/1.1 503"), h2.left(100).constData());
    QVERIFY(waitFor([&] {
        int q = 0, t = 0;
        for (const mx::TransferInfo &r : rows) { q += r.state == mx::TransferState::Queued; t += r.state == mx::TransferState::Transferring; }
        return q >= 1 && t >= 1;
    }, 10000));

    // its folder stops being shared (the sub-folder is taken out of Shared Files)
    const Stopwatch sw;
    L.lib->unshareFile(L.share + QStringLiteral("/sub"));
    QVERIFY(waitFor([&] { return run.s.state() != QAbstractSocket::ConnectedState; }, 10000));
    qInfo("Gnutella: the running upload stopped %lld ms after the folder was unshared", sw.ms());
    int stopped = 0;
    for (const mx::TransferInfo &r : rows)
        if (r.state == mx::TransferState::Error && r.statusText == QStringLiteral("Stopped: the file is no longer shared")) ++stopped;
    QCOMPARE(stopped, 2);   // the running upload and the queue place
    QVERIFY(owner->queuedUploadIds().isEmpty());
    QCOMPARE(browsed(2), QStringList{QStringLiteral("Keep Meadow.bin")});
    GnutClient again;
    QVERIFY(again.open(port));
    const QByteArray h3 = again.get(gone.hashes.sha1, 0, 99);
    QVERIFY2(h3.startsWith("HTTP/1.1 404"), h3.left(100).constData());
    const QByteArray h4 = again.get(kept.hashes.sha1, 0, 99);
    QVERIFY2(h4.startsWith("HTTP/1.1 2") || h4.startsWith("HTTP/1.1 503"), h4.left(100).constData());
    browser->stop();
    delete browser;
    owner->stop();
    delete owner;
}

// ------------------------------------------------------------------------------------ BitTorrent
void TstUnshare::bitTorrentStopsSeedingAtOnce()
{
    const QString root = bttest::freshDir(QStringLiteral("unshare-bt"));
    // A seeds its library (bt/seedLibrary) and keeps seeding what it downloaded (bt/seedCompleted)
    bttest::Node a(root + QStringLiteral("/a"), true), b(root + QStringLiteral("/b")), c(root + QStringLiteral("/c"));
    a.settings.setValue(QString(mx::keys::bt::SeedLibrary), true);
    a.settings.setValue(QString(mx::keys::bt::SeedCompleted), true);
    a.create();
    b.create();
    c.create();
    QVERIFY(a.start() && b.start() && c.start());
    a.ctl->setRateLimits(0, 300 * 1024);   // the running upload lasts (127.0.0.1 counts: limitLan)
    a.settings.setValue(QString(mx::keys::library::SharedTypes), mx::sharedTypeIds());
    Lib L(QStringLiteral("unshare-btlib"), &a.settings);
    writeRandom(L.share + QStringLiteral("/Unshare Comet.bin"), 6 * 1024 * 1024, 7);
    writeRandom(L.share + QStringLiteral("/Keep Comet.bin"), 512 * 1024, 8);
    L.start();
    L.feed(a.plugin.get());
    QVERIFY(waitFor([&] { return a.seeded.size() == 2; }, 30000));
    QString goneId, keptId;
    for (auto it = a.seeded.cbegin(); it != a.seeded.cend(); ++it)
        (it.key().contains(QStringLiteral("Unshare")) ? goneId : keptId) = it.value();
    QVERIFY(!goneId.isEmpty() && !keptId.isEmpty());
    // C downloads the library file from A
    QString err;
    QCOMPARE(c.be->addTorrent(QStringLiteral("magnet:?xt=urn:btih:%1").arg(goneId), {}, root + QStringLiteral("/c/dl"), false, {}, &err), goneId);
    c.ctl->addPeer(goneId, a.endpoint());
    QVERIFY(waitFor([&] { return c.haveCount(goneId) > 0; }, 30000));
    QVERIFY(waitFor([&] {
        for (const mx::TransferInfo &t : a.uploads) if (t.state == mx::TransferState::Transferring) return true;
        return false;
    }, 10000));
    // excluded by a rule: the torrent goes from A at once, C's upload row says why, C gets nothing more
    const Stopwatch sw;
    a.settings.setValue(QString(mx::keys::library::ExcludePatterns), QStringList{QStringLiteral("unshare*")});
    QVERIFY(waitFor([&] { return !a.be->torrents().contains(goneId); }, 15000));
    qInfo("BitTorrent: the library seed stopped %lld ms after the rule was set", sw.ms());
    QVERIFY(a.be->torrents().contains(keptId));
    bool sawReason = false;
    for (const mx::TransferInfo &t : a.uploads)
        if (t.state == mx::TransferState::Error && t.statusText == QStringLiteral("Stopped: the file is no longer shared")) sawReason = true;
    QVERIFY(sawReason);
    QTest::qWait(1500);
    const int have = c.haveCount(goneId);
    QTest::qWait(3000);
    QCOMPARE(c.haveCount(goneId), have);
    QVERIFY(c.haveCount(goneId) < c.snap(goneId).numPieces);
    c.be->removeTorrent(goneId, true);

    // a finished download of A that is shared (it lies in A's shared folder): B seeds it to A first
    const QString src = root + QStringLiteral("/b/Done Comet.mp3");
    writeRandom(src, 2 * 1024 * 1024, 9);
    QVERIFY(b.be->seedLocalFile(src, &err));
    QVERIFY(waitFor([&] { return b.seeded.contains(src) || !b.seeded.isEmpty(); }, 30000));
    const QString doneId = b.seeded.value(src, b.seeded.isEmpty() ? QString() : b.seeded.cbegin().value());
    QCOMPARE(a.be->addTorrent(QStringLiteral("magnet:?xt=urn:btih:%1").arg(doneId), {}, L.share, false, {}, &err), doneId);
    a.ctl->addPeer(doneId, b.endpoint());
    QVERIFY(waitFor([&] { return a.finished.contains(doneId); }, 60000));
    L.lib->rescan();   // the engine asks the Library to hash a finished download; here a rescan finds it
    QVERIFY(waitFor([&] { return !L.file(QStringLiteral("Done Comet.mp3")).hashes.sha1.isEmpty(); }, 30000));
    QTest::qWait(3000);   // A's worker compares the torrent's files with the shared list
    QVERIFY(!a.snap(doneId).paused);
    // C fetches it from A only
    b.be->removeTorrent(doneId, false);
    QCOMPARE(c.be->addTorrent(QStringLiteral("magnet:?xt=urn:btih:%1").arg(doneId), {}, root + QStringLiteral("/c/dl2"), false, {}, &err), doneId);
    c.ctl->addPeer(doneId, a.endpoint());
    QVERIFY(waitFor([&] { return c.haveCount(doneId) > 0; }, 30000));
    c.be->removeTorrent(doneId, true);
    // its type is no longer shared (Audio unticked): A stops seeding it, says why, and keeps the torrent
    QStringList types = a.settings.value(QString(mx::keys::library::SharedTypes)).toStringList();
    QVERIFY2(types.contains(QStringLiteral("audio")), qPrintable(types.join(QLatin1Char(' '))));
    const QStringList allTypes = types;
    types.removeAll(QStringLiteral("audio"));
    a.settings.setValue(QString(mx::keys::library::SharedTypes), types);
    QVERIFY(waitFor([&] { return a.snap(doneId).paused; }, 15000));
    QCOMPARE(a.snap(doneId).statusText, QStringLiteral("Completed, not seeding: no longer shared"));
    QVERIFY(a.be->torrents().contains(doneId));
    QCOMPARE(c.be->addTorrent(QStringLiteral("magnet:?xt=urn:btih:%1").arg(doneId), {}, root + QStringLiteral("/c/dl3"), false, {}, &err), doneId);
    c.ctl->addPeer(doneId, a.endpoint());
    QTest::qWait(4000);
    QCOMPARE(c.haveCount(doneId), 0);
    // shared again: it seeds again
    a.settings.setValue(QString(mx::keys::library::SharedTypes), allTypes);
    QVERIFY(waitFor([&] { return !a.snap(doneId).paused; }, 30000));
    c.be->removeTorrent(doneId, true);   // a fresh one (libtorrent waits before it calls a peer that refused it again)
    QTest::qWait(500);
    QCOMPARE(c.be->addTorrent(QStringLiteral("magnet:?xt=urn:btih:%1").arg(doneId), {}, root + QStringLiteral("/c/dl4"), false, {}, &err), doneId);
    c.ctl->addPeer(doneId, a.endpoint());
    QVERIFY(waitFor([&] { return c.haveCount(doneId) > 0; }, 30000));
}

QTEST_GUILESS_MAIN(TstUnshare)
#include "tst_unshare.moc"
