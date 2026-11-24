// SPDX-License-Identifier: GPL-3.0-or-later
// [ver-1] "Verify File" and "Restart Download" in the download engine, each proven by its effect:
//  * a finished file with a few bytes changed on disk inside one block: exactly that block is fetched
//    again (the bytes the test source served = that block, not the file), the file ends right, at the
//    same path with the same name; for each hash scheme: eD2k parts, tiger tree leaves, the eD2k part
//    set an OctoMX network source sends, the block hashes a file keeps from its final check, torrent
//    pieces (fake swarm here; real libtorrent in tst_engine_bt); damage in the first, a middle and the
//    last (short) block;
//  * an intact file: nothing fetched, "intact", state as before;
//  * a paused partial download with a damaged held block: not held any more, still paused; resume
//    fetches it and the file completes right;
//  * a finished file deleted / truncated / grown: an honest answer, nothing fetched, row unchanged;
//  * only a whole-file checksum: block hashes from a source locate the damage; none: honest answer;
//  * no checksum at all: honest answer;
//  * Restart Download on a paused, a running, an errored and a finished download and on a bundle:
//    every byte fetched again, the old data in the (test) trash, the options kept;
//  * a program restart in the middle of a check and in the middle of a repair; Pause during a check.
#include <QtCore/QCborValue>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtTest/QTest>

#include "core/dao.h"
#include "engine_env.h"
#include "mx/trash.h"
#include "mxhash/ed2k.h"
#include "mxhash/tiger_tree.h"

using namespace mx;
using namespace mx::etest;

namespace {
constexpr qint64 P = hash::Ed2kHasher::PartSize;

DownloadRequest req(const QString &name, const QByteArray &data, const FileHashes &h, const QList<SearchSource> &src)
{
    DownloadRequest r;
    r.name = name;
    r.size = data.size();
    r.hashes = h;
    r.sources = src;
    return r;
}

// change n bytes of a file on disk at off (the disk's damage, not a peer's)
bool damage(const QString &path, qint64 off, int n = 3)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadWrite) || !f.seek(off)) return false;
    QByteArray b = f.read(n);
    for (char &c : b) c = char(c ^ 0x5a);
    return f.seek(off) && f.write(b) == b.size();
}

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

QStringList trashed()
{
    QStringList out;
    const QString dir = trash::seamDir();
    if (dir.isEmpty()) return out;
    QDirIterator it(dir, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) out << it.next();
    return out;
}

// the damage is the disk's: no source is struck or banned for it
bool nobodyBlamed(const QVariantMap &dbg)
{
    for (const QVariant &v : dbg.value(QStringLiteral("sources")).toList()) {
        const QVariantMap m = v.toMap();
        if (m.value(QStringLiteral("banned")).toBool() || m.value(QStringLiteral("strikes")).toInt() > 0) return false;
    }
    return true;
}

// a finished record of an older profile: no block hashes kept
void dropKeptBlockHashes(Env &env, const QString &id)
{
    DownloadsDao dao(env.db.get());
    DownloadRecord r;
    QVERIFY(dao.get(id, &r));
    QVariantMap m = QCborValue::fromCbor(r.engineState).toVariant().toMap();
    for (const char *k : {"tthLeaves", "tthUnit", "ed2kParts"}) m.remove(QLatin1String(k));
    r.engineState = QCborValue::fromVariant(m).toCbor();
    QVERIFY(dao.upsert(r));
    env.db->flush();
}
} // namespace

class TstEngineVerify : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { QVERIFY2(trash::redirected(), "MX_OS_ROOT must point into the build tree"); }
    void repairOneBlock_data();
    void repairOneBlock();
    void torrentPieceRepaired();
    void intactNothingFetched();
    void pausedPartialDamagedHeld();
    void finishedFileGoneOrChanged();
    void wholeFileChecksumOnly();
    void noChecksumAtAll();
    void restartDownloads();
    void restartTorrentBundle();
    void programRestartDuringCheckAndRepair();
    void pauseDuringCheck();
};

// ---- 1. one damaged block of a finished file, per hash scheme and place

void TstEngineVerify::repairOneBlock_data()
{
    QTest::addColumn<QString>("scheme");
    QTest::addColumn<int>("where");   // 0 first, 1 middle, 2 last (short)
    for (const char *s : {"ed2k", "tth", "octomx", "kept"})
        for (int w = 0; w < 3; ++w) QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(QLatin1String(s)).arg(w))) << QString::fromLatin1(s) << w;
}

void TstEngineVerify::repairOneBlock()
{
    QFETCH(QString, scheme);
    QFETCH(int, where);
    Env env(QStringLiteral("ver-%1-%2").arg(scheme).arg(where));
    const quint32 seed = 7000 + quint32(where) + quint32(qHash(scheme) % 100);
    QByteArray data;
    FileHashes h;
    SourceConfig c;
    TestPlugin *p = nullptr;
    QString uri;
    qint64 unit = 0;
    if (scheme == QLatin1String("ed2k") || scheme == QLatin1String("octomx")) {
        data = randomBytes(2 * P + 12345, seed);
        h = hashesOf(data, 2);
        unit = P;
        c.hashSet = ed2kParts(data);
        p = env.addPlugin(scheme == QLatin1String("ed2k") ? Net::ED2K : Net::Overlay, NetworkPlugin::CanFetchRanges);
        uri = scheme == QLatin1String("ed2k") ? QStringLiteral("ed2k://verify") : QStringLiteral("octomx://verify");
    } else if (scheme == QLatin1String("tth")) {
        data = randomBytes(1024 * 1024 + 5000, seed);
        unit = 64 * 1024;
        h.tth = tthRoot(data);
        h.sha1 = hashesOf(data, 1).sha1;
        c.hashSet = tthLeaves(data, unit);
        c.hashSetKind = QStringLiteral("tth-leaves");
        c.hashSetUnit = unit;
        p = env.addPlugin(Net::Gnutella, NetworkPlugin::CanFetchRanges);
        uri = QStringLiteral("gnutella://verify");
    } else {   // a sha1 only: the block hashes of its final check are kept (256 KiB)
        data = randomBytes(1024 * 1024 + 5000, seed);
        unit = 256 * 1024;
        h = hashesOf(data, 1);
        p = env.addPlugin(Net::Http, NetworkPlugin::CanFetchRanges);
        uri = QStringLiteral("http://192.0.2.10/verify.bin");
    }
    c.data = data;
    const SearchSource s = p->addSource(uri, c, QStringLiteral("198.51.100.7:4662"));
    env.engineUp();
    QSignalSpy notices(env.engine.get(), &TransferService::downloadNotice);
    const QString id = env.engine->download(req(QStringLiteral("Lantern %1.bin").arg(scheme), data, h, {s}));
    QVERIFY2(env.waitDone(id, 60000), qPrintable(env.info(id).statusText));
    const QString path = env.info(id).savePath;
    QCOMPARE(readFile(path), data);
    QCOMPARE(p->served(uri), qint64(data.size()));
    const qint64 nUnits = (data.size() + unit - 1) / unit;
    const qint64 blockStart = where == 0 ? 0 : where == 1 ? (nUnits / 2) * unit : (nUnits - 1) * unit;
    const qint64 blockEnd = qMin<qint64>(data.size(), blockStart + unit);
    QVERIFY(damage(path, blockStart + (blockEnd - blockStart) / 2));
    QVERIFY(env.engine->canVerifyDownload(id));
    QVERIFY(env.engine->verifyDownload(id));
    QTRY_VERIFY_WITH_TIMEOUT(!notices.isEmpty(), 30000);
    const QString text = notices.last().at(1).toString();
    QVERIFY2(text.contains(QLatin1String("damaged")) && text.contains(QLatin1String("being downloaded again")), qPrintable(text));
    QVERIFY2(text.startsWith(QLatin1String("Verified: 1 ")), qPrintable(text));
    QVERIFY(env.info(id).state != TransferState::Completed);
    QVERIFY2(env.waitDone(id, 60000), qPrintable(env.info(id).statusText));
    // exactly that block came again, the file is right, where it was, with its name
    QCOMPARE(p->served(uri) - data.size(), blockEnd - blockStart);
    QCOMPARE(env.info(id).savePath, path);
    QCOMPARE(readFile(path), data);
    QCOMPARE(QDir(QFileInfo(path).absolutePath()).entryList(QDir::Files).size(), 1);   // no "name (1)" copy
    QVERIFY(env.completed->size() >= 2);   // the completed state comes again
    QVERIFY(nobodyBlamed(env.engine->debugInfo(id)));
}

// ---- torrent pieces, fake swarm: libtorrent's recheck loses the damaged piece, the swarm sends it

void TstEngineVerify::torrentPieceRepaired()
{
    Env env(QStringLiteral("ver-torrent"));
    FakeTorrentBackend be;
    const QByteArray data = randomBytes(40 * 16384 + 777, 7400);
    const QString tid = be.makeTorrent(QStringLiteral("Lantern.mkv"), {{QStringLiteral("Lantern.mkv"), data}}, 16384);
    be.known[tid].peers = 3;
    be.known[tid].peerPiecesPerTick = 8;
    env.engineUp(&be);
    QSignalSpy notices(env.engine.get(), &TransferService::downloadNotice);
    QSignalSpy pieces(be.events, &TorrentEvents::torrentPieceFinished);
    const QString id = env.engine->downloadUri(QStringLiteral("magnet:?xt=urn:btih:%1&dn=Lantern.mkv").arg(tid));
    QVERIFY(env.waitDone(id, 30000));
    const QString path = QDir(env.root + QStringLiteral("/incoming")).filePath(QStringLiteral("Lantern.mkv"));
    QCOMPARE(readFile(path), data);
    pieces.clear();
    QVERIFY(damage(path, 20 * 16384 + 100));
    QVERIFY(env.engine->verifyDownload(id));
    QTRY_VERIFY_WITH_TIMEOUT(!notices.isEmpty(), 20000);
    const QString text = notices.last().at(1).toString();
    QVERIFY2(text.contains(QLatin1String("1 piece of")) && text.contains(QLatin1String("being downloaded again")), qPrintable(text));
    QVERIFY(env.waitDone(id, 30000));
    QCOMPARE(pieces.size(), 1);                    // only that piece came again
    QCOMPARE(pieces.first().at(1).toInt(), 20);
    QCOMPARE(readFile(path), data);
    // intact now: nothing more
    notices.clear();
    pieces.clear();
    QVERIFY(env.engine->verifyDownload(id));
    QTRY_VERIFY_WITH_TIMEOUT(!notices.isEmpty(), 20000);
    QCOMPARE(notices.last().at(1).toString(), QStringLiteral("Verified: 'Lantern.mkv' is intact."));
    QTest::qWait(600);
    QCOMPARE(pieces.size(), 0);
    QCOMPARE(env.info(id).state, TransferState::Completed);
}

// ---- 2. intact: nothing fetched, the answer, the state as before

void TstEngineVerify::intactNothingFetched()
{
    Env env(QStringLiteral("ver-intact"));
    auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(2 * P + 999, 7500);
    SourceConfig c;
    c.data = data;
    c.hashSet = ed2kParts(data);
    const SearchSource s = ed->addSource(QStringLiteral("ed2k://intact"), c, QStringLiteral("198.51.100.8:4662"));
    env.engineUp();
    QSignalSpy notices(env.engine.get(), &TransferService::downloadNotice);
    const QString id = env.engine->download(req(QStringLiteral("Intact.iso"), data, hashesOf(data, 2), {s}));
    QVERIFY(env.waitDone(id, 60000));
    const qint64 served = ed->served(QStringLiteral("ed2k://intact"));
    QVERIFY(env.engine->verifyDownload(id));
    QCOMPARE(env.info(id).state, TransferState::Verifying);
    QVERIFY(!env.engine->canVerifyDownload(id));   // one check at a time
    QTRY_VERIFY_WITH_TIMEOUT(!notices.isEmpty(), 30000);
    QCOMPARE(notices.last().at(1).toString(), QStringLiteral("Verified: 'Intact.iso' is intact."));
    QCOMPARE(env.info(id).state, TransferState::Completed);
    QVERIFY(env.info(id).statusText.contains(QLatin1String("verified intact")));
    QTest::qWait(500);
    QCOMPARE(ed->served(QStringLiteral("ed2k://intact")), served);
    QCOMPARE(env.completed->size(), 1);
}

// ---- 3. a paused partial download with a damaged held block

void TstEngineVerify::pausedPartialDamagedHeld()
{
    Env env(QStringLiteral("ver-paused"));
    auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(3 * P + 4321, 7600);
    SourceConfig c;
    c.data = data;
    c.hashSet = ed2kParts(data);
    c.avail = {{0, 2 * P}};   // the source has the first two parts for now
    const QString uri = QStringLiteral("ed2k://paused");
    const SearchSource s = ed->addSource(uri, c, QStringLiteral("198.51.100.9:4662"));
    env.engineUp();
    QSignalSpy notices(env.engine.get(), &TransferService::downloadNotice);
    const QString id = env.engine->download(req(QStringLiteral("Paused.iso"), data, hashesOf(data, 2), {s}));
    QTRY_COMPARE_WITH_TIMEOUT(env.info(id).done, 2 * P, 60000);
    env.engine->pause(id);
    QTRY_COMPARE_WITH_TIMEOUT(env.info(id).state, TransferState::Paused, 5000);
    const QString part = env.info(id).savePath;
    QVERIFY(part.endsWith(QLatin1String(".part")));
    QVERIFY(damage(part, P / 3));   // inside part 0, verified long ago
    QVERIFY(env.engine->verifyDownload(id));
    QTRY_VERIFY_WITH_TIMEOUT(!notices.isEmpty(), 30000);
    const QString text = notices.last().at(1).toString();
    QVERIFY2(text.contains(QLatin1String("1 part of 'Paused.iso'")) && text.contains(QLatin1String("when you resume it")), qPrintable(text));
    QCOMPARE(env.info(id).state, TransferState::Paused);
    QCOMPARE(env.info(id).done, P);   // part 0 is no longer held
    QVERIFY(env.info(id).statusText.contains(QLatin1String("damaged")));
    QTest::qWait(500);
    QCOMPARE(ed->served(uri), 2 * P);   // nothing fetched while paused
    ed->configs[uri].avail.clear();     // the source has it all now
    env.engine->resume(id);
    QVERIFY2(env.waitDone(id, 60000), qPrintable(env.info(id).statusText));
    QCOMPARE(readFile(env.info(id).savePath), data);
    QCOMPARE(ed->served(uri), qint64(data.size()) + P);   // part 0 once more, nothing else twice
}

// ---- 4. the finished file is gone, shorter or longer: say so, change nothing

void TstEngineVerify::finishedFileGoneOrChanged()
{
    for (int mode = 0; mode < 3; ++mode) {
        Env env(QStringLiteral("ver-gone-%1").arg(mode));
        auto *http = env.addPlugin(Net::Http, NetworkPlugin::CanFetchRanges);
        const QByteArray data = randomBytes(700 * 1024, 7700 + quint32(mode));
        SourceConfig c;
        c.data = data;
        const QString uri = QStringLiteral("http://192.0.2.11/gone%1.bin").arg(mode);
        const SearchSource s = http->addSource(uri, c);
        env.engineUp();
        QSignalSpy notices(env.engine.get(), &TransferService::downloadNotice);
        const QString id = env.engine->download(req(QStringLiteral("Gone%1.bin").arg(mode), data, hashesOf(data, 1), {s}));
        QVERIFY(env.waitDone(id, 30000));
        const QString path = env.info(id).savePath;
        if (mode == 0) {
            QVERIFY(QFile::remove(path));
        } else {
            QFile f(path);
            QVERIFY(f.open(QIODevice::ReadWrite));
            if (mode == 1) QVERIFY(f.resize(data.size() - 1000));
            else { f.seek(f.size()); f.write("tail added by the user"); }
        }
        const QByteArray before = readFile(path);
        QVERIFY(env.engine->verifyDownload(id));
        QTRY_VERIFY_WITH_TIMEOUT(!notices.isEmpty(), 10000);
        const QString text = notices.last().at(1).toString();
        QVERIFY2(text.startsWith(QLatin1String("Cannot verify 'Gone")) && text.contains(QLatin1String("Restart Download")), qPrintable(text));
        QVERIFY2(text.contains(mode == 0 ? QLatin1String("no longer at") : QLatin1String("was changed")), qPrintable(text));
        QTest::qWait(500);
        QCOMPARE(http->served(uri), qint64(data.size()));
        QCOMPARE(env.info(id).state, TransferState::Completed);
        QCOMPARE(readFile(path), before);
        QCOMPARE(QFileInfo::exists(path), mode != 0);
    }
}

// ---- 5. only a whole-file checksum (an older record): block hashes from a source; and none

void TstEngineVerify::wholeFileChecksumOnly()
{
    for (const bool setAvailable : {true, false}) {
        Env env(setAvailable ? QStringLiteral("ver-whole-set") : QStringLiteral("ver-whole-none"));
        env.tuning.verifyHashSetWaitMs = 4000;
        auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
        const QByteArray data = randomBytes(2 * P + 54321, setAvailable ? 7800 : 7801);
        SourceConfig c;
        c.data = data;
        const QString uri = QStringLiteral("ed2k://whole");
        const SearchSource s = ed->addSource(uri, c, QStringLiteral("198.51.100.10:4662"));
        env.engineUp();
        // the download had only the file's eD2k hash (no part hash set came)
        FileHashes h = hashesOf(data, 2);
        const QString id = env.engine->download(req(QStringLiteral("Whole.iso"), data, h, {s}));
        QVERIFY(env.waitDone(id, 60000));
        const QString path = env.info(id).savePath;
        env.engineDown(true);
        dropKeptBlockHashes(env, id);
        if (setAvailable) ed->configs[uri].hashSet = ed2kParts(data);   // a source can send the set now
        env.engineUp();
        QSignalSpy notices(env.engine.get(), &TransferService::downloadNotice);
        const qint64 served = ed->served(uri);
        QVERIFY(damage(path, P + 777));   // in part 1
        QVERIFY(env.engine->verifyDownload(id));
        QTRY_VERIFY_WITH_TIMEOUT(!notices.isEmpty(), 30000);
        QVERIFY2(notices.last().at(1).toString().contains(QLatin1String("cannot be located yet")), qPrintable(notices.last().at(1).toString()));
        if (setAvailable) {
            QVERIFY2(env.waitDone(id, 60000), qPrintable(env.info(id).statusText));
            QCOMPARE(readFile(path), data);
            QCOMPARE(ed->served(uri) - served, P);   // only part 1
            QCOMPARE(env.info(id).savePath, path);
        } else {
            QTRY_VERIFY_WITH_TIMEOUT(notices.size() >= 2, 20000);
            const QString text = notices.last().at(1).toString();
            QVERIFY2(text.contains(QLatin1String("no network gave block hashes")) && text.contains(QLatin1String("Restart Download")), qPrintable(text));
            QCOMPARE(env.info(id).state, TransferState::Completed);
            QVERIFY(env.info(id).statusText.contains(QLatin1String("damaged")));
            QCOMPARE(ed->served(uri), served);
            QVERIFY(readFile(path) != data);   // nothing changed in the file
        }
    }
}

// ---- 6. no checksum of any kind (a plain HTTP link)

void TstEngineVerify::noChecksumAtAll()
{
    Env env(QStringLiteral("ver-nosum"));
    auto *http = env.addPlugin(Net::Http, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(300 * 1024, 7900);
    SourceConfig c;
    c.data = data;
    const QString uri = QStringLiteral("http://192.0.2.12/plain.bin");
    http->addSource(uri, c);
    env.engineUp();
    QSignalSpy notices(env.engine.get(), &TransferService::downloadNotice);
    DownloadRequest r;
    r.uri = uri;
    r.name = QStringLiteral("plain.bin");
    r.size = data.size();
    SearchSource s;
    s.net = Net::Http;
    s.uri = uri;
    r.sources = {s};
    const QString id = env.engine->download(r);
    QVERIFY(env.waitDone(id, 30000));
    QVERIFY(env.engine->verifyDownload(id));
    QTRY_VERIFY_WITH_TIMEOUT(!notices.isEmpty(), 10000);
    QCOMPARE(notices.last().at(1).toString(), QStringLiteral("'plain.bin' cannot be verified: no checksum is known for this file."));
    QCOMPARE(env.info(id).state, TransferState::Completed);
    QCOMPARE(http->served(uri), qint64(data.size()));
}

// ---- 8. Restart Download: paused, running, errored, finished

void TstEngineVerify::restartDownloads()
{
    Env env(QStringLiteral("ver-restart"));
    auto *http = env.addPlugin(Net::Http, NetworkPlugin::CanFetchRanges);
    env.engineUp();
    QSignalSpy notices(env.engine.get(), &TransferService::downloadNotice);
    struct Case { QString name; int how; QByteArray data; QString uri; QString id; };   // how: 0 paused 1 running 2 errored 3 finished
    QList<Case> cases;
    for (int how = 0; how < 4; ++how) {
        Case k;
        k.how = how;
        k.name = QStringLiteral("Restart%1.bin").arg(how);
        k.data = randomBytes(2 * 1024 * 1024 + 333, 8000 + quint32(how));
        k.uri = QStringLiteral("http://192.0.2.13/restart%1.bin").arg(how);
        SourceConfig c;
        c.data = k.data;
        c.bytesPerSec = how <= 1 ? 300 * 1024 : 0;
        c.corruptAll = how == 2;   // every byte it sends is wrong: an error after the whole-file check
        http->addSource(k.uri, c);
        SearchSource s;
        s.net = Net::Http;
        s.uri = k.uri;
        k.id = env.engine->download(req(k.name, k.data, hashesOf(k.data, 1), {s}));
        env.engine->setPriority(k.id, 1);
        env.engine->setSequentialDownload(k.id, true);
        env.engine->setAutoFindSources(k.id, 25);
        cases << k;
    }
    QTRY_VERIFY_WITH_TIMEOUT(http->served(cases[0].uri) > 300 * 1024, 20000);
    env.engine->pause(cases[0].id);
    QTRY_VERIFY_WITH_TIMEOUT(http->served(cases[1].uri) > 300 * 1024, 20000);
    QTRY_VERIFY_WITH_TIMEOUT(env.info(cases[2].id).state == TransferState::Error || env.info(cases[2].id).state == TransferState::Paused, 60000);
    QVERIFY(env.waitDone(cases[3].id, 30000));
    const QString finishedPath = env.info(cases[3].id).savePath;
    const QString partOf0 = env.info(cases[0].id).savePath;
    QVERIFY(QFile::exists(partOf0));
    const QStringList trashBefore = trashed();
    QHash<QString, qint64> servedBefore;
    for (const Case &k : std::as_const(cases)) servedBefore.insert(k.uri, http->served(k.uri));
    http->configs[cases[2].uri].corruptAll = false;   // the errored one's source is fine now
    for (const Case &k : std::as_const(cases)) {
        QVERIFY(env.engine->canRestartDownload(k.id));
        QVERIFY(env.engine->restartDownload(k.id));
    }
    // same rows, every one running from zero (the paused one too), options kept
    QCOMPARE(env.engine->downloads().size(), 4);
    for (const Case &k : std::as_const(cases)) {
        const TransferInfo t = env.info(k.id);
        QCOMPARE(t.id, k.id);
        QVERIFY2(t.state != TransferState::Paused && t.state != TransferState::Completed, qPrintable(t.statusText));
        QCOMPARE(t.priority, 1);
        QVERIFY(t.sequential);
        QCOMPARE(t.autoFindSourcesMin, 25);
    }
    QTRY_VERIFY_WITH_TIMEOUT(notices.size() >= 4, 10000);
    // the old data is in the trash: the finished file and the partial files
    QTRY_VERIFY_WITH_TIMEOUT(trashed().size() >= trashBefore.size() + 3, 5000);
    for (const Case &k : std::as_const(cases)) {
        QVERIFY2(env.waitDone(k.id, 90000), qPrintable(k.name + QLatin1Char(' ') + env.info(k.id).statusText));
        QCOMPARE(readFile(env.info(k.id).savePath), k.data);
        // every byte fetched again (nothing of the old data was reused)
        QVERIFY2(http->served(k.uri) - servedBefore.value(k.uri) >= k.data.size(), qPrintable(k.name));
    }
    QCOMPARE(env.info(cases[3].id).savePath, finishedPath);   // the old one went to the trash: no "(1)"
}

// ---- Restart Download of a torrent bundle (fake swarm): its own files to the trash, all pieces again

void TstEngineVerify::restartTorrentBundle()
{
    Env env(QStringLiteral("ver-restart-bundle"));
    FakeTorrentBackend be;
    const QByteArray a = randomBytes(10 * 16384 + 5, 8100), b = randomBytes(6 * 16384 + 9, 8101), extra = randomBytes(3 * 16384, 8102);
    const QString tid = be.makeTorrent(QStringLiteral("Lantern Pack"), {{QStringLiteral("one.bin"), a}, {QStringLiteral("two.bin"), b},
                                                                        {QStringLiteral("extra.bin"), extra}}, 16384);
    be.known[tid].peers = 2;
    be.known[tid].peerPiecesPerTick = 4;
    env.engineUp(&be);
    QSignalSpy pieces(be.events, &TorrentEvents::torrentPieceFinished);
    DownloadRequest r;
    r.uri = QStringLiteral("magnet:?xt=urn:btih:%1&dn=Lantern%20Pack").arg(tid);
    r.wantedFiles = {0, 1};
    const QString id = env.engine->download(r);
    QVERIFY(env.waitDone(id, 30000));
    const QString dir = env.root + QStringLiteral("/incoming/Lantern Pack");
    QCOMPARE(readFile(dir + QStringLiteral("/one.bin")), a);
    QCOMPARE(readFile(dir + QStringLiteral("/two.bin")), b);
    const int firstRound = int(pieces.size());
    pieces.clear();
    const QStringList trashBefore = trashed();
    QVERIFY(env.engine->restartDownload(id));
    QTRY_VERIFY_WITH_TIMEOUT(trashed().size() > trashBefore.size(), 10000);
    QVERIFY(env.waitDone(id, 30000));
    QCOMPARE(int(pieces.size()), firstRound);   // every wanted piece again
    QCOMPARE(readFile(dir + QStringLiteral("/one.bin")), a);
    QCOMPARE(readFile(dir + QStringLiteral("/two.bin")), b);
    QCOMPARE(be.active.value(tid).wanted, (QList<int>{0, 1}));   // the wanted files are kept
}

// ---- 9. the program ends in the middle of a check, and in the middle of a repair

void TstEngineVerify::programRestartDuringCheckAndRepair()
{
    Env env(QStringLiteral("ver-crash"));
    env.tuning.verifyBlockPauseMs = 150;   // the check takes a few seconds
    auto *gn = env.addPlugin(Net::Gnutella, NetworkPlugin::CanFetchRanges);
    const qint64 unit = 64 * 1024;
    const QByteArray data = randomBytes(40 * unit + 100, 8200);
    FileHashes h;
    h.tth = tthRoot(data);
    h.sha1 = hashesOf(data, 1).sha1;
    SourceConfig c;
    c.data = data;
    c.hashSet = tthLeaves(data, unit);
    c.hashSetKind = QStringLiteral("tth-leaves");
    c.hashSetUnit = unit;
    const QString uri = QStringLiteral("gnutella://crash");
    const SearchSource s = gn->addSource(uri, c, QStringLiteral("198.51.100.11:6346"));
    env.engineUp();
    const QString id = env.engine->download(req(QStringLiteral("Crash.bin"), data, h, {s}));
    QVERIFY(env.waitDone(id, 60000));
    const QString path = env.info(id).savePath;
    // killed while checking: the record is the finished one, nothing else changed
    QVERIFY(damage(path, 30 * unit + 10));
    QVERIFY(env.engine->verifyDownload(id));
    QTRY_VERIFY_WITH_TIMEOUT(env.info(id).statusText.contains(QLatin1String("Verifying 1")), 10000);
    env.engineDown(false);
    env.engineUp();
    QCOMPARE(env.info(id).state, TransferState::Completed);
    QCOMPARE(env.info(id).savePath, path);
    // killed while repairing: the next session goes on with it, the intact blocks stay
    gn->configs[uri].bytesPerSec = 8 * 1024;   // the repair takes a while
    QSignalSpy notices(env.engine.get(), &TransferService::downloadNotice);
    const qint64 served = gn->served(uri);
    QVERIFY(env.engine->verifyDownload(id));
    QTRY_VERIFY_WITH_TIMEOUT(!notices.isEmpty(), 30000);
    QTRY_VERIFY_WITH_TIMEOUT(gn->served(uri) > served, 20000);
    env.engineDown(false);
    gn->configs[uri].bytesPerSec = 0;
    env.engineUp();
    QVERIFY(env.info(id).state != TransferState::Completed);
    QVERIFY2(env.waitDone(id, 60000), qPrintable(env.info(id).statusText));
    QCOMPARE(readFile(path), data);
    QCOMPARE(env.info(id).savePath, path);
    QVERIFY2(gn->served(uri) - served <= 2 * unit, qPrintable(QString::number(gn->served(uri) - served)));   // that block, at most twice
}

// ---- 10. Pause during a check: what was read has its result, the rest its old status

void TstEngineVerify::pauseDuringCheck()
{
    Env env(QStringLiteral("ver-pause"));
    env.tuning.verifyBlockPauseMs = 150;
    auto *gn = env.addPlugin(Net::Gnutella, NetworkPlugin::CanFetchRanges);
    const qint64 unit = 64 * 1024;
    const QByteArray data = randomBytes(40 * unit + 200, 8300);
    FileHashes h;
    h.tth = tthRoot(data);
    SourceConfig c;
    c.data = data;
    c.hashSet = tthLeaves(data, unit);
    c.hashSetKind = QStringLiteral("tth-leaves");
    c.hashSetUnit = unit;
    const QString uri = QStringLiteral("gnutella://pause");
    const SearchSource s = gn->addSource(uri, c, QStringLiteral("198.51.100.12:6346"));
    env.engineUp();
    QSignalSpy notices(env.engine.get(), &TransferService::downloadNotice);
    const QString id = env.engine->download(req(QStringLiteral("Pause.bin"), data, h, {s}));
    QVERIFY(env.waitDone(id, 60000));
    const QString path = env.info(id).savePath;
    QVERIFY(damage(path, unit + 5));        // block 1: found before the pause
    QVERIFY(damage(path, 38 * unit + 5));   // block 38: not read before the pause
    QVERIFY(env.engine->verifyDownload(id));
    QTRY_VERIFY_WITH_TIMEOUT(env.info(id).statusText.contains(QLatin1String("damaged so far")), 10000);
    env.engine->pause(id);
    QTRY_VERIFY_WITH_TIMEOUT(!notices.isEmpty(), 5000);
    const QString text = notices.last().at(1).toString();
    QVERIFY2(text.startsWith(QLatin1String("Verification of 'Pause.bin' stopped: 1 block")), qPrintable(text));
    QCOMPARE(env.info(id).state, TransferState::Paused);
    const qint64 served = gn->served(uri);
    QTest::qWait(500);
    QCOMPARE(gn->served(uri), served);
    // resumed: the damaged block comes again; the blocks the check did not reach are checked as the
    // download's own (held, not verified yet): block 38 fails there and comes again too
    env.engine->resume(id);
    QVERIFY2(env.waitDone(id, 60000), qPrintable(env.info(id).statusText));
    QCOMPARE(readFile(path), data);
    QCOMPARE(gn->served(uri) - served, 2 * unit);
    QVERIFY(nobodyBlamed(env.engine->debugInfo(id)));
}

QTEST_GUILESS_MAIN(TstEngineVerify)
#include "tst_engine_verify.moc"
