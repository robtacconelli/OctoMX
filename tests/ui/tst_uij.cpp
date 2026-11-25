// SPDX-License-Identifier: GPL-3.0-or-later
// [ui-j] Removing a download (keep the file or delete it too), Shared Files ▸ Delete File... /
// Delete Folder..., the trash seam, and two Search fixes (a filter of exclusions only, the wording of
// eD2k / Kad source rows). Frozen demo core; every file this test deletes is one it created below
// its own folder in the build tree, and the "trash" is the seam under MX_OS_ROOT (set here to a
// folder of this test), never the real trash or Recycle Bin.
#include <QtCore/QDirIterator>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>

#include "uib_helpers.h"
#include "mx/settings_keys.h"
#include "mx/trash.h"
#include "ui/dialogs/remove_dialogs.h"
#include "ui/dialogs/settings_dialog.h"
#include "ui/kit/format.h"
#include "ui/kit/list_view.h"
#include "ui/kit/session.h"
#include "ui/kit/text_filter.h"
#include "ui/kit/widgets.h"
#include "ui/models/search_model.h"
#include "ui/models/shared_model.h"
#include "ui/models/transfer_model.h"
#include "ui/pages/search_page.h"
#include "ui/pages/shared_delete.h"
#include "ui/pages/shared_page.h"
#include "ui/pages/transfers_page.h"

using namespace mx;
using namespace mx::ui;

namespace {
void writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(data);
}

DownloadDiskInfo disk(bool finished, const QStringList &paths, qint64 bytes, qint64 total, bool trashOk = true)
{
    DownloadDiskInfo d;
    d.known = true;
    d.finished = finished;
    d.paths = paths;
    d.bytes = bytes;
    d.total = total;
    d.trash = trashOk;
    d.folder = paths.isEmpty() ? QString() : QFileInfo(paths.first()).absolutePath();
    return d;
}

TransferInfo info(const QString &id, const QString &name)
{
    TransferInfo t;
    t.id = id;
    t.name = name;
    return t;
}

// the files below a folder (relative paths), to prove "nothing else touched"
QStringList listing(const QString &dir)
{
    QStringList out;
    QDirIterator it(dir, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext())
        out << QDir(dir).relativeFilePath(it.next());
    out.sort();
    return out;
}
} // namespace

class TstUij : public QObject {
    Q_OBJECT
private:
    QString m_root;           // <build>/tst_uij_work
    QString m_osRoot;
    uib::App m_app;
    int m_run = 0;
    QList<RemoveTexts> m_asked;
    RemoveDownloadDialog::Choice m_answer = RemoveDownloadDialog::Cancel;
    bool m_tick = false;
    QStringList m_messages;

    QString seamFiles() const { return QDir(trash::seamDir()).filePath(QStringLiteral("files")); }
    QString profile() const { return m_root + QStringLiteral("/profile%1").arg(m_run); }
    Settings *st() const { return m_app.core->settings(); }
    TransfersPage *transfers() const { return qobject_cast<TransfersPage *>(m_app.mw->page(QStringLiteral("transfers"))); }
    SharedPage *shared() const { return qobject_cast<SharedPage *>(m_app.mw->page(QStringLiteral("shared"))); }
    QString incoming() const { return m_app.core->transfers()->incomingDir(); }
    bool hasRow(const QString &id) const { return transfers()->downloadModel()->rowOfId(id) >= 0; }

    // a fresh demo core and window in a profile of its own (the demo rows d1 ... d11 again)
    void freshApp()
    {
        m_app.stop();
        ++m_run;
        QDir(profile()).removeRecursively();
        QVERIFY(QDir().mkpath(profile()));
        m_app.start(profile());
        m_app.mw->showPage(QStringLiteral("transfers"));
        m_asked.clear();
        m_messages.clear();
    }
    void select(const QStringList &ids)
    {
        MxListView *l = transfers()->downloadList();
        l->clearSelection();
        for (const QString &id : ids) {
            const QModelIndex vi = l->mapFromSource(transfers()->downloadModel()->index(transfers()->downloadModel()->rowOfId(id), 0));
            l->selectionModel()->select(vi, QItemSelectionModel::Select | QItemSelectionModel::Rows);
        }
    }
    void click(const char *name)
    {
        PushButton *b = transfers()->findChild<PushButton *>(QString::fromLatin1(name));
        QVERIFY(b);
        b->click();
        QTest::qWait(20);
    }
    void menuPick(const QString &id, const QString &entry)
    {
        MxListView *l = transfers()->downloadList();
        const QModelIndex vi = l->mapFromSource(transfers()->downloadModel()->index(transfers()->downloadModel()->rowOfId(id), 0));
        transfers()->showDownloadMenu(vi, QPoint(5, 5));
        QTest::qWait(10);
        QAction *a = uib::menuAction(uib::openMenu(), entry);
        QVERIFY2(a, qPrintable(entry));
        QVERIFY(a->isEnabled());
        QVERIFY(!a->toolTip().isEmpty() && a->toolTip() != entry);
        a->trigger();
        uib::closeMenus();
        QTest::qWait(20);
    }
    QString demoFile(const QString &id) const { return m_app.core->transfers()->transfer(id).savePath; }

private slots:
    void initTestCase()
    {
        // in the build tree, but not in the program's folder (Shared Files never deletes there)
        m_root = QStringLiteral(MX_UIJ_WORK_DIR);
#ifdef Q_OS_WIN
        if (m_root.startsWith(QLatin1Char('/')))
            m_root.prepend(QStringLiteral("Z:"));   // built on Linux, run under wine
#endif
        QDir(m_root).removeRecursively();
        QVERIFY(QDir().mkpath(m_root));
        // the trash seam of this test (never the real trash), under the build tree
        m_osRoot = m_root + QStringLiteral("/os-root");
        qputenv("MX_OS_ROOT", m_osRoot.toUtf8());
        qunsetenv("MX_TRASH");
        QVERIFY(trash::redirected());
        QVERIFY(trash::seamDir().startsWith(m_root));
        RemoveDownloadDialog::s_execHook = [this](RemoveDownloadDialog *d) {
            m_asked << d->texts();
            // what the user sees: Cancel is the default button, every button has a tool tip
            if (!d->cancelButton()->isDefault() || d->keepButton()->isDefault() || d->deleteButton()->isDefault())
                return int(RemoveDownloadDialog::Cancel);
            if (m_tick)
                d->dontAskBox()->setChecked(true);
            return int(m_answer);
        };
        SharedPage::s_messageHook = [this](const QString &title, const QString &text) { m_messages << title + QLatin1Char('|') + text; };
        TransfersPage::s_messageHook = [this](const QString &why) { m_messages << why; };
        freshApp();
    }
    void cleanupTestCase()
    {
        m_app.stop();
        RemoveDownloadDialog::s_execHook = nullptr;
        DeleteFilesDialog::s_execHook = nullptr;
        SharedPage::s_messageHook = nullptr;
        TransfersPage::s_messageHook = nullptr;
    }

    // ------------------------------------------------------------------------------ trash seam
    void trashSeam()
    {
        const QString dir = m_root + QStringLiteral("/seam");
        writeFile(dir + QStringLiteral("/a.txt"), "a");
        writeFile(dir + QStringLiteral("/sub/b.txt"), "bb");
        writeFile(seamFiles() + QStringLiteral("/a.txt"), "older");   // a name already in the trash
        QVERIFY(trash::available(dir + QStringLiteral("/a.txt")));
        const trash::Result r = trash::dispose({dir + QStringLiteral("/a.txt"), dir + QStringLiteral("/sub")}, false);
        QVERIFY(r.ok());
        QCOMPARE(r.trashed.size(), 2);
        QVERIFY(!QFileInfo::exists(dir + QStringLiteral("/a.txt")));
        QVERIFY(!QFileInfo::exists(dir + QStringLiteral("/sub")));
        QVERIFY(QFileInfo::exists(seamFiles() + QStringLiteral("/a (1).txt")));   // the older one stays
        QVERIFY(QFileInfo::exists(seamFiles() + QStringLiteral("/sub/b.txt")));   // a folder goes as one
        QFile infoFile(QDir(trash::seamDir()).filePath(QStringLiteral("info/a (1).txt.trashinfo")));
        QVERIFY(infoFile.open(QIODevice::ReadOnly));
        QVERIFY(infoFile.readAll().contains(QUrl::toPercentEncoding(QFileInfo(dir).absoluteFilePath(), "/")));
        // a volume without a trash: refused unless the user was told "for good"
        qputenv("MX_TRASH", "none");
        writeFile(dir + QStringLiteral("/c.txt"), "c");
        QVERIFY(!trash::available(dir + QStringLiteral("/c.txt")));
        trash::Result n = trash::dispose({dir + QStringLiteral("/c.txt")}, false);
        QCOMPARE(n.failed, QStringList{dir + QStringLiteral("/c.txt")});
        QVERIFY(QFileInfo::exists(dir + QStringLiteral("/c.txt")));
        n = trash::dispose({dir + QStringLiteral("/c.txt")}, true);
        QCOMPARE(n.deleted.size(), 1);
        QVERIFY(!QFileInfo::exists(dir + QStringLiteral("/c.txt")));
        qunsetenv("MX_TRASH");
        // never "/" or a relative path; a path that is gone already is no failure
        n = trash::dispose({QStringLiteral("/"), QStringLiteral("relative.txt"), dir + QStringLiteral("/gone")}, true);
        QCOMPARE(n.failed.size(), 2);
        QVERIFY(n.trashed.isEmpty() && n.deleted.isEmpty());
        // a link is moved, never what it points to
#ifndef Q_OS_WIN
        writeFile(dir + QStringLiteral("/target/keep.txt"), "k");
        QVERIFY(QFile::link(dir + QStringLiteral("/target"), dir + QStringLiteral("/link")));
        QVERIFY(trash::dispose({dir + QStringLiteral("/link")}, false).ok());
        QVERIFY(QFileInfo::exists(dir + QStringLiteral("/target/keep.txt")));
        qputenv("MX_TRASH", "none");
        QVERIFY(QFile::link(dir + QStringLiteral("/target"), dir + QStringLiteral("/link2")));
        QVERIFY(trash::dispose({dir + QStringLiteral("/link2")}, true).ok());
        QVERIFY(QFileInfo::exists(dir + QStringLiteral("/target/keep.txt")));
        qunsetenv("MX_TRASH");
#endif
    }

    // The real platform call, only by hand: MX_TRASH_PROBE=<a file this run created> moves that one
    // file with QFile::moveToTrash (MX_OS_ROOT is unset for it). Used under wine with HOME and the
    // XDG folders redirected into the build tree, to see what wine does with the Recycle Bin.
    void realTrashProbe()
    {
        const QString probe = qEnvironmentVariable("MX_TRASH_PROBE");
        if (probe.isEmpty())
            QSKIP("MX_TRASH_PROBE not set (the real trash is never used by the normal run)");
        QVERIFY(QFileInfo(probe).isFile());
        const QByteArray saved = qgetenv("MX_OS_ROOT");
        qunsetenv("MX_OS_ROOT");
        QVERIFY(!trash::redirected());
        const bool avail = trash::available(probe);
        QString where, err;
        const bool ok = trash::moveToTrash(probe, &where, &err);
        qputenv("MX_OS_ROOT", saved);
        qInfo("probe %s: available %d, moved %d, at \"%s\", error \"%s\", still there %d", qPrintable(probe), avail, ok, qPrintable(where),
              qPrintable(err), int(QFileInfo::exists(probe)));
    }

    // ------------------------------------------------------------------------------ the question
    void removeTextsVariants()
    {
        const QString where = trash::placeName();
        // one finished file, to the trash
        RemoveTexts t = removeTexts({{info(QStringLiteral("a"), QStringLiteral("Big Buck Bunny.mp4")),
                                      disk(true, {QStringLiteral("/x/in/Big Buck Bunny.mp4")}, 263 << 20, 263 << 20)}});
        QCOMPARE(t.question, QStringLiteral("Remove \"Big Buck Bunny.mp4\" from the download list?"));
        QCOMPARE(t.keepButton, QStringLiteral("Remove from the list, keep the file"));
        QCOMPARE(t.deleteButton, QStringLiteral("Remove from the list and delete the file"));
        QVERIFY(t.details.contains(QStringLiteral("finished")) && t.details.contains(QStringLiteral("263 MB")));
        QVERIFY(t.deleteLine.contains(QStringLiteral("moved to the %1").arg(where)));
        QVERIFY(!t.forGood && t.canDelete && t.warning.isEmpty());
        // one partial download: what is on the disk, how it is resumed, what deleting does
        t = removeTexts({{info(QStringLiteral("b"), QStringLiteral("ubuntu.iso")),
                          disk(false, {QStringLiteral("/x/inc/b.part")}, 120 << 20, 263 << 20)}});
        QVERIFY(t.details.contains(QStringLiteral("not finished")) && t.details.contains(QStringLiteral("120 of 263 MB")));
        QVERIFY(t.keepLine.contains(QStringLiteral("Load Incomplete")));
        QVERIFY(t.deleteLine.contains(QStringLiteral("partial file (120 MB) is moved to the %1").arg(where)));
        // no trash on that drive: "deleted for good" in the question, in bold, with the warning icon
        t = removeTexts({{info(QStringLiteral("c"), QStringLiteral("debian.iso")),
                          disk(false, {QStringLiteral("/net/inc/c.part")}, 50 << 20, 100 << 20, false)}});
        QVERIFY(t.forGood);
        QVERIFY(t.deleteLine.contains(QStringLiteral("deleted for good")));
        QVERIFY(t.deleteLine.contains(QStringLiteral("cannot be restored")));
        QVERIFY(t.warning.isEmpty());   // said once, in the bold delete line
        // nothing on the disk: the delete button is off
        t = removeTexts({{info(QStringLiteral("d"), QStringLiteral("x.mp3")), disk(false, {}, 0, 5 << 20)}});
        QVERIFY(!t.canDelete);
        QVERIFY(t.details.contains(QStringLiteral("Nothing of this download is on the disk")));
        // resumed in place from a file the user chose
        DownloadDiskInfo inPlace = disk(false, {QStringLiteral("/home/u/Music/song.mp3")}, 3 << 20, 5 << 20);
        inPlace.inPlaceFile = QStringLiteral("/home/u/Music/song.mp3");
        t = removeTexts({{info(QStringLiteral("e"), QStringLiteral("song.mp3")), inPlace}});
        QVERIFY(t.warning.contains(QStringLiteral("writes into it")) && t.warning.contains(QStringLiteral("song.mp3")));
        // files the download did not create stay in any case and are named
        DownloadDiskInfo kept = disk(true, {}, 0, 10);
        kept.kept << QStringLiteral("/x/in/old.iso");
        kept.keptWhy = QStringLiteral("it was on the disk before this download started");
        t = removeTexts({{info(QStringLiteral("f"), QStringLiteral("old.iso")), kept}});
        QVERIFY(t.keptNote.contains(QStringLiteral("old.iso")) && t.keptNote.contains(QStringLiteral("before this download")));
        // several: one question, "these 5 downloads", their size on the disk, plural buttons
        QList<RemoveItem> five;
        for (int i = 0; i < 5; ++i)
            five << RemoveItem{info(QString::number(i), QStringLiteral("file %1.iso").arg(i)),
                               disk(i < 2, {QStringLiteral("/x/%1").arg(i)}, qint64(100) << 20, qint64(200) << 20)};
        t = removeTexts(five);
        QCOMPARE(t.question, QStringLiteral("Remove these 5 downloads from the download list?"));
        QCOMPARE(t.keepButton, QStringLiteral("Remove from the list, keep the files"));
        QCOMPARE(t.deleteButton, QStringLiteral("Remove from the list and delete the files"));
        QVERIFY(t.details.contains(QStringLiteral("500 MB")));
        QVERIFY(t.details.contains(QStringLiteral("2 finished files")) && t.details.contains(QStringLiteral("partial data of 3 downloads")));
        QCOMPARE(t.list.split(QLatin1Char('\n')).size(), 5);
        QVERIFY(t.deleteLine.contains(QStringLiteral("all of it (500 MB) is moved to the %1").arg(where)));
        five[3].disk.trash = false;
        t = removeTexts(five);
        QVERIFY(t.forGood && t.deleteLine.contains(QStringLiteral("deleted for good")));
        // more than six: "and 2 more"
        for (int i = 5; i < 8; ++i)
            five << RemoveItem{info(QString::number(i), QStringLiteral("more %1").arg(i)), disk(true, {QStringLiteral("/x/m%1").arg(i)}, 1, 1)};
        QVERIFY(removeTexts(five).list.endsWith(QStringLiteral("and 2 more")));
    }

    void dialogDefaultsToCancel()
    {
        RemoveDownloadDialog dlg({{info(QStringLiteral("a"), QStringLiteral("Big Buck Bunny.mp4")),
                                   disk(true, {m_root + QStringLiteral("/none.mp4")}, 10, 10)}},
                                 m_app.mw);
        QVERIFY(dlg.cancelButton()->isDefault());
        QVERIFY(!dlg.keepButton()->isDefault() && !dlg.deleteButton()->isDefault());
        for (QWidget *w : QList<QWidget *>{dlg.keepButton(), dlg.deleteButton(), dlg.cancelButton(), dlg.dontAskBox()})
            QVERIFY2(!w->toolTip().isEmpty(), qPrintable(w->objectName()));
        // Enter and Esc both leave everything as it is
        dlg.open();
        QVERIFY(QTest::qWaitForWindowExposed(&dlg));
        QTest::keyClick(&dlg, Qt::Key_Return);
        QTRY_VERIFY(!dlg.isVisible());
        QCOMPARE(dlg.choice(), RemoveDownloadDialog::Cancel);
        QCOMPARE(dlg.result(), int(QDialog::Rejected));
        dlg.open();
        QVERIFY(QTest::qWaitForWindowExposed(&dlg));
        QTest::keyClick(&dlg, Qt::Key_Escape);
        QTRY_VERIFY(!dlg.isVisible());
        QCOMPARE(dlg.choice(), RemoveDownloadDialog::Cancel);
        DeleteFilesDialog del(DeleteTexts{QStringLiteral("Delete File"), QStringLiteral("Delete?"), QStringLiteral("/x"), QString(),
                                          QString(), QString(), QString(), QStringLiteral("Delete File"), false, false},
                              m_app.mw);
        QVERIFY(del.cancelButton()->isDefault() && !del.deleteButton()->isDefault());
        del.open();
        QVERIFY(QTest::qWaitForWindowExposed(&del));
        QTest::keyClick(&del, Qt::Key_Return);
        QTRY_VERIFY(!del.isVisible());
        QCOMPARE(del.result(), int(QDialog::Rejected));
    }

    // ------------------------------------------------------------------------------ transfers
    void keepAndDeleteFinished()
    {
        freshApp();
        const QString file = demoFile(QStringLiteral("d5"));   // the finished demo download's placeholder
        QVERIFY(QFileInfo::exists(file));
        writeFile(incoming() + QStringLiteral("/someone else's file.txt"), "not ours");
        const QStringList before = listing(incoming());
        // Clear + "keep": the row goes, the file stays
        select({QStringLiteral("d5")});
        m_answer = RemoveDownloadDialog::Keep;
        click("dlClear");
        QCOMPARE(m_asked.size(), 1);
        QVERIFY(m_asked.first().details.contains(QStringLiteral("finished")));
        QVERIFY(!hasRow(QStringLiteral("d5")));
        QVERIFY(QFileInfo::exists(file));
        QCOMPARE(listing(incoming()), before);
        // again, now Del + "delete": the file goes to the trash, nothing else in the folder is touched
        freshApp();
        const QString file2 = demoFile(QStringLiteral("d5"));
        QVERIFY(QFileInfo::exists(file2));
        writeFile(incoming() + QStringLiteral("/someone else's file.txt"), "not ours");
        const QStringList all = listing(incoming());
        QSignalSpy deleted(m_app.core->transfers(), &TransferService::downloadFilesDeleted);
        select({QStringLiteral("d5")});
        m_answer = RemoveDownloadDialog::Delete;
        QTest::keyClick(transfers()->downloadList(), Qt::Key_Delete);
        QCOMPARE(m_asked.size(), 1);
        QVERIFY(m_asked.first().deleteLine.contains(QStringLiteral("moved to the")));
        QVERIFY(!hasRow(QStringLiteral("d5")));
        QTRY_COMPARE(deleted.count(), 1);
        QCOMPARE(deleted.first().at(2).toStringList(), QStringList{file2});   // trashed
        QVERIFY(!QFileInfo::exists(file2));
        QVERIFY(QFileInfo::exists(file));   // the first profile's copy was kept above
        QVERIFY(QFileInfo::exists(seamFiles() + QLatin1Char('/') + QFileInfo(file2).fileName()));
        QStringList rest = all;
        rest.removeAll(QFileInfo(file2).fileName());
        QCOMPARE(listing(incoming()), rest);
        QTRY_VERIFY(transfers()->statusFields().join(QLatin1Char(' ')).contains(QStringLiteral("to the")));
        // Cancel in the question leaves everything as it is
        freshApp();
        select({QStringLiteral("d5")});
        m_answer = RemoveDownloadDialog::Cancel;
        click("dlClear");
        QCOMPARE(m_asked.size(), 1);
        QVERIFY(hasRow(QStringLiteral("d5")));
        QVERIFY(QFileInfo::exists(demoFile(QStringLiteral("d5"))));
    }

    void partialKeepAndDelete()
    {
        freshApp();
        // d7: a failed download with 1.2 MB; its partial file here is one this test writes
        auto partOf = [this] {
            return QDir(m_app.core->transfers()->incompleteDir()).filePath(
                QStringLiteral("__INCOMPLETE___") + m_app.core->transfers()->transfer(QStringLiteral("d7")).name);
        };
        QString part = partOf();
        writeFile(part, QByteArray(1234567, 'p'));
        select({QStringLiteral("d7")});
        m_answer = RemoveDownloadDialog::Keep;
        click("dlCancel");   // Cancel = the same question for an unfinished download
        QCOMPARE(m_asked.size(), 1);
        QVERIFY(m_asked.first().details.contains(QStringLiteral("not finished")));
        QVERIFY(m_asked.first().keepLine.contains(QStringLiteral("continues it")));
        QVERIFY(!hasRow(QStringLiteral("d7")));
        QVERIFY(QFileInfo::exists(part));
        freshApp();
        part = partOf();
        writeFile(part, QByteArray(1234567, 'p'));
        m_answer = RemoveDownloadDialog::Delete;
        select({QStringLiteral("d7")});   // a right click selects the row first
        menuPick(QStringLiteral("d7"), QStringLiteral("Cancel"));
        QCOMPARE(m_asked.size(), 1);
        QTRY_VERIFY(!QFileInfo::exists(part));
        QVERIFY(QFileInfo::exists(seamFiles() + QLatin1Char('/') + QFileInfo(part).fileName()));
    }

    void multiSelectionOneQuestion()
    {
        freshApp();
        const QString finishedFile = demoFile(QStringLiteral("d5"));
        select({QStringLiteral("d3"), QStringLiteral("d5"), QStringLiteral("d7"), QStringLiteral("d8"), QStringLiteral("d10")});
        m_answer = RemoveDownloadDialog::Keep;
        click("dlClear");
        QCOMPARE(m_asked.size(), 1);   // one question for all of them
        QCOMPARE(m_asked.first().question, QStringLiteral("Remove these 5 downloads from the download list?"));
        QVERIFY(m_asked.first().details.startsWith(QStringLiteral("On the disk: ")));
        for (const char *id : {"d3", "d5", "d7", "d8", "d10"})
            QVERIFY(!hasRow(QString::fromLatin1(id)));
        QVERIFY(QFileInfo::exists(finishedFile));
        // Cancel acts on the unfinished ones only: a finished selection alone asks nothing
        freshApp();
        select({QStringLiteral("d5")});
        click("dlCancel");
        QCOMPARE(m_asked.size(), 0);
        QVERIFY(hasRow(QStringLiteral("d5")));
        // the context menu's Delete File... on a selected row takes the whole selection
        select({QStringLiteral("d5"), QStringLiteral("d7")});
        m_answer = RemoveDownloadDialog::Cancel;
        menuPick(QStringLiteral("d7"), QStringLiteral("Delete File..."));
        QCOMPARE(m_asked.size(), 1);
        QVERIFY(m_asked.first().question.contains(QStringLiteral("these 2 downloads")));
        QVERIFY(hasRow(QStringLiteral("d5")) && hasRow(QStringLiteral("d7")));
    }

    void rememberedChoiceAndSetting()
    {
        freshApp();
        QCOMPARE(st()->s(keys::transfers::RemoveAction), QStringLiteral("ask"));
        // "Do not ask again" + keep: the setting says keep, the next removal asks nothing
        m_tick = true;
        m_answer = RemoveDownloadDialog::Keep;
        select({QStringLiteral("d10")});
        click("dlClear");
        m_tick = false;
        QCOMPARE(m_asked.size(), 1);
        QCOMPARE(st()->s(keys::transfers::RemoveAction), QStringLiteral("keep"));
        const QString kept = demoFile(QStringLiteral("d5"));
        select({QStringLiteral("d5")});
        click("dlClear");
        QCOMPARE(m_asked.size(), 1);
        QVERIFY(!hasRow(QStringLiteral("d5")));
        QTest::qWait(100);
        QVERIFY(QFileInfo::exists(kept));
        // Delete File... always asks, whatever is remembered
        m_answer = RemoveDownloadDialog::Cancel;
        menuPick(QStringLiteral("d3"), QStringLiteral("Delete File..."));
        QCOMPARE(m_asked.size(), 2);
        QVERIFY(hasRow(QStringLiteral("d3")));
        // the setting in Settings ▸ File Transfers: three choices, a tool tip, applied at once
        m_app.mw->openSettings(QStringLiteral("transfers"));
        auto *d = m_app.mw->findChild<SettingsDialog *>();
        QVERIFY(d);
        auto *cb = d->findChild<QComboBox *>(QStringLiteral("set_transfers_removeAction"));
        QVERIFY(cb);
        QVERIFY(cb->isVisibleTo(d));
        QVERIFY(cb->toolTip().contains(QStringLiteral("trash")));
        QCOMPARE(cb->count(), 3);
        QCOMPARE(cb->currentText(), QStringLiteral("Keep the file"));
        const int di = cb->findText(QStringLiteral("Delete the file too"));
        cb->setCurrentIndex(di);
        emit cb->activated(di);   // what a click on the entry does
        QCOMPARE(st()->s(keys::transfers::RemoveAction), QStringLiteral("delete"));
        QPointer<SettingsDialog> p(d);
        d->close();
        QTRY_VERIFY(p.isNull());
        // remembered "delete": to the trash without a question
        freshApp();
        st()->setValue(keys::transfers::RemoveAction, QStringLiteral("delete"));
        const QString file = demoFile(QStringLiteral("d5"));
        select({QStringLiteral("d5")});
        click("dlClear");
        QCOMPARE(m_asked.size(), 0);
        QTRY_VERIFY(!QFileInfo::exists(file));
        // ... but never "for good" without the question
        freshApp();
        st()->setValue(keys::transfers::RemoveAction, QStringLiteral("delete"));
        qputenv("MX_TRASH", "none");
        select({QStringLiteral("d5")});
        m_answer = RemoveDownloadDialog::Cancel;
        click("dlClear");
        qunsetenv("MX_TRASH");
        QCOMPARE(m_asked.size(), 1);
        QVERIFY(m_asked.first().forGood);
        QVERIFY(hasRow(QStringLiteral("d5")));
        QVERIFY(QFileInfo::exists(demoFile(QStringLiteral("d5"))));
        // a profile that had the retired "Confirm cancel" off (no question, the partial file kept):
        // the question starts as "Keep the file"; with it on (the default) it asks
        st()->setValue(keys::transfers::RemoveAction, QVariant());
        st()->setValue(keys::transfers::ConfirmCancel, false);
        st()->setValue(QStringLiteral("ui/settingsMigrated"), 2);
        migrateLegacySettings(st());
        QCOMPARE(st()->s(keys::transfers::RemoveAction), QStringLiteral("keep"));
        st()->setValue(keys::transfers::RemoveAction, QVariant());
        st()->setValue(keys::transfers::ConfirmCancel, QVariant());
        st()->setValue(QStringLiteral("ui/settingsMigrated"), 2);
        migrateLegacySettings(st());
        QCOMPARE(st()->s(keys::transfers::RemoveAction), QStringLiteral("ask"));
    }

    void bulkButtonsNeverDelete()
    {
        freshApp();
        st()->setValue(keys::transfers::RemoveAction, QStringLiteral("delete"));   // even then
        const QString file = demoFile(QStringLiteral("d5"));
        click("dlClearComplete");
        QCOMPARE(m_asked.size(), 0);
        QVERIFY(!hasRow(QStringLiteral("d5")));
        QTest::qWait(100);
        QVERIFY(QFileInfo::exists(file));
        // uploads: Clear / Cancel / the bulk buttons never touch the shared file
        const QStringList filesBefore = listing(profile());
        TransferModel *um = transfers()->uploadModel();
        QVERIFY(um->rowCount() > 0);
        transfers()->uploadList()->selectAll();
        transfers()->findChild<PushButton *>(QStringLiteral("ulCancel"))->click();
        transfers()->findChild<PushButton *>(QStringLiteral("ulClear"))->click();
        transfers()->findChild<PushButton *>(QStringLiteral("ulClearComplete"))->click();
        transfers()->findChild<PushButton *>(QStringLiteral("ulClearIncomplete"))->click();
        QTest::qWait(50);
        QCOMPARE(m_asked.size(), 0);
        QCOMPARE(listing(profile()), filesBefore);
        st()->setValue(keys::transfers::RemoveAction, QStringLiteral("ask"));
    }

    // ------------------------------------------------------------------------------ shared files
    void sharedDeleteRules()
    {
        const QString base = m_root + QStringLiteral("/rules");
        QDir(base).removeRecursively();
        const QString share = base + QStringLiteral("/Share");
        writeFile(share + QStringLiteral("/a.mp3"), "aaaa");
        writeFile(share + QStringLiteral("/Album/b.mp3"), "bbbbbb");
        writeFile(share + QStringLiteral("/Album/c.mp3"), "cc");
        writeFile(share + QStringLiteral("/Incoming/done.iso"), "iso");
        writeFile(share + QStringLiteral("/Incomplete/x.part"), "part");
        writeFile(share + QStringLiteral("/Profile/settings.json"), "{}");
        writeFile(share + QStringLiteral("/Running/ubuntu.iso"), "partial");
        writeFile(share + QStringLiteral("/Up/sent.mp3"), "123456789");
        writeFile(base + QStringLiteral("/Outside/secret.txt"), "s");
        shdel::Context ctx;
        ctx.roots << share;
        ctx.incoming = share + QStringLiteral("/Incoming");
        ctx.incomplete = share + QStringLiteral("/Incomplete");
        ctx.profile = share + QStringLiteral("/Profile");
        ctx.program = base + QStringLiteral("/Program");
        ctx.busy << shdel::Busy{share + QStringLiteral("/Running/ubuntu.iso"), QStringLiteral("ubuntu.iso"), -1, QString(), false};
        ctx.busy << shdel::Busy{QString(), QStringLiteral("sent.mp3"), 9, QStringLiteral("Bobwib234"), true};
        auto refused = [&](const QString &path, bool folder, const QString &word) {
            const shdel::Plan p = shdel::plan({shdel::Item{path, folder}}, ctx);
            if (p.refusal.isEmpty() || !p.refusal.contains(word))
                qWarning() << path << p.refusal;
            return !p.refusal.isEmpty() && p.refusal.contains(word) && p.paths.isEmpty();
        };
        // allowed: a file, a sub folder, the shared root itself, a file in the incoming folder
        shdel::Plan p = shdel::plan({shdel::Item{share + QStringLiteral("/a.mp3"), false}}, ctx);
        QVERIFY(p.refusal.isEmpty());
        QCOMPARE(p.paths, QStringList{share + QStringLiteral("/a.mp3")});
        p = shdel::plan({shdel::Item{share + QStringLiteral("/Album"), true}}, ctx);
        QVERIFY(p.refusal.isEmpty());
        QCOMPARE(p.files, 2);
        QCOMPARE(p.bytes, qint64(8));
        QVERIFY(p.roots.isEmpty());
        QVERIFY(shdel::plan({shdel::Item{share + QStringLiteral("/Incoming/done.iso"), false}}, ctx).refusal.isEmpty());
        // the selection: a folder and a file inside it count once
        p = shdel::plan({shdel::Item{share + QStringLiteral("/Album/b.mp3"), false}, shdel::Item{share + QStringLiteral("/Album"), true}}, ctx);
        QCOMPARE(p.paths, QStringList{share + QStringLiteral("/Album")});
        // refused, with the reason
        QVERIFY(refused(base + QStringLiteral("/Outside/secret.txt"), false, QStringLiteral("not inside one of your shared folders")));
        QVERIFY(refused(share + QStringLiteral("/Incoming"), true, QStringLiteral("incoming folder")));
        QVERIFY(refused(share, true, QStringLiteral("incoming folder")));   // it holds the incoming folder
        QVERIFY(refused(share + QStringLiteral("/Incomplete"), true, QStringLiteral("unfinished downloads")));
        QVERIFY(refused(share + QStringLiteral("/Incomplete/x.part"), false, QStringLiteral("unfinished downloads")));
        QVERIFY(refused(share + QStringLiteral("/Profile"), true, QStringLiteral("profile")));
        QVERIFY(refused(share + QStringLiteral("/Profile/settings.json"), false, QStringLiteral("profile")));
        QVERIFY(refused(share + QStringLiteral("/Running"), true, QStringLiteral("ubuntu.iso")));
        QVERIFY(refused(share + QStringLiteral("/Running/ubuntu.iso"), false, QStringLiteral("Remove that download")));
        QVERIFY(refused(share + QStringLiteral("/Up/sent.mp3"), false, QStringLiteral("being sent to Bobwib234")));
        QVERIFY(refused(share + QStringLiteral("/Up"), true, QStringLiteral("being sent")));
        // a refusal of one item refuses the whole selection
        p = shdel::plan({shdel::Item{share + QStringLiteral("/a.mp3"), false}, shdel::Item{share + QStringLiteral("/Up"), true}}, ctx);
        QVERIFY(!p.refusal.isEmpty() && p.paths.isEmpty());
        // the program folder, and a folder that holds it
        ctx.roots << base;
        ctx.program = share + QStringLiteral("/Album");
        QVERIFY(refused(share + QStringLiteral("/Album/b.mp3"), false, QStringLiteral("program")));
        ctx.program = base + QStringLiteral("/Program");
        ctx.roots.removeAll(base);
#ifndef Q_OS_WIN
        // a linked folder inside the share that leads outside it
        QVERIFY(QFile::link(base + QStringLiteral("/Outside"), share + QStringLiteral("/Linked")));
        QVERIFY(refused(share + QStringLiteral("/Linked/secret.txt"), false, QStringLiteral("link")));
        QVERIFY(QFileInfo::exists(base + QStringLiteral("/Outside/secret.txt")));
#endif
        // a shared root: its share goes too, the texts say it unmistakably
        ctx.incoming = base + QStringLiteral("/Elsewhere");
        ctx.incomplete.clear();
        ctx.profile.clear();
        ctx.busy.clear();
        p = shdel::plan({shdel::Item{share, true}}, ctx);
        QVERIFY2(p.refusal.isEmpty(), qPrintable(p.refusal));
        QCOMPARE(p.roots, QStringList{share});
        const DeleteTexts t = shdel::texts({shdel::Item{share, true}}, p);
        QVERIFY(t.boldTarget);
        QVERIFY(t.details.contains(QStringLiteral("removes the share AND deletes its content")));
        QVERIFY(t.hint.contains(QStringLiteral("Unshare Folder")));
        QCOMPARE(t.button, QStringLiteral("Delete Folder"));
        QVERIFY(t.effect.contains(QStringLiteral("moved to the %1").arg(trash::placeName())));
        qputenv("MX_TRASH", "none");
        const DeleteTexts g = shdel::texts({shdel::Item{share, true}}, shdel::plan({shdel::Item{share, true}}, ctx));
        qunsetenv("MX_TRASH");
        QVERIFY(g.forGood && g.effect.contains(QStringLiteral("deleted for good")) && g.warning.contains(QStringLiteral("cannot be undone")));
        // nothing was deleted by any of this
        QVERIFY(QFileInfo::exists(share + QStringLiteral("/a.mp3")) && QFileInfo::exists(share + QStringLiteral("/Up/sent.mp3")));
    }

    void sharedDeleteOnThePage()
    {
        freshApp();
        const QString share = m_root + QStringLiteral("/share%1").arg(m_run);
        QDir(share).removeRecursively();
        writeFile(share + QStringLiteral("/Moonlit Ferry.mp3"), QByteArray(4000, 'm'));
        writeFile(share + QStringLiteral("/Old Album/one.mp3"), QByteArray(3000, '1'));
        writeFile(share + QStringLiteral("/Old Album/two.mp3"), QByteArray(2000, '2'));
        writeFile(share + QStringLiteral("/keep.mp3"), QByteArray(1000, 'k'));
        LibraryService *lib = m_app.core->library();
        lib->addFolder(share, true);
        QTRY_VERIFY(lib->files(share).size() == 4);
        m_app.mw->showPage(QStringLiteral("shared"));
        SharedPage *page = shared();
        page->reload();
        QList<DeleteTexts> asked;
        int answer = QDialog::Rejected;
        DeleteFilesDialog::s_execHook = [&](DeleteFilesDialog *d) {
            asked << d->texts();
            return d->cancelButton()->isDefault() ? answer : int(QDialog::Rejected);
        };
        // a file: Cancel leaves it; Delete moves it to the trash and the library forgets it at once
        page->deleteSelected({share + QStringLiteral("/Moonlit Ferry.mp3")});
        QCOMPARE(asked.size(), 1);
        QCOMPARE(asked.first().question, QStringLiteral("Delete the file \"Moonlit Ferry.mp3\" from your disk?"));
        QVERIFY(asked.first().effect.contains(QStringLiteral("shared files and the disk")));
        QVERIFY(QFileInfo::exists(share + QStringLiteral("/Moonlit Ferry.mp3")));
        answer = QDialog::Accepted;
        page->deleteSelected({share + QStringLiteral("/Moonlit Ferry.mp3")});
        QVERIFY(!QFileInfo::exists(share + QStringLiteral("/Moonlit Ferry.mp3")));
        QVERIFY(QFileInfo::exists(seamFiles() + QStringLiteral("/Moonlit Ferry.mp3")));
        QCOMPARE(lib->files(share).size(), 3);
        // a sub folder: number of files and their size
        page->deleteSelected({QDir::fromNativeSeparators(share + QStringLiteral("/Old Album"))});
        QCOMPARE(asked.last().target, QStringLiteral("Old Album"));
        QVERIFY(asked.last().details.endsWith(QStringLiteral(": 2 files, %1.").arg(fmt::size(5000))));
        QVERIFY(!QFileInfo::exists(share + QStringLiteral("/Old Album")));
        QCOMPARE(lib->files(share).size(), 1);
        QVERIFY(QFileInfo::exists(share + QStringLiteral("/keep.mp3")));
        // a refusal is a message, nothing is asked or deleted: the incoming folder shared as a root
        const QString in = incoming();
        writeFile(in + QStringLiteral("/done.mp3"), "done");
        lib->addFolder(in, true);
        QTRY_VERIFY(!lib->files(in).isEmpty());
        const int n = int(asked.size());
        page->deleteSelected({QDir::fromNativeSeparators(in)});
        QCOMPARE(int(asked.size()), n);
        QVERIFY(!m_messages.isEmpty() && m_messages.last().contains(QStringLiteral("incoming folder")));
        QVERIFY(QFileInfo::exists(in + QStringLiteral("/done.mp3")));
        // the shared root: the share goes and its content is deleted (bold name, Unshare hint)
        page->deleteSelected({QDir::fromNativeSeparators(share)});
        QVERIFY(asked.last().boldTarget);
        QVERIFY(!QFileInfo::exists(share));
        bool stillShared = false;
        for (const SharedFolder &f : lib->folders())
            stillShared = stillShared || QDir::cleanPath(QDir::fromNativeSeparators(f.path)) == QDir::cleanPath(share);
        QVERIFY(!stillShared);
        QVERIFY(lib->files(share).isEmpty());
        // the context menus have the entries, with their tool tips
        writeFile(share + QStringLiteral("2/x.mp3"), "x");
        lib->addFolder(share + QStringLiteral("2"), true);
        QTRY_VERIFY(!lib->files(share + QStringLiteral("2")).isEmpty());
        page->reload();
        MxListView *l = page->list();
        QModelIndex folderRow;
        for (int r = 0; r < l->model()->rowCount(); ++r)
            if (l->model()->index(r, 0).data().toString().contains(QStringLiteral("share%12").arg(m_run)))
                folderRow = l->model()->index(r, 0);
        QVERIFY(folderRow.isValid());
        page->showContextMenu(folderRow, QPoint(5, 5));
        QTest::qWait(10);
        QAction *df = uib::menuAction(uib::openMenu(), QStringLiteral("Delete Folder..."));
        QVERIFY(df && df->statusTip().contains(QStringLiteral("You are asked first")));
        uib::closeMenus();
        // the file row comes a moment later ([mac-1] a CI Mac), and on a busy machine the list can be
        // filled again while the menu opens: look the rows up afresh until the file's menu is there
        const auto fileMenuHasDelete = [&] {
            QModelIndex fr;
            for (int r = 0; r < l->model()->rowCount(); ++r)
                if (l->model()->index(r, 0).data().toString().contains(QStringLiteral("share%12").arg(m_run)))
                    fr = l->model()->index(r, 0);
            if (!fr.isValid()) return false;
            l->expand(fr);
            const QModelIndex file = l->model()->index(0, 0, fr);
            if (!file.isValid()) return false;
            page->showContextMenu(file, QPoint(5, 5));
            QTest::qWait(10);
            QAction *dfile = uib::menuAction(uib::openMenu(), QStringLiteral("Delete File..."));
            const bool ok = dfile && dfile->statusTip().contains(QStringLiteral("You are asked first"));
            uib::closeMenus();
            return ok;
        };
        QTRY_VERIFY_WITH_TIMEOUT(fileMenuHasDelete(), 10000);
        // Del on the list asks (Cancel here)
        answer = QDialog::Rejected;
        const int k = int(asked.size());
        // [mac-1] a late library update reloads the list and drops the selection (seen on a CI Mac):
        // select the file row again until the key reaches a selected row
        auto pressDel = [&] {
            QModelIndex folder;
            for (int r = 0; r < l->model()->rowCount(); ++r)
                if (l->model()->index(r, 0).data().toString().contains(QStringLiteral("share%12").arg(m_run)))
                    folder = l->model()->index(r, 0);
            if (!folder.isValid())
                return false;
            l->expand(folder);
            const QModelIndex file = l->model()->index(0, 0, folder);
            if (!file.isValid())
                return false;
            l->setCurrentIndex(file);
            l->selectionModel()->select(file, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            QTest::keyClick(l, Qt::Key_Delete);
            return int(asked.size()) > k;
        };
        for (int tries = 0; tries < 50 && !pressDel(); ++tries)
            QTest::qWait(100);
        QCOMPARE(int(asked.size()), k + 1);
        QVERIFY(QFileInfo::exists(share + QStringLiteral("2/x.mp3")));
        DeleteFilesDialog::s_execHook = nullptr;
    }

    // ------------------------------------------------------------------------------ search fixes
    void filterOfExclusionsOnly()
    {
        TextFilter f(QStringLiteral("-debian -ubuntu"));
        QVERIFY(!f.isEmpty());
        QVERIFY(f.hasRequired(QStringLiteral("anything")));
        QVERIFY(f.excludes(QStringLiteral("debian-13.iso")) && !f.excludes(QStringLiteral("fedora.iso")));
        freshApp();
        m_app.mw->showPage(QStringLiteral("search"));
        auto *sp = qobject_cast<SearchPage *>(m_app.mw->page(QStringLiteral("search")));
        SearchTab *t = sp->currentTab();
        t->setTexts(QStringLiteral("linux"), QString());
        t->startSearch();
        QTest::qWait(50);
        t->flushPending();
        MxListView *l = t->list();
        const int total = l->model()->rowCount();
        int debianRows = 0;
        for (int r = 0; r < total; ++r)
            debianRows += l->model()->index(r, 0).data().toString().contains(QStringLiteral("debian"), Qt::CaseInsensitive) ? 1 : 0;
        QVERIFY(debianRows > 0 && debianRows < total);
        QLineEdit *edit = t->filterRow()->edit();
        edit->setText(QStringLiteral("-debian"));
        QTRY_COMPARE(l->filterMatches(), total - debianRows);
        for (int r = 0; r < total; ++r) {
            const QModelIndex i = l->model()->index(r, 0);
            const bool deb = i.data().toString().contains(QStringLiteral("debian"), Qt::CaseInsensitive);
            QCOMPARE(l->isRowHidden(r, QModelIndex()), deb);
        }
        // a word of the merged row that its source rows do not all have (the network): the row goes,
        // a source row without the word no longer keeps it on the list (the reported bug)
        int ed2kRows = 0;
        for (int r = 0; r < total; ++r)
            ed2kRows += l->model()->index(r, search::Network).data().toString().contains(QStringLiteral("eD2k")) ? 1 : 0;
        QVERIFY(ed2kRows > 0 && ed2kRows < total);
        edit->setText(QStringLiteral("-eD2k -Kad"));
        QTRY_VERIFY(!l->filterPending());
        QVERIFY(l->filterMatches() <= total - ed2kRows);
        for (int r = 0; r < total; ++r) {
            const QString net = l->model()->index(r, search::Network).data().toString();
            if (net.contains(QStringLiteral("eD2k")) || net.contains(QStringLiteral("Kad")))
                QVERIFY2(l->isRowHidden(r, QModelIndex()), qPrintable(net));
            else
                QVERIFY2(!l->isRowHidden(r, QModelIndex()), qPrintable(net));
        }
        // with a positive word as well: the same rule
        edit->setText(QStringLiteral("iso -debian"));
        QTRY_VERIFY(l->filterMatches() < total);
        for (int r = 0; r < total; ++r)
            if (!l->isRowHidden(r, QModelIndex()))
                QVERIFY(!l->model()->index(r, 0).data().toString().contains(QStringLiteral("debian"), Qt::CaseInsensitive));
        edit->clear();
        QTRY_COMPARE(l->filterMatches(), total);
    }

    void sourceRowWording()
    {
        SearchModel m;
        auto src = [](Net n, int seeds, int peers) {
            SearchSource s;
            s.net = n;
            s.seeds = seeds;
            s.peers = peers;
            s.connection = netName(n);
            return s;
        };
        SearchHit h;
        h.key = QStringLiteral("k");
        h.fileName = QStringLiteral("Big Buck Bunny 1080p.avi");
        h.size = 734003200;
        h.sources = {src(Net::ED2K, 2, 4), src(Net::Kad, 2, 2), src(Net::BitTorrent, 2, 2)};
        m.applyHits({h});
        const QModelIndex parent = m.index(m.rowOfKey(QStringLiteral("k")), 0);
        QCOMPARE(m.rowCount(parent), 3);
        QStringList texts;
        for (int r = 0; r < 3; ++r)
            texts << m.index(r, search::Status, parent).data().toString();
        QVERIFY2(texts.contains(QStringLiteral("4 sources (2 complete)")), qPrintable(texts.join(QLatin1Char('|'))));
        QVERIFY2(texts.contains(QStringLiteral("2 sources")), qPrintable(texts.join(QLatin1Char('|'))));
        QVERIFY2(texts.contains(QStringLiteral("2 seeds, 2 peers")), qPrintable(texts.join(QLatin1Char('|'))));   // BitTorrent keeps its words
        int btWords = 0;
        for (const QString &x : std::as_const(texts))
            btWords += x.contains(QStringLiteral("seed")) ? 1 : 0;
        QCOMPARE(btWords, 1);
    }
};

QTEST_MAIN(TstUij)
#include "tst_uij.moc"
