// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: [ui-e] script verbs for the usability features (screenshots, tests).
//
//   header-menu <list>              open the column chooser of a list (header right-click)
//   column-visible <list> <col> on|off   show / hide a column like the chooser does
//   reset-columns <list>            the chooser's "Reset Columns"
//   filter <list> [text]            open the type-to-filter box (Ctrl+F) and type text
//   expect-filter <list> <n>        n top-level rows shown
//   search-history                  open the recent-searches menu of the current search tab
//   expect-column-header <list> <col> <text>
//   expect-cell <list> <row> <col> <text>   (row = visible row index)
//   notice                          echo the current status-bar notice into the log
//   wait-download <seconds> <name part>   wait until such a download is complete (real-core runs)
//   report-windows                  [qa-2] log every visible top-level's frame vs the screen work area
//   report-clipped                  [qa-2] log captions wider than their widget / widgets outside their parent
//   tooltip <objectName>            [ui-g] show the widget's tool tip as hovering does (shot composites it)
//   tooltip-row <list> <row> <col>  [merge-1] the tool tip of one cell of a list (visible row index)
//   expect-columns-fill <list>      [ui-g] the visible columns end exactly at the viewport's right edge
//   report-rows <list> <file>       [ui-g] append "row <n> <x> <y> <w> <h> <text>" (screen px) for the visible rows
//   wait-file <file> <seconds>      [ui-g] wait until <file> exists (a driving script creates it)
//   echo-file <file>                [ui-g] append a line to <file> (a phase mark for a driving script)
//   watch <seconds> <file> [stop]   [ui-g] from now on (it returns at once: follow it with "wait") log to <file>
//                                   every mouse press, every row a context menu is opened for, each menu shown
//                                   (entries with their screen centres), clipboard changes and message boxes
//                                   (answered No / Cancel); after <seconds>, or once <stop> exists, the app exits.
//                                   For real-mouse checks driven from outside (tests/ui/rightclick_real_mouse.py)
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QTextStream>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtGui/QHelpEvent>
#include <QtGui/QContextMenuEvent>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtGui/QScreen>
#include <memory>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QApplication>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QStyle>

#include "mx/services.h"
#include "ui/kit/list_view.h"
#include "ui/kit/scale.h"
#include "ui/kit/widgets.h"
#include "ui/main_window.h"
#include "ui/script_driver.h"

namespace mx::ui {

namespace {

MxListView *listNamed(MainWindow *mw, const QString &name, QString *error)
{
    auto *l = qobject_cast<MxListView *>(ScriptDriver::findWidget(mw, name));
    if (!l)
        *error = QStringLiteral("no list view named %1").arg(name);
    return l;
}

void appendLine(const QString &file, const QString &line)
{
    QFile f(file);
    if (f.open(QIODevice::Append | QIODevice::Text))
        QTextStream(&f) << line << '\n';
}

QString rowText(MxListView *l, const QModelIndex &i)
{
    return i.isValid() ? i.siblingAtColumn(0).data().toString() : QStringLiteral("<none>");
}

// [ui-g] logs the mouse presses the application receives (which widget, where, which button)
class PressLogger : public QObject {
public:
    PressLogger(const QString &file, QObject *parent) : QObject(parent), m_file(file) {}

protected:
    bool eventFilter(QObject *o, QEvent *e) override
    {
        if ((e->type() == QEvent::MouseButtonPress || e->type() == QEvent::ContextMenu) && o->isWidgetType()) {
            QPoint g;
            int b = 0;
            if (e->type() == QEvent::MouseButtonPress) {
                auto *me = static_cast<QMouseEvent *>(e);
                g = me->globalPosition().toPoint();
                b = int(me->button());
            } else {
                g = static_cast<QContextMenuEvent *>(e)->globalPos();
            }
            appendLine(m_file, QStringLiteral("%1 %2/%3 at %4 %5 button %6")
                                   .arg(e->type() == QEvent::ContextMenu ? QStringLiteral("contextevent") : QStringLiteral("press"),
                                        QString::fromLatin1(o->metaObject()->className()), o->objectName())
                                   .arg(g.x()).arg(g.y()).arg(b));
        }
        return false;
    }

private:
    QString m_file;
};

// [ui-g] the real-mouse watcher (see "watch" above). It must not block: under wine the mouse input only
// arrives while the normal event loop runs, so it installs its hooks and a timer and returns at once
// (the script goes on with "wait"); after <seconds> or when <stop> exists it ends the application.
void watch(MainWindow *mw, int seconds, const QString &file, const QString &stopFile)
{
    auto *presses = new PressLogger(file, mw);
    qApp->installEventFilter(presses);
    for (MxListView *l : mw->findChildren<MxListView *>()) {
        QPointer<MxListView> pl(l);
        QObject::connect(l, &MxListView::contextMenuAt, presses, [pl, file](const QModelIndex &vi, const QPoint &g) {
            if (!pl)
                return;
            QStringList sel;
            for (const QModelIndex &i : pl->selectionModel()->selectedRows(0))
                sel << rowText(pl, i);
            appendLine(file, QStringLiteral("context %1 at %2 %3 row=%4 current=%5 selected=%6")
                                 .arg(pl->objectName()).arg(g.x()).arg(g.y()).arg(rowText(pl, vi), rowText(pl, pl->currentIndex()),
                                                                                  sel.join(QLatin1Char('|'))));
        });
    }
    QObject::connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, presses, [file] {
        appendLine(file, QStringLiteral("clipboard %1").arg(QGuiApplication::clipboard()->text()));
    });
    // the menus delete themselves when closed: keep guarded pointers only
    auto seen = std::make_shared<QList<QPointer<QMenu>>>();
    auto started = std::make_shared<QElapsedTimer>();
    started->start();
    auto *timer = new QTimer(presses);
    timer->setInterval(50);
    QObject::connect(timer, &QTimer::timeout, presses, [=] {
        for (QWidget *w : QApplication::topLevelWidgets()) {
            if (auto *m = qobject_cast<QMenu *>(w); m && m->isVisible() && !seen->contains(QPointer<QMenu>(m))) {
                seen->append(QPointer<QMenu>(m));
                QStringList entries;
                for (QAction *a : m->actions()) {
                    if (a->isSeparator() || !a->isVisible())
                        continue;
                    const QPoint c = m->mapToGlobal(m->actionGeometry(a).center());
                    entries << QStringLiteral("%1@%2,%3").arg(a->text().remove(QLatin1Char('&'))).arg(c.x()).arg(c.y());
                }
                appendLine(file, QStringLiteral("menu %1 %2").arg(m->objectName(), entries.join(QLatin1Char(';'))));
            }
            if (auto *box = qobject_cast<QMessageBox *>(w); box && box->isVisible()) {
                appendLine(file, QStringLiteral("question %1").arg(box->text().simplified()));
                QAbstractButton *no = box->button(QMessageBox::No);
                if (!no)
                    no = box->button(QMessageBox::Cancel);
                if (no)
                    no->click();
                else
                    box->reject();
            }
        }
        seen->removeIf([](const QPointer<QMenu> &m) { return !m || !m->isVisible(); });
        if (started->elapsed() > qint64(seconds) * 1000 || (!stopFile.isEmpty() && QFile::exists(stopFile))) {
            timer->stop();
            qApp->removeEventFilter(presses);
            appendLine(file, QStringLiteral("watch done"));
            presses->deleteLater();
            QTimer::singleShot(100, qApp, [] { QCoreApplication::exit(0); });
        }
    });
    timer->start();
}

bool uieVerbs(MainWindow *mw, const QString &verb, const QString &rest, const QStringList &args, QString *error)
{
    if (verb == QLatin1String("tooltip")) {
        QWidget *w = ScriptDriver::findWidget(mw, args.value(0));
        if (!w) {
            *error = QStringLiteral("no widget named %1").arg(args.value(0));
            return true;
        }
        const QPoint local(w->width() / 2, w->height() / 2);
        QHelpEvent he(QEvent::ToolTip, local, w->mapToGlobal(local));
        QApplication::sendEvent(w, &he);
        return true;
    }
    if (verb == QLatin1String("tooltip-row")) {   // [merge-1] tooltip-row <list> <visible row> <column>: a cell's tool tip
        if (MxListView *l = listNamed(mw, args.value(0), error)) {
            const QModelIndex row = l->visibleRow(args.value(1).toInt());
            if (!row.isValid()) {
                *error = QStringLiteral("%1: no visible row %2").arg(args.value(0), args.value(1));
                return true;
            }
            const QModelIndex cell = row.sibling(row.row(), args.value(2).toInt());
            QRect r = l->visualRect(cell);
            const QPoint local = r.center();
            QHelpEvent he(QEvent::ToolTip, local, l->viewport()->mapToGlobal(local));
            QApplication::sendEvent(l->viewport(), &he);
        }
        return true;
    }
    if (verb == QLatin1String("expect-columns-fill")) {
        if (MxListView *l = listNamed(mw, args.value(0), error))
            if (!l->columnsFillViewport())
                *error = QStringLiteral("%1: the columns do not end at the right edge").arg(l->objectName());
        return true;
    }
    if (verb == QLatin1String("report-rows")) {
        if (MxListView *l = listNamed(mw, args.value(0), error)) {
            for (int n = 0; n < l->visibleRowCount(); ++n) {
                const QModelIndex i = l->visibleRow(n);
                QRect r = l->visualRect(i);
                if (!r.isValid() || !l->viewport()->rect().intersects(r))
                    continue;
                r.setLeft(0);
                r.setRight(l->viewport()->width() - 1);
                const QPoint g = l->viewport()->mapToGlobal(r.topLeft());
                appendLine(args.value(1), QStringLiteral("row %1 %2 %3 %4 %5 %6")
                                              .arg(n).arg(g.x()).arg(g.y()).arg(r.width()).arg(r.height()).arg(rowText(l, i)));
            }
        }
        return true;
    }
    if (verb == QLatin1String("wait-file")) {   // [ui-g] wait-file <path> <seconds>: until a driving script creates it
        // [ovl-2] through holdUntil: a nested event loop here let the already armed next-step timer run
        // the following lines (shot, quit) while still "waiting", so the app quit long before the file
        const QString path = args.value(0);
        if (ScriptDriver *d = ScriptDriver::current()) {
            d->holdUntil([path] { return QFile::exists(path); }, args.value(1).toInt() * 1000,
                         QStringLiteral("%1 did not appear").arg(path));
        } else if (!QFile::exists(path)) {
            *error = QStringLiteral("%1 did not appear").arg(path);
        }
        return true;
    }
    if (verb == QLatin1String("echo-file")) {   // [ui-g] mark a phase for a driving shell script
        appendLine(rest, QStringLiteral("done"));
        return true;
    }
    if (verb == QLatin1String("watch")) {
        watch(mw, args.value(0).toInt(), args.value(1), args.value(2));
        return true;
    }
    if (verb == QLatin1String("header-menu")) {
        if (MxListView *l = listNamed(mw, args.value(0), error)) {
            const QPoint at = l->header()->mapToGlobal(QPoint(l->header()->width() / 3, l->header()->height() / 2));
            l->showColumnMenu(at);
        }
        return true;
    }
    if (verb == QLatin1String("column-visible")) {
        if (MxListView *l = listNamed(mw, args.value(0), error))
            l->setColumnVisible(args.value(1).toInt(), args.value(2) != QLatin1String("off"));
        return true;
    }
    if (verb == QLatin1String("reset-columns")) {
        if (MxListView *l = listNamed(mw, args.value(0), error))
            l->resetColumns();
        return true;
    }
    if (verb == QLatin1String("filter")) {
        if (MxListView *l = listNamed(mw, args.value(0), error)) {
            if (args.size() > 1)
                l->setFilterText(rest.section(QLatin1Char(' '), 1));
            else
                l->showFilter();
        }
        return true;
    }
    if (verb == QLatin1String("expect-filter")) {
        if (MxListView *l = listNamed(mw, args.value(0), error))
            if (l->filterMatches() != args.value(1).toInt())
                *error = QStringLiteral("%1 rows shown").arg(l->filterMatches());
        return true;
    }
    if (verb == QLatin1String("search-history")) {
        // the visible search tab's keyword box has the history action
        for (QLineEdit *e : mw->findChildren<QLineEdit *>(QStringLiteral("searchText")))
            if (e->isVisible())
                for (QAction *a : e->actions())
                    if (a->objectName() == QLatin1String("searchHistoryButton")) {
                        a->trigger();
                        return true;
                    }
        *error = QStringLiteral("no search history button");
        return true;
    }
    if (verb == QLatin1String("expect-column-header")) {
        if (MxListView *l = listNamed(mw, args.value(0), error)) {
            const QString have = l->model()->headerData(args.value(1).toInt(), Qt::Horizontal, Qt::DisplayRole).toString();
            const QString want = rest.section(QLatin1Char(' '), 2);
            if (have != want)
                *error = QStringLiteral("header is '%1'").arg(have);
        }
        return true;
    }
    if (verb == QLatin1String("expect-cell")) {
        if (MxListView *l = listNamed(mw, args.value(0), error)) {
            const QModelIndex row = l->visibleRow(args.value(1).toInt());
            const QString have = row.sibling(row.row(), args.value(2).toInt()).data().toString();
            const QString want = rest.section(QLatin1Char(' '), 3);
            if (have != want)
                *error = QStringLiteral("cell is '%1'").arg(have);
        }
        return true;
    }
    // wait-download <seconds> <name part>: until a download whose name contains the text is complete
    if (verb == QLatin1String("wait-download")) {
        const int secs = args.value(0).toInt();
        const QString want = rest.section(QLatin1Char(' '), 1);
        ScriptDriver *d = ScriptDriver::current();
        if (!d) {
            *error = QStringLiteral("not inside a script");
            return true;
        }
        auto clock = std::make_shared<QElapsedTimer>();
        clock->start();
        d->holdUntil([mw, want, clock] {
            for (const TransferInfo &x : mw->core()->transfers()->downloads())
                if (x.name.contains(want, Qt::CaseInsensitive) &&
                    (x.state == TransferState::Completed || x.state == TransferState::Seeding)) {
                    qInfo("wait-download: %s complete after %lld s (%s)", qPrintable(x.name), clock->elapsed() / 1000,
                          qPrintable(x.savePath));
                    return true;
                }
            return false;
        }, secs * 1000, QStringLiteral("no complete download matching '%1' after %2 s").arg(want).arg(secs));
        return true;
    }
    if (verb == QLatin1String("notice")) {
        qInfo("notice: %s", qPrintable(mw->notice()));
        return true;
    }
    // [qa-2] report-windows: every visible top-level with its frame and the screen's work area;
    // "OUTSIDE" when part of it is off the screen (a dialog larger than the screen)
    if (verb == QLatin1String("report-windows")) {
        for (QWidget *w : QApplication::topLevelWidgets()) {
            if (!w->isVisible())
                continue;
            const QRect f = w->frameGeometry();
            const QRect avail = w->screen() ? w->screen()->availableGeometry() : QRect();
            qInfo("window %s (%s) frame %d,%d %dx%d client %dx%d screen %dx%d%s", qPrintable(w->objectName()),
                  w->metaObject()->className(), f.x(), f.y(), f.width(), f.height(), w->width(), w->height(), avail.width(),
                  avail.height(), avail.contains(f) ? "" : " OUTSIDE");
        }
        return true;
    }
    // [qa-2] report-clipped: visible captions whose text is wider than the widget, wrapped texts taller
    // than their box, and widgets that stick out of their parent (QA of every page at every scale)
    if (verb == QLatin1String("report-clipped")) {
        int n = 0;
        for (QWidget *top : QApplication::topLevelWidgets()) {
            if (!top->isVisible())
                continue;
            for (QWidget *w : top->findChildren<QWidget *>()) {
                if (!w->isVisibleTo(top) || w->width() <= 0 || w->window() != top)
                    continue;
                QString what;
                const QFontMetrics fm(w->font());
                const QString name = w->objectName().isEmpty() ? QString::fromLatin1(w->metaObject()->className()) : w->objectName();
                QString text;
                int need = 0;
                if (auto *st = qobject_cast<StaticText *>(w)) {
                    text = st->text();
                    need = QFontMetrics(fitFont(w->font(), text, w->width())).horizontalAdvance(text);
                } else if (auto *wl = qobject_cast<WrapLabel *>(w)) {
                    text = wl->text();
                    QFont f;
                    int pitch = 0;
                    QStringList lines;
                    if (!wl->fitText(&f, &pitch, &lines))
                        what = QStringLiteral("wrapped text does not fit %1 px height (%2 lines at %3 px): last line '%4'")
                                   .arg(wl->height()).arg(lines.size()).arg(pitch).arg(lines.value(int(lines.size()) - 1));
                } else if (auto *lb = qobject_cast<QLabel *>(w)) {
                    if (!lb->wordWrap() && lb->textFormat() != Qt::RichText && !Qt::mightBeRichText(lb->text())) {
                        text = lb->text();
                        need = fm.horizontalAdvance(text) + 2 * lb->margin();
                    }
                } else if (qobject_cast<QCheckBox *>(w) || qobject_cast<QRadioButton *>(w)) {
                    // indicator + label spacing + text (sizeHint adds style margins the text does not need)
                    text = static_cast<QAbstractButton *>(w)->text();
                    const bool radio = qobject_cast<QRadioButton *>(w);
                    need = w->style()->pixelMetric(radio ? QStyle::PM_ExclusiveIndicatorWidth : QStyle::PM_IndicatorWidth, nullptr, w) +
                           w->style()->pixelMetric(radio ? QStyle::PM_RadioButtonLabelSpacing : QStyle::PM_CheckBoxLabelSpacing, nullptr, w) +
                           QFontMetrics(fitFont(w->font(), QString(text).remove(QLatin1Char('&')), w->width() - S(17)))
                               .horizontalAdvance(QString(text).remove(QLatin1Char('&')));
                } else if (auto *pb = qobject_cast<QPushButton *>(w)) {
                    text = pb->text();
                    need = fm.horizontalAdvance(QString(text).remove(QLatin1Char('&'))) + S(6) +
                           (pb->icon().isNull() ? 0 : pb->iconSize().width() + S(4)) + (pb->menu() ? S(12) : 0);
                }
                if (what.isEmpty() && need > 0 && !text.isEmpty() && !text.contains(QLatin1Char('\n')) && need > w->width())
                    what = QStringLiteral("'%1' needs %2 px, has %3").arg(text).arg(need).arg(w->width());
                QWidget *p = w->parentWidget();
                if (what.isEmpty() && p && p != top->parentWidget() && !qobject_cast<QAbstractScrollArea *>(p) &&
                    !qobject_cast<QAbstractScrollArea *>(p->parentWidget()) && p->objectName() != QLatin1String("qt_scrollarea_viewport") &&
                    !p->rect().contains(w->geometry()) && !w->isWindow())
                    what = QStringLiteral("outside its parent %1 (%2,%3 %4x%5 in %6x%7)")
                               .arg(p->objectName().isEmpty() ? QString::fromLatin1(p->metaObject()->className()) : p->objectName())
                               .arg(w->x()).arg(w->y()).arg(w->width()).arg(w->height()).arg(p->width()).arg(p->height());
                if (!what.isEmpty()) {
                    ++n;
                    qInfo("clipped [%s] %s: %s", qPrintable(top->objectName()), qPrintable(name), qPrintable(what));
                }
            }
        }
        qInfo("clipped total %d", n);
        return true;
    }
    return false;
}

} // namespace

// called from the MainWindow constructor (a static initializer alone would be dropped by the linker,
// nothing else in this file is referenced)
void registerUieScriptVerbs()
{
    static const bool once = [] {
        ScriptDriver::addCommandHandler(&uieVerbs);
        return true;
    }();
    Q_UNUSED(once)
}

} // namespace mx::ui
