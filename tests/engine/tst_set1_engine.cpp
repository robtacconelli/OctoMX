// SPDX-License-Identifier: GPL-3.0-or-later
// [set-1] The File Transfers settings the download engine reads, each with what the user sees on the
// Transfers page: where the files go, how many download at once, new downloads paused, the final
// check, the space reserved on disk, the free-space guard, sources per download, time outs, automatic
// retry, Auto Enter Queue, and the list cleared of finished downloads. Fake plugins (engine_fakes.h).
#include <QtCore/QDir>
#include <QtCore/QStorageInfo>
#include <QtTest/QTest>

#include "core/dao.h"
#include "engine_env.h"
#include "mxhash/ed2k.h"

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
} // namespace

class TstSet1Engine : public QObject {
    Q_OBJECT
private slots:
    // transfers/incomingDir, transfers/incompleteDir, transfers/startPaused, transfers/preallocate
    void foldersPausedAndReserved()
    {
        Env env(QStringLiteral("set1-folders"));
        const QString in = env.root + QStringLiteral("/my incoming"), part = env.root + QStringLiteral("/my parts");
        env.settings.setValue(QString(keys::transfers::IncomingDir), in);
        env.settings.setValue(QString(keys::transfers::IncompleteDir), part);
        env.settings.setValue(QString(keys::transfers::Preallocate), true);
        auto *http = env.addPlugin(Net::Http, NetworkPlugin::CanFetchRanges);
        const QByteArray data = randomBytes(3 * 1024 * 1024, 101);
        SourceConfig slow;
        slow.data = data;
        slow.bytesPerSec = 400 * 1024;
        const SearchSource s = http->addSource(QStringLiteral("http://a/ubuntu.iso"), slow);
        env.engineUp();
        const QString id = env.engine->download(req(QStringLiteral("ubuntu.iso"), data, hashesOf(data, 1), {s}));
        QTRY_VERIFY_WITH_TIMEOUT(env.info(id).done > 0, 10000);
        const QString partFile = part + QLatin1Char('/') + id + QStringLiteral(".part");
        QVERIFY2(QFile::exists(partFile), qPrintable(partFile));               // Incomplete files folder
        QCOMPARE(QFileInfo(partFile).size(), qint64(data.size()));             // reserved at once
        QVERIFY(env.waitDone(id, 30000));
        QVERIFY(env.info(id).savePath.startsWith(in + QLatin1Char('/')));       // Incoming files folder
        QCOMPARE(readFile(env.info(id).savePath), data);
        // without pre-allocation the part file grows with the data
        env.settings.setValue(QString(keys::transfers::Preallocate), false);
        const QByteArray d2 = randomBytes(3 * 1024 * 1024, 102);
        SourceConfig slow2 = slow;
        slow2.data = d2;
        const SearchSource s2 = http->addSource(QStringLiteral("http://a/debian.iso"), slow2);
        const QString id2 = env.engine->download(req(QStringLiteral("debian.iso"), d2, hashesOf(d2, 1), {s2}));
        QTRY_VERIFY_WITH_TIMEOUT(env.info(id2).done > 0, 10000);
        const QString part2 = part + QLatin1Char('/') + id2 + QStringLiteral(".part");
        QVERIFY(QFileInfo(part2).size() < qint64(d2.size()));
        QVERIFY(env.waitDone(id2, 30000));
        // Add new downloads paused: nothing is asked for until resumed
        env.settings.setValue(QString(keys::transfers::StartPaused), true);
        const QByteArray d3 = randomBytes(1024 * 1024, 103);
        SourceConfig fast;
        fast.data = d3;
        const SearchSource s3 = http->addSource(QStringLiteral("http://a/bunny.mkv"), fast);
        const QString id3 = env.engine->download(req(QStringLiteral("bunny.mkv"), d3, hashesOf(d3, 1), {s3}));
        QTest::qWait(1500);
        QCOMPARE(env.info(id3).state, TransferState::Paused);
        QCOMPARE(http->served(QStringLiteral("http://a/bunny.mkv")), qint64(0));
        env.engine->resume(id3);
        QVERIFY(env.waitDone(id3, 20000));
    }

    // transfers/maxDownloads: files that download at the same time, the others Locally Queued
    void downloadsAtOnce()
    {
        Env env(QStringLiteral("set1-slots"));
        env.settings.setValue(QString(keys::transfers::MaxDownloads), 1);
        auto *http = env.addPlugin(Net::Http, NetworkPlugin::CanFetchRanges);
        QStringList ids;
        env.engineUp();
        for (int i = 0; i < 3; ++i) {
            const QByteArray d = randomBytes(2 * 1024 * 1024, 110 + quint32(i));
            SourceConfig c;
            c.data = d;
            c.bytesPerSec = 200 * 1024;
            const SearchSource s = http->addSource(QStringLiteral("http://a/f%1.iso").arg(i), c);
            ids << env.engine->download(req(QStringLiteral("f%1.iso").arg(i), d, hashesOf(d, 1), {s}));
        }
        QTRY_VERIFY_WITH_TIMEOUT(env.info(ids.first()).done > 0, 10000);
        QTest::qWait(500);
        auto activeCount = [&] {
            int n = 0;
            for (const QString &id : ids) n += env.info(id).state != TransferState::Queued;
            return n;
        };
        QCOMPARE(activeCount(), 1);
        QCOMPARE(env.info(ids.at(2)).statusText, transferStateText(TransferState::Queued));
        env.settings.setValue(QString(keys::transfers::MaxDownloads), 3);
        // at once: long before the first file (ten seconds at this rate) could finish and free a slot
        QTRY_COMPARE_WITH_TIMEOUT(activeCount(), 3, 3000);
        for (const QString &id : ids) QVERIFY(env.info(id).state != TransferState::Completed);
    }

    // transfers/verifyCompleted: a file whose parts were all checked against the eD2k part hashes is
    // read again at the end only with the box ticked (the full check computes its other hashes)
    void finalCheck()
    {
        for (const bool verify : {true, false}) {
            Env env(verify ? QStringLiteral("set1-verify-on") : QStringLiteral("set1-verify-off"));
            env.settings.setValue(QString(keys::transfers::VerifyCompleted), verify);
            auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
            const QByteArray data = randomBytes(2 * P + 12345, verify ? 120 : 121);
            SourceConfig c;
            c.data = data;
            c.hashSet = ed2kParts(data);
            const SearchSource s = ed->addSource(QStringLiteral("ed2k://v"), c, QStringLiteral("42.0.0.1:4662"));
            env.engineUp();
            const QString id = env.engine->download(req(QStringLiteral("v.iso"), data, hashesOf(data, 2), {s}));
            QVERIFY2(env.waitDone(id, 60000), qPrintable(env.info(id).statusText));
            QCOMPARE(readFile(env.info(id).savePath), data);
            QCOMPARE(env.info(id).hashes.sha256.isEmpty(), !verify);
        }
    }

    // transfers/minFreeSpaceMB: downloads pause when the disk has less free space
    void freeSpaceGuard()
    {
        Env env(QStringLiteral("set1-space"));
        auto *http = env.addPlugin(Net::Http, NetworkPlugin::CanFetchRanges);
        const QByteArray d = randomBytes(4 * 1024 * 1024, 130);
        SourceConfig c;
        c.data = d;
        c.bytesPerSec = 200 * 1024;
        const SearchSource s = http->addSource(QStringLiteral("http://a/space.iso"), c);
        env.engineUp();
        const QString id = env.engine->download(req(QStringLiteral("space.iso"), d, hashesOf(d, 1), {s}));
        QTRY_VERIFY_WITH_TIMEOUT(env.info(id).done > 0, 10000);
        const QStorageInfo si(env.engine->incompleteDir());
        // [safe-1] wine reports no valid storage information for folders below this mount (available -1)
        if (!si.isValid() || si.bytesAvailable() < 0)
            QSKIP("the storage information of the incomplete folder is not valid here (wine, a path below a mount it does not know)");
        const qint64 freeMb = si.bytesAvailable() >> 20;
        QVERIFY(freeMb > 10);
        env.settings.setValue(QString(keys::transfers::MinFreeSpaceMB), int(qMin<qint64>(freeMb + 1000, 1048576)));
        QTRY_VERIFY_WITH_TIMEOUT(env.info(id).state == TransferState::Paused, 15000);
        QVERIFY2(env.info(id).statusText.contains(QStringLiteral("disk space"), Qt::CaseInsensitive) ||
                 env.info(id).diagnosis.contains(QStringLiteral("disk space"), Qt::CaseInsensitive), qPrintable(env.info(id).statusText));
        env.settings.setValue(QString(keys::transfers::MinFreeSpaceMB), 0);
        env.engine->resume(id);
        QVERIFY(env.waitDone(id, 120000));   // 4 MB at the mock's 200 KB/s, slower on a loaded machine
    }

    // transfers/maxSourcesPerDownload
    void sourcesPerDownload()
    {
        Env env(QStringLiteral("set1-sources"));
        env.settings.setValue(QString(keys::transfers::MaxSourcesPerDownload), 2);
        auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
        const QByteArray data = randomBytes(2 * P, 140);
        QList<SearchSource> srcs;
        for (int i = 0; i < 6; ++i) {
            SourceConfig c;
            c.data = data;
            c.bytesPerSec = 100 * 1024;
            srcs << ed->addSource(QStringLiteral("ed2k://s%1").arg(i), c, QStringLiteral("43.0.0.%1:4662").arg(i + 1));
        }
        env.engineUp();
        const QString id = env.engine->download(req(QStringLiteral("s.iso"), data, hashesOf(data, 2), srcs));
        QTRY_VERIFY_WITH_TIMEOUT(env.info(id).done > 0, 10000);
        QTest::qWait(1000);
        // connected at the same time: two; the other four are known and wait
        QCOMPARE(int(ed->stats.size()), 2);
        QCOMPARE(int(env.info(id).sources.size()), 6);
        env.settings.setValue(QString(keys::transfers::MaxSourcesPerDownload), 5);
        QTRY_COMPARE_WITH_TIMEOUT(int(ed->stats.size()), 5, 10000);
    }

    // transfers/pendingDownloadTimeoutSec: no source answers within the time: "Timed Out"
    void pendingTimeout()
    {
        Env env(QStringLiteral("set1-pending"));
        env.settings.setValue(QString(keys::transfers::PendingDownloadTimeoutSec), 10);
        env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges | NetworkPlugin::CanFindSources);
        env.engineUp();
        const QByteArray data = randomBytes(1024 * 1024, 150);
        const QString id = env.engine->download(req(QStringLiteral("nobody.iso"), data, hashesOf(data, 2), {}));
        QTest::qWait(5000);
        QVERIFY2(env.info(id).state != TransferState::TimedOut, qPrintable(env.info(id).statusText));
        QTRY_COMPARE_WITH_TIMEOUT(env.info(id).state, TransferState::TimedOut, 12000);
        QVERIFY(env.info(id).statusText.startsWith(QStringLiteral("Timed Out")));
        env.settings.setValue(QString(keys::transfers::PendingDownloadTimeoutSec), 300);
        env.engine->retry(id);
        QTest::qWait(12000);
        QVERIFY2(env.info(id).state != TransferState::TimedOut, qPrintable(env.info(id).statusText));
    }

    // transfers/autoRetry, transfers/retryIntervalSec, transfers/retryMaxAttempts: a download whose
    // every source failed is tried again after the interval, as often as set (it used to stay stopped)
    void automaticRetry()
    {
        Env env(QStringLiteral("set1-retry"));
        env.tuning.deadEndMs = 1000;
        env.tuning.sourceRetryBaseMs = 100;
        env.settings.setValue(QString(keys::transfers::AutoRetry), true);
        env.settings.setValue(QString(keys::transfers::RetryIntervalSec), 5);
        env.settings.setValue(QString(keys::transfers::RetryMaxAttempts), 1);
        auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
        const QByteArray data = randomBytes(1024 * 1024, 160);
        SourceConfig gone;
        gone.data = data;
        gone.failOpen = true;
        const SearchSource s = ed->addSource(QStringLiteral("ed2k://gone"), gone, QStringLiteral("44.0.0.1:4662"));
        env.engineUp();
        const QString id = env.engine->download(req(QStringLiteral("gone.iso"), data, hashesOf(data, 2), {s}));
        QVERIFY2(env.waitState(id, TransferState::Error, 60000), qPrintable(env.info(id).statusText));
        QVERIFY(env.info(id).statusText.startsWith(QStringLiteral("File/User Unavailable")));
        QElapsedTimer t;
        t.start();
        const int opened = int(ed->stats.size());
        QTRY_VERIFY_WITH_TIMEOUT(int(ed->stats.size()) > opened, 20000);   // tried again
        QVERIFY2(t.elapsed() >= 4000, qPrintable(QString::number(t.elapsed())));   // after the interval
        QVERIFY2(env.waitState(id, TransferState::Error, 60000), qPrintable(env.info(id).statusText));
        const int opened2 = int(ed->stats.size());
        QTest::qWait(8000);
        QCOMPARE(int(ed->stats.size()), opened2);                           // one attempt only
        QCOMPARE(env.info(id).state, TransferState::Error);
        // switched off: no retry at all
        env.settings.setValue(QString(keys::transfers::RetryMaxAttempts), 0);
        env.settings.setValue(QString(keys::transfers::AutoRetry), false);
        QTest::qWait(7000);
        QCOMPARE(int(ed->stats.size()), opened2);
    }

    // search/autoEnterQueue (the default of new downloads): a source that would put us further back
    // in its queue than this is left while it says so
    void autoEnterQueueDefault()
    {
        Env env(QStringLiteral("set1-aeq"));
        env.settings.setValue(QString(keys::search::AutoEnterQueue), 3);
        auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
        const QByteArray data = randomBytes(1024 * 1024, 170);
        SourceConfig c;
        c.data = data;
        const SearchSource s = ed->addSource(QStringLiteral("ed2k://q"), c, QStringLiteral("45.0.0.1:4662"));
        env.engineUp();
        const QString id = env.engine->download(req(QStringLiteral("q.iso"), data, hashesOf(data, 2), {s}));
        QCOMPARE(env.info(id).autoEnterQueue, 3);
        env.settings.setValue(QString(keys::search::AutoEnterQueue), 40);
        QTRY_COMPARE(env.info(id).autoEnterQueue, 40);   // follows the setting while not set per download
        QVERIFY(env.waitDone(id, 20000));
    }

    // transfers/autoClearCompleted, transfers/clearCompleteDownloadsSec
    void clearedWhenComplete()
    {
        Env env(QStringLiteral("set1-clear"));
        env.settings.setValue(QString(keys::transfers::AutoClearCompleted), true);
        env.settings.setValue(QString(keys::transfers::ClearCompleteDownloadsSec), 2);
        auto *http = env.addPlugin(Net::Http, NetworkPlugin::CanFetchRanges);
        env.engineUp();
        auto add = [&](const QString &name, quint32 seed) {
            const QByteArray d = randomBytes(512 * 1024, seed);
            SourceConfig c;
            c.data = d;
            const SearchSource s = http->addSource(QStringLiteral("http://a/") + name, c);
            return env.engine->download(req(name, d, hashesOf(d, 1), {s}));
        };
        auto listed = [&](const QString &id) {
            for (const TransferInfo &t : env.engine->downloads())
                if (t.id == id) return true;
            return false;
        };
        const QString a = add(QStringLiteral("clear-a.bin"), 180);
        QVERIFY(env.waitDone(a, 20000));
        QVERIFY(listed(a));
        QTest::qWait(800);
        QVERIFY(listed(a));                                   // still there before the time
        QTRY_VERIFY_WITH_TIMEOUT(!listed(a), 5000);
        QVERIFY(QFile::exists(env.root + QStringLiteral("/incoming/clear-a.bin")));   // the file stays
        env.settings.setValue(QString(keys::transfers::AutoClearCompleted), false);
        const QString b = add(QStringLiteral("clear-b.bin"), 181);
        QVERIFY(env.waitDone(b, 20000));
        QTest::qWait(3000);
        QVERIFY(listed(b));
    }
};

QTEST_GUILESS_MAIN(TstSet1Engine)
#include "tst_set1_engine.moc"
