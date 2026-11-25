// SPDX-License-Identifier: GPL-3.0-or-later
// [str-1] Streaming over BitTorrent with the real libtorrent, offline on 127.0.0.1: node A seeds a
// torrent of two files (throttled), the core (node B, with its own libtorrent) streams the SECOND
// file through the API: the head, the end, sequential, seeks forward and back. Every byte equals
// the original; libtorrent fetched only the pieces the player read plus the read-ahead (the other
// file not at all); the time to the first byte and after a seek for both libtorrent mechanisms
// (piece deadlines, mode 0, and piece priorities only, mode 1); stop: the torrent leaves the
// session and its cache folder goes, nothing of it is kept for the next start.
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

// the files of the folder `root` in the order given: lt::add_files takes the order the file system
// lists them in, which is not the same on every system (and the test names a file by its index)
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
    // no adapter choice: the connection policy would restart the session once it has resolved it (the
    // core's own node; nothing is announced anywhere: no DHT, LSD, trackers, port mapping)
    s->setValue(QString(k::bt::ListenPort), 0);
    s->setValue(QString(k::bt::ExtraTrackers), QStringList());
    s->setValue(QString(k::bt::SeedRatioLimit), 0.0);
    s->setValue(QStringLiteral("bt/allowMultipleConnectionsPerIp"), true);
}

} // namespace

class TstStreamBt : public QObject {
    Q_OBJECT
private slots:
    void secondFileOfABundle_data();
    void secondFileOfABundle();
    void mergedRowWithoutFileList();
};

void TstStreamBt::secondFileOfABundle_data()
{
    QTest::addColumn<int>("mode");
    QTest::addColumn<bool>("late");
    QTest::newRow("piece deadlines") << 0 << false;
    QTest::newRow("piece priorities") << 1 << false;
    // libtorrent verifies a piece from its memory and writes it to the file afterwards (on a slow
    // machine much later): a player must not get the bytes of the file before they are in it
    QTest::newRow("written late") << 0 << true;
}

void TstStreamBt::secondFileOfABundle()
{
    QFETCH(int, mode);
    QFETCH(bool, late);
    qputenv("MX_STREAM_BT_MODE", QByteArray::number(mode));
    // late: the files are looked at 600 ms after a piece is verified, and the first 3 looks find
    // them without it
    struct Seams {
        explicit Seams(bool on)
        {
            if (!on) return;
            qputenv("MX_BT_ONDISK_TEST_DELAY_MS", "600");
            qputenv("MX_BT_ONDISK_TEST_MISSES", "3");
        }
        ~Seams()
        {
            qunsetenv("MX_BT_ONDISK_TEST_DELAY_MS");
            qunsetenv("MX_BT_ONDISK_TEST_MISSES");
        }
    } seams(late);
    StreamEnv env(QStringLiteral("bt%1%2").arg(mode).arg(late ? QStringLiteral("late") : QString()));
    // node A: a folder torrent of two files, seeded at 3 MB/s
    const QByteArray f0 = randomBytes(6 * MiB + 1111, 300), f1 = randomBytes(40 * MiB + 2222, 301);
    const QString folder = env.root + QStringLiteral("/seed/Big Buck Bunny pack");
    writeFile(folder + QStringLiteral("/a-trailer.mkv"), f0);
    writeFile(folder + QStringLiteral("/b-feature.mp4"), f1);
    const QByteArray torrent = makeTorrent(folder, 256 * 1024, {QStringLiteral("a-trailer.mkv"), QStringLiteral("b-feature.mp4")});
    etest::BtNode a(env.root + QStringLiteral("/nodeA"), /*limitLan*/ true);
    QVERIFY(a.start());
    a.ctl->setRateLimits(0, 3 * 1000 * 1000);
    QString err;
    const QString tid = a.be->addTorrent(QString(), torrent, env.root + QStringLiteral("/seed"), false, {}, &err);
    QVERIFY2(!tid.isEmpty(), qPrintable(err));
    QVERIFY(QTest::qWaitFor([&] { return a.be->snapshot(tid).finished; }, 60000));
    {
        // the feature is the torrent's second file and begins in the middle of a piece
        const TorrentSnapshot seeded = a.be->snapshot(tid);
        QCOMPARE(seeded.files.size(), 2);
        QVERIFY2(seeded.files.at(1).path.endsWith(QLatin1String("b-feature.mp4")), qPrintable(seeded.files.at(1).path));
        QCOMPARE(seeded.files.at(1).size, qint64(f1.size()));
        QCOMPARE(seeded.files.at(1).offset, qint64(f0.size()));
    }
    // the core with its own libtorrent and a search network that finds the torrent
    TestPlugin *search = env.addPlugin(Net::Web, 0);
    SearchHit hit;
    hit.fileName = QStringLiteral("Big Buck Bunny pack");
    hit.size = f0.size() + f1.size();
    hit.isBundle = true;
    hit.hashes.btih = QByteArray::fromHex(tid.toLatin1());
    SearchSource src;
    src.net = Net::BitTorrent;
    src.uri = QStringLiteral("magnet:?xt=urn:btih:%1&dn=Big+Buck+Bunny+pack").arg(tid);
    hit.sources << src;
    hit.files = {BundleFile{QStringLiteral("Big Buck Bunny pack/a-trailer.mkv"), f0.size(), 0},
                 BundleFile{QStringLiteral("Big Buck Bunny pack/b-feature.mp4"), f1.size(), 1}};
    search->cannedHits << hit;
    NetworkPlugin *bt = createBitTorrentPlugin();
    auto *btBe = dynamic_cast<TorrentBackend *>(bt);
    auto *btCtl = dynamic_cast<BitTorrentControl *>(bt);
    env.configure = [](Settings *s) { localBt(s); };
    env.up({bt});
    {
        // the seed is found again after the connection policy restarted the session, which can take
        // longer than the 8 s the other tests give a player without a source
        stream::StreamService::Tuning st = env.core->streamService()->tuning();
        st.stallNoSourceMs = 60000;
        env.core->streamService()->setTuning(st);
    }
    QVERIFY(QTest::qWaitFor([&] { return bt->status().listenPort > 0; }, 10000));
    int status = 0;
    const QJsonObject so = env.streamOf(QStringLiteral("bunny"), QStringLiteral("Big Buck Bunny pack"), 1, false, &status);
    QCOMPARE(status, 201);
    QVERIFY(QTest::qWaitFor([&] { return btBe->torrents().contains(tid); }, 10000));
    // the seed, told again until the session is settled (the connection policy may restart it once
    // after the start)
    QTimer peerHint;
    peerHint.setInterval(1000);
    QObject::connect(&peerHint, &QTimer::timeout, [&] {
        if (btBe->snapshot(tid).peers + btBe->snapshot(tid).seeds == 0) btCtl->addPeer(tid, a.endpoint());
    });
    btCtl->addPeer(tid, a.endpoint());
    peerHint.start();
    // the torrent's folder is the stream's cache
    QVERIFY(QTest::qWaitFor([&] { return !btBe->snapshot(tid).savePath.isEmpty(); }, 10000));
    QVERIFY2(QDir::cleanPath(btBe->snapshot(tid).savePath).startsWith(env.root + QStringLiteral("/incomplete/Streams/")),
             qPrintable(btBe->snapshot(tid).savePath));
    const QByteArray path = StreamEnv::pathOf(so);
    const qint64 size = f1.size();
    struct R { qint64 from, to; const char *what; };
    const QList<R> plan{{0, 256 * 1024, "head"}, {size - MiB, size, "end"}, {256 * 1024, 6 * MiB, "sequential"},
                        {28 * MiB, 30 * MiB, "seek +"}, {3 * MiB, 4 * MiB, "seek -"}, {16 * MiB, 17 * MiB, "seek +"}};
    QStringList times;
    engine::RangeSet read;
    for (const R &r : plan) {
        Player p(env.port, path, r.from, r.to);
        QVERIFY2(p.waitDone(120000), r.what);
        QCOMPARE(p.reply().status, 206);
        QVERIFY2(p.data() == f1.mid(int(r.from), int(r.to - r.from)), r.what);
        times << QStringLiteral("%1 %2 ms").arg(QLatin1String(r.what)).arg(p.firstByteMs());
        read.add(r.from, r.to);
    }
    // seeks to places nothing was fetched of: the time to the first byte, 8 times
    QList<qint64> seeks;
    for (qint64 at : {33 * MiB, 10 * MiB, 24 * MiB, 36 * MiB + 77, 8 * MiB + 5, 21 * MiB, 13 * MiB, 38 * MiB}) {
        Player p(env.port, path, at, at + 256 * 1024);
        QVERIFY(p.waitDone(60000));
        QVERIFY(p.data() == f1.mid(int(at), 256 * 1024));
        seeks << p.firstByteMs();
        read.add(at, at + 256 * 1024);
    }
    QList<qint64> sorted = seeks;
    std::sort(sorted.begin(), sorted.end());
    QStringList sl;
    for (qint64 v : seeks) sl << QString::number(v);
    qInfo("NUMBERS BitTorrent mode %d (%s%s): 8 seeks to fresh places, first byte %s ms (median %lld, most %lld)", mode,
          mode == 0 ? "piece deadlines" : "piece priorities", late ? ", pieces in their file 600 ms after they are verified" : "",
          qPrintable(sl.join(QStringLiteral(" / "))), sorted.at(4), sorted.last());
    // nothing was served before the files were looked at (and every byte was right, above)
    if (late) QVERIFY2(sorted.first() >= 600, qPrintable(sl.join(QStringLiteral(" / "))));
    QTest::qWait(1500);   // the snapshot is refreshed about once a second
    const TorrentSnapshot sn = btBe->snapshot(tid);
    qint64 doneOther = 0, doneMine = 0;
    for (const TorrentFile &f : sn.files) {
        if (f.path.endsWith(QLatin1String("a-trailer.mkv"))) doneOther = f.done;
        if (f.path.endsWith(QLatin1String("b-feature.mp4"))) doneMine = f.done;
    }
    qInfo("NUMBERS BitTorrent mode %d (%s), seed 3 MB/s, 256 KiB pieces: first byte %s; read %.1f MB of a %.1f MB file, libtorrent holds "
          "%.1f MB of it, of the other file %.1f MB; the seed sent %.1f MB",
          mode, mode == 0 ? "piece deadlines" : "piece priorities", qPrintable(times.join(QStringLiteral(", "))), read.total() / 1048576.0,
          size / 1048576.0, doneMine / 1048576.0, doneOther / 1048576.0, a.be->snapshot(tid).uploadedBytes / 1048576.0);
    // only the pieces read: the reads are bounded, what is left is piece rounding and the pieces
    // libtorrent had requested when a read ended
    QVERIFY2(doneMine <= read.total() + qint64(plan.size() + 8) * 2 * MiB, qPrintable(QString::number(doneMine)));
    QVERIFY(doneMine < size / 2);
    QVERIFY2(doneOther <= 256 * 1024, qPrintable(QString::number(doneOther)));   // at most the piece both files share
    const QJsonObject st = env.status(so.value(QStringLiteral("id")).toString());
    QCOMPARE(st.value(QStringLiteral("unverifiedServed")).toDouble(), 0.0);   // every piece checked before it is served
    // stop: out of the session, the cache folder gone, no record of it in the plugin's folder
    const QString cache = QFileInfo(btBe->snapshot(tid).savePath).absoluteFilePath();
    QVERIFY(QFileInfo::exists(cache));
    QCOMPARE(env.api("DELETE", "/api/v1/streams/" + so.value(QStringLiteral("id")).toString().toLatin1()).status, 200);
    QVERIFY(QTest::qWaitFor([&] { return !btBe->torrents().contains(tid); }, 10000));
    QVERIFY(QTest::qWaitFor([&] { return !QFileInfo::exists(cache); }, 15000));
    QVERIFY(!QFileInfo::exists(env.profile + QStringLiteral("/bt/torrents/") + tid + QStringLiteral(".json")));
    QVERIFY(!QFileInfo::exists(env.profile + QStringLiteral("/bt/torrents/") + tid + QStringLiteral(".fastresume")));
    qunsetenv("MX_STREAM_BT_MODE");
}

// one row, two networks: a torrent nobody answers for (no file list) and an eD2k source. The player is
// not turned away: its request waits, the status says for what, and after the stream's fall-back time
// the eD2k source serves it
void TstStreamBt::mergedRowWithoutFileList()
{
    StreamEnv env(QStringLiteral("btnometa"));
    const QByteArray data = randomBytes(12 * MiB + 99, 310);
    TestPlugin *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
    SourceConfig c;
    c.data = data;
    c.bytesPerSec = 3 * 1000 * 1000;
    c.hashSet = ed2kParts(data);
    SearchHit hit = hitOf(QStringLiteral("Big Buck Bunny merged.mkv"), data, hashesOf(data, 2),
                          {ed->addSource(QStringLiteral("test-ed2k://merged"), c, QStringLiteral("198.51.100.97:4662"))});
    hit.hashes.btih = QByteArray::fromHex("0123456789abcdef0123456789abcdef01234567");   // a torrent no peer has
    SearchSource bt;
    bt.net = Net::BitTorrent;
    bt.uri = QStringLiteral("magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567&dn=Big+Buck+Bunny+merged.mkv");
    hit.sources.prepend(bt);
    ed->cannedHits << hit;
    NetworkPlugin *btp = createBitTorrentPlugin();
    env.configure = [](Settings *s) { localBt(s); };
    env.up({btp});
    const QJsonObject so = env.streamOf(QStringLiteral("bunny"), QStringLiteral("Big Buck Bunny merged.mkv"));
    QVERIFY2(so.contains(QStringLiteral("url")), QJsonDocument(so).toJson().constData());
    const QString sid = so.value(QStringLiteral("id")).toString();
    Player p(env.port, StreamEnv::pathOf(so), 0, MiB);
    QString waiting;
    QVERIFY(QTest::qWaitFor([&] {
        waiting = env.status(sid).value(QStringLiteral("waitingFor")).toString();
        return !waiting.isEmpty() || p.firstByteMs() >= 0;
    }, 10000));
    QVERIFY(p.waitDone(60000));
    QCOMPARE(p.reply().status, 206);
    QCOMPARE(p.data(), data.left(MiB));
    qInfo("NUMBERS merged row, its torrent gives no file list: first byte after %lld ms (the stream's fall-back %d ms); "
          "meanwhile the status said: %s", p.firstByteMs(), env.core->engine()->tuning().streamMetadataFallbackMs, qPrintable(waiting));
    QVERIFY(p.firstByteMs() < env.core->engine()->tuning().streamMetadataFallbackMs + 15000);
}

QTEST_MAIN(TstStreamBt)
#include "tst_stream_bt.moc"
