// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/dialogs/setup_wizard.h"

#include <QtCore/QPointer>
#include <QtCore/QUrl>

#include <QtCore/QDir>
#include <QtGui/QIntValidator>
#include <QtGui/QPainter>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QVBoxLayout>

#include "mx/services.h"
#include "mx/settings_keys.h"
#include "ui/dialogs/settings_binding.h"
#include "ui/kit/icons.h"
#include "ui/kit/theme.h"
#include "ui/kit/widgets.h"
#include "ui/kit/scale.h"

namespace mx::ui {

using namespace mx::keys;

namespace {

SetupWizard::PortTester &portTester()
{
    static SetupWizard::PortTester t;
    return t;
}

QPixmap watermark()
{
    QPixmap pm(SS(150, 286));
    pm.fill(Qt::black);
    QPainter p(&pm);
    p.drawImage(SR(11, 24, 128, 128), iconImage(IconId::App, S(128)));
    p.setPen(QColor(0x00, 0xFF, 0x00));
    p.setFont(classicFont(24, true));
    p.drawText(SR(0, 166, 150, 34), Qt::AlignHCenter | Qt::AlignVCenter, QStringLiteral("OctoMX"));
    p.setPen(QColor(0xFF, 0xFF, 0x00));
    p.setFont(classicFont(12, true));
    p.drawText(SR(0, 202, 150, 18), Qt::AlignHCenter | Qt::AlignVCenter, QObject::tr("Setup Wizard"));
    p.setPen(QColor(0xC0, 0xC0, 0xC0));
    p.setFont(normalFont());
    p.drawText(SR(0, 250, 150, 16), Qt::AlignHCenter | Qt::AlignVCenter, QStringLiteral("v" MX_VERSION_STRING));
    return pm;
}

QWizardPage *makePage(const QString &title, const QString &sub)
{
    auto *p = new QWizardPage;
    // rich text with a pixel size: QWizard derives the title font from a point size, and the
    // classic look uses pixel-sized fonts (point size -1)
    p->setTitle(QStringLiteral("<span style=\"font-size:%2px; font-weight:bold\">%1</span>").arg(title.toHtmlEscaped(), QString::number(fontPx(13))));
    p->setSubTitle(sub);
    p->setFinalPage(true);   // "Finish" works on every page (the one-click recommended path)
    auto *lay = new QVBoxLayout(p);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(S(6));
    return p;
}

WrapLabel *wrapped(const QString &t)
{
    auto *w = new WrapLabel(t);
    w->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    return w;
}

} // namespace

SetupWizard::RealPortTester &realPortTester()
{
    static SetupWizard::RealPortTester t;
    return t;
}

void SetupWizard::setRealPortTester(const RealPortTester &t)
{
    realPortTester() = t;
}

void SetupWizard::setPortTester(const PortTester &t)
{
    portTester() = t;
}

SetupWizard::SetupWizard(mx::Core *core, QWidget *parent) : QWizard(parent), m_core(core)
{
    setObjectName(QStringLiteral("setupWizard"));
    setWindowTitle(tr("OctoMX Setup Wizard"));
    setWindowIcon(appIcon());
    setWizardStyle(QWizard::ClassicStyle);
    setTitleFormat(Qt::RichText);
    setOptions(QWizard::HaveFinishButtonOnEarlyPages | QWizard::NoBackButtonOnStartPage);
    setPixmap(QWizard::WatermarkPixmap, watermark());
    setFixedSize(SS(520, 372));
    setPage(Welcome, makeWelcome());
    setPage(ScreenName, makeScreenName());
    setPage(Connection, makeConnection());
    setPage(TcpPort, makePort(false));
    // [ui-h] no UDP page any more: connection/udpPort and udpEnabled were read by nothing (the OctoMX
    // Peer Network uses the TCP port number for UDP too, the other networks have their own ports)
    setPage(Folders, makeFolders());
    setPage(Networks, makeNetworks());
    setPage(Finish, makeFinish());
    setStartId(Welcome);
    // object names for the script driver / tests
    const struct { WizardButton b; const char *n; } names[] = {
        {BackButton, "wizBack"}, {NextButton, "wizNext"}, {FinishButton, "wizFinish"}, {CancelButton, "wizCancel"}};
    for (const auto &n : names)
        if (QAbstractButton *b = button(n.b))
            b->setObjectName(QString::fromLatin1(n.n));
    connect(this, &QWizard::currentIdChanged, this, [this](int id) {
        if (id == TcpPort)
            runPortTest(false);
        else if (id == UdpPort)
            runPortTest(true);
        else if (id == Finish)
            updateSummary();
    });
}

bool SetupWizard::recommended() const
{
    return m_recommended->isChecked();
}

int SetupWizard::nextId() const
{
    if (currentId() == Welcome && recommended())
        return Finish;
    return QWizard::nextId();
}

QWizardPage *SetupWizard::makeWelcome()
{
    QWizardPage *p = makePage(tr("Welcome to the OctoMX Setup Wizard"), QString());
    p->setObjectName(QStringLiteral("wizWelcome"));
    auto *lay = static_cast<QVBoxLayout *>(p->layout());
    lay->addWidget(wrapped(tr("This wizard sets OctoMX up for the networks it uses: the OctoMX Peer Network, BitTorrent, "
                              "eD2k and Kad, Gnutella, web sources and torrent indexes. Everything can be changed "
                              "later in Settings.")));
    lay->addSpacing(S(8));
    m_recommended = new QRadioButton(tr("Recommended settings (everything on)"));
    m_recommended->setObjectName(QStringLiteral("wizRecommended"));
    m_recommended->setFont(boldFont());
    m_recommended->setChecked(true);
    m_recommended->setToolTip(tr("Turn every network on with the usual ports and folders and finish at once. Best for most users."));
    lay->addWidget(m_recommended);
    auto *r1 = wrapped(tr("Every network enabled, ports mapped automatically, standard folders, magnet: and ed2k:// "
                          "links and .torrent files opened by OctoMX. Press Finish."));
    r1->setContentsMargins(S(19), 0, 0, 0);
    lay->addWidget(r1);
    lay->addSpacing(S(6));
    m_custom = new QRadioButton(tr("Let me choose each setting"));
    m_custom->setObjectName(QStringLiteral("wizCustom"));
    m_custom->setToolTip(tr("Go through the pages one by one, for example to pick your own ports, folders or networks."));
    lay->addWidget(m_custom);
    auto *r2 = wrapped(tr("Screen name, connection type, ports, folders and networks, one page at a time. Press Next."));
    r2->setContentsMargins(S(19), 0, 0, 0);
    lay->addWidget(r2);
    lay->addStretch(1);
    // [ui-e] interface size: the main window opens at the chosen scale (Automatic = the display's setting)
    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(S(6));
    auto *scaleLabel = new StaticText(tr("Interface size:"));
    scaleLabel->setFixedSize(scaleLabel->fontMetrics().horizontalAdvance(scaleLabel->text()) + S(4), S(21));
    row->addWidget(scaleLabel);
    m_scale = new QComboBox;
    m_scale->setObjectName(QStringLiteral("wizScale"));
    for (const QString &c : scaleChoices())
        m_scale->addItem(scaleChoiceLabel(c), c);
    m_scale->setCurrentIndex(qMax(0, m_scale->findData(m_core->settings()->value(keys::ui::Scale).toString())));
    m_scale->setFixedHeight(S(21));
    m_scale->setMinimumWidth(S(150));
    m_scale->setToolTip(tr("How large OctoMX draws its windows, text and icons. Automatic follows the display's scale setting."));   // [qa-2]
    connect(m_scale, &QComboBox::activated, this, [this](int i) {
        m_core->settings()->setValue(keys::ui::Scale, m_scale->itemData(i).toString());
    });
    row->addWidget(m_scale);
    row->addStretch(1);
    lay->addLayout(row);
    return p;
}

QWizardPage *SetupWizard::makeScreenName()
{
    QWizardPage *p = makePage(tr("Screen Name"), QString());
    p->setObjectName(QStringLiteral("wizScreenName"));
    auto *lay = static_cast<QVBoxLayout *>(p->layout());
    lay->addWidget(wrapped(tr("Other users see you by this name in chat rooms, in search results and on their Hotlist.")));
    lay->addSpacing(S(6));
    lay->addWidget(new StaticText(tr("Screen name:")));
    m_name = new QLineEdit(m_core->network()->screenName());
    m_name->setObjectName(QStringLiteral("wizName"));
    m_name->setToolTip(tr("The name other users see in chat rooms, the Hotlist and transfers. You can change it later."));
    m_name->setMaxLength(40);
    m_name->setFixedHeight(S(21));
    lay->addWidget(m_name);
    lay->addStretch(1);
    return p;
}

QWizardPage *SetupWizard::makeConnection()
{
    QWizardPage *p = makePage(tr("Connection Type"), QString());
    p->setObjectName(QStringLiteral("wizConnection"));
    auto *lay = static_cast<QVBoxLayout *>(p->layout());
    lay->addWidget(wrapped(tr("Select the type of internet connection you have. Other users see it next to your name.")));
    auto *grid = new QGridLayout;
    grid->setVerticalSpacing(1);
    // [qa-1] owner request: modern connection types with their Mbit ranges (6 + 7 rows)
    const QString cur = m_core->settings()->s(identity::Connection);
    for (int i = 0; i < connectionTypes().size(); ++i) {
        auto *r = new QRadioButton(connectionTypeLabel(connectionTypes().at(i)));
        r->setObjectName(QStringLiteral("wizConn%1").arg(i));
        r->setProperty("mxValue", connectionTypes().at(i));
        r->setChecked(connectionTypes().at(i) == cur);
        r->setToolTip(tr("Choose this when your internet line is of this kind. Other users see it next to your name, "
                         "and OctoMX picks how many uploads and downloads run at once from it, unless you changed those numbers."));
        grid->addWidget(r, i < 6 ? i : i - 6, i < 6 ? 0 : 1);
        m_conn << r;
    }
    lay->addLayout(grid);
    lay->addStretch(1);
    return p;
}

QWizardPage *SetupWizard::makePort(bool udp)
{
    Settings *s = m_core->settings();
    QWizardPage *p = makePage(udp ? tr("UDP Port") : tr("Incoming TCP Port"), QString());
    p->setObjectName(udp ? QStringLiteral("wizUdp") : QStringLiteral("wizTcp"));
    auto *lay = static_cast<QVBoxLayout *>(p->layout());
    auto *row = new QHBoxLayout;
    auto *on = new QRadioButton(udp ? tr("Send and receive UDP datagrams on port") : tr("Listen on port"));
    auto *port = new QLineEdit(QString::number(s->i(udp ? connection::UdpPort : connection::TcpPort)));
    port->setValidator(new QIntValidator(1, 65535, port));
    port->setFixedSize(SS(52, 21));
    row->addWidget(on);
    row->addWidget(port);
    if (!udp)
        row->addWidget(new StaticText(tr("for incoming TCP connections")), 1);
    else
        row->addStretch(1);
    lay->addLayout(row);
    auto *test = new PushButton(tr("Test Port"));
    test->setObjectName(udp ? QStringLiteral("wizUdpTest") : QStringLiteral("wizTcpTest"));
    test->setFixedSize(SS(90, 23));
    test->setToolTip(udp ? tr("Check that this UDP port is free on this computer and ask the router to forward it. The result is shown on the right.")
                         : tr("Check that this TCP port is free, ask the router to forward it and, when a public checker is set, "
                              "whether the internet can reach it. The result is shown on the right."));
    // [qa-2] a WrapLabel: "Port 6699 is in use (by OctoMX itself when it is connected)." was cut off
    auto *result = new WrapLabel;
    result->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    result->setObjectName(udp ? QStringLiteral("wizUdpResult") : QStringLiteral("wizTcpResult"));
    auto *trow = new QHBoxLayout;
    trow->addWidget(test);
    trow->addWidget(result, 1);
    result->setMinimumHeight(S(16));
    lay->addLayout(trow);
    lay->addWidget(wrapped(udp ? tr("If you are behind a router or firewall, the UDP port must be forwarded to this computer "
                                    "(OctoMX tries UPnP / NAT-PMP).")
                               : tr("If you are behind a router or firewall, the TCP port must be forwarded to this computer. "
                                    "OctoMX tries to do it with UPnP / NAT-PMP.%1")
                                     .arg(QUrl(s->s(keys::diag::PortCheckUrl).replace(QLatin1String("%1"), QStringLiteral("1"))).host().isEmpty()
                                              ? QString()
                                              : tr(" Test Port also asks the public checker %1 whether the port can be reached from the internet.")
                                                    .arg(QUrl(s->s(keys::diag::PortCheckUrl).replace(QLatin1String("%1"), QStringLiteral("1"))).host()))));
    auto *off = new QRadioButton(udp ? tr("Unable to send or receive UDP datagrams on any port")
                                     : tr("Unable to accept incoming TCP connections"));
    on->setToolTip(udp ? tr("Use this UDP port for searches, Kad, DHT and the other networks' small messages.")
                       : tr("Accept connections from other users on this port, so firewalled users can download from you too."));
    off->setToolTip(udp ? tr("Choose this only when your network blocks UDP completely: Kad, DHT and several searches then do not work.")
                        : tr("Choose this when no port can be opened. You can still download, but only from users who accept connections."));
    lay->addWidget(off);
    (s->b(udp ? connection::UdpEnabled : connection::AcceptIncoming) ? on : off)->setChecked(true);
    if (!udp) {
        m_upnp = new QCheckBox(tr("Map the ports automatically (UPnP / NAT-PMP)"));
        m_upnp->setObjectName(QStringLiteral("wizUpnp"));
        m_upnp->setChecked(s->b(connection::Upnp) || s->b(connection::NatPmp));
        m_upnp->setToolTip(tr("Ask your router to forward the TCP and UDP ports to this computer by itself, so you need not do it by hand."));
        lay->addSpacing(S(4));
        lay->addWidget(m_upnp);
        m_tcpOn = on;
        m_tcpOff = off;
        m_tcpPort = port;
        m_tcpResult = result;
        port->setObjectName(QStringLiteral("wizTcpPort"));
        port->setToolTip(tr("The port of the OctoMX Peer Network, for TCP and UDP alike (default 6699). Forward it in your router "
                            "or let UPnP do it."));
    } else {
        m_udpOn = on;
        m_udpOff = off;
        m_udpPort = port;
        m_udpResult = result;
        port->setObjectName(QStringLiteral("wizUdpPort"));
    }
    lay->addStretch(1);
    connect(test, &QPushButton::clicked, this, [this, udp] { runPortTest(udp); });
    return p;
}

void SetupWizard::runPortTest(bool udp)
{
    QLineEdit *port = udp ? m_udpPort : m_tcpPort;
    WrapLabel *result = udp ? m_udpResult : m_tcpResult;
    const int n = port->text().toInt();
    result->setText(udp ? tr("Testing UDP port %1...").arg(n) : tr("Testing TCP port %1...").arg(n));
    result->repaint();
    if (realPortTester()) {   // [diag-1]
        auto *button = findChild<QPushButton *>(udp ? QStringLiteral("wizUdpTest") : QStringLiteral("wizTcpTest"));
        if (button) button->setEnabled(false);
        const bool map = m_upnp ? m_upnp->isChecked() : m_core->settings()->b(connection::Upnp);
        QPointer<WrapLabel> res(result);
        QPointer<QPushButton> btn(button);
        QPointer<QRadioButton> off(udp ? m_udpOff : m_tcpOff);
        realPortTester()(n, udp, map, this, [res, btn, off](const PortTest &t, bool final) {
            if (res) res->setText(t.message);
            if (final && btn) btn->setEnabled(true);
            if (final && !t.ok && off) off->setFocus();
        });
        return;
    }
    if (!portTester()) {
        result->setText(tr("The port test is not available."));
        return;
    }
    const PortTest t = portTester()(n, udp);
    result->setText(t.message);
    if (!t.ok)
        (udp ? m_udpOff : m_tcpOff)->setFocus();
}

QWizardPage *SetupWizard::makeFolders()
{
    Settings *s = m_core->settings();
    QWizardPage *p = makePage(tr("Folders"), QString());
    p->setObjectName(QStringLiteral("wizFolders"));
    auto *lay = static_cast<QVBoxLayout *>(p->layout());
    lay->addWidget(new StaticText(tr("Incoming Files Folder (completed downloads):")));
    m_incoming = new PathField(tr("Change..."), true);
    m_incoming->setObjectName(QStringLiteral("wizIncoming"));
    m_incoming->setFixedHeight(S(23));
    m_incoming->setBoundValue(s->s(transfers::IncomingDir));
    lay->addWidget(m_incoming);
    lay->addSpacing(S(4));
    lay->addWidget(wrapped(tr("Shared folders: the files in these folders can be downloaded by other users.")));
    m_folderList = new QTreeWidget;
    m_folderList->setObjectName(QStringLiteral("wizFolderList"));
    m_folderList->setHeaderHidden(true);
    m_folderList->setRootIsDecorated(false);
    m_folderList->setFixedHeight(S(90));
    lay->addWidget(m_folderList);
    auto *brow = new QHBoxLayout;
    auto *add = new PushButton(tr("Share Folder..."));
    add->setObjectName(QStringLiteral("wizShareAdd"));
    auto *rem = new PushButton(tr("Unshare Folder"));
    rem->setObjectName(QStringLiteral("wizShareRemove"));
    add->setFixedSize(SS(100, 23));
    rem->setFixedSize(SS(100, 23));
    add->setToolTip(tr("Pick a folder whose files other users may download. Its sub folders are shared too."));
    rem->setToolTip(tr("Stop sharing the folder selected in the list. The files stay on your disk."));
    brow->addWidget(add);
    brow->addWidget(rem);
    brow->addStretch(1);
    lay->addLayout(brow);
    lay->addStretch(1);
    for (const SharedFolder &f : m_core->library()->folders())
        m_folders << f.path;
    refreshFolders();
    connect(add, &QPushButton::clicked, this, [this] {
        const QString d = PathField::browse(QDir::homePath(), true, this);
        if (!d.isEmpty() && !m_folders.contains(d)) {
            m_folders << d;
            refreshFolders();
        }
    });
    connect(rem, &QPushButton::clicked, this, [this] {
        if (QTreeWidgetItem *it = m_folderList->currentItem()) {
            m_folders.removeAll(it->data(0, Qt::UserRole).toString());
            refreshFolders();
        }
    });
    // PathField's own button sets the path
    return p;
}

void SetupWizard::refreshFolders()
{
    m_folderList->clear();
    for (const QString &f : std::as_const(m_folders)) {
        auto *it = new QTreeWidgetItem({QDir::toNativeSeparators(f)});
        it->setData(0, Qt::UserRole, f);
        m_folderList->addTopLevelItem(it);
    }
}

QWizardPage *SetupWizard::makeNetworks()
{
    Settings *s = m_core->settings();
    QWizardPage *p = makePage(tr("Networks"), QString());
    p->setObjectName(QStringLiteral("wizNetworks"));
    auto *lay = static_cast<QVBoxLayout *>(p->layout());
    lay->setSpacing(S(1));
    lay->addWidget(wrapped(tr("OctoMX searches and downloads on all of these networks at once. Leave them all on unless "
                              "you have a reason.")));
    lay->addSpacing(S(4));
    const struct { QLatin1StringView key; const char *t; } nets[] = {
        {overlay::Enabled, QT_TR_NOOP("OctoMX Peer Network (chat, browse, hash links)")},
        {bt::Enabled, QT_TR_NOOP("BitTorrent (DHT, trackers, magnet links)")},
        {ed2k::Enabled, QT_TR_NOOP("eD2k servers")},
        {kad::Enabled, QT_TR_NOOP("Kad (serverless eD2k)")},
        {gnutella::Enabled, QT_TR_NOOP("Gnutella")},
        {http::Enabled, QT_TR_NOOP("HTTP and FTP web sources")},
        {web::Enabled, QT_TR_NOOP("Web torrent indexes")},
        {rats::Enabled, QT_TR_NOOP("rats-search torrent network")},
    };
    for (const auto &n : nets) {
        auto *c = new QCheckBox(tr(n.t));
        c->setObjectName(QStringLiteral("wizNet_") + QString(n.key).section(QLatin1Char('/'), 0, 0));
        c->setChecked(true);   // recommended: everything on
        c->setToolTip(tr("Search, download and share on this network too. Untick it to leave it out completely; "
                         "its page in Settings can turn it on again."));
        Q_UNUSED(s)
        lay->addWidget(c);
        m_nets << qMakePair(QString(n.key), c);
    }
    lay->addSpacing(S(8));
    m_assoc = new QCheckBox(tr("Open magnet:, ed2k:// links and .torrent files with OctoMX"));
    m_assoc->setObjectName(QStringLiteral("wizAssoc"));
    m_assoc->setChecked(true);
    m_assoc->setToolTip(tr("Clicking such a link in the web browser or opening a .torrent file starts the download in OctoMX."));
    lay->addWidget(m_assoc);
    m_autostart = new QCheckBox(tr("Start OctoMX when the computer starts (hidden in the tray)"));
    m_autostart->setObjectName(QStringLiteral("wizAutostart"));
    m_autostart->setChecked(m_core->settings()->b(misc::StartWithOs));
    m_autostart->setToolTip(tr("Start OctoMX in the tray when you log in, so downloads and sharing go on without opening it."));
    lay->addWidget(m_autostart);
    lay->addStretch(1);
    return p;
}

QWizardPage *SetupWizard::makeFinish()
{
    QWizardPage *p = makePage(tr("Completing the OctoMX Setup Wizard"), QString());
    p->setObjectName(QStringLiteral("wizFinishPage"));
    p->setFinalPage(true);
    auto *lay = static_cast<QVBoxLayout *>(p->layout());
    m_summary = wrapped(QString());
    m_summary->setObjectName(QStringLiteral("wizSummary"));
    m_summary->setMinimumHeight(S(150));
    lay->addWidget(m_summary);
    m_connectNow = new QCheckBox(tr("Connect to the network now"));
    m_connectNow->setObjectName(QStringLiteral("wizConnectNow"));
    m_connectNow->setChecked(m_core->settings()->b(network::ConnectOnStart));
    m_connectNow->setToolTip(tr("Join the networks as soon as you press Finish, and at every start."));
    lay->addWidget(m_connectNow);
    lay->addStretch(1);
    lay->addWidget(wrapped(tr("Press Finish to save these settings.")));
    return p;
}

void SetupWizard::updateSummary()
{
    QStringList on;
    for (const auto &n : std::as_const(m_nets))
        if (recommended() || n.second->isChecked())
            on << n.second->text().section(QLatin1String(" ("), 0, 0);
    const QString name = m_name->text().trimmed().isEmpty() ? m_core->network()->screenName() : m_name->text().trimmed();
    QString conn;
    for (QRadioButton *r : std::as_const(m_conn))
        if (r->isChecked())
            conn = r->text();
    m_summary->setText(tr("Screen name:  %1\nConnection:  %2\nPort:  %3 (TCP and UDP)%4\nNetworks:  %5\nIncoming files:  %6")
                           .arg(name, conn.isEmpty() ? tr("Unknown") : conn, m_tcpPort->text(),
                                (recommended() || m_upnp->isChecked()) ? tr(" (mapped automatically)") : QString(),
                                on.join(QStringLiteral(", ")),
                                QDir::toNativeSeparators(m_incoming->boundValue().toString().isEmpty()
                                                             ? m_core->transfers()->incomingDir()
                                                             : m_incoming->boundValue().toString())));
}

void SetupWizard::applySettings()
{
    if (m_applied)
        return;
    m_applied = true;
    Settings *s = m_core->settings();
    const bool rec = recommended();
    const QString name = m_name->text().trimmed();
    if (!name.isEmpty() && name != m_core->network()->screenName())
        m_core->network()->setScreenName(name);
    for (QRadioButton *r : std::as_const(m_conn))
        if (r->isChecked())
            s->setValue(identity::Connection, r->property("mxValue"));
    const int tcp = m_tcpPort->text().toInt();
    if (tcp > 0)
        s->setValue(connection::TcpPort, tcp);
    s->setValue(connection::AcceptIncoming, rec || m_tcpOn->isChecked());
    const bool map = rec || m_upnp->isChecked();
    s->setValue(connection::Upnp, map);
    s->setValue(connection::NatPmp, map);
    const QString in = m_incoming->boundValue().toString();
    if (in != s->s(transfers::IncomingDir))
        s->setValue(transfers::IncomingDir, in);
    for (const auto &n : std::as_const(m_nets))
        s->setValue(n.first, rec || n.second->isChecked());
    const bool assoc = rec || m_assoc->isChecked();
    s->setValue(misc::AssociateMagnet, assoc);
    s->setValue(misc::AssociateEd2k, assoc);
    s->setValue(misc::AssociateTorrent, assoc);
    s->setValue(misc::StartWithOs, !rec && m_autostart->isChecked() ? true : (rec ? s->b(misc::StartWithOs) : false));
    s->setValue(network::ConnectOnStart, m_connectNow->isChecked() || rec);
    s->setValue(internal::WizardDone, true);
    s->setValue(internal::FirstRunDone, true);
    s->sync();
}

void SetupWizard::applyFolders()
{
    QStringList existing;
    for (const SharedFolder &f : m_core->library()->folders())
        existing << f.path;
    for (const QString &f : std::as_const(m_folders))
        if (!existing.contains(f))
            m_core->library()->addFolder(f, true);
    for (const QString &f : std::as_const(existing))
        if (!m_folders.contains(f))
            m_core->library()->removeFolder(f);
}

void SetupWizard::accept()
{
    applySettings();
    QWizard::accept();
}

} // namespace mx::ui
