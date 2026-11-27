// SPDX-License-Identifier: GPL-3.0-or-later
// [ui-h] The settings registry: for every setting the user can see (Settings pages and the Advanced
// page: every schema entry that is not hidden) the proof that it does something. tst_settings_registry
// fails for a visible key without an entry, for an entry of a key that is hidden or gone, and when the
// named file no longer contains the named test function / reader reference.
//   test     file = the test source, what = the test function that changes the key and checks the effect
//   reader   file = the source that reads the key and acts on it, what = the reference to the key there
//            (the static smoke check of the long tail: the reader must stay; see docs/status/ui-h.md)
//   internal state the program keeps for itself (window layout, ids); file / what = its reader
#pragma once

namespace mxreg {

struct Proof {
    const char *key;
    const char *kind;   // "test" | "reader" | "internal"
    const char *file;   // relative to the source tree
    const char *what;   // test function / reference in the file
    const char *why;    // internal: what it is
};

inline const Proof kProofs[] = {
    {"identity/screenName", "test", "tests/overlay/tst_set1_overlay.cpp", "othersSeeTheConnectionTypeAtOnce", ""},
    {"identity/connection", "test", "tests/overlay/tst_set1_overlay.cpp", "othersSeeTheConnectionTypeAtOnce", ""},
    {"identity/primaryConnection", "test", "tests/ui/tst_set1.cpp", "primaryConnection", ""},
    {"connection/tcpPort", "test", "tests/netpolicy/tst_netpolicy_netns.cpp", "overlayBoundWhenTheRouteIsTheAdapters", ""},   // [net-2]
    {"connection/randomizePorts", "test", "tests/netpolicy/tst_netpolicy_bt.cpp", "randomPortsAndTorrentLimits", ""},   // [net-2]
    {"connection/firewalled", "test", "tests/netpolicy/tst_netsettings.cpp", "gnutellaPortUltrapeersAndCaches", ""},   // [net-2]
    {"connection/upnp", "test", "tests/netpolicy/tst_netsettings.cpp", "btSessionSettingsReachLibtorrent", ""},   // [net-2]
    {"connection/natpmp", "test", "tests/netpolicy/tst_netsettings.cpp", "btSessionSettingsReachLibtorrent", ""},   // [net-2]
    {"connection/ipv6", "test", "tests/netpolicy/tst_netsettings.cpp", "btSessionSettingsReachLibtorrent", ""},   // [net-2]
    {"connection/bindInterface", "test", "tests/netpolicy/tst_netpolicy.cpp", "ed2kOnAdapter", ""},   // [net-2]
    {"connection/proxyType", "test", "tests/netpolicy/tst_netpolicy.cpp", "ed2kThroughProxy", ""},   // [net-2]
    {"connection/proxyHost", "test", "tests/netpolicy/tst_netpolicy.cpp", "ed2kThroughProxy", ""},   // [net-2]
    {"connection/proxyPort", "test", "tests/netpolicy/tst_netpolicy.cpp", "ed2kThroughProxy", ""},   // [net-2]
    {"connection/proxyAuth", "test", "tests/netpolicy/tst_netpolicy.cpp", "proxyNamesAndCredentials", ""},   // [net-2]
    {"connection/proxyUser", "test", "tests/netpolicy/tst_netpolicy.cpp", "proxyNamesAndCredentials", ""},   // [net-2]
    {"connection/proxyPassword", "test", "tests/netpolicy/tst_netpolicy.cpp", "proxyNamesAndCredentials", ""},   // [net-2]
    {"connection/proxyHostnames", "test", "tests/netpolicy/tst_netpolicy.cpp", "proxyNamesAndCredentials", ""},   // [net-2]
    {"connection/connectTimeoutSec", "test", "tests/netpolicy/tst_netpolicy.cpp", "ed2kConnectionPacing", ""},   // [net-2]
    {"connection/acceptIncoming", "test", "tests/netpolicy/tst_netsettings.cpp", "gnutellaPortUltrapeersAndCaches", ""},   // [net-2]
    {"network/connectOnStart", "test", "tests/core/tst_network.cpp", "noAutoConnectAndNoPlugins", ""},
    {"bt/enabled", "test", "tests/core/tst_network.cpp", "lifecycle", ""},
    {"bt/listenPort", "test", "tests/netpolicy/tst_netsettings.cpp", "btSessionSettingsReachLibtorrent", ""},   // [net-2]
    {"bt/dht", "test", "tests/netpolicy/tst_netsettings.cpp", "btSessionSettingsReachLibtorrent", ""},   // [net-2]
    {"bt/pex", "test", "tests/netpolicy/tst_netsettings_bt.cpp", "pexInTheExtensionHandshake", ""},   // [net-3]
    {"bt/lsd", "test", "tests/netpolicy/tst_netsettings.cpp", "btSessionSettingsReachLibtorrent", ""},   // [net-2]
    {"bt/utp", "test", "tests/netpolicy/tst_netsettings.cpp", "btSessionSettingsReachLibtorrent", ""},   // [net-2]
    {"bt/encryption", "test", "tests/netpolicy/tst_netpolicy_bt.cpp", "requiredEncryptionRefusesHeaderOnlyPeers", ""},   // [net-2]
    {"bt/maxConnections", "test", "tests/netpolicy/tst_netsettings.cpp", "btSessionSettingsReachLibtorrent", ""},   // [net-2]
    {"bt/maxConnectionsPerTorrent", "test", "tests/netpolicy/tst_netpolicy_bt.cpp", "randomPortsAndTorrentLimits", ""},   // [net-2]
    {"bt/maxUploads", "test", "tests/netpolicy/tst_netsettings.cpp", "btSessionSettingsReachLibtorrent", ""},   // [net-2]
    {"bt/maxUploadsPerTorrent", "test", "tests/netpolicy/tst_netsettings_bt.cpp", "uploadSlotsPerTorrent", ""},   // [net-3]
    {"bt/maxActiveDownloads", "test", "tests/netpolicy/tst_netpolicy_bt.cpp", "randomPortsAndTorrentLimits", ""},   // [net-2]
    {"bt/maxActiveSeeds", "test", "tests/netpolicy/tst_netsettings.cpp", "btSessionSettingsReachLibtorrent", ""},   // [net-2]
    {"bt/seedRatioLimit", "test", "tests/netpolicy/tst_netsettings_bt.cpp", "seedRatioAndActionStopTheSeed", ""},   // [net-3]
    {"bt/seedTimeLimitMin", "test", "tests/netpolicy/tst_netsettings_bt.cpp", "seedTimeLimitStopsTheSeed", ""},   // [net-3]
    {"bt/seedLimitAction", "test", "tests/netpolicy/tst_netsettings_bt.cpp", "seedRatioAndActionStopTheSeed", ""},   // [net-3]
    {"bt/seedCompleted", "test", "tests/netpolicy/tst_netsettings_bt.cpp", "completedTorrentsSeedOrStop", ""},   // [net-3]
    {"bt/seedLibrary", "test", "tests/netpolicy/tst_netsettings_bt.cpp", "libraryFilesSeededAsTorrents", ""},   // [net-3]
    {"bt/announceToAllTrackers", "test", "tests/netpolicy/tst_netsettings.cpp", "btSessionSettingsReachLibtorrent", ""},   // [net-2]
    {"bt/extraTrackers", "test", "tests/netpolicy/tst_netsettings_bt.cpp", "extraTrackersAnnouncedTo", ""},   // [net-3]
    {"bt/dhtBootstrapNodes", "test", "tests/netpolicy/tst_netpolicy_bt.cpp", "adapterBindsPeersAndDht", ""},   // [net-2]
    {"bt/anonymousMode", "test", "tests/netpolicy/tst_netsettings.cpp", "btSessionSettingsReachLibtorrent", ""},   // [net-2]
    {"bt/superSeeding", "test", "tests/netpolicy/tst_netsettings_bt.cpp", "superSeedingRevealsPiecesOneByOne", ""},   // [net-3]
    {"bt/preallocate", "test", "tests/netpolicy/tst_netsettings_bt.cpp", "preallocateReservesTheFile", ""},   // [net-3]
    {"bt/webSeeds", "test", "tests/netpolicy/tst_netsettings_bt.cpp", "webSeedsUsedOrNot", ""},   // [net-3]
    {"bt/allowMultipleConnectionsPerIp", "test", "tests/netpolicy/tst_netsettings.cpp", "btSessionSettingsReachLibtorrent", ""},   // [net-2]
    {"bt/validateHttpsTrackers", "test", "tests/netpolicy/tst_netsettings.cpp", "btSessionSettingsReachLibtorrent", ""},   // [net-2]
    {"ed2k/enabled", "test", "tests/core/tst_network.cpp", "lifecycle", ""},
    {"ed2k/tcpPort", "test", "tests/netpolicy/tst_netsettings.cpp", "ed2kPortsAndServerChoices", ""},   // [net-2]
    {"ed2k/udpPort", "test", "tests/netpolicy/tst_netsettings.cpp", "ed2kPortsAndServerChoices", ""},   // [net-2]
    {"ed2k/serverListUrl", "test", "tests/netpolicy/tst_netsettings.cpp", "ed2kServerListDownload", ""},   // [net-2]
    {"ed2k/autoUpdateServerList", "test", "tests/netpolicy/tst_netsettings.cpp", "ed2kServerListDownload", ""},   // [net-2]
    {"ed2k/autoConnect", "test", "tests/netpolicy/tst_netsettings.cpp", "ed2kPortsAndServerChoices", ""},   // [net-2]
    {"ed2k/reconnect", "test", "tests/netpolicy/tst_netsettings.cpp", "ed2kServerAnnouncementsAndReconnect", ""},   // [net-2]
    {"ed2k/staticServersOnly", "test", "tests/netpolicy/tst_netsettings.cpp", "ed2kPortsAndServerChoices", ""},   // [net-2]
    {"ed2k/addServersFromServer", "test", "tests/netpolicy/tst_netsettings.cpp", "ed2kServerAnnouncementsAndReconnect", ""},   // [net-2]
    {"ed2k/maxSourcesPerFile", "test", "tests/netpolicy/tst_netsettings_p2p.cpp", "ed2kSourcesPerDownload", ""},   // [net-3]
    {"ed2k/maxConnections", "test", "tests/netpolicy/tst_netpolicy.cpp", "ed2kConnectionPacing", ""},   // [net-2]
    {"ed2k/obfuscation", "test", "tests/netpolicy/tst_netpolicy.cpp", "ed2kObfuscationChoices", ""},   // [net-2]
    {"ed2k/requireObfuscation", "test", "tests/netpolicy/tst_netpolicy.cpp", "ed2kObfuscationChoices", ""},   // [net-2]
    {"ed2k/userHash", "internal", "src/net/ed2k/ed2k_core.cpp", "\"ed2k/userHash\"", "the eD2k user hash made at the first start (identity on eD2k)"},
    {"ed2k/uploadSlots", "test", "tests/netpolicy/tst_netsettings_p2p.cpp", "ed2kOwnUploadSlots", ""},   // [net-3]
    {"ed2k/maxHalfOpen", "test", "tests/netpolicy/tst_netpolicy.cpp", "ed2kConnectionPacing", ""},   // [net-2]
    {"ed2k/maxConnectsPer5s", "test", "tests/netpolicy/tst_netpolicy.cpp", "ed2kConnectionPacing", ""},   // [net-2]
    {"ed2k/parallelServerConnects", "test", "tests/netpolicy/tst_netsettings.cpp", "ed2kPortsAndServerChoices", ""},   // [net-2]
    {"ed2k/udpPing", "test", "tests/netpolicy/tst_netsettings_p2p.cpp", "ed2kServerUdpSwitches", ""},   // [net-3]
    {"ed2k/udpSearch", "test", "tests/netpolicy/tst_netsettings_p2p.cpp", "ed2kServerUdpSwitches", ""},   // [net-3]
    {"ed2k/udpSources", "test", "tests/netpolicy/tst_netsettings_p2p.cpp", "ed2kServerUdpSwitches", ""},   // [net-3]
    {"ed2k/compressUploads", "test", "tests/netpolicy/tst_netpolicy.cpp", "ed2kCompressUploads", ""},   // [net-2]
    {"kad/enabled", "test", "tests/core/tst_search.cpp", "managerFanOutAndMerge", ""},
    {"kad/nodesDatUrl", "test", "tests/netpolicy/tst_netsettings_p2p.cpp", "kadNodesDatFromTheUrl", ""},   // [net-3]
    {"kad/bootstrapNodes", "test", "tests/netpolicy/tst_netpolicy.cpp", "kadOffWithProxyAndBoundToAdapter", ""},   // [net-2]
    {"kad/autoBootstrap", "test", "tests/ed2k/tst_kad.cpp", "simulatedNetwork", ""},
    {"kad/storeIndex", "test", "tests/netpolicy/tst_netsettings_p2p.cpp", "kadStoreIndexOff", ""},   // [net-3]
    {"kad/firewallChecks", "test", "tests/netpolicy/tst_netsettings_p2p.cpp", "kadPublishStoreFirewallBuddies", ""},   // [net-3]
    {"kad/publish", "test", "tests/netpolicy/tst_netsettings_p2p.cpp", "kadPublishStoreFirewallBuddies", ""},   // [net-3]
    {"kad/buddies", "test", "tests/netpolicy/tst_netsettings_p2p.cpp", "kadPublishStoreFirewallBuddies", ""},   // [net-3]
    {"kad/maxPacketsPerSec", "test", "tests/netpolicy/tst_netsettings_p2p.cpp", "kadPacketRate", ""},   // [net-3]
    {"gnutella/enabled", "test", "tests/core/tst_network.cpp", "lifecycle", ""},
    {"gnutella/port", "test", "tests/netpolicy/tst_netsettings.cpp", "gnutellaPortUltrapeersAndCaches", ""},   // [net-2]
    {"gnutella/maxUltrapeers", "test", "tests/netpolicy/tst_netsettings_p2p.cpp", "gnutellaUltrapeerCount", ""},   // [net-3]
    {"gnutella/gwebcaches", "test", "tests/netpolicy/tst_netsettings_p2p.cpp", "gnutellaUserCaches", ""},   // [net-3]
    {"gnutella/bootstrapHosts", "test", "tests/netpolicy/tst_netpolicy.cpp", "gnutellaThroughProxy", ""},   // [net-2]
    {"gnutella/browseHost", "test", "tests/gnutella/tst_gnutella_loopback.cpp", "browseDirect", ""},
    {"gnutella/uploadSlots", "test", "tests/netpolicy/tst_netsettings_p2p.cpp", "gnutellaSlotsQueueAndPoll", ""},   // [net-3]
    {"gnutella/uploadQueueSize", "test", "tests/netpolicy/tst_netsettings_p2p.cpp", "gnutellaSlotsQueueAndPoll", ""},   // [net-3]
    {"gnutella/uploadQueuePollSec", "test", "tests/netpolicy/tst_netsettings_p2p.cpp", "gnutellaSlotsQueueAndPoll", ""},   // [net-3]
    {"gnutella/autoBootstrap", "test", "tests/netpolicy/tst_netsettings.cpp", "gnutellaPortUltrapeersAndCaches", ""},   // [net-2]
    {"gnutella/udpHostCaches", "test", "tests/netpolicy/tst_netsettings_p2p.cpp", "gnutellaUserCaches", ""},   // [net-3]
    {"gnutella/encryption", "test", "tests/gnutella/tst_gnutella_loopback.cpp", "tlsRequireRefusesPlain", ""},   // [net-2]
    {"http/enabled", "test", "tests/netpolicy/tst_netsettings_web.cpp", "httpEnabledInTheCore", ""},   // [net-3]
    {"http/maxConnectionsPerServer", "test", "tests/http/tst_http.cpp", "perHostCap", ""},   // [net-2]
    {"http/timeoutSec", "test", "tests/netpolicy/tst_netsettings_web.cpp", "httpTimeoutAndRetryDelay", ""},   // [net-3]
    {"http/userAgent", "test", "tests/http/tst_http.cpp", "probeBasic", ""},
    {"http/verifyTls", "test", "tests/netpolicy/tst_netsettings_web.cpp", "httpCertificatesAndRedirects", ""},   // [net-3]
    {"http/connectionsPerSource", "test", "tests/netpolicy/tst_netsettings_web.cpp", "httpPartsPerSource", ""},   // [net-3]
    {"http/allowInsecureRedirect", "test", "tests/netpolicy/tst_netsettings_web.cpp", "httpCertificatesAndRedirects", ""},   // [net-3]
    {"http/stallTimeoutSec", "test", "tests/http/tst_http.cpp", "stallRetry", ""},   // [net-2]
    {"http/maxRetries", "test", "tests/http/tst_http.cpp", "transient5xxGivesUp", ""},   // [net-2]
    {"http/retryBaseMs", "test", "tests/netpolicy/tst_netsettings_web.cpp", "httpTimeoutAndRetryDelay", ""},   // [net-3]
    {"overlay/enabled", "test", "tests/overlay/tst_netsettings_ovl.cpp", "enabledSwitchesStartAndStop", ""},   // [net-3]
    {"overlay/bootstrapPeers", "test", "tests/overlay/tst_overlay_misc.cpp", "bootstrapPeersFromSettings", ""},
    {"overlay/maxPeers", "test", "tests/overlay/tst_netsettings_ovl.cpp", "overlayMaxPeersCapsInboundLinks", ""},   // [net-3]
    {"overlay/relay", "test", "tests/overlay/tst_netsettings_ovl.cpp", "overlayRelayServesCircuits", ""},   // [net-3]
    {"overlay/shareHashLinks", "test", "tests/overlay/tst_netsettings_ovl.cpp", "overlayShareHashLinksAnswersAndGossip", ""},   // [net-3]
    {"web/enabled", "test", "tests/netpolicy/tst_netsettings_web.cpp", "webEnabledInTheNetworkManager", ""},   // [net-3]
    {"web/maxResults", "test", "tests/netpolicy/tst_netsettings_web.cpp", "webResultsAdultWordsAndTrackers", ""},   // [net-3]
    {"web/userAgent", "test", "tests/netpolicy/tst_netsettings_web.cpp", "webUserAgentAndSearchTimeout", ""},   // [net-3]
    {"web/providerEnabled", "internal", "src/net/web/web_search_plugin.h", "\"web/providerEnabled\"", "per web index on/off, set from the Networks page"},
    {"web/adultKeywords", "test", "tests/netpolicy/tst_netsettings_web.cpp", "webResultsAdultWordsAndTrackers", ""},   // [net-3]
    {"web/timeoutSec", "test", "tests/netpolicy/tst_netsettings_web.cpp", "webUserAgentAndSearchTimeout", ""},   // [net-3]
    {"web/probeOnStart", "test", "tests/netpolicy/tst_netsettings_web.cpp", "webIndexCheckAtStart", ""},   // [net-3]
    {"web/trackers", "test", "tests/netpolicy/tst_netsettings_web.cpp", "webResultsAdultWordsAndTrackers", ""},   // [net-3]
    {"api/enabled", "test", "tests/stream/tst_stream_api.cpp", "onOffPortAndLanAtRunTime", ""},   // [str-1]
    {"api/port", "test", "tests/stream/tst_stream_api.cpp", "onOffPortAndLanAtRunTime", ""},   // [str-1]
    {"api/allowLan", "test", "tests/stream/tst_stream_api.cpp", "onOffPortAndLanAtRunTime", ""},   // [str-1]
    {"api/token", "test", "tests/stream/tst_stream_api.cpp", "newTokenEndsOldLinks", ""},   // [str-1]
    {"api/cacheLimitMB", "test", "tests/stream/tst_stream_play.cpp", "idleTimeAndCacheLimit", ""},   // [str-1]
    {"api/idleMin", "test", "tests/stream/tst_stream_play.cpp", "pausedStoppedAndBack", ""},   // [str-1]
    {"api/keepStreams", "test", "tests/stream/tst_stream_play.cpp", "keepAsDownload", ""},   // [str-1]
    {"api/allowDelete", "test", "tests/stream/tst_api_downloads.cpp", "deleteSwitch", ""},   // [api-2]
    {"rats/enabled", "test", "tests/overlay/tst_netsettings_ovl.cpp", "enabledSwitchesStartAndStop", ""},   // [net-3]
    {"rats/protocol", "internal", "src/net/overlay/overlay_plugin.h", "\"rats/protocol\"", "rats-search protocol id (compatibility with other clients)"},
    {"rats/listenPort", "test", "tests/overlay/tst_netsettings_ovl.cpp", "ratsListenPortAndBootstrapPeers", ""},   // [net-3]
    {"rats/dhtPort", "test", "tests/overlay/tst_netsettings_ovl.cpp", "ratsDhtPortIsTheDhtSource", ""},   // [net-3]
    {"rats/safeSearch", "test", "tests/overlay/tst_netsettings_ovl.cpp", "ratsSafeSearchAndMaxResults", ""},   // [net-3]
    {"rats/maxPeers", "test", "tests/overlay/tst_netsettings_ovl.cpp", "ratsMaxPeersCapsInboundLinks", ""},   // [net-3]
    {"rats/bootstrapPeers", "test", "tests/overlay/tst_netsettings_ovl.cpp", "ratsListenPortAndBootstrapPeers", ""},   // [net-3]
    {"rats/maxResults", "test", "tests/overlay/tst_netsettings_ovl.cpp", "ratsSafeSearchAndMaxResults", ""},   // [net-3]
    {"other/defaultLogin", "test", "tests/ed2k/tst_ed2k.cpp", "helloRoundTrip", ""},
    {"messages/allowPrivateMessages", "test", "tests/core/tst_set1_core.cpp", "messagePrivacy", ""},
    {"messages/allowBrowse", "test", "tests/overlay/tst_overlay_net.cpp", "browseHotlistOnly", ""},
    {"messages/allowWhois", "test", "tests/overlay/tst_overlay_users.cpp", "whois", ""},
    {"messages/autoReply", "test", "tests/core/tst_set1_core.cpp", "messagePrivacy", ""},
    {"messages/autoReplyText", "test", "tests/core/tst_set1_core.cpp", "messagePrivacy", ""},
    {"messages/openWindowOnMessage", "test", "tests/ui/tst_set1.cpp", "privateMessageWindows", ""},
    {"messages/flashOnMessage", "test", "tests/ui/tst_set1.cpp", "privateMessageWindows", ""},
    {"messages/soundOnMessage", "test", "tests/ui/tst_uih.cpp", "soundChoices", ""},
    {"messages/blockFloods", "test", "tests/core/tst_chat.cpp", "withBackend", ""},
    {"messages/floodLimit", "test", "tests/core/tst_chat.cpp", "withBackend", ""},
    {"messages/ignoreNonHotlist", "test", "tests/core/tst_set1_core.cpp", "messagePrivacy", ""},
    {"messages/filterWords", "test", "tests/core/tst_set1_core.cpp", "messagePrivacy", ""},
    {"messages/allowFrom", "test", "tests/core/tst_set1_core.cpp", "messagePrivacy", ""},
    {"messages/allowFromHotlist", "test", "tests/core/tst_set1_core.cpp", "messagePrivacy", ""},
    {"messages/allowFromDownloadingFrom", "test", "tests/core/tst_set1_core.cpp", "messagePrivacy", ""},
    {"messages/allowFromUploadingTo", "test", "tests/core/tst_set1_core.cpp", "messagePrivacy", ""},
    {"messages/allowFromSameRoom", "test", "tests/core/tst_set1_core.cpp", "messagePrivacy", ""},
    {"messages/alertMode", "test", "tests/ui/tst_uih.cpp", "privateMessageAlert", ""},
    {"messages/alertWav", "test", "tests/ui/tst_uih.cpp", "soundChoices", ""},
    {"chat/backlogLines", "test", "tests/core/tst_set1_core.cpp", "roomsAndBackscroll", ""},
    {"chat/autoJoinFavourites", "test", "tests/core/tst_set1_core.cpp", "roomsAndBackscroll", ""},
    {"chat/showJoinsParts", "test", "tests/core/tst_set1_core.cpp", "messagePrivacy", ""},
    {"chat/showTimestamps", "test", "tests/ui/tst_set1.cpp", "chatRoomLines", ""},
    {"chat/timestampFormat", "test", "tests/ui/tst_set1.cpp", "chatRoomLines", ""},
    {"chat/fontFamily", "test", "tests/ui/tst_set1.cpp", "privateMessageWindows", ""},
    {"chat/fontSize", "test", "tests/ui/tst_set1.cpp", "chatRoomLines", ""},
    {"chat/defaultRoom", "test", "tests/core/tst_set1_core.cpp", "roomsAndBackscroll", ""},
    {"chat/fetchListImmediately", "test", "tests/ui/tst_set1.cpp", "channelList", ""},
    {"chat/topicInFilter", "test", "tests/ui/tst_set1.cpp", "channelList", ""},
    {"chat/allowLargeBackscroll", "test", "tests/core/tst_set1_core.cpp", "roomsAndBackscroll", ""},
    {"hotlist/notifyOnline", "test", "tests/ui/tst_uig.cpp", "hotlistTellsWhenAUserComesOnline", ""},
    {"hotlist/soundOnline", "test", "tests/ui/tst_uih.cpp", "downloadAndHotlistSounds", ""},
    {"hotlist/autoBrowse", "test", "tests/ui/tst_set1.cpp", "hotlistDoubleClick", ""},
    {"search/maxResults", "test", "tests/core/tst_set1_core.cpp", "searchResults", ""},
    {"search/networks", "test", "tests/core/tst_set1_core.cpp", "searchResults", ""},
    {"search/timeoutSec", "test", "tests/core/tst_search.cpp", "timeoutAndLateHits", ""},
    {"search/autoComplete", "test", "tests/ui/tst_set1.cpp", "searchTabSettings", ""},
    {"search/saveHistory", "test", "tests/core/tst_set1_core.cpp", "searchResults", ""},
    {"search/historySize", "test", "tests/core/tst_set1_core.cpp", "searchResults", ""},
    {"search/adultFilter", "test", "tests/core/tst_set1_core.cpp", "searchResults", ""},
    {"search/fakeFilter", "test", "tests/core/tst_set1_core.cpp", "searchResults", ""},
    {"search/mergeByNameAndSize", "test", "tests/core/tst_set1_core.cpp", "searchResults", ""},
    {"search/showFullPaths", "test", "tests/ui/tst_set1.cpp", "searchTabSettings", ""},
    {"search/minCandidateSize", "test", "tests/core/tst_set1_core.cpp", "searchResults", ""},
    {"search/blockedExtensions", "test", "tests/core/tst_set1_core.cpp", "searchResults", ""},
    {"search/defaultType", "test", "tests/ui/tst_set1.cpp", "searchTabSettings", ""},
    {"search/customExtensions", "test", "tests/ui/tst_set1.cpp", "customExtensions", ""},
    {"search/defaultConnection", "test", "tests/ui/tst_set1.cpp", "searchTabSettings", ""},
    {"search/autoFindSourcesMin", "test", "tests/engine/tst_engine_sources.cpp", "autoFindSourcesPerDownload", ""},
    {"search/autoEnterQueue", "test", "tests/engine/tst_set1_engine.cpp", "autoEnterQueueDefault", ""},
    {"search/typeButtonAudio", "test", "tests/ui/tst_set1.cpp", "searchTabSettings", ""},
    {"search/typeButtonVideo", "test", "tests/ui/tst_set1.cpp", "searchTabSettings", ""},
    {"search/typeButtonPictures", "test", "tests/ui/tst_set1.cpp", "searchTabSettings", ""},
    {"search/typeButtonAudioLabel", "test", "tests/ui/tst_set1.cpp", "searchTabSettings", ""},
    {"search/typeButtonVideoLabel", "test", "tests/ui/tst_set1.cpp", "searchTabSettings", ""},
    {"search/typeButtonPicturesLabel", "test", "tests/ui/tst_set1.cpp", "searchTabSettings", ""},
    {"search/groupsExpanded", "test", "tests/ui/tst_uih.cpp", "groupsExpanded", ""},
    {"search/dropTorrentBridges", "test", "tests/core/tst_glue2.cpp", "searchManagerHygiene", ""},
    {"search/flagSuspects", "test", "tests/core/tst_glue2.cpp", "searchManagerHygiene", ""},
    {"search/hideSuspects", "test", "tests/core/tst_glue2.cpp", "searchManagerHygiene", ""},
    {"transfers/incomingDir", "test", "tests/engine/tst_set1_engine.cpp", "foldersPausedAndReserved", ""},
    {"transfers/incompleteDir", "test", "tests/engine/tst_set1_engine.cpp", "foldersPausedAndReserved", ""},
    {"transfers/maxDownloads", "test", "tests/engine/tst_set1_engine.cpp", "downloadsAtOnce", ""},
    {"transfers/maxUploads", "test", "tests/overlay/tst_set1_overlay.cpp", "uploadSlotsAndQueue", ""},
    {"transfers/maxUploadsPerUser", "test", "tests/overlay/tst_set1_overlay.cpp", "uploadSlotsAndQueue", ""},
    {"transfers/maxSourcesPerDownload", "test", "tests/engine/tst_set1_engine.cpp", "sourcesPerDownload", ""},
    {"transfers/autoRetry", "test", "tests/engine/tst_set1_engine.cpp", "automaticRetry", ""},
    {"transfers/retryIntervalSec", "test", "tests/engine/tst_set1_engine.cpp", "automaticRetry", ""},
    {"transfers/retryMaxAttempts", "test", "tests/engine/tst_set1_engine.cpp", "automaticRetry", ""},
    {"transfers/metadataTimeoutSec", "test", "tests/engine/tst_engine_torrent.cpp", "metadataTimeoutFallsBackToPartFile", ""},
    {"transfers/autoClearCompleted", "test", "tests/engine/tst_set1_engine.cpp", "clearedWhenComplete", ""},
    {"transfers/autoClearUploads", "test", "tests/core/tst_set1_core.cpp", "finishedUploadsCleared", ""},
    {"transfers/shareCompleted", "test", "tests/core/tst_set1_core.cpp", "sharingRulesOnEveryPath", ""},
    {"transfers/preallocate", "test", "tests/engine/tst_set1_engine.cpp", "foldersPausedAndReserved", ""},
    {"transfers/startPaused", "test", "tests/engine/tst_set1_engine.cpp", "foldersPausedAndReserved", ""},
    {"transfers/sequentialDefault", "test", "tests/engine/tst_engine_order.cpp", "sequentialMakesAPrefix", ""},
    {"transfers/firstLastDefault", "test", "tests/engine/tst_engine_order.cpp", "firstLastHeadThenTail", ""},
    {"transfers/removeAction", "test", "tests/ui/tst_uij.cpp", "rememberedChoiceAndSetting", ""},   // [ui-j]
    {"transfers/minFreeSpaceMB", "test", "tests/engine/tst_set1_engine.cpp", "freeSpaceGuard", ""},
    {"transfers/verifyCompleted", "test", "tests/engine/tst_set1_engine.cpp", "finalCheck", ""},
    {"transfers/uploadsToHotlistOnly", "test", "tests/overlay/tst_overlay_net.cpp", "uploadsToHotlistOnly", ""},
    {"transfers/clearCompleteDownloadsSec", "test", "tests/engine/tst_set1_engine.cpp", "clearedWhenComplete", ""},
    {"transfers/clearCompleteUploadsSec", "test", "tests/core/tst_set1_core.cpp", "finishedUploadsCleared", ""},
    {"transfers/pendingDownloadTimeoutSec", "test", "tests/engine/tst_set1_engine.cpp", "pendingTimeout", ""},
    {"transfers/switchToTransfers", "test", "tests/ui/tst_uih.cpp", "newDownloadsExpandedAndSwitch", ""},
    {"transfers/newDownloadsExpanded", "test", "tests/ui/tst_uih.cpp", "newDownloadsExpandedAndSwitch", ""},
    {"transfers/suppressDangerousWarning", "test", "tests/ui/tst_uid_wiring.cpp", "dangerousWarningSessionAndPermanent", ""},
    {"queue/enabled", "test", "tests/overlay/tst_set1_overlay.cpp", "uploadSlotsAndQueue", ""},
    {"queue/maxLength", "test", "tests/overlay/tst_set1_overlay.cpp", "uploadSlotsAndQueue", ""},
    {"queue/maxPerUser", "test", "tests/overlay/tst_set1_overlay.cpp", "uploadSlotsAndQueue", ""},
    {"queue/hotlistPriority", "test", "tests/overlay/tst_overlay_net.cpp", "hotlistPriorityInQueue", ""},
    {"queue/sendPosition", "test", "tests/overlay/tst_set1_overlay.cpp", "uploadSlotsAndQueue", ""},
    {"queue/timeoutMin", "test", "tests/overlay/tst_set1_overlay.cpp", "queuedUserDroppedAfterTheTimeSet", ""},
    {"queue/smallFileBypassKB", "test", "tests/overlay/tst_set1_overlay.cpp", "uploadSlotsAndQueue", ""},
    {"queue/changeSpeedWhenBusy", "test", "tests/overlay/tst_set1_overlay.cpp", "othersSeeTheConnectionTypeAtOnce", ""},
    {"queue/changeSpeedThreshold", "test", "tests/overlay/tst_set1_overlay.cpp", "othersSeeTheConnectionTypeAtOnce", ""},
    {"queue/changedSpeed", "test", "tests/overlay/tst_set1_overlay.cpp", "othersSeeTheConnectionTypeAtOnce", ""},
    {"bandwidth/downLimitKBs", "test", "tests/core/tst_ratelimiter.cpp", "settingsAndSchedule", ""},   // [net-3] was a live probe tool
    {"bandwidth/upLimitKBs", "test", "tests/core/tst_ratelimiter.cpp", "settingsAndSchedule", ""},   // [net-3] was a live probe tool
    {"bandwidth/scheduleEnabled", "test", "tests/core/tst_ratelimiter.cpp", "settingsAndSchedule", ""},
    {"bandwidth/scheduleStart", "test", "tests/core/tst_ratelimiter.cpp", "settingsAndSchedule", ""},
    {"bandwidth/scheduleEnd", "test", "tests/core/tst_ratelimiter.cpp", "settingsAndSchedule", ""},
    {"bandwidth/scheduleDays", "test", "tests/core/tst_ratelimiter.cpp", "settingsAndSchedule", ""},
    {"bandwidth/scheduleDownKBs", "test", "tests/core/tst_ratelimiter.cpp", "settingsAndSchedule", ""},
    {"bandwidth/scheduleUpKBs", "test", "tests/core/tst_ratelimiter.cpp", "settingsAndSchedule", ""},
    {"bandwidth/limitLan", "test", "tests/netpolicy/tst_netsettings_bt.cpp", "lanPeersLimitedOrNot", ""},   // [net-3]
    {"diag/portCheckUrl", "test", "tests/core/tst_set1_core.cpp", "portCheckAddress", ""},
    {"diag/hideAddresses", "test", "tests/ui/tst_uih.cpp", "connectionTestHidesAddressesInTheWindow", ""},
    {"diag/answerProbes", "test", "tests/overlay/tst_overlay_probe.cpp", "twoNodeProbe", ""},
    {"bandwidth/lastDownLimitKBs", "test", "tests/ui/tst_bw1.cpp", "rowAndSettingsInStep", ""},   // [net-3] was a live probe tool
    {"bandwidth/lastUpLimitKBs", "test", "tests/ui/tst_bw1.cpp", "rowAndSettingsInStep", ""},   // [net-3] was a live probe tool
    {"bandwidth/graphAverageSec", "test", "tests/ui/tst_uib_bandwidth.cpp", "historyIsSplitIntoSeries", ""},
    {"launch/useOsDefault", "test", "tests/ui/tst_uih.cpp", "launchEntriesAndCommands", ""},
    {"launch/audioCommand", "test", "tests/ui/tst_uih.cpp", "launchEntriesAndCommands", ""},
    {"launch/videoCommand", "test", "tests/ui/tst_set1.cpp", "programPerFileType", ""},
    {"launch/imageCommand", "test", "tests/ui/tst_set1.cpp", "programPerFileType", ""},
    {"launch/documentCommand", "test", "tests/ui/tst_set1.cpp", "programPerFileType", ""},
    {"launch/otherCommand", "test", "tests/ui/tst_set1.cpp", "programPerFileType", ""},
    {"launch/doubleClickAction", "test", "tests/ui/tst_uih.cpp", "doubleClickAction", ""},
    {"launch/previewIncomplete", "test", "tests/ui/tst_dl1.cpp", "previewEnablement", ""},
    {"launch/previewPlayer", "test", "tests/ui/tst_dl1.cpp", "firstTimeDialogAndRememberedChoice", ""},
    {"launch/previewProgram", "test", "tests/ui/tst_dl1.cpp", "commandLinePerChoice", ""},
    {"launch/entries", "test", "tests/ui/tst_uih.cpp", "launchEntriesAndCommands", ""},
    {"library/rescanIntervalMin", "test", "tests/core/tst_set1_core.cpp", "watchAndRescan", ""},
    {"library/hashThreads", "test", "tests/core/tst_set1_core.cpp", "hashThreads", ""},
    {"library/watchFolders", "test", "tests/core/tst_set1_core.cpp", "watchAndRescan", ""},
    {"library/shareHidden", "test", "tests/core/tst_set1_core.cpp", "sharingRulesOnEveryPath", ""},
    {"library/excludePatterns", "test", "tests/core/tst_set1_core.cpp", "sharingRulesOnEveryPath", ""},
    {"library/shareIncomplete", "test", "tests/engine/tst_engine.cpp", "partialSharing", ""},
    {"library/sharedTypes", "test", "tests/core/tst_set1_core.cpp", "sharingRulesOnEveryPath", ""},
    {"library/shareUnlistedTypes", "test", "tests/core/tst_set1_core.cpp", "sharingRulesOnEveryPath", ""},
    {"misc/startWithOs", "test", "tests/ui/tst_os_integration.cpp", "fakeFollowsSettings", ""},
    {"misc/startMinimized", "test", "tests/ui/tst_restart.cpp", "minimiseAndCloseAfterRestart", ""},
    {"misc/closeAction", "test", "tests/ui/tst_restart.cpp", "minimiseAndCloseAfterRestart", ""},
    {"misc/confirmExit", "test", "tests/ui/tst_tray_close.cpp", "exitConfirmation", ""},
    {"misc/sounds", "test", "tests/ui/tst_uih.cpp", "soundChoices", ""},
    {"misc/soundDownloadComplete", "test", "tests/ui/tst_uih.cpp", "downloadAndHotlistSounds", ""},
    {"misc/checkAssociations", "test", "tests/ui/tst_set1.cpp", "linkAndFileAssociations", ""},
    {"misc/associateMagnet", "test", "tests/ui/tst_os_integration.cpp", "fakeFollowsSettings", ""},
    {"misc/associateEd2k", "test", "tests/ui/tst_os_integration.cpp", "fakeFollowsSettings", ""},
    {"misc/associateTorrent", "test", "tests/ui/tst_set1.cpp", "linkAndFileAssociations", ""},
    {"misc/trayBalloons", "test", "tests/ui/tst_uih.cpp", "trayBalloonsAndFlash", ""},
    {"misc/flashTaskbar", "test", "tests/ui/tst_uih.cpp", "trayBalloonsAndFlash", ""},
    {"misc/alwaysOnTop", "test", "tests/ui/tst_uih.cpp", "alwaysOnTop", ""},
    {"misc/logLevel", "test", "tests/core/tst_set1_core.cpp", "logFile", ""},
    {"misc/logMaxSizeMB", "test", "tests/core/tst_set1_core.cpp", "logFile", ""},
    {"misc/logKeepFiles", "test", "tests/core/tst_set1_core.cpp", "logFile", ""},
    {"misc/languageFile", "test", "tests/ui/tst_uig_lang.cpp", "loadFromAppearancePageAndReset", ""},
    {"ui/geometry", "internal", "src/app/main.cpp", "ui::Geometry", "main window position and size, saved at exit"},
    {"ui/maximized", "internal", "src/ui/main_window.cpp", "ui::Maximized", "main window maximized at exit"},
    {"ui/lastPage", "internal", "src/ui/main_window.cpp", "ui::LastPage", "the page shown at exit (used with ui/restoreLastPage)"},
    {"ui/restoreLastPage", "test", "tests/ui/tst_set1.cpp", "startPageAndFonts", ""},
    {"ui/floatingPages", "internal", "src/ui/main_window_c.cpp", "ui::FloatingPages", "pages floated in their own windows"},
    {"ui/searchType", "internal", "src/ui/pages/search_page.cpp", "ui::SearchType", "old search type, migrated to search/defaultType"},
    {"ui/searchNetworkFilter", "internal", "src/ui/pages/search_page.cpp", "ui::SearchNetworkFilter", "last network filter of the Search page"},
    {"ui/searchConnectionFilter", "internal", "src/ui/kit/session.cpp", "ui::SearchConnectionFilter", "old connection filter, migrated to search/defaultConnection"},
    {"ui/settingsPage", "test", "tests/ui/tst_uih.cpp", "settingsPageRemembered", ""},
    {"ui/showGrid", "test", "tests/ui/tst_uih.cpp", "gridHeaderSelectedColours", ""},
    {"ui/fontNormal", "test", "tests/ui/tst_uid_fonts.cpp", "normalFontChangesChromeAndMenus", ""},
    {"ui/fontDynamic", "test", "tests/ui/tst_uid_fonts.cpp", "dynamicFontResizesListsLive", ""},
    {"ui/fontLarge", "test", "tests/ui/tst_uid_fonts.cpp", "largeFontChangesHeadline", ""},
    {"ui/colorSchemeFile", "internal", "", "", "last colour scheme file loaded or saved"},
    {"ui/hotlistVisible", "internal", "src/ui/main_window.cpp", "\"ui/hotlistVisible\"", "Hotlist pane open at exit"},
    {"ui/hotlistConfirmRemove", "internal", "src/ui/hotlist_pane.cpp", "\"ui/hotlistConfirmRemove\"", "Hotlist pane: confirm removing (its menu switch)"},
    {"ui/chatOpenRooms", "internal", "src/ui/pages/chat_page.h", "\"ui/chatOpenRooms\"", "chat rooms open at exit, joined again"},
    {"ui/chatHoldOpenAfterPart", "internal", "src/ui/pages/chat_page.h", "\"ui/chatHoldOpenAfterPart\"", "chat room tab kept after leaving (its menu switch)"},
    {"ui/chatWarningDisabled", "test", "tests/ui/tst_uih.cpp", "chatWarningDoNotShowAgain", ""},
    {"ui/lastShareDir", "internal", "src/ui/pages/shared_page.cpp", "\"ui/lastShareDir\"", "last folder of Share Folder"},
    {"ui/scale", "test", "tests/ui/tst_uie.cpp", "scaleChangeAsksForRestart", ""},
    {"ui/smoothFonts", "test", "tests/ui/tst_set1.cpp", "startPageAndFonts", ""},
    {"ui/exactSizes", "test", "tests/ui/tst_uie.cpp", "exactSizesSettingIsLive", ""},
    {"ui/notifyDownloadDone", "test", "tests/ui/tst_uie.cpp", "completedDownloadNotifies", ""},
    {"ui/searchHistoryMenu", "test", "tests/ui/tst_set1.cpp", "searchTabSettings", ""},
    {"ui/connectionDefaults", "internal", "src/ui/kit/session.cpp", "ui::ConnectionDefaults", "connection type the slots were set for (applyConnectionDefaults)"},
    {"ui/colorTheme", "test", "tests/ui/tst_theme.cpp", "switchKeepsCustomColours", ""},
    {"ui/columns/*", "internal", "src/ui/kit/list_view.h", "\"ui/columns/", "column layouts of the lists (MxListView::setStateKey)"},
    {"ui/splitters/*", "internal", "src/ui/main_window.cpp", "splitterKey(", "splitter positions"},
    {"ui/sort/*", "internal", "src/ui/kit/list_view.h", "\"ui/sort/", "sort column of the lists"},
    {"ui/dialogs/*", "internal", "src/app/main.cpp", "\"ui/dialogs/", "positions of dialogs and floating pages"},
    {"colors/listBackground", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/listText", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/listSelectedBackground", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/listSelectedText", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/listGrid", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/listHeaderText", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/partialText", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/bitrateLow", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/bitrateMedium", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/bitrateHigh", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/speedSlow", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/speedMedium", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/speedFast", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/available", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/unavailable", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/pingGood", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/pingMedium", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/pingBad", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/matchIncomplete", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/canJoinDownload", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/currentlyDownloading", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/networkLabel", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/slotOne", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/channelName", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/channelUsers", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/channelTopic", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/ignoredText", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/hotlistHeader", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/hotlistTemporary", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/inactiveText", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/transferActive", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/transferQueued", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/transferComplete", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/transferError", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/transferPaused", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/transferConnecting", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/transferNegotiating", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/transferRemoteQueued", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/transferBusy", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/transferCancelled", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/progressBar", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/progressBackground", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/progressActive", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/userOnline", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/userOffline", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/serverConnected", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/serverConnecting", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/serverDisconnected", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/chatBackground", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/chatText", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/chatOwnName", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/chatOtherName", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/chatAction", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/chatJoin", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/chatPart", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/chatTopic", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/chatMotd", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/chatSystem", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/chatError", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/chatTimestamp", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/chatLink", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/chatInputBackground", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/chatInputText", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/chatUserListBackground", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/chatUserListText", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/chatUserAdmin", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/chatUserVoiced", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/graphBackground", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/graphGrid", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/graphDown", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/graphUp", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/graphText", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/graphOverlay", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/graphPackets", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/graphIncomingPeers", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/graphOutgoingPeers", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/graphServerIn", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/graphServerOut", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/graphOther", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/statusLedOn", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/statusLedOff", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/statusLedConnecting", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/statusLedError", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/newsBackground", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/newsText", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/newsLink", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"colors/suspectText", "test", "tests/ui/tst_set1_colors.cpp", "everyColourInItsWindowDark", ""},
    {"updates/mode", "test", "tests/ui/tst_update.cpp", "automaticCheck", ""},
    {"internal/firstRunDone", "internal", "src/core/real_core.cpp", "internal::FirstRunDone", "first start done"},
    {"internal/wizardDone", "internal", "src/app/main.cpp", "internal::WizardDone", "Setup Wizard completed"},
    {"internal/termsAccepted", "internal", "src/ui/dialogs/legal_notice.cpp", "internal::TermsAccepted", "version of the terms the user accepted"},
    {"internal/lastVersion", "internal", "src/core/real_core.cpp", "internal::LastVersion", "last version run (upgrade steps)"},
    {"internal/installId", "internal", "src/core/network_manager.cpp", "internal::InstallId", "random id of this installation"},
};

} // namespace mxreg
