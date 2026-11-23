// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/kit/text_filter.h"

#include <QtWidgets/QLineEdit>

#include "ui/kit/format.h"
#include "ui/kit/list_view.h"
#include "ui/kit/scale.h"
#include "ui/kit/theme.h"
#include "ui/kit/widgets.h"

namespace mx::ui {

void TextFilter::setQuery(const QString &query)
{
    m_need.clear();
    m_not.clear();
    const QString q = query;
    int i = 0;
    const int n = int(q.size());
    while (i < n) {
        while (i < n && q.at(i).isSpace())
            ++i;
        if (i >= n)
            break;
        bool negate = false;
        if (q.at(i) == QLatin1Char('-') && i + 1 < n && !q.at(i + 1).isSpace()) {
            negate = true;
            ++i;
        }
        QString term;
        if (q.at(i) == QLatin1Char('"')) {
            const int end = int(q.indexOf(QLatin1Char('"'), i + 1));
            term = q.mid(i + 1, end < 0 ? -1 : end - i - 1).simplified();
            i = end < 0 ? n : end + 1;
        } else {
            const int start = i;
            while (i < n && !q.at(i).isSpace())
                ++i;
            term = q.mid(start, i - start);
            if (term == QLatin1String("-"))
                term.clear();   // a lone dash is no term
        }
        if (term.isEmpty())
            continue;
        (negate ? m_not : m_need) << term;
    }
}

bool TextFilter::matches(const QString &text) const
{
    for (const QString &t : m_need)
        if (!text.contains(t, Qt::CaseInsensitive))
            return false;
    for (const QString &t : m_not)
        if (text.contains(t, Qt::CaseInsensitive))
            return false;
    return true;
}

// ------------------------------------------------------------------------------------- FilterRow

FilterRow::FilterRow(const QString &label, const QString &name, QWidget *parent) : QWidget(parent)
{
    setObjectName(name + QStringLiteral("Row"));
    m_label = new StaticText(label, this);
    m_label->setObjectName(name + QStringLiteral("Label"));
    m_edit = new QLineEdit(this);
    m_edit->setObjectName(name);
    m_edit->setPlaceholderText(tr("words, -exclude, \"a phrase\""));
    m_edit->setToolTip(tr("Show only the results whose name, network or status contains every word typed here. "
                          "-word hides the rows with that word, \"two words\" finds a phrase. "
                          "Esc or Clear shows every result again; Ctrl+F comes here."));
    m_clear = new PushButton(tr("Clear"), this);
    m_clear->setObjectName(name + QStringLiteral("Clear"));
    m_clear->setToolTip(tr("Empty the filter field and show every result again"));
    m_count = new StaticText(QString(), this);
    m_count->setObjectName(name + QStringLiteral("Count"));
    connect(m_clear, &QPushButton::clicked, this, [this] {
        m_edit->clear();
        m_edit->setFocus();
    });
}

void FilterRow::attach(MxListView *list)
{
    list->setFilterEdit(m_edit);
    connect(list, &MxListView::filterApplied, this, &FilterRow::showCount);
    showCount(list->filterMatches(), list->filterTotal());
}

void FilterRow::showCount(int shown, int total)
{
    m_count->setText(m_edit->text().trimmed().isEmpty()
                         ? QString()
                         : tr("%1 of %2 shown").arg(fmt::number(shown), fmt::number(total)));
    m_clear->setEnabled(!m_edit->text().isEmpty());
}

QString FilterRow::countText() const
{
    return m_count->text();
}

int FilterRow::minimumRowWidth() const
{
    const QFontMetrics fm(normalFont());
    return fm.horizontalAdvance(m_label->text()) + S(6) + S(140) + S(4) + S(45);
}

int FilterRow::preferredWidth() const
{
    const QFontMetrics fm(normalFont());
    return fm.horizontalAdvance(m_label->text()) + S(6) + S(220) + S(4) + S(45) + S(8) +
           fm.horizontalAdvance(tr("%1 of %2 shown").arg(fmt::number(9999), fmt::number(9999))) + S(4);
}

void FilterRow::resizeEvent(QResizeEvent *)
{
    // Filter results: [edit .........] [Clear]  37 of 165 shown
    const QFontMetrics fm(normalFont());
    const int h = height(), fieldH = qMin(h, S(21)), y = (h - fieldH) / 2;
    const int lw = fm.horizontalAdvance(m_label->text()) + S(2);
    m_label->setGeometry(0, y + (fieldH - S(14)) / 2, lw, S(14));
    const int countW = fm.horizontalAdvance(tr("%1 of %2 shown").arg(fmt::number(9999), fmt::number(9999))) + S(4);
    const int x0 = lw + S(4);
    int editW = width() - x0 - S(4) - S(45) - S(8) - countW;
    editW = qBound(S(100), editW, S(320));
    m_edit->setGeometry(x0, y, editW, fieldH);
    m_clear->setGeometry(x0 + editW + S(4), y, S(45), fieldH);
    const int cx = x0 + editW + S(4) + S(45) + S(8);
    m_count->setGeometry(cx, y + (fieldH - S(14)) / 2, qMax(0, width() - cx), S(14));
}

} // namespace mx::ui
