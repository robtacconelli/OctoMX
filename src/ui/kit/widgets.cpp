// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/kit/widgets.h"

#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QActionGroup>
#include <QtWidgets/QMenu>
#include <QtWidgets/QStyle>
#include <QtWidgets/QStyleOptionButton>

#include "ui/kit/colors.h"
#include "ui/kit/draw.h"
#include "ui/kit/format.h"
#include "ui/kit/scale.h"
#include "ui/kit/theme.h"

namespace mx::ui {

using namespace chrome;

// ------------------------------------------------------------------------------------ ThinButton

ThinButton::ThinButton(const QString &text, QWidget *parent) : QAbstractButton(parent)
{
    setText(text);
    setFocusPolicy(Qt::NoFocus);
}

ThinButton::ThinButton(IconId icon, const QString &text, QWidget *parent) : ThinButton(text, parent)
{
    m_icon = icon;
    m_hasIcon = true;
}

void ThinButton::setSelected(bool s)
{
    if (m_selected == s)
        return;
    m_selected = s;
    update();
}

QSize ThinButton::sizeHint() const
{
    return SS(94, 21);
}

void ThinButton::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const QRect r = rect();
    const bool sel = isSelected() && !m_blank;
    const bool down = isDown() && !sel;
    p.fillRect(r, sel ? Light : Face);
    if (sel)
        draw::thinSelected(&p, r);
    else if (down)
        draw::thinSunken(&p, r);
    else
        draw::thinRaised(&p, r);
    if (m_blank)
        return;
    const int lw = uiLine();
    const int shift = down ? lw : 0;
    int x = S(2) + shift;
    if (m_hasIcon) {
        const QPixmap pm = iconPixmap(m_icon);
        p.drawPixmap(x, (r.height() - pm.height()) / 2 + shift, pm);
        x += pm.width() + S(3);
    }
    p.setFont(font());
    const QRect tr(x, shift, r.width() - x - lw, r.height() - lw);
    const QFontMetrics fm(font());
    const QString t = fmt::elide(text(), fm, tr.width());
    const QRect cr = m_centered ? QRect(shift, shift, r.width(), r.height() - lw) : tr;
    const int ha = m_centered ? Qt::AlignHCenter : Qt::AlignLeft;
    if (!isEnabled()) {
        p.setPen(Light);
        draw::text(&p, cr.translated(lw, lw), ha, t);
        p.setPen(DisabledText);
    } else {
        p.setPen(Text);
    }
    draw::text(&p, cr, ha, t);
}

// ------------------------------------------------------------------------------------ WrapLabel

WrapLabel::WrapLabel(const QString &text, QWidget *parent) : QWidget(parent), m_text(text) {}

QStringList WrapLabel::wrap(int width) const
{
    return wrapWith(QFontMetrics(font()), width);
}

QStringList WrapLabel::wrapWith(const QFontMetrics &fm, int width) const
{
    QStringList lines;
    for (const QString &para : m_text.split(QLatin1Char('\n'))) {
        // tokens: words with their trailing spaces
        QStringList tokens;
        QString cur;
        for (int i = 0; i < para.size(); ++i) {
            cur += para.at(i);
            if (para.at(i) == QLatin1Char(' ') && (i + 1 == para.size() || para.at(i + 1) != QLatin1Char(' '))) {
                tokens << cur;
                cur.clear();
            }
        }
        if (!cur.isEmpty())
            tokens << cur;
        QString line;
        for (const QString &t : std::as_const(tokens)) {
            const QString candidate = line + t;
            if (!line.isEmpty() && fm.horizontalAdvance(candidate.trimmed()) > width) {
                lines << line.trimmed();
                line = t;
            } else {
                line = candidate;
            }
        }
        lines << line.trimmed();
    }
    return lines;
}

QStringList WrapLabel::wrappedLines() const
{
    return wrap(width());
}

int WrapLabel::linePitch() const
{
    // [ui-e] the pitch is in design pixels; a scaled font may need more
    return isScaled() ? qMax(S(m_pitch), QFontMetrics(font()).lineSpacing()) : m_pitch;
}

int WrapLabel::heightForWidth(int w) const
{
    return int(wrap(w).size()) * linePitch();
}

bool WrapLabel::fitText(QFont *font, int *pitch, QStringList *lines) const
{
    *font = this->font();
    *pitch = linePitch();
    *lines = wrap(width());
    auto needs = [](const QStringList &ls, int pt, const QFontMetrics &m) {
        return ls.isEmpty() ? 0 : int(ls.size() - 1) * pt + m.ascent() + m.descent();
    };
    if (needs(*lines, *pitch, QFontMetrics(*font)) <= height())
        return true;
    if (!isScaled())
        return false;
    // [qa-2] scaled, fixed classic box: a text that needs more lines than the box holds (the font grew a
    // little faster than the box) first gets a tighter line pitch, then a font up to 15 % smaller,
    // rather than losing its last line
    const QFont f0 = *font;
    const int px = f0.pixelSize(), floorPx = qMax(1, px * 85 / 100);
    for (int n = px; n >= floorPx && px > 0; --n) {
        QFont g = f0;
        g.setPixelSize(n);
        const QFontMetrics gm(g);
        *font = g;
        *pitch = n == px ? qMin(linePitch(), gm.lineSpacing()) : gm.lineSpacing();
        *lines = wrapWith(gm, width());
        if (needs(*lines, *pitch, gm) <= height())
            return true;
    }
    return false;
}

void WrapLabel::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setPen(isEnabled() ? Text : DisabledText);
    QFont f;
    int pitch = 0;
    QStringList lines;
    fitText(&f, &pitch, &lines);
    p.setFont(f);
    const QFontMetrics fm(f);
    int y = 0;
    for (const QString &l : std::as_const(lines)) {
        p.drawText(QPoint(0, y + fm.ascent()), l);
        y += pitch;
    }
}

// ----------------------------------------------------------------------------------- StaticText

StaticText::StaticText(const QString &text, QWidget *parent) : QWidget(parent), m_text(text) {}

void StaticText::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setFont(font());
    p.setPen(isEnabled() ? palette().color(QPalette::WindowText) : DisabledText);
    p.setFont(fitFont(font(), m_text, width()));   // [qa-2] scaled: shrink a little rather than cut
    if (p.font().pixelSize() == fontPx(11) || p.font().pixelSize() <= 0) {
        draw::text(&p, rect(), m_align, m_text);
    } else {
        const QFontMetrics fm(font());
        p.drawText(rect(), m_align | Qt::AlignVCenter, m_text);
    }
}

// ------------------------------------------------------------------------------------ PushButton

PushButton::PushButton(const QString &text, QWidget *parent) : QPushButton(text, parent)
{
    setAutoDefault(false);
    setFocusPolicy(Qt::TabFocus);
}

// ------------------------------------------------------------------------------------ MenuButton

MenuButton::MenuButton(QWidget *parent) : PushButton(QString(), parent)
{
    m_menu = new QMenu(this);
    m_group = new QActionGroup(this);
    m_group->setExclusive(true);
    connect(this, &QPushButton::clicked, this, &MenuButton::popup);
    connect(m_group, &QActionGroup::triggered, this, [this](QAction *a) { choose(a, true); });
}

QAction *MenuButton::addChoice(const QString &label, const QVariant &data, const QString &buttonText)
{
    return addChoiceTo(m_menu, label, data, buttonText);
}

QMenu *MenuButton::addSubmenu(const QString &title)
{
    return m_menu->addMenu(title);
}

QAction *MenuButton::addChoiceTo(QMenu *menu, const QString &label, const QVariant &data, const QString &buttonText)
{
    QAction *a = menu->addAction(label);
    a->setCheckable(true);
    a->setData(data);
    a->setProperty("buttonText", buttonText.isEmpty() ? label : buttonText);
    m_group->addAction(a);
    if (!m_current.isValid())
        choose(a, false);
    return a;
}

void MenuButton::addSeparator()
{
    m_menu->addSeparator();
}

void MenuButton::setCurrentData(const QVariant &d)
{
    for (QAction *a : m_group->actions())
        if (a->data() == d) {
            choose(a, false);
            return;
        }
}

void MenuButton::choose(QAction *a, bool emitSignal)
{
    a->setChecked(true);
    m_current = a->data();
    setText(a->property("buttonText").toString());
    refitText();
    if (emitSignal)
        emit choiceChanged(m_current);
}

void MenuButton::setTextFitter(TextFitter fitter)
{
    m_fitter = std::move(fitter);
    refitText();
}

void MenuButton::refitText()
{
    if (!m_fitter || !m_current.isValid())
        return;
    const QString t = m_fitter(m_current, fontMetrics(), qMax(10, width() - 2 * S(6)));
    if (!t.isEmpty())
        setText(t);
}

void MenuButton::resizeEvent(QResizeEvent *e)
{
    PushButton::resizeEvent(e);
    refitText();
}

void MenuButton::popup()
{
    m_menu->popup(mapToGlobal(QPoint(0, height())));
}

// -------------------------------------------------------------------------------- VerticalButton

VerticalButton::VerticalButton(const QString &text, QWidget *parent) : QAbstractButton(parent)
{
    setText(text);
    setFocusPolicy(Qt::NoFocus);
}

void VerticalButton::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const QRect r = rect();
    p.fillRect(r, Face);
    const bool down = isDown();
    if (down)
        draw::buttonPressed(&p, r);
    else
        draw::buttonRaised(&p, r);
    p.save();
    p.translate(r.width() / 2.0 + (down ? uiLine() : 0), r.height() / 2.0 + (down ? uiLine() : 0));
    p.rotate(-90);
    p.setPen(Text);
    const QFontMetrics fm(font());
    const int tw = fm.horizontalAdvance(text());
    p.drawText(QPoint(-tw / 2, fm.ascent() / 2 - 1), text());
    p.restore();
}

// ------------------------------------------------------------------------------------ LegendItem

LegendItem::LegendItem(const QString &colorKey, const QString &text, QWidget *parent)
    : QWidget(parent), m_key(colorKey), m_text(text)
{
    connect(Colors::instance(), &Colors::changed, this, qOverload<>(&QWidget::update));
}

QSize LegendItem::sizeHint() const
{
    return QSize(S(30 + 8) + fontMetrics().horizontalAdvance(m_text), S(16));
}

void LegendItem::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const QRect sw(0, (height() - S(9)) / 2, S(30), S(9));
    p.fillRect(sw, color(m_key));
    draw::edge(&p, sw, Text, Text);
    p.setPen(Text);
    draw::text(&p, QRect(S(38), 0, width() - S(38), height()), Qt::AlignLeft, m_text);
}

// --------------------------------------------------------------------------------------- NewsBar

NewsBar::NewsBar(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("newsBar"));
    connect(Colors::instance(), &Colors::changed, this, qOverload<>(&QWidget::update));
}

void NewsBar::setText(const QString &text, const QString &link)
{
    m_text = text;
    m_link = link;
    update();
}

void NewsBar::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const QRect r = rect();
    p.fillRect(draw::inset(r, 2), color(ckeys::NewsBackground));
    draw::newsEdge(&p, r);
    const QFont f = largeFont();
    p.setFont(f);
    const QFontMetrics fm(f);
    const QRect tr = r.adjusted(S(6), 2 * uiLine(), -S(4), -2 * uiLine());
    const QString t = fmt::elide(m_text, fm, tr.width());
    p.setPen(color(ckeys::NewsText));
    // explicit baseline (same on every font engine): measured, cap top 6 px below the bar's top;
    // [ui-e] scaled faces are centred by their metrics
    const int base = isScaled() ? tr.top() + (tr.height() - fm.height()) / 2 + fm.ascent() : tr.top() + tr.height() / 2 + 5;
    p.drawText(QPoint(tr.left(), base), t);
    m_linkRect = QRect();
    if (!m_link.isEmpty()) {
        const int x = tr.left() + fm.horizontalAdvance(t) + S(8);
        QFont lf = classicFont(11, false);
        lf.setUnderline(true);
        p.setFont(lf);
        p.setPen(color(ckeys::NewsLink));
        const QFontMetrics lfm(lf);
        m_linkRect = QRect(x, tr.top(), lfm.horizontalAdvance(m_link), tr.height());
        p.drawText(QPoint(x, base), m_link);
    }
}

void NewsBar::mousePressEvent(QMouseEvent *e)
{
    if (!m_link.isEmpty() && m_linkRect.contains(e->position().toPoint()))
        emit linkActivated(m_link);
}

// -------------------------------------------------------------------------------------- TabStrip

TabStrip::TabStrip(QWidget *parent) : QWidget(parent)
{
    setFixedHeight(S(21));
}

void TabStrip::setSlotGeometry(int slotWidth, int pitch, int minSlots, int bandWidth)
{
    m_slotWidth = slotWidth;
    m_pitch = pitch;
    m_minSlots = minSlots;
    m_band = bandWidth;
    relayout();
}

ThinButton *TabStrip::setLeadingButton(IconId icon, const QString &text)
{
    if (!m_leading) {
        m_leading = new ThinButton(icon, text, this);
        m_leading->setObjectName(QStringLiteral("tabLeading"));
        connect(m_leading, &QAbstractButton::clicked, this, &TabStrip::leadingClicked);
    }
    relayout();
    return m_leading;
}

int TabStrip::addTab(const QString &text, bool withIcon, IconId icon)
{
    ThinButton *b = withIcon ? new ThinButton(icon, text, this) : new ThinButton(text, this);
    b->setCentered(!withIcon);
    b->setCheckable(false);
    b->show();
    m_tabs.append(b);
    connect(b, &QAbstractButton::clicked, this, [this, b] { setCurrentIndex(m_tabs.indexOf(b)); });
    relayout();
    if (m_current < 0)
        setCurrentIndex(0);
    return m_tabs.size() - 1;
}

void TabStrip::removeTab(int index)
{
    if (index < 0 || index >= m_tabs.size())
        return;
    delete m_tabs.takeAt(index);
    if (m_tabs.isEmpty()) {
        m_current = -1;
    } else if (m_current >= m_tabs.size() || m_current > index) {
        m_current = qMax(0, m_current - 1);
    } else if (m_current == index) {
        m_current = qMin(index, int(m_tabs.size()) - 1);
    }
    for (int i = 0; i < m_tabs.size(); ++i)
        m_tabs[i]->setSelected(i == m_current);
    relayout();
    emit currentChanged(m_current);
}

void TabStrip::setTabText(int index, const QString &text)
{
    if (ThinButton *b = m_tabs.value(index))
        b->setText(text);
}

QString TabStrip::tabText(int index) const
{
    ThinButton *b = m_tabs.value(index);
    return b ? b->text() : QString();
}

void TabStrip::setCurrentIndex(int i)
{
    if (i < 0 || i >= m_tabs.size())
        return;
    for (int k = 0; k < m_tabs.size(); ++k)
        m_tabs[k]->setSelected(k == i);
    if (m_current != i) {
        m_current = i;
        emit currentChanged(i);
    }
}

void TabStrip::resizeEvent(QResizeEvent *)
{
    relayout();
}

void TabStrip::relayout()
{
    const int h = height();
    if (m_stretch) {
        const int n = m_tabs.size();
        if (!n)
            return;
        const int w = width() / n;
        for (int i = 0; i < n; ++i) {
            const int x = i * w;
            const int ww = (i == n - 1) ? width() - x : w - S(4);
            m_tabs[i]->setGeometry(x, 0, ww, h);
        }
        return;
    }
    const int lead = m_leading ? 1 : 0;
    const int n = m_tabs.size() + lead;
    const int nslots = qMax(n, m_minSlots);
    // slot geometry is in design pixels
    // [ui-e] a wider window offers more room before the slots shrink
    const int mPitch = S(m_pitch), mSlot = S(m_slotWidth), band = qMax(S(m_band), width() - S(2));
    int pitch = mPitch, sw = mSlot;
    if (nslots * mPitch > band + (mPitch - mSlot)) {
        pitch = qMax(S(70), (band + (mPitch - mSlot)) / nslots);
        sw = pitch - (mPitch - mSlot);
    }
    int x = 0;
    if (m_leading) {
        m_leading->setGeometry(x, 0, sw, h);
        x += pitch;
    }
    for (ThinButton *b : std::as_const(m_tabs)) {
        b->setGeometry(x, 0, sw, h);
        x += pitch;
    }
    // blank nslots
    const int blanks = qMax(0, nslots - n);
    while (m_blanks.size() < blanks) {
        auto *b = new ThinButton(QString(), this);
        b->setObjectName(QStringLiteral("tabBlank"));   // [ui-g] an empty slot, not a button to explain
        b->setBlank(true);
        b->show();
        m_blanks.append(b);
    }
    while (m_blanks.size() > blanks)
        delete m_blanks.takeLast();
    for (ThinButton *b : std::as_const(m_blanks)) {
        b->setGeometry(x, 0, sw, h);
        x += pitch;
    }
}

// -------------------------------------------------------------------------- ProportionalSplitter

void ProportionalSplitter::resizeEvent(QResizeEvent *e)
{
    const QList<int> before = sizes();
    qint64 total = 0;
    for (int v : before)
        total += v;
    QSplitter::resizeEvent(e);
    if (total <= 0 || before.size() < 2 || !isVisible())
        return;
    QList<int> now = sizes();
    qint64 avail = 0;
    for (int v : now)
        avail += v;
    if (avail <= 0)
        return;
    QList<int> out;
    qint64 cum = 0, given = 0;
    for (int i = 0; i < before.size(); ++i) {
        cum += before.at(i);
        const qint64 edge = i + 1 == before.size() ? avail : (cum * avail + total / 2) / total;
        out << int(edge - given);
        given = edge;
    }
    if (out != now)
        setSizes(out);
}

} // namespace mx::ui
