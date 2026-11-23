// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/dialogs/auto_responders.h"

#include <QtGui/QActionGroup>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMenu>
#include <QtWidgets/QPlainTextEdit>

#include <functional>

#include "mx/services.h"
#include "mx/settings_keys.h"
#include "ui/kit/widgets.h"
#include "ui/kit/scale.h"

namespace mx::ui {

namespace {
const QString kList = QStringLiteral("ui/autoResponders");          // list of [name, text]
const QString kActive = QStringLiteral("ui/autoResponderActive");
}

// ------------------------------------------------------------------------ NewAutoResponderDialog

NewAutoResponderDialog::NewAutoResponderDialog(QWidget *parent) : QDialog(parent)
{
    setObjectName(QStringLiteral("newAutoResponderDialog"));
    setWindowTitle(tr("New Auto Responder"));
    setFixedSize(SS(287, 82));
    auto *l = new QLabel(tr("Enter a title for your new\nauto-responder"), this);
    l->setGeometry(SR(8, 8, 190, 28));
    m_edit = new QLineEdit(this);
    m_edit->setObjectName(QStringLiteral("autoResponderTitle"));
    m_edit->setToolTip(tr("A name for the new auto responder, so you find it in the list. At most 40 characters."));
    m_edit->setMaxLength(40);
    m_edit->setGeometry(SR(8, 48, 185, 21));
    auto *ok = new PushButton(tr("OK"), this);
    ok->setObjectName(QStringLiteral("dlgOk"));
    ok->setDefault(true);
    ok->setGeometry(SR(207, 8, 75, 23));
    connect(ok, &QPushButton::clicked, this, &QDialog::accept);
    auto *cancel = new PushButton(tr("Cancel"), this);
    cancel->setObjectName(QStringLiteral("dlgCancel"));
    cancel->setGeometry(SR(207, 37, 75, 23));
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
}

QString NewAutoResponderDialog::title() const
{
    return m_edit->text().trimmed();
}

// -------------------------------------------------------------------------- AutoRespondersWindow

std::function<QString()> AutoRespondersWindow::s_titleHook;

QList<QPair<QString, QString>> AutoRespondersWindow::responders(mx::Settings *s)
{
    QList<QPair<QString, QString>> out;
    for (const QVariant &v : s->value(kList).toList()) {
        const QStringList p = v.toStringList();
        if (p.size() == 2 && !p.at(0).isEmpty())
            out.append({p.at(0), p.at(1)});
    }
    return out;
}

void AutoRespondersWindow::setResponders(mx::Settings *s, const QList<QPair<QString, QString>> &list)
{
    QVariantList v;
    for (const auto &p : list)
        v.append(QStringList{p.first, p.second});
    s->setValue(kList, v);
}

void AutoRespondersWindow::activate(mx::Settings *s, const QString &name)
{
    QString text;
    bool found = false;
    for (const auto &p : responders(s))
        if (p.first == name) {
            text = p.second;
            found = true;
        }
    s->setValue(kActive, found ? name : QString());
    s->setValue(keys::messages::AutoReply, found);
    if (found)
        s->setValue(keys::messages::AutoReplyText, text);
}

void AutoRespondersWindow::fillMenu(QMenu *menu, mx::Settings *s, const std::function<void()> &edit)
{
    const QString active = s->b(keys::messages::AutoReply) ? s->value(kActive).toString() : QString();
    auto *group = new QActionGroup(menu);
    QAction *off = menu->addAction(tr("Off"), menu, [s] { activate(s, QString()); });
    off->setCheckable(true);
    off->setToolTip(tr("Send no automatic reply to private messages."));
    off->setStatusTip(off->toolTip());
    off->setChecked(active.isEmpty());
    group->addAction(off);
    for (const auto &p : responders(s)) {
        QAction *a = menu->addAction(p.first, menu, [s, n = p.first] { activate(s, n); });
        a->setCheckable(true);
        a->setToolTip(tr("Answer every private message with the text of this auto responder until you choose Off."));
        a->setStatusTip(a->toolTip());
        a->setChecked(p.first == active);
        group->addAction(a);
    }
    menu->addSeparator();
    QAction *ed = menu->addAction(tr("Edit Auto Responders..."), menu, edit);
    ed->setToolTip(tr("Open the list of auto responders to add, remove or change their texts."));
    ed->setStatusTip(ed->toolTip());
    menu->setToolTipsVisible(true);
}

AutoRespondersWindow::AutoRespondersWindow(mx::Settings *settings, QWidget *parent)
    : QDialog(parent), m_settings(settings)
{
    setObjectName(QStringLiteral("autoRespondersWindow"));
    setWindowTitle(tr("Auto Responders"));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    resize(SS(353, 190));
    setMinimumSize(SS(240, 140));

    auto *insert = new PushButton(tr("Insert New"), this);
    insert->setObjectName(QStringLiteral("autoResponderInsert"));
    insert->setToolTip(tr("Add a new auto responder: give it a title, then type the reply text in the box below."));
    connect(insert, &QPushButton::clicked, this, &AutoRespondersWindow::insertNew);
    auto *remove = new PushButton(tr("Remove Selected"), this);
    remove->setObjectName(QStringLiteral("autoResponderRemove"));
    remove->setToolTip(tr("Delete the selected auto responder. If it is the one in use, automatic replies are turned off."));
    connect(remove, &QPushButton::clicked, this, &AutoRespondersWindow::removeSelected);

    // classic white list / edit (the Auto Responders window is not one of the black classic WinMX lists)
    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("autoResponderList"));
    m_text = new QPlainTextEdit(this);
    m_text->setObjectName(QStringLiteral("autoResponderText"));
    connect(m_list, &QListWidget::currentRowChanged, this, [this, remove](int row) {
        remove->setEnabled(row >= 0);
        m_loading = true;
        const auto list = responders(m_settings);
        m_text->setPlainText(row >= 0 && row < list.size() ? list.at(row).second : QString());
        m_text->setEnabled(row >= 0);
        m_loading = false;
    });
    connect(m_text, &QPlainTextEdit::textChanged, this, &AutoRespondersWindow::textEdited);

    auto layoutAll = [this, insert, remove] {
        const int w = width(), h = height();
        const int one = S(1), top = S(23), gap = S(6);
        insert->setGeometry(0, 0, w / 2 - one, S(21));
        remove->setGeometry(w / 2 + one, 0, w - w / 2 - one, S(21));
        const int listH = qMax(S(40), (h - top) / 2 - S(2));
        m_list->setGeometry(0, top, w, listH);
        m_text->setGeometry(0, top + listH + gap, w, h - (top + listH + gap));
    };
    m_layout = layoutAll;
    reload();
    remove->setEnabled(m_list->currentRow() >= 0);
}

void AutoRespondersWindow::resizeEvent(QResizeEvent *e)
{
    QDialog::resizeEvent(e);
    if (m_layout)
        m_layout();
}

void AutoRespondersWindow::reload(const QString &select)
{
    m_loading = true;
    m_list->clear();
    int sel = -1, i = 0;
    for (const auto &p : responders(m_settings)) {
        m_list->addItem(p.first);
        if (p.first == select)
            sel = i;
        ++i;
    }
    m_loading = false;
    m_list->setCurrentRow(sel >= 0 ? sel : (m_list->count() ? 0 : -1));
    m_text->setEnabled(m_list->currentRow() >= 0);
}

void AutoRespondersWindow::insertNew()
{
    QString title;
    if (s_titleHook) {
        title = s_titleHook();
    } else {
        NewAutoResponderDialog dlg(this);
        if (dlg.exec() != QDialog::Accepted)
            return;
        title = dlg.title();
    }
    if (title.isEmpty())
        return;
    auto list = responders(m_settings);
    for (const auto &p : list)
        if (p.first.compare(title, Qt::CaseInsensitive) == 0) {
            reload(p.first);
            return;
        }
    list.append({title, QString()});
    setResponders(m_settings, list);
    reload(title);
    m_text->setFocus();
}

void AutoRespondersWindow::removeSelected()
{
    const int row = m_list->currentRow();
    auto list = responders(m_settings);
    if (row < 0 || row >= list.size())
        return;
    const QString name = list.at(row).first;
    list.removeAt(row);
    setResponders(m_settings, list);
    if (m_settings->value(kActive).toString() == name)
        activate(m_settings, QString());
    reload();
}

void AutoRespondersWindow::textEdited()
{
    if (m_loading)
        return;
    const int row = m_list->currentRow();
    auto list = responders(m_settings);
    if (row < 0 || row >= list.size())
        return;
    list[row].second = m_text->toPlainText();
    setResponders(m_settings, list);
    // editing the active responder changes the reply that is sent
    if (m_settings->b(keys::messages::AutoReply) && m_settings->value(kActive).toString() == list.at(row).first)
        m_settings->setValue(keys::messages::AutoReplyText, list.at(row).second);
}

} // namespace mx::ui
