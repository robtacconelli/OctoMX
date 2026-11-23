// SPDX-License-Identifier: GPL-3.0-or-later
#include <QtTest/QtTest>

#include "core/dao.h"
#include "core/database.h"

#include <QtCore/QTemporaryDir>
#include <QtSql/QSqlQuery>

using namespace mx;

namespace {
QByteArray h(char c, int n) { return QByteArray(n, c); }
}

class TestDatabase : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    QString path() const { return m_dir.filePath(QStringLiteral("octomx.db")); }

private slots:
    void migrations()
    {
        Database db;
        QString err;
        QVERIFY2(db.open(path(), &err), qPrintable(err));
        QCOMPARE(db.schemaVersion(), Database::latestSchemaVersion());
        QSqlQuery q(db.db());
        QVERIFY(q.exec(QStringLiteral("PRAGMA journal_mode")) && q.next());
        QCOMPARE(q.value(0).toString().toLower(), QStringLiteral("wal"));
        QVERIFY(q.exec(QStringLiteral("PRAGMA foreign_keys")) && q.next());
        QCOMPARE(q.value(0).toInt(), 1);
        for (const char *t : {"downloads", "download_sources", "shared_folders", "library_excluded", "library_files",
                              "hash_links", "hotlist", "ignore_list", "search_history", "chat_favourites", "kv", "stats"}) {
            QVERIFY(q.exec(QStringLiteral("SELECT COUNT(*) FROM %1").arg(QLatin1String(t))));
        }
        db.close();
        // reopening an up-to-date database is a no-op
        Database again;
        QVERIFY(again.open(path()));
        QCOMPARE(again.schemaVersion(), Database::latestSchemaVersion());
    }

    void downloads()
    {
        Database db;
        QVERIFY(db.open(path()));
        DownloadsDao dao(&db);
        DownloadRecord r;
        r.id = QStringLiteral("d1");
        r.name = QStringLiteral("ubuntu.iso");
        r.size = 123456789;
        r.hashes.ed2k = h('e', 16);
        r.hashes.btih = h('b', 20);
        r.hashes.btFileIndex = 2;
        r.state = 4;
        r.wantedFiles = {0, 2, 5};
        r.isBundle = true;
        r.files = {BundleFile{QStringLiteral("a/b.txt"), 10, 0}, BundleFile{QStringLiteral("c.iso"), 20, 1}};
        r.engineState = QByteArray("\x00\x01state", 7);
        r.added = QDateTime::fromMSecsSinceEpoch(1700000000000);
        QVERIFY(dao.upsert(r));

        DownloadRecord g;
        QVERIFY(dao.get(QStringLiteral("d1"), &g));
        QCOMPARE(g.name, r.name);
        QCOMPARE(g.size, r.size);
        QCOMPARE(g.hashes.ed2k, r.hashes.ed2k);
        QCOMPARE(g.hashes.btFileIndex, 2);
        QCOMPARE(g.wantedFiles, r.wantedFiles);
        QCOMPARE(g.files.size(), 2);
        QCOMPARE(g.files.at(0).path, QStringLiteral("a/b.txt"));
        QCOMPARE(g.engineState, r.engineState);
        QCOMPARE(g.added, r.added);

        // deferred progress update becomes visible after flush
        dao.updateProgress(QStringLiteral("d1"), 7, 5000, QStringLiteral("Verifying..."));
        db.flush();
        QVERIFY(dao.get(QStringLiteral("d1"), &g));
        QCOMPARE(g.state, 7);
        QCOMPARE(g.done, 5000);
        dao.setEngineState(QStringLiteral("d1"), QByteArray("new"));
        db.flush();
        QVERIFY(dao.get(QStringLiteral("d1"), &g));
        QCOMPARE(g.engineState, QByteArray("new"));

        // lookup by any hash
        FileHashes q;
        q.ed2k = h('e', 16);
        QVERIFY(dao.findByHashes(q, &g));
        QCOMPARE(g.id, QStringLiteral("d1"));
        q.ed2k = h('x', 16);
        QVERIFY(!dao.findByHashes(q, nullptr));

        // sources + cascade
        SearchSource s1;
        s1.net = Net::ED2K;
        s1.user.id = QStringLiteral("1.2.3.4:4662");
        s1.user.name = QStringLiteral("peer");
        s1.uri = QStringLiteral("ed2k://x");
        s1.connection = QStringLiteral("DSL");
        s1.extra.insert(QStringLiteral("k"), 5);
        SearchSource s2;
        s2.net = Net::BitTorrent;
        s2.uri = QStringLiteral("magnet:?xt=urn:btih:abc");
        QVERIFY(dao.addSources(QStringLiteral("d1"), {s1, s2}));
        QVERIFY(dao.addSources(QStringLiteral("d1"), {s1}));     // refresh, no duplicate
        QList<SourceRecord> src = dao.sources(QStringLiteral("d1"));
        QCOMPARE(src.size(), 2);
        QCOMPARE(src.at(0).source.connection, QStringLiteral("DSL"));
        QCOMPARE(src.at(0).source.extra.value(QStringLiteral("k")).toInt(), 5);
        QCOMPARE(src.at(0).source.user.name, QStringLiteral("peer"));
        QVERIFY(dao.remove(QStringLiteral("d1")));
        QCOMPARE(dao.sources(QStringLiteral("d1")).size(), 0);  // ON DELETE CASCADE
        QCOMPARE(dao.all().size(), 0);
    }

    void libraryCache()
    {
        Database db;
        QVERIFY(db.open(path()));
        LibraryDao dao(&db);
        QVERIFY(dao.addFolder(QStringLiteral("/music"), true));
        QVERIFY(dao.addFolder(QStringLiteral("/video"), false));
        QCOMPARE(dao.folders().size(), 2);
        QVERIFY(dao.removeFolder(QStringLiteral("/video")));
        QCOMPARE(dao.folders().size(), 1);
        QVERIFY(dao.setExcluded(QStringLiteral("/music/secret.mp3"), true));
        QCOMPARE(dao.excluded(), QStringList{QStringLiteral("/music/secret.mp3")});

        LibraryRecord r;
        r.path = QStringLiteral("/music/a.mp3");
        r.size = 1000;
        r.mtimeMs = 42;
        r.hashes.ed2k = h('1', 16);
        r.hashes.sha1 = h('2', 20);
        r.ed2kParts = {h('a', 16), h('b', 16)};
        r.aichParts = {h('c', 20)};
        r.durationSec = 215;
        dao.store(r);
        db.flush();
        LibraryRecord g;
        QVERIFY(dao.lookup(r.path, 1000, 42, &g));
        QCOMPARE(g.hashes.sha1, r.hashes.sha1);
        QCOMPARE(g.ed2kParts, r.ed2kParts);
        QCOMPARE(g.aichParts, r.aichParts);
        QCOMPARE(g.durationSec, 215);
        QVERIFY(!dao.lookup(r.path, 1000, 43, nullptr));   // mtime changed → miss
        dao.addUpload(r.path, 500);
        db.flush();
        QVERIFY(dao.lookup(r.path, 1000, 42, &g));
        QCOMPARE(g.uploaded, 500);
        QCOMPARE(g.requests, 1);
        dao.store(r);   // re-hash keeps statistics
        db.flush();
        QVERIFY(dao.lookup(r.path, 1000, 42, &g));
        QCOMPARE(g.uploaded, 500);

        // worker-thread connection sees the data
        QHash<QString, LibraryRecord> all;
        QThread *t = QThread::create([&] {
            const QString conn = QStringLiteral("test-worker");
            Database::openThreadConnection(path(), conn);
            all = LibraryDao::loadAll(conn);
            Database::closeThreadConnection(conn);
        });
        t->start();
        QVERIFY(t->wait(5000));
        delete t;
        QCOMPARE(all.size(), 1);
        QCOMPARE(all.value(r.path).hashes.ed2k, r.hashes.ed2k);

        dao.removeFiles({r.path});
        db.flush();
        QVERIFY(!dao.lookup(r.path, 1000, 42, nullptr));
    }

    void hashLinks()
    {
        Database db;
        QVERIFY(db.open(path()));
        HashLinksDao dao(&db);
        FileHashes a;
        a.ed2k = h('e', 16);
        a.sha1 = h('s', 20);
        dao.upsert(a, 5000, QStringLiteral("file.bin"), QStringLiteral("library"));
        FileHashes b;
        b.btih = h('b', 20);
        b.btFileIndex = 0;
        b.tth = h('t', 24);
        dao.upsert(b, 5000, QString(), QStringLiteral("overlay"));
        QCOMPARE(dao.count(), 2);
        // a third observation linking both rows merges them into one
        FileHashes c;
        c.sha1 = h('s', 20);
        c.tth = h('t', 24);
        const HashLinkRecord m = dao.upsert(c, 5000, QString(), QStringLiteral("overlay"));
        QCOMPARE(dao.count(), 1);
        QCOMPARE(m.hashes.ed2k, a.ed2k);
        QCOMPARE(m.hashes.btih, b.btih);
        QCOMPARE(m.name, QStringLiteral("file.bin"));
        // lookup by any single hash
        FileHashes q;
        q.btih = h('b', 20);
        q.btFileIndex = 0;
        const FileHashes all = dao.expand(q, 5000);
        QCOMPARE(all.ed2k, a.ed2k);
        QCOMPARE(all.sha1, a.sha1);
        QCOMPARE(all.tth, b.tth);
        // size mismatch / conflicting hashes are not linked
        QVERIFY(dao.find(q, 4000).isEmpty());
        FileHashes conflict;
        conflict.ed2k = h('e', 16);
        conflict.sha1 = h('z', 20);
        QVERIFY(dao.find(conflict).isEmpty());
    }

    void userLists()
    {
        Database db;
        QVERIFY(db.open(path()));
        UserListsDao dao(&db);
        HotlistEntry e;
        e.user.net = Net::Overlay;
        e.user.id = QStringLiteral("peer1");
        e.user.name = QStringLiteral("Quill233_25471");
        QVERIFY(dao.addHotlist(e));
        e.note = QStringLiteral("good stuff");
        e.files = 1234;
        QVERIFY(dao.updateHotlist(e));
        QList<HotlistEntry> hl = dao.hotlist();
        QCOMPARE(hl.size(), 1);
        QCOMPARE(hl.at(0).note, QStringLiteral("good stuff"));
        QCOMPARE(hl.at(0).files, 1234);
        QVERIFY(dao.removeHotlist(e.user));
        QCOMPARE(dao.hotlist().size(), 0);

        QVERIFY(dao.setIgnored(e.user, true));
        QCOMPARE(dao.ignored().size(), 1);
        QCOMPARE(dao.ignored().at(0).name, e.user.name);
        QVERIFY(dao.setIgnored(e.user, false));
        QCOMPARE(dao.ignored().size(), 0);

        QVERIFY(dao.setFavouriteRoom(QStringLiteral("OctoMX Lobby"), true));
        QVERIFY(dao.setFavouriteRoom(QStringLiteral("Music"), true));
        QCOMPARE(dao.favouriteRooms().size(), 2);
        QVERIFY(dao.setFavouriteRoom(QStringLiteral("Music"), false));
        QCOMPARE(dao.favouriteRooms(), QStringList{QStringLiteral("OctoMX Lobby")});

        for (int i = 0; i < 10; ++i) {
            QVERIFY(dao.addSearchHistory(QStringLiteral("query %1").arg(i), 5));
            QTest::qWait(2);
        }
        QStringList hist = dao.searchHistory();
        QCOMPARE(hist.size(), 5);
        QCOMPARE(hist.first(), QStringLiteral("query 9"));
        dao.addSearchHistory(QStringLiteral("query 6"), 5);  // moves to the top, no duplicate
        hist = dao.searchHistory();
        QCOMPARE(hist.first(), QStringLiteral("query 6"));
        QCOMPARE(hist.size(), 5);
        QVERIFY(dao.clearSearchHistory());
        QCOMPARE(dao.searchHistory().size(), 0);
    }

    void kvAndStats()
    {
        Database db;
        QVERIFY(db.open(path()));
        KvStore kv(&db);
        QVERIFY(kv.put(QStringLiteral("ed2k"), QStringLiteral("servers"), QByteArray("server.met bytes")));
        QCOMPARE(kv.get(QStringLiteral("ed2k"), QStringLiteral("servers")), QByteArray("server.met bytes"));
        QCOMPARE(kv.get(QStringLiteral("ed2k"), QStringLiteral("none"), "def"), QByteArray("def"));
        QCOMPARE(kv.keys(QStringLiteral("ed2k")), QStringList{QStringLiteral("servers")});
        QVERIFY(kv.remove(QStringLiteral("ed2k"), QStringLiteral("servers")));
        QCOMPARE(kv.keys(QStringLiteral("ed2k")).size(), 0);

        StatsDao st(&db);
        QVERIFY(st.set(QStringLiteral("up"), 100));
        st.add(QStringLiteral("up"), 50);
        st.add(QStringLiteral("down"), 7);
        db.flush();
        QCOMPARE(st.get(QStringLiteral("up")), 150);
        QCOMPARE(st.get(QStringLiteral("down")), 7);
        QCOMPARE(st.get(QStringLiteral("none")), 0);
    }

    void closeFlushesDeferredWrites()
    {
        {
            Database db;
            QVERIFY(db.open(path()));
            StatsDao(&db).add(QStringLiteral("flushOnClose"), 3);
        } // destructor → close → flush
        Database db;
        QVERIFY(db.open(path()));
        QCOMPARE(StatsDao(&db).get(QStringLiteral("flushOnClose")), 3);
    }
};

QTEST_GUILESS_MAIN(TestDatabase)
#include "tst_database.moc"
