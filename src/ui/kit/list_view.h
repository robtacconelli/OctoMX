// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: the black classic WinMX list view (UI_SPEC §1.6), used by every data list.
//
// * QTreeView based (model/view: only visible rows are painted, 10 000+ rows stay fast).
// * Black background, per-cell colours from the model (Qt::ForegroundRole, ItemRole::SegmentsRole),
//   14-px rows (16 with icons / progress bars: setRowHeight), no grid, no focus rectangle.
// * Selection = full-row #808080 background, the text keeps its own colours.
// * Tree decoration drawn like the original WinMX: a 9×9 [+]/[-] box and dotted connector lines in a fixed
//   margin at the left of column 0; children are NOT indented (setTreeMode(true)).
// * Right-aligned numbers, "..." elision, in-cell progress bars (ItemRole::ProgressRole).
// * Classic header: resizable, movable (reorder) sections, click to sort, persisted through
//   mx::Settings ("ui/columns/<name>", "ui/sort/<name>") with setStateKey().
// * Signals rowActivated (double-click / Enter) and contextMenuAt (right click, after selecting
//   the row under the mouse as Windows does).
#pragma once

#include <QtWidgets/QAbstractItemDelegate>
#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QPersistentModelIndex>

#include <functional>
#include <QtWidgets/QTreeView>

class QMenu;
class QLineEdit;

class QTimer;

namespace mx {
class Settings;
}

namespace mx::ui {

class MxListView;
class StaticText;

class MxItemDelegate : public QAbstractItemDelegate {
    Q_OBJECT
public:
    explicit MxItemDelegate(MxListView *view);
    void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &idx) const override;
    QSize sizeHint(const QStyleOptionViewItem &opt, const QModelIndex &idx) const override;

    // geometry of the tree decoration inside a column-0 cell (default: Search / Transfers layout)
    static QRect expanderRect(const QRect &cell, int boxX = 3);
    static constexpr int kTreeMargin = 21;   // text starts this far right in tree mode

private:
    void paintTree(QPainter *p, const QRect &cell, const QModelIndex &idx) const;
    MxListView *m_view;
};

class MxListView : public QTreeView {
    Q_OBJECT
public:
    explicit MxListView(QWidget *parent = nullptr);
    static constexpr int kMinColumn = 20;     // [ui-g] design px a column keeps when others are dragged

    void setRowHeight(int h);                 // base height at the default 11-px font (14, 16 for icons)
    int rowHeight() const { return m_rowHeight; }   // effective height (grows with the dynamic font)
    static int headerHeightFor(const QFont &f);
    // tree mode: draw [+] boxes / dotted lines in column 0 and reserve the margin
    void setTreeMode(bool on);
    bool treeMode() const { return m_treeMode; }
    // tree decoration geometry: [+] box x in the cell, text margin, dotted connectors, icon gap.
    // Default 3 / 21 / true / 3 (Search, Transfers); the 3.53 Shared Files list uses 5 / 20 / false / 2.
    void setTreeGeometry(int boxX, int textMargin, bool connectors, int iconGap = 3);
    int treeBoxX() const { return m_treeBoxX; }
    int treeTextMargin() const { return m_treeMargin; }
    bool treeConnectors() const { return m_treeConnectors; }
    int iconGap() const { return m_iconGap; }
    // Columns that grow (in proportion to their default widths) so that, until the user resizes a
    // column or a stored layout exists, the defaults fill a wider list (as the original WinMX's first-run widths).
    //
    // [ui-g] Invariant of every MxListView: the visible columns end exactly at the right edge of the
    // viewport (no empty strip, no horizontal scroll bar) at every size and scale. Untouched lists
    // give the slack to the fill columns; a user layout keeps its fractions of the width. A border
    // dragged by the user (or a section resized by code) changes the columns to its right in
    // proportion (the last border: the last two columns), each keeping minimumColumnWidth(); when
    // they cannot give more the drag stops. Hide / show / reorder / Reset Columns, stored layouts,
    // window resize, splitters and the vertical scroll bar all keep the invariant.
    void setFillColumns(const QList<int> &columns);
    // [qa-1] kept for source compatibility; [ui-g] has no effect (the columns always fill the viewport)
    void setFillReserve(int px);
    // [ui-g] the smallest width a column keeps when another column is dragged (design 20 px, or the
    // column's default when that is smaller)
    int minimumColumnWidth(int column) const;
    // [ui-g] true when the visible columns end exactly at the viewport's right edge (tests)
    bool columnsFillViewport() const;
    // [ui-g] the column layout now (normally done by itself on every change)
    void relayoutColumns() { autoFill(); }
    // [qa-1] share of the extra width per fill column (same order as setFillColumns; default = widths)
    void setFillWeights(const QList<int> &weights);
    // default column widths in design pixels (used until the user resizes; then the persisted ones win)
    void setDefaultColumnWidths(const QList<int> &widths);
    // [ui-h] the smallest width of a column from what it shows: the widest of `texts` in the row font
    // (plus the cell padding), kept up to date when the fonts change. Wins over kMinColumn, also
    // when the list is too narrow (the list's minimum width grows instead: see minimumListWidth()).
    void setMinimumColumnTexts(int column, const QStringList &texts);
    // the width the visible columns need at least (their minimum widths + frame + scroll bar)
    int minimumListWidth() const;
    // [ui-h] the background colour key of this list (default colors/listBackground; the chat user
    // list uses colors/chatUserListBackground)
    void setBackgroundKey(const QString &key);
    QString backgroundKey() const { return m_backgroundKey; }
    // [ui-e] columns hidden until the user shows them (header right-click ▸ column chooser)
    void setDefaultHiddenColumns(const QList<int> &columns);
    void setColumnVisible(int column, bool visible);
    // [ui-e] back to the default columns, order and widths (header menu "Reset Columns")
    void resetColumns();
    // [ui-e] the header menu: one checkable entry per column + "Reset Columns" (objectName "columnMenu")
    QMenu *columnMenu();
    void showColumnMenu(const QPoint &globalPos);
    bool hasUserLayout() const { return !m_autoFill; }

    // [ui-e] type-to-filter: a "Filter:" box at the bottom of the list (Ctrl+F); rows whose text
    // contains the words stay (a parent stays when a child matches). Esc / Close ends it.
    void setFilterEnabled(bool on) { m_filterEnabled = on; }
    bool filterEnabled() const { return m_filterEnabled; }
    void showFilter();
    void hideFilter();
    void setFilterText(const QString &text);
    QString filterText() const { return m_filterText; }
    int filterMatches() const { return m_filterMatches; }
    int filterTotal() const { return m_filterTotal; }
    bool filterPending() const;   // [ui-g] typed text not applied yet (tests)
    // [ui-g] an always visible filter field outside the list (FilterRow, kit/text_filter.h) instead of
    // the Ctrl+F bar: the list filters as it is typed, Esc clears it, showFilter() focuses it.
    // Rules: TextFilter (all words, -word excludes, "a phrase", any case)
    void setFilterEdit(QLineEdit *edit);
    QLineEdit *filterEdit() const { return m_filterEdit; }
    bool hasExternalFilter() const { return m_externalFilter; }
    // [ui-g] the columns the filter looks at (default: every column)
    void setFilterColumns(const QList<int> &columns) { m_filterColumns = columns; }
    // [ui-g] Ctrl+F on a list whose filter lives elsewhere (the channel list's classic Filter well)
    void setFindHandler(std::function<void()> handler) { m_findHandler = std::move(handler); }
    // [ui-g] the row (or its parent) is hidden by the filter
    bool isFilteredOut(const QModelIndex &viewIndex) const;
    int availableWidth() const;
    // persistence of widths / order / hidden columns / sort ("ui/columns/<name>", "ui/sort/<name>")
    void setStateKey(mx::Settings *settings, const QString &name);
    QString stateKey() const { return m_stateName; }
    void saveState();
    void restoreState();

    // the source-model index of the current row (maps through proxies)
    QModelIndex currentSourceIndex() const;
    QModelIndexList selectedSourceRows() const;   // column-0 indexes in the source model
    QModelIndex mapToSource(const QModelIndex &viewIndex) const;
    QModelIndex mapFromSource(const QModelIndex &sourceIndex) const;

    // select + scroll to a row (script driver, tests)
    void selectRow(const QModelIndex &viewIndex);
    // view index of the n-th visible row (counting expanded children), invalid if out of range
    QModelIndex visibleRow(int n) const;
    // The row at a viewport position, anywhere across the full width of the list (also right of
    // the last column, where indexAt() finds nothing): rows are selected and drawn full width.
    QModelIndex rowIndexAt(const QPoint &viewportPos) const;
    int visibleRowCount() const;

signals:
    void rowActivated(const QModelIndex &viewIndex);
    void contextMenuAt(const QModelIndex &viewIndex, const QPoint &globalPos);
    // [qa-1] Del pressed with a selection (only consumed when something is connected)
    void deleteRequested();
    // [ui-g] the filter was applied: rows shown / all rows (top level)
    void filterApplied(int shown, int total);

protected:
    void mousePressEvent(QMouseEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;
    void drawRow(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &idx) const override;
    void drawBranches(QPainter *, const QRect &, const QModelIndex &) const override {}
    void paintEvent(QPaintEvent *e) override;
    bool viewportEvent(QEvent *e) override;   // [ui-e] tool tip with the full text of an elided cell

private:
    friend class MxHeaderView;
    void scheduleSave();
    void updateRowHeight();
    void autoFill();
    void captureFractions();
    QList<int> visibleColumns() const;   // logical indexes in visual order
    int defaultWidth(int column) const;
    void absorbResize(int column, int oldSize, int newSize);
    void scheduleLayout();
    QTimer *m_layoutTimer = nullptr;
    QPersistentModelIndex m_rightPressRow;   // [ui-g] the row a right press selected (its menu follows)
    QElapsedTimer m_rightPressTimer;
    bool m_stretchLast = false;          // the list asked for QHeaderView::stretchLastSection
    QHash<int, double> m_fractions;     // [ui-e] user layout: column width / available width
    QHash<int, int> m_pendingAbsolute;  // stored pixel widths waiting for the first layout
    QList<int> m_defaultHidden;
    bool m_layoutTouched = false;       // order / visibility changed by the user
    void applyFilter();
    void layoutFilterBar();
    void ensureFilterSetup();
    bool m_filterSetup = false;
    bool m_externalFilter = false;
    bool m_rowsFiltered = false;         // rows may be hidden by the filter
    QList<int> m_filterColumns;
    int m_filterTotal = 0;
    std::function<void()> m_findHandler;
    bool m_filterEnabled = false;
    QWidget *m_filterBar = nullptr;
    QLineEdit *m_filterEdit = nullptr;
    StaticText *m_filterInfo = nullptr;
    QTimer *m_filterTimer = nullptr;
    QString m_filterText;
    int m_filterMatches = 0;
    int m_rowHeight = 14;
    int m_baseRowHeight = 14;
    int m_treeBoxX = 3;
    int m_treeMargin = 21;
    bool m_treeConnectors = true;
    int m_iconGap = 3;
    QList<int> m_fillColumns;
    QList<int> m_fillWeights;
    int fillWeight(int column) const;
    bool m_autoFill = true;      // no stored layout and no user resize yet
    bool m_filling = false;
    bool m_treeMode = false;
    QList<int> m_defaultWidths;
    QHash<int, QStringList> m_minTexts;                     // [ui-h] setMinimumColumnTexts
    mutable QHash<int, int> m_minTextWidth;                 // [ui-h] cache (cleared on font change)
    QString m_backgroundKey = QStringLiteral("colors/listBackground");   // [ui-h]
    mx::Settings *m_settings = nullptr;
    QString m_stateName;
    QTimer *m_saveTimer = nullptr;
    bool m_restoring = false;
};

} // namespace mx::ui
