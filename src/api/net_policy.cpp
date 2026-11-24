// SPDX-License-Identifier: GPL-3.0-or-later
// [net-2] OctoMX: the connection policy (see mx/net_policy.h).
#include "mx/net_policy.h"
#include "bind_relay.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QLoggingCategory>
#include <QtCore/QMutex>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtNetwork/QHostInfo>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkInterface>
#include <QtNetwork/QUdpSocket>

#include "mx/services.h"
#include "mx/settings_keys.h"

Q_LOGGING_CATEGORY(lcPolicy, "mx.netpolicy")

namespace mx {
namespace netpolicy {

namespace {

QVariant sv(const Settings *s, QLatin1StringView k, const QVariant &def)
{
    if (!s) return def;
    const QVariant v = s->value(QString(k));
    return v.isValid() ? v : def;
}

QMutex g_mutex;
AdapterResolver g_resolver;
struct CacheEntry { Adapter a; QElapsedTimer age; };
QHash<QString, CacheEntry> g_cache;
QPointer<Monitor> g_monitor;

QString stripBrackets(QString s)
{
    if (s.startsWith(QLatin1Char('[')) && s.endsWith(QLatin1Char(']'))) s = s.mid(1, s.size() - 2);
    return s;
}

bool usable(const QHostAddress &a)
{
    if (a.isNull()) return false;
    if (a.protocol() == QAbstractSocket::IPv6Protocol && a.isLinkLocal()) return false;
    return a.protocol() == QAbstractSocket::IPv4Protocol || a.protocol() == QAbstractSocket::IPv6Protocol;
}

Adapter realResolve(const QString &wanted)
{
    Adapter a;
    a.wanted = wanted;
    if (wanted.isEmpty()) return a;
    a.present = false;
    QHostAddress lit;
    if (lit.setAddress(stripBrackets(wanted))) {
        // an address: it must be one of this computer's (a bind test answers that on every
        // system, also for 127.0.0.2 and the like that no interface lists)
        a.name = lit.toString();
        QUdpSocket probe;
        probe.setProxy(QNetworkProxy::NoProxy);
        if (probe.bind(lit, 0)) {
            a.present = true;
            a.addresses << lit;
        }
        return a;
    }
    const QList<QNetworkInterface> all = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &i : all) {
        if (i.name().compare(wanted, Qt::CaseInsensitive) != 0
            && i.humanReadableName().compare(wanted, Qt::CaseInsensitive) != 0)
            continue;
        a.name = i.humanReadableName().isEmpty() ? i.name() : i.humanReadableName();
        if (!(i.flags() & QNetworkInterface::IsUp)) break;
        for (const QNetworkAddressEntry &e : i.addressEntries())
            if (usable(e.ip())) a.addresses << e.ip();
        a.present = !a.addresses.isEmpty();
        break;
    }
    return a;
}

bool isNameLiteral(const QString &host, QHostAddress *out)
{
    QHostAddress a;
    if (!a.setAddress(stripBrackets(host))) return false;
    if (out) *out = a;
    return true;
}

} // namespace

// ------------------------------------------------------------------------------------ values
QString Proxy::describe() const
{
    const QString k = kind == Http ? QStringLiteral("HTTP") : QStringLiteral("SOCKS5");
    return QStringLiteral("%1 proxy %2:%3").arg(k, host.contains(QLatin1Char(':')) ? QStringLiteral("[%1]").arg(host) : host).arg(port);
}

QHostAddress Adapter::v4() const
{
    for (const QHostAddress &h : addresses)
        if (h.protocol() == QAbstractSocket::IPv4Protocol) return h;
    return QHostAddress();
}

QHostAddress Adapter::v6() const
{
    for (const QHostAddress &h : addresses)
        if (h.protocol() == QAbstractSocket::IPv6Protocol) return h;
    return QHostAddress();
}

QString Adapter::fingerprint() const
{
    QStringList l;
    for (const QHostAddress &h : addresses) l << h.toString();
    l.sort();
    return QStringLiteral("%1|%2|%3").arg(wanted, present ? QStringLiteral("1") : QStringLiteral("0"), l.join(QLatin1Char(',')));
}

QString Policy::fingerprint() const
{
    return QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8").arg(int(proxy.kind)).arg(proxy.host).arg(proxy.port)
        .arg(proxy.user, proxy.password, proxy.remoteDns ? QStringLiteral("r") : QStringLiteral("l"), bind,
             ipv6 ? QStringLiteral("6") : QStringLiteral("4"));
}

Policy fromSettings(const Settings *s)
{
    namespace c = keys::connection;
    Policy p;
    const QString type = sv(s, c::ProxyType, QStringLiteral("none")).toString().trimmed().toLower();
    if (type == QLatin1String("socks5") || type == QLatin1String("socks4"))   // SOCKS4: no client in Qt; SOCKS5 instead
        p.proxy.kind = Proxy::Socks5;
    else if (type == QLatin1String("http"))
        p.proxy.kind = Proxy::Http;
    if (p.proxy.active()) {
        p.proxy.host = sv(s, c::ProxyHost, QString()).toString().trimmed();
        p.proxy.port = quint16(qBound(0, sv(s, c::ProxyPort, 1080).toInt(), 65535));
        if (sv(s, c::ProxyAuth, false).toBool()) {
            p.proxy.user = sv(s, c::ProxyUser, QString()).toString();
            p.proxy.password = sv(s, c::ProxyPassword, QString()).toString();
        }
        p.proxy.remoteDns = p.proxy.kind == Proxy::Http || sv(s, c::ProxyHostnames, true).toBool();
    }
    p.bind = sv(s, c::BindInterface, QString()).toString().trimmed();
    p.ipv6 = sv(s, c::Ipv6, true).toBool();
    return p;
}

Adapter resolveAdapter(const QString &wantedIn)
{
    const QString wanted = wantedIn.trimmed();
    if (wanted.isEmpty()) {
        Adapter a;
        return a;
    }
    QMutexLocker lock(&g_mutex);
    auto it = g_cache.find(wanted);
    if (it != g_cache.end() && it->age.isValid() && it->age.elapsed() < 1000) return it->a;
    const AdapterResolver r = g_resolver;
    lock.unlock();
    Adapter a = r ? r(wanted) : realResolve(wanted);
    a.wanted = wanted;
    lock.relock();
    CacheEntry &e = g_cache[wanted];
    e.a = a;
    e.age.start();
    return a;
}

void setAdapterResolver(const AdapterResolver &r)
{
    QMutexLocker lock(&g_mutex);
    g_resolver = r;
    g_cache.clear();
}

void clearAdapterCache()
{
    QMutexLocker lock(&g_mutex);
    g_cache.clear();
}

QNetworkProxy qtProxy(const Proxy &p)
{
    if (!p.active()) return QNetworkProxy(QNetworkProxy::NoProxy);
    QNetworkProxy q(p.kind == Proxy::Http ? QNetworkProxy::HttpProxy : QNetworkProxy::Socks5Proxy,
                    stripBrackets(p.host), p.port);
    if (!p.user.isEmpty()) {
        q.setUser(p.user);
        q.setPassword(p.password);
    }
    QNetworkProxy::Capabilities caps = q.capabilities();
    // HTTP CONNECT always takes the host name; SOCKS5 as the user chose
    caps.setFlag(QNetworkProxy::HostNameLookupCapability, p.kind == Proxy::Http || p.remoteDns);
    // UDP cannot go through: the networks send no UDP while a proxy is set (udpAllowed())
    caps.setFlag(QNetworkProxy::UdpTunnelingCapability, false);
    caps.setFlag(QNetworkProxy::ListeningCapability, false);
    q.setCapabilities(caps);
    return q;
}

QHostAddress localAddressTowards(const QHostAddress &target)
{
    if (target.isNull()) return QHostAddress();
    QUdpSocket s;
    s.setProxy(QNetworkProxy::NoProxy);
    s.connectToHost(target, 9);   // UDP: only selects the route, nothing is sent
    if (s.state() != QAbstractSocket::ConnectedState && !s.waitForConnected(200)) return QHostAddress();
    QHostAddress a = s.localAddress();
    s.close();
    if (a.isNull() || a == QHostAddress::AnyIPv4 || a == QHostAddress::Any || a == QHostAddress::AnyIPv6) return QHostAddress();
    return a;
}

// ------------------------------------------------------------------------------------ verdicts
Verdict verdictFor(Net n, const Policy &p, const Adapter &a, const Routes &r)
{
    Verdict v;
    auto no = [&v](const QString &why) {
        v.allowed = false;
        v.reason = why;
        return v;
    };
    if (p.proxy.active() && (p.proxy.host.isEmpty() || p.proxy.port == 0))
        return no(QStringLiteral("Off: a proxy is chosen but its address is empty (Internet Connection, Outgoing)"));
    if (p.bindWanted()) {
        if (!a.present)
            return no(QStringLiteral("Waiting for the network adapter \"%1\": OctoMX uses no other one").arg(p.bind));
        const bool v4only = n == Net::ED2K || n == Net::Kad || n == Net::Gnutella;
        if (v4only && a.v4().isNull())
            return no(QStringLiteral("Off: the network adapter \"%1\" has no IPv4 address (this network is IPv4 only)").arg(p.bind));
        if (p.proxy.active() && !r.proxyViaAdapter)
            return no(QStringLiteral("Off: the proxy is not reached through the adapter \"%1\" (%2)").arg(p.bind, r.proxyProblem));
    }
    if (p.proxy.active()) {
        switch (n) {
        case Net::Kad:
            return no(QStringLiteral("Off while a proxy is set: Kad works over UDP only, which cannot go through the proxy"));
        case Net::Overlay:
            return no(QStringLiteral("Off while a proxy is set: the OctoMX network cannot run through a proxy"));
        case Net::Rats:
            return no(QStringLiteral("Off while a proxy is set: the Rats network cannot run through a proxy"));
        default:
            break;
        }
    } else if (p.bindWanted() && !r.internetViaAdapter && (n == Net::Overlay || n == Net::Rats)) {
        // librats binds its listeners and UDP to the adapter but not its outgoing links: those
        // follow the system's route, so the network runs only while that route is the adapter's
        return no(QStringLiteral("Off: the %1 runs with a chosen adapter only while the internet route goes through it")
                      .arg(n == Net::Overlay ? QStringLiteral("OctoMX network") : QStringLiteral("Rats network")));
    }
    return v;
}

namespace {
Routes quickRoutes(const Policy &p, const Adapter &a)
{
    Routes r;
    if (!p.bindWanted() || !a.present) return r;
    auto viaOk = [&a](const QHostAddress &target) {
        const QHostAddress l = localAddressTowards(target);
        return l.isNull() || a.addresses.contains(l);
    };
    r.internetViaAdapter = viaOk(QHostAddress(QStringLiteral("198.51.100.1")))
                           && (!p.ipv6 || viaOk(QHostAddress(QStringLiteral("2001:db8::1"))));
    if (p.proxy.active()) {
        QHostAddress ip;
        if (!isNameLiteral(p.proxy.host, &ip)) {
            r.proxyViaAdapter = false;
            r.proxyProblem = QStringLiteral("its address is not known yet");
        } else if (!ip.isLoopback()) {
            const QHostAddress l = localAddressTowards(ip);
            r.proxyViaAdapter = !l.isNull() && a.addresses.contains(l);
            if (!r.proxyViaAdapter) r.proxyProblem = QStringLiteral("the route to it uses another adapter");
        }
    }
    return r;
}
} // namespace

Verdict verdict(Net n, const Settings *s)
{
    if (Monitor *m = Monitor::instance()) return m->verdict(n);
    const Policy p = fromSettings(s);
    const Adapter a = resolveAdapter(p.bind);
    return verdictFor(n, p, a, quickRoutes(p, a));
}

// ------------------------------------------------------------------------------------ sockets
bool prepareTcp(QAbstractSocket *sock, const Settings *s, QString *why, QAbstractSocket::NetworkLayerProtocol family)
{
    const Policy p = fromSettings(s);
    auto fail = [why, sock](const QString &t) {
        if (why) *why = t;
        // a caller that connects anyway reaches a closed door: the local relay refuses every
        // connection for an adapter that does not exist, so nothing leaves this computer
        sock->setProxy(relay::proxyFor(QStringLiteral("\u0001refused"), false));
        return false;
    };
    if (p.proxy.active()) {
        if (p.proxy.host.isEmpty() || p.proxy.port == 0) return fail(QStringLiteral("the proxy address is empty"));
        sock->setProxy(qtProxy(p.proxy));
        return true;
    }
    sock->setProxy(QNetworkProxy::NoProxy);
    if (!p.bindWanted()) return true;
    const Adapter a = resolveAdapter(p.bind);
    if (!a.present) return fail(QStringLiteral("the network adapter \"%1\" is not there").arg(p.bind));
    const QHostAddress local = family == QAbstractSocket::IPv6Protocol ? a.v6() : a.v4();
    if (local.isNull())
        return fail(QStringLiteral("the network adapter \"%1\" has no %2 address").arg(p.bind, family == QAbstractSocket::IPv6Protocol ? QStringLiteral("IPv6") : QStringLiteral("IPv4")));
    if (!sock->bind(local, 0)) {
        const QString e = sock->errorString();
        sock->abort();   // unbind again: the closed door below needs a fresh socket state
        return fail(QStringLiteral("cannot use %1: %2").arg(local.toString(), e));
    }
    return true;
}

bool udpAllowed(const Settings *s, QString *why)
{
    const Policy p = fromSettings(s);
    if (p.proxy.active()) {
        if (why) *why = QStringLiteral("no UDP while a proxy is set");
        return false;
    }
    if (p.bindWanted() && resolveAdapter(p.bind).v4().isNull()) {
        if (why) *why = QStringLiteral("the network adapter \"%1\" is not there").arg(p.bind);
        return false;
    }
    return true;
}

bool bindUdp(QUdpSocket *sock, const Settings *s, quint16 port, QString *why, int bindMode)
{
    if (!udpAllowed(s, why)) return false;
    const Policy p = fromSettings(s);
    sock->setProxy(QNetworkProxy::NoProxy);
    const QHostAddress local = p.bindWanted() ? resolveAdapter(p.bind).v4() : QHostAddress(QHostAddress::AnyIPv4);
    if (!sock->bind(local, port, QAbstractSocket::BindMode(bindMode))) {
        if (why) *why = sock->errorString();
        return false;
    }
    return true;
}

QHostAddress listenAddress(const Settings *s, bool *ok, QString *why)
{
    const Policy p = fromSettings(s);
    if (ok) *ok = true;
    if (p.proxy.active()) {
        // nobody can reach us through the proxy, and a listener on the real address would only
        // ever be found by someone who knows that address: OctoMX does not listen then
        if (ok) *ok = false;
        if (why) *why = QStringLiteral("not listening while a proxy is set");
        return QHostAddress();
    }
    if (!p.bindWanted()) return QHostAddress(QHostAddress::AnyIPv4);
    const QHostAddress a = resolveAdapter(p.bind).v4();
    if (a.isNull()) {
        if (ok) *ok = false;
        if (why) *why = QStringLiteral("the network adapter \"%1\" is not there").arg(p.bind);
    }
    return a;
}

void applyTo(QNetworkAccessManager *nam, const Settings *s)
{
    if (!nam) return;
    const Policy p = fromSettings(s);
    if (p.proxy.active()) {
        nam->setProxy(qtProxy(p.proxy));
        return;
    }
    if (p.bindWanted() || !p.ipv6) {
        // QNetworkAccessManager cannot bind its connections: they go through a relay on this
        // computer that binds each one to the adapter (and keeps to IPv4 when IPv6 is off)
        nam->setProxy(relay::proxyFor(p.bind, p.ipv6));
        return;
    }
    nam->setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
}

QNetworkProxy adapterRelayProxy(const QString &bind, bool ipv6)
{
    return relay::proxyFor(bind, ipv6);
}

QString bindAddressFor(const Settings *s, bool *ok, QString *why)
{
    const Policy p = fromSettings(s);
    if (ok) *ok = true;
    if (!p.bindWanted()) return p.ipv6 ? QString() : QStringLiteral("0.0.0.0");
    const Adapter a = resolveAdapter(p.bind);
    QHostAddress h = a.v4();
    if (h.isNull() && p.ipv6) h = a.v6();
    if (h.isNull()) {
        if (ok) *ok = false;
        if (why) *why = QStringLiteral("the network adapter \"%1\" is not there").arg(p.bind);
        return QString();
    }
    return h.toString();
}

bool proxyResolvesNames(const Settings *s)
{
    const Policy p = fromSettings(s);
    return p.proxy.active() && p.proxy.remoteDns;
}

// ------------------------------------------------------------------------------------ monitor
Monitor::Monitor(Settings *settings, QObject *parent) : QObject(parent), m_settings(settings)
{
    g_monitor = this;
    m_timer = new QTimer(this);
    m_timer->setInterval(2000);
    connect(m_timer, &QTimer::timeout, this, &Monitor::recheck);
    if (m_settings)
        connect(m_settings, &Settings::changed, this, [this](const QString &key, const QVariant &) {
            if (key.startsWith(QLatin1String("connection/"))) QTimer::singleShot(0, this, &Monitor::recheck);
        });
    recheck();
}

Monitor::~Monitor()
{
    if (m_lookupId >= 0) QHostInfo::abortHostLookup(m_lookupId);
    if (g_monitor == this) g_monitor = nullptr;
}

Verdict Monitor::verdict(Net n) const
{
    // a setting changed a moment ago (its recheck is still queued): never decide on the old one
    if (fromSettings(m_settings).fingerprint() != m_policy.fingerprint())
        const_cast<Monitor *>(this)->recheck();
    return verdictFor(n, m_policy, m_adapter, m_routes);
}

Monitor *Monitor::instance()
{
    return g_monitor.data();
}

void Monitor::setIntervalMs(int ms)
{
    m_timer->setInterval(ms);
}

void Monitor::lookupProxy()
{
    const QString host = m_policy.proxy.host;
    if (m_lookupId >= 0 || host.isEmpty()) return;
    m_lookupId = QHostInfo::lookupHost(host, this, [this, host](const QHostInfo &hi) {
        m_lookupId = -1;
        if (host != m_policy.proxy.host) return;
        m_proxyIpFor = host;
        m_proxyIp = QHostAddress();
        for (const QHostAddress &a : hi.addresses())
            if (usable(a)) { m_proxyIp = a; break; }
        recheck();
    });
}

void Monitor::recheck()
{
    m_policy = fromSettings(m_settings);
    clearAdapterCache();
    m_adapter = resolveAdapter(m_policy.bind);
    Routes r;
    if (m_policy.bindWanted() && m_adapter.present) {
        auto viaOk = [this](const QHostAddress &target) {
            const QHostAddress l = localAddressTowards(target);
            return l.isNull() || m_adapter.addresses.contains(l);
        };
        r.internetViaAdapter = viaOk(QHostAddress(QStringLiteral("198.51.100.1")))
                               && (!m_policy.ipv6 || viaOk(QHostAddress(QStringLiteral("2001:db8::1"))));
        if (m_policy.proxy.active() && !m_policy.proxy.host.isEmpty()) {
            QHostAddress ip;
            if (!isNameLiteral(m_policy.proxy.host, &ip)) {
                if (m_proxyIpFor == m_policy.proxy.host) ip = m_proxyIp;
                else lookupProxy();
            }
            if (ip.isNull()) {
                r.proxyViaAdapter = false;
                r.proxyProblem = m_proxyIpFor == m_policy.proxy.host ? QStringLiteral("its name cannot be looked up")
                                                                     : QStringLiteral("looking up its address");
            } else if (!ip.isLoopback()) {
                const QHostAddress l = localAddressTowards(ip);
                r.proxyViaAdapter = !l.isNull() && m_adapter.addresses.contains(l);
                if (!r.proxyViaAdapter)
                    r.proxyProblem = l.isNull() ? QStringLiteral("there is no route to it") : QStringLiteral("the route to it uses another adapter");
            }
        }
    }
    m_routes = r;
    if (m_policy.bindWanted()) {
        if (!m_timer->isActive()) m_timer->start();
    } else {
        m_timer->stop();
    }
    const QString state = m_policy.fingerprint() + QLatin1Char('#') + m_adapter.fingerprint() + QLatin1Char('#')
                          + (r.proxyViaAdapter ? QLatin1Char('1') : QLatin1Char('0')) + r.proxyProblem
                          + (r.internetViaAdapter ? QLatin1Char('1') : QLatin1Char('0'));
    if (state == m_state) return;
    const bool first = m_state.isEmpty();
    m_state = state;
    if (!first)
        qCInfo(lcPolicy).noquote() << "connection policy:" << summary();
    emit changed();
}

QString Monitor::summary() const
{
    QStringList parts;
    const Verdict all = verdict(Net::Unknown);
    if (!all.allowed) return all.reason;
    if (m_policy.proxy.active())
        parts << QStringLiteral("Everything goes through the %1; Kad, the OctoMX network, Rats and the UDP parts of eD2k and Gnutella are off")
                     .arg(m_policy.proxy.describe());
    if (m_policy.bindWanted())
        parts << QStringLiteral("Every connection uses the adapter \"%1\" (%2)").arg(m_policy.bind, m_adapter.v4().isNull() ? m_adapter.v6().toString() : m_adapter.v4().toString());
    return parts.join(QStringLiteral(". "));
}

QString proxyCoverageText()
{
    return QStringLiteral(
        "Every network sends its TCP connections through the proxy: BitTorrent, eD2k servers and peers, Gnutella, "
        "HTTP downloads, the web indexes and the update check. Off while a proxy is set: Kad, the OctoMX network and "
        "Rats (they cannot run through a proxy) and the UDP parts of eD2k and Gnutella. BitTorrent's DHT, uTP and UDP "
        "trackers go through a SOCKS5 proxy that relays UDP, else they are off. Nobody can connect to you through a "
        "proxy: OctoMX does not listen and maps no ports while one is set. Nothing ever connects directly.");
}

QString adapterCoverageText()
{
    return QStringLiteral(
        "Every network, web request, the update check and port mapping use only this adapter. When it is missing "
        "or loses its address (VPN down), every network stops and starts again when it is back. The OctoMX network "
        "and Rats run with it only while the internet route goes through it. Name lookups use the system's DNS.");
}

} // namespace netpolicy
} // namespace mx
