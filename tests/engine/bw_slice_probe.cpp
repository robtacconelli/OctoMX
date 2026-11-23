// SPDX-License-Identifier: GPL-3.0-or-later
// bw_slice_probe [bw-2]: how evenly the shared speed limit holds WITHIN a second, on 127.0.0.1.
// Two processes, so the measuring side is outside the program under test (works for the Windows
// build under wine too):
//
//   bw_slice_probe serve --dir D [--mb N] [--pull] [--peers N] [--rtt MS]
//       The "internet": an unlimited BitTorrent seeder, an HTTP server with Range support and an
//       eD2k client sharing one test file (D/data/octomx-bw2-test.bin, random bytes), described in
//       D/serve.json. With --pull it instead downloads from the program under test (its client.json:
//       BitTorrent leecher + eD2k downloader), for upload runs. Every 50 ms it writes the TCP byte
//       counters of its own sockets (Linux sock_diag: bytes_acked = what the other side received,
//       bytes_received = what it sent us), summed per network by port, to D/os.csv. --peers N: N
//       seeders; --rtt MS: a relay in front of each seeder and the HTTP server adds MS ms of round
//       trip (the meter then counts the relays' sockets towards the program).
//   bw_slice_probe client --profile P --serve D/serve.json --nets bt,http,ed2k --down KB --up KB
//                  [--mode down|up] [--seconds S] [--join NET:S ...] [--leave NET:S ...]
//                  [--at S:DOWN:UP ...]   (limits changed S s after the start, KB/s)
//       The program under test: the REAL core with the BitTorrent, eD2k and HTTP plugins of this
//       build. down: one download of the test file fed by the chosen networks (torrent + peer,
//       HTTP mirror, eD2k peer). up: seeds the file over BitTorrent and shares it on eD2k, writes
//       P/client.json for `serve --pull`. LAN peers are limited (bandwidth/limitLan). Every 50 ms it
//       writes the limiter's totals per network (what the program counts) to P/prog.csv, and every
//       sample of the Bandwidth service (what the graph draws) to P/graph.csv. The download and its
//       data are removed at the end. --join NET:S starts that network's source S s after the
//       start, --leave NET:S closes it (fairness when a network joins and leaves).
//
// Analysis: tests/engine/bw_slices.py (rates of 100 ms, 250 ms and 1 s windows as a share of the limit).
// Not a ctest; no peer of the real networks is contacted (DHT, LSD, Kad, servers are off).
#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRandomGenerator>
#include <QtCore/QTextStream>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtTest/QTest>

#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "core/rate_limiter.h"
#include "core/real_core.h"
#include "mx/plugin.h"
#include "mx/services.h"
#include "mx/settings_keys.h"
#include "mx/simple_settings.h"

#include "bt_plugin.h"
#include "ed2k_core.h"
#include "ed2k_plugin.h"
#include "ed2k_plugin_impl.h"
#include "http_plugin.h"
#include "test_support.h"

#ifdef Q_OS_LINUX
#include <linux/inet_diag.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/sock_diag.h>
#include <linux/tcp.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace mx;

namespace {

qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

QJsonObject readJson(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

void writeJson(const QString &path, const QJsonObject &o)
{
    QFile f(path + QStringLiteral(".tmp"));
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write(QJsonDocument(o).toJson());
    f.close();
    QFile::remove(path);
    QFile::rename(path + QStringLiteral(".tmp"), path);
}

// ---------------------------------------------------------------- OS counters (serve side)
// TCP sockets on this machine that use one of the ports in portNet (summed per network):
//   own sockets (inode in /proc/self/fd): bytes_acked (= what the other side received) and
//   bytes_received (= what it sent us);
//   the other side's sockets (the program under test, also under wine): bytes_received minus what
//   still waits unread in the kernel (= what the program really read), bytes_acked plus its send
//   queue (= what the program really wrote).
// A closed socket keeps what it had.
struct OsMeter {
    struct C { qint64 srvSent = 0, srvRecv = 0, cliRead = 0, cliWritten = 0; };
    QHash<int, QString> portNet;   // port of this side (or of the program, for --pull) -> network
    QHash<quint64, C> last;        // socket -> counters
    QHash<quint64, QString> sockNet;
    QHash<QString, C> gone;
    static void add(C &a, const C &b) { a.srvSent += b.srvSent; a.srvRecv += b.srvRecv; a.cliRead += b.cliRead; a.cliWritten += b.cliWritten; }

    QHash<QString, C> poll()
    {
        QHash<QString, C> sum;
#ifdef Q_OS_LINUX
        QSet<quint32> mine;
        for (const QFileInfo &fi : QDir(QStringLiteral("/proc/self/fd")).entryInfoList(QDir::System | QDir::NoDotAndDotDot)) {
            const QString t = QFile::symLinkTarget(fi.filePath());
            const int a = t.indexOf(QLatin1String("socket:["));
            if (a >= 0) mine.insert(t.mid(a + 8, t.size() - a - 9).toUInt());
        }
        const int fd = ::socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_SOCK_DIAG);
        if (fd < 0) return sum;
        struct {
            nlmsghdr nlh;
            inet_diag_req_v2 r;
        } req{};
        req.nlh.nlmsg_len = sizeof(req);
        req.nlh.nlmsg_type = SOCK_DIAG_BY_FAMILY;
        req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
        req.r.sdiag_family = AF_INET;
        req.r.sdiag_protocol = IPPROTO_TCP;
        req.r.idiag_states = ~0u & ~(1u << 10);   // not LISTEN
        req.r.idiag_ext = 1 << (INET_DIAG_INFO - 1);
        if (::send(fd, &req, sizeof(req), 0) < 0) { ::close(fd); return sum; }
        QHash<quint64, C> now;
        static char buf[1 << 16];
        bool done = false;
        while (!done) {
            ssize_t got = ::recv(fd, buf, sizeof(buf), 0);
            if (got <= 0) break;
            int n = int(got);
            for (auto *h = reinterpret_cast<nlmsghdr *>(buf); NLMSG_OK(h, n); h = NLMSG_NEXT(h, n)) {
                if (h->nlmsg_type == NLMSG_DONE || h->nlmsg_type == NLMSG_ERROR) { done = true; break; }
                auto *m = reinterpret_cast<inet_diag_msg *>(NLMSG_DATA(h));
                const int sport = ntohs(m->id.idiag_sport), dport = ntohs(m->id.idiag_dport);
                const bool own = mine.contains(m->idiag_inode);
                // own: our port is local or remote (we connect out in --pull); other: it talks to our port
                QString net = portNet.value(own ? sport : dport);
                if (net.isEmpty()) net = portNet.value(own ? dport : sport);
                if (net.isEmpty()) continue;
                int len = int(h->nlmsg_len - NLMSG_LENGTH(sizeof(*m)));
                for (auto *a = reinterpret_cast<rtattr *>(m + 1); RTA_OK(a, len); a = RTA_NEXT(a, len)) {
                    if (a->rta_type != INET_DIAG_INFO) continue;
                    tcp_info ti{};
                    memcpy(&ti, RTA_DATA(a), qMin<size_t>(sizeof(ti), RTA_PAYLOAD(a)));
                    C c;
                    if (own) {
                        c.srvSent = qint64(ti.tcpi_bytes_acked);
                        c.srvRecv = qint64(ti.tcpi_bytes_received);
                    } else {
                        c.cliRead = qMax<qint64>(0, qint64(ti.tcpi_bytes_received) - qint64(m->idiag_rqueue));
                        c.cliWritten = qint64(ti.tcpi_bytes_acked) + qint64(m->idiag_wqueue);
                    }
                    const quint64 key = (quint64(m->idiag_inode) << 1) | (own ? 1 : 0);
                    now.insert(key, c);
                    sockNet.insert(key, net);
                }
            }
        }
        ::close(fd);
        for (auto it = last.constBegin(); it != last.constEnd(); ++it)
            if (!now.contains(it.key())) add(gone[sockNet.value(it.key())], it.value());   // closed
        sum = gone;
        for (auto it = now.constBegin(); it != now.constEnd(); ++it) add(sum[sockNet.value(it.key())], it.value());
        last = now;
#endif
        return sum;
    }
};

// ---------------------------------------------------------------- a plain HTTP/1.1 file server
class FileServer : public QTcpServer {
public:
    explicit FileServer(const QString &path, QObject *parent = nullptr) : QTcpServer(parent), m_path(path)
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *s = nextPendingConnection()) serve(s);
        });
    }

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
                const QList<QByteArray> req = lines.value(0).trimmed().split(' ');
                const bool head = req.value(0) == "HEAD";
                c->f = std::make_unique<QFile>(m_path);
                if (!c->f->open(QIODevice::ReadOnly)) { s->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n"); continue; }
                const qint64 size = c->f->size();
                qint64 from = 0, to = size - 1;
                bool range = false;
                for (const QByteArray &l : lines) {
                    const QByteArray t = l.trimmed();
                    if (t.toLower().startsWith("range: bytes=")) {
                        const QList<QByteArray> p = t.mid(13).split('-');
                        from = p.value(0).toLongLong();
                        if (!p.value(1).isEmpty()) to = qMin(size - 1, p.value(1).toLongLong());
                        range = true;
                    }
                }
                QByteArray h = range ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n";
                h += "Accept-Ranges: bytes\r\nContent-Type: application/octet-stream\r\nContent-Length: " + QByteArray::number(to - from + 1) + "\r\n";
                if (range) h += "Content-Range: bytes " + QByteArray::number(from) + '-' + QByteArray::number(to) + '/' + QByteArray::number(size) + "\r\n";
                h += "\r\n";
                s->write(h);
                if (head) continue;
                c->f->seek(from);
                c->left = to - from + 1;
                pump();
            }
        });
    }
    QString m_path;
};

// [bw-2] a relay that adds a round trip: every byte in either direction waits oneWayMs before it is
// passed on (store and forward, no bandwidth limit), as a far peer does on the internet
class DelayRelay : public QTcpServer {
public:
    DelayRelay(quint16 target, int oneWayMs, QObject *parent = nullptr) : QTcpServer(parent), m_target(target), m_delay(oneWayMs)
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *c = nextPendingConnection()) relay(c);
        });
    }

private:
    void relay(QTcpSocket *c)
    {
        auto *u = new QTcpSocket(c);
        const int d = m_delay;
        auto pass = [d](QTcpSocket *from, QTcpSocket *to) {
            const QByteArray b = from->readAll();
            if (b.isEmpty()) return;
            QPointer<QTcpSocket> t(to);
            QTimer::singleShot(d, Qt::PreciseTimer, to, [t, b] { if (t && t->state() == QAbstractSocket::ConnectedState) t->write(b); });
        };
        connect(c, &QTcpSocket::readyRead, c, [=] { if (u->state() == QAbstractSocket::ConnectedState) pass(c, u); });
        connect(u, &QTcpSocket::connected, c, [=] { pass(c, u); });
        connect(u, &QTcpSocket::readyRead, c, [=] { pass(u, c); });
        auto close = [d](QTcpSocket *s) { QPointer<QTcpSocket> p(s); QTimer::singleShot(d + 50, s, [p] { if (p) p->disconnectFromHost(); }); };
        connect(u, &QTcpSocket::disconnected, c, [=] { close(c); });
        connect(c, &QTcpSocket::disconnected, c, [=] { close(u); c->deleteLater(); });
        u->connectToHost(QHostAddress::LocalHost, m_target);
    }
    quint16 m_target;
    int m_delay;
};

struct BtNode {
    SimpleSettings settings;
    std::unique_ptr<NetworkPlugin> plugin;
    TorrentBackend *be = nullptr;
    BitTorrentControl *ctl = nullptr;
    QHash<QString, QString> seeded;
    explicit BtNode(const QString &profile)
    {
        namespace k = keys;
        settings.setValue(QString(k::bt::Dht), false);
        settings.setValue(QString(k::bt::Lsd), false);
        settings.setValue(QString(k::connection::Upnp), false);
        settings.setValue(QString(k::connection::NatPmp), false);
        settings.setValue(QString(k::connection::Ipv6), false);
        settings.setValue(QString(k::connection::BindInterface), QStringLiteral("127.0.0.1"));
        settings.setValue(QString(k::bt::ListenPort), 0);
        settings.setValue(QString(k::bt::ExtraTrackers), QStringList());
        settings.setValue(QString(k::bt::SeedRatioLimit), 0.0);
        settings.setValue(QStringLiteral("bt/utp"), false);   // TCP only: the OS counters see everything
        settings.setValue(QStringLiteral("bt/allowMultipleConnectionsPerIp"), true);
        plugin.reset(createBitTorrentPlugin());
        be = dynamic_cast<TorrentBackend *>(plugin.get());
        ctl = dynamic_cast<BitTorrentControl *>(plugin.get());
        auto *ev = qobject_cast<BitTorrentEvents *>(be->torrentEvents());
        QObject::connect(ev, &BitTorrentEvents::seedReady, ev, [this](const QString &p, const QString &id) { seeded.insert(p, id); });
        PluginContext ctx;
        ctx.settings = &settings;
        ctx.profileDir = profile;
        ctx.dataDir = profile + QStringLiteral("/bt");
        ctx.clientName = QStringLiteral("OctoMX");
        ctx.clientVersion = QStringLiteral(MX_VERSION_STRING);
        plugin->init(ctx);
        plugin->start();
    }
    int port() const { return plugin->status().listenPort; }
};

struct Ed2kNode {
    mxtest::MemSettings settings;
    mxtest::FileLibrary lib;
    std::unique_ptr<ed2k::Ed2kPlugin> plugin;
    explicit Ed2kNode(const QString &profile)
    {
        settings.setValue(QStringLiteral("ed2k/tcpPort"), 0);
        settings.setValue(QStringLiteral("ed2k/udpPort"), 0);
        settings.setValue(QStringLiteral("ed2k/autoConnect"), false);
        settings.setValue(QStringLiteral("ed2k/autoUpdateServerList"), false);
        settings.setValue(QStringLiteral("ed2k/udpPing"), false);
        settings.setValue(QStringLiteral("ed2k/udpSearch"), false);
        settings.setValue(QStringLiteral("ed2k/udpSources"), false);
        settings.setValue(QStringLiteral("ed2k/allowLan"), true);
        settings.setValue(QStringLiteral("ed2k/uploadSlots"), 4);
        settings.setValue(QStringLiteral("ed2k/obfuscation"), true);
        QDir().mkpath(profile);
        plugin = std::make_unique<ed2k::Ed2kPlugin>();
        plugin->init(mxtest::makeContext(&settings, &lib, nullptr, profile));
        plugin->start();
    }
    QString uri() const
    {
        return QStringLiteral("ed2k-peer://127.0.0.1:%1?uid=%2&crypt=3").arg(plugin->core()->tcpPort()).arg(QString::fromLatin1(plugin->core()->userHash().toHex()));
    }
};

QString makeData(const QString &path, qint64 size)
{
    if (QFileInfo(path).size() == size) return path;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return QString();
    QRandomGenerator rng(2026);
    QByteArray buf(1 << 20, '\0');
    while (f.size() < size) {
        for (int i = 0; i < buf.size(); i += 4) {
            const quint32 v = rng.generate();
            memcpy(buf.data() + i, &v, 4);
        }
        f.write(buf.left(int(qMin<qint64>(buf.size(), size - f.size()))));
    }
    return path;
}

// ================================================================ serve
int serve(const QStringList &args)
{
    QString dir;
    int mb = 512;
    bool pull = false;
    int peers = 1, rttMs = 0;
    for (int i = 0; i < args.size(); ++i) {
        if (args[i] == QLatin1String("--dir")) dir = args.value(++i);
        else if (args[i] == QLatin1String("--mb")) mb = args.value(++i).toInt();
        else if (args[i] == QLatin1String("--pull")) pull = true;
        else if (args[i] == QLatin1String("--peers")) peers = qBound(1, args.value(++i).toInt(), 16);   // [bw-2] seeders
        else if (args[i] == QLatin1String("--rtt")) rttMs = qMax(0, args.value(++i).toInt());         // [bw-2] added round trip
    }
    if (dir.isEmpty()) { std::fprintf(stderr, "serve --dir D [--mb N] [--pull]\n"); return 2; }
    dir = QDir(dir).absolutePath();
    QDir().mkpath(dir);
    const QString file = makeData(dir + QStringLiteral("/data/octomx-bw2-test.bin"), qint64(mb) << 20);
    bool ok = false;
    const mxtest::Ed2kFileHash eh = mxtest::hashFile(file, &ok);
    if (!ok) return 3;
    OsMeter meter;
    QFile csv(dir + (pull ? QStringLiteral("/os-up.csv") : QStringLiteral("/os.csv")));
    if (!csv.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 8;
    QTextStream out(&csv);
    QStringList nets{QStringLiteral("bt"), QStringLiteral("http"), QStringLiteral("ed2k")};
    out << "ms";
    for (const QString &n : nets) out << ',' << n << "_sent," << n << "_recv," << n << "_read," << n << "_written";
    out << '\n';
    auto sampler = new QTimer(qApp);
    sampler->setTimerType(Qt::PreciseTimer);
    sampler->setInterval(50);
    QObject::connect(sampler, &QTimer::timeout, qApp, [&] {
        const auto s = meter.poll();
        out << nowMs();
        for (const QString &n : nets) {
            const OsMeter::C c = s.value(n);
            out << ',' << c.srvSent << ',' << c.srvRecv << ',' << c.cliRead << ',' << c.cliWritten;
        }
        out << '\n';
        out.flush();
    });

    std::unique_ptr<BtNode> bt;
    std::vector<std::unique_ptr<BtNode>> moreSeeds;   // [bw-2] --peers
    std::vector<std::unique_ptr<DelayRelay>> relays;  // [bw-2] --rtt
    auto front = [&](quint16 port) -> quint16 {      // the port the program is given (a relay with --rtt)
        if (rttMs <= 0) return port;
        relays.push_back(std::make_unique<DelayRelay>(port, rttMs / 2));
        relays.back()->listen(QHostAddress::LocalHost, 0);
        return relays.back()->serverPort();
    };
    std::unique_ptr<Ed2kNode> ed;
    std::unique_ptr<FileServer> http;
    QList<mx::RangeSource *> sources;
    std::unique_ptr<mxtest::SimpleDownloader> dl;
    if (!pull) {
        bt = std::make_unique<BtNode>(dir + QStringLiteral("/seed"));
        if (!QTest::qWaitFor([&] { return bt->port() > 0; }, 10000)) return 4;
        QString err;
        if (!bt->be->seedLocalFile(file, &err)) return 5;
        if (!QTest::qWaitFor([&] { return !bt->seeded.isEmpty(); }, 120000)) return 6;
        const QString tid = bt->seeded.begin().value();
        const QString torrent = dir + QStringLiteral("/seed/bt/torrents/") + tid + QStringLiteral(".torrent");
        if (!QTest::qWaitFor([&] { return QFileInfo(torrent).size() > 0; }, 10000)) return 9;
        for (int i = 1; i < peers; ++i) {   // [bw-2] more seeders of the same file
            moreSeeds.push_back(std::make_unique<BtNode>(dir + QStringLiteral("/seed%1").arg(i + 1)));
            BtNode &n = *moreSeeds.back();
            if (!QTest::qWaitFor([&] { return n.port() > 0; }, 10000)) return 4;
            if (!n.be->seedLocalFile(file, &err)) return 5;
            if (!QTest::qWaitFor([&] { return !n.seeded.isEmpty(); }, 120000)) return 6;
        }
        ed = std::make_unique<Ed2kNode>(dir + QStringLiteral("/ed2k"));
        ed->lib.addFile(file);
        ed->plugin->setSharedFiles(ed->lib.sharedFiles());
        http = std::make_unique<FileServer>(file);
        http->listen(QHostAddress::LocalHost, 0);
        QJsonArray btPeers;
        for (BtNode *n : [&] { QList<BtNode *> l{bt.get()}; for (auto &m : moreSeeds) l << m.get(); return l; }()) {
            const quint16 p = front(quint16(n->port()));
            meter.portNet.insert(p, QStringLiteral("bt"));
            btPeers << QStringLiteral("127.0.0.1:%1").arg(p);
        }
        const quint16 httpPort = front(http->serverPort());
        meter.portNet.insert(httpPort, QStringLiteral("http"));
        meter.portNet.insert(ed->plugin->core()->tcpPort(), QStringLiteral("ed2k"));
        QJsonObject o;
        o[QStringLiteral("file")] = file;
        o[QStringLiteral("size")] = QFileInfo(file).size();
        o[QStringLiteral("ed2k")] = QString::fromLatin1(eh.fileHash.toHex());
        o[QStringLiteral("btih")] = tid;
        o[QStringLiteral("torrent")] = torrent;
        o[QStringLiteral("btPeer")] = btPeers.first().toString();
        o[QStringLiteral("btPeers")] = btPeers;
        o[QStringLiteral("rttMs")] = rttMs;
        o[QStringLiteral("http")] = QStringLiteral("http://127.0.0.1:%1/octomx-bw2-test.bin").arg(httpPort);
        o[QStringLiteral("ed2kPeer")] = ed->uri();
        writeJson(dir + QStringLiteral("/serve.json"), o);
        std::printf("serve: ready, bt %d http %d ed2k %d\n", bt->port(), http->serverPort(), ed->plugin->core()->tcpPort());
        std::fflush(stdout);
    } else {
        // wait for the program under test to seed / share, then pull from it
        const QString cj = dir + QStringLiteral("/client.json");
        QFile::remove(cj);
        std::printf("serve --pull: waiting for %s\n", qPrintable(cj));
        std::fflush(stdout);
        if (!QTest::qWaitFor([&] { return QFile::exists(cj); }, 600000)) return 7;
        const QJsonObject c = readJson(cj);
        const QStringList pnets = c.value(QStringLiteral("nets")).toString().split(QLatin1Char(','));
        if (pnets.contains(QLatin1String("bt"))) {
            bt = std::make_unique<BtNode>(dir + QStringLiteral("/leech"));
            if (!QTest::qWaitFor([&] { return bt->port() > 0; }, 10000)) return 4;
            meter.portNet.insert(bt->port(), QStringLiteral("bt"));
            meter.portNet.insert(c.value(QStringLiteral("btPort")).toInt(), QStringLiteral("bt"));
            QString err;
            const QString tid = c.value(QStringLiteral("btih")).toString();
            QDir(dir + QStringLiteral("/leech-dl")).removeRecursively();
            bt->be->addTorrent(QStringLiteral("magnet:?xt=urn:btih:%1&dn=x").arg(tid), {}, dir + QStringLiteral("/leech-dl"), false, {}, &err);
            const QString peer = QStringLiteral("127.0.0.1:%1").arg(c.value(QStringLiteral("btPort")).toInt());
            bt->ctl->addPeer(tid, peer);
            auto again = new QTimer(qApp);
            again->setInterval(3000);
            QObject::connect(again, &QTimer::timeout, qApp, [&bt, tid, peer] { bt->ctl->addPeer(tid, peer); });
            again->start();
        }
        if (pnets.contains(QLatin1String("ed2k"))) {
            ed = std::make_unique<Ed2kNode>(dir + QStringLiteral("/ed2k-pull"));
            meter.portNet.insert(ed->plugin->core()->tcpPort(), QStringLiteral("ed2k"));
            meter.portNet.insert(c.value(QStringLiteral("ed2kPort")).toInt(), QStringLiteral("ed2k"));
            SearchSource ss;
            ss.net = Net::ED2K;
            ss.uri = c.value(QStringLiteral("ed2kPeer")).toString();
            FileHashes fh;
            fh.ed2k = eh.fileHash;
            const qint64 size = QFileInfo(file).size();
            QFile::remove(dir + QStringLiteral("/ed2k-pull.bin"));
            dl = std::make_unique<mxtest::SimpleDownloader>(dir + QStringLiteral("/ed2k-pull.bin"), size, eh.fileHash);
            for (int i = 0; i < 2; ++i)
                if (RangeSource *rs = ed->plugin->openSource(QStringLiteral("pull%1").arg(i), fh, size, ss)) dl->addSource(rs);
        }
        std::printf("serve --pull: pulling %s\n", qPrintable(pnets.join(QLatin1Char(','))));
        std::fflush(stdout);
    }
    sampler->start();
    return qApp->exec();
}

// ================================================================ client
int client(const QStringList &args)
{
    QString profile, servePath, mode = QStringLiteral("down");
    QStringList nets{QStringLiteral("bt"), QStringLiteral("http"), QStringLiteral("ed2k")};
    int down = 0, up = 0, seconds = 60;
    QHash<QString, int> joinAt, leaveAt;
    QList<std::tuple<int, int, int>> changes;
    for (int i = 0; i < args.size(); ++i) {
        const QString a = args[i];
        if (a == QLatin1String("--profile")) profile = args.value(++i);
        else if (a == QLatin1String("--serve")) servePath = args.value(++i);
        else if (a == QLatin1String("--nets")) nets = args.value(++i).split(QLatin1Char(','));
        else if (a == QLatin1String("--down")) down = args.value(++i).toInt();
        else if (a == QLatin1String("--up")) up = args.value(++i).toInt();
        else if (a == QLatin1String("--mode")) mode = args.value(++i);
        else if (a == QLatin1String("--seconds")) seconds = args.value(++i).toInt();
        else if (a == QLatin1String("--join") || a == QLatin1String("--leave")) {
            const QStringList p = args.value(++i).split(QLatin1Char(':'));
            (a == QLatin1String("--join") ? joinAt : leaveAt).insert(p.value(0), p.value(1).toInt());
        } else if (a == QLatin1String("--at")) {
            const QStringList p = args.value(++i).split(QLatin1Char(':'));
            changes << std::make_tuple(p.value(0).toInt(), p.value(1).toInt(), p.value(2).toInt());
        }
    }
    if (profile.isEmpty() || servePath.isEmpty()) { std::fprintf(stderr, "client --profile P --serve serve.json ...\n"); return 2; }
    profile = QDir(profile).absolutePath();
    QDir(profile).removeRecursively();
    QDir().mkpath(profile);
    const QJsonObject sj = readJson(servePath);
    if (sj.isEmpty()) return 3;

    QList<NetworkPlugin *> plugins;
    NetworkPlugin *btp = nullptr;
    ed2k::Ed2kPlugin *edp = nullptr;
    if (nets.contains(QLatin1String("bt"))) plugins << (btp = createBitTorrentPlugin());
    if (nets.contains(QLatin1String("ed2k"))) {
        const QList<NetworkPlugin *> e = createEd2kPlugins();
        edp = dynamic_cast<ed2k::Ed2kPlugin *>(e.value(0));
        plugins << e.value(0);
        delete e.value(1);   // no Kad
    }
    if (nets.contains(QLatin1String("http")) && mode == QLatin1String("down")) plugins << createHttpPlugin();
    Core *core = createRealCore(profile, plugins);
    Settings *st = core->settings();
    auto set = [st](const char *k, const QVariant &v) { st->setValue(QString::fromLatin1(k), v); };
    st->setValue(QString(keys::transfers::IncomingDir), profile + QStringLiteral("/incoming"));
    set("bt/dht", false);
    set("bt/lsd", false);
    set("bt/utp", false);
    set("bt/listenPort", 0);
    st->setValue(QString(keys::bt::ExtraTrackers), QStringList());
    set("bt/allowMultipleConnectionsPerIp", true);
    st->setValue(QString(keys::connection::Upnp), false);
    st->setValue(QString(keys::connection::NatPmp), false);
    st->setValue(QString(keys::connection::BindInterface), QStringLiteral("127.0.0.1"));
    set("ed2k/autoConnect", false);
    set("ed2k/autoUpdateServerList", false);
    set("ed2k/udpPing", false);
    set("ed2k/udpSearch", false);
    set("ed2k/udpSources", false);
    set("ed2k/allowLan", true);
    set("ed2k/tcpPort", 0);
    set("ed2k/udpPort", 0);
    set("kad/enabled", false);
    st->setValue(QString(keys::bandwidth::LimitLan), true);   // 127.0.0.1 is limited like the internet
    auto setLimits = [st](int d, int u) {
        if (d > 0) st->setValue(QString(keys::bandwidth::LastDownLimitKBs), d);
        st->setValue(QString(keys::bandwidth::DownLimitKBs), d);
        if (u > 0) st->setValue(QString(keys::bandwidth::LastUpLimitKBs), u);
        st->setValue(QString(keys::bandwidth::UpLimitKBs), u);
    };
    setLimits(down, up);
    core->start();
    auto *rc = static_cast<RealCore *>(core);
    CoreRateLimiter *lim = rc->rateLimiter();

    QFile prog(profile + QStringLiteral("/prog.csv")), graph(profile + QStringLiteral("/graph.csv"));
    if (!prog.open(QIODevice::WriteOnly | QIODevice::Truncate) || !graph.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 8;
    QTextStream po(&prog), go(&graph);
    const QList<QPair<QString, Net>> netIds{{QStringLiteral("bt"), Net::BitTorrent}, {QStringLiteral("http"), Net::Http}, {QStringLiteral("ed2k"), Net::ED2K}};
    po << "ms,limDown,limUp,btBudgetDown,btBudgetUp";
    for (const auto &n : netIds) po << ',' << n.first << "_down," << n.first << "_up," << n.first << "_payDown," << n.first << "_payUp";
    po << '\n';
    go << "ms,down,up,payDown,payUp";
    for (const auto &n : netIds) go << ',' << n.first << "_down," << n.first << "_up";
    go << ",xferDown,xferUp\n";   // the Transfers page's rate columns, summed (what its total shows)
    QObject::connect(core->bandwidth(), &BandwidthService::sample, core, [&](const BandwidthSample &s) {
        go << nowMs() << ',' << s.downBps << ',' << s.upBps << ',' << s.payloadDownBps << ',' << s.payloadUpBps;
        for (const auto &n : netIds) go << ',' << s.netDownBps.value(n.second) << ',' << s.netUpBps.value(n.second);
        qint64 xd = 0, xu = 0;
        for (const TransferInfo &t : core->transfers()->downloads()) xd += t.rateDownBps;
        for (const TransferInfo &t : core->transfers()->uploads()) xu += t.rateUpBps;
        go << ',' << xd << ',' << xu;
        go << '\n';
        go.flush();
    });
    QTimer sampler;
    sampler.setTimerType(Qt::PreciseTimer);
    sampler.setInterval(50);
    QObject::connect(&sampler, &QTimer::timeout, core, [&] {
        po << nowMs() << ',' << lim->limitBps(RateLimiter::Down) << ',' << lim->limitBps(RateLimiter::Up) << ','
           << lim->budgetBps(RateLimiter::Down, Net::BitTorrent) << ',' << lim->budgetBps(RateLimiter::Up, Net::BitTorrent);
        for (const auto &n : netIds)
            po << ',' << lim->total(RateLimiter::Down, n.second) << ',' << lim->total(RateLimiter::Up, n.second) << ','
               << lim->payloadTotal(RateLimiter::Down, n.second) << ',' << lim->payloadTotal(RateLimiter::Up, n.second);
        po << '\n';
        po.flush();
    });
    sampler.start();

    QString dlId, tid;
    const QString btPeer = sj.value(QStringLiteral("btPeer")).toString();
    auto *btc = dynamic_cast<BitTorrentControl *>(btp);
    auto *btb = dynamic_cast<TorrentBackend *>(btp);
    QHash<QString, QString> seeded;
    if (btb)
        QObject::connect(qobject_cast<BitTorrentEvents *>(btb->torrentEvents()), &BitTorrentEvents::seedReady, core,
                         [&seeded](const QString &p, const QString &id) { seeded.insert(p, id); });
    auto sourceOf = [&](const QString &n) {
        SearchSource s;
        if (n == QLatin1String("http")) { s.net = Net::Http; s.uri = sj.value(QStringLiteral("http")).toString(); }
        else if (n == QLatin1String("ed2k")) { s.net = Net::ED2K; s.uri = sj.value(QStringLiteral("ed2kPeer")).toString(); }
        return s;
    };
    QTextStream log(stdout);
    // start: after 3 s (plugins listening)
    QTimer::singleShot(3000, core, [&] {
        if (mode == QLatin1String("down")) {
            DownloadRequest r;
            r.name = QStringLiteral("octomx-bw2-test.bin");
            r.size = sj.value(QStringLiteral("size")).toVariant().toLongLong();
            r.hashes.ed2k = QByteArray::fromHex(sj.value(QStringLiteral("ed2k")).toString().toLatin1());
            if (nets.contains(QLatin1String("bt")) && !joinAt.contains(QStringLiteral("bt"))) {
                r.uri = sj.value(QStringLiteral("torrent")).toString();
                tid = sj.value(QStringLiteral("btih")).toString();
            } else {
                r.uri = FileHashes(r.hashes).toMagnet(r.name, r.size);
            }
            for (const QString &n : nets)
                if (n != QLatin1String("bt") && !joinAt.contains(n)) r.sources << sourceOf(n);
            dlId = core->transfers()->download(r);
            log << "client: download " << dlId << " nets " << nets.join(QLatin1Char(',')) << Qt::endl;
        } else {
            QJsonObject c;
            c[QStringLiteral("nets")] = nets.join(QLatin1Char(','));
            const QString file = sj.value(QStringLiteral("file")).toString();
            if (btb) {
                QString err;
                btb->seedLocalFile(file, &err);
                if (!QTest::qWaitFor([&] { return !seeded.isEmpty(); }, 120000)) log << "client: seeding did not start" << Qt::endl;
                c[QStringLiteral("btih")] = seeded.value(file, seeded.isEmpty() ? QString() : seeded.begin().value());
                c[QStringLiteral("btPort")] = btp->status().listenPort;
            }
            if (edp) {
                core->library()->addFolder(QFileInfo(file).absolutePath(), false);
                const bool hashed = QTest::qWaitFor([&] {
                    for (const SharedFile &f : core->library()->files())
                        if (f.path == file && !f.hashes.ed2k.isEmpty()) return true;
                    return false;
                }, 120000);
                if (!hashed) log << "client: the library did not hash the file" << Qt::endl;
                QTest::qWait(1500);   // pushed to the plugins
                c[QStringLiteral("ed2kPort")] = edp->core()->tcpPort();
                c[QStringLiteral("ed2kPeer")] = QStringLiteral("ed2k-peer://127.0.0.1:%1?uid=%2&crypt=3").arg(edp->core()->tcpPort()).arg(QString::fromLatin1(edp->core()->userHash().toHex()));
            }
            writeJson(QFileInfo(servePath).absolutePath() + QStringLiteral("/client.json"), c);
            log << "client: seeding / sharing, client.json written" << Qt::endl;
        }
    });
    // BitTorrent: tell libtorrent the seeder (again, until connected)
    QTimer peerTimer;
    peerTimer.setInterval(2000);
    QStringList btPeers;
    for (const QJsonValue &v : sj.value(QStringLiteral("btPeers")).toArray()) btPeers << v.toString();
    if (btPeers.isEmpty()) btPeers << btPeer;
    QObject::connect(&peerTimer, &QTimer::timeout, core, [&] {
        if (btc && !tid.isEmpty())
            for (const QString &p : btPeers) btc->addPeer(tid, p);
    });
    peerTimer.start();
    int tick = 0;
    QTimer secTimer;
    secTimer.setInterval(1000);
    QObject::connect(&secTimer, &QTimer::timeout, core, [&] {
        ++tick;
        for (const auto &c : changes)
            if (std::get<0>(c) == tick) { setLimits(std::get<1>(c), std::get<2>(c)); log << "[" << tick << "s] limits " << std::get<1>(c) << "/" << std::get<2>(c) << Qt::endl; }
        for (auto it = joinAt.constBegin(); it != joinAt.constEnd(); ++it)
            if (it.value() == tick && mode == QLatin1String("down")) {
                DownloadRequest r;
                r.name = QStringLiteral("octomx-bw2-test.bin");
                r.size = sj.value(QStringLiteral("size")).toVariant().toLongLong();
                r.hashes.ed2k = QByteArray::fromHex(sj.value(QStringLiteral("ed2k")).toString().toLatin1());
                if (it.key() == QLatin1String("bt")) {
                    r.uri = sj.value(QStringLiteral("torrent")).toString();
                    tid = sj.value(QStringLiteral("btih")).toString();
                } else {
                    r.sources << sourceOf(it.key());
                }
                core->transfers()->download(r);   // joins the running download
                log << "[" << tick << "s] joined " << it.key() << Qt::endl;
            }
        for (auto it = leaveAt.constBegin(); it != leaveAt.constEnd(); ++it)
            if (it.value() == tick && !dlId.isEmpty()) {
                for (const TransferInfo &t : core->transfers()->downloads())
                    if (t.id == dlId)
                        for (const TransferSource &c : t.sources)
                            if (!c.id.isEmpty() && netShortName(c.net).toLower() == it.key())
                                core->transfers()->cancelSource(dlId, c.id);
                log << "[" << tick << "s] left " << it.key() << Qt::endl;
            }
        if (tick % 10 == 0) {
            const TransferInfo t = core->transfers()->transfer(dlId);
            log << "[" << tick << "s] done " << t.done << " state " << int(t.state) << " " << t.statusText << Qt::endl;
        }
        if (tick >= seconds) {
            secTimer.stop();
            sampler.stop();
            if (!dlId.isEmpty()) core->transfers()->cancel(dlId, true);
            if (btb && !seeded.isEmpty()) btb->removeTorrent(seeded.begin().value(), false);
            core->shutdown();
            QTimer::singleShot(6000, qApp, &QCoreApplication::quit);
        }
    });
    secTimer.start();
    QObject::connect(core, &Core::shutdownFinished, qApp, &QCoreApplication::quit);
    const int rcode = qApp->exec();
    QDir(profile + QStringLiteral("/incoming")).removeRecursively();
    return rcode;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    registerMetaTypes();
    const QStringList args = app.arguments().mid(1);
    if (args.value(0) == QLatin1String("serve")) return serve(args.mid(1));
    if (args.value(0) == QLatin1String("client")) return client(args.mid(1));
    std::fprintf(stderr, "usage: see the head of tests/engine/bw_slice_probe.cpp\n");
    return 2;
}
