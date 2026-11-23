// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: the "OctoMX Decentralized Chat" warning shown the first time per session the Chat
// page is opened (UI_SPEC §3.3.1, reference web/dialog_chat_warning.png). Text verbatim, centred,
// wrapped exactly like the original; Continue (default) / Cancel. [ui-h] "Do not show this warning
// again" (ui/chatWarningDisabled, stored when Continue is pressed with the box ticked).
#pragma once

#include <QtWidgets/QDialog>

class QCheckBox;

namespace mx::ui {

class ChatWarningDialog : public QDialog {
    Q_OBJECT
public:
    explicit ChatWarningDialog(QWidget *parent = nullptr);
    static QStringList lines();
    bool doNotShowAgain() const;
    QCheckBox *doNotShowBox() const { return m_again; }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QCheckBox *m_again = nullptr;
};

} // namespace mx::ui
