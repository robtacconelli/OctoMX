// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX: BitTorrent plugin: seed/download, pause/resume/remove, wanted files, URL add.
#include <QtCore/QElapsedTimer>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

#include "bt_test_util.h"

using namespace bttest;

class TstBtTransfer : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { mx::registerMetaTypes(); }
    void seedLocalFileAndDownload();
    void multiFileWantedPauseResumeRemove();
    void addFromHttpUrl();
    void liveReconfigureAndServers();
};

void TstBtTransfer::seedLocalFileAndDownload()
{
    const QString root = freshDir(QStringLiteral("transfer1"));
    const QString src = root + QStringLiteral("/share/octomx-test-32M.bin");
    makeFile(src, 32 * 1024 * 1024 + 12345, 1);

    Node a(root + QStringLiteral("/a")), b(root + QStringLiteral("/b"), /*limitLan*/ true);
    a.create();
    b.create();
    QVERIFY(a.start());
    QVERIFY(b.start());
    QCOMPARE(a.plugin->net(), mx::Net::BitTorrent);
    QVERIFY(a.plugin->capabilities() & mx::NetworkPlugin::CanServe);

    QString err;
    QVERIFY(!a.be->seedLocalFile(root + QStringLiteral("/nope.bin"), &err));
    QVERIFY(!err.isEmpty());
    int progressEvents = 0;
    connect(a.ev, &mx::BitTorrentEvents::seedProgress, this, [&](const QString &, qint64 d, qint64 t) {
        ++progressEvents;
        QVERIFY(d <= t);
    });
    QVERIFY2(a.be->seedLocalFile(src, &err), qPrintable(err));
    QVERIFY(a.be->seedLocalFile(src, &err));   // in flight → no second hash job
    QVERIFY(QTest::qWaitFor([&] { return !a.seeded.isEmpty(); }, 60000));
    const QString id = a.seeded.begin().value();
    QCOMPARE(id.size(), 40);
    QVERIFY(progressEvents >= 1);
    QVERIFY(QTest::qWaitFor([&] { return a.snap(id).finished; }, 20000));
    QVERIFY2(QTest::qWaitFor([&] { return a.snap(id).state == mx::TransferState::Seeding; }, 5000), qPrintable(a.snap(id).statusText + QString::number(int(a.snap(id).state))));
    QVERIFY(QFileInfo::exists(a.profile + QStringLiteral("/bt/torrents/%1.torrent").arg(id)));
    // idempotent for an already-seeded path
    a.seeded.clear();
    QVERIFY(a.be->seedLocalFile(src, &err));
    QVERIFY(QTest::qWaitFor([&] { return a.seeded.value(QFileInfo(src).canonicalFilePath()) == id; }, 5000));
    QCOMPARE(a.be->torrents().size(), 1);

    // b fetches metadata from a (ut_metadata) through a bare magnet, then the data
    const QString dl = root + QStringLiteral("/b-dl");
    b.ctl->setRateLimits(8 * 1024 * 1024, 0);
    const QString bid = b.be->addTorrent(QStringLiteral("magnet:?xt=urn:btih:%1&dn=x").arg(id), {}, dl, false, {}, &err);
    QCOMPARE(bid, id);
    QCOMPARE(b.be->addTorrent(QStringLiteral("magnet:?xt=urn:btih:%1").arg(id), {}, dl, false, {}, &err), id); // duplicate
    QCOMPARE(b.be->torrents(), QStringList{id});
    QVERIFY(b.be->pieceHash(id, 0).isEmpty());   // no metadata yet
    b.ctl->addPeer(id, a.endpoint());
    QVERIFY(QTest::qWaitFor([&] { return b.metadata.contains(id); }, 20000));
    const mx::TorrentSnapshot s0 = b.snap(id);
    QVERIFY(s0.hasMetadata);
    QCOMPARE(s0.files.size(), 1);
    QCOMPARE(s0.files.first().size, QFileInfo(src).size());
    QCOMPARE(b.be->pieceHash(id, 0), a.be->pieceHash(id, 0));
    QCOMPARE(b.be->pieceHash(id, 0).size(), 20);
    QCOMPARE(b.be->pieceHash(id, 0), QCryptographicHash::hash(readRange(src, 0, s0.pieceLength), QCryptographicHash::Sha1));
    QVERIFY(b.be->pieceHash(id, s0.numPieces).isEmpty());

    QList<qint64> changedAt;
    QElapsedTimer clock;
    clock.start();
    connect(b.ev, &mx::TorrentEvents::torrentChanged, this, [&](const QString &i) {
        if (i == id)
            changedAt << clock.elapsed();
    });
    bool sawTransfer = false;
    QVERIFY(QTest::qWaitFor([&] {
        const mx::TorrentSnapshot s = b.snap(id);
        if (s.state == mx::TransferState::Transferring && !s.sources.isEmpty())
            sawTransfer = true;
        return b.finished.contains(id);
    }, 120000));
    const mx::TorrentSnapshot s = b.snap(id);
    QVERIFY(s.finished);
    QCOMPARE(s.totalDone, s.totalSize);
    QCOMPARE(s.have.count(true), s.numPieces);
    QCOMPARE(QSet<int>(b.done[id].cbegin(), b.done[id].cend()).size(), s.numPieces);
    QVERIFY(b.failed[id].isEmpty());
    QCOMPARE(sha1File(dl + QStringLiteral("/octomx-test-32M.bin")), sha1File(src));
    qInfo("download ok: %d pieces of %d bytes, sawTransfer=%d", s.numPieces, s.pieceLength, int(sawTransfer));
    QVERIFY(changedAt.size() >= 2);
    for (int i = 1; i < changedAt.size(); ++i)
        QVERIFY2(changedAt[i] - changedAt[i - 1] >= 950, "torrentChanged more often than 1/s");
    qInfo("torrentChanged %d times during the download (>= 950 ms apart)", int(changedAt.size()));

    // a reported b as an upload (peer + bytes)
    QVERIFY(QTest::qWaitFor([&] { return !a.uploads.isEmpty(); }, 5000));
    const mx::TransferInfo up = a.uploads.begin().value();
    QVERIFY(up.isUpload);
    QCOMPARE(up.peer.net, mx::Net::BitTorrent);
    QVERIFY(up.peer.address.startsWith(QLatin1String("127.0.0.1:")));
    QVERIFY(up.done > 0);
    qInfo("upload row: %s peer=%s client=%s bytes=%lld", qPrintable(up.id), qPrintable(up.peer.address),
          qPrintable(up.peer.name), up.done);
    // [core-5] the row ends with the real byte count and a finished state, not "Complete 0 of ...":
    // "Complete" when b is known to have finished, "Disconnected" when it left before our last
    // look (once a second) could tell (two seeds drop their link at once on a fast LAN)
    const auto ended = [&] {
        const mx::TransferState st = a.uploads.value(up.id).state;
        return st == mx::TransferState::Completed || st == mx::TransferState::Cancelled;
    };
    QVERIFY2(QTest::qWaitFor(ended, 40000), qPrintable(a.uploads.value(up.id).statusText));
    const mx::TransferInfo fin = a.uploads.value(up.id);
    QVERIFY2(fin.statusText == QStringLiteral("Complete") || fin.statusText == QStringLiteral("Disconnected"), qPrintable(fin.statusText));
    qInfo("upload row ended: %s, %lld of %lld bytes", qPrintable(fin.statusText), fin.done, s.totalSize);
    // the bytes of the last look before b left (refreshed once a second): most of the file
    QVERIFY2(fin.done >= s.totalSize / 2, qPrintable(QStringLiteral("%1 of %2").arg(fin.done).arg(s.totalSize)));
    QVERIFY(fin.done >= up.done);
    QCOMPARE(fin.rateUpBps, 0);

    // status
    const mx::NetworkStatus st = b.plugin->status();
    QCOMPARE(st.net, mx::Net::BitTorrent);
    QCOMPARE(st.state, mx::ConnState::Connected);
    QVERIFY(st.listenPort > 0);
    QVERIFY(st.statusText.contains(QLatin1String("1 torrent")) && !st.statusText.contains(QLatin1String("1 torrents")));   // [core-5] singular for one
}

void TstBtTransfer::multiFileWantedPauseResumeRemove()
{
    const QString root = freshDir(QStringLiteral("transfer2"));
    const QString bundle = root + QStringLiteral("/share/Bundle");
    makeFile(bundle + QStringLiteral("/one.bin"), 5 * 1024 * 1024 + 7, 11);
    makeFile(bundle + QStringLiteral("/sub/two.bin"), 3 * 1024 * 1024 + 3, 12);
    makeFile(bundle + QStringLiteral("/three.bin"), 7 * 1024 * 1024 + 1, 13);
    const QByteArray tor = makeTorrent(bundle, 256 * 1024);
    const QString id = infoHashOf(tor);
    QVERIFY(!id.isEmpty());

    Node a(root + QStringLiteral("/a")), b(root + QStringLiteral("/b"), /*limitLan*/ true);
    a.create();
    b.create();
    QVERIFY(a.start());
    QVERIFY(b.start());
    QString err;
    // seeder: existing data is found by the initial check
    QCOMPARE(a.be->addTorrent(QString(), tor, root + QStringLiteral("/share"), false, {}, &err), id);
    QVERIFY(QTest::qWaitFor([&] { return a.snap(id).finished; }, 30000));

    // downloader: only one.bin and three.bin, start paused
    const QString dl = root + QStringLiteral("/b-dl");
    // resolve file indexes by name (add_files order is OS dependent)
    lt::error_code ec;
    lt::torrent_info ti(lt::span<char const>(tor.constData(), tor.size()), ec, lt::from_span);
    int iOne = -1, iTwo = -1, iThree = -1;
    for (int i = 0; i < ti.num_files(); ++i) {
        const QString p = QString::fromStdString(ti.files().file_path(lt::file_index_t(i)));
        if (p.endsWith(QLatin1String("one.bin")))
            iOne = i;
        else if (p.endsWith(QLatin1String("two.bin")))
            iTwo = i;
        else if (p.endsWith(QLatin1String("three.bin")))
            iThree = i;
    }
    QVERIFY(iOne >= 0 && iTwo >= 0 && iThree >= 0);
    QCOMPARE(b.be->addTorrent(QString(), tor, dl, true, {iOne, iThree}, &err), id);
    {
        const mx::TorrentSnapshot s = b.snap(id);
        QVERIFY(s.paused);
        QCOMPARE(s.state, mx::TransferState::Paused);
        QCOMPARE(s.files.size(), 3);
        QCOMPARE(s.files[iTwo].priority, 0);
        QVERIFY(s.files[iOne].priority > 0);
        QCOMPARE(s.totalSize, s.files[iOne].size + s.files[iThree].size);
    }
    QVERIFY(QTest::qWaitFor([&] { return b.added.contains(id); }, 5000));
    b.ctl->addPeer(id, a.endpoint());
    QTest::qWait(2500);
    QCOMPARE(b.snap(id).totalDone, 0);   // paused → nothing moves
    QVERIFY(b.snap(id).paused);

    // resume slowly, pause in the middle, resume again
    b.ctl->setRateLimits(1500 * 1024, 0);
    b.be->resumeTorrent(id);
    b.ctl->addPeer(id, a.endpoint());
    QVERIFY(QTest::qWaitFor([&] { return b.snap(id).totalDone > 2 * 1024 * 1024; }, 30000));
    QVERIFY(!b.snap(id).finished);
    b.be->pauseTorrent(id);
    QVERIFY(QTest::qWaitFor([&] { return b.snap(id).paused && b.snap(id).rateDownBps == 0; }, 5000));
    QTest::qWait(1500);
    const qint64 atPause = b.snap(id).totalDone;
    QTest::qWait(2000);
    QCOMPARE(b.snap(id).totalDone, atPause);
    QCOMPARE(b.snap(id).state, mx::TransferState::Paused);
    b.ctl->setRateLimits(0, 0);
    b.be->resumeTorrent(id);
    b.ctl->addPeer(id, a.endpoint());
    QVERIFY(QTest::qWaitFor([&] { return b.finished.contains(id); }, 60000));
    QCOMPARE(sha1File(dl + QStringLiteral("/Bundle/one.bin")), sha1File(bundle + QStringLiteral("/one.bin")));
    QCOMPARE(sha1File(dl + QStringLiteral("/Bundle/three.bin")), sha1File(bundle + QStringLiteral("/three.bin")));
    QVERIFY(sha1File(dl + QStringLiteral("/Bundle/sub/two.bin")) != sha1File(bundle + QStringLiteral("/sub/two.bin")));
    QVERIFY(b.snap(id).finished);
    QVERIFY(b.snap(id).files[iTwo].done < b.snap(id).files[iTwo].size);

    // now want everything → unfinished again, then finished with all three files
    b.finished.clear();
    b.be->setWantedFiles(id, {iOne, iTwo, iThree});
    {
        const bool ok = QTest::qWaitFor([&] { return !b.snap(id).finished || b.finished.contains(id); }, 10000);
        const mx::TorrentSnapshot s = b.snap(id);
        QStringList fp;
        for (const mx::TorrentFile &f : s.files)
            fp << QStringLiteral("%1:%2/%3").arg(f.priority).arg(f.done).arg(f.size);
        QVERIFY2(ok, qPrintable(QStringLiteral("state=%1 '%2' err='%3' files %4").arg(int(s.state)).arg(s.statusText, s.errorText, fp.join(QLatin1Char(' ')))));
    }
    b.ctl->addPeer(id, a.endpoint());
    const bool fin2 = QTest::qWaitFor([&] { return b.finished.contains(id); }, 30000);
    if (!fin2) {
        const mx::TorrentSnapshot s = b.snap(id);
        qWarning("b: state=%d '%s' done=%lld/%lld peers=%d seeds=%d srcs=%d", int(s.state), qPrintable(s.statusText),
                 s.totalDone, s.totalSize, s.peers, s.seeds, int(s.sources.size()));
        const mx::TorrentSnapshot sa = a.snap(id);
        qWarning("a: state=%d '%s' peers=%d", int(sa.state), qPrintable(sa.statusText), sa.peers);
    }
    QVERIFY(fin2);
    QCOMPARE(sha1File(dl + QStringLiteral("/Bundle/sub/two.bin")), sha1File(bundle + QStringLiteral("/sub/two.bin")));
    QCOMPARE(b.snap(id).totalSize, ti.total_size());

    // priority + recheck + reannounce must not break anything
    b.be->setTorrentPriority(id, 1);
    b.be->reannounce(id);
    b.be->recheck(id);
    QVERIFY(QTest::qWaitFor([&] { return b.snap(id).finished && b.snap(id).state != mx::TransferState::Verifying; }, 30000));
    QCOMPARE(b.snap(id).have.count(true), ti.num_pieces());

    // remove with files
    b.be->removeTorrent(id, true);
    QVERIFY(b.be->torrents().isEmpty());
    QVERIFY(QTest::qWaitFor([&] { return b.removed.contains(id); }, 10000));
    QVERIFY(QTest::qWaitFor([&] { return !QFileInfo::exists(dl + QStringLiteral("/Bundle/one.bin")); }, 10000));
    QVERIFY(!QFileInfo::exists(b.profile + QStringLiteral("/bt/torrents/%1.fastresume").arg(id)));
    QVERIFY(!QFileInfo::exists(b.profile + QStringLiteral("/bt/torrents/%1.json").arg(id)));
    // remove without files keeps the data
    a.be->removeTorrent(id, false);
    QVERIFY(QTest::qWaitFor([&] { return a.removed.contains(id); }, 10000));
    QVERIFY(QFileInfo::exists(bundle + QStringLiteral("/one.bin")));
}

// minimal one-shot HTTP server for the .torrent
class TinyHttp : public QTcpServer {
public:
    QByteArray body;
    TinyHttp()
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *s = nextPendingConnection()) {
                connect(s, &QTcpSocket::readyRead, s, [this, s] {
                    const QByteArray req = s->readAll();
                    if (!req.contains("\r\n\r\n"))
                        return;
                    if (req.startsWith("GET /none")) {
                        s->write("HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
                        s->disconnectFromHost();
                        return;
                    }
                    s->write("HTTP/1.1 200 OK\r\nContent-Type: application/x-bittorrent\r\nConnection: close\r\nContent-Length: "
                             + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                    s->disconnectFromHost();
                });
                connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
            }
        });
    }
};

void TstBtTransfer::addFromHttpUrl()
{
    const QString root = freshDir(QStringLiteral("transfer3"));
    makeFile(root + QStringLiteral("/share/small.bin"), 600 * 1024, 21);
    TinyHttp http;
    http.body = makeTorrent(root + QStringLiteral("/share/small.bin"), 64 * 1024);
    QVERIFY(http.listen(QHostAddress::LocalHost));
    const QString id = infoHashOf(http.body);
    Node b(root + QStringLiteral("/b"));
    b.create();
    QVERIFY(b.start());
    QString added, failed;
    connect(b.ev, &mx::BitTorrentEvents::urlTorrentAdded, this, [&](const QString &, const QString &i) { added = i; });
    connect(b.ev, &mx::BitTorrentEvents::urlTorrentFailed, this, [&](const QString &, const QString &e) { failed = e; });
    QString err = QStringLiteral("x");
    const QString url = QStringLiteral("http://127.0.0.1:%1/file.torrent").arg(http.serverPort());
    QVERIFY(b.be->addTorrent(url, {}, root + QStringLiteral("/dl"), true, {}, &err).isEmpty());
    QVERIFY(err.isEmpty());   // pending
    QVERIFY(QTest::qWaitFor([&] { return !added.isEmpty() || !failed.isEmpty(); }, 15000));
    QVERIFY2(failed.isEmpty(), qPrintable(failed));
    QCOMPARE(added, id);
    QVERIFY(b.be->snapshot(id).hasMetadata);
    QVERIFY(b.be->snapshot(id).paused);
    // bad URL fails cleanly
    added.clear();
    QVERIFY(b.be->addTorrent(QStringLiteral("http://127.0.0.1:%1/none.torrent").arg(http.serverPort()), {}, QString(), false, {}, &err).isEmpty());
    QVERIFY(QTest::qWaitFor([&] { return !failed.isEmpty(); }, 15000));
    // garbage bytes fail synchronously
    QVERIFY(b.be->addTorrent(QString(), QByteArray("not a torrent"), QString(), false, {}, &err).isEmpty());
    QVERIFY(!err.isEmpty());
    // local .torrent path
    const QString tf = root + QStringLiteral("/x.torrent");
    {
        QFile f(tf);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(http.body);
    }
    QCOMPARE(b.be->addTorrent(tf, {}, QString(), true, {}, &err), id);   // duplicate by path
}

void TstBtTransfer::liveReconfigureAndServers()
{
    const QString root = freshDir(QStringLiteral("transfer4"));
    const QString profile = root + QStringLiteral("/n");
    {
        Node n(profile);
        n.create();
        QVERIFY(n.start());
        QCOMPARE(n.plugin->status().state, mx::ConnState::Connected);   // DHT off: listening is enough
        // live re-configuration: a new listen port is applied without restarting the plugin
        QTcpServer probe;
        QVERIFY(probe.listen(QHostAddress::LocalHost));
        const int port = probe.serverPort();
        probe.close();
        n.settings.setValue(QString(mx::keys::bt::ListenPort), port);
        n.settings.setValue(QString(mx::keys::bt::MaxConnections), 50);
        QVERIFY2(QTest::qWaitFor([&] { return n.plugin->status().listenPort == port; }, 10000),
                 qPrintable(QString::number(n.plugin->status().listenPort)));
        // servers: bootstrap routers from the settings + user-added nodes/trackers (persisted)
        const QList<mx::ServerEntry> before = n.plugin->servers();
        QVERIFY(!before.isEmpty());
        n.plugin->addServer(QStringLiteral("192.0.2.7:6881"), QString());
        n.plugin->addServer(QStringLiteral("udp://tracker.example.invalid:1337/announce"), QString());
        QCOMPARE(n.plugin->servers().size(), before.size() + 2);
        n.plugin->stop();
        QCOMPARE(n.plugin->status().state, mx::ConnState::Disconnected);
    }
    Node m(profile);
    m.create();
    QStringList ids;
    for (const mx::ServerEntry &e : m.plugin->servers())
        ids << e.id;
    QVERIFY(ids.contains(QStringLiteral("192.0.2.7:6881")));
    QVERIFY(ids.contains(QStringLiteral("udp://tracker.example.invalid:1337/announce")));
    m.plugin->removeServer(QStringLiteral("192.0.2.7:6881"));
    QVERIFY(!QFile(profile + QStringLiteral("/bt/nodes.json")).open(QIODevice::ReadOnly)
            || !readRange(profile + QStringLiteral("/bt/nodes.json"), 0, 4096).contains("192.0.2.7"));
}

QTEST_GUILESS_MAIN(TstBtTransfer)
#include "tst_bt_transfer.moc"
