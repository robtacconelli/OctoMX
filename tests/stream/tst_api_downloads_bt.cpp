// SPDX-License-Identifier: GPL-3.0-or-later
// [api-2] Downloads through the API over BitTorrent with the real libtorrent, offline on 127.0.0.1:
// node A seeds, the core (node B) downloads. A bundle with "files": only those files are fetched, the
// wanted files changed afterwards; a magnet link (peers and seeds in the status); a file of the torrent
// that was on the disk before the download stays when the download is deleted with its files.
#include <QtTest/QTest>

#include <libtorrent/bencode.hpp>
#include <libtorrent/create_torrent.hpp>

#include "bt_node.h"
#include "bt_plugin.h"
#include "stream_support.h"

using namespace mx;
using namespace mx::stest;

namespace {

constexpr qint64 MiB = 1024 * 1024;

QByteArray makeTorrent(const QString &root, int pieceSize, const QStringList &names)
{
    lt::file_storage fs;
    const QString top = QFileInfo(root).fileName();
    for (const QString &n : names)
        fs.add_file(QDir::toNativeSeparators(top + QLatin1Char('/') + n).toStdString(), QFileInfo(root + QLatin1Char('/') + n).size());
    lt::create_torrent ct(fs, pieceSize, lt::create_torrent::v1_only);
    lt::set_piece_hashes(ct, QDir::toNativeSeparators(QFileInfo(root).absolutePath()).toStdString());
    std::vector<char> out;
    lt::bencode(std::back_inserter(out), ct.generate());
    return QByteArray(out.data(), int(out.size()));
}

void writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
}

void localBt(Settings *s)
{
    namespace k = keys;
    s->setValue(QString(k::bt::Dht), false);
    s->setValue(QString(k::bt::Lsd), false);
    s->setValue(QString(k::connection::Upnp), false);
    s->setValue(QString(k::connection::NatPmp), false);
    s->setValue(QString(k::connection::Ipv6), false);
    s->setValue(QString(k::bt::ListenPort), 0);
    s->setValue(QString(k::bt::ExtraTrackers), QStringList());
    s->setValue(QString(k::bt::SeedRatioLimit), 0.0);
    s->setValue(QStringLiteral("bt/allowMultipleConnectionsPerIp"), true);
    s->setValue(keys::api::AllowDelete, true);
}

QJsonObject getDl(StreamEnv &env, const QString &id) { return env.api("GET", "/api/v1/downloads/" + id.toLatin1()).json(); }

// node A seeding a folder torrent of `files`, and the core with its own libtorrent
struct Swarm {
    StreamEnv env;
    std::unique_ptr<etest::BtNode> a;
    QString tid, folderName;
    NetworkPlugin *bt = nullptr;
    TorrentBackend *btBe = nullptr;
    BitTorrentControl *btCtl = nullptr;
    QTimer hint;
    QList<QPair<QString, QByteArray>> files;

    explicit Swarm(const QString &name) : env(name) {}
    // seed first, then the core (search: a Web network that lists the torrent as one result)
    bool up(const QString &folder, const QList<QPair<QString, QByteArray>> &fl, bool listInSearch)
    {
        files = fl;
        folderName = folder;
        const QString dir = env.root + QStringLiteral("/seed/") + folder;
        QStringList names;
        for (const auto &f : fl) {
            writeFile(dir + QLatin1Char('/') + f.first, f.second);
            names << f.first;
        }
        const QByteArray torrent = makeTorrent(dir, 256 * 1024, names);
        a = std::make_unique<etest::BtNode>(env.root + QStringLiteral("/nodeA"), true);
        if (!a->start()) return false;
        a->ctl->setRateLimits(0, 4 * 1000 * 1000);
        QString err;
        tid = a->be->addTorrent(QString(), torrent, env.root + QStringLiteral("/seed"), false, {}, &err);
        if (tid.isEmpty() || !QTest::qWaitFor([&] { return a->be->snapshot(tid).finished; }, 60000)) return false;
        if (listInSearch) {
            TestPlugin *search = env.addPlugin(Net::Web, 0);
            SearchHit hit;
            hit.fileName = folder;
            hit.isBundle = true;
            hit.hashes.btih = QByteArray::fromHex(tid.toLatin1());
            SearchSource src;
            src.net = Net::BitTorrent;
            src.uri = QStringLiteral("magnet:?xt=urn:btih:%1&dn=%2").arg(tid, QString::fromLatin1(QUrl::toPercentEncoding(folder)));
            hit.sources << src;
            int i = 0;
            for (const auto &f : fl) {
                hit.files << BundleFile{folder + QLatin1Char('/') + f.first, f.second.size(), i++};
                hit.size += f.second.size();
            }
            search->cannedHits << hit;
        }
        bt = createBitTorrentPlugin();
        btBe = dynamic_cast<TorrentBackend *>(bt);
        btCtl = dynamic_cast<BitTorrentControl *>(bt);
        env.configure = [](Settings *s) { localBt(s); };
        env.up({bt});
        if (!QTest::qWaitFor([&] { return bt->status().listenPort > 0; }, 10000)) return false;
        // the connection policy restarts the session once after the start (see tst_stream_bt)
        const int firstPort = bt->status().listenPort;
        (void)QTest::qWaitFor([&] { const int p = bt->status().listenPort; return p > 0 && p != firstPort; }, 5000);
        if (!QTest::qWaitFor([&] { return bt->status().listenPort > 0; }, 10000)) return false;
        // the seed, told until a peer is connected (no DHT, no tracker)
        hint.setInterval(1000);
        QObject::connect(&hint, &QTimer::timeout, [this] {
            if (btBe->torrents().contains(tid) && btBe->snapshot(tid).peers + btBe->snapshot(tid).seeds == 0) btCtl->addPeer(tid, a->endpoint());
        });
        hint.start();
        return true;
    }
    QString magnet() const
    {
        return QStringLiteral("magnet:?xt=urn:btih:%1&dn=%2").arg(tid, QString::fromLatin1(QUrl::toPercentEncoding(folderName)));
    }
    QString target(int i) const { return env.root + QStringLiteral("/incoming/") + folderName + QLatin1Char('/') + files.at(i).first; }
};

QJsonObject fileOf(const QJsonObject &dl, int index)
{
    for (const QJsonValue &v : dl.value(QStringLiteral("files")).toArray())
        if (v.toObject().value(QStringLiteral("index")).toInt() == index) return v.toObject();
    return {};
}

bool fileDone(const QJsonObject &dl, int index, qint64 size)
{
    return qint64(fileOf(dl, index).value(QStringLiteral("done")).toDouble()) == size;
}

} // namespace

class TstApiDownloadsBt : public QObject {
    Q_OBJECT
private slots:
    void bundleWithFiles();
    void magnetLink();
    void fileThatWasThereStays();
};

void TstApiDownloadsBt::bundleWithFiles()
{
    Swarm sw(QStringLiteral("dlbt-bundle"));
    const QByteArray f0 = randomBytes(MiB + 11, 700), f1 = randomBytes(6 * MiB + 22, 701), f2 = randomBytes(2 * MiB + 33, 702);
    QVERIFY(sw.up(QStringLiteral("Big Buck Bunny extras"),
                  {{QStringLiteral("notes.txt"), f0}, {QStringLiteral("poster.png"), f1}, {QStringLiteral("trailer.mkv"), f2}}, true));
    StreamEnv &env = sw.env;
    const Reply s = env.api("POST", "/api/v1/searches", QJsonObject{{QStringLiteral("text"), QStringLiteral("bunny")}});
    const int sid = s.json().value(QStringLiteral("id")).toInt();
    QString key;
    QVERIFY(QTest::qWaitFor([&] {
        const QJsonArray r = env.api("GET", "/api/v1/searches/" + QByteArray::number(sid)).json().value(QStringLiteral("results")).toArray();
        if (!r.isEmpty()) key = r.at(0).toObject().value(QStringLiteral("key")).toString();
        return !key.isEmpty();
    }, 10000));
    // a file that is not in the bundle: 400; then files 0 and 2
    QCOMPARE(env.api("POST", "/api/v1/downloads", QJsonObject{{QStringLiteral("search"), sid}, {QStringLiteral("result"), key}, {QStringLiteral("files"), QJsonArray{0, 7}}}).status, 400);
    const Reply r = env.api("POST", "/api/v1/downloads", QJsonObject{{QStringLiteral("search"), sid}, {QStringLiteral("result"), key}, {QStringLiteral("files"), QJsonArray{0, 2}}});
    QCOMPARE(r.status, 201);
    const QString id = r.json().value(QStringLiteral("id")).toString();
    QJsonObject o;
    bool sawPeers = false;
    QVERIFY2(QTest::qWaitFor([&] {
        o = getDl(env, id);
        sawPeers = sawPeers || o.value(QStringLiteral("peers")).toInt() + o.value(QStringLiteral("seeds")).toInt() > 0;
        return fileDone(o, 0, f0.size()) && fileDone(o, 2, f2.size());
    }, 120000), QJsonDocument(o).toJson().constData());
    QVERIFY(sawPeers);
    QCOMPARE(o.value(QStringLiteral("bundle")).toBool(), true);   // once the torrent's file list is known
    QCOMPARE(fileOf(o, 0).value(QStringLiteral("wanted")).toBool(), true);
    QCOMPARE(fileOf(o, 1).value(QStringLiteral("wanted")).toBool(true), false);
    QCOMPARE(fileOf(o, 2).value(QStringLiteral("wanted")).toBool(), true);
    // only those files: of the one not wanted, at most the two pieces it shares with them
    QVERIFY(QTest::qWaitFor([&] { return QStringList{QStringLiteral("completed"), QStringLiteral("seeding")}.contains(getDl(env, id).value(QStringLiteral("state")).toString()); }, 30000));
    o = getDl(env, id);
    const qint64 notWanted = qint64(fileOf(o, 1).value(QStringLiteral("done")).toDouble());
    qInfo("the file not wanted: %lld of %lld bytes", notWanted, qint64(f1.size()));
    QVERIFY(notWanted <= 2 * 256 * 1024);
    QVERIFY(!QFileInfo::exists(sw.target(1)) || QFileInfo(sw.target(1)).size() < f1.size());
    QCOMPARE(readFile(sw.target(0)), f0);
    QCOMPARE(readFile(sw.target(2)), f2);
    // the wanted files changed afterwards: the third one is fetched too
    QCOMPARE(env.api("POST", "/api/v1/downloads/" + id.toLatin1(), QJsonObject{{QStringLiteral("files"), QJsonArray{0, 7}}}).status, 400);
    const Reply ch = env.api("POST", "/api/v1/downloads/" + id.toLatin1(), QJsonObject{{QStringLiteral("files"), QJsonArray{0, 1, 2}}});
    QCOMPARE(ch.status, 200);
    QCOMPARE(fileOf(ch.json(), 1).value(QStringLiteral("wanted")).toBool(), true);
    QVERIFY2(QTest::qWaitFor([&] { o = getDl(env, id); return fileDone(o, 1, f1.size()); }, 120000), QJsonDocument(o).toJson().constData());
    QVERIFY(QTest::qWaitFor([&] { return QStringList{QStringLiteral("completed"), QStringLiteral("seeding")}.contains(getDl(env, id).value(QStringLiteral("state")).toString()); }, 30000));
    QCOMPARE(readFile(sw.target(1)), f1);
    QCOMPARE(getDl(env, id).value(QStringLiteral("progress")).toDouble(), 1.0);
}

void TstApiDownloadsBt::magnetLink()
{
    Swarm sw(QStringLiteral("dlbt-magnet"));
    const QByteArray f0 = randomBytes(5 * MiB + 1, 710), f1 = randomBytes(MiB + 2, 711);
    QVERIFY(sw.up(QStringLiteral("ubuntu-24.04-netboot"), {{QStringLiteral("netboot.tar.gz"), f0}, {QStringLiteral("SHA256SUMS"), f1}}, false));
    StreamEnv &env = sw.env;
    const Reply r = env.api("POST", "/api/v1/downloads", QJsonObject{{QStringLiteral("link"), sw.magnet()}});
    QCOMPARE(r.status, 201);
    const QString id = r.json().value(QStringLiteral("id")).toString();
    QCOMPARE(r.json().value(QStringLiteral("hashes")).toObject().value(QStringLiteral("btih")).toString(), sw.tid);
    QCOMPARE(r.json().value(QStringLiteral("origin")).toString(), QStringLiteral("api"));
    // the file list arrives from the peer; peers and seeds as BitTorrent tells them
    int maxSeeds = 0, maxSpeed = 0;
    QJsonObject o;
    QVERIFY2(QTest::qWaitFor([&] {
        o = getDl(env, id);
        maxSeeds = qMax(maxSeeds, o.value(QStringLiteral("seeds")).toInt());
        maxSpeed = qMax(maxSpeed, o.value(QStringLiteral("speedBps")).toInt());
        return QStringList{QStringLiteral("completed"), QStringLiteral("seeding")}.contains(o.value(QStringLiteral("state")).toString());
    }, 120000), QJsonDocument(o).toJson().constData());
    qInfo("seeds at most %d, speed at most %d B/s", maxSeeds, maxSpeed);
    QVERIFY(maxSeeds >= 1);
    QVERIFY(o.value(QStringLiteral("peers")).isDouble());
    QCOMPARE(o.value(QStringLiteral("bundle")).toBool(), true);
    QCOMPARE(o.value(QStringLiteral("files")).toArray().size(), 2);
    QCOMPARE(o.value(QStringLiteral("name")).toString(), QStringLiteral("ubuntu-24.04-netboot"));
    QCOMPARE(readFile(sw.target(0)), f0);
    QCOMPARE(readFile(sw.target(1)), f1);
    // the same link again: the same download
    const Reply again = env.api("POST", "/api/v1/downloads", QJsonObject{{QStringLiteral("link"), sw.magnet()}});
    QCOMPARE(again.status, 200);
    QCOMPARE(again.json().value(QStringLiteral("id")).toString(), id);
}

void TstApiDownloadsBt::fileThatWasThereStays()
{
    Swarm sw(QStringLiteral("dlbt-there"));
    const QByteArray f0 = randomBytes(MiB + 5, 720), f1 = randomBytes(3 * MiB + 6, 721);
    QVERIFY(sw.up(QStringLiteral("debian-12.8.0-amd64-netinst"), {{QStringLiteral("README.txt"), f0}, {QStringLiteral("netinst.img"), f1}}, false));
    StreamEnv &env = sw.env;
    // the first file is in the incoming folder already, before the download
    writeFile(sw.target(0), f0);
    const Reply r = env.api("POST", "/api/v1/downloads", QJsonObject{{QStringLiteral("link"), sw.magnet()}});
    QCOMPARE(r.status, 201);
    const QString id = r.json().value(QStringLiteral("id")).toString();
    QVERIFY(QTest::qWaitFor([&] { return QStringList{QStringLiteral("completed"), QStringLiteral("seeding")}.contains(getDl(env, id).value(QStringLiteral("state")).toString()); }, 120000));
    QCOMPARE(readFile(sw.target(1)), f1);
    const DownloadDiskInfo di = env.core->engine()->diskInfo(id);
    QVERIFY2(di.kept.contains(QFileInfo(sw.target(0)).absoluteFilePath()), qPrintable(di.kept.join(QLatin1Char(' '))));
    QVERIFY(!di.paths.contains(QFileInfo(sw.target(0)).absoluteFilePath()));
    // deleted with its files: what it created goes, the file that was there stays
    Reply d = env.api("DELETE", "/api/v1/downloads/" + id.toLatin1() + "?files=delete");
    QVERIFY2(d.status == 200 || d.status == 202, d.body.constData());
    if (d.status == 202) {
        QCOMPARE(d.json().value(QStringLiteral("pending")).toBool(), true);
        QVERIFY(QTest::qWaitFor([&] {
            d = env.api("GET", "/api/v1/downloads/" + id.toLatin1());
            return d.status == 410 && !d.json().value(QStringLiteral("pending")).toBool(true);
        }, 30000));
    }
    const QJsonObject o = d.json();
    QVERIFY(o.value(QStringLiteral("kept")).toArray().contains(QDir::toNativeSeparators(QFileInfo(sw.target(0)).absoluteFilePath())));
    QVERIFY(o.value(QStringLiteral("deleted")).toArray().contains(QDir::toNativeSeparators(QFileInfo(sw.target(1)).absoluteFilePath())));
    QVERIFY2(o.value(QStringLiteral("failed")).toArray().isEmpty(), QJsonDocument(o).toJson().constData());
    QCOMPARE(readFile(sw.target(0)), f0);
    QVERIFY(!QFileInfo::exists(sw.target(1)));
}

QTEST_MAIN(TstApiDownloadsBt)
#include "tst_api_downloads_bt.moc"
