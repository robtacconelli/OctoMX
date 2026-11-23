// SPDX-License-Identifier: GPL-3.0-or-later
// ui-b: the Shared Files page on the frozen demo core, tree contents, actions reaching
// LibraryService, list updates on signals, context menu, persistence across a restart, speed.
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtWidgets/QHeaderView>

#include "uib_helpers.h"
#include "ui/kit/list_view.h"
#include "ui/models/roles.h"
#include "ui/models/shared_model.h"
#include "ui/pages/shared_page.h"

using namespace mx;
using namespace mx::ui;

class TstUibShared : public QObject {
    Q_OBJECT
private:
    QTemporaryDir m_dir;
    uib::App m_app;
    QString m_share;   // a real folder with files

    SharedPage *page() const { return qobject_cast<SharedPage *>(m_app.mw->page(QStringLiteral("shared"))); }
    QString profile() const { return m_dir.filePath(QStringLiteral("profile")); }

    static void touch(const QString &path, int bytes)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QByteArray(bytes, 'x'));
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_share = m_dir.filePath(QStringLiteral("My Music"));
        touch(m_share + QStringLiteral("/a.mp3"), 1000);
        touch(m_share + QStringLiteral("/b.mp3"), 2000);
        touch(m_share + QStringLiteral("/c.txt"), 30);
        touch(m_share + QStringLiteral("/Sub/d.mp3"), 4000);
        touch(m_share + QStringLiteral("/Sub/e.mp3"), 5000);
        m_app.start(profile());
        m_app.mw->showPage(QStringLiteral("shared"));
        QTest::qWait(50);
    }

    void cleanupTestCase() { m_app.stop(); }

    void demoContent()
    {
        SharedPage *p = page();
        QVERIFY(p);
        QCOMPARE(p->model()->fileCount(), m_app.core->library()->fileCount());
        QVERIFY(p->model()->folderCount() >= 8);
        QCOMPARE(p->list()->model()->columnCount(), 5);
        QCOMPARE(p->list()->model()->headerData(0, Qt::Horizontal).toString(), QStringLiteral("File Path"));
        QCOMPARE(p->list()->model()->headerData(4, Qt::Horizontal).toString(), QStringLiteral("Freq"));
        // folder rows: native path with trailing separator, folder icon
        const QModelIndex first = p->list()->model()->index(0, 0);
        QVERIFY(first.data().toString().endsWith(QDir::separator()));
        QVERIFY(first.data(Qt::DecorationRole).isValid());
        QVERIFY(p->statusFields().value(0).contains(QStringLiteral("files shared")));
        QVERIFY(p->statusFields().value(0).startsWith(QString::number(m_app.core->library()->fileCount())));
        // the five buttons in the 682-px band
        for (const char *n : {"sharedShareFolder", "sharedUnshareFolder", "sharedRefresh", "sharedFileTypes", "sharedScanForMedia"})
            QVERIFY2(p->findChild<QWidget *>(QString::fromLatin1(n)), n);
        QCOMPARE(p->findChild<QWidget *>(QStringLiteral("sharedScanForMedia"))->geometry().right(), 683);
    }

    void shareRealFolderUpdatesList()
    {
        SharedPage *p = page();
        const int before = p->model()->fileCount();
        QSignalSpy spy(m_app.core->library(), &LibraryService::foldersChanged);
        QString err;
        QVERIFY(p->scriptCommand(QStringLiteral("share-folder"), {QDir::toNativeSeparators(m_share)}, &err));
        QVERIFY(err.isEmpty());
        QCOMPARE(spy.count(), 1);
        QTRY_COMPARE(p->model()->fileCount(), before + 5);   // reload is debounced
        QVERIFY(p->model()->folderIndex(shared::dirKey(m_share)).isValid());
        QVERIFY(p->model()->folderIndex(shared::dirKey(m_share + QStringLiteral("/Sub"))).isValid());
        QVERIFY(p->model()->isSharedRoot(p->model()->folderIndex(shared::dirKey(m_share))));
        QVERIFY(!p->model()->isSharedRoot(p->model()->folderIndex(shared::dirKey(m_share + QStringLiteral("/Sub")))));
    }

    void expansionSurvivesReload()
    {
        SharedPage *p = page();
        const QModelIndex src = p->model()->folderIndex(shared::dirKey(m_share));
        const QModelIndex vi = p->list()->mapFromSource(src);
        p->list()->expand(vi);
        QCOMPARE(p->list()->model()->rowCount(vi), 3);
        p->reload();
        const QModelIndex vi2 = p->list()->mapFromSource(p->model()->folderIndex(shared::dirKey(m_share)));
        QVERIFY(p->list()->isExpanded(vi2));
    }

    void unshareFileFromMenu()
    {
        SharedPage *p = page();
        const QModelIndex folder = p->list()->mapFromSource(p->model()->folderIndex(shared::dirKey(m_share)));
        p->list()->expand(folder);
        const QModelIndex fileRow = p->list()->model()->index(0, 0, folder);
        QVERIFY(fileRow.isValid());
        const QString path = fileRow.data(IdRole).toString();
        p->list()->selectRow(fileRow);
        p->showContextMenu(fileRow, p->list()->viewport()->mapToGlobal(QPoint(40, 40)));
        QMenu *m = uib::openMenu();
        QVERIFY(m);
        QCOMPARE(m->defaultAction()->text(), QStringLiteral("Open File"));   // [ui-e]
        QVERIFY(uib::menuAction(m, QStringLiteral("Open Containing Folder")));
        QAction *un = uib::menuAction(m, QStringLiteral("Un-share File"));
        QVERIFY(un);
        un->trigger();
        uib::closeMenus();
        bool still = false;
        for (const SharedFile &f : m_app.core->library()->files())
            if (f.path == path)
                still = true;
        QVERIFY(!still);
        QTRY_VERIFY(!p->model()->fileIndex(path).isValid());
    }

    void folderMenuMatchesReference()
    {
        SharedPage *p = page();
        const QModelIndex vi = p->list()->model()->index(0, 0);
        p->list()->collapse(vi);
        p->showContextMenu(vi, p->list()->viewport()->mapToGlobal(QPoint(40, 8)));
        QMenu *m = uib::openMenu();
        QVERIFY(m);
        QStringList texts;
        for (QAction *a : m->actions())
            texts << (a->isSeparator() ? QStringLiteral("-") : a->text());
        // the 3.53 entries, then [ui-j] Delete Folder... (UI_SPEC 8.11)
        QCOMPARE(texts, (QStringList{QStringLiteral("Expand"), QStringLiteral("-"), QStringLiteral("Open Folder"), QStringLiteral("-"),
                                     QStringLiteral("Un-share Folder"), QStringLiteral("Delete Folder...")}));
        QCOMPARE(m->defaultAction()->text(), QStringLiteral("Expand"));
        uib::menuAction(m, QStringLiteral("Expand"))->trigger();
        uib::closeMenus();
        QVERIFY(p->list()->isExpanded(vi));
    }

    void unshareMenuListsFolders()
    {
        SharedPage *p = page();
        p->showUnshareMenu();
        QMenu *m = uib::openMenu();
        QVERIFY(m);
        QCOMPARE(m->actions().size(), m_app.core->library()->folders().size());
        uib::closeMenus();
    }

    void persistsAcrossRestart()
    {
        SharedPage *p = page();
        p->list()->header()->resizeSection(1, 140);
        p->saveState();
        const int files = m_app.core->library()->fileCount();
        m_app.stop();
        m_app.start(profile());
        p = page();
        QVERIFY(p);
        bool found = false;
        for (const SharedFolder &f : m_app.core->library()->folders())
            if (shared::dirKey(f.path) == shared::dirKey(m_share))
                found = true;
        QVERIFY(found);
        QCOMPARE(m_app.core->library()->fileCount(), files);   // incl. the excluded file staying excluded
        QCOMPARE(p->list()->header()->sectionSize(1), 140);
    }

    void unshareFolder()
    {
        SharedPage *p = page();
        const int before = p->model()->fileCount();
        QString err;
        p->scriptCommand(QStringLiteral("unshare-folder"), {QDir::toNativeSeparators(m_share)}, &err);
        QTRY_COMPARE(p->model()->fileCount(), before - 4);
        QVERIFY(!p->model()->folderIndex(shared::dirKey(m_share)).isValid());
    }

    void liveHashingProgress()
    {
        // without --demo-frozen the demo hasher works through a few files and reports progress
        uib::App live;
        QTemporaryDir d;
        live.start(d.filePath(QStringLiteral("p")), false);
        auto *p = qobject_cast<SharedPage *>(live.mw->page(QStringLiteral("shared")));
        live.mw->showPage(QStringLiteral("shared"));
        QVERIFY(p->model()->hashedCount() < p->model()->fileCount());
        QTRY_VERIFY_WITH_TIMEOUT(p->statusFields().value(1).startsWith(QStringLiteral("Hashing ")), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(p->model()->hashedCount() == p->model()->fileCount(), 8000);
        QTRY_COMPARE(p->statusFields().size(), 1);
        live.stop();
    }

    void bigLibraryStaysFast()
    {
        QList<SharedFolder> roots;
        SharedFolder r;
        r.path = QStringLiteral("/big");
        roots << r;
        QList<SharedFile> files;
        for (int i = 0; i < 20000; ++i) {
            SharedFile f;
            f.name = QStringLiteral("Track %1.mp3").arg(i);
            f.path = QStringLiteral("/big/Artist %1/").arg(i % 400) + f.name;
            f.size = 4000000 + i;
            f.bitrateKbps = 128;
            f.hashed = true;
            files << f;
        }
        SharedFilesModel m;
        QElapsedTimer t;
        t.start();
        m.setContent(roots, files);
        const qint64 ms = t.elapsed();
        qInfo("20000 files in 400 folders: %lld ms", ms);
        QCOMPARE(m.fileCount(), 20000);
        QCOMPARE(m.folderCount(), 401);
        QVERIFY(ms < 1500);
    }
};

QTEST_MAIN(TstUibShared)
#include "tst_uib_shared.moc"
