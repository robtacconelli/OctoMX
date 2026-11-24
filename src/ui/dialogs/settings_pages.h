// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: internal helpers for building the Settings pages [ui-c]. Not a public API.
//
// PageKit places widgets at absolute positions given in *dialog client coordinates* (as measured
// on the reference screenshots: client = outer - (3, 22)); the page widget itself starts at
// x = SettingsDialog::kPageX. Form stacks rows from top to bottom for the pages that have no
// original (BitTorrent, eD2k, ...) using the same widgets, metrics and wording style.
#pragma once

#include <QtCore/QString>
#include <QtCore/QVariant>

#include "mx/settings_keys.h"
#include "ui/dialogs/settings_binding.h"
#include "ui/dialogs/settings_dialog.h"
#include "ui/kit/scale.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLineEdit;
class QRadioButton;
class QSpinBox;

namespace mx::ui {

class WrapLabel;
class StaticText;

class PageKit {
public:
    PageKit(SettingsDialog *d, QWidget *page);
    SettingsDialog *dlg;
    QWidget *page;
    SettingsBinder *b;
    mx::Settings *s;

    template <class W> W *place(W *w, int x, int y, int wd, int h)
    {
        w->setParent(page);
        // [ui-e] design coordinates -> device pixels (edges scaled in dialog coordinates)
        w->setGeometry(SR(x, y, wd, h).translated(-S(SettingsDialog::kPageX), 0));
        return w;
    }
    StaticText *label(const QString &t, int x, int y, int w, int align = 0x0001 /*Qt::AlignLeft*/, int h = 13);
    WrapLabel *wrap(const QString &t, int x, int y, int w, int h);
    QGroupBox *group(const QString &t, int x, int y, int w, int h);
    QCheckBox *check(const QString &t, const QString &key, int x, int y, int w, int h = 16);
    QRadioButton *radio(const QString &t, const QString &key, const QVariant &v, int x, int y, int w, int h = 16);
    QSpinBox *spin(const QString &key, int x, int y, int w = 59, int h = 20);
    QDoubleSpinBox *dspin(const QString &key, int x, int y, int w = 59, int h = 20);
    QLineEdit *edit(const QString &key, int x, int y, int w, int h = 20, SettingType fallback = SettingType::String);
    QComboBox *combo(const QString &key, int x, int y, int w, int h = 21);
    PushButton *button(const QString &t, int x, int y, int w, int h = 23);
    PushButton *toggle(const QString &t, const QString &key, int x, int y, int w, int h = 23);
    ColorButton *color(const QString &t, const QString &key, int x, int y, int w, int h = 17);
    void hline(int x, int y, int w);
    // object name for a bound control: "set_" + key with '/' -> '_'
    static QString nameFor(const QString &key);
};

// Row-by-row layout for the new pages.
class Form {
public:
    Form(PageKit &k, int x = 215, int y = 9, int w = 307);
    PageKit &k;
    int x0, y, w;
    int indent = 0;

    void group(const QString &title);            // starts a group box (ends with endGroup)
    void endGroup();
    QCheckBox *check(const QString &key, const QString &text = QString());
    // "<label> [spin] <unit>"; labelW = width reserved for the label
    QSpinBox *spin(const QString &key, const QString &text = QString(), const QString &unit = QString(), int labelW = 0);
    QDoubleSpinBox *dspin(const QString &key, const QString &text = QString(), const QString &unit = QString(), int labelW = 0);
    QComboBox *combo(const QString &key, const QString &text = QString(), int labelW = 0, int comboW = 0);
    // label on its own line, edit below (full width)
    QLineEdit *edit(const QString &key, const QString &text = QString(), SettingType fallback = SettingType::String);
    // "<label> [edit]" on one line
    QLineEdit *editRow(const QString &key, const QString &text, int labelW, int editW = 0);
    WrapLabel *text(const QString &t, int lines = 1);
    void gap(int px) { y += px; }
    int left() const { return x0 + indent; }
    int width() const { return w - 2 * indent; }

private:
    int m_groupTop = -1;
    QString m_groupTitle;
    QGroupBox *m_groupBox = nullptr;
};

QString settingLabel(const QString &key);   // schema label (with "(seconds)" etc. stripped)
// [ui-h] the tool tip of a setting (settings_tips.cpp): what it does and its default
QString settingTip(const QString &key);
bool hasSettingTip(const QString &key);   // a hand-written entry exists (tests)

// builders (called by SettingsDialog in tree order)
void buildAppearancePages(SettingsDialog *d);
void buildConnectionPages(SettingsDialog *d);
void buildPeerNetworkPages(SettingsDialog *d);
void buildOtherNetworkPages(SettingsDialog *d);
void buildStreamApiPage(SettingsDialog *d);   // [str-1] settings_stream.cpp (under Other Networks)
void buildMessagePages(SettingsDialog *d);
void buildLibraryPages(SettingsDialog *d);
void buildChatPages(SettingsDialog *d);
void buildSearchPages(SettingsDialog *d);
void buildTransferPages(SettingsDialog *d);
void buildMiscPages(SettingsDialog *d);
void buildAdvancedPage(SettingsDialog *d);

} // namespace mx::ui
