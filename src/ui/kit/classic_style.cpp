// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/kit/classic_style.h"

#include <QtGui/QPainter>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMenu>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QStyleFactory>
#include <QtWidgets/QStyleOption>

#include "ui/kit/colors.h"
#include "ui/kit/draw.h"
#include "ui/kit/format.h"
#include "ui/kit/scale.h"
#include "ui/kit/theme.h"

namespace mx::ui {

using namespace chrome;

ClassicStyle::ClassicStyle() : QProxyStyle(QStyleFactory::create(QStringLiteral("Windows")))
{
    setObjectName(QStringLiteral("mxclassic"));
}

QPalette ClassicStyle::standardPalette() const
{
    return classicPalette();
}

void ClassicStyle::polish(QPalette &pal)
{
    pal = classicPalette();
}

void ClassicStyle::polish(QWidget *w)
{
    QProxyStyle::polish(w);
    if (qobject_cast<QMenu *>(w))
        w->setFont(normalFont());
}

int ClassicStyle::pixelMetric(PixelMetric m, const QStyleOption *opt, const QWidget *w) const
{
    // [ui-e] every metric is a length in design pixels: S() / uiLine() scale it (identity at 100 %)
    const int t = uiLine();
    switch (m) {
    case PM_ButtonShiftHorizontal:
    case PM_ButtonShiftVertical:
        return t;
    case PM_DefaultFrameWidth:
        return 2 * t;
    case PM_ScrollBarExtent:
        return S(16);
    case PM_ScrollBarSliderMin:
        return S(8);
    case PM_SplitterWidth:
        return S(4);
    case PM_IndicatorWidth:
    case PM_IndicatorHeight:
        return S(13);
    case PM_ExclusiveIndicatorWidth:
    case PM_ExclusiveIndicatorHeight:
        return S(12);
    case PM_SmallIconSize:
    case PM_ButtonIconSize:
    case PM_ListViewIconSize:
        return S(16);
    case PM_MenuPanelWidth:
        return 3 * t;
    case PM_MenuHMargin:
    case PM_MenuVMargin:
        return 0;
    case PM_SubMenuOverlap:
        return -3 * t;
    case PM_MenuBarPanelWidth:
        return 0;
    case PM_ToolTipLabelFrameWidth:
        return t;
    case PM_HeaderMargin:
        return S(4);
    case PM_FocusFrameHMargin:
    case PM_FocusFrameVMargin:
        return t;
    case PM_CheckBoxLabelSpacing:
    case PM_RadioButtonLabelSpacing:
        return S(7);
    case PM_MenuButtonIndicator:
        return 0;
    case PM_SizeGripSize:
        return S(13);
    case PM_ButtonDefaultIndicator:
        return 0;   // [qa-1] the default frame is drawn inside the button rect (PE_PanelButtonCommand)
    default: {
        const int v = QProxyStyle::pixelMetric(m, opt, w);
        return isScaled() ? S(v) : v;
    }
    }
}

int ClassicStyle::styleHint(StyleHint h, const QStyleOption *opt, const QWidget *w, QStyleHintReturn *ret) const
{
    switch (h) {
    case SH_EtchDisabledText:
    case SH_DitherDisabledText:
        return 1;
    case SH_ItemView_ShowDecorationSelected:
        return 1;
    case SH_Menu_AllowActiveAndDisabled:
        return 0;
    case SH_Menu_SpaceActivatesItem:
        return 1;
    case SH_Menu_SubMenuPopupDelay:
        return 400;
    case SH_UnderlineShortcut:
        return 0;
    case SH_Header_ArrowAlignment:
        return int(Qt::AlignRight | Qt::AlignVCenter);
    case SH_ToolTip_WakeUpDelay:
        return 500;
    case SH_ItemView_ActivateItemOnSingleClick:
        return 0;
    case SH_ScrollBar_LeftClickAbsolutePosition:
        return 0;
    case SH_ScrollView_FrameOnlyAroundContents:
        return 0;
    case SH_Widget_Animation_Duration:
        return 0;
    default:
        return QProxyStyle::styleHint(h, opt, w, ret);
    }
}

void ClassicStyle::drawPrimitive(PrimitiveElement pe, const QStyleOption *opt, QPainter *p, const QWidget *w) const
{
    switch (pe) {
    case PE_PanelButtonCommand:
    case PE_PanelButtonBevel:
    case PE_PanelButtonTool: {
        QRect r = opt->rect;
        const bool sunken = opt->state & State_Sunken;
        const bool on = opt->state & State_On;
        // [qa-1] Win32 default push button (dialog OK / Continue / Yes): 1-px black frame, bevel inside;
        // pressed = black frame + flat 1-px shadow frame (dialog_close_prompt / dialog_chat_warning refs)
        const auto *btn = qstyleoption_cast<const QStyleOptionButton *>(opt);
        if (pe == PE_PanelButtonCommand && btn && (btn->features & QStyleOptionButton::DefaultButton) && !on) {
            draw::edge(p, r, Text, Text);
            r = draw::inset(r, 1);
            if (sunken) {
                p->fillRect(r, Face);
                draw::edge(p, r, Shadow, Shadow);
                return;
            }
        }
        if (on && !sunken) {
            // latched toggle: 2-px sunken edge + 50 % dither face (UI_SPEC §1.3.4)
            draw::dither(p, draw::inset(r, 2), Light, Face);
            draw::edge(p, r, DarkShadow, Light);
            draw::edge(p, draw::inset(r, 1), Shadow, Face);
        } else if (sunken) {
            p->fillRect(draw::inset(r, 2), Face);
            draw::buttonPressed(p, r);
        } else {
            p->fillRect(draw::inset(r, 2), opt->palette.button());
            draw::buttonRaised(p, r);
        }
        return;
    }
    case PE_FrameDefaultButton:
        return;
    case PE_IndicatorCheckBox:
    case PE_IndicatorItemViewItemCheck:
        if (isScaled()) {   // [ui-e] 2-line sunken box + a check mark drawn at the scaled size
            const QRect r = opt->rect;
            const bool enabled = opt->state & State_Enabled;
            const bool down = opt->state & State_Sunken;
            p->fillRect(draw::inset(r, 2), enabled && !down ? opt->palette.base() : opt->palette.button());
            draw::fieldSunken(p, r);
            if (opt->state & (State_On | State_NoChange)) {
                const QColor c = !enabled || (opt->state & State_NoChange) ? QColor(DisabledText) : QColor(Text);
                const QRect in = draw::inset(r, 2);
                const double u = in.width() / 9.0;   // the classic 7×7 check in a 9×9 field
                const QPointF o = in.topLeft();
                QPolygonF poly;
                poly << o + QPointF(1 * u, 3.5 * u) << o + QPointF(3.5 * u, 6 * u) << o + QPointF(8 * u, 1.5 * u)
                     << o + QPointF(8 * u, 4 * u) << o + QPointF(3.5 * u, 8.5 * u) << o + QPointF(1 * u, 6 * u);
                p->save();
                p->setRenderHint(QPainter::Antialiasing, true);
                p->setPen(Qt::NoPen);
                p->setBrush(c);
                p->drawPolygon(poly);
                p->restore();
            }
            return;
        }
        break;
    case PE_IndicatorRadioButton:
        if (isScaled()) {   // [ui-e] the classic two-ring radio at the scaled size
            const QRectF r = QRectF(opt->rect).adjusted(0.5, 0.5, -0.5, -0.5);
            const double t = uiLine();
            p->save();
            p->setRenderHint(QPainter::Antialiasing, true);
            auto arc = [&](const QRectF &rr, const QColor &tl, const QColor &br) {
                p->setPen(QPen(tl, t, Qt::SolidLine, Qt::FlatCap));
                p->drawArc(rr, 45 * 16, 180 * 16);
                p->setPen(QPen(br, t, Qt::SolidLine, Qt::FlatCap));
                p->drawArc(rr, 225 * 16, 180 * 16);
            };
            const double h = t / 2.0;
            p->setPen(Qt::NoPen);
            p->setBrush((opt->state & State_Enabled) && !(opt->state & State_Sunken) ? opt->palette.base() : opt->palette.button());
            p->drawEllipse(r.adjusted(t, t, -t, -t));
            arc(r.adjusted(h, h, -h, -h), Shadow, Light);
            arc(r.adjusted(t + h, t + h, -t - h, -t - h), DarkShadow, Face);
            if (opt->state & State_On) {
                const double d = r.width() * 4.0 / 12.0;
                p->setPen(Qt::NoPen);
                p->setBrush((opt->state & State_Enabled) ? QColor(Text) : QColor(DisabledText));
                p->drawEllipse(QRectF(r.center().x() - d / 2, r.center().y() - d / 2, d, d));
            }
            p->restore();
            return;
        }
        break;
    case PE_FrameGroupBox:
        if (isScaled()) {   // [ui-e] etched frame, one line = uiLine()
            const QRect r = opt->rect;
            const int t = uiLine();
            draw::edge(p, r.adjusted(0, 0, -t, -t), Shadow, Shadow);
            draw::edge(p, r.adjusted(t, t, 0, 0), Light, Light);
            draw::edge(p, r.adjusted(t, t, -t, -t), Shadow, Light);
            return;
        }
        break;
    case PE_IndicatorBranch:
        if (isScaled()) {   // [ui-e] Win32 tree decoration (dotted lines, [+] box) at the scaled size
            const QRect r = opt->rect;
            const int t = uiLine();
            int b = S(9);
            if (!(b & 1))
                ++b;
            const int cx = r.left() + (r.width() - t) / 2, cy = r.top() + (r.height() - t) / 2;
            const QColor lc(Shadow);
            auto dotsV = [&](int y0, int y1) {
                for (int y = y0; y <= y1; ++y)
                    if (((y / t) & 1) == 0)
                        p->fillRect(QRect(cx, y, t, 1), lc);
            };
            auto dotsH = [&](int x0, int x1) {
                for (int x = x0; x <= x1; ++x)
                    if (((x / t) & 1) == 0)
                        p->fillRect(QRect(x, cy, 1, t), lc);
            };
            if (opt->state & State_Item)
                dotsH(cx, r.right());
            if (opt->state & State_Sibling)
                dotsV(r.top(), r.bottom());
            else if (opt->state & State_Item)
                dotsV(r.top(), cy);
            if (opt->state & State_Children) {
                const QRect box(r.left() + (r.width() - b) / 2, r.top() + (r.height() - b) / 2, b, b);
                p->fillRect(box, opt->palette.base());
                draw::edge(p, box, Shadow, Shadow);
                const int arm = b - 4 * t;
                const int mx = box.left() + (b - t) / 2, my = box.top() + (b - t) / 2;
                p->fillRect(QRect(box.left() + 2 * t, my, arm, t), Text);
                if (!(opt->state & State_Open))
                    p->fillRect(QRect(mx, box.top() + 2 * t, t, arm), Text);
            }
            return;
        }
        break;
    case PE_IndicatorMenuCheckMark:
        if (isScaled()) {
            const QRect r = opt->rect;
            const double u = qMin(r.width(), r.height()) / 12.0;
            const QPointF o(r.center().x() - 6 * u, r.center().y() - 6 * u);
            QPolygonF poly;
            poly << o + QPointF(3 * u, 5.5 * u) << o + QPointF(5 * u, 7.5 * u) << o + QPointF(9.5 * u, 3 * u)
                 << o + QPointF(9.5 * u, 5.5 * u) << o + QPointF(5 * u, 10 * u) << o + QPointF(3 * u, 8 * u);
            p->save();
            p->setRenderHint(QPainter::Antialiasing, true);
            p->setPen(Qt::NoPen);
            p->setBrush((opt->state & State_On) ? opt->palette.highlightedText() : opt->palette.buttonText());
            if (!(opt->state & State_Enabled))
                p->setBrush(QColor(DisabledText));
            p->drawPolygon(poly);
            p->restore();
            return;
        }
        break;
    case PE_FrameMenu:
    case PE_PanelMenu: {
        const QRect r = opt->rect;
        if (pe == PE_PanelMenu)
            p->fillRect(r, Face);
        // EDGE_RAISED: outer 3D-light/dark-shadow, inner white/shadow
        draw::edge(p, r, Face, DarkShadow);
        draw::edge(p, draw::inset(r, 1), Light, Shadow);
        return;
    }
    case PE_PanelTipLabel: {
        p->fillRect(opt->rect, Info);
        draw::edge(p, opt->rect, Text, Text);
        return;
    }
    case PE_FrameLineEdit:
    case PE_Frame: {
        draw::fieldSunken(p, opt->rect);
        return;
    }
    case PE_PanelLineEdit: {
        if (const auto *f = qstyleoption_cast<const QStyleOptionFrame *>(opt)) {
            if (f->lineWidth > 0) {
                p->fillRect(draw::inset(opt->rect, 2), opt->palette.brush(QPalette::Base));
                draw::fieldSunken(p, opt->rect);
                return;
            }
        }
        p->fillRect(opt->rect, opt->palette.brush(QPalette::Base));
        return;
    }
    case PE_PanelItemViewRow:
    case PE_PanelItemViewItem:
        // MxListView paints its own row backgrounds (black / #808080 selection)
        if (w && w->inherits("mx::ui::MxListView"))
            return;
        break;
    case PE_FrameFocusRect: {
        // item views never show a focus rectangle (the original WinMX lists don't)
        if (w && qobject_cast<const QAbstractItemView *>(w))
            return;
        if (w && w->inherits("QAbstractItemView"))
            return;
        // [ui-b] QTreeView::drawRow draws the row focus rect without passing the widget
        if (!w && opt->styleObject && opt->styleObject->inherits("QAbstractItemView"))
            return;
        break;
    }
    case PE_IndicatorHeaderArrow: {
        // Win2000-style embossed triangle: shadow on the left edge, white on the right edge.
        const auto *hdr = qstyleoption_cast<const QStyleOptionHeader *>(opt);
        if (!hdr)
            break;
        const QRect r = opt->rect;
        const int cx = r.center().x(), cy = r.center().y();
        // The Windows convention: ascending (A to Z, small to large) points up, descending points
        // down. QHeaderView reports ascending as "SortDown" (Qt's own styles draw it the other way
        // round), so the triangle points up for SortDown.
        const bool up = hdr->sortIndicator == QStyleOptionHeader::SortDown;
        // 7 wide, 4 tall; [ui-e] at larger scales every pixel is a t×t block (the arrow stays an arrow)
        const int t = uiLine();
        for (int i = 0; i < 4; ++i) {
            const int y = up ? cy - 2 * t + i * t : cy + t - i * t;
            const int half = i * t;   // 0..3
            p->fillRect(QRect(cx - half, y, t, t), Shadow);
            p->fillRect(QRect(cx + half, y, t, t), Light);
        }
        const int baseY = up ? cy + t : cy - 2 * t;
        p->fillRect(QRect(cx - 3 * t, baseY, 7 * t, t), up ? Light : Shadow);
        return;
    }
    default:
        break;
    }
    QProxyStyle::drawPrimitive(pe, opt, p, w);
}

// [ui-e] menu items at interface scales above 100 %: the layout of Qt's Windows style (check column,
// margins, sub-menu arrow, embossed disabled text) with every constant scaled
namespace {
constexpr int kItemFrame = 2, kItemHMargin = 3, kItemVMargin = 2, kArrowHMargin = 6, kRightBorder = 15,
              kCheckWidth = 12, kTabSpacing = 20;
}

QSize ClassicStyle::scaledMenuItemSize(const QStyleOptionMenuItem *mi, const QSize &contents) const
{
    if (mi->menuItemType == QStyleOptionMenuItem::Separator)
        return QSize(S(10), S(9));
    const QFontMetrics fm(mi->font);
    QString text = mi->text;
    const qsizetype tab = text.indexOf(QLatin1Char('\t'));
    int w = fm.horizontalAdvance(QString(text).remove(QLatin1Char('&')).replace(QLatin1Char('\t'), QLatin1Char(' ')));
    w = qMax(w, contents.width());
    if (tab >= 0)
        w += S(kTabSpacing);
    else if (mi->menuItemType == QStyleOptionMenuItem::SubMenu)
        w += 2 * S(kArrowHMargin);
    const int checkcol = qMax(mi->maxIconWidth, S(kCheckWidth));
    w += checkcol + S(kRightBorder) + S(10) + 2 * S(kItemFrame);
    const int h = qMax(S(17), fm.height() + 2 * S(kItemVMargin));
    return QSize(w, h);
}

void ClassicStyle::drawScaledMenuItem(const QStyleOptionMenuItem *mi, QPainter *p, const QWidget *w) const
{
    const QRect r = mi->rect;
    const int x = r.x(), y = r.y(), wd = r.width(), h = r.height();
    const bool dis = !(mi->state & State_Enabled);
    const bool act = mi->state & State_Selected;
    const int t = uiLine();
    p->fillRect(r, act ? mi->palette.brush(QPalette::Highlight) : mi->palette.brush(QPalette::Button));
    if (mi->menuItemType == QStyleOptionMenuItem::Separator) {
        const int yoff = y + h / 2 - t;
        p->fillRect(QRect(x + S(2), yoff, wd - S(6), t), Shadow);
        p->fillRect(QRect(x + S(2), yoff + t, wd - S(6), t), Light);
        return;
    }
    const int checkcol = qMax(mi->maxIconWidth, S(kCheckWidth));
    const QColor fg = dis ? QColor(DisabledText) : act ? mi->palette.highlightedText().color() : mi->palette.buttonText().color();
    if (mi->checkType != QStyleOptionMenuItem::NotCheckable && mi->checked) {
        const QRect cr(x + S(kItemFrame), y + S(kItemFrame), checkcol - 2 * S(kItemFrame) + S(4), h - 2 * S(kItemFrame));
        if (mi->checkType == QStyleOptionMenuItem::Exclusive) {
            const double d = S(5) + 0.5;
            p->save();
            p->setRenderHint(QPainter::Antialiasing, true);
            p->setPen(Qt::NoPen);
            p->setBrush(fg);
            p->drawEllipse(QRectF(x + S(7), cr.center().y() - d / 2 + 0.5, d, d));
            p->restore();
        } else {
            QStyleOptionMenuItem c = *mi;
            c.rect = cr;
            c.state = dis ? State_None : State_Enabled;
            if (act)
                c.state |= State_On;
            drawPrimitive(PE_IndicatorMenuCheckMark, &c, p, w);
        }
    } else if (!mi->icon.isNull()) {
        const int is = S(16);
        const QPixmap pm = mi->icon.pixmap(QSize(is, is), dis ? QIcon::Disabled : QIcon::Normal);
        p->drawPixmap(x + (checkcol + 2 * S(kItemFrame) - pm.width()) / 2, y + (h - pm.height()) / 2, pm);
    }
    const int xm = S(kItemFrame) + checkcol + S(kItemHMargin);
    QString text = mi->text;
    QString shortcut;
    const qsizetype tab = text.indexOf(QLatin1Char('\t'));
    if (tab >= 0) {
        shortcut = text.mid(tab + 1);
        text = text.left(tab);
    }
    text.remove(QLatin1Char('&'));
    QFont f = mi->font;
    if (mi->menuItemType == QStyleOptionMenuItem::DefaultItem)
        f.setBold(true);
    p->save();
    p->setFont(f);
    const QRect tr(x + xm, y + S(kItemVMargin), wd - xm - S(kRightBorder) - mi->reservedShortcutWidth + 1, h - 2 * S(kItemVMargin));
    const QRect sr(tr.right() + 1, tr.top(), r.right() - tr.right() - S(kRightBorder), tr.height());
    const int flags = Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine | Qt::TextDontClip;
    if (dis && !act) {
        p->setPen(Light);
        p->drawText(tr.translated(t, t), flags, text);
        if (!shortcut.isEmpty())
            p->drawText(sr.translated(t, t), flags, shortcut);
    }
    p->setPen(fg);
    p->drawText(tr, flags, text);
    if (!shortcut.isEmpty())
        p->drawText(sr, flags, shortcut);
    p->restore();
    if (mi->menuItemType == QStyleOptionMenuItem::SubMenu) {
        // solid right-pointing triangle
        const int dim = qMax(S(4), (h - 2 * S(kItemFrame)) / 2);
        const int ax = x + wd - S(kArrowHMargin) - S(kItemFrame) - dim / 2;
        const int ay = y + h / 2;
        const int half = dim / 2;
        p->save();
        p->setPen(Qt::NoPen);
        p->setBrush(fg);
        p->drawPolygon(QPolygon({QPoint(ax, ay - half), QPoint(ax + half, ay), QPoint(ax, ay + half)}));
        p->restore();
    }
}

void ClassicStyle::drawControl(ControlElement ce, const QStyleOption *opt, QPainter *p, const QWidget *w) const
{
    switch (ce) {
    case CE_HeaderSection: {
        const QRect r = opt->rect;
        p->fillRect(r, Face);
        if (opt->state & State_Sunken) {
            draw::edge(p, r, Shadow, Shadow);
        } else {
            draw::edge(p, r, Light, DarkShadow);
            draw::edge(p, draw::inset(r, 1), Face, Shadow);
        }
        return;
    }
    case CE_HeaderLabel: {
        const auto *h = qstyleoption_cast<const QStyleOptionHeader *>(opt);
        if (!h)
            break;
        QRect r = h->rect.adjusted(S(6), 0, -S(6), 0);
        if (h->state & State_Sunken)
            r.translate(uiLine(), uiLine());
        const QFontMetrics fm(p->font());
        const QString t = fmt::elide(h->text, fm, r.width());
        p->setPen(opt->palette.color(QPalette::ButtonText));
        draw::text(p, r, int(h->textAlignment & Qt::AlignHorizontal_Mask), t);
        return;
    }
    case CE_Header: {
        const auto *h = qstyleoption_cast<const QStyleOptionHeader *>(opt);
        if (!h)
            break;
        drawControl(CE_HeaderSection, h, p, w);
        QStyleOptionHeader sub = *h;
        sub.rect = h->rect;
        if (h->sortIndicator != QStyleOptionHeader::None) {
            // make room for the arrow
            const int aw = S(12);
            QStyleOptionHeader arrow = *h;
            if (h->textAlignment & Qt::AlignRight) {
                arrow.rect = QRect(h->rect.left() + S(3), h->rect.top(), aw, h->rect.height());
            } else {
                const QFontMetrics fm(p->font());
                const int tw = fm.horizontalAdvance(h->text);
                const int x = qMin(h->rect.left() + S(5) + tw + S(4), h->rect.right() - aw - S(2));
                arrow.rect = QRect(x, h->rect.top(), aw, h->rect.height());
                sub.rect.setRight(x - 1 + S(5));
            }
            drawPrimitive(PE_IndicatorHeaderArrow, &arrow, p, w);
            if (h->textAlignment & Qt::AlignRight)
                sub.rect.setLeft(h->rect.left() + aw);
        }
        drawControl(CE_HeaderLabel, &sub, p, w);
        return;
    }
    case CE_PushButtonLabel: {
        if (const auto *b = qstyleoption_cast<const QStyleOptionButton *>(opt)) {
            QStyleOptionButton c = *b;
            if (c.state & (State_On | State_Sunken)) // pressed / latched buttons shift their text
                c.rect.translate(uiLine(), uiLine());
            if (!c.icon.isNull() || c.text.contains(QLatin1Char('\n'))) {
                QProxyStyle::drawControl(ce, &c, p, w);
                return;
            }
            // single-line label: centred; 1 px above the generic baseline of the contents rect
            // (measured on the native references for 19-, 21- and 42-px buttons)
            const QRect br = c.rect;
            QString t = c.text;
            t.remove(QLatin1Char('&'));
            p->setFont(fitFont(p->font(), t, br.width() - 2 * uiLine()));   // [qa-2] scaled only
            const QFontMetrics fm(p->font());
            const int x = br.left() + (br.width() - fm.horizontalAdvance(t)) / 2;
            const int y = fm.ascent() <= 11 && fm.height() <= 13 ? draw::baselineFor(br.top(), br.height()) - 1
                                                                 : br.top() + (br.height() - fm.height()) / 2 + fm.ascent();
            if (!(c.state & State_Enabled)) {
                p->setPen(Light);
                p->drawText(QPoint(x + uiLine(), y + uiLine()), t);
                p->setPen(DisabledText);
            } else {
                p->setPen(c.palette.color(QPalette::ButtonText));
            }
            p->drawText(QPoint(x, y), t);
            return;
        }
        break;
    }
    case CE_RadioButtonLabel:
    case CE_CheckBoxLabel: {
        // single-line labels at the explicit classic WinMX baseline (identical on every font engine)
        const auto *b = qstyleoption_cast<const QStyleOptionButton *>(opt);
        if (b && b->icon.isNull() && !b->text.contains(QLatin1Char('\n'))) {
            QString t = b->text;
            t.remove(QLatin1Char('&'));
            const QFont keep = p->font();   // [qa-2] scaled: shrink a little rather than cut the text
            p->setFont(fitFont(keep, t, b->rect.width()));
            if (!(b->state & State_Enabled)) {
                p->setPen(Light);
                draw::text(p, b->rect.translated(uiLine(), uiLine()), Qt::AlignLeft, t);
                p->setPen(DisabledText);
            } else {
                p->setPen(b->palette.color(QPalette::WindowText));
            }
            draw::text(p, b->rect, Qt::AlignLeft, t);
            p->setFont(keep);
            return;
        }
        break;
    }
    case CE_MenuItem: {
        // exclusive choices show the classic radio bullet instead of a check mark (UI_SPEC §1.7)
        const auto *mi = qstyleoption_cast<const QStyleOptionMenuItem *>(opt);
        if (mi && isScaled()) {   // [ui-e] Qt's Windows style uses fixed pixel constants for menus
            drawScaledMenuItem(mi, p, w);
            return;
        }
        if (mi && mi->checkType == QStyleOptionMenuItem::Exclusive && mi->checked) {
            QStyleOptionMenuItem c = *mi;
            c.checked = false;
            QProxyStyle::drawControl(ce, &c, p, w);
            const bool hi = (mi->state & State_Selected) && (mi->state & State_Enabled);
            const QColor col = !(mi->state & State_Enabled) ? DisabledText : hi ? Light : Text;
            static const char *dot[] = {".###.", "#####", "#####", "#####", ".###."};
            const int x0 = mi->rect.left() + 7, y0 = mi->rect.top() + (mi->rect.height() - 5) / 2;
            for (int y = 0; y < 5; ++y)
                for (int x = 0; x < 5; ++x)
                    if (dot[y][x] == '#')
                        p->fillRect(QRect(x0 + x, y0 + y, 1, 1), col);
            return;
        }
        break;
    }
    default:
        break;
    }
    QProxyStyle::drawControl(ce, opt, p, w);
}

void ClassicStyle::drawComplexControl(ComplexControl cc, const QStyleOptionComplex *opt, QPainter *p, const QWidget *w) const
{
    QProxyStyle::drawComplexControl(cc, opt, p, w);
}

QSize ClassicStyle::sizeFromContents(ContentsType ct, const QStyleOption *opt, const QSize &size, const QWidget *w) const
{
    switch (ct) {
    case CT_HeaderSection: {
        QSize s = QProxyStyle::sizeFromContents(ct, opt, size, w);
        s.setHeight(S(17));
        return s;
    }
    case CT_MenuItem: {
        if (isScaled())
            if (const auto *mi = qstyleoption_cast<const QStyleOptionMenuItem *>(opt))
                return scaledMenuItemSize(mi, size);
        QSize s = QProxyStyle::sizeFromContents(ct, opt, size, w);
        if (const auto *mi = qstyleoption_cast<const QStyleOptionMenuItem *>(opt)) {
            if (mi->menuItemType == QStyleOptionMenuItem::Separator)
                s.setHeight(9);
            else
                s.setHeight(17);
        }
        return s;
    }
    default:
        return QProxyStyle::sizeFromContents(ct, opt, size, w);
    }
}

QRect ClassicStyle::subControlRect(ComplexControl cc, const QStyleOptionComplex *opt, SubControl sc, const QWidget *w) const
{
    // [qa-2] Qt's Windows style gives the combo box arrow button a fixed 16 px: at 250 % that was a
    // sliver with a 7-px arrow next to a 51-px tall field. Scaled: S(16) wide (the arrow scales with it)
    if (cc == CC_ComboBox && isScaled() && (sc == SC_ComboBoxArrow || sc == SC_ComboBoxEditField)) {
        if (const auto *cb = qstyleoption_cast<const QStyleOptionComboBox *>(opt)) {
            const QRect r = cb->rect;
            const int f = cb->frame ? 2 * uiLine() : 0;
            const int aw = qMin(S(16), r.width() / 2);
            const QRect arrow(r.right() - f - aw + 1, r.top() + f, aw, r.height() - 2 * f);
            if (sc == SC_ComboBoxArrow)
                return visualRect(cb->direction, r, arrow);
            const int m = f + uiLine();
            return visualRect(cb->direction, r, QRect(r.left() + m, r.top() + m, arrow.left() - r.left() - m - uiLine(), r.height() - 2 * m));
        }
    }
    return QProxyStyle::subControlRect(cc, opt, sc, w);
}

QRect ClassicStyle::subElementRect(SubElement se, const QStyleOption *opt, const QWidget *w) const
{
    return QProxyStyle::subElementRect(se, opt, w);
}

} // namespace mx::ui
