// SPDX-License-Identifier: GPL-3.0-or-later
// [ui-e] modern usability in the classic WinMX style: human-readable sizes, the column chooser, proportional
// columns, the interface scale (geometry, uniform chrome lines, font sizes), Open File / Open
// Containing Folder (fake launcher), failure feedback, connection-derived slots, notifications.
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtGui/QPainter>
#include <QtTest/QtTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QHeaderView>

#include "uib_helpers.h"
#include "mx/settings_keys.h"
#include "ui/kit/draw.h"
#include "ui/kit/format.h"
#include "ui/kit/launcher.h"
#include "ui/kit/list_view.h"
#include "ui/kit/scale.h"
#include "ui/kit/session.h"
#include "ui/kit/colors.h"
#include "ui/kit/icons.h"
#include "ui/kit/theme.h"
#include "ui/kit/widgets.h"
#include "ui/models/search_model.h"
#include "ui/pages/search_page.h"
#include "ui/pages/transfers_page.h"
#include "ui/dialogs/settings_dialog.h"
#include "ui/dialogs/settings_binding.h"
#include <QtWidgets/QComboBox>
#include <QtWidgets/QSplitter>

using namespace mx;
using namespace mx::ui;

class TstUie : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    uib::App m_app;
    QString profile() const { return m_dir.filePath(QStringLiteral("p")); }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        fmt::setGroupSeparator(QLatin1Char(' '));
        m_app.start(profile());
    }
    void cleanupTestCase() { m_app.stop(); }

    void humanReadableSizes()
    {
        QCOMPARE(fmt::size(0), QStringLiteral("0 B"));
        QCOMPARE(fmt::size(512), QStringLiteral("512 B"));
        QCOMPARE(fmt::size(1000), QStringLiteral("0.98 KB"));
        QCOMPARE(fmt::size(812 * 1024), QStringLiteral("812 KB"));
        QCOMPARE(fmt::size(qint64(45.7 * 1024 * 1024)), QStringLiteral("45.7 MB"));
        QCOMPARE(fmt::size(qint64(1.16 * 1024 * 1024 * 1024)), QStringLiteral("1.16 GB"));
        QCOMPARE(fmt::size(qint64(3) << 40), QStringLiteral("3.00 TB"));
        QCOMPARE(fmt::sizeOf(qint64(12.4 * 1048576), qint64(45.7 * 1048576)), QStringLiteral("12.4 of 45.7 MB"));
        QCOMPARE(fmt::sizeOf(0, qint64(45.7 * 1048576)), QStringLiteral("0 of 45.7 MB"));
        QCOMPARE(fmt::sizeOf(5000, 0), QStringLiteral("4.88 KB of ?"));
        QCOMPARE(fmt::rate(6666), QStringLiteral("6.51 KB/s"));
        QCOMPARE(fmt::rate(1310720), QStringLiteral("1.25 MB/s"));
        QCOMPARE(fmt::etaText(44), QStringLiteral("44 s"));
        QCOMPARE(fmt::etaText(274), QStringLiteral("4 min 34 s"));
        QCOMPARE(fmt::etaText(3 * 3600 + 12 * 60 + 5), QStringLiteral("3 h 12 min"));
        QCOMPARE(fmt::etaText(2 * 86400 + 3 * 3600), QStringLiteral("2 d 3 h"));
        QCOMPARE(fmt::exactBytes(4388571), QStringLiteral("4 388 571 bytes"));
        // a German-style locale: 1.234.567 grouping goes with a decimal comma
        qputenv("MX_DIGIT_GROUPING", "dot");
        QCOMPARE(fmt::size(qint64(45.7 * 1048576)), QStringLiteral("45,7 MB"));
        qunsetenv("MX_DIGIT_GROUPING");
        // "Show exact byte counts": the classic formats
        fmt::setExactSizes(true);
        QCOMPARE(fmt::size(4388571), QStringLiteral("4 388 571"));
        QCOMPARE(fmt::sizeOf(583408, 6110000), QStringLiteral("583 408 of 6 110 000"));
        QCOMPARE(fmt::rate(6666), QStringLiteral("6.51"));
        QCOMPARE(fmt::etaText(274), QStringLiteral("4:34"));
        fmt::setExactSizes(false);
    }

    void exactSizesSettingIsLive()
    {
        auto *page = qobject_cast<TransfersPage *>(m_app.mw->page(QStringLiteral("transfers")));
        QVERIFY(page);
        auto *list = page->findChild<MxListView *>(QStringLiteral("downloadList"));
        QVERIFY(list);
        QCOMPARE(list->model()->headerData(3, Qt::Horizontal).toString(), QStringLiteral("Size"));
        m_app.core->settings()->setValue(keys::ui::ExactSizes, true);
        QVERIFY(fmt::exactSizes());
        QCOMPARE(list->model()->headerData(3, Qt::Horizontal).toString(), QStringLiteral("Bytes"));
        m_app.core->settings()->setValue(keys::ui::ExactSizes, false);
        QVERIFY(!fmt::exactSizes());
    }

    void columnChooserAndReset()
    {
        m_app.mw->showPage(QStringLiteral("search"));
        auto *page = qobject_cast<SearchPage *>(m_app.mw->page(QStringLiteral("search")));
        QVERIFY(page);
        MxListView *list = page->currentTab()->findChild<MxListView *>(QStringLiteral("searchResults"));
        QVERIFY(list);
        QHeaderView *h = list->header();
        // default: User / Speed / Ping hidden, the new columns shown
        QVERIFY(h->isSectionHidden(search::User) && h->isSectionHidden(search::Speed) && h->isSectionHidden(search::Ping));
        QVERIFY(!h->isSectionHidden(search::Network) && !h->isSectionHidden(search::Peers));
        QMenu *menu = list->columnMenu();
        QCOMPARE(menu->objectName(), QStringLiteral("columnMenu"));
        QAction *user = uib::menuAction(menu, QStringLiteral("User"));
        QVERIFY(user && user->isCheckable() && !user->isChecked());
        QAction *file = uib::menuAction(menu, QStringLiteral("File"));
        QVERIFY(file && !file->isEnabled());   // the first column always stays
        QVERIFY(uib::menuAction(menu, QStringLiteral("Reset Columns")));
        user->trigger();   // checking it shows the column
        QVERIFY(!h->isSectionHidden(search::User));
        delete menu;
        list->setColumnVisible(search::Peers, false);
        QVERIFY(h->isSectionHidden(search::Peers));
        list->resetColumns();
        QVERIFY(h->isSectionHidden(search::User));
        QVERIFY(!h->isSectionHidden(search::Peers));
        QVERIFY(!list->hasUserLayout());
    }

    void columnsFollowTheWindow()
    {
        auto *page = qobject_cast<SearchPage *>(m_app.mw->page(QStringLiteral("search")));
        MxListView *list = page->currentTab()->findChild<MxListView *>(QStringLiteral("searchResults"));
        QHeaderView *h = list->header();
        m_app.mw->resize(1024, 689);
        QTest::qWait(20);
        // untouched: File takes the slack of a wider window, the other columns keep their size
        const int file0 = h->sectionSize(search::File), size0 = h->sectionSize(search::Size);
        m_app.mw->resize(1400, 689);
        QTest::qWait(20);
        QVERIFY(h->sectionSize(search::File) > file0 + 250);
        QCOMPARE(h->sectionSize(search::Size), size0);
        // a column resized by hand: from now on every column keeps its share of the width
        h->resizeSection(search::Size, 120);
        QVERIFY(list->hasUserLayout());
        const int avail = list->availableWidth();
        const double shareFile = double(h->sectionSize(search::File)) / avail, shareSize = 120.0 / avail;
        m_app.mw->resize(1000, 689);
        QTest::qWait(20);
        const int avail2 = list->availableWidth();
        QVERIFY(qAbs(h->sectionSize(search::File) - shareFile * avail2) <= 2);
        QVERIFY(qAbs(h->sectionSize(search::Size) - shareSize * avail2) <= 2);
        // the layout persists with the fractions
        list->saveState();
        const QVariantMap stored = m_app.core->settings()->value(keys::ui::columnsKey(QStringLiteral("search"))).toMap();
        QVERIFY(stored.contains(QStringLiteral("fractions")));
        list->resetColumns();
        m_app.mw->resize(1024, 689);
        QTest::qWait(20);
    }

    void rowsFollowTheWindow()
    {
        // the seven page buttons share a wider window; at the classic width they are the classic 94 px
        m_app.mw->resize(690, 445);
        QTest::qWait(20);
        QCOMPARE(m_app.mw->pageButton(QStringLiteral("networks"))->width(), 94);
        m_app.mw->resize(1200, 700);
        QTest::qWait(20);
        QVERIFY(m_app.mw->pageButton(QStringLiteral("networks"))->width() > 140);
        m_app.mw->resize(1024, 689);
        QTest::qWait(20);
    }

    void openFileAndContainingFolder()
    {
        QStringList calls;
        launcher::setHook([&calls](const QString &action, const QString &path) {
            calls << action + QLatin1Char(' ') + QFileInfo(path).fileName();
            return true;
        });
        // a plain file opens; Open Containing Folder shows the file
        const QString f = m_dir.filePath(QStringLiteral("song.mp3"));
        QFile out(f);
        QVERIFY(out.open(QIODevice::WriteOnly));
        out.write("x");
        out.close();
        QString why;
        QVERIFY(launcher::openFile(nullptr, m_app.core->settings(), f, &why));
        QVERIFY(launcher::showInFolder(f, &why));
        QCOMPARE(calls, QStringList({QStringLiteral("open song.mp3"), QStringLiteral("show song.mp3")}));
        // a dangerous type asks first; "No" opens nothing and is not an error
        const QString exe = m_dir.filePath(QStringLiteral("setup.exe"));
        QFile e(exe);
        QVERIFY(e.open(QIODevice::WriteOnly));
        e.close();
        int asked = 0;
        launcher::setAskHook([&asked](QWidget *, const QString &) { ++asked; return false; });
        calls.clear();
        why.clear();
        QVERIFY(!launcher::openFile(nullptr, m_app.core->settings(), exe, &why));
        QCOMPARE(asked, 1);
        QVERIFY(why.isEmpty());
        QVERIFY(calls.isEmpty());
        // a file that is gone says so
        QVERIFY(!launcher::openFile(nullptr, m_app.core->settings(), m_dir.filePath(QStringLiteral("gone.avi")), &why));
        QVERIFY(why.contains(QStringLiteral("not there")));
        // the completed demo download: double-click and the menu entries
        m_app.mw->showPage(QStringLiteral("transfers"));
        auto *page = qobject_cast<TransfersPage *>(m_app.mw->page(QStringLiteral("transfers")));
        MxListView *list = page->findChild<MxListView *>(QStringLiteral("downloadList"));
        QModelIndex done;
        for (int i = 0; i < list->visibleRowCount(); ++i) {
            const QModelIndex r = list->visibleRow(i);
            if (r.data().toString() == QLatin1String("Big Buck Bunny 1080p.mkv"))
                done = r;
        }
        QVERIFY(done.isValid());
        calls.clear();
        emit list->rowActivated(done);
        QCOMPARE(calls, QStringList({QStringLiteral("open Big Buck Bunny 1080p.mkv")}));
        list->selectRow(done);
        emit list->contextMenuAt(done, list->viewport()->mapToGlobal(QPoint(30, 30)));
        QMenu *m = uib::openMenu();
        QVERIFY(m);
        QCOMPARE(m->defaultAction()->text(), QStringLiteral("Open File"));
        QAction *show = uib::menuAction(m, QStringLiteral("Open Containing Folder"));
        QVERIFY(show);
        calls.clear();
        show->trigger();
        uib::closeMenus();
        QCOMPARE(calls, QStringList({QStringLiteral("show Big Buck Bunny 1080p.mkv")}));
        launcher::setHook({});
        launcher::setAskHook({});
    }

    // a user's report: "right click takes the row above". A context menu event arrives through
    // the viewport, in viewport coordinates; the list once subtracted the header height again.
    void rightClickActsOnTheRowUnderTheMouse()
    {
        m_app.mw->showPage(QStringLiteral("transfers"));
        auto *page = qobject_cast<TransfersPage *>(m_app.mw->page(QStringLiteral("transfers")));
        MxListView *list = page->findChild<MxListView *>(QStringLiteral("downloadList"));
        QVERIFY(list && list->visibleRowCount() >= 4);
        QVERIFY(list->header()->isVisible() && list->header()->height() > 0);
        QSignalSpy spy(list, &MxListView::contextMenuAt);
        // [ui-g] a narrowed column no longer leaves a strip right of the last column (the columns to its
        // right take the width): the last pixel of the row is the far right of the last column
        QHeaderView *h = list->header();
        const int firstCol = h->logicalIndex(0);
        const int firstWidth = h->sectionSize(firstCol);
        h->resizeSection(firstCol, firstWidth - 60);
        QCoreApplication::processEvents();
        QVERIFY(list->columnsFillViewport());
        for (int i = 0; i < 4; ++i) {
            const QModelIndex row = list->visibleRow(i);
            const QRect r = list->visualRect(row);
            // near the top and near the bottom edge of the row: both belong to this row
            // and so does the right edge of the list (rows are full width)
            const int beside = list->viewport()->width() - 1;
            QVERIFY(list->rowIndexAt(QPoint(beside, r.center().y())).isValid());
            for (const QPoint &pos : {QPoint(r.left() + 40, r.top() + 1), QPoint(r.left() + 40, r.bottom() - 1),
                                      QPoint(r.left() + 40, r.top()), QPoint(r.left() + 40, r.bottom()),
                                      QPoint(beside, r.top() + 1), QPoint(beside, r.bottom() - 1)}) {
                spy.clear();
                QTest::mouseClick(list->viewport(), Qt::RightButton, Qt::NoModifier, pos);
                QContextMenuEvent ev(QContextMenuEvent::Mouse, pos, list->viewport()->mapToGlobal(pos));
                QApplication::sendEvent(list->viewport(), &ev);
                QCOMPARE(spy.size(), 1);
                const QModelIndex got = spy.at(0).at(0).toModelIndex();
                QVERIFY(got.isValid());
                QCOMPARE(got, row.sibling(row.row(), 0));
                QCOMPARE(list->currentIndex().sibling(list->currentIndex().row(), 0), row.sibling(row.row(), 0));
                uib::closeMenus();
            }
        }
        // below the last row: the menu of the empty area
        spy.clear();
        const QPoint empty(40, list->viewport()->height() - 2);
        if (!list->indexAt(empty).isValid()) {
            QContextMenuEvent ev(QContextMenuEvent::Mouse, empty, list->viewport()->mapToGlobal(empty));
            QApplication::sendEvent(list->viewport(), &ev);
            QCOMPARE(spy.size(), 1);
            QVERIFY(!spy.at(0).at(0).toModelIndex().isValid());
            uib::closeMenus();
        }
        list->resetColumns();
    }

    // owner's report: the sort triangle was upside down. Windows convention: ascending points up
    // (narrow end on top), descending points down.
    void sortTrianglePointsUpForAscending()
    {
        m_app.mw->showPage(QStringLiteral("transfers"));
        auto *page = qobject_cast<TransfersPage *>(m_app.mw->page(QStringLiteral("transfers")));
        MxListView *list = page->findChild<MxListView *>(QStringLiteral("downloadList"));
        QHeaderView *h = list->header();
        const int oldCol = h->sortIndicatorSection();
        const Qt::SortOrder oldOrder = h->sortIndicatorOrder();
        const int col = h->logicalIndex(0);
        auto grab = [&] {
            QCoreApplication::processEvents();
            return h->viewport()->grab().toImage().convertToFormat(QImage::Format_RGB32);
        };
        h->setSortIndicator(-1, Qt::AscendingOrder);
        const QImage plain = grab();
        // rows of the triangle, top to bottom: how many pixels differ from the header without it
        auto shape = [&](Qt::SortOrder o) {
            h->setSortIndicator(col, o);
            const QImage img = grab();
            QList<int> rows;
            for (int y = 0; y < img.height(); ++y) {
                int n = 0;
                for (int x = 0; x < qMin(img.width(), h->sectionSize(col)); ++x)
                    n += img.pixel(x, y) != plain.pixel(x, y);
                if (n > 0)
                    rows << n;
            }
            return rows;
        };
        const QList<int> asc = shape(Qt::AscendingOrder);
        const QList<int> desc = shape(Qt::DescendingOrder);
        QVERIFY2(asc.size() >= 4 && desc.size() >= 4, "no sort triangle drawn");
        QVERIFY2(asc.first() < asc.last(), "ascending: the narrow end must be on top");
        QVERIFY2(desc.first() > desc.last(), "descending: the narrow end must be at the bottom");
        h->setSortIndicator(oldCol, oldOrder);
    }

    void failuresAreNeverSilent()
    {
        // a dropped file that is no download
        m_app.mw->addLinks({m_dir.filePath(QStringLiteral("song.mp3"))});
        QVERIFY(m_app.mw->notice().contains(QStringLiteral("Not a link")));
        // Download without a selection
        m_app.mw->showPage(QStringLiteral("search"));
        auto *page = qobject_cast<SearchPage *>(m_app.mw->page(QStringLiteral("search")));
        page->currentTab()->findChild<MxListView *>(QStringLiteral("searchResults"))->clearSelection();
        page->currentTab()->downloadSelected();
        QVERIFY(m_app.mw->notice().contains(QStringLiteral("Select a file")));
        // open of a vanished file in Transfers: a message names the reason
        QString shown;
        TransfersPage::s_messageHook = [&shown](const QString &why) { shown = why; };
        launcher::setHook({});
        qputenv("MX_LAUNCH_LOG", QFile::encodeName(m_dir.filePath(QStringLiteral("launch.log"))));
        auto *tp = qobject_cast<TransfersPage *>(m_app.mw->page(QStringLiteral("transfers")));
        MxListView *list = tp->findChild<MxListView *>(QStringLiteral("downloadList"));
        QModelIndex done;
        for (int i = 0; i < list->visibleRowCount(); ++i)
            if (list->visibleRow(i).data().toString() == QLatin1String("Big Buck Bunny 1080p.mkv"))
                done = list->visibleRow(i);
        const QString path = m_app.core->transfers()->downloads().value(0).savePath;
        Q_UNUSED(path)
        QVERIFY(done.isValid());
        for (const TransferInfo &t : m_app.core->transfers()->downloads())
            if (t.name == QLatin1String("Big Buck Bunny 1080p.mkv"))
                QFile::remove(t.savePath);
        emit list->rowActivated(done);
        QVERIFY(shown.contains(QStringLiteral("not there")));
        QVERIFY(tp->statusFields().join(QLatin1Char(' ')).contains(QStringLiteral("not there")));   // status bar too
        TransfersPage::s_messageHook = nullptr;
        qunsetenv("MX_LAUNCH_LOG");
    }

    void connectionDefaultsFollowTheType()
    {
        Settings *s = m_app.core->settings();
        s->setValue(keys::identity::Connection, QStringLiteral("FTTH"));
        QCOMPARE(s->i(keys::transfers::MaxUploads), 16);
        QCOMPARE(s->i(keys::queue::MaxLength), 800);
        // a value the user chose is kept when the type changes again
        s->setValue(keys::transfers::MaxDownloads, 77);
        s->setValue(keys::identity::Connection, QStringLiteral("10 Gbit+"));
        QCOMPARE(s->i(keys::transfers::MaxUploads), 48);
        QCOMPARE(s->i(keys::transfers::MaxDownloads), 77);
        QCOMPARE(s->value(keys::ui::ConnectionDefaults).toString(), QStringLiteral("10 Gbit+"));
    }

    void completedDownloadNotifies()
    {
        QStringList told;
        MainWindow::s_notifyHook = [&told](const QString &name) { told << name; };
        TransferInfo t = m_app.core->transfers()->downloads().value(1);
        QVERIFY(!t.id.isEmpty());
        t.id = QStringLiteral("uie-notify");
        t.state = TransferState::Transferring;
        emit m_app.core->transfers()->transferChanged(t);
        QVERIFY(told.isEmpty());
        t.state = TransferState::Completed;
        emit m_app.core->transfers()->transferChanged(t);
        emit m_app.core->transfers()->transferChanged(t);   // once only
        QCOMPARE(told, QStringList({t.name}));
        QVERIFY(m_app.mw->notice().contains(QStringLiteral("Download complete")));
        m_app.core->settings()->setValue(keys::ui::NotifyDownloadDone, false);
        t.id = QStringLiteral("uie-notify-2");
        emit m_app.core->transfers()->transferChanged(t);
        QCOMPARE(told.size(), 1);   // setting off: only the status-bar note
        MainWindow::s_notifyHook = nullptr;
    }

    void splittersKeepProportions()
    {
        m_app.mw->showPage(QStringLiteral("transfers"));
        auto *sp = m_app.mw->findChild<QSplitter *>(QStringLiteral("transfersSplitter"));
        QVERIFY(sp);
        QTest::qWait(20);
        const QList<int> s0 = sp->sizes();
        const int total0 = s0.value(0) + s0.value(1);
        sp->setSizes({total0 / 5, total0 - total0 / 5});
        m_app.mw->resize(1024, 900);
        QTest::qWait(20);
        const QList<int> s1 = sp->sizes();
        const double r = double(s1.value(0)) / (s1.value(0) + s1.value(1));
        QVERIFY2(qAbs(r - 0.2) < 0.02, qPrintable(QString::number(r)));
        m_app.mw->resize(1024, 689);
        QTest::qWait(20);
    }

    void scaleChangeAsksForRestart()
    {
        m_app.mw->openSettings(QStringLiteral("appearance/interface"));
        auto *d = m_app.mw->findChild<SettingsDialog *>();
        if (!d)
            for (QWidget *w : QApplication::topLevelWidgets())
                if (auto *x = qobject_cast<SettingsDialog *>(w))
                    d = x;
        QVERIFY(d);
        QVERIFY(d->findChild<QComboBox *>(QStringLiteral("set_ui_scale")));
        QVERIFY(d->binder()->commit(QString(keys::ui::Scale), QStringLiteral("150")));
        QVERIFY(!d->restartMessage().isEmpty());
        d->binder()->commit(QString(keys::ui::Scale), QStringLiteral("auto"));
        d->binder()->clearRestartPending();
        d->close();
    }

    // ---- the interface scale (last: it changes process-wide state)
    void scaleGeometryAndLines()
    {
        setUiScale(1.0);
        QCOMPARE(S(17), 17);
        QCOMPARE(SR(3, 4, 5, 6), QRect(3, 4, 5, 6));
        QCOMPARE(uiLine(), 1);
        setUiScale(1.5);
        QCOMPARE(S(16), 24);
        QCOMPARE(uiLine(), 1);
        // edges scale: rects that touch at 100 % still touch
        const QRect a = SR(0, 0, 61, 21), b = SR(61, 0, 61, 21);
        QCOMPARE(a.right() + 1, b.left());
        QCOMPARE(SR(457, 41, 123, 21).right() + 1, SR(580, 41, 123, 21).left());
        setUiScale(1.75);
        QCOMPARE(uiLine(), 2);
        setUiScale(2.0);
        QCOMPARE(uiLine(), 2);
        QCOMPARE(S(98), 196);
        setUiScale(3.0);
        QCOMPARE(uiLine(), 3);
        // a raised bevel is the same width on every side (no 1/2-px mix)
        for (double sc : {1.25, 1.5, 1.75, 2.0, 2.5, 3.0}) {
            setUiScale(sc);
            QImage img(SS(40, 20), QImage::Format_RGB32);
            img.fill(chrome::Face);
            QPainter p(&img);
            draw::buttonRaised(&p, img.rect());
            p.end();
            const int t = uiLine();
            auto run = [&](int x0, int y0, int dx, int dy) {
                int n = 0;
                const QRgb first = img.pixel(x0, y0);
                for (int x = x0, y = y0; x >= 0 && y >= 0 && x < img.width() && y < img.height() && img.pixel(x, y) == first; x += dx, y += dy)
                    ++n;
                return n;
            };
            const int mid = img.height() / 2, cx = img.width() / 2;
            QCOMPARE(run(0, mid, 1, 0), t);                     // left: white
            QCOMPARE(run(img.width() - 1, mid, -1, 0), t);      // right: dark shadow
            QCOMPARE(run(cx, 0, 0, 1), t);                      // top
            QCOMPARE(run(cx, img.height() - 1, 0, -1), t);      // bottom
        }
        setUiScale(1.0);
    }

    void scaleChoicesAndFonts()
    {
        QCOMPARE(scaleChoices().first(), QStringLiteral("auto"));
        QCOMPARE(scaleChoices().size(), 8);
        QCOMPARE(scaleForChoice(QStringLiteral("150")), 1.5);
        QCOMPARE(scaleForChoice(QStringLiteral("300")), 3.0);
        const double a = scaleForChoice(QStringLiteral("auto"));
        QVERIFY(a >= 1.0 && a <= 3.0);
        QVERIFY(scaleChoiceLabel(QStringLiteral("auto")).startsWith(QStringLiteral("Automatic (")));
        // the font of a scaled interface: about 1.5 × 11 px, never wider than the scaled layout
        setUiScale(1.5);
        const int px = fontPx(11);
        QVERIFY2(px >= 15 && px <= 17, qPrintable(QString::number(px)));
        QVERIFY(smoothFonts());
        setUiScale(1.0);
        QCOMPARE(fontPx(11), 11);
        QVERIFY(!smoothFonts());
    }

    void iconsAreDrawnAtTheScaledSize()
    {
        for (IconId id : allIcons()) {
            for (int s : {16, 24, 32, 48}) {
                const QImage img = iconImage(id, s);
                QCOMPARE(img.size(), QSize(s, s));
                // drawn, not empty, and not blurred: only the palette colours (fully opaque or clear)
                int ink = 0;
                for (int y = 0; y < s; ++y)
                    for (int x = 0; x < s; ++x) {
                        const QRgb p = img.pixel(x, y);
                        QVERIFY2(qAlpha(p) == 0 || qAlpha(p) == 255, qPrintable(iconName(id)));
                        ink += qAlpha(p) ? 1 : 0;
                    }
                QVERIFY2(ink > s * s / 12, qPrintable(iconName(id) + QStringLiteral(" %1").arg(s)));
            }
        }
    }
};

int main(int argc, char **argv)
{
    preApplicationSetup();
    QApplication app(argc, argv);
    installClassicLook(&app);
    TstUie t;
    return QTest::qExec(&t, argc, argv);
}

#include "tst_uie.moc"
