// SPDX-License-Identifier: GPL-3.0-or-later
// [ui-i] The download bar as a map of the file: cells to pixel columns (edge cases), the incremental
// fallback, the colours of both themes, the bar painted inside its cell over the whole column at every
// interface scale (and following the column when it is resized), the tool tip line and the roles the
// Transfers model gives.
#include <QtGui/QPainter>
#include <QtGui/QStandardItemModel>
#include <QtTest/QtTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QHeaderView>

#include "mx/settings_keys.h"
#include "mx/simple_settings.h"
#include "ui/kit/colors.h"
#include "ui/kit/format.h"
#include "ui/kit/list_view.h"
#include "ui/kit/piece_bar.h"
#include "ui/kit/scale.h"
#include "ui/kit/theme.h"
#include "ui/models/roles.h"
#include "ui/models/transfer_model.h"

using namespace mx;
using namespace mx::ui;
using namespace mx::ui::transfers;
using piecebar::Column;

namespace {

QByteArray cells(int n, char v) { return QByteArray(n, v); }

int countActive(const QVector<Column> &c)
{
    int n = 0;
    for (const Column &x : c) n += x.active;
    return n;
}

TransferInfo dl(const QString &id, TransferState st, const QString &status, qint64 done, qint64 size)
{
    TransferInfo t;
    t.id = id;
    t.name = id + QStringLiteral(".iso");
    t.state = st;
    t.statusText = status;
    t.done = done;
    t.size = size;
    return t;
}

// the x range of pixels in `row` of `img` that have colour c
QPair<int, int> spanOf(const QImage &img, int y, const QColor &c)
{
    int lo = -1, hi = -1;
    for (int x = 0; x < img.width(); ++x)
        if (QColor(img.pixel(x, y)) == c) {
            if (lo < 0) lo = x;
            hi = x;
        }
    return {lo, hi};
}

} // namespace

class TstPieceBar : public QObject {
    Q_OBJECT
private slots:
    void columnsEdgeCases();
    void singleCellVisibleAtAnyWidth();
    void incrementalFallback();
    void themeColors();
    void paintsInsideCellAtEveryScale();
    void tipLine();
    void modelRoles();
};

void TstPieceBar::columnsEdgeCases()
{
    // empty map, no width: nothing (the delegate then draws the incremental bar)
    QVERIFY(piecebar::columns(QByteArray(), QByteArray(), 100).isEmpty());
    QVERIFY(piecebar::columns(cells(10, 2), QByteArray(), 0).isEmpty());
    // all complete: one solid bar; nothing: the empty frame
    for (int w : {1, 7, 100, 512, 513, 1300}) {
        for (const Column &c : piecebar::columns(cells(512, 2), QByteArray(), w)) QCOMPARE(c.share, 255);
        for (const Column &c : piecebar::columns(cells(512, 0), cells(512, 0), w)) {
            QCOMPARE(c.share, 0);
            QVERIFY(!c.active);
        }
    }
    // 1 px wide: the whole file in one column, partly there unless every cell is complete
    QByteArray half = cells(512, 0);
    for (int i = 0; i < 256; ++i) half[i] = 2;
    QVector<Column> one = piecebar::columns(half, QByteArray(), 1);
    QCOMPARE(one.size(), 1);
    QVERIFY(qAbs(one[0].share - 128) <= 1);
    QByteArray almost = cells(512, 2);
    almost[511] = 0;
    QCOMPARE(piecebar::columns(almost, QByteArray(), 1)[0].share, 254);   // never full while a cell is missing
    QByteArray tiny = cells(512, 0);
    tiny[0] = 0;
    QByteArray tinyFill = cells(512, 0);
    tinyFill[0] = 1;                                                   // one verified byte somewhere
    QCOMPARE(piecebar::columns(tiny, tinyFill, 1)[0].share, 1);         // never empty while something is there
    // more cells than pixels: alternating complete / missing cells give partly-there columns
    QByteArray alt = cells(512, 0);
    for (int i = 0; i < 512; i += 2) alt[i] = 2;
    for (const Column &c : piecebar::columns(alt, QByteArray(), 100)) {
        QVERIFY(c.share > 0 && c.share < 255);
        QVERIFY(!c.active);
    }
    // wider than the map: a complete cell spans its pixels exactly, the neighbours stay empty
    QByteArray single = cells(512, 0);
    single[100] = 2;
    const QVector<Column> wide = piecebar::columns(single, QByteArray(), 1024);   // 2 px per cell
    QCOMPARE(wide[200].share, 255);
    QCOMPARE(wide[201].share, 255);
    QCOMPARE(wide[199].share, 0);
    QCOMPARE(wide[202].share, 0);
    // the fill of a cell that is not complete counts by its share
    QByteArray st = cells(4, 0), fill = cells(4, char(128));
    for (const Column &c : piecebar::columns(st, fill, 10)) QCOMPARE(c.share, 128);
    // a fill of 255 on a cell that is not complete is not "all there"
    for (const Column &c : piecebar::columns(cells(4, 1), cells(4, char(255)), 10)) {
        QVERIFY(c.share < 255);
        QVERIFY(c.active);
    }
}

void TstPieceBar::singleCellVisibleAtAnyWidth()
{
    for (int n : {1, 3, 100, 512}) {
        for (int w = 1; w <= 2000; w += (w < 64 ? 1 : 37)) {
            for (int cell : {0, n / 2, n - 1}) {
                QByteArray st = cells(n, 2);
                st[cell] = 1;
                const QVector<Column> c = piecebar::columns(st, QByteArray(), w);
                QVERIFY2(countActive(c) >= 1, qPrintable(QStringLiteral("n=%1 w=%2 cell=%3").arg(n).arg(w).arg(cell)));
                // only the pixels that cover the cell: [cell*w/n, (cell+1)*w/n] rounded outwards
                const int lo = int(qint64(cell) * w / n), hi = int((qint64(cell + 1) * w + n - 1) / n) - 1;
                for (int x = 0; x < w; ++x)
                    if (c[x].active) QVERIFY2(x >= lo && x <= qMax(lo, hi), qPrintable(QStringLiteral("n=%1 w=%2 x=%3").arg(n).arg(w).arg(x)));
            }
        }
    }
}

void TstPieceBar::incrementalFallback()
{
    QVERIFY(piecebar::incremental(50, 0).isEmpty());
    const QVector<Column> z = piecebar::incremental(0, 68);
    for (const Column &c : z) QCOMPARE(c.share, 0);
    const QVector<Column> f = piecebar::incremental(100, 68);
    for (const Column &c : f) QCOMPARE(c.share, 255);
    const QVector<Column> h = piecebar::incremental(49, 68);
    int full = 0;
    for (int x = 0; x < h.size(); ++x) {
        if (h[x].share == 255) {
            ++full;
            QVERIFY(x < 34);   // filled from the left
        }
    }
    QCOMPARE(full, 33);       // (68 * 49 + 50) / 100, as the classic bar
}

void TstPieceBar::themeColors()
{
    SimpleSettings s;
    Colors::instance()->attach(&s);
    for (const char *theme : {"dark", "light"}) {
        s.setValue(QString(keys::ui::ColorTheme), QString::fromLatin1(theme));
        QTRY_COMPARE(Colors::instance()->theme(), QByteArray(theme) == "dark" ? ColorTheme::Dark : ColorTheme::Light);
        const piecebar::Colors c = piecebar::themeColors();
        if (QByteArray(theme) == "dark") {
            QCOMPARE(c.bar, QColor(QStringLiteral("#0080ff")));      // the classic blue for "done"
            QCOMPARE(c.background, QColor(Qt::black));
            QCOMPARE(c.active, QColor(QStringLiteral("#00ff00")));   // the transferring green of the original WinMX
        } else {
            QCOMPARE(c.bar, QColor(QStringLiteral("#0050b0")));
            QCOMPARE(c.background, QColor(Qt::white));
            QCOMPARE(c.active, QColor(QStringLiteral("#2ea02e")));
        }
        // "arriving now" differs from "done", "missing" and "partly there" by brightness, not only by hue
        const QColor partial = piecebar::partialColor(c);
        QVERIFY2(Colors::contrast(c.active, c.bar) >= 2.0, theme);
        QVERIFY2(Colors::contrast(c.active, c.background) >= 3.0, theme);
        QVERIFY2(Colors::contrast(c.active, partial) >= 1.8, theme);
        QVERIFY2(Colors::contrast(partial, c.background) >= 1.4, theme);
        QVERIFY2(Colors::contrast(partial, c.bar) >= 1.4, theme);
        // the text stays legible over every part
        QVERIFY2(Colors::contrast(piecebar::textOverActive(c), c.active) >= 4.5, theme);
        QVERIFY2(Colors::contrast(c.bar, c.background) >= 4.5, theme);
        QVERIFY2(Colors::contrast(c.bar, partial) >= 3.5, theme);
    }
    s.setValue(QString(keys::ui::ColorTheme), QStringLiteral("dark"));
    QTRY_COMPARE(Colors::instance()->theme(), ColorTheme::Dark);
    // the user's own colour for the parts arriving now (Settings, Appearance, Transfers Window)
    s.setValue(QString(keys::colors::ProgressActive), QStringLiteral("#ffa040"));
    QTRY_COMPARE(piecebar::themeColors().active, QColor(QStringLiteral("#ffa040")));
    Colors::instance()->attach(nullptr);
}

void TstPieceBar::paintsInsideCellAtEveryScale()
{
    for (double scale : {1.0, 1.25, 1.5, 2.0, 2.5, 3.0}) {
        setUiScale(scale);
        QStandardItemModel model(3, 3);
        QByteArray map = cells(512, 0), fill = cells(512, 0);
        for (int i = 0; i < 128; ++i) map[i] = 2;          // the first quarter there
        map[60] = 1;                                         // one cell arriving (left of the text)
        for (int i = 400; i < 512; i += 2) fill[i] = char(100);   // partly there at the end
        model.setData(model.index(0, 1), 38, ProgressRole);
        model.setData(model.index(0, 1), QStringLiteral("38%"), Qt::DisplayRole);
        model.setData(model.index(0, 1), map, PieceMapRole);
        model.setData(model.index(0, 1), fill, PieceFillRole);
        model.setData(model.index(1, 1), 50, ProgressRole);   // no map: the incremental fallback
        model.setData(model.index(1, 1), QStringLiteral("50%"), Qt::DisplayRole);
        MxListView view;
        view.setModel(&model);
        view.resize(S(500), S(140));
        view.header()->resizeSection(0, S(100));
        view.header()->resizeSection(1, S(250));
        view.header()->resizeSection(2, S(100));
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        const piecebar::Colors c = piecebar::themeColors();
        for (int round = 0; round < 2; ++round) {
            if (round == 1) {   // the bar follows the column
                view.header()->resizeSection(1, S(170));
                QCoreApplication::processEvents();
            }
            const QRect cell = view.visualRect(model.index(0, 1));
            const QImage img = view.viewport()->grab().toImage();
            const piecebar::Geometry g = piecebar::geometry(cell);
            const QString at = QStringLiteral("scale %1 round %2").arg(scale).arg(round);
            QVERIFY2(cell.contains(g.frame) && cell.contains(g.block), qPrintable(at));
            QCOMPARE(g.block.left(), cell.left());
            QCOMPARE(g.frame.right(), cell.right() - S(2));     // to the cell's normal right margin
            QCOMPARE(g.frame.top(), cell.top() + S(2));
            QCOMPARE(g.frame.bottom(), cell.bottom());
            // the outline row: bar colour from the block's left to the frame's right, nothing outside the cell
            const int yTop = g.frame.top();
            const auto top = spanOf(img, yTop, c.bar);
            QVERIFY2(top.first == cell.left() && top.second == g.frame.right(), qPrintable(at + QStringLiteral(" %1..%2").arg(top.first).arg(top.second)));
            // a row through the middle of the bar: the arriving cell is drawn, inside the frame
            const int yMid = g.inner.top() + 1;
            const auto act = spanOf(img, yMid, c.active);
            QVERIFY2(act.first > g.inner.left() && act.second < g.inner.right(),
                     qPrintable(at + QStringLiteral(" active %1..%2 inner %3..%4 cell %5..%6 img %7")
                                         .arg(act.first).arg(act.second).arg(g.inner.left()).arg(g.inner.right())
                                         .arg(cell.left()).arg(cell.right()).arg(img.width())));
            const int expect = g.inner.left() + int(qint64(60) * g.inner.width() / 512);
            QVERIFY2(qAbs(act.first - expect) <= 1, qPrintable(at + QStringLiteral(" active at %1, want %2").arg(act.first).arg(expect)));
            // the first quarter is there (bar colour right after the outline), the end partly there
            QCOMPARE(QColor(img.pixel(g.inner.left() + 1, g.inner.bottom())), c.bar);
            QCOMPARE(QColor(img.pixel(g.inner.right() - 1, g.inner.bottom())), piecebar::partialColor(c));
            // the fallback row fills from the left by the percentage
            const QRect cell2 = view.visualRect(model.index(1, 1));
            const piecebar::Geometry g2 = piecebar::geometry(cell2);
            QCOMPARE(QColor(img.pixel(g2.inner.left() + 1, g2.inner.bottom())), c.bar);
            QCOMPARE(QColor(img.pixel(g2.inner.right() - 1, g2.inner.bottom())), c.background);
            const int mid = g2.inner.left() + g2.inner.width() / 2;
            QCOMPARE(QColor(img.pixel(mid - 2, g2.inner.bottom())), c.bar);
            QCOMPARE(QColor(img.pixel(mid + 2, g2.inner.bottom())), c.background);
            // nothing of the bar leaks into the next column
            const QRect next = view.visualRect(model.index(0, 2));
            for (int x = next.left(); x <= next.right(); ++x)
                QVERIFY2(QColor(img.pixel(x, yTop)) != c.bar, qPrintable(at));
        }
    }
    setUiScale(1.0);
}

void TstPieceBar::tipLine()
{
    QByteArray st = cells(20, 0);
    st[2] = st[3] = 1;   // one run
    st[7] = 1;
    st[10] = st[11] = st[12] = 1;
    st[19] = 1;
    QCOMPARE(piecebar::activeRuns(st), 4);
    QCOMPARE(piecebar::activeRuns(cells(5, 2)), 0);
    QCOMPARE(piecebar::tipLine(432013312LL, 1170378588LL, st), QStringLiteral("412 MB of 1.09 GB (36 %), 4 parts downloading"));
    st.fill(0);
    st[5] = 1;
    QCOMPARE(piecebar::tipLine(0, 1000, st), fmt::size(0) + QStringLiteral(" of ") + fmt::size(1000) + QStringLiteral(" (0 %), 1 part downloading"));
    QCOMPARE(piecebar::tipLine(500, 1000, QByteArray()), fmt::size(500) + QStringLiteral(" of ") + fmt::size(1000) + QStringLiteral(" (50 %)"));
}

void TstPieceBar::modelRoles()
{
    TransferModel m(nullptr, false);
    QByteArray map = cells(8, 0);
    map[0] = 2;
    map[3] = 1;
    QByteArray fill = cells(8, 0);
    fill[0] = char(255);
    TransferInfo run = dl(QStringLiteral("run"), TransferState::Transferring, QString(), 400, 1000);
    run.pieceMap = map;
    run.pieceFill = fill;
    TransferInfo paused = dl(QStringLiteral("paused"), TransferState::Paused, QStringLiteral("Paused"), 400, 1000);
    paused.pieceMap = cells(8, 0);
    paused.pieceMap[0] = 2;
    TransferInfo fresh = dl(QStringLiteral("fresh"), TransferState::FindingSources, QStringLiteral("Finding sources..."), 0, 1000);
    fresh.pieceMap = cells(8, 0);
    TransferInfo err = dl(QStringLiteral("err"), TransferState::Error, QStringLiteral("Disk full"), 400, 1000);
    err.pieceMap = map;
    TransferInfo done = dl(QStringLiteral("done"), TransferState::Completed, QStringLiteral("Complete"), 1000, 1000);
    TransferInfo queued = dl(QStringLiteral("queued"), TransferState::Queued, QStringLiteral("Locally Queued"), 300, 1000);
    for (const TransferInfo &t : {run, paused, fresh, err, done, queued}) m.addTransfer(t);
    auto at = [&](int row, int role) { return m.index(row, DStatus).data(role); };
    // transferring: the map, the percentage in the classic colours
    QCOMPARE(at(0, ProgressRole).toInt(), 40);
    QCOMPARE(at(0, PieceMapRole).toByteArray(), map);
    QCOMPARE(at(0, PieceFillRole).toByteArray(), fill);
    QVERIFY(!at(0, BarTextColorRole).isValid());
    QCOMPARE(at(0, Qt::DisplayRole).toString(), QStringLiteral("40%"));
    QVERIFY(at(0, Qt::ToolTipRole).toString().startsWith(fmt::size(400) + QStringLiteral(" of ") + fmt::size(1000) + QStringLiteral(" (40 %), 1 part downloading")));
    // paused / queued with something: the bar keeps the map, the status text inside in its colour
    QVERIFY(at(1, ProgressRole).isValid());
    QCOMPARE(at(1, PieceMapRole).toByteArray(), paused.pieceMap);
    QCOMPARE(at(1, Qt::DisplayRole).toString(), QStringLiteral("Paused"));
    QCOMPARE(at(1, BarTextColorRole).value<QColor>(), at(1, Qt::ForegroundRole).value<QColor>());
    QVERIFY(at(1, Qt::ToolTipRole).toString().contains(QStringLiteral("(40 %)")));
    QVERIFY(at(5, ProgressRole).isValid());
    QCOMPARE(at(5, Qt::DisplayRole).toString(), QStringLiteral("Locally Queued"));
    // nothing yet, an error, a finished file: the text as before, no bar
    for (int row : {2, 3, 4}) {
        QVERIFY2(!at(row, ProgressRole).isValid(), qPrintable(QString::number(row)));
        QVERIFY(!at(row, PieceMapRole).isValid());
    }
    QVERIFY(at(3, OverflowRole).toBool());   // the red text still runs on over the empty cells
    // a running download without a map (size unknown, magnet without metadata): incremental bar
    TransferInfo nomap = dl(QStringLiteral("nomap"), TransferState::Transferring, QString(), 250, 1000);
    m.addTransfer(nomap);
    QCOMPARE(m.index(6, DStatus).data(ProgressRole).toInt(), 25);
    QVERIFY(m.index(6, DStatus).data(PieceMapRole).toByteArray().isEmpty());
    // uploads: incremental only
    TransferModel u(nullptr, true);
    TransferInfo up = dl(QStringLiteral("up"), TransferState::Transferring, QString(), 300, 1000);
    up.isUpload = true;
    up.pieceMap = map;
    u.addTransfer(up);
    QCOMPARE(u.index(0, UStatus).data(ProgressRole).toInt(), 30);
    QVERIFY(!u.index(0, UStatus).data(PieceMapRole).isValid());
}

int main(int argc, char **argv)
{
    preApplicationSetup();
    QApplication app(argc, argv);
    installClassicLook(&app);
    fmt::setGroupSeparator(QLatin1Char(' '));
    TstPieceBar t;
    return QTest::qExec(&t, argc, argv);
}

#include "tst_piece_bar.moc"
