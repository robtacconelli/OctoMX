// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX: interfaces between the core engine and the network plugins.
// Each protocol module (src/net/<name>) provides one NetworkPlugin per mx::Net value it handles.
// Threading: the plugin facade lives on the main thread; all methods are called there and all
// signals are emitted there. Internally a plugin may use its own threads / async sockets, but it
// must never block the caller for more than a few milliseconds.
// Owner: coordinator. Additive changes only.
#pragma once

#include <QtCore/QBitArray>
#include <QtCore/QObject>
#include <QtCore/QPair>
#include "mx/diagnostics.h"
#include "mx/services.h"
#include "mx/types.h"

class QIODevice;
class QHostAddress;

namespace mx {

class RangeSource;
class RateLimiter;

// ---------------------------------------------------------------- what the core gives a plugin
class LibraryAccess {                       // read access to shared (and partially downloaded) files
public:
    virtual ~LibraryAccess() = default;
    virtual QList<SharedFile> sharedFiles() const = 0;
    // Find a shared file by any hash. Returns false if unknown.
    virtual bool findByHash(const FileHashes &h, SharedFile *out) const = 0;
    // Thread-safe positional read of a shared file (or of the verified part of a running
    // download). Returns the bytes read (may be short / empty if the range is not available).
    virtual QByteArray read(const FileHashes &h, qint64 offset, qint64 length) = 0;
    // Hash sets for serving (eD2k part hashes, AICH, TTH leaves), computed by the library hasher.
    // [up-1] Only sets that are known to be right: the library's (hashed over the complete file) or,
    // for a running download, the trusted set its own parts are checked against. Never a set
    // computed over a partial file, never a provisional one.
    virtual QList<QByteArray> ed2kPartHashes(const FileHashes &h) const = 0;
    // [added by up-1] true when h names a file that is still downloading and is served from its
    // verified part: *have = the byte ranges [first, second) that read() delivers now. false = a
    // complete shared file (every range) or not known. Thread-safe like read().
    virtual bool partialRanges(const FileHashes &h, QList<QPair<qint64, qint64>> *have) const
    { Q_UNUSED(h) Q_UNUSED(have) return false; }
};

struct PluginContext {
    Settings      *settings = nullptr;
    LibraryAccess *library = nullptr;
    RateLimiter   *limiter = nullptr;       // shared up/down token buckets for non-libtorrent traffic
    QString        profileDir;              // <profile>
    QString        dataDir;                 // <profile>/<net>, plugin-private persistent state
                                            // (known nodes, server lists, keys). Create on demand.
    QString        screenName;
    QString        clientName;              // "OctoMX"
    QString        clientVersion;           // MX_VERSION_STRING
    // [diag-1] true: the core's shared port mapper (UPnP / NAT-PMP) forwards every port a plugin
    // lists in diagnostics().bindings with mapByCore, so the plugin must not map ports itself
    // (libtorrent / librats mapping off). false (tests, old cores): plugins map as before.
    bool           coreMapsPorts = false;
};

// ---------------------------------------------------------------- diagnostics [added by diag-1]
// One listening socket of a plugin.
struct PortBinding {
    QString proto;                 // "TCP" or "UDP"
    int     port = 0;              // the port in use (or the one wanted, when binding failed)
    bool    ok = false;
    QString error;                 // the OS error text when binding failed
    QString purpose;               // "peers", "DHT and uTP", "Kad and client UDP", ...
    bool    mapByCore = true;      // forward it with the core's port mapper (connection/upnp, natpmp)
};
// A port mapping on the router (made by the core's mapper, or by a plugin that maps itself).
struct PortMapping {
    enum Result { Pending = 0, Mapped, Failed, Off, Removed };
    Net     net = Net::Unknown;    // the network the port belongs to
    QString proto;                 // "TCP" / "UDP"
    int     internalPort = 0;
    int     externalPort = 0;      // when Mapped (may differ from internalPort)
    QString method;                // "UPnP", "NAT-PMP", "" (none)
    Result  result = Pending;
    QString error;                 // router's answer ("500 Internal Server Error", "no answer", ...)
    QDateTime when;                // last attempt / success
    QDateTime expires;             // lease end (invalid = permanent / unknown)
};
enum class Reach : int { Unknown = 0, Yes, No };
// What a plugin knows about its own connectivity (cheap, cached; called on the main thread).
struct PluginDiagnostics {
    bool    valid = false;         // false = the plugin reports nothing (defaults)
    QList<PortBinding> bindings;
    QList<PortMapping> ownMappings;   // mappings the plugin made itself (when !coreMapsPorts)
    Reach   tcpIn = Reach::Unknown;   // reachable from the internet over TCP / UDP
    QString tcpEvidence;              // "eD2k server assigned a HighID", "12 incoming peer connections"
    Reach   udpIn = Reach::Unknown;
    QString udpEvidence;
    QString externalAddress;          // our public address as this network sees it
    QString externalSeenBy;           // "eD2k server eMule Sunrise", "DHT", "tracker"
    int     externalUdpPort = 0;      // the UDP port others see (Kad), 0 = unknown
    QStringList hosts;                // host names this network depends on (DNS check)
    int     dhtNodes = -1;            // DHT / Kad routing table size (-1 = not a DHT network)
    int     dhtReplies = -1;          // DHT / Kad answers received so far (-1 = unknown)
    int     connectAttempts = -1;     // outgoing TCP connection attempts (session) and ...
    int     connectOk = -1;           // ... how many of them connected
    int     incomingConnections = -1; // incoming connections in the last 15 min (-1 = unknown)
    QList<DiagItem> items;            // network-specific extra checks
};
// Overlay "probe my ports": one port the requester wants tested and the helper's answer.
struct PortProbe {
    QString proto;                 // "TCP" / "UDP"
    int     port = 0;
    QString kind;                  // UDP: "dht" (the helper sends a DHT ping and waits for the
                                   // answer), else a datagram the requester must see
    Reach   reach = Reach::Unknown;
    QString helper;                // who tested ("OctoMX user Name_12345")
    QString error;
};



// Shared token-bucket limiter (implemented in src/core). Thread-safe.
class RateLimiter {
public:
    virtual ~RateLimiter() = default;
    enum Dir { Down = 0, Up = 1 };
    // Ask to move `bytes`; returns how many may be moved right now (0..bytes). Unlimited → bytes.
    virtual qint64 acquire(Dir d, qint64 bytes) = 0;
    // Account bytes moved (for the bandwidth graph), whether or not limited.
    virtual void account(Dir d, Net net, qint64 bytes) = 0;
    virtual int limitBps(Dir d) const = 0;  // 0 = unlimited
    // [added by core] The rate this network may use right now (0 = unlimited). For BitTorrent it is
    // its share of the global limit ([bw-2] recomputed every 100 ms, see src/core/rate_limiter.h);
    // the BitTorrent plugin polls it several times a second and applies it to libtorrent.
    virtual int budgetBps(Dir d, Net net) const { Q_UNUSED(net) return limitBps(d); }
    // [added by glue-3] Bandwidth graph accounting with a payload split. wireBytes = everything this
    // network moved (file data + protocol: handshakes, queries, DHT/Kad/tracker packets, headers);
    // payloadBytes = the file-data part of it. Payload may also be reported later with wireBytes = 0
    // for bytes already accounted as wire (e.g. once a packet is parsed). account(d, net, n) is the same as
    // accountTraffic(d, net, n, 0): "traffic of unknown kind" (counted in the totals only).
    virtual void accountTraffic(Dir d, Net net, qint64 wireBytes, qint64 payloadBytes)
    { Q_UNUSED(payloadBytes) account(d, net, wireBytes); }
    // [bw-1] One shared budget: the core charges every accounted wire byte that did not pass
    // acquire() (BitTorrent, Kad / UDP packets, protocol messages) to the same buckets, so the total
    // of all networks stays within the limit. BitTorrent's budgetBps() is its fair share of the
    // global limit (all of it while the others do not want more).
    // LAN peers are exempt unless bandwidth/limitLan: a plugin asks exempt(peer) as it moves data
    // (cheap, so a change of the setting applies at once); for an exempt peer it skips acquire() and
    // accounts with accountExempt() (counted in the graph, not charged to the budget).
    // [bw-2] accountExempt() is also for search and discovery traffic of every network (servers,
    // DHT, trackers, queries and their answers, overlay control): the limits are for file transfers
    // only, so this traffic is never slowed down and never charged; it is counted for the graph.
    virtual bool exempt(const QHostAddress &peer) const { Q_UNUSED(peer) return false; }
    virtual void accountExempt(Dir d, Net net, qint64 wireBytes, qint64 payloadBytes)
    { accountTraffic(d, net, wireBytes, payloadBytes); }
    // [bw-2] acquire() for one network: the core shares the limit fairly between the networks that
    // want more than they get (each gets its share while another one waits; what one does not use,
    // the others may take). A plugin that moves file data calls this one; acquire() is the same for
    // an unknown network.
    virtual qint64 acquireFor(Dir d, Net net, qint64 bytes) { Q_UNUSED(net) return acquire(d, bytes); }
    // [bw-2] a network that limits itself (libtorrent) tells how many bytes wait for its budget right
    // now; when that is more than it gets in a moment, it counts as wanting more (as a refused
    // acquireFor() does), so it gets an equal share
    virtual void reportWaiting(Dir d, Net net, qint64 bytes) { Q_UNUSED(d) Q_UNUSED(net) Q_UNUSED(bytes) }
};

// ---------------------------------------------------------------- a source of byte ranges
// One remote place that can deliver arbitrary byte ranges of ONE file (an eD2k peer, a Gnutella
// host, an HTTP URL, an overlay peer). Created by NetworkPlugin::openSource(), owned and driven by
// the core download engine, which splits the file's missing ranges among all sources.
// BitTorrent peers are NOT RangeSources: libtorrent drives them itself (see TorrentBackend).
class RangeSource : public QObject {
    Q_OBJECT
public:
    enum State { Idle = 0, Connecting, Queued, Ready, Busy, Failed, Closed };
    Q_ENUM(State)
    using QObject::QObject;
    virtual SearchSource descriptor() const = 0;
    virtual State state() const = 0;
    virtual QString stateText() const = 0;          // "Remotely Queued (17)", "Connecting..."
    virtual QString clientName() const { return QString(); }
    // Ranges [first, second) the remote has. Empty list while unknown; a single {0,size} = complete.
    virtual QList<QPair<qint64, qint64>> available() const = 0;
    // [up-1] A request that fails with this error asked for a range the remote does not hold (any
    // more): not a fault of the source; the engine asks again where available() says it can.
    static QString rangeNotAvailableError() { return QStringLiteral("Range not available"); }
    virtual qint64 preferredRequestSize() const { return 256 * 1024; }   // eD2k: multiples of 180 kB
    virtual int maxPendingRequests() const { return 1; }
    virtual void open() = 0;                        // connect / handshake / get into the queue
    // Only called in state Ready/Busy and with at most maxPendingRequests() outstanding.
    virtual void request(qint64 offset, qint64 length) = 0;
    virtual void cancelRequests() = 0;
    virtual void close() = 0;                       // then deleteLater() by the engine
    // [added by up-1] Why the engine stops using this source, said just before close() so a network
    // that can tell the uploader does (the OctoMX overlay's get_close reason). code: "done" (the
    // download is complete), "paused", "cancelled", "quit", "banned" (text = the reason shown to the
    // user), "dropped" (any other: slow, failing, not needed now).
    virtual void setCloseReason(const QString &code, const QString &text) { Q_UNUSED(code) Q_UNUSED(text) }
signals:
    void stateChanged();
    void availabilityChanged();
    // Payload; chunks of one request arrive in order and are contiguous.
    void data(qint64 offset, const QByteArray &bytes);
    void requestFinished(qint64 offset, qint64 length, bool ok, const QString &error);
    // [engine-3] Verification data THIS remote sent for the file (same kinds as
    // NetworkPlugin::hashSetLearned: "ed2k-parts", "tth-leaves" + unitSize, "aich-leaves").
    // Attributed to the source: the engine checks it against the trusted identity, uses a set
    // that reduces to no trusted hash only provisionally and bans the source if it proves wrong.
    void hashSetReceived(const QString &kind, qint64 unitSize, const QList<QByteArray> &hashes);
};

// ---------------------------------------------------------------- the plugin
class NetworkPlugin : public QObject {
    Q_OBJECT
public:
    enum Capability {
        CanSearch      = 0x01,   // keyword search
        CanFindSources = 0x02,   // locate sources for a known hash
        CanFetchRanges = 0x04,   // openSource() works
        CanServe       = 0x08,   // uploads shared files to others
        CanBrowse      = 0x10,
        CanChat        = 0x20,
        CanHashLinks   = 0x40,   // cross-network hash registry
    };
    using QObject::QObject;
    virtual Net net() const = 0;
    virtual int capabilities() const = 0;
    virtual void init(const PluginContext &ctx) = 0;   // once, before start(); load persisted state
    virtual void start() = 0;                          // go online (async)
    virtual void stop() = 0;                           // go offline, persist nodes/servers; fast
    virtual NetworkStatus status() const = 0;

    // --- servers / nodes list (optional)
    virtual QList<ServerEntry> servers() const { return {}; }
    virtual void addServer(const QString &address, const QString &name) { Q_UNUSED(address) Q_UNUSED(name) }
    virtual void removeServer(const QString &id) { Q_UNUSED(id) }
    virtual void connectServer(const QString &id) { Q_UNUSED(id) }
    virtual void disconnectServer(const QString &id) { Q_UNUSED(id) }

    // --- keyword search (CanSearch)
    virtual void search(const SearchQuery &q) { Q_UNUSED(q) }      // → searchHits*, searchDone
    virtual void cancelSearch(quint32 queryId) { Q_UNUSED(queryId) }

    // --- sources for a running download (CanFindSources). The plugin keeps looking (periodic
    // re-asks) until stopFindSources(). It may also learn more hashes of the same file.
    virtual void findSources(const QString &downloadId, const FileHashes &h, qint64 size,
                             const QString &name) { Q_UNUSED(downloadId) Q_UNUSED(h) Q_UNUSED(size) Q_UNUSED(name) }
    virtual void stopFindSources(const QString &downloadId) { Q_UNUSED(downloadId) }

    // --- byte-range transfer (CanFetchRanges). Returns nullptr if the source is not usable.
    virtual RangeSource *openSource(const QString &downloadId, const FileHashes &h, qint64 size,
                                    const SearchSource &src) { Q_UNUSED(downloadId) Q_UNUSED(h) Q_UNUSED(size) Q_UNUSED(src) return nullptr; }

    // --- serving (CanServe): the library changed; (re)publish. Data is read via ctx.library.
    virtual void setSharedFiles(const QList<SharedFile> &files) { Q_UNUSED(files) }

    // --- hash links (CanHashLinks): "which other hashes does the file with these hashes have?"
    virtual void lookupHashLinks(const FileHashes &known, qint64 size) { Q_UNUSED(known) Q_UNUSED(size) }
    virtual void publishHashLinks(const FileHashes &all, qint64 size, const QString &name) { Q_UNUSED(all) Q_UNUSED(size) Q_UNUSED(name) }

    // --- users (CanBrowse)
    virtual void browse(quint32 requestId, const UserRef &u) { Q_UNUSED(requestId) Q_UNUSED(u) }
    virtual void whois(const UserRef &u) { Q_UNUSED(u) }

    // --- uploads (CanServe) [added by core]: abort an upload reported via uploadChanged
    // (uploadId as reported by the plugin). The plugin then emits uploadFinished(uploadId).
    virtual void cancelUpload(const QString &uploadId) { Q_UNUSED(uploadId) }

    // --- [added by core, glue batch 1] generic hooks (the core cannot include plugin headers)
    // The user's hotlist (presence tracking, "hotlist only" privacy choices). The core calls it once
    // after start() and whenever the hotlist changes.
    virtual void setHotlist(const QList<UserRef> &users) { Q_UNUSED(users) }
    // [added by ovl-2] the local user as this network's other users see it (the OctoMX overlay: id = the
    // user key, name = "<screen name>_<5 digits from the key>"), invalid when the network has no own
    // user identity. The core takes the one screen name from it and recognises own rows by id.
    virtual UserRef localUser() const { return UserRef(); }
    // Lazily fetch the file list of a torrent bundle found by search (h.btih set). Plugins that can
    // answer emit bundleFiles(h, files) (possibly much later, or never); others ignore the call.
    virtual void requestBundleFiles(const FileHashes &h) { Q_UNUSED(h) }

    // --- [added by glue-2] more generic hooks; every default keeps old plugins working.
    // Edit a server/node entry. Default: remove + add (when the address changed) or re-add.
    virtual bool editServer(const QString &id, const QString &address, const QString &name)
    {
        removeServer(id);
        addServer(address, name);
        return true;
    }
    // Import a server list file's bytes (eD2k server.met, possibly gzip'ed, or the network's own
    // text format). Returns the number of servers added/updated, -1 = not understood (the core then
    // tries its own server.met / "host:port" text parser and calls addServer() per entry).
    virtual int importServers(const QByteArray &data) { Q_UNUSED(data) return -1; }
    // Ping a source/user with the network's own mechanism. Return false if unsupported (the core
    // then measures the TCP connect time to the source's address); true = pingResult(token, ms)
    // follows (ms = -1 for no answer).
    virtual bool ping(quint32 token, const SearchSource &src) { Q_UNUSED(token) Q_UNUSED(src) return false; }
    // Uploads reported via uploadChanged: give a queued one a slot now / reorder it
    // (priority -1 low, 0 normal, 1 high). false = unsupported for this upload.
    virtual bool startUploadNow(const QString &uploadId) { Q_UNUSED(uploadId) return false; }
    virtual bool setUploadPriority(const QString &uploadId, int priority) { Q_UNUSED(uploadId) Q_UNUSED(priority) return false; }

    // --- [added by diag-1] connection diagnostics; every default keeps old plugins working
    virtual PluginDiagnostics diagnostics() const { return {}; }
    // "Test Now": re-run the network's own reachability checks where the protocol allows it
    // (Kad firewall check, eD2k reconnect for a new ID, ...). Results show in diagnostics().
    virtual void recheckConnectivity() {}
    // The core's port mapper made (or lost) a mapping for one of this plugin's bindings. A plugin
    // that announces its port (BitTorrent trackers / DHT) should announce externalPort.
    virtual void portMappingChanged(const PortMapping &m) { Q_UNUSED(m) }
    // Overlay only: ask OctoMX peers to connect back to our public address on these ports.
    // false = unsupported / no peer to ask; true = portProbeResult(token, ...) follows.
    virtual bool probePorts(quint32 token, const QList<PortProbe> &ports) { Q_UNUSED(token) Q_UNUSED(ports) return false; }

signals:
    void statusChanged(const mx::NetworkStatus &st);
    void serversChanged();
    void searchHits(quint32 queryId, const QList<mx::SearchHit> &hits);
    void searchDone(quint32 queryId);
    void sourcesFound(const QString &downloadId, const QList<mx::SearchSource> &sources);
    // More hashes of the same file (e.g. AICH from an eD2k peer, TTH from a Gnutella hit).
    void hashesLearned(const QString &downloadId, const mx::FileHashes &more);
    // Verification data: kind = "ed2k-parts" (16-byte MD4 per 9 728 000-byte part),
    // "aich-leaves", "tth-leaves" (+ leaf size in `unitSize`).
    void hashSetLearned(const QString &downloadId, const QString &kind, qint64 unitSize,
                        const QList<QByteArray> &hashes);
    void hashLinksFound(const mx::FileHashes &known, const mx::FileHashes &all, qint64 size);
    // Uploads served by this plugin (isUpload = true; id unique per upload session).
    void uploadChanged(const mx::TransferInfo &upload);
    void uploadFinished(const QString &uploadId);
    void browseResults(quint32 requestId, const mx::UserRef &u, const QList<mx::BrowseEntry> &files, bool finished);
    void browseFailed(quint32 requestId, const mx::UserRef &u, const QString &reason);
    void whoisResult(const mx::UserRef &u, const QVariantMap &info);
    void logMessage(const QString &text);              // shown in the network log / status bar
    // [added by core] answer to requestBundleFiles(): the files of the bundle identified by h.btih
    void bundleFiles(const mx::FileHashes &h, const QList<mx::BundleFile> &files);
    // [added by glue-2] a message from a server (MOTD, OP_SERVERMESSAGE), shown in "View Server Messages"
    void serverMessage(const QString &serverId, const QString &text);
    // [added by glue-2] answer to ping()
    void pingResult(quint32 token, int ms);
    // [added by glue-2] we answered/routed `count` incoming search requests (the "SPM" figure)
    void incomingSearches(int count);
    // [added by diag-1] answer to probePorts() (one entry per requested port; reach Unknown when
    // nobody could test it)
    void portProbeResult(quint32 token, const QList<mx::PortProbe> &results);
};

// ---------------------------------------------------------------- BitTorrent backend
// Implemented by the BitTorrent plugin (the same object also is-a NetworkPlugin). libtorrent owns
// the files of torrent-backed downloads and verifies every piece; the core engine can feed it
// bytes obtained from other networks through injectPiece().
struct TorrentFile { QString path; qint64 size = 0; qint64 offset = 0; int priority = 4; qint64 done = 0; };
struct TorrentSnapshot {
    QString   id;                  // info-hash v1, 40 hex chars (lower case)
    QString   name;
    QString   savePath;
    bool      hasMetadata = false;
    qint64    totalSize = 0;       // of wanted files
    qint64    totalDone = 0;
    int       pieceLength = 0;
    int       numPieces = 0;
    QBitArray have;                // per piece
    // [ui-i] per piece: in libtorrent's download queue with at least one block requested from a
    // peer or being written now (empty when paused / not reported)
    QBitArray downloading;
    QList<TorrentFile> files;
    TransferState state = TransferState::FindingSources;
    QString   statusText;
    QString   errorText;
    int       rateDownBps = 0, rateUpBps = 0;
    qint64    uploadedBytes = 0;
    int       seeds = 0, peers = 0;          // connected
    int       seedsTotal = -1, peersTotal = -1; // swarm (scrape/DHT)
    QList<TransferSource> sources;           // connected peers (web seeds with net = Net::Http)
    // [engine-4] where the swarm's peers come from, one row each: the DHT, every tracker, PEX/LSD,
    // web seeds that are not connected. statusText in the original WinMX wording ("Working (12 seeds, 40 peers)",
    // "Waiting for Network Reply...", "Error: ..."); user.name = "DHT" / tracker host / seed host.
    // Empty from backends that do not report it (the engine then shows one summary row).
    QList<TransferSource> channels;
    bool      paused = false;
    bool      finished = false;
    QDateTime added;
};

// [diag-1] per-torrent connection counters for "why is this download slow" (cheap, cached).
struct TrackerDiag {
    QString host;
    QString url;
    bool    contacted = false;     // an announce was sent
    bool    working = false;       // the last announce succeeded
    bool    updating = false;      // an announce is in flight
    bool    dnsFailed = false;     // the host name did not resolve
    int     fails = 0;             // failures in a row
    int     peersReturned = -1;    // peers in the last reply (-1 = none yet)
    int     seeds = -1, leechers = -1;   // scrape
    QString error;
};
struct TorrentDiag {
    bool    valid = false;
    bool    hasMetadata = false;
    bool    paused = false;
    bool    queued = false;        // waiting for an active slot (bt/maxActiveDownloads)
    qint64  activeSec = 0;         // time running in this session
    int     knownPeers = -1;       // libtorrent's peer list size (connectable candidates)
    int     knownSeeds = -1;
    // where the candidates came from (sum of the answers: trackers / DHT / incoming / PEX)
    int     fromTrackers = 0, fromDht = 0, fromPex = 0, fromLsd = 0, fromIncoming = 0, fromOther = 0;
    int     connectAttempts = 0;   // outgoing connection attempts (session)
    int     connectedNow = 0;
    int     seedsConnected = 0;
    int     chokingUs = 0;         // connected peers that have something for us but keep us choked
    int     interesting = 0;       // connected peers that have pieces we want
    int     failedTimeout = 0, failedRefused = 0, failedHandshake = 0, failedBanned = 0, failedOther = 0;
    int     dhtLookups = 0;        // get_peers lookups finished
    int     dhtPeers = 0;          // peers they returned
    int     downBps = 0;
    int     slowPeers = 0;         // connected peers sending < 2 KB/s while we are interested
    int     dhtNodes = -1;         // the session's DHT routing table size
    bool    dhtEnabled = true;
    QList<TrackerDiag> trackers;
};

class TorrentBackend {
public:
    virtual ~TorrentBackend() = default;
    // uri: magnet link, or empty when torrentFile (raw .torrent bytes) is given. Returns the id
    // (info-hash hex) or an empty string + *error. Adding an existing torrent returns its id.
    virtual QString addTorrent(const QString &uri, const QByteArray &torrentFile, const QString &saveDir,
                               bool paused, const QList<int> &wantedFiles, QString *error) = 0;
    virtual void removeTorrent(const QString &id, bool deleteFiles) = 0;
    virtual void pauseTorrent(const QString &id) = 0;
    virtual void resumeTorrent(const QString &id) = 0;
    virtual void setWantedFiles(const QString &id, const QList<int> &indexes) = 0;
    virtual void setTorrentPriority(const QString &id, int priority) = 0;
    virtual void recheck(const QString &id) = 0;
    virtual void reannounce(const QString &id) = 0;
    virtual QStringList torrents() const = 0;
    virtual TorrentSnapshot snapshot(const QString &id) const = 0;   // cheap (cached, ≤1 s old)
    // Per-piece SHA-1 (v1) so the engine can pre-verify foreign data; empty if no metadata yet.
    virtual QByteArray pieceHash(const QString &id, int piece) const = 0;
    // Tell libtorrent not to pick these pieces itself for now (the engine fetches them elsewhere),
    // or give them back (reserve = false).
    virtual void reservePieces(const QString &id, const QList<int> &pieces, bool reserve) = 0;
    // Feed a complete piece obtained from another network. Verified asynchronously:
    // → torrentPieceFinished or torrentPieceFailed.
    virtual void injectPiece(const QString &id, int piece, const QByteArray &data) = 0;
    // Share an existing local file as a (single-file) torrent: hash it, seed it, announce to DHT.
    // Async → torrentAdded. Returns false if the file cannot be read.
    virtual bool seedLocalFile(const QString &path, QString *error) = 0;
    // Signals are declared on the QObject returned here (see TorrentEvents).
    virtual QObject *torrentEvents() = 0;
    // [engine-3] Move a torrent's files to another directory (libtorrent move_storage), async →
    // TorrentEvents::torrentStorageMoved. Fails (no overwrite) when a file of the same name exists
    // at the target. Returns false at once if unsupported or the torrent is unknown.
    virtual bool moveStorage(const QString &id, const QString &newSaveDir) { Q_UNUSED(id) Q_UNUSED(newSaveDir) return false; }
    // [diag-1] connection counters of one torrent (valid = false when unsupported / unknown)
    virtual TorrentDiag torrentDiagnostics(const QString &id) const { Q_UNUSED(id) return {}; }
    // [dl-1] download order of one torrent: sequential = libtorrent's sequential_download flag
    // (wanted files in file order); firstLast = the pieces of the head and tail band of every wanted
    // file (mx::order::headBandBytes / tailBandBytes) get the top piece priority. Not persisted by the
    // backend: the engine sets it again whenever it (re)attaches to the torrent. Returns false when
    // unsupported (the torrent then downloads in libtorrent's normal order).
    virtual bool setDownloadOrder(const QString &id, bool sequential, bool firstLast)
    { Q_UNUSED(id) Q_UNUSED(sequential) Q_UNUSED(firstLast) return false; }
};
// The QObject returned by TorrentBackend::torrentEvents() has these signals:
class TorrentEvents : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
signals:
    void torrentAdded(const QString &id);
    void torrentMetadata(const QString &id);                 // name / files / piece hashes now known
    void torrentChanged(const QString &id);                  // ≤ 1/s per torrent
    void torrentPieceFinished(const QString &id, int piece); // verified (from peers or injected)
    void torrentPieceFailed(const QString &id, int piece);   // hash failure
    void torrentFinished(const QString &id);
    void torrentRemoved(const QString &id);
    void torrentError(const QString &id, const QString &text);
    // [engine-3] answer to moveStorage(): ok → files now under savePath; else error (old place kept)
    void torrentStorageMoved(const QString &id, const QString &savePath, bool ok, const QString &error);
};

// ---------------------------------------------------------------- chat backend (CanChat)
// Implemented by the overlay plugin; the core ChatService forwards to it and adds persistence.
// [core] Conventions: the backend must NOT echo the local user's own room/private messages back
// through messageReceived (the core ChatService shows them itself); slash commands are parsed by
// the core, sendRoomMessage() receives plain text with kind Message or Action (/me).
class ChatBackend {
public:
    virtual ~ChatBackend() = default;
    virtual void refreshRoomList() = 0;
    virtual QList<ChatRoomInfo> roomList() const = 0;
    virtual void joinRoom(const QString &roomIdOrName) = 0;
    virtual void leaveRoom(const QString &roomId) = 0;
    virtual QList<ChatUser> roomUsers(const QString &roomId) const = 0;
    virtual void sendRoomMessage(const QString &roomId, const QString &text, ChatKind kind) = 0;
    virtual void sendPrivateMessage(const UserRef &to, const QString &text) = 0;
    virtual void setTopic(const QString &roomId, const QString &topic) = 0;
    // [added by core-5] the room's message of the day (lines separated by '\n'); a joining member
    // gets the topic as a ChatKind::Topic line and the MOTD as ChatKind::Motd lines
    virtual void setMotd(const QString &roomId, const QString &motd) { Q_UNUSED(roomId) Q_UNUSED(motd) }
    virtual QObject *chatEvents() = 0;                       // a ChatEvents
    // [added by ovl-2] a private message with the core's message id. A backend that returns true reports
    // its delivery through ChatEvents::privateMessageStatus(to, msgId, ...); false: no delivery reports
    virtual bool sendPrivateMessageWithId(const UserRef &to, const QString &text, const QString &msgId)
    {
        Q_UNUSED(msgId)
        sendPrivateMessage(to, text);
        return false;
    }
    // [added by ovl-2] the core did not show an incoming private message (privacy rules the backend
    // cannot check itself). Called synchronously from the messageReceived handler; the backend may tell
    // the sender ("not accepting private messages"). reason: "hotlist" | "nobody" | "restricted"
    virtual void refusePrivateMessage(const ChatMessage &msg, const QString &reason) { Q_UNUSED(msg) Q_UNUSED(reason) }
};
class ChatEvents : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
signals:
    void roomListChanged();
    void roomJoined(const mx::ChatRoomInfo &room);
    void roomLeft(const QString &roomId);
    void roomUpdated(const mx::ChatRoomInfo &room);
    void roomUsersChanged(const QString &roomId);
    void messageReceived(const mx::ChatMessage &msg);
    void userPresence(const mx::UserRef &u, bool online, int files, const QString &connection);
    // [added by ovl-2] delivery of a PM sent with sendPrivateMessageWithId (status: a PmDelivery)
    void privateMessageStatus(const mx::UserRef &to, const QString &msgId, int status, const QString &detail);
};

} // namespace mx

Q_DECLARE_METATYPE(mx::PortProbe)   // [diag-1]
Q_DECLARE_METATYPE(mx::PortMapping)
