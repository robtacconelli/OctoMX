// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: the terms of use, shown once per profile at the first start (and again when the text
// changes): the program is a general tool for exchanging legal content, the user is responsible for
// what they exchange, the authors are not. "I Agree" is enabled by the check box; "Exit" leaves the
// program before any network starts. Help ▸ Terms of Use... shows the same text with a Close button.
#pragma once

#include <QtWidgets/QDialog>

class QCheckBox;

namespace mx {
class Settings;
}

namespace mx::ui {

class PushButton;

class LegalNotice : public QDialog {
    Q_OBJECT
public:
    // raise this when the text changes in substance: every profile is asked again
    static constexpr int kTermsVersion = 1;

    explicit LegalNotice(bool readOnly = false, QWidget *parent = nullptr);

    // the paragraphs of the notice, in order (also used by the tests)
    static QStringList paragraphs();
    static bool accepted(const Settings *settings);
    // true when the terms are (or now have been) accepted; false = the user chose Exit
    static bool ensureAccepted(Settings *settings, QWidget *parent = nullptr);

private:
    QCheckBox *m_check = nullptr;
    PushButton *m_agree = nullptr;
};

} // namespace mx::ui
