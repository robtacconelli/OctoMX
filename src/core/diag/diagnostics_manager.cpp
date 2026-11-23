// SPDX-License-Identifier: GPL-3.0-or-later
// [diag-1] the Connection Test service (see diagnostics_manager.h)
#include "core/diag/diagnostics_manager.h"
#include "core/diag/diag_log.h"
#include "core/engine/engine.h"
#include "core/log.h"
#include "core/network_manager.h"

#include "mx/bandwidth_limits.h"
#include "mx/settings_keys.h"

#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QFutureWatcher>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <QtCore/QSysInfo>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkAccessManager>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

Q_LOGGING_CATEGORY(lcDiag, "mx.diag")

namespace mx {

using diag::CheckResult;

namespace {
QString ago(const QDateTime &t)
{
    if (!t.isValid()) return QString();
    const qint64 s = qMax<qint64>(0, t.secsTo(QDateTime::currentDateTime()));
    if (s < 60) return QStringLiteral("%1 s ago").arg(s);
    if (s < 3600) return QStringLiteral("%1 min ago").arg(s / 60);
    return QStringLiteral("%1 h ago").arg(s / 3600);
}

QString hostOf(const QString &urlOrHostPort)
{
    const QString t = urlOrHostPort.trimmed();
    if (t.contains(QLatin1String("://"))) return QUrl(t).host();
    QString h;
    quint16 p = 0;
    if (diag::parseHostPortText(t, &h, &p, 1)) return h;
    return QString();
}

bool isIpLiteral(const QString &h)
{
    QHostAddress a;
    return a.setAddress(h);
}

DiagItem item(const QString &id, const QString &check, DiagLevel level, const QString &result,
              const QString &details = QString(), const QString &explain = QString())
{
    DiagItem i;
    i.id = id;
    i.check = check;
    i.level = level;
    i.result = result;
    i.details = details;
    i.explain = explain;
    return i;
}

QString reachText(Reach r)
{
    switch (r) {
    case Reach::Yes: return QStringLiteral("Yes");
    case Reach::No: return QStringLiteral("No");
    case Reach::Unknown: break;
    }
    return QStringLiteral("Unknown");
}

QString idPart(Net n) { return netName(n).toLower().remove(QLatin1Char(' ')); }

// how an OctoMX helper can test one of our UDP ports: BitTorrent answers DHT pings, the eD2k / Kad
// and Gnutella sockets report probe datagrams (mx::consumeProbeDatagram); librats' sockets cannot
QString probeKind(Net n, const QString &proto)
{
    if (proto != QLatin1String("UDP")) return QString();
    if (n == Net::BitTorrent) return QStringLiteral("dht");
    if (n == Net::ED2K || n == Net::Kad || n == Net::Gnutella) return QStringLiteral("datagram");
    return QStringLiteral("none");
}
} // namespace

DiagnosticsManager::DiagnosticsManager(Settings *settings, NetworkManager *network, DownloadEngine *engine,
                                       RateLimiter *limiter, const QString &profileDir, QObject *parent)
    : DiagnosticsService(parent)
    , m_settings(settings)
    , m_network(network)
    , m_engine(engine)
    , m_limiter(limiter)
    , m_profile(profileDir)
{
    m_mapper = new diag::PortMapper(this);
    if (m_settings) m_mapper->setCache(m_settings->value(keys::diag::IgdCache).toMap());
    connect(m_mapper, &diag::PortMapper::cacheUpdated, this, [this] {
        if (m_settings) m_settings->setValue(QString(keys::diag::IgdCache), m_mapper->cache());
    });
    connect(m_mapper, &diag::PortMapper::mappingChanged, this, &DiagnosticsManager::onMappingChanged);
    connect(m_mapper, &diag::PortMapper::routerChanged, this, &DiagnosticsManager::scheduleReportChanged);
    m_nam = new QNetworkAccessManager(this);
    m_bindTimer.setInterval(2000);
    connect(&m_bindTimer, &QTimer::timeout, this, &DiagnosticsManager::syncBindings);
    m_verdictTimer.setInterval(5000);
    connect(&m_verdictTimer, &QTimer::timeout, this, &DiagnosticsManager::updateVerdicts);
    m_reportTimer.setSingleShot(true);
    m_reportTimer.setInterval(1000);
    connect(&m_reportTimer, &QTimer::timeout, this, &DiagnosticsService::reportChanged);
    m_envTimer.setInterval(60000);
    connect(&m_envTimer, &QTimer::timeout, this, [this] { m_env = diag::probeNetEnv(); });
    m_summaryTimer.setInterval(30000);
    connect(&m_summaryTimer, &QTimer::timeout, this, &DiagnosticsManager::logTorrentSummaries);
    m_stepTimer.setInterval(200);
    connect(&m_stepTimer, &QTimer::timeout, this, &DiagnosticsManager::testStep);
    if (m_settings) {
        connect(m_settings, &Settings::changed, this, [this](const QString &key, const QVariant &) {
            if (key == keys::connection::Upnp || key == keys::connection::NatPmp) applyMapperConfig();
        });
    }
}

DiagnosticsManager::~DiagnosticsManager() = default;

void DiagnosticsManager::applyMapperConfig()
{
    // MX_PORT_MAPPING=0: never ask the router (unit tests that start a real core set it)
    if (qgetenv("MX_PORT_MAPPING") == "0") {
        m_mapper->setEnabled(false, false);
        scheduleReportChanged();
        return;
    }
    m_mapper->setEnabled(m_settings->b(keys::connection::Upnp), m_settings->b(keys::connection::NatPmp));
    scheduleReportChanged();
}

void DiagnosticsManager::onProbeResult(quint32 token, const QList<PortProbe> &res)
{
    if (!m_probeTokens.remove(token)) return;
    m_probes = res;
    m_probeWhen = QDateTime::currentDateTime();
    for (const PortProbe &x : res)
        qCInfo(lcDiag).noquote() << QStringLiteral("overlay port probe: %1 %2 %3 (%4%5)").arg(x.proto).arg(x.port)
                                        .arg(reachText(x.reach), x.helper, x.error.isEmpty() ? QString() : QStringLiteral(", ") + x.error);
    if (m_testRunning && m_testStep == 5) m_pending = 0;
    scheduleReportChanged();
}

void DiagnosticsManager::start()
{
    m_started = QDateTime::currentDateTime();
    for (NetworkPlugin *p : m_network->plugins())
        connect(p, &NetworkPlugin::portProbeResult, this, &DiagnosticsManager::onProbeResult);
    m_env = diag::probeNetEnv();
    applyMapperConfig();
    syncBindings();
    m_bindTimer.start();
    m_verdictTimer.start();
    m_envTimer.start();
    m_summaryTimer.start();
    runFirewallCheck();
    QTimer::singleShot(25000, this, [this] {
        if (m_startupLogged) return;
        m_startupLogged = true;
        const QStringList lines = startupBlock().split(QLatin1Char('\n'));
        for (const QString &l : lines) qCInfo(lcDiag).noquote() << l;
    });
}

void DiagnosticsManager::shutdown(int timeoutMs)
{
    m_bindTimer.stop();
    m_verdictTimer.stop();
    m_summaryTimer.stop();
    m_stepTimer.stop();
    m_mapper->shutdown(timeoutMs);
}

// ------------------------------------------------------------------------------------ bindings
void DiagnosticsManager::syncBindings()
{
    QList<diag::PortMapper::Want> wants;
    QHash<QString, QList<NetworkPlugin *>> owners;
    QHash<QString, QStringList> labels;
    for (NetworkPlugin *p : m_network->plugins()) {
        if (!m_network->isRunning(p->net())) continue;
        const PluginDiagnostics d = p->diagnostics();
        for (const PortBinding &b : d.bindings) {
            if (!b.ok || !b.mapByCore || b.port <= 0) continue;
            const QString k = b.proto + QLatin1Char('/') + QString::number(b.port);
            if (!owners.contains(k)) {
                diag::PortMapper::Want w;
                w.net = p->net();
                w.proto = b.proto;
                w.port = b.port;
                wants << w;
            }
            if (!owners[k].contains(p)) {
                owners[k] << p;
                labels[k] << netName(p->net());
            }
        }
    }
    for (diag::PortMapper::Want &w : wants) {
        const QStringList l = labels.value(w.proto + QLatin1Char('/') + QString::number(w.port));
        if (l.size() > 1) w.label = l.join(QStringLiteral(" + "));
    }
    m_portOwners = owners;
    m_mapper->setWanted(wants);
}

void DiagnosticsManager::onMappingChanged(const PortMapping &m)
{
    for (NetworkPlugin *p : m_portOwners.value(m.proto + QLatin1Char('/') + QString::number(m.internalPort))) {
        PortMapping x = m;
        x.net = p->net();
        p->portMappingChanged(x);
    }
    scheduleReportChanged();
}

void DiagnosticsManager::scheduleReportChanged()
{
    if (!m_reportTimer.isActive()) m_reportTimer.start();
}

// ------------------------------------------------------------------------------------ verdicts
diag::DownloadContext DiagnosticsManager::downloadContext() const
{
    diag::DownloadContext c;
    c.activeDownloads = m_engine ? m_engine->activeDownloadCount() : 0;
    c.maxDownloads = m_settings->i(keys::transfers::MaxDownloads);
    c.btActiveLimit = m_settings->i(keys::bt::MaxActiveDownloads);
    const int lim = m_limiter ? m_limiter->limitBps(RateLimiter::Down) : m_settings->i(keys::bandwidth::DownLimitKBs) * 1024;
    c.downLimitKBs = lim > 0 ? lim / 1024 : 0;
    const NetworkStatus e = m_network->status(Net::ED2K);
    c.weFirewalledEd2k = e.state == ConnState::Connected && e.firewalled;
    c.ed2kOrKadUp = e.state == ConnState::Connected || m_network->status(Net::Kad).state == ConnState::Connected;
    return c;
}

void DiagnosticsManager::updateVerdicts()
{
    if (!m_engine) return;
    const diag::DownloadContext c = downloadContext();
    QSet<QString> seen;
    for (const diag::DownloadFacts &f : m_engine->diagFacts()) {
        seen.insert(f.id);
        TorrentDiag t;
        if (m_torrents && !f.torrentId.isEmpty()) t = m_torrents->torrentDiagnostics(f.torrentId);
        const diag::Verdict v = diag::downloadVerdict(f, t, c);
        if (qEnvironmentVariableIsSet("MX_DIAG_DEBUG"))
            qCInfo(lcDiag).noquote() << QStringLiteral("verdict %1: active=%2 paused=%3 activeSec=%4 tvalid=%5 tpaused=%6 tqueued=%7 tactive=%8 trackers=%9 dht=%10 -> %11 %12")
                                           .arg(f.id).arg(f.active).arg(f.paused).arg(f.activeSec).arg(t.valid).arg(t.paused).arg(t.queued).arg(t.activeSec)
                                           .arg(t.trackers.size()).arg(t.dhtEnabled).arg(int(v.level)).arg(v.sentence);
        m_verdicts.insert(f.id, v);
        m_engine->setDiagnosis(f.id, v.sentence, int(v.level));
    }
    for (auto it = m_verdicts.begin(); it != m_verdicts.end();) {
        if (!seen.contains(it.key())) it = m_verdicts.erase(it);
        else ++it;
    }
}

QString DiagnosticsManager::downloadDiagnosis(const QString &transferId) const
{
    return m_verdicts.value(transferId).sentence;
}

void DiagnosticsManager::logTorrentSummaries()
{
    if (!m_engine || !m_torrents) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const diag::DownloadFacts &f : m_engine->diagFacts()) {
        if (f.torrentId.isEmpty() || !f.active || f.paused) continue;
        const TorrentDiag t = m_torrents->torrentDiagnostics(f.torrentId);
        if (!t.valid || t.connectedNow >= 3 || t.activeSec < 60) continue;
        if (now - m_lastSummaryLog.value(f.id) < 180000) continue;
        m_lastSummaryLog.insert(f.id, now);
        QStringList tr;
        for (const TrackerDiag &x : t.trackers.mid(0, 8)) {
            QString s = x.host + QLatin1Char(' ');
            if (x.dnsFailed) s += QStringLiteral("DNS failed");
            else if (x.working) s += QStringLiteral("working, %1 peers").arg(qMax(0, x.peersReturned));
            else if (x.updating) s += QStringLiteral("announcing");
            else if (!x.error.isEmpty()) s += QStringLiteral("error \"%1\"").arg(x.error.left(50));
            else s += x.contacted ? QStringLiteral("no answer") : QStringLiteral("not contacted");
            tr << s;
        }
        qCInfo(lcDiag).noquote() << QStringLiteral("torrent %1 (%2): connected %3 (%4 seeds), known %5, tried %6 (timed out %7, refused %8, handshake %9, banned %10, other %11), choking us %12, DHT %13 nodes / %14 lookups / %15 peers, metadata %16, trackers: %17; %18")
                                        .arg(f.name.left(60), f.torrentId.left(8)).arg(t.connectedNow).arg(t.seedsConnected)
                                        .arg(t.knownPeers).arg(t.connectAttempts).arg(t.failedTimeout).arg(t.failedRefused)
                                        .arg(t.failedHandshake).arg(t.failedBanned).arg(t.failedOther).arg(t.chokingUs)
                                        .arg(t.dhtNodes).arg(t.dhtLookups).arg(t.dhtPeers)
                                        .arg(t.hasMetadata ? QStringLiteral("yes") : QStringLiteral("no"))
                                        .arg(tr.isEmpty() ? QStringLiteral("none") : tr.join(QStringLiteral(", ")),
                                             m_verdicts.value(f.id).sentence.isEmpty() ? QStringLiteral("no diagnosis yet") : m_verdicts.value(f.id).sentence);
    }
}

// ------------------------------------------------------------------------------------ facts
QList<DiagnosticsManager::NetFacts> DiagnosticsManager::gatherNets() const
{
    QList<NetFacts> out;
    for (NetworkPlugin *p : m_network->plugins()) {
        NetFacts n;
        n.plugin = p;
        n.enabled = m_network->isEnabled(p->net());
        n.running = m_network->isRunning(p->net());
        n.st = p->status();
        if (n.running) n.d = p->diagnostics();
        out << n;
    }
    return out;
}

QStringList DiagnosticsManager::hostsFor(const NetFacts &n) const
{
    QStringList hosts = n.d.hosts;
    auto addUrl = [&hosts](const QString &u) {
        const QString h = hostOf(u);
        if (!h.isEmpty() && !isIpLiteral(h) && !hosts.contains(h)) hosts << h;
    };
    switch (n.plugin->net()) {
    case Net::BitTorrent:
        for (const QString &s : m_settings->sl(keys::bt::DhtBootstrapNodes)) addUrl(s);
        for (const QString &s : m_settings->sl(keys::bt::ExtraTrackers)) addUrl(s);
        if (m_torrents)
            for (const QString &id : m_torrents->torrents())
                for (const TrackerDiag &t : m_torrents->torrentDiagnostics(id).trackers) addUrl(t.url);
        break;
    case Net::ED2K: addUrl(m_settings->s(keys::ed2k::ServerListUrl)); break;
    case Net::Kad: addUrl(m_settings->s(keys::kad::NodesDatUrl)); break;
    case Net::Gnutella:
        for (const QString &s : m_settings->sl(keys::gnutella::GWebCaches)) addUrl(s);
        break;
    default: break;
    }
    QStringList clean;
    for (const QString &h : hosts)
        if (!h.isEmpty() && !isIpLiteral(h) && !clean.contains(h)) clean << h;
    return clean.mid(0, 24);
}

DiagItem DiagnosticsManager::mappingItem(const PortBinding &b, Net net) const
{
    const QString id = QStringLiteral("%1.map.%2.%3").arg(idPart(net), b.proto.toLower()).arg(b.port);
    const QString check = QStringLiteral("Port mapping %1 %2").arg(b.proto).arg(b.port);
    if (!b.mapByCore) return item(id, check, DiagLevel::Off, QStringLiteral("Not needed"), b.purpose);
    PortMapping m;
    bool found = false;
    for (const PortMapping &x : m_mapper->mappings())
        if (x.proto == b.proto && x.internalPort == b.port) { m = x; found = true; break; }
    const QString local = m_mapper->router().localAddress.isEmpty() ? m_env.defaultSource : m_mapper->router().localAddress;
    const QString manual = QStringLiteral("In your router, forward %1 port %2 to %3.").arg(b.proto).arg(b.port).arg(local.isEmpty() ? QStringLiteral("this computer") : local);
    if (!found || m.result == PortMapping::Off) {
        if (!m_mapper->upnpEnabled() && !m_mapper->natPmpEnabled())
            return item(id, check, DiagLevel::Off, QStringLiteral("Off"), QStringLiteral("UPnP and NAT-PMP are switched off"),
                        QStringLiteral("Automatic port mapping is off (Settings, Internet Connection). %1").arg(manual));
        return item(id, check, DiagLevel::Unknown, QStringLiteral("Not requested yet"));
    }
    switch (m.result) {
    case PortMapping::Mapped: {
        QString r = m.externalPort == m.internalPort ? QStringLiteral("Mapped by %1").arg(m.method)
                                                     : QStringLiteral("Mapped by %1, internal %2, external %3").arg(m.method).arg(m.internalPort).arg(m.externalPort);
        QString det = ago(m.when);
        if (m.expires.isValid()) det += QStringLiteral(", lease until %1").arg(m.expires.toString(QStringLiteral("HH:mm")));
        const diag::RouterInfo ri = m_mapper->router();
        if (m.method == QLatin1String("UPnP") && ((!ri.gateway.isEmpty() && !ri.isGateway()) || ri.upnpWan == QLatin1String("0.0.0.0")))
            return item(id, check, DiagLevel::Limited, r + QStringLiteral(" on %1").arg(ri.address), det,
                        QStringLiteral("The port is mapped on %1, which is not the router connected to the internet (see General). %2").arg(ri.address, manual));
        return item(id, check, DiagLevel::Ok, r, det,
                    m.externalPort == m.internalPort ? QString()
                        : QStringLiteral("The router gave external port %1 for internal port %2 (another computer, or an old mapping, holds %2). OctoMX announces %1 where the network supports it.")
                              .arg(m.externalPort).arg(m.internalPort));
    }
    case PortMapping::Failed: {
        const diag::RouterInfo ri = m_mapper->router();
        if (!ri.upnpFound && !ri.natPmpAnswered && !ri.winNat)   // no router at all: the reason is in General
            return item(id, check, DiagLevel::Limited, QStringLiteral("Not mapped"), QStringLiteral("no router answered, see General"),
                        QStringLiteral("%1 Or enable UPnP in the router.").arg(manual));
        return item(id, check, DiagLevel::Limited, QStringLiteral("Failed"), QStringLiteral("the router answered: %1").arg(m.error),
                    QStringLiteral("The router refused to forward this port. %1").arg(manual));
    }
    case PortMapping::Pending:
        return item(id, check, DiagLevel::Unknown, QStringLiteral("Asking the router..."), m.method);
    default:
        return item(id, check, DiagLevel::Unknown, QStringLiteral("Removed"), ago(m.when));
    }
}

QString DiagnosticsManager::forwardAdvice(const QList<PortBinding> &bindings) const
{
    QStringList tcp, udp;
    for (const PortBinding &b : bindings) {
        if (!b.ok || !b.mapByCore) continue;
        (b.proto == QLatin1String("UDP") ? udp : tcp) << QString::number(b.port);
    }
    tcp.removeDuplicates();
    udp.removeDuplicates();
    const QString local = m_mapper->router().localAddress.isEmpty() ? m_env.defaultSource : m_mapper->router().localAddress;
    QStringList parts;
    if (!tcp.isEmpty()) parts << QStringLiteral("TCP port %1").arg(tcp.join(QStringLiteral(", ")));
    if (!udp.isEmpty()) parts << QStringLiteral("UDP port %1").arg(udp.join(QStringLiteral(", ")));
    if (parts.isEmpty()) return QString();
    QString a = QStringLiteral("In your router, forward %1 to %2.").arg(parts.join(QStringLiteral(" and ")), local.isEmpty() ? QStringLiteral("this computer") : local);
    const diag::RouterInfo ri = m_mapper->router();
    const diag::NatVerdict nat = diag::natVerdict(ri.wan(), {});
    if (ri.upnpFound && !ri.gateway.isEmpty() && !ri.isGateway())
        a = QStringLiteral("In your internet router (%1), forward %2 to %3.").arg(ri.gateway, parts.join(QStringLiteral(" and ")), local.isEmpty() ? QStringLiteral("this computer") : local);
    if (nat.kind == QLatin1String("carrier-grade NAT"))
        a = QStringLiteral("Your provider uses carrier-grade NAT: port forwarding is not possible, downloads still work (fewer sources).");
    else if (nat.kind == QLatin1String("double NAT"))
        a += QStringLiteral(" There is a second router in front of yours: forward the ports there too.");
#ifdef Q_OS_WIN
    if (m_firewall.checked && m_firewall.enabled && m_firewall.allowRules == 0)
        a += QStringLiteral(" Windows Firewall has no rule for OctoMX: allow it in Windows Security ▸ Firewall & network protection ▸ Allow an app through firewall.");
#endif
    return a;
}

// ------------------------------------------------------------------------------------ report
DiagGroup DiagnosticsManager::generalGroup(const QList<NetFacts> &nets) const
{
    DiagGroup g;
    g.id = QStringLiteral("general");
    g.title = QStringLiteral("General");
    // adapters
    const diag::RouterInfo rinfo = m_mapper->router();
    for (const diag::AdapterInfo &a : m_env.adapters) {
        QStringList addr = a.ipv4 + a.ipv6.mid(0, 2);
        QStringList dets;
        if (!a.description.isEmpty()) dets << a.description;
        if (a.isDefault) dets << QStringLiteral("default route");
        if (!a.kindNote.isEmpty()) dets << a.kindNote;
        // [diag-1 follow-up] what the router search heard on this adapter
        for (int i = 0; i < rinfo.adapters.size(); ++i) {
            bool mine = false;
            for (const QString &ip : a.ipv4) mine = mine || rinfo.adapters.at(i).endsWith(QLatin1Char(' ') + ip);
            if (!mine) continue;
            if (rinfo.adapterRouter.value(i)) dets << QStringLiteral("the router answered here");
            else if (rinfo.adapterAnswers.value(i) > 0) dets << QStringLiteral("UPnP: %1, no router").arg(countNoun(rinfo.adapterAnswers.value(i), QStringLiteral("answer")));
            else dets << QStringLiteral("UPnP: no answer");
        }
        const QString det = dets.join(QStringLiteral(", "));
        DiagLevel lv = DiagLevel::Ok;
        QString explain;
        if (a.isDefault && a.kindNote == QLatin1String("VPN")) {
            lv = DiagLevel::Limited;
            explain = QStringLiteral("Your internet traffic goes through the VPN adapter %1: port forwarding in your own router does not apply. Downloads work; to be reachable, the VPN provider must offer port forwarding.").arg(a.humanName);
        } else if (a.kindNote == QLatin1String("VPN")) {
            explain = QStringLiteral("A VPN adapter (%1) is present but the internet route does not use it.").arg(a.humanName);
        }
        g.items << item(QStringLiteral("general.adapter.") + a.name, QStringLiteral("Adapter %1").arg(a.humanName), lv,
                        addr.join(QStringLiteral(", ")), det, explain);
    }
    if (m_env.defaultSource.isEmpty())
        g.items << item(QStringLiteral("general.route"), QStringLiteral("Internet route"), DiagLevel::Broken, QStringLiteral("None"),
                        QString(), QStringLiteral("This computer has no route to the internet: check the network cable or Wi-Fi."));
    else
        g.items << item(QStringLiteral("general.route"), QStringLiteral("Internet route"), DiagLevel::Ok,
                        m_env.defaultAdapter.isEmpty() ? QStringLiteral("Through %1").arg(m_env.defaultSource)
                                                       : QStringLiteral("Through %1 (%2)").arg(m_env.defaultAdapter, m_env.defaultSource),
                        m_env.gateway.isNull() ? QString() : QStringLiteral("gateway %1").arg(m_env.gateway.toString()));
    // proxy / bind interface
    const QString proxy = m_settings->s(keys::connection::ProxyType);
    if (proxy.isEmpty() || proxy == QLatin1String("none"))
        g.items << item(QStringLiteral("general.proxy"), QStringLiteral("Proxy"), DiagLevel::Ok, QStringLiteral("None"));
    else
        g.items << item(QStringLiteral("general.proxy"), QStringLiteral("Proxy"), DiagLevel::Limited,
                        QStringLiteral("%1 %2:%3").arg(proxy.toUpper(), m_settings->s(keys::connection::ProxyHost)).arg(m_settings->i(keys::connection::ProxyPort)),
                        m_settings->b(keys::connection::ProxyPeers) ? QStringLiteral("peer connections too") : QStringLiteral("servers and trackers only"),
                        QStringLiteral("Connections go through a proxy: incoming connections are not possible through it, and a slow proxy slows every download."));
    const QString bind = m_settings->s(keys::connection::BindInterface);
    if (bind.isEmpty()) {
        g.items << item(QStringLiteral("general.bind"), QStringLiteral("Bind interface"), DiagLevel::Ok, QStringLiteral("All adapters"));
    } else {
        bool exists = false;
        for (const diag::AdapterInfo &a : m_env.adapters)
            if (a.ipv4.contains(bind) || a.ipv6.contains(bind) || a.name == bind || a.humanName == bind) exists = true;
        g.items << item(QStringLiteral("general.bind"), QStringLiteral("Bind interface"), exists ? DiagLevel::Ok : DiagLevel::Broken, bind,
                        exists ? QString() : QStringLiteral("not on this computer"),
                        exists ? QStringLiteral("Only this adapter is used for BitTorrent.")
                               : QStringLiteral("The bind interface %1 does not exist on this computer, so BitTorrent cannot listen or connect: clear it in Settings, Internet Connection.").arg(bind));
    }
    // router
    const diag::RouterInfo r = m_mapper->router();
    if (!m_mapper->upnpEnabled() && !m_mapper->natPmpEnabled()) {
        g.items << item(QStringLiteral("general.router"), QStringLiteral("Router (UPnP / NAT-PMP)"), DiagLevel::Off, QStringLiteral("Not asked"),
                        QStringLiteral("automatic port mapping is off"));
    } else if (r.searching && !r.searched) {
        g.items << item(QStringLiteral("general.router"), QStringLiteral("Router (UPnP / NAT-PMP)"), DiagLevel::Unknown, QStringLiteral("Searching..."));
    } else if (r.upnpFound) {
        QString name = r.friendlyName;
        const QString mm = QStringList({r.manufacturer, r.model}).join(QLatin1Char(' ')).trimmed();
        if (!mm.isEmpty() && !name.contains(mm)) name += QStringLiteral(" (%1)").arg(mm);
        const bool noWan = r.upnpWan.isEmpty() || r.upnpWan == QLatin1String("0.0.0.0");
        const bool notGw = !r.gateway.isEmpty() && !r.isGateway();
        QString explain;
        if (notGw && noWan)
            explain = QStringLiteral("The UPnP router that answered (%1) is not your default gateway (%2) and has no internet address: it is probably a second router used as an access point. Port mappings there do not reach the internet. Enable UPnP on the gateway %2, or forward the ports there by hand.")
                          .arg(r.address, r.gateway);
        else if (notGw)
            explain = QStringLiteral("The UPnP router that answered (%1) is not your default gateway (%2): mappings there only help if all traffic passes through it.").arg(r.address, r.gateway);
        else if (noWan)
            explain = QStringLiteral("The router reports no internet address: it may not be connected to the internet itself (a second router in front of it?).");
        g.items << item(QStringLiteral("general.router"), QStringLiteral("Router (UPnP)"), notGw || noWan ? DiagLevel::Limited : DiagLevel::Ok,
                        name.isEmpty() ? QStringLiteral("Found") : name,
                        QStringLiteral("at %1%2%3").arg(r.address, r.isGateway() ? QStringLiteral(", the default gateway") : notGw ? QStringLiteral(", not the default gateway") : QString(),
                                                        r.natPmpAnswered ? QStringLiteral(", NAT-PMP too") : QString()),
                        explain);
        for (int i = 0; i < r.others.size(); ++i)
            g.items << item(QStringLiteral("general.router.other%1").arg(i), QStringLiteral("Other UPnP router"), DiagLevel::Ok, QStringLiteral("Not used"),
                            r.others.at(i), QStringLiteral("Another device on your network also offers UPnP port mapping. OctoMX uses the default gateway, or the one with an internet address."));
    } else if (r.natPmpAnswered) {
        g.items << item(QStringLiteral("general.router"), QStringLiteral("Router (NAT-PMP)"), DiagLevel::Ok, QStringLiteral("Found"),
                        QStringLiteral("at %1, no UPnP: %2").arg(r.gateway, r.upnpError));
    } else if (r.winNat) {
        g.items << item(QStringLiteral("general.router"), QStringLiteral("Router (UPnP through Windows)"), DiagLevel::Ok, QStringLiteral("Found by Windows"),
                        QStringLiteral("own search: no answer; %1").arg(r.winResult),
                        QStringLiteral("OctoMX's own search got no answer, but the Windows UPnP service knows the router, so the ports are mapped through Windows."));
    } else {
        diag::DiscoveryFacts f;
        f.adapters = r.adapters;
        f.answers = r.adapterAnswers;
        f.gateway = r.gateway;
        f.unicastTried = r.unicastTried;
        f.searchSkipped = r.searchSkipped;
        f.cacheTried = r.cacheTried;
        f.notifyListening = r.notifyListening;
        f.upnpOn = m_mapper->upnpEnabled();
        f.natPmpOn = m_mapper->natPmpEnabled();
        f.natPmpResult = r.natPmpError;
        f.winTried = r.winTried;
        f.winResult = r.winResult;
        f.winFoundNone = r.winResult.contains(QLatin1String("found no UPnP router"));
        f.defaultAdapter = m_env.defaultAdapter;
        f.defaultIsVirtual = false;
        for (const diag::AdapterInfo &a : m_env.adapters)
            if (a.isDefault && a.virtualOrVpn) f.defaultIsVirtual = true;
        f.firewallChecked = m_firewall.checked;
        f.firewallOn = m_firewall.enabled;
        f.firewallProfiles = m_firewall.profiles;
        f.firewallAllowRules = m_firewall.allowRules;
        int answers = 0;
        for (int n : r.adapterAnswers) answers += n;
        g.items << item(QStringLiteral("general.router"), QStringLiteral("Router (UPnP / NAT-PMP)"), DiagLevel::Limited, QStringLiteral("Not found"),
                        QStringLiteral("searched on %1, %2").arg(countNoun(r.adapters.size(), QStringLiteral("adapter")),
                                                                  answers ? countNoun(answers, QStringLiteral("answer")) + QStringLiteral(" from other devices") : QStringLiteral("nothing answered")),
                        diag::discoveryFailureText(f));
    }
    // external addresses and NAT
    QStringList seen;
    QStringList seenText;
    for (const NetFacts &n : nets) {
        const QString ext = !n.d.externalAddress.isEmpty() ? n.d.externalAddress : n.st.externalAddress;
        if (ext.isEmpty() || !n.running) continue;
        const QString ip = ext.section(QLatin1Char(':'), 0, 0);
        seen << ip;
        seenText << QStringLiteral("%1 (%2)").arg(ip, n.d.externalSeenBy.isEmpty() ? netName(n.plugin->net()) : n.d.externalSeenBy);
    }
    const diag::NatVerdict nat = diag::natVerdict(r.wan(), seen);
    const bool wanZero = r.wan() == QLatin1String("0.0.0.0");
    g.items << item(QStringLiteral("general.wan"), QStringLiteral("Router internet address"),
                    r.wan().isEmpty() ? DiagLevel::Unknown : wanZero ? DiagLevel::Limited : DiagLevel::Ok,
                    r.wan().isEmpty() ? QStringLiteral("Unknown") : wanZero ? QStringLiteral("None (0.0.0.0)") : r.wan(), r.method());
    g.items << item(QStringLiteral("general.seen"), QStringLiteral("Seen from the internet as"), seen.isEmpty() ? DiagLevel::Unknown : DiagLevel::Ok,
                    seen.isEmpty() ? QStringLiteral("Not reported yet") : QStringList(QSet<QString>(seen.begin(), seen.end()).values()).join(QStringLiteral(", ")),
                    seenText.join(QStringLiteral(", ")));
    g.items << item(QStringLiteral("general.nat"), QStringLiteral("NAT"), nat.level,
                    nat.kind.isEmpty() ? (nat.level == DiagLevel::Ok ? QStringLiteral("One router, public address") : QStringLiteral("Unknown")) : nat.kind,
                    QString(), nat.text);
    if (m_env.hasIpv6)
        g.items << item(QStringLiteral("general.ipv6"), QStringLiteral("IPv6"), DiagLevel::Ok, QStringLiteral("Available"),
                        m_settings->b(keys::connection::Ipv6) ? QStringLiteral("used by BitTorrent") : QStringLiteral("switched off in the settings"));
    // limits
    // [bw-1] the limits in force and why (schedule, or the user's limits during a schedule window)
    const bwlimit::InForce lim = bwlimit::inForce(m_settings);
    const int down = lim.source == bwlimit::InForce::Schedule ? -1 : lim.downKBs, up = lim.source == bwlimit::InForce::Schedule ? -1 : lim.upKBs;
    const int nowDown = m_limiter ? m_limiter->limitBps(RateLimiter::Down) / 1024 : lim.downKBs;
    const int nowUp = m_limiter ? m_limiter->limitBps(RateLimiter::Up) / 1024 : lim.upKBs;
    g.items << item(QStringLiteral("general.limit.down"), QStringLiteral("Download limit"), nowDown > 0 ? DiagLevel::Limited : DiagLevel::Ok,
                    nowDown > 0 ? QStringLiteral("%1 KB/s").arg(nowDown) : QStringLiteral("Unlimited"),
                    nowDown != down ? QStringLiteral("schedule in force") : QString(),
                    nowDown > 0 ? QStringLiteral("Downloads are limited to %1 KB/s in total (Settings, Bandwidth).").arg(nowDown) : QString());
    g.items << item(QStringLiteral("general.limit.up"), QStringLiteral("Upload limit"), nowUp > 0 && nowUp < 8 ? DiagLevel::Limited : DiagLevel::Ok,
                    nowUp > 0 ? QStringLiteral("%1 KB/s").arg(nowUp) : QStringLiteral("Unlimited"),
                    nowUp != up ? QStringLiteral("schedule in force") : QString(),
                    nowUp > 0 && nowUp < 8 ? QStringLiteral("An upload limit this low also slows BitTorrent downloads (peers give back to those who give).") : QString());
    if (m_settings->b(keys::bandwidth::ScheduleEnabled))
        g.items << item(QStringLiteral("general.limit.schedule"), QStringLiteral("Bandwidth schedule"), DiagLevel::Ok,
                        QStringLiteral("%1 to %2: %3 / %4 KB/s").arg(m_settings->s(keys::bandwidth::ScheduleStart), m_settings->s(keys::bandwidth::ScheduleEnd))
                            .arg(m_settings->i(keys::bandwidth::ScheduleDownKBs)).arg(m_settings->i(keys::bandwidth::ScheduleUpKBs)),
                        QStringLiteral("down / up, 0 = unlimited"));
    g.items << item(QStringLiteral("general.slots"), QStringLiteral("Download slots"), DiagLevel::Ok,
                    QStringLiteral("%1 active of %2, torrents %3 at once").arg(m_engine ? m_engine->activeDownloadCount() : 0)
                        .arg(m_settings->i(keys::transfers::MaxDownloads)).arg(m_settings->i(keys::bt::MaxActiveDownloads)),
                    QStringLiteral("sources per download %1").arg(m_settings->i(keys::transfers::MaxSourcesPerDownload)),
                    QStringLiteral("More torrents than \"Active torrent downloads\" wait as \"Queued by BitTorrent\" until one finishes or is paused."));
    g.items << item(QStringLiteral("general.connections"), QStringLiteral("Connection limits"), DiagLevel::Ok,
                    QStringLiteral("BitTorrent %1 (%2 per torrent), eD2k %3").arg(m_settings->i(keys::bt::MaxConnections))
                        .arg(m_settings->i(keys::bt::MaxConnectionsPerTorrent)).arg(m_settings->i(keys::ed2k::MaxConnections)));
    // firewall
#ifdef Q_OS_WIN
    if (m_firewallRunning && !m_firewall.checked)
        g.items << item(QStringLiteral("general.firewall"), QStringLiteral("Windows Firewall"), DiagLevel::Unknown, QStringLiteral("Checking..."));
    else if (!m_firewall.checked)
        g.items << item(QStringLiteral("general.firewall"), QStringLiteral("Windows Firewall"), DiagLevel::Unknown, QStringLiteral("Could not be checked"), m_firewall.error);
    else if (!m_firewall.enabled)
        g.items << item(QStringLiteral("general.firewall"), QStringLiteral("Windows Firewall"), DiagLevel::Ok, QStringLiteral("Off"), m_firewall.profiles);
    else if (m_firewall.blockAllInbound)
        g.items << item(QStringLiteral("general.firewall"), QStringLiteral("Windows Firewall"), DiagLevel::Limited, QStringLiteral("Blocks all incoming connections"),
                        m_firewall.profiles, QStringLiteral("\"Block all incoming connections\" is on for the %1 profile: nobody can connect to OctoMX. Turn it off in Windows Security ▸ Firewall & network protection.").arg(m_firewall.profiles));
    else if (m_firewall.blockRules > 0)
        g.items << item(QStringLiteral("general.firewall"), QStringLiteral("Windows Firewall"), DiagLevel::Limited, QStringLiteral("A rule blocks OctoMX"),
                        m_firewall.ruleNames.join(QStringLiteral(", ")), QStringLiteral("A firewall rule blocks incoming connections to OctoMX: remove it or allow OctoMX in Windows Security ▸ Firewall & network protection ▸ Allow an app through firewall."));
    else if (m_firewall.allowRules > 0)
        g.items << item(QStringLiteral("general.firewall"), QStringLiteral("Windows Firewall"), DiagLevel::Ok, QStringLiteral("On, OctoMX allowed"),
                        QStringLiteral("%1 profile, rule %2").arg(m_firewall.profiles, m_firewall.ruleNames.value(0)));
    else
        g.items << item(QStringLiteral("general.firewall"), QStringLiteral("Windows Firewall"), DiagLevel::Limited, QStringLiteral("On, no rule for OctoMX"),
                        m_firewall.profiles, QStringLiteral("Windows Firewall has no rule for OctoMX, so incoming connections are blocked: allow it in Windows Security ▸ Firewall & network protection ▸ Allow an app through firewall.%1")
                                                 .arg(m_firewall.oldProgramRules.isEmpty() ? QString()
                                                      : QStringLiteral(" The rule \"%1\" is for winmx.exe, an older program file in this folder: Windows does not apply it to octomx.exe.")
                                                            .arg(m_firewall.oldProgramRules.first())));
#else
    g.items << item(QStringLiteral("general.firewall"), QStringLiteral("Firewall"), DiagLevel::Off, QStringLiteral("Not checked"),
                    QStringLiteral("only checked on Windows"));
#endif
    DiagLevel lv = DiagLevel::Unknown;
    for (const DiagItem &i : g.items) lv = diagWorse(lv, i.level);
    g.level = lv;
    const DiagItem *worst = nullptr;
    for (const DiagItem &i : g.items)
        if (i.level == lv && (lv == DiagLevel::Broken || lv == DiagLevel::Limited)) { worst = &i; break; }
    g.result = worst ? QStringLiteral("%1: %2").arg(worst->check, worst->result) : QStringLiteral("Fine");
    return g;
}

DiagGroup DiagnosticsManager::networkGroup(const NetFacts &n) const
{
    const Net net = n.plugin->net();
    const QString p = idPart(net);
    DiagGroup g;
    g.id = QStringLiteral("net.") + netName(net);
    g.title = netName(net);
    g.net = net;
    if (!n.enabled) {
        g.level = DiagLevel::Off;
        g.result = QStringLiteral("Switched off");
        g.items << item(p + QStringLiteral(".state"), QStringLiteral("State"), DiagLevel::Off, QStringLiteral("Switched off"),
                        QString(), QStringLiteral("This network is disabled on the Networks page."));
        return g;
    }
    if (!n.running) {
        g.level = DiagLevel::Off;
        g.result = QStringLiteral("Not connected");
        g.items << item(p + QStringLiteral(".state"), QStringLiteral("State"), DiagLevel::Off, QStringLiteral("Not connected"),
                        n.st.statusText, QStringLiteral("Press Connect on the Networks page."));
        return g;
    }
    const DiagLevel stLevel = n.st.state == ConnState::Connected ? DiagLevel::Ok
                              : n.st.state == ConnState::Error ? DiagLevel::Broken
                                                                : DiagLevel::Unknown;
    g.items << item(p + QStringLiteral(".state"), QStringLiteral("State"), stLevel,
                    n.st.state == ConnState::Connected ? QStringLiteral("Connected") : n.st.state == ConnState::Error ? QStringLiteral("Error") : QStringLiteral("Connecting..."),
                    n.st.statusText);
    // bindings
    for (const PortBinding &b : n.d.bindings) {
        const QString purpose = b.purpose.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(b.purpose);
        if (b.ok && !b.error.isEmpty())   // listening, but not on the port that was set ("busy, using ...")
            g.items << item(QStringLiteral("%1.port.%2.%3").arg(p, b.proto.toLower()).arg(b.port),
                            QStringLiteral("Listen %1 %2").arg(b.proto).arg(b.port), DiagLevel::Limited, QStringLiteral("Listening, other port"), b.error,
                            QStringLiteral("The %1 port set in the settings is used by another program (%2), so OctoMX took another one. It works, but a port you forwarded by hand now points to the wrong port: close the other program or change the port in the settings.")
                                .arg(b.proto, b.error));
        else if (b.ok)
            g.items << item(QStringLiteral("%1.port.%2.%3").arg(p, b.proto.toLower()).arg(b.port),
                            QStringLiteral("Listen %1 %2").arg(b.proto).arg(b.port), DiagLevel::Ok, QStringLiteral("Listening"), b.purpose);
        else
            g.items << item(QStringLiteral("%1.port.%2.%3").arg(p, b.proto.toLower()).arg(b.port),
                            QStringLiteral("Listen %1 %2").arg(b.proto).arg(b.port), DiagLevel::Broken, QStringLiteral("Failed"), b.error,
                            QStringLiteral("OctoMX could not open %1 port %2%3: %4. Another program probably uses it: close it or pick another port in the settings.")
                                .arg(b.proto).arg(b.port).arg(purpose, b.error));
    }
    for (const PortBinding &b : n.d.bindings)
        if (b.ok) g.items << mappingItem(b, net);
    for (const PortMapping &m : n.d.ownMappings) {
        g.items << item(QStringLiteral("%1.map.%2.%3").arg(p, m.proto.toLower()).arg(m.internalPort),
                        QStringLiteral("Port mapping %1 %2").arg(m.proto).arg(m.internalPort),
                        m.result == PortMapping::Mapped ? DiagLevel::Ok : m.result == PortMapping::Failed ? DiagLevel::Limited : DiagLevel::Unknown,
                        m.result == PortMapping::Mapped ? QStringLiteral("Mapped by %1, external %2").arg(m.method).arg(m.externalPort) : m.error,
                        QStringLiteral("by the network itself"));
    }
    // reachability, with the public checker and the overlay probe as extra evidence
    Reach tcp = n.d.tcpIn, udp = n.d.udpIn;
    QString tcpEv = n.d.tcpEvidence, udpEv = n.d.udpEvidence;
    int blockedPort = 0;   // forwarded by the router, yet closed from the outside
    for (const PortBinding &b : n.d.bindings) {
        if (!b.ok) continue;
        if (b.proto == QLatin1String("TCP") && m_portChecks.contains(b.port)) {
            const Timed &t = m_portChecks.value(b.port);
            if (t.r.answered) {
                const Reach r = t.r.ok ? Reach::Yes : Reach::No;
                if (tcp == Reach::Unknown || r == Reach::Yes || (r == Reach::No && tcp == Reach::No)) {
                    tcp = r;
                    tcpEv = QStringLiteral("%1 says port %2 is %3 (%4)").arg(t.what).arg(b.port).arg(t.r.ok ? QStringLiteral("open") : QStringLiteral("closed"), ago(t.when));
                    if (r == Reach::No) {
                        const diag::RouterInfo ri = m_mapper->router();
                        for (const PortMapping &pm : m_mapper->mappings())
                            if (pm.proto == QLatin1String("TCP") && pm.internalPort == b.port && pm.result == PortMapping::Mapped && ri.isGateway())
                                blockedPort = pm.externalPort;
                    }
                }
            }
        }
        for (const PortProbe &pr : m_probes) {
            if (pr.proto != b.proto || pr.port != b.port || pr.reach == Reach::Unknown) continue;
            Reach &dst = b.proto == QLatin1String("TCP") ? tcp : udp;
            QString &ev = b.proto == QLatin1String("TCP") ? tcpEv : udpEv;
            if (dst == Reach::Unknown || pr.reach == Reach::Yes) {
                dst = pr.reach;
                ev = QStringLiteral("%1 %2 port %3 (%4)").arg(pr.helper, pr.reach == Reach::Yes ? QStringLiteral("reached") : QStringLiteral("could not reach")).arg(pr.port).arg(ago(m_probeWhen));
            }
        }
    }
    bool hasTcp = false, hasUdp = false;
    for (const PortBinding &b : n.d.bindings) {
        if (b.proto == QLatin1String("TCP") && b.ok) hasTcp = true;
        if (b.proto == QLatin1String("UDP") && b.ok) hasUdp = true;
    }
    const QString advice = forwardAdvice(n.d.bindings);
    auto reachItem = [&](const QString &proto, Reach r, const QString &ev) {
        const DiagLevel lv = r == Reach::Yes ? DiagLevel::Ok : r == Reach::No ? DiagLevel::Limited : DiagLevel::Unknown;
        QString explain;
        if (r == Reach::No && proto == QLatin1String("TCP") && blockedPort > 0)
            explain = QStringLiteral("Your router forwards port %1, but it is still closed from the internet: your provider (or a firewall in the router) blocks it. "
                                     "Ports 6881 to 6889 and 4444 are often blocked. Choose another port in the settings (for example one between 20000 and 60000).")
                          .arg(blockedPort);
        else if (r == Reach::No)
            explain = QStringLiteral("Others cannot connect to you over %1 on %2: downloads work, but with fewer sources (two firewalled users cannot connect to each other). %3")
                          .arg(proto, netName(net), advice);
        else if (r == Reach::Unknown)
            explain = QStringLiteral("No evidence yet whether %1 can reach you over %2. Press Test Now, or Check Ports Online.").arg(netName(net), proto);
        return item(QStringLiteral("%1.reach.%2").arg(p, proto.toLower()), QStringLiteral("Reachable from the internet (%1)").arg(proto), lv,
                    reachText(r), ev, explain);
    };
    if (hasTcp) g.items << reachItem(QStringLiteral("TCP"), tcp, tcpEv);
    if (hasTcp && tcp == Reach::No && blockedPort > 0)
        g.items << item(p + QStringLiteral(".blocked"), QStringLiteral("Forwarded port"), DiagLevel::Limited, QStringLiteral("Blocked before the router"),
                        QStringLiteral("TCP %1 is forwarded, the public checker still finds it closed").arg(blockedPort));
    if (hasUdp) g.items << reachItem(QStringLiteral("UDP"), udp, udpEv);
    const QString ext = !n.d.externalAddress.isEmpty() ? n.d.externalAddress : n.st.externalAddress;
    if (!ext.isEmpty())
        g.items << item(p + QStringLiteral(".external"), QStringLiteral("External address"), DiagLevel::Ok, ext,
                        QStringList({n.d.externalSeenBy, n.d.externalUdpPort > 0 ? QStringLiteral("UDP port %1 as seen by others").arg(n.d.externalUdpPort) : QString()})
                            .filter(QRegularExpression(QStringLiteral("."))).join(QStringLiteral(", ")));
    // outbound health
    const QStringList hosts = hostsFor(n);
    if (!hosts.isEmpty()) {
        QStringList bad, unknown;
        int good = 0;
        for (const QString &h : hosts) {
            if (!m_dns.contains(h)) unknown << h;
            else if (m_dns.value(h).r.ok) ++good;
            else bad << h;
        }
        if (good + bad.size() == 0)
            g.items << item(p + QStringLiteral(".dns"), QStringLiteral("Name lookups (DNS)"), DiagLevel::Unknown,
                            QStringLiteral("Not tested"), QStringLiteral("%1 host names").arg(hosts.size()),
                            QStringLiteral("Press Test Now to check that the server and tracker names of this network resolve."));
        else if (bad.isEmpty())
            g.items << item(p + QStringLiteral(".dns"), QStringLiteral("Name lookups (DNS)"), DiagLevel::Ok,
                            QStringLiteral("%1 of %2 resolve").arg(good).arg(good + bad.size()), unknown.isEmpty() ? QString() : QStringLiteral("%1 not tested").arg(unknown.size()));
        else
            g.items << item(p + QStringLiteral(".dns"), QStringLiteral("Name lookups (DNS)"), good == 0 ? DiagLevel::Broken : DiagLevel::Limited,
                            QStringLiteral("%1 of %2 do not resolve").arg(bad.size()).arg(good + bad.size()), bad.mid(0, 6).join(QStringLiteral(", ")),
                            QStringLiteral("These names do not resolve: %1. If they work elsewhere, your provider's DNS may block them: use another DNS server (Settings, Internet Connection, or your router).")
                                .arg(bad.join(QStringLiteral(", "))));
    }
    if (n.d.dhtNodes >= 0) {
        const bool replies = n.d.dhtReplies != 0;
        const DiagLevel lv = n.d.dhtNodes > 0 ? DiagLevel::Ok : (n.st.state == ConnState::Connected || m_started.secsTo(QDateTime::currentDateTime()) > 60 ? DiagLevel::Broken : DiagLevel::Unknown);
        QString det = n.d.dhtReplies >= 0 ? QStringLiteral("%1 answers received").arg(n.d.dhtReplies) : QString();
        g.items << item(p + QStringLiteral(".dht"), net == Net::Kad ? QStringLiteral("Kad contacts") : QStringLiteral("DHT"), lv,
                        QStringLiteral("%1").arg(countNoun(n.d.dhtNodes, QStringLiteral("node"))), det,
                        lv == DiagLevel::Broken ? QStringLiteral("No %1 node answers: UDP seems blocked (firewall, VPN, or a router that drops UDP). Magnet links and serverless search need it.")
                                                       .arg(net == Net::Kad ? QStringLiteral("Kad") : QStringLiteral("DHT"))
                                                : replies ? QString() : QStringLiteral("No answers yet."));
    }
    if (n.d.connectAttempts > 0) {
        const int pct = n.d.connectOk * 100 / qMax(1, n.d.connectAttempts);
        g.items << item(p + QStringLiteral(".tcpout"), QStringLiteral("Outgoing connections"), pct >= 5 || n.d.connectAttempts < 20 ? DiagLevel::Ok : DiagLevel::Limited,
                        QStringLiteral("%1 of %2 connected (%3 %)").arg(n.d.connectOk).arg(n.d.connectAttempts).arg(pct), QString(),
                        pct < 5 && n.d.connectAttempts >= 20 ? QStringLiteral("Almost no outgoing connection succeeds: a firewall or security program may block OctoMX.") : QString());
    }
    if (n.d.incomingConnections >= 0)
        g.items << item(p + QStringLiteral(".incoming"), QStringLiteral("Incoming connections"), n.d.incomingConnections > 0 ? DiagLevel::Ok : DiagLevel::Unknown,
                        QString::number(n.d.incomingConnections), net == Net::BitTorrent ? QStringLiteral("last 15 min") : QStringLiteral("this session"));
    if (net == Net::BitTorrent) {
        // one silent tracker or router is normal (they go down); all of them silent means UDP is blocked
        int udpOk = 0;
        for (const Timed &t : m_udpTrackers) if (t.r.ok) ++udpOk;
        for (const Timed &t : m_dhtPings) if (t.r.ok) ++udpOk;
        const bool udpTested = !m_udpTrackers.isEmpty() || !m_dhtPings.isEmpty();
        const DiagLevel silent = udpOk > 0 ? DiagLevel::Unknown : DiagLevel::Broken;
        for (const Timed &t : m_udpTrackers)
            g.items << item(QStringLiteral("%1.udptracker.%2").arg(p, t.what), QStringLiteral("UDP tracker %1").arg(t.what), t.r.ok ? DiagLevel::Ok : silent,
                            t.r.ok ? QStringLiteral("Answers (%1 ms)").arg(t.r.ms) : QStringLiteral("No answer"), t.r.ok ? t.r.detail : t.r.error,
                            t.r.ok ? QString() : udpOk > 0 ? QStringLiteral("This tracker did not answer, the others did: it is probably down.")
                                                           : QStringLiteral("No UDP tracker and no DHT router answered: UDP seems blocked (firewall, VPN or router)."));
        for (const Timed &t : m_dhtPings)
            g.items << item(QStringLiteral("%1.dhtping.%2").arg(p, t.what), QStringLiteral("DHT ping %1").arg(t.what), t.r.ok ? DiagLevel::Ok : silent,
                            t.r.ok ? QStringLiteral("Answers (%1 ms)").arg(t.r.ms) : QStringLiteral("No answer"), t.r.ok ? t.r.detail : t.r.error,
                            t.r.ok ? QString() : udpOk > 0 ? QStringLiteral("This DHT router did not answer, another one did: no problem.") : QString());
        if (udpTested && udpOk == 0)
            g.items << item(p + QStringLiteral(".udp"), QStringLiteral("Outgoing UDP"), DiagLevel::Broken, QStringLiteral("Blocked"),
                            QStringLiteral("no UDP tracker or DHT router answered"),
                            QStringLiteral("Nothing answers over UDP: magnet links (DHT), UDP trackers and uTP peers cannot work. Check the firewall, security program or VPN."));
    }
    for (const Timed &t : m_https) {
        if (t.what.section(QLatin1Char('|'), 0, 0) != netName(net)) continue;
        const QString host = t.what.section(QLatin1Char('|'), 1);
        g.items << item(QStringLiteral("%1.https.%2").arg(p, host), QStringLiteral("Web access %1").arg(host), t.r.ok ? DiagLevel::Ok : DiagLevel::Limited,
                        t.r.ok ? QStringLiteral("Reachable (%1)").arg(t.r.detail) : QStringLiteral("Not reachable"), t.r.ok ? QString() : t.r.error);
    }
    g.items << n.d.items;
    // group level and line
    DiagLevel lv = DiagLevel::Unknown;
    const DiagItem *firstBad = nullptr;
    for (const DiagItem &i : g.items) {
        lv = diagWorse(lv, i.level);
        if (!firstBad && i.level == DiagLevel::Broken) firstBad = &i;
    }
    g.level = lv;
    if (firstBad) g.result = QStringLiteral("%1: %2").arg(firstBad->check, firstBad->result);
    else if (tcp == Reach::No || udp == Reach::No) g.result = QStringLiteral("Works, not reachable from the internet");
    else if (tcp == Reach::Yes) g.result = QStringLiteral("Works, reachable from the internet");
    else if (lv == DiagLevel::Limited) {
        for (const DiagItem &i : g.items)
            if (i.level == DiagLevel::Limited) { g.result = QStringLiteral("%1: %2").arg(i.check, i.result); break; }
    } else if (n.st.state == ConnState::Connected) g.result = hasTcp || hasUdp ? QStringLiteral("Works, reachability unknown") : QStringLiteral("Works");
    else g.result = QStringLiteral("Connecting...");
    // the forwarding advice only while some port is not proven reachable
    const bool allReachable = (!hasTcp || tcp == Reach::Yes) && (!hasUdp || udp == Reach::Yes);
    g.explain = allReachable ? QString() : advice;
    return g;
}

QList<DiagGroup> DiagnosticsManager::downloadGroups() const
{
    QList<DiagGroup> out;
    if (!m_engine) return out;
    const diag::DownloadContext c = downloadContext();
    for (const diag::DownloadFacts &f : m_engine->diagFacts()) {
        if (f.finished) continue;
        DiagGroup g;
        g.id = diagGroupIdForDownload(f.id);
        g.title = f.name.isEmpty() ? f.id : f.name;
        g.downloadId = f.id;
        TorrentDiag t;
        if (m_torrents && !f.torrentId.isEmpty()) t = m_torrents->torrentDiagnostics(f.torrentId);
        const diag::Verdict v = diag::downloadVerdict(f, t, c);
        g.level = f.paused ? DiagLevel::Off : v.level;
        g.result = f.paused ? QStringLiteral("Paused") : !v.sentence.isEmpty() ? v.sentence
                                       : f.rateBps > 0 ? QStringLiteral("Downloading at %1 KB/s").arg(f.rateBps / 1024)
                                                       : QStringLiteral("Nothing unusual");
        const QString dp = QStringLiteral("dl.%1.").arg(f.id);
        if (!v.sentence.isEmpty()) g.items << item(dp + QStringLiteral("diagnosis"), QStringLiteral("Diagnosis"), v.level, v.sentence);
        if (!f.active && !f.finished && !f.paused)
            g.items << item(dp + QStringLiteral("slot"), QStringLiteral("Slot"), DiagLevel::Limited, QStringLiteral("Locally queued"),
                            QStringLiteral("%1 of %2 active").arg(c.activeDownloads).arg(c.maxDownloads));
        if (f.indexSeeds >= 0 || f.indexPeers >= 0)
            g.items << item(dp + QStringLiteral("index"), QStringLiteral("Index numbers"), DiagLevel::Unknown,
                            QStringLiteral("%1, %2").arg(countNoun(qMax(0, f.indexSeeds), QStringLiteral("seed")), countNoun(qMax(0, f.indexPeers), QStringLiteral("peer"))),
                            f.indexName.isEmpty() ? QStringLiteral("reported by the index") : QStringLiteral("reported by %1").arg(f.indexName),
                            QStringLiteral("What the search index said when it listed the torrent. Index numbers can be days or years old."));
        if (t.valid) {
            if (t.queued)
                g.items << item(dp + QStringLiteral("btqueue"), QStringLiteral("BitTorrent queue"), DiagLevel::Limited, QStringLiteral("Queued"),
                                QStringLiteral("Active torrent downloads = %1").arg(c.btActiveLimit));
            g.items << item(dp + QStringLiteral("metadata"), QStringLiteral("Torrent details (metadata)"), t.hasMetadata ? DiagLevel::Ok : DiagLevel::Unknown,
                            t.hasMetadata ? QStringLiteral("Received") : QStringLiteral("Waiting for a peer to send them"));
            QStringList from;
            if (t.fromTrackers) from << QStringLiteral("trackers %1").arg(t.fromTrackers);
            if (t.fromDht) from << QStringLiteral("DHT %1").arg(t.fromDht);
            if (t.fromPex) from << QStringLiteral("peer exchange %1").arg(t.fromPex);
            if (t.fromLsd) from << QStringLiteral("local network %1").arg(t.fromLsd);
            if (t.fromIncoming) from << QStringLiteral("incoming %1").arg(t.fromIncoming);
            if (t.fromOther) from << QStringLiteral("other %1").arg(t.fromOther);
            g.items << item(dp + QStringLiteral("known"), QStringLiteral("Peers known"), t.knownPeers > 0 ? DiagLevel::Ok : DiagLevel::Unknown,
                            QStringLiteral("%1 (%2)").arg(countNoun(qMax(0, t.knownPeers), QStringLiteral("peer")), countNoun(qMax(0, t.knownSeeds), QStringLiteral("seed"))),
                            from.isEmpty() ? QString() : QStringLiteral("from ") + from.join(QStringLiteral(", ")));
            const int failed = t.failedTimeout + t.failedRefused + t.failedHandshake + t.failedBanned + t.failedOther;
            g.items << item(dp + QStringLiteral("connect"), QStringLiteral("Connections"), t.connectedNow > 0 ? DiagLevel::Ok : (t.connectAttempts > 10 ? DiagLevel::Limited : DiagLevel::Unknown),
                            QStringLiteral("%1 connected (%2), %3 attempts").arg(t.connectedNow).arg(countNoun(t.seedsConnected, QStringLiteral("seed"))).arg(t.connectAttempts),
                            failed ? QStringLiteral("failed: %1 timed out, %2 refused, %3 handshake, %4 banned, %5 other")
                                         .arg(t.failedTimeout).arg(t.failedRefused).arg(t.failedHandshake).arg(t.failedBanned).arg(t.failedOther)
                                   : QString());
            if (t.connectedNow > 0)
                g.items << item(dp + QStringLiteral("choke"), QStringLiteral("Peers that choke us"), t.interesting > 0 && t.chokingUs >= t.interesting ? DiagLevel::Limited : DiagLevel::Ok,
                                QStringLiteral("%1 of %2 that have pieces we need").arg(t.chokingUs).arg(t.interesting),
                                t.slowPeers ? QStringLiteral("%1 slow (under 2 KB/s)").arg(t.slowPeers) : QString());
            if (t.dhtEnabled)
                g.items << item(dp + QStringLiteral("dht"), QStringLiteral("DHT lookup"), t.dhtPeers > 0 ? DiagLevel::Ok : (t.dhtLookups > 0 ? DiagLevel::Limited : DiagLevel::Unknown),
                                t.dhtLookups == 0 ? QStringLiteral("No answer yet") : QStringLiteral("%1 returned %2").arg(countNoun(t.dhtLookups, QStringLiteral("lookup")), countNoun(t.dhtPeers, QStringLiteral("peer"))),
                                QStringLiteral("%1 in the routing table").arg(countNoun(qMax(0, t.dhtNodes), QStringLiteral("node"))));
            else
                g.items << item(dp + QStringLiteral("dht"), QStringLiteral("DHT lookup"), DiagLevel::Off, QStringLiteral("DHT switched off"));
            for (const TrackerDiag &tr : t.trackers.mid(0, 10)) {
                DiagLevel lv = DiagLevel::Unknown;
                QString res;
                if (tr.dnsFailed) { lv = DiagLevel::Limited; res = QStringLiteral("Name does not resolve"); }
                else if (tr.working) { lv = DiagLevel::Ok; res = tr.peersReturned >= 0 ? QStringLiteral("Works, %1").arg(countNoun(tr.peersReturned, QStringLiteral("peer"))) : QStringLiteral("Works"); }
                else if (tr.updating) res = QStringLiteral("Announcing...");
                else if (!tr.error.isEmpty() || tr.fails > 0) { lv = DiagLevel::Limited; res = QStringLiteral("Fails"); }
                else res = tr.contacted ? QStringLiteral("No answer") : QStringLiteral("Not contacted yet");
                QString det = tr.error;
                if (tr.seeds >= 0 || tr.leechers >= 0)
                    det = QStringList({det, QStringLiteral("scrape %1, %2").arg(countNoun(qMax(0, tr.seeds), QStringLiteral("seed")), countNoun(qMax(0, tr.leechers), QStringLiteral("peer")))})
                              .filter(QRegularExpression(QStringLiteral("."))).join(QStringLiteral(", "));
                g.items << item(dp + QStringLiteral("tracker.") + tr.host, QStringLiteral("Tracker %1").arg(tr.host), lv, res, det);
            }
        }
        if (f.sourcesKnown > 0 || !t.valid) {
            QStringList nets;
            for (auto it = f.knownByNet.constBegin(); it != f.knownByNet.constEnd(); ++it)
                nets << QStringLiteral("%1 %2").arg(netName(Net(it.key()))).arg(it.value());
            nets.sort();
            g.items << item(dp + QStringLiteral("sources"), QStringLiteral("Other sources"), f.sourcesTransferring > 0 ? DiagLevel::Ok : (f.sourcesKnown > 0 ? DiagLevel::Unknown : DiagLevel::Unknown),
                            QStringLiteral("%1 known, %2 open, %3 queued, %4 sending, %5 failed").arg(f.sourcesKnown).arg(f.sourcesOpen).arg(f.sourcesQueued)
                                .arg(f.sourcesTransferring).arg(f.sourcesFailed),
                            nets.join(QStringLiteral(", ")));
            QStringList reasons;
            for (auto it = f.failReasons.constBegin(); it != f.failReasons.constEnd(); ++it)
                reasons << QStringLiteral("%1 x %2").arg(it.value()).arg(it.key());
            reasons.sort();
            if (!reasons.isEmpty())
                g.items << item(dp + QStringLiteral("failures"), QStringLiteral("Source failures"), DiagLevel::Unknown, QString::number(f.sourcesFailed),
                                reasons.mid(0, 5).join(QStringLiteral("; ")));
        }
        out << g;
    }
    return out;
}

QStringList DiagnosticsManager::recentProblems(int max) const
{
    QString path = logFilePath();
    if (path.isEmpty()) path = m_profile + QStringLiteral("/logs/octomx.log");
    QFile f(path);
    QStringList out;
    if (!f.open(QIODevice::ReadOnly)) return out;
    const qint64 tail = 1024 * 1024;
    if (f.size() > tail) f.seek(f.size() - tail);
    const QList<QByteArray> lines = f.readAll().split('\n');
    static const QRegularExpression re(QStringLiteral("^\\d{4}-\\d\\d-\\d\\d \\d\\d:\\d\\d:\\d\\d\\.\\d+ [WCF] "));
    for (const QByteArray &l : lines) {
        const QString s = QString::fromUtf8(l).trimmed();
        if (re.match(s).hasMatch()) out << s.left(400);
    }
    if (out.size() > max) out = out.mid(out.size() - max);
    return out;
}

DiagReport DiagnosticsManager::report() const
{
    DiagReport r;
    r.generated = QDateTime::currentDateTime();
    r.version = QStringLiteral(MX_VERSION_STRING);
    r.os = QStringLiteral("%1, %2 %3, %4").arg(QSysInfo::prettyProductName(), QSysInfo::kernelType(), QSysInfo::kernelVersion(), QSysInfo::currentCpuArchitecture());
#ifdef Q_OS_WIN
    {
        // running under wine? (ntdll exports wine_get_version)
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll && GetProcAddress(ntdll, "wine_get_version")) r.os += QStringLiteral(" (under wine)");
    }
#endif
    const QList<NetFacts> nets = gatherNets();
    r.groups << generalGroup(nets);
    for (const NetFacts &n : nets) r.groups << networkGroup(n);
    r.groups << downloadGroups();
    const diag::Verdict s = diag::reportSummary(r.groups);
    r.level = s.level;
    r.summary = s.sentence;
    r.logLines = recentProblems(200);
    r.logFile = logFilePath().isEmpty() ? m_profile + QStringLiteral("/logs/octomx.log") : logFilePath();
    r.testRunning = m_testRunning;
    r.testPercent = m_testPercent;
    r.testStep = m_testText;
    r.lastTest = m_lastTest;
    return r;
}

QString DiagnosticsManager::startupBlock() const
{
    QStringList l;
    QStringList ad;
    for (const diag::AdapterInfo &a : m_env.adapters)
        ad << QStringLiteral("%1 %2%3%4").arg(a.humanName, (a.ipv4 + a.ipv6.mid(0, 1)).join(QLatin1Char('/')),
                                              a.isDefault ? QStringLiteral(" (default route)") : QString(),
                                              a.kindNote.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(a.kindNote));
    l << QStringLiteral("connection at start: adapters %1; gateway %2; proxy %3; bind %4")
             .arg(ad.join(QStringLiteral(", ")), m_env.gateway.isNull() ? QStringLiteral("unknown") : m_env.gateway.toString(),
                  m_settings->s(keys::connection::ProxyType), m_settings->s(keys::connection::BindInterface).isEmpty() ? QStringLiteral("all") : m_settings->s(keys::connection::BindInterface));
    QStringList ls;
    for (const NetFacts &n : gatherNets()) {
        if (!n.running) continue;
        QStringList b;
        for (const PortBinding &x : n.d.bindings)
            b << QStringLiteral("%1 %2 %3").arg(x.proto).arg(x.port).arg(x.ok ? QStringLiteral("ok") : QStringLiteral("FAILED (%1)").arg(x.error));
        ls << QStringLiteral("%1: %2").arg(netName(n.plugin->net()), b.isEmpty() ? QStringLiteral("no listening ports") : b.join(QStringLiteral(", ")));
    }
    l << QStringLiteral("connection at start: listen %1").arg(ls.join(QStringLiteral("; ")));
    const diag::RouterInfo r = m_mapper->router();
    QStringList mp;
    for (const PortMapping &m : m_mapper->mappings()) {
        QString s = QStringLiteral("%1 %2").arg(m.proto).arg(m.internalPort);
        switch (m.result) {
        case PortMapping::Mapped: s += QStringLiteral(" -> %1 (%2)").arg(m.externalPort).arg(m.method); break;
        case PortMapping::Failed: s += QStringLiteral(" failed (%1)").arg(m.error); break;
        case PortMapping::Off: s += QStringLiteral(" off"); break;
        default: s += QStringLiteral(" pending"); break;
        }
        mp << s;
    }
    l << QStringLiteral("connection at start: router %1%2, internet address %3; mappings %4")
             .arg(r.method().isEmpty() ? QStringLiteral("not found") : r.method(),
                  r.friendlyName.isEmpty() ? QString() : QStringLiteral(" \"%1\"").arg(r.friendlyName),
                  r.wan().isEmpty() ? QStringLiteral("unknown") : r.wan(), mp.isEmpty() ? QStringLiteral("none") : mp.join(QStringLiteral(", ")));
    return l.join(QLatin1Char('\n'));
}

// ------------------------------------------------------------------------------------ firewall
void DiagnosticsManager::runFirewallCheck()
{
#ifdef Q_OS_WIN
    if (m_firewallRunning) return;
    m_firewallRunning = true;
    const QString exe = QCoreApplication::applicationFilePath();
    auto *w = new QFutureWatcher<diag::FirewallState>(this);
    connect(w, &QFutureWatcher<diag::FirewallState>::finished, this, [this, w] {
        m_firewall = w->result();
        m_firewallRunning = false;
        w->deleteLater();
        qCInfo(lcDiag).noquote() << QStringLiteral("Windows Firewall: %1").arg(
            !m_firewall.checked ? m_firewall.error
                                : QStringLiteral("%1 (%2), %3 allow / %4 block rules for this program%5")
                                      .arg(m_firewall.enabled ? QStringLiteral("on") : QStringLiteral("off"), m_firewall.profiles)
                                      .arg(m_firewall.allowRules).arg(m_firewall.blockRules)
                                      .arg(m_firewall.blockAllInbound ? QStringLiteral(", blocks all incoming") : QString()));
        scheduleReportChanged();
    });
    w->setFuture(QtConcurrent::run([exe] { return diag::checkWindowsFirewall(exe); }));
#endif
}

// ------------------------------------------------------------------------------------ the test
QString DiagnosticsManager::portCheckService() const
{
    const QString t = m_settings->s(keys::diag::PortCheckUrl).trimmed();
    if (t.isEmpty() || !t.contains(QLatin1String("%1"))) return QString();
    return QUrl(QString(t).replace(QLatin1String("%1"), QStringLiteral("1"))).host();
}

void DiagnosticsManager::checkPortsOnline()
{
    if (m_portCheckRunning) return;
    const QString tmpl = m_settings->s(keys::diag::PortCheckUrl).trimmed();
    QList<int> ports;
    QList<PortProbe> probes;
    for (NetworkPlugin *p : m_network->plugins()) {
        if (!m_network->isRunning(p->net())) continue;
        for (const PortBinding &b : p->diagnostics().bindings) {
            if (!b.ok) continue;
            if (b.proto == QLatin1String("TCP") && !ports.contains(b.port)) ports << b.port;
            bool dup = false;
            for (const PortProbe &x : probes) if (x.proto == b.proto && x.port == b.port) dup = true;
            if (!dup && probes.size() < 8) {
                PortProbe pr;
                pr.proto = b.proto;
                pr.port = b.port;
                pr.kind = probeKind(p->net(), b.proto);
                probes << pr;
            }
        }
    }
    // OctoMX peers first (they test UDP as well), if any is connected
    for (NetworkPlugin *p : m_network->plugins()) {
        if (!m_network->isRunning(p->net())) continue;
        const quint32 token = m_probeToken++;
        if (p->probePorts(token, probes)) {
            m_probeAsked = true;
            m_probeTokens.insert(token);
            break;
        }
    }
    if (tmpl.isEmpty() || !tmpl.contains(QLatin1String("%1")) || ports.isEmpty()) {
        scheduleReportChanged();
        return;
    }
    m_portCheckRunning = true;
    const QString service = portCheckService();
    auto left = std::make_shared<int>(int(ports.size()));
    int delay = 0;
    for (int port : ports) {
        // one after the other, a second apart: the checker is someone else's service
        QTimer::singleShot(delay, this, [this, tmpl, port, service, left] {
            diag::publicPortCheck(this, m_nam, tmpl, port, 15000, [this, port, service, left](const CheckResult &r) {
                m_portChecks.insert(port, Timed{r, QDateTime::currentDateTime(), service});
                qCInfo(lcDiag).noquote() << QStringLiteral("port check by %1: TCP %2 %3").arg(service).arg(port)
                                                .arg(r.answered ? (r.ok ? QStringLiteral("open") : QStringLiteral("closed")) : QStringLiteral("no verdict (%1)").arg(r.error));
                if (--*left == 0) m_portCheckRunning = false;
                scheduleReportChanged();
            });
        });
        delay += 1000;
    }
}

void DiagnosticsManager::progress(int pct, const QString &step)
{
    m_testPercent = pct;
    m_testText = step;
    emit testProgress(pct, step);
}

void DiagnosticsManager::startTest()
{
    if (m_testRunning) return;
    m_testRunning = true;
    m_testCancel = false;
    m_testStep = 0;
    m_pending = 0;
    qCInfo(lcDiag) << "connection test started";
    progress(1, QStringLiteral("Checking adapters and ports..."));
    m_stepTimer.start();
    testStep();
}

void DiagnosticsManager::cancelTest()
{
    if (!m_testRunning) return;
    m_testCancel = true;
    finishTest(true);
}

void DiagnosticsManager::finishTest(bool cancelled)
{
    m_stepTimer.stop();
    m_testRunning = false;
    m_lastTest = QDateTime::currentDateTime();
    progress(cancelled ? m_testPercent : 100, cancelled ? QStringLiteral("Cancelled") : QStringLiteral("Done"));
    qCInfo(lcDiag).noquote() << QStringLiteral("connection test %1; summary: %2").arg(cancelled ? QStringLiteral("cancelled") : QStringLiteral("finished"), report().summary);
    emit testFinished(cancelled);
    emit reportChanged();
}

void DiagnosticsManager::testStep()
{
    if (!m_testRunning || m_testCancel) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    auto enter = [this, now](int step, int pct, const QString &text) {
        m_testStep = step;
        m_stepStartMs = now;
        progress(pct, text);
    };
    auto timedOut = [this, now](int ms) { return now - m_stepStartMs >= ms; };
    switch (m_testStep) {
    case 0: {
        m_env = diag::probeNetEnv();
        for (NetworkPlugin *p : m_network->plugins())
            if (m_network->isRunning(p->net())) p->recheckConnectivity();
        syncBindings();
        runFirewallCheck();
        m_mapper->refresh();
        enter(1, 8, QStringLiteral("Asking the router to forward the ports..."));
        break;
    }
    case 1:
        if (!m_mapper->busy() || timedOut(m_stepTimeoutMs + 8000)) {
            // DNS of every host every running network depends on
            QStringList hosts;
            for (const NetFacts &n : gatherNets())
                if (n.running) for (const QString &h : hostsFor(n)) if (!hosts.contains(h)) hosts << h;
            m_dns.clear();
            m_pending = int(hosts.size());
            for (const QString &h : hosts)
                diag::dnsLookup(this, h, 6000, [this, h](const CheckResult &r) {
                    m_dns.insert(h, Timed{r, QDateTime::currentDateTime(), h});
                    if (!r.ok) qCInfo(lcDiag).noquote() << QStringLiteral("DNS: %1 does not resolve (%2)").arg(h, r.error);
                    --m_pending;
                });
            enter(2, 30, QStringLiteral("Looking up %1...").arg(countNoun(hosts.size(), QStringLiteral("host name"))));
        } else {
            progress(qMin(28, m_testPercent + 1), m_testText);
        }
        break;
    case 2:
        if (m_pending <= 0 || timedOut(m_stepTimeoutMs)) {
            m_udpTrackers.clear();
            m_dhtPings.clear();
            m_pending = 0;
            const bool btRunning = m_network->isRunning(Net::BitTorrent);
            if (btRunning) {
                int n = 0;
                for (const QString &u : m_settings->sl(keys::bt::ExtraTrackers)) {
                    QString h;
                    quint16 port = 0;
                    if (!diag::parseUdpTracker(u, &h, &port) || n >= 3) continue;
                    ++n;
                    ++m_pending;
                    const QString what = QStringLiteral("%1:%2").arg(h).arg(port);
                    diag::udpTrackerHandshake(this, h, port, 8000, [this, what](const CheckResult &r) {
                        m_udpTrackers << Timed{r, QDateTime::currentDateTime(), what};
                        --m_pending;
                    });
                }
                n = 0;
                for (const QString &s : m_settings->sl(keys::bt::DhtBootstrapNodes)) {
                    QString h;
                    quint16 port = 0;
                    if (!diag::parseHostPortText(s, &h, &port, 6881) || n >= 2) continue;
                    ++n;
                    ++m_pending;
                    const QString what = QStringLiteral("%1:%2").arg(h).arg(port);
                    diag::dhtPing(this, h, port, 8000, [this, what](const CheckResult &r) {
                        m_dhtPings << Timed{r, QDateTime::currentDateTime(), what};
                        --m_pending;
                    });
                }
            }
            enter(3, 45, QStringLiteral("Testing UDP: tracker handshake and DHT ping..."));
        }
        break;
    case 3:
        if (m_pending <= 0 || timedOut(m_stepTimeoutMs)) {
            m_https.clear();
            m_pending = 0;
            QList<QPair<Net, QString>> urls;
            if (m_network->isRunning(Net::ED2K)) urls << qMakePair(Net::ED2K, m_settings->s(keys::ed2k::ServerListUrl));
            if (m_network->isRunning(Net::Kad)) urls << qMakePair(Net::Kad, m_settings->s(keys::kad::NodesDatUrl));
            if (m_network->isRunning(Net::Gnutella) && !m_settings->sl(keys::gnutella::GWebCaches).isEmpty())
                urls << qMakePair(Net::Gnutella, m_settings->sl(keys::gnutella::GWebCaches).value(0));
            for (NetworkPlugin *p : m_network->plugins())
                if ((p->net() == Net::Web || p->net() == Net::Http) && m_network->isRunning(p->net()))
                    for (const QString &h : p->diagnostics().hosts.mid(0, 4)) urls << qMakePair(p->net(), QStringLiteral("https://%1/").arg(h));
            for (const auto &u : urls) {
                if (u.second.isEmpty()) continue;
                ++m_pending;
                const QString what = netName(u.first) + QLatin1Char('|') + QUrl(u.second).host();
                diag::httpReach(this, m_nam, u.second, 8000, [this, what](const CheckResult &r) {
                    m_https << Timed{r, QDateTime::currentDateTime(), what};
                    --m_pending;
                });
            }
            enter(4, 60, QStringLiteral("Checking web access to servers and indexes..."));
        }
        break;
    case 4:
        if (m_pending <= 0 || timedOut(m_stepTimeoutMs)) {
            // only OctoMX peers here: the public port checker is asked from Check Ports Online
            m_probeAsked = false;
            QList<PortProbe> probes;
            for (NetworkPlugin *p : m_network->plugins())
                if (m_network->isRunning(p->net()))
                    for (const PortBinding &b : p->diagnostics().bindings)
                        if (b.ok && probes.size() < 8) {
                            PortProbe pr;
                            pr.proto = b.proto;
                            pr.port = b.port;
                            pr.kind = probeKind(p->net(), b.proto);
                            bool dup = false;
                            for (const PortProbe &x : probes) if (x.proto == pr.proto && x.port == pr.port) dup = true;
                            if (!dup) probes << pr;
                        }
            for (NetworkPlugin *p : m_network->plugins()) {
                if (!m_network->isRunning(p->net())) continue;
                const quint32 token = m_probeToken++;
                if (p->probePorts(token, probes)) {
                    m_probeAsked = true;
                    m_probeTokens.insert(token);
                    m_pending = 1;
                    break;
                }
            }
            enter(5, 72, m_probeAsked ? QStringLiteral("Asking other OctoMX users to connect back...") : QStringLiteral("Waiting for the networks' own checks..."));
        }
        break;
    case 5:
        if (!m_probeAsked || m_pending <= 0 || timedOut(m_stepTimeoutMs)) enter(6, 80, QStringLiteral("Waiting for the networks' own reachability checks..."));
        break;
    case 6: {
        // passive evidence: done early when every running network with ports has a verdict
        bool allKnown = true;
        for (const NetFacts &n : gatherNets()) {
            if (!n.running) continue;
            bool hasPorts = false;
            for (const PortBinding &b : n.d.bindings) if (b.ok && b.proto == QLatin1String("TCP")) hasPorts = true;
            if (hasPorts && n.d.tcpIn == Reach::Unknown) allKnown = false;
        }
        const qint64 waited = now - m_stepStartMs;
        progress(80 + int(qMin<qint64>(19, waited * 19 / qMax(1, m_passiveWaitMs))), m_testText);
        if (allKnown || waited >= m_passiveWaitMs) finishTest(false);
        break;
    }
    default:
        finishTest(false);
        break;
    }
    if (m_testRunning) scheduleReportChanged();
}

} // namespace mx
