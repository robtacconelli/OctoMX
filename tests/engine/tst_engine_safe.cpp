// SPDX-License-Identifier: GPL-3.0-or-later
// [safe-1] The rules that keep a download intact through any stop, one by one, in process:
//   * the record never claims a byte that is not synced to the disk (the sync schedule)
//   * a record that claims more than the part file holds is cut back before anything trusts it
//   * a move to the target folder that a stop interrupted is finished (the file arrived) or redone
//     (a half copy never carries the download's name)
//   * the unclean-stop mark: set while running, cleared by the safe quit; after an unclean stop the
//     downloads with data are marked "check before trusting", torrents are rechecked first
//   * the database: a damaged file is kept aside, the last good copy comes back
// The kills, power cuts and slow disks of a real process: tst_engine_crash.
#include <QtCore/QCborValue>
#include <QtTest/QTest>

#include "core/dao.h"
#include "core/engine/part_store.h"
#include "engine_env.h"
#include "mx/durable_file.h"

using namespace mx;
using namespace mx::etest;

namespace {
QString magnetFor(const QString &id, const QString &name) { return QStringLiteral("magnet:?xt=urn:btih:%1&dn=%2").arg(id, name); }

// the stored record of a download: held ranges of its engine state
engine::RangeSet storedHeld(Database *db, const QString &id)
{
    DownloadsDao dao(db);
    for (const DownloadRecord &r : dao.all()) {
        if (r.id != id) continue;
        const QVariantMap m = QCborValue::fromCbor(r.engineState).toVariant().toMap();
        return engine::RangeSet::fromVariant(m.value(QStringLiteral("held")).toList());
    }
    return {};
}

DownloadRecord storedRecord(Database *db, const QString &id)
{
    DownloadsDao dao(db);
    for (const DownloadRecord &r : dao.all())
        if (r.id == id) return r;
    return {};
}
} // namespace

class TstEngineSafe : public QObject {
    Q_OBJECT
private slots:
    void recordOnlyClaimsSyncedBytes();
    void recordLongerThanFile();
    void interruptedMoveFileArrived();
    void interruptedCopyRedone();
    void uncleanStopMark();
    void torrentRecheckedAfterUncleanStop();
    void databaseTornWrite();
};

void TstEngineSafe::recordOnlyClaimsSyncedBytes()
{
    Env env(QStringLiteral("safe-sync"));
    auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges | NetworkPlugin::CanFindSources);
    const QByteArray data = randomBytes(80 * 1024 * 1024 + 123, 61);
    ed->hashSetOnFind << qMakePair(qint64(hash::Ed2kHasher::PartSize), ed2kParts(data));
    SourceConfig c;
    c.data = data;
    c.bytesPerSec = 12 << 20;
    c.chunk = 256 * 1024;
    c.maxPending = 4;
    c.pref = 1 << 20;
    const SearchSource s = ed->addSource(QStringLiteral("ed2k://sync"), c, QStringLiteral("30.0.0.1:4662"));
    env.engineUp();
    DownloadRequest r;
    r.name = QStringLiteral("sync.bin");
    r.size = data.size();
    r.hashes = hashesOf(data, 2);
    r.sources = {s};
    const QString id = env.engine->download(r);
    // while it runs: every record stored so far claims only what was synced at that moment
    int samples = 0, ahead = 0;
    qint64 maxGap = 0;
    while (env.info(id).state != TransferState::Completed && samples < 400) {
        QTest::qWait(50);
        env.db->flush();
        const QVariantMap dbg = env.engine->debugInfo(id);
        if (!dbg.contains(QStringLiteral("syncedBytes"))) continue;
        const qint64 synced = dbg.value(QStringLiteral("syncedBytes")).toLongLong();
        const qint64 written = dbg.value(QStringLiteral("writtenBytes")).toLongLong();
        const qint64 claimed = storedHeld(env.db.get(), id).total();
        ++samples;
        if (claimed > synced) ++ahead;
        maxGap = qMax(maxGap, written - synced);
    }
    const QVariantMap dbg = env.engine->debugInfo(id);
    qInfo("%d samples; the record ahead of the synced bytes: %d times; written but not yet synced at most %lld bytes; %d syncs",
          samples, ahead, maxGap, dbg.value(QStringLiteral("syncs")).toInt());
    QCOMPARE(ahead, 0);
    QVERIFY2(maxGap <= engine::PartStore::kSyncBytes + (16 << 20), "the sync schedule fell behind");
    QVERIFY2(env.waitDone(id, 60000), qPrintable(env.info(id).statusText));
    QCOMPARE(readFile(env.info(id).savePath), data);
}

// the part file is shorter than its record claims (replaced, cut by a disk tool, restored from an
// old copy): the claims beyond its end are dropped and downloaded again
void TstEngineSafe::recordLongerThanFile()
{
    Env env(QStringLiteral("safe-short"));
    auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges | NetworkPlugin::CanFindSources);
    const QByteArray data = randomBytes(3 * hash::Ed2kHasher::PartSize + 999, 62);
    ed->hashSetOnFind << qMakePair(qint64(hash::Ed2kHasher::PartSize), ed2kParts(data));
    SourceConfig c;
    c.data = data;
    c.bytesPerSec = 8 << 20;
    c.chunk = 128 * 1024;
    const SearchSource s = ed->addSource(QStringLiteral("ed2k://short"), c, QStringLiteral("30.0.0.1:4662"));
    env.engineUp();
    DownloadRequest r;
    r.name = QStringLiteral("short.bin");
    r.size = data.size();
    r.hashes = hashesOf(data, 2);
    r.sources = {s};
    const QString id = env.engine->download(r);
    QVERIFY(QTest::qWaitFor([&] { return env.info(id).done >= 2 * hash::Ed2kHasher::PartSize; }, 30000));
    env.engine->pause(id);
    const QString part = env.root + QStringLiteral("/incomplete/") + id + QStringLiteral(".part");
    env.engineDown(true);
    QVERIFY(storedHeld(env.db.get(), id).total() >= 2 * hash::Ed2kHasher::PartSize);
    {
        QFile f(part);
        QVERIFY2(f.open(QIODevice::ReadWrite), qPrintable(part));
        QVERIFY(f.resize(hash::Ed2kHasher::PartSize / 2));
    }
    env.engineUp();
    env.engine->resume(id);
    QVERIFY2(env.waitDone(id, 60000), qPrintable(env.info(id).statusText));
    QCOMPARE(readFile(env.info(id).savePath), data);
}

// the stop came after the file reached the incoming folder, before the record said so
void TstEngineSafe::interruptedMoveFileArrived()
{
    Env env(QStringLiteral("safe-moved"));
    auto *ov = env.addPlugin(Net::Overlay, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(3 * 1024 * 1024 + 5, 63);
    SourceConfig c;
    c.data = data;
    const SearchSource s = ov->addSource(QStringLiteral("octomx://moved"), c, QStringLiteral("30.0.0.3:4700"));
    env.engineUp();
    DownloadRequest r;
    r.name = QStringLiteral("moved.bin");
    r.size = data.size();
    r.hashes = hashesOf(data, 1);
    r.sources = {s};
    const QString id = env.engine->download(r);
    QVERIFY2(env.waitDone(id, 30000), qPrintable(env.info(id).statusText));
    const QString target = env.info(id).savePath;
    env.engineDown(true);
    // the record as it was right before the move finished: not complete yet, everything held
    DownloadRecord rec = storedRecord(env.db.get(), id);
    rec.state = int(TransferState::Verifying);
    rec.statusText = QStringLiteral("Moving...");
    rec.savePath.clear();
    DownloadsDao(env.db.get()).upsert(rec);
    const QString part = env.root + QStringLiteral("/incomplete/") + id + QStringLiteral(".part");
    {
        QFile m(engine::PartStore::moveMarkerPath(part));
        QVERIFY(m.open(QIODevice::WriteOnly));
        m.write((target + QStringLiteral("\n") + target + QStringLiteral(".copy\n")).toUtf8());
    }
    env.engineUp();
    QVERIFY2(env.waitDone(id, 10000), qPrintable(env.info(id).statusText));
    QCOMPARE(env.info(id).savePath, target);
    QCOMPARE(readFile(target), data);
    QCOMPARE(QDir(env.root + QStringLiteral("/incoming")).entryList(QDir::Files | QDir::Hidden), QStringList{QStringLiteral("moved.bin")});
    QVERIFY(!QFileInfo::exists(engine::PartStore::moveMarkerPath(part)) || env.info(id).state == TransferState::Completed);
    env.engineDown(true);
    env.engineUp();   // the marker of a finished download goes at the next start
    QVERIFY(!QFileInfo::exists(engine::PartStore::moveMarkerPath(part)));
}

// the stop came in the middle of the copy to another volume: the half copy has a temporary name,
// it goes, the completion runs again
void TstEngineSafe::interruptedCopyRedone()
{
    Env env(QStringLiteral("safe-copy"));
    auto *ov = env.addPlugin(Net::Overlay, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(3 * 1024 * 1024 + 7, 64);
    SourceConfig c;
    c.data = data;
    const SearchSource s = ov->addSource(QStringLiteral("octomx://copy"), c, QStringLiteral("30.0.0.3:4700"));
    env.engineUp();
    DownloadRequest r;
    r.name = QStringLiteral("copy.bin");
    r.size = data.size();
    r.hashes = hashesOf(data, 1);
    r.sources = {s};
    const QString id = env.engine->download(r);
    QVERIFY2(env.waitDone(id, 30000), qPrintable(env.info(id).statusText));
    const QString target = env.info(id).savePath;
    env.engineDown(true);
    DownloadRecord rec = storedRecord(env.db.get(), id);
    rec.state = int(TransferState::Verifying);
    rec.savePath.clear();
    DownloadsDao(env.db.get()).upsert(rec);
    // the part file is still there, the copy got half way under its temporary name
    const QString part = env.root + QStringLiteral("/incomplete/") + id + QStringLiteral(".part");
    QVERIFY(QFile::rename(target, part));
    const QString tmp = durable::copyTempName(target);
    {
        QFile t(tmp);
        QVERIFY(t.open(QIODevice::WriteOnly));
        t.write(data.left(data.size() / 2));
        QFile m(engine::PartStore::moveMarkerPath(part));
        QVERIFY(m.open(QIODevice::WriteOnly));
        m.write((target + QStringLiteral("\n") + tmp + QStringLiteral("\n")).toUtf8());
    }
    env.engineUp();
    QVERIFY2(env.waitDone(id, 20000), qPrintable(env.info(id).statusText));
    QCOMPARE(env.info(id).savePath, target);
    QCOMPARE(readFile(target), data);
    QCOMPARE(QDir(env.root + QStringLiteral("/incoming")).entryList(QDir::Files | QDir::Hidden), QStringList{QStringLiteral("copy.bin")});
}

void TstEngineSafe::uncleanStopMark()
{
    Env env(QStringLiteral("safe-mark"));
    auto *ov = env.addPlugin(Net::Overlay, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(24 * 1024 * 1024, 65);
    SourceConfig c;
    c.data = data;
    c.bytesPerSec = 1 << 20;   // slow enough to be running at each restart below
    const SearchSource s = ov->addSource(QStringLiteral("octomx://mark"), c, QStringLiteral("30.0.0.3:4700"));
    const QString mark = DownloadEngine::runningMarkPath(env.root);
    env.engineUp();
    QVERIFY(QFileInfo::exists(mark));
    QVERIFY(!env.engine->uncleanStart());
    DownloadRequest r;
    r.name = QStringLiteral("mark.bin");
    r.size = data.size();
    r.hashes = hashesOf(data, 1);
    r.sources = {s};
    const QString id = env.engine->download(r);
    QVERIFY(QTest::qWaitFor([&] { return storedHeld(env.db.get(), id).total() > 0; }, 15000));
    env.engineDown(false);   // a kill: the mark stays
    QVERIFY(QFileInfo::exists(mark));
    env.engineUp();
    QVERIFY(env.engine->uncleanStart());
    QVERIFY(env.engine->debugInfo(id).value(QStringLiteral("checkFirst")).toBool());
    QVERIFY2(env.info(id).statusText.contains(QLatin1String("not closed properly")), qPrintable(env.info(id).statusText));
    env.engineDown(true);   // the safe quit clears the mark; the flag is stored with the download
    QVERIFY(!QFileInfo::exists(mark));
    env.engineUp();
    QVERIFY(!env.engine->uncleanStart());
    QVERIFY(env.engine->debugInfo(id).value(QStringLiteral("checkFirst")).toBool());
    // the whole-file check at the end is a check: the flag goes
    QVERIFY2(env.waitDone(id, 60000), qPrintable(env.info(id).statusText));
    QVERIFY(!env.engine->debugInfo(id).value(QStringLiteral("checkFirst")).toBool());
    QCOMPARE(readFile(env.info(id).savePath), data);
}

void TstEngineSafe::torrentRecheckedAfterUncleanStop()
{
    Env env(QStringLiteral("safe-recheck"));
    FakeTorrentBackend be;
    const QByteArray data = randomBytes(4 * 1024 * 1024, 66);
    const QString tid = be.makeTorrent(QStringLiteral("re.bin"), {{QStringLiteral("re.bin"), data}}, 64 * 1024);
    be.known[tid].peerPiecesPerTick = 1;
    be.known[tid].peers = 1;
    env.engineUp(&be);
    DownloadRequest r;
    r.name = QStringLiteral("re.bin");
    r.size = data.size();
    r.hashes.btih = QByteArray::fromHex(tid.toLatin1());
    r.uri = magnetFor(tid, r.name);
    const QString id = env.engine->download(r);
    QVERIFY(QTest::qWaitFor([&] { return env.info(id).done > 0; }, 15000));
    env.engineDown(false);
    QVERIFY(be.rechecks.isEmpty());
    env.engineUp(&be);
    QVERIFY(env.engine->uncleanStart());
    QVERIFY(QTest::qWaitFor([&] { return be.rechecks.contains(tid); }, 10000));
    QVERIFY2(env.waitDone(id, 60000), qPrintable(env.info(id).statusText));
    QVERIFY(!env.engine->debugInfo(id).value(QStringLiteral("checkFirst")).toBool());
}

void TstEngineSafe::databaseTornWrite()
{
    const QString dir = testRoot() + QStringLiteral("/safe-db");
    QDir(dir).removeRecursively();
    QDir().mkpath(dir);
    const QString path = dir + QStringLiteral("/octomx.db");
    {
        Database db;
        QVERIFY(db.open(path));
        DownloadsDao dao(&db);
        for (int i = 0; i < 20; ++i) {
            DownloadRecord r;
            r.id = QStringLiteral("d%1").arg(i);
            r.name = QStringLiteral("file %1.iso").arg(i);
            r.size = 1000 + i;
            QVERIFY(dao.upsert(r));
        }
        db.close();   // writes the last good copy
    }
    QVERIFY(QFileInfo::exists(path + QStringLiteral(".bak")));
    // a torn write: the first pages are garbage
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadWrite));
        f.write(randomBytes(8192, 67));
    }
    {
        Database db;
        QVERIFY(db.open(path));
        QCOMPARE(db.recovered(), Database::FromBackup);
        QCOMPARE(DownloadsDao(&db).all().size(), 20);
        QVERIFY(QFileInfo::exists(path + QStringLiteral(".bad")));
        db.close();
    }
    // a damaged page further in (the header still fine) is found by the check too
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadWrite));
        QVERIFY(f.size() > 8192);
        f.seek(f.size() - 2048);
        f.write(randomBytes(2048, 68));
    }
    {
        Database db;
        QVERIFY(db.open(path));
        QVERIFY(db.recovered() != Database::Fresh || DownloadsDao(&db).all().isEmpty());
        qInfo("a damaged page: recovered %d, %lld downloads", int(db.recovered()), qint64(DownloadsDao(&db).all().size()));
        db.close();
    }
    // both damaged: a new database, the program starts
    for (const QString &p : {path, path + QStringLiteral(".bak")}) {
        QFile f(p);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write("this is not a database");
    }
    Database db;
    QVERIFY(db.open(path));
    QCOMPARE(db.recovered(), Database::Fresh);
    QVERIFY(DownloadsDao(&db).all().isEmpty());
    db.close();
    QDir(dir).removeRecursively();
}

QTEST_GUILESS_MAIN(TstEngineSafe)
#include "tst_engine_safe.moc"
