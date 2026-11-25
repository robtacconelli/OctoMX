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
    void checkAfterUncleanStop_data();
    void checkAfterUncleanStop();
    void checksTwoAtATime();
    void quitDuringCheck();
    void repairInPlaceCrash();
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
    // [step D] no block hashes (OctoMX, a whole-file checksum only): the check has nothing to read, the
    // flag goes at once and the current rule applies (the whole file is checked at completion)
    QVERIFY(QTest::qWaitFor([&] { return !env.engine->debugInfo(id).value(QStringLiteral("checkFirst")).toBool(); }, 10000));
    env.engineDown(true);   // the safe quit clears the mark
    QVERIFY(!QFileInfo::exists(mark));
    env.engineUp();
    QVERIFY(!env.engine->uncleanStart());
    QVERIFY(!env.engine->debugInfo(id).value(QStringLiteral("checkFirst")).toBool());
    // 24 MiB from a 1 MiB/s source: wide for a machine whose timers tick slowly
    QVERIFY2(env.waitDone(id, 300000), qPrintable(env.info(id).statusText));
    QCOMPARE(readFile(env.info(id).savePath), data);
}

namespace {
qint64 served(TestPlugin *p)
{
    qint64 n = 0;
    for (const auto &st : std::as_const(p->stats)) n += st->served;
    return n;
}
void corruptAt(const QString &path, qint64 off)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadWrite));
    QVERIFY(f.seek(off));
    f.write(QByteArray(4096, char(0)));   // what a disk that lied about a sync leaves: zeros
}
} // namespace

// [step D] after an unclean stop the download is verified before it runs or is shared: a part the
// disk lost (zeroed although the record claims it verified) is found and fetched again, nobody is
// blamed, there is no notice of its own, a paused download stays paused through its check
void TstEngineSafe::checkAfterUncleanStop()
{
    QFETCH(bool, paused);
    Env env(paused ? QStringLiteral("safe-d-paused") : QStringLiteral("safe-d"));
    auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges | NetworkPlugin::CanFindSources);
    const qint64 P = hash::Ed2kHasher::PartSize;
    const QByteArray data = randomBytes(4 * P + 777, 69);
    ed->hashSetOnFind << qMakePair(P, ed2kParts(data));
    SourceConfig c;
    c.data = data;
    c.bytesPerSec = 6 << 20;
    c.chunk = 128 * 1024;
    const SearchSource s = ed->addSource(QStringLiteral("ed2k://d"), c, QStringLiteral("30.0.0.1:4662"));
    env.engineUp();
    DownloadRequest r;
    r.name = QStringLiteral("d.bin");
    r.size = data.size();
    r.hashes = hashesOf(data, 2);
    r.sources = {s};
    const QString id = env.engine->download(r);
    // two parts verified and recorded
    QVERIFY(QTest::qWaitFor([&] {
        env.db->flush();
        DownloadsDao dao(env.db.get());
        for (const DownloadRecord &x : dao.all())
            if (x.id == id) {
                const QVariantMap m = QCborValue::fromCbor(x.engineState).toVariant().toMap();
                return engine::RangeSet::fromVariant(m.value(QStringLiteral("verified")).toList()).contains(0, 2 * P);
            }
        return false;
    }, 30000));
    if (paused) {
        env.engine->pause(id);
        QVERIFY(env.waitState(id, TransferState::Paused, 5000));
    }
    env.engineDown(false);   // a crash
    corruptAt(env.root + QStringLiteral("/incomplete/") + id + QStringLiteral(".part"), P + 1000);   // part 1, recorded verified
    const qint64 servedBefore = served(ed);
    env.engineUp();
    QSignalSpy notices(env.engine.get(), &TransferService::downloadNotice);
    QVERIFY(env.engine->uncleanStart());
    QVERIFY(env.engine->debugInfo(id).value(QStringLiteral("checkFirst")).toBool() || env.info(id).statusText.contains(QLatin1String("damaged")));
    QVERIFY(QTest::qWaitFor([&] { return !env.engine->debugInfo(id).value(QStringLiteral("checkFirst")).toBool(); }, 20000));
    qInfo("after the check: %s, done %lld", qPrintable(env.info(id).statusText), env.info(id).done);
    QCOMPARE(notices.count(), 0);   // the program says it once, not per download
    if (paused) {
        QTest::qWait(1500);
        QCOMPARE(env.info(id).state, TransferState::Paused);
        QCOMPARE(served(ed), servedBefore);   // nothing fetched while paused
        QVERIFY(env.info(id).done < 2 * P);    // the lost part is no longer counted
        env.engine->resume(id);
    }
    QVERIFY2(env.waitDone(id, 60000), qPrintable(env.info(id).statusText));
    QCOMPARE(readFile(env.info(id).savePath), data);
    for (const TransferSource &ts : env.info(id).sources)
        QVERIFY2(!ts.statusText.contains(QLatin1String("anned")) && !ts.statusText.contains(QLatin1String("Mismatch")), qPrintable(ts.statusText));
    QCOMPARE(notices.count(), 0);
}

void TstEngineSafe::checkAfterUncleanStop_data()
{
    QTest::addColumn<bool>("paused");
    QTest::newRow("running") << false;
    QTest::newRow("paused") << true;
}

// [step D] many downloads after an unclean stop: at most two checks at a time, the others say they wait
void TstEngineSafe::checksTwoAtATime()
{
    Env env(QStringLiteral("safe-d-many"));
    env.tuning.verifyBlockPauseMs = 400;   // a slow disk: each check takes a while
    auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges | NetworkPlugin::CanFindSources);
    const qint64 P = hash::Ed2kHasher::PartSize;
    QStringList ids;
    QList<QByteArray> datas;
    for (int i = 0; i < 4; ++i) {
        const QByteArray data = randomBytes(3 * P + 100 + i, 70 + i);
        datas << data;
        ed->hashSetOnFind << qMakePair(P, ed2kParts(data));
        SourceConfig c;
        c.data = data;
        c.bytesPerSec = 4 << 20;
        c.chunk = 128 * 1024;
        const SearchSource s = ed->addSource(QStringLiteral("ed2k://m%1").arg(i), c, QStringLiteral("30.0.0.%1:4662").arg(i + 10));
        DownloadRequest r;
        r.name = QStringLiteral("m%1.bin").arg(i);
        r.size = data.size();
        r.hashes = hashesOf(data, 2);
        r.sources = {s};
        r.startPaused = true;
        if (i == 0) env.engineUp();
        ids << env.engine->download(r);
    }
    // each holds data: resume, wait for some parts, pause again, then a crash
    for (const QString &id : std::as_const(ids)) env.engine->resume(id);
    QVERIFY(QTest::qWaitFor([&] {
        for (const QString &id : std::as_const(ids)) if (env.info(id).done < P) return false;
        return true;
    }, 30000));
    for (const QString &id : std::as_const(ids)) env.engine->pause(id);
    QTest::qWait(1500);   // synced and recorded
    for (const QString &id : std::as_const(ids)) QVERIFY(env.info(id).state == TransferState::Paused);
    env.engineDown(false);
    env.engineUp();
    int maxAtOnce = 0, waitingSeen = 0;
    QVERIFY(QTest::qWaitFor([&] {
        int verifying = 0, flagged = 0;
        for (const QString &id : std::as_const(ids)) {
            const TransferInfo t = env.info(id);
            const bool flag = env.engine->debugInfo(id).value(QStringLiteral("checkFirst")).toBool();
            if (flag) ++flagged;
            if (flag && t.state == TransferState::Verifying) ++verifying;
            if (flag && t.statusText.startsWith(QLatin1String("Paused, waiting to be checked"))) ++waitingSeen;
        }
        maxAtOnce = qMax(maxAtOnce, verifying);
        return flagged == 0;
    }, 60000));
    qInfo("at most %d checks at once; a waiting status seen %d times", maxAtOnce, waitingSeen);
    QVERIFY(maxAtOnce >= 1 && maxAtOnce <= 2);
    QVERIFY(waitingSeen > 0);
    for (const QString &id : std::as_const(ids)) QCOMPARE(env.info(id).state, TransferState::Paused);   // stayed paused
}

// [step D] a quit while a check runs: the quit does not wait for it, the check runs again next time
void TstEngineSafe::quitDuringCheck()
{
    Env env(QStringLiteral("safe-d-quit"));
    env.tuning.verifyBlockPauseMs = 2000;
    auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges | NetworkPlugin::CanFindSources);
    const qint64 P = hash::Ed2kHasher::PartSize;
    const QByteArray data = randomBytes(4 * P, 75);
    ed->hashSetOnFind << qMakePair(P, ed2kParts(data));
    SourceConfig c;
    c.data = data;
    c.chunk = 1 << 20;
    const SearchSource s = ed->addSource(QStringLiteral("ed2k://q"), c, QStringLiteral("30.0.0.1:4662"));
    env.engineUp();
    DownloadRequest r;
    r.name = QStringLiteral("q.bin");
    r.size = data.size();
    r.hashes = hashesOf(data, 2);
    r.sources = {s};
    const QString id = env.engine->download(r);
    QVERIFY(QTest::qWaitFor([&] { return env.info(id).done >= 3 * P; }, 30000));
    env.engine->pause(id);
    QTest::qWait(1500);
    env.engineDown(false);
    env.engineUp();
    QVERIFY(QTest::qWaitFor([&] { return env.info(id).state == TransferState::Verifying; }, 10000));
    QElapsedTimer t;
    t.start();
    env.engine->beginShutdown();
    QCOMPARE(env.engine->unsavedBytes(), qint64(0));   // a check writes nothing: the closing window does not wait for it
    env.engineDown(true);
    qInfo("quit during the check took %lld ms", t.elapsed());
    QVERIFY(t.elapsed() < 3000);
    env.engineUp();   // clean start: the check that did not end runs again
    QVERIFY(!env.engine->uncleanStart());
    QVERIFY(env.engine->debugInfo(id).value(QStringLiteral("checkFirst")).toBool());
    QVERIFY(QTest::qWaitFor([&] { return !env.engine->debugInfo(id).value(QStringLiteral("checkFirst")).toBool(); }, 60000));
    QCOMPARE(env.info(id).state, TransferState::Paused);
}

// [step D x ver-1] a finished file repaired in place, then a crash in the middle of the repair: the
// part store sits on the finished file in the incoming folder; the next start checks it and finishes
// the repair there (no copy, no "name (1)", nobody blamed)
void TstEngineSafe::repairInPlaceCrash()
{
    Env env(QStringLiteral("safe-d-repair"));
    auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges | NetworkPlugin::CanFindSources);
    const qint64 P = hash::Ed2kHasher::PartSize;
    const QByteArray data = randomBytes(4 * P + 11, 76);
    ed->hashSetOnFind << qMakePair(P, ed2kParts(data));
    SourceConfig c;
    c.data = data;
    c.chunk = 256 * 1024;
    const SearchSource s = ed->addSource(QStringLiteral("ed2k://r"), c, QStringLiteral("30.0.0.1:4662"));
    env.engineUp();
    DownloadRequest r;
    r.name = QStringLiteral("r.bin");
    r.size = data.size();
    r.hashes = hashesOf(data, 2);
    r.sources = {s};
    const QString id = env.engine->download(r);
    QVERIFY2(env.waitDone(id, 60000), qPrintable(env.info(id).statusText));
    const QString target = env.info(id).savePath;
    // two parts damaged on disk; a slow source for the repair
    corruptAt(target, 1000);
    corruptAt(target, 3 * P + 1000);
    env.engineDown(true);
    ed->configs[QStringLiteral("ed2k://r")].bytesPerSec = 2 << 20;
    env.engineUp();
    QVERIFY(env.engine->verifyDownload(id));
    QVERIFY(QTest::qWaitFor([&] { return !env.info(id).sources.isEmpty() && env.info(id).state == TransferState::Transferring; }, 20000));
    QTest::qWait(2500);   // a part of the repair written (and synced)
    env.engineDown(false);   // a crash in the middle of the repair
    env.engineUp();
    QVERIFY(env.engine->uncleanStart());
    QVERIFY2(env.waitDone(id, 60000), qPrintable(env.info(id).statusText));
    QCOMPARE(env.info(id).savePath, target);
    QCOMPARE(readFile(target), data);
    QCOMPARE(QDir(env.root + QStringLiteral("/incoming")).entryList(QDir::Files | QDir::Hidden), QStringList{QStringLiteral("r.bin")});
    QVERIFY(!env.engine->debugInfo(id).value(QStringLiteral("checkFirst")).toBool());
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
