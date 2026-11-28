// SPDX-License-Identifier: GPL-3.0-or-later
// [api-2] Removing a download that is finishing, with the step held by the PartStore test seam (no
// sleeping and hoping): during the final check the check is cancelled and the download is removed as
// a partial one (keep: a kept record whose part file exists, nothing in the incoming folder; delete:
// the part file goes); during the move to the incoming folder the move is never cut and the removal
// runs when it ends: the download is removed as a finished one (keep: the finished file stays, no kept
// record; delete: the finished file is what goes). Never a kept record naming a file that is gone,
// never a file in the incoming folder that no list knows, never a move marker left, and the outcome
// signals name the real paths.
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QElapsedTimer>
#include <QtCore/QThread>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

#include <atomic>

#include "core/engine/download.h"
#include "core/engine/part_store.h"
#include "engine_env.h"
#include "mx/trash.h"

using namespace mx;
using namespace mx::etest;

namespace {

// engine_engine_remove_finishing and its _copy twin run this program at the same time (ctest -j):
// each has data folders of its own
QString variant()
{
    return qEnvironmentVariableIntValue("MX_TEST_COPY_MOVE") > 0 ? QStringLiteral("-copy") : QString();
}

QStringList filesBelow(const QString &dir)
{
    QStringList out;
    QDirIterator it(dir, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) out << it.next();
    out.sort();
    return out;
}

// holds one step of the store's I/O thread ("final check" or "move") until released; a held final
// check also lets go when its cancel flag is set (as the hashing does)
struct Hold {
    QString step;
    std::atomic<bool> entered{false}, release{false}, cancelled{false};
    std::function<void()> onRelease;   // runs on the I/O thread when released, before the step goes on
    explicit Hold(const QString &s) : step(s)
    {
        engine::PartStore::setTestHold([this](const QString &st, const std::atomic<bool> *cancel) {
            if (st != step || entered.exchange(true)) return;
            const auto go = [this] { if (onRelease) onRelease(); };
            while (!release.load()) {
                if (cancel && cancel->load()) {
                    cancelled = true;
                    return;
                }
                QThread::msleep(2);
            }
            go();
        });
    }
    ~Hold()
    {
        release = true;
        engine::PartStore::setTestHold({});
    }
};

} // namespace

class TstEngineRemoveFinishing : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void inTheFinalCheck_data();
    void inTheFinalCheck();
    void inTheMove_data();
    void inTheMove();
    void moveFailsWhileRemoved_data();
    void moveFailsWhileRemoved();
    void quitWhileRemoved_data();
    void quitWhileRemoved();

private:
    // a download of `data` from one fast eD2k source, up to the held step
    QString startAndHold(Env &env, const QByteArray &data, Hold &hold, SearchSource *src);
};

void TstEngineRemoveFinishing::initTestCase()
{
    const QString root = testRoot() + QStringLiteral("/remove-finishing-os-root") + variant();
    QDir(root).removeRecursively();
    qputenv("MX_OS_ROOT", root.toUtf8());
    qunsetenv("MX_TRASH");
    QVERIFY(trash::redirected());
}

QString TstEngineRemoveFinishing::startAndHold(Env &env, const QByteArray &data, Hold &hold, SearchSource *src)
{
    auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
    SourceConfig c;
    c.data = data;
    *src = ed->addSource(QStringLiteral("ed2k://finishing"), c, QStringLiteral("198.51.100.90:4662"));
    env.settings.setValue(QString(keys::transfers::VerifyCompleted), true);
    env.engineUp();
    DownloadRequest r;
    r.name = QStringLiteral("debian-12.8.0-amd64-netinst.iso");
    r.size = data.size();
    r.hashes = hashesOf(data, 3);
    r.sources = {*src};
    const QString id = env.engine->download(r);
    if (!QTest::qWaitFor([&] { return hold.entered.load(); }, 30000)) return QString();
    return id;
}

void TstEngineRemoveFinishing::inTheFinalCheck_data()
{
    QTest::addColumn<int>("mode");
    QTest::newRow("keep") << int(FileRemoval::Keep);
    QTest::newRow("delete") << int(FileRemoval::Delete);
    QTest::newRow("trash") << int(FileRemoval::Trash);
}

// the final check is held: the removal cancels it (no long wait), the download goes as a partial one
void TstEngineRemoveFinishing::inTheFinalCheck()
{
    QFETCH(int, mode);
    Env env(QStringLiteral("rmf-check-%1").arg(mode) + variant());
    const QByteArray data = randomBytes(3 * 1024 * 1024 + 17, 900 + quint32(mode));
    Hold hold(QStringLiteral("final check"));
    SearchSource src;
    const QString id = startAndHold(env, data, hold, &src);
    QVERIFY(!id.isEmpty());
    QCOMPARE(env.info(id).state, TransferState::Verifying);
    const QString part = env.engine->incompleteDir() + QLatin1Char('/') + id + QStringLiteral(".part");
    const QString incoming = env.engine->incomingDir();
    QVERIFY(QFileInfo::exists(part));
    QSignalSpy removed(env.engine.get(), &DownloadEngine::downloadRemoved);
    QSignalSpy deleted(env.engine.get(), &TransferService::downloadFilesDeleted);
    QSignalSpy rows(env.engine.get(), &TransferService::transferRemoved);
    QElapsedTimer t;
    t.start();
    env.engine->removeDownload(id, FileRemoval(mode));
    const qint64 took = t.elapsed();
    qInfo("removed in its final check in %lld ms", took);
    QVERIFY(took < DownloadEngine::kRemoveCheckWaitMs);   // the check stopped on its cancel flag
    QVERIFY(hold.cancelled.load());
    QCOMPARE(rows.count(), 1);
    QCOMPARE(removed.count(), 1);
    QCOMPARE(removed.at(0).at(0).toString(), id);
    QCOMPARE(removed.at(0).at(1).toBool(), false);   // a partial download
    QVERIFY(env.engine->downloads().isEmpty());
    if (FileRemoval(mode) == FileRemoval::Keep) {
        QCOMPARE(removed.at(0).at(2).toStringList(), QStringList{part});
        QVERIFY(QFileInfo::exists(part));
        QCOMPARE(env.engine->keptDownloads().size(), 1);
        QCOMPARE(QDir::cleanPath(env.engine->keptDownloads().first().savePath), QDir::cleanPath(part));
    } else {
        QVERIFY(removed.at(0).at(2).toStringList().isEmpty());
        QVERIFY(QTest::qWaitFor([&] { return deleted.count() == 1; }, 10000));
        const QStringList gone = deleted.at(0).at(2).toStringList() + deleted.at(0).at(3).toStringList();   // trashed + deleted
        QCOMPARE(gone, QStringList{part});
        QVERIFY(deleted.at(0).at(4).toStringList().isEmpty());
        QVERIFY(!QFileInfo::exists(part));
        QVERIFY(env.engine->keptDownloads().isEmpty());
    }
    // the check's late result changes nothing: no file arrives in the incoming folder, nothing completes
    hold.release = true;
    QTest::qWait(300);
    QVERIFY(filesBelow(incoming).isEmpty());
    QCOMPARE(env.completed->count(), 0);
    QVERIFY(!QFileInfo::exists(engine::PartStore::moveMarkerPath(part)));
    if (FileRemoval(mode) == FileRemoval::Keep) {
        // the kept data continues when the same file is downloaded again, and completes
        engine::PartStore::setTestHold({});
        DownloadRequest r;
        r.name = QStringLiteral("debian-12.8.0-amd64-netinst.iso");
        r.size = data.size();
        r.hashes = hashesOf(data, 3);
        r.sources = {src};
        const QString again = env.engine->download(r);
        QVERIFY(env.waitDone(again, 30000));
        QCOMPARE(readFile(env.info(again).savePath), data);
        QVERIFY(!QFileInfo::exists(part));
    }
}

void TstEngineRemoveFinishing::inTheMove_data()
{
    QTest::addColumn<int>("mode");
    QTest::addColumn<bool>("copy");
    QTest::newRow("keep, rename") << int(FileRemoval::Keep) << false;
    QTest::newRow("delete, rename") << int(FileRemoval::Delete) << false;
    QTest::newRow("trash, rename") << int(FileRemoval::Trash) << false;
    QTest::newRow("keep, copy") << int(FileRemoval::Keep) << true;
    QTest::newRow("delete, copy") << int(FileRemoval::Delete) << true;
}

// the move is held: the removal does not cut it and runs when it ends, as of a finished download
void TstEngineRemoveFinishing::inTheMove()
{
    QFETCH(int, mode);
    QFETCH(bool, copy);
    // the store reads MX_TEST_COPY_MOVE once per process: the copy rows run in engine_engine_remove_finishing_copy
    const bool copying = qEnvironmentVariableIntValue("MX_TEST_COPY_MOVE") > 0;
    if (copy != copying) QSKIP(copy ? "a copy row: runs with MX_TEST_COPY_MOVE=1" : "a rename row: runs without MX_TEST_COPY_MOVE");
    Env env(QStringLiteral("rmf-move-%1-%2").arg(mode).arg(copy) + variant());
    const QByteArray data = randomBytes(2 * 1024 * 1024 + 5, 910 + quint32(mode));
    Hold hold(QStringLiteral("move"));
    SearchSource src;
    const QString id = startAndHold(env, data, hold, &src);
    QVERIFY(!id.isEmpty());
    QCOMPARE(env.info(id).statusText, QStringLiteral("Moving..."));
    const QString part = env.engine->incompleteDir() + QLatin1Char('/') + id + QStringLiteral(".part");
    const QString incoming = env.engine->incomingDir();
    QSignalSpy removed(env.engine.get(), &DownloadEngine::downloadRemoved);
    QSignalSpy deleted(env.engine.get(), &TransferService::downloadFilesDeleted);
    QSignalSpy rows(env.engine.get(), &TransferService::transferRemoved);
    QElapsedTimer t;
    t.start();
    env.engine->removeDownload(id, FileRemoval(mode));
    QVERIFY(t.elapsed() < 500);   // nothing waits for the move on this thread
    // the Transfers row says the removal was taken (the state stays Verifying)
    QCOMPARE(env.info(id).statusText, engine::Download::removalStatusText());
    QCOMPARE(env.info(id).state, TransferState::Verifying);
    // the row stays while the move runs; asking again changes nothing
    QCOMPARE(rows.count(), 0);
    QCOMPARE(removed.count(), 0);
    QCOMPARE(env.engine->downloads().size(), 1);
    env.engine->removeDownload(id, FileRemoval::Keep);
    QCOMPARE(rows.count(), 0);
    hold.release = true;
    QVERIFY(QTest::qWaitFor([&] { return removed.count() == 1; }, 15000));
    QCOMPARE(rows.count(), 1);
    QCOMPARE(removed.at(0).at(0).toString(), id);
    QCOMPARE(removed.at(0).at(1).toBool(), true);   // finished in that moment
    QCOMPARE(env.completed->count(), 1);
    const QString target = QDir(incoming).filePath(QStringLiteral("debian-12.8.0-amd64-netinst.iso"));
    QVERIFY(env.engine->downloads().isEmpty());
    QVERIFY(env.engine->keptDownloads().isEmpty());   // a finished download leaves no kept record
    QVERIFY(!QFileInfo::exists(part));
    if (FileRemoval(mode) == FileRemoval::Keep) {
        QCOMPARE(removed.at(0).at(2).toStringList(), QStringList{target});
        QCOMPARE(readFile(target), data);
        QCOMPARE(filesBelow(incoming), QStringList{target});
    } else {
        QVERIFY(removed.at(0).at(2).toStringList().isEmpty());
        QVERIFY(QTest::qWaitFor([&] { return deleted.count() == 1; }, 10000));
        const QStringList gone = deleted.at(0).at(2).toStringList() + deleted.at(0).at(3).toStringList();
        QCOMPARE(gone, QStringList{target});
        QVERIFY(deleted.at(0).at(4).toStringList().isEmpty());
        QVERIFY(!QFileInfo::exists(target));
        QVERIFY(filesBelow(incoming).isEmpty());
    }
    // no move marker, no temporary copy, nothing else in the incomplete folder
    QVERIFY(filesBelow(env.engine->incompleteDir()).isEmpty());
}

void TstEngineRemoveFinishing::moveFailsWhileRemoved_data()
{
    QTest::addColumn<int>("mode");
    QTest::newRow("keep") << int(FileRemoval::Keep);
    QTest::newRow("delete") << int(FileRemoval::Delete);
}

// the move fails while a removal waits for it (the incoming folder became a plain file): the download
// is removed as a partial one; its part file is the data
void TstEngineRemoveFinishing::moveFailsWhileRemoved()
{
    QFETCH(int, mode);
    Env env(QStringLiteral("rmf-movefail-%1").arg(mode) + variant());
    const QByteArray data = randomBytes(2 * 1024 * 1024 + 11, 920 + quint32(mode));
    Hold hold(QStringLiteral("move"));
    const QString incoming = env.root + QStringLiteral("/incoming");
    hold.onRelease = [incoming] {   // on the I/O thread, just before the move: the folder is a file now
        QDir(incoming).removeRecursively();
        QFile f(incoming);
        if (f.open(QIODevice::WriteOnly)) f.write("not a folder");
    };
    SearchSource src;
    const QString id = startAndHold(env, data, hold, &src);
    QVERIFY(!id.isEmpty());
    const QString part = env.engine->incompleteDir() + QLatin1Char('/') + id + QStringLiteral(".part");
    QSignalSpy removed(env.engine.get(), &DownloadEngine::downloadRemoved);
    QSignalSpy deleted(env.engine.get(), &TransferService::downloadFilesDeleted);
    env.engine->removeDownload(id, FileRemoval(mode));
    QCOMPARE(removed.count(), 0);
    QCOMPARE(env.info(id).statusText, engine::Download::removalStatusText());
    hold.release = true;
    QVERIFY(QTest::qWaitFor([&] { return removed.count() == 1; }, 15000));
    QCOMPARE(removed.at(0).at(1).toBool(), false);   // removed as a partial download
    QVERIFY(env.engine->downloads().isEmpty());
    QCOMPARE(env.completed->count(), 0);
    if (FileRemoval(mode) == FileRemoval::Keep) {
        QCOMPARE(removed.at(0).at(2).toStringList(), QStringList{part});
        QVERIFY(QFileInfo::exists(part));
        QCOMPARE(env.engine->keptDownloads().size(), 1);
        QCOMPARE(QDir::cleanPath(env.engine->keptDownloads().first().savePath), QDir::cleanPath(part));
    } else {
        QVERIFY(removed.at(0).at(2).toStringList().isEmpty());
        QVERIFY(QTest::qWaitFor([&] { return deleted.count() == 1; }, 10000));
        QCOMPARE(deleted.at(0).at(2).toStringList() + deleted.at(0).at(3).toStringList(), QStringList{part});
        QVERIFY(!QFileInfo::exists(part));
    }
    QVERIFY(!QFileInfo::exists(engine::PartStore::moveMarkerPath(part)));
    // the folder again: nothing of this download arrived in it
    QVERIFY(QFile::remove(incoming));
    QVERIFY(QDir().mkpath(incoming));
    QVERIFY(filesBelow(incoming).isEmpty());
    if (FileRemoval(mode) == FileRemoval::Keep) {
        // the kept record of a store the move had closed is right: the same file continues and completes
        engine::PartStore::setTestHold({});
        DownloadRequest r;
        r.name = QStringLiteral("debian-12.8.0-amd64-netinst.iso");
        r.size = data.size();
        r.hashes = hashesOf(data, 3);
        r.sources = {src};
        const QString again = env.engine->download(r);
        QVERIFY(env.waitDone(again, 30000));
        QCOMPARE(readFile(env.info(again).savePath), data);
        QVERIFY(!QFileInfo::exists(part));
    }
}

void TstEngineRemoveFinishing::quitWhileRemoved_data()
{
    QTest::addColumn<int>("mode");
    QTest::addColumn<bool>("closeNow");
    QTest::newRow("keep, quit waits for the move") << int(FileRemoval::Keep) << false;
    QTest::newRow("delete, quit waits for the move") << int(FileRemoval::Delete) << false;
    QTest::newRow("keep, quit does not wait") << int(FileRemoval::Keep) << true;
    QTest::newRow("delete, quit does not wait") << int(FileRemoval::Delete) << true;
}

// OctoMX is closed while a removal waits for the move: the removal is stored with the download and
// done at the next start, by the same rule (here the file arrived, so as a finished download)
void TstEngineRemoveFinishing::quitWhileRemoved()
{
    QFETCH(int, mode);
    QFETCH(bool, closeNow);
    Env env(QStringLiteral("rmf-quit-%1-%2").arg(mode).arg(closeNow) + variant());
    const QByteArray data = randomBytes(2 * 1024 * 1024 + 13, 930 + quint32(mode));
    Hold hold(QStringLiteral("move"));
    SearchSource src;
    const QString id = startAndHold(env, data, hold, &src);
    QVERIFY(!id.isEmpty());
    const QString part = env.engine->incompleteDir() + QLatin1Char('/') + id + QStringLiteral(".part");
    const QString incoming = env.engine->incomingDir();
    const QString incomplete = env.engine->incompleteDir();
    env.engine->removeDownload(id, FileRemoval(mode));
    QCOMPARE(env.engine->downloads().size(), 1);
    // the quit begins while the move is held; then the move goes on: it ends inside the quit (the quit
    // waits for it), or after it, when the engine is gone (closeNow: the quit does not wait)
    env.engine->beginShutdown();
    if (closeNow) {
        env.engine->closeNow();
        env.engine->shutdown();
        hold.release = true;
    } else {
        hold.release = true;
        env.engine->shutdown();
    }
    env.engineDown(true);   // the store's last task (the move) has ended when the engine is gone
    engine::PartStore::setTestHold({});
    // the next start: the download is not in the list, the stored removal was done
    env.engineUp();
    QVERIFY(QTest::qWaitFor([&] { return env.engine->downloads().isEmpty(); }, 10000));
    const QString target = QDir(incoming).filePath(QStringLiteral("debian-12.8.0-amd64-netinst.iso"));
    QVERIFY(!QFileInfo::exists(part));
    QVERIFY(env.engine->keptDownloads().isEmpty());
    if (FileRemoval(mode) == FileRemoval::Keep) {
        QCOMPARE(filesBelow(incoming), QStringList{target});
        QCOMPARE(readFile(target), data);
    } else {
        QVERIFY(QTest::qWaitFor([&] { return filesBelow(incoming).isEmpty(); }, 10000));
    }
    QVERIFY(filesBelow(incomplete).isEmpty());   // no part, no marker, no temporary copy
    // and it stays removed across one more start
    env.engineDown(true);
    env.engineUp();
    QTest::qWait(200);
    QVERIFY(env.engine->downloads().isEmpty());
}

QTEST_MAIN(TstEngineRemoveFinishing)
#include "tst_engine_remove_finishing.moc"
