// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/dialogs/close_prompt.h"

#include <QtWidgets/QCheckBox>
#include <QtWidgets/QRadioButton>

#include "ui/kit/icons.h"
#include "ui/kit/widgets.h"
#include "ui/kit/scale.h"

namespace mx::ui {

// geometry measured on reference/web/dialog_close_prompt_minimize_or_exit.png (client = outer - (3, 29))
ClosePrompt::ClosePrompt(bool downloadsRunning, QWidget *parent) : QDialog(parent)
{
    setObjectName(QStringLiteral("closePrompt"));
    setWindowTitle(tr("OctoMX"));
    setWindowIcon(appIcon());
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    const int extra = downloadsRunning ? 21 : 0;
    setFixedSize(SS(374, 214 + extra));

    auto *q = new WrapLabel(tr("Should OctoMX quit now, or keep running in the background?"), this);
    q->setGeometry(SR(51, 18, 300, 26));
    // [post-1] the wording: "Exit" first and preselected, the tray only when the user asks for it
    m_exit = new QRadioButton(tr("Exit OctoMX"), this);
    m_exit->setObjectName(QStringLiteral("closeExit"));
    m_exit->setToolTip(tr("Close OctoMX completely: transfers stop, you leave the networks and the chat."));
    m_exit->setGeometry(SR(64, 57, 280, 16));
    m_tray = new QRadioButton(tr("Keep running in the tray"), this);
    m_tray->setObjectName(QStringLiteral("closeTray"));
    m_tray->setToolTip(tr("Hide the window but keep downloading, sharing and chatting. Click the tray icon to show it again."));
    m_tray->setGeometry(SR(64, 78, 280, 16));
    if (downloadsRunning) {
        m_whenDone = new QRadioButton(tr("Exit when downloads have finished"), this);
        m_whenDone->setObjectName(QStringLiteral("closeWhenDone"));
        m_whenDone->setToolTip(tr("Hide to the tray now and exit by itself once the running downloads are complete."));
        m_whenDone->setGeometry(SR(64, 99, 280, 16));
    }
    m_exit->setChecked(true);
    auto *info = new WrapLabel(tr("To quit OctoMX completely later, right-click its icon in the tray, or use the small icon at the "
                                  "top left of the main window."),
                               this);
    info->setGeometry(SR(51, 109 + extra, 300, 40));
    m_dont = new QCheckBox(tr("Do not ask again"), this);
    m_dont->setObjectName(QStringLiteral("closeDontShow"));
    m_dont->setToolTip(tr("Remember Exit or Keep running in the tray for the close button. You can change it later in Settings, Miscellaneous."));
    m_dont->setGeometry(SR(132, 156 + extra, 150, 16));
    auto *ok = new PushButton(tr("OK"), this);
    ok->setObjectName(QStringLiteral("closeOk"));
    ok->setDefault(true);
    ok->setAutoDefault(true);
    ok->setGeometry(SR(150, 181 + extra, 75, 23));
    connect(ok, &QPushButton::clicked, this, &QDialog::accept);
}

ClosePrompt::Choice ClosePrompt::choice() const
{
    if (m_exit->isChecked())
        return ExitNow;
    if (m_whenDone && m_whenDone->isChecked())
        return ExitWhenDone;
    return MinimizeToTray;
}

bool ClosePrompt::dontShowAgain() const
{
    return m_dont->isChecked();
}

} // namespace mx::ui
