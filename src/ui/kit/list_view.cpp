// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/kit/list_view.h"

#include <QtCore/QMetaMethod>

#include <functional>

#include <QtCore/QAbstractProxyModel>
#include <QtCore/QTimer>
#include <QtGui/QKeyEvent>
#include <QtGui/QPainter>
#include <QtGui/QShortcut>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QToolTip>
#include <QtGui/QHelpEvent>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QMenu>
#include <QtWidgets/QStyle>
#include <QtWidgets/QScrollBar>

#include "mx/services.h"
#include "mx/settings_keys.h"
#include "ui/kit/colors.h"
#include "ui/kit/draw.h"
#include "ui/kit/format.h"
#include "ui/kit/piece_bar.h"
#include "ui/kit/scale.h"
#include "ui/kit/text_filter.h"
#include "ui/kit/widgets.h"
#include "ui/kit/theme.h"
#include "ui/models/roles.h"

namespace mx::ui {

// ------------------------------------------------------------------------------- MxItemDelegate

MxItemDelegate::MxItemDelegate(MxListView *view) : QAbstractItemDelegate(view), m_view(view) {}

QSize MxItemDelegate::sizeHint(const QStyleOptionViewItem &opt, const QModelIndex &idx) const
{
    const QString t = idx.data(Qt::DisplayRole).toString();
    return QSize(opt.fontMetrics.horizontalAdvance(t) + S(8), m_view->rowHeight());
}

QRect MxItemDelegate::expanderRect(const QRect &cell, int boxX)
{
    // [ui-e] the 9×9 box scales to an odd size so the +/- stays centred; boxX is in design pixels
    int b = S(9);
    if (!(b & 1))
        ++b;
    const int top = cell.top() + (cell.height() - b) / 2 + S(2);
    return QRect(cell.left() + S(boxX), top, b, b);
}

// dotted lines: one dot every 2 lines (1 px at 100 %; t×t dots on a 2t grid when scaled)
static void dottedH(QPainter *p, int x0, int x1, int y, const QColor &c)
{
    const int t = uiLine();
    for (int x = x0; x <= x1; x += 2 * t)
        p->fillRect(QRect(x, y, t, t), c);
}

static void dottedV(QPainter *p, int x, int y0, int y1, const QColor &c)
{
    const int t = uiLine();
    for (int y = y0; y <= y1; ++y)
        if (((y / t) & 1) == 0)
            p->fillRect(QRect(x, y, t, 1), c);
}

void MxItemDelegate::paintTree(QPainter *p, const QRect &cell, const QModelIndex &idx) const
{
    const QAbstractItemModel *m = idx.model();
    const QModelIndex first = idx.sibling(idx.row(), 0);
    const QColor lineC = color(ckeys::ListInactive);
    const QColor boxC = color(keys::colors::ListText);   // [post-1] white on the dark lists, black on the light ones
    const QRect box = expanderRect(cell, m_view->treeBoxX());
    const int t = uiLine();
    // centre lines of width t: the box is odd-sized, so a t-wide line is centred for odd t
    const int cx = box.left() + (box.width() - t) / 2;
    const int cy = box.top() + (box.height() - t) / 2;
    const int textX = cell.left() + S(m_view->treeTextMargin());
    const bool lines = m_view->treeConnectors();
    if (!first.parent().isValid()) {
        if (m->hasChildren(first)) {
            draw::edge(p, box, boxC, boxC);
            const int arm = box.width() - 4 * t;   // 5 at 100 %
            p->fillRect(QRect(box.left() + 2 * t, cy, arm, t), boxC);
            if (!m_view->isExpanded(first))
                p->fillRect(QRect(cx, box.top() + 2 * t, t, arm), boxC);
            // dotted connector to the text
            if (lines)
                dottedH(p, box.right() + 2 * t, textX - S(3), cy, boxC);
            if (lines && m_view->isExpanded(first) && m->rowCount(first) > 0)
                dottedV(p, cx, box.bottom() + 2 * t, cell.bottom(), lineC);
        }
    } else if (lines) {
        // child: vertical line from the parent's box, horizontal dots to the text
        const bool last = first.row() == m->rowCount(first.parent()) - 1;
        dottedV(p, cx, cell.top(), last ? cy : cell.bottom(), lineC);
        dottedH(p, cx + 2 * t, textX - S(3), cy, lineC);
    }
}

void MxItemDelegate::paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &idx) const
{
    // the row background is painted once by MxListView::drawRow (so overflowing text of a previous
    // cell is not erased here)
    QRect r = opt.rect;

    // section header row (Hotlist): centred label with rule lines
    if (idx.data(SectionHeaderRole).toBool()) {
        if (idx.column() != 0)
            return;
        QColor c = idx.data(SectionColorRole).value<QColor>();
        if (!c.isValid())
            c = color(ckeys::HotlistHeader);
        const QString t = idx.data(Qt::DisplayRole).toString();
        const QRect full(0, r.top(), m_view->viewport()->width(), r.height());
        const int tw = opt.fontMetrics.horizontalAdvance(t);
        const int tx = full.left() + (full.width() - tw) / 2;
        const int ly = full.top() + full.height() / 2;
        const int g = S(4), lw = uiLine();
        p->fillRect(QRect(full.left() + g, ly, qMax(0, tx - g - full.left() - g), lw), c);
        p->fillRect(QRect(tx + tw + g, ly, qMax(0, full.right() - g - (tx + tw + g)), lw), c);
        p->setPen(c);
        draw::text(p, QRect(tx, full.top(), tw + 1, full.height()), Qt::AlignLeft, t);
        return;
    }

    int textLeft = r.left() + S(2);
    if (idx.column() == 0 && m_view->treeMode()) {
        paintTree(p, r, idx);
        textLeft = r.left() + S(m_view->treeTextMargin());
    }
    // icon
    const QVariant deco = idx.data(Qt::DecorationRole);
    if (deco.isValid()) {
        QPixmap pm;
        if (deco.canConvert<QPixmap>())
            pm = deco.value<QPixmap>();
        if (!pm.isNull()) {
            p->drawPixmap(textLeft, r.top() + (r.height() - pm.height()) / 2, pm);
            textLeft += pm.width() + S(m_view->iconGap());
        }
    }
    const QRect trBase(textLeft, r.top(), r.right() - S(2) - textLeft + 1, r.height());
    if (trBase.width() <= 0)
        return;

    QFont f = opt.font;
    const QVariant fv = idx.data(Qt::FontRole);
    if (fv.isValid())
        f = fv.value<QFont>();
    p->setFont(f);
    const QFontMetrics fm(f);

    QColor fg = idx.data(Qt::ForegroundRole).value<QColor>();
    if (!fg.isValid())
        fg = color(keys::colors::ListText);
    // [ui-h] colors/listSelectedText: the plain text of a selected row (state colours stay, as in the original WinMX)
    if ((opt.state & QStyle::State_Selected) && fg == color(keys::colors::ListText))
        fg = color(keys::colors::ListSelectedText);
    const QString text = idx.data(Qt::DisplayRole).toString();
    const QVariant al = idx.data(Qt::TextAlignmentRole);
    const Qt::Alignment align = al.isValid() ? Qt::Alignment(al.toInt()) : Qt::Alignment(Qt::AlignLeft);
    const int hAlign = int(align & Qt::AlignHorizontal_Mask);
    // as in the original WinMX: numbers end 5 px before the column edge (measured), text starts 2 px after it
    QRect tr = trBase;
    if (hAlign & Qt::AlignRight)
        tr.setRight(r.right() - S(4));
    const bool clip = idx.data(ClipRole).toBool();

    // progress bar (kit/piece_bar.h): the classic WinMX cell, a 6-px block, a 2-px gap, then a framed
    // bar from row top + 2 to the row bottom, [ui-i] over the whole cell; with a map of the file it
    // shows which parts are there and which are arriving now, else it fills by the percentage
    const QVariant prog = idx.data(ProgressRole);
    if (prog.isValid() && prog.toInt() >= 0) {
        const int pct = qBound(0, prog.toInt(), 100);
        piecebar::Colors pc = piecebar::themeColors();
        const QColor tc = idx.data(BarTextColorRole).value<QColor>();
        if (tc.isValid())
            pc.text = tc;
        const piecebar::Geometry g = piecebar::geometry(r);
        const QByteArray map = idx.data(PieceMapRole).toByteArray();
        const QVector<piecebar::Column> cols = map.isEmpty()
            ? piecebar::incremental(pct, g.inner.width())
            : piecebar::columns(map, idx.data(PieceFillRole).toByteArray(), g.inner.width());
        piecebar::paint(p, r, cols, text.isEmpty() ? QStringLiteral("%1%").arg(pct) : text, pc);
        return;
    }

    // multi-colour segments
    const QVariant segv = idx.data(SegmentsRole);
    if (segv.isValid()) {
        const QList<Segment> segs = segmentsFrom(segv);
        int total = 0;
        for (const Segment &s : segs)
            total += fm.horizontalAdvance(s.text);
        int x = tr.left();
        if (hAlign & Qt::AlignRight)
            x = qMax(tr.left(), tr.right() + 1 - total);
        else if (hAlign & Qt::AlignHCenter)
            x = tr.left() + qMax(0, (tr.width() - total) / 2);
        p->save();
        p->setClipRect(tr);
        int remaining = tr.right() + 1 - x;
        for (const Segment &s : segs) {
            if (remaining <= 0)
                break;
            const int w = fm.horizontalAdvance(s.text);
            QString t = s.text;
            if (w > remaining && !clip)
                t = fmt::elide(s.text, fm, remaining);
            p->setPen(s.color.isValid() ? s.color : fg);
            draw::text(p, QRect(x, tr.top(), w + S(2), tr.height()), Qt::AlignLeft, t);
            x += w;
            remaining -= w;
        }
        p->restore();
        return;
    }

    if (text.isEmpty())
        return;
    if (idx.data(OverflowRole).toBool() && !(hAlign & Qt::AlignRight)) {
        // run on over the following cells while they are empty
        int right = r.right();
        const QAbstractItemModel *m = idx.model();
        const QHeaderView *hv = m_view->header();
        for (int v = hv->visualIndex(idx.column()) + 1; v < hv->count(); ++v) {
            const int li = hv->logicalIndex(v);
            if (hv->isSectionHidden(li))
                continue;
            if (!m->index(idx.row(), li, idx.parent()).data(Qt::DisplayRole).toString().isEmpty() ||
                m->index(idx.row(), li, idx.parent()).data(ProgressRole).isValid())
                break;
            right = hv->sectionViewportPosition(li) + hv->sectionSize(li) - 1;
        }
        const QRect wide(tr.left(), tr.top(), right - S(2) - tr.left() + 1, tr.height());
        p->setPen(fg);
        draw::text(p, wide, Qt::AlignLeft, fmt::elide(text, fm, wide.width()));
        return;
    }
    const QString shown = clip ? text : fmt::elide(text, fm, tr.width());
    p->setPen(fg);
    if (clip) {
        p->save();
        p->setClipRect(QRect(r.left(), r.top(), r.width() - 1, r.height()));
        draw::text(p, QRect(tr.left(), tr.top(), qMax(tr.width(), fm.horizontalAdvance(text) + S(2)), tr.height()),
                   hAlign ? hAlign : Qt::AlignLeft, shown);
        p->restore();
        return;
    }
    draw::text(p, tr, hAlign ? hAlign : Qt::AlignLeft, shown);
}

// ---------------------------------------------------------------------------------- MxHeaderView

// [ui-g] a resize by mouse (drag or double click on a border) is absorbed by the columns to the right
// (MxListView::absorbResize()); when the drag ends the layout is checked once more
class MxHeaderView : public QHeaderView {
public:
    explicit MxHeaderView(QWidget *parent) : QHeaderView(Qt::Horizontal, parent) {}

protected:
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        QHeaderView::mouseReleaseEvent(e);
        // a drag ends: make sure the columns still end at the edge (rounding of many small steps)
        if (auto *v = qobject_cast<MxListView *>(parentWidget()))
            v->scheduleLayout();
    }
};

// ---------------------------------------------------------------------------------- MxListView

MxListView::MxListView(QWidget *parent) : QTreeView(parent)
{
    setHeader(new MxHeaderView(this));
    setItemDelegate(new MxItemDelegate(this));
    setUniformRowHeights(true);
    setIndentation(0);
    setRootIsDecorated(false);
    setAllColumnsShowFocus(true);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setExpandsOnDoubleClick(false);
    setItemsExpandable(true);
    setAnimated(false);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);   // [ui-g] the columns always fit the width
    setVerticalScrollMode(QAbstractItemView::ScrollPerItem);
    setAutoScroll(false);
    setFrameStyle(QFrame::StyledPanel | QFrame::Sunken);
    setLineWidth(2 * uiLine());
    setFont(dynamicFont());
    m_rowHeight = S(m_baseRowHeight) + qMax(0, QFontMetrics(font()).height() - S(13));
    setMouseTracking(false);
    setAttribute(Qt::WA_MacShowFocusRect, false);
    setFocusPolicy(Qt::StrongFocus);

    QPalette pal = palette();
    pal.setColor(QPalette::Base, color(keys::colors::ListBackground));
    pal.setColor(QPalette::Window, color(keys::colors::ListBackground));
    pal.setColor(QPalette::Text, color(keys::colors::ListText));
    setPalette(pal);
    viewport()->setAutoFillBackground(true);

    QHeaderView *h = header();
    h->setSectionsMovable(true);
    h->setSectionsClickable(true);
    h->setHighlightSections(false);
    h->setStretchLastSection(false);
    h->setMinimumSectionSize(S(kMinColumn));   // [ui-g] a dragged column keeps this much
    h->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    h->setFont(normalFont());
    h->setFixedHeight(headerHeightFor(normalFont()));
    h->setSortIndicatorShown(true);
    h->setTextElideMode(Qt::ElideNone);   // the style elides with "..." itself
    h->setSortIndicator(-1, Qt::AscendingOrder);
    setSortingEnabled(true);

    // [ui-h] header text in colors/listHeaderText (Settings > Appearance > Main Window)
    {
        QPalette hp = h->palette();
        hp.setColor(QPalette::ButtonText, color(keys::colors::ListHeaderText));
        h->setPalette(hp);
    }
    connect(Colors::instance(), &Colors::changed, this, [this] {
        QPalette p = palette();
        p.setColor(QPalette::Base, color(m_backgroundKey));
        p.setColor(QPalette::Window, color(m_backgroundKey));
        p.setColor(QPalette::Text, color(keys::colors::ListText));   // [post-1] the theme switches it too
        setPalette(p);
        QPalette hp = header()->palette();
        hp.setColor(QPalette::ButtonText, color(keys::colors::ListHeaderText));
        header()->setPalette(hp);
        header()->viewport()->update();
        viewport()->update();
    });

    // Settings ▸ Appearance fonts: rows use the dynamic font, the header the normal one; row and
    // header heights grow with the font (the 11-px default keeps the measured 14 / 17 px)
    connect(Fonts::instance(), &Fonts::changed, this, [this] {
        m_minTextWidth.clear();   // [ui-h]
        setFont(dynamicFont());
        header()->setFont(normalFont());
        header()->setFixedHeight(headerHeightFor(normalFont()));
        updateRowHeight();
    });

    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(600);
    connect(m_saveTimer, &QTimer::timeout, this, &MxListView::saveState);
    m_layoutTimer = new QTimer(this);
    m_layoutTimer->setSingleShot(true);
    m_layoutTimer->setInterval(0);
    connect(m_layoutTimer, &QTimer::timeout, this, &MxListView::autoFill);
    // [ui-e] a column resized by hand: the user's layout from now on, kept as fractions of the width.
    // [ui-g] the columns to its right absorb the change, so the columns keep ending at the edge
    connect(h, &QHeaderView::sectionResized, this, [this](int logical, int oldSize, int newSize) {
        if (m_filling || m_restoring)
            return;
        if (oldSize == 0 || newSize == 0 || header()->isSectionHidden(logical)) {
            scheduleLayout();   // a column hidden or shown by code: lay the others out again
            return;
        }
        absorbResize(logical, oldSize, newSize);
        m_autoFill = false;
        captureFractions();
        scheduleSave();
    });
    connect(h, &QHeaderView::sectionCountChanged, this, &MxListView::scheduleLayout);
    connect(h, &QHeaderView::sectionMoved, this, [this] {
        if (m_filling || m_restoring)
            return;
        m_layoutTouched = true;
        captureFractions();
        scheduleSave();
    });
    connect(h, &QHeaderView::sortIndicatorChanged, this, [this] {
        if (!m_restoring)
            scheduleSave();
    });
    // [ui-e] header right-click: column chooser + Reset Columns
    h->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(h, &QHeaderView::customContextMenuRequested, this, [this](const QPoint &pos) {
        // [ui-g] a scroll area reports this position in its viewport's coordinates (as the rows' menus)
        showColumnMenu(header()->viewport()->mapToGlobal(pos));
    });
}

int MxListView::headerHeightFor(const QFont &f)
{
    return S(17) + qMax(0, QFontMetrics(f).height() - S(13));
}

void MxListView::setRowHeight(int h)
{
    m_baseRowHeight = h;
    updateRowHeight();
}

void MxListView::updateRowHeight()
{
    // the base height belongs to the 13-px line of Tahoma 8 pt; a bigger font adds its extra pixels
    m_rowHeight = S(m_baseRowHeight) + qMax(0, QFontMetrics(font()).height() - S(13));
    doItemsLayout();
    viewport()->update();
}

void MxListView::setTreeMode(bool on)
{
    m_treeMode = on;
    viewport()->update();
}

void MxListView::setTreeGeometry(int boxX, int textMargin, bool connectors, int iconGap)
{
    m_treeBoxX = boxX;
    m_treeMargin = textMargin;
    m_treeConnectors = connectors;
    m_iconGap = iconGap;
    viewport()->update();
}

void MxListView::setFillColumns(const QList<int> &columns)
{
    m_fillColumns = columns;
    autoFill();
}

void MxListView::setFillWeights(const QList<int> &weights)
{
    m_fillWeights = weights;
    autoFill();
}

int MxListView::fillWeight(int column) const
{
    const int k = int(m_fillColumns.indexOf(column));
    return k >= 0 && k < m_fillWeights.size() ? m_fillWeights.at(k) : m_defaultWidths.value(column);
}

void MxListView::setFillReserve(int px)
{
    // [ui-g] no effect any more: a reserved strip right of the columns is exactly the gap the owner
    // reported; the columns now follow the viewport, also when the vertical scroll bar comes and goes
    Q_UNUSED(px)
    autoFill();
}

int MxListView::availableWidth() const
{
    return viewport()->width();
}

int MxListView::minimumColumnWidth(int column) const
{
    const int d = m_defaultWidths.value(column, 0);
    const int base = d > 0 ? qMin(S(kMinColumn), d) : S(kMinColumn);
    const auto it = m_minTexts.constFind(column);
    if (it == m_minTexts.constEnd())
        return base;
    // [ui-h] the widest text the column shows, with the delegate's padding (2 px before the text,
    // 2 px after it, 2 more for the right-aligned numbers) and the header label in the header font
    auto cached = m_minTextWidth.constFind(column);
    if (cached == m_minTextWidth.constEnd()) {
        const QFontMetrics fm(font());
        int w = 0;
        for (const QString &t : it.value())
            w = qMax(w, fm.horizontalAdvance(t));
        const QString head = model() ? model()->headerData(column, Qt::Horizontal, Qt::DisplayRole).toString() : QString();
        const int hw = head.isEmpty() ? 0 : QFontMetrics(header()->font()).horizontalAdvance(head) + S(8);
        cached = m_minTextWidth.insert(column, qMax(w + S(6), hw));
    }
    return qMax(base, cached.value());
}

void MxListView::setMinimumColumnTexts(int column, const QStringList &texts)
{
    m_minTexts.insert(column, texts);
    m_minTextWidth.clear();
    scheduleLayout();
}

int MxListView::minimumListWidth() const
{
    int w = 0;
    for (int c : visibleColumns())
        w += minimumColumnWidth(c);
    const int scroll = verticalScrollBar() ? style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, this) : 0;
    return w + 2 * frameWidth() + scroll;
}

void MxListView::setBackgroundKey(const QString &key)
{
    m_backgroundKey = key;
    QPalette p = palette();
    p.setColor(QPalette::Base, color(m_backgroundKey));
    p.setColor(QPalette::Window, color(m_backgroundKey));
    setPalette(p);
    viewport()->update();
}

QList<int> MxListView::visibleColumns() const
{
    QList<int> out;
    const QHeaderView *h = header();
    for (int v = 0; v < h->count(); ++v) {
        const int li = h->logicalIndex(v);
        if (li >= 0 && !h->isSectionHidden(li))
            out << li;
    }
    return out;
}

int MxListView::defaultWidth(int column) const
{
    const int d = m_defaultWidths.value(column, 0);
    return d > 0 ? d : S(80);
}

bool MxListView::columnsFillViewport() const
{
    const QList<int> cols = visibleColumns();
    if (cols.isEmpty() || !model())
        return true;
    int sum = 0;
    for (int c : cols)
        sum += header()->sectionSize(c);
    return sum == availableWidth() && header()->offset() == 0;
}

void MxListView::scheduleLayout()
{
    if (m_layoutTimer)
        m_layoutTimer->start();
}

namespace {
// desired widths -> whole pixels that add up to exactly `avail`; every column keeps its minimum
// (a list too narrow for the minimums shrinks them alike)
QList<int> fitWidths(QList<double> want, QList<int> mins, int avail)
{
    const int n = int(want.size());
    QList<int> out(n, 0);
    if (n == 0 || avail <= 0)
        return out;
    int minSum = 0;
    for (int m : std::as_const(mins))
        minSum += m;
    if (minSum > avail) {
        for (int &m : mins)
            m = qMax(1, int(qint64(m) * avail / qMax(1, minSum)));
    }
    double wantSum = 0;
    for (double &w : want) {
        w = qMax(0.0, w);
        wantSum += w;
    }
    if (wantSum <= 0)
        for (double &w : want)
            w = 1.0;
    QList<double> w(n, 0);
    QList<bool> fixed(n, false);
    for (int round = 0; round <= n; ++round) {
        double freeW = avail, freeWant = 0;
        for (int i = 0; i < n; ++i) {
            if (fixed[i])
                freeW -= mins[i];
            else
                freeWant += want[i];
        }
        bool changed = false;
        for (int i = 0; i < n; ++i) {
            if (fixed[i])
                continue;
            w[i] = freeWant > 0 ? want[i] * freeW / freeWant : freeW / n;
            if (w[i] < mins[i]) {
                fixed[i] = true;
                w[i] = mins[i];
                changed = true;
            }
        }
        if (!changed)
            break;
    }
    double cum = 0;
    int x = 0;
    for (int i = 0; i < n; ++i) {
        cum += w[i];
        const int right = i == n - 1 ? avail : qMin(avail, qMax(x, int(qRound(cum))));
        out[i] = right - x;
        x = right;
    }
    return out;
}
} // namespace

void MxListView::autoFill()
{
    if (!model() || header()->count() == 0)
        return;
    QHeaderView *h = header();
    const int avail = availableWidth();
    if (avail <= 0)
        return;
    if (h->stretchLastSection()) {
        // [ui-g] Qt's stretched last section overflows a narrow list; the last column fills instead
        m_stretchLast = true;
        m_filling = true;
        h->setStretchLastSection(false);
        m_filling = false;
    }
    const QList<int> cols = visibleColumns();
    if (cols.isEmpty())
        return;
    // lists without default widths: the widths they have now are their defaults
    if (m_defaultWidths.size() < h->count()) {
        for (int i = int(m_defaultWidths.size()); i < h->count(); ++i)
            m_defaultWidths << (h->isSectionHidden(i) || h->sectionSize(i) <= 0 ? S(80) : h->sectionSize(i));
    }
    QList<double> want;
    if (!m_autoFill) {
        // [ui-e] the user's layout scales with the list: every visible column keeps its fraction.
        // [ui-g] the fractions of the visible columns are normalised to the whole width: a layout
        // stored with a gap or an overflow (an earlier layout, an older scale) fills the list again
        if (!m_pendingAbsolute.isEmpty()) {   // a layout stored as pixels (older versions): fractions now
            for (auto it = m_pendingAbsolute.constBegin(); it != m_pendingAbsolute.constEnd(); ++it)
                m_fractions.insert(it.key(), double(it.value()) / avail);
            m_pendingAbsolute.clear();
        }
        double sum = 0;
        for (int c : cols) {
            double f = m_fractions.value(c, -1.0);
            if (f <= 0)
                f = double(defaultWidth(c)) / avail;   // a column shown later: its default share
            want << f;
            sum += f;
        }
        if (sum > 0)
            for (int k = 0; k < cols.size(); ++k)
                m_fractions.insert(cols[k], want[k] / sum);
    } else {
        // untouched list: the default widths; the fill columns (File) take the slack of a wider list,
        // a narrower one shrinks the fill columns first (to 40 % of their default), then every column
        QList<int> fill;
        for (int c : m_fillColumns)
            if (cols.contains(c))
                fill << c;
        if (fill.isEmpty())
            fill << (m_stretchLast ? cols.last() : cols.first());
        int sum = 0, fillSum = 0;
        for (int c : cols)
            sum += defaultWidth(c);
        for (int c : std::as_const(fill))
            fillSum += qMax(1, fillWeight(c));
        const int extra = avail - sum;
        for (int c : cols) {
            double w = defaultWidth(c);
            if (fill.contains(c)) {
                if (extra >= 0) {
                    w += double(extra) * qMax(1, fillWeight(c)) / fillSum;
                } else {
                    int fillAvail = 0;
                    for (int f : std::as_const(fill))
                        fillAvail += defaultWidth(f) * 6 / 10;
                    if (fillAvail > 0)
                        w -= double(qMin(-extra, fillAvail)) * (defaultWidth(c) * 6 / 10) / fillAvail;
                }
            }
            want << w;
        }
    }
    QList<int> mins;
    for (int c : cols)
        mins << minimumColumnWidth(c);
    const QList<int> w = fitWidths(want, mins, avail);
    m_filling = true;
    for (int k = 0; k < cols.size(); ++k)
        if (h->sectionSize(cols[k]) != w[k])
            h->resizeSection(cols[k], w[k]);
    m_filling = false;
    if (h->offset() != 0)
        h->setOffset(0);
    if (qEnvironmentVariableIsSet("MX_LIST_DEBUG")) {
        QStringList l;
        for (int i = 0; i < h->count(); ++i)
            l << QString::number(h->sectionSize(i));
        qInfo("autoFill %s avail=%d: %s", qPrintable(objectName()), avail, qPrintable(l.join(QLatin1Char(','))));
    }
}

void MxListView::absorbResize(int column, int oldSize, int newSize)
{
    // [ui-g] the user dragged the right border of `column` (or code resized it): the columns to its
    // right give or take the difference in proportion; the last border moves the last two columns
    QHeaderView *h = header();
    const QList<int> cols = visibleColumns();
    const int k = int(cols.indexOf(column));
    if (k < 0)
        return;
    QList<int> others = k < cols.size() - 1 ? cols.mid(k + 1) : (k > 0 ? QList<int>{cols[k - 1]} : QList<int>{});
    int delta = newSize - oldSize;
    m_filling = true;
    if (others.isEmpty()) {   // a single column: it is the whole width
        h->resizeSection(column, availableWidth());
        m_filling = false;
        return;
    }
    // what is wrong already (should not happen) is repaired with the same move
    int sum = 0;
    for (int c : cols)
        sum += h->sectionSize(c);
    const int target = availableWidth();
    int give = (sum - target);   // > 0: the others must give this much
    if (give > 0) {
        int room = 0;
        for (int c : std::as_const(others))
            room += qMax(0, h->sectionSize(c) - minimumColumnWidth(c));
        if (give > room) {   // they cannot: the dragged column stops
            const int stop = qMax(minimumColumnWidth(column), newSize - (give - room));
            give -= newSize - stop;
            h->resizeSection(column, stop);
            delta = stop - oldSize;
        }
        // take `give` from the others in proportion to what each can give
        int roomLeft = 0;
        for (int c : std::as_const(others))
            roomLeft += qMax(0, h->sectionSize(c) - minimumColumnWidth(c));
        int taken = 0;
        for (int i = 0; i < others.size() && roomLeft > 0; ++i) {
            const int c = others[i];
            const int can = qMax(0, h->sectionSize(c) - minimumColumnWidth(c));
            const int t = i == others.size() - 1 ? qMin(can, give - taken) : int(qint64(give) * can / roomLeft);
            if (t > 0)
                h->resizeSection(c, h->sectionSize(c) - t);
            taken += t;
        }
    } else if (give < 0) {
        // the others take the free width in proportion to their widths
        int total = 0;
        for (int c : std::as_const(others))
            total += h->sectionSize(c);
        const int add = -give;
        int given = 0;
        for (int i = 0; i < others.size(); ++i) {
            const int c = others[i];
            const int a = i == others.size() - 1 ? add - given : (total > 0 ? int(qint64(add) * h->sectionSize(c) / total) : add / int(others.size()));
            if (a > 0)
                h->resizeSection(c, h->sectionSize(c) + a);
            given += a;
        }
    }
    Q_UNUSED(delta)
    m_filling = false;
}

void MxListView::captureFractions()
{
    const int avail = availableWidth();
    if (avail <= 0)
        return;
    m_pendingAbsolute.clear();
    // [ui-g] only the visible columns: a hidden one keeps the share it had (for when it comes back)
    QHeaderView *h = header();
    for (int c : visibleColumns())
        m_fractions.insert(c, double(h->sectionSize(c)) / avail);
}

void MxListView::setDefaultColumnWidths(const QList<int> &designWidths)
{
    // [ui-e] widths are given in design pixels
    QList<int> widths;
    for (int w : designWidths)
        widths << S(w);
    m_defaultWidths = widths;
    m_filling = true;   // programmatic: neither a user layout nor something to save
    for (int i = 0; i < widths.size() && i < header()->count(); ++i)
        header()->resizeSection(i, widths.at(i));
    m_filling = false;
    autoFill();
}

void MxListView::setDefaultHiddenColumns(const QList<int> &columns)
{
    m_defaultHidden = columns;
    m_filling = true;
    if (!m_layoutTouched && model())
        for (int i = 0; i < header()->count(); ++i)
            header()->setSectionHidden(i, m_defaultHidden.contains(i));
    m_filling = false;
    autoFill();
}

void MxListView::setColumnVisible(int column, bool visible)
{
    if (column <= 0 || column >= header()->count() || header()->isSectionHidden(column) == !visible)
        return;
    QHeaderView *h = header();
    if (!m_autoFill) {
        // the user's layout: a shown column takes its default share from the others, a hidden one
        // gives its share to them (autoFill normalises the fractions of the visible columns)
        if (m_fractions.isEmpty())
            captureFractions();
        if (visible && availableWidth() > 0) {
            double total = 0;
            for (int c : visibleColumns())
                total += m_fractions.value(c);
            m_fractions[column] = total * double(defaultWidth(column)) / availableWidth();
        }
    }
    m_filling = true;
    h->setSectionHidden(column, !visible);
    m_filling = false;
    m_layoutTouched = true;
    autoFill();
    scheduleSave();
}

void MxListView::resetColumns()
{
    QHeaderView *h = header();
    m_restoring = true;
    for (int target = 0; target < h->count(); ++target) {
        const int from = h->visualIndex(target);
        if (from != target)
            h->moveSection(from, target);
    }
    for (int i = 0; i < h->count(); ++i)
        h->setSectionHidden(i, m_defaultHidden.contains(i));
    m_restoring = false;
    m_autoFill = true;
    m_layoutTouched = false;
    m_fractions.clear();
    m_pendingAbsolute.clear();
    m_filling = true;
    for (int i = 0; i < m_defaultWidths.size() && i < h->count(); ++i)
        h->resizeSection(i, m_defaultWidths.at(i));
    m_filling = false;
    autoFill();
    if (m_settings && !m_stateName.isEmpty())
        m_settings->setValue(keys::ui::columnsKey(m_stateName), QVariant());
}

QMenu *MxListView::columnMenu()
{
    auto *menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("columnMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    QHeaderView *h = header();
    const QAbstractItemModel *m = model();
    for (int v = 0; m && v < h->count(); ++v) {
        const int li = h->logicalIndex(v);
        const QString title = m->headerData(li, Qt::Horizontal, Qt::DisplayRole).toString();
        if (title.isEmpty())
            continue;
        QAction *a = menu->addAction(title);
        a->setCheckable(true);
        a->setChecked(!h->isSectionHidden(li));
        a->setEnabled(li != 0);   // the first column (File / Name) always stays
        connect(a, &QAction::toggled, this, [this, li](bool on) { setColumnVisible(li, on); });
    }
    menu->addSeparator();
    QAction *reset = menu->addAction(tr("Reset Columns"));
    connect(reset, &QAction::triggered, this, &MxListView::resetColumns);
    return menu;
}

void MxListView::showColumnMenu(const QPoint &globalPos)
{
    columnMenu()->popup(globalPos);
}

void MxListView::setStateKey(mx::Settings *settings, const QString &name)
{
    m_settings = settings;
    m_stateName = name;
    restoreState();
}

void MxListView::scheduleSave()
{
    if (m_filling || m_restoring)
        return;   // our own fill is not a layout to keep
    if (m_settings && !m_stateName.isEmpty())
        m_saveTimer->start();
}

void MxListView::saveState()
{
    if (!m_settings || m_stateName.isEmpty() || !model())
        return;
    QHeaderView *h = header();
    QVariantList widths, order, hidden, fractions;
    for (int i = 0; i < h->count(); ++i) {
        widths << h->sectionSize(i);
        order << h->visualIndex(i);
        if (h->isSectionHidden(i))
            hidden << i;
        fractions << m_fractions.value(i, -1.0);
    }
    // an untouched list keeps no layout: its defaults (and the first-run fill) apply next time too
    if (!m_autoFill || m_layoutTouched) {
        QVariantMap m;
        m.insert(QStringLiteral("order"), order);
        m.insert(QStringLiteral("hidden"), hidden);
        if (!m_autoFill) {
            m.insert(QStringLiteral("widths"), widths);
            m.insert(QStringLiteral("fractions"), fractions);   // [ui-e] the layout scales with the list
        } else {
            m.insert(QStringLiteral("auto"), true);
        }
        m_settings->setValue(keys::ui::columnsKey(m_stateName), m);
    }
    QVariantMap s;
    s.insert(QStringLiteral("column"), h->sortIndicatorSection());
    s.insert(QStringLiteral("order"), int(h->sortIndicatorOrder()));
    m_settings->setValue(keys::ui::sortKey(m_stateName), s);
}

void MxListView::restoreState()
{
    if (!model())
        return;
    m_restoring = true;
    QHeaderView *h = header();
    for (int i = 0; i < m_defaultWidths.size() && i < h->count(); ++i)
        h->resizeSection(i, m_defaultWidths.at(i));
    for (int i = 0; i < h->count(); ++i)
        h->setSectionHidden(i, m_defaultHidden.contains(i));
    if (m_settings && !m_stateName.isEmpty()) {
        const QVariantMap m = m_settings->value(keys::ui::columnsKey(m_stateName)).toMap();
        const QVariantList widths = m.value(QStringLiteral("widths")).toList();
        const QVariantList order = m.value(QStringLiteral("order")).toList();
        const QVariantList hidden = m.value(QStringLiteral("hidden")).toList();
        const QVariantList fractions = m.value(QStringLiteral("fractions")).toList();
        if (widths.size() == h->count() && !m.value(QStringLiteral("auto")).toBool()) {
            for (int i = 0; i < widths.size(); ++i)
                h->resizeSection(i, qMax(S(8), widths.at(i).toInt()));
            m_autoFill = false;   // a stored layout wins over the first-run fill
            m_fractions.clear();
            m_pendingAbsolute.clear();
            if (fractions.size() == h->count() && fractions.first().toDouble() >= 0) {
                for (int i = 0; i < fractions.size(); ++i)
                    m_fractions.insert(i, fractions.at(i).toDouble());
            } else {
                for (int i = 0; i < widths.size(); ++i)
                    m_pendingAbsolute.insert(i, qMax(S(8), widths.at(i).toInt()));
            }
        }
        if (order.size() == h->count()) {
            m_layoutTouched = true;
            // move sections so that logical i ends up at visual order[i]
            for (int target = 0; target < h->count(); ++target) {
                for (int logical = 0; logical < order.size(); ++logical) {
                    if (order.at(logical).toInt() == target) {
                        const int from = h->visualIndex(logical);
                        if (from != target)
                            h->moveSection(from, target);
                        break;
                    }
                }
            }
            for (int i = 0; i < h->count(); ++i)
                h->setSectionHidden(i, false);
            for (const QVariant &v : hidden)
                if (v.toInt() > 0 && v.toInt() < h->count())
                    h->setSectionHidden(v.toInt(), true);
        }
        const QVariantMap s = m_settings->value(keys::ui::sortKey(m_stateName)).toMap();
        if (s.contains(QStringLiteral("column"))) {
            const int col = s.value(QStringLiteral("column")).toInt();
            const auto ord = Qt::SortOrder(s.value(QStringLiteral("order")).toInt());
            if (col >= 0 && col < h->count())
                sortByColumn(col, ord);
        }
    }
    m_restoring = false;
    autoFill();
}

QModelIndex MxListView::mapToSource(const QModelIndex &viewIndex) const
{
    QModelIndex i = viewIndex;
    const QAbstractItemModel *m = model();
    while (auto *proxy = qobject_cast<const QAbstractProxyModel *>(m)) {
        i = proxy->mapToSource(i);
        m = proxy->sourceModel();
    }
    return i;
}

QModelIndex MxListView::mapFromSource(const QModelIndex &sourceIndex) const
{
    QList<const QAbstractProxyModel *> chain;
    const QAbstractItemModel *m = model();
    while (auto *proxy = qobject_cast<const QAbstractProxyModel *>(m)) {
        chain.prepend(proxy);
        m = proxy->sourceModel();
    }
    QModelIndex i = sourceIndex;
    for (const QAbstractProxyModel *p : chain)
        i = p->mapFromSource(i);
    return i;
}

QModelIndex MxListView::currentSourceIndex() const
{
    return mapToSource(currentIndex());
}

QModelIndexList MxListView::selectedSourceRows() const
{
    QModelIndexList out;
    if (!selectionModel())
        return out;
    const QModelIndexList rows = selectionModel()->selectedRows(0);
    for (const QModelIndex &i : rows)
        out << mapToSource(i);
    return out;
}

void MxListView::selectRow(const QModelIndex &viewIndex)
{
    if (!viewIndex.isValid())
        return;
    setCurrentIndex(viewIndex);
    selectionModel()->select(viewIndex, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    scrollTo(viewIndex);
}

QModelIndex MxListView::visibleRow(int n) const
{
    if (!model() || n < 0)
        return QModelIndex();
    QModelIndex i = model()->index(0, 0, rootIndex());
    while (i.isValid() && n > 0) {
        i = indexBelow(i);
        --n;
    }
    return i;
}

int MxListView::visibleRowCount() const
{
    if (!model())
        return 0;
    int n = 0;
    for (QModelIndex i = model()->index(0, 0, rootIndex()); i.isValid(); i = indexBelow(i))
        ++n;
    return n;
}

void MxListView::resizeEvent(QResizeEvent *e)
{
    QTreeView::resizeEvent(e);
    layoutFilterBar();
    autoFill();
}

// ------------------------------------------------------------------------- [ui-e] type-to-filter

void MxListView::ensureFilterSetup()
{
    if (m_filterSetup)
        return;
    m_filterSetup = true;
    m_filterTimer = new QTimer(this);
    m_filterTimer->setSingleShot(true);
    m_filterTimer->setInterval(150);
    connect(m_filterTimer, &QTimer::timeout, this, &MxListView::applyFilter);
    // follow the model while filtering (new search results arriving, rows merged, sorting ...)
    auto refilter = [this] {
        if (!m_filterTimer->isActive())
            m_filterTimer->start();   // also the count of an empty filter follows the rows
    };
    if (model()) {
        connect(model(), &QAbstractItemModel::rowsInserted, this, refilter);
        connect(model(), &QAbstractItemModel::rowsRemoved, this, refilter);
        connect(model(), &QAbstractItemModel::modelReset, this, refilter);
        connect(model(), &QAbstractItemModel::layoutChanged, this, refilter);
        connect(model(), &QAbstractItemModel::dataChanged, this, refilter);
    }
}

void MxListView::setFilterEdit(QLineEdit *edit)
{
    ensureFilterSetup();
    m_externalFilter = true;
    m_filterEnabled = true;
    m_filterEdit = edit;
    connect(edit, &QLineEdit::textChanged, this, [this](const QString &t) {
        m_filterText = t.trimmed();
        m_filterTimer->start();
    });
    auto *esc = new QShortcut(QKeySequence(Qt::Key_Escape), edit, nullptr, nullptr, Qt::WidgetShortcut);
    connect(esc, &QShortcut::activated, edit, &QLineEdit::clear);
    // Enter in the field goes to the list, so the arrows and Enter (download) work at once
    connect(edit, &QLineEdit::returnPressed, this, [this] {
        setFocus();
        if (!currentIndex().isValid() || isFilteredOut(currentIndex()))
            for (QModelIndex i = model() ? model()->index(0, 0, rootIndex()) : QModelIndex(); i.isValid(); i = indexBelow(i))
                if (!isFilteredOut(i)) {
                    selectRow(i);
                    break;
                }
    });
    applyFilter();
}

bool MxListView::filterPending() const
{
    return m_filterTimer && m_filterTimer->isActive();
}

bool MxListView::isFilteredOut(const QModelIndex &viewIndex) const
{
    for (QModelIndex i = viewIndex.sibling(viewIndex.row(), 0); i.isValid(); i = i.parent())
        if (isRowHidden(i.row(), i.parent()))
            return true;
    return false;
}

void MxListView::showFilter()
{
    if (m_findHandler) {
        m_findHandler();
        return;
    }
    if (m_externalFilter && m_filterEdit) {
        m_filterEdit->setFocus(Qt::ShortcutFocusReason);
        m_filterEdit->selectAll();
        return;
    }
    if (!m_filterBar) {
        ensureFilterSetup();
        m_filterBar = new QWidget(this);
        m_filterBar->setObjectName(objectName() + QStringLiteral("Filter"));
        m_filterBar->setAutoFillBackground(true);
        m_filterBar->setPalette(classicPalette());   // not the black list colours
        auto *label = new StaticText(tr("Filter:"), m_filterBar);
        label->setObjectName(QStringLiteral("filterLabel"));
        m_filterEdit = new QLineEdit(m_filterBar);
        m_filterEdit->setObjectName(objectName() + QStringLiteral("FilterText"));
        m_filterEdit->setToolTip(tr("Shows only the rows that contain every word typed here (-word hides rows with that word, "
                                    "\"two words\" finds a phrase). Esc closes the filter."));   // [qa-2]
        m_filterInfo = new StaticText(QString(), m_filterBar);
        auto *close = new PushButton(tr("Close"), m_filterBar);
        close->setObjectName(QStringLiteral("filterClose"));
        close->setToolTip(tr("Close the filter and show every row again"));
        connect(close, &QPushButton::clicked, this, &MxListView::hideFilter);
        connect(m_filterEdit, &QLineEdit::textChanged, this, [this](const QString &t) {
            m_filterText = t.trimmed();
            m_filterTimer->start();
        });
        auto *esc = new QShortcut(QKeySequence(Qt::Key_Escape), m_filterEdit, nullptr, nullptr, Qt::WidgetShortcut);
        connect(esc, &QShortcut::activated, this, &MxListView::hideFilter);
        m_filterBar->hide();
    }
    m_filterBar->show();
    layoutFilterBar();
    m_filterEdit->setFocus();
    m_filterEdit->selectAll();
}

void MxListView::hideFilter()
{
    if (m_externalFilter) {
        if (m_filterEdit)
            m_filterEdit->clear();
        m_filterText.clear();
        applyFilter();
        return;
    }
    if (!m_filterBar)
        return;
    m_filterEdit->clear();
    m_filterText.clear();
    applyFilter();
    m_filterBar->hide();
    setViewportMargins(0, 0, 0, 0);
    setFocus();
}

void MxListView::setFilterText(const QString &text)
{
    if (!m_externalFilter)
        showFilter();
    if (m_filterEdit)
        m_filterEdit->setText(text);
    m_filterText = text.trimmed();
    applyFilter();
}

void MxListView::layoutFilterBar()
{
    if (!m_filterBar || m_filterBar->isHidden())
        return;
    const int h = S(25), fw = frameWidth();
    setViewportMargins(0, 0, 0, h);
    m_filterBar->setGeometry(fw, height() - fw - h, width() - 2 * fw, h);
    const QList<QWidget *> kids = m_filterBar->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly);
    // Filter: [edit] <n of m>        [Close]
    for (QWidget *k : kids) {
        if (k->objectName() == QLatin1String("filterLabel"))
            k->setGeometry(S(4), S(2), S(36), S(21));
        else if (k == m_filterEdit)
            k->setGeometry(S(42), S(2), S(220), S(21));
        else if (k == m_filterInfo)
            k->setGeometry(S(270), S(2), qMax(0, m_filterBar->width() - S(270) - S(70)), S(21));
        else if (k->objectName() == QLatin1String("filterClose"))
            k->setGeometry(m_filterBar->width() - S(64), S(2), S(60), S(21));
    }
}

void MxListView::applyFilter()
{
    QAbstractItemModel *m = model();
    if (!m)
        return;
    const TextFilter filter(m_filterText);
    if (filter.isEmpty() && !m_rowsFiltered) {   // nothing to hide or to show again: just count
        m_filterMatches = m_filterTotal = m->rowCount(rootIndex());
        if (m_filterInfo)
            m_filterInfo->setText(QString());
        emit filterApplied(m_filterMatches, m_filterTotal);
        return;
    }
    m_rowsFiltered = !filter.isEmpty();
    const int cols = m->columnCount();
    QList<int> look = m_filterColumns;
    if (look.isEmpty())
        for (int c = 0; c < cols; ++c)
            look << c;
    auto matches = [&](const QModelIndex &idx) {
        if (filter.isEmpty())
            return true;
        QString text;
        for (int c : std::as_const(look))
            if (c < cols)
                text += idx.sibling(idx.row(), c).data(Qt::DisplayRole).toString() + QLatin1Char('\t');
        return filter.matches(text);
    };
    int shown = 0, total = 0;
    // returns true when the row (or one of its children) is visible
    std::function<bool(const QModelIndex &, bool)> walk = [&](const QModelIndex &parent, bool parentMatched) {
        bool any = false;
        const int n = m->rowCount(parent);
        for (int r = 0; r < n; ++r) {
            const QModelIndex idx = m->index(r, 0, parent);
            const bool self = parentMatched || matches(idx);
            bool kid = false;
            if (m->hasChildren(idx))
                kid = walk(idx, self);
            const bool vis = self || kid;
            if (!parent.isValid()) {
                ++total;
                if (vis)
                    ++shown;
            }
            if (isRowHidden(r, parent) == vis)
                setRowHidden(r, parent, !vis);
            any = any || vis;
        }
        return any;
    };
    walk(rootIndex(), false);
    m_filterMatches = shown;
    m_filterTotal = total;
    // [ui-g] what the filter hides is not selected: Download, Cancel ... act only on what is shown
    if (!filter.isEmpty() && selectionModel() && selectionModel()->hasSelection()) {
        QItemSelection drop;
        for (const QModelIndex &i : selectionModel()->selectedRows(0))
            if (isFilteredOut(i))
                drop.select(i, i.sibling(i.row(), m->columnCount(i.parent()) - 1));
        if (!drop.isEmpty())
            selectionModel()->select(drop, QItemSelectionModel::Deselect);
    }
    if (m_filterInfo)
        m_filterInfo->setText(filter.isEmpty() ? QString() : tr("%1 of %2 shown").arg(fmt::number(shown), fmt::number(total)));
    emit filterApplied(shown, total);
}

bool MxListView::viewportEvent(QEvent *e)
{
    if (e->type() == QEvent::ToolTip) {
        auto *he = static_cast<QHelpEvent *>(e);
        const QModelIndex idx = indexAt(he->pos());
        if (idx.isValid()) {
            const QString tip = idx.data(Qt::ToolTipRole).toString();
            const QString text = idx.data(Qt::DisplayRole).toString();
            // the full text when the cell shows it elided ("...")
            bool elided = false;
            if (!text.isEmpty() && !idx.data(ProgressRole).isValid()) {
                const QRect r = visualRect(idx);
                int avail = r.width() - S(4);
                if (idx.column() == 0 && m_treeMode)
                    avail -= S(m_treeMargin);
                if (idx.data(Qt::DecorationRole).isValid())
                    avail -= S(16) + S(m_iconGap);
                elided = QFontMetrics(font()).horizontalAdvance(text) > avail;
            }
            QString show = tip;
            if (elided && !tip.contains(text))
                show = tip.isEmpty() ? text : text + QLatin1Char('\n') + tip;
            if (!show.isEmpty()) {
                QToolTip::showText(he->globalPos(), show, viewport(), visualRect(idx));
                return true;
            }
        }
        QToolTip::hideText();
        e->ignore();
        return true;
    }
    return QTreeView::viewportEvent(e);
}

QModelIndex MxListView::rowIndexAt(const QPoint &viewportPos) const
{
    const QModelIndex idx = indexAt(viewportPos);
    if (idx.isValid() || !model() || !viewport()->rect().contains(viewportPos))
        return idx;
    // beside the columns: the same height inside the first visible column
    const QHeaderView *h = header();
    for (int v = 0; v < h->count(); ++v) {
        const int logical = h->logicalIndex(v);
        if (h->isSectionHidden(logical) || h->sectionSize(logical) <= 0)
            continue;
        return indexAt(QPoint(h->sectionViewportPosition(logical) + qMin(2, h->sectionSize(logical) - 1), viewportPos.y()));
    }
    return QModelIndex();
}

void MxListView::mousePressEvent(QMouseEvent *e)
{
    const QPoint pos = e->position().toPoint();
    QModelIndex idx = indexAt(pos);
    if (!idx.isValid()) {
        // a click beside the columns still means the row at that height
        const QModelIndex row = rowIndexAt(pos);
        if (row.isValid() && e->button() == Qt::LeftButton) {
            const QPoint inRow(visualRect(row).center().x(), pos.y());
            QMouseEvent moved(e->type(), inRow, viewport()->mapToGlobal(inRow), e->button(), e->buttons(), e->modifiers());
            QTreeView::mousePressEvent(&moved);
            return;
        }
        idx = row;
    }
    if (m_treeMode && idx.isValid() && idx.column() == 0 && e->button() == Qt::LeftButton) {
        const QModelIndex first = idx.sibling(idx.row(), 0);
        if (!first.parent().isValid() && model()->hasChildren(first)) {
            const QRect box = MxItemDelegate::expanderRect(visualRect(idx), m_treeBoxX).adjusted(-S(2), -S(2), S(2), S(2));
            if (box.contains(pos)) {
                setExpanded(first, !isExpanded(first));
                return;
            }
        }
    }
    if (e->button() == Qt::RightButton && idx.isValid()) {
        // Windows selects the row under the mouse on right click (unless it is already selected).
        // [ui-g] Without scrolling: selectRow() scrolled a partly visible row (the last one, often at a
        // larger scale) fully into view, the rows moved under the mouse, and the context menu that
        // follows at the same screen point then belonged to the row below the highlighted one.
        const QModelIndex first = idx.sibling(idx.row(), 0);
        if (!selectionModel()->isRowSelected(idx.row(), idx.parent())) {
            setCurrentIndex(first);
            selectionModel()->select(first, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        } else {
            setCurrentIndex(idx);
        }
        m_rightPressRow = QPersistentModelIndex(first);
        m_rightPressTimer.start();
        return;
    }
    QTreeView::mousePressEvent(e);
}

void MxListView::mouseDoubleClickEvent(QMouseEvent *e)
{
    const QModelIndex idx = rowIndexAt(e->position().toPoint());
    if (e->button() == Qt::LeftButton && idx.isValid()) {
        emit rowActivated(idx.sibling(idx.row(), 0));
        return;
    }
    QTreeView::mouseDoubleClickEvent(e);
}

void MxListView::keyPressEvent(QKeyEvent *e)
{
    if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && currentIndex().isValid()) {
        emit rowActivated(currentIndex().sibling(currentIndex().row(), 0));
        return;
    }
    if (e->key() == Qt::Key_Menu && currentIndex().isValid()) {
        const QRect r = visualRect(currentIndex());
        emit contextMenuAt(currentIndex(), viewport()->mapToGlobal(r.bottomLeft()));
        return;
    }
    if (e->key() == Qt::Key_Delete && e->modifiers() == Qt::NoModifier && selectionModel() &&
        selectionModel()->hasSelection() && isSignalConnected(QMetaMethod::fromSignal(&MxListView::deleteRequested))) {
        emit deleteRequested();
        return;
    }
    QTreeView::keyPressEvent(e);
}

void MxListView::contextMenuEvent(QContextMenuEvent *e)
{
    // A mouse context menu event reaches a scroll area through its viewport, so e->pos() is in
    // viewport coordinates already. Mapping it from the view again moved it up by the header
    // height: the menu then acted on the row above the one under the mouse.
    if (e->reason() == QContextMenuEvent::Keyboard && currentIndex().isValid()) {
        const QRect r = visualRect(currentIndex());
        emit contextMenuAt(currentIndex().sibling(currentIndex().row(), 0), viewport()->mapToGlobal(r.bottomLeft()));
        return;
    }
    // [ui-g] the menu of a right click belongs to the row that was pressed (and highlighted), even if the
    // rows moved in between (new rows arriving, a scroll)
    QModelIndex idx;
    if (m_rightPressRow.isValid() && m_rightPressTimer.isValid() && m_rightPressTimer.elapsed() < 3000)
        idx = m_rightPressRow;
    else
        idx = rowIndexAt(viewport()->mapFromGlobal(e->globalPos()));
    m_rightPressRow = QPersistentModelIndex();
    m_rightPressTimer.invalidate();
    emit contextMenuAt(idx.isValid() ? idx.sibling(idx.row(), 0) : QModelIndex(), e->globalPos());
}

void MxListView::drawRow(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &idx) const
{
    const bool sel = selectionModel() && selectionModel()->isSelected(idx.sibling(idx.row(), 0));
    p->fillRect(opt.rect, sel ? color(keys::colors::ListSelectedBackground) : color(m_backgroundKey));
    QTreeView::drawRow(p, opt, idx);
    // [ui-h] Settings > Interface and Logs "Show grid lines in lists": cell borders in colors/listGrid
    if (Colors::instance()->showGrid()) {
        const QColor g = color(keys::colors::ListGrid);
        const QHeaderView *h = header();
        const int line = uiLine();
        p->fillRect(QRect(opt.rect.left(), opt.rect.bottom() - line + 1, opt.rect.width(), line), g);
        for (int v = 0; v < h->count(); ++v) {
            const int li = h->logicalIndex(v);
            if (li < 0 || h->isSectionHidden(li))
                continue;
            const int x = h->sectionViewportPosition(li) + h->sectionSize(li) - line;
            if (x >= opt.rect.left() && x <= opt.rect.right())
                p->fillRect(QRect(x, opt.rect.top(), line, opt.rect.height()), g);
        }
    }
}

void MxListView::paintEvent(QPaintEvent *e)
{
    QTreeView::paintEvent(e);
}

} // namespace mx::ui
