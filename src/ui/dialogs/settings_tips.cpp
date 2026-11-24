// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: [ui-h] the tool tips of the Settings controls, one per setting key. PageKit gives every
// bound control the tip of its key (settingTip); a page may set a more specific one afterwards. The
// default value is added when the text does not name it already. tst_uig checks that every control
// has a tip, tst_settings_registry that every key bound on a page has a hand-written one here.
#include "ui/dialogs/settings_pages.h"

#include <QtCore/QHash>

namespace mx::ui {

namespace {

const QHash<QString, const char *> &tipTable()
{
    static const QHash<QString, const char *> t = {
        // ---- identity / Internet Connection
        {"identity/screenName", QT_TR_NOOP("The name other users see in chat rooms, the Hotlist and transfers. A five-digit number is added on the OctoMX Peer Network.")},
        {"identity/connection", QT_TR_NOOP("The kind of internet line you have. Other users see it next to your name, and OctoMX sets upload slots, downloads and queues for it.")},
        {"identity/primaryConnection", QT_TR_NOOP("What OctoMX shows: with a primary connection the status line counts your connections and searches per minute and the Chat page offers Host; without, only the time online. The OctoMX Peer Network itself treats every user alike; passing connections on for others is \"Act as a relay\" below.")},
        {"connection/tcpPort", QT_TR_NOOP("The port of the OctoMX Peer Network, for TCP and UDP alike. Change it when another program uses it or your router forwards another one. After a restart.")},
        {"connection/acceptIncoming", QT_TR_NOOP("Whether other users can connect to you. Choose \"Unable\" only when no port can be opened: OctoMX then counts as firewalled and downloads only from users who accept connections.")},
        {"connection/randomizePorts", QT_TR_NOOP("Use a new random port for the OctoMX Peer Network and BitTorrent at every start, instead of the fixed ones. Port forwarding by hand then stops working.")},
        {"connection/firewalled", QT_TR_NOOP("Whether others can reach you. Detect automatically watches for incoming connections; choose one of the others only when the detection is wrong.")},
        {"connection/ipv6", QT_TR_NOOP("Use IPv6 addresses when your line has them: BitTorrent, the OctoMX network, Rats and HTTP / web downloads. eD2k, Kad and Gnutella are IPv4 only. Off: no network uses IPv6. Applies at once.")},
        {"connection/bindInterface", QT_TR_NOOP("The network adapter (its name, for example a VPN adapter, or its address) every network uses: all connections, datagrams, web requests, the update check and port mapping. When it is missing or loses its address, every network stops until it is back; nothing uses another adapter. The OctoMX network and Rats run with it only while the internet route goes through it. Name lookups use the system's DNS. Empty uses every adapter. Applies at once.")},
        {"connection/proxyType", QT_TR_NOOP("Send every network through a proxy server: BitTorrent, eD2k servers and peers, Gnutella, HTTP, web indexes, the update check. Kad, the OctoMX network, Rats and the UDP parts of eD2k and Gnutella are off while a proxy is set (they cannot go through one); BitTorrent's DHT and uTP only through a SOCKS5 proxy that relays UDP. Nobody can connect to you through a proxy. Nothing connects directly. Applies at once.")},
        {"connection/proxyHost", QT_TR_NOOP("The proxy server's address, or address:port when its port is not 1080. Empty with a proxy chosen: every network stays off.")},
        {"connection/proxyPort", QT_TR_NOOP("The proxy server's address, or address:port when its port is not 1080. Empty with a proxy chosen: every network stays off.")},
        {"connection/proxyUser", QT_TR_NOOP("The user name the proxy asks for. Empty when the proxy needs no login.")},
        {"connection/proxyPassword", QT_TR_NOOP("The password the proxy asks for. It is stored in your profile as typed.")},
        {"connection/proxyHostnames", QT_TR_NOOP("Let the SOCKS5 proxy look up server, tracker and web names, so this computer makes no name lookups of its own for them (and the Connection Test does not look them up either). An HTTP proxy always does.")},
        {"connection/connectTimeoutSec", QT_TR_NOOP("How long OctoMX waits for another computer to answer a connection attempt before it gives up: eD2k peers and Gnutella. BitTorrent, HTTP and the web indexes have their own time outs.")},
        {"connection/upnp", QT_TR_NOOP("Ask your router to forward the ports to this computer by itself (UPnP and NAT-PMP, IPv4 only), so you need not do it by hand. Not while a proxy is set; with a chosen adapter only on that adapter.")},
        {"connection/natpmp", QT_TR_NOOP("Ask your router to forward the ports to this computer by itself (UPnP and NAT-PMP, IPv4 only), so you need not do it by hand. Not while a proxy is set; with a chosen adapter only on that adapter.")},
        {"network/connectOnStart", QT_TR_NOOP("Join the networks by itself at every start. Otherwise connect from the Networks page.")},
        // ---- OctoMX Peer Network
        {"overlay/enabled", QT_TR_NOOP("Search, chat and share on the OctoMX Peer Network, the network of OctoMX users.")},
        {"overlay/maxPeers", QT_TR_NOOP("How many other OctoMX users may open a direct link to this computer (the links it opens itself are not counted). More links find more users but cost a little bandwidth. Applies when the OctoMX network connects again.")},
        {"overlay/bootstrapPeers", QT_TR_NOOP("Addresses (host:port) to join through, besides the ones OctoMX finds by itself. Separate several with ;. Usually empty.")},
        // ---- BitTorrent
        {"bt/enabled", QT_TR_NOOP("Search for and download torrents, and seed them to other BitTorrent users.")},
        {"bt/listenPort", QT_TR_NOOP("The port other BitTorrent users connect to. 0 picks a free random port at every start.")},
        {"bt/dht", QT_TR_NOOP("Find peers without a tracker through the BitTorrent DHT. Needed for most magnet links.")},
        {"bt/utp", QT_TR_NOOP("Also talk to peers over uTP, which yields to other traffic on your line. Turn it off only when uTP connections fail.")},
        {"bt/encryption", QT_TR_NOOP("Prefer encrypts connections when the peer can. Require refuses plain peers (fewer sources). It hides the content from simple filtering, not your address.")},
        {"bt/maxConnections", QT_TR_NOOP("Connections to BitTorrent peers for all torrents together. Lower it when your router struggles with many connections.")},
        {"bt/maxConnectionsPerTorrent", QT_TR_NOOP("Connections to peers for one torrent at most.")},
        {"bt/maxUploads", QT_TR_NOOP("How many peers receive data from you at the same time, for all torrents together, peers of your own network included. These are BitTorrent's own slots: Max total uploads (File Transfers) does not count them.")},
        {"bt/maxUploadsPerTorrent", QT_TR_NOOP("How many peers of one torrent receive data from you at the same time, peers of your own network included.")},
        {"bt/maxActiveDownloads", QT_TR_NOOP("How many torrents download at the same time; the others wait in the queue.")},
        {"bt/maxActiveSeeds", QT_TR_NOOP("How many complete torrents are seeded at the same time; the others pause.")},
        {"bt/seedRatioLimit", QT_TR_NOOP("Stop seeding a downloaded torrent when you have uploaded this many times its size. 0 seeds without a limit. Shared files offered as torrents are not limited.")},
        {"bt/seedTimeLimitMin", QT_TR_NOOP("Stop seeding a downloaded torrent after it has seeded this many minutes. 0 seeds without a limit. Shared files offered as torrents are not limited.")},
        {"bt/seedLimitAction", QT_TR_NOOP("What happens when a seed limit is reached: stop seeding and keep the entry, or remove it from the list (the files stay).")},
        {"bt/seedCompleted", QT_TR_NOOP("Keep giving complete torrents to other users. Untick it to stop each torrent when its download is complete. A finished torrent whose shared file you stop sharing stops seeding as well.")},
        {"bt/seedLibrary", QT_TR_NOOP("Also offer your shared files as torrents, so BitTorrent users can download them.")},
        {"bt/extraTrackers", QT_TR_NOOP("Trackers added to every magnet link, so a link with few trackers finds more peers. Separate several with ;.")},
        {"bt/dhtBootstrapNodes", QT_TR_NOOP("The DHT routers (host:port) used to join the DHT the first time. Separate several with ;. After a restart.")},
        {"bt/announceToAllTrackers", QT_TR_NOOP("Ask every tracker of a torrent for peers, not only the first one that answers.")},
        {"bt/validateHttpsTrackers", QT_TR_NOOP("Check the certificate of https trackers. Untick it only for a tracker you trust that has a broken certificate.")},
        {"bt/allowMultipleConnectionsPerIp", QT_TR_NOOP("Allow several peers behind the same address, for example several computers at one home or a shared network.")},
        {"bt/preallocate", QT_TR_NOOP("Reserve the whole size of a torrent on disk as soon as its first data is written. Avoids fragmented files, takes the space at once.")},
        {"bt/anonymousMode", nullptr}, {"bt/superSeeding", nullptr}, {"bt/lsd", nullptr}, {"bt/pex", nullptr}, {"bt/webSeeds", nullptr},
        // ---- eD2k / Kad
        {"ed2k/enabled", QT_TR_NOOP("Search and download on eD2k servers (the eDonkey / eMule network).")},
        {"ed2k/tcpPort", QT_TR_NOOP("The eD2k TCP port other clients connect to. Forward it in the router for a high ID. After a restart.")},
        {"ed2k/udpPort", QT_TR_NOOP("The eD2k UDP port, shared with Kad (Kad has no port of its own). No UDP at all while a proxy is set. After a restart.")},
        {"ed2k/maxConnections", QT_TR_NOOP("Connections to eD2k clients and servers at most.")},
        {"ed2k/maxSourcesPerFile", QT_TR_NOOP("How many eD2k sources one download keeps at most.")},
        {"ed2k/obfuscation", QT_TR_NOOP("Encrypt eD2k connections when the other side can, so simple filtering by a provider does not see what is transferred. Off: plain connections only, both ways. Kad packets are obfuscated whenever the contact supports it.")},
        {"ed2k/requireObfuscation", QT_TR_NOOP("Refuse every eD2k client and server that does not encrypt, and Kad connections over TCP. Fewer sources. Kad's own packets stay obfuscated where the contact supports it.")},
        {"ed2k/autoConnect", QT_TR_NOOP("Connect to an eD2k server by itself when the networks connect.")},
        {"ed2k/reconnect", QT_TR_NOOP("Connect to another server when the connection to the current one is lost.")},
        {"ed2k/staticServersOnly", QT_TR_NOOP("Only connect to servers marked static in the server list.")},
        {"ed2k/addServersFromServer", QT_TR_NOOP("Add the servers the current server tells about to the server list.")},
        {"ed2k/autoUpdateServerList", QT_TR_NOOP("Download a fresh server list from the address below at every start.")},
        {"ed2k/serverListUrl", QT_TR_NOOP("Where the server list (server.met) is downloaded from.")},
        {"ed2k/uploadSlots", QT_TR_NOOP("eD2k clients that download from you at the same time, kept apart for eD2k. 0: eD2k shares Max total uploads (File Transfers) with the OctoMX Peer Network and Gnutella.")},
        {"ed2k/maxHalfOpen", QT_TR_NOOP("Connection attempts that may wait for an answer at the same time. Old routers and Windows versions need a low value.")},
        {"ed2k/maxConnectsPer5s", QT_TR_NOOP("New eD2k connections in five seconds at most.")},
        {"ed2k/parallelServerConnects", QT_TR_NOOP("How many servers are tried at the same time when connecting.")},
        {"ed2k/compressUploads", QT_TR_NOOP("Compress the data sent to clients that support it when it gets smaller.")},
        {"ed2k/udpPing", QT_TR_NOOP("Ask servers over UDP how many users and files they have, for the server list.")},
        {"ed2k/udpSearch", QT_TR_NOOP("Also search the other servers of the list over UDP, not only the one you are connected to.")},
        {"ed2k/udpSources", QT_TR_NOOP("Ask the other servers of the list over UDP for more sources of a download.")},
        {"kad/enabled", QT_TR_NOOP("Search and find sources on Kad, the serverless part of eD2k.")},
        {"kad/autoBootstrap", QT_TR_NOOP("Join Kad with the contacts of a nodes.dat downloaded from the address below.")},
        {"kad/nodesDatUrl", QT_TR_NOOP("Where nodes.dat, the list of Kad contacts to join with, is downloaded from.")},
        {"kad/bootstrapNodes", QT_TR_NOOP("Kad contacts (host:port) to join through, besides nodes.dat. Separate several with ;.")},
        {"kad/publish", QT_TR_NOOP("Tell Kad which files you share, so others find you as a source.")},
        {"kad/storeIndex", QT_TR_NOOP("Keep the keywords and sources other Kad users publish to you, as every Kad node does.")},
        {"kad/firewallChecks", QT_TR_NOOP("Ask other Kad nodes whether they can reach you, to know whether you are firewalled.")},
        {"kad/buddies", QT_TR_NOOP("A firewalled user finds a buddy through which others can reach him; an open user may be such a buddy.")},
        {"kad/maxPacketsPerSec", QT_TR_NOOP("Kad packets sent per second at most.")},
        // ---- Gnutella
        {"gnutella/enabled", QT_TR_NOOP("Search and download on Gnutella, as a leaf of a few ultrapeers.")},
        {"gnutella/port", QT_TR_NOOP("The port other Gnutella users connect to. After a restart.")},
        {"gnutella/maxUltrapeers", QT_TR_NOOP("How many ultrapeers OctoMX keeps as a leaf. More find more results and cost a little bandwidth.")},
        {"gnutella/browseHost", QT_TR_NOOP("Let Gnutella users see the list of your shared files.")},
        {"gnutella/autoBootstrap", QT_TR_NOOP("Find ultrapeers through the web caches and UDP host caches below. Untick it to use only the bootstrap hosts.")},
        {"gnutella/gwebcaches", QT_TR_NOOP("Web caches that tell Gnutella clients where ultrapeers are. Separate several with ;.")},
        {"gnutella/udpHostCaches", QT_TR_NOOP("UDP host caches (host:port) besides the built-in ones. Separate several with ;.")},
        {"gnutella/bootstrapHosts", QT_TR_NOOP("Ultrapeers (host:port) to connect to first. Separate several with ;.")},
        {"gnutella/uploadSlots", QT_TR_NOOP("Gnutella users that download from you at the same time, kept apart for Gnutella. 0: Gnutella shares Max total uploads (File Transfers) with the OctoMX Peer Network and eD2k.")},
        {"gnutella/uploadQueueSize", QT_TR_NOOP("How many Gnutella requests wait in your queue when the slots are busy.")},
        {"gnutella/uploadQueuePollSec", QT_TR_NOOP("How often a queued Gnutella user must ask again to keep his place.")},
        {"gnutella/encryption", nullptr},
        // ---- HTTP / web / rats
        {"http/enabled", QT_TR_NOOP("Download from web and FTP addresses, and use web sources of torrents and eD2k links.")},
        {"http/maxConnectionsPerServer", QT_TR_NOOP("Connections to one web server at most, for all downloads together.")},
        {"http/connectionsPerSource", QT_TR_NOOP("Parts of one file fetched from the same web source at the same time.")},
        {"http/timeoutSec", QT_TR_NOOP("How long a web server may take to answer before the request is given up.")},
        {"http/stallTimeoutSec", QT_TR_NOOP("A web request that sends nothing for this long is started again.")},
        {"http/maxRetries", QT_TR_NOOP("How often a failed web request is tried again before the source is dropped.")},
        {"http/retryBaseMs", QT_TR_NOOP("The wait before the first retry; each further retry waits longer.")},
        {"http/verifyTls", QT_TR_NOOP("Check the certificates of https servers. Untick it only for a server you trust that has a broken certificate.")},
        {"http/allowInsecureRedirect", QT_TR_NOOP("Follow an https address that sends you on to a plain http one. Off: such downloads fail, which is safer.")},
        {"http/userAgent", QT_TR_NOOP("The program name OctoMX gives web servers. Change it only for a server that refuses unknown programs.")},
        {"web/enabled", QT_TR_NOOP("Also search the public web torrent indexes. Their results are torrents: BitTorrent downloads them.")},
        {"web/maxResults", QT_TR_NOOP("Results per search taken from the web indexes at most.")},
        {"web/timeoutSec", QT_TR_NOOP("How long a web index may take to answer a search.")},
        {"web/trackers", QT_TR_NOOP("Trackers added to the magnet links of web results. Empty: the extra trackers of the BitTorrent page.")},
        {"web/adultKeywords", QT_TR_NOOP("Words that mark a result as adult for \"Filter adult content\" (Search Options). Empty: the built-in list.")},
        {"web/userAgent", QT_TR_NOOP("The program name given to web indexes. Empty: the built-in one.")},
        {"web/probeOnStart", nullptr},
        {"rats/enabled", QT_TR_NOOP("Search the rats-search network, a peer-to-peer index of torrents.")},
        {"rats/safeSearch", QT_TR_NOOP("Ask rats-search peers to leave out adult results.")},
        {"rats/listenPort", QT_TR_NOOP("The port other rats-search peers connect to (a free one is taken when it is busy). After a restart.")},
        {"rats/dhtPort", QT_TR_NOOP("The DHT port of the rats-search network. 0 picks a random one. After a restart.")},
        {"rats/maxPeers", QT_TR_NOOP("rats-search peers that may connect to you at most (the ones OctoMX calls itself are not counted). Applies when Rats starts again.")},
        {"rats/maxResults", QT_TR_NOOP("Results per search taken from rats-search at most (the search's own limit still applies).")},
        {"rats/bootstrapPeers", QT_TR_NOOP("rats-search peers (host:port) dialled at start, besides the ones the DHT finds. Separate several with ;.")},
        {"other/defaultLogin", QT_TR_NOOP("The user name OctoMX gives eD2k servers and other users there. Empty: your screen name.")},
        // ---- messages / chat / hotlist
        {"messages/allowFrom", QT_TR_NOOP("Who may send you private messages: anyone, or only the kinds of users ticked below.")},
        {"messages/allowFromHotlist", QT_TR_NOOP("With \"Only allow messages from the following sources\": users on your Hotlist may write to you.")},
        {"messages/allowFromDownloadingFrom", QT_TR_NOOP("With \"Only allow messages from the following sources\": users you download from may write to you.")},
        {"messages/allowFromUploadingTo", QT_TR_NOOP("With \"Only allow messages from the following sources\": users who download from you may write to you.")},
        {"messages/allowFromSameRoom", QT_TR_NOOP("With \"Only allow messages from the following sources\": users in a chat room you are in may write to you.")},
        {"messages/allowPrivateMessages", QT_TR_NOOP("Who may send you private messages on the OctoMX Peer Network.")},
        {"messages/allowBrowse", QT_TR_NOOP("Who may see the list of your shared files: users of the OctoMX Peer Network, and Gnutella users (they are anonymous, so Hotlist and Nobody both refuse every Gnutella browse). eD2k users cannot browse OctoMX.")},
        {"messages/allowWhois", QT_TR_NOOP("Tell other users of the OctoMX Peer Network your connection, files and client when they ask (Whois).")},
        {"messages/blockFloods", QT_TR_NOOP("Mute a user who sends many messages in a short time, in private and in chat rooms.")},
        {"messages/floodLimit", QT_TR_NOOP("Messages in 10 seconds after which a sender is muted (with \"Block message floods\").")},
        {"messages/filterWords", QT_TR_NOOP("Messages that contain one of these words are not shown. Separate several with ;.")},
        {"messages/openWindowOnMessage", QT_TR_NOOP("Open a Private Message window by itself when someone writes to you. Off: the alert still plays and the message waits; it is shown when you open a message window to that user.")},
        {"messages/flashOnMessage", QT_TR_NOOP("Flash the message window (or OctoMX's taskbar button) when a private message arrives.")},
        {"messages/soundOnMessage", QT_TR_NOOP("Play the Incoming Message Alert (Incoming Msgs & Privacy) when a private message arrives.")},
        {"messages/autoReply", QT_TR_NOOP("Answer every new private message with the text below, for example while you are away.")},
        {"messages/autoReplyText", QT_TR_NOOP("The automatic answer (with \"Send an automatic reply\").")},
        {"messages/ignoreNonHotlist", nullptr},
        {"hotlist/notifyOnline", QT_TR_NOOP("Say in the status bar and the tray when a user of your Hotlist comes online.")},
        {"hotlist/soundOnline", QT_TR_NOOP("Also play the Incoming Message Alert when a Hotlist user comes online (with the notification above).")},
        {"hotlist/autoBrowse", QT_TR_NOOP("A double-click on a Hotlist user opens his file list instead of a message window.")},
        {"chat/fetchListImmediately", QT_TR_NOOP("Load the channel list as soon as the list is opened, instead of waiting for Refresh.")},
        {"chat/topicInFilter", QT_TR_NOOP("The channel list filter also looks at the topics, not only the channel names.")},
        {"chat/allowLargeBackscroll", QT_TR_NOOP("Keep more than 1000 lines in a channel window (see Lines kept per room). Resizing such windows gets slow.")},
        {"chat/showJoinsParts", QT_TR_NOOP("Show a line when a user joins or leaves a room.")},
        {"chat/showTimestamps", QT_TR_NOOP("Start every chat line with the time it arrived.")},
        {"chat/timestampFormat", QT_TR_NOOP("How the time looks: hh hours, mm minutes, ss seconds (for example hh:mm:ss).")},
        {"chat/backlogLines", QT_TR_NOOP("Lines a room window keeps; older ones are dropped. Above 1000 only with the box on the Channels page.")},
        {"chat/fontFamily", QT_TR_NOOP("The font of chat rooms and private messages, when this computer has it; else the standard font. Open windows follow at once.")},
        {"chat/fontSize", QT_TR_NOOP("The font size of chat rooms and private messages.")},
        {"chat/autoJoinFavourites", QT_TR_NOOP("Join your favourite rooms by themselves when the OctoMX Peer Network connects.")},
        {"chat/defaultRoom", QT_TR_NOOP("A room joined at every connect. Empty: none.")},
        // ---- search
        {"search/typeButtonAudioLabel", QT_TR_NOOP("The caption of the first quick type button on the Search page.")},
        {"search/typeButtonVideoLabel", QT_TR_NOOP("The caption of the second quick type button on the Search page.")},
        {"search/typeButtonPicturesLabel", QT_TR_NOOP("The caption of the third quick type button on the Search page.")},
        {"search/groupsExpanded", QT_TR_NOOP("Whether a result that several users have shows its users (source rows) at once or only after a click on [+].")},
        {"search/maxResults", QT_TR_NOOP("Results one search collects at most from all networks.")},
        {"search/mergeByNameAndSize", QT_TR_NOOP("Show results of different networks as one row when they have exactly the same size, the same extension and nearly the same name, even without a common hash. Every block is still checked while downloading. Off: besides results with the same hash, only plain files with exactly the same name and size are joined, the classic rule (both above the size on the Search Networks page).")},
        {"search/timeoutSec", QT_TR_NOOP("A search stops asking the networks after this time; results keep their sources.")},
        {"search/showFullPaths", QT_TR_NOOP("Show the folder of a result as the other user shares it, not only the file name.")},
        {"search/fakeFilter", QT_TR_NOOP("Leave out results that disguise a program as media, such as \"song.mp3.exe\".")},
        {"search/flagSuspects", QT_TR_NOOP("Show results that look like fakes in their own colour, with the reason in the tool tip.")},
        {"search/hideSuspects", QT_TR_NOOP("Leave out the results marked as likely fakes altogether.")},
        {"search/dropTorrentBridges", QT_TR_NOOP("Leave out eD2k entries that only point to a torrent or Kad hash and cannot be downloaded as such.")},
        {"search/adultFilter", QT_TR_NOOP("Leave out results whose names contain adult words (list on the Web Search page).")},
        {"search/autoComplete", QT_TR_NOOP("Complete the search text from earlier searches while you type.")},
        {"search/saveHistory", QT_TR_NOOP("Remember the searches you made, also after a restart.")},
        {"search/historySize", QT_TR_NOOP("How many searches are remembered.")},
        {"search/networks", QT_TR_NOOP("The networks a search asks, by name, separated by ;. Empty asks every network that is on.")},
        {"search/blockedExtensions", QT_TR_NOOP("Results with these extensions are not shown, for example programs that pretend to be music. Separate several with ;.")},
        {"search/minCandidateSize", QT_TR_NOOP("Files of the same name and size above this size count as one file from several users (checked by hash when downloaded).")},
        // ---- transfers / queue / bandwidth
        {"transfers/switchToTransfers", QT_TR_NOOP("Show the Transfers page as soon as a download is added.")},
        {"transfers/newDownloadsExpanded", QT_TR_NOOP("Show the sources of every download at once, also after a restart. Unticked, a download is one line and its sources open with a click on the plus sign.")},
        {"transfers/suppressDangerousWarning", QT_TR_NOOP("Never ask before opening programs, scripts and archives you downloaded. Risky: they may contain a virus.")},
        {"transfers/autoClearCompleted", QT_TR_NOOP("A finished download leaves the Transfers list after the time on the right. The file stays on disk.")},
        {"transfers/clearCompleteDownloadsSec", QT_TR_NOOP("How long a finished download stays in the list (with the box on the left ticked).")},
        {"transfers/autoClearUploads", QT_TR_NOOP("A finished upload leaves the upload list after the time on the right.")},
        {"transfers/clearCompleteUploadsSec", QT_TR_NOOP("How long a finished upload stays in the list (with the box on the left ticked).")},
        {"transfers/pendingDownloadTimeoutSec", QT_TR_NOOP("A download whose sources do not answer within this time is marked as timed out and tried again later.")},
        {"transfers/maxDownloads", QT_TR_NOOP("Files that download at the same time; the others wait (Locally Queued). Set for your connection type.")},
        {"transfers/maxUploads", QT_TR_NOOP("Users that download from you at the same time: one total for the OctoMX Peer Network, eD2k and Gnutella together, shared fairly between them (eD2k or Gnutella keep their own number apart when their page sets one). 0: nobody downloads from you on these networks. BitTorrent has its own upload slots (BitTorrent page). Set for your connection type.")},
        {"transfers/maxUploadsPerUser", QT_TR_NOOP("Files one user of the OctoMX Peer Network may download from you at the same time.")},
        {"transfers/autoRetry", QT_TR_NOOP("Try a failed download again by itself after the time below.")},
        {"transfers/retryIntervalSec", QT_TR_NOOP("The wait before a failed download is tried again.")},
        {"transfers/retryMaxAttempts", QT_TR_NOOP("How often a failed download is tried again. 0 tries for ever.")},
        {"transfers/sourceSearchIntervalMin", QT_TR_NOOP("How often a download that needs sources searches for more.")},
        {"transfers/maxSourcesPerDownload", QT_TR_NOOP("Sources one download takes data from at the same time, from all networks. Further sources it knows wait and take over when one stops (BitTorrent peers are set on the BitTorrent page).")},
        {"transfers/metadataTimeoutSec", QT_TR_NOOP("A magnet link whose torrent information does not arrive within this time also uses the file's other sources.")},
        {"transfers/minFreeSpaceMB", QT_TR_NOOP("Downloads pause when the disk has less free space than this, so the disk never runs full.")},
        {"transfers/verifyCompleted", QT_TR_NOOP("Check a finished file against its hash before it counts as complete.")},
        {"transfers/preallocate", QT_TR_NOOP("Reserve the whole size of a download on disk when it starts.")},
        {"transfers/startPaused", QT_TR_NOOP("New downloads wait paused until you resume them.")},
        {"transfers/confirmCancel", QT_TR_NOOP("Ask before a download is cancelled and its partial file deleted.")},
        {"transfers/removeAction", QT_TR_NOOP("What happens to the file when you remove a download from the Transfers list (Cancel, Clear, Del). Ask: a question each time. Keep the file: a finished file stays, a partial one can be continued later. Delete the file too: it goes to the trash or Recycle Bin; where there is none you are still asked. Default: Ask.")},
        {"transfers/shareCompleted", QT_TR_NOOP("Share finished downloads (the Incoming Files Folder) with other users, under the same rules as your shared folders. Finished torrents keep seeding as set on the Torrent Seeding page; one that was shared here stops seeding when its file stops being shared.")},
        {"transfers/uploadsToHotlistOnly", nullptr},
        {"queue/changeSpeedWhenBusy", QT_TR_NOOP("While this many uploads run on the OctoMX Peer Network, other users see the connection type on the right instead of yours, so fewer of them queue.")},
        {"queue/changeSpeedThreshold", QT_TR_NOOP("The number of uploads from which the other connection type is shown (with the box on the left).")},
        {"queue/changedSpeed", QT_TR_NOOP("The connection type others see while you are busy (with the box on the left).")},
        {"queue/enabled", QT_TR_NOOP("Users of the OctoMX Peer Network who ask while every upload slot is busy wait in a queue instead of being refused (eD2k and Gnutella keep their own queues).")},
        {"queue/maxLength", QT_TR_NOOP("Requests that may wait in your OctoMX Peer Network queue at most; more are refused.")},
        {"queue/maxPerUser", QT_TR_NOOP("Files one user may have in your OctoMX Peer Network queue at most.")},
        {"queue/timeoutMin", QT_TR_NOOP("A queued user who does not ask again within this time loses his place (OctoMX asks again every minute while it waits, so at least 2 minutes).")},
        {"queue/smallFileBypassKB", QT_TR_NOOP("Files smaller than this are sent at once on the OctoMX Peer Network, without waiting in the queue. 0: none.")},
        {"queue/sendPosition", QT_TR_NOOP("Tell queued users their place in the queue (they see \"Remotely Queued (3)\" instead of \"Remotely Queued\").")},
        {"queue/hotlistPriority", nullptr},
        {"bandwidth/graphAverageSec", QT_TR_NOOP("The Bandwidth graphs show the average of this many seconds: longer is smoother, shorter shows every peak.")},
        {"bandwidth/scheduleEnabled", QT_TR_NOOP("Use the limits below on the chosen days and times, for example less upload while you work.")},
        {"bandwidth/scheduleDownKBs", QT_TR_NOOP("The download limit during the scheduled time. 0 = no limit.")},
        {"bandwidth/scheduleUpKBs", QT_TR_NOOP("The upload limit during the scheduled time. 0 = no limit (uploads never go below 10 KB/s).")},
        {"bandwidth/scheduleStart", QT_TR_NOOP("When the scheduled limits start (hours:minutes).")},
        {"bandwidth/scheduleEnd", QT_TR_NOOP("When the scheduled limits end (hours:minutes). Earlier than the start: the time runs over midnight.")},
        {"bandwidth/scheduleDays", QT_TR_NOOP("The days the schedule applies.")},
        {"bandwidth/limitLan", QT_TR_NOOP("Also limit transfers with computers of your own network. Off: they run at full speed. A change applies to new connections.")},
        // ---- launching / library / misc
        {"launch/entries", QT_TR_NOOP("The extra entries of the Transfers menu for complete files: a title, the extensions (* = all), and a program with its parameters, or a Windows verb such as Enqueue.")},
        {"launch/useOsDefault", QT_TR_NOOP("Open files with the programs your system uses for their type. Untick it to use the programs below.")},
        {"launch/doubleClickAction", QT_TR_NOOP("What a double-click on a complete download does.")},
        {"launch/previewIncomplete", QT_TR_NOOP("Offer Preview for audio and video downloads that are not complete yet.")},
        {"launch/audioCommand", QT_TR_NOOP("The program for audio files when the system default is off, %1 stands for the file (else it is added at the end). Empty: the system default.")},
        {"launch/videoCommand", QT_TR_NOOP("The program for video files when the system default is off, %1 stands for the file. Empty: the system default.")},
        {"launch/imageCommand", QT_TR_NOOP("The program for pictures when the system default is off, %1 stands for the file. Empty: the system default.")},
        {"launch/documentCommand", QT_TR_NOOP("The program for documents when the system default is off, %1 stands for the file. Empty: the system default.")},
        {"launch/otherCommand", QT_TR_NOOP("The program for all other files when the system default is off, %1 stands for the file. Empty: the system default.")},
        {"launch/previewPlayer", nullptr}, {"launch/previewProgram", nullptr},
        {"library/watchFolders", QT_TR_NOOP("Notice new, changed and deleted files in shared folders at once, not only at the next rescan.")},
        {"library/shareHidden", QT_TR_NOOP("Also share hidden files and folders.")},
        {"library/shareUnlistedTypes", QT_TR_NOOP("Share files whose type is in none of the groups of Shared File Types too.")},
        {"library/rescanIntervalMin", QT_TR_NOOP("Look through the shared folders again at this interval. 0 only at start and when you press Refresh.")},
        {"library/hashThreads", QT_TR_NOOP("Files hashed at the same time. More is faster on a fast disk, slower on a single hard disk.")},
        {"library/excludePatterns", QT_TR_NOOP("Files matching one of these patterns are never shared (* = any text). Separate several with ;.")},
        {"library/shareIncomplete", nullptr},
        {"misc/closeAction", QT_TR_NOOP("What the X of the main window does: ask each time, keep OctoMX running in the tray, or exit.")},
        {"misc/startWithOs", QT_TR_NOOP("Start OctoMX hidden in the tray when you log in, so sharing and downloads go on.")},
        {"misc/startMinimized", QT_TR_NOOP("Start with the main window minimized to the taskbar.")},
        {"misc/trayBalloons", QT_TR_NOOP("Show notifications (download complete, Hotlist user online) as balloons from the tray icon. The status bar shows them anyway.")},
        {"misc/confirmExit", QT_TR_NOOP("Ask before OctoMX exits while downloads or uploads are running.")},
        {"misc/associateMagnet", QT_TR_NOOP("Clicking a magnet: link in the web browser starts the download in OctoMX.")},
        {"misc/associateEd2k", QT_TR_NOOP("Clicking an ed2k:// link in the web browser starts the download in OctoMX.")},
        {"misc/associateTorrent", QT_TR_NOOP("Opening a .torrent file starts the download in OctoMX.")},
        {"misc/checkAssociations", QT_TR_NOOP("At every start, register OctoMX again for the links and files ticked above when another program took them.")},
        {"misc/alwaysOnTop", QT_TR_NOOP("Keep the main window above other windows.")},
        {"misc/flashTaskbar", QT_TR_NOOP("Flash OctoMX's taskbar button when a download completes or a Hotlist user comes online while OctoMX is in the background.")},
        {"misc/sounds", QT_TR_NOOP("Switches every sound of OctoMX on or off: message alerts, Hotlist and download sounds.")},
        {"misc/soundDownloadComplete", QT_TR_NOOP("Play the built-in chime when a download completes (with \"Play sounds\").")},
        {"misc/logLevel", QT_TR_NOOP("How much the log file (logs/octomx.log in your profile) records. Warning: problems only. Info: also searches, downloads and connections. Debug: every detail, only when you look for a fault (it grows fast). No level records the text of a message or a password.")},
        {"misc/logMaxSizeMB", QT_TR_NOOP("When the log file is larger than this, it is renamed to octomx.1.log and a new one starts.")},
        {"misc/logKeepFiles", QT_TR_NOOP("How many old log files (octomx.1.log ...) are kept.")},
        {"ui/restoreLastPage", QT_TR_NOOP("Open the page that was shown when OctoMX was closed, instead of the Networks page.")},
        {"ui/showGrid", QT_TR_NOOP("Draw lines between the rows and columns of every list, in the Grid Lines colour (Appearance, Main Window).")},
        {"ui/scale", nullptr}, {"ui/smoothFonts", nullptr}, {"ui/exactSizes", nullptr}, {"ui/searchHistoryMenu", nullptr},
        {"ui/notifyDownloadDone", nullptr}, {"ui/colorTheme", nullptr}, {"updates/mode", nullptr},
        {"transfers/sequentialDefault", nullptr}, {"transfers/firstLastDefault", nullptr},
    };
    return t;
}

QString defaultText(const SettingDef &d)
{
    const QVariant v = d.defaultValue;
    switch (d.type) {
    case SettingType::Bool:
        return v.toBool() ? QObject::tr("on") : QObject::tr("off");
    case SettingType::Int:
    case SettingType::Double:
        return v.toString();
    case SettingType::Enum: {
        const int i = int(d.choices.indexOf(v.toString()));
        return i >= 0 && i < d.choiceLabels.size() ? d.choiceLabels.at(i) : v.toString();
    }
    case SettingType::String:
    case SettingType::Path:
        return v.toString().isEmpty() ? QObject::tr("empty") : v.toString();
    case SettingType::StringList:
        return v.toStringList().isEmpty() ? QObject::tr("empty") : QObject::tr("the built-in list");
    default:
        return QString();
    }
}

} // namespace

bool hasSettingTip(const QString &key)
{
    return tipTable().contains(key);
}

QString settingTip(const QString &key)
{
    const SettingDef *d = findSetting(key);
    const auto it = tipTable().constFind(key);
    QString t = it != tipTable().constEnd() && it.value() ? QObject::tr(it.value()) : QString();
    if (t.isEmpty()) {
        // keys whose page sets its own tip (nullptr above), and the Advanced page: the schema label
        if (!d)
            return QString();
        t = d->label + QLatin1Char('.');
    }
    if (d && !t.contains(QLatin1String("default"), Qt::CaseInsensitive)) {
        const QString def = defaultText(*d);
        if (!def.isEmpty() && def.size() < 60)
            t += QLatin1Char(' ') + QObject::tr("Default: %1.").arg(def);
    }
    // [set-1] every setting that waits for the next start says so at the control
    if (d && d->restartRequired && !t.contains(QLatin1String("restart"), Qt::CaseInsensitive))
        t += QLatin1Char(' ') + QObject::tr("Takes effect after a restart.");
    return t;
}

} // namespace mx::ui
