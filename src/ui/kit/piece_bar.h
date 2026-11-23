// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI [ui-i]: the classic progress cell of the Transfers lists as a map of the file.
//
// The look of the original WinMX bar (a 6-px block, a 2-px gap, then a bar with a 1-px outline in the
// bar colour, flat colours, the text centred and inverted over the fill) stretched over the whole cell.
// With a map (TransferInfo::pieceMap / pieceFill) the bar shows the file from its first byte (left)
// to its last byte (right), one flat colour per pixel column:
//   verified       the bar colour (colors/progressBar, the classic blue)
//   partly there   one intermediate tone, a third of the way from the background to the bar colour
//                  (a column whose cells are only partly verified: never drawn full, never empty)
//   arriving now   colors/progressActive (green: the colour of the original WinMX for "transferring"); wins over the
//                  rest, so a single cell in progress stays visible at any width
//   missing        the bar background (colors/progressBackground)
// Columns are full height, so a glyph of the centred text changes colour only at column edges, as at
// the classic fill edge. Without a map the bar fills from the left by the percentage (as before).
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QRect>
#include <QtCore/QString>
#include <QtCore/QVector>
#include <QtGui/QColor>

class QPainter;

namespace mx::ui::piecebar {

struct Column {
    int  share = 0;        // verified share: 0 none, 1 ... 254 partly, 255 all
    bool active = false;   // the column covers a cell in progress
    bool operator==(const Column &o) const { return share == o.share && active == o.active; }
    bool operator!=(const Column &o) const { return !(*this == o); }
};

// The columns of a bar `width` pixels wide for a map (state: 0 missing, 1 in progress, 2 complete;
// fill: 0 ... 255 share of verified bytes per cell, may be empty). Pixel x covers the cells
// [x * n / width, (x + 1) * n / width) weighted by overlap. Empty map or width < 1: no columns.
QVector<Column> columns(const QByteArray &state, const QByteArray &fill, int width);
// The classic incremental bar: the first width * percent / 100 columns full.
QVector<Column> incremental(int percent, int width);

struct Colors {
    QColor bar;          // outline, block, verified parts (colors/progressBar)
    QColor background;   // missing parts (colors/progressBackground)
    QColor active;       // parts arriving now (colors/progressActive)
    QColor text;         // text over the missing parts (default: bar)
};
Colors themeColors();    // from mx::ui::Colors (current theme / user colours)
QColor partialColor(const Colors &c);   // the "partly there" tone
// the text colour over the active colour: the more legible of background / bar, else black or white
QColor textOverActive(const Colors &c);

// Geometry inside a list cell (design pixels scaled): the block, then the frame up to the right
// cell margin. A cell too narrow for the block shows the frame alone.
struct Geometry {
    QRect block;   // may be empty
    QRect frame;   // outline rectangle
    QRect inner;   // frame minus the outline: where the columns go
};
Geometry geometry(const QRect &cell);

// Paint a bar into `cell` (block, frame, columns, centred text elided to the frame).
void paint(QPainter *p, const QRect &cell, const QVector<Column> &cols, const QString &text, const Colors &c);

// Count of runs of cells in progress ("6 parts downloading") and the tool tip line:
// "412 MB of 1.09 GB (38 %), 6 parts downloading".
int activeRuns(const QByteArray &state);
QString tipLine(qint64 done, qint64 size, const QByteArray &state);

} // namespace mx::ui::piecebar
