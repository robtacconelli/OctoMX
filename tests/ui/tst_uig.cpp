// SPDX-License-Identifier: GPL-3.0-or-later
// [ui-g] 4.3: columns that always fill the list (a property test of random user actions on several
// lists at 100 / 150 / 200 %, drag rules, stored layouts with a gap), the always visible results
// filter of the Search tabs (rules and UI), the search label that follows the file type, the wide
// search form, and tool tips on every button of every page, pane and dialog.
#include <QtCore/QRandomGenerator>
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QAbstractSlider>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QScrollBar>

#include "uib_helpers.h"
#include "mx/settings_keys.h"
#include "ui/kit/list_view.h"
#include "ui/kit/scale.h"
#include "ui/kit/text_filter.h"
#include "ui/kit/theme.h"
#include "ui/kit/widgets.h"
#include "ui/models/search_model.h"
#include "ui/pages/chat_page.h"
#include "ui/pages/search_page.h"
#include "ui/pages/transfers_page.h"
#include "ui/dialogs/about_dialog.h"
#include "ui/dialogs/auto_responders.h"
#include "ui/dialogs/chat_warning_dialog.h"
#include "ui/dialogs/close_prompt.h"
#include "ui/dialogs/connection_test.h"
#include "ui/dialogs/file_download_dialog.h"
#include "ui/dialogs/settings_dialog.h"
#include "ui/dialogs/setup_wizard.h"
#include "ui/dialogs/share_dialogs.h"
#include "ui/dialogs/simple_dialogs.h"
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QTabBar>
#include <QtWidgets/QAbstractSpinBox>

using namespace mx;
using namespace mx::ui;

namespace {

void settle()
{
    // the column layout of a change runs at once or from a 0 ms timer; the filter after 150 ms
    for (int i = 0; i < 3; ++i)
        QCoreApplication::processEvents();
}

// the filter applies 150 ms after the last change
void waitFilter(MxListView *l)
{
    QTest::qWait(20);
    QTRY_VERIFY_WITH_TIMEOUT(!l->filterPending(), 5000);
    QCoreApplication::processEvents();
}

QString widthsOf(MxListView *l)
{
    QStringList w;
    QHeaderView *h = l->header();
    for (int v = 0; v < h->count(); ++v) {
        const int li = h->logicalIndex(v);
        w << (h->isSectionHidden(li) ? QStringLiteral("-") : QString::number(h->sectionSize(li)));
    }
    return QStringLiteral("%1 [%2] viewport %3").arg(l->objectName(), w.join(QLatin1Char(' '))).arg(l->viewport()->width());
}

// drag the right border of a column with real mouse events on the header
void dragBorder(MxListView *l, int logical, int dx)
{
    QHeaderView *h = l->header();
    QWidget *vp = h->viewport();
    const int x = h->sectionViewportPosition(logical) + h->sectionSize(logical) - 1;
    const int y = h->height() / 2;
    QTest::mousePress(vp, Qt::LeftButton, Qt::NoModifier, QPoint(x, y));
    for (int k = 1; k <= 4; ++k) {
        const QPoint p(x + dx * k / 4, y);
        QMouseEvent mv(QEvent::MouseMove, p, vp->mapToGlobal(p), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(vp, &mv);
    }
    QTest::mouseRelease(vp, Qt::LeftButton, Qt::NoModifier, QPoint(x + dx, y));
    settle();
}

QList<int> visibleOf(MxListView *l)
{
    QList<int> out;
    QHeaderView *h = l->header();
    for (int v = 0; v < h->count(); ++v)
        if (!h->isSectionHidden(h->logicalIndex(v)))
            out << h->logicalIndex(v);
    return out;
}

} // namespace

class TstUig : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    uib::App m_app;
    QString profile(const QString &n = QStringLiteral("p")) const { return m_dir.filePath(n); }

    SearchTab *searchTab()
    {
        auto *page = qobject_cast<SearchPage *>(m_app.mw->page(QStringLiteral("search")));
        return page ? page->currentTab() : nullptr;
    }
    SearchTab *searchLinux()
    {
        m_app.mw->showPage(QStringLiteral("search"));
        SearchTab *t = searchTab();
        t->setTexts(QStringLiteral("linux"), QString());
        t->startSearch();
        QTest::qWait(50);
        t->flushPending();
        settle();
        return t;
    }
    MxListView *list(const QString &name)
    {
        return m_app.mw->findChild<MxListView *>(name);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_app.start(profile());
    }
    void cleanupTestCase() { m_app.stop(); }

    // ------------------------------------------------------------------------------ filter rules
    void textFilterRules()
    {
        TextFilter f(QStringLiteral("debian ISO"));
        QVERIFY(f.matches(QStringLiteral("debian-13.2.0-amd64-netinst.iso\t754 MB")));
        QVERIFY(!f.matches(QStringLiteral("debian-13.2.0.tar")));               // every word must appear
        f.setQuery(QStringLiteral("iso -debian"));
        QVERIFY(f.matches(QStringLiteral("ubuntu.iso")));
        QVERIFY(!f.matches(QStringLiteral("debian.iso")));                      // -word excludes
        f.setQuery(QStringLiteral("\"big buck\" -\"1080p\""));
        QCOMPARE(f.required(), QStringList({QStringLiteral("big buck")}));
        QCOMPARE(f.excluded(), QStringList({QStringLiteral("1080p")}));
        QVERIFY(f.matches(QStringLiteral("Big Buck Bunny 720p.mkv")));
        QVERIFY(!f.matches(QStringLiteral("Big Buck Bunny 1080p.mkv")));
        QVERIFY(!f.matches(QStringLiteral("Buck Big.mkv")));                    // a phrase, not two words
        f.setQuery(QStringLiteral("bt"));
        QVERIFY(f.matches(QStringLiteral("x\tBT+eD2k")));                       // any case, inside a cell
        f.setQuery(QStringLiteral("  -  "));
        QVERIFY(f.isEmpty());                                                   // a lone dash is nothing
        f.setQuery(QStringLiteral("\"open quote"));
        QCOMPARE(f.required(), QStringList({QStringLiteral("open quote")}));
    }

    // ---------------------------------------------------------------------------- results filter
    void filterIsAlwaysVisibleAndFilters()
    {
        m_app.mw->resize(1024, 689);
        SearchTab *t = searchLinux();
        MxListView *l = t->list();
        QVERIFY(t->filterRow() && t->filterRow()->isVisible());
        QLineEdit *edit = t->filterRow()->edit();
        QCOMPARE(edit->objectName(), QStringLiteral("searchFilter"));
        QVERIFY(!edit->toolTip().isEmpty());
        const int total = l->model()->rowCount();
        QVERIFY(total >= 5);
        // typing filters live; the count says what is shown
        QTest::keyClicks(edit, QStringLiteral("debian"));
        waitFilter(l);
        QCOMPARE(l->filterMatches(), 1);
        QCOMPARE(t->filterRow()->countText(), QStringLiteral("1 of %1 shown").arg(total));
        QVERIFY(t->statusFields().value(0).startsWith(QStringLiteral("1 of %1 results shown").arg(total)));
        // the parent stays for a matching source row, other sources are hidden
        QModelIndex parent;
        for (int r = 0; r < l->model()->rowCount(); ++r)
            if (!l->isRowHidden(r, QModelIndex()))
                parent = l->model()->index(r, 0);
        QVERIFY(parent.isValid() && l->model()->rowCount(parent) > 1);
        edit->setText(QStringLiteral("mule_1000"));
        waitFilter(l);
        QCOMPARE(l->filterMatches(), 1);
        int kidsShown = 0;
        for (int r = 0; r < l->model()->rowCount(parent); ++r)
            kidsShown += l->isRowHidden(r, parent) ? 0 : 1;
        QCOMPARE(kidsShown, 1);
        // Network text and exclusion
        edit->setText(QStringLiteral("eD2k"));
        waitFilter(l);
        const int withEd2k = l->filterMatches();
        QVERIFY(withEd2k > 0 && withEd2k < total);
        edit->setText(QStringLiteral("iso -eD2k"));
        waitFilter(l);
        for (int r = 0; r < l->model()->rowCount(); ++r) {
            const QModelIndex i = l->model()->index(r, 0);
            if (!l->isRowHidden(r, QModelIndex()) && !l->model()->hasChildren(i))
                QVERIFY(!i.siblingAtColumn(search::Network).data().toString().contains(QStringLiteral("eD2k")));
        }
        // survives sorting
        edit->setText(QStringLiteral("iso"));
        waitFilter(l);
        const int isoRows = l->filterMatches();
        l->sortByColumn(search::Size, Qt::DescendingOrder);
        waitFilter(l);
        QCOMPARE(l->filterMatches(), isoRows);
        for (int r = 0; r < l->model()->rowCount(); ++r)
            if (!l->isRowHidden(r, QModelIndex()))
                QVERIFY(l->model()->index(r, 0).data().toString().contains(QStringLiteral("iso"), Qt::CaseInsensitive) ||
                        l->model()->hasChildren(l->model()->index(r, 0)));
        // what the filter hides is never selected: Download acts on what is shown
        l->selectAll();
        edit->setText(QStringLiteral("debian"));
        waitFilter(l);
        for (const QModelIndex &i : l->selectionModel()->selectedRows(0))
            QVERIFY(!l->isFilteredOut(i));
        l->clearSelection();
        edit->clear();
        waitFilter(l);
        l->selectRow(l->visibleRow(0));
        const QString first = l->visibleRow(0).data().toString();
        edit->setText(QStringLiteral("no such words here"));
        waitFilter(l);
        QCOMPARE(l->filterMatches(), 0);
        t->downloadSelected();
        QVERIFY2(m_app.mw->notice().contains(QStringLiteral("Select a file")), qPrintable(first));
        // Esc clears; Ctrl+F comes back to the field; CLEAR empties it too
        edit->setFocus();
        QTest::keyClick(edit, Qt::Key_Escape);
        waitFilter(l);
        QVERIFY(edit->text().isEmpty());
        QCOMPARE(l->filterMatches(), total);
        l->setFocus();
        QTest::keyClick(m_app.mw, Qt::Key_F, Qt::ControlModifier);
        QCOMPARE(QApplication::focusWidget(), edit);
        edit->setText(QStringLiteral("debian"));
        waitFilter(l);
        t->findChild<QAbstractButton *>(QStringLiteral("searchClear"))->click();
        waitFilter(l);
        QVERIFY(edit->text().isEmpty());
        QCOMPARE(l->filterMatches(), total);
    }

    void filterAppliesWhileResultsArriveAndPerTab()
    {
        SearchTab *t = searchLinux();
        MxListView *l = t->list();
        t->filterRow()->edit()->setText(QStringLiteral("debian"));
        waitFilter(l);
        // a new search with the filter typed: the arriving rows are filtered as they come
        t->setTexts(QStringLiteral("linux"), QString());
        t->startSearch();
        QTest::qWait(50);
        t->flushPending();
        waitFilter(l);
        QVERIFY(l->model()->rowCount() > 1);
        QCOMPARE(l->filterMatches(), 1);
        // another tab has its own (empty) filter
        auto *page = qobject_cast<SearchPage *>(m_app.mw->page(QStringLiteral("search")));
        SearchTab *t2 = page->newTab(true);
        QVERIFY(t2 != t);
        QVERIFY(t2->filterRow()->edit()->text().isEmpty());
        QCOMPARE(t->filterRow()->edit()->text(), QStringLiteral("debian"));
        page->closeTab(t2);
        t->filterRow()->edit()->clear();
        waitFilter(l);
    }

    // --------------------------------------------------------------------- label and wide form
    void searchLabelFollowsTheType()
    {
        m_app.mw->showPage(QStringLiteral("search"));
        SearchTab *t = searchTab();
        for (const char *c : {"audio", "audio:min:192", "mp3:eq:128", "lossless"}) {
            t->setFileTypeCode(QString::fromLatin1(c));
            QCOMPARE(t->searchLabel(), QStringLiteral("Artist and / or Title"));
        }
        t->setFileTypeCode(QStringLiteral("video"));
        QCOMPARE(t->searchLabel(), QStringLiteral("Title of the film or show"));
        t->setFileTypeCode(QStringLiteral("documents"));
        QCOMPARE(t->searchLabel(), QStringLiteral("Title or author"));
        t->setFileTypeCode(QStringLiteral("discimages"));
        QCOMPARE(t->searchLabel(), QStringLiteral("Name or words"));
        t->setFileTypeCode(QStringLiteral("any"));
        QCOMPARE(t->searchLabel(), QStringLiteral("File name or words"));
        // the second line says what it does outside music
        QCOMPARE(t->findChild<QLineEdit *>(QStringLiteral("searchText2"))->placeholderText(), QStringLiteral("more words (optional)"));
        t->setFileTypeCode(QStringLiteral("audio"));
        QVERIFY(t->findChild<QLineEdit *>(QStringLiteral("searchText2"))->placeholderText().isEmpty());
        t->setFileTypeCode(QStringLiteral("any"));
    }

    void wideFormAndNothingCut()
    {
        m_app.mw->showPage(QStringLiteral("search"));
        SearchTab *t = searchTab();
        // 800 x 600: the classic rows; 1024 and wider: the wide form with the four extra quick types
        m_app.mw->resize(800, 600);
        QTest::qWait(30);
        QVERIFY(!t->isWideForm());
        QVERIFY(!t->findChild<QWidget *>(QStringLiteral("searchDiscImages"))->isVisible());
        m_app.mw->resize(1280, 800);
        QTest::qWait(30);
        QVERIFY(t->isWideForm());
        auto *disc = t->findChild<QAbstractButton *>(QStringLiteral("searchDiscImages"));
        QVERIFY(disc->isVisible());
        disc->click();
        QCOMPARE(t->fileTypeCode(), QStringLiteral("discimages"));
        QVERIFY(disc->isChecked());
        QVERIFY(!t->findChild<QAbstractButton *>(QStringLiteral("searchAll"))->isChecked());
        t->setFileTypeCode(QStringLiteral("any"));
        // at both widths every control of the form lies inside the tab, none overlaps another, and the
        // filter row ends above the list
        for (const QSize &sz : {QSize(800, 600), QSize(1024, 768), QSize(1920, 1040)}) {
            m_app.mw->resize(sz);
            QTest::qWait(30);
            QList<QWidget *> ctl;
            for (QWidget *w : t->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly))
                if (w->isVisible() && w != t->list())
                    ctl << w;
            for (QWidget *w : std::as_const(ctl)) {
                QVERIFY2(t->rect().contains(w->geometry()), qPrintable(w->objectName()));
                QVERIFY2(w->geometry().bottom() < t->list()->geometry().top(), qPrintable(w->objectName()));
            }
            for (int a = 0; a < ctl.size(); ++a)
                for (int b = a + 1; b < ctl.size(); ++b) {
                    const QRect i = ctl[a]->geometry().intersected(ctl[b]->geometry());
                    // touching edges of the classic button grid share one pixel at most
                    QVERIFY2(i.isEmpty() || i.width() <= 1 || i.height() <= 1,
                             qPrintable(ctl[a]->objectName() + QLatin1Char('/') + ctl[b]->objectName()));
                }
        }
        m_app.mw->resize(1024, 689);
        QTest::qWait(30);
    }

    // ------------------------------------------------------------------------------ row clicks
    // found with real mouse clicks under wine at 150 / 200 %: a right click on the partly visible last row
    // selected it AND scrolled it into view, so the menu that followed belonged to the row below
    void rightClickOnAPartlyVisibleRowKeepsItsRow()
    {
        SearchTab *t = searchLinux();
        MxListView *l = t->list();
        l->expandAll();
        QModelIndex partial;
        for (int h = 600; h <= 640 && !partial.isValid(); ++h) {
            m_app.mw->resize(1024, h);
            QTest::qWait(20);
            for (int i = 0; i < l->visibleRowCount(); ++i) {
                const QRect r = l->visualRect(l->visibleRow(i));
                if (r.top() < l->viewport()->height() - 3 && r.bottom() >= l->viewport()->height()) {
                    partial = l->visibleRow(i);
                    break;
                }
            }
        }
        QVERIFY2(partial.isValid(), "no partly visible row found");
        QSignalSpy spy(l, &MxListView::contextMenuAt);
        const int scrollBefore = l->verticalScrollBar()->value();
        const QPoint pos(40, l->visualRect(partial).top() + 2);
        QTest::mouseClick(l->viewport(), Qt::RightButton, Qt::NoModifier, pos);
        QCOMPARE(l->verticalScrollBar()->value(), scrollBefore);   // the rows did not move under the mouse
        QContextMenuEvent ev(QContextMenuEvent::Mouse, pos, l->viewport()->mapToGlobal(pos));
        QApplication::sendEvent(l->viewport(), &ev);
        QCOMPARE(spy.size(), 1);
        QCOMPARE(spy.at(0).at(0).toModelIndex(), partial.sibling(partial.row(), 0));
        QCOMPARE(l->currentIndex().sibling(l->currentIndex().row(), 0), partial.sibling(partial.row(), 0));
        uib::closeMenus();
        // rows that move between the press and the menu (new results, a scroll) do not change its row
        const QModelIndex first = l->visibleRow(1);
        const QPoint p1(40, l->visualRect(first).center().y());
        QTest::mouseClick(l->viewport(), Qt::RightButton, Qt::NoModifier, p1);
        l->verticalScrollBar()->setValue(l->verticalScrollBar()->value() + 1);
        spy.clear();
        QContextMenuEvent ev2(QContextMenuEvent::Mouse, p1, l->viewport()->mapToGlobal(p1));
        QApplication::sendEvent(l->viewport(), &ev2);
        QCOMPARE(spy.size(), 1);
        QCOMPARE(spy.at(0).at(0).toModelIndex(), first.sibling(first.row(), 0));
        uib::closeMenus();
        m_app.mw->resize(1024, 689);
        QTest::qWait(20);
    }

    // ------------------------------------------------------------------------------- hotlist
    void hotlistTellsWhenAUserComesOnline()
    {
        const QList<HotlistEntry> hl = m_app.core->users()->hotlist();
        QString name;
        for (const HotlistEntry &e : hl)
            if (e.user.net == Net::Overlay && !e.user.name.isEmpty())
                name = e.user.name;
        QVERIFY(!name.isEmpty());
        QStringList told;
        MainWindow::s_notifyHook = [&told](const QString &t) { told << t; };
        m_app.core->settings()->setValue(keys::hotlist::NotifyOnline, true);
        QVERIFY(mockSetHotlistOnline(m_app.core, name, false));
        QVERIFY(told.isEmpty());
        QVERIFY(mockSetHotlistOnline(m_app.core, name, true));
        QCOMPARE(told, QStringList({QStringLiteral("%1 is online").arg(name)}));
        QVERIFY(m_app.mw->notice().contains(name));
        // switched off in Settings: nothing
        m_app.core->settings()->setValue(keys::hotlist::NotifyOnline, false);
        mockSetHotlistOnline(m_app.core, name, false);
        mockSetHotlistOnline(m_app.core, name, true);
        QCOMPARE(told.size(), 1);
        // a user just added whose state is learned at once did not "come online"
        m_app.core->settings()->setValue(keys::hotlist::NotifyOnline, true);
        UserRef u;
        u.net = Net::Overlay;
        u.id = u.name = QStringLiteral("JustAdded_12345");
        m_app.core->users()->addToHotlist(u);
        mockSetHotlistOnline(m_app.core, u.name, false);
        mockSetHotlistOnline(m_app.core, u.name, true);
        QCOMPARE(told.size(), 1);
        m_app.core->users()->removeFromHotlist(u);
        MainWindow::s_notifyHook = {};
    }

    void emptyHotlistExplainsItself()
    {
        m_app.mw->setHotlistVisible(true);
        QTest::qWait(20);
        QWidget *hint = m_app.mw->findChild<QWidget *>(QStringLiteral("hotlistHint"));
        QVERIFY(hint);
        const QList<HotlistEntry> keep = m_app.core->users()->hotlist();
        for (const HotlistEntry &e : keep)
            m_app.core->users()->removeFromHotlist(e.user);
        QTest::qWait(20);
        QVERIFY(hint->isVisible());
        for (const HotlistEntry &e : keep)
            m_app.core->users()->addToHotlist(e.user);
        QTest::qWait(20);
        QVERIFY(!hint->isVisible());
        m_app.mw->setHotlistVisible(false);
    }

    // ---------------------------------------------------------------------- tool tips everywhere
    // every visible button of every page, pane and window says what it does in a tool tip, except
    // the ones whose text is the whole story
    static bool selfExplaining(const QString &text)
    {
        static const QStringList ok = {QStringLiteral("OK"), QStringLiteral("Cancel"), QStringLiteral("Close"), QStringLiteral("Yes"),
                                       QStringLiteral("No"), QStringLiteral("Apply"), QStringLiteral("< Back"), QStringLiteral("Next >"),
                                       QStringLiteral("Back"), QStringLiteral("Next"), QStringLiteral("Finish"), QStringLiteral("Done")};
        return ok.contains(text);
    }
    static void audit(QWidget *root, const QString &where, QStringList *missing, int *checked)
    {
        for (QAbstractButton *b : root->findChildren<QAbstractButton *>()) {
            if (!b->isVisibleTo(root) && b != root)
                continue;
            // parts of Qt's own widgets (line edit actions, tab scrollers, spin boxes, combo arrows)
            QWidget *p = b->parentWidget();
            if (qobject_cast<QLineEdit *>(p) || qobject_cast<QTabBar *>(p) || qobject_cast<QAbstractSpinBox *>(p) ||
                qobject_cast<QComboBox *>(p))
                continue;
            if (qobject_cast<QCheckBox *>(b) || qobject_cast<QRadioButton *>(b))
                continue;   // check boxes / radios: only where the label does not say it all (not decidable here)
            const QString text = b->text().remove(QLatin1Char('&')).trimmed();
            // tabs named after their content (a search, a chat room) and colour swatches
            if (b->objectName().startsWith(QLatin1String("searchTab")) || b->property("roomTab").toBool() ||
                b->objectName() == QLatin1String("tabBlank"))
                continue;
            ++*checked;
            if (selfExplaining(text) || !b->toolTip().trimmed().isEmpty())
                continue;
            *missing << QStringLiteral("%1: %2 \"%3\" (%4)").arg(where, b->objectName(), text, QString::fromLatin1(b->metaObject()->className()));
        }
    }

    // [ui-h] every interactive control (check box, radio, combo, spin box, line edit, slider) says what
    // it does in a tool tip, except where the label is the whole story (allow-list below)
    static bool labelSaysItAll(const QWidget *w, const QString &text)
    {
        // the day boxes of the bandwidth schedule (their row says "Schedule days"); Advanced page "Find:"
        static const QStringList ok = {QStringLiteral("Mon"), QStringLiteral("Tue"), QStringLiteral("Wed"), QStringLiteral("Thu"),
                                       QStringLiteral("Fri"), QStringLiteral("Sat"), QStringLiteral("Sun")};
        Q_UNUSED(w)
        return ok.contains(text);
    }
    static void auditControls(QWidget *root, const QString &where, QStringList *missing, int *checked)
    {
        for (QWidget *w : root->findChildren<QWidget *>()) {
            if (!w->isVisibleTo(root) && w != root)
                continue;
            const bool control = qobject_cast<QCheckBox *>(w) || qobject_cast<QRadioButton *>(w) || qobject_cast<QComboBox *>(w) ||
                                 qobject_cast<QAbstractSpinBox *>(w) || qobject_cast<QLineEdit *>(w) || qobject_cast<QAbstractSlider *>(w);
            if (!control || qobject_cast<QScrollBar *>(w))
                continue;
            QWidget *p = w->parentWidget();
            // the editor inside a combo / spin box (the box itself is checked)
            if (qobject_cast<QLineEdit *>(w) && (qobject_cast<QComboBox *>(p) || qobject_cast<QAbstractSpinBox *>(p)))
                continue;
            QString text;
            if (auto *b = qobject_cast<QAbstractButton *>(w))
                text = b->text().remove(QLatin1Char('&')).trimmed();
            ++*checked;
            if (labelSaysItAll(w, text) || !w->toolTip().trimmed().isEmpty())
                continue;
            *missing << QStringLiteral("%1: %2 \"%3\" (%4)").arg(where, w->objectName(), text.left(50), QString::fromLatin1(w->metaObject()->className()));
        }
    }

    void everyControlExplainsItself()
    {
        QStringList missing;
        int checked = 0;
        m_app.mw->resize(1280, 800);
        m_app.mw->setHotlistVisible(true);
        for (const char *id : {"networks", "shared", "search", "transfers", "bandwidth", "chat"}) {
            m_app.mw->showPage(QString::fromLatin1(id));
            QTest::qWait(20);
            auditControls(m_app.mw, QString::fromLatin1(id), &missing, &checked);
        }
        if (auto *chat = qobject_cast<ChatPage *>(m_app.mw->page(QStringLiteral("chat")))) {
            m_app.mw->showPage(QStringLiteral("chat"));
            chat->selectSubTab(QStringLiteral("list"));
            QTest::qWait(20);
            auditControls(m_app.mw, QStringLiteral("chat/list"), &missing, &checked);
            chat->join(QStringLiteral("Linux Users"));
            QTest::qWait(30);
            auditControls(m_app.mw, QStringLiteral("chat/room"), &missing, &checked);
        }
        UserRef u;
        u.net = Net::Overlay;
        u.id = QStringLiteral("demo_user_12345");
        u.name = u.id;
        m_app.mw->openBrowse(u);
        m_app.mw->openWhois(u);
        m_app.mw->openPrivateMessage(u);
        m_app.mw->openIgnored();
        QTest::qWait(50);
        for (QWidget *w : QApplication::topLevelWidgets())
            if (w != m_app.mw && w->isVisible() && !qobject_cast<QMenu *>(w))
                auditControls(w, w->objectName(), &missing, &checked);
        {
            SettingsDialog sd(m_app.core, m_app.mw);
            for (const QString &id : sd.pageIds()) {
                sd.showPage(id);
                auditControls(sd.pageWidget(id), QStringLiteral("settings/") + id, &missing, &checked);
            }
        }
        {
            SetupWizard wiz(m_app.core, m_app.mw);
            for (int id : wiz.pageIds())
                if (QWizardPage *pg = wiz.page(id))
                    auditControls(pg, QStringLiteral("wizard/%1").arg(id), &missing, &checked);
        }
        QList<QWidget *> dialogs;
        dialogs << new AboutDialog(m_app.core, m_app.mw) << new ChatWarningDialog(m_app.mw) << new ClosePrompt(true, m_app.mw)
                << new AddLinkDialog(m_app.mw) << new CustomExtensionsDialog(QStringLiteral("mkv srt"), m_app.mw)
                << new ShareFolderDialog(m_dir.path(), m_app.mw)
                << new SharedFileTypesDialog(m_app.core->library(), m_app.core->settings(), m_app.mw)
                << new AutoRespondersWindow(m_app.core->settings(), m_app.mw) << new NewAutoResponderDialog(m_app.mw)
                << new LoadIncompleteDialog(m_app.core->transfers()->partialFiles(), m_app.core->transfers()->incompleteDir(), m_app.mw)
                << new ConnectionTestDialog(m_app.core);
        for (QWidget *d : std::as_const(dialogs)) {
            auditControls(d, QString::fromLatin1(d->metaObject()->className()), &missing, &checked);
            delete d;
        }
        for (const QString &m : std::as_const(missing))
            qInfo("no tool tip: %s", qPrintable(m));
        QVERIFY(checked > 300);
        QVERIFY2(missing.isEmpty(), qPrintable(QStringLiteral("%1 controls without a tool tip:\n").arg(missing.size()) +
                                               missing.join(QLatin1Char('\n'))));
        for (QWidget *w : QApplication::topLevelWidgets())
            if (w != m_app.mw && w->isVisible() && !qobject_cast<QMenu *>(w))
                w->close();
        m_app.mw->setHotlistVisible(false);
    }

    void everyButtonExplainsItself()
    {
        QStringList missing;
        int checked = 0;
        m_app.mw->resize(1280, 800);
        m_app.mw->setHotlistVisible(true);
        // the pages (and their sub tabs), the chrome, the Hotlist pane
        for (const char *id : {"networks", "shared", "search", "transfers", "bandwidth", "chat"}) {
            m_app.mw->showPage(QString::fromLatin1(id));
            QTest::qWait(20);
            audit(m_app.mw, QString::fromLatin1(id), &missing, &checked);
        }
        m_app.mw->showPage(QStringLiteral("networks"));
        m_app.mw->page(QStringLiteral("networks"))->selectSubTab(QStringLiteral("other"));
        QTest::qWait(20);
        audit(m_app.mw, QStringLiteral("networks/other"), &missing, &checked);
        if (auto *chat = qobject_cast<ChatPage *>(m_app.mw->page(QStringLiteral("chat")))) {
            m_app.mw->showPage(QStringLiteral("chat"));
            chat->selectSubTab(QStringLiteral("list"));
            QTest::qWait(20);
            audit(m_app.mw, QStringLiteral("chat/list"), &missing, &checked);
            chat->join(QStringLiteral("Linux Users"));
            QTest::qWait(30);
            audit(m_app.mw, QStringLiteral("chat/room"), &missing, &checked);
        }
        // the user windows
        UserRef u;
        u.net = Net::Overlay;
        u.id = QStringLiteral("demo_user_12345");
        u.name = u.id;
        m_app.mw->openBrowse(u);
        m_app.mw->openWhois(u);
        m_app.mw->openPrivateMessage(u);
        m_app.mw->openIgnored();
        QTest::qWait(50);
        for (QWidget *w : QApplication::topLevelWidgets())
            if (w != m_app.mw && w->isVisible() && !qobject_cast<QMenu *>(w))
                audit(w, w->objectName(), &missing, &checked);
        // the dialogs
        {
            SettingsDialog sd(m_app.core, m_app.mw);
            for (const QString &id : sd.pageIds()) {
                sd.showPage(id);
                audit(&sd, QStringLiteral("settings/") + id, &missing, &checked);
            }
        }
        {
            SetupWizard wiz(m_app.core, m_app.mw);
            for (int id : wiz.pageIds())
                if (QWizardPage *pg = wiz.page(id))
                    audit(pg, QStringLiteral("wizard/%1").arg(id), &missing, &checked);
        }
        QList<QWidget *> dialogs;
        dialogs << new AboutDialog(m_app.core, m_app.mw) << new ChatWarningDialog(m_app.mw) << new ClosePrompt(true, m_app.mw)
                << new AddLinkDialog(m_app.mw) << new CustomExtensionsDialog(QStringLiteral("mkv srt"), m_app.mw)
                << new ShareFolderDialog(m_dir.path(), m_app.mw)
                << new SharedFileTypesDialog(m_app.core->library(), m_app.core->settings(), m_app.mw)
                << new AutoRespondersWindow(m_app.core->settings(), m_app.mw) << new NewAutoResponderDialog(m_app.mw)
                << new LoadIncompleteDialog(m_app.core->transfers()->partialFiles(), m_app.core->transfers()->incompleteDir(), m_app.mw)
                << new ConnectionTestDialog(m_app.core);
        for (QWidget *d : std::as_const(dialogs)) {
            audit(d, QString::fromLatin1(d->metaObject()->className()), &missing, &checked);
            delete d;
        }
        QVERIFY(checked > 150);
        QVERIFY2(missing.isEmpty(), qPrintable(QStringLiteral("%1 buttons without a tool tip:\n").arg(missing.size()) +
                                               missing.join(QLatin1Char('\n'))));
        for (QWidget *w : QApplication::topLevelWidgets())
            if (w != m_app.mw && w->isVisible() && !qobject_cast<QMenu *>(w))
                w->close();
        m_app.mw->setHotlistVisible(false);
    }

    void menusExplainTheirEntries()
    {
        m_app.mw->showPage(QStringLiteral("transfers"));
        QTest::qWait(30);
        MxListView *l = list(QStringLiteral("downloadList"));
        QModelIndex row;
        for (int i = 0; i < l->visibleRowCount() && !row.isValid(); ++i)
            if (!l->visibleRow(i).parent().isValid())
                row = l->visibleRow(i);
        l->selectRow(row);
        emit l->contextMenuAt(row, l->viewport()->mapToGlobal(QPoint(30, 30)));
        QMenu *m = uib::openMenu();
        QVERIFY(m && m->toolTipsVisible());
        QAction *trim = uib::menuAction(m, QStringLiteral("Trim 10K from end of file"));
        QVERIFY(trim && trim->toolTip().contains(QStringLiteral("hashes")));
        QAction *eq = uib::menuAction(m, QStringLiteral("Find Sources Now"));
        QVERIFY(eq && !eq->statusTip().isEmpty());
        // hovering an entry puts its explanation in the status bar
        eq->hover();
        QTest::qWait(20);
        QCOMPARE(m_app.mw->notice(), eq->statusTip());
        uib::closeMenus();
    }

    // ------------------------------------------------------------------- columns fill the list
    void dragRules()
    {
        m_app.mw->resize(1280, 800);
        m_app.mw->showPage(QStringLiteral("transfers"));
        settle();
        MxListView *l = list(QStringLiteral("downloadList"));
        QHeaderView *h = l->header();
        l->resetColumns();
        settle();
        QVERIFY2(l->columnsFillViewport(), qPrintable(widthsOf(l)));
        const QList<int> cols = visibleOf(l);
        // a middle border: the columns left of it stay, the dragged one changes, the right ones absorb
        const int mid = cols.at(2);
        const int left0 = h->sectionSize(cols.at(0)), left1 = h->sectionSize(cols.at(1)), before = h->sectionSize(mid);
        dragBorder(l, mid, 40);
        QCOMPARE(h->sectionSize(cols.at(0)), left0);
        QCOMPARE(h->sectionSize(cols.at(1)), left1);
        QVERIFY2(qAbs(h->sectionSize(mid) - (before + 40)) <= 1, qPrintable(widthsOf(l)));
        QVERIFY2(l->columnsFillViewport(), qPrintable(widthsOf(l)));
        QVERIFY(l->hasUserLayout());
        // the last border: the last two columns change
        const int last = cols.last(), prev = cols.at(cols.size() - 2);
        const int lastW = h->sectionSize(last), prevW = h->sectionSize(prev);
        dragBorder(l, last, -30);
        QVERIFY2(h->sectionSize(last) < lastW && h->sectionSize(prev) > prevW, qPrintable(widthsOf(l)));
        QCOMPARE(h->sectionSize(last) + h->sectionSize(prev), lastW + prevW);
        QVERIFY2(l->columnsFillViewport(), qPrintable(widthsOf(l)));
        // a drag the others cannot pay for stops: they keep their minimum
        dragBorder(l, cols.first(), 5000);
        QVERIFY2(l->columnsFillViewport(), qPrintable(widthsOf(l)));
        for (int c : cols.mid(1))
            QVERIFY2(h->sectionSize(c) >= l->minimumColumnWidth(c) - 1, qPrintable(widthsOf(l)));
        // narrowing gives the width to the right
        dragBorder(l, cols.first(), -5000);
        QVERIFY2(l->columnsFillViewport(), qPrintable(widthsOf(l)));
        QVERIFY(h->sectionSize(cols.first()) >= l->minimumColumnWidth(cols.first()) - 1);
        l->resetColumns();
        settle();
    }

    void storedLayoutsAreNormalised()
    {
        // a 4.1 layout with a gap (fractions add up to 0.7) and a 4.0 one (pixels only, too wide)
        m_app.stop();
        const QString prof = profile(QStringLiteral("stored"));
        {
            m_app.start(prof);
            m_app.stop();
        }
        {
            mx::MockOptions o;
            o.frozen = true;
            mx::Core *core = mx::createMockCoreWithOptions(prof, o);
            QVariantMap gap;
            gap.insert(QStringLiteral("order"), QVariantList({0, 1, 2, 3, 4, 5, 6}));
            gap.insert(QStringLiteral("hidden"), QVariantList());
            gap.insert(QStringLiteral("widths"), QVariantList({150, 213, 96, 110, 218, 62, 60}));
            gap.insert(QStringLiteral("fractions"), QVariantList({0.12, 0.17, 0.075, 0.086, 0.17, 0.049, 0.047}));
            core->settings()->setValue(keys::ui::columnsKey(QStringLiteral("downloads")), gap);
            QVariantMap old;
            old.insert(QStringLiteral("order"), QVariantList({0, 1, 2, 3, 4, 5, 6}));
            old.insert(QStringLiteral("hidden"), QVariantList());
            old.insert(QStringLiteral("widths"), QVariantList({900, 400, 300, 300, 300, 200, 200}));
            core->settings()->setValue(keys::ui::columnsKey(QStringLiteral("uploads")), old);
            core->settings()->sync();
            delete core;
        }
        m_app.start(prof);
        m_app.mw->resize(1280, 800);
        m_app.mw->showPage(QStringLiteral("transfers"));
        QTest::qWait(30);
        MxListView *down = list(QStringLiteral("downloadList"));
        MxListView *up = list(QStringLiteral("uploadList"));
        QVERIFY2(down->columnsFillViewport(), qPrintable(widthsOf(down)));
        QVERIFY2(up->columnsFillViewport(), qPrintable(widthsOf(up)));
        // the proportions of the stored layout are kept (File : User = 0.12 : 0.17)
        const double ratio = double(down->header()->sectionSize(0)) / down->header()->sectionSize(1);
        QVERIFY2(qAbs(ratio - 0.12 / 0.17) < 0.03, qPrintable(widthsOf(down)));
        const double ratioUp = double(up->header()->sectionSize(0)) / up->header()->sectionSize(1);
        QVERIFY2(qAbs(ratioUp - 900.0 / 400.0) < 0.05, qPrintable(widthsOf(up)));
        // and saved without the gap
        down->saveState();
        const QVariantList fr = m_app.core->settings()->value(keys::ui::columnsKey(QStringLiteral("downloads"))).toMap()
                                    .value(QStringLiteral("fractions")).toList();
        double sum = 0;
        for (const QVariant &v : fr)
            sum += qMax(0.0, v.toDouble());
        QVERIFY2(qAbs(sum - 1.0) < 0.01, qPrintable(QString::number(sum)));
        m_app.stop();
        m_app.start(profile());
    }

    void columnsAlwaysFillTheList_data()
    {
        QTest::addColumn<double>("scale");
        QTest::newRow("100%") << 1.0;
        QTest::newRow("150%") << 1.5;
        QTest::newRow("200%") << 2.0;
    }

    void columnsAlwaysFillTheList()
    {
        QFETCH(double, scale);
        m_app.stop();
        Fonts::instance()->attach(nullptr);   // the main window attached the settings of the stopped core
        setUiScale(scale);
        installClassicLook(qApp);
        const QString prof = profile(QStringLiteral("prop%1").arg(int(scale * 100)));
        m_app.start(prof);
        const QSize minSize = m_app.mw->minimumSize().expandedTo(QSize(S(640), S(480)));
        m_app.mw->resize(minSize.width() + S(300), minSize.height() + S(200));
        searchLinux();
        if (auto *chat = qobject_cast<ChatPage *>(m_app.mw->page(QStringLiteral("chat")))) {
            m_app.mw->showPage(QStringLiteral("chat"));
            chat->join(QStringLiteral("Linux Users"));
            QTest::qWait(30);
        }
        m_app.mw->setHotlistVisible(true);
        settle();

        struct Where { const char *page; const char *sub; QStringList lists; };
        const QList<Where> where = {
            {"search", nullptr, {QStringLiteral("searchResults")}},
            {"transfers", nullptr, {QStringLiteral("downloadList"), QStringLiteral("uploadList")}},
            {"shared", nullptr, {QStringLiteral("sharedList")}},
            {"chat", nullptr, {QStringLiteral("chatUserList")}},
            {"networks", "other", {QStringLiteral("serverList")}},
        };
        QRandomGenerator rnd(quint32(4242 + scale * 100));
        auto visibleLists = [this]() {
            QList<MxListView *> out;
            for (MxListView *l : m_app.mw->findChildren<MxListView *>())
                if (l->isVisible() && l->model() && l->viewport()->width() > 0)
                    out << l;
            return out;
        };
        auto check = [&](const QString &step) {
            settle();
            for (MxListView *l : visibleLists())
                if (!l->columnsFillViewport())
                    return QStringLiteral("after %1: %2").arg(step, widthsOf(l));
            return QString();
        };
        int checkedLists = 0;
        QSet<QString> seen;
        for (int stepNo = 0; stepNo < 160; ++stepNo) {
            const Where &w = where.at(rnd.bounded(int(where.size())));
            m_app.mw->showPage(QString::fromLatin1(w.page));
            if (w.sub)
                m_app.mw->page(QString::fromLatin1(w.page))->selectSubTab(QString::fromLatin1(w.sub));
            settle();
            QList<MxListView *> targets;
            for (const QString &n : w.lists)
                for (MxListView *l : m_app.mw->findChildren<MxListView *>(n))
                    if (l->isVisible())
                        targets << l;
            if (MxListView *hl = list(QStringLiteral("hotlistList")); hl && hl->isVisible())
                targets << hl;
            if (targets.isEmpty())
                continue;
            MxListView *l = targets.at(rnd.bounded(int(targets.size())));
            seen.insert(l->objectName());
            QHeaderView *h = l->header();
            const QList<int> cols = visibleOf(l);
            QString step;
            switch (rnd.bounded(9)) {
            case 0: {
                const QSize s(minSize.width() + rnd.bounded(S(900)), minSize.height() + rnd.bounded(S(400)));
                m_app.mw->resize(s);
                step = QStringLiteral("resize %1x%2").arg(s.width()).arg(s.height());
                break;
            }
            case 1:
            case 2:
                if (!h->isHidden() && !cols.isEmpty()) {
                    const int c = cols.at(rnd.bounded(int(cols.size())));
                    const int dx = rnd.bounded(S(400)) - S(200);
                    dragBorder(l, c, dx);
                    step = QStringLiteral("drag %1 col %2 by %3").arg(l->objectName()).arg(c).arg(dx);
                } else {
                    m_app.mw->resize(m_app.mw->width() + S(17), m_app.mw->height());
                    step = QStringLiteral("resize +17");
                }
                break;
            case 3:
                if (!cols.isEmpty()) {
                    const int c = cols.at(rnd.bounded(int(cols.size())));
                    const int wpx = S(20) + rnd.bounded(S(300));
                    h->resizeSection(c, wpx);
                    step = QStringLiteral("code resize %1 col %2 to %3").arg(l->objectName()).arg(c).arg(wpx);
                }
                break;
            case 4:
                if (h->count() > 1) {
                    const int c = 1 + rnd.bounded(h->count() - 1);
                    const bool on = h->isSectionHidden(c);
                    l->setColumnVisible(c, on);
                    step = QStringLiteral("%1 col %2 %3").arg(on ? QStringLiteral("show") : QStringLiteral("hide")).arg(c).arg(l->objectName());
                }
                break;
            case 5:
                if (h->count() > 2 && !h->isHidden()) {
                    const int from = 1 + rnd.bounded(h->count() - 1), to = 1 + rnd.bounded(h->count() - 1);
                    h->moveSection(from, to);
                    step = QStringLiteral("move %1 %2 -> %3").arg(l->objectName()).arg(from).arg(to);
                }
                break;
            case 6:
                l->resetColumns();
                step = QStringLiteral("reset %1").arg(l->objectName());
                break;
            case 7:
                m_app.mw->setHotlistVisible(!m_app.mw->isHotlistVisible());
                step = QStringLiteral("hotlist toggled");
                break;
            case 8:
                if (l->objectName() == QLatin1String("searchResults")) {
                    // the vertical scroll bar comes (many rows) and goes (one row)
                    l->setFilterText(rnd.bounded(2) ? QStringLiteral("debian") : QString());
                    l->expandAll();
                    QTest::qWait(200);
                    step = QStringLiteral("filter / expand");
                } else {
                    m_app.mw->resize(m_app.mw->width(), minSize.height() + rnd.bounded(S(300)));
                    step = QStringLiteral("height change");
                }
                break;
            }
            if (step.isEmpty())
                continue;
            const QString err = check(step);
            QVERIFY2(err.isEmpty(), qPrintable(QStringLiteral("%1% step %2: %3").arg(int(scale * 100)).arg(stepNo).arg(err)));
            checkedLists += int(visibleLists().size());
        }
        QVERIFY(checkedLists > 160);
        QVERIFY2(seen.size() >= 5, qPrintable(QStringList(seen.values()).join(QLatin1Char(','))));
        // a restart keeps the layout of the lists the user touched
        m_app.mw->showPage(QStringLiteral("transfers"));
        MxListView *down = list(QStringLiteral("downloadList"));
        dragBorder(down, visibleOf(down).first(), S(30));
        const QSize winSize = m_app.mw->size();
        settle();
        QStringList before;
        for (int c = 0; c < down->header()->count(); ++c)
            before << QString::number(down->header()->isSectionHidden(c) ? -1 : down->header()->sectionSize(c));
        m_app.stop();
        m_app.start(prof);
        m_app.mw->resize(winSize);
        m_app.mw->showPage(QStringLiteral("transfers"));
        QTest::qWait(30);
        down = list(QStringLiteral("downloadList"));
        QStringList after;
        for (int c = 0; c < down->header()->count(); ++c)
            after << QString::number(down->header()->isSectionHidden(c) ? -1 : down->header()->sectionSize(c));
        QVERIFY2(down->columnsFillViewport(), qPrintable(widthsOf(down)));
        for (int c = 0; c < before.size(); ++c)
            QVERIFY2(qAbs(before[c].toInt() - after[c].toInt()) <= 1,
                     qPrintable(before.join(QLatin1Char(' ')) + QStringLiteral(" / ") + after.join(QLatin1Char(' '))));
        m_app.stop();
        Fonts::instance()->attach(nullptr);
        setUiScale(1.0);
        installClassicLook(qApp);
        m_app.start(profile());
    }
};

QTEST_MAIN(TstUig)
#include "tst_uig.moc"
