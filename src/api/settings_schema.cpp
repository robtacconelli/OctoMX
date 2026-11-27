// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX: settings catalogue (see mx/settings_keys.h). Owner: core task.
#include "mx/settings_keys.h"

#include <QtCore/QHash>
#include <QtCore/QRegularExpression>

#include <algorithm>
#include <climits>

namespace mx {

// [qa-1] owner request (see settings_keys.h): short label, full label, speed rank
namespace {
struct ConnType { const char *shortLabel; const char *full; int speedRank; };
const ConnType kConnTypes[] = {
    {"Unknown", "Unknown", 0},
    {"DSL", "DSL (up to 24 Mbit)", 1},
    {"VDSL", "VDSL (25-100 Mbit)", 3},
    {"FTTC", "FTTC (30-300 Mbit)", 5},
    {"Cable", "Cable (50-1000 Mbit)", 6},
    {"4G", "4G / LTE (5-150 Mbit)", 2},
    {"5G", "5G mobile (50-1000 Mbit)", 7},
    {"Satellite", "Satellite (25-250 Mbit)", 4},
    {"FTTB", "FTTB (100-1000 Mbit)", 8},
    {"FTTH", "FTTH / Gbit (300-1000 Mbit)", 9},
    {"2.5 Gbit", "2.5 Gbit (2500 Mbit)", 10},
    {"5 Gbit", "5 Gbit (5000 Mbit)", 11},
    {"10 Gbit+", "10 Gbit or faster (10000+ Mbit)", 12},
};
} // namespace

const QStringList &connectionTypes()
{
    static const QStringList l = [] {
        QStringList out;
        for (const ConnType &c : kConnTypes)
            out << QString::fromLatin1(c.shortLabel);
        return out;
    }();
    return l;
}

QString connectionTypeLabel(const QString &type)
{
    for (const ConnType &c : kConnTypes)
        if (type.compare(QLatin1String(c.shortLabel), Qt::CaseInsensitive) == 0)
            return QString::fromLatin1(c.full);
    return type;
}

QString migrateConnectionType(const QString &type)
{
    const QString t = type.trimmed();
    for (const ConnType &c : kConnTypes)
        if (t.compare(QLatin1String(c.shortLabel), Qt::CaseInsensitive) == 0 ||
            t.compare(QLatin1String(c.full), Qt::CaseInsensitive) == 0)
            return QString::fromLatin1(c.shortLabel);
    const QString l = t.toLower();
    if (l.startsWith(QLatin1String("14.4")) || l.startsWith(QLatin1String("28.8")) || l.startsWith(QLatin1String("33.6")) ||
        l.startsWith(QLatin1String("56k")) || l.startsWith(QLatin1String("64k")) || l.startsWith(QLatin1String("128k")) ||
        l == QLatin1String("t1") || l.startsWith(QLatin1String("t1 ")))
        return QStringLiteral("DSL");
    if (l.startsWith(QLatin1String("t3")))
        return QStringLiteral("FTTH");
    return type;
}

bool isConnectionType(const QString &type)
{
    const QString m = migrateConnectionType(type);
    for (const ConnType &c : kConnTypes)
        if (m == QLatin1String(c.shortLabel))
            return true;
    return false;
}

int connectionSpeedRank(const QString &type)
{
    const QString m = migrateConnectionType(type);
    for (const ConnType &c : kConnTypes)
        if (m == QLatin1String(c.shortLabel))
            return c.speedRank;
    return 0;
}

QString connectionTypeForSpeedRank(int rank)
{
    for (const ConnType &c : kConnTypes)
        if (c.speedRank == rank)
            return QString::fromLatin1(c.shortLabel);
    return QStringLiteral("Unknown");
}

QString reportedConnectionType(const QString &own, bool changeWhenBusy, int threshold, const QString &changed, int uploads)
{
    if (changeWhenBusy && threshold > 0 && uploads >= threshold && !changed.trimmed().isEmpty())
        return changed;
    return own;
}

QString settingsPrefixForNet(Net n)
{
    switch (n) {
    case Net::BitTorrent: return QStringLiteral("bt");
    case Net::ED2K:       return QStringLiteral("ed2k");
    case Net::Kad:        return QStringLiteral("kad");
    case Net::Gnutella:   return QStringLiteral("gnutella");
    case Net::Http:       return QStringLiteral("http");
    case Net::Overlay:    return QStringLiteral("overlay");
    case Net::Web:        return QStringLiteral("web");
    case Net::Rats:       return QStringLiteral("rats");
    case Net::Unknown:    break;
    }
    return QString();
}

namespace {

// [ui-f] Settings > Library > "Available Filetypes": the groups of the type table, full lists
QStringList availableFileTypes()
{
    auto row = [](const char *name, const QStringList &exts) {
        return QStringLiteral("%1 (.%2)").arg(QLatin1String(name), exts.join(QStringLiteral(" .")));
    };
    return {row("Audio", extensionsForMediaType(MediaType::Audio)),
            row("Video", extensionsForMediaType(MediaType::Video)),
            row("Pictures", extensionsForMediaType(MediaType::Image)),
            row("Documents", extensionsForMediaType(MediaType::Document)),
            row("Software", extensionsForMediaType(MediaType::Software)),
            row("Archives", extensionsForMediaType(MediaType::Archive)),
            row("Torrents", {QStringLiteral("torrent")}),
            row("Server Lists", {QStringLiteral("met"), QStringLiteral("wsx")}),
            row("Colour Schemes", {QStringLiteral("clr")})};
}

struct Builder {
    QList<SettingDef> list;

    SettingDef &add(QLatin1StringView key, SettingType t, const QVariant &def, const char *label,
                    bool restart = false)
    {
        SettingDef d;
        d.key = QString(key);
        d.type = t;
        d.defaultValue = def;
        d.label = QString::fromUtf8(label);
        d.restartRequired = restart;
        list.append(d);
        return list.last();
    }
    void b(QLatin1StringView k, bool def, const char *label, bool restart = false)
    { add(k, SettingType::Bool, def, label, restart); }
    void i(QLatin1StringView k, qint64 def, qint64 mn, qint64 mx, const char *label, bool restart = false)
    {
        SettingDef &d = add(k, SettingType::Int, def, label, restart);
        d.min = mn;
        d.max = mx;
    }
    void port(QLatin1StringView k, int def, const char *label)
    { i(k, def, 0, 65535, label, true); }
    void d(QLatin1StringView k, double def, double mn, double mx, const char *label)
    {
        SettingDef &s = add(k, SettingType::Double, def, label);
        s.min = mn;
        s.max = mx;
    }
    void s(QLatin1StringView k, const QString &def, const char *label, bool restart = false)
    { add(k, SettingType::String, def, label, restart); }
    void sl(QLatin1StringView k, const QStringList &def, const char *label, bool restart = false)
    { add(k, SettingType::StringList, def, label, restart); }
    void e(QLatin1StringView k, const QString &def, const QStringList &choices, const char *label,
           const QStringList &choiceLabels = {}, bool restart = false)
    {
        SettingDef &s = add(k, SettingType::Enum, def, label, restart);
        s.choices = choices;
        s.choiceLabels = choiceLabels;
    }
    void c(QLatin1StringView k, const char *def, const char *label)
    { add(k, SettingType::Color, QString::fromLatin1(def), label); }
    void p(QLatin1StringView k, const char *label) { add(k, SettingType::Path, QString(), label); }
    void bytes(QLatin1StringView k, const char *label) { add(k, SettingType::Bytes, QByteArray(), label); }
    void map(QLatin1StringView k, const char *label) { add(k, SettingType::Map, QVariantMap(), label); }
    // [engine-3] mark the entry just added as hidden (internal / test switch)
    void hide() { list.last().hidden = true; }
};

void add(Builder &B, QLatin1StringView k, SettingType t, const QVariant &def, const char *label)
{
    B.add(k, t, def, label);
}

QStringList L(std::initializer_list<const char *> items)
{
    QStringList out;
    for (const char *s : items) out << QString::fromUtf8(s);
    return out;
}

QList<SettingDef> buildSchema()
{
    using namespace keys;
    Builder B;
    const QStringList who = L({"everyone", "hotlist", "nobody"});
    const QStringList whoLabels = L({"Everyone", "Only users on my Hot List", "Nobody"});

    // ---- identity
    B.s(identity::ScreenName, QString(), "Screen name");
    B.e(identity::Connection, QStringLiteral("DSL"), connectionTypes(), "Connection type (shown to others)");
    B.b(identity::PrimaryConnection, true, "Make a primary connection to the network");

    // ---- Internet Connection
    B.port(connection::TcpPort, 6699, "Incoming TCP port");
    B.port(connection::UdpPort, 6257, "UDP port"); B.hide();   // [ui-h] nothing reads it: off the UI
    B.b(connection::RandomizePorts, false, "Pick random ports at every start", true);
    B.e(connection::Firewalled, QStringLiteral("auto"), L({"auto", "open", "firewalled"}),
        "Firewall", L({"Detect automatically", "I can accept incoming connections",
                       "I am firewalled (cannot accept incoming connections)"}));
    B.b(connection::Upnp, true, "Map ports with UPnP", true);
    B.b(connection::NatPmp, true, "Map ports with NAT-PMP", true);   // [net-2] no PCP: the label said so
    // [net-2] proxy, adapter and IPv6 apply at once: the networks restart on them (NetworkManager)
    B.b(connection::Ipv6, true, "Use IPv6 when available (BitTorrent, OctoMX network, Rats, HTTP)");
    B.s(connection::BindInterface, QString(), "Network adapter for every network (empty = all)");
    B.i(connection::MaxConnections, 500, 20, 5000, "Maximum connections (all networks)"); B.hide();   // [ui-h] nothing reads it: off the UI
    // [net-2] "socks4" stays a valid stored value (old profiles keep their proxy) but is used as SOCKS5:
    // Qt has no SOCKS4 client; the page offers SOCKS5 and HTTP only
    B.e(connection::ProxyType, QStringLiteral("none"), L({"none", "socks4", "socks5", "http"}), "Proxy type (every network)",
        L({"No proxy", "SOCKS 4 (used as SOCKS 5)", "SOCKS 5", "HTTP"}));
    B.s(connection::ProxyHost, QString(), "Proxy host");
    B.i(connection::ProxyPort, 1080, 0, 65535, "Proxy port");
    B.b(connection::ProxyAuth, false, "Proxy requires authentication");
    B.s(connection::ProxyUser, QString(), "Proxy user name");
    B.s(connection::ProxyPassword, QString(), "Proxy password");
    // [net-2] every peer connection goes through the proxy when one is set (one clear choice: a
    // proxy that hid only the servers showed the address to every peer)
    B.b(connection::ProxyPeers, true, "Use the proxy for peer connections"); B.hide();
    B.b(connection::ProxyHostnames, true, "Resolve host names through the proxy");
    B.i(connection::ConnectTimeoutSec, 25, 5, 300, "Connection attempts timeout after (seconds)");
    B.b(connection::AcceptIncoming, true, "Listen for incoming TCP connections", true);
    B.b(connection::UdpEnabled, true, "Send and receive UDP datagrams", true); B.hide();   // [ui-h] nothing reads it: off the UI
    B.i(connection::DnsTimeoutSec, 30, 5, 300, "DNS queries time out after (seconds)"); B.hide();   // [ui-h] nothing reads it: off the UI
    B.e(connection::DnsMode, QStringLiteral("os"), L({"os", "external"}), "Host name lookup",
        L({"Use operating system to resolve host names", "Use external DNS server to resolve host names"}), true); B.hide();   // [ui-h] nothing reads it: off the UI
    B.s(connection::DnsServer, QString(), "External DNS server address", true); B.hide();   // [ui-h] nothing reads it: off the UI
    B.port(connection::DnsPort, 53, "External DNS server port"); B.hide();   // [ui-h] nothing reads it: off the UI

    // ---- networks page
    B.b(network::ConnectOnStart, true, "Connect to the networks when OctoMX starts");

    // ---- BitTorrent
    B.b(bt::Enabled, true, "Enable BitTorrent");
    B.port(bt::ListenPort, 6881, "BitTorrent listen port (0 = random)");
    B.b(bt::Dht, true, "Enable DHT (trackerless torrents)");
    B.b(bt::Pex, true, "Enable peer exchange (PEX)");
    B.b(bt::Lsd, true, "Enable local peer discovery");
    B.b(bt::Utp, true, "Enable uTP");
    B.e(bt::Encryption, QStringLiteral("enabled"), L({"enabled", "forced", "disabled"}), "Protocol encryption",
        L({"Prefer encryption", "Require encryption", "Disable encryption"}));
    B.i(bt::MaxConnections, 300, 10, 5000, "Maximum BitTorrent connections");
    B.i(bt::MaxConnectionsPerTorrent, 80, 2, 1000, "Maximum connections per torrent");
    B.i(bt::MaxUploads, 16, 1, 500, "BitTorrent upload slots");
    B.i(bt::MaxUploadsPerTorrent, 4, 1, 100, "Upload slots per torrent");
    B.i(bt::MaxActiveDownloads, 5, 1, 100, "Active torrent downloads");
    B.i(bt::MaxActiveSeeds, 10, 0, 500, "Active seeding torrents");
    B.d(bt::SeedRatioLimit, 2.0, 0.0, 100.0, "Stop seeding at ratio (0 = never)");
    B.i(bt::SeedTimeLimitMin, 0, 0, 525600, "Stop seeding after minutes (0 = never)");
    B.e(bt::SeedLimitAction, QStringLiteral("pause"), L({"pause", "remove"}), "When the seed limit is reached",
        L({"Stop seeding", "Remove from the list"}));
    B.b(bt::SeedCompleted, true, "Keep seeding completed torrents");
    B.b(bt::SeedLibrary, false, "Also seed shared library files as torrents");
    B.b(bt::AnnounceToAllTrackers, true, "Announce to all trackers");
    B.sl(bt::ExtraTrackers, L({"udp://tracker.opentrackr.org:1337/announce",
                               "udp://open.stealth.si:80/announce",
                               "udp://tracker.torrent.eu.org:451/announce",
                               "udp://exodus.desync.com:6969/announce"}),
         "Extra trackers added to magnet links");
    B.sl(bt::DhtBootstrapNodes, L({"router.bittorrent.com:6881", "dht.transmissionbt.com:6881",
                                   "router.utorrent.com:6881", "dht.libtorrent.org:25401"}),
         "DHT bootstrap nodes", true);
    B.b(bt::AnonymousMode, false, "Anonymous mode (hide client identity)");
    B.b(bt::SuperSeeding, false, "Super-seeding for new torrents");
    B.b(bt::Preallocate, false, "Pre-allocate disk space");
    B.b(bt::WebSeeds, true, "Use web seeds");
    B.b(bt::AllowMultipleConnectionsPerIp, false, "Allow several connections from the same IP");
    B.b(bt::ValidateHttpsTrackers, true, "Validate HTTPS tracker certificates");

    // ---- eD2k
    B.b(ed2k::Enabled, true, "Enable eD2k (eDonkey servers)");
    B.port(ed2k::TcpPort, 4662, "eD2k TCP port");
    B.port(ed2k::UdpPort, 4672, "eD2k UDP port");
    B.s(ed2k::ServerListUrl, QStringLiteral("http://upd.emule-security.org/server.met"), "Server list URL");
    B.b(ed2k::AutoUpdateServerList, true, "Update the server list at startup");
    B.b(ed2k::AutoConnect, true, "Connect to a server automatically");
    B.b(ed2k::Reconnect, true, "Reconnect when the connection is lost");
    B.b(ed2k::StaticServersOnly, false, "Connect to static servers only");
    B.b(ed2k::AddServersFromServer, true, "Add servers announced by the server");
    B.b(ed2k::AddServersFromClients, false, "Add servers announced by other clients"); B.hide();   // [ui-h] nothing reads it: off the UI
    B.i(ed2k::MaxSourcesPerFile, 400, 10, 5000, "Maximum sources per file");
    B.i(ed2k::MaxConnections, 250, 10, 2000, "Maximum eD2k connections");
    B.b(ed2k::Obfuscation, true, "Enable protocol obfuscation");
    B.b(ed2k::RequireObfuscation, false, "Only allow obfuscated connections");
    B.s(ed2k::UserHash, QString(), "eD2k user hash");
    B.i(ed2k::UploadSlots, 0, 0, 50, "eD2k upload slots (0 = use the global upload limit)");
    B.i(ed2k::MaxHalfOpen, 8, 1, 100, "eD2k connection attempts at once");
    B.i(ed2k::MaxConnectsPer5s, 20, 1, 200, "eD2k new connections per 5 seconds");
    B.i(ed2k::ParallelServerConnects, 2, 1, 4, "Servers tried at the same time");
    B.b(ed2k::UdpPing, true, "Ping servers over UDP");
    B.b(ed2k::UdpSearch, true, "Search other servers over UDP (global search)");
    B.b(ed2k::UdpSources, true, "Ask other servers for sources over UDP");
    B.b(ed2k::CompressUploads, true, "Compress uploads when it helps");
    B.b(ed2k::AllowLan, false, "[test] accept LAN / loopback eD2k peers and servers"); B.hide();

    // ---- Kad
    B.b(kad::Enabled, true, "Enable Kad (serverless eD2k)");
    B.port(kad::UdpPort, 4672, "Kad UDP port (may equal the eD2k UDP port)"); B.hide();   // [net-2] Kad uses the eD2k UDP socket: off the UI
    B.s(kad::NodesDatUrl, QStringLiteral("http://upd.emule-security.org/nodes.dat"), "nodes.dat URL");
    B.sl(kad::BootstrapNodes, QStringList(), "Kad bootstrap nodes (host:port)");
    B.b(kad::AutoBootstrap, true, "Bootstrap automatically from nodes.dat");
    B.b(kad::StoreIndex, true, "Store what other Kad nodes publish to us");
    B.b(kad::FirewallChecks, true, "Run Kad firewall checks");
    B.b(kad::Publish, true, "Publish my shared files on Kad");
    B.b(kad::Buddies, true, "Use Kad buddies (firewalled peers)");
    B.i(kad::MaxPacketsPerSec, 80, 10, 1000, "Kad packets per second (outgoing)");
    // [engine-3] hidden test switches
    B.b(kad::AllowLan, false, "[test] accept LAN Kad contacts"); B.hide();
    B.b(kad::AllowSameIp, false, "[test] accept several Kad contacts per IP"); B.hide();
    B.b(kad::ForceFirewalled, false, "[test] pretend to be TCP-firewalled"); B.hide();
    B.b(kad::ForceUdpFirewalled, false, "[test] pretend to be UDP-firewalled"); B.hide();
    B.i(kad::FindBuddyDelay, 300, 1, 3600, "[test] seconds before looking for a buddy"); B.hide();

    // ---- Gnutella
    B.b(gnutella::Enabled, true, "Enable Gnutella");
    B.port(gnutella::Port, 6346, "Gnutella port");
    B.i(gnutella::MaxUltrapeers, 3, 1, 10, "Ultrapeer connections (leaf mode)");
    // [gnutella] verified live 2026-11-20 (gwc.dietpc.org was dead); the plugin also has them built in
    B.sl(gnutella::GWebCaches, L({"http://cache.jayl.de/g2/gwc.php", "http://skulls.gwc.dyslexicfish.net/skulls.php",
                                  "http://gweb.4octets.co.uk/skulls.php", "http://gweb4.4octets.co.uk/gwc.php",
                                  "http://scissors.gwc.dyslexicfish.net:3709/", "http://bj.ddns.net/skulls/skulls.php",
                                  "http://ghc4.gtkgnutella.com/list4"}),
         "GWebCache URLs");
    B.sl(gnutella::BootstrapHosts, QStringList(), "Gnutella bootstrap hosts (host:port)");
    B.b(gnutella::BrowseHost, true, "Allow browse-host");
    B.i(gnutella::UploadSlots, 0, 0, 100, "Gnutella upload slots (0 = use the global upload limit)");
    B.i(gnutella::UploadQueueSize, 32, 0, 1000, "Gnutella upload queue length");
    B.i(gnutella::UploadQueuePollSec, 30, 1, 40, "Gnutella queue poll interval (seconds)");
    B.b(gnutella::AutoBootstrap, true, "Find Gnutella hosts through web and UDP host caches");
    B.sl(gnutella::UdpHostCaches, QStringList(), "Extra UDP host caches (host:port)");
    // [enc-1] TLS on TCP connections (ultrapeers, transfers, push, browse); UDP stays plain
    B.e(gnutella::Encryption, QStringLiteral("enabled"), L({"enabled", "forced", "disabled"}), "Gnutella encryption (TLS)",
        L({"Prefer encryption", "Require encryption", "Disable encryption"}));

    // ---- HTTP
    B.b(http::Enabled, true, "Enable HTTP/FTP sources");
    B.i(http::MaxConnectionsPerServer, 4, 1, 16, "Connections per web server");
    B.i(http::TimeoutSec, 30, 5, 300, "HTTP timeout (seconds)");
    B.s(http::UserAgent, QStringLiteral("OctoMX/" MX_VERSION_STRING), "HTTP user agent");
    B.b(http::VerifyTls, true, "Verify TLS certificates");
    B.i(http::ConnectionsPerSource, 4, 1, 16, "Parallel requests per web source");
    B.b(http::AllowInsecureRedirect, false, "Allow https to http redirects");
    B.i(http::StallTimeoutSec, 30, 1, 600, "Restart a stalled HTTP request after (seconds)");
    B.i(http::MaxRetries, 5, 0, 100, "HTTP retries per request");
    B.i(http::RetryBaseMs, 1000, 10, 60000, "First HTTP retry delay (ms)");

    // ---- overlay
    B.b(overlay::Enabled, true, "Enable the OctoMX peer network");
    B.sl(overlay::BootstrapPeers, QStringList(), "Overlay bootstrap peers");
    B.i(overlay::MaxPeers, 50, 4, 1000, "Maximum overlay peers");
    B.b(overlay::Relay, true, "Act as a relay for other users");
    B.b(overlay::ShareHashLinks, true, "Share hash links with the network");
    B.b(overlay::Chat, true, "Enable chat"); B.hide();   // [ui-h] nothing reads it: off the UI
    B.e(overlay::PeerCacheRegion, QStringLiteral("europe"),
        L({"northAmerica", "europe", "australia", "southAmerica", "asia", "africa"}), "Preferred peer cache",
        L({"North America", "Europe", "Australia and NZ", "South America", "Asia", "Africa"})); B.hide();   // [ui-h] nothing reads it: off the UI
    B.i(overlay::RoutingBudgetKBs, 7, 1, 1000, "Allocated bandwidth for a primary connection (KB/s out)"); B.hide();   // [ui-h] nothing reads it: off the UI
    B.e(overlay::SearchThreadPriority, QStringLiteral("idle"), L({"idle", "low", "normal"}), "Search thread priority",
        L({"IDLE (Recommended)", "LOW", "NORMAL"})); B.hide();   // [ui-h] nothing reads it: off the UI
    B.i(overlay::SearchDutyCycle, 50, 10, 100, "Search thread duty cycle (%)"); B.hide();   // [ui-h] nothing reads it: off the UI
    B.b(overlay::AllowSlowCpu, false, "Allow primary connection with a slow CPU"); B.hide();   // [ui-h] nothing reads it: off the UI
    B.s(overlay::TestCorruptUploads, QString(), "[test] corrupt served bytes in \"start-end\""); B.hide();

    // ---- web search
    B.b(web::Enabled, true, "Enable web torrent search");
    B.sl(web::Sources, L({"rats", "dht-index"}), "Web search back-ends"); B.hide();   // [ui-h] nothing reads it: off the UI
    B.i(web::MaxResults, 200, 10, 5000, "Maximum web results per search");
    B.b(web::LocalIndex, true, "Keep a local torrent index"); B.hide();   // [ui-h] nothing reads it: off the UI
    B.s(web::UserAgent, QString(), "Web search user agent (empty = built-in)");
    B.map(web::ProviderEnabled, "Enabled web indexes (per provider)");
    B.sl(web::AdultKeywords, QStringList(), "Adult filter keywords (empty = built-in list)");
    B.i(web::TimeoutSec, 25, 3, 300, "Web search timeout (seconds)");
    B.b(web::ProbeOnStart, true, "Check web indexes on start");
    B.sl(web::Trackers, QStringList(), "Trackers added to web search magnets (empty = extra trackers)");

    // ---- rats-search network
    B.b(rats::Enabled, true, "Enable the rats-search torrent network");
    B.s(rats::Protocol, QStringLiteral("rats-search/4"), "rats-search protocol id", true);
    B.port(rats::ListenPort, 4444, "rats-search listen port");
    B.port(rats::DhtPort, 0, "rats-search DHT port (0 = random)");
    B.b(rats::SafeSearch, true, "rats-search safe search");
    B.i(rats::MaxPeers, 30, 1, 1000, "Maximum rats-search peers");
    B.sl(rats::BootstrapPeers, QStringList(), "rats-search bootstrap peers");
    B.i(rats::MaxResults, 500, 10, 5000, "Maximum rats-search results per search");

    // ---- [str-1] the local streaming API
    B.b(api::Enabled, false, "Switch on the local API");
    B.i(api::Port, 17580, 1, 65535, "Local API port");   // applies at once (no restart)
    B.b(api::AllowLan, false, "Allow other devices on my network");
    B.s(api::Token, QString(), "Local API access token");
    B.i(api::CacheLimitMB, 4096, 64, 1024 * 1024, "Stream cache limit (MB)");
    B.i(api::IdleMin, 240, 1, 7 * 1440, "Drop a stream's cache when no player read it for (minutes)");   // 4 hours
    B.b(api::KeepStreams, false, "Keep what was streamed as a download");
    B.b(api::AllowDelete, false, "Programs may delete downloaded files");   // [api-2] off: removal off the list only

    // ---- "Other Networks" (original WinMX OpenNap Networks page)
    B.s(other::DefaultLogin, QString(), "Default login (empty = screen name)");
    B.s(other::DefaultPassword, QString(), "Default password (blank = random)"); B.hide();   // [ui-h] nothing reads it: off the UI
    B.i(other::LoginTimeoutSec, 121, 10, 3600, "Login attempts timeout after (seconds)"); B.hide();   // [ui-h] nothing reads it: off the UI
    B.i(other::MaxConnectAttempts, 10, 1, 100, "Limit simultaneous connection attempts to"); B.hide();   // [ui-h] nothing reads it: off the UI
    B.b(other::NoRawIpImport, true, "Do not import raw IP addresses from server lists"); B.hide();   // [ui-h] nothing reads it: off the UI

    // ---- Incoming Messages & privacy
    B.e(messages::AllowPrivateMessages, QStringLiteral("everyone"), who, "Accept private messages from", whoLabels);
    B.e(messages::AllowBrowse, QStringLiteral("everyone"), who, "Allow browsing my files", whoLabels);
    B.b(messages::AllowWhois, true, "Answer whois requests");
    B.b(messages::AutoReply, false, "Send an automatic reply");
    B.s(messages::AutoReplyText, QStringLiteral("I am away from my computer right now."), "Automatic reply text");
    B.b(messages::OpenWindowOnMessage, true, "Open a window for new private messages");
    B.b(messages::FlashOnMessage, true, "Flash the window on new messages");
    B.b(messages::SoundOnMessage, true, "Play a sound on new messages");
    B.b(messages::BlockFloods, true, "Block message floods");
    B.i(messages::FloodLimit, 6, 2, 100, "Messages per 10 seconds before muting");
    B.b(messages::IgnoreHotlistOnly, false, "Ignore messages from users not on my Hot List");
    B.b(messages::LogPrivate, false, "Log private messages to disk"); B.hide();   // [ui-h] nothing reads it: off the UI
    B.sl(messages::FilterWords, QStringList(), "Filter messages containing");
    B.e(messages::AllowFrom, QStringLiteral("any"), L({"any", "restricted"}), "Incoming message privacy",
        L({"Allow incoming messages from any source", "Only allow messages from the following sources :"}));
    B.b(messages::AllowFromHotlist, true, "Users on your hotlist (both perm and temp)");
    B.b(messages::AllowFromDownloadingFrom, true, "Users you are downloading from");
    B.b(messages::AllowFromUploadingTo, true, "Users you are uploading to");
    B.b(messages::AllowFromSameRoom, true, "Users in the same chat room");
    // [ui-h] "sound" = the short built-in chime (kit/sound.h), played on every platform
    B.e(messages::AlertMode, QStringLiteral("quiet"), L({"quiet", "beep", "sound", "wav"}), "Incoming message alert",
        L({"Quiet", "Use System Beep", "Built-in Sound", "Play a .wav file"}));
    add(B, messages::AlertWav, SettingType::Path, QString(), "Alert sound file");

    // ---- chat
    B.i(chat::BacklogLines, 1000, 50, 20000, "Lines kept per chat room");
    B.b(chat::AutoJoinFavourites, true, "Join favourite rooms on connect");
    B.b(chat::ShowJoinsParts, true, "Show joins and parts");
    B.b(chat::ShowTimestamps, false, "Show timestamps");
    B.s(chat::TimestampFormat, QStringLiteral("hh:mm"), "Timestamp format");
    B.b(chat::LogRooms, false, "Log chat rooms to disk"); B.hide();   // [ui-h] nothing reads it: off the UI
    B.s(chat::FontFamily, QStringLiteral("Arial"), "Chat font");
    B.i(chat::FontSize, 10, 6, 32, "Chat font size");
    B.s(chat::DefaultRoom, QString(), "Default room to join");
    B.b(chat::FetchListImmediately, true, "Fetch channels immediately when list opened");
    B.b(chat::TopicInFilter, false, "Include topic in channel filter");
    B.b(chat::AllowLargeBackscroll, false, "Allow channel output backscroll over 1000 lines");

    // ---- hotlist
    B.b(hotlist::NotifyOnline, true, "Notify when Hot List users come online");
    B.b(hotlist::SoundOnline, false, "Play the message alert when a Hot List user comes online");   // [ui-h]
    B.b(hotlist::AutoBrowse, false, "Browse Hot List users automatically");

    // ---- search
    B.i(search::MaxResults, 2000, 50, 20000, "Maximum results per search");
    B.sl(search::Networks, QStringList(), "Networks to search (empty = all enabled)");
    B.i(search::TimeoutSec, 90, 10, 1800, "Search timeout (seconds)");
    B.b(search::AutoComplete, true, "Auto-complete search text");
    B.b(search::SaveHistory, true, "Remember searches");
    B.i(search::HistorySize, 50, 0, 1000, "Searches remembered");
    B.b(search::AdultFilter, false, "Filter adult content");
    B.b(search::FakeFilter, true, "Hide suspected fake results");
    B.b(search::ShowFullPaths, false, "Show full paths");
    B.b(search::GroupResults, true, "Group identical files"); B.hide();   // [ui-h] nothing reads it: off the UI
    B.b(search::AutoPing, false, "Ping results automatically"); B.hide();   // [ui-h] nothing reads it: off the UI
    B.i(search::MinCandidateSize, 1048576, 0, Q_INT64_C(1099511627776), "Minimum size for name+size matching (bytes)");
    B.b(search::MergeByNameAndSize, true, "Merge files of every network (torrents too) by exact size and name");   // [merge-1]
    B.sl(search::BlockedExtensions, L({"exe", "scr", "pif", "vbs", "lnk"}), "Hide results with extensions");
    // [ui-f] file-type codes (mx::FileTypeFilter): "any" = All Files ([engine-4] was MP3 only)
    B.s(search::DefaultType, QStringLiteral("any"), "Default file type");
    B.s(search::CustomExtensions, QString(), "Custom Extensions of the Search file-type menu");
    B.s(search::DefaultConnection, QStringLiteral("Any Connection"), "Default connection filter");
    B.i(search::AutoFindSourcesMin, 10, 0, 1440, "Auto Find Sources (minutes, 0 = off)");
    B.i(search::AutoEnterQueue, 100, 0, 100000, "Auto Enter Queue (queue rank)");
    B.s(search::TypeButtonAudio, QStringLiteral("audio"), "Audio button");          // [ui-f] codes, all audio
    B.s(search::TypeButtonVideo, QStringLiteral("video"), "Video button");
    B.s(search::TypeButtonPictures, QStringLiteral("pictures"), "Pictures button");
    B.s(search::TypeButtonAudioLabel, QStringLiteral("Audio"), "Audio button caption");
    B.s(search::TypeButtonVideoLabel, QStringLiteral("Video"), "Video button caption");
    B.s(search::TypeButtonPicturesLabel, QStringLiteral("Pictures"), "Pictures button caption");
    B.b(search::GroupsExpanded, false, "Groups of duplicate results expanded by default");
    B.b(search::DropTorrentBridges, true, "Drop eD2k torrent/Kad bridge entries (synthetic hashes)");
    B.b(search::FlagSuspects, true, "Mark likely fake results");
    B.b(search::HideSuspects, false, "Hide results marked as likely fakes");

    // ---- File Transfers
    B.p(transfers::IncomingDir, "Incoming (completed) files directory");
    B.p(transfers::IncompleteDir, "Incomplete files directory");
    B.i(transfers::MaxDownloads, 40, 1, 500, "Max total downloads");
    B.i(transfers::MaxUploads, 3, 0, 500, "Max total uploads");
    B.i(transfers::MaxUploadsPerUser, 4, 1, 100, "Max uploads per user");
    B.i(transfers::MaxSourcesPerDownload, 200, 1, 5000, "Maximum sources per download");
    B.b(transfers::AutoRetry, true, "Automatically retry failed downloads");
    B.i(transfers::RetryIntervalSec, 60, 5, 86400, "Retry interval (seconds)");
    B.i(transfers::RetryMaxAttempts, 0, 0, 10000, "Retry attempts (0 = forever)");
    B.i(transfers::SourceSearchIntervalMin, 10, 1, 1440, "Look for more sources every (minutes)"); B.hide();   // [set-1] nothing reads it: Search > Auto Find Sources (search/autoFindSourcesMin) sets it
    B.i(transfers::StallTimeoutSec, 300, 30, 86400, "Consider a download stalled after (seconds)"); B.hide();   // [ui-h] nothing reads it: off the UI
    B.i(transfers::MetadataTimeoutSec, 90, 1, 86400, "Torrent metadata timeout before using other sources (seconds)");
    B.b(transfers::AutoClearCompleted, false, "Clear complete downloads");
    B.b(transfers::AutoClearUploads, false, "Clear complete uploads");
    B.b(transfers::AutoClearCancelled, true, "Clear cancelled downloads automatically"); B.hide();   // [ui-h] nothing reads it: off the UI
    B.b(transfers::ShareCompleted, true, "Share completed downloads");
    B.b(transfers::Preallocate, false, "Pre-allocate disk space");
    B.b(transfers::StartPaused, false, "Add new downloads paused");
    B.b(transfers::SequentialDefault, false, "Download new files in sequential order");   // [dl-1] qBittorrent: off
    B.b(transfers::FirstLastDefault, false, "Download first and last pieces of new files first");
    B.b(transfers::ConfirmCancel, true, "Confirm before cancelling a download"); B.hide();   // [ui-j] the Remove question took its place
    B.e(transfers::RemoveAction, QStringLiteral("ask"), L({"ask", "keep", "delete"}), "When a download is removed from the list",   // [ui-j]
        L({"Ask what to do with the file", "Keep the file", "Delete the file too"}));
    B.b(transfers::BundleSubfolders, true, "Create a folder for multi-file downloads"); B.hide();   // [ui-h] nothing reads it: off the UI
    B.i(transfers::MinFreeSpaceMB, 500, 0, 1048576, "Pause downloads when free space is below (MB)");
    B.b(transfers::VerifyCompleted, true, "Verify completed files");
    B.b(transfers::UploadsToHotlistOnly, false, "Only upload to users on my Hot List");
    B.i(transfers::MaxDownloadsPerUser, 12, 1, 100, "Max downloads per user"); B.hide();   // [ui-h] nothing reads it: off the UI
    B.b(transfers::PurgeInactiveSources, true, "Purge inactive download sources"); B.hide();   // [ui-h] nothing reads it: off the UI
    B.i(transfers::PurgeInactiveSourcesSec, 60, 5, 86400, "Purge inactive DL sources after (seconds)"); B.hide();   // [ui-h] nothing reads it: off the UI
    B.b(transfers::ClearIncompleteDownloads, false, "Clear incomplete downloads"); B.hide();   // [ui-h] nothing reads it: off the UI
    B.i(transfers::ClearIncompleteDownloadsSec, 600, 5, 864000, "Clear incomplete downloads after (seconds)"); B.hide();   // [ui-h] nothing reads it: off the UI
    B.i(transfers::ClearCompleteDownloadsSec, 300, 0, 864000, "Clear complete downloads after (seconds)");
    B.b(transfers::ClearIncompleteUploads, false, "Clear incomplete uploads"); B.hide();   // [ui-h] nothing reads it: off the UI
    B.i(transfers::ClearIncompleteUploadsSec, 300, 0, 864000, "Clear incomplete uploads after (seconds)"); B.hide();   // [ui-h] nothing reads it: off the UI
    B.i(transfers::ClearCompleteUploadsSec, 300, 0, 864000, "Clear complete uploads after (seconds)");
    B.i(transfers::PendingDownloadTimeoutSec, 300, 10, 86400, "Pending downloads timeout after (seconds)");
    B.i(transfers::RunningDownloadTimeoutSec, 300, 10, 86400, "Running downloads timeout after (seconds)"); B.hide();   // [ui-h] nothing reads it: off the UI
    B.i(transfers::PendingUploadTimeoutSec, 300, 10, 86400, "Pending uploads timeout after (seconds)"); B.hide();   // [ui-h] nothing reads it: off the UI
    B.i(transfers::RunningUploadTimeoutSec, 300, 10, 86400, "Running uploads timeout after (seconds)"); B.hide();   // [ui-h] nothing reads it: off the UI
    B.b(transfers::SwitchToTransfers, false, "Switch to Transfers window when starting downloads");
    // [ui-h] true: the Transfers page always expanded new downloads before the setting was read
    B.b(transfers::NewDownloadsExpanded, false, "New downloads expanded by default");
    B.b(transfers::SuppressDangerousWarning, false, "Suppress dangerous filetype warning (permanent)");

    // ---- Queuing
    B.b(queue::Enabled, true, "Queue requests when all upload slots are busy");
    B.i(queue::MaxLength, 200, 0, 10000, "Maximum queue length");
    B.i(queue::MaxPerUser, 10, 1, 1000, "Maximum queued files per user");
    B.b(queue::HotlistPriority, true, "Hot List users skip the queue");
    B.b(queue::SendPosition, true, "Tell users their queue position");
    B.i(queue::TimeoutMin, 60, 2, 1440, "Drop queued users after (minutes) of inactivity");   // [set-1] 2: a waiting client asks every minute
    B.i(queue::SmallFileBypassKB, 128, 0, 1048576, "Files below this size skip the queue (KB)");
    B.b(queue::ChangeSpeedWhenBusy, false, "Change reported connection speed when busy");
    B.i(queue::ChangeSpeedThreshold, 2, 1, 100, "When there are this many uploads or more");
    B.e(queue::ChangedSpeed, QStringLiteral("DSL"), connectionTypes(), "Report connection speed as");

    // ---- Bandwidth Throttle
    B.i(bandwidth::DownLimitKBs, 0, 0, 10000000, "Download limit (KB/s, 0 = unlimited)");
    B.i(bandwidth::UpLimitKBs, 0, 0, 10000000, "Upload limit (KB/s, 0 = unlimited)");
    B.b(bandwidth::ScheduleEnabled, false, "Use alternative limits on a schedule");
    B.s(bandwidth::ScheduleStart, QStringLiteral("08:00"), "Schedule from (HH:mm)");
    B.s(bandwidth::ScheduleEnd, QStringLiteral("18:00"), "Schedule until (HH:mm)");
    B.i(bandwidth::ScheduleDays, 31, 0, 127, "Schedule days (Mon=1 ... Sun=64)");
    B.i(bandwidth::ScheduleDownKBs, 0, 0, 10000000, "Scheduled download limit (KB/s)");
    B.i(bandwidth::ScheduleUpKBs, 32, 0, 10000000, "Scheduled upload limit (KB/s)");
    B.b(bandwidth::LimitLan, false, "Apply limits to LAN peers");
    // [diag-1] Connection Test
    B.s(diag::PortCheckUrl, QStringLiteral("https://portcheck.transmissionbt.com/%1"),
        "Public port checker URL (%1 = port; only asked when you press the button)");
    B.b(diag::HideAddresses, true, "Hide my IP addresses in the connection report");
    B.b(diag::AnswerProbes, true, "Test other OctoMX users' ports when they ask");
    B.map(diag::IgdCache, "Remembered UPnP router descriptions");
    B.hide();
    B.i(bandwidth::GraphMinutes, 10, 1, 60, "Bandwidth graph span (minutes)"); B.hide();   // [ui-h] nothing reads it: off the UI
    B.i(bandwidth::LastDownLimitKBs, 15, 1, 10000000, "Remembered download limit (KB/s)");
    B.i(bandwidth::LastUpLimitKBs, 24, 1, 10000000, "Remembered upload limit (KB/s)");
    B.e(bandwidth::ThrottleIntervalMs, QStringLiteral("200"), L({"50", "100", "200", "500", "1000"}),
        "Throttle process interval (ms)"); B.hide();   // [ui-h] nothing reads it: off the UI
    B.e(bandwidth::GraphAverageSec, QStringLiteral("4"), L({"1", "2", "4", "8", "16"}), "BW graph rolling average (seconds)");
    // [bw-1] hidden state of the quick speed-limit controls (mx/bandwidth_limits.h)
    B.s(bandwidth::ScheduleOverrideUntil, QString(), "Normal limits win over the schedule until"); B.hide();
    B.i(bandwidth::LimitsOnMask, 3, 0, 3, "Limits switched back on by Speed Limits On"); B.hide();

    // ---- File Launching
    B.b(launch::UseOsDefault, true, "Use the system default programs");
    B.s(launch::AudioCommand, QString(), "Audio player command (%1 = file)");
    B.s(launch::VideoCommand, QString(), "Video player command (%1 = file)");
    B.s(launch::ImageCommand, QString(), "Image viewer command (%1 = file)");
    B.s(launch::DocumentCommand, QString(), "Document viewer command (%1 = file)");
    B.s(launch::OtherCommand, QString(), "Other files command (%1 = file)");
    B.e(launch::DoubleClickAction, QStringLiteral("open"), L({"open", "openFolder", "nothing"}),
        "Double-clicking a completed file", L({"Opens the file", "Opens the containing folder", "Does nothing"}));
    B.b(launch::PreviewIncomplete, true, "Allow previewing incomplete files");
    B.e(launch::PreviewPlayer, QString(), L({"", "system", "program"}), "Preview player",   // [dl-1]
        L({"Ask the first time", "The system's default player", "This program"}));
    B.p(launch::PreviewProgram, "Preview player program");
    B.b(launch::WarnExecutables, true, "Warn before opening programs"); B.hide();   // [ui-h] nothing reads it: off the UI
    {
        auto entry = [](const char *title, const char *action) {
            return QVariantMap{{QStringLiteral("title"), QString::fromLatin1(title)}, {QStringLiteral("ext"), QStringLiteral("*")},
                               {QStringLiteral("action"), QString::fromLatin1(action)}, {QStringLiteral("command"), QString()},
                               {QStringLiteral("params"), QStringLiteral("%path%")}, {QStringLiteral("workdir"), QString()}};
        };
        B.add(launch::Entries, SettingType::Map,
              QVariantMap{{QStringLiteral("entries"),
                           QVariantList{entry("Open", "open"), entry("Play", "play"),
                                        entry("Enqueue", "enqueue"), entry("View", "view")}}},
              "File launching entries");
    }

    // ---- library
    B.i(library::RescanIntervalMin, 60, 0, 10080, "Rescan shared folders every (minutes, 0 = never)");
    B.i(library::HashThreads, 2, 1, 8, "Hashing threads");
    B.b(library::WatchFolders, true, "Watch shared folders for changes");
    B.b(library::ShareHidden, false, "Share hidden files");
    B.sl(library::ExcludePatterns, L({"*.part", "*.tmp", "*.!ut", "*.crdownload", "desktop.ini",
                                      "Thumbs.db", ".DS_Store", "*.lnk"}),
         "Never share files matching");
    B.b(library::ShareIncomplete, true, "Share verified parts of incomplete downloads");
    B.sl(library::FileTypes, availableFileTypes(), "Available file types"); B.hide();   // [ui-f] from the type table; [set-1] nothing reads the value (the Library page shows the table itself)
    B.sl(library::SharedTypes, sharedTypeIds(), "Shared file types");          // [ui-f] audio video pictures ...
    B.s(library::OtherExtensions, QString(), "Other shared file extensions (separate with spaces)"); B.hide();   // [ui-h] nothing reads it: off the UI
    B.b(library::ShareUnlistedTypes, true, "Also share files of types not listed above");

    // ---- Miscellaneous
    B.b(misc::StartWithOs, false, "Start OctoMX when the computer starts");
    B.b(misc::StartMinimized, false, "Start minimized");
    // [post-1] read from old profiles, no effect: the minimise button always minimises to the
    // taskbar, only the close button (misc/closeAction) can keep OctoMX running in the tray
    B.b(misc::MinimizeToTray, true, "Minimize to the system tray (no effect)");
    B.hide();
    B.e(misc::CloseAction, QStringLiteral("ask"), L({"ask", "tray", "exit"}), "When main window closed:",
        L({"Prompt", "Keep running in tray", "Exit"}));
    B.b(misc::ConfirmExit, true, "Confirm before exiting");
    B.b(misc::Sounds, true, "Play sounds");
    B.b(misc::SoundDownloadComplete, true, "Play a sound when a download completes");
    B.b(misc::SingleInstance, true, "Allow only one copy of OctoMX to run"); B.hide();   // [ui-h] nothing reads it: off the UI
    B.b(misc::CheckAssociations, true, "Check file associations at startup");
    B.b(misc::AssociateMagnet, true, "Open magnet: links with OctoMX");
    B.b(misc::AssociateEd2k, true, "Open ed2k:// links with OctoMX");
    B.b(misc::AssociateTorrent, true, "Open .torrent files with OctoMX");
    B.b(misc::TrayBalloons, true, "Show notifications in the system tray");
    B.b(misc::FlashTaskbar, true, "Flash the taskbar button on events");
    B.b(misc::AlwaysOnTop, false, "Always on top");
    B.b(misc::PreventSleep, false, "Prevent sleep while transferring"); B.hide();   // [ui-h] nothing reads it: off the UI
    B.s(misc::Language, QStringLiteral("en"), "Language", true); B.hide();   // [ui-h] nothing reads it: off the UI
    B.e(misc::LogLevel, QStringLiteral("info"), L({"warning", "info", "debug"}), "Log level");
    B.i(misc::LogMaxSizeMB, 5, 1, 1024, "Log file size limit (MB)");
    B.i(misc::LogKeepFiles, 5, 1, 100, "Old log files kept");
    B.e(misc::MainThreadPriority, QStringLiteral("normal"), L({"low", "normal", "high"}), "Main thread priority",
        L({"Low", "Normal (Recommended)", "High"})); B.hide();   // [ui-h] nothing reads it: off the UI
    // [set-1] windows opened later use it at once, the main window only after a restart
    B.add(misc::LanguageFile, SettingType::Path, QString(), "Language file", true);

    // ---- UI state
    B.bytes(ui::Geometry, "Main window geometry");
    B.bytes(ui::WindowState, "Main window state"); B.hide();   // [ui-h] nothing reads it: off the UI
    B.b(ui::Maximized, false, "Main window maximized");
    B.e(ui::LastPage, QStringLiteral("networks"),
        L({"networks", "shared", "chat", "search", "hotlist", "transfers", "bandwidth"}), "Last page");
    B.b(ui::RestoreLastPage, true, "Open the last page at startup");
    B.sl(ui::FloatingPages, QStringList(), "Floating pages");
    B.e(ui::SearchType, QStringLiteral("any"),
        L({"any", "audio", "video", "image", "document", "software", "archive"}), "Search media type");
    B.s(ui::SearchNetworkFilter, QString(), "Search network filter");
    B.s(ui::SearchConnectionFilter, QString(), "Search connection filter");
    B.s(ui::SettingsPage, QString(), "Last settings page");
    B.b(ui::ShowGrid, false, "Show grid lines in lists");
    B.i(ui::FontSize, 0, 0, 32, "List font size (0 = system)"); B.hide();   // [ui-h] nothing reads it: off the UI
    B.s(ui::FontNormal, QStringLiteral("Tahoma,8"), "Normal font");
    B.s(ui::FontDynamic, QStringLiteral("Tahoma,8"), "Dynamic font");
    B.s(ui::FontLarge, QStringLiteral("Tahoma,10"), "Large font");
    add(B, ui::ColorSchemeFile, SettingType::Path, QString(), "Colour scheme file");
    B.b(ui::HotlistVisible, false, "Hot List pane visible");
    B.b(ui::HotlistConfirmRemove, false, "Confirm before removing Hot List users");
    B.sl(ui::ChatOpenRooms, QStringList(), "Chat rooms open at exit");
    B.b(ui::ChatHoldOpenAfterPart, false, "Keep a chat room window open after leaving the room");
    B.b(ui::ChatWarningDisabled, false, "Do not show the chat room warning");
    B.s(ui::LastShareDir, QString(), "Last folder chosen in Share Folder");
    // [ui-e]
    B.e(ui::Scale, QStringLiteral("auto"), L({"auto", "100", "125", "150", "175", "200", "250", "300"}),
        "Interface size", L({"Automatic", "100 %", "125 %", "150 %", "175 %", "200 %", "250 %", "300 %"}), true);
    B.e(ui::SmoothFonts, QStringLiteral("auto"), L({"auto", "on", "off"}), "Smooth font edges",
        L({"Automatic (above 100 %)", "On", "Off"}), true);
    B.b(ui::ExactSizes, false, "Show exact byte counts");
    B.b(ui::NotifyDownloadDone, true, "Notify when a download completes");
    B.b(ui::SearchHistoryMenu, true, "Offer recent searches in the search box");
    B.s(ui::ConnectionDefaults, QString(), "Connection type the transfer slots were set for");
    // [post-1]
    B.e(ui::ColorTheme, QStringLiteral("dark"), L({"dark", "light", "system"}), "Color theme",
        L({"Dark (classic)", "Light", "Follow system"}));   // [ui-h] not hidden: Settings > Appearance shows it
    B.map(QLatin1StringView("ui/columns/*"), "Column widths / order");
    B.bytes(QLatin1StringView("ui/splitters/*"), "Splitter positions");
    B.map(QLatin1StringView("ui/sort/*"), "Sort column");
    B.bytes(QLatin1StringView("ui/dialogs/*"), "Dialog geometry");

    // ---- Colors: the black scheme of the classic WinMX look
    B.c(colors::ListBackground, "#000000", "List background");
    B.c(colors::ListText, "#ffffff", "List text");
    B.c(colors::ListSelectedBackground, "#808080", "Selected row background");
    B.c(colors::ListSelectedText, "#ffffff", "Selected row text");
    B.c(colors::ListGrid, "#404040", "Grid lines");
    B.c(colors::ListHeaderText, "#000000", "Column header text");
    B.c(colors::PartialText, "#ff8040", "Partially downloaded size");
    B.c(colors::BitrateLow, "#ff0000", "Bitrate below 112 kbps");
    B.c(colors::BitrateMedium, "#ffff00", "Bitrate 112-159 kbps");
    B.c(colors::BitrateHigh, "#00ff00", "Bitrate 160 kbps and above");
    B.c(colors::SpeedSlow, "#ff0000", "Slow connection (modem, unknown)");
    B.c(colors::SpeedMedium, "#ffff00", "Medium connection (ISDN)");
    B.c(colors::SpeedFast, "#00ff00", "Fast connection (Cable, DSL, T1, T3)");
    B.c(colors::Available, "#00ff00", "Slots / seeds available");
    B.c(colors::Unavailable, "#ffffff", "No slots available");
    B.c(colors::PingGood, "#00ff00", "Connection Test: OK");
    B.c(colors::PingMedium, "#ffff00", "Connection Test: warning");
    B.c(colors::PingBad, "#ff0000", "Connection Test: problem");
    B.c(colors::MatchIncomplete, "#ffff00", "Matches incomplete local file");
    B.c(colors::CanJoinDownload, "#80ffff", "Can be added to running multi-point download");
    B.c(colors::CurrentlyDownloading, "#004080", "Currently downloading");
    B.c(colors::AlreadyShared, "#808080", "Already in my shared files"); B.hide();   // [ui-h] nothing reads it: off the UI
    B.c(colors::NetworkLabel, "#c0c0c0", "Network name in the source rows of Transfers");
    B.c(colors::SlotOne, "#ff8040", "Exactly one slot free");
    B.c(colors::ChannelName, "#ffffff", "Channel list: channel");
    B.c(colors::ChannelUsers, "#ffffff", "Channel list: users");
    B.c(colors::ChannelTopic, "#ffffff", "Channel list: topic");
    B.c(colors::IgnoredText, "#ffffcc", "Ignored users");
    B.c(colors::HotlistHeader, "#ff8080", "Hot List section headers");
    B.c(colors::HotlistTemporary, "#ffffcc", "Hot List temporary entries");
    B.c(colors::InactiveText, "#808080", "Inactive text");
    B.c(colors::TransferActive, "#00ff00", "Transfer: active");
    B.c(colors::TransferQueued, "#4060ff", "Transfer: queued");
    B.c(colors::TransferComplete, "#0080ff", "Transfer: complete");
    B.c(colors::TransferError, "#ff0000", "Transfer: error");
    B.c(colors::TransferPaused, "#ffffff", "Transfer: paused");
    B.c(colors::TransferConnecting, "#ffff00", "Transfer: finding sources / connecting");
    B.c(colors::TransferNegotiating, "#ffffff", "Transfer: negotiating");
    B.c(colors::TransferRemoteQueued, "#c080ff", "Transfer: remotely queued");
    B.c(colors::TransferBusy, "#ff80ff", "Transfer: busy");
    B.c(colors::TransferCancelled, "#ffffff", "Transfer: cancelled");
    B.c(colors::ProgressBar, "#0080ff", "Progress bar");
    B.c(colors::ProgressBackground, "#000000", "Progress bar background");
    B.c(colors::ProgressActive, "#00ff00", "Progress bar: parts arriving now");   // [ui-i]
    B.c(colors::UserOnline, "#00ff00", "User online");
    B.c(colors::UserOffline, "#ff4040", "User offline");
    B.c(colors::ServerConnected, "#00ff00", "Server: connected");
    B.c(colors::ServerConnecting, "#ffff00", "Server: connecting");
    B.c(colors::ServerDisconnected, "#ff0000", "Server: not connected");
    B.c(colors::ChatBackground, "#000000", "Chat background");
    B.c(colors::ChatText, "#ffffff", "Chat text");
    B.c(colors::ChatOwnName, "#ffff00", "My name");
    B.c(colors::ChatOtherName, "#ffff00", "Other users' names");
    B.c(colors::ChatAction, "#ff00ff", "Actions (/me)");
    B.c(colors::ChatJoin, "#80ff00", "Joins");
    B.c(colors::ChatPart, "#ff8080", "Parts");
    B.c(colors::ChatTopic, "#0000ff", "Topic");
    B.c(colors::ChatMotd, "#80ffff", "Message of the day");
    B.c(colors::ChatSystem, "#ffff00", "System messages");
    B.c(colors::ChatPrivate, "#ff9933", "Private messages"); B.hide();   // [ui-h] nothing reads it: off the UI
    B.c(colors::ChatError, "#ff0000", "Errors");
    B.c(colors::ChatTimestamp, "#808080", "Timestamps");
    B.c(colors::ChatLink, "#0000ff", "Links");
    B.c(colors::ChatInputBackground, "#000000", "Chat input background");
    B.c(colors::ChatInputText, "#ffffff", "Chat input text");
    B.c(colors::ChatUserListBackground, "#000000", "User list background");
    B.c(colors::ChatUserListText, "#ffffff", "User list text");
    B.c(colors::ChatUserAdmin, "#ff0000", "Room administrators (@)");
    B.c(colors::ChatUserVoiced, "#00ff00", "Voiced users (+)");
    B.c(colors::ChatUserListOpenNap, "#000000", "User list: OpenNap Traffic"); B.hide();   // [ui-h] nothing reads it: off the UI
    B.c(colors::ChatUserListOther, "#808080", "User list: Other"); B.hide();   // [ui-h] nothing reads it: off the UI
    B.c(colors::GraphBackground, "#000000", "Graph background");
    B.c(colors::GraphGrid, "#808080", "Graph grid");
    B.c(colors::GraphDown, "#ffffff", "Graph: incoming");
    B.c(colors::GraphUp, "#ffff00", "Graph: outgoing");
    B.c(colors::GraphText, "#ffffff", "Graph text");
    B.c(colors::GraphOverlay, "#ff8000", "Graph: overlay routing");
    B.c(colors::GraphPackets, "#ff00ff", "Graph: DHT/UDP packets");
    B.c(colors::GraphIncomingPeers, "#0080c0", "Graph: incoming peers");
    B.c(colors::GraphOutgoingPeers, "#00ffff", "Graph: outgoing peers");
    B.c(colors::GraphServerIn, "#80ff00", "Graph: server traffic (incoming)");
    B.c(colors::GraphServerOut, "#00ff00", "Graph: server traffic (outgoing)");
    B.c(colors::GraphOther, "#ff0000", "Graph: other");
    B.c(colors::StatusLedOn, "#00ff00", "Connection indicator: on");
    B.c(colors::StatusLedOff, "#000000", "Connection indicator: off");
    B.c(colors::StatusLedConnecting, "#ffff00", "Connection indicator: connecting");
    B.c(colors::StatusLedError, "#ff0000", "Connection indicator: failed");
    B.c(colors::NewsBackground, "#000000", "News bar background");
    B.c(colors::NewsText, "#ffff00", "News bar text");
    B.c(colors::NewsLink, "#ff00ff", "News bar link");
    B.c(colors::SuspectText, "#c08080", "Suspected fake (search)");   // [post-1] was UI-only
    B.e(colors::ThemeApplied, QStringLiteral("dark"), L({"dark", "light"}), "Theme the colours were made for");
    B.hide();

    // ---- [post-1] update check
    B.b(updates::CheckAtStart, true, "Check for a new OctoMX version at start (at most once a day)");
    B.hide();   // [upd-1] replaced by updates/mode (migrated once)
    B.s(updates::Url, QStringLiteral("https://api.github.com/repos/robtacconelli/OctoMX/releases/latest"), "Update check address");
    B.hide();
    B.s(updates::LastCheck, QString(), "Last automatic update check");
    B.hide();
    B.s(updates::SkipVersion, QString(), "Version the user chose to skip");
    B.hide();
    // [upd-1] signed self-update
#ifdef Q_OS_MACOS
    // [mac-1] the macOS version does not install updates by itself (mx/update_install.h): it tells
    // (Settings shows only "notify" and "off"; "auto" / "ask" behave like "notify" there)
    const QString updateModeDefault = QStringLiteral("notify");
#else
    const QString updateModeDefault = QStringLiteral("auto");
#endif
    B.e(updates::Mode, updateModeDefault, L({"auto", "ask", "notify", "off"}), "New versions",
        L({"Install updates automatically", "Ask before downloading", "Only tell me", "Do not check at start"}));
    B.b(updates::ModeMigrated, false, "Update mode migrated from checkAtStart");
    B.hide();
    B.s(updates::LatestSeen, QString(), "Latest release seen by the update check");
    B.hide();

    // ---- internal
    B.b(internal::FirstRunDone, false, "First run done");
    B.b(internal::WizardDone, false, "Setup Wizard completed");
    B.i(internal::TermsAccepted, 0, 0, 1000000, "Terms of use accepted (version)");
    B.s(internal::LastVersion, QString(), "Last version run");
    B.s(internal::InstallId, QString(), "Installation id");

    return B.list;
}

struct Index {
    QList<SettingDef> list;
    QHash<QString, int> exact;
    QList<QPair<QString, int>> wildcards;   // prefix (with trailing '/'), index
    Index()
    {
        list = buildSchema();
        for (int i = 0; i < list.size(); ++i) {
            const QString &k = list.at(i).key;
            if (k.endsWith(QLatin1String("/*")))
                wildcards.append({k.left(k.size() - 1), i});
            else
                exact.insert(k, i);
        }
        std::sort(wildcards.begin(), wildcards.end(),
                  [](const auto &a, const auto &b) { return a.first.size() > b.first.size(); });
    }
};

const Index &index()
{
    static const Index idx;
    return idx;
}

bool toBoolLoose(const QVariant &v, bool *ok)
{
    *ok = true;
    if (v.typeId() == QMetaType::Bool) return v.toBool();
    if (v.typeId() == QMetaType::QString) {
        const QString s = v.toString().trimmed().toLower();
        if (s == QLatin1String("true") || s == QLatin1String("1") || s == QLatin1String("yes") || s == QLatin1String("on"))
            return true;
        if (s == QLatin1String("false") || s == QLatin1String("0") || s == QLatin1String("no") || s == QLatin1String("off") || s.isEmpty())
            return false;
        *ok = false;
        return false;
    }
    if (v.canConvert<double>()) {
        bool k = false;
        const double d = v.toDouble(&k);
        if (k) return d != 0;
    }
    *ok = false;
    return false;
}

} // namespace

// [post-1] the Light theme: white lists, black text, and for every state colour of the dark theme a
// darker counterpart of the same hue with at least 7:1 contrast on white and 4.5:1 on the light
// selection (#d0d0d0), checked by tst_theme. Graph series and lamps are fills, not text.
static const QHash<QString, QString> &lightColorTable()
{
    static const QHash<QString, QString> t = [] {
        const struct { const char *k, *v; } rows[] = {
            {"colors/listBackground", "#ffffff"}, {"colors/listText", "#000000"},
            {"colors/listSelectedBackground", "#d0d0d0"}, {"colors/listSelectedText", "#000000"},
            {"colors/listGrid", "#c0c0c0"}, {"colors/listHeaderText", "#000000"},
            {"colors/partialText", "#8a3800"},
            {"colors/bitrateLow", "#a00000"}, {"colors/bitrateMedium", "#6e4a00"}, {"colors/bitrateHigh", "#006000"},
            {"colors/speedSlow", "#a00000"}, {"colors/speedMedium", "#6e4a00"}, {"colors/speedFast", "#006000"},
            {"colors/available", "#006000"}, {"colors/unavailable", "#000000"},
            {"colors/pingGood", "#006000"}, {"colors/pingMedium", "#6e4a00"}, {"colors/pingBad", "#a00000"},
            {"colors/matchIncomplete", "#6e4a00"}, {"colors/canJoinDownload", "#005a70"},
            {"colors/currentlyDownloading", "#00408a"}, {"colors/alreadyShared", "#595959"},
            {"colors/networkLabel", "#555555"}, {"colors/slotOne", "#8a3800"},
            {"colors/channelName", "#000000"}, {"colors/channelUsers", "#000000"}, {"colors/channelTopic", "#000000"},
            {"colors/ignoredText", "#5c4500"}, {"colors/hotlistHeader", "#953030"}, {"colors/hotlistTemporary", "#5c4500"},
            {"colors/inactiveText", "#595959"},
            {"colors/transferActive", "#006000"}, {"colors/transferQueued", "#1838b0"}, {"colors/transferComplete", "#0050b0"},
            {"colors/transferError", "#a00000"}, {"colors/transferPaused", "#000000"}, {"colors/transferConnecting", "#6e4a00"},
            {"colors/transferNegotiating", "#000000"}, {"colors/transferRemoteQueued", "#6a2aa8"},
            {"colors/transferBusy", "#8c1a8c"}, {"colors/transferCancelled", "#000000"},
            {"colors/progressBar", "#0050b0"}, {"colors/progressBackground", "#ffffff"}, {"colors/progressActive", "#2ea02e"},
            {"colors/userOnline", "#006000"}, {"colors/userOffline", "#a00000"},
            {"colors/serverConnected", "#006000"}, {"colors/serverConnecting", "#6e4a00"}, {"colors/serverDisconnected", "#a00000"},
            {"colors/chatBackground", "#ffffff"}, {"colors/chatText", "#000000"},
            {"colors/chatOwnName", "#6e4a00"}, {"colors/chatOtherName", "#6e4a00"}, {"colors/chatAction", "#900090"},
            {"colors/chatJoin", "#3a6000"}, {"colors/chatPart", "#953030"}, {"colors/chatTopic", "#0000e0"},
            {"colors/chatMotd", "#005a70"}, {"colors/chatSystem", "#6e4a00"}, {"colors/chatPrivate", "#8a4400"},
            {"colors/chatError", "#a00000"}, {"colors/chatTimestamp", "#595959"}, {"colors/chatLink", "#0000e0"},
            {"colors/chatInputBackground", "#ffffff"}, {"colors/chatInputText", "#000000"},
            {"colors/chatUserListBackground", "#ffffff"}, {"colors/chatUserListText", "#000000"},
            {"colors/chatUserAdmin", "#a00000"}, {"colors/chatUserVoiced", "#006000"},
            {"colors/chatUserListOpenNap", "#000000"}, {"colors/chatUserListOther", "#808080"},
            {"colors/graphBackground", "#ffffff"}, {"colors/graphGrid", "#c0c0c0"}, {"colors/graphDown", "#404040"},
            {"colors/graphUp", "#c09000"}, {"colors/graphText", "#000000"}, {"colors/graphOverlay", "#e07000"},
            {"colors/graphPackets", "#c000c0"}, {"colors/graphIncomingPeers", "#0080c0"},
            {"colors/graphOutgoingPeers", "#00a0b0"}, {"colors/graphServerIn", "#60a000"},
            {"colors/graphServerOut", "#00a000"}, {"colors/graphOther", "#e00000"},
            // the Networks status lamp is a little traffic light: the same in both themes
            {"colors/statusLedOn", "#00ff00"}, {"colors/statusLedOff", "#000000"},
            {"colors/statusLedConnecting", "#ffff00"}, {"colors/statusLedError", "#ff0000"},
            {"colors/newsBackground", "#ffffff"}, {"colors/newsText", "#6e4a00"}, {"colors/newsLink", "#900090"},
            {"colors/suspectText", "#7a3a3a"},
        };
        QHash<QString, QString> h;
        for (const auto &r : rows)
            h.insert(QString::fromLatin1(r.k), QString::fromLatin1(r.v));
        return h;
    }();
    return t;
}

QVariantMap colorSchemePreset(bool light)
{
    QVariantMap out;
    for (const SettingDef &d : settingsSchema()) {
        if (d.type != SettingType::Color) continue;
        QString c = d.defaultValue.toString();
        if (light) {
            const auto it = lightColorTable().constFind(d.key);
            if (it != lightColorTable().constEnd()) {
                c = *it;
            } else {
                // a colour added without a light value: invert greys, darken saturated colours (keep hue)
                const int r = c.mid(1, 2).toInt(nullptr, 16), g = c.mid(3, 2).toInt(nullptr, 16), b = c.mid(5, 2).toInt(nullptr, 16);
                int nr, ng, nb;
                if (r == g && g == b) {
                    nr = ng = nb = 255 - r;
                } else {
                    nr = r * 6 / 10;
                    ng = g * 6 / 10;
                    nb = b * 6 / 10;
                }
                c = QStringLiteral("#%1%2%3").arg(nr, 2, 16, QLatin1Char('0')).arg(ng, 2, 16, QLatin1Char('0')).arg(nb, 2, 16, QLatin1Char('0'));
            }
        }
        out.insert(d.key, c);
    }
    return out;
}

bool hasLightColor(const QString &key)
{
    return lightColorTable().contains(key);
}

const QList<SettingDef> &settingsSchema()
{
    return index().list;
}

const SettingDef *findSetting(const QString &key)
{
    const Index &idx = index();
    const auto it = idx.exact.constFind(key);
    if (it != idx.exact.constEnd()) return &idx.list.at(it.value());
    for (const auto &w : idx.wildcards) {
        if (key.startsWith(w.first) && key.size() > w.first.size())
            return &idx.list.at(w.second);
    }
    return nullptr;
}

QVariant settingDefault(const QString &key)
{
    const SettingDef *d = findSetting(key);
    return d ? d->defaultValue : QVariant();
}

QVariant coerceSetting(const SettingDef &def, const QVariant &v, bool *okOut)
{
    bool ok = true;
    QVariant out;
    if (!v.isValid() || v.isNull()) {
        ok = false;
    } else {
        switch (def.type) {
        case SettingType::Bool:
            out = toBoolLoose(v, &ok);
            break;
        case SettingType::Int: {
            qint64 n = 0;
            if (v.typeId() == QMetaType::Bool) {
                n = v.toBool() ? 1 : 0;
            } else if (v.typeId() == QMetaType::Double || v.typeId() == QMetaType::Float) {
                n = qRound64(v.toDouble(&ok));
            } else {
                n = v.toLongLong(&ok);
                if (!ok) { // "12.0"
                    const double d = v.toString().trimmed().toDouble(&ok);
                    n = qRound64(d);
                }
            }
            if (ok) {
                if (def.min.isValid()) n = qMax(n, def.min.toLongLong());
                if (def.max.isValid()) n = qMin(n, def.max.toLongLong());
                if (n >= INT_MIN && n <= INT_MAX) out = int(n);
                else out = n;
            }
            break;
        }
        case SettingType::Double: {
            double d = v.toDouble(&ok);
            if (ok && d != d) ok = false; // NaN
            if (ok) {
                if (def.min.isValid()) d = qMax(d, def.min.toDouble());
                if (def.max.isValid()) d = qMin(d, def.max.toDouble());
                out = d;
            }
            break;
        }
        case SettingType::String:
        case SettingType::Path:
            if (v.typeId() == QMetaType::QVariantMap || v.typeId() == QMetaType::QVariantList) ok = false;
            else out = v.toString();
            break;
        case SettingType::StringList:
            if (v.typeId() == QMetaType::QString) {
                const QString s = v.toString();
                out = s.isEmpty() ? QStringList() : QStringList{s};
            } else if (v.canConvert<QStringList>()) {
                out = v.toStringList();
            } else {
                ok = false;
            }
            break;
        case SettingType::Enum: {
            const QString s = v.toString().trimmed();
            ok = false;
            for (const QString &c : def.choices) {
                if (c.compare(s, Qt::CaseInsensitive) == 0) { out = c; ok = true; break; }
            }
            if (!ok && def.choices == connectionTypes()) {
                // [qa-1] stored values of the original WinMX connection list map onto the new one
                const QString m = migrateConnectionType(s);
                if (def.choices.contains(m)) { out = m; ok = true; }
            }
            if (!ok) { // numeric index accepted too
                bool isNum = false;
                const int idx = s.toInt(&isNum);
                if (isNum && idx >= 0 && idx < def.choices.size()) { out = def.choices.at(idx); ok = true; }
            }
            break;
        }
        case SettingType::Color: {
            QString s = v.toString().trimmed().toLower();
            if (!s.startsWith(QLatin1Char('#'))) s.prepend(QLatin1Char('#'));
            static const QRegularExpression re6(QStringLiteral("^#[0-9a-f]{6}$"));
            static const QRegularExpression re3(QStringLiteral("^#[0-9a-f]{3}$"));
            if (re6.match(s).hasMatch()) {
                out = s;
            } else if (re3.match(s).hasMatch()) {
                out = QStringLiteral("#%1%1%2%2%3%3").arg(s.at(1)).arg(s.at(2)).arg(s.at(3));
            } else {
                ok = false;
            }
            break;
        }
        case SettingType::Bytes:
            if (v.typeId() == QMetaType::QByteArray) out = v.toByteArray();
            else if (v.typeId() == QMetaType::QString) {
                const auto r = QByteArray::fromBase64Encoding(v.toString().toLatin1());
                if (r) out = r.decoded;
                else ok = false;
            } else ok = false;
            break;
        case SettingType::Map:
            if (v.canConvert<QVariantMap>() && (v.typeId() == QMetaType::QVariantMap
                                                || v.typeId() == QMetaType::QVariantHash
                                                || v.typeId() == QMetaType::QJsonObject))
                out = v.toMap();
            else ok = false;
            break;
        }
    }
    if (okOut) *okOut = ok;
    return ok ? out : def.defaultValue;
}

QVariant settingToJson(const SettingDef *def, const QVariant &v)
{
    if ((def && def->type == SettingType::Bytes) || v.typeId() == QMetaType::QByteArray)
        return QString::fromLatin1(v.toByteArray().toBase64());
    return v;
}

QVariant settingFromJson(const SettingDef *def, const QVariant &json)
{
    if (!def) return json;
    bool ok = false;
    QVariant v = coerceSetting(*def, json, &ok);
    return ok ? v : def->defaultValue;
}

} // namespace mx
