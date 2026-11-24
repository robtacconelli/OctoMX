// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX: the catalogue of every setting (keys, types, defaults, ranges).
// Owner: core task. The UI (Settings dialog), the core and every plugin use these constants instead
// of string literals, so a key cannot be mistyped. The values live in <profile>/settings.json and are
// accessed through mx::Settings (mx/services.h). Every key below has exactly one SettingDef in
// mx::settingsSchema() (src/api/settings_schema.cpp) with its type, default, range and label.
//
// Usage:   settings->i(mx::keys::bt::ListenPort)          // QLatin1StringView converts to QString
//          settings->setValue(mx::keys::misc::StartMinimized, true);
//          mx::settingDefault(mx::keys::transfers::MaxDownloads)
//
// Families of dynamic keys (UI state per list) use a prefix + name, e.g.
//          mx::keys::ui::columnsKey("search")  == "ui/columns/search"
// They are matched by wildcard entries ("ui/columns/*") in the schema.
//
// Colours are "#rrggbb" strings. Paths are strings; an empty incoming/incomplete dir means "use the
// computed default" (<Downloads>/OctoMX, or an existing legacy <Downloads>/WinMX, and <profile>/incomplete): mx::Settings::value() returns the
// resolved path for those two keys.
#pragma once

#include <QtCore/QLatin1StringView>
#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVariant>

#include "mx/types.h"

namespace mx {

// ---------------------------------------------------------------- schema
enum class SettingType : int {
    Bool = 0,
    Int,          // qint64 range-checked int (min/max)
    Double,
    String,
    StringList,
    Enum,         // one of `choices` (stored as the choice string)
    Color,        // "#rrggbb"
    Path,         // file or directory path (string)
    Bytes,        // opaque binary (window geometry...), stored base64 in JSON
    Map,          // free-form JSON object (QVariantMap)
};

struct SettingDef {
    QString     key;             // "transfers/maxDownloads"; a trailing "/*" makes a wildcard family
    SettingType type = SettingType::String;
    QVariant    defaultValue;
    QVariant    min;             // Int/Double only (invalid = unbounded)
    QVariant    max;
    QStringList choices;         // Enum only
    QString     label;           // short human label for the Settings dialog
    bool        restartRequired = false;   // takes effect after restart (ports, ...)
    QStringList choiceLabels;    // Enum only, optional: display text per choice (same order)
    bool        hidden = false;  // [engine-3] internal/test switch: valid key, never shown in the Settings dialog
};

// The complete catalogue (static, built once; thread-safe after first call).
const QList<SettingDef> &settingsSchema();
// The definition for a key (exact match first, then the longest matching wildcard family);
// nullptr if the key is unknown.
const SettingDef *findSetting(const QString &key);
// Schema default for a key (invalid QVariant if unknown).
QVariant settingDefault(const QString &key);
// Convert/clamp `v` to the definition's type: strings → numbers/bools, ints clamped to [min,max],
// enums validated (case-insensitive), colours normalised to "#rrggbb", bytes accepted as QByteArray
// or base64 string. Returns the default (and *ok=false) if `v` cannot be converted.
QVariant coerceSetting(const SettingDef &def, const QVariant &v, bool *ok = nullptr);
// JSON (de)serialisation helpers used by the file-backed Settings: Bytes → base64 string, the rest
// unchanged; fromJson reverses it (using the schema type).
QVariant settingToJson(const SettingDef *def, const QVariant &v);
QVariant settingFromJson(const SettingDef *def, const QVariant &json);

// "bt", "ed2k", "kad", "gnutella", "http", "overlay", "web" (settings prefix of a network)
QString settingsPrefixForNet(Net n);

// [qa-1] Connection types, an owner request: modern lines instead of the original WinMX dial-up list.
// The stored / sent value is the short label (identity/connection, queue/changedSpeed); the full
// label with its Mbit range is shown in the Setup Wizard and Settings.
const QStringList &connectionTypes();   // short labels in list order: "Unknown", "DSL", "VDSL", "FTTC",
                                        // "Cable", "4G", "5G", "Satellite", "FTTB", "FTTH", "2.5 Gbit",
                                        // "5 Gbit", "10 Gbit+"
QString connectionTypeLabel(const QString &type);     // "DSL" -> "DSL (up to 24 Mbit)"; else as given
// speed rank for "minimum connection" filters and colours: 0 Unknown < 1 DSL < 2 4G < 3 VDSL <
// 4 Satellite < 5 FTTC < 6 Cable < 7 5G < 8 FTTB < 9 FTTH < 10 2.5 Gbit < 11 5 Gbit < 12 10 Gbit+.
// Full labels and the original WinMX labels are understood; anything else ranks 0 (Unknown).
int connectionSpeedRank(const QString &type);
constexpr int kConnectionSpeedRankMax = 12;
QString connectionTypeForSpeedRank(int rank);         // short label
bool isConnectionType(const QString &type);           // new short/full or original WinMX label (incl. Unknown)
// original WinMX labels -> new short label (14.4K ... 128K ISDN, T1 -> DSL; Cable; DSL; T3 -> FTTH); a full
// label -> its short label; anything else unchanged
QString migrateConnectionType(const QString &type);
// [ui-h] Settings > Queuing "When there are N or more uploads, change reported connection speed
// to ...": the connection type other users see while `uploads` uploads run (else `own`)
QString reportedConnectionType(const QString &own, bool changeWhenBusy, int threshold, const QString &changed, int uploads);

// Colour presets for Settings ▸ Appearance: "Load Standard Dark Background / Light Text Defaults"
// (= the schema defaults) and "... Light Background / Dark Text Defaults". key → "#rrggbb".
QVariantMap colorSchemePreset(bool light);
// [post-1] true when the Light theme has a hand-picked value for this colour key (tests: every
// colour of the schema has one)
bool hasLightColor(const QString &key);

// ---------------------------------------------------------------- keys
namespace keys {
using K = QLatin1StringView;

namespace identity {
inline constexpr K ScreenName{"identity/screenName"};      // empty until first run → "Guest123_45678"
inline constexpr K Connection{"identity/connection"};      // Enum: connectionTypes()
inline constexpr K PrimaryConnection{"identity/primaryConnection"}; // original WinMX "primary / secondary connection"
}

namespace connection {   // "Internet Connection" page (ports used by the OctoMX overlay network)
inline constexpr K TcpPort{"connection/tcpPort"};          // incoming TCP (default 6699 as in the original WinMX)
inline constexpr K UdpPort{"connection/udpPort"};          // UDP (default 6257 as in the original WinMX)
inline constexpr K RandomizePorts{"connection/randomizePorts"};
inline constexpr K Firewalled{"connection/firewalled"};    // Enum: auto | open | firewalled
inline constexpr K Upnp{"connection/upnp"};
inline constexpr K NatPmp{"connection/natpmp"};
inline constexpr K Ipv6{"connection/ipv6"};
inline constexpr K BindInterface{"connection/bindInterface"}; // empty = all interfaces
inline constexpr K MaxConnections{"connection/maxConnections"}; // global half-open/open cap hint
inline constexpr K ProxyType{"connection/proxyType"};      // Enum: none | socks4 | socks5 | http
inline constexpr K ProxyHost{"connection/proxyHost"};
inline constexpr K ProxyPort{"connection/proxyPort"};
inline constexpr K ProxyAuth{"connection/proxyAuth"};
inline constexpr K ProxyUser{"connection/proxyUser"};
inline constexpr K ProxyPassword{"connection/proxyPassword"};
inline constexpr K ProxyPeers{"connection/proxyPeers"};    // also proxy peer connections (not only trackers/servers)
inline constexpr K ProxyHostnames{"connection/proxyHostnames"}; // resolve host names through the proxy
inline constexpr K ConnectTimeoutSec{"connection/connectTimeoutSec"}; // "Connection attempts timeout after"
inline constexpr K AcceptIncoming{"connection/acceptIncoming"};  // false = "Unable to accept incoming TCP connections"
inline constexpr K UdpEnabled{"connection/udpEnabled"};          // false = "Unable to send or receive UDP datagrams"
inline constexpr K DnsTimeoutSec{"connection/dnsTimeoutSec"};
inline constexpr K DnsMode{"connection/dnsMode"};                // Enum: os | external
inline constexpr K DnsServer{"connection/dnsServer"};
inline constexpr K DnsPort{"connection/dnsPort"};
}

namespace network {      // Networks page
inline constexpr K ConnectOnStart{"network/connectOnStart"};
}

namespace bt {
inline constexpr K Enabled{"bt/enabled"};
inline constexpr K ListenPort{"bt/listenPort"};
inline constexpr K Dht{"bt/dht"};
inline constexpr K Pex{"bt/pex"};
inline constexpr K Lsd{"bt/lsd"};
inline constexpr K Utp{"bt/utp"};
inline constexpr K Encryption{"bt/encryption"};            // Enum: enabled | forced | disabled
inline constexpr K MaxConnections{"bt/maxConnections"};
inline constexpr K MaxConnectionsPerTorrent{"bt/maxConnectionsPerTorrent"};
inline constexpr K MaxUploads{"bt/maxUploads"};            // upload slots (global)
inline constexpr K MaxUploadsPerTorrent{"bt/maxUploadsPerTorrent"};
inline constexpr K MaxActiveDownloads{"bt/maxActiveDownloads"};
inline constexpr K MaxActiveSeeds{"bt/maxActiveSeeds"};
inline constexpr K SeedRatioLimit{"bt/seedRatioLimit"};    // Double, 0 = unlimited
inline constexpr K SeedTimeLimitMin{"bt/seedTimeLimitMin"}; // minutes, 0 = unlimited
inline constexpr K SeedLimitAction{"bt/seedLimitAction"};  // Enum: pause | remove
inline constexpr K SeedCompleted{"bt/seedCompleted"};      // keep seeding finished torrents
inline constexpr K SeedLibrary{"bt/seedLibrary"};          // also seed library files as single-file torrents
inline constexpr K AnnounceToAllTrackers{"bt/announceToAllTrackers"};
inline constexpr K ExtraTrackers{"bt/extraTrackers"};      // StringList appended to magnets
inline constexpr K DhtBootstrapNodes{"bt/dhtBootstrapNodes"};
inline constexpr K AnonymousMode{"bt/anonymousMode"};
inline constexpr K SuperSeeding{"bt/superSeeding"};
inline constexpr K Preallocate{"bt/preallocate"};
inline constexpr K WebSeeds{"bt/webSeeds"};
inline constexpr K AllowMultipleConnectionsPerIp{"bt/allowMultipleConnectionsPerIp"};
inline constexpr K ValidateHttpsTrackers{"bt/validateHttpsTrackers"};
}

namespace ed2k {
inline constexpr K Enabled{"ed2k/enabled"};
inline constexpr K TcpPort{"ed2k/tcpPort"};
inline constexpr K UdpPort{"ed2k/udpPort"};
inline constexpr K ServerListUrl{"ed2k/serverListUrl"};
inline constexpr K AutoUpdateServerList{"ed2k/autoUpdateServerList"};
inline constexpr K AutoConnect{"ed2k/autoConnect"};
inline constexpr K Reconnect{"ed2k/reconnect"};
inline constexpr K StaticServersOnly{"ed2k/staticServersOnly"};
inline constexpr K AddServersFromServer{"ed2k/addServersFromServer"};
inline constexpr K AddServersFromClients{"ed2k/addServersFromClients"};
inline constexpr K MaxSourcesPerFile{"ed2k/maxSourcesPerFile"};
inline constexpr K MaxConnections{"ed2k/maxConnections"};
inline constexpr K Obfuscation{"ed2k/obfuscation"};
inline constexpr K RequireObfuscation{"ed2k/requireObfuscation"};
inline constexpr K UserHash{"ed2k/userHash"};              // 32 hex chars, generated on first start by the plugin
// [glue-2] tuning keys the eD2k plugin reads (were read with fallbacks before)
inline constexpr K UploadSlots{"ed2k/uploadSlots"};        // 0 = transfers/maxUploads
inline constexpr K MaxHalfOpen{"ed2k/maxHalfOpen"};        // connection attempts in flight
inline constexpr K MaxConnectsPer5s{"ed2k/maxConnectsPer5s"};
inline constexpr K ParallelServerConnects{"ed2k/parallelServerConnects"};
inline constexpr K UdpPing{"ed2k/udpPing"};                // ping servers over UDP (users/files/ping columns)
inline constexpr K UdpSearch{"ed2k/udpSearch"};            // global UDP search on other servers
inline constexpr K UdpSources{"ed2k/udpSources"};          // ask other servers for sources over UDP
inline constexpr K CompressUploads{"ed2k/compressUploads"}; // zlib-pack upload blocks when it helps
inline constexpr K AllowLan{"ed2k/allowLan"};              // [engine-3] hidden test switch: accept LAN/loopback peers
}

namespace kad {
inline constexpr K Enabled{"kad/enabled"};
inline constexpr K UdpPort{"kad/udpPort"};                 // may equal ed2k/udpPort (shared socket)
inline constexpr K NodesDatUrl{"kad/nodesDatUrl"};
inline constexpr K BootstrapNodes{"kad/bootstrapNodes"};   // "host:port" list
inline constexpr K AutoBootstrap{"kad/autoBootstrap"};
// [glue-2]
inline constexpr K StoreIndex{"kad/storeIndex"};           // keep what others publish to us (bounded index)
inline constexpr K FirewallChecks{"kad/firewallChecks"};   // TCP/UDP firewall checks, external port detection
inline constexpr K Publish{"kad/publish"};                 // publish our shared files
inline constexpr K Buddies{"kad/buddies"};                 // find / be a buddy (firewalled peers)
inline constexpr K MaxPacketsPerSec{"kad/maxPacketsPerSec"}; // outgoing datagram budget (burst 2 s)
// [engine-3] hidden test switches (SettingDef::hidden, valid keys, not shown in the Settings dialog)
inline constexpr K AllowLan{"kad/allowLan"};               // accept LAN contacts (kad/allowLan || ed2k/allowLan)
inline constexpr K AllowSameIp{"kad/allowSameIp"};         // several contacts per IP (same-host tests)
inline constexpr K ForceFirewalled{"kad/forceFirewalled"}; // pretend TCP-firewalled
inline constexpr K ForceUdpFirewalled{"kad/forceUdpFirewalled"};
inline constexpr K FindBuddyDelay{"kad/findBuddyDelay"};   // seconds before looking for a buddy
}

namespace gnutella {
inline constexpr K Enabled{"gnutella/enabled"};
inline constexpr K Port{"gnutella/port"};
inline constexpr K MaxUltrapeers{"gnutella/maxUltrapeers"};
inline constexpr K GWebCaches{"gnutella/gwebcaches"};
inline constexpr K BootstrapHosts{"gnutella/bootstrapHosts"};
inline constexpr K BrowseHost{"gnutella/browseHost"};
// [glue-2]
inline constexpr K UploadSlots{"gnutella/uploadSlots"};    // 0 = transfers/maxUploads
inline constexpr K UploadQueueSize{"gnutella/uploadQueueSize"}; // remote queue length before "Busy"
inline constexpr K UploadQueuePollSec{"gnutella/uploadQueuePollSec"}; // PARQ pollMin we advertise (pollMax = 4x)
inline constexpr K AutoBootstrap{"gnutella/autoBootstrap"}; // contact GWebCaches / UDP host caches
inline constexpr K UdpHostCaches{"gnutella/udpHostCaches"}; // extra UDP host caches (host:port)
inline constexpr K Encryption{"gnutella/encryption"};   // [enc-1] Enum: enabled (prefer TLS) | forced | disabled
}

namespace http {
inline constexpr K Enabled{"http/enabled"};
inline constexpr K MaxConnectionsPerServer{"http/maxConnectionsPerServer"};
inline constexpr K TimeoutSec{"http/timeoutSec"};
inline constexpr K UserAgent{"http/userAgent"};
inline constexpr K VerifyTls{"http/verifyTls"};
inline constexpr K ConnectionsPerSource{"http/connectionsPerSource"};   // parallel ranged requests per URL
inline constexpr K AllowInsecureRedirect{"http/allowInsecureRedirect"}; // allow https → http redirects
inline constexpr K StallTimeoutSec{"http/stallTimeoutSec"};             // no body bytes this long → abort + resume
inline constexpr K MaxRetries{"http/maxRetries"};                       // transient retries per request / probe
inline constexpr K RetryBaseMs{"http/retryBaseMs"};                     // first back-off step
}

namespace overlay {      // our OctoMX peer network (chat, browse, hash links); ports: connection/*
inline constexpr K Enabled{"overlay/enabled"};
inline constexpr K BootstrapPeers{"overlay/bootstrapPeers"};
inline constexpr K MaxPeers{"overlay/maxPeers"};
inline constexpr K Relay{"overlay/relay"};                 // act as a relay / DHT server ("primary")
inline constexpr K ShareHashLinks{"overlay/shareHashLinks"};
inline constexpr K Chat{"overlay/chat"};
inline constexpr K PeerCacheRegion{"overlay/peerCacheRegion"}; // Enum: preferred bootstrap region
inline constexpr K RoutingBudgetKBs{"overlay/routingBudgetKBs"}; // "Allocated Bandwidth" for a primary connection
inline constexpr K SearchThreadPriority{"overlay/searchThreadPriority"}; // Enum: idle | low | normal
inline constexpr K SearchDutyCycle{"overlay/searchDutyCycle"};   // percent
inline constexpr K AllowSlowCpu{"overlay/allowSlowCpu"};
// [engine-3] hidden test hook: "start-end" byte range (file offsets) whose served bytes are flipped
inline constexpr K TestCorruptUploads{"overlay/testCorruptUploads"};
}

namespace web {
inline constexpr K Enabled{"web/enabled"};
inline constexpr K Sources{"web/sources"};                 // StringList of enabled index back-ends
inline constexpr K MaxResults{"web/maxResults"};
inline constexpr K LocalIndex{"web/localIndex"};
inline constexpr K UserAgent{"web/userAgent"};             // empty = built-in desktop browser UA
inline constexpr K ProviderEnabled{"web/providerEnabled"}; // Map provider id → bool
inline constexpr K AdultKeywords{"web/adultKeywords"};     // empty = built-in list
inline constexpr K TimeoutSec{"web/timeoutSec"};
inline constexpr K ProbeOnStart{"web/probeOnStart"};       // check the web indexes at start
inline constexpr K Trackers{"web/trackers"};               // empty = bt/extraTrackers
}

namespace rats {         // rats-search P2P torrent index network (search only)
inline constexpr K Enabled{"rats/enabled"};
inline constexpr K Protocol{"rats/protocol"};             // "rats-search/4" (the network bumps it)
inline constexpr K ListenPort{"rats/listenPort"};          // busy → a free port is used
inline constexpr K DhtPort{"rats/dhtPort"};                // 0 = ephemeral
inline constexpr K SafeSearch{"rats/safeSearch"};
inline constexpr K MaxPeers{"rats/maxPeers"};
inline constexpr K BootstrapPeers{"rats/bootstrapPeers"};
inline constexpr K MaxResults{"rats/maxResults"};
}

namespace api {          // [str-1] the local streaming API (Settings ▸ Other Networks ▸ Streaming API)
inline constexpr K Enabled{"api/enabled"};                 // off by default: nothing listens
inline constexpr K Port{"api/port"};
inline constexpr K AllowLan{"api/allowLan"};               // also the private (LAN) addresses, never public ones
inline constexpr K Token{"api/token"};                     // the access token (made when first switched on)
inline constexpr K CacheLimitMB{"api/cacheLimitMB"};       // all stream caches together
inline constexpr K IdleMin{"api/idleMin"};                 // no player read for this long: the stream and its cache go (4 h)
inline constexpr K KeepStreams{"api/keepStreams"};         // a stream that ends becomes a normal download
}

namespace other {        // Settings ▸ "Other Networks" (original WinMX "OpenNap Networks" page)
inline constexpr K DefaultLogin{"other/defaultLogin"};      // nick for eD2k/Gnutella; empty = screen name
inline constexpr K DefaultPassword{"other/defaultPassword"};
inline constexpr K LoginTimeoutSec{"other/loginTimeoutSec"};
inline constexpr K MaxConnectAttempts{"other/maxConnectAttempts"}; // simultaneous connection attempts
inline constexpr K NoRawIpImport{"other/noRawIpImport"};    // "Do not import raw IP addresses from server lists"
}

namespace messages {     // "Incoming Messages" + privacy
inline constexpr K AllowPrivateMessages{"messages/allowPrivateMessages"}; // Enum: everyone | hotlist | nobody
inline constexpr K AllowBrowse{"messages/allowBrowse"};    // Enum: everyone | hotlist | nobody
inline constexpr K AllowWhois{"messages/allowWhois"};
inline constexpr K AutoReply{"messages/autoReply"};
inline constexpr K AutoReplyText{"messages/autoReplyText"};
inline constexpr K OpenWindowOnMessage{"messages/openWindowOnMessage"};
inline constexpr K FlashOnMessage{"messages/flashOnMessage"};
inline constexpr K SoundOnMessage{"messages/soundOnMessage"};
inline constexpr K BlockFloods{"messages/blockFloods"};
inline constexpr K FloodLimit{"messages/floodLimit"};      // messages per 10 s before the sender is muted
inline constexpr K IgnoreHotlistOnly{"messages/ignoreNonHotlist"};
inline constexpr K LogPrivate{"messages/logPrivate"};
inline constexpr K FilterWords{"messages/filterWords"};
// original WinMX "Incoming Message Privacy": allowFrom = any | restricted; with restricted, the four checks apply.
inline constexpr K AllowFrom{"messages/allowFrom"};
inline constexpr K AllowFromHotlist{"messages/allowFromHotlist"};
inline constexpr K AllowFromDownloadingFrom{"messages/allowFromDownloadingFrom"};
inline constexpr K AllowFromUploadingTo{"messages/allowFromUploadingTo"};
inline constexpr K AllowFromSameRoom{"messages/allowFromSameRoom"};
inline constexpr K AlertMode{"messages/alertMode"};         // Enum: quiet | beep | sound | wav ("Incoming Message Alert")
inline constexpr K AlertWav{"messages/alertWav"};
}

namespace chat {
inline constexpr K BacklogLines{"chat/backlogLines"};      // per-room backlog kept in memory
inline constexpr K AutoJoinFavourites{"chat/autoJoinFavourites"};
inline constexpr K ShowJoinsParts{"chat/showJoinsParts"};
inline constexpr K ShowTimestamps{"chat/showTimestamps"};
inline constexpr K TimestampFormat{"chat/timestampFormat"};
inline constexpr K LogRooms{"chat/logRooms"};
inline constexpr K FontFamily{"chat/fontFamily"};
inline constexpr K FontSize{"chat/fontSize"};
inline constexpr K DefaultRoom{"chat/defaultRoom"};
inline constexpr K FetchListImmediately{"chat/fetchListImmediately"};
inline constexpr K TopicInFilter{"chat/topicInFilter"};
inline constexpr K AllowLargeBackscroll{"chat/allowLargeBackscroll"}; // off: backlog capped at 1000 lines
}

namespace hotlist {
inline constexpr K NotifyOnline{"hotlist/notifyOnline"};
inline constexpr K SoundOnline{"hotlist/soundOnline"};   // [ui-h] message alert sound when a Hot List user comes online
inline constexpr K AutoBrowse{"hotlist/autoBrowse"};
}

namespace search {
inline constexpr K MaxResults{"search/maxResults"};
inline constexpr K Networks{"search/networks"};            // StringList of network short names; empty = all enabled
inline constexpr K TimeoutSec{"search/timeoutSec"};        // a search "finishes" after this at the latest
inline constexpr K AutoComplete{"search/autoComplete"};
inline constexpr K SaveHistory{"search/saveHistory"};
inline constexpr K HistorySize{"search/historySize"};
inline constexpr K AdultFilter{"search/adultFilter"};
inline constexpr K FakeFilter{"search/fakeFilter"};        // hide hits that conflict with known-bad hashes / spam names
inline constexpr K ShowFullPaths{"search/showFullPaths"};
inline constexpr K GroupResults{"search/groupResults"};
inline constexpr K AutoPing{"search/autoPing"};
inline constexpr K MinCandidateSize{"search/minCandidateSize"}; // bytes; size+name candidate merge threshold
// [merge-1] name+size matching across every pair of networks, torrents (one file or an inner file)
// included; false = the strict rule (plain files only, a bundle never meets a non-bundle)
inline constexpr K MergeByNameAndSize{"search/mergeByNameAndSize"};
inline constexpr K BlockedExtensions{"search/blockedExtensions"};
inline constexpr K DefaultType{"search/defaultType"};         // [ui-f] file-type code (mx::FileTypeFilter), default "any"
inline constexpr K CustomExtensions{"search/customExtensions"}; // [ui-f] "mkv srt": the last Custom Extensions...
inline constexpr K DefaultConnection{"search/defaultConnection"}; // "Any Connection"
inline constexpr K AutoFindSourcesMin{"search/autoFindSourcesMin"}; // default AFS for new downloads (0 = off)
inline constexpr K AutoEnterQueue{"search/autoEnterQueue"};   // default AEQ: enter remote queues up to rank n
inline constexpr K TypeButtonAudio{"search/typeButtonAudio"};   // file-type button presets ([ui-f] codes)
inline constexpr K TypeButtonVideo{"search/typeButtonVideo"};
inline constexpr K TypeButtonPictures{"search/typeButtonPictures"};
inline constexpr K TypeButtonAudioLabel{"search/typeButtonAudioLabel"};       // [engine-3] button captions
inline constexpr K TypeButtonVideoLabel{"search/typeButtonVideoLabel"};
inline constexpr K TypeButtonPicturesLabel{"search/typeButtonPicturesLabel"};
inline constexpr K GroupsExpanded{"search/groupsExpanded"};   // duplicate groups expanded by default
// [glue-2] search hygiene (core/search_hygiene.h)
inline constexpr K DropTorrentBridges{"search/dropTorrentBridges"}; // drop eD2k "[torrent ...]"/"[kad ...]" bridge entries
inline constexpr K FlagSuspects{"search/flagSuspects"};   // set SearchHit::suspect on likely fakes
inline constexpr K HideSuspects{"search/hideSuspects"};   // additionally hide flagged hits (off: flag only)
}

namespace transfers {
inline constexpr K IncomingDir{"transfers/incomingDir"};   // empty = <Downloads>/OctoMX (or an existing legacy <Downloads>/WinMX)
inline constexpr K IncompleteDir{"transfers/incompleteDir"}; // empty = <profile>/incomplete
inline constexpr K MaxDownloads{"transfers/maxDownloads"};
inline constexpr K MaxUploads{"transfers/maxUploads"};
inline constexpr K MaxUploadsPerUser{"transfers/maxUploadsPerUser"};
inline constexpr K MaxSourcesPerDownload{"transfers/maxSourcesPerDownload"};
inline constexpr K AutoRetry{"transfers/autoRetry"};
inline constexpr K RetryIntervalSec{"transfers/retryIntervalSec"};
inline constexpr K RetryMaxAttempts{"transfers/retryMaxAttempts"}; // 0 = forever
inline constexpr K SourceSearchIntervalMin{"transfers/sourceSearchIntervalMin"};
inline constexpr K StallTimeoutSec{"transfers/stallTimeoutSec"};
inline constexpr K MetadataTimeoutSec{"transfers/metadataTimeoutSec"}; // [engine] magnet without metadata → part-file fallback / "still looking"
inline constexpr K AutoClearCompleted{"transfers/autoClearCompleted"};
inline constexpr K AutoClearUploads{"transfers/autoClearUploads"};
inline constexpr K AutoClearCancelled{"transfers/autoClearCancelled"};
inline constexpr K ShareCompleted{"transfers/shareCompleted"};
inline constexpr K Preallocate{"transfers/preallocate"};
inline constexpr K StartPaused{"transfers/startPaused"};
inline constexpr K ConfirmCancel{"transfers/confirmCancel"};   // [ui-j] retired: removeAction asks instead (kept hidden)
inline constexpr K RemoveAction{"transfers/removeAction"};     // [ui-j] Enum: ask | keep | delete (a download leaves the list)
inline constexpr K BundleSubfolders{"transfers/bundleSubfolders"};
inline constexpr K MinFreeSpaceMB{"transfers/minFreeSpaceMB"};
inline constexpr K VerifyCompleted{"transfers/verifyCompleted"};
inline constexpr K UploadsToHotlistOnly{"transfers/uploadsToHotlistOnly"};
inline constexpr K MaxDownloadsPerUser{"transfers/maxDownloadsPerUser"};
// original WinMX "File Transfers" page timers (seconds); the *Enabled flags are the check boxes
inline constexpr K PurgeInactiveSources{"transfers/purgeInactiveSources"};
inline constexpr K PurgeInactiveSourcesSec{"transfers/purgeInactiveSourcesSec"};
inline constexpr K ClearIncompleteDownloads{"transfers/clearIncompleteDownloads"};
inline constexpr K ClearIncompleteDownloadsSec{"transfers/clearIncompleteDownloadsSec"};
inline constexpr K ClearCompleteDownloadsSec{"transfers/clearCompleteDownloadsSec"}; // with AutoClearCompleted
inline constexpr K ClearIncompleteUploads{"transfers/clearIncompleteUploads"};
inline constexpr K ClearIncompleteUploadsSec{"transfers/clearIncompleteUploadsSec"};
inline constexpr K ClearCompleteUploadsSec{"transfers/clearCompleteUploadsSec"};     // with AutoClearUploads
inline constexpr K PendingDownloadTimeoutSec{"transfers/pendingDownloadTimeoutSec"};
inline constexpr K RunningDownloadTimeoutSec{"transfers/runningDownloadTimeoutSec"};
inline constexpr K PendingUploadTimeoutSec{"transfers/pendingUploadTimeoutSec"};
inline constexpr K RunningUploadTimeoutSec{"transfers/runningUploadTimeoutSec"};
inline constexpr K SwitchToTransfers{"transfers/switchToTransfers"}; // show Transfers when a download starts
inline constexpr K NewDownloadsExpanded{"transfers/newDownloadsExpanded"};
inline constexpr K SuppressDangerousWarning{"transfers/suppressDangerousWarning"};
// [dl-1] defaults of the two order flags for new downloads (TransferService::setSequentialDownload ...)
inline constexpr K SequentialDefault{"transfers/sequentialDefault"};
inline constexpr K FirstLastDefault{"transfers/firstLastDefault"};
}

namespace queue {
inline constexpr K Enabled{"queue/enabled"};               // queue remote requests when all slots are busy
inline constexpr K MaxLength{"queue/maxLength"};
inline constexpr K MaxPerUser{"queue/maxPerUser"};
inline constexpr K HotlistPriority{"queue/hotlistPriority"};
inline constexpr K SendPosition{"queue/sendPosition"};
inline constexpr K TimeoutMin{"queue/timeoutMin"};
inline constexpr K SmallFileBypassKB{"queue/smallFileBypassKB"}; // files below this skip the queue (0 = off)
inline constexpr K ChangeSpeedWhenBusy{"queue/changeSpeedWhenBusy"}; // "When there are [n] or more uploads, change
inline constexpr K ChangeSpeedThreshold{"queue/changeSpeedThreshold"}; //  reported connection speed to [x]"
inline constexpr K ChangedSpeed{"queue/changedSpeed"};       // Enum: connectionTypes()
}

namespace bandwidth {
inline constexpr K DownLimitKBs{"bandwidth/downLimitKBs"}; // KiB/s, 0 = unlimited
inline constexpr K UpLimitKBs{"bandwidth/upLimitKBs"};
inline constexpr K ScheduleEnabled{"bandwidth/scheduleEnabled"};
inline constexpr K ScheduleStart{"bandwidth/scheduleStart"}; // "HH:mm"
inline constexpr K ScheduleEnd{"bandwidth/scheduleEnd"};
inline constexpr K ScheduleDays{"bandwidth/scheduleDays"};  // bitmask Mon=1 ... Sun=64
inline constexpr K ScheduleDownKBs{"bandwidth/scheduleDownKBs"};
inline constexpr K ScheduleUpKBs{"bandwidth/scheduleUpKBs"};
inline constexpr K LimitLan{"bandwidth/limitLan"};
inline constexpr K GraphMinutes{"bandwidth/graphMinutes"};
inline constexpr K LastDownLimitKBs{"bandwidth/lastDownLimitKBs"}; // UI memory: value shown while the limit is unchecked
inline constexpr K LastUpLimitKBs{"bandwidth/lastUpLimitKBs"};
inline constexpr K ThrottleIntervalMs{"bandwidth/throttleIntervalMs"};
inline constexpr K GraphAverageSec{"bandwidth/graphAverageSec"}; // "BW Graph Rolling Avg. Duration"
// [bw-1] hidden: normal limits changed during a schedule window win until then (ISO date time,
// "" = none), see mx/bandwidth_limits.h; which limits "Speed Limits On" switches back on (1 = down, 2 = up)
inline constexpr K ScheduleOverrideUntil{"bandwidth/scheduleOverrideUntil"};
inline constexpr K LimitsOnMask{"bandwidth/limitsOnMask"};
}

namespace diag {         // [diag-1] Connection Test
// public TCP port checker, "%1" = the port; asked ONLY when the user presses the button
inline constexpr K PortCheckUrl{"diag/portCheckUrl"};
inline constexpr K HideAddresses{"diag/hideAddresses"};    // "Hide my IP addresses in the report"
inline constexpr K AnswerProbes{"diag/answerProbes"};      // test other OctoMX users' ports when they ask
inline constexpr K IgdCache{"diag/igdCache"};              // hidden: gateway address -> last working UPnP description / control URL
}

namespace launch {       // "File Launching"
inline constexpr K UseOsDefault{"launch/useOsDefault"};
inline constexpr K AudioCommand{"launch/audioCommand"};    // "%1" = file path; empty = OS default
inline constexpr K VideoCommand{"launch/videoCommand"};
inline constexpr K ImageCommand{"launch/imageCommand"};
inline constexpr K DocumentCommand{"launch/documentCommand"};
inline constexpr K OtherCommand{"launch/otherCommand"};
inline constexpr K DoubleClickAction{"launch/doubleClickAction"}; // Enum: open | openFolder | nothing
inline constexpr K PreviewIncomplete{"launch/previewIncomplete"};
inline constexpr K WarnExecutables{"launch/warnExecutables"};
inline constexpr K Entries{"launch/entries"};              // Map {"entries": [{title, ext, action, command, params, workdir}]}
// [dl-1] Preview of incomplete audio / video files: "" = ask the first time ("Preview With"), "system" =
// the system default player, "program" = launch/previewProgram (a player executable; the file is its argument)
inline constexpr K PreviewPlayer{"launch/previewPlayer"};
inline constexpr K PreviewProgram{"launch/previewProgram"};
}

namespace library {
inline constexpr K RescanIntervalMin{"library/rescanIntervalMin"}; // 0 = never (watcher still active)
inline constexpr K HashThreads{"library/hashThreads"};
inline constexpr K WatchFolders{"library/watchFolders"};
inline constexpr K ShareHidden{"library/shareHidden"};
inline constexpr K ExcludePatterns{"library/excludePatterns"}; // wildcard patterns on file names
inline constexpr K ShareIncomplete{"library/shareIncomplete"}; // serve verified parts of running downloads
inline constexpr K FileTypes{"library/fileTypes"};          // "Available Filetypes": "Audio (.mp3 .ogg)", ...
// [glue-3] Shared File Types dialog (filter applied by the core Library)
inline constexpr K SharedTypes{"library/sharedTypes"};      // StringList of group ids (mx::sharedTypeIds())
inline constexpr K OtherExtensions{"library/otherExtensions"}; // String: "flac epub" (space/comma separated)
inline constexpr K ShareUnlistedTypes{"library/shareUnlistedTypes"}; // share files whose extension is in no group
}

namespace misc {
inline constexpr K StartWithOs{"misc/startWithOs"};
inline constexpr K StartMinimized{"misc/startMinimized"};
inline constexpr K MinimizeToTray{"misc/minimizeToTray"};   // [post-1] read, no effect (hidden)
inline constexpr K CloseAction{"misc/closeAction"};        // Enum: ask | tray | exit
inline constexpr K ConfirmExit{"misc/confirmExit"};
inline constexpr K Sounds{"misc/sounds"};
inline constexpr K SoundDownloadComplete{"misc/soundDownloadComplete"};
inline constexpr K SingleInstance{"misc/singleInstance"};
inline constexpr K CheckAssociations{"misc/checkAssociations"};
inline constexpr K AssociateMagnet{"misc/associateMagnet"};
inline constexpr K AssociateEd2k{"misc/associateEd2k"};
inline constexpr K AssociateTorrent{"misc/associateTorrent"};
inline constexpr K TrayBalloons{"misc/trayBalloons"};
inline constexpr K FlashTaskbar{"misc/flashTaskbar"};
inline constexpr K AlwaysOnTop{"misc/alwaysOnTop"};
inline constexpr K PreventSleep{"misc/preventSleep"};
inline constexpr K Language{"misc/language"};
inline constexpr K LogLevel{"misc/logLevel"};              // Enum: warning | info | debug
inline constexpr K LogMaxSizeMB{"misc/logMaxSizeMB"};
inline constexpr K LogKeepFiles{"misc/logKeepFiles"};
inline constexpr K MainThreadPriority{"misc/mainThreadPriority"}; // Enum: low | normal | high
inline constexpr K LanguageFile{"misc/languageFile"};      // empty = English
}

namespace ui {
inline constexpr K Geometry{"ui/geometry"};                // Bytes (QWidget::saveGeometry)
inline constexpr K WindowState{"ui/windowState"};          // Bytes
inline constexpr K Maximized{"ui/maximized"};
inline constexpr K LastPage{"ui/lastPage"};                // Enum: networks | shared | chat | search | hotlist | transfers | bandwidth
inline constexpr K RestoreLastPage{"ui/restoreLastPage"};
inline constexpr K FloatingPages{"ui/floatingPages"};      // StringList of pages floated in own windows
inline constexpr K SearchType{"ui/searchType"};            // last media type tab
inline constexpr K SearchNetworkFilter{"ui/searchNetworkFilter"};
inline constexpr K SearchConnectionFilter{"ui/searchConnectionFilter"};
inline constexpr K SettingsPage{"ui/settingsPage"};        // last page of the Settings dialog
inline constexpr K ShowGrid{"ui/showGrid"};
inline constexpr K FontSize{"ui/fontSize"};
inline constexpr K FontNormal{"ui/fontNormal"};            // "Tahoma,8" (QFont::toString or family,size)
inline constexpr K FontDynamic{"ui/fontDynamic"};
inline constexpr K FontLarge{"ui/fontLarge"};
inline constexpr K ColorSchemeFile{"ui/colorSchemeFile"};
// [glue-2] keys the UI foundation (ui-a) already uses. [ui-d] the four that mirrored a canonical key
// (ui/searchFileType, ui/searchAutoFindSources, ui/searchAutoEnterQueue, ui/suppressDangerWarning)
// are gone; the UI migrates stored values at start (ui/kit/session.cpp).
inline constexpr K HotlistVisible{"ui/hotlistVisible"};    // Hotlist side pane shown
inline constexpr K HotlistConfirmRemove{"ui/hotlistConfirmRemove"};
// [glue-3] keys of the Chat / Shared Files pages (ui-b)
inline constexpr K ChatOpenRooms{"ui/chatOpenRooms"};      // StringList: rooms open at exit (rejoined)
inline constexpr K ChatHoldOpenAfterPart{"ui/chatHoldOpenAfterPart"};
inline constexpr K ChatWarningDisabled{"ui/chatWarningDisabled"};
inline constexpr K LastShareDir{"ui/lastShareDir"};
// [ui-e] interface size and modern conveniences
inline constexpr K Scale{"ui/scale"};                      // Enum: auto | 100 | 125 | 150 | 175 | 200 | 250 | 300
inline constexpr K SmoothFonts{"ui/smoothFonts"};          // Enum: auto (on above 100 %) | on | off
inline constexpr K ExactSizes{"ui/exactSizes"};            // sizes as exact byte counts (classic) instead of KB/MB/GB
inline constexpr K NotifyDownloadDone{"ui/notifyDownloadDone"}; // tray notification when a download completes
inline constexpr K SearchHistoryMenu{"ui/searchHistoryMenu"};   // recent searches drop-down on the search box
inline constexpr K ConnectionDefaults{"ui/connectionDefaults"}; // connection type the slot / queue values were derived for
// [post-1] colour theme of the lists, chat views and graphs (the gray chrome is the same in both)
inline constexpr K ColorTheme{"ui/colorTheme"};            // Enum: dark (classic) | light | system (follow the OS)
inline constexpr K ColumnsPrefix{"ui/columns/"};          // + list name: Map {widths:[..], order:[..], hidden:[..]}
inline constexpr K SplittersPrefix{"ui/splitters/"};       // + splitter name: Bytes (QSplitter::saveState)
inline constexpr K SortPrefix{"ui/sort/"};                 // + list name: Map {column:int, order:int}
inline constexpr K DialogsPrefix{"ui/dialogs/"};           // + dialog name: Bytes (geometry)
inline QString columnsKey(const QString &list) { return QString(ColumnsPrefix) + list; }
inline QString splitterKey(const QString &name) { return QString(SplittersPrefix) + name; }
inline QString sortKey(const QString &list) { return QString(SortPrefix) + list; }
inline QString dialogKey(const QString &name) { return QString(DialogsPrefix) + name; }
}

namespace colors {
// lists (search, transfers, shared files, hotlist, networks)
inline constexpr K ListBackground{"colors/listBackground"};
inline constexpr K ListText{"colors/listText"};
inline constexpr K ListSelectedBackground{"colors/listSelectedBackground"};
inline constexpr K ListSelectedText{"colors/listSelectedText"};
inline constexpr K ListGrid{"colors/listGrid"};
inline constexpr K ListHeaderText{"colors/listHeaderText"};
inline constexpr K PartialText{"colors/partialText"};      // "25% of 109 878 998" in the Bytes column
inline constexpr K BitrateLow{"colors/bitrateLow"};        // < 128 kbps
inline constexpr K BitrateMedium{"colors/bitrateMedium"};  // 128 kbps
inline constexpr K BitrateHigh{"colors/bitrateHigh"};      // > 128 kbps
inline constexpr K SpeedSlow{"colors/speedSlow"};          // 56K and below, Unknown
inline constexpr K SpeedMedium{"colors/speedMedium"};      // ISDN
inline constexpr K SpeedFast{"colors/speedFast"};          // Cable, DSL, T1, T3
inline constexpr K Available{"colors/available"};          // "x of y available" with free slots / seeds
inline constexpr K Unavailable{"colors/unavailable"};
inline constexpr K PingGood{"colors/pingGood"};
inline constexpr K PingMedium{"colors/pingMedium"};
inline constexpr K PingBad{"colors/pingBad"};
inline constexpr K MatchIncomplete{"colors/matchIncomplete"};   // legend: yellow
inline constexpr K CanJoinDownload{"colors/canJoinDownload"};   // legend: cyan
inline constexpr K CurrentlyDownloading{"colors/currentlyDownloading"}; // legend: dark blue
inline constexpr K AlreadyShared{"colors/alreadyShared"};
inline constexpr K NetworkLabel{"colors/networkLabel"};    // network tag in source rows
inline constexpr K SlotOne{"colors/slotOne"};              // "1 of 4 available" (exactly one free slot)
inline constexpr K ChannelName{"colors/channelName"};      // Channel List window
inline constexpr K ChannelUsers{"colors/channelUsers"};
inline constexpr K ChannelTopic{"colors/channelTopic"};
inline constexpr K IgnoredText{"colors/ignoredText"};      // Ignored Users window entries
inline constexpr K HotlistHeader{"colors/hotlistHeader"};  // section titles + rule lines
inline constexpr K HotlistTemporary{"colors/hotlistTemporary"};
inline constexpr K InactiveText{"colors/inactiveText"};
// transfers
inline constexpr K TransferActive{"colors/transferActive"};
inline constexpr K TransferQueued{"colors/transferQueued"};
inline constexpr K TransferComplete{"colors/transferComplete"};
inline constexpr K TransferError{"colors/transferError"};
inline constexpr K TransferPaused{"colors/transferPaused"};
inline constexpr K TransferConnecting{"colors/transferConnecting"}; // Finding sources / Waiting / Connecting
inline constexpr K TransferNegotiating{"colors/transferNegotiating"};
inline constexpr K TransferRemoteQueued{"colors/transferRemoteQueued"};
inline constexpr K TransferBusy{"colors/transferBusy"};
inline constexpr K TransferCancelled{"colors/transferCancelled"};
inline constexpr K ProgressBar{"colors/progressBar"};
inline constexpr K ProgressBackground{"colors/progressBackground"};
inline constexpr K ProgressActive{"colors/progressActive"};   // [ui-i] parts of a download arriving now
// hotlist / networks
inline constexpr K UserOnline{"colors/userOnline"};
inline constexpr K UserOffline{"colors/userOffline"};
inline constexpr K ServerConnected{"colors/serverConnected"};
inline constexpr K ServerConnecting{"colors/serverConnecting"};
inline constexpr K ServerDisconnected{"colors/serverDisconnected"};
// chat
inline constexpr K ChatBackground{"colors/chatBackground"};
inline constexpr K ChatText{"colors/chatText"};
inline constexpr K ChatOwnName{"colors/chatOwnName"};
inline constexpr K ChatOtherName{"colors/chatOtherName"};
inline constexpr K ChatAction{"colors/chatAction"};
inline constexpr K ChatJoin{"colors/chatJoin"};
inline constexpr K ChatPart{"colors/chatPart"};
inline constexpr K ChatTopic{"colors/chatTopic"};
inline constexpr K ChatMotd{"colors/chatMotd"};
inline constexpr K ChatSystem{"colors/chatSystem"};
inline constexpr K ChatPrivate{"colors/chatPrivate"};
inline constexpr K ChatError{"colors/chatError"};
inline constexpr K ChatTimestamp{"colors/chatTimestamp"};
inline constexpr K ChatLink{"colors/chatLink"};
inline constexpr K ChatInputBackground{"colors/chatInputBackground"};
inline constexpr K ChatInputText{"colors/chatInputText"};
inline constexpr K ChatUserListBackground{"colors/chatUserListBackground"};
inline constexpr K ChatUserListText{"colors/chatUserListText"};
inline constexpr K ChatUserAdmin{"colors/chatUserAdmin"};
inline constexpr K ChatUserVoiced{"colors/chatUserVoiced"};
inline constexpr K ChatUserListOpenNap{"colors/chatUserListOpenNap"}; // original WinMX "OpenNap Traffic" / "Other" buttons
inline constexpr K ChatUserListOther{"colors/chatUserListOther"};
// bandwidth graph
inline constexpr K GraphBackground{"colors/graphBackground"};
inline constexpr K GraphGrid{"colors/graphGrid"};
inline constexpr K GraphDown{"colors/graphDown"};
inline constexpr K GraphUp{"colors/graphUp"};
inline constexpr K GraphText{"colors/graphText"};
// per-graph series (Incoming / Outgoing graph, UI_SPEC §2.5): GraphBackground/Text/Grid above are
// the "Background"/"Labels"/"Lines" of both graphs; GraphDown/GraphUp are the "Downloads"/"Uploads" series
inline constexpr K GraphOverlay{"colors/graphOverlay"};         // "WPN Primary" → overlay routing
inline constexpr K GraphPackets{"colors/graphPackets"};         // "WPN Packets" → DHT/UDP packets
inline constexpr K GraphIncomingPeers{"colors/graphIncomingPeers"};
inline constexpr K GraphOutgoingPeers{"colors/graphOutgoingPeers"};
inline constexpr K GraphServerIn{"colors/graphServerIn"};       // "OpenNap Traffic" (incoming graph)
inline constexpr K GraphServerOut{"colors/graphServerOut"};     // "OpenNap Traffic" (outgoing graph)
inline constexpr K GraphOther{"colors/graphOther"};
// status box on the Networks page
inline constexpr K StatusLedOn{"colors/statusLedOn"};
inline constexpr K StatusLedOff{"colors/statusLedOff"};
inline constexpr K StatusLedConnecting{"colors/statusLedConnecting"};
inline constexpr K StatusLedError{"colors/statusLedError"};
// news bar (top-left strip) [glue-2: used by the UI kit]
inline constexpr K NewsBackground{"colors/newsBackground"};
inline constexpr K NewsText{"colors/newsText"};
inline constexpr K NewsLink{"colors/newsLink"};
// [post-1] (was UI-only) search hits flagged as suspected fakes
inline constexpr K SuspectText{"colors/suspectText"};
// [post-1] the theme the stored colours were last made for (dark | light, hidden). Under colors/ so
// that a reset of the colours resets it too; when it differs from the chosen theme, every colour
// still at this theme's default is replaced by the chosen theme's default (customised ones stay).
inline constexpr K ThemeApplied{"colors/themeApplied"};
}

// [post-1] Help ▸ Check for Updates... and the automatic check at start (at most once a day)
namespace updates {
inline constexpr K CheckAtStart{"updates/checkAtStart"};
inline constexpr K Url{"updates/url"};                     // hidden: the releases/latest endpoint (tests use a local server)
inline constexpr K LastCheck{"updates/lastCheck"};         // hidden: time of the last automatic check (ISO 8601, UTC)
inline constexpr K SkipVersion{"updates/skipVersion"};     // hidden: "Skip This Version" (e.g. "4.2.0")
// [upd-1] what a newer signed release does: auto (download quietly, ask to restart), ask (offer
// Update Now), notify (tell, open the download page), off (no check at start). CheckAtStart above
// is kept for older profiles: off there means mode off (migrated once, ModeMigrated).
inline constexpr K Mode{"updates/mode"};
inline constexpr K ModeMigrated{"updates/modeMigrated"};   // hidden
inline constexpr K LatestSeen{"updates/latestSeen"};       // hidden: latest release version of the last check (About box)
}

namespace internal {     // bookkeeping, not shown in the Settings dialog
inline constexpr K FirstRunDone{"internal/firstRunDone"};
inline constexpr K LastVersion{"internal/lastVersion"};
inline constexpr K InstallId{"internal/installId"};        // random hex, generated once
inline constexpr K WizardDone{"internal/wizardDone"};      // Setup Wizard completed
inline constexpr K TermsAccepted{"internal/termsAccepted"}; // version of the terms of use the user accepted (0 = none)
}

// "<prefix>/enabled" for a network (bt/enabled, ed2k/enabled, ...); empty for Net::Unknown.
inline QString enabledKey(Net n)
{
    const QString p = settingsPrefixForNet(n);
    return p.isEmpty() ? QString() : p + QLatin1String("/enabled");
}

} // namespace keys
} // namespace mx
