// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: the close prompt (UI_SPEC §5.5) [ui-c]: "Should OctoMX quit now, or keep running in
// the background?" with Exit OctoMX (preselected) / Keep
// running in the tray / (downloads running) Exit when downloads have finished, and "Do not ask
// again". [post-1] wording: the X never "minimises", it exits or keeps running in the tray.
#pragma once

#include <QtWidgets/QDialog>

class QCheckBox;
class QRadioButton;

namespace mx::ui {

class ClosePrompt : public QDialog {
    Q_OBJECT
public:
    enum Choice { MinimizeToTray, ExitNow, ExitWhenDone };
    static constexpr Choice KeepRunningInTray = MinimizeToTray;   // [post-1] the current name
    ClosePrompt(bool downloadsRunning, QWidget *parent = nullptr);
    Choice choice() const;
    bool dontShowAgain() const;

private:
    QRadioButton *m_tray;
    QRadioButton *m_exit;
    QRadioButton *m_whenDone = nullptr;
    QCheckBox *m_dont;
};

} // namespace mx::ui
