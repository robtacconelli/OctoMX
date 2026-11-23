// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI [ui-g]: the "Filter results" rules and the always visible filter row of a list.
//
// Rules (TextFilter): words separated by spaces must all appear (in any order), `-word` must not
// appear, "a phrase" in quotes is one term (also -"a phrase"), case does not matter, a term matches
// anywhere inside the text (so "BT" finds "BT+eD2k", "flac" finds "[FLAC]" and ".flac").
//
// FilterRow: `Filter results: [ text ] [Clear]  37 of 165 shown`, classic gray chrome, for a
// MxListView (MxListView::setFilterEdit). Esc in the field clears it, Ctrl+F focuses it.
#pragma once

#include <QtCore/QStringList>
#include <QtWidgets/QWidget>

class QLineEdit;

namespace mx::ui {

class MxListView;
class PushButton;
class StaticText;

class TextFilter {
public:
    TextFilter() = default;
    explicit TextFilter(const QString &query) { setQuery(query); }
    void setQuery(const QString &query);
    bool isEmpty() const { return m_need.isEmpty() && m_not.isEmpty(); }
    // true when `text` (for a row: its searchable cells joined with tabs) passes the filter
    bool matches(const QString &text) const;
    QStringList required() const { return m_need; }
    QStringList excluded() const { return m_not; }

private:
    QStringList m_need, m_not;
};

class FilterRow : public QWidget {
    Q_OBJECT
public:
    // `name` is the objectName of the text field; the label, button and count get name + "Label",
    // "Clear", "Count"
    FilterRow(const QString &label, const QString &name, QWidget *parent = nullptr);
    void attach(MxListView *list);
    QLineEdit *edit() const { return m_edit; }
    QString countText() const;
    // the width the row needs to show everything at the current scale (label, field, button, count)
    int preferredWidth() const;
    int minimumRowWidth() const;

protected:
    void resizeEvent(QResizeEvent *) override;

private:
    void showCount(int shown, int total);
    StaticText *m_label = nullptr;
    QLineEdit *m_edit = nullptr;
    PushButton *m_clear = nullptr;
    StaticText *m_count = nullptr;
};

} // namespace mx::ui
