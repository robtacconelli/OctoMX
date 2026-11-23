// SPDX-License-Identifier: GPL-3.0-or-later
// [ui-d] MainWindow / FloatWindow additions: floating-window placement and the script verbs used by
// tst_restart and the screenshot scripts (lists, splitters, sub-tabs, windows, menu buttons).
#include <QtCore/QTimer>
#include <QtGui/QMoveEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QTreeView>

#include "mx/services.h"
#include "mx/settings_keys.h"
#include "ui/kit/list_view.h"
#include "ui/kit/widgets.h"
#include "ui/main_window.h"
#include "ui/pages/page.h"
#include "ui/pages/networks_page.h"
#include "ui/script_driver.h"

namespace mx::ui {

bool FloatWindow::restorePlacement()
{
    if (!m_page || !m_page->core())
        return false;
    const QByteArray g = m_page->core()->settings()->value(keys::ui::dialogKey(QStringLiteral("float_") + m_pageId)).toByteArray();
    const bool ok = !g.isEmpty() && restoreGeometry(g);
    m_placed = true;
    return ok;
}

void FloatWindow::savePlacement()
{
    if (!m_page || !m_page->core() || !m_placed)
        return;
    m_page->core()->settings()->setValue(keys::ui::dialogKey(QStringLiteral("float_") + m_pageId), saveGeometry());
}

void FloatWindow::moveEvent(QMoveEvent *e)
{
    QWidget::moveEvent(e);
    if (isVisible())
        savePlacement();
}

void FloatWindow::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    if (isVisible())
        savePlacement();
}

namespace {

QWidget *topLevelNamed(MainWindow *mw, const QString &name)
{
    if (QWidget *w = ScriptDriver::findWidget(mw, name))
        return w->window();
    return nullptr;
}

// move-window <obj> x y | resize-window <obj> w h | expect-window <obj> x y w h
// sort <list> <col> asc|desc | expect-sort <list> <col> asc|desc
// move-column <list> <logical> <visual> | expect-column-pos <list> <logical> <visual>
// set-splitter <splitter> <size0> | expect-splitter <splitter> <size0>
// expect-subtab <page> <index> | expect-choice <menubutton> <data> | expect-text <widget> <text>
bool uidVerbs(MainWindow *mw, const QString &verb, const QString &rest, const QStringList &args, QString *error)
{
    Q_UNUSED(rest)
    if (verb == QLatin1String("move-window") || verb == QLatin1String("resize-window") || verb == QLatin1String("expect-window")) {
        QWidget *w = topLevelNamed(mw, args.value(0));
        if (!w) {
            *error = QStringLiteral("no window %1").arg(args.value(0));
            return true;
        }
        const int a = args.value(1).toInt(), b = args.value(2).toInt();
        if (verb == QLatin1String("move-window"))
            w->move(a, b);
        else if (verb == QLatin1String("resize-window"))
            w->resize(a, b);
        else {
            const QRect want(a, b, args.value(3).toInt(), args.value(4).toInt());
            const QRect have(w->pos(), w->size());
            if (have != want)
                *error = QStringLiteral("%1 is at %2,%3 %4x%5").arg(args.value(0)).arg(have.x()).arg(have.y()).arg(have.width()).arg(have.height());
        }
        return true;
    }
    if (verb == QLatin1String("sort") || verb == QLatin1String("expect-sort") || verb == QLatin1String("move-column") ||
        verb == QLatin1String("expect-column-pos")) {
        auto *l = qobject_cast<QTreeView *>(ScriptDriver::findWidget(mw, args.value(0)));
        if (!l) {
            *error = QStringLiteral("no list view named %1").arg(args.value(0));
            return true;
        }
        QHeaderView *h = l->header();
        const int col = args.value(1).toInt();
        if (verb == QLatin1String("sort")) {
            l->sortByColumn(col, args.value(2) == QLatin1String("desc") ? Qt::DescendingOrder : Qt::AscendingOrder);
        } else if (verb == QLatin1String("expect-sort")) {
            const Qt::SortOrder o = args.value(2) == QLatin1String("desc") ? Qt::DescendingOrder : Qt::AscendingOrder;
            if (h->sortIndicatorSection() != col || h->sortIndicatorOrder() != o)
                *error = QStringLiteral("sorted by %1 (%2)").arg(h->sortIndicatorSection()).arg(int(h->sortIndicatorOrder()));
        } else if (verb == QLatin1String("move-column")) {
            h->moveSection(h->visualIndex(col), args.value(2).toInt());
        } else if (h->visualIndex(col) != args.value(2).toInt()) {
            *error = QStringLiteral("column %1 is at %2").arg(col).arg(h->visualIndex(col));
        }
        return true;
    }
    if (verb == QLatin1String("set-splitter") || verb == QLatin1String("expect-splitter")) {
        auto *sp = qobject_cast<QSplitter *>(ScriptDriver::findWidget(mw, args.value(0)));
        if (!sp) {
            *error = QStringLiteral("no splitter %1").arg(args.value(0));
            return true;
        }
        const int want = args.value(1).toInt();
        QList<int> sizes = sp->sizes();
        if (verb == QLatin1String("set-splitter")) {
            int total = 0;
            for (int x : sizes)
                total += x;
            sizes = {want, qMax(1, total - want)};
            sp->setSizes(sizes);
            emit sp->splitterMoved(want, 1);
        } else if (qAbs(sizes.value(0) - want) > 1) {
            *error = QStringLiteral("splitter %1 first size %2").arg(args.value(0)).arg(sizes.value(0));
        }
        return true;
    }
    // select-text / dblclick-text / context-menu-text <list> <text>: the first visible row whose
    // any column contains <text>
    // [merge-1] expand-text <list> <text>; tooltip-text <list> <column> <text>: the cell's tool tip
    if (verb == QLatin1String("select-text") || verb == QLatin1String("dblclick-text") || verb == QLatin1String("context-menu-text")
        || verb == QLatin1String("expand-text") || verb == QLatin1String("tooltip-text")) {
        auto *l = qobject_cast<MxListView *>(ScriptDriver::findWidget(mw, args.value(0)));
        const bool tip = verb == QLatin1String("tooltip-text");
        const int tipCol = tip ? args.value(1).toInt() : 0;
        const QString want = args.mid(tip ? 2 : 1).join(QLatin1Char(' '));
        if (!l) {
            *error = QStringLiteral("no list view named %1").arg(args.value(0));
            return true;
        }
        QModelIndex hit;
        for (int i = 0; i < l->visibleRowCount() && !hit.isValid(); ++i) {
            const QModelIndex idx = l->visibleRow(i);
            for (int c = 0; c < l->model()->columnCount(idx.parent()) && !hit.isValid(); ++c)
                if (idx.sibling(idx.row(), c).data().toString().contains(want, Qt::CaseInsensitive))
                    hit = idx;
        }
        if (!hit.isValid()) {
            *error = QStringLiteral("no row with '%1' in %2").arg(want, args.value(0));
            return true;
        }
        l->selectRow(hit);
        l->scrollTo(hit);
        if (verb == QLatin1String("expand-text")) {
            l->expand(hit.sibling(hit.row(), 0));
        } else if (tip) {
            const QRect r = l->visualRect(hit.sibling(hit.row(), tipCol));
            const QPoint local = r.center();
            QHelpEvent he(QEvent::ToolTip, local, l->viewport()->mapToGlobal(local));
            QApplication::sendEvent(l->viewport(), &he);
        } else if (verb == QLatin1String("dblclick-text")) {
            emit l->rowActivated(hit);
        } else if (verb == QLatin1String("context-menu-text")) {
            const QRect r = l->visualRect(hit);
            emit l->contextMenuAt(hit, l->viewport()->mapToGlobal(QPoint(r.left() + 40, r.center().y())));
        }
        return true;
    }
    // accept-dialogs: Yes / OK on open message boxes, accept() on other dialogs
    if (verb == QLatin1String("accept-dialogs")) {
        for (QWidget *w : QApplication::topLevelWidgets()) {
            if (auto *mb = qobject_cast<QMessageBox *>(w); mb && mb->isVisible()) {
                QAbstractButton *b = mb->button(QMessageBox::Yes);
                if (!b)
                    b = mb->button(QMessageBox::Ok);
                if (b)
                    b->click();
                else
                    mb->accept();
            } else if (auto *d = qobject_cast<QDialog *>(w); d && d->isVisible()) {
                d->accept();
            }
        }
        return true;
    }
    if (verb == QLatin1String("expect-subtab")) {
        if (args.value(0) == QLatin1String("networks")) {
            auto *np = qobject_cast<NetworksPage *>(mw->page(QStringLiteral("networks")));
            auto *tabs = np ? np->findChild<TabStrip *>(QStringLiteral("networksTabs")) : nullptr;
            if (!tabs || tabs->currentIndex() != args.value(1).toInt())
                *error = QStringLiteral("networks sub-tab is %1").arg(tabs ? tabs->currentIndex() : -1);
        } else {
            *error = QStringLiteral("expect-subtab: unknown page %1").arg(args.value(0));
        }
        return true;
    }
    if (verb == QLatin1String("expect-choice")) {
        auto *mb = qobject_cast<MenuButton *>(ScriptDriver::findWidget(mw, args.value(0)));
        const QString want = args.mid(1).join(QLatin1Char(' '));
        if (!mb)
            *error = QStringLiteral("no menu button %1").arg(args.value(0));
        else if (mb->currentData().toString() != want)
            *error = QStringLiteral("%1 = %2").arg(args.value(0), mb->currentData().toString());
        return true;
    }
    return false;
}

[[maybe_unused]] const bool g_registered = [] {
    ScriptDriver::addCommandHandler(&uidVerbs);
    return true;
}();

} // namespace

} // namespace mx::ui
