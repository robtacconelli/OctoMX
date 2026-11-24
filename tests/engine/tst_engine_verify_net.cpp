// SPDX-License-Identifier: GPL-3.0-or-later
// [ver-1] A finished file that Verify File finds damaged is not offered as a complete file while it
// is repaired: it leaves the Library's shared files and a second OctoMX network node no longer finds
// it by search; once the damaged block is back (and checked) both offer it again. The owner's
// OctoMX network plugin is fed by the core's Library, as in OctoMX; the download itself comes from a
// fake source (local only).
#include <QtCore/QDir>
#include <QtTest/QTest>

#include "engine_env.h"
#include "ovl_test_support.h"
#include "mxhash/tiger_tree.h"

using namespace mx;
using namespace mx::etest;

class TstEngineVerifyNet : public QObject {
    Q_OBJECT
private slots:
    void damagedFileNotOfferedWhileRepaired();
};

void TstEngineVerifyNet::damagedFileNotOfferedWhileRepaired()
{
    Env env(QStringLiteral("ver-net"), /*withLibrary*/ true);
    const QString in = env.root + QStringLiteral("/incoming");
    QDir().mkpath(in);
    env.settings.setValue(QString(keys::library::WatchFolders), false);
    env.library->addFolder(in, true);
    auto *http = env.addPlugin(Net::Http, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(3 * 1024 * 1024 + 77, 9100);
    SourceConfig c;
    c.data = data;
    const QString uri = QStringLiteral("http://192.0.2.20/lantern.bin");
    const SearchSource s = http->addSource(uri, c);
    env.engineUp();
    QVERIFY(env.library->waitIdle(30000));

    ovltest::Instance owner(QStringLiteral("VerOwner"));
    ovltest::Instance asker(QStringLiteral("VerAsker"));
    QObject::connect(env.library.get(), &Library::sharedFilesUpdated, owner.plugin,
                     [&owner](const QList<SharedFile> &f) { owner.plugin->setSharedFiles(f); });
    owner.start();
    asker.start();
    QVERIFY(ovltest::link(asker, owner));
    quint32 qid = 0;
    auto found = [&] {   // the second node's search answer
        QSignalSpy hits(asker.plugin, &NetworkPlugin::searchHits);
        QSignalSpy done(asker.plugin, &NetworkPlugin::searchDone);
        SearchQuery q;
        q.id = ++qid;
        q.text = QStringLiteral("lantern");
        asker.plugin->search(q);
        (void)QTest::qWaitFor([&] { return !done.isEmpty(); }, 15000);
        for (const auto &a : hits)
            for (const SearchHit &h : a.at(1).value<QList<SearchHit>>())
                if (h.fileName == QLatin1String("Lantern Repair.bin")) return true;
        return false;
    };
    auto shared = [&] {
        for (const SharedFile &f : env.library->files())
            if (f.name == QLatin1String("Lantern Repair.bin")) return true;
        return false;
    };

    DownloadRequest r;
    r.name = QStringLiteral("Lantern Repair.bin");
    r.size = data.size();
    r.hashes = hashesOf(data, 1);
    r.sources = {s};
    const QString id = env.engine->download(r);
    QVERIFY(env.waitDone(id, 30000));
    const QString path = env.info(id).savePath;
    QTRY_VERIFY_WITH_TIMEOUT(shared(), 30000);
    QTRY_VERIFY_WITH_TIMEOUT(found(), 40000);

    // damaged on disk; the repair is slow enough to look at it in the middle
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadWrite) && f.seek(1024 * 1024 + 9));
        f.write("XYZ");
    }
    http->configs[uri].bytesPerSec = 16 * 1024;
    QSignalSpy notices(env.engine.get(), &TransferService::downloadNotice);
    QVERIFY(env.engine->verifyDownload(id));
    QTRY_VERIFY_WITH_TIMEOUT(!notices.isEmpty(), 20000);
    QVERIFY2(notices.last().at(1).toString().contains(QStringLiteral("being downloaded again")), qPrintable(notices.last().at(1).toString()));
    QVERIFY(env.info(id).state != TransferState::Completed);
    QVERIFY(!shared());                // the Library does not list it as a shared file
    QVERIFY(env.library->isBusy(path));
    QVERIFY(!found());                 // and the other node does not find it
    QVERIFY(env.info(id).state != TransferState::Completed);   // (still repairing while it was asked)

    http->configs[uri].bytesPerSec = 0;
    QVERIFY2(env.waitDone(id, 120000), qPrintable(env.info(id).statusText));
    QCOMPARE(readFile(path), data);
    QTRY_VERIFY_WITH_TIMEOUT(shared(), 30000);
    QTRY_VERIFY_WITH_TIMEOUT(found(), 40000);
}

QTEST_GUILESS_MAIN(TstEngineVerifyNet)
#include "tst_engine_verify_net.moc"
