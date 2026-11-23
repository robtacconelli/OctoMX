// SPDX-License-Identifier: GPL-3.0-or-later
// [qa-1] Behaviour regression tests: keyboard, selection, sorting, floating, sizes, dialogs.
#include <QtCore/QTemporaryDir>
#include <QtGui/QScreen>
#include <QtTest/QtTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>

#include "mx/services.h"
#include "ui/hotlist_pane.h"
#include "ui/kit/list_view.h"
#include "ui/kit/widgets.h"
#include "ui/main_window.h"
#include "ui/models/chat_models.h"
#include "ui/models/hotlist_model.h"
#include "ui/pages/networks_page.h"
#include "ui/pages/search_page.h"
#include "ui/pages/transfers_page.h"
#include "ui/dialogs/user_windows.h"
#include "uib_helpers.h"

using namespace mx;
using namespace mx::ui;

// answer the next modal message box / dialog with its default (accept) button
static void acceptNextModal(int ms = 50)
{
    QTimer::singleShot(ms, [] {
        if (QWidget *w = QApplication::activeModalWidget()) {
            if (auto *mb = qobject_cast<QMessageBox *>(w)) {
                QAbstractButton *b = mb->button(QMessageBox::Ok) ? mb->button(QMessageBox::Ok) : mb->button(QMessageBox::Yes);
                if (b)
                    b->click();
                return;
            }
            if (auto *d = qobject_cast<QDialog *>(w))
                d->accept();
        }
    });
}

class TestQa1Behaviour : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    uib::App m_app;

    SearchTab *searchArtist()
    {
        m_app.mw->showPage(QStringLiteral("search"));
        auto *sp = qobject_cast<SearchPage *>(m_app.mw->page(QStringLiteral("search")));
        SearchTab *t = sp->currentTab();
        auto *le = t->findChild<QLineEdit *>(QStringLiteral("searchText"));
        le->setText(QStringLiteral("velorine"));
        QTest::keyClick(le, Qt::Key_Return);
        for (int i = 0; i < 150 && t->list()->model()->rowCount() <= 20; ++i)
            QTest::qWait(20);
        return t;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_app.start(m_dir.filePath(QStringLiteral("p")));
    }
    void cleanupTestCase() { m_app.stop(); }

    void pageShortcuts()
    {
        MainWindow *mw = m_app.mw;
        mw->activateWindow();
        // Ctrl+1...7 follow the button row: Networks, Shared Files, Search, Transfers, Bandwidth, Chat, Hotlist
        const QStringList ids{QStringLiteral("networks"), QStringLiteral("shared"), QStringLiteral("search"),
                              QStringLiteral("transfers"), QStringLiteral("bandwidth"), QStringLiteral("chat")};
        QCOMPARE(MainWindow::pageIds(), ids);
        // the buttons stand in that order, the Hotlist button last
        int lastX = -1;
        QStringList row = ids;
        row << QStringLiteral("hotlist");
        for (const QString &id : std::as_const(row)) {
            QVERIFY(mw->pageButton(id)->x() > lastX);
            lastX = mw->pageButton(id)->x();
        }
        for (int i = 0; i < ids.size(); ++i) {
            if (ids[i] == QLatin1String("chat"))
                continue;   // the first visit shows the chat warning
            QTest::keyClick(mw, Qt::Key(Qt::Key_1 + i), Qt::ControlModifier);
            QCOMPARE(mw->currentPageId(), ids[i]);
        }
        QTest::keyClick(mw, Qt::Key_4, Qt::ControlModifier);
        QCOMPARE(mw->currentPageId(), QStringLiteral("transfers"));
        const bool hot = mw->isHotlistVisible();
        QTest::keyClick(mw, Qt::Key_7, Qt::ControlModifier);
        QCOMPARE(mw->isHotlistVisible(), !hot);
        QTest::keyClick(mw, Qt::Key_7, Qt::ControlModifier);
        QCOMPARE(mw->isHotlistVisible(), hot);
    }

    void selectAllAndTypeAhead()
    {
        SearchTab *t = searchArtist();
        MxListView *l = t->list();
        l->setFocus();
        QTest::keyClick(l, Qt::Key_A, Qt::ControlModifier);
        QCOMPARE(l->selectionModel()->selectedRows().size(), l->model()->rowCount());
        // type-ahead: "kvt" jumps to the first row starting with it ("kvt - Song For Velorine.mp3")
        l->clearSelection();
        l->setCurrentIndex(l->model()->index(0, 0));
        QTest::keyClicks(l, QStringLiteral("kvt"));
        QVERIFY2(l->currentIndex().data().toString().startsWith(QLatin1String("kvt")), qPrintable(l->currentIndex().data().toString()));
    }

    void headerClickSortsAndReverses()
    {
        SearchTab *t = searchArtist();
        MxListView *l = t->list();
        QHeaderView *h = l->header();
        const int bytesX = h->sectionViewportPosition(1) + h->sectionSize(1) / 2;
        QTest::mouseClick(h->viewport(), Qt::LeftButton, {}, QPoint(bytesX, 5));
        const Qt::SortOrder first = h->sortIndicatorOrder();
        QCOMPARE(h->sortIndicatorSection(), 1);
        QTest::mouseClick(h->viewport(), Qt::LeftButton, {}, QPoint(bytesX, 5));
        QCOMPARE(h->sortIndicatorSection(), 1);
        QVERIFY(h->sortIndicatorOrder() != first);
    }

    void enterDownloadsSelectedResult()
    {
        SearchTab *t = searchArtist();
        MxListView *l = t->list();
        const int before = int(m_app.core->transfers()->downloads().size());
        // a single-source row near the end
        const int row = l->model()->rowCount() - 1;
        l->setCurrentIndex(l->model()->index(row, 0));
        l->setFocus();
        QTest::keyClick(l, Qt::Key_Return);
        QTRY_COMPARE(int(m_app.core->transfers()->downloads().size()), before + 1);
    }

    void deleteKeyInLists()
    {
        MainWindow *mw = m_app.mw;
        // Hotlist: Del removes the selected entries
        mw->setHotlistVisible(true);
        auto *pane = mw->findChild<HotlistPane *>();
        QVERIFY(pane);
        MxListView *hl = pane->list();
        int userRow = -1;
        for (int r = 0; r < hl->model()->rowCount(); ++r)
            if (pane->model()->kind(r) != HotlistModel::SectionRow) {
                userRow = r;
                break;
            }
        QVERIFY(userRow >= 0);
        const int hotBefore = int(m_app.core->users()->hotlist().size());
        hl->setCurrentIndex(hl->model()->index(userRow, 0));
        hl->selectionModel()->select(hl->model()->index(userRow, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        hl->setFocus();
        acceptNextModal();
        QTest::keyClick(hl, Qt::Key_Delete);
        QTRY_COMPARE(int(m_app.core->users()->hotlist().size()), hotBefore - 1);
        // Ignored Users: Del removes
        mw->openIgnored();
        auto *ign = mw->findChild<IgnoredWindow *>();
        if (!ign)
            for (QWidget *w : QApplication::topLevelWidgets())
                if ((ign = qobject_cast<IgnoredWindow *>(w)))
                    break;
        QVERIFY(ign);
        const int ignBefore = ign->model()->rowCount();
        QVERIFY(ignBefore > 0);
        ign->list()->setCurrentIndex(ign->list()->model()->index(0, 0));
        ign->list()->selectionModel()->select(ign->list()->model()->index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        ign->list()->setFocus();
        acceptNextModal();
        QTest::keyClick(ign->list(), Qt::Key_Delete);
        QTRY_COMPARE(ign->model()->rowCount(), ignBefore - 1);
        ign->close();
        // Transfers uploads: Del cancels the selected upload
        mw->showPage(QStringLiteral("transfers"));
        auto *tp = qobject_cast<TransfersPage *>(mw->page(QStringLiteral("transfers")));
        MxListView *ul = tp->uploadList();
        QVERIFY(ul->model()->rowCount() > 0);
        int active = -1;
        const QList<TransferInfo> ups = m_app.core->transfers()->uploads();
        for (int r = 0; r < ul->model()->rowCount() && active < 0; ++r) {
            const QString text = ul->model()->index(r, 0).data().toString();
            for (const TransferInfo &u : ups)
                if (u.name == text && u.state == TransferState::Transferring)
                    active = r;
        }
        QVERIFY(active >= 0);
        const QString name = ul->model()->index(active, 0).data().toString();
        ul->setCurrentIndex(ul->model()->index(active, 0));
        ul->selectionModel()->select(ul->model()->index(active, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        ul->setFocus();
        QTest::keyClick(ul, Qt::Key_Delete);
        auto stillActive = [&] {
            for (const TransferInfo &u : m_app.core->transfers()->uploads())
                if (u.name == name && u.state == TransferState::Transferring)
                    return true;
            return false;
        };
        QTRY_VERIFY2(!stillActive(), "Del on the upload list cancels the selected upload");
    }

    // a download the core refuses is reported (status bar + information box), never silent
    void downloadNotStartedIsReported()
    {
        MainWindow *mw = m_app.mw;
        emit m_app.core->transfers()->downloadNotStarted(QStringLiteral("Big Buck Bunny"),
                                                         QStringLiteral("Nothing to download: the index gave no link or hash"));
        QVERIFY(mw->notice().contains(QStringLiteral("Nothing to download")));
        QMessageBox *box = nullptr;
        for (QWidget *w : QApplication::topLevelWidgets())
            if (w->objectName() == QLatin1String("downloadProblem") && w->isVisible())
                box = qobject_cast<QMessageBox *>(w);
        QVERIFY(box);
        QVERIFY(box->text().contains(QStringLiteral("Big Buck Bunny: Nothing to download")));
        // the same problem again does not stack up; a second one is added to the open box
        emit m_app.core->transfers()->downloadNotStarted(QStringLiteral("Big Buck Bunny"),
                                                         QStringLiteral("Nothing to download: the index gave no link or hash"));
        emit m_app.core->transfers()->downloadNotStarted(QStringLiteral("Sintel"), QStringLiteral("no source"));
        QCOMPARE(box->text().count(QStringLiteral("Big Buck Bunny")), 1);
        QVERIFY(box->text().contains(QStringLiteral("Sintel: no source")));
        box->close();
        QTest::qWait(10);
    }

    // per-download Auto Find Sources / Auto Enter Queue from the file-row menu (engine-4 API)
    void perDownloadAfsAeq()
    {
        MainWindow *mw = m_app.mw;
        mw->showPage(QStringLiteral("transfers"));
        auto *tp = qobject_cast<TransfersPage *>(mw->page(QStringLiteral("transfers")));
        MxListView *l = tp->downloadList();
        const QModelIndex first = l->model()->index(0, 0);
        l->selectionModel()->select(first, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        tp->showDownloadMenu(first, QPoint(5, 5));
        QTest::qWait(10);
        QMenu *menu = uib::openMenu();
        QVERIFY(menu);
        QMenu *afs = menu->findChild<QMenu *>(QStringLiteral("afsMenu"));
        QVERIFY(afs);
        QAction *every30 = uib::menuAction(afs, QStringLiteral("Every 30 min"));
        QVERIFY(every30);
        every30->trigger();
        QMenu *aeq = menu->findChild<QMenu *>(QStringLiteral("aeqMenu"));
        uib::menuAction(aeq, QStringLiteral("5"))->trigger();
        uib::closeMenus();
        const QString name = first.data().toString();
        TransferInfo d;
        for (const TransferInfo &t : m_app.core->transfers()->downloads())
            if (t.name == name)
                d = t;
        QCOMPARE(d.autoFindSourcesMin, 30);
        QCOMPARE(d.autoEnterQueue, 5);
        QTRY_VERIFY(l->model()->index(0, 1).data().toString().endsWith(QStringLiteral("30m - 5")));
    }

    void floatAndDockBack()
    {
        MainWindow *mw = m_app.mw;
        mw->showPage(QStringLiteral("bandwidth"));
        mw->floatPage(QStringLiteral("bandwidth"));
        QVERIFY(mw->isFloating(QStringLiteral("bandwidth")));
        QWidget *fw = mw->page(QStringLiteral("bandwidth"))->window();
        QVERIFY(fw != mw);
        // closing the floating window docks the page back (it is not lost)
        fw->close();
        QTest::qWait(20);
        QVERIFY(!mw->isFloating(QStringLiteral("bandwidth")));
        QCOMPARE(mw->page(QStringLiteral("bandwidth"))->window(), mw);
        mw->showPage(QStringLiteral("bandwidth"));
        QVERIFY(mw->page(QStringLiteral("bandwidth"))->isVisible());
    }

    void minimumAndLargeSizes()
    {
        MainWindow *mw = m_app.mw;
        const QSize old = mw->size();
        mw->resize(200, 150);
        QTest::qWait(10);
        // the original could be made narrower than its 682-px button band (networks_wpn_tab_354b3.png: 643 px)
        QVERIFY2(mw->width() <= 643, qPrintable(QStringLiteral("min width %1").arg(mw->width())));
        for (const QString &id : MainWindow::pageIds()) {
            if (id == QLatin1String("chat"))
                continue;
            mw->showPage(id);
            QTest::qWait(5);
        }
        // up to 1920x1080 (or the screen, which the window manager / Windows clamps to)
        const QSize avail = mw->screen()->availableGeometry().size();
        mw->resize(1920, 1080);
        QTest::qWait(10);
        if (avail.width() >= 1920 && avail.height() >= 1080)
            QCOMPARE(mw->size(), QSize(1920, 1080));
        else
            QVERIFY(mw->width() >= qMin(1920, avail.width()) - 40);
        mw->resize(old);
    }
};

QTEST_MAIN(TestQa1Behaviour)
#include "tst_qa1_behaviour.moc"
