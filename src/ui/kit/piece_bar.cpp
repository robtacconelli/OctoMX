// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/kit/piece_bar.h"

#include <QtCore/QCoreApplication>
#include <QtGui/QFontMetrics>
#include <QtGui/QPainter>
#include <QtGui/QRegion>

#include "mx/settings_keys.h"
#include "ui/kit/colors.h"
#include "ui/kit/format.h"
#include "ui/kit/scale.h"

namespace mx::ui::piecebar {

QVector<Column> columns(const QByteArray &state, const QByteArray &fill, int width)
{
    const int n = int(state.size());
    if (n <= 0 || width < 1)
        return {};
    const bool haveFill = fill.size() == n;
    QVector<Column> out(width);
    // pixel x covers cells [x*n/width, (x+1)*n/width): in units of 1/width cell that is [x*n, (x+1)*n),
    // and cell i is [i*width, (i+1)*width)
    const qint64 W = width, N = n;
    for (int x = 0; x < width; ++x) {
        const qint64 s = x * N, e = (x + 1) * N;
        const int first = int(s / W), last = int(qMin<qint64>(N - 1, (e - 1) / W));
        qint64 done = 0;   // verified share in units of 1/(width*255) cell
        bool all = true, active = false;
        for (int i = first; i <= last; ++i) {
            const qint64 ov = qMin(e, (i + 1) * W) - qMax(s, i * W);
            if (ov <= 0)
                continue;
            const char st = state.at(i);
            // a cell that is not complete never counts as all there, whatever its fill says
            const int f = st == 2 ? 255 : haveFill ? qMin(254, int(quint8(fill.at(i)))) : 0;
            if (st != 2)
                all = false;
            if (st == 1)
                active = true;
            done += ov * f;
        }
        Column &c = out[x];
        c.active = active;
        if (all)
            c.share = 255;
        else if (done > 0)
            c.share = int(qBound<qint64>(qint64(1), (done + N / 2) / N, qint64(254)));
    }
    return out;
}

QVector<Column> incremental(int percent, int width)
{
    if (width < 1)
        return {};
    const int pct = qBound(0, percent, 100);
    const int full = (width * pct + 50) / 100;
    QVector<Column> out(width);
    for (int x = 0; x < full; ++x)
        out[x].share = 255;
    return out;
}

Colors themeColors()
{
    Colors c;
    c.bar = color(keys::colors::ProgressBar);
    c.background = color(keys::colors::ProgressBackground);
    c.active = color(keys::colors::ProgressActive);
    c.text = c.bar;
    return c;
}

QColor partialColor(const Colors &c)
{
    auto mix = [](int bg, int bar) { return (bg * 2 + bar + 1) / 3; };
    return QColor(mix(c.background.red(), c.bar.red()), mix(c.background.green(), c.bar.green()),
                  mix(c.background.blue(), c.bar.blue()));
}

QColor textOverActive(const Colors &c)
{
    const double bg = mx::ui::Colors::contrast(c.background, c.active);
    const double bar = mx::ui::Colors::contrast(c.bar, c.active);
    if (qMax(bg, bar) >= 4.5)
        return bg >= bar ? c.background : c.bar;
    const QColor black(0, 0, 0), white(255, 255, 255);
    return mx::ui::Colors::contrast(black, c.active) >= mx::ui::Colors::contrast(white, c.active) ? black : white;
}

Geometry geometry(const QRect &cell)
{
    // measured on web/transfers_page_354b3.png: a 6-px block at the cell's left edge, a 2-px gap, the
    // bar from row top + 2 to the row bottom; [ui-i] the bar now runs to the cell's right text margin
    Geometry g;
    const int top = cell.top() + S(2), h = cell.bottom() - top + 1;
    if (h <= 0 || cell.width() <= 0)
        return g;
    const int right = cell.right() - S(2);
    const int t = uiLine();
    int left = cell.left();
    if (right - left + 1 >= S(8) + S(12)) {   // room for the block and a bar of a few pixels
        g.block = QRect(left, top, S(6), h);
        left += S(8);
    }
    g.frame = QRect(left, top, qMax(0, right - left + 1), h);
    if (g.frame.width() <= 0)
        g.frame = QRect();
    g.inner = g.frame.adjusted(t, t, -t, -t);
    if (g.inner.width() <= 0 || g.inner.height() <= 0)
        g.inner = QRect();
    return g;
}

void paint(QPainter *p, const QRect &cell, const QVector<Column> &cols, const QString &text, const Colors &c)
{
    const Geometry g = geometry(cell);
    if (g.frame.isEmpty())
        return;
    if (!g.block.isEmpty())
        p->fillRect(g.block, c.bar);
    p->fillRect(g.frame, c.background);
    // the outline: one chrome line in the bar colour (as the classic bar)
    const int t = uiLine();
    p->fillRect(QRect(g.frame.left(), g.frame.top(), g.frame.width(), t), c.bar);
    p->fillRect(QRect(g.frame.left(), g.frame.bottom() - t + 1, g.frame.width(), t), c.bar);
    p->fillRect(QRect(g.frame.left(), g.frame.top(), t, g.frame.height()), c.bar);
    p->fillRect(QRect(g.frame.right() - t + 1, g.frame.top(), t, g.frame.height()), c.bar);

    // the columns, merged into runs of equal colour; the regions remember what lies under the text
    QRegion doneR, activeR;
    const QRect in = g.inner;
    if (!in.isEmpty() && cols.size() == in.width()) {
        const QColor partial = partialColor(c);
        auto kind = [](const Column &col) { return col.active ? 3 : col.share >= 255 ? 2 : col.share > 0 ? 1 : 0; };
        for (int x = 0; x < cols.size();) {
            const int k = kind(cols.at(x));
            int y = x + 1;
            while (y < cols.size() && kind(cols.at(y)) == k)
                ++y;
            const QRect a(in.left() + x, in.top(), y - x, in.height());
            if (k == 3) {
                p->fillRect(a, c.active);
                activeR += a;
            } else if (k == 2) {
                p->fillRect(a, c.bar);
                doneR += a;
            } else if (k == 1) {
                p->fillRect(a, partial);   // the text keeps the bar colour there (as over the empty part)
            }
            x = y;
        }
    }
    if (text.isEmpty())
        return;
    // the text, centred over the bar as the classic one: the text colour over the missing and the
    // partly there parts, the background colour over the verified parts and the outline, a legible
    // colour over the parts arriving now
    const QFontMetrics fm(p->font());
    const QString shown = fmt::elide(text, fm, qMax(0, g.frame.width() - 2 * t - S(2)));
    if (shown.isEmpty())
        return;
    const QRect textRect(g.frame.left(), cell.top(), g.frame.width(), cell.height());
    const QRegion filled = doneR + (QRegion(g.frame) - QRegion(in));
    const QRegion empty = QRegion(textRect) - filled - activeR;
    p->save();
    p->setClipRegion(empty);
    p->setPen(c.text.isValid() ? c.text : c.bar);
    p->drawText(textRect, Qt::AlignCenter, shown);
    p->setClipRegion(filled);
    p->setPen(c.background);
    p->drawText(textRect, Qt::AlignCenter, shown);
    if (!activeR.isEmpty()) {
        p->setClipRegion(activeR);
        p->setPen(textOverActive(c));
        p->drawText(textRect, Qt::AlignCenter, shown);
    }
    p->restore();
}

int activeRuns(const QByteArray &state)
{
    int runs = 0;
    bool in = false;
    for (char st : state) {
        const bool a = st == 1;
        if (a && !in)
            ++runs;
        in = a;
    }
    return runs;
}

QString tipLine(qint64 done, qint64 size, const QByteArray &state)
{
    auto tr = [](const char *s) { return QCoreApplication::translate("PieceBar", s); };
    QString line;
    if (size > 0)
        line = tr("%1 of %2 (%3 %)").arg(fmt::size(done), fmt::size(size)).arg(fmt::percentInt(done, size));
    else
        line = fmt::size(done);
    const int runs = activeRuns(state);
    if (runs == 1)
        line += tr(", 1 part downloading");
    else if (runs > 1)
        line += tr(", %1 parts downloading").arg(runs);
    return line;
}

} // namespace mx::ui::piecebar
