// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/pages/networks_page.h"
#include "ui/dialogs/connection_test.h"

#include <QtCore/QEvent>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QTextStream>
#include <QtGui/QPainter>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QStackedWidget>

#include "mx/services.h"
#include "mx/settings_keys.h"
#include "ui/dialogs/server_messages.h"
#include "ui/dialogs/simple_dialogs.h"
#include "ui/kit/tips.h"
#include "ui/kit/colors.h"
#include "ui/kit/list_view.h"
#include "ui/kit/theme.h"
#include "ui/kit/widgets.h"
#include "ui/kit/draw.h"
#include "ui/kit/scale.h"
#include "ui/main_window.h"
#include "ui/models/server_model.h"

namespace mx::ui {

// ------------------------------------------------------------------------------ StatusIndicator

StatusIndicator::StatusIndicator(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("statusIndicator"));
    setFixedSize(SS(32, 56));
    connect(Colors::instance(), &Colors::changed, this, qOverload<>(&QWidget::update));
}

void StatusIndicator::setState(int s)
{
    if (m_state == s)
        return;
    m_state = s;
    update();
}

void StatusIndicator::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), color(keys::colors::StatusLedOff));
    // lamp 24×13, inset 4 px from the sides; traffic-light positions (green at the bottom)
    const int lampH = S(13);
    int y = height() - S(4) - lampH;                     // green
    QColor c = color(keys::colors::StatusLedOn);
    if (m_state == 1) {
        y = (height() - lampH) / 2;
        c = color(ckeys::StatusLedConnecting);
    } else if (m_state == 0) {
        y = S(4);
        c = color(ckeys::StatusLedError);
    }
    const QRect lamp(S(4), y, width() - S(8), lampH);
    p.fillRect(lamp, c);
    draw::edge(&p, lamp, c.darker(200), c.darker(200));
}

// --------------------------------------------------------------------------------- NetworksPage

namespace {
constexpr int kColW = 476;   // the centred content column of the WPN sub-page
}

NetworksPage::NetworksPage(mx::Core *core, MainWindow *mw, QWidget *parent)
    : Page(QStringLiteral("networks"), tr("Networks"), core, mw, parent)
{
    m_tabs = new TabStrip(this);
    m_tabs->setObjectName(QStringLiteral("networksTabs"));
    m_tabs->setStretch(true);
    m_tabs->addTab(tr("OctoMX Peer Network"));
    m_tabs->addTab(tr("Other Protocol Networks"));
    m_tabs->tabButton(0)->setObjectName(QStringLiteral("subtabWpn"));
    m_tabs->tabButton(1)->setObjectName(QStringLiteral("subtabOther"));
    m_tabs->tabButton(0)->setToolTip(tr("The OctoMX peer network: connection status, screen name and connection type"));
    m_tabs->tabButton(1)->setToolTip(tr("BitTorrent, eD2k, Kad, Gnutella and web indexes: their state and the eD2k servers"));
    m_stack = new QStackedWidget(this);
    m_wpn = new QWidget(m_stack);
    m_other = new QWidget(m_stack);
    m_stack->addWidget(m_wpn);
    m_stack->addWidget(m_other);
    buildWpn();
    buildOther();
    // the sub-tab persists (ui/networksSubTab)
    const int sub = qBound(0, core->settings()->value(QStringLiteral("ui/networksSubTab")).toInt(), 1);
    m_tabs->setCurrentIndex(sub);
    m_stack->setCurrentIndex(sub);
    connect(m_tabs, &TabStrip::currentChanged, this, [this](int i) {
        m_stack->setCurrentIndex(i);
        this->core()->settings()->setValue(QStringLiteral("ui/networksSubTab"), i);
        emit statusChanged();
    });
    // the sub-pages get their final size from the stacked widget after our resizeEvent: lay them
    // out on their own resize events
    m_wpn->installEventFilter(this);
    m_other->installEventFilter(this);

    NetworkService *net = core->network();
    connect(net, &NetworkService::overallChanged, this, &NetworksPage::refreshStatus);
    connect(net, &NetworkService::networkChanged, this, &NetworksPage::refreshStatus);
    connect(net, &NetworkService::summaryChanged, this, &NetworksPage::statusChanged);
    connect(net, &NetworkService::screenNameChanged, this, [this](const QString &n) { m_name->setText(n); });
    connect(core->settings(), &Settings::changed, this, [this](const QString &key, const QVariant &v) {
        if (key == keys::identity::PrimaryConnection) {
            (v.toBool() ? m_primary : m_secondary)->setChecked(true);
            emit statusChanged();
        }
    });
    refreshStatus();
}

void NetworksPage::buildWpn()
{
    m_connect = new PushButton(tr("Connect"), m_wpn);
    m_connect->setObjectName(QStringLiteral("btnConnect"));
    m_disconnect = new PushButton(tr("Disconnect"), m_wpn);
    m_disconnect->setObjectName(QStringLiteral("btnDisconnect"));
    connect(m_connect, &QPushButton::clicked, this, [this] { core()->network()->connectAll(); });
    m_connect->setToolTip(tr("Connect to every network that is switched on in Settings (OctoMX, BitTorrent, eD2k, Kad, Gnutella, web indexes)"));
    m_disconnect->setToolTip(tr("Close the connections to all networks; transfers stop until you connect again"));
    // [diag-1] Connection Test (UI_SPEC 8.11): in the Connection Status box, and on the other tab
    m_connTest = new PushButton(tr("Connection Test..."), m_wpn);
    m_connTest->setObjectName(QStringLiteral("btnConnectionTest"));
    m_connTest->setToolTip(tr("Check the ports, the router, port forwarding and why a download is slow"));
    connect(m_connTest, &QPushButton::clicked, this, [this] { ConnectionTestDialog::open(core(), this); });
    connect(m_disconnect, &QPushButton::clicked, this, [this] { core()->network()->disconnectAll(); });

    m_statusBox = new QGroupBox(tr("Connection Status"), m_wpn);
    m_statusBox->setObjectName(QStringLiteral("groupConnectionStatus"));
    m_lamp = new StatusIndicator(m_statusBox);
    m_headline = new QLabel(m_statusBox);
    m_headline->setObjectName(QStringLiteral("statusHeadline"));
    m_headline->setFont(largeFont());   // the headline is ~10 pt bold in the original WinMX
    connect(Fonts::instance(), &Fonts::changed, m_headline, [this] { m_headline->setFont(largeFont()); });
    m_detail = new StaticText(QString(), m_statusBox);
    m_detail->setObjectName(QStringLiteral("statusDetail"));

    m_identityBox = new QGroupBox(tr("Network Identity"), m_wpn);
    m_identityBox->setObjectName(QStringLiteral("groupNetworkIdentity"));
    m_nameLabel = new StaticText(tr("Screen Name:"), m_identityBox);
    m_name = new StaticText(core()->network()->screenName(), m_identityBox);
    m_name->setObjectName(QStringLiteral("screenName"));
    m_change = new PushButton(tr("Change"), m_identityBox);
    m_change->setObjectName(QStringLiteral("btnChangeName"));
    connect(m_change, &QPushButton::clicked, this, &NetworksPage::changeScreenName);
    m_change->setToolTip(tr("Choose another screen name: the name other users see in chat, the Hotlist and transfers"));

    m_typeBox = new QGroupBox(tr("Connection Type"), m_wpn);
    m_typeBox->setObjectName(QStringLiteral("groupConnectionType"));
    m_secondary = new QRadioButton(tr("Make a secondary connection to the network"), m_typeBox);
    m_secondary->setObjectName(QStringLiteral("radioSecondary"));
    m_primary = new QRadioButton(tr("Make a primary connection to the network"), m_typeBox);
    m_primary->setObjectName(QStringLiteral("radioPrimary"));
    // [ui-h] tips on every control
    // [set-1] what the choice really changes: the OctoMX Peer Network has no primary / secondary roles,
    // every user connects alike; passing connections on for others is "Act as a relay" (Settings)
    m_secondary->setToolTip(tr("A shorter status line (only the time online) and no Host button for chat rooms. Searching, "
                               "chat and downloads work the same."));
    m_primary->setToolTip(tr("The status line counts your connections and searches per minute, and the Chat page offers Host "
                             "for new rooms (the default)."));
    m_secondaryText = new WrapLabel(QString(), m_typeBox);
    m_secondaryText->setText(tr(
        "On the OctoMX Peer Network every user connects the same way.  This choice only changes what OctoMX shows: "
        "the time online instead of the connection counts, and no Host button for chat rooms."));
    m_primaryText = new WrapLabel(QString(), m_typeBox);
    m_primaryText->setText(tr(
        "The status line counts your connections and searches per minute, and you can host chat rooms.  Passing "
        "connections on for users who cannot accept incoming ones is set in Settings > Peer Network Options "
        "(\"Act as a relay for other users\"); limits for your line are on the Bandwidth Throttle page."));
    const bool primary = core()->settings()->b(keys::identity::PrimaryConnection);
    (primary ? m_primary : m_secondary)->setChecked(true);
    connect(m_primary, &QRadioButton::toggled, this, [this](bool on) {
        core()->settings()->setValue(keys::identity::PrimaryConnection, on);
    });
}

void NetworksPage::buildOther()
{
    struct B { const char *name; const char *text; };
    const B defs[] = {
        {"btnSrvConnect", QT_TR_NOOP("Connect")},       {"btnSrvDisconnect", QT_TR_NOOP("Disconnect")},
        {"btnSrvStop", QT_TR_NOOP("Stop Attempts")},    {"btnSrvAdd", QT_TR_NOOP("Add")},
        {"btnSrvEdit", QT_TR_NOOP("Edit")},             {"btnSrvRemove", QT_TR_NOOP("Remove")},
        {"btnFindMet", QT_TR_NOOP("Find .MET")},        {"btnImportMet", QT_TR_NOOP("Import .MET")},
        {"btnExportMet", QT_TR_NOOP("Export .MET")},
    };
    for (const B &b : defs) {
        auto *btn = new PushButton(tr(b.text), m_other);
        btn->setObjectName(QString::fromLatin1(b.name));
        m_otherButtons << btn;
    }
    // [ui-g] what each button does (on the network or server selected in the list; nothing selected = all)
    const char *const tipsOther[] = {
        QT_TR_NOOP("Connect the selected network or server (nothing selected: every network)"),
        QT_TR_NOOP("Disconnect the selected network or server (nothing selected: every network)"),
        QT_TR_NOOP("Stop trying to connect the selected network (it retries by itself otherwise)"),
        QT_TR_NOOP("Add an eD2k server by address and port"),
        QT_TR_NOOP("Change the name, address or port of the selected server"),
        QT_TR_NOOP("Remove the selected server from the list"),
        QT_TR_NOOP("Download a fresh eD2k server list (server.met) from an address you choose"),
        QT_TR_NOOP("Add the eD2k servers of a server.met file on this computer"),
        QT_TR_NOOP("Save the eD2k server list as a server.met file, for another program or computer"),
    };
    for (int i = 0; i < m_otherButtons.size() && i < 9; ++i)
        m_otherButtons[i]->setToolTip(tr(tipsOther[i]));
    m_connTest2 = new PushButton(tr("Connection Test..."), m_other);
    m_connTest2->setObjectName(QStringLiteral("btnConnectionTest2"));
    m_connTest2->setToolTip(tr("Check the ports, the router, port forwarding and why a download is slow"));
    connect(m_connTest2, &QPushButton::clicked, this, [this] { ConnectionTestDialog::open(core(), this); });
    m_list = new MxListView(m_other);
    m_list->setObjectName(QStringLiteral("serverList"));
    m_list->setHeaderHidden(true);
    m_list->setSortingEnabled(false);
    m_list->setTreeMode(true);
    m_model = new ServerModel(core()->network(), this);
    m_list->setModel(m_model);
    m_list->header()->setStretchLastSection(true);
    m_list->setStateKey(core()->settings(), QStringLiteral("servers"));
    // the servers and peers of a network are folded away until the user opens the network row
    // (owner's request: everything collapsed by default); a row the user opened stays open while
    // its children are refreshed
    connect(m_list, &MxListView::contextMenuAt, this, &NetworksPage::showServerMenu);
    connect(m_list, &MxListView::deleteRequested, this, &NetworksPage::removeServer);   // [qa-1] Del
    connect(m_list, &MxListView::rowActivated, this, [this](const QModelIndex &vi) {
        const QModelIndex i = m_list->mapToSource(vi);
        const Net n = m_model->netAt(i);
        if (m_model->isNetworkRow(i))
            core()->network()->connectNetwork(n);
        else
            core()->network()->connectServer(n, m_model->serverAt(i).id);
    });

    auto act = [this](int which) {
        const QModelIndex i = currentServerIndex();
        NetworkService *net = core()->network();
        const Net n = m_model->netAt(i);
        const bool isNet = m_model->isNetworkRow(i);
        switch (which) {
        case 0: if (!i.isValid()) net->connectAll(); else if (isNet) net->connectNetwork(n); else net->connectServer(n, m_model->serverAt(i).id); break;
        case 1: if (!i.isValid()) net->disconnectAll(); else if (isNet) net->disconnectNetwork(n); else net->disconnectServer(n, m_model->serverAt(i).id); break;
        case 2: if (i.isValid()) net->disconnectNetwork(n); break;
        default: break;
        }
    };
    connect(m_otherButtons[0], &QPushButton::clicked, this, [act] { act(0); });
    connect(m_otherButtons[1], &QPushButton::clicked, this, [act] { act(1); });
    connect(m_otherButtons[2], &QPushButton::clicked, this, [act] { act(2); });
    connect(m_otherButtons[3], &QPushButton::clicked, this, &NetworksPage::addServer);
    connect(m_otherButtons[4], &QPushButton::clicked, this, &NetworksPage::editServer);
    connect(m_otherButtons[5], &QPushButton::clicked, this, &NetworksPage::removeServer);
    connect(m_otherButtons[6], &QPushButton::clicked, this, &NetworksPage::findList);
    connect(m_otherButtons[7], &QPushButton::clicked, this, &NetworksPage::importList);
    connect(m_otherButtons[8], &QPushButton::clicked, this, &NetworksPage::exportList);
}

void NetworksPage::resizeEvent(QResizeEvent *)
{
    // sub-tab strip at page y = 5 (client y 53), each half of the width
    m_tabs->setGeometry(S(2), S(5), width() - S(4), S(21));
    m_stack->setGeometry(0, S(26), width(), height() - S(26));
    m_wpn->resize(m_stack->size());
    m_other->resize(m_stack->size());
    layoutWpn();
    layoutOther();
}

bool NetworksPage::eventFilter(QObject *o, QEvent *e)
{
    if (e->type() == QEvent::Resize) {
        if (o == m_wpn)
            layoutWpn();
        else if (o == m_other)
            layoutOther();
    }
    return Page::eventFilter(o, e);
}

void NetworksPage::layoutWpn()
{
    // centred column; every number below is measured on reference/ref1_wikipedia_1024x709.jpg
    // (sub-page y; client y = 48 + 26 + y)
    const int x = (m_wpn->width() - S(kColW)) / 2;
    m_connect->setGeometry(SR(0, 9, 232, 22).translated(x, 0));
    m_disconnect->setGeometry(SR(246, 9, 232, 22).translated(x, 0));

    const int bw = kColW + 1;
    m_statusBox->setGeometry(SR(1, 42, bw, 89).translated(x, 0));
    m_lamp->move(SP(18, 21));
    m_headline->setGeometry(SR(66, 25, 380, 16));
    m_detail->setGeometry(SR(66, 46, 380, 14));
    m_connTest->setGeometry(SR(1 + 340, 42 + 62, 126, 21).translated(x, 0));
    m_connTest->raise();

    m_identityBox->setGeometry(SR(1, 142, bw, 61).translated(x, 0));
    m_nameLabel->setGeometry(SR(39, 25, 72, 14));
    m_name->setGeometry(SR(112, 25, 200, 14));
    m_change->setGeometry(SR(332, 23, 122, 22));

    m_typeBox->setGeometry(SR(1, 214, bw, 224).translated(x, 0));
    m_secondary->setGeometry(SR(15, 20, 440, 16));
    m_secondaryText->setGeometry(SR(34, 39, 418, 56));
    m_primary->setGeometry(SR(15, 104, 440, 16));
    m_primaryText->setGeometry(SR(34, 125, 428, 92));   // wraps like the reference (6 lines); room for 7
}

void NetworksPage::layoutOther()
{
    int x = 2;
    const int widths[] = {72, 72, 90, 60, 60, 60, 80, 80, 80};
    for (int i = 0; i < m_otherButtons.size(); ++i) {
        if (i == 6)
            x += 12;
        m_otherButtons[i]->setGeometry(SR(x, 4, widths[i], 21));
        x += widths[i];
    }
    // [diag-1] at the end of the row; right-aligned when the page is wider
    const int ctW = S(112);
    const int ctX = qMax(S(x + 12), m_other->width() - ctW - S(2));
    m_connTest2->setGeometry(ctX, S(4), ctW, S(21));
    m_list->setGeometry(0, S(29), m_other->width(), m_other->height() - S(29));
}

void NetworksPage::saveState()
{
    m_list->saveState();
    core()->settings()->setValue(QStringLiteral("ui/networksSubTab"), m_tabs->currentIndex());
}

void NetworksPage::refreshStatus()
{
    NetworkService *net = core()->network();
    const ConnState s = net->overallState();
    m_lamp->setState(s == ConnState::Connected ? 2 : s == ConnState::Connecting ? 1 : 0);
    m_headline->setText(net->overallHeadline());
    m_detail->setText(net->overallDetail());
    m_connect->setEnabled(s != ConnState::Connected && s != ConnState::Connecting);
    m_disconnect->setEnabled(s == ConnState::Connected || s == ConnState::Connecting);
    emit statusChanged();
}

QStringList NetworksPage::statusFields() const
{
    NetworkService *net = core()->network();
    const ConnState s = net->overallState();
    if (s == ConnState::Connecting)
        return {tr("Connecting...")};
    if (s != ConnState::Connected)
        return {tr("Offline")};
    if (!core()->settings()->b(keys::identity::PrimaryConnection))
        return {tr("Online")};
    // primary: Online · <P> Primary Connections · <S> Incoming Secondary Connections · <n> / <n> SPPM
    const NetworkSummary sum = net->summary();
    return {tr("Online"), tr("%1 Primary Connections").arg(sum.primary),
            tr("%1 Incoming Secondary Connections").arg(sum.secondary),   // [ui-e] engine-4: NetworkSummary::secondary
            tr("%1 / %2 SPPM").arg(sum.searchesPerMinute).arg(sum.searchesPerMinute)};
}

bool NetworksPage::selectSubTab(const QString &name)
{
    const QString n = name.toLower();
    if (n.startsWith(QLatin1String("wpn")) || n.startsWith(QLatin1String("octomx")) || n.startsWith(QLatin1String("winmx")) /* [ren-1] the legacy name in scripts */ || n == QLatin1String("0")) {
        m_tabs->setCurrentIndex(0);
        return true;
    }
    if (n.startsWith(QLatin1String("other")) || n.startsWith(QLatin1String("opennap")) || n == QLatin1String("1")) {
        m_tabs->setCurrentIndex(1);
        return true;
    }
    return false;
}

void NetworksPage::changeScreenName()
{
    ChangeNameDialog dlg(core()->network()->screenName(), this);
    if (dlg.exec() == QDialog::Accepted && !dlg.name().isEmpty())
        core()->network()->setScreenName(dlg.name());
}

QModelIndex NetworksPage::currentServerIndex() const
{
    const QModelIndexList sel = m_list->selectedSourceRows();
    return sel.isEmpty() ? QModelIndex() : sel.first();
}

void NetworksPage::showServerMenu(const QModelIndex &viewIndex, const QPoint &globalPos)
{
    const QModelIndex i = m_list->mapToSource(viewIndex);
    NetworkService *net = core()->network();
    const Net n = m_model->netAt(i);
    const bool isNet = m_model->isNetworkRow(i);
    const ServerEntry e = m_model->serverAt(i);
    auto *menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("serverMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    QAction *c = menu->addAction(tr("Connect"), this, [=] {
        if (isNet) net->connectNetwork(n); else net->connectServer(n, e.id);
    });
    QAction *d = menu->addAction(tr("Disconnect"), this, [=] {
        if (isNet) net->disconnectNetwork(n); else net->disconnectServer(n, e.id);
    });
    c->setEnabled(i.isValid());
    d->setEnabled(i.isValid());
    menu->addSeparator();
    menu->addAction(tr("Add"), this, &NetworksPage::addServer);
    menu->addAction(tr("Edit"), this, &NetworksPage::editServer)->setEnabled(i.isValid() && !isNet);
    menu->addAction(tr("Remove"), this, &NetworksPage::removeServer)->setEnabled(i.isValid() && !isNet);
    menu->addSeparator();
    if (isNet) {
        QAction *en = menu->addAction(tr("Enabled"));
        en->setCheckable(true);
        en->setChecked(net->isEnabled(n));
        connect(en, &QAction::toggled, this, [net, n](bool on) { net->setEnabled(n, on); });
        menu->addSeparator();
    }
    menu->addAction(tr("View Server Messages"), this, [this, n, e, isNet] {
        viewServerMessages(n, isNet ? QString() : e.id);
    })->setEnabled(i.isValid());
    menu->addSeparator();
    menu->addAction(tr("Select All"), m_list, &QTreeView::selectAll);
    tips::explainMenu(menu);   // [ui-g] tool tips + status bar text of the entries
    menu->popup(globalPos);
}

void NetworksPage::addServer()
{
    ServerDialog dlg(tr("Add Server"), this);
    const QModelIndex i = currentServerIndex();
    if (i.isValid())
        dlg.setNetwork(m_model->netAt(i));
    if (dlg.exec() == QDialog::Accepted && !dlg.address().isEmpty())
        core()->network()->addServer(dlg.network(), dlg.address(), dlg.name());
}

void NetworksPage::editServer()
{
    const QModelIndex i = currentServerIndex();
    if (!i.isValid() || m_model->isNetworkRow(i))
        return;
    const ServerEntry e = m_model->serverAt(i);
    ServerDialog dlg(tr("Edit Server"), this);
    dlg.setNetwork(e.net);
    dlg.setAddress(e.address);
    dlg.setName(e.name);
    if (dlg.exec() == QDialog::Accepted && !dlg.address().isEmpty()) {
        if (dlg.network() == e.net) {
            if (!core()->network()->editServer(e.net, e.id, dlg.address(), dlg.name()))
                QMessageBox::warning(this, tr("OctoMX"), tr("The server %1 is no longer in the list.").arg(e.address));
        } else {   // moved to another network: a new entry there
            core()->network()->removeServer(e.net, e.id);
            core()->network()->addServer(dlg.network(), dlg.address(), dlg.name());
        }
    }
}

void NetworksPage::removeServer()
{
    const QModelIndexList sel = m_list->selectedSourceRows();
    QList<ServerEntry> victims;
    for (const QModelIndex &i : sel)
        if (!m_model->isNetworkRow(i))
            victims << m_model->serverAt(i);
    if (victims.isEmpty())
        return;
    if (QMessageBox::question(this, tr("OctoMX"), tr("Remove the selected server(s)?"), QMessageBox::Ok | QMessageBox::Cancel) !=
        QMessageBox::Ok)
        return;
    for (const ServerEntry &e : victims)
        core()->network()->removeServer(e.net, e.id);
}

void NetworksPage::findList()
{
    // "Find .MET": the URL the eD2k plugin downloads its server list from
    Settings *s = core()->settings();
    bool ok = false;
    const QString url = QInputDialog::getText(this, tr("Find .MET"), tr("Download the eD2k server list from this address:"),
                                              QLineEdit::Normal, s->s(keys::ed2k::ServerListUrl), &ok);
    if (ok)
        s->setValue(keys::ed2k::ServerListUrl, url.trimmed());
}

void NetworksPage::importList()
{
    const QString path = s_importHook ? s_importHook(this)
                                      : QFileDialog::getOpenFileName(this, tr("Import .MET"), QString(),
                                                                     tr("Server lists (*.met *.met.gz *.txt *.dat);;All files (*)"));
    if (path.isEmpty())
        return;
    // binary eD2k server.met (also gzip'ed), "host:port name" text lists and ed2k://|server| links
    const int n = core()->network()->importServerList(Net::ED2K, path);
    const QString msg = n < 0 ? tr("%1 is not a server list OctoMX can read.").arg(QFileInfo(path).fileName())
                              : n == 1 ? tr("1 server imported.") : tr("%1 servers imported.").arg(n);   // [qa-2] no "(s)"
    if (s_importHook)
        m_lastImportMessage = msg;
    else
        QMessageBox::information(this, tr("Import .MET"), msg);
}

std::function<QString(QWidget *)> NetworksPage::s_importHook;

void NetworksPage::viewServerMessages(Net n, const QString &serverId)
{
    const QString key = netName(n) + QLatin1Char('|') + serverId;
    if (ServerMessagesWindow *w = m_msgWindows.value(key)) {
        w->show();
        w->raise();
        w->activateWindow();
        return;
    }
    auto *w = new ServerMessagesWindow(core(), n, serverId, mainWindow());
    m_msgWindows.insert(key, w);
    connect(w, &QObject::destroyed, this, [this, key] { m_msgWindows.remove(key); });
    w->show();
}

void NetworksPage::exportList()
{
    const QString path = QFileDialog::getSaveFileName(this, tr("Export .MET"), QStringLiteral("servers.txt"),
                                                      tr("Text server list (*.txt)"));
    if (path.isEmpty())
        return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;
    QTextStream ts(&f);
    for (const ServerEntry &e : core()->network()->servers(Net::ED2K))
        ts << e.address << ' ' << e.name << '\n';
}

} // namespace mx::ui
