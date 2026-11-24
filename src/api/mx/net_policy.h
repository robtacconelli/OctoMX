// SPDX-License-Identifier: GPL-3.0-or-later
// [net-2] OctoMX: the connection policy every network follows (proxy, chosen adapter, IPv6).
//
// The settings on the Internet Connection pages apply to the whole program. This is the one
// place that turns them into what a socket may do:
//  * Proxy (connection/proxyType, host, port, user, password, proxyHostnames): every TCP
//    connection of every network goes through it (Qt sockets: QNetworkProxy; libtorrent: its own
//    proxy settings). UDP cannot go through it in the Qt networks: while a proxy is set they send
//    no UDP at all (udpAllowed() is false), and the networks that live on UDP or on librats (Kad,
//    the OctoMX network, Rats) do not run (verdict()). Nothing ever falls back to a direct
//    connection. SOCKS4 is not offered any more: a stored "socks4" is used as SOCKS5.
//  * Adapter (connection/bindInterface: an interface name, its friendly name, or an address):
//    every socket is bound to that adapter's address, or is not opened. When the adapter is
//    missing or has lost its address, every network stops (verdict()), the Qt sockets refuse to
//    open (prepareTcp(), bindUdp(), listenAddress()) and web requests fail (applyTo(): a local
//    relay that binds each connection). With a proxy as well, the proxy must be reached through
//    the adapter (or run on this computer), checked by route every two seconds.
//  * IPv6 (connection/ipv6): only BitTorrent and the OctoMX network use IPv6 at all.
// Name lookups: with a SOCKS5 proxy and "push host names through the proxy", host names are
// handed to the proxy (remoteDns). Otherwise the system resolver answers (a VPN normally sets
// its own DNS server; OctoMX has no resolver of its own).
#pragma once

#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtNetwork/QAbstractSocket>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QNetworkProxy>

#include <functional>

#include "mx/types.h"

class QNetworkAccessManager;
class QTimer;
class QUdpSocket;

namespace mx {
class Settings;
namespace netpolicy {

struct Proxy {
    enum Kind { None = 0, Socks5, Http };
    Kind kind = None;
    QString host;
    quint16 port = 0;
    QString user, password;   // empty user: no authentication
    bool remoteDns = true;    // SOCKS5: host names are resolved by the proxy
    bool active() const { return kind != None; }
    QString describe() const; // "SOCKS5 proxy 10.0.0.1:1080"
};

struct Adapter {
    QString wanted;                 // the setting, trimmed ("" = any adapter)
    bool present = true;            // nothing wanted, or the adapter is there with an address
    QString name;                   // the interface it resolved to (or the address)
    QList<QHostAddress> addresses;  // usable addresses (no IPv6 link-local)
    QHostAddress v4() const;
    QHostAddress v6() const;
    QString fingerprint() const;    // changes when the adapter or its addresses change
};

struct Policy {
    Proxy proxy;
    QString bind;      // connection/bindInterface, trimmed
    bool ipv6 = true;  // connection/ipv6
    bool bindWanted() const { return !bind.isEmpty(); }
    QString fingerprint() const;
};

Policy fromSettings(const Settings *s);

// The adapter connection/bindInterface names, looked up now (cached for a second).
Adapter resolveAdapter(const QString &wanted);
using AdapterResolver = std::function<Adapter(const QString &)>;
void setAdapterResolver(const AdapterResolver &r);   // tests; an empty function restores the real one
void clearAdapterCache();

QNetworkProxy qtProxy(const Proxy &p);   // NoProxy for Proxy::None

// The local address the operating system would send from towards `target` (a UDP route
// lookup: nothing is sent). Null when there is no route.
QHostAddress localAddressTowards(const QHostAddress &target);

struct Verdict {
    bool allowed = true;
    QString reason;   // user text when not allowed
};

// Route facts the Monitor collects (true when the rule does not apply)
struct Routes {
    bool proxyViaAdapter = true;     // with a proxy and an adapter: the proxy is reached through it
    QString proxyProblem;            // why not (looking it up, other route)
    bool internetViaAdapter = true;  // with an adapter: the route to the internet uses it
};

// What network `n` may do (Net::Unknown: traffic of no network, the update check and the
// Connection Test). Pure: the caller supplies the adapter and the route facts.
Verdict verdictFor(Net n, const Policy &p, const Adapter &a, const Routes &r);
// The same with the facts looked up now (the Monitor's when there is one, else a quick check)
Verdict verdict(Net n, const Settings *s);

// ---- socket helpers: every connection of the Qt based networks is made through these
// Outgoing TCP: sets the proxy, or binds the socket to the adapter. False (with *why) when the
// socket must not be opened: then do not call connectToHost().
bool prepareTcp(QAbstractSocket *sock, const Settings *s, QString *why = nullptr,
                QAbstractSocket::NetworkLayerProtocol family = QAbstractSocket::IPv4Protocol);
// UDP: false while a proxy is set (no UDP at all) or when the adapter is missing.
bool udpAllowed(const Settings *s, QString *why = nullptr);
bool bindUdp(QUdpSocket *sock, const Settings *s, quint16 port, QString *why = nullptr,
             int bindMode = 0 /* QAbstractSocket::BindMode */);
// Where to listen for incoming TCP: AnyIPv4, or the adapter's IPv4 address. *ok false when the
// adapter is missing (do not listen then).
QHostAddress listenAddress(const Settings *s, bool *ok, QString *why = nullptr);
// HTTP(S) of a network or the program: the proxy, or (adapter chosen, no proxy) the local relay
// that binds every connection to the adapter and refuses when it is missing.
void applyTo(QNetworkAccessManager *nam, const Settings *s);
// The local relay that binds connections to `bind` (refuses while it is missing): for a
// QNetworkAccessManager that must keep to the adapter without a Settings object.
QNetworkProxy adapterRelayProxy(const QString &bind, bool ipv6);
// Networks with their own sockets (librats): the address to bind every socket to: "" (all,
// both families), "0.0.0.0" (IPv6 off) or the adapter's address. *ok false: do not start.
QString bindAddressFor(const Settings *s, bool *ok, QString *why = nullptr);
// Host names: true when a SOCKS5 proxy resolves them (do not look them up here then).
bool proxyResolvesNames(const Settings *s);

// ---- the monitor: one per program (the core makes it); watches the adapter and the routes
// every two seconds while an adapter is chosen, and tells when what is allowed changes.
class Monitor : public QObject {
    Q_OBJECT
public:
    explicit Monitor(Settings *settings, QObject *parent = nullptr);
    ~Monitor() override;
    static Monitor *instance();   // the one made last, or null

    Policy policy() const { return m_policy; }
    Adapter adapter() const { return m_adapter; }
    Routes routes() const { return m_routes; }
    // with the settings as they are now (a change not yet rechecked is rechecked first)
    Verdict verdict(Net n) const;
    // one line for the status bar / Connection Test: what is reduced or off, and why
    QString summary() const;
    void recheck();               // now (tests, settings changes)
    void setIntervalMs(int ms);   // tests

signals:
    void changed();               // the policy, the adapter or a route fact changed

private:
    void lookupProxy();
    Settings *m_settings;
    Policy m_policy;
    Adapter m_adapter;
    Routes m_routes;
    QString m_state;
    QTimer *m_timer = nullptr;
    QHostAddress m_proxyIp;
    QString m_proxyIpFor;
    int m_lookupId = -1;
};

// The text the Settings pages and the Connection Test show for a proxy: what goes through it,
// what is off. Kept here so every place says the same.
QString proxyCoverageText();
QString adapterCoverageText();

} // namespace netpolicy
} // namespace mx
