// SPDX-License-Identifier: GPL-3.0-or-later
// [str-1] Streams read like a player reads: the head, the end (where mp4 / mkv keep their index),
// sequentially at a bit rate, seeks forward and back, two connections at once. Every byte equals
// the original; the sources served about what was read plus the read-ahead, not the file; time to
// first byte and after a seek against throttled sources. Each store and network mix (eD2k parts,
// tiger tree leaves, OctoMX, a file with only a whole-file checksum, a merged row), a source that
// disappears, no source at all, stop, idle time, the cache limit, "keep as a download", a quit in
// the middle, Pause / Cancel in the Transfers window, and a stream of a normal download.
#include <QtCore/QCryptographicHash>
#include <QtCore/QDirIterator>
#include <QtTest/QTest>

#include "core/dao.h"
#include "core/database.h"
#include "mxhash/tiger_tree.h"
#include "stream_support.h"

using namespace mx;
using namespace mx::stest;

namespace {

constexpr qint64 MiB = 1024 * 1024;

QList<QByteArray> tthLeaves(const QByteArray &data, qint64 unit)
{
    hash::TigerTree t;
    t.update(data);
    t.finish();
    return t.levelForUnit(unit);
}

QByteArray tthRoot(const QByteArray &data)
{
    hash::TigerTree t;
    t.update(data);
    return t.finish();
}

struct Read {
    qint64 from, to;
    qint64 rate = 0;
};

// several reads one after the other (a player that seeks): each checked; the time to the first
// byte of each
QList<qint64> readAll(StreamEnv &env, const QByteArray &path, const QByteArray &data, const QList<Read> &reads, engine::RangeSet *read)
{
    QList<qint64> ttfb;
    for (const Read &r : reads) {
        Player p(env.port, path, r.from, r.to, r.rate);
        if (!p.waitDone(120000)) {
            qWarning("read [%lld, %lld) did not finish (status %d, %lld bytes)", r.from, r.to, p.reply().status, qint64(p.data().size()));
            for (const TransferInfo &t : env.core->transfers()->downloads())
                qWarning("%s: %s", qPrintable(t.statusText), QJsonDocument(QJsonObject::fromVariantMap(env.core->engine()->debugInfo(t.id))).toJson(QJsonDocument::Compact).left(3000).constData());
            return {};
        }
        if (p.data() != data.mid(int(r.from), int(r.to - r.from))) {
            qWarning("read [%lld, %lld): wrong bytes", r.from, r.to);
            return {};
        }
        ttfb << p.firstByteMs();
        if (read) read->add(r.from, r.to);
    }
    return ttfb;
}

// a folder as it is on the disk now (files open in this process too)
bool copyTree(const QString &from, const QString &to)
{
    QDirIterator it(from, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    QDir().mkpath(to);
    while (it.hasNext()) {
        const QString f = it.next();
        const QString t = to + f.mid(from.size());
        QDir().mkpath(QFileInfo(t).absolutePath());
        QFile in(f), out(t);
        if (!in.open(QIODevice::ReadOnly) || !out.open(QIODevice::WriteOnly)) return false;
        while (!in.atEnd()) {
            const QByteArray b = in.read(4 << 20);
            if (out.write(b) != b.size()) return false;
        }
    }
    return true;
}

QString msList(const QList<qint64> &l)
{
    QStringList s;
    for (qint64 v : l) s << QString::number(v);
    return s.join(QStringLiteral(" / "));
}

SourceConfig tthConfig(const QByteArray &data, int bps)
{
    SourceConfig c;
    c.data = data;
    c.bytesPerSec = bps;
    c.hashSet = tthLeaves(data, 256 * 1024);
    c.hashSetKind = QStringLiteral("tth-leaves");
    c.hashSetUnit = 256 * 1024;
    return c;
}

SourceConfig ed2kConfig(const QByteArray &data, int bps)
{
    SourceConfig c;
    c.data = data;
    c.bytesPerSec = bps;
    c.hashSet = ed2kParts(data);
    return c;
}

} // namespace

class TstStreamPlay : public QObject {
    Q_OBJECT
private slots:
    void storesAndNetworks_data();
    void storesAndNetworks();
    void readAheadAtBitRate();
    void twoConnectionsAndSecondPlayer();
    void sourceDisappears();
    void noSourceAtAll();
    void stopDropsTheCache();
    void idleTimeAndCacheLimit();
    void keepAsDownload();
    void quitInTheMiddle();
    void pauseAndCancelInTransfers();
    void streamOfANormalDownload();
    void pausedStoppedAndBack();
    void idleDefaultIsFourHours();
    void notStarvedByDownloadsAtOnce();
    void verifyAndRestartNotForStreams();
    void crashInTheMiddle();
};

void TstStreamPlay::storesAndNetworks_data()
{
    QTest::addColumn<QString>("scheme");
    QTest::newRow("ed2k parts") << QStringLiteral("ed2k");
    QTest::newRow("tiger tree") << QStringLiteral("tth");
    QTest::newRow("octomx blocks") << QStringLiteral("octomx");
    QTest::newRow("whole file only") << QStringLiteral("whole");
    QTest::newRow("merged row") << QStringLiteral("merged");
}

// the head, the end, sequential, seeks forward and back: right bytes, little fetched beyond them
void TstStreamPlay::storesAndNetworks()
{
    QFETCH(QString, scheme);
    StreamEnv env(QStringLiteral("play-") + scheme);
    const qint64 size = 40 * MiB + 12345;
    const QByteArray data = randomBytes(size, 900 + quint32(qHash(scheme) % 50));
    FileHashes h;
    QList<QPair<TestPlugin *, QString>> srcs;
    QList<SearchSource> descs;
    const int rate = 3 * 1000 * 1000;   // bytes/s per source (throttled)
    auto add = [&](Net net, const QString &uri, SourceConfig c) {
        TestPlugin *p = nullptr;
        for (TestPlugin *x : std::as_const(env.pending))
            if (x->net() == net) p = x;
        if (!p) p = env.addPlugin(net, NetworkPlugin::CanFetchRanges);
        c.data = data;
        c.bytesPerSec = rate;
        c.maxPending = 2;
        descs << p->addSource(uri, c, QStringLiteral("198.51.100.%1:4662").arg(30 + descs.size()));
        srcs << qMakePair(p, uri);
        return p;
    };
    TestPlugin *first = nullptr;
    if (scheme == QLatin1String("ed2k") || scheme == QLatin1String("octomx")) {
        h = hashesOf(data, 2);
        const Net n = scheme == QLatin1String("ed2k") ? Net::ED2K : Net::Overlay;
        first = add(n, QStringLiteral("test-%1://a").arg(scheme), ed2kConfig(data, rate));
        add(n, QStringLiteral("test-%1://b").arg(scheme), ed2kConfig(data, rate));
    } else if (scheme == QLatin1String("tth")) {
        h.tth = tthRoot(data);
        h.sha1 = hashesOf(data, 1).sha1;
        first = add(Net::Gnutella, QStringLiteral("test-gnut://a"), tthConfig(data, rate));
        add(Net::Gnutella, QStringLiteral("test-gnut://b"), tthConfig(data, rate));
    } else if (scheme == QLatin1String("whole")) {
        h = hashesOf(data, 1);
        first = add(Net::Http, QStringLiteral("http://192.0.2.40/bunny.mkv"), SourceConfig());
        add(Net::Http, QStringLiteral("http://192.0.2.41/bunny.mkv"), SourceConfig());
    } else {   // one row, three networks: eD2k (parts), Gnutella, HTTP
        h = hashesOf(data, 3);
        first = add(Net::ED2K, QStringLiteral("test-ed2k://m"), ed2kConfig(data, rate));
        add(Net::Gnutella, QStringLiteral("test-gnut://m"), SourceConfig());
        add(Net::Http, QStringLiteral("http://192.0.2.42/bunny.mkv"), SourceConfig());
    }
    const QString name = QStringLiteral("Big Buck Bunny %1.mkv").arg(scheme);
    first->cannedHits << hitOf(name, data, h, descs);
    env.up();
    const QJsonObject so = env.streamOf(QStringLiteral("bunny"), name);
    QVERIFY2(so.contains(QStringLiteral("url")), QJsonDocument(so).toJson().constData());
    const QByteArray path = StreamEnv::pathOf(so);
    engine::RangeSet read;
    // head, the end (the index), sequential, a seek forward, back, forward again
    const QList<Read> plan{{0, 256 * 1024}, {size - 2 * MiB, size}, {256 * 1024, 8 * MiB}, {30 * MiB, 32 * MiB},
                           {4 * MiB, 5 * MiB}, {20 * MiB, 21 * MiB}};
    const QList<qint64> ttfb = readAll(env, path, data, plan, &read);
    QCOMPARE(ttfb.size(), plan.size());
    qint64 served = 0;
    QStringList perSource;
    for (const auto &s : std::as_const(srcs)) {
        served += s.first->served(s.second);
        perSource << QStringLiteral("%1 %2 KB").arg(s.second).arg(s.first->served(s.second) / 1024);
    }
    const QJsonObject st = env.status(so.value(QStringLiteral("id")).toString());
    qInfo("NUMBERS %s: size %.1f MB, read %.1f MB, fetched %.1f MB (%s); first byte %s ms (head, end, sequential, seek +, seek -, seek +); "
          "unverified served %.1f MB",
          qPrintable(scheme), size / 1048576.0, read.total() / 1048576.0, served / 1048576.0, qPrintable(perSource.join(QStringLiteral(", "))),
          qPrintable(msList(ttfb)), st.value(QStringLiteral("unverifiedServed")).toDouble() / 1048576.0);
    // only what was read plus the read-ahead, never the file: the reads are bounded ranges here, so
    // the read-ahead stops at their ends; what is left is the request granularity
    QVERIFY2(served <= read.total() + qint64(plan.size()) * 2 * MiB, qPrintable(QStringLiteral("fetched %1 for %2 read").arg(served).arg(read.total())));
    QVERIFY(served < size / 2);
    // the status says when bytes went out before a check could prove them
    const bool unverified = scheme != QLatin1String("tth");
    if (unverified) QVERIFY(st.value(QStringLiteral("unverifiedServed")).toDouble() > 0);
    else QCOMPARE(st.value(QStringLiteral("unverifiedServed")).toDouble(), 0.0);
    QVERIFY(!st.value(QStringLiteral("verification")).toString().isEmpty());
    if (scheme == QLatin1String("merged")) {   // more than one network of the row served
        int nets = 0;
        for (const auto &s : std::as_const(srcs)) nets += s.first->served(s.second) > 0 ? 1 : 0;
        QVERIFY2(nets >= 2, qPrintable(perSource.join(QStringLiteral(", "))));
    }
    // the cache: below <incomplete>/Streams, holding about what was fetched (sparse, not preallocated)
    engine::Download *d = env.download(so);
    QVERIFY(d);
    QVERIFY(d->streamRestricts());
    const QString part = env.root + QStringLiteral("/incomplete/Streams/") + so.value(QStringLiteral("download")).toString();
    QVERIFY(QFileInfo(part).isDir());
    QVERIFY(d->streamDiskBytes() < size / 2);
}

// the owner's point: a player reading sequentially at its bit rate never waits after the start
// when the sources are faster than that rate; the buffer ahead stays filled and bounded
void TstStreamPlay::readAheadAtBitRate()
{
    StreamEnv env(QStringLiteral("readahead"));
    const qint64 size = 48 * MiB;
    const QByteArray data = randomBytes(size, 77);
    FileHashes h;
    h.tth = tthRoot(data);
    TestPlugin *g = env.addPlugin(Net::Gnutella, NetworkPlugin::CanFetchRanges);
    QList<SearchSource> descs;
    for (int i = 0; i < 2; ++i)   // two sources: 1.4 MB/s together
        descs << g->addSource(QStringLiteral("test-gnut://ra%1").arg(i), tthConfig(data, 700 * 1000), QStringLiteral("198.51.100.%1:6346").arg(50 + i));
    const QString name = QStringLiteral("Big Buck Bunny 720p.mp4");
    g->cannedHits << hitOf(name, data, h, descs);
    env.up();
    const QJsonObject so = env.streamOf(QStringLiteral("bunny"), name);
    const QString sid = so.value(QStringLiteral("id")).toString();
    const qint64 bitrate = 400 * 1000;   // the player plays 400 KB/s (3.2 Mbit/s)
    auto servedNow = [&] {
        qint64 n = 0;
        for (int i = 0; i < 2; ++i) n += g->served(QStringLiteral("test-gnut://ra%1").arg(i));
        return n;
    };
    Player p(env.port, StreamEnv::pathOf(so), 0, 0 /* open: bytes=0- like mpv */, bitrate);
    QVERIFY(QTest::qWaitFor([&] { return p.firstByteMs() >= 0; }, 20000));
    const qint64 first = p.firstByteMs();
    QStringList aheadSec;
    qint64 minAhead = -1, maxOver = 0, maxInTransit = 0, maxTarget = 0;
    QElapsedTimer t;
    t.start();
    const qint64 target = bitrate * 30;   // the buffer: 30 s of playing
    while (t.elapsed() < 30000) {
        QTest::qWait(1000);
        const QJsonArray players = env.status(sid).value(QStringLiteral("players")).toArray();
        if (players.isEmpty()) continue;
        const QJsonObject pl = players.first().toObject();
        const qint64 ahead = qint64(pl.value(QStringLiteral("bufferedAhead")).toDouble());
        maxTarget = qMax(maxTarget, qint64(pl.value(QStringLiteral("bufferTarget")).toDouble()));
        aheadSec << QString::number(pl.value(QStringLiteral("bufferedAheadSec")).toDouble(), 'f', 1);
        if (t.elapsed() > 15000) minAhead = minAhead < 0 ? ahead : qMin(minAhead, ahead);   // filled after the start
        // beyond what was sent to the player (what it holds in its socket buffers is sent already)
        maxOver = qMax(maxOver, servedNow() - qint64(pl.value(QStringLiteral("sent")).toDouble()));
        maxInTransit = qMax(maxInTransit, qint64(pl.value(QStringLiteral("sent")).toDouble()) - qint64(p.data().size()));
    }
    const qint64 readBytes = p.data().size();
    QCOMPARE(p.data(), data.left(int(readBytes)));
    qInfo("NUMBERS read-ahead: bit rate 400 KB/s, sources 2 x 700 KB/s; first byte %lld ms; longest wait after it %lld ms; "
          "read %.1f MB in 30 s; buffered ahead (s): %s; least after 15 s %.1f MB; most fetched beyond what was sent %.1f MB (largest buffer target %.1f MB); "
          "sent but not read yet by the player (socket buffers) at most %.1f MB",
          first, p.longestWaitMs(), readBytes / 1048576.0, qPrintable(aheadSec.join(QLatin1Char(' '))), minAhead / 1048576.0,
          maxOver / 1048576.0, maxTarget / 1048576.0, maxInTransit / 1048576.0);
    QVERIFY2(p.longestWaitMs() < 300, qPrintable(QString::number(p.longestWaitMs())));
    QVERIFY2(minAhead >= 8 * MiB, qPrintable(QString::number(minAhead)));        // most of 30 s x 400 KB/s (12 MB) stays filled
    // never more than the buffer (30 s at the measured rate, bounded) and a request or two
    QVERIFY2(maxOver <= maxTarget + 2 * MiB, qPrintable(QString::number(maxOver)));
    QVERIFY(maxTarget <= 64 * MiB);
    // a seek: the buffer moves at once to the new place
    p.stop();
    const qint64 servedAtSeek = servedNow();
    Player q(env.port, StreamEnv::pathOf(so), 30 * MiB, 0, bitrate);
    QVERIFY(QTest::qWaitFor([&] { return q.firstByteMs() >= 0; }, 20000));
    QTest::qWait(8000);
    QVERIFY(q.data() == data.mid(30 * MiB, q.data().size()));
    qInfo("NUMBERS read-ahead seek to 30 MB: first byte %lld ms, longest wait after it %lld ms, fetched since the seek %.1f MB for %.1f MB read",
          q.firstByteMs(), q.longestWaitMs(), (servedNow() - servedAtSeek) / 1048576.0, q.data().size() / 1048576.0);
    QVERIFY(q.longestWaitMs() < 300);
    const QJsonArray qp = env.status(sid).value(QStringLiteral("players")).toArray();
    const qint64 sentSinceSeek = qp.isEmpty() ? q.data().size() : qint64(qp.first().toObject().value(QStringLiteral("sent")).toDouble());
    QVERIFY(servedNow() - servedAtSeek <= sentSinceSeek + target + 3 * MiB);
    q.stop();
}

// mpv / ffmpeg open a second connection (the index at the end) while the first one reads; another
// player opens the same link; a player that closes and reopens
void TstStreamPlay::twoConnectionsAndSecondPlayer()
{
    StreamEnv env(QStringLiteral("twoconn"));
    const qint64 size = 24 * MiB;
    const QByteArray data = randomBytes(size, 78);
    TestPlugin *e = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
    const QString name = QStringLiteral("Big Buck Bunny 2.mkv");
    e->cannedHits << hitOf(name, data, hashesOf(data, 2),
                           {e->addSource(QStringLiteral("test-ed2k://two"), ed2kConfig(data, 3 * 1000 * 1000), QStringLiteral("198.51.100.60:4662"))});
    env.up();
    const QJsonObject so = env.streamOf(QStringLiteral("bunny"), name);
    const QByteArray path = StreamEnv::pathOf(so);
    Player a(env.port, path, 0, 6 * MiB);
    Player b(env.port, path, size - MiB, size);
    Player c2(env.port, path, 12 * MiB, 14 * MiB);
    QVERIFY(a.waitDone(60000));
    QVERIFY(b.waitDone(60000));
    QVERIFY(c2.waitDone(60000));
    QCOMPARE(a.data(), data.left(6 * MiB));
    QCOMPARE(b.data(), data.right(MiB));
    QCOMPARE(c2.data(), data.mid(12 * MiB, 2 * MiB));
    Player r(env.port, path, 3 * MiB, 7 * MiB);
    QVERIFY(r.waitDone(60000));
    QCOMPARE(r.data(), data.mid(3 * MiB, 4 * MiB));
    qInfo("NUMBERS three connections at once: first bytes %lld / %lld / %lld ms; fetched %.1f MB for 10 MB read",
          a.firstByteMs(), b.firstByteMs(), c2.firstByteMs(), e->served(QStringLiteral("test-ed2k://two")) / 1048576.0);
    QVERIFY(e->served(QStringLiteral("test-ed2k://two")) <= 10 * MiB + 4 * MiB);
}

void TstStreamPlay::sourceDisappears()
{
    StreamEnv env(QStringLiteral("gone"));
    const qint64 size = 16 * MiB;
    const QByteArray data = randomBytes(size, 79);
    TestPlugin *g = env.addPlugin(Net::Gnutella, NetworkPlugin::CanFetchRanges);
    FileHashes h;
    h.tth = tthRoot(data);
    QList<SearchSource> descs;
    for (int i = 0; i < 2; ++i) {
        SourceConfig c = tthConfig(data, 2 * 1000 * 1000);
        if (i == 0) {   // the first one stops sending after 1 MB, for good
            c.stallAfterBytes = true;
            c.stallAt = 1000 * 1000;
        }
        descs << g->addSource(QStringLiteral("test-gnut://g%1").arg(i), c, QStringLiteral("198.51.100.%1:6346").arg(70 + i));
    }
    const QString name = QStringLiteral("Big Buck Bunny gone.mp4");
    g->cannedHits << hitOf(name, data, h, descs);
    env.up();
    const QJsonObject so = env.streamOf(QStringLiteral("bunny"), name);
    Player p(env.port, StreamEnv::pathOf(so), 0, 10 * MiB);
    QVERIFY(p.waitDone(90000));
    QCOMPARE(p.data(), data.left(10 * MiB));
    qInfo("NUMBERS a source stops: longest wait %lld ms (request time-out %d ms), the other served %.1f MB",
          p.longestWaitMs(), env.tuning.requestTimeoutMs, g->served(QStringLiteral("test-gnut://g1")) / 1048576.0);
    QVERIFY(g->served(QStringLiteral("test-gnut://g1")) > 5 * MiB);
}

void TstStreamPlay::noSourceAtAll()
{
    StreamEnv env(QStringLiteral("nosource"));
    const QByteArray data = randomBytes(4 * MiB, 80);
    TestPlugin *e = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
    SourceConfig c;
    c.data = data;
    c.failOpen = true;   // nobody answers
    const QString name = QStringLiteral("Big Buck Bunny none.mkv");
    e->cannedHits << hitOf(name, data, hashesOf(data, 2), {e->addSource(QStringLiteral("test-ed2k://none"), c, QStringLiteral("198.51.100.80:4662"))});
    env.up();
    const QJsonObject so = env.streamOf(QStringLiteral("bunny"), name);
    const QString sid = so.value(QStringLiteral("id")).toString();
    Player p(env.port, StreamEnv::pathOf(so), 0, MiB);
    // the player is answered (the size is known) and waits; the status says for what
    QVERIFY(QTest::qWaitFor([&] {
        const QJsonArray pl = env.status(sid).value(QStringLiteral("players")).toArray();
        return !pl.isEmpty() && pl.first().toObject().contains(QStringLiteral("waitingFor"));
    }, 10000));
    QVERIFY(QTest::qWaitFor([&] { return env.status(sid).value(QStringLiteral("state")).toString() == QLatin1String("waiting for data"); }, 3000));
    // after the stall time with no source the connection is closed, with the reason in the status
    QVERIFY(QTest::qWaitFor([&] { return p.failed(); }, 30000));
    const QString err = env.status(sid).value(QStringLiteral("error")).toString();
    QVERIFY2(err.contains(QStringLiteral("no source")), qPrintable(err));
    QCOMPARE(p.data().size(), 0);
}

void TstStreamPlay::stopDropsTheCache()
{
    StreamEnv env(QStringLiteral("stop"));
    const QByteArray data = randomBytes(12 * MiB, 81);
    TestPlugin *e = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
    const QString name = QStringLiteral("Big Buck Bunny stop.mkv");
    e->cannedHits << hitOf(name, data, hashesOf(data, 2),
                           {e->addSource(QStringLiteral("test-ed2k://stop"), ed2kConfig(data, 4 * 1000 * 1000), QStringLiteral("198.51.100.81:4662"))});
    env.up();
    QSignalSpy removed(env.core->transfers(), &TransferService::transferRemoved);
    const QJsonObject so = env.streamOf(QStringLiteral("bunny"), name);
    const QString did = so.value(QStringLiteral("download")).toString();
    // the Transfers window shows it with an honest status
    QVERIFY(QTest::qWaitFor([&] { return env.core->transfers()->transfer(did).statusText.startsWith(QStringLiteral("Streaming:")); }, 5000));
    {
        Player p(env.port, StreamEnv::pathOf(so), 0, 3 * MiB);
        QVERIFY(p.waitDone(30000));
    }
    const QString cache = env.root + QStringLiteral("/incomplete/Streams/") + did;
    QVERIFY(QFileInfo::exists(cache));
    QVERIFY(QDir(cache).entryList(QDir::Files).size() >= 1);
    QCOMPARE(env.api("DELETE", "/api/v1/streams/" + so.value(QStringLiteral("id")).toString().toLatin1()).status, 200);
    QVERIFY(QTest::qWaitFor([&] { return !QFileInfo::exists(cache); }, 10000));
    QVERIFY(!removed.isEmpty());
    QVERIFY(env.core->transfers()->transfer(did).id.isEmpty());
    QVERIFY(QDir(env.root + QStringLiteral("/incoming")).entryList(QDir::Files).isEmpty());
}

void TstStreamPlay::idleTimeAndCacheLimit()
{
    StreamEnv env(QStringLiteral("idle"));
    const qint64 size = 40 * MiB;
    const QByteArray d1 = randomBytes(size, 82), d2 = randomBytes(size, 83);
    TestPlugin *e = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
    e->cannedHits << hitOf(QStringLiteral("Big Buck Bunny one.mkv"), d1, hashesOf(d1, 2),
                           {e->addSource(QStringLiteral("test-ed2k://one"), ed2kConfig(d1, 0), QStringLiteral("198.51.100.82:4662"))});
    e->cannedHits << hitOf(QStringLiteral("Big Buck Bunny two.mkv"), d2, hashesOf(d2, 2),
                           {e->addSource(QStringLiteral("test-ed2k://two"), ed2kConfig(d2, 0), QStringLiteral("198.51.100.83:4662"))});
    env.up();
    env.core->settings()->setValue(keys::api::CacheLimitMB, 64);
    const QJsonObject s1 = env.streamOf(QStringLiteral("bunny"), QStringLiteral("Big Buck Bunny one.mkv"));
    const QJsonObject s2 = env.streamOf(QStringLiteral("bunny"), QStringLiteral("Big Buck Bunny two.mkv"));
    const QString id1 = s1.value(QStringLiteral("id")).toString(), id2 = s2.value(QStringLiteral("id")).toString();
    {   // the first stream is read whole, then its player leaves
        Player p(env.port, StreamEnv::pathOf(s1), 0, size);
        QVERIFY(p.waitDone(60000));
        QCOMPARE(p.data(), d1);
    }
    // the second is played: together over 64 MB: the idle first one goes, the played one stays
    Player p2(env.port, StreamEnv::pathOf(s2), 0, size, 8 * 1000 * 1000);
    QVERIFY(QTest::qWaitFor([&] { return p2.firstByteMs() >= 0; }, 10000));
    stream::StreamService::Tuning t = env.core->streamService()->tuning();
    t.idleMinuteMs = 3000;   // "1 minute" = 3 s from here on
    env.core->streamService()->setTuning(t);
    env.core->settings()->setValue(keys::api::IdleMin, 1);
    QVERIFY(QTest::qWaitFor([&] { return env.status(id1).value(QStringLiteral("state")).toString() == QLatin1String("ended"); }, 15000));
    const QString why = env.status(id1).value(QStringLiteral("ended")).toString();
    QVERIFY2(why.contains(QStringLiteral("limit")) || why.contains(QStringLiteral("no player")), qPrintable(why));
    QVERIFY(QTest::qWaitFor([&] { return !QFileInfo::exists(env.root + QStringLiteral("/incomplete/Streams/") + s1.value(QStringLiteral("download")).toString()); }, 10000));
    QVERIFY(p2.waitDone(60000));
    if (p2.data() != d2) {
        qint64 at = 0;
        while (at < p2.data().size() && p2.data().at(int(at)) == d2.at(int(at))) ++at;
        qWarning("first wrong byte at %lld of %lld; status %s", at, qint64(p2.data().size()),
                 QJsonDocument(env.status(id2)).toJson(QJsonDocument::Compact).left(1500).constData());
    }
    QCOMPARE(p2.data(), d2);
    QVERIFY(env.status(id2).value(QStringLiteral("state")).toString() != QLatin1String("ended"));
    // no player any more: over the limit and idle, it goes at once; else after the idle time
    p2.stop();
    QElapsedTimer w;
    w.start();
    QVERIFY(QTest::qWaitFor([&] { return env.status(id2).value(QStringLiteral("state")).toString() == QLatin1String("ended"); }, 15000));
    qInfo("NUMBERS idle / limit: the second stream ended %lld ms after its player left: %s", w.elapsed(),
          qPrintable(env.status(id2).value(QStringLiteral("ended")).toString()));
}

void TstStreamPlay::keepAsDownload()
{
    StreamEnv env(QStringLiteral("keep"));
    const qint64 size = 20 * MiB + 99;
    const QByteArray data = randomBytes(size, 84);
    TestPlugin *e = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
    const QString name = QStringLiteral("Big Buck Bunny keep.mkv");
    const FileHashes h = hashesOf(data, 3);
    e->cannedHits << hitOf(name, data, h, {e->addSource(QStringLiteral("test-ed2k://keep"), ed2kConfig(data, 8 * 1000 * 1000), QStringLiteral("198.51.100.84:4662"))});
    env.up();
    env.core->settings()->setValue(keys::api::KeepStreams, true);
    const QJsonObject so = env.streamOf(QStringLiteral("bunny"), name);
    const QString did = so.value(QStringLiteral("download")).toString();
    {
        Player p(env.port, StreamEnv::pathOf(so), 5 * MiB, 7 * MiB);
        QVERIFY(p.waitDone(30000));
    }
    QCOMPARE(env.api("DELETE", "/api/v1/streams/" + so.value(QStringLiteral("id")).toString().toLatin1()).status, 200);
    // a normal download row now: it completes, passes its final check, lands in the incoming folder
    QVERIFY(QTest::qWaitFor([&] { return env.core->transfers()->transfer(did).state == TransferState::Completed; }, 60000));
    const TransferInfo t = env.core->transfers()->transfer(did);
    QVERIFY2(t.savePath.startsWith(env.root + QStringLiteral("/incoming")), qPrintable(t.savePath));
    QCOMPARE(QCryptographicHash::hash(readFile(t.savePath), QCryptographicHash::Sha1), h.sha1);
    QVERIFY(!t.statusText.startsWith(QStringLiteral("Streaming")));
    // in the database like any download: it is there after a restart
    env.down();
    env.up();
    QCOMPARE(env.core->transfers()->transfer(did).state, TransferState::Completed);
}

void TstStreamPlay::quitInTheMiddle()
{
    StreamEnv env(QStringLiteral("quit"));
    const qint64 size = 24 * MiB;
    const QByteArray data = randomBytes(size, 85);
    TestPlugin *e = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
    const QString name = QStringLiteral("Big Buck Bunny quit.mkv");
    e->cannedHits << hitOf(name, data, hashesOf(data, 2),
                           {e->addSource(QStringLiteral("test-ed2k://quit"), ed2kConfig(data, 2 * 1000 * 1000), QStringLiteral("198.51.100.85:4662"))});
    // a cache left by a crash of an earlier session goes at the start
    const QString stale = env.root + QStringLiteral("/incomplete/Streams/d1234567890abcdef");
    QDir().mkpath(stale);
    {
        QFile f(stale + QStringLiteral("/d1234567890abcdef.part"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QByteArray(1000, 'z'));
    }
    env.up();
    QVERIFY(!QFileInfo::exists(stale));
    const QJsonObject so = env.streamOf(QStringLiteral("bunny"), name);
    Player p(env.port, StreamEnv::pathOf(so), 0, size, 1000 * 1000);
    QVERIFY(QTest::qWaitFor([&] { return p.data().size() > 2 * MiB; }, 20000));
    // the safe quit in the middle of the stream (safe-1's steps): it waits neither for the player nor
    // for the read-ahead; the connection is cut at once; no record of the stream is stored (no record
    // claims bytes that are not on the disk: here there is no record at all)
    QElapsedTimer quit;
    quit.start();
    env.core->beginShutdown();
    QVERIFY(QTest::qWaitFor([&] { return !env.core->shutdownBusy(); }, 3000));
    const qint64 busyMs = quit.elapsed();
    QVERIFY(QTest::qWaitFor([&] { return p.failed(); }, 3000));   // after what it had in its socket
    QVERIFY(env.core->engine()->downloads().isEmpty());   // the stream's row went with it
    env.down();
    qInfo("NUMBERS safe quit with a player reading: nothing left to save after %lld ms, the whole quit %lld ms", busyMs, quit.elapsed());
    QVERIFY(quit.elapsed() < 5000);
    {
        Database db;
        QVERIFY(db.open(env.profile + QStringLiteral("/octomx.db")));
        DownloadsDao dao(&db);
        QVERIFY(dao.all().isEmpty());
        db.close();
    }
    env.up();
    QVERIFY(env.core->transfers()->downloads().isEmpty());
    QVERIFY(QTest::qWaitFor([&] { return QDir(env.root + QStringLiteral("/incomplete/Streams")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty(); }, 5000));
}

void TstStreamPlay::pauseAndCancelInTransfers()
{
    StreamEnv env(QStringLiteral("pause"));
    const qint64 size = 24 * MiB;
    const QByteArray data = randomBytes(size, 86);
    TestPlugin *e = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
    const QString name = QStringLiteral("Big Buck Bunny pause.mkv");
    e->cannedHits << hitOf(name, data, hashesOf(data, 2),
                           {e->addSource(QStringLiteral("test-ed2k://pause"), ed2kConfig(data, 3 * 1000 * 1000), QStringLiteral("198.51.100.86:4662"))});
    env.up();
    const QJsonObject so = env.streamOf(QStringLiteral("bunny"), name);
    const QString sid = so.value(QStringLiteral("id")).toString(), did = so.value(QStringLiteral("download")).toString();
    Player p(env.port, StreamEnv::pathOf(so), 0, size, 600 * 1000);
    QVERIFY(QTest::qWaitFor([&] { return p.data().size() > MiB; }, 20000));
    env.core->transfers()->pause(did);
    QVERIFY(QTest::qWaitFor([&] { return env.status(sid).value(QStringLiteral("state")).toString() == QLatin1String("paused in the Transfers window"); }, 5000));
    const qint64 servedAtPause = e->served(QStringLiteral("test-ed2k://pause"));
    QTest::qWait(2000);
    QVERIFY(e->served(QStringLiteral("test-ed2k://pause")) - servedAtPause < 3 * MiB);   // nothing more but what was in flight
    env.core->transfers()->resume(did);
    const qint64 before = p.data().size();
    QVERIFY(QTest::qWaitFor([&] { return p.data().size() > before + 2 * MiB; }, 30000));
    QCOMPARE(p.data(), data.left(p.data().size()));
    // Cancel in the Transfers window: the player's connection ends, the status says why
    env.core->transfers()->cancel(did, true);
    QVERIFY(QTest::qWaitFor([&] { return env.status(sid).value(QStringLiteral("state")).toString() == QLatin1String("ended"); }, 5000));
    QVERIFY(QTest::qWaitFor([&] { return p.failed(); }, 60000));   // once it has played what it had received
    QVERIFY(env.status(sid).value(QStringLiteral("ended")).toString().contains(QStringLiteral("Transfers")));
    QVERIFY(QTest::qWaitFor([&] { return !QFileInfo::exists(env.root + QStringLiteral("/incomplete/Streams/") + did); }, 10000));
}

void TstStreamPlay::streamOfANormalDownload()
{
    StreamEnv env(QStringLiteral("normal"));
    const qint64 size = 16 * MiB;
    const QByteArray data = randomBytes(size, 87);
    TestPlugin *e = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
    const QString name = QStringLiteral("Big Buck Bunny normal.mkv");
    const FileHashes h = hashesOf(data, 3);
    e->cannedHits << hitOf(name, data, h, {e->addSource(QStringLiteral("test-ed2k://normal"), ed2kConfig(data, 3 * 1000 * 1000), QStringLiteral("198.51.100.87:4662"))});
    env.up();
    int status = 0;
    const QJsonObject so = env.streamOf(QStringLiteral("bunny"), name, -1, /*download*/ true, &status);
    QCOMPARE(status, 201);
    const QString did = so.value(QStringLiteral("download")).toString();
    QVERIFY(!env.download(so)->streamRestricts());
    // the player still gets its bytes first: the end of the file before the middle
    Player p(env.port, StreamEnv::pathOf(so), size - MiB, size);
    QVERIFY(p.waitDone(30000));
    QCOMPARE(p.data(), data.right(MiB));
    QCOMPARE(env.api("DELETE", "/api/v1/streams/" + so.value(QStringLiteral("id")).toString().toLatin1()).status, 200);
    // the download stays and completes into the incoming folder
    QVERIFY(QTest::qWaitFor([&] { return env.core->transfers()->transfer(did).state == TransferState::Completed; }, 60000));
    QCOMPARE(QCryptographicHash::hash(readFile(env.core->transfers()->transfer(did).savePath), QCryptographicHash::Sha1), h.sha1);
}

// the owner: "in case of pause / stop you have to stop to download and after a while delete the cache"
void TstStreamPlay::pausedStoppedAndBack()
{
    StreamEnv env(QStringLiteral("pausestop"));
    const qint64 size = 64 * MiB;
    const QByteArray data = randomBytes(size, 88);
    FileHashes h;
    h.tth = tthRoot(data);
    TestPlugin *g = env.addPlugin(Net::Gnutella, NetworkPlugin::CanFetchRanges);
    const QString uri = QStringLiteral("test-gnut://ps");
    const QString name = QStringLiteral("Big Buck Bunny pause.mp4");
    g->cannedHits << hitOf(name, data, h, {g->addSource(uri, tthConfig(data, 3 * 1000 * 1000), QStringLiteral("198.51.100.88:6346"))});
    env.up();
    stream::StreamService::Tuning t = env.core->streamService()->tuning();
    t.releaseMs = 2000;
    t.idleMinuteMs = 1000;   // "1 minute" = 1 s here
    env.core->streamService()->setTuning(t);
    // 60 s: the paused player below reads nothing for 5 s and more on a busy machine, which must not
    // be the idle time yet (8 s further down, where the idle time is the point)
    env.core->settings()->setValue(keys::api::IdleMin, 60);
    const QJsonObject so = env.streamOf(QStringLiteral("bunny"), name);
    const QString sid = so.value(QStringLiteral("id")).toString(), did = so.value(QStringLiteral("download")).toString();
    const QByteArray path = StreamEnv::pathOf(so);
    // ---- a paused player: the connection stays open, it reads nothing; fetching stops once the
    // bounded buffer is full, it never goes on to the rest of the file
    auto *p = new Player(env.port, path, 0, 0, 1000 * 1000);
    QVERIFY(QTest::qWaitFor([&] { return p->data().size() >= 3 * MiB; }, 30000));
    QJsonObject st;
    QVERIFY(QTest::qWaitFor([&] {
        st = env.status(sid);
        return st.value(QStringLiteral("state")).toString() == QLatin1String("playing");
    }, 5000));
    p->stopReading();   // pause: open connection, nothing read
    QVERIFY(QTest::qWaitFor([&] { return env.status(sid).value(QStringLiteral("state")).toString() == QLatin1String("paused"); }, 10000));
    QVERIFY(env.status(sid).contains(QStringLiteral("cacheDropInSec")));
    qint64 last = -1, stableFor = 0;
    QElapsedTimer w;
    w.start();
    while (w.elapsed() < 20000 && stableFor < 3000) {   // fetching stops: no new bytes for 3 s
        QTest::qWait(250);
        const qint64 now = g->served(uri);
        stableFor = now == last ? stableFor + 250 : 0;
        last = now;
    }
    const qint64 pausedServed = g->served(uri);
    const qint64 sentToPlayer = qint64(env.status(sid).value(QStringLiteral("players")).toArray().first().toObject().value(QStringLiteral("sent")).toDouble());
    const qint64 target = qint64(env.status(sid).value(QStringLiteral("players")).toArray().first().toObject().value(QStringLiteral("bufferTarget")).toDouble());
    qInfo("NUMBERS paused player: fetching stopped %lld ms after the pause; fetched %.1f MB, sent to the player %.1f MB (it read %.1f MB), buffer target %.1f MB, file %.1f MB",
          w.elapsed() - stableFor, pausedServed / 1048576.0, sentToPlayer / 1048576.0, p->data().size() / 1048576.0, target / 1048576.0, size / 1048576.0);
    QVERIFY(stableFor >= 3000);
    QVERIFY(pausedServed <= sentToPlayer + target + 2 * MiB);
    QVERIFY(pausedServed < size / 2);
    QTest::qWait(1500);
    QCOMPARE(g->served(uri), pausedServed);   // still nothing
    // reading again continues at once from the buffer
    const qint64 had = p->data().size();
    p->resumeReading();
    QElapsedTimer r;
    r.start();
    QVERIFY(QTest::qWaitFor([&] { return p->data().size() > had; }, 5000));
    qInfo("NUMBERS paused player reads again: the next bytes after %lld ms", r.elapsed());
    QVERIFY(r.elapsed() < 500);
    // ---- the player stops (closes): nothing new is asked, the sources are let go
    QVERIFY(QTest::qWaitFor([&] { return p->data().size() > had + MiB; }, 10000));
    const qint64 readUpTo = p->data().size();
    QVERIFY(p->data() == data.left(int(readUpTo)));
    delete p;
    QTest::qWait(500);   // a request on its way may finish
    const qint64 atStop = g->served(uri);
    const int requestsAtStop = g->source(uri) ? int(g->source(uri)->requests.size()) : 0;
    QVERIFY(QTest::qWaitFor([&] { return env.core->transfers()->transfer(did).statusText.startsWith(QStringLiteral("Streaming: idle, no player")); }, 5000));
    QCOMPARE(env.status(sid).value(QStringLiteral("state")).toString(), QStringLiteral("idle"));
    QVERIFY(QTest::qWaitFor([&] { return env.core->transfers()->transfer(did).sourcesActive == 0 && g->source(uri) && g->source(uri)->closeCode == QLatin1String("dropped"); }, 8000));
    QTest::qWait(1000);
    const int requestsAfter = g->source(uri) ? int(g->source(uri)->requests.size()) : 0;
    qInfo("NUMBERS player gone: %.1f MB fetched at the stop, %.1f MB 2 s later; requests %d then %d; the source was let go (%s)",
          atStop / 1048576.0, g->served(uri) / 1048576.0, requestsAtStop, requestsAfter, qPrintable(g->source(uri)->closeCode));
    QCOMPARE(g->served(uri), atStop);
    QCOMPARE(requestsAfter, requestsAtStop);
    // ---- it comes back within the idle time: what is cached is served from the cache, nothing twice
    const qint64 servedBefore = g->served(uri);
    {
        Player back(env.port, path, 0, readUpTo);
        QVERIFY(back.waitDone(10000));
        QCOMPARE(back.data(), data.left(int(readUpTo)));
        qInfo("NUMBERS a player back within the idle time: %.1f MB read again, %.1f MB fetched for it, first byte %lld ms",
              readUpTo / 1048576.0, (g->served(uri) - servedBefore) / 1048576.0, back.firstByteMs());
        QCOMPARE(g->served(uri), servedBefore);   // nothing fetched twice
    }
    // ---- the idle time passes: the stream ends, its cache file and its row go
    env.core->settings()->setValue(keys::api::IdleMin, 8);   // 8 s from the moment that player left
    QElapsedTimer idle;
    idle.start();
    QVERIFY(QTest::qWaitFor([&] { return env.status(sid).value(QStringLiteral("state")).toString() == QLatin1String("ended"); }, 20000));
    QVERIFY(QTest::qWaitFor([&] { return !QFileInfo::exists(env.root + QStringLiteral("/incomplete/Streams/") + did); }, 10000));
    QVERIFY(env.core->transfers()->transfer(did).id.isEmpty());
    qInfo("NUMBERS idle time 8 s (test value): the stream ended %lld ms after its last player left: %s", idle.elapsed(),
          qPrintable(env.status(sid).value(QStringLiteral("ended")).toString()));
    QVERIFY(idle.elapsed() >= 6000);
}

void TstStreamPlay::idleDefaultIsFourHours()
{
    QCOMPARE(settingDefault(QString(keys::api::IdleMin)).toInt(), 240);
    StreamEnv env(QStringLiteral("idledefault"));
    TestPlugin *e = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(2 * MiB, 89);
    const QString name = QStringLiteral("Big Buck Bunny default.mkv");
    e->cannedHits << hitOf(name, data, hashesOf(data, 2), {e->addSource(QStringLiteral("test-ed2k://def"), ed2kConfig(data, 0), QStringLiteral("198.51.100.89:4662"))});
    env.up();
    const QJsonObject so = env.streamOf(QStringLiteral("bunny"), name);
    QVERIFY(QTest::qWaitFor([&] { return env.status(so.value(QStringLiteral("id")).toString()).contains(QStringLiteral("cacheDropInSec")); }, 5000));
    const double left = env.status(so.value(QStringLiteral("id")).toString()).value(QStringLiteral("cacheDropInSec")).toDouble();
    QVERIFY2(left > 4 * 3600 - 60 && left <= 4 * 3600, qPrintable(QString::number(left)));
    QVERIFY(env.core->transfers()->transfer(so.value(QStringLiteral("download")).toString()).statusText.contains(QStringLiteral("the cache goes in 3 h 59 min"))
            || env.core->transfers()->transfer(so.value(QStringLiteral("download")).toString()).statusText.contains(QStringLiteral("4 h 0 min")));
}

// "maximum downloads at once" = 1 and a normal download runs: the stream still runs (it takes no
// place of that limit) and its status says so
void TstStreamPlay::notStarvedByDownloadsAtOnce()
{
    StreamEnv env(QStringLiteral("atonce"));
    const QByteArray big = randomBytes(30 * MiB, 90), clip = randomBytes(8 * MiB, 91);
    TestPlugin *e = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
    const SearchSource bs = e->addSource(QStringLiteral("test-ed2k://big"), ed2kConfig(big, 300 * 1000), QStringLiteral("198.51.100.91:4662"));
    const QString name = QStringLiteral("Big Buck Bunny atonce.mkv");
    e->cannedHits << hitOf(name, clip, hashesOf(clip, 2),
                           {e->addSource(QStringLiteral("test-ed2k://clip"), ed2kConfig(clip, 3 * 1000 * 1000), QStringLiteral("198.51.100.92:4662"))});
    env.up();
    env.core->settings()->setValue(keys::transfers::MaxDownloads, 1);
    DownloadRequest r;
    r.name = QStringLiteral("ubuntu-24.04-desktop-amd64.iso");
    r.size = big.size();
    r.hashes = hashesOf(big, 2);
    r.sources = {bs};
    const QString normal = env.core->transfers()->download(r);
    QVERIFY(QTest::qWaitFor([&] { return env.core->transfers()->transfer(normal).state == TransferState::Transferring; }, 10000));
    const QJsonObject so = env.streamOf(QStringLiteral("bunny"), name);
    Player p(env.port, StreamEnv::pathOf(so), 0, 4 * MiB);
    QVERIFY(p.waitDone(30000));
    QCOMPARE(p.data(), clip.left(4 * MiB));
    QCOMPARE(env.core->transfers()->transfer(normal).state, TransferState::Transferring);   // both run
    const QJsonObject st = env.status(so.value(QStringLiteral("id")).toString());
    QVERIFY(st.value(QStringLiteral("limits")).toObject().value(QStringLiteral("downloadsAtOnce")).toString().contains(QStringLiteral("not counted")));
}

// Verify File and Restart Download make no sense for a stream: greyed, with the reason
void TstStreamPlay::verifyAndRestartNotForStreams()
{
    StreamEnv env(QStringLiteral("verify"));
    const QByteArray data = randomBytes(6 * MiB, 92);
    TestPlugin *e = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
    const QString name = QStringLiteral("Big Buck Bunny verify.mkv");
    e->cannedHits << hitOf(name, data, hashesOf(data, 2), {e->addSource(QStringLiteral("test-ed2k://ver"), ed2kConfig(data, 0), QStringLiteral("198.51.100.93:4662"))});
    env.up();
    const QJsonObject so = env.streamOf(QStringLiteral("bunny"), name);
    const QString did = so.value(QStringLiteral("download")).toString();
    {
        Player p(env.port, StreamEnv::pathOf(so), 0, 2 * MiB);
        QVERIFY(p.waitDone(20000));
    }
    QString why;
    QVERIFY(!env.core->transfers()->canVerifyDownload(did, &why));
    QVERIFY2(why.contains(QStringLiteral("stream")), qPrintable(why));
    QVERIFY(!env.core->transfers()->verifyDownload(did));
    QVERIFY(!env.core->transfers()->canRestartDownload(did, &why));
    QVERIFY2(why.contains(QStringLiteral("stream")), qPrintable(why));
    QVERIFY(!env.core->transfers()->restartDownload(did));
    // the stream goes on as before
    QVERIFY(env.status(so.value(QStringLiteral("id")).toString()).value(QStringLiteral("state")).toString() != QLatin1String("ended"));
    Player p2(env.port, StreamEnv::pathOf(so), 3 * MiB, 4 * MiB);
    QVERIFY(p2.waitDone(20000));
    QCOMPARE(p2.data(), data.mid(3 * MiB, MiB));
    // a stream of a normal download (download: true) is a normal download: both allowed
    StreamEnv env2(QStringLiteral("verify2"));
    TestPlugin *e2 = env2.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
    e2->cannedHits << hitOf(name, data, hashesOf(data, 2), {e2->addSource(QStringLiteral("test-ed2k://ver2"), ed2kConfig(data, 0), QStringLiteral("198.51.100.94:4662"))});
    env.down();
    env2.up();
    const QJsonObject n = env2.streamOf(QStringLiteral("bunny"), name, -1, true);
    QVERIFY(env2.core->transfers()->canRestartDownload(n.value(QStringLiteral("download")).toString(), nullptr));
}

// a crash in the middle of a stream (what is on the disk at that moment, taken while it streams),
// then a start: nothing of the stream is left, no row, nothing "waiting to be checked"
void TstStreamPlay::crashInTheMiddle()
{
    StreamEnv env(QStringLiteral("crash"));
    const qint64 size = 24 * MiB;
    const QByteArray data = randomBytes(size, 93);
    TestPlugin *e = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
    const QString name = QStringLiteral("Big Buck Bunny crash.mkv");
    e->cannedHits << hitOf(name, data, hashesOf(data, 2), {e->addSource(QStringLiteral("test-ed2k://crash"), ed2kConfig(data, 2 * 1000 * 1000), QStringLiteral("198.51.100.95:4662"))});
    env.up();
    // a normal download beside it: it is checked after the crash, the stream is not
    TestPlugin *n = e;
    const QByteArray other = randomBytes(12 * MiB, 94);
    DownloadRequest r;
    r.name = QStringLiteral("debian-12.8.0-amd64-netinst.iso");
    r.size = other.size();
    r.hashes = hashesOf(other, 2);
    r.sources = {n->addSource(QStringLiteral("test-ed2k://iso"), ed2kConfig(other, 1000 * 1000), QStringLiteral("198.51.100.96:4662"))};
    const QString iso = env.core->transfers()->download(r);
    const QJsonObject so = env.streamOf(QStringLiteral("bunny"), name);
    Player p(env.port, StreamEnv::pathOf(so), 0, size, 1000 * 1000);
    QVERIFY(QTest::qWaitFor([&] { return p.data().size() > 2 * MiB && env.core->transfers()->transfer(iso).done > MiB; }, 30000));
    // the disk as a crash leaves it: the running mark, the database, the caches
    QVERIFY(QFileInfo::exists(env.profile + QStringLiteral("/octomx.running")));
    env.core->database()->flush();
    const QString copy = env.root + QStringLiteral("/after-crash");
    QDir(copy).removeRecursively();
    QDir().mkpath(copy);
    QVERIFY(copyTree(env.profile, copy + QStringLiteral("/profile")));
    QVERIFY(copyTree(env.root + QStringLiteral("/incomplete"), copy + QStringLiteral("/incomplete")));
    QVERIFY(!QDir(copy + QStringLiteral("/incomplete/Streams")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
    p.stop();
    env.down();
    // the start after the crash, on that disk
    StreamEnv after(QStringLiteral("crash-start"));
    QDir(after.root).removeRecursively();
    QVERIFY(QDir().rename(copy, after.root));
    TestPlugin *e2 = after.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
    e2->addSource(QStringLiteral("test-ed2k://iso"), ed2kConfig(other, 1000 * 1000), QStringLiteral("198.51.100.96:4662"));
    after.up();
    QVERIFY(after.core->lastStopWasUnclean());
    const QList<TransferInfo> rows = after.core->transfers()->downloads();
    QCOMPARE(rows.size(), 1);   // the normal download only
    QCOMPARE(rows.first().id, iso);
    for (const TransferInfo &t : rows) QVERIFY(!t.statusText.startsWith(QStringLiteral("Streaming")));
    QVERIFY(QTest::qWaitFor([&] { return QDir(after.root + QStringLiteral("/incomplete/Streams")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty(); }, 5000));
    QVERIFY(QDir(after.root + QStringLiteral("/incomplete")).entryList({QStringLiteral("*.moving")}, QDir::Files).isEmpty());
    // the normal download is checked and finishes
    QVERIFY(QTest::qWaitFor([&] { return after.core->transfers()->transfer(iso).state == TransferState::Completed; }, 60000));
}

QTEST_MAIN(TstStreamPlay)
#include "tst_stream_play.moc"
