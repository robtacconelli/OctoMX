// SPDX-License-Identifier: GPL-3.0-or-later
// [diag-1] the shared port mapper (see port_mapper.h)
#include "core/diag/port_mapper.h"
#include "core/diag/diag_log.h"
#include "core/diag/net_env.h"
#include "core/diag/win_upnp.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QRandomGenerator>
#include <QtCore/QSet>
#include <QtCore/QXmlStreamReader>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkInterface>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QUdpSocket>

#include <thread>

namespace mx::diag {

namespace {
constexpr int kLeaseSec = 3600;
constexpr int kPermanentRenewSec = 1800;   // permanent mappings: add again (router reboots)
constexpr quint16 kPmpPort = 5351;
const QHostAddress kSsdpGroup(QStringLiteral("239.255.255.250"));

QElapsedTimer &clock()
{
    static QElapsedTimer t = [] { QElapsedTimer e; e.start(); return e; }();
    return t;
}

QString xmlEscape(const QString &s) { return s.toHtmlEscaped(); }

QString pmpResultText(int code)
{
    switch (code) {
    case 1: return QStringLiteral("NAT-PMP version not supported");
    case 2: return QStringLiteral("NAT-PMP refused (not authorized)");
    case 3: return QStringLiteral("NAT-PMP network failure (the router has no internet address)");
    case 4: return QStringLiteral("NAT-PMP out of resources");
    case 5: return QStringLiteral("NAT-PMP operation not supported");
    default: return QStringLiteral("NAT-PMP error %1").arg(code);
    }
}

// all leaf element texts by local name (SOAP responses / faults)
QHash<QString, QString> leafTexts(const QByteArray &xml)
{
    QHash<QString, QString> out;
    QXmlStreamReader r(xml);
    QString cur, text;
    while (!r.atEnd()) {
        r.readNext();
        if (r.isStartElement()) {
            cur = r.name().toString();
            text.clear();
        } else if (r.isCharacters()) {
            text += r.text();
        } else if (r.isEndElement()) {
            if (r.name().toString() == cur && !out.contains(cur)) out.insert(cur, text.trimmed());
            cur.clear();
        }
    }
    return out;
}
} // namespace

PortMapper::PortMapper(QObject *parent)
    : QObject(parent)
{
    m_nam = new QNetworkAccessManager(this);
    m_nam->setProxy(QNetworkProxy::NoProxy);   // the router is on the LAN: never through a proxy
    m_nam->setTransferTimeout(6000);
    m_pump.setInterval(1000);
    connect(&m_pump, &QTimer::timeout, this, &PortMapper::pump);
    m_ssdpTimer.setSingleShot(true);
    connect(&m_ssdpTimer, &QTimer::timeout, this, &PortMapper::ssdpDone);
    m_netTimer.setInterval(30000);   // adapters appear / go (Wi-Fi, VPN): search again
    connect(&m_netTimer, &QTimer::timeout, this, &PortMapper::checkNetworkChange);
    m_pmpTimer.setSingleShot(true);
    connect(&m_pmpTimer, &QTimer::timeout, this, [this] {
        if (!m_pmpDiscovering) return;
        if (++m_pmpTries < 4) {
            natPmpStart();
            return;
        }
        m_pmpDiscovering = false;
        m_router.natPmpError = QStringLiteral("no answer from %1").arg(m_router.gateway.isEmpty() ? QStringLiteral("the gateway") : m_router.gateway);
        discoveryFinished();
    });
}

PortMapper::~PortMapper() = default;

qint64 PortMapper::now() const { return clock().elapsed(); }

QString PortMapper::internalClient() const
{
    if (!m_testClient.isEmpty()) return m_testClient;
    return m_router.localAddress;
}

void PortMapper::setEnabled(bool upnp, bool natpmp)
{
    const bool changed = upnp != m_upnp || natpmp != m_natpmp;
    m_upnp = upnp;
    m_natpmp = natpmp;
    if (!changed && m_pump.isActive()) return;
    if (!m_upnp && !m_natpmp) {
        // switched off: delete what we made, then report every port as Off
        for (const QString &k : m_order) {
            Entry &e = m_entries[k];
            if (e.m.result == PortMapping::Mapped && m_router.upnpFound)
                deleteUpnp(e.m.proto, e.m.externalPort, [](bool, const QString &) {});
            setResult(e, PortMapping::Off, QStringLiteral("port mapping is switched off (Settings, Internet Connection)"));
        }
        m_pump.stop();
        // a discovery still in flight must not report anything any more
        m_ssdpTimer.stop();
        m_pmpTimer.stop();
        m_pmpDiscovering = false;
        m_router.searching = false;
        m_descPending = m_wanPending = 0;
        m_requestBusy = false;
        ++m_epoch;
        m_netTimer.stop();
        emit routerChanged();
        return;
    }
    if (!m_pump.isActive()) m_pump.start();
    if (!m_netTimer.isActive()) m_netTimer.start();
    if (changed || !m_router.searched) refresh();
}

void PortMapper::setWanted(const QList<Want> &wants)
{
    QSet<QString> keep;
    for (const Want &w : wants) {
        if (w.port <= 0 || w.port > 65535) continue;
        const QString k = keyOf(w.proto, w.port);
        keep.insert(k);
        auto it = m_entries.find(k);
        if (it == m_entries.end()) {
            Entry e;
            e.want = w;
            e.m.net = w.net;
            e.m.proto = w.proto;
            e.m.internalPort = w.port;
            e.m.result = (m_upnp || m_natpmp) ? PortMapping::Pending : PortMapping::Off;
            if (!m_upnp && !m_natpmp) e.m.error = QStringLiteral("port mapping is switched off (Settings, Internet Connection)");
            m_entries.insert(k, e);
            m_order << k;
            emit mappingChanged(e.m);
        } else {
            it->want.label = w.label;
            if (it->remove) {
                it->remove = false;
                it->nextTryMs = 0;
            }
        }
    }
    for (const QString &k : m_order)
        if (!keep.contains(k)) m_entries[k].remove = true;
    if (m_upnp || m_natpmp) {
        if (!m_pump.isActive()) m_pump.start();
        if (!m_router.searched && !m_router.searching) startDiscovery();
        QTimer::singleShot(0, this, &PortMapper::pump);
    } else {
        // forget removed entries at once (nothing on the router)
        for (int i = int(m_order.size()) - 1; i >= 0; --i)
            if (m_entries[m_order.at(i)].remove) {
                m_entries.remove(m_order.at(i));
                m_order.removeAt(i);
            }
    }
}

QList<PortMapping> PortMapper::mappings() const
{
    QList<PortMapping> out;
    for (const QString &k : m_order) {
        const Entry &e = m_entries.value(k);
        if (!e.remove) out << e.m;
    }
    return out;
}

bool PortMapper::busy() const { return m_router.searching || m_requestBusy; }

void PortMapper::refresh()
{
    if (!m_upnp && !m_natpmp) return;
    for (const QString &k : m_order) {
        Entry &e = m_entries[k];
        e.attempts = 0;
        e.nextTryMs = 1;   // due now: mapped ones are added again (renewed) too
        e.conflicts = 0;
    }
    startDiscovery();
}

// ------------------------------------------------------------------------------------ discovery
namespace {
const char *const kSearchTargets[] = {"urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                                      "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
                                      "urn:schemas-upnp-org:service:WANIPConnection:1",
                                      "urn:schemas-upnp-org:service:WANIPConnection:2",
                                      "urn:schemas-upnp-org:service:WANPPPConnection:1",
                                      "upnp:rootdevice"};

QByteArray searchMessage(const char *st, const QByteArray &host)
{
    return QByteArray("M-SEARCH * HTTP/1.1\r\nHOST: ") + host + "\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: " + st
           + "\r\nUSER-AGENT: OctoMX/" MX_VERSION_STRING " UPnP/1.1\r\n\r\n";
}

bool namesGateway(const QByteArray &s)
{
    return s.contains("InternetGatewayDevice") || s.contains("WANIPConnection") || s.contains("WANPPPConnection");
}

QHash<QByteArray, QByteArray> ssdpHeaders(const QByteArray &d)
{
    QHash<QByteArray, QByteArray> h;
    const QList<QByteArray> lines = d.split('\n');
    for (int i = 1; i < lines.size(); ++i) {
        const int c = int(lines.at(i).indexOf(':'));
        if (c <= 0) continue;
        h.insert(lines.at(i).left(c).trimmed().toLower(), lines.at(i).mid(c + 1).trimmed());
    }
    if (!lines.isEmpty()) h.insert("#first", lines.first().trimmed());
    return h;
}

// up, non-loopback adapters with an IPv4 address: (display name, address, interface index)
struct AdapterAddr { QString name; QHostAddress addr; int index = 0; };
QList<AdapterAddr> adapterAddresses()
{
    QList<AdapterAddr> out;
    for (const QNetworkInterface &ni : QNetworkInterface::allInterfaces()) {
        const auto fl = ni.flags();
        if (fl != QNetworkInterface::InterfaceFlags() && !(fl & QNetworkInterface::IsUp)) continue;   // wine: no flags
        if ((fl & QNetworkInterface::IsLoopBack) || ni.type() == QNetworkInterface::Loopback) continue;
        for (const QNetworkAddressEntry &e : ni.addressEntries()) {
            const QHostAddress a = e.ip();
            if (a.protocol() != QAbstractSocket::IPv4Protocol || a.isLoopback()) continue;
            AdapterAddr x;
            x.name = ni.humanReadableName().isEmpty() ? ni.name() : ni.humanReadableName();
            x.addr = a;
            x.index = ni.index();
            out << x;
        }
    }
    return out;
}
// adapterAddresses(), or (wine: QNetworkInterface lists nothing usable) the internet route's address
QList<AdapterAddr> searchAdapters()
{
    QList<AdapterAddr> want = adapterAddresses();
    if (want.isEmpty()) {
        const QHostAddress src = localAddressTowards(QHostAddress(QStringLiteral("8.8.8.8")), 53);
        if (!src.isNull()) want << AdapterAddr{QStringLiteral("default route"), src, 0};
    }
    return want;
}

QString adapterKey(const QList<AdapterAddr> &l)
{
    QStringList parts;
    for (const AdapterAddr &a : l) parts << a.name + QLatin1Char('=') + a.addr.toString();
    parts.sort();
    return parts.join(QLatin1Char(','));
}
} // namespace

void PortMapper::ensureSearchSockets()
{
    QList<AdapterAddr> want;
    if (!m_testAdapters.isEmpty()) {
        for (const auto &t : m_testAdapters) want << AdapterAddr{t.first, t.second, 0};
    } else {
        want = searchAdapters();
    }
    const QString key = adapterKey(want);
    if (key == m_searchKey && !m_search.isEmpty()) return;
    for (SearchSocket &s : m_search) s.sock->deleteLater();
    m_search.clear();
    m_searchKey = key;
    for (const AdapterAddr &a : want) {
        SearchSocket s;
        s.name = a.name;
        s.addr = a.addr;
        s.sock = new QUdpSocket(this);
        if (!s.sock->bind(a.addr, 0)) {
            qCInfo(lcDiag).noquote() << QStringLiteral("UPnP search: cannot use %1 %2: %3").arg(a.name, a.addr.toString(), s.sock->errorString());
            s.sock->deleteLater();
            continue;
        }
        s.sock->setSocketOption(QAbstractSocket::MulticastTtlOption, 4);
        if (a.index > 0) {   // the multicast leaves through THIS adapter, not the one Windows prefers
            const QNetworkInterface ni = QNetworkInterface::interfaceFromIndex(a.index);
            if (ni.isValid()) s.sock->setMulticastInterface(ni);
        }
        QUdpSocket *sock = s.sock;
        connect(sock, &QUdpSocket::readyRead, this, [this, sock] { onSsdpReady(sock); });
        m_search << s;
    }
}

void PortMapper::ensureNotifySocket()
{
    if (m_notify || !m_testDescription.isEmpty()) return;
    if (m_testAdapters.size() && !m_testNotifyPort) return;   // tests without the passive path
    m_notify = new QUdpSocket(this);
    connect(m_notify, &QUdpSocket::readyRead, this, &PortMapper::onNotifyReady);
    // on Windows the SSDP Discovery service holds port 1900 as well: share it, and a failure is harmless
    const bool ok = m_testNotifyPort ? m_notify->bind(QHostAddress::LocalHost, m_testNotifyPort, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)
                                     : m_notify->bind(QHostAddress::AnyIPv4, 1900, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
    if (!ok) {
        m_router.notifyError = m_notify->errorString();
        qCInfo(lcDiag).noquote() << QStringLiteral("UPnP: not listening for router announcements (%1), searching only").arg(m_router.notifyError);
        m_notify->deleteLater();
        m_notify = nullptr;
        return;
    }
    int joined = 0;
    if (!m_testNotifyPort) {
        for (const AdapterAddr &a : adapterAddresses()) {
            const QNetworkInterface ni = QNetworkInterface::interfaceFromIndex(a.index);
            if (ni.isValid() && m_notify->joinMulticastGroup(kSsdpGroup, ni)) ++joined;
        }
        if (joined == 0 && m_notify->joinMulticastGroup(kSsdpGroup)) ++joined;
    }
    qCInfo(lcDiag).noquote() << QStringLiteral("UPnP: listening for router announcements on %1 adapter(s)").arg(m_testNotifyPort ? 1 : joined);
}

void PortMapper::startDiscovery()
{
    if (m_router.searching) return;
    if (!m_router.controlUrl.isEmpty()) {   // mappings made there must stay removable while searching again
        m_lastControl = m_router.controlUrl;
        m_lastService = m_router.serviceType;
    }
    const QString notifyErr = m_router.notifyError;
    m_router = RouterInfo();
    m_router.searching = true;
    m_router.notifyError = notifyErr;
    m_router.when = QDateTime::currentDateTime();
    const QHostAddress gw = m_testPmpAddr.isNull() ? defaultGateway() : m_testPmpAddr;
    m_router.gateway = gw.isNull() ? QString() : gw.toString();
    m_locations.clear();
    m_candidates.clear();
    m_fetched.clear();
    m_locIface.clear();
    m_locSource.clear();
    m_descPending = 0;
    m_wanPending = 0;
    emit routerChanged();
    bool upnpRunning = false;
    const bool forceWindows = qEnvironmentVariable("MX_UPNP_FORCE_WINDOWS") == QLatin1String("1");
    if (m_upnp && forceWindows) {
        qCInfo(lcDiag) << "UPnP: own discovery skipped (MX_UPNP_FORCE_WINDOWS=1), asking Windows";
        m_router.searchSkipped = true;
        m_router.upnpError = QStringLiteral("own search skipped");
    } else if (m_upnp) {
        if (!m_testDescription.isEmpty()) {
            m_locations << m_testDescription;
            upnpRunning = true;
            QTimer::singleShot(0, this, &PortMapper::ssdpDone);
        } else {
            ensureNotifySocket();
            m_router.notifyListening = m_notify != nullptr;
            // shortcuts first: a router announcement just heard, the description remembered from before
            const QVariantMap cached = m_cache.value(m_router.gateway).toMap();
            QList<QPair<QUrl, QString>> first;
            for (const QUrl &u : std::as_const(m_seeds)) first << qMakePair(u, QStringLiteral("announcement"));
            m_seeds.clear();
            if (!cached.value(QStringLiteral("location")).toString().isEmpty()) {
                m_router.cacheTried = true;
                first << qMakePair(QUrl(cached.value(QStringLiteral("location")).toString()), QStringLiteral("cache"));
                qCInfo(lcDiag).noquote() << QStringLiteral("UPnP: trying the router description remembered for %1: %2")
                                               .arg(m_router.gateway, cached.value(QStringLiteral("location")).toString());
            }
            for (const auto &f : first) {
                if (!f.first.isValid() || m_fetched.contains(f.first.toString())) continue;
                m_locations << f.first;
                m_locSource.insert(f.first.toString(), f.second);
                m_fetched.insert(f.first.toString());
                ++m_descPending;
                fetchDescription(f.first);
            }
            ensureSearchSockets();
            for (SearchSocket &so : m_search) so.answers = 0;
            if (m_search.isEmpty()) m_router.upnpError = QStringLiteral("no network adapter with an IPv4 address to search on");
            upnpRunning = !m_search.isEmpty() || m_descPending > 0;
            if (!m_search.isEmpty()) {
                sendSearches(0);
                QTimer::singleShot(int(300 * qMax(0.05, m_retryScale)), this, [this, e = m_epoch] { if (e == m_epoch && m_router.searching) sendSearches(1); });
                QTimer::singleShot(int(1000 * qMax(0.05, m_retryScale)), this, [this, e = m_epoch] { if (e == m_epoch && m_router.searching) sendSearches(2); });
                m_ssdpTimer.start(int(3000 * qMax(0.05, m_retryScale)));
            }
        }
    }
    if (m_natpmp && !m_router.gateway.isEmpty()) {
        m_pmpTries = 0;
        m_pmpDiscovering = true;
        natPmpStart();
    } else if (m_natpmp) {
        m_router.natPmpError = QStringLiteral("no default gateway found");
    }
    if (!upnpRunning && !m_pmpDiscovering) discoveryFinished();
}

// M-SEARCH for every target on every adapter (3 rounds: 0, 300 ms, 1 s); round 0 also asks the
// gateway directly (unicast; some routers answer that, many only answer multicast)
void PortMapper::sendSearches(int round)
{
    const bool test = !m_testSsdpTarget.isNull();
    const QHostAddress target = test ? m_testSsdpTarget : kSsdpGroup;
    const quint16 port = test ? m_testSsdpPort : 1900;
    const QHostAddress gw(m_router.gateway);
    for (SearchSocket &so : m_search) {
        int sent = 0;
        for (const char *st : kSearchTargets)
            if (so.sock->writeDatagram(searchMessage(st, "239.255.255.250:1900"), target, port) > 0) ++sent;
        if (round == 0 && !test && !gw.isNull()) {
            const QByteArray host = gw.toString().toLatin1() + ":1900";
            so.sock->writeDatagram(searchMessage(kSearchTargets[0], host), gw, 1900);
            so.sock->writeDatagram(searchMessage(kSearchTargets[2], host), gw, 1900);
            m_router.unicastTried = true;
        }
        if (round == 0)
            qCInfo(lcDiag).noquote() << QStringLiteral("UPnP search on %1 %2: %3 of %4 searches sent%5")
                                           .arg(so.name, so.addr.toString()).arg(sent).arg(int(std::size(kSearchTargets)))
                                           .arg(sent ? QString() : QStringLiteral(" (%1)").arg(so.sock->errorString()));
    }
}

void PortMapper::onSsdpReady(QUdpSocket *sock)
{
    SearchSocket *so = nullptr;
    for (SearchSocket &x : m_search)
        if (x.sock == sock) so = &x;
    while (sock->hasPendingDatagrams()) {
        QByteArray d;
        d.resize(int(qMax<qint64>(0, sock->pendingDatagramSize())));
        QHostAddress from;
        sock->readDatagram(d.data(), d.size(), &from);
        if (!m_router.searching) continue;
        ++m_router.ssdpReplies;
        const QHash<QByteArray, QByteArray> h = ssdpHeaders(d);
        const QByteArray st = h.value("st"), usn = h.value("usn");
        const QUrl loc(QString::fromLatin1(h.value("location")));
        const QString fromIp = QHostAddress(from.toIPv4Address()).toString();
        const bool igd = namesGateway(st) || namesGateway(usn);
        if (so) ++so->answers;
        if (!loc.isValid() || loc.host().isEmpty()) continue;
        // a root device that is not our gateway (TV, NAS, media server) is no router candidate
        if (!igd && loc.host() != m_router.gateway) continue;
        if (igd) ++m_router.igdAnswers;
        const QString key = loc.toString();
        if (!m_locIface.contains(key)) {
            const QString on = so ? QStringLiteral("%1 %2").arg(so->name, so->addr.toString()) : QStringLiteral("?");
            m_locIface.insert(key, on);
            qCInfo(lcDiag).noquote() << QStringLiteral("UPnP answer on %1 from %2: ST %3, LOCATION %4").arg(on, fromIp, QString::fromLatin1(st), key);
        }
        if (!m_locations.contains(loc)) {
            m_locations << loc;
            if (!m_locSource.contains(key)) m_locSource.insert(key, QStringLiteral("search"));
        }
    }
}

void PortMapper::onNotifyReady()
{
    while (m_notify && m_notify->hasPendingDatagrams()) {
        QByteArray d;
        d.resize(int(qMax<qint64>(0, m_notify->pendingDatagramSize())));
        QHostAddress from;
        m_notify->readDatagram(d.data(), d.size(), &from);
        const QHash<QByteArray, QByteArray> h = ssdpHeaders(d);
        if (!h.value("#first").startsWith("NOTIFY")) continue;   // our own and others' M-SEARCHes
        if (!h.value("nts").contains("ssdp:alive") || !(namesGateway(h.value("nt")) || namesGateway(h.value("usn")))) continue;
        const QUrl loc(QString::fromLatin1(h.value("location")));
        if (!loc.isValid() || loc.host().isEmpty()) continue;
        // only interesting while no router is known, or a better one (the gateway) announces itself
        const bool useful = !m_router.upnpFound || (!m_router.isGateway() && loc.host() == m_router.gateway);
        if (!useful || !m_upnp) continue;
        const QString key = loc.toString();
        if (m_fetched.contains(key) || m_seeds.contains(loc)) continue;
        qCInfo(lcDiag).noquote() << QStringLiteral("UPnP router announcement from %1: NT %2, LOCATION %3")
                                       .arg(QHostAddress(from.toIPv4Address()).toString(), QString::fromLatin1(h.value("nt")), key);
        if (m_router.searching) {
            m_locations << loc;
            m_locSource.insert(key, QStringLiteral("announcement"));
            m_locIface.insert(key, QStringLiteral("router announcement"));
            if (!m_ssdpTimer.isActive()) {   // the search phase is over: fetch it now
                m_fetched.insert(key);
                ++m_descPending;
                fetchDescription(loc);
            }
        } else {
            m_seeds << loc;
            startDiscovery();
        }
    }
}

void PortMapper::checkNetworkChange()
{
    if ((!m_upnp && !m_natpmp) || m_shuttingDown || !m_testAdapters.isEmpty()) return;
    const QString key = adapterKey(searchAdapters());
    if (m_searchKey.isEmpty() || key == m_searchKey) return;
    qCInfo(lcDiag).noquote() << QStringLiteral("UPnP: the network adapters changed (%1), searching again").arg(key);
    ensureSearchSockets();
    if (m_notify) {   // join the group on the new adapters too
        m_notify->deleteLater();
        m_notify = nullptr;
    }
    if (!m_router.searching) refresh();
}

void PortMapper::recordAdapters()
{
    m_router.adapters.clear();
    m_router.adapterAnswers.clear();
    m_router.adapterRouter.clear();
    for (const SearchSocket &so : std::as_const(m_search)) {
        const QString name = QStringLiteral("%1 %2").arg(so.name, so.addr.toString());
        bool router = false;
        for (auto it = m_locIface.cbegin(); it != m_locIface.cend(); ++it)
            if (it.value() == name) router = true;
        m_router.adapters << name;
        m_router.adapterAnswers << so.answers;
        m_router.adapterRouter << router;
    }
}

void PortMapper::ssdpDone()
{
    if (!m_router.searching) return;
    recordAdapters();
    if (m_locations.isEmpty() && m_descPending == 0 && m_candidates.isEmpty()) {
        if (m_router.upnpError.isEmpty())
            m_router.upnpError = m_router.ssdpReplies > 0 ? QStringLiteral("devices answered, but none is an internet gateway")
                                                          : QStringLiteral("no UPnP router answered");
        if (!m_pmpDiscovering) discoveryFinished();
        return;
    }
    // the gateway's own description first
    std::stable_sort(m_locations.begin(), m_locations.end(), [this](const QUrl &a, const QUrl &b) {
        return (a.host() == m_router.gateway) > (b.host() == m_router.gateway);
    });
    for (const QUrl &u : std::as_const(m_locations)) {
        if (m_fetched.contains(u.toString())) continue;
        m_fetched.insert(u.toString());
        ++m_descPending;
        fetchDescription(u);
    }
    if (m_descPending == 0 && m_wanPending == 0) chooseRouter();
}

void PortMapper::fetchDescription(const QUrl &url)
{
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("OctoMX UPnP/1.1"));
    QNetworkReply *r = m_nam->get(req);
    connect(r, &QNetworkReply::finished, this, [this, r, url] {
        r->deleteLater();
        --m_descPending;
        if (!m_router.searching) return;
        if (r->error() != QNetworkReply::NoError) {
            m_router.upnpError = QStringLiteral("router description %1: %2").arg(url.toString(), r->errorString());
        } else {
            QXmlStreamReader x(r->readAll());
            QString urlBase, friendly, manu, model, curType, curCtl, el;
            int deviceDepth = 0, rootDeviceDepth = 0;
            QList<QPair<QString, QString>> services;
            bool inService = false;
            while (!x.atEnd()) {
                x.readNext();
                if (x.isStartElement()) {
                    el = x.name().toString();
                    if (el == QLatin1String("device")) {
                        ++deviceDepth;
                        if (!rootDeviceDepth) rootDeviceDepth = deviceDepth;
                    } else if (el == QLatin1String("service")) {
                        inService = true;
                        curType.clear();
                        curCtl.clear();
                    }
                } else if (x.isCharacters() && !x.isWhitespace()) {
                    const QString t = x.text().toString().trimmed();
                    if (el == QLatin1String("URLBase")) urlBase = t;
                    else if (inService && el == QLatin1String("serviceType")) curType = t;
                    else if (inService && el == QLatin1String("controlURL")) curCtl = t;
                    else if (deviceDepth == 1 && el == QLatin1String("friendlyName") && friendly.isEmpty()) friendly = t;
                    else if (deviceDepth == 1 && el == QLatin1String("manufacturer") && manu.isEmpty()) manu = t;
                    else if (deviceDepth == 1 && el == QLatin1String("modelName") && model.isEmpty()) model = t;
                } else if (x.isEndElement()) {
                    const QString n = x.name().toString();
                    if (n == QLatin1String("device")) --deviceDepth;
                    else if (n == QLatin1String("service")) {
                        inService = false;
                        if (curType.contains(QLatin1String("WANIPConnection")) || curType.contains(QLatin1String("WANPPPConnection")))
                            services << qMakePair(curType, curCtl);
                    }
                    el.clear();
                }
            }
            if (services.isEmpty()) {
                m_router.upnpError = QStringLiteral("%1 offers no port mapping service").arg(friendly.isEmpty() ? url.host() : friendly);
            } else {
                // WANIPConnection before WANPPPConnection
                std::stable_sort(services.begin(), services.end(), [](const auto &a, const auto &b) {
                    return a.first.contains(QLatin1String("WANIP")) > b.first.contains(QLatin1String("WANIP"));
                });
                const QUrl base = urlBase.isEmpty() ? url : QUrl(urlBase);
                RouterInfo c;
                c.upnpFound = true;
                c.friendlyName = friendly;
                c.manufacturer = manu;
                c.model = model;
                c.location = url.toString();
                c.serviceType = services.first().first;
                c.controlUrl = base.resolved(QUrl(services.first().second)).toString();
                c.address = url.host();
                const QHostAddress igd(url.host());
                const QHostAddress local = igd.isNull() ? QHostAddress() : localAddressTowards(igd, quint16(url.port(80)));
                c.localAddress = local.isNull() ? QString() : local.toString();
                const int idx = int(m_candidates.size());
                m_candidates << c;
                ++m_wanPending;
                // its internet address decides which device is the real internet router
                soapTo(c.controlUrl, c.serviceType, QStringLiteral("GetExternalIPAddress"), {},
                       [this, idx](bool ok, const QHash<QString, QString> &o, const QString &err) {
                           --m_wanPending;
                           if (idx < m_candidates.size()) {
                               if (ok) m_candidates[idx].upnpWan = o.value(QStringLiteral("NewExternalIPAddress"));
                               else m_candidates[idx].upnpError = QStringLiteral("GetExternalIPAddress: %1").arg(err);
                           }
                           if (m_descPending == 0 && m_wanPending == 0) chooseRouter();
                       });
                return;
            }
        }
        if (m_descPending == 0 && m_wanPending == 0) chooseRouter();
    });
}

// Several devices may answer as an internet gateway (a second router used as an access point
// still offers UPnP). The one that is our default gateway wins, then one with a real internet
// address; mappings on the others would not reach the internet.
void PortMapper::chooseRouter()
{
    if (!m_router.searching) return;
    if (!m_candidates.isEmpty()) {
        auto score = [this](const RouterInfo &c) {
            int s = 0;
            if (!m_router.gateway.isEmpty() && c.address == m_router.gateway) s += 4;
            const QHostAddress wan(c.upnpWan);
            if (!wan.isNull() && wan != QHostAddress::AnyIPv4) s += 2;
            if (c.upnpError.isEmpty()) s += 1;
            return s;
        };
        std::stable_sort(m_candidates.begin(), m_candidates.end(), [&](const RouterInfo &a, const RouterInfo &b) { return score(a) > score(b); });
        const RouterInfo best = m_candidates.first();
        // still searching: only the gateway with a real internet address ends the search early
        // (the remembered description is the usual case); otherwise wait for every answer
        if (m_ssdpTimer.isActive()) {
            if (score(best) < 6) return;
            m_ssdpTimer.stop();
        }
        const QString gw = m_router.gateway;
        const QString pmpWan = m_router.natPmpWan, pmpErr = m_router.natPmpError;
        const bool pmp = m_router.natPmpAnswered;
        const int replies = m_router.ssdpReplies;
        m_router.upnpFound = true;
        m_router.friendlyName = best.friendlyName;
        m_router.manufacturer = best.manufacturer;
        m_router.model = best.model;
        m_router.location = best.location;
        m_router.serviceType = best.serviceType;
        m_router.controlUrl = best.controlUrl;
        m_router.address = best.address;
        m_router.localAddress = best.localAddress;
        m_router.upnpWan = best.upnpWan;
        m_router.upnpError = best.upnpError;
        m_router.gateway = gw;
        m_router.natPmpWan = pmpWan;
        m_router.natPmpError = pmpErr;
        m_router.natPmpAnswered = pmp;
        m_router.ssdpReplies = replies;
        m_router.answeredOn = m_locIface.value(best.location);
        m_router.foundBy = m_locSource.value(best.location, QStringLiteral("search"));
        m_router.cacheWorked = m_router.foundBy == QLatin1String("cache");
        if (best.address == gw && !gw.isEmpty()) {   // remember it for the next start
            QVariantMap e;
            e.insert(QStringLiteral("location"), best.location);
            e.insert(QStringLiteral("controlUrl"), best.controlUrl);
            e.insert(QStringLiteral("serviceType"), best.serviceType);
            if (m_cache.value(gw).toMap() != e) {
                m_cache.insert(gw, e);
                emit cacheUpdated();
            }
        }
        m_router.others.clear();
        for (int i = 1; i < m_candidates.size(); ++i) {
            const RouterInfo &c = m_candidates.at(i);
            m_router.others << QStringLiteral("%1 at %2, internet address %3").arg(c.friendlyName.isEmpty() ? QStringLiteral("a router") : c.friendlyName, c.address,
                                                                               c.upnpWan.isEmpty() ? QStringLiteral("unknown") : c.upnpWan);
        }
        qCInfo(lcDiag).noquote() << QStringLiteral("UPnP router: %1 (%2 %3) at %4%5, internet address %6, this computer %7, found by %8%9%10")
                                       .arg(best.friendlyName, best.manufacturer, best.model, best.address,
                                            best.address == gw ? QStringLiteral(" (default gateway)") : QStringLiteral(" (NOT the default gateway %1)").arg(gw),
                                            best.upnpWan.isEmpty() ? QStringLiteral("unknown") : best.upnpWan, best.localAddress, m_router.foundBy,
                                            m_router.answeredOn.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(m_router.answeredOn),
                                            m_router.others.isEmpty() ? QString() : QStringLiteral("; also answered: ") + m_router.others.join(QStringLiteral("; ")));
        m_pmpDiscovering = false;
        m_pmpTimer.stop();
    }
    if (!m_pmpDiscovering) discoveryFinished();
}

void PortMapper::natPmpStart()
{
    if (!m_pmp) {
        m_pmp = new QUdpSocket(this);
        connect(m_pmp, &QUdpSocket::readyRead, this, &PortMapper::onPmpReady);
    }
    if (m_pmp->state() != QAbstractSocket::BoundState) m_pmp->bind(QHostAddress::AnyIPv4, 0);
    const QHostAddress gw(m_router.gateway);
    const quint16 port = m_testPmpPort ? m_testPmpPort : kPmpPort;
    const char req[2] = {0, 0};
    m_pmp->writeDatagram(req, 2, gw, port);
    m_pmpTimer.start(int((250 << m_pmpTries) * qMax(0.05, m_retryScale)));
}

void PortMapper::onPmpReady()
{
    while (m_pmp && m_pmp->hasPendingDatagrams()) {
        QByteArray d;
        d.resize(int(qMax<qint64>(0, m_pmp->pendingDatagramSize())));
        m_pmp->readDatagram(d.data(), d.size());
        if (d.size() < 8 || quint8(d[0]) != 0) continue;
        const int op = quint8(d[1]);
        const int result = (quint8(d[2]) << 8) | quint8(d[3]);
        if (op == 128 && d.size() >= 12) {
            if (!m_pmpDiscovering) continue;
            m_pmpDiscovering = false;
            m_pmpTimer.stop();
            if (result == 0) {
                m_router.natPmpAnswered = true;
                m_router.natPmpWan = QHostAddress(quint32((quint8(d[8]) << 24) | (quint8(d[9]) << 16) | (quint8(d[10]) << 8) | quint8(d[11]))).toString();
                if (m_router.localAddress.isEmpty()) {
                    const QHostAddress l = localAddressTowards(QHostAddress(m_router.gateway), kPmpPort);
                    if (!l.isNull()) m_router.localAddress = l.toString();
                }
                qCInfo(lcDiag).noquote() << QStringLiteral("NAT-PMP router %1 answered, internet address %2").arg(m_router.gateway, m_router.natPmpWan);
            } else {
                m_router.natPmpError = pmpResultText(result);
            }
            if (m_descPending == 0 && m_wanPending == 0 && !m_ssdpTimer.isActive()) {
                if (!m_candidates.isEmpty()) chooseRouter();
                else discoveryFinished();
            }
        } else if ((op == 129 || op == 130) && d.size() >= 16) {
            const int internal = (quint8(d[8]) << 8) | quint8(d[9]);
            const int external = (quint8(d[10]) << 8) | quint8(d[11]);
            const quint32 life = (quint32(quint8(d[12])) << 24) | (quint8(d[13]) << 16) | (quint8(d[14]) << 8) | quint8(d[15]);
            const QString key = m_pmpWaiting.take(((op - 128) << 16) | internal);
            if (key.isEmpty() || !m_entries.contains(key)) continue;
            Entry &e = m_entries[key];
            m_requestBusy = false;
            if (result != 0) {
                scheduleRetry(e, pmpResultText(result));
            } else if (life == 0) {
                if (e.remove) {
                    m_order.removeAll(key);
                    m_entries.remove(key);
                } else {
                    setResult(e, PortMapping::Removed, QString());
                }
            } else {
                e.m.method = QStringLiteral("NAT-PMP");
                e.m.externalPort = external;
                e.m.expires = QDateTime::currentDateTime().addSecs(life);
                e.attempts = 0;
                e.nextTryMs = now() + qint64(life) * 500 * qMax(0.001, m_retryScale);
                setResult(e, PortMapping::Mapped, QString());
            }
            QTimer::singleShot(0, this, &PortMapper::pump);
        }
    }
}

void PortMapper::discoveryFinished()
{
    if (!m_router.searching || (!m_upnp && !m_natpmp)) return;
    if (m_descPending > 0 || m_wanPending > 0 || m_ssdpTimer.isActive() || m_pmpDiscovering) return;
    if (m_router.adapters.isEmpty()) recordAdapters();
    if (!m_router.upnpFound && !m_router.natPmpAnswered && m_upnp && !m_router.winTried) {
        // our own search found nothing: on Windows ask the system's UPnP service (it uses the
        // SSDP Discovery service, which the firewall allows and which handles the adapters itself)
#ifdef Q_OS_WIN
        const bool win = true;
#else
        const bool win = m_testWin;
#endif
        if (win) {
            m_router.winTried = true;
            winFallback();
            return;
        }
    }
    m_router.searching = false;
    m_router.searched = true;
    m_router.when = QDateTime::currentDateTime();
    if (!m_router.upnpFound && !m_router.natPmpAnswered && !m_router.winNat) {
        QStringList parts;
        parts << (m_upnp ? QStringLiteral("UPnP: %1").arg(m_router.upnpError.isEmpty() ? QStringLiteral("no answer") : m_router.upnpError)
                         : QStringLiteral("UPnP: off"));
        parts << (m_natpmp ? QStringLiteral("NAT-PMP: %1").arg(m_router.natPmpError.isEmpty() ? QStringLiteral("no answer") : m_router.natPmpError)
                           : QStringLiteral("NAT-PMP: off"));
        if (m_router.winTried) parts << QStringLiteral("Windows: %1").arg(m_router.winResult);
        const QString why = QStringLiteral("No router answered; %1.").arg(parts.join(QStringLiteral("; ")));
        // back off: 30 s, 1 min, 2 min, 5 min, then every 10 min (announcements still heal it at once)
        static const int steps[] = {30, 60, 120, 300, 600};
        const int sec = steps[qMin(m_failRounds, 4)];
        ++m_failRounds;
        int n = 0;
        for (const QString &k : m_order) {
            Entry &e = m_entries[k];
            if (e.remove) continue;
            ++n;
            e.m.method.clear();
            e.m.result = PortMapping::Failed;   // set quietly: one log line for all ports below
            e.m.error = why;
            e.m.when = QDateTime::currentDateTime();
            e.nextTryMs = now() + qint64(sec * 1000 * m_retryScale);
            emit mappingChanged(e.m);
        }
        QStringList where;
        for (int i = 0; i < m_router.adapters.size(); ++i)
            where << QStringLiteral("%1 (%2)").arg(m_router.adapters.at(i), countNoun(m_router.adapterAnswers.value(i), QStringLiteral("answer")));
        qCWarning(lcDiag).noquote() << QStringLiteral("port mapping failed for %1: %2 Searched on: %3. Next search in %4 s.")
                                          .arg(countNoun(n, QStringLiteral("port")), why,
                                               where.isEmpty() ? QStringLiteral("no adapter") : where.join(QStringLiteral(", ")))
                                          .arg(sec);
    } else {
        m_failRounds = 0;
    }
    emit routerChanged();
    pump();
}

void PortMapper::winFallback()
{
    qCInfo(lcDiag) << "UPnP: own search found no router, asking the Windows UPnP service";
    auto done = [this](const WinUpnpResult &r) {
        m_router.winNat = r.available;
        m_router.winResult = r.available ? QStringLiteral("Windows found a UPnP router (%1 mappings listed)").arg(r.count) : r.error;
        if (r.available && !r.externalAddress.isEmpty()) m_router.upnpWan = r.externalAddress;
        qCInfo(lcDiag).noquote() << QStringLiteral("UPnP (Windows): %1").arg(m_router.winResult);
        discoveryFinished();
    };
    if (m_testWin) {
        WinUpnpResult r;
        r.available = m_testWinAvailable;
        r.error = m_testWinAvailable ? QString() : QStringLiteral("Windows found no UPnP router either");
        QTimer::singleShot(0, this, [done, r] { done(r); });
        return;
    }
    QPointer<PortMapper> guard(this);
    const qint64 epoch = m_epoch;
    std::thread([guard, done, epoch] {
        const WinUpnpResult r = winUpnpProbe();
        QMetaObject::invokeMethod(QCoreApplication::instance(), [guard, done, r, epoch] {
            if (guard && guard->m_epoch == epoch && guard->m_router.searching) done(r);
        });
    }).detach();
}

void PortMapper::winAdd(const QString &key)
{
    Entry &e = m_entries[key];
    const int ext = e.tryExternal > 0 ? e.tryExternal : e.m.internalPort;
    const QString who = e.want.label.isEmpty() ? netName(e.m.net) : e.want.label;
    const QString proto = e.m.proto, client = internalClient().isEmpty() ? localAddressTowards(QHostAddress(m_router.gateway)).toString() : internalClient();
    const int internal = e.m.internalPort;
    QPointer<PortMapper> guard(this);
    const qint64 epoch = m_epoch;
    std::thread([=] {
        const WinUpnpResult r = winUpnpAdd(ext, proto, internal, client, QStringLiteral("OctoMX %1 %2").arg(who, proto));
        QMetaObject::invokeMethod(QCoreApplication::instance(), [guard, key, r, epoch, ext] {
            if (!guard || guard->m_epoch != epoch || !guard->m_entries.contains(key)) return;
            PortMapper *self = guard;
            self->m_requestBusy = false;
            Entry &e = self->m_entries[key];
            e.m.method = QStringLiteral("UPnP (Windows)");
            if (!r.available) {
                // Windows said it knew a router, now it does not (or the API is a stub, as in wine):
                // back to "no router", search again later
                self->m_router.winNat = false;
                self->m_router.winResult = QStringLiteral("Windows reported a router, then could not use it (%1)").arg(r.error);
                qCInfo(lcDiag).noquote() << "UPnP (Windows):" << self->m_router.winResult;
                const QString why = QStringLiteral("No router answered; UPnP: %1; Windows: %2.")
                                        .arg(self->m_router.upnpError.isEmpty() ? QStringLiteral("no answer") : self->m_router.upnpError,
                                             self->m_router.winResult);
                for (const QString &k : std::as_const(self->m_order)) {
                    Entry &x = self->m_entries[k];
                    if (x.remove) continue;
                    x.m.method.clear();
                    x.m.result = PortMapping::Failed;
                    x.m.error = why;
                    x.nextTryMs = self->now() + qint64(60000 * self->m_retryScale);
                    emit self->mappingChanged(x.m);
                }
                emit self->routerChanged();
                return;
            }
            if (r.ok) {
                e.m.externalPort = r.externalPort > 0 ? r.externalPort : ext;
                e.m.expires = QDateTime();   // the Windows API makes permanent mappings
                e.attempts = 0;
                e.nextTryMs = self->now() + qint64(kPermanentRenewSec * 1000 * self->m_retryScale);
                if (!r.externalAddress.isEmpty()) self->m_router.upnpWan = r.externalAddress;
                self->setResult(e, PortMapping::Mapped, QString());
            } else {
                self->scheduleRetry(e, r.error);
            }
            QTimer::singleShot(0, self, &PortMapper::pump);
        });
    }).detach();
}

void PortMapper::winRemove(const QString &proto, int external, std::function<void(bool, const QString &)> done)
{
    QPointer<PortMapper> guard(this);
    const qint64 epoch = m_epoch;   // shutdown() bumps it: a late answer then reaches nobody
    std::thread([guard, proto, external, done, epoch] {
        const WinUpnpResult r = winUpnpRemove(external, proto);
        QMetaObject::invokeMethod(QCoreApplication::instance(), [guard, done, r, epoch] {
            if (guard && guard->m_epoch == epoch) done(r.ok, r.error);
        });
    }).detach();
}

// ------------------------------------------------------------------------------------ requests
void PortMapper::pump()
{
    if (m_shuttingDown || m_router.searching || m_requestBusy) return;
    if (!m_upnp && !m_natpmp) return;
    if (!m_router.searched) {
        startDiscovery();
        return;
    }
    if (!m_router.upnpFound && !m_router.natPmpAnswered && !m_router.winNat) {
        // nothing found: look again with back-off (see discoveryFinished)
        bool due = false;
        for (const QString &k : m_order)
            if (!m_entries[k].remove && m_entries[k].nextTryMs <= now()) due = true;
        for (int i = int(m_order.size()) - 1; i >= 0; --i)
            if (m_entries[m_order.at(i)].remove) {
                m_entries.remove(m_order.at(i));
                m_order.removeAt(i);
            }
        if (due) startDiscovery();
        return;
    }
    const qint64 t = now();
    for (const QString &k : std::as_const(m_order)) {
        Entry &e = m_entries[k];
        if (e.remove) {
            if (e.m.result == PortMapping::Mapped) {
                m_requestBusy = true;
                const QString key = k;
                if (e.m.method == QLatin1String("NAT-PMP")) {
                    mapPmp(key, 0);
                } else if (e.m.method == QLatin1String("UPnP (Windows)")) {
                    winRemove(e.m.proto, e.m.externalPort, [this, key](bool, const QString &) {
                        m_requestBusy = false;
                        m_order.removeAll(key);
                        m_entries.remove(key);
                        QTimer::singleShot(0, this, &PortMapper::pump);
                    });
                } else {
                    deleteUpnp(e.m.proto, e.m.externalPort, [this, key](bool, const QString &) {
                        m_requestBusy = false;
                        m_order.removeAll(key);
                        m_entries.remove(key);
                        QTimer::singleShot(0, this, &PortMapper::pump);
                    });
                }
                return;
            }
            m_entries.remove(k);
            m_order.removeAll(k);
            QTimer::singleShot(0, this, &PortMapper::pump);
            return;
        }
        const bool needs = (e.m.result != PortMapping::Mapped && e.nextTryMs <= t)
                           || (e.m.result == PortMapping::Mapped && e.nextTryMs > 0 && e.nextTryMs <= t);
        if (!needs) continue;
        m_requestBusy = true;
        if (m_router.upnpFound && m_upnp) addUpnp(k);
        else if (m_router.winNat && m_upnp) winAdd(k);
        else mapPmp(k, kLeaseSec);
        return;
    }
}

void PortMapper::scheduleRetry(Entry &e, const QString &error)
{
    if (e.m.method.isEmpty()) e.m.method = m_router.upnpFound ? QStringLiteral("UPnP") : QStringLiteral("NAT-PMP");
    ++e.attempts;
    static const int steps[] = {5, 15, 60};
    const int sec = e.attempts <= 3 ? steps[e.attempts - 1] : 600;
    e.nextTryMs = now() + qint64(sec * 1000 * m_retryScale);
    // a mapping that worked before keeps "Mapped" until its lease ends
    if (e.m.result == PortMapping::Mapped && e.m.expires.isValid() && e.m.expires > QDateTime::currentDateTime()) {
        qCInfo(lcDiag).noquote() << QStringLiteral("port mapping %1 %2: renewal failed (%3), lease still valid").arg(e.m.proto).arg(e.m.internalPort).arg(error);
        return;
    }
    setResult(e, PortMapping::Failed, error);
}

void PortMapper::setResult(Entry &e, PortMapping::Result r, const QString &error)
{
    const bool changed = e.m.result != r || e.m.error != error;
    e.m.result = r;
    e.m.error = error;
    e.m.when = QDateTime::currentDateTime();
    if (r != PortMapping::Mapped) {
        if (r != PortMapping::Failed) e.m.externalPort = 0;
    }
    if (changed || r == PortMapping::Mapped) {
        const QString who = e.want.label.isEmpty() ? netName(e.m.net) : e.want.label;
        if (r == PortMapping::Mapped)
            qCInfo(lcDiag).noquote() << QStringLiteral("port mapping: %1 %2 %3 -> external %4 (%5)%6")
                                           .arg(e.m.method, e.m.proto).arg(e.m.internalPort).arg(e.m.externalPort).arg(who)
                                           .arg(e.m.expires.isValid() ? QStringLiteral(", lease until %1").arg(e.m.expires.toString(QStringLiteral("HH:mm:ss"))) : QStringLiteral(", permanent"));
        else if (r == PortMapping::Failed)
            qCWarning(lcDiag).noquote() << QStringLiteral("port mapping: %1 %2 %3 failed (%4): %5")
                                              .arg(e.m.method, e.m.proto).arg(e.m.internalPort).arg(who, error);
    }
    emit mappingChanged(e.m);
}

void PortMapper::addUpnp(const QString &key)
{
    Entry &e = m_entries[key];
    const QString client = internalClient();
    if (client.isEmpty()) {
        m_requestBusy = false;
        scheduleRetry(e, QStringLiteral("no local address towards the router"));
        return;
    }
    const int ext = e.tryExternal > 0 ? e.tryExternal : e.m.internalPort;
    const int lease = e.permanentOnly ? 0 : kLeaseSec;
    const QString who = e.want.label.isEmpty() ? netName(e.m.net) : e.want.label;
    soap(QStringLiteral("AddPortMapping"),
         {{QStringLiteral("NewRemoteHost"), QString()},
          {QStringLiteral("NewExternalPort"), QString::number(ext)},
          {QStringLiteral("NewProtocol"), e.m.proto},
          {QStringLiteral("NewInternalPort"), QString::number(e.m.internalPort)},
          {QStringLiteral("NewInternalClient"), client},
          {QStringLiteral("NewEnabled"), QStringLiteral("1")},
          {QStringLiteral("NewPortMappingDescription"), QStringLiteral("OctoMX %1 %2").arg(who, e.m.proto)},
          {QStringLiteral("NewLeaseDuration"), QString::number(lease)}},
         [this, key, ext, lease](bool ok, const QHash<QString, QString> &, const QString &err) {
             if (!m_entries.contains(key)) {
                 m_requestBusy = false;
                 return;
             }
             Entry &e = m_entries[key];
             if (ok) {
                 m_requestBusy = false;
                 e.m.method = QStringLiteral("UPnP");
                 e.m.externalPort = ext;
                 e.m.expires = lease ? QDateTime::currentDateTime().addSecs(lease) : QDateTime();
                 e.attempts = 0;
                 e.conflicts = 0;
                 e.nextTryMs = now() + qint64((lease ? lease / 2 : kPermanentRenewSec) * 1000 * m_retryScale);
                 setResult(e, PortMapping::Mapped, QString());
                 QTimer::singleShot(0, this, &PortMapper::pump);
                 return;
             }
             if (err.contains(QLatin1String("725")) && !e.permanentOnly) {   // OnlyPermanentLeasesSupported
                 e.permanentOnly = true;
                 addUpnp(key);
                 return;
             }
             checkSpecific(key, err);
         });
}

// After a failed add: who holds this external port? Ours (stale, from an earlier run whose delete
// failed) is re-claimed, someone else's makes us pick another external port.
void PortMapper::checkSpecific(const QString &key, const QString &addError)
{
    Entry &e = m_entries[key];
    const int ext = e.tryExternal > 0 ? e.tryExternal : e.m.internalPort;
    soap(QStringLiteral("GetSpecificPortMappingEntry"),
         {{QStringLiteral("NewRemoteHost"), QString()},
          {QStringLiteral("NewExternalPort"), QString::number(ext)},
          {QStringLiteral("NewProtocol"), e.m.proto}},
         [this, key, ext, addError](bool ok, const QHash<QString, QString> &o, const QString &) {
             if (!m_entries.contains(key)) {
                 m_requestBusy = false;
                 return;
             }
             Entry &e = m_entries[key];
             if (ok) {
                 const QString client = o.value(QStringLiteral("NewInternalClient"));
                 const int internal = o.value(QStringLiteral("NewInternalPort")).toInt();
                 if (client == internalClient() && internal == e.m.internalPort) {
                     if (e.conflicts++ == 0) {
                         // ours: delete and add again (fresh lease, our description)
                         qCInfo(lcDiag).noquote() << QStringLiteral("port mapping: %1 %2 is still mapped to this computer from an earlier start, re-claiming it")
                                                         .arg(e.m.proto).arg(ext);
                         deleteUpnp(e.m.proto, ext, [this, key](bool, const QString &) {
                             if (m_entries.contains(key)) addUpnp(key);
                             else m_requestBusy = false;
                         });
                         return;
                     }
                     // the router refuses to replace it but it points to us: good enough
                     m_requestBusy = false;
                     e.m.method = QStringLiteral("UPnP");
                     e.m.externalPort = ext;
                     const int left = o.value(QStringLiteral("NewLeaseDuration")).toInt();
                     e.m.expires = left > 0 ? QDateTime::currentDateTime().addSecs(left) : QDateTime();
                     e.attempts = 0;
                     e.nextTryMs = now() + qint64((left > 0 ? qMax(60, left / 2) : kPermanentRenewSec) * 1000 * m_retryScale);
                     setResult(e, PortMapping::Mapped, QString());
                     QTimer::singleShot(0, this, &PortMapper::pump);
                     return;
                 }
                 if (!client.isEmpty() && e.conflicts < 4) {
                     // another computer holds it: try another external port
                     ++e.conflicts;
                     e.tryExternal = 20000 + int(QRandomGenerator::global()->bounded(40000));
                     qCInfo(lcDiag).noquote() << QStringLiteral("port mapping: external %1 %2 belongs to %3, trying external port %4")
                                                     .arg(e.m.proto).arg(ext).arg(client).arg(e.tryExternal);
                     addUpnp(key);
                     return;
                 }
             }
             m_requestBusy = false;
             scheduleRetry(e, addError);
             QTimer::singleShot(0, this, &PortMapper::pump);
         });
}

void PortMapper::deleteUpnp(const QString &proto, int external, std::function<void(bool, const QString &)> done)
{
    soap(QStringLiteral("DeletePortMapping"),
         {{QStringLiteral("NewRemoteHost"), QString()},
          {QStringLiteral("NewExternalPort"), QString::number(external)},
          {QStringLiteral("NewProtocol"), proto}},
         [done](bool ok, const QHash<QString, QString> &, const QString &err) { done(ok, err); });
}

void PortMapper::mapPmp(const QString &key, int lifetime)
{
    Entry &e = m_entries[key];
    const QHostAddress gw(m_router.gateway);
    const int op = e.m.proto == QLatin1String("UDP") ? 1 : 2;
    QByteArray p(12, 0);
    p[1] = char(op);
    p[4] = char(e.m.internalPort >> 8);
    p[5] = char(e.m.internalPort & 0xFF);
    const int ext = lifetime == 0 ? 0 : (e.m.externalPort > 0 ? e.m.externalPort : e.m.internalPort);
    p[6] = char(ext >> 8);
    p[7] = char(ext & 0xFF);
    p[8] = char(quint32(lifetime) >> 24);
    p[9] = char((lifetime >> 16) & 0xFF);
    p[10] = char((lifetime >> 8) & 0xFF);
    p[11] = char(lifetime & 0xFF);
    m_pmpWaiting.insert((op << 16) | e.m.internalPort, key);
    if (!m_pmp || m_pmp->writeDatagram(p, gw, m_testPmpPort ? m_testPmpPort : kPmpPort) < 0) {
        m_requestBusy = false;
        m_pmpWaiting.remove((op << 16) | e.m.internalPort);
        scheduleRetry(e, QStringLiteral("could not send the NAT-PMP request"));
        return;
    }
    // no answer within 2 s: retry later
    QTimer::singleShot(int(2000 * qMax(0.05, m_retryScale)), this, [this, key, op, port = e.m.internalPort] {
        if (m_pmpWaiting.value((op << 16) | port) != key) return;
        m_pmpWaiting.remove((op << 16) | port);
        m_requestBusy = false;
        if (m_entries.contains(key)) {
            Entry &e = m_entries[key];
            if (e.remove) {
                m_order.removeAll(key);
                m_entries.remove(key);
            } else {
                e.m.method = QStringLiteral("NAT-PMP");
                scheduleRetry(e, QStringLiteral("no answer"));
            }
        }
        pump();
    });
}

void PortMapper::soap(const QString &action, const QList<QPair<QString, QString>> &args, SoapDone done)
{
    if (m_router.controlUrl.isEmpty() && !m_lastControl.isEmpty())
        soapTo(m_lastControl, m_lastService, action, args, std::move(done));
    else
        soapTo(m_router.controlUrl, m_router.serviceType, action, args, std::move(done));
}

void PortMapper::soapTo(const QString &controlUrl, const QString &serviceType, const QString &action,
                        const QList<QPair<QString, QString>> &args, SoapDone done)
{
    const QString svc = serviceType;
    QString body = QStringLiteral("<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                                  "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:%1 xmlns:u=\"%2\">")
                       .arg(action, svc);
    for (const auto &a : args) body += QStringLiteral("<%1>%2</%1>").arg(a.first, xmlEscape(a.second));
    body += QStringLiteral("</u:%1></s:Body></s:Envelope>\r\n").arg(action);
    QNetworkRequest req{QUrl(controlUrl)};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("text/xml; charset=\"utf-8\""));
    req.setRawHeader("SOAPAction", QStringLiteral("\"%1#%2\"").arg(svc, action).toLatin1());
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("OctoMX UPnP/1.1"));
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    QNetworkReply *r = m_nam->post(req, body.toUtf8());
    const qint64 epoch = m_epoch;
    connect(r, &QNetworkReply::finished, this, [this, r, done, epoch] {
        r->deleteLater();
        if (epoch != m_epoch) return;
        const QByteArray data = r->readAll();
        const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QString reason = r->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString();
        const QHash<QString, QString> leaves = leafTexts(data);
        if (status == 200 && r->error() == QNetworkReply::NoError) {
            done(true, leaves, QString());
            return;
        }
        QString err;
        if (status > 0) err = QStringLiteral("%1 %2").arg(status).arg(reason.isEmpty() ? QStringLiteral("HTTP error") : reason);
        else if (r->error() == QNetworkReply::OperationCanceledError || r->error() == QNetworkReply::TimeoutError) err = QStringLiteral("no answer");
        else err = r->errorString();
        if (leaves.contains(QStringLiteral("errorCode")))
            err += QStringLiteral(" (UPnP error %1%2)").arg(leaves.value(QStringLiteral("errorCode")),
                                                            leaves.value(QStringLiteral("errorDescription")).isEmpty() ? QString()
                                                                : QStringLiteral(": ") + leaves.value(QStringLiteral("errorDescription")));
        done(false, leaves, err);
    });
}

// ------------------------------------------------------------------------------------ shutdown
void PortMapper::shutdown(int timeoutMs)
{
    m_shuttingDown = true;
    m_pump.stop();
    QList<Entry> mapped;
    for (const QString &k : std::as_const(m_order)) {
        const Entry &e = m_entries.value(k);
        if (e.m.result == PortMapping::Mapped) mapped << e;
    }
    if (mapped.isEmpty() || (!m_router.upnpFound && !m_router.natPmpAnswered && !m_router.winNat && m_lastControl.isEmpty())) return;
    m_ssdpTimer.stop();
    m_router.searching = false;
    QElapsedTimer t;
    t.start();
    QEventLoop loop;
    QTimer guard;
    guard.setSingleShot(true);
    connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
    guard.start(timeoutMs);
    int removed = 0, failed = 0;
    QStringList failures;
    std::function<void(int)> next = [&](int i) {
        if (i >= mapped.size()) {
            loop.quit();
            return;
        }
        const Entry &e = mapped.at(i);
        if (e.m.method == QLatin1String("NAT-PMP")) {
            // NAT-PMP delete = lifetime 0, fire and forget
            QByteArray p(12, 0);
            p[1] = char(e.m.proto == QLatin1String("UDP") ? 1 : 2);
            p[4] = char(e.m.internalPort >> 8);
            p[5] = char(e.m.internalPort & 0xFF);
            if (m_pmp) m_pmp->writeDatagram(p, QHostAddress(m_router.gateway), m_testPmpPort ? m_testPmpPort : kPmpPort);
            ++removed;
            next(i + 1);
            return;
        }
        auto remove = [this](const Entry &x, std::function<void(bool, const QString &)> cb) {
            if (x.m.method == QLatin1String("UPnP (Windows)")) winRemove(x.m.proto, x.m.externalPort, cb);
            else deleteUpnp(x.m.proto, x.m.externalPort, cb);
        };
        remove(e, [&, i](bool ok, const QString &err) {
            const Entry &x = mapped.at(i);
            // 714 NoSuchEntryInArray: the router has no such mapping (any more), nothing left to remove
            if (ok || err.contains(QLatin1String("error 714"))) ++removed;
            else {
                ++failed;
                failures << QStringLiteral("%1 %2 (%3)").arg(x.m.proto).arg(x.m.externalPort).arg(err);
            }
            next(i + 1);
        });
    };
    next(0);
    if (removed + failed < mapped.size()) loop.exec();
    guard.stop();
    ++m_epoch;   // late replies are ignored
    if (failed || removed + failed < mapped.size())
        qCInfo(lcDiag).noquote() << QStringLiteral("port mapping: removed %1 of %2 mappings at shutdown; the router did not remove %3, they expire by themselves within an hour (not an error)")
                                       .arg(removed).arg(mapped.size()).arg(failures.isEmpty() ? QStringLiteral("the rest in time") : failures.join(QStringLiteral(", ")));
    else
        qCInfo(lcDiag).noquote() << QStringLiteral("port mapping: removed %1 mappings at shutdown in %2 ms").arg(removed).arg(t.elapsed());
    for (Entry &e : m_entries) e.m.result = PortMapping::Removed;
}

} // namespace mx::diag
