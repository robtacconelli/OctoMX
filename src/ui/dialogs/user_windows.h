// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: the small per-user windows (UI_SPEC §5.1, §5.3, §5.4):
//
//   WhoisWindow            "Whois: <user>", black read-only info lines from
//                          mx::UserService::whois(), buttons Browse / Send Message / Close.
//   PrivateMessageWindow   "<user> - Private Message": black output + input line, buttons
//                          Browse / Whois / Add To Hotlist; one window per partner.
//   PrivateChatHub         listens to ChatService::messageReceived for private messages, keeps the
//                          conversation of the session per partner, opens a window for a new
//                          incoming message (messages/openWindowOnMessage), drops ignored users.
//   IgnoredWindow          the "Ignored Users" tool window: Add / Whois / Remove, the black
//                          #FFFFCC list, "<n> users in list".
//
// All are top-level windows owned by the MainWindow, deleted on close, geometry persisted under
// ui/dialogs/<name>.
#pragma once

#include <QtCore/QHash>
#include <QtCore/QPointer>
#include <QtGui/QTextCursor>
#include <QtWidgets/QDialog>
#include <QtWidgets/QWidget>

#include "mx/types.h"

class QComboBox;
class QLineEdit;

namespace mx {
class Core;
}

namespace mx::ui {

class ChatInput;
class ChatOutput;
class IgnoredModel;
class MainWindow;
class MxListView;
class PushButton;
class StatusBar;

// --------------------------------------------------------------------------------------- Whois

class WhoisView : public QWidget {
    Q_OBJECT
public:
    explicit WhoisView(QWidget *parent = nullptr);
    void setLines(const QList<QPair<QString, QString>> &lines, const QString &error = QString());
    QList<QPair<QString, QString>> lines() const { return m_lines; }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QList<QPair<QString, QString>> m_lines;
    QString m_error;
};

class WhoisWindow : public QWidget {
    Q_OBJECT
public:
    WhoisWindow(mx::Core *core, MainWindow *mw, const mx::UserRef &user, QWidget *parent = nullptr);
    mx::UserRef user() const { return m_user; }
    void request();
    bool hasResult() const { return m_hasResult; }
    WhoisView *view() const { return m_view; }
    // map a whois result (real core: lower-case keys; mock: the original WinMX labels) to "Label:" / value rows
    static QList<QPair<QString, QString>> format(const mx::UserRef &u, const QVariantMap &info, QString *error);

protected:
    void resizeEvent(QResizeEvent *) override;
    void closeEvent(QCloseEvent *) override;

private:
    mx::Core *m_core;
    MainWindow *m_main;
    mx::UserRef m_user;
    WhoisView *m_view;
    PushButton *m_browse, *m_message, *m_close;
    bool m_hasResult = false;
    quint32 m_requestId = 0;              // UserService::requestWhois id (0 = old core: match by user)
};

// ---------------------------------------------------------------------------- private messages

class PrivateMessageWindow : public QWidget {
    Q_OBJECT
public:
    PrivateMessageWindow(mx::Core *core, MainWindow *mw, const mx::UserRef &user, QWidget *parent = nullptr);
    mx::UserRef user() const { return m_user; }
    void addMessage(const mx::ChatMessage &m);
    void submit(const QString &text);
    // [ovl-2] delivery of a message we sent (status a PmDelivery): the line is recoloured (waiting:
    // grey, delivered: normal, not delivered: red) and a failure adds a red "Not delivered: ..." line
    void setDelivery(const QString &msgId, int status, const QString &detail);
    QString deliveryText() const { return m_deliveryText; }   // tests / scripts: last status shown
    ChatOutput *output() const { return m_out; }
    ChatInput *input() const { return m_input; }
    static QString windowTitleFor(const mx::UserRef &u);

protected:
    void resizeEvent(QResizeEvent *) override;
    void closeEvent(QCloseEvent *) override;

private:
    mx::Core *m_core;
    MainWindow *m_main;
    mx::UserRef m_user;
    ChatOutput *m_out;
    ChatInput *m_input;
    PushButton *m_browse, *m_whois, *m_hotlist;
    StatusBar *m_status;
    QHash<QString, QTextCursor> m_ownLines;   // [ovl-2] msg id -> the text of our line
    QString m_deliveryText;
};

class PrivateChatHub : public QObject {
    Q_OBJECT
public:
    // the hub of a main window (created on first use; ChatPage creates it at startup)
    static PrivateChatHub *instance(MainWindow *mw);
    PrivateMessageWindow *open(const mx::UserRef &u, bool activate);
    PrivateMessageWindow *window(const mx::UserRef &u) const;
    QList<mx::ChatMessage> history(const mx::UserRef &u) const { return m_history.value(u.key()); }
    QList<PrivateMessageWindow *> windows() const;

private:
    PrivateChatHub(mx::Core *core, MainWindow *mw);
    void onMessage(const mx::ChatMessage &m);
    void onDelivery(const mx::UserRef &to, const QString &msgId, int status, const QString &detail);
public:
    QPair<int, QString> delivery(const QString &msgId) const { return m_delivery.value(msgId, qMakePair(-1, QString())); }
private:
    QHash<QString, QPair<int, QString>> m_delivery;   // [ovl-2] msg id -> (PmDelivery, detail)
    QStringList m_deliveryOrder;
    mx::Core *m_core;
    MainWindow *m_main;
    QHash<QString, QList<mx::ChatMessage>> m_history;
    QHash<QString, QPointer<PrivateMessageWindow>> m_windows;
};

// ------------------------------------------------------------------------------------- Ignored

class AddIgnoreDialog : public QDialog {
    Q_OBJECT
public:
    explicit AddIgnoreDialog(QWidget *parent = nullptr);
    mx::UserRef user() const;

private:
    QLineEdit *m_name;
    QComboBox *m_net;
};

class IgnoredWindow : public QWidget {
    Q_OBJECT
public:
    IgnoredWindow(mx::Core *core, MainWindow *mw, QWidget *parent = nullptr);
    MxListView *list() const { return m_list; }
    IgnoredModel *model() const { return m_model; }
    StatusBar *statusBar() const { return m_status; }
    QList<mx::UserRef> selectedUsers() const;
    void removeSelected();
    void addUser();
    void reload();

protected:
    void resizeEvent(QResizeEvent *) override;
    void closeEvent(QCloseEvent *) override;

private:
    void updateButtons();
    mx::Core *m_core;
    MainWindow *m_main;
    PushButton *m_add, *m_whois, *m_remove;
    MxListView *m_list;
    IgnoredModel *m_model;
    StatusBar *m_status;
};

} // namespace mx::ui
