// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/kit/draw.h"

#include <QtGui/QFontMetrics>
#include <QtGui/QImage>
#include <QtGui/QPainter>

#include "ui/kit/colors.h"
#include "ui/kit/scale.h"
#include "ui/kit/theme.h"

namespace mx::ui::draw {

void edge(QPainter *p, const QRect &r, const QColor &tl, const QColor &br)
{
    if (r.width() <= 0 || r.height() <= 0)
        return;
    // [ui-e] one chrome line = uiLine() device pixels, the same on every side
    const int t = uiLine();
    p->fillRect(QRect(r.left(), r.top(), r.width(), t), tl);
    p->fillRect(QRect(r.left(), r.top(), t, r.height()), tl);
    p->fillRect(QRect(r.left(), r.bottom() - t + 1, r.width(), t), br);
    p->fillRect(QRect(r.right() - t + 1, r.top(), t, r.height()), br);
}

int lw()
{
    return uiLine();
}

QRect inset(const QRect &r, int lines)
{
    const int d = lines * uiLine();
    return r.adjusted(d, d, -d, -d);
}

int baselineFor(int top, int height)
{
    if (isScaled()) {   // [ui-e] the measured offsets belong to the 11-px face: centre other sizes
        const QFontMetrics fm(normalFont());
        return top + (height - fm.height()) / 2 + fm.ascent();
    }
    return top + (height - 12) / 2 + 11;
}

void text(QPainter *p, const QRect &r, int hAlign, const QString &s)
{
    const QFontMetrics fm(p->font());
    const int w = fm.horizontalAdvance(s);
    int x = r.left();
    if (hAlign & Qt::AlignRight)
        x = r.right() + 1 - w;
    else if (hAlign & Qt::AlignHCenter)
        x = r.left() + (r.width() - w) / 2;
    // the measured baseline of Tahoma 8 pt; any other (Settings ▸ Appearance) font is centred
    const int base = fm.ascent() <= 11 && fm.height() <= 13 ? baselineFor(r.top(), r.height())
                                                             : r.top() + (r.height() - fm.height()) / 2 + fm.ascent();
    p->drawText(QPoint(x, base), s);
}

void thinRaised(QPainter *p, const QRect &r)
{
    edge(p, r, chrome::Light, chrome::Shadow);
}

void thinSunken(QPainter *p, const QRect &r)
{
    edge(p, r, chrome::Shadow, chrome::Light);
}

void thinSelected(QPainter *p, const QRect &r)
{
    edge(p, r, chrome::Shadow, chrome::Light);
    const int t = uiLine();
    const QRect in = inset(r, 1);
    p->fillRect(QRect(in.left(), in.bottom() - t + 1, in.width(), t), chrome::Face);
    p->fillRect(QRect(in.right() - t + 1, in.top(), t, in.height()), chrome::Face);
}

void buttonRaised(QPainter *p, const QRect &r)
{
    edge(p, r, chrome::Light, chrome::DarkShadow);
    edge(p, inset(r, 1), chrome::Face, chrome::Shadow);
}

void buttonPressed(QPainter *p, const QRect &r)
{
    edge(p, r, chrome::DarkShadow, chrome::DarkShadow);
    edge(p, inset(r, 1), chrome::Shadow, chrome::Shadow);
}

void fieldSunken(QPainter *p, const QRect &r)
{
    edge(p, r, chrome::Shadow, chrome::Light);
    edge(p, inset(r, 1), chrome::DarkShadow, chrome::Face);
}

void newsEdge(QPainter *p, const QRect &r)
{
    edge(p, r, chrome::Shadow, chrome::Shadow);
    edge(p, inset(r, 1), chrome::DarkShadow, chrome::DarkShadow);
}

void etchedHLine(QPainter *p, int x0, int x1, int y)
{
    const int t = uiLine();
    p->fillRect(QRect(x0, y, x1 - x0 + 1, t), chrome::Shadow);
    p->fillRect(QRect(x0, y + t, x1 - x0 + 1, t), chrome::Light);
}

void dither(QPainter *p, const QRect &r, const QColor &a, const QColor &b)
{
    // Pattern anchored to absolute device coordinates, as Windows does.
    // [ui-e] at larger scales the checkerboard cells are uiLine() pixels wide
    const int t = uiLine();
    QImage img(2 * t, 2 * t, QImage::Format_RGB32);
    img.fill(b.rgb());
    for (int y = 0; y < t; ++y)
        for (int x = 0; x < t; ++x) {
            img.setPixel(x, y, a.rgb());
            img.setPixel(t + x, t + y, a.rgb());
        }
    QBrush br(img);
    p->save();
    p->setBrushOrigin(0, 0);
    p->fillRect(r, br);
    p->restore();
}

} // namespace mx::ui::draw
