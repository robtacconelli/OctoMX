// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: "View Server Messages" (UI_SPEC §3.1.2): a floating window with the messages of one
// server (eD2k MOTD / OP_SERVERMESSAGE lines ...) or of a whole network, black text view in the
// Settings ▸ Appearance ▸ "Server Msgs" colours. Fed by NetworkService::serverMessages() and kept
// live by serverMessageReceived. Position and size persist (ui/dialogs/serverMessages).
#pragma once

#include <QtWidgets/QWidget>

#include "mx/types.h"

namespace mx {
class Core;
}

namespace mx::ui {

class ChatOutput;
class StatusBar;

class ServerMessagesWindow : public QWidget {
    Q_OBJECT
public:
    ServerMessagesWindow(mx::Core *core, mx::Net net, const QString &serverId, QWidget *parent = nullptr);
    int lineCount() const;
    QString plainText() const;

protected:
    void closeEvent(QCloseEvent *e) override;
    void moveEvent(QMoveEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;

private:
    bool wants(const mx::ServerMessage &m) const;
    void append(const mx::ServerMessage &m);
    void updateStatus();
    mx::Core *m_core;
    mx::Net m_net;
    QString m_serverId;
    ChatOutput *m_out;
    StatusBar *m_status;
    int m_count = 0;
    bool m_restored = false;
};

} // namespace mx::ui
