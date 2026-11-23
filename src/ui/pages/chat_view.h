// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: chat building blocks shared by the Chat page rooms and the private message
// windows (UI_SPEC §3.3.2, §5.4, colours §2.4):
//
//   ChatOutput   black, word-wrapped, selectable rich-text pane; one block per line with a hanging
//                indent for wrapped lines; coloured segments; clickable http(s) links (ChatLink
//                colour, underlined); auto-scroll only while at the bottom; backscroll limit.
//   ChatInput    the input line (Input Box colours), Enter = submitted(), Up/Down history,
//                400-character limit.
//   Well         a 2-px sunken frame around one push button: the look of the Chat list's
//                Filter / Join / Host / Close / Join Selected Channel / Refresh List / Favorites
//                buttons (measured on web/chat_channel_list_354b3.png).
//   chatSegments() turns a ChatMessage into coloured text ("<Nick> text", "* Nick does", joins ...).
#pragma once

#include <QtWidgets/QLineEdit>
#include <QtWidgets/QTextBrowser>

#include "mx/types.h"
#include "ui/models/roles.h"

namespace mx {
class Settings;
}

namespace mx::ui {

// The chat font (chat/fontFamily + chat/fontSize in points; the bundled UI face if the family is
// not installed), no antialiasing like the rest of the UI.
QFont chatFont(mx::Settings *s);

struct ChatFormat {
    QString ownName;
    bool timestamps = false;
    QString timestampFormat = QStringLiteral("hh:mm");
};
QList<Segment> chatSegments(const mx::ChatMessage &m, const ChatFormat &f);

class ChatOutput : public QTextBrowser {
    Q_OBJECT
public:
    explicit ChatOutput(QWidget *parent = nullptr);
    // one line (block); text segments are scanned for http(s):// links
    void appendSegments(const QList<Segment> &segs);
    void appendLine(const QString &text, const QColor &c);
    void setMaxLines(int n);
    int lineCount() const;
    QString plainText() const { return toPlainText(); }
    void applyColors();
    // drop everything (Clear Window, /clr)
    void clearView();

protected:
    void resizeEvent(QResizeEvent *e) override;
};

class ChatInput : public QLineEdit {
    Q_OBJECT
public:
    explicit ChatInput(QWidget *parent = nullptr);
    void applyColors();
    QStringList history() const { return m_history; }

signals:
    void submitted(const QString &text);

protected:
    void keyPressEvent(QKeyEvent *e) override;

private:
    QStringList m_history;
    int m_pos = 0;
};

class Well : public QWidget {
    Q_OBJECT
public:
    Well(QWidget *child, QWidget *parent);
    QWidget *child() const { return m_child; }

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;

private:
    QWidget *m_child;
};

} // namespace mx::ui
