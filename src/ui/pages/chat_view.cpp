// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/pages/chat_view.h"

#include <QtCore/QRegularExpression>
#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>
#include <QtGui/QFontDatabase>
#include <QtGui/QKeyEvent>
#include <QtGui/QPainter>
#include <QtGui/QTextBlock>
#include <QtGui/QTextCursor>
#include <QtGui/QTextDocument>
#include <QtWidgets/QScrollBar>

#include "mx/services.h"
#include "mx/settings_keys.h"
#include "ui/kit/colors.h"
#include "ui/kit/draw.h"
#include "ui/kit/theme.h"
#include "ui/kit/scale.h"

namespace mx::ui {

QFont chatFont(mx::Settings *s)
{
    int pt = s ? s->i(keys::chat::FontSize) : 10;
    if (pt <= 0)
        pt = 10;
    const QString family = s ? s->s(keys::chat::FontFamily) : QString();
    QFont f = classicFont(qMax(8, qRound(pt * 96.0 / 72.0)));
    static const QStringList installed = QFontDatabase::families();
    if (!family.isEmpty() && installed.contains(family, Qt::CaseInsensitive))
        f.setFamily(family);
    return f;
}

QList<Segment> chatSegments(const ChatMessage &m, const ChatFormat &f)
{
    QList<Segment> segs;
    if (f.timestamps && m.time.isValid())
        segs << Segment{QLatin1Char('[') + m.time.toLocalTime().toString(f.timestampFormat) + QStringLiteral("] "),
                        color(keys::colors::ChatTimestamp), QString(keys::colors::ChatTimestamp)};
    const QString nick = m.from.name.isEmpty() ? m.from.id : m.from.name;
    switch (m.kind) {
    case ChatKind::Message:
    case ChatKind::Private: {
        const bool own = !f.ownName.isEmpty() && nick == f.ownName;
        segs << Segment{QLatin1Char('<') + nick + QLatin1Char('>'),
                        color(own ? keys::colors::ChatOwnName : keys::colors::ChatOtherName),
                        QString(own ? keys::colors::ChatOwnName : keys::colors::ChatOtherName)};
        segs << Segment{QLatin1Char(' ') + m.text, color(keys::colors::ChatText), QString(keys::colors::ChatText)};
        break;
    }
    case ChatKind::Action:
        segs << Segment{QStringLiteral("* %1 %2").arg(nick, m.text), color(keys::colors::ChatAction), QString(keys::colors::ChatAction)};
        break;
    case ChatKind::Join:
        segs << Segment{m.text.isEmpty() ? QObject::tr("%1 has entered").arg(nick) : m.text, color(keys::colors::ChatJoin), QString(keys::colors::ChatJoin)};
        break;
    case ChatKind::Part:
        segs << Segment{m.text.isEmpty() ? QObject::tr("%1 has left").arg(nick) : m.text, color(keys::colors::ChatPart), QString(keys::colors::ChatPart)};
        break;
    case ChatKind::Topic:
        segs << Segment{m.text, color(keys::colors::ChatTopic), QString(keys::colors::ChatTopic)};
        break;
    case ChatKind::Motd:
        segs << Segment{m.text, color(keys::colors::ChatMotd), QString(keys::colors::ChatMotd)};
        break;
    case ChatKind::Error:
        segs << Segment{m.text, color(keys::colors::ChatError), QString(keys::colors::ChatError)};
        break;
    case ChatKind::System:
    default:
        segs << Segment{m.text, color(keys::colors::ChatSystem), QString(keys::colors::ChatSystem)};
        break;
    }
    return segs;
}

// ------------------------------------------------------------------------------------ ChatOutput

namespace {
constexpr int kColorKeyProp = QTextFormat::UserProperty + 7;   // [set-1] QString: the colour setting
}

ChatOutput::ChatOutput(QWidget *parent) : QTextBrowser(parent)
{
    setFrameStyle(QFrame::StyledPanel | QFrame::Sunken);
    setLineWidth(2 * uiLine());
    setOpenLinks(false);
    setOpenExternalLinks(false);
    setUndoRedoEnabled(false);
    setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard | Qt::LinksAccessibleByMouse);
    document()->setDocumentMargin(S(2));
    document()->setMaximumBlockCount(1000);
    connect(this, &QTextBrowser::anchorClicked, this, [](const QUrl &u) {
        if (u.scheme() == QLatin1String("http") || u.scheme() == QLatin1String("https"))
            QDesktopServices::openUrl(u);
    });
    applyColors();
    connect(Colors::instance(), &Colors::changed, this, &ChatOutput::applyColors);
}

void ChatOutput::applyColors()
{
    QPalette p = palette();
    p.setColor(QPalette::Base, color(keys::colors::ChatBackground));
    p.setColor(QPalette::Text, color(keys::colors::ChatText));
    setPalette(p);
    // [set-1] the lines already shown take a changed colour too ("used at once in every window"):
    // every piece of text keeps the key of its colour
    for (QTextBlock b = document()->begin(); b.isValid(); b = b.next()) {
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment fr = it.fragment();
            const QString key = fr.charFormat().property(kColorKeyProp).toString();
            if (key.isEmpty() || fr.charFormat().foreground().color() == color(key))
                continue;
            QTextCursor c(document());
            c.setPosition(fr.position());
            c.setPosition(fr.position() + fr.length(), QTextCursor::KeepAnchor);
            QTextCharFormat f;
            f.setForeground(color(key));
            c.mergeCharFormat(f);
        }
    }
    viewport()->setAutoFillBackground(true);
    viewport()->update();
}

void ChatOutput::setMaxLines(int n)
{
    document()->setMaximumBlockCount(qMax(10, n));
}

int ChatOutput::lineCount() const
{
    const QTextDocument *d = document();
    if (d->blockCount() == 1 && d->firstBlock().length() <= 1)
        return 0;
    return d->blockCount();
}

void ChatOutput::clearView()
{
    clear();
}

void ChatOutput::appendLine(const QString &text, const QColor &c)
{
    appendSegments({Segment{text, c, QString()}});
}

void ChatOutput::appendLine(const QString &text, QLatin1StringView colorKey)
{
    appendSegments({Segment{text, color(colorKey), QString(colorKey)}});
}

void ChatOutput::appendSegments(const QList<Segment> &segs)
{
    static const QRegularExpression link(QStringLiteral("(https?://|www\\.)[^\\s<>\"]+"),
                                         QRegularExpression::CaseInsensitiveOption);
    QScrollBar *sb = verticalScrollBar();
    const bool atEnd = sb->value() >= sb->maximum() - 2;
    QTextCursor c(document());
    c.movePosition(QTextCursor::End);
    QTextBlockFormat bf;
    // wrapped lines are indented (hanging indent)
    bf.setLeftMargin(S(14));
    bf.setTextIndent(-S(14));
    // [qa-1] chat_room_window.png: 13-px glyphs on an 18-px line pitch (the RichEdit adds the font's 2-px
    // external leading); Qt's line height for the face is 16, so add 2 px between lines
    bf.setLineHeight(S(2), QTextBlockFormat::LineDistanceHeight);
    if (lineCount() == 0)
        c.setBlockFormat(bf);
    else
        c.insertBlock(bf);
    for (const Segment &s : segs) {
        QTextCharFormat cf;
        cf.setForeground(s.color.isValid() ? s.color : color(keys::colors::ChatText));
        cf.setProperty(kColorKeyProp, s.key.isEmpty() && !s.color.isValid() ? QString(keys::colors::ChatText) : s.key);
        int pos = 0;
        QRegularExpressionMatchIterator it = link.globalMatch(s.text);
        while (it.hasNext()) {
            const QRegularExpressionMatch mt = it.next();
            if (mt.capturedStart() > pos)
                c.insertText(s.text.mid(pos, mt.capturedStart() - pos), cf);
            QString url = mt.captured(0);
            while (!url.isEmpty() && QStringLiteral(".,;:!?)]").contains(url.back()))
                url.chop(1);
            QTextCharFormat lf = cf;
            lf.setForeground(color(keys::colors::ChatLink));
            lf.setProperty(kColorKeyProp, QString(keys::colors::ChatLink));
            lf.setFontUnderline(true);
            lf.setAnchor(true);
            lf.setAnchorHref(url.startsWith(QLatin1String("www."), Qt::CaseInsensitive) ? QStringLiteral("http://") + url : url);
            c.insertText(url, lf);
            pos = int(mt.capturedStart() + url.size());
        }
        if (pos < s.text.size())
            c.insertText(s.text.mid(pos), cf);
    }
    if (atEnd)
        sb->setValue(sb->maximum());
}

void ChatOutput::resizeEvent(QResizeEvent *e)
{
    QScrollBar *sb = verticalScrollBar();
    const bool atEnd = sb->value() >= sb->maximum() - 2;
    QTextBrowser::resizeEvent(e);
    if (atEnd)
        sb->setValue(sb->maximum());
}

// ------------------------------------------------------------------------------------- ChatInput

ChatInput::ChatInput(QWidget *parent) : QLineEdit(parent)
{
    setMaxLength(400);
    applyColors();
    connect(Colors::instance(), &Colors::changed, this, &ChatInput::applyColors);
}

void ChatInput::applyColors()
{
    QPalette p = palette();
    p.setColor(QPalette::Base, color(keys::colors::ChatInputBackground));
    p.setColor(QPalette::Text, color(keys::colors::ChatInputText));
    setPalette(p);
}

void ChatInput::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
        const QString t = text();
        if (!t.trimmed().isEmpty()) {
            m_history.removeAll(t);
            m_history << t;
            while (m_history.size() > 50)
                m_history.removeFirst();
            m_pos = int(m_history.size());
            clear();
            emit submitted(t);
        }
        return;
    }
    if (e->key() == Qt::Key_Up && !m_history.isEmpty()) {
        m_pos = qMax(0, m_pos - 1);
        setText(m_history.at(m_pos));
        return;
    }
    if (e->key() == Qt::Key_Down && !m_history.isEmpty()) {
        m_pos = qMin(int(m_history.size()), m_pos + 1);
        setText(m_pos < m_history.size() ? m_history.at(m_pos) : QString());
        return;
    }
    QLineEdit::keyPressEvent(e);
}

// ------------------------------------------------------------------------------------------ Well

Well::Well(QWidget *child, QWidget *parent) : QWidget(parent), m_child(child)
{
    child->setParent(this);
    child->show();
    setObjectName(child->objectName() + QStringLiteral("Well"));
}

void Well::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    draw::fieldSunken(&p, rect());
}

void Well::resizeEvent(QResizeEvent *)
{
    m_child->setGeometry(draw::inset(rect(), 2));
}

} // namespace mx::ui
