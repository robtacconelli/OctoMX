// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/kit/status_bar.h"

#include <QtGui/QPainter>
#include <QtWidgets/QSizeGrip>

#include "ui/kit/colors.h"
#include "ui/kit/draw.h"
#include "ui/kit/scale.h"
#include "ui/kit/theme.h"

namespace mx::ui {

using namespace chrome;

FloatTab::FloatTab(QWidget *parent) : QWidget(parent), m_text(tr("Float"))
{
    setObjectName(QStringLiteral("floatTab"));
    // [ui-g]
    setToolTip(tr("Float: show this page in a window of its own, to see it beside another page. "
                  "Dock (or closing the window) puts it back."));
    setFixedSize(SS(38, 12));
    setCursor(Qt::ArrowCursor);
}

void FloatTab::setText(const QString &t)
{
    m_text = t;
    update();
}

void FloatTab::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), Face);
    const int h = height();
    const int t = uiLine();
    // upside-down sheet tab: white slanted left edge, gray slanted right edge and bottom
    for (int y = 0; y < h; ++y) {
        const int inset = y * S(4) / (h - 1);   // 0..4
        p.fillRect(QRect(inset, y, t, 1), Light);
        p.fillRect(QRect(width() - t - inset, y, t, 1), Shadow);
    }
    p.fillRect(QRect(S(4), h - t, width() - 2 * S(4), t), Shadow);
    p.setPen(Text);
    p.setFont(normalFont());
    p.drawText(QRect(0, -S(2), width(), h + S(1)), Qt::AlignHCenter | Qt::AlignVCenter, m_text);
}

void FloatTab::mousePressEvent(QMouseEvent *)
{
    emit clicked();
}

StatusBar::StatusBar(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("statusBar"));
    setFixedHeight(S(20));
    m_float = new FloatTab(this);
    m_grip = new QSizeGrip(this);
    m_grip->setFixedSize(SS(13, 13));
}

void StatusBar::setFields(const QStringList &fields)
{
    if (m_fields == fields)
        return;
    m_fields = fields;
    update();
}

void StatusBar::setSizeGripVisible(bool v)
{
    m_grip->setVisible(v);
}

void StatusBar::resizeEvent(QResizeEvent *)
{
    m_grip->move(width() - m_grip->width(), height() - m_grip->height());
    m_float->move(width() - S(26) - m_float->width(), S(1));
}

void StatusBar::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), Face);
    p.setPen(Text);
    p.setFont(normalFont());
    const QFontMetrics fm(normalFont());
    int x = S(5);
    const int limit = m_float->x() - S(4);
    for (const QString &f : std::as_const(m_fields)) {
        if (x >= limit)
            break;
        const int w = fm.horizontalAdvance(f);
        p.save();
        p.setClipRect(QRect(x, 0, qMax(0, limit - x), height()));
        draw::text(&p, QRect(x, 0, w + S(2), height()), Qt::AlignLeft, f);
        p.restore();
        x += w + S(11);
    }
}

} // namespace mx::ui
