// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX engine tests: [dl-1] download order ("Download In Sequential Order", "Download First And
// Last Pieces First") over fake range sources and the fake torrent backend, and preview of the
// partial file (readiness rules, the hard link a player opens, a reader that keeps it open).
#include <QtTest/QtTest>

#include "engine_env.h"
#include "mx/download_order.h"

using namespace mx;
using namespace mx::etest;

namespace {

constexpr qint64 MiB = 1024 * 1024;

DownloadRequest req(const QString &name, const QByteArray &data, const QList<SearchSource> &src)
{
    DownloadRequest r;
    r.name = name;
    r.size = data.size();
    r.hashes = hashesOf(data, 1);
    r.sources = src;
    return r;
}

} // namespace

// the block map the engine reports ([[s, e], ...])
struct RangeSetLike {
    QList<QPair<qint64, qint64>> r;
    qint64 total() const { qint64 n = 0; for (const auto &x : r) n += x.second - x.first; return n; }
    qint64 lastEnd() const { return r.isEmpty() ? 0 : r.last().second; }
    QString text() const
    {
        QStringList l;
        for (const auto &x : r) l << QStringLiteral("[%1,%2)").arg(x.first).arg(x.second);
        return l.join(QLatin1Char(' '));
    }
};

namespace {
RangeSetLike heldOf(DownloadEngine *e, const QString &id)
{
    RangeSetLike out;
    for (const QVariant &v : e->debugInfo(id).value(QStringLiteral("heldRanges")).toList()) {
        const QVariantList p = v.toList();
        if (p.size() == 2) out.r << qMakePair(p.at(0).toLongLong(), p.at(1).toLongLong());
    }
    return out;
}

// H = head band, T = tail band, M = the middle, for each request offset (in issue order)
QString shape(const QList<QPair<qint64, qint64>> &reqs, qint64 size)
{
    const qint64 h = order::headBandBytes(size), t = order::tailBandBytes(size);
    QString s;
    for (const auto &r : reqs) s += r.first < h ? QLatin1Char('H') : r.first >= size - t ? QLatin1Char('T') : QLatin1Char('M');
    return s;
}

// a tiny ISO BMFF file: ftyp, mdat (payload), moov at the end (not "fast start") or before mdat
QByteArray isoFile(qint64 size, bool moovFirst, quint32 seed)
{
    auto box = [](const char *type, qint64 len) {
        QByteArray b(8, '\0');
        b[0] = char((len >> 24) & 0xff);
        b[1] = char((len >> 16) & 0xff);
        b[2] = char((len >> 8) & 0xff);
        b[3] = char(len & 0xff);
        b.replace(4, 4, QByteArray(type, 4));
        return b;
    };
    const qint64 moovLen = 64 * 1024;
    QByteArray ftyp = box("ftyp", 24) + QByteArray("isom\0\0\0\0isomiso2", 16);
    const qint64 mdatLen = size - ftyp.size() - moovLen;
    QByteArray mdat = box("mdat", mdatLen) + randomBytes(mdatLen - 8, seed);
    QByteArray moov = box("moov", moovLen) + randomBytes(moovLen - 8, seed + 1);
    return moovFirst ? ftyp + moov + mdat : ftyp + mdat + moov;
}

} // namespace

class TstEngineOrder : public QObject {
    Q_OBJECT
private slots:
    void bandsAndRules();
    void defaultIsRarestFirst();
    void sequentialMakesAPrefix();
    void firstLastHeadThenTail();
    void bothFlags();
    void fallbackWhenNoSourceHasIt();
    void toggledMidway();
    void survivesRestart();
    void torrentOrder();
    void previewPartFile();
    void previewReaderKeepsFileOpen();
};

void TstEngineOrder::bandsAndRules()
{
    QCOMPARE(order::headBandBytes(0), qint64(0));
    QCOMPARE(order::headBandBytes(8 * MiB), 2 * MiB);            // a quarter of a small file
    QCOMPARE(order::headBandBytes(100 * MiB), 4 * MiB);          // at least 4 MiB
    QCOMPARE(order::tailBandBytes(1000 * MiB), 10 * MiB);        // 1 %
    QVERIFY(preview::isMediaFile(QStringLiteral("Sintel.mkv")));
    QVERIFY(preview::isMediaFile(QStringLiteral("a b.MP3")));
    QVERIFY(!preview::isMediaFile(QStringLiteral("setup.exe")));
    QVERIFY(!preview::isMediaFile(QStringLiteral("debian.iso")));
    QCOMPARE(preview::neededHead(QStringLiteral("x.mkv"), 1000 * MiB), 10 * MiB);
    QCOMPARE(preview::neededHead(QStringLiteral("x.mkv"), 2 * MiB), 2 * MiB);
    QCOMPARE(preview::neededHead(QStringLiteral("x.mp3"), 5 * MiB), qint64(512 * 1024));
    // mkv: the head is enough; avi: the tail band too; exe: never
    QVERIFY(preview::evaluate(QStringLiteral("x.mkv"), 100 * MiB, 4 * MiB, 0).ready);
    QCOMPARE(preview::evaluate(QStringLiteral("x.mkv"), 100 * MiB, 1 * MiB, 0).reason, QStringLiteral("Preview: needs the beginning of the file"));
    QVERIFY(!preview::evaluate(QStringLiteral("x.avi"), 100 * MiB, 50 * MiB, 0).ready);
    QVERIFY(preview::evaluate(QStringLiteral("x.avi"), 100 * MiB, 50 * MiB, 4 * MiB).ready);
    QVERIFY(!preview::evaluate(QStringLiteral("x.exe"), 100 * MiB, 100 * MiB, 0).media);
    // ISO BMFF: moov before mdat = fast start; moov after mdat = the tail from the end of mdat
    for (bool first : {true, false}) {
        const QByteArray f = isoFile(12 * MiB, first, 5);
        auto read = [&f](qint64 o, qint64 l) { return f.mid(int(o), int(l)); };
        constexpr qint64 headOnly = 5 * MiB;
        auto haveHead = [](qint64 o, qint64 l) { return o + l <= headOnly; };
        auto haveAll = [](qint64, qint64) { return true; };
        const preview::Mp4Layout a = preview::scanIsoBmff(f.size(), read, haveHead);
        QVERIFY(a.decided);
        if (first) {
            QCOMPARE(a.tailFrom, qint64(0));
            QVERIFY(preview::evaluate(QStringLiteral("x.mp4"), f.size(), headOnly, 0, a).ready);
        } else {
            QCOMPARE(a.tailFrom, qint64(f.size() - 64 * 1024));
            const preview::Readiness r = preview::evaluate(QStringLiteral("x.mp4"), f.size(), headOnly, 0, a);
            QVERIFY(!r.ready);
            QCOMPARE(r.reason, QStringLiteral("Preview: needs the end of the file"));
            QVERIFY(preview::evaluate(QStringLiteral("x.mp4"), f.size(), headOnly, 64 * 1024, preview::scanIsoBmff(f.size(), read, haveAll)).ready);
        }
    }
    // nothing at the head yet: not decided, "needs the beginning"
    const preview::Mp4Layout none = preview::scanIsoBmff(10 * MiB, [](qint64, qint64) { return QByteArray(); },
                                                         [](qint64, qint64) { return false; });
    QVERIFY(!none.decided);
}

void TstEngineOrder::defaultIsRarestFirst()
{
    // without a flag the engine stays rarest-first: the full source starts where only it has data
    Env env(QStringLiteral("o-default"));
    auto *http = env.addPlugin(Net::Http, NetworkPlugin::CanFetchRanges);
    auto *gnut = env.addPlugin(Net::Gnutella, NetworkPlugin::CanFetchRanges);   // a partial source (has half)
    const QByteArray data = randomBytes(40 * MiB, 71);
    SourceConfig full;
    full.data = data;
    full.bytesPerSec = 4 << 20;
    SourceConfig half = full;
    half.avail = {{0, data.size() / 2}};
    const SearchSource a = http->addSource(QStringLiteral("https://a/f.bin"), full);
    const SearchSource b = gnut->addSource(QStringLiteral("gnut://b/f.bin"), half);
    env.engineUp();
    const QString id = env.engine->download(req(QStringLiteral("f.bin"), data, {a, b}));
    QTRY_VERIFY_WITH_TIMEOUT(http->source(QStringLiteral("https://a/f.bin")) && !http->source(QStringLiteral("https://a/f.bin"))->requests.isEmpty(), 5000);
    QVERIFY2(http->source(QStringLiteral("https://a/f.bin"))->requests.first().first >= data.size() / 2,
             "the full source should start with the part only it has");
    QVERIFY(!env.info(id).sequential && !env.info(id).firstLastFirst);
    env.engine->cancel(id, true);
}

void TstEngineOrder::sequentialMakesAPrefix()
{
    Env env(QStringLiteral("o-seq"));
    env.settings.setValue(QString(keys::transfers::SequentialDefault), true);   // the default for new downloads
    auto *http = env.addPlugin(Net::Http, NetworkPlugin::CanFetchRanges);
    auto *gnut = env.addPlugin(Net::Gnutella, NetworkPlugin::CanFetchRanges);   // a partial source (has half)
    const QByteArray data = randomBytes(40 * MiB, 72);
    SourceConfig full;
    full.data = data;
    full.bytesPerSec = 4 << 20;
    SourceConfig half = full;
    half.avail = {{0, data.size() / 2}};
    const SearchSource a = http->addSource(QStringLiteral("https://a/f.bin"), full);
    const SearchSource b = gnut->addSource(QStringLiteral("gnut://b/f.bin"), half);
    env.engineUp();
    const QString id = env.engine->download(req(QStringLiteral("f.bin"), data, {a, b}));
    QVERIFY(env.info(id).sequential);
    QTest::qWait(2500);
    const RangeSetLike held = heldOf(env.engine.get(), id);
    qInfo("sequential after 2.5 s: %s (%lld bytes)", qPrintable(held.text()), held.total());
    QVERIFY(held.total() > 4 * MiB);
    QVERIFY(!held.r.isEmpty() && held.r.first().first == 0);
    // only the requests in flight may leave gaps: everything held is a prefix of the file
    QVERIFY2(held.lastEnd() - held.total() <= 2 * MiB, qPrintable(held.text()));
    // both sources asked for the lowest missing range each time
    for (const SourceStats *st : {http->source(QStringLiteral("https://a/f.bin")), gnut->source(QStringLiteral("gnut://b/f.bin"))}) {
        QVERIFY(st && st->requests.size() > 2);
        for (int i = 1; i < st->requests.size(); ++i) QVERIFY2(st->requests.at(i).first > st->requests.at(i - 1).first, qPrintable(st->uri));
    }
    const QList<Net> followed = env.info(id).orderFollowedBy;
    QVERIFY(followed.contains(Net::Http) && followed.contains(Net::Gnutella));
    QVERIFY2(env.waitDone(id, 30000), qPrintable(env.info(id).statusText));
    QCOMPARE(readFile(env.info(id).savePath), data);
}

void TstEngineOrder::firstLastHeadThenTail()
{
    Env env(QStringLiteral("o-fl"));
    env.settings.setValue(QString(keys::transfers::FirstLastDefault), true);
    auto *http = env.addPlugin(Net::Http, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(40 * MiB, 73);
    SourceConfig c;
    c.data = data;
    c.bytesPerSec = 16 << 20;
    const SearchSource a = http->addSource(QStringLiteral("https://a/f.bin"), c);
    env.engineUp();
    const QString id = env.engine->download(req(QStringLiteral("f.bin"), data, {a}));
    QVERIFY2(env.waitDone(id, 30000), qPrintable(env.info(id).statusText));
    const QString s = shape(http->source(QStringLiteral("https://a/f.bin"))->requests, data.size());
    qInfo("first and last: %s", qPrintable(s));
    QVERIFY2(QRegularExpression(QStringLiteral("^H+T+M+$")).match(s).hasMatch(), qPrintable(s));
    QCOMPARE(readFile(env.info(id).savePath), data);
}

void TstEngineOrder::bothFlags()
{
    Env env(QStringLiteral("o-both"));
    env.settings.setValue(QString(keys::transfers::SequentialDefault), true);
    env.settings.setValue(QString(keys::transfers::FirstLastDefault), true);
    auto *http = env.addPlugin(Net::Http, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(40 * MiB, 74);
    SourceConfig c;
    c.data = data;
    c.bytesPerSec = 16 << 20;
    const SearchSource a = http->addSource(QStringLiteral("https://a/f.bin"), c);
    env.engineUp();
    const QString id = env.engine->download(req(QStringLiteral("f.bin"), data, {a}));
    QVERIFY2(env.waitDone(id, 30000), qPrintable(env.info(id).statusText));
    const auto &r = http->source(QStringLiteral("https://a/f.bin"))->requests;
    const QString s = shape(r, data.size());
    qInfo("both: %s", qPrintable(s));
    QVERIFY2(QRegularExpression(QStringLiteral("^H+T+M+$")).match(s).hasMatch(), qPrintable(s));
    // after head and tail: the middle in ascending order
    qint64 prev = -1;
    for (int i = 0; i < r.size(); ++i) {
        if (s.at(i) != QLatin1Char('M')) continue;
        QVERIFY(r.at(i).first > prev);
        prev = r.at(i).first;
    }
}

void TstEngineOrder::fallbackWhenNoSourceHasIt()
{
    // a priority, never a condition: the only source lacks the head, it still gives what it has
    Env env(QStringLiteral("o-fallback"));
    env.settings.setValue(QString(keys::transfers::SequentialDefault), true);
    env.settings.setValue(QString(keys::transfers::FirstLastDefault), true);
    auto *http = env.addPlugin(Net::Http, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(40 * MiB, 75);
    SourceConfig c;
    c.data = data;
    c.bytesPerSec = 8 << 20;
    c.avail = {{12 * MiB, 40 * MiB}};
    const SearchSource a = http->addSource(QStringLiteral("https://a/f.bin"), c);
    env.engineUp();
    const QString id = env.engine->download(req(QStringLiteral("f.bin"), data, {a}));
    QTRY_VERIFY_WITH_TIMEOUT(http->served(QStringLiteral("https://a/f.bin")) > 2 * MiB, 8000);
    const auto &r = http->source(QStringLiteral("https://a/f.bin"))->requests;
    QVERIFY(r.first().first >= 12 * MiB);
    QCOMPARE(shape({r.first()}, data.size()), QStringLiteral("T"));   // the tail band first (it has that)
    // later: the lowest missing range it has
    QTRY_VERIFY_WITH_TIMEOUT(shape(http->source(QStringLiteral("https://a/f.bin"))->requests, data.size()).contains(QLatin1Char('M')), 8000);
    for (const auto &x : http->source(QStringLiteral("https://a/f.bin"))->requests) QVERIFY(x.first >= 12 * MiB);
    env.engine->cancel(id, true);
}

void TstEngineOrder::toggledMidway()
{
    Env env(QStringLiteral("o-toggle"));
    auto *http = env.addPlugin(Net::Http, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(40 * MiB, 76);
    SourceConfig c;
    c.data = data;
    c.bytesPerSec = 3 << 20;
    c.maxPending = 1;
    const SearchSource a = http->addSource(QStringLiteral("https://a/f.bin"), c);
    env.engineUp();
    const QString id = env.engine->download(req(QStringLiteral("f.bin"), data, {a}));
    QTRY_VERIFY_WITH_TIMEOUT(http->served(QStringLiteral("https://a/f.bin")) > 6 * MiB, 8000);
    const int before = http->source(QStringLiteral("https://a/f.bin"))->requests.size();
    QVERIFY(!shape(http->source(QStringLiteral("https://a/f.bin"))->requests, data.size()).contains(QLatin1Char('T')));
    env.engine->setFirstLastPiecesFirst(id, true);
    QVERIFY(env.info(id).firstLastFirst);
    QTRY_VERIFY_WITH_TIMEOUT(http->source(QStringLiteral("https://a/f.bin"))->requests.size() > before + 2, 8000);
    const QString after = shape(http->source(QStringLiteral("https://a/f.bin"))->requests.mid(before), data.size());
    qInfo("after switching first and last on: %s", qPrintable(after));
    QVERIFY2(after.startsWith(QLatin1Char('T')), qPrintable(after));
    env.engine->setFirstLastPiecesFirst(id, false);
    QVERIFY(!env.info(id).firstLastFirst);
    env.engine->cancel(id, true);
}

void TstEngineOrder::survivesRestart()
{
    Env env(QStringLiteral("o-restart"));
    auto *http = env.addPlugin(Net::Http, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(40 * MiB, 77);
    SourceConfig c;
    c.data = data;
    c.bytesPerSec = 2 << 20;
    c.maxPending = 1;
    const SearchSource a = http->addSource(QStringLiteral("https://a/f.bin"), c);
    env.engineUp();
    const QString id = env.engine->download(req(QStringLiteral("f.bin"), data, {a}));
    env.engine->setSequentialDownload(id, true);
    env.engine->setFirstLastPiecesFirst(id, true);
    QTRY_VERIFY_WITH_TIMEOUT(http->served(QStringLiteral("https://a/f.bin")) > 1 * MiB, 8000);
    env.engineDown(true);
    env.engineUp();
    const TransferInfo t = env.info(id);
    QVERIFY(t.sequential);
    QVERIFY(t.firstLastFirst);
    QVERIFY(env.engine->debugInfo(id).value(QStringLiteral("sequential")).toBool());
    // the new session goes on with the head band, then the tail
    QTRY_VERIFY_WITH_TIMEOUT(shape(http->source(QStringLiteral("https://a/f.bin"))->requests, data.size()).contains(QLatin1Char('T')), 15000);
    const QString s = shape(http->source(QStringLiteral("https://a/f.bin"))->requests, data.size());
    QVERIFY2(QRegularExpression(QStringLiteral("^H*T")).match(s).hasMatch(), qPrintable(s));
    env.engine->cancel(id, true);
}

void TstEngineOrder::torrentOrder()
{
    // BitTorrent: the engine hands the flags to the backend (libtorrent's sequential mode and band
    // priorities) and reserves pieces for other networks in the same order
    Env env(QStringLiteral("o-torrent"));
    FakeTorrentBackend be;
    auto *http = env.addPlugin(Net::Http, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(24 * MiB, 78);
    const QString tid = be.makeTorrent(QStringLiteral("t.mkv"), {{QStringLiteral("t.mkv"), data}}, 256 * 1024);
    SourceConfig c;
    c.data = data;
    c.bytesPerSec = 1 << 20;
    const SearchSource s = http->addSource(QStringLiteral("https://mirror/t.mkv"), c);
    env.settings.setValue(QString(keys::transfers::FirstLastDefault), true);
    env.engineUp(&be);
    DownloadRequest r;
    r.name = QStringLiteral("t.mkv");
    r.size = data.size();
    r.hashes.btih = QByteArray::fromHex(tid.toLatin1());
    r.uri = QStringLiteral("magnet:?xt=urn:btih:%1&dn=t.mkv").arg(tid);
    SearchSource bt;
    bt.net = Net::BitTorrent;
    bt.uri = r.uri;
    r.sources << bt << s;
    const QString id = env.engine->download(r);
    QTRY_VERIFY_WITH_TIMEOUT(be.order.contains(tid), 5000);
    QCOMPARE(be.order.value(tid), qMakePair(false, true));
    QTRY_VERIFY_WITH_TIMEOUT(!be.reserveLog.isEmpty(), 10000);
    // the first pieces reserved for HTTP are the head band (4 MiB = 16 pieces), then the tail band
    QList<int> first;
    for (const auto &e : be.reserveLog) if (e.second && first.size() < 4) first << e.first;
    qInfo("first reserved pieces: %s", qPrintable([&] { QStringList l; for (int p : first) l << QString::number(p); return l.join(QLatin1Char(',')); }()));
    const int n = int((data.size() + 256 * 1024 - 1) / (256 * 1024));
    for (int p : first) QVERIFY2(p < 16 || p >= n - 16, "a reserved piece outside the head / tail band");
    QVERIFY(first.first() == 0);
    env.engine->setSequentialDownload(id, true);
    QCOMPARE(be.order.value(tid), qMakePair(true, true));
    QVERIFY(env.info(id).orderFollowedBy.contains(Net::BitTorrent));
    env.engine->setFirstLastPiecesFirst(id, false);
    QCOMPARE(be.order.value(tid), qMakePair(true, false));
    env.engine->cancel(id, true);
}

void TstEngineOrder::previewPartFile()
{
    Env env(QStringLiteral("o-preview"));
    env.settings.setValue(QString(keys::transfers::FirstLastDefault), true);
    auto *http = env.addPlugin(Net::Http, NetworkPlugin::CanFetchRanges);
    const QByteArray data = isoFile(40 * MiB, false, 79);   // moov at the end
    SourceConfig c;
    c.data = data;
    c.bytesPerSec = 6 << 20;
    c.maxPending = 1;
    const SearchSource a = http->addSource(QStringLiteral("https://a/clip.mp4"), c);
    env.engineUp();
    const QString name = QStringLiteral("Big Buck Bunny (été) clip.mp4");   // spaces and non-ASCII
    const QString id = env.engine->download(req(name, data, {a}));
    {
        const PreviewInfo pi = env.engine->previewInfo(id);
        QVERIFY(pi.supported);
        QCOMPARE(pi.files.size(), 1);
        QVERIFY(pi.files.first().media);
        QVERIFY(!pi.files.first().ready);
        QString err;
        QVERIFY(env.engine->preparePreview(id, -1, &err).isEmpty());
        QCOMPARE(err, QStringLiteral("Preview: needs the beginning of the file"));
    }
    // first and last: head band (4 MiB) then tail band: the moov box at the end arrives early
    QTRY_VERIFY_WITH_TIMEOUT(env.engine->previewInfo(id).files.value(0).ready, 15000);
    const PreviewFile pf = env.engine->previewInfo(id).files.first();
    qInfo("preview ready at %lld of %lld bytes (head %lld, tail %lld)", pf.done, pf.size, pf.headBytes, pf.tailBytes);
    QVERIFY(pf.done < data.size());
    QVERIFY(pf.tailBytes >= 64 * 1024);
    QString err;
    const QString path = env.engine->preparePreview(id, -1, &err);
    QVERIFY2(!path.isEmpty(), qPrintable(err));
    QCOMPARE(QFileInfo(path).fileName(), name);
    QVERIFY(path.contains(QStringLiteral("/incomplete/Preview/") + id + QLatin1Char('/')));
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.read(4 * MiB), data.left(4 * MiB));   // the beginning a player reads is the real data
    }
    QVERIFY2(env.waitDone(id, 30000), qPrintable(env.info(id).statusText));
    QCOMPARE(readFile(env.info(id).savePath), data);
    QVERIFY(!QFileInfo::exists(path));   // the link went with the completion
    // a file that is not audio or video is refused
    const QByteArray other = randomBytes(2 * MiB, 80);
    const SearchSource b = http->addSource(QStringLiteral("https://a/setup.exe"), c);
    const QString id2 = env.engine->download(req(QStringLiteral("setup.exe"), other, {b}));
    QVERIFY(env.engine->preparePreview(id2, -1, &err).isEmpty());
    QCOMPARE(err, QStringLiteral("Preview: not an audio or video file"));
    env.engine->cancel(id2, true);
}

void TstEngineOrder::previewReaderKeepsFileOpen()
{
    // a player keeps the preview open (read access, sharing read + write as players do, no delete
    // sharing): the download goes on writing, and completes once the player lets go (Windows) or at
    // once (POSIX); the reader sees the bytes written after it opened the file
    Env env(QStringLiteral("o-reader"));
    env.settings.setValue(QString(keys::transfers::SequentialDefault), true);
    auto *http = env.addPlugin(Net::Http, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(24 * MiB, 81);
    SourceConfig c;
    c.data = data;
    c.bytesPerSec = 3 << 20;
    c.maxPending = 1;
    const SearchSource a = http->addSource(QStringLiteral("https://a/song.mp3"), c);
    env.engineUp();
    const QString id = env.engine->download(req(QStringLiteral("song.mp3"), data, {a}));
    QTRY_VERIFY_WITH_TIMEOUT(env.engine->previewInfo(id).files.value(0).ready, 10000);
    QString err;
    const QString path = env.engine->preparePreview(id, -1, &err);
    QVERIFY2(!path.isEmpty(), qPrintable(err));
    QFile reader(path);
    QVERIFY2(reader.open(QIODevice::ReadOnly), qPrintable(reader.errorString()));
    QCOMPARE(reader.read(256 * 1024), data.left(256 * 1024));
    const qint64 doneAtOpen = env.engine->previewInfo(id).files.value(0).headBytes;
    // the writer is not locked out: more data arrives, and the open reader sees it
    QTRY_VERIFY_WITH_TIMEOUT(env.engine->previewInfo(id).files.value(0).headBytes > doneAtOpen + 4 * MiB, 10000);
    QVERIFY(reader.seek(doneAtOpen));
    QCOMPARE(reader.read(2 * MiB), data.mid(int(doneAtOpen), int(2 * MiB)));
    bool waited = false;
    QTRY_VERIFY_WITH_TIMEOUT((waited = env.info(id).statusText.contains(QStringLiteral("preview player"))) ||
                                 env.info(id).state == TransferState::Completed,
                             20000);
    qInfo("with the reader open: %s", qPrintable(env.info(id).statusText));
#ifdef Q_OS_WIN
    QVERIFY2(waited, "Windows: the rename must wait for the reader");
#endif
    reader.close();
    QVERIFY2(env.waitDone(id, 20000), qPrintable(env.info(id).statusText));
    QCOMPARE(readFile(env.info(id).savePath), data);
}

QTEST_GUILESS_MAIN(TstEngineOrder)
#include "tst_engine_order.moc"
