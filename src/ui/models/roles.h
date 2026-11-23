// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: custom item-data roles understood by mx::ui::MxListView's delegate.
//
// Standard roles used: Qt::DisplayRole (text), Qt::ForegroundRole (QColor, default list text),
// Qt::TextAlignmentRole (Qt::AlignRight for numbers), Qt::DecorationRole (16×16 QPixmap),
// Qt::ToolTipRole, Qt::FontRole (QFont, e.g. bold).
#pragma once

#include <QtCore/QVariant>
#include <QtGui/QColor>

namespace mx::ui {

enum ItemRole : int {
    // QVariantList of Segment (see makeSegments): text drawn in several colours, e.g. "2 + 0"
    // with a green 2, white " + " and red 0. Takes precedence over DisplayRole when painting.
    SegmentsRole = Qt::UserRole + 100,
    // int 0..100: draw a progress bar over the whole cell with the DisplayRole text centred over it
    ProgressRole,
    // value used for sorting (numbers as qint64/double, strings case-insensitive)
    SortRole,
    // bool: the row is a centred section header with rule lines (Hotlist "Temporary")
    SectionHeaderRole,
    // QColor: colour of a section header (default colors/hotlistHeader)
    SectionColorRole,
    // stable id of the row's object (search hit key, transfer id, server id ...)
    IdRole,
    // the row's payload (mx::SearchHit, mx::TransferInfo ... as QVariant)
    PayloadRole,
    // int: row kind (model specific, e.g. 0 = file row, 1 = source row)
    KindRole,
    // bool: clip overlong text at the cell edge instead of eliding with "..." (Speed, Status)
    ClipRole,
    // bool: the text may run on over the following (empty) cells of the row, as the original WinMX draws long
    // transfer status strings ("Timed out (Waiting for incoming connection)")
    OverflowRole,
    // [ui-i] with ProgressRole: QByteArray map of the file (TransferInfo::pieceMap: 0 missing, 1 in progress,
    // 2 complete per cell); the bar then shows the map instead of filling from the left by the percentage
    PieceMapRole,
    // [ui-i] QByteArray, TransferInfo::pieceFill (share of verified bytes per cell), optional
    PieceFillRole,
    // [ui-i] QColor: the text over the missing part of a progress bar (default: the bar colour); a status
    // text drawn in the bar keeps its state colour there
    BarTextColorRole,
    // free for follow-up models
    UserRoleBase = Qt::UserRole + 200,
};

struct Segment {
    QString text;
    QColor color;
};

QVariant makeSegments(const QList<Segment> &segs);
QList<Segment> segmentsFrom(const QVariant &v);

} // namespace mx::ui

Q_DECLARE_METATYPE(mx::ui::Segment)
