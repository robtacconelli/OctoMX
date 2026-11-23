// SPDX-License-Identifier: GPL-3.0-or-later
// [ui-j] Removing a download from the list: "keep the file" (a finished file stays; partial data
// stays and the same download continues from it: downloading it again, resumeDownload on its data,
// Load Incomplete's list, a restart in between) and "delete the file" (only what the download
// created goes, to the trash seam under MX_OS_ROOT; nothing else in the folders is touched; folder
// torrents; files that were there before; a library file a row only points to). Fake plugins and an
// in-memory torrent backend; every file is created by the test below its own folder.
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtTest/QTest>

#include "engine_env.h"
#include "mx/trash.h"
#include "mxhash/ed2k.h"
#include "mxhash/links.h"

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

QString magnetFor(const QString &id, const QString &name)
{
    return QStringLiteral("magnet:?xt=urn:btih:%1&dn=%2").arg(id, name);
}

void writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(data);
}

QStringList listing(const QString &dir)
{
    QStringList out;
    QDirIterator it(dir, QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) out << QDir(dir).relativeFilePath(it.next());
    out.sort();
    return out;
}

QString seamFiles()
{
    return QDir(trash::seamDir()).filePath(QStringLiteral("files"));
}
} // namespace

class TstEngineRemove : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void partKeepThenDownloadAgain();
    void partKeepResumeFromItsFile();
    void partDeleteGoesToTrash();
    void finishedKeepAndDeleteNothingElse();
    void finishedChangedOrAdoptedIsKept();
    void torrentKeepThenDownloadAgain();
    void torrentDeleteFileAndPartsFile();
    void folderTorrentDelete();
    void preexistingTorrentFileKept();
    void deleteForGoodOnlyWhenAllowed();
};

void TstEngineRemove::initTestCase()
{
    // the trash seam of this test, inside its data folder (never the real trash)
    const QString root = testRoot() + QStringLiteral("/remove-os-root");
    QDir(root).removeRecursively();
    qputenv("MX_OS_ROOT", root.toUtf8());
    qunsetenv("MX_TRASH");
    QVERIFY(trash::redirected());
    QVERIFY(trash::seamDir().startsWith(testRoot()));
}

void TstEngineRemove::partKeepThenDownloadAgain()
{
    Env env(QStringLiteral("rm-keep"));
    auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges | NetworkPlugin::CanFindSources);
    const QByteArray data = randomBytes(3 * P + 4321, 81);
    ed->hashSetOnFind << qMakePair(P, ed2kParts(data));
    SourceConfig c;
    c.data = data;
    c.bytesPerSec = 8 << 20;
    const SearchSource s = ed->addSource(QStringLiteral("ed2k://keep"), c, QStringLiteral("6.6.6.6:4662"));
    env.engineUp();
    const QString id = env.engine->download(req(QStringLiteral("keep.bin"), data, hashesOf(data, 2), {s}));
    QVERIFY(QTest::qWaitFor([&] { return env.info(id).done >= P; }, 20000));
    const DownloadDiskInfo di = env.engine->diskInfo(id);
    QVERIFY(di.known && !di.finished && !di.torrent);
    const QString part = env.engine->incompleteDir() + QLatin1Char('/') + id + QStringLiteral(".part");
    QCOMPARE(di.paths, QStringList{part});
    QVERIFY(di.bytes >= P && di.trash && di.inPlaceFile.isEmpty());
    // "keep": the row goes (running: stopped first), the part file stays, the engine remembers it
    QSignalSpy removed(env.engine.get(), &TransferService::transferRemoved);
    QSignalSpy deleted(env.engine.get(), &TransferService::downloadFilesDeleted);
    env.engine->removeDownload(id, FileRemoval::Keep);
    QCOMPARE(removed.count(), 1);
    QVERIFY(env.engine->downloads().isEmpty());
    QVERIFY(QFileInfo::exists(part));
    QVERIFY(QFileInfo::exists(env.root + QStringLiteral("/kept-downloads.json")));
    QCOMPARE(env.engine->keptDownloads().size(), 1);
    const qint64 kept = env.engine->keptDownloads().first().done;
    QVERIFY(kept >= P);
    // Load Incomplete lists it with its name, what it holds and the link that continues it
    bool listed = false;
    for (const PartialFile &pf : env.engine->partialFiles())
        if (pf.kept && pf.path == part) {
            listed = true;
            QCOMPARE(pf.name, QStringLiteral("keep.bin"));
            QVERIFY(pf.link.startsWith(QStringLiteral("ed2k://")));
            QCOMPARE(pf.held, kept);
            QVERIFY(pf.downloadId.isEmpty());
        }
    QVERIFY(listed);
    // the Search mark: a hit of the same file "matches incomplete local file" (by hash, or name and size)
    SearchHit hit;
    hit.fileName = QStringLiteral("keep.bin");
    hit.size = data.size();
    hit.hashes.ed2k = hashesOf(data, 2).ed2k;
    QVERIFY(env.engine->matchesKept(hit));
    hit.hashes = FileHashes();
    QVERIFY(env.engine->matchesKept(hit));   // same name and size, no hash known
    hit.hashes.ed2k = hashesOf(randomBytes(64, 3), 2).ed2k;
    QVERIFY(!env.engine->matchesKept(hit));  // another file of that name
    // across a restart too
    env.engineDown(true);
    env.engineUp();
    QCOMPARE(env.engine->keptDownloads().size(), 1);
    QVERIFY(env.engine->downloads().isEmpty());
    // downloading the same file again (Search: "matches incomplete local file") continues it
    const int opened = ed->stats.size();
    const QString again = env.engine->download(req(QStringLiteral("keep.bin"), data, hashesOf(data, 2), {s}));
    QCOMPARE(again, id);   // the same download comes back
    QVERIFY(env.engine->keptDownloads().isEmpty());
    QVERIFY(env.info(again).done >= kept - P);   // what was verified counts at once
    QVERIFY2(env.waitDone(again, 30000), qPrintable(env.info(again).statusText));
    QCOMPARE(readFile(env.info(again).savePath), data);
    for (int i = opened; i < ed->stats.size(); ++i)
        for (const auto &r : ed->stats[i]->requests)
            QVERIFY2(r.first >= kept / P * P, "data that was kept was fetched again");
    QCOMPARE(deleted.count(), 0);
    QVERIFY(!QFileInfo::exists(env.root + QStringLiteral("/kept-downloads.json")));
}

void TstEngineRemove::partKeepResumeFromItsFile()
{
    Env env(QStringLiteral("rm-keep-file"));
    auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges | NetworkPlugin::CanFindSources);
    const QByteArray data = randomBytes(2 * P + 99, 82);
    ed->hashSetOnFind << qMakePair(P, ed2kParts(data));
    SourceConfig c;
    c.data = data;
    c.bytesPerSec = 6 << 20;
    const SearchSource s = ed->addSource(QStringLiteral("ed2k://keepfile"), c, QStringLiteral("7.7.7.7:4662"));
    ed->onFind << s;
    env.engineUp();
    const QString id = env.engine->download(req(QStringLiteral("file.bin"), data, hashesOf(data, 2), {s}));
    QVERIFY(QTest::qWaitFor([&] { return env.info(id).done >= P; }, 20000));
    env.engine->removeDownload(id, FileRemoval::Keep);
    PartialFile kept;
    for (const PartialFile &pf : env.engine->partialFiles())
        if (pf.kept) kept = pf;
    QVERIFY(!kept.path.isEmpty());
    // Load Incomplete: the chosen file + its link (TransfersPage::loadIncomplete)
    DownloadRequest r;
    r.uri = kept.link;
    r.name = kept.name;
    const QString back = env.engine->resumeDownload(r, kept.path);
    QCOMPARE(back, id);
    QVERIFY(env.info(back).done >= P);
    QVERIFY2(env.waitDone(back, 30000), qPrintable(env.info(back).statusText));
    QCOMPARE(readFile(env.info(back).savePath), data);
    // the kept data of one file is never continued with the link of another
    const QByteArray other = randomBytes(2 * P, 83);
    SourceConfig c3;
    c3.data = other;
    c3.bytesPerSec = 4 << 20;
    const SearchSource s3 = ed->addSource(QStringLiteral("ed2k://three"), c3, QStringLiteral("7.7.7.9:4662"));
    const QString id3 = env.engine->download(req(QStringLiteral("three.bin"), other, hashesOf(other, 2), {s3}));
    QVERIFY(QTest::qWaitFor([&] { return env.engine->diskInfo(id3).bytes > 0; }, 20000));
    env.engine->removeDownload(id3, FileRemoval::Keep);
    QString keptPath;
    for (const PartialFile &pf : env.engine->partialFiles())
        if (pf.kept && pf.name == QLatin1String("three.bin")) keptPath = pf.path;
    QVERIFY(!keptPath.isEmpty());
    QSignalSpy refused(env.engine.get(), &TransferService::downloadNotStarted);
    DownloadRequest wrong;
    wrong.uri = hash::formatEd2kLink(QStringLiteral("file.bin"), data.size(), hashesOf(data, 2));
    QVERIFY(env.engine->resumeDownload(wrong, keptPath).isEmpty());
    QCOMPARE(refused.count(), 1);
    QVERIFY(QFileInfo::exists(keptPath));
    QCOMPARE(env.engine->keptDownloads().size(), 1);
}

void TstEngineRemove::partDeleteGoesToTrash()
{
    Env env(QStringLiteral("rm-del"));
    auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges | NetworkPlugin::CanFindSources);
    const QByteArray data = randomBytes(3 * P, 84);
    ed->hashSetOnFind << qMakePair(P, ed2kParts(data));
    SourceConfig c;
    c.data = data;
    c.bytesPerSec = 6 << 20;
    const SearchSource s = ed->addSource(QStringLiteral("ed2k://del"), c, QStringLiteral("8.8.8.8:4662"));
    env.engineUp();
    const QString inc = env.engine->incompleteDir();
    writeFile(inc + QStringLiteral("/someone else.part"), "not ours");
    const QString id = env.engine->download(req(QStringLiteral("del.bin"), data, hashesOf(data, 2), {s}));
    QVERIFY(QTest::qWaitFor([&] { return env.info(id).done >= P; }, 20000));
    const QString part = inc + QLatin1Char('/') + id + QStringLiteral(".part");
    QSignalSpy deleted(env.engine.get(), &TransferService::downloadFilesDeleted);
    env.engine->removeDownload(id, FileRemoval::Trash);
    QVERIFY(env.engine->downloads().isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(deleted.count(), 1, 5000);
    QCOMPARE(deleted.first().at(0).toString(), id);
    QCOMPARE(deleted.first().at(2).toStringList(), QStringList{part});   // trashed
    QVERIFY(deleted.first().at(4).toStringList().isEmpty());             // nothing failed
    QVERIFY(!QFileInfo::exists(part));
    QVERIFY(QFileInfo::exists(seamFiles() + QLatin1Char('/') + id + QStringLiteral(".part")));
    QVERIFY(QFileInfo::exists(inc + QStringLiteral("/someone else.part")));
    QVERIFY(env.engine->keptDownloads().isEmpty());
    // no trace in the profile: the record is gone after a restart
    env.engineDown(true);
    env.engineUp();
    QVERIFY(env.engine->downloads().isEmpty());
    QVERIFY(env.engine->keptDownloads().isEmpty());
}

void TstEngineRemove::finishedKeepAndDeleteNothingElse()
{
    Env env(QStringLiteral("rm-fin"));
    auto *http = env.addPlugin(Net::Http, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(1 << 20, 85);
    SourceConfig c;
    c.data = data;
    const SearchSource s = http->addSource(QStringLiteral("http://a/same.bin"), c);
    env.engineUp();
    const QString in = env.engine->incomingDir();
    // a file of the same name that this download did not create, and another file
    writeFile(in + QStringLiteral("/same.bin"), "the user's own file");
    writeFile(in + QStringLiteral("/other.txt"), "other");
    const QString id = env.engine->download(req(QStringLiteral("same.bin"), data, hashesOf(data, 1), {s}));
    QVERIFY2(env.waitDone(id, 20000), qPrintable(env.info(id).statusText));
    const QString done = env.info(id).savePath;
    QCOMPARE(QFileInfo(done).fileName(), QStringLiteral("same (1).bin"));
    const DownloadDiskInfo di = env.engine->diskInfo(id);
    QVERIFY(di.finished);
    QCOMPARE(di.paths, QStringList{done});
    QCOMPARE(di.bytes, qint64(data.size()));
    const QStringList before = listing(in);
    QSignalSpy deleted(env.engine.get(), &TransferService::downloadFilesDeleted);
    env.engine->removeDownload(id, FileRemoval::Trash);
    QTRY_COMPARE_WITH_TIMEOUT(deleted.count(), 1, 5000);
    QVERIFY(!QFileInfo::exists(done));
    QCOMPARE(readFile(in + QStringLiteral("/same.bin")), QByteArray("the user's own file"));
    QStringList rest = before;
    rest.removeAll(QStringLiteral("same (1).bin"));
    QCOMPARE(listing(in), rest);
    QCOMPARE(readFile(seamFiles() + QStringLiteral("/same (1).bin")), data);
    // keep: the finished file stays where it is
    const QString id2 = env.engine->download(req(QStringLiteral("same.bin"), data, hashesOf(data, 1), {s}));
    QVERIFY2(env.waitDone(id2, 20000), qPrintable(env.info(id2).statusText));
    const QString done2 = env.info(id2).savePath;
    env.engine->removeDownload(id2, FileRemoval::Keep);
    QTest::qWait(400);
    QCOMPARE(readFile(done2), data);
    QVERIFY(env.engine->keptDownloads().isEmpty());   // a finished file is no partial download
}

void TstEngineRemove::finishedChangedOrAdoptedIsKept()
{
    Env env(QStringLiteral("rm-changed"), true);
    auto *http = env.addPlugin(Net::Http, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(1 << 20, 86);
    SourceConfig c;
    c.data = data;
    const SearchSource s = http->addSource(QStringLiteral("http://a/changed.bin"), c);
    env.engineUp();
    const QString id = env.engine->download(req(QStringLiteral("changed.bin"), data, hashesOf(data, 1), {s}));
    QVERIFY2(env.waitDone(id, 20000), qPrintable(env.info(id).statusText));
    const QString path = env.info(id).savePath;
    // the user replaced the file since: it is not the download's any more
    writeFile(path, "something else now");
    DownloadDiskInfo di = env.engine->diskInfo(id);
    QVERIFY(di.paths.isEmpty());
    QCOMPARE(di.kept, QStringList{path});
    QVERIFY(di.keptWhy.contains(QStringLiteral("size changed")));
    QSignalSpy deleted(env.engine.get(), &TransferService::downloadFilesDeleted);
    env.engine->removeDownload(id, FileRemoval::TrashOrDelete);
    QTRY_COMPARE_WITH_TIMEOUT(deleted.count(), 1, 5000);
    QVERIFY(deleted.first().at(2).toStringList().isEmpty() && deleted.first().at(3).toStringList().isEmpty());
    QCOMPARE(readFile(path), QByteArray("something else now"));
    // a row that only points to a file of the library ("already in your library"): never deleted
    const QString shared = env.root + QStringLiteral("/shared");
    const QByteArray mine = randomBytes(300000, 87);
    writeFile(shared + QStringLiteral("/mine.bin"), mine);
    env.library->addFolder(shared, true);
    QVERIFY(QTest::qWaitFor([&] {
        for (const SharedFile &f : env.library->files()) if (f.hashed && f.name == QLatin1String("mine.bin")) return true;
        return false;
    }, 20000));
    const QString row = env.engine->download(req(QStringLiteral("mine.bin"), mine, hashesOf(mine, 2), {}));
    QCOMPARE(env.info(row).state, TransferState::Completed);
    di = env.engine->diskInfo(row);
    QVERIFY(di.paths.isEmpty());
    QVERIFY(di.keptWhy.contains(QStringLiteral("library")));
    env.engine->removeDownload(row, FileRemoval::TrashOrDelete);
    QTest::qWait(400);
    QCOMPARE(readFile(shared + QStringLiteral("/mine.bin")), mine);
}

void TstEngineRemove::torrentKeepThenDownloadAgain()
{
    Env env(QStringLiteral("rm-t-keep"));
    FakeTorrentBackend be;
    be.checkOnAdd = true;
    const QByteArray data = randomBytes(2 * 1024 * 1024, 88);
    const QString tid = be.makeTorrent(QStringLiteral("tkeep.bin"), {{QStringLiteral("tkeep.bin"), data}}, 64 * 1024);
    be.known[tid].peerPiecesPerTick = 1;
    be.known[tid].peers = 1;
    env.engineUp(&be);
    const QString id = env.engine->downloadUri(magnetFor(tid, QStringLiteral("tkeep.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(env.info(id).done >= 6 * 64 * 1024, 10000);
    be.known[tid].peerPiecesPerTick = 0;   // the swarm goes quiet
    be.active[tid].peerPiecesPerTick = 0;
    const QString file = env.engine->incomingDir() + QStringLiteral("/tkeep.bin");
    const DownloadDiskInfo di = env.engine->diskInfo(id);
    QVERIFY(di.torrent && !di.finished);
    QCOMPARE(di.paths, QStringList{file});
    env.engine->removeDownload(id, FileRemoval::Keep);
    QVERIFY(!be.active.contains(tid));
    QVERIFY(QFileInfo::exists(file));
    QCOMPARE(env.engine->keptDownloads().size(), 1);
    const qint64 had = env.engine->keptDownloads().first().done;
    QVERIFY(had > 0);
    bool listed = false;
    for (const PartialFile &pf : env.engine->partialFiles())
        listed = listed || (pf.kept && pf.path == file && pf.link.contains(tid, Qt::CaseInsensitive));
    QVERIFY(listed);
    // the same magnet again: the download comes back and continues from the data on disk
    be.known[tid].peerPiecesPerTick = 2;
    const QString again = env.engine->downloadUri(magnetFor(tid, QStringLiteral("tkeep.bin")));
    QCOMPARE(again, id);
    QTRY_VERIFY_WITH_TIMEOUT(env.info(again).done >= had, 5000);
    QVERIFY2(env.waitDone(again, 30000), qPrintable(env.info(again).statusText));
    QCOMPARE(readFile(file), data);
}

void TstEngineRemove::torrentDeleteFileAndPartsFile()
{
    Env env(QStringLiteral("rm-t-del"));
    FakeTorrentBackend be;
    const QByteArray data = randomBytes(2 * 1024 * 1024, 89);
    const QString tid = be.makeTorrent(QStringLiteral("tdel.bin"), {{QStringLiteral("tdel.bin"), data}}, 64 * 1024);
    be.known[tid].peerPiecesPerTick = 1;
    be.known[tid].peers = 1;
    env.engineUp(&be);
    const QString in = env.engine->incomingDir();
    writeFile(in + QStringLiteral("/unrelated.iso"), "unrelated");
    const QString id = env.engine->downloadUri(magnetFor(tid, QStringLiteral("tdel.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(env.info(id).done > 0, 10000);
    // libtorrent's file of the pieces of unwanted files belongs to the download too
    writeFile(in + QStringLiteral("/.") + tid + QStringLiteral(".parts"), "parts");
    const DownloadDiskInfo di = env.engine->diskInfo(id);
    QCOMPARE(di.paths.size(), 2);
    QSignalSpy deleted(env.engine.get(), &TransferService::downloadFilesDeleted);
    env.engine->removeDownload(id, FileRemoval::Trash);
    QTRY_COMPARE_WITH_TIMEOUT(deleted.count(), 1, 15000);   // after the backend let the torrent go
    QCOMPARE(deleted.first().at(2).toStringList().size(), 2);
    QVERIFY(!QFileInfo::exists(in + QStringLiteral("/tdel.bin")));
    QVERIFY(!QFileInfo::exists(in + QStringLiteral("/.") + tid + QStringLiteral(".parts")));
    QCOMPARE(listing(in), QStringList{QStringLiteral("unrelated.iso")});
    QVERIFY(env.engine->keptDownloads().isEmpty());
}

void TstEngineRemove::folderTorrentDelete()
{
    Env env(QStringLiteral("rm-t-folder"));
    FakeTorrentBackend be;
    const QByteArray a = randomBytes(600 * 1024, 90), b = randomBytes(700 * 1024, 91);
    const QString tid = be.makeTorrent(QStringLiteral("Album"), {{QStringLiteral("cd1/a.flac"), a}, {QStringLiteral("b.flac"), b}}, 64 * 1024);
    be.known[tid].peerPiecesPerTick = 2;
    be.known[tid].peers = 1;
    const QString tid2 = be.makeTorrent(QStringLiteral("Album2"), {{QStringLiteral("a.flac"), a}, {QStringLiteral("b.flac"), b}}, 64 * 1024);
    be.known[tid2].peerPiecesPerTick = 2;
    be.known[tid2].peers = 1;
    env.engineUp(&be);
    const QString in = env.engine->incomingDir();
    // 1. the folder the download created, only its files in it: it goes as one item
    const QString id = env.engine->downloadUri(magnetFor(tid, QStringLiteral("Album")));
    QVERIFY2(env.waitDone(id, 30000), qPrintable(env.info(id).statusText));
    DownloadDiskInfo di = env.engine->diskInfo(id);
    QVERIFY(di.finished && di.torrent);
    QCOMPARE(di.paths, QStringList{in + QStringLiteral("/Album")});
    QSignalSpy deleted(env.engine.get(), &TransferService::downloadFilesDeleted);
    env.engine->removeDownload(id, FileRemoval::Trash);
    QTRY_COMPARE_WITH_TIMEOUT(deleted.count(), 1, 15000);
    QVERIFY(!QFileInfo::exists(in + QStringLiteral("/Album")));
    QVERIFY(QFileInfo::exists(seamFiles() + QStringLiteral("/Album/cd1/a.flac")));
    // 2. the user put a file of their own into the folder: the torrent's files go one by one, the
    //    folder and the user's file stay
    const QString id2 = env.engine->downloadUri(magnetFor(tid2, QStringLiteral("Album2")));
    QTRY_VERIFY_WITH_TIMEOUT(env.info(id2).done > 0, 10000);
    writeFile(in + QStringLiteral("/Album2/cover.jpg"), "the user's picture");
    di = env.engine->diskInfo(id2);
    QVERIFY(!di.paths.contains(in + QStringLiteral("/Album2")));
    for (const QString &p : di.paths) QVERIFY(p.startsWith(in + QStringLiteral("/Album2/")) && !p.endsWith(QStringLiteral("cover.jpg")));
    env.engine->removeDownload(id2, FileRemoval::Trash);
    QTRY_COMPARE_WITH_TIMEOUT(deleted.count(), 2, 15000);
    QCOMPARE(listing(in + QStringLiteral("/Album2")), QStringList{QStringLiteral("cover.jpg")});
    // 3. the same, and the user's file removed later: nothing of the torrent is left anyway
    QCOMPARE(readFile(in + QStringLiteral("/Album2/cover.jpg")), QByteArray("the user's picture"));
}

void TstEngineRemove::preexistingTorrentFileKept()
{
    Env env(QStringLiteral("rm-t-pre"));
    FakeTorrentBackend be;
    const QByteArray data = randomBytes(1024 * 1024, 92);
    const QString tid = be.makeTorrent(QStringLiteral("pre.bin"), {{QStringLiteral("pre.bin"), data}}, 64 * 1024);
    be.known[tid].peerPiecesPerTick = 1;
    be.known[tid].peers = 1;
    env.engineUp(&be);
    const QString in = env.engine->incomingDir();
    // a file at the torrent's place before the download started (not created by it)
    writeFile(in + QStringLiteral("/pre.bin"), QByteArray(1024 * 1024, 'u'));
    const QString id = env.engine->downloadUri(magnetFor(tid, QStringLiteral("pre.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(env.engine->diskInfo(id).kept.size() == 1, 10000);
    const DownloadDiskInfo di = env.engine->diskInfo(id);
    QVERIFY(di.paths.isEmpty());
    QVERIFY(di.keptWhy.contains(QStringLiteral("before this download")));
    QSignalSpy deleted(env.engine.get(), &TransferService::downloadFilesDeleted);
    env.engine->removeDownload(id, FileRemoval::TrashOrDelete);
    QTRY_COMPARE_WITH_TIMEOUT(deleted.count(), 1, 15000);
    QVERIFY(QFileInfo::exists(in + QStringLiteral("/pre.bin")));
}

void TstEngineRemove::deleteForGoodOnlyWhenAllowed()
{
    Env env(QStringLiteral("rm-forgood"));
    auto *http = env.addPlugin(Net::Http, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(512 * 1024, 93);
    SourceConfig c;
    c.data = data;
    const SearchSource s = http->addSource(QStringLiteral("http://a/fg.bin"), c);
    env.engineUp();
    qputenv("MX_TRASH", "none");   // a drive without a trash
    const QString id = env.engine->download(req(QStringLiteral("fg.bin"), data, hashesOf(data, 1), {s}));
    QVERIFY2(env.waitDone(id, 20000), qPrintable(env.info(id).statusText));
    const QString path = env.info(id).savePath;
    QVERIFY(!env.engine->diskInfo(id).trash);
    QSignalSpy deleted(env.engine.get(), &TransferService::downloadFilesDeleted);
    // "Trash" only: the user was told "to the trash", so it stays
    env.engine->removeDownload(id, FileRemoval::Trash);
    QTRY_COMPARE_WITH_TIMEOUT(deleted.count(), 1, 5000);
    QCOMPARE(deleted.first().at(4).toStringList(), QStringList{path});
    QVERIFY(QFileInfo::exists(path));
    // the question said "deleted for good": then it is, and nothing goes to the trash
    const QByteArray data2 = randomBytes(256 * 1024, 94);
    SourceConfig c2;
    c2.data = data2;
    const SearchSource s2 = http->addSource(QStringLiteral("http://a/fg2.bin"), c2);
    const QString id2 = env.engine->download(req(QStringLiteral("fg2.bin"), data2, hashesOf(data2, 1), {s2}));
    QVERIFY2(env.waitDone(id2, 20000), qPrintable(env.info(id2).statusText));
    const QString path2 = env.info(id2).savePath;
    env.engine->removeDownload(id2, FileRemoval::TrashOrDelete);
    QTRY_COMPARE_WITH_TIMEOUT(deleted.count(), 2, 5000);
    QCOMPARE(deleted.at(1).at(3).toStringList(), QStringList{path2});   // deleted for good
    QVERIFY(!QFileInfo::exists(path2));
    QVERIFY(!QFileInfo::exists(seamFiles() + QStringLiteral("/fg2.bin")));
    qunsetenv("MX_TRASH");
}

QTEST_GUILESS_MAIN(TstEngineRemove)
#include "tst_engine_remove.moc"
