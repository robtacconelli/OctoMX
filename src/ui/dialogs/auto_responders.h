// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: [qa-1] "Auto Responders" tool window and "New Auto Responder" dialog
// (web/dialog_auto_responders.png, web/dialog_new_auto_responder.png) and the Hotlist Options ▸
// Auto Responder submenu. Responders are stored in ui/autoResponders (name -> text, in order) and
// ui/autoResponderActive; choosing one writes messages/autoReply + messages/autoReplyText, which the
// core's chat manager uses to answer new private messages.
#pragma once

#include <QtWidgets/QDialog>

#include <functional>

class QLineEdit;
class QListWidget;
class QMenu;
class QPlainTextEdit;

namespace mx {
class Settings;
}

namespace mx::ui {

class NewAutoResponderDialog : public QDialog {
    Q_OBJECT
public:
    explicit NewAutoResponderDialog(QWidget *parent = nullptr);
    QString title() const;

private:
    QLineEdit *m_edit = nullptr;
};

class AutoRespondersWindow : public QDialog {
    Q_OBJECT
public:
    explicit AutoRespondersWindow(mx::Settings *settings, QWidget *parent = nullptr);
    // test hook: replaces the "New Auto Responder" prompt (returns the title, empty = cancel)
    static std::function<QString()> s_titleHook;

    // stored responders, in order: (name, text)
    static QList<QPair<QString, QString>> responders(mx::Settings *s);
    static void setResponders(mx::Settings *s, const QList<QPair<QString, QString>> &list);
    // choose the active responder ("" = off): updates messages/autoReply + autoReplyText
    static void activate(mx::Settings *s, const QString &name);
    // fill a menu: Off / <names> (radio) / - / Edit Auto Responders...
    static void fillMenu(QMenu *menu, mx::Settings *s, const std::function<void()> &edit);

protected:
    void resizeEvent(QResizeEvent *e) override;

private:
    void reload(const QString &select = QString());
    void insertNew();
    void removeSelected();
    void textEdited();

    mx::Settings *m_settings;
    QListWidget *m_list = nullptr;
    QPlainTextEdit *m_text = nullptr;
    bool m_loading = false;
    std::function<void()> m_layout;
};

} // namespace mx::ui
