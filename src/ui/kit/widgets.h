// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: the small classic widgets every page uses (UI_SPEC §1.3-§1.5).
//
//   ThinButton    1-px raised "thin button" (page buttons, top-row buttons, toolbars, sub-tabs);
//                 checkable: the checked one is drawn sunken with a white face (= current page).
//   MenuButton    push button that pops a menu of exclusive choices ("Any Connection" ...);
//                 its text is the current choice's label.
//   VerticalButton push button with text rotated 90° (the Search page "CLEAR" button).
//   LegendItem    30×9 colour swatch with a 1-px black border + label (Search legend row).
//   NewsBar       black strip with bold yellow text (row A), links in magenta.
//   TabStrip      row of thin buttons acting as tabs (Search / Chat row C, Networks sub-tabs).
//   PushButton    QPushButton with fixed classic height and no focus-steal oddities.
#pragma once

#include <functional>

#include <QtCore/QVariant>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QWidget>

#include "ui/kit/icons.h"

class QMenu;
class QActionGroup;

namespace mx::ui {

class ThinButton : public QAbstractButton {
    Q_OBJECT
public:
    explicit ThinButton(const QString &text, QWidget *parent = nullptr);
    ThinButton(IconId icon, const QString &text, QWidget *parent = nullptr);

    void setCentered(bool c) { m_centered = c; update(); }
    bool isCentered() const { return m_centered; }
    // draw selected (white, sunken), independent of the checked state for the Hotlist toggle
    void setSelected(bool s);
    bool isSelected() const { return m_selected || isChecked(); }
    // blank slot (unused tab-strip slot): no text, not clickable
    void setBlank(bool b) { m_blank = b; setEnabled(!b); update(); }
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;

private:
    bool m_centered = false;
    bool m_selected = false;
    bool m_blank = false;
    bool m_hasIcon = false;
    IconId m_icon = IconId::App;
};

// Multi-line static text wrapped like Win32 DrawText(DT_WORDBREAK): greedy word wrap, fixed line
// pitch (13 px for Tahoma 8 pt, which Qt's own QLabel cannot reproduce), runs of spaces kept.
class WrapLabel : public QWidget {
    Q_OBJECT
public:
    explicit WrapLabel(const QString &text = QString(), QWidget *parent = nullptr);
    void setText(const QString &t) { m_text = t; updateGeometry(); update(); }   // [qa-2] layouts re-measure
    QString text() const { return m_text; }
    void setLinePitch(int px) { m_pitch = px; update(); }
    QStringList wrappedLines() const;
    int heightForWidth(int w) const override;
    bool hasHeightForWidth() const override { return true; }
    int linePitch() const;   // effective pitch in device pixels
    // [qa-2] the font, pitch and lines the label draws with; false = the text does not fit its height
    // even after the scaled-mode shrink (the last lines are cut)
    bool fitText(QFont *font, int *pitch, QStringList *lines) const;

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QStringList wrap(int width) const;
    QStringList wrapWith(const QFontMetrics &fm, int width) const;
    QString m_text;
    int m_pitch = 13;
};

// Single-line static text drawn at the explicit classic WinMX baseline (draw::text); use instead of a
// plain QLabel for one-line captions so Linux and Windows render identically.
class StaticText : public QWidget {
    Q_OBJECT
public:
    explicit StaticText(const QString &text = QString(), QWidget *parent = nullptr);
    void setText(const QString &t) { m_text = t; update(); }
    QString text() const { return m_text; }
    void setAlignment(int hAlign) { m_align = hAlign; update(); }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QString m_text;
    int m_align = Qt::AlignLeft;
};

class PushButton : public QPushButton {
    Q_OBJECT
public:
    explicit PushButton(const QString &text, QWidget *parent = nullptr);
};

class MenuButton : public PushButton {
    Q_OBJECT
public:
    explicit MenuButton(QWidget *parent = nullptr);
    // Adds an exclusive choice; `label` is the menu text, `buttonText` the text shown on the
    // button when selected (defaults to the label).
    QAction *addChoice(const QString &label, const QVariant &data, const QString &buttonText = QString());
    QMenu *addSubmenu(const QString &title);                  // nested choices: use addChoiceTo()
    QAction *addChoiceTo(QMenu *menu, const QString &label, const QVariant &data, const QString &buttonText = QString());
    void addSeparator();
    QVariant currentData() const { return m_current; }
    void setCurrentData(const QVariant &d);                   // no signal
    QMenu *menu() const { return m_menu; }
    // pop the menu below the button (what a click does)
    void popup();
    // [ui-f] optional: the button text for the current choice and the room (px) the button has,
    // applied on every choice and resize (e.g. fewer extensions instead of a clipped label)
    using TextFitter = std::function<QString(const QVariant &data, const QFontMetrics &fm, int room)>;
    void setTextFitter(TextFitter fitter);
    void refitText();

signals:
    void choiceChanged(const QVariant &data);

protected:
    void resizeEvent(QResizeEvent *e) override;

private:
    void choose(QAction *a, bool emitSignal);
    QMenu *m_menu;
    QActionGroup *m_group;
    QVariant m_current;
    TextFitter m_fitter;
};

class VerticalButton : public QAbstractButton {
    Q_OBJECT
public:
    explicit VerticalButton(const QString &text, QWidget *parent = nullptr);

protected:
    void paintEvent(QPaintEvent *) override;
};

class LegendItem : public QWidget {
    Q_OBJECT
public:
    LegendItem(const QString &colorKey, const QString &text, QWidget *parent = nullptr);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QString m_key;
    QString m_text;
};

class NewsBar : public QWidget {
    Q_OBJECT
public:
    explicit NewsBar(QWidget *parent = nullptr);
    void setText(const QString &text, const QString &link = QString());
    QString text() const { return m_text; }

signals:
    void linkActivated(const QString &link);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;

private:
    QString m_text;
    QString m_link;
    QRect m_linkRect;
};

// A row of tab-like thin buttons. Tabs are left-aligned in fixed slots
// (slotWidth, pitch); with too many tabs the slots shrink (min 70 px).
class TabStrip : public QWidget {
    Q_OBJECT
public:
    explicit TabStrip(QWidget *parent = nullptr);
    void setSlotGeometry(int slotWidth, int pitch, int minSlots = 0, int bandWidth = 682);
    // stretch mode: n tabs share the full width equally (Networks sub-tabs)
    void setStretch(bool on) { m_stretch = on; relayout(); }
    // A leading command button that is not a tab (e.g. "Open New Search").
    ThinButton *setLeadingButton(IconId icon, const QString &text);
    int addTab(const QString &text, bool withIcon = false, IconId icon = IconId::SearchTab);
    void removeTab(int index);
    void setTabText(int index, const QString &text);
    QString tabText(int index) const;
    int count() const { return m_tabs.size(); }
    int currentIndex() const { return m_current; }
    void setCurrentIndex(int i);
    ThinButton *tabButton(int i) const { return m_tabs.value(i); }
    ThinButton *leadingButton() const { return m_leading; }

signals:
    void currentChanged(int index);
    void leadingClicked();

protected:
    void resizeEvent(QResizeEvent *) override;

private:
    void relayout();
    QList<ThinButton *> m_tabs;
    QList<ThinButton *> m_blanks;
    ThinButton *m_leading = nullptr;
    int m_current = -1;
    int m_slotWidth = 166, m_pitch = 171, m_minSlots = 0, m_band = 682;
    bool m_stretch = false;
};

// [ui-e] a splitter whose panes keep their proportions when the window is resized (QSplitter alone
// spreads the change by the stretch factors, so a pane the user made small grew back)
class ProportionalSplitter : public QSplitter {
    Q_OBJECT
public:
    using QSplitter::QSplitter;

protected:
    void resizeEvent(QResizeEvent *e) override;
};

} // namespace mx::ui
