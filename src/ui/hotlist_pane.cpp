// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/hotlist_pane.h"

#include "ui/dialogs/auto_responders.h"

#include <QtWidgets/QHeaderView>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>

#include "mx/services.h"
#include "mx/settings_keys.h"
#include "ui/kit/tips.h"
#include "ui/kit/colors.h"
#include "ui/kit/theme.h"
#include "ui/kit/list_view.h"
#include "ui/kit/widgets.h"
#include "ui/kit/scale.h"
#include "ui/main_window.h"
#include "ui/models/hotlist_model.h"
#include "ui/models/roles.h"

namespace mx::ui {

namespace {
const QString kConfirmRemove = QStringLiteral("ui/hotlistConfirmRemove");
}

HotlistPane::HotlistPane(mx::Core *core, MainWindow *mw, QWidget *parent) : QWidget(parent), m_core(core), m_main(mw)
{
    setObjectName(QStringLiteral("hotlistPane"));
    setAutoFillBackground(true);
    setMinimumWidth(S(120));
    m_options = new PushButton(tr("Hotlist Options"), this);
    m_options->setObjectName(QStringLiteral("hotlistOptions"));
    connect(m_options, &QPushButton::clicked, this, &HotlistPane::showOptionsMenu);
    m_options->setToolTip(tr("Add a user by name, the auto responder, privacy, and how the Hotlist behaves"));

    m_list = new MxListView(this);
    m_list->setObjectName(QStringLiteral("hotlistList"));
    m_list->setHeaderHidden(true);
    m_list->setSortingEnabled(false);
    // [qa-1] entries are indented 22 px under their section rule (hotlist_temporary_and_button.png:
    // list edge x=145, names start at x=167); tree mode without boxes/lines gives exactly that margin
    m_list->setTreeMode(true);
    m_list->setTreeGeometry(5, 22, false, 2);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_model = new HotlistModel(core->users(), core->chat(), this);
    m_model->setOwnName(core->network()->screenName());
    connect(core->network(), &NetworkService::screenNameChanged, this, [this](const QString &n) { m_model->setOwnName(n); });
    m_list->setModel(m_model);
    m_list->header()->setStretchLastSection(true);
    m_list->setStateKey(core->settings(), QStringLiteral("hotlist"));
    // [ui-g] an empty Hotlist says how to fill it (the owner asked how it works)
    m_hint = new QLabel(tr("Right-click a user in a chat room, a search result or Transfers and choose "
                           "Add To Hotlist. You then see here when they are online and can message or browse them."),
                        m_list->viewport());
    m_hint->setObjectName(QStringLiteral("hotlistHint"));
    m_hint->setWordWrap(true);
    m_hint->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_hint->setFont(normalFont());
    auto hintColors = [this] {
        QPalette p = m_hint->palette();
        p.setColor(QPalette::WindowText, color(ckeys::ListInactive));
        m_hint->setPalette(p);
    };
    hintColors();
    connect(Colors::instance(), &Colors::changed, m_hint, hintColors);
    connect(m_model, &QAbstractItemModel::modelReset, this, &HotlistPane::updateHint);
    connect(m_model, &QAbstractItemModel::rowsInserted, this, &HotlistPane::updateHint);
    connect(m_model, &QAbstractItemModel::rowsRemoved, this, &HotlistPane::updateHint);
    updateHint();
    connect(m_list, &MxListView::rowActivated, this, [this](const QModelIndex &i) { activate(i.row()); });
    connect(m_list, &MxListView::deleteRequested, this, &HotlistPane::removeSelected);   // [qa-1] Del
    connect(m_list, &MxListView::contextMenuAt, this, [this](const QModelIndex &i, const QPoint &g) {
        if (i.isValid())
            showContextMenu(i.row(), g);
    });
}

void HotlistPane::openAutoResponders()
{
    if (!m_autoResponders) {
        m_autoResponders = new AutoRespondersWindow(m_core->settings(), window());
        m_autoResponders->setAttribute(Qt::WA_DeleteOnClose);
    }
    m_autoResponders->show();
    m_autoResponders->raise();
    m_autoResponders->activateWindow();
}

void HotlistPane::resizeEvent(QResizeEvent *)
{
    // pane: 19-px push button over the full width, then the list
    m_options->setGeometry(0, S(5), width(), S(19));
    m_list->setGeometry(0, S(27), width(), height() - S(27));
    updateHint();
}

void HotlistPane::updateHint()
{
    if (!m_hint)
        return;
    bool users = false;
    for (int r = 0; r < m_model->rowCount() && !users; ++r)
        users = m_model->kind(r) != HotlistModel::SectionRow;
    const QRect vr = m_list->viewport()->rect().adjusted(S(6), S(6), -S(6), -S(6));
    m_hint->setGeometry(vr.left(), vr.top(), vr.width(), qMax(S(14), m_hint->heightForWidth(vr.width())));
    m_hint->setVisible(!users);
}

void HotlistPane::activate(int row)
{
    const auto kind = m_model->kind(row);
    if (kind == HotlistModel::SectionRow)
        return;
    const UserRef u = m_model->user(row);
    if (m_core->settings()->b(keys::hotlist::AutoBrowse))
        m_main->openBrowse(u);
    else
        m_main->openPrivateMessage(u);
}

void HotlistPane::showContextMenu(int row, const QPoint &globalPos)
{
    const auto kind = m_model->kind(row);
    if (kind == HotlistModel::SectionRow)
        return;
    const UserRef u = m_model->user(row);
    auto *menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("hotlistMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    QAction *send = menu->addAction(tr("Send Message"), this, [this, u] { m_main->openPrivateMessage(u); });
    menu->setDefaultAction(send);
    menu->addAction(tr("Chat"), this, [this, u] { m_main->openChat(u); });
    menu->addSeparator();
    QAction *perm = menu->addAction(tr("Perm add to list"), this, [this, u] { m_core->users()->addToHotlist(u); });
    perm->setEnabled(kind == HotlistModel::TemporaryRow);
    menu->addSeparator();
    menu->addAction(tr("Browse"), this, [this, u] { m_main->openBrowse(u); });
    menu->addAction(tr("Whois"), this, [this, u] { m_main->openWhois(u); });
    menu->addSeparator();
    menu->addAction(tr("Ignore"), this, [this, u] { m_core->users()->ignore(u, true); });
    menu->addSeparator();
    QAction *ren = menu->addAction(tr("Rename on list"), this, [this, u] {
        bool ok = false;
        const QString n = QInputDialog::getText(this, tr("Rename on list"), tr("Display name:"), QLineEdit::Normal, u.name, &ok);
        if (ok)
            m_core->users()->setNote(u, n == u.name ? QString() : n);
    });
    ren->setEnabled(kind != HotlistModel::TemporaryRow);
    menu->addSeparator();
    // [qa-1] Remove acts on every selected entry (the clicked row is part of the selection)
    menu->addAction(tr("Remove"), this, [this, row] {
        // the clicked row is normally part of the selection (right click selects); otherwise only that row
        if (m_list->selectionModel()->isRowSelected(row, QModelIndex()))
            removeSelected();
        else
            removeRows({row});
    });
    tips::explainMenu(menu);   // [ui-g] tool tips + status bar text of the entries
    menu->popup(globalPos);
}

void HotlistPane::removeSelected()
{
    QList<int> rows;
    for (const QModelIndex &i : m_list->selectionModel()->selectedRows())
        rows << i.row();
    removeRows(rows);
}

void HotlistPane::removeRows(const QList<int> &rows)
{
    QList<QPair<UserRef, int>> victims;
    for (int r : rows) {
        const auto kind = m_model->kind(r);
        if (kind != HotlistModel::SectionRow)
            victims.append({m_model->user(r), int(kind)});
    }
    if (victims.isEmpty())
        return;
    const QString what = victims.size() == 1 ? victims.first().first.name : tr("%1 users").arg(victims.size());
    if (m_core->settings()->value(kConfirmRemove).toBool() &&
        QMessageBox::question(this, tr("Hotlist"), tr("Remove %1 from your hotlist?").arg(what),
                              QMessageBox::Ok | QMessageBox::Cancel) != QMessageBox::Ok)
        return;
    for (const auto &v : victims) {
        if (v.second == HotlistModel::TemporaryRow)
            m_model->removeTemporary(v.first);
        else
            m_core->users()->removeFromHotlist(v.first);
    }
}

void HotlistPane::showOptionsMenu()
{
    auto *menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("hotlistOptionsMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->addAction(tr("Add User..."), this, [this] {
        bool ok = false;
        const QString n = QInputDialog::getText(this, tr("Add User"), tr("Screen name of the user to add:"), QLineEdit::Normal, QString(), &ok);
        if (ok && !n.trimmed().isEmpty()) {
            UserRef u;
            u.net = Net::Overlay;
            u.id = n.trimmed();
            u.name = n.trimmed();
            m_core->users()->addToHotlist(u);
        }
    });
    menu->addSeparator();
    QMenu *ar = menu->addMenu(tr("Auto Responder"));
    ar->setObjectName(QStringLiteral("autoResponderMenu"));
    // [qa-1] Off / the user's responders / Edit Auto Responders... (dialog_auto_responders.png)
    AutoRespondersWindow::fillMenu(ar, m_core->settings(), [this] { openAutoResponders(); });
    menu->addSeparator();
    menu->addAction(tr("Privacy Level..."), this, [this] { m_main->openSettings(QStringLiteral("Incoming Msgs & Privacy")); });
    menu->addSeparator();
    QAction *confirm = menu->addAction(tr("Confirm Remove"));
    confirm->setCheckable(true);
    confirm->setChecked(m_core->settings()->value(kConfirmRemove).toBool());
    connect(confirm, &QAction::toggled, this, [this](bool on) { m_core->settings()->setValue(kConfirmRemove, on); });
    QAction *browse = menu->addAction(tr("Browse on Double-Click"));
    browse->setCheckable(true);
    browse->setChecked(m_core->settings()->b(keys::hotlist::AutoBrowse));
    connect(browse, &QAction::toggled, this, [this](bool on) { m_core->settings()->setValue(keys::hotlist::AutoBrowse, on); });
    menu->addSeparator();
    // display templates: not supported yet, the entries are shown disabled as in the original menu
    menu->addAction(tr("Change Temp User Display Template..."))->setEnabled(false);
    menu->addAction(tr("Change Offline User Display Template..."))->setEnabled(false);
    menu->addAction(tr("Change Online User Display Template..."))->setEnabled(false);
    tips::explainMenu(menu);   // [ui-g] tool tips + status bar text of the entries
    menu->popup(m_options->mapToGlobal(QPoint(0, m_options->height())));
}

} // namespace mx::ui
