// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/dialogs/settings_dialog.h"

#include <QtGui/QCloseEvent>
#include <QtGui/QKeyEvent>
#include <QtGui/QPainter>
#include <QtWidgets/QApplication>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QStyledItemDelegate>
#include <QtWidgets/QTreeWidget>

#include "mx/services.h"
#include "mx/settings_keys.h"
#include "ui/dialogs/settings_binding.h"
#include "ui/dialogs/settings_pages.h"
#include "ui/kit/classic_style.h"
#include "ui/kit/colors.h"
#include "ui/kit/draw.h"
#include "ui/kit/icons.h"
#include "ui/kit/widgets.h"
#include "ui/kit/scale.h"
#include "ui/main_window.h"

namespace mx::ui {

namespace {

// Win32 tree look: the selection covers only the text (navy while the tree has the focus, button
// face when it does not), 16-px rows, text at the classic WinMX baseline.
class TreeDelegate : public QStyledItemDelegate {
public:
    TreeDelegate(QTreeWidget *tree) : QStyledItemDelegate(tree), m_tree(tree) {}
    QSize sizeHint(const QStyleOptionViewItem &opt, const QModelIndex &idx) const override
    {
        return QSize(QFontMetrics(opt.font).horizontalAdvance(idx.data().toString()) + S(6), S(16));
    }
    void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &idx) const override
    {
        const QString t = idx.data().toString();
        const int tw = QFontMetrics(opt.font).horizontalAdvance(t);
        const QRect r(opt.rect.left(), opt.rect.top(), tw + S(4), opt.rect.height());
        const bool sel = opt.state & QStyle::State_Selected;
        const bool focus = m_tree->hasFocus();
        p->save();
        p->setFont(opt.font);
        if (sel) {
            p->fillRect(r, focus ? chrome::MenuHighlight : chrome::Face);
            if (focus) {
                // dotted focus rectangle like Win32
                QStyleOptionFocusRect fr;
                fr.rect = r;
                fr.state = QStyle::State_KeyboardFocusChange;
                fr.backgroundColor = chrome::MenuHighlight;
                m_tree->style()->drawPrimitive(QStyle::PE_FrameFocusRect, &fr, p, m_tree);
            }
        }
        p->setPen(sel && focus ? chrome::Light : chrome::Text);
        draw::text(p, r.adjusted(S(2), 0, 0, 0), Qt::AlignLeft, t);
        p->restore();
    }

private:
    QTreeWidget *m_tree;
};

// Win32 trees do not paint the selection under the [+]/[-] box and the dotted lines
class TreeStyle : public ClassicStyle {
public:
    int styleHint(StyleHint h, const QStyleOption *opt, const QWidget *w, QStyleHintReturn *ret) const override
    {
        if (h == SH_ItemView_ShowDecorationSelected)
            return 0;
        return ClassicStyle::styleHint(h, opt, w, ret);
    }
};

} // namespace

SettingsDialog::SettingsDialog(mx::Core *core, MainWindow *mw, QWidget *parent)
    : QDialog(parent), m_core(core), m_mw(mw)
{
    setObjectName(QStringLiteral("settingsDialog"));
    setWindowTitle(tr("Settings"));
    setWindowIcon(icon(IconId::Settings));
    setFixedSize(SS(kClientW, kClientH));
    setModal(false);
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);

    m_binder = new SettingsBinder(core->settings(), this);

    m_wizardBtn = new PushButton(tr("Setup Wizard..."), this);
    m_wizardBtn->setObjectName(QStringLiteral("settingsWizard"));
    m_wizardBtn->setToolTip(tr("Run the first-start questions again: screen name, connection, folders, ports and networks."));
    m_wizardBtn->setGeometry(SR(9, 9, 194, 23));
    connect(m_wizardBtn, &QPushButton::clicked, this, &SettingsDialog::wizardRequested);

    m_tree = new QTreeWidget(this);
    m_tree->setObjectName(QStringLiteral("settingsTree"));
    m_tree->setGeometry(SR(9, 40, 194, 267));
    m_tree->setHeaderHidden(true);
    m_tree->setIndentation(S(19));
    m_tree->setRootIsDecorated(true);
    m_tree->setUniformRowHeights(true);
    auto *treeStyle = new TreeStyle;
    treeStyle->setParent(m_tree);
    m_tree->setStyle(treeStyle);
    m_tree->setItemDelegate(new TreeDelegate(m_tree));
    m_tree->setFocusPolicy(Qt::StrongFocus);
    m_tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_tree->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    m_tree->setExpandsOnDoubleClick(true);

    m_closeBtn = new PushButton(tr("Close"), this);
    m_closeBtn->setObjectName(QStringLiteral("settingsClose"));
    m_closeBtn->setGeometry(SR(9, 316, 194, 23));
    connect(m_closeBtn, &QPushButton::clicked, this, [this] { close(); });

    m_stack = new QStackedWidget(this);
    m_stack->setObjectName(QStringLiteral("settingsStack"));
    m_stack->setGeometry(S(kPageX), 0, S(kClientW) - S(kPageX), S(kClientH));

    // tree order = the original order, new pages next to their closest relative
    buildAppearancePages(this);
    buildConnectionPages(this);
    buildPeerNetworkPages(this);
    buildOtherNetworkPages(this);
    buildMessagePages(this);
    buildLibraryPages(this);
    buildChatPages(this);
    buildSearchPages(this);
    buildTransferPages(this);
    buildMiscPages(this);
    buildAdvancedPage(this);   // last: lists what the others did not bind

    // initial expansion as in the references: Internet Connection and File Transfers open
    for (const char *id : {"connection", "transfers"})
        if (QTreeWidgetItem *it = m_items.value(QString::fromLatin1(id)))
            it->setExpanded(true);

    connect(m_tree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *it) {
        if (!it)
            return;
        const QString id = it->data(0, Qt::UserRole).toString();
        if (QWidget *p = m_pages.value(id)) {
            m_stack->setCurrentWidget(p);
            m_core->settings()->setValue(keys::ui::SettingsPage, id);
            emit pageShown(id);
        }
    });

    const QString last = m_core->settings()->s(keys::ui::SettingsPage);
    if (!showPage(last))
        showPage(QStringLiteral("appearance"));
}

SettingsDialog::~SettingsDialog() = default;

QWidget *SettingsDialog::addPage(const QString &id, const QString &label, const QString &parentId)
{
    auto *page = new QWidget(m_stack);
    page->setObjectName(QStringLiteral("settingsPage_") + QString(id).replace(QLatin1Char('/'), QLatin1Char('_')));
    page->setProperty("mxPageId", id);
    m_stack->addWidget(page);
    QTreeWidgetItem *parentItem = m_items.value(parentId);
    auto *it = parentItem ? new QTreeWidgetItem(parentItem, {label}) : new QTreeWidgetItem(m_tree, {label});
    it->setData(0, Qt::UserRole, id);
    m_pages.insert(id, page);
    m_items.insert(id, it);
    m_labels.insert(id, label);
    m_order << id;
    return page;
}

QString SettingsDialog::resolvePage(const QString &idOrLabel) const
{
    const QString s = idOrLabel.trimmed();
    if (s.isEmpty())
        return QString();
    if (m_pages.contains(s))
        return s;
    for (const QString &id : m_order) {
        if (id.compare(s, Qt::CaseInsensitive) == 0 || m_labels.value(id).compare(s, Qt::CaseInsensitive) == 0)
            return id;
    }
    // aliases for the original names that were relabelled (UI_SPEC §8.9)
    static const QHash<QString, QString> alias = {
        {QStringLiteral("opennap networks"), QStringLiteral("other")},
        {QStringLiteral("colors"), QStringLiteral("appearance")},
        {QStringLiteral("octomx network"), QStringLiteral("wpn")},
        {QStringLiteral("incoming messages"), QStringLiteral("messages")},
        {QStringLiteral("privacy"), QStringLiteral("messages")},
        {QStringLiteral("bandwidth"), QStringLiteral("throttle")},
        {QStringLiteral("shared files"), QStringLiteral("library")},
        {QStringLiteral("chat"), QStringLiteral("channels")},
    };
    return alias.value(s.toLower());
}

bool SettingsDialog::showPage(const QString &idOrLabel)
{
    const QString id = resolvePage(idOrLabel);
    QTreeWidgetItem *it = m_items.value(id);
    if (!it)
        return false;
    QList<QTreeWidgetItem *> chain;
    for (QTreeWidgetItem *p = it->parent(); p; p = p->parent())
        chain << p;
    for (QTreeWidgetItem *p : std::as_const(m_autoExpanded))
        if (!chain.contains(p))
            p->setExpanded(false);
    m_autoExpanded.clear();
    for (QTreeWidgetItem *p : std::as_const(chain))
        if (!p->isExpanded()) {
            p->setExpanded(true);
            m_autoExpanded << p;
        }
    m_tree->setCurrentItem(it);
    m_tree->scrollToItem(it);
    m_stack->setCurrentWidget(m_pages.value(id));
    return true;
}

QString SettingsDialog::currentPageId() const
{
    QWidget *w = m_stack->currentWidget();
    return w ? w->property("mxPageId").toString() : QString();
}

QString SettingsDialog::pageLabel(const QString &id) const
{
    return m_labels.value(id);
}

QStringList SettingsDialog::pageKeys() const
{
    QStringList out;
    for (const QString &id : m_order) {
        if (id == QLatin1String("advanced"))
            continue;
        for (const QString &k : m_binder->keysOf(m_pages.value(id)))
            if (!out.contains(k))
                out << k;
    }
    out.sort();
    return out;
}

QStringList SettingsDialog::advancedKeys() const
{
    QWidget *adv = m_pages.value(QStringLiteral("advanced"));
    return adv ? m_binder->keysOf(adv) : QStringList();
}

QString SettingsDialog::restartMessage() const
{
    const QStringList keys = m_binder->restartPending();
    if (keys.isEmpty())
        return QString();
    QStringList names;
    for (const QString &k : keys) {
        const SettingDef *d = findSetting(k);
        names << QStringLiteral("    ") + (d && !d->label.isEmpty() ? d->label : k);
    }
    return tr("THESE SETTINGS TAKE EFFECT WHEN OCTOMX STARTS AGAIN:\n\n%1").arg(names.join(QLatin1Char('\n')));
}

void SettingsDialog::finish()
{
    if (m_finishing)
        return;
    m_finishing = true;
    // an edit that still has the focus commits on focus loss
    if (QWidget *f = focusWidget())
        f->clearFocus();
    m_tree->setFocus();
    const QString msg = restartMessage();
    if (!msg.isEmpty()) {
        m_binder->clearRestartPending();
        SettingsBinder::message(this, tr("OctoMX"), msg);
    }
    m_core->settings()->sync();
    m_finishing = false;
}

void SettingsDialog::closeEvent(QCloseEvent *e)
{
    finish();
    e->accept();
}

void SettingsDialog::reject()
{
    finish();
    QDialog::reject();
}

void SettingsDialog::keyPressEvent(QKeyEvent *e)
{
    // Enter in an edit commits it; it must not close the dialog
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
        if (QWidget *f = focusWidget())
            f->clearFocus();
        e->accept();
        return;
    }
    QDialog::keyPressEvent(e);
}

} // namespace mx::ui
