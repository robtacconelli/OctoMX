// SPDX-License-Identifier: GPL-3.0-or-later
// [net-3] The BitTorrent settings that had only a reader proof, each changed through the plugin's
// Settings (as the program does) and observed on the wire or on disk, with local peers only
// (127.0.0.1, DHT / LSD / UPnP off, bttest::Node):
//   * a raw BitTorrent peer written here talks to the plugin's session and reads what it is told:
//     the extension handshake (bt/pex), the bitfield (bt/superSeeding), the unchokes (bt/maxUploadsPerTorrent);
//   * a second session downloads (bt/seedLibrary, bt/seedCompleted, the seed limits, bandwidth/limitLan);
//   * a local HTTP tracker and a local web seed count what reaches them (bt/extraTrackers, bt/webSeeds);
//   * the file on disk (bt/preallocate).
#include "bt_test_util.h"

#include "mx/settings_keys.h"

#include <libtorrent/create_torrent.hpp>

#include <QtCore/QElapsedTimer>
#include <QtCore/QTimer>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtTest/QtTest>

#include <memory>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

using namespace bttest;
namespace k = mx::keys;

namespace {

// A minimal BitTorrent peer: plain handshake (optionally announcing the extension protocol), then
// it collects every message the other side sends.
class RawPeer : public QObject {
public:
    struct Msg { int id; QByteArray payload; };
    RawPeer(const QString &infoHashHex, quint16 port, bool extensions)
    {
        const QByteArray ih = QByteArray::fromHex(infoHashHex.toLatin1());
        QByteArray hs;
        hs.append(char(19));
        hs.append("BitTorrent protocol");
        QByteArray reserved(8, '\0');
        if (extensions) reserved[5] = char(0x10);   // BEP 10
        hs.append(reserved);
        hs.append(ih);
        QByteArray pid = "-NT0001-";
        while (pid.size() < 20) pid.append(char('a' + QRandomGenerator::global()->bounded(26)));
        hs.append(pid);
        connect(&sock, &QTcpSocket::connected, this, [this, hs] { sock.write(hs); });
        connect(&sock, &QTcpSocket::readyRead, this, [this] {
            buf += sock.readAll();
            if (!gotHandshake) {
                if (buf.size() < 68) return;
                gotHandshake = true;
                buf.remove(0, 68);
            }
            while (buf.size() >= 4) {
                const quint32 len = (quint32(quint8(buf[0])) << 24) | (quint32(quint8(buf[1])) << 16) | (quint32(quint8(buf[2])) << 8) | quint8(buf[3]);
                if (quint32(buf.size()) < 4 + len) return;
                if (len > 0) msgs.append({int(quint8(buf[4])), buf.mid(5, int(len) - 1)});
                buf.remove(0, int(4 + len));
            }
        });
        sock.connectToHost(QHostAddress::LocalHost, port);
    }
    void send(int id, const QByteArray &payload = QByteArray())
    {
        const quint32 len = quint32(payload.size() + 1);
        QByteArray m;
        for (int i = 3; i >= 0; --i) m.append(char((len >> (8 * i)) & 0xFF));
        m.append(char(id));
        m.append(payload);
        sock.write(m);
    }
    bool has(int id) const
    {
        for (const Msg &m : msgs) if (m.id == id) return true;
        return false;
    }
    int count(int id) const
    {
        int n = 0;
        for (const Msg &m : msgs) n += m.id == id;
        return n;
    }
    QByteArray first(int id) const
    {
        for (const Msg &m : msgs) if (m.id == id) return m.payload;
        return QByteArray();
    }
    // BEP 10 handshake: we take part in peer exchange (ut_pex = our message id 1) and, when given, we
    // listen on `port` (so the other side may pass us on)
    void sendExtHandshake(int port = 0)
    {
        QByteArray d = "d1:md6:ut_pexi1ee";
        if (port > 0) d += "1:pi" + QByteArray::number(port) + "e";
        d += "e";
        send(20, QByteArray(1, '\0') + d);
    }
    // a peer exchange message from the other side (sent to our id 1)
    bool gotPex() const
    {
        for (const Msg &m : msgs)
            if (m.id == 20 && !m.payload.isEmpty() && m.payload.at(0) == char(1) && m.payload.contains("5:added")) return true;
        return false;
    }
    QTcpSocket sock;
    QByteArray buf;
    bool gotHandshake = false;
    QList<Msg> msgs;
};
enum { kChoke = 0, kUnchoke = 1, kInterested = 2, kHave = 4, kBitfield = 5, kExtended = 20 };

quint16 portOf(const Node &n) { return quint16(n.plugin->status().listenPort); }

// a seeder: a file seeded with seedLocalFile, ready to serve
QString seedFile(Node &a, const QString &path)
{
    QString err;
    if (!a.be->seedLocalFile(path, &err)) return QString();
    if (!QTest::qWaitFor([&] { return a.seeded.contains(path); }, 60000)) return QString();
    const QString id = a.seeded.value(path);
    if (!QTest::qWaitFor([&] { return a.snap(id).state == mx::TransferState::Seeding; }, 20000)) return QString();
    return id;
}

void setKey(Node &n, const QString &key, const QVariant &v)
{
    n.settings.setValue(key, v);
    QTest::qWait(700);   // the plugin re-reads bt/* and bandwidth/* 300 ms after a change
}

QString magnet(const QString &id) { return QStringLiteral("magnet:?xt=urn:btih:%1&dn=x").arg(id); }

// a plain HTTP server for one file (web seed): HEAD, GET, Range; counts the requests
class FileServer : public QTcpServer {
public:
    explicit FileServer(const QString &path) : m_path(path)
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *s = nextPendingConnection()) serve(s);
        });
    }
    int requests = 0;

private:
    struct Conn { QByteArray in; std::unique_ptr<QFile> f; qint64 left = 0; };
    void serve(QTcpSocket *s)
    {
        auto c = std::make_shared<Conn>();
        auto pump = [s, c] {
            while (c->left > 0 && s->bytesToWrite() < 1024 * 1024) {
                const QByteArray b = c->f->read(qMin<qint64>(c->left, 64 * 1024));
                if (b.isEmpty()) { c->left = 0; break; }
                c->left -= b.size();
                s->write(b);
            }
        };
        connect(s, &QTcpSocket::bytesWritten, s, pump);
        connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
        connect(s, &QTcpSocket::readyRead, s, [this, s, c, pump] {
            c->in += s->readAll();
            for (;;) {
                const int end = c->in.indexOf("\r\n\r\n");
                if (end < 0 || c->left > 0) return;
                const QList<QByteArray> lines = c->in.left(end).split('\n');
                c->in.remove(0, end + 4);
                ++requests;
                const bool head = lines.value(0).startsWith("HEAD");
                c->f = std::make_unique<QFile>(m_path);
                if (!c->f->open(QIODevice::ReadOnly)) { s->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n"); continue; }
                const qint64 size = c->f->size();
                qint64 from = 0, to = size - 1;
                bool range = false;
                for (const QByteArray &l : lines)
                    if (l.trimmed().toLower().startsWith("range: bytes=")) {
                        const QList<QByteArray> p = l.trimmed().mid(13).split('-');
                        from = p.value(0).toLongLong();
                        if (!p.value(1).isEmpty()) to = qMin(size - 1, p.value(1).toLongLong());
                        range = true;
                    }
                QByteArray h = range ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n";
                h += "Accept-Ranges: bytes\r\nContent-Type: application/octet-stream\r\nContent-Length: " + QByteArray::number(to - from + 1) + "\r\n";
                if (range) h += "Content-Range: bytes " + QByteArray::number(from) + '-' + QByteArray::number(to) + '/' + QByteArray::number(size) + "\r\n";
                s->write(h + "\r\n");
                if (head) continue;
                c->f->seek(from);
                c->left = to - from + 1;
                pump();
            }
        });
    }
    QString m_path;
};

// a .torrent of one file with a web seed
QByteArray torrentWithWebSeed(const QString &file, const QString &url)
{
    lt::file_storage fs;
    lt::add_files(fs, QDir::toNativeSeparators(file).toStdString());
    lt::create_torrent ct(fs, 256 * 1024, lt::create_torrent::v1_only);
    ct.add_url_seed(url.toStdString());
    lt::set_piece_hashes(ct, QDir::toNativeSeparators(QFileInfo(file).absolutePath()).toStdString());
    std::vector<char> out;
    lt::bencode(std::back_inserter(out), ct.generate());
    return QByteArray(out.data(), int(out.size()));
}

// b downloads `src` from `seeder` (which seeds it), then the seeder lets go: b is the only seed
QString downloadFrom(Node &seeder, Node &b, const QString &src, const QString &dir)
{
    const QString id = seedFile(seeder, src);
    if (id.isEmpty()) return QString();
    QString err;
    if (b.be->addTorrent(magnet(id), {}, dir, false, {}, &err) != id) return QString();
    b.ctl->addPeer(id, seeder.endpoint());
    if (!QTest::qWaitFor([&] { return b.finished.contains(id); }, 60000)) return QString();
    seeder.be->removeTorrent(id, false);
    return id;
}

} // namespace

class TstNetSettingsBt : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { mx::registerMetaTypes(); }
    void pexInTheExtensionHandshake();
    void superSeedingRevealsPiecesOneByOne();
    void uploadSlotsPerTorrent();
    void libraryFilesSeededAsTorrents();
    void completedTorrentsSeedOrStop();
    void seedRatioAndActionStopTheSeed();
    void seedTimeLimitStopsTheSeed();
    void extraTrackersAnnouncedTo();
    void webSeedsUsedOrNot();
    void preallocateReservesTheFile();
    void lanPeersLimitedOrNot();
};

// bt/pex: a peer that takes part in peer exchange is told the other peers of the torrent (a ut_pex
// message) only with the key on; a change applies at once
void TstNetSettingsBt::pexInTheExtensionHandshake()
{
    const QString root = freshDir(QStringLiteral("ns-pex"));
    const QString src = root + QStringLiteral("/share/pex.bin");
    makeFile(src, 2 * 1024 * 1024, 11);
    Node a(root + QStringLiteral("/a"));
    a.create();
    a.settings.setValue(QString(k::bt::Encryption), QStringLiteral("disabled"));   // the raw peers speak plain
    a.settings.setValue(QString(k::bt::Pex), false);
    QVERIFY(a.start());
    const QString id = seedFile(a, src);
    QVERIFY(!id.isEmpty());
    QTcpServer listener;   // the other peer says it listens here, so it may be passed on
    QVERIFY(listener.listen(QHostAddress::LocalHost, 0));
    RawPeer other(id, portOf(a), true);
    QVERIFY(QTest::qWaitFor([&] { return other.gotHandshake; }, 10000));
    other.sendExtHandshake(listener.serverPort());
    auto pexTo = [&](int waitMs) {
        RawPeer p(id, portOf(a), true);
        if (!QTest::qWaitFor([&] { return p.gotHandshake; }, 10000)) return false;
        p.sendExtHandshake();
        return QTest::qWaitFor([&] { return p.gotPex(); }, waitMs);
    };
    QVERIFY2(!pexTo(6000), "peer exchange although bt/pex is off");
    setKey(a, QString(k::bt::Pex), true);
    QVERIFY2(pexTo(15000), "no peer exchange with bt/pex on");
    setKey(a, QStringLiteral("bt/pex"), false);
    QVERIFY(!pexTo(6000));
}

// bt/superSeeding: a new torrent of a super seed shows its pieces one at a time (no full bitfield,
// single "have" messages); without it the seed sends the full bitfield at once
void TstNetSettingsBt::superSeedingRevealsPiecesOneByOne()
{
    const QString root = freshDir(QStringLiteral("ns-super"));
    const QString f1 = root + QStringLiteral("/share/plain.bin"), f2 = root + QStringLiteral("/share/super.bin");
    makeFile(f1, 4 * 1024 * 1024, 12);
    makeFile(f2, 4 * 1024 * 1024, 13);
    Node a(root + QStringLiteral("/a"));
    a.create();
    a.settings.setValue(QString(k::bt::Encryption), QStringLiteral("disabled"));
    QVERIFY(a.start());
    auto fullBitfield = [](const QByteArray &bf, int pieces) {
        if (pieces <= 0 || bf.size() * 8 < pieces) return false;
        for (int i = 0; i < pieces; ++i)
            if (!(quint8(bf.at(i / 8)) & (0x80 >> (i % 8)))) return false;
        return true;
    };
    // off: the full bitfield
    const QString plainId = seedFile(a, f1);
    QVERIFY(!plainId.isEmpty());
    {
        RawPeer p(plainId, portOf(a), false);
        QVERIFY(QTest::qWaitFor([&] { return p.has(kBitfield); }, 10000));
        QVERIFY(fullBitfield(p.first(kBitfield), a.snap(plainId).numPieces));
    }
    // on: the next torrent is a super seed
    setKey(a, QStringLiteral("bt/superSeeding"), true);
    const QString superId = seedFile(a, f2);
    QVERIFY(!superId.isEmpty());
    {
        RawPeer p(superId, portOf(a), false);
        QVERIFY(QTest::qWaitFor([&] { return p.gotHandshake; }, 10000));
        // a peer that stays uninterested is offered one more piece every 10 s (libtorrent)
        QVERIFY2(QTest::qWaitFor([&] { return p.has(kHave); }, 25000), "a super seed tells single pieces");
        const int pieces = a.snap(superId).numPieces;
        QVERIFY(pieces > 4);
        QVERIFY(!(p.has(kBitfield) && fullBitfield(p.first(kBitfield), pieces)));
        QVERIFY2(p.count(kHave) < pieces, qPrintable(QString::number(p.count(kHave))));
    }
}

// bt/maxUploadsPerTorrent: of five interested peers of one torrent only that many are unchoked
// (plus at most one optimistic unchoke); a change applies to the running torrent
void TstNetSettingsBt::uploadSlotsPerTorrent()
{
    const QString root = freshDir(QStringLiteral("ns-slots"));
    const QString src = root + QStringLiteral("/share/slots.bin");
    makeFile(src, 4 * 1024 * 1024, 14);
    // [net-3] bandwidth/limitLan off (the default): the slots hold for peers of this computer / the LAN
    // too (libtorrent's local class ignored them before: every LAN peer was unchoked)
    Node a(root + QStringLiteral("/a"), /*limitLan*/ false);
    a.create();
    a.settings.setValue(QString(k::bt::Encryption), QStringLiteral("disabled"));
    a.settings.setValue(QString(k::bt::MaxUploadsPerTorrent), 1);
    QVERIFY(a.start());
    const QString id = seedFile(a, src);
    QVERIFY(!id.isEmpty());
    auto unchokedOf = [&](int n) {
        std::vector<std::unique_ptr<RawPeer>> peers;
        for (int i = 0; i < n; ++i) peers.push_back(std::make_unique<RawPeer>(id, portOf(a), false));
        if (!QTest::qWaitFor([&] {
                for (auto &p : peers) if (!p->gotHandshake) return false;
                return true;
            }, 10000))
            return -1;
        for (auto &p : peers) p->send(kInterested);
        QTest::qWait(6000);
        int unchoked = 0;
        for (auto &p : peers) unchoked += p->has(kUnchoke);
        return unchoked;
    };
    const int one = unchokedOf(5);
    QVERIFY2(one >= 1 && one <= 2, qPrintable(QString::number(one)));
    setKey(a, QStringLiteral("bt/maxUploadsPerTorrent"), 4);
    QTest::qWait(1500);   // the five above are gone
    const int four = unchokedOf(5);
    QVERIFY2(four >= 4, qPrintable(QString::number(four)));
    qInfo("unchoked of 5 interested peers: %d with 1 slot per torrent, %d with 4", one, four);
}

// bt/seedLibrary: on, a shared library file becomes a torrent another session downloads; off (at
// run time), the library seed goes at once and nobody gets the file from us any more
void TstNetSettingsBt::libraryFilesSeededAsTorrents()
{
    const QString root = freshDir(QStringLiteral("ns-lib"));
    const QString src = root + QStringLiteral("/share/library file.bin");
    makeFile(src, 3 * 1024 * 1024, 15);
    mx::SharedFile f;
    f.path = src;
    f.name = QFileInfo(src).fileName();
    f.size = QFileInfo(src).size();
    f.hashed = true;
    {   // off (the default): a shared file is no torrent
        Node a(root + QStringLiteral("/off"));
        a.create();
        a.settings.setValue(QString(k::bt::SeedLibrary), false);
        QVERIFY(a.start());
        a.plugin->setSharedFiles({f});
        QTest::qWait(2500);
        QVERIFY(a.seeded.isEmpty());
        QVERIFY(a.be->torrents().isEmpty());
    }
    Node a(root + QStringLiteral("/a")), b(root + QStringLiteral("/b"));
    a.create();
    b.create();
    a.settings.setValue(QString(k::bt::SeedLibrary), true);
    QVERIFY(a.start());
    QVERIFY(b.start());
    a.plugin->setSharedFiles({f});
    QVERIFY(QTest::qWaitFor([&] { return a.seeded.contains(src); }, 60000));
    const QString id = a.seeded.value(src);
    QVERIFY(QTest::qWaitFor([&] { return a.snap(id).state == mx::TransferState::Seeding; }, 20000));
    QString err;
    const QString dl = root + QStringLiteral("/b/dl");
    QCOMPARE(b.be->addTorrent(magnet(id), {}, dl, false, {}, &err), id);
    b.ctl->addPeer(id, a.endpoint());
    QVERIFY(QTest::qWaitFor([&] { return b.finished.contains(id); }, 60000));
    QCOMPARE(sha1File(dl + QStringLiteral("/library file.bin")), sha1File(src));
    b.be->removeTorrent(id, true);
    // off at run time: the seed is removed
    a.settings.setValue(QStringLiteral("bt/seedLibrary"), false);
    QVERIFY(QTest::qWaitFor([&] { return !a.be->torrents().contains(id); }, 5000));
    // a third session finds nobody serving it
    Node c(root + QStringLiteral("/c"));
    c.create();
    QVERIFY(c.start());
    QCOMPARE(c.be->addTorrent(magnet(id), {}, root + QStringLiteral("/c/dl"), false, {}, &err), id);
    c.ctl->addPeer(id, a.endpoint());
    QTest::qWait(4000);
    QVERIFY(c.metadata.isEmpty());
    QCOMPARE(c.haveCount(id), 0);
}

// bt/seedCompleted: on, a finished download keeps seeding (a third session gets the file from it);
// off, the torrent stops when complete and gives nothing
void TstNetSettingsBt::completedTorrentsSeedOrStop()
{
    const QString root = freshDir(QStringLiteral("ns-seedc"));
    for (const bool seed : {true, false}) {
        const QString tag = seed ? QStringLiteral("on") : QStringLiteral("off");
        const QString src = root + QStringLiteral("/share/completed-%1.bin").arg(tag);
        makeFile(src, 3 * 1024 * 1024, seed ? 16 : 17);
        Node a(root + QStringLiteral("/a") + tag), b(root + QStringLiteral("/b") + tag), c(root + QStringLiteral("/c") + tag);
        a.create();
        b.create();
        c.create();
        b.settings.setValue(QString(k::bt::SeedCompleted), seed);
        QVERIFY(a.start() && b.start() && c.start());
        const QString id = downloadFrom(a, b, src, root + QStringLiteral("/b") + tag + QStringLiteral("/dl"));
        QVERIFY(!id.isEmpty());
        if (!seed) QVERIFY(QTest::qWaitFor([&] { return b.snap(id).paused; }, 5000));
        else QVERIFY(QTest::qWaitFor([&] { return b.snap(id).state == mx::TransferState::Seeding; }, 10000));
        QString err;
        QCOMPARE(c.be->addTorrent(magnet(id), {}, root + QStringLiteral("/c") + tag + QStringLiteral("/dl"), false, {}, &err), id);
        c.ctl->addPeer(id, b.endpoint());
        const bool got = QTest::qWaitFor([&] { return c.finished.contains(id); }, seed ? 60000 : 5000);
        QCOMPARE(got, seed);
        if (!seed) QCOMPARE(c.haveCount(id), 0);
    }
}

// bt/seedRatioLimit with bt/seedLimitAction: a finished download stops seeding once it has sent
// that share of its size; "pause" keeps it in the list (paused), "remove" takes it off (the files
// stay); 0 seeds without a limit
void TstNetSettingsBt::seedRatioAndActionStopTheSeed()
{
    const QString root = freshDir(QStringLiteral("ns-ratio"));
    struct Case { double ratio; const char *action; quint32 seed; };
    for (const Case &cs : {Case{0.3, "pause", 21}, Case{0.3, "remove", 22}, Case{0.0, "pause", 23}}) {
        const QString tag = QStringLiteral("%1-%2").arg(int(cs.ratio * 10)).arg(QLatin1String(cs.action));
        const QString name = QStringLiteral("ratio-%1.bin").arg(tag);
        const QString src = root + QStringLiteral("/share/") + name;
        makeFile(src, 6 * 1024 * 1024, cs.seed);
        Node a(root + QStringLiteral("/a") + tag), b(root + QStringLiteral("/b") + tag, /*limitLan*/ true), c(root + QStringLiteral("/c") + tag);
        a.create();
        b.create();
        c.create();
        QVERIFY(a.start() && b.start() && c.start());
        const QString bdir = root + QStringLiteral("/b") + tag + QStringLiteral("/dl");
        const QString id = downloadFrom(a, b, src, bdir);
        QVERIFY(!id.isEmpty());
        // the limit is set now (Node::create sets 0): it applies to the seeding torrent at once
        b.settings.setValue(QString(k::bt::SeedLimitAction), QString::fromLatin1(cs.action));
        b.settings.setValue(QString(k::bt::SeedRatioLimit), cs.ratio);
        QTest::qWait(700);
        b.ctl->setRateLimits(0, 1024 * 1024);   // a few seconds of seeding (limitLan: 127.0.0.1 is limited too)
        QString err;
        QCOMPARE(c.be->addTorrent(magnet(id), {}, root + QStringLiteral("/c") + tag + QStringLiteral("/dl"), false, {}, &err), id);
        c.ctl->addPeer(id, b.endpoint());
        if (cs.ratio <= 0) {
            QVERIFY(QTest::qWaitFor([&] { return c.finished.contains(id); }, 60000));
            QVERIFY(!b.snap(id).paused);
            continue;
        }
        if (QLatin1String(cs.action) == QLatin1String("pause")) {
            QVERIFY(QTest::qWaitFor([&] { return b.snap(id).paused; }, 30000));
            QVERIFY(b.be->torrents().contains(id));
        } else {
            QVERIFY(QTest::qWaitFor([&] { return !b.be->torrents().contains(id); }, 30000));
            QVERIFY(QFileInfo::exists(bdir + QLatin1Char('/') + name));   // the files stay
        }
        QTest::qWait(2000);
        const int have = c.haveCount(id);
        const int total = c.snap(id).numPieces;
        QVERIFY2(!c.finished.contains(id) && have < total, qPrintable(QStringLiteral("%1 of %2").arg(have).arg(total)));
        qInfo("ratio %.1f, %s: the third session got %d of %d pieces", cs.ratio, cs.action, have, total);
    }
}

// bt/seedTimeLimitMin: a finished download stops seeding after that many minutes of seeding (a
// third session that downloads from it slowly meanwhile gets no more from it)
void TstNetSettingsBt::seedTimeLimitStopsTheSeed()
{
    const QString root = freshDir(QStringLiteral("ns-time"));
    const QString src = root + QStringLiteral("/share/time.bin");
    makeFile(src, 12 * 1024 * 1024, 30);
    Node a(root + QStringLiteral("/a")), b(root + QStringLiteral("/b"), /*limitLan*/ true), c(root + QStringLiteral("/c"));
    a.create();
    b.create();
    c.create();
    QVERIFY(a.start() && b.start() && c.start());
    const QString id = downloadFrom(a, b, src, root + QStringLiteral("/b/dl"));
    QVERIFY(!id.isEmpty());
    QElapsedTimer t;
    t.start();
    b.settings.setValue(QStringLiteral("bt/seedTimeLimitMin"), 1);
    b.ctl->setRateLimits(0, 100 * 1024);   // 12 MB take two minutes
    QString err;
    QCOMPARE(c.be->addTorrent(magnet(id), {}, root + QStringLiteral("/c/dl"), false, {}, &err), id);
    c.ctl->addPeer(id, b.endpoint());
    QVERIFY(QTest::qWaitFor([&] { return c.haveCount(id) > 0; }, 20000));
    QTest::qWait(40000);
    QVERIFY(!b.snap(id).paused);   // not yet: one minute
    QVERIFY2(QTest::qWaitFor([&] { return b.snap(id).paused; }, 45000), qPrintable(b.snap(id).statusText));
    qInfo("paused %lld s after the limit of 1 minute was set", t.elapsed() / 1000);
    QTest::qWait(1500);
    const int have = c.haveCount(id);
    QTest::qWait(3000);
    QCOMPARE(c.haveCount(id), have);   // nothing more from it
    QVERIFY(!c.finished.contains(id));
}

// bt/extraTrackers: added to every magnet link: a local HTTP tracker gets the announce of a magnet
// that names no tracker; without it nobody is asked
void TstNetSettingsBt::extraTrackersAnnouncedTo()
{
    QTcpServer tracker;
    QVERIFY(tracker.listen(QHostAddress::LocalHost, 0));
    QList<QByteArray> got;
    connect(&tracker, &QTcpServer::newConnection, &tracker, [&] {
        while (QTcpSocket *s = tracker.nextPendingConnection()) {
            connect(s, &QTcpSocket::readyRead, s, [s, &got] {
                const QByteArray r = s->readAll();
                got << r.left(r.indexOf('\n'));
                s->write("HTTP/1.1 200 OK\r\nContent-Length: 26\r\nConnection: close\r\n\r\nd8:intervali1800e5:peers0:e");
                s->disconnectFromHost();
            });
            connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
        }
    });
    const QString root = freshDir(QStringLiteral("ns-trackers"));
    const QString ih1 = QStringLiteral("0123456789abcdef0123456789abcdef01234567");
    const QString ih2 = QStringLiteral("89abcdef0123456789abcdef0123456789abcdef");
    {
        Node a(root + QStringLiteral("/none"));
        a.create();   // bt/extraTrackers empty
        QVERIFY(a.start());
        QString err;
        QCOMPARE(a.be->addTorrent(magnet(ih1), {}, root + QStringLiteral("/none/dl"), false, {}, &err), ih1);
        QTest::qWait(3000);
        QVERIFY(got.isEmpty());
    }
    Node a(root + QStringLiteral("/a"));
    a.create();
    a.settings.setValue(QString(k::bt::ExtraTrackers), QStringList{QStringLiteral("http://127.0.0.1:%1/announce").arg(tracker.serverPort())});
    QVERIFY(a.start());
    QString err;
    QCOMPARE(a.be->addTorrent(magnet(ih2), {}, root + QStringLiteral("/a/dl"), false, {}, &err), ih2);
    QVERIFY(QTest::qWaitFor([&] { return !got.isEmpty(); }, 15000));
    QVERIFY2(got.first().startsWith("GET /announce?"), got.first().constData());
    QVERIFY(got.first().contains("info_hash="));
}

// bt/webSeeds: on, a torrent's web seed (a local HTTP server) is asked and the download completes
// from it alone; off, the web seed is never asked
void TstNetSettingsBt::webSeedsUsedOrNot()
{
    const QString root = freshDir(QStringLiteral("ns-webseed"));
    const QString src = root + QStringLiteral("/www/webseed.bin");
    makeFile(src, 2 * 1024 * 1024, 31);
    FileServer www(src);
    QVERIFY(www.listen(QHostAddress::LocalHost, 0));
    const QByteArray tor = torrentWithWebSeed(src, QStringLiteral("http://127.0.0.1:%1/webseed.bin").arg(www.serverPort()));
    const QString id = infoHashOf(tor);
    for (const bool on : {false, true}) {
        const QString tag = on ? QStringLiteral("on") : QStringLiteral("off");
        Node b(root + QStringLiteral("/b") + tag);
        b.create();
        b.settings.setValue(QString(k::bt::WebSeeds), on);
        QVERIFY(b.start());
        const int before = www.requests;
        QString err;
        const QString dl = root + QStringLiteral("/b") + tag + QStringLiteral("/dl");
        QCOMPARE(b.be->addTorrent(QString(), tor, dl, false, {}, &err), id);
        if (on) {
            QVERIFY(QTest::qWaitFor([&] { return b.finished.contains(id); }, 30000));
            QVERIFY(www.requests > before);
            QCOMPARE(sha1File(dl + QStringLiteral("/webseed.bin")), sha1File(src));
        } else {
            QTest::qWait(5000);
            // [net-3] the web seed is never asked (the torrent is added without it)
            QCOMPARE(www.requests, before);
            QCOMPARE(b.haveCount(id), 0);
        }
    }
}

// bt/preallocate: on, the whole file is reserved on disk (allocated blocks) once the torrent's first
// data is written; off, the file is sparse: only what arrived takes space
void TstNetSettingsBt::preallocateReservesTheFile()
{
#ifndef Q_OS_UNIX
    QSKIP("allocated disk blocks are read with stat() (Unix)");
#else
    const QString root = freshDir(QStringLiteral("ns-prealloc"));
    const QString src = root + QStringLiteral("/share/prealloc.bin");
    const qint64 size = 16 * 1024 * 1024;
    makeFile(src, size, 32);
    Node a(root + QStringLiteral("/a"), /*limitLan*/ true);
    a.create();
    QVERIFY(a.start());
    const QString id = seedFile(a, src);
    QVERIFY(!id.isEmpty());
    a.ctl->setRateLimits(0, 256 * 1024);
    for (const bool on : {true, false}) {
        const QString tag = on ? QStringLiteral("on") : QStringLiteral("off");
        Node b(root + QStringLiteral("/b") + tag);
        b.create();
        b.settings.setValue(QString(k::bt::Preallocate), on);
        QVERIFY(b.start());
        const QString dl = root + QStringLiteral("/b") + tag + QStringLiteral("/dl");
        QString err;
        QCOMPARE(b.be->addTorrent(magnet(id), {}, dl, false, {}, &err), id);
        b.ctl->addPeer(id, a.endpoint());
        QVERIFY(QTest::qWaitFor([&] { return b.haveCount(id) > 0; }, 20000));
        const QByteArray file = QFile::encodeName(dl + QStringLiteral("/prealloc.bin"));
        struct stat st {};
        QVERIFY(QTest::qWaitFor([&] { return ::stat(file.constData(), &st) == 0; }, 5000));
        const qint64 allocated = qint64(st.st_blocks) * 512;
        qInfo("preallocate %s: %lld of %lld bytes on disk after %d pieces", on ? "on" : "off", allocated, size, b.haveCount(id));
        QVERIFY(!b.finished.contains(id));
        if (on) QVERIFY2(allocated >= size, qPrintable(QString::number(allocated)));
        else QVERIFY2(allocated < size / 2, qPrintable(QString::number(allocated)));
        b.be->removeTorrent(id, true);
    }
#endif
}

// bandwidth/limitLan: the upload limit holds for a peer on this computer (127.0.0.1) only with the
// key on; off, the same transfer runs at full speed. A change applies to the next connections
void TstNetSettingsBt::lanPeersLimitedOrNot()
{
    const QString root = freshDir(QStringLiteral("ns-lan"));
    const QString src = root + QStringLiteral("/share/lan.bin");
    const qint64 size = 24 * 1024 * 1024;
    makeFile(src, size, 33);
    const int limit = 200 * 1024;
    Node a(root + QStringLiteral("/a"), /*limitLan*/ true);
    a.create();
    QVERIFY(a.start());
    const QString id = seedFile(a, src);
    QVERIFY(!id.isEmpty());
    a.ctl->setRateLimits(0, limit);
    // on: a 127.0.0.1 peer gets the limit
    Node b(root + QStringLiteral("/b"));
    b.create();
    QVERIFY(b.start());
    QString err;
    QCOMPARE(b.be->addTorrent(magnet(id), {}, root + QStringLiteral("/b/dl"), false, {}, &err), id);
    b.ctl->addPeer(id, a.endpoint());
    QVERIFY(QTest::qWaitFor([&] { return b.snap(id).totalDone > 0; }, 20000));
    QTest::qWait(1000);
    const qint64 d0 = b.snap(id).totalDone;
    QElapsedTimer t;
    t.start();
    QTest::qWait(5000);
    const double limited = double(b.snap(id).totalDone - d0) * 1000.0 / double(t.elapsed());
    QVERIFY2(limited < limit * 1.6, qPrintable(QString::number(limited)));
    QVERIFY(!b.finished.contains(id));
    b.be->removeTorrent(id, true);
    // off: the next connection of a 127.0.0.1 peer is not limited (24 MB would take two minutes)
    a.settings.setValue(QString(k::bandwidth::LimitLan), false);
    QTest::qWait(1000);
    Node c(root + QStringLiteral("/c"));
    c.create();
    QVERIFY(c.start());
    QElapsedTimer t2;
    t2.start();
    QCOMPARE(c.be->addTorrent(magnet(id), {}, root + QStringLiteral("/c/dl"), false, {}, &err), id);
    c.ctl->addPeer(id, a.endpoint());
    QVERIFY2(QTest::qWaitFor([&] { return c.finished.contains(id); }, 30000), qPrintable(QString::number(c.snap(id).totalDone)));
    qInfo("127.0.0.1 peer: %.0f KB/s with limitLan on (limit %d KB/s); with it off all %lld MB in %lld ms",
          limited / 1024, limit / 1024, size / (1024 * 1024), t2.elapsed());
}

QTEST_GUILESS_MAIN(TstNetSettingsBt)
#include "tst_netsettings_bt.moc"
