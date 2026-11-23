// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: classic Win32 edge drawing helpers (UI_SPEC §1.3). All rectangles are the outer
// widget rectangles; the helpers paint exactly 1 or 2 lines wide edges inside them (a line is 1 px at
// 100 %, uiLine() px at larger interface scales).
#pragma once

#include <QtCore/QRect>
#include <QtGui/QColor>
#include <QtCore/QString>

class QPainter;

namespace mx::ui::draw {

// 1-px raised edge (thin buttons): top/left white, bottom/right #808080.
void thinRaised(QPainter *p, const QRect &r);
// 1-px sunken edge: top/left #808080, bottom/right white.
void thinSunken(QPainter *p, const QRect &r);
// Selected thin button: top/left #808080, bottom/right white, inner bottom/right #D4D0C8.
void thinSelected(QPainter *p, const QRect &r);
// 2-px push-button bevel (outer white/#404040, inner #D4D0C8/#808080).
void buttonRaised(QPainter *p, const QRect &r);
// pressed push button: 1-px #404040 frame + 1-px #808080 inside.
void buttonPressed(QPainter *p, const QRect &r);
// 2-px sunken field (edits, list frames): outer #808080/white, inner #404040/#D4D0C8.
void fieldSunken(QPainter *p, const QRect &r);
// news-bar edge: 2 px, outer #808080, inner #404040 on all sides.
void newsEdge(QPainter *p, const QRect &r);
// classic etched line (group boxes): #808080 then white.
void etchedHLine(QPainter *p, int x0, int x1, int y);
// 50 % dither (white / face checkerboard) used for latched toggle buttons and scrollbar troughs.
void dither(QPainter *p, const QRect &r, const QColor &a, const QColor &b);
// Text at an explicit baseline, vertically centred like the original WinMX does for the 8-pt UI font in a row
// of height h (baseline = top + (h - 12) / 2 + 11; measured on the references). Font engines
// disagree by a pixel on Qt's own AlignVCenter (FreeType vs DirectWrite), this does not.
int baselineFor(int top, int height);
void text(QPainter *p, const QRect &r, int hAlign, const QString &s);
// generic two-colour edge: tl colour on top+left, br colour on bottom+right (1 px).
void edge(QPainter *p, const QRect &r, const QColor &tl, const QColor &br);
// [ui-e] scale helpers: the width of one chrome line (1 px at 100 %) and a rect shrunk by n lines
int lw();
QRect inset(const QRect &r, int lines);

} // namespace mx::ui::draw
