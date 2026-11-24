// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: Settings pages, part 1 [ui-c]: helpers, Internet Connection, OctoMX Peer Network,
// Other Networks (+ BitTorrent, eD2k, Kad, Gnutella, HTTP, Web Search, rats-search).
// Coordinates are dialog client coordinates measured on reference/web/settings_*.png.
#include "ui/dialogs/settings_pages.h"

#include <QtCore/QRegularExpression>
#include <QtCore/QSignalBlocker>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QSpinBox>

#include "mx/services.h"
#include "ui/dialogs/connection_test.h"
#include "ui/dialogs/settings_dialog.h"
#include "ui/kit/theme.h"
#include "ui/kit/widgets.h"
#include "ui/kit/scale.h"

namespace mx::ui {

using namespace mx::keys;

// =========================================================================================== kit

PageKit::PageKit(SettingsDialog *d, QWidget *p) : dlg(d), page(p), b(d->binder()), s(d->core()->settings()) {}

QString PageKit::nameFor(const QString &key)
{
    QString n = QStringLiteral("set_") + key;
    n.replace(QLatin1Char('/'), QLatin1Char('_'));
    n.replace(QLatin1Char('*'), QLatin1Char('_'));
    return n;
}

StaticText *PageKit::label(const QString &t, int x, int y, int w, int align, int h)
{
    auto *l = place(new StaticText(t), x, y, w, h);
    l->setAlignment(align);
    return l;
}

WrapLabel *PageKit::wrap(const QString &t, int x, int y, int w, int h)
{
    return place(new WrapLabel(t), x, y, w, h);
}

QGroupBox *PageKit::group(const QString &t, int x, int y, int w, int h)
{
    auto *g = place(new QGroupBox(t), x, y, w, h);
    g->lower();
    return g;
}

QCheckBox *PageKit::check(const QString &t, const QString &key, int x, int y, int w, int h)
{
    auto *c = place(new QCheckBox(t), x, y, w, h);
    c->setObjectName(nameFor(key));
    c->setToolTip(settingTip(key));   // [ui-h] every control explains itself (a page may set a better one)
    b->bind(c, key, SettingType::Bool);
    return c;
}

QRadioButton *PageKit::radio(const QString &t, const QString &key, const QVariant &v, int x, int y, int w, int h)
{
    auto *r = place(new QRadioButton(t), x, y, w, h);
    r->setObjectName(nameFor(key) + QLatin1Char('_') + v.toString());
    r->setToolTip(settingTip(key));
    b->bindRadio(r, key, v);
    return r;
}

QSpinBox *PageKit::spin(const QString &key, int x, int y, int w, int h)
{
    auto *sb = place(new QSpinBox, x, y, w, h);
    sb->setObjectName(nameFor(key));
    sb->setToolTip(settingTip(key));
    b->bind(sb, key, SettingType::Int);
    return sb;
}

QDoubleSpinBox *PageKit::dspin(const QString &key, int x, int y, int w, int h)
{
    auto *sb = place(new QDoubleSpinBox, x, y, w, h);
    sb->setObjectName(nameFor(key));
    sb->setDecimals(2);
    sb->setSingleStep(0.1);
    sb->setToolTip(settingTip(key));
    b->bind(sb, key, SettingType::Double);
    return sb;
}

QLineEdit *PageKit::edit(const QString &key, int x, int y, int w, int h, SettingType fallback)
{
    auto *le = place(new QLineEdit, x, y, w, h);
    le->setObjectName(nameFor(key));
    le->setToolTip(settingTip(key));
    b->bind(le, key, fallback);
    return le;
}

QComboBox *PageKit::combo(const QString &key, int x, int y, int w, int h)
{
    auto *cb = place(new QComboBox, x, y, w, h);
    cb->setObjectName(nameFor(key));
    if (!key.isEmpty()) {
        cb->setToolTip(settingTip(key));
        b->bind(cb, key);
    }
    return cb;
}

PushButton *PageKit::button(const QString &t, int x, int y, int w, int h)
{
    return place(new PushButton(t), x, y, w, h);
}

PushButton *PageKit::toggle(const QString &t, const QString &key, int x, int y, int w, int h)
{
    auto *pb = place(new PushButton(t), x, y, w, h);
    pb->setCheckable(true);
    pb->setObjectName(nameFor(key));
    pb->setToolTip(settingTip(key));
    b->bind(pb, key, SettingType::Bool);
    return pb;
}

ColorButton *PageKit::color(const QString &t, const QString &key, int x, int y, int w, int h)
{
    auto *cb = place(new ColorButton(t), x, y, w, h);
    if (cb->objectName().isEmpty())
        cb->setObjectName(nameFor(key));
    b->bind(cb, key, SettingType::Color);
    return cb;
}

void PageKit::hline(int x, int y, int w)
{
    auto *f = place(new QFrame, x, y, w, 2);
    f->setFrameShape(QFrame::HLine);
    f->setFrameShadow(QFrame::Sunken);
}

QString settingLabel(const QString &key)
{
    const SettingDef *d = findSetting(key);
    QString l = d ? d->label : key;
    static const QRegularExpression unit(QStringLiteral("\\s*\\((seconds|minutes|ms|KB/s|KB|MB|%|bytes)\\)$"));
    l.remove(unit);
    return l;
}

// ========================================================================================== Form

Form::Form(PageKit &kit, int x, int y0, int width) : k(kit), x0(x), y(y0), w(width) {}

void Form::group(const QString &title)
{
    m_groupTop = y;
    m_groupTitle = title;
    m_groupBox = k.group(title, x0, y, w, 20);
    y += 18;
    indent = 9;
}

void Form::endGroup()
{
    if (m_groupBox)
        m_groupBox->setGeometry(SR(x0, m_groupTop, w, y - m_groupTop + 5).translated(-S(SettingsDialog::kPageX), 0));
    m_groupBox = nullptr;
    indent = 0;
    y += 11;
}

// [ui-g] tool tips for the check boxes whose label does not say what they do (keyed by setting)
static QString checkTip(const QString &key)
{
    static const QHash<QString, const char *> tips = {
        {QString(overlay::Relay), QT_TR_NOOP("Pass connections on for users who cannot accept incoming ones, so they can chat and download too. Uses a little upload bandwidth.")},
        {QString(overlay::ShareHashLinks), QT_TR_NOOP("Tell the OctoMX Peer Network which other hashes (eD2k, BitTorrent, SHA-1) belong to files you have, so others find more sources.")},
        {QString(queue::HotlistPriority), QT_TR_NOOP("On the OctoMX Peer Network, users on your Hotlist are put in front of the upload queue.")},
        {QString(transfers::UploadsToHotlistOnly), QT_TR_NOOP("On the OctoMX Peer Network, only users on your Hotlist may download from you; everyone else is refused.")},
        {QString(messages::IgnoreHotlistOnly), QT_TR_NOOP("Chat lines and private messages from users who are not on your Hotlist are not shown.")},
        {QString(library::ShareIncomplete), QT_TR_NOOP("Other users may download the parts of a running download that are already checked, before the whole file is complete.")},
        {QString(bt::SuperSeeding), QT_TR_NOOP("When you are the first seed of a new torrent, hand out each piece to one peer only, so the torrent spreads faster with less upload.")},
        {QString(bt::AnonymousMode), QT_TR_NOOP("Do not tell trackers and peers which client and version you run. Some private trackers refuse such clients.")},
        {QString(bt::Lsd), QT_TR_NOOP("Find BitTorrent peers on your own local network, for example another computer at home with the same torrent.")},
        {QString(bt::Pex), QT_TR_NOOP("Peers tell each other about more peers of the same torrent, so you find sources without a tracker.")},
        {QString(bt::WebSeeds), QT_TR_NOOP("Also download torrent pieces from web servers listed in the torrent.")},
        {QString(web::ProbeOnStart), QT_TR_NOOP("Shortly after the start, check which web torrent indexes answer, so searches skip the ones that are down.")},
    };
    const auto it = tips.constFind(key);
    return it == tips.constEnd() ? QString() : QObject::tr(it.value());
}

QCheckBox *Form::check(const QString &key, const QString &text)
{
    QCheckBox *c = k.check(text.isEmpty() ? settingLabel(key) : text, key, left(), y, width());
    const QString tip = checkTip(key);
    if (!tip.isEmpty())
        c->setToolTip(tip + QLatin1Char(' ') + QObject::tr("Default: %1.").arg(settingDefault(key).toBool() ? QObject::tr("on") : QObject::tr("off")));
    y += 17;
    return c;
}

QSpinBox *Form::spin(const QString &key, const QString &text, const QString &unit, int labelW)
{
    const QString t = text.isEmpty() ? settingLabel(key) : text;
    if (labelW <= 0)
        labelW = toDesign(QFontMetrics(normalFont()).horizontalAdvance(t)) + 6;
    k.label(t, left(), y + 4, labelW - 4);
    QSpinBox *sb = k.spin(key, left() + labelW, y, 59, 20);
    if (!unit.isEmpty())
        k.label(unit, left() + labelW + 64, y + 4, width() - labelW - 64);
    y += 23;
    return sb;
}

QDoubleSpinBox *Form::dspin(const QString &key, const QString &text, const QString &unit, int labelW)
{
    const QString t = text.isEmpty() ? settingLabel(key) : text;
    if (labelW <= 0)
        labelW = toDesign(QFontMetrics(normalFont()).horizontalAdvance(t)) + 6;
    k.label(t, left(), y + 4, labelW - 4);
    QDoubleSpinBox *sb = k.dspin(key, left() + labelW, y, 59, 20);
    if (!unit.isEmpty())
        k.label(unit, left() + labelW + 64, y + 4, width() - labelW - 64);
    y += 23;
    return sb;
}

QComboBox *Form::combo(const QString &key, const QString &text, int labelW, int comboW)
{
    const QString t = text.isEmpty() ? settingLabel(key) : text;
    if (labelW <= 0)
        labelW = toDesign(QFontMetrics(normalFont()).horizontalAdvance(t)) + 6;
    if (comboW <= 0)
        comboW = width() - labelW;
    k.label(t, left(), y + 4, labelW - 4);
    QComboBox *cb = k.combo(key, left() + labelW, y, comboW, 21);
    y += 24;
    return cb;
}

QLineEdit *Form::edit(const QString &key, const QString &text, SettingType fallback)
{
    k.label(text.isEmpty() ? settingLabel(key) : text, left(), y, width());
    QLineEdit *le = k.edit(key, left(), y + 14, width(), 20, fallback);
    y += 38;
    return le;
}

QLineEdit *Form::editRow(const QString &key, const QString &text, int labelW, int editW)
{
    if (editW <= 0)
        editW = width() - labelW;
    k.label(text, left(), y + 4, labelW - 4);
    QLineEdit *le = k.edit(key, left() + labelW, y, editW, 20);
    y += 23;
    return le;
}

WrapLabel *Form::text(const QString &t, int lines)
{
    WrapLabel *l = k.wrap(t, left(), y, width(), lines * 13);
    y += lines * 13 + 4;
    return l;
}

// ============================================================================ Internet Connection

namespace {

// "Address  (if port not 1080, use ip:port format)": host + port in one edit
void bindProxyAddress(PageKit &k, QLineEdit *le)
{
    le->setObjectName(QStringLiteral("set_connection_proxyAddress"));
    k.b->bindCustom({le}, {QString(connection::ProxyHost), QString(connection::ProxyPort)}, [le, s = k.s] {
        QSignalBlocker bl(le);
        const QString host = s->s(connection::ProxyHost);
        const int port = s->i(connection::ProxyPort);
        le->setText(host.isEmpty() || port == 1080 ? host : QStringLiteral("%1:%2").arg(host).arg(port));
    });
    QObject::connect(le, &QLineEdit::editingFinished, k.b, [le, b = k.b] {
        QString t = le->text().trimmed();
        int port = 1080;
        const int c = t.lastIndexOf(QLatin1Char(':'));
        if (c > 0 && !t.endsWith(QLatin1Char(']'))) {
            bool ok = false;
            const int p = t.mid(c + 1).toInt(&ok);
            if (!ok || p < 1 || p > 65535) {
                SettingsBinder::message(le->window(), QObject::tr("OctoMX"), QObject::tr("Please enter the proxy as address or address:port."));
                b->reloadAll();
                return;
            }
            port = p;
            t = t.left(c);
        }
        b->commit(QString(connection::ProxyHost), t, le);
        b->commit(QString(connection::ProxyPort), port, le);
    });
}

} // namespace

void buildConnectionPages(SettingsDialog *d)
{
    // ---- Internet Connection [obs: settings_internet_connection.png]
    {
        PageKit k(d, d->addPage(QStringLiteral("connection"), QObject::tr("Internet Connection")));
        // [qa-1] owner request: the 13 modern connection types with their Mbit ranges. Two columns do not
        // fit the original 246-px group: the group takes the page width (6 + 7 rows) and the four buttons
        // below move down 5 px with a 29-px pitch (the dialog keeps its fixed size)
        k.group(QObject::tr("Connection Type"), 213, 29, 309, 138);
        const QStringList &types = connectionTypes();
        for (int i = 0; i < types.size(); ++i) {
            const bool col2 = i >= 6;
            const int row = col2 ? i - 6 : i;
            k.radio(connectionTypeLabel(types.at(i)), identity::Connection, types.at(i), col2 ? 348 : 223, 46 + row * 16,
                    col2 ? 172 : 123);
        }
        auto jump = [&](const QString &t, int y, const char *page, const QString &tip) {
            PushButton *pb = k.button(t, 241, y, 247, 23);
            pb->setToolTip(tip);
            QObject::connect(pb, &QPushButton::clicked, d, [d, page] { d->showPage(QString::fromLatin1(page)); });
        };
        jump(QObject::tr("Outgoing TCP Connection Settings..."), 176, "connection/outgoing",
             QObject::tr("Open the page for connections this computer makes: the time out and a proxy server."));
        jump(QObject::tr("Incoming TCP Connection Settings..."), 205, "connection/incoming",
             QObject::tr("Open the page for connections other users make to you: the TCP port and port mapping on the router."));
        // [ui-h] the UDP and DNS pages are gone (UI_SPEC 8.11): nothing read their settings; each
        // network has its own UDP port on its page, names are looked up by the operating system
        // [diag-1] the Connection Test (UI_SPEC 8.11)
        PushButton *ct = k.button(QObject::tr("Connection Test..."), 241, 234, 247, 23);
        ct->setObjectName(QStringLiteral("set_connectionTest"));
        ct->setToolTip(QObject::tr("Check the ports, the router, port forwarding and why a download is slow"));
        QObject::connect(ct, &QPushButton::clicked, d, [d] { ConnectionTestDialog::open(d->core(), d); });
        k.wrap(QObject::tr("Some of these settings take effect when OctoMX starts again."), 241, 266, 250, 28);
    }
    // ---- Outgoing TCP Connections [obs: settings_outgoing_tcp.png]
    {
        PageKit k(d, d->addPage(QStringLiteral("connection/outgoing"), QObject::tr("Outgoing TCP Connections"), QStringLiteral("connection")));
        k.label(QObject::tr("Connection attempts timeout after"), 233, 33, 166);
        k.spin(connection::ConnectTimeoutSec, 400, 28, 61, 21);
        k.label(QObject::tr("seconds"), 466, 33, 50);
        // [net-2] the proxy is for every network: SOCKS5 and HTTP (Qt has no SOCKS4 client; a stored
        // SOCKS4 is used as SOCKS5 and shown so), no separate "peers too" choice any more
        k.group(QObject::tr("Proxy Server"), 243, 63, 246, 258);
        k.radio(QObject::tr("Don't use a proxy server (connect directly)"), connection::ProxyType, QStringLiteral("none"), 258, 87, 226);
        QRadioButton *socks5 = k.radio(QObject::tr("Use SOCKS5 proxy"), connection::ProxyType, QStringLiteral("socks5"), 258, 107, 110);
        k.radio(QObject::tr("Use HTTP proxy"), connection::ProxyType, QStringLiteral("http"), 372, 107, 110);
        k.b->bindCustom({}, {QString(connection::ProxyType)}, [socks5, s = k.s] {
            if (s->s(connection::ProxyType) == QLatin1String("socks4")) {
                QSignalBlocker bl(socks5);
                socks5->setChecked(true);
            }
        });
        StaticText *l1 = k.label(QObject::tr("Address  (if port not 1080, use ip:port format)"), 261, 135, 226);
        auto *addr = k.place(new QLineEdit, 261, 148, 214, 20);
        addr->setToolTip(settingTip(connection::ProxyHost));
        bindProxyAddress(k, addr);
        StaticText *l2 = k.label(QObject::tr("Proxy Login Name (if required by proxy server)"), 261, 171, 226);
        auto *user = k.place(new QLineEdit, 261, 184, 214, 20);
        user->setObjectName(QStringLiteral("set_connection_proxyUser"));
        user->setToolTip(settingTip(connection::ProxyUser));
        // the login name also switches authentication on/off (the original has no separate check)
        k.b->bindCustom({user}, {QString(connection::ProxyUser), QString(connection::ProxyAuth)}, [user, s = k.s] {
            QSignalBlocker bl(user);
            user->setText(s->s(connection::ProxyUser));
        });
        QObject::connect(user, &QLineEdit::editingFinished, k.b, [user, b = k.b] {
            b->commit(QString(connection::ProxyUser), user->text().trimmed(), user);
            b->commit(QString(connection::ProxyAuth), !user->text().trimmed().isEmpty(), user);
        });
        StaticText *l3 = k.label(QObject::tr("Password (if required by proxy server)"), 261, 205, 226);
        QLineEdit *pw = k.edit(connection::ProxyPassword, 261, 218, 214, 20);
        pw->setEchoMode(QLineEdit::Password);
        QCheckBox *hn = k.check(QObject::tr("Push host names through SOCKS5 proxy"), connection::ProxyHostnames, 259, 244, 226);
        WrapLabel *note = k.wrap(QObject::tr("Every network goes through the proxy. Off while it is set: Kad, the OctoMX network, "
                                             "Rats and UDP; nobody can connect to you."),
                                 261, 264, 222, 52);
        note->setObjectName(QStringLiteral("set_connection_proxyCoverage"));
        note->setToolTip(QObject::tr("What a proxy covers and what it switches off, network by network (the Connection Test says the same)."));
        // fields are disabled while "Don't use a proxy server" is selected (as the original)
        QList<QWidget *> dep = {l1, addr, l2, user, l3, pw, hn};
        k.b->bindCustom({}, {QString(connection::ProxyType)}, [dep, s = k.s] {
            const QString t = s->s(connection::ProxyType);
            for (QWidget *w : dep)
                w->setEnabled(t != QLatin1String("none"));
            dep.at(6)->setEnabled(t == QLatin1String("socks5") || t == QLatin1String("socks4"));
        });
    }
    // ---- Incoming TCP Connections [obs: settings_incoming_tcp.png, ref4]
    const QString restartHeader = QObject::tr("THESE SETTINGS TAKE EFFECT WHEN OCTOMX STARTS AGAIN.");
    auto header = [&](PageKit &k) {
        k.label(QObject::tr("THESE SETTINGS TAKE EFFECT"), 222, 10, 280, Qt::AlignHCenter);
        k.label(QObject::tr("WHEN OCTOMX STARTS AGAIN."), 222, 23, 280, Qt::AlignHCenter);
    };
    {
        PageKit k(d, d->addPage(QStringLiteral("connection/incoming"), QObject::tr("Incoming TCP Connections"), QStringLiteral("connection")));
        header(k);
        k.radio(QObject::tr("Listen on port"), connection::AcceptIncoming, true, 223, 67, 86);
        k.edit(connection::TcpPort, 310, 65, 56, 21);
        k.label(QObject::tr("for incoming TCP connections"), 371, 69, 150);
        k.wrap(QObject::tr("Important:  Behind a router, a firewall (program or device), shared internet access or a LAN, "
                           "this port must be forwarded to this computer, by hand or with the automatic mapping below.  "
                           "When that cannot be done, choose the other option."),
               242, 90, 272, 92);
        k.radio(QObject::tr("Unable to accept incoming TCP connections"), connection::AcceptIncoming, false, 223, 205, 280);
        k.wrap(QObject::tr("For a computer that cannot be reached from outside (a LAN, a firewall or a router you "
                           "cannot change): downloads then only come from users who can be reached."),
               242, 223, 272, 53);
        // NG: automatic port mapping (UI_SPEC §8.9), one check for UPnP + NAT-PMP
        auto *map = k.place(new QCheckBox(QObject::tr("Map the ports automatically (UPnP / NAT-PMP)")), 223, 290, 290, 16);
        map->setObjectName(QStringLiteral("set_connection_portMapping"));
        map->setToolTip(settingTip(connection::Upnp));
        k.b->bindCustom({map}, {QString(connection::Upnp), QString(connection::NatPmp)}, [map, s = k.s] {
            QSignalBlocker bl(map);
            map->setChecked(s->b(connection::Upnp) || s->b(connection::NatPmp));
        });
        QObject::connect(map, &QCheckBox::toggled, k.b, [map, b = k.b](bool on) {
            b->commit(QString(connection::Upnp), on, map);
            b->commit(QString(connection::NatPmp), on, map);
        });
        k.check(QObject::tr("Pick random ports at every start"), connection::RandomizePorts, 223, 307, 290);
    }
    // ---- NG: Connection Options (interface, IPv6, firewall detection, connection cap)
    {
        PageKit k(d, d->addPage(QStringLiteral("connection/options"), QObject::tr("Connection Options"), QStringLiteral("connection")));
        Form f(k);
        f.group(QObject::tr("Firewall"));
        f.combo(connection::Firewalled, QObject::tr("Incoming connections"), 110);
        f.endGroup();
        f.group(QObject::tr("Network Adapter"));
        f.editRow(connection::BindInterface, QObject::tr("Adapter (empty = all)"), 120);
        f.check(connection::Ipv6, QObject::tr("Use IPv6 when available"));
        f.endGroup();
        // [net-2] what the adapter choice really does, on every network
        f.text(QObject::tr("Every network, web request, the update check and port mapping use only this adapter (a VPN "
                           "adapter's name or address). When it is missing, every network stops until it is back. "
                           "IPv6: BitTorrent, the OctoMX network, Rats and HTTP; eD2k, Kad and Gnutella are IPv4 only."),
               5);
    }
}

// ============================================================================ OctoMX Peer Network

void buildPeerNetworkPages(SettingsDialog *d)
{
    // [obs: settings_winmx_peer_network.png]
    {
        PageKit k(d, d->addPage(QStringLiteral("wpn"), QObject::tr("OctoMX Peer Network")));
        k.toggle(QObject::tr("Automatically connect on startup"), network::ConnectOnStart, 221, 9, 291, 21)
            ->setToolTip(QObject::tr("When pressed in, OctoMX joins its networks by itself at every start. "
                                     "Otherwise you connect from the Networks page."));
        // [ui-h] the Preferred Peer Cache and the Primary Connection Advanced Settings of WinMX 3.53 are
        // gone (UI_SPEC 8.11): the OctoMX Peer Network finds its peers by itself, has no
        // search thread and no separate routing budget, so nothing read them
        k.wrap(QObject::tr("OctoMX finds the other users of the OctoMX Peer Network by itself: there is no peer cache "
                           "to choose and no search thread to tune. The limits for your connection are on the "
                           "\"Bandwidth Throttle\" page, the peer network's own options on \"Peer Network Options\" below."),
               221, 40, 291, 66);
    }
    // ---- NG: the overlay itself (chat, browse, hash links)
    {
        PageKit k(d, d->addPage(QStringLiteral("wpn/overlay"), QObject::tr("Peer Network Options"), QStringLiteral("wpn")));
        Form f(k);
        f.group(QObject::tr("Identity"));
        QLineEdit *name = f.editRow(identity::ScreenName, QObject::tr("Screen name"), 90);
        name->setMaxLength(40);
        f.check(identity::PrimaryConnection, QObject::tr("Primary connection (accept incoming, route for others)"));
        f.endGroup();
        f.group(QObject::tr("OctoMX Peer Network"));
        f.check(overlay::Enabled, QObject::tr("Enable the OctoMX peer network"));
        f.check(overlay::Relay);
        f.check(overlay::ShareHashLinks);
        f.spin(overlay::MaxPeers, QObject::tr("Maximum peers"));
        f.edit(overlay::BootstrapPeers, QObject::tr("Bootstrap peers (host:port; separated by ;)"));
        f.endGroup();
    }
}

// ================================================================== Other Networks + protocols

void buildOtherNetworkPages(SettingsDialog *d)
{
    // [obs: settings_opennap_networks.png], "OpenNap Networks" → "Other Networks" (UI_SPEC §8.9)
    {
        PageKit k(d, d->addPage(QStringLiteral("other"), QObject::tr("Other Networks")));
        k.label(QObject::tr("Default Login"), 260, 58, 200);
        QLineEdit *login = k.edit(other::DefaultLogin, 259, 70, 208, 22);
        login->setPlaceholderText(QObject::tr("(your screen name)"));
        login->setToolTip(QObject::tr("The user name OctoMX gives eD2k servers and other users there. Empty: your screen name. "
                                      "Change it only when you want another name on eD2k."));
        // [ui-h] the password, login time out, connection attempt limit and raw IP switch of the OpenNap
        // page are gone (UI_SPEC 8.11): no network of OctoMX logs in with a password; the eD2k page
        // has its own server connection settings
        PushButton *reset = k.button(QObject::tr("Reset all server login params to default"), 259, 104, 208, 23);
        reset->setObjectName(QStringLiteral("otherResetLogin"));
        reset->setToolTip(QObject::tr("Clear the default login above, so your screen name is used again."));
        QObject::connect(reset, &QPushButton::clicked, d, [d] { d->binder()->resetKeys({QString(other::DefaultLogin)}); });
        k.wrap(QObject::tr("The networks below have their own pages for ports, servers and limits."), 259, 140, 220, 28);
    }
    const QString parent = QStringLiteral("other");
    // ---- BitTorrent
    {
        PageKit k(d, d->addPage(QStringLiteral("bt"), QObject::tr("BitTorrent"), parent));
        Form f(k);
        f.group(QObject::tr("BitTorrent Network"));
        f.check(bt::Enabled, QObject::tr("Enable BitTorrent"));
        f.spin(bt::ListenPort, QObject::tr("Listen on port"), QObject::tr("(0 = random)"));
        f.check(bt::Dht);
        f.check(bt::Pex);
        f.check(bt::Lsd);
        f.check(bt::Utp, QObject::tr("Enable uTP (micro transport protocol)"));
        f.combo(bt::Encryption, QObject::tr("Encryption"), 70);
        f.endGroup();
        f.group(QObject::tr("Connections and Upload Slots"));
        f.spin(bt::MaxConnections, QObject::tr("Max connections"), QString(), 120);
        f.spin(bt::MaxConnectionsPerTorrent, QObject::tr("Max per torrent"), QString(), 120);
        f.spin(bt::MaxUploads, QObject::tr("Upload slots"), QString(), 120);
        f.spin(bt::MaxUploadsPerTorrent, QObject::tr("Upload slots per torrent"), QString(), 120);
        f.endGroup();
    }
    {
        PageKit k(d, d->addPage(QStringLiteral("bt/seeding"), QObject::tr("Torrent Seeding"), QStringLiteral("bt")));
        Form f(k);
        f.group(QObject::tr("Active Torrents"));
        f.spin(bt::MaxActiveDownloads, QObject::tr("Active downloads"), QString(), 120);
        f.spin(bt::MaxActiveSeeds, QObject::tr("Active seeds"), QString(), 120);
        f.endGroup();
        f.group(QObject::tr("Seeding"));
        f.check(bt::SeedCompleted);
        f.dspin(bt::SeedRatioLimit, QObject::tr("Stop seeding at ratio"), QObject::tr("(0 = never)"), 136);
        f.spin(bt::SeedTimeLimitMin, QObject::tr("Stop seeding after"), QObject::tr("min (0 = never)"), 136);   // [qa-2] "minutes ..." was cut off
        f.combo(bt::SeedLimitAction, QObject::tr("Then"), 136);
        f.check(bt::SeedLibrary);
        f.check(bt::SuperSeeding);
        f.endGroup();
    }
    {
        PageKit k(d, d->addPage(QStringLiteral("bt/trackers"), QObject::tr("Trackers and Peers"), QStringLiteral("bt")));
        Form f(k);
        f.edit(bt::ExtraTrackers, QObject::tr("Extra trackers added to magnet links (separated by ;)"));
        f.edit(bt::DhtBootstrapNodes, QObject::tr("DHT bootstrap nodes (host:port; separated by ;)"));
        f.check(bt::AnnounceToAllTrackers);
        f.check(bt::ValidateHttpsTrackers);
        f.check(bt::WebSeeds);
        f.check(bt::AllowMultipleConnectionsPerIp);
        f.check(bt::AnonymousMode);
        f.check(bt::Preallocate, QObject::tr("Pre-allocate disk space for torrents"));
    }
    // ---- eD2k
    {
        PageKit k(d, d->addPage(QStringLiteral("ed2k"), QObject::tr("eD2k Servers"), parent));
        Form f(k);
        f.group(QObject::tr("eD2k Network"));
        f.check(ed2k::Enabled);
        const int y = f.y;
        f.spin(ed2k::TcpPort, QObject::tr("TCP port"), QString(), 60);
        f.y = y;
        f.indent = 9 + 140;
        f.spin(ed2k::UdpPort, QObject::tr("UDP port"), QString(), 60);
        f.indent = 9;
        f.spin(ed2k::MaxConnections, QObject::tr("Max connections"), QString(), 120);
        f.spin(ed2k::MaxSourcesPerFile, QObject::tr("Max sources per file"), QString(), 120);
        f.check(ed2k::Obfuscation);
        f.check(ed2k::RequireObfuscation);
        f.endGroup();
        f.group(QObject::tr("Servers"));
        f.check(ed2k::AutoConnect);
        f.check(ed2k::Reconnect);
        f.check(ed2k::StaticServersOnly);
        f.check(ed2k::AddServersFromServer);
        f.check(ed2k::AutoUpdateServerList);
        f.editRow(ed2k::ServerListUrl, QObject::tr("Server list URL"), 80);
        f.endGroup();
    }
    {
        PageKit k(d, d->addPage(QStringLiteral("ed2k/connections"), QObject::tr("eD2k Connections"), QStringLiteral("ed2k")));
        Form f(k);
        f.group(QObject::tr("Connections and Uploads"));
        f.spin(ed2k::UploadSlots, QObject::tr("Upload slots"), QObject::tr("0 = global"), 168);
        f.spin(ed2k::MaxHalfOpen, QObject::tr("Connection attempts at once"), QString(), 168);
        f.spin(ed2k::MaxConnectsPer5s, QObject::tr("New connections per 5 seconds"), QString(), 168);
        f.spin(ed2k::ParallelServerConnects, QObject::tr("Servers tried at the same time"), QString(), 168);
        f.check(ed2k::CompressUploads, QObject::tr("Compress uploads when it helps"));
        f.endGroup();
        f.group(QObject::tr("UDP"));
        f.check(ed2k::UdpPing, QObject::tr("Ping servers over UDP"));
        f.check(ed2k::UdpSearch, QObject::tr("Search other servers over UDP (global search)"));
        f.check(ed2k::UdpSources, QObject::tr("Ask other servers for sources over UDP"));
        f.endGroup();
    }
    // ---- Kad
    {
        PageKit k(d, d->addPage(QStringLiteral("kad"), QObject::tr("Kad"), parent));
        Form f(k);
        f.group(QObject::tr("Kad Network (serverless eD2k)"));
        f.check(kad::Enabled, QObject::tr("Enable Kad"));
        // [net-2] Kad has no port of its own: it uses the eD2k UDP socket (kad/udpPort was read by nothing)
        f.text(QObject::tr("Kad uses the eD2k UDP port (eD2k Servers page). Kad is off while a proxy is set: it works over UDP only."), 2);
        f.check(kad::AutoBootstrap);
        f.editRow(kad::NodesDatUrl, QObject::tr("nodes.dat URL"), 80);
        f.edit(kad::BootstrapNodes, QObject::tr("Bootstrap nodes (host:port; separated by ;)"));
        f.endGroup();
        f.group(QObject::tr("Kad Options"));
        f.check(kad::Publish, QObject::tr("Publish my shared files in Kad"));
        f.check(kad::StoreIndex, QObject::tr("Keep what other users publish to me"));
        f.check(kad::FirewallChecks, QObject::tr("Check whether I am firewalled"));
        f.check(kad::Buddies, QObject::tr("Find / act as a buddy for firewalled users"));
        f.spin(kad::MaxPacketsPerSec, QObject::tr("Send at most"), QObject::tr("packets per second"));
        f.endGroup();
    }
    // ---- Gnutella
    {
        PageKit k(d, d->addPage(QStringLiteral("gnutella"), QObject::tr("Gnutella"), parent));
        Form f(k);
        f.group(QObject::tr("Gnutella Network"));
        f.check(gnutella::Enabled);
        f.spin(gnutella::Port, QObject::tr("Listen on port"));
        f.spin(gnutella::MaxUltrapeers, QObject::tr("Ultrapeer connections (leaf mode)"));
        f.check(gnutella::BrowseHost, QObject::tr("Allow other users to browse my files"));
        // [enc-1] TLS on Gnutella TCP connections, like BitTorrent's Encryption choice
        f.combo(gnutella::Encryption, QObject::tr("Encryption"), 70)
            ->setToolTip(QObject::tr("TLS for ultrapeer links, downloads, uploads, push and browse. "
                                     "Prefer: encrypt when the other side can, else plain. Require: only TLS (fewer sources). "
                                     "It hides the content from simple filtering by a provider; it does not hide your IP "
                                     "address and is not anonymity. UDP (host caches) stays plain."));
        f.endGroup();
        f.check(gnutella::AutoBootstrap, QObject::tr("Find hosts automatically (GWebCaches, UDP host caches)"));
        f.edit(gnutella::GWebCaches, QObject::tr("GWebCache URLs (separated by ;)"));
        f.edit(gnutella::UdpHostCaches, QObject::tr("Extra UDP host caches (host:port; separated by ;)"));
        f.edit(gnutella::BootstrapHosts, QObject::tr("Bootstrap hosts (host:port; separated by ;)"));
    }
    {
        PageKit k(d, d->addPage(QStringLiteral("gnutella/uploads"), QObject::tr("Gnutella Uploads"), QStringLiteral("gnutella")));
        Form f(k);
        f.group(QObject::tr("Uploads"));
        f.spin(gnutella::UploadSlots, QObject::tr("Upload slots"), QObject::tr("(0 = global limit)"), 146);   // [qa-2] 150 cut the unit
        f.spin(gnutella::UploadQueueSize, QObject::tr("Remote queue length"), QString(), 150);
        f.spin(gnutella::UploadQueuePollSec, QObject::tr("Queued users ask every"), QObject::tr("seconds"), 150);
        f.endGroup();
    }
    // ---- HTTP
    {
        PageKit k(d, d->addPage(QStringLiteral("http"), QObject::tr("HTTP / Web Sources"), parent));
        Form f(k);
        f.group(QObject::tr("HTTP and FTP Downloads"));
        f.check(http::Enabled, QObject::tr("Enable HTTP sources"));
        f.spin(http::MaxConnectionsPerServer, QObject::tr("Connections per web server"), QString(), 170);
        f.spin(http::ConnectionsPerSource, QObject::tr("Parallel requests per source"), QString(), 170);
        f.spin(http::TimeoutSec, QObject::tr("Timeout after"), QObject::tr("seconds"), 170);
        f.spin(http::StallTimeoutSec, QObject::tr("Restart stalled requests after"), QObject::tr("seconds"), 170);
        f.spin(http::MaxRetries, QObject::tr("Retries per request"), QString(), 170);
        f.spin(http::RetryBaseMs, QObject::tr("First retry after"), QObject::tr("ms"), 170);
        f.check(http::VerifyTls);
        f.check(http::AllowInsecureRedirect);
        f.endGroup();
        f.editRow(http::UserAgent, QObject::tr("User agent"), 64);
    }
    // ---- Web torrent search
    {
        PageKit k(d, d->addPage(QStringLiteral("web"), QObject::tr("Web Search"), parent));
        Form f(k);
        f.group(QObject::tr("Web Torrent Indexes"));
        f.check(web::Enabled);
        f.check(web::ProbeOnStart);
        f.spin(web::MaxResults, QObject::tr("Maximum results per search"), QString(), 150);
        f.spin(web::TimeoutSec, QObject::tr("Timeout after"), QObject::tr("seconds"), 150);
        f.endGroup();
        f.edit(web::Trackers, QObject::tr("Trackers for web magnets (empty = extra trackers)"));
        f.edit(web::AdultKeywords, QObject::tr("Adult filter keywords (empty = built-in list)"));
        f.editRow(web::UserAgent, QObject::tr("User agent"), 64);
    }
    // ---- rats-search P2P index
    {
        PageKit k(d, d->addPage(QStringLiteral("rats"), QObject::tr("Rats Search"), parent));
        Form f(k);
        f.group(QObject::tr("rats-search Torrent Network"));
        f.check(rats::Enabled, QObject::tr("Enable the rats-search network"));
        f.check(rats::SafeSearch, QObject::tr("Safe search"));
        f.spin(rats::ListenPort, QObject::tr("Listen on port"), QString(), 120);
        f.spin(rats::DhtPort, QObject::tr("DHT port"), QObject::tr("(0 = random)"), 120);
        f.spin(rats::MaxPeers, QObject::tr("Maximum peers"), QString(), 120);
        f.spin(rats::MaxResults, QObject::tr("Maximum results"), QString(), 120);
        f.endGroup();
        f.edit(rats::BootstrapPeers, QObject::tr("Bootstrap peers (host:port; separated by ;)"));
    }
}

} // namespace mx::ui
