// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX: shared value types (the contract between UI, core and network plugins).
// Owner: coordinator. Additive changes only (new fields with defaults, new enum values at the end).
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QDateTime>
#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QMetaType>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVariantMap>

#define MX_APP_NAME "OctoMX"
// [post-1] defined by the top-level CMakeLists.txt from project(OctoMX VERSION ...); these
// fallbacks only serve a build without it
#ifndef MX_VERSION_FULL
#define MX_VERSION_FULL "5.2.0"
#endif
#ifndef MX_VERSION_STRING
#define MX_VERSION_STRING "5.2"
#endif

namespace mx {

// Call once at startup (registers all metatypes below for queued signals).
void registerMetaTypes();

// ---------------------------------------------------------------- networks
enum class Net : int {
    Unknown = 0,
    BitTorrent,   // libtorrent: swarm transfers, mainline DHT
    ED2K,         // eDonkey2000 servers + client-to-client
    Kad,          // eMule Kademlia (serverless eD2k search / source finding)
    Gnutella,     // Gnutella 0.6 (leaf mode), HTTP range transfers
    Http,         // plain HTTP/HTTPS/FTP byte-range sources, web seeds
    Overlay,      // our own OctoMX peer network: chat, browse, hash links, PMs, direct transfers
    Web,          // web torrent index sites (search only)
    Rats,         // rats-search P2P network: other people's DHT torrent indexes (search only)
};
QString netName(Net n);        // "BitTorrent", "eD2k", "Kad", "Gnutella", "HTTP", "OctoMX", "Web", "Rats"
QString netShortName(Net n);   // "BT", "ed2k", "Kad", "Gnut", "HTTP", "MX", "Web"
Net netFromName(const QString &name);

// ---------------------------------------------------------------- identity of a file
// One physical file can be known under several hashes; any non-empty one identifies it.
struct FileHashes {
    QByteArray btih;          // 20 bytes: BitTorrent v1 info-hash of the torrent that CONTAINS the file
    int        btFileIndex = -1; // index of the file inside that torrent (-1 = whole torrent / unknown)
    QByteArray btFileRoot;    // 32 bytes: BitTorrent v2 per-file merkle root (pieces root), if known
    QByteArray ed2k;          // 16 bytes: eD2k file hash (MD4 of MD4 part hashes)
    QByteArray aich;          // 20 bytes: eMule AICH root (SHA-1 tree)
    QByteArray sha1;          // 20 bytes: whole-file SHA-1 (Gnutella urn:sha1)
    QByteArray tth;           // 24 bytes: Tiger tree root (Gnutella/DC)
    QByteArray sha256;        // 32 bytes: whole-file SHA-256
    QByteArray md5;           // 16 bytes

    bool isEmpty() const;
    // true if both sides carry at least one hash of the same kind and it is equal (same file)
    bool matches(const FileHashes &o) const;
    // true if some hash kind is present on both sides and DIFFERENT (definitely not the same file)
    bool conflicts(const FileHashes &o) const;
    // copy every hash that we lack from o
    void mergeFrom(const FileHashes &o);
    QVariantMap toVariant() const;                 // hex strings, for persistence / JSON
    static FileHashes fromVariant(const QVariantMap &m);
    // "magnet:?xt=urn:btih:..&xt=urn:ed2k:..&xt=urn:sha1:..&xl=<size>&dn=<name>"
    QString toMagnet(const QString &name, qint64 size) const;
};

enum class MediaType : int { Any = 0, Audio, Video, Image, Document, Software, Archive, Other };
// The type of a file by its extension (Other = no or an unknown extension). [ui-f] Reads the same
// table as extensionsForMediaType(): every extension belongs to exactly one type.
MediaType mediaTypeForFileName(const QString &fileName);
// [ui-f] The extensions of a type, most common first (lower case, no dot); empty for Any / Other.
// Software includes installers, packages and disc images (iso img bin cue nrg mdf ...); Documents
// include e-books, comics, office files, web pages and subtitles (srt ass sub vtt).
QStringList extensionsForMediaType(MediaType t);
// [ui-f] Lossless audio formats (a subset of the Audio extensions): flac wav ape alac aiff wv ...
QStringList losslessAudioExtensions();
// Disc images (a subset of the Software extensions): iso img bin cue nrg mdf mds ccd isz
QStringList discImageExtensions();
// [ui-f] "mkv, .SRT *.ass" -> {"mkv", "srt", "ass"} (lower case, no dots / wildcards, no duplicates)
QStringList parseExtensionList(const QString &text);

// ---------------------------------------------------------------- users
struct UserRef {
    Net     net = Net::Unknown;
    QString id;          // stable per network: overlay peer id, eD2k user hash (hex), "ip:port" otherwise
    QString name;        // screen name to show ("Quill233_2..."); may equal id
    QString address;     // "ip:port" if known
    bool isValid() const { return net != Net::Unknown && !id.isEmpty(); }
    QString key() const; // "<net>:<id>"
};

// ---------------------------------------------------------------- search
struct SearchQuery {
    quint32   id = 0;              // assigned by SearchService
    QString   text;                // keywords (original WinMX "Artist and / or Title", first line)
    QString   text2;               // second keyword line (ANDed), may be empty
    MediaType type = MediaType::Any;
    QString   extension;           // optional: "mp3", "avi"...
    qint64    minSize = 0;         // bytes, 0 = no limit
    qint64    maxSize = 0;
    int       minBitrate = 0;      // kbps, 0 = any
    QList<Net> networks;           // empty = all enabled networks
    int       maxResults = 2000;
    // [added by glue-2] search by hash: when any hash is set the search looks up sources of exactly
    // this file on every network that can (NetworkPlugin::findSources) instead of a keyword search;
    // `text` is then only the display name. See SearchService::searchHash().
    FileHashes hashes;
    qint64    fileSize = -1;       // size from the pasted link (-1 = unknown)
    // [ui-f] more extensions: a hit matches when its extension is `extension` or one of these
    // (lower case, no dot). Networks that can ask for only one extension ask for none when there
    // are several and filter by name. See queryExtensions(), hitMatchesFileType().
    QStringList extensions;
    int       maxBitrate = 0;      // [ui-f] kbps, 0 = any ("Bitrate at best"; "equal to" = min == max)
};

// One place a file can be fetched from, as reported by a search.
struct SearchSource {
    Net     net = Net::Unknown;
    UserRef user;                  // may be invalid for swarm-type sources (BitTorrent, Kad counts)
    QString uri;                   // magnet:, ed2k://, http(s)://, or internal source descriptor
    QString connection;            // original WinMX "Speed" column text: "DSL", "Cable", "T1"... or network label
    int     pingMs = -1;
    int     seeds = -1;            // complete sources (BT seeders, eD2k complete sources)
    int     peers = -1;            // total known sources / leechers
    int     freeSlots = -1;        // original WinMX "x of y available": x
    int     totalSlots = -1;       //                           y
    QVariantMap extra;
};

struct BundleFile {                // a file inside a torrent bundle
    QString path;                  // relative path inside the bundle
    qint64  size = 0;
    int     index = -1;
    qint64  done = -1;             // [added by engine] downloads: verified bytes of this file (-1 = unknown)
    bool    wanted = true;         // [added by engine] downloads: selected for download
};

struct SearchHit {
    quint32   queryId = 0;
    QString   key;                 // grouping key: equal key == same file (set by SearchService merge)
    QString   fileName;            // display name (no directories)
    QString   path;                // full remote path if the network exposes it ("Show Full Paths")
    qint64    size = 0;            // bytes (whole bundle for bundles)
    MediaType type = MediaType::Any;
    int       durationSec = -1;    // "Time"
    int       bitrateKbps = -1;    // "Bitrate"
    int       sampleRateHz = -1;   // "Freq"
    FileHashes hashes;
    bool      isBundle = false;    // multi-file torrent
    QList<BundleFile> files;       // for bundles, when known
    QList<SearchSource> sources;   // >= 1; merged across networks by SearchService
    QDateTime published;           // if known
    // Local knowledge, filled by core before the hit reaches the UI (original WinMX legend colours):
    bool matchesIncompleteLocal = false;  // yellow
    bool canJoinRunningDownload = false;  // cyan
    bool currentlyDownloading   = false;  // blue
    bool alreadyInLibrary       = false;
    // [added by glue-2] why this hit looks like a fake ("archive wrapped around a media file", ...);
    // empty = nothing suspicious. Set by the core search hygiene (settings search/flagSuspects,
    // search/hideSuspects); the UI may grey the row or show the reason as a tooltip.
    QString   suspect;
    // [engine-4] plain numbers for sortable columns, kept up to date by the core as hits merge
    // (computed from `sources` each time the hit is emitted):
    int        totalPeers = -1;    // every peer known across all networks (BT swarm seeds + leechers,
                                   // eD2k/Kad reported sources, direct sources); -1 = unknown
                                   // [core-5] per network: BitTorrent = the best seeds + leechers an
                                   // index reported; eD2k = a server's FT_SOURCES (all sources, the
                                   // complete ones are FT_COMPLETE_SOURCES = seeds), the largest of
                                   // the merged server rows; Kad = TAG_SOURCES (availability), seeds
                                   // TAG_COMPLETE_SOURCES; Gnutella / OctoMX / HTTP = one source per
                                   // listed row (distinct users). Never below the listed sources of
                                   // that network (BT rows are index entries, not peers).
    int        seeds = -1;         // complete sources where reported (BT seeds, eD2k complete sources); -1 unknown
    int        leechers = -1;      // BitTorrent leechers where reported; -1 unknown
    int        sourceCount = 0;    // distinct sources / users in `sources`
    QList<Net> networks;           // networks that have the file (sorted, unique)
    QStringList indexes;           // web/Rats torrent index names that listed it ("LimeTorrents", ...)
    // [merge-1] files of this bundle that are also plain-file rows of the same search (exact size,
    // same name by SearchMerger::sameName, no common hash): probable matches, checked block by block
    // when downloading. The plain row lists the torrent as a source (extra["innerOf"] = this key).
    struct InnerMatch {
        int        file = -1;      // index in the torrent (-1 = not known)
        QString    path;           // path inside the bundle
        qint64     size = 0;
        QString    rowKey;         // key of the plain-file row
        QList<Net> networks;       // networks of that row
        QList<SearchSource> sources;   // that row's own sources, marked candidate, extra["bundlePath"] = path
    };
    QList<InnerMatch> innerMatches;
};

// ---------------------------------------------------------------- transfers
enum class TransferState : int {
    Queued = 0,        // "Locally Queued"
    FindingSources,    // "Finding sources..." / fetching torrent metadata
    Connecting,
    RemotelyQueued,    // all sources have us in their queue
    Transferring,
    Paused,
    Verifying,         // hashing / rechecking
    Completed,
    Seeding,           // completed and still uploading (shown in the upload pane)
    Error,             // see statusText
    Cancelled,
    TimedOut,
};
QString transferStateText(TransferState s);   // original WinMX wording
// [added by core-5] "1 peer" / "3 peers": a count with its noun in singular or plural (plural
// defaults to singular + "s"); for every core-produced status text
QString countNoun(qint64 n, const QString &singular, const QString &plural = QString());

struct TransferSource {            // one peer/channel feeding (or fed by) a transfer
    Net     net = Net::Unknown;
    UserRef user;
    QString statusText;            // "Transferring", "Remotely Queued (12)", "Connecting..."
    qint64  bytes = 0;             // bytes moved through this source
    int     rateBps = 0;
    QString connection;
    QString client;                // "eMule 0.50a", "qBittorrent 4.6"...
    // [added by glue-2] downloads: stable id of this source within its transfer (for
    // TransferService::cancelSource / setSourcePriority); empty = not addressable (e.g. BT peers)
    QString id;
    int     priority = 0;          // -1 low, 0 normal, 1 high (setSourcePriority)
};

struct TransferInfo {
    QString   id;                  // stable across restarts
    bool      isUpload = false;
    QString   name;                // file (or bundle) name
    QString   savePath;            // absolute path of file / bundle directory (downloads)
    qint64    size = 0;
    qint64    done = 0;            // verified bytes (downloads) / bytes sent (uploads)
    int       rateDownBps = 0;
    int       rateUpBps = 0;
    int       etaSec = -1;
    TransferState state = TransferState::Queued;
    QString   statusText;          // full status string to display
    QString   errorText;
    FileHashes hashes;
    bool      isBundle = false;
    QList<BundleFile> files;
    QList<Net> networks;           // networks currently contributing; [core-5] for a download also every
                                   // network it has a usable source on (sorted, unique)
    QList<TransferSource> sources; // children in the transfer tree
    int       sourcesTotal = 0;    // known sources
    int       sourcesActive = 0;   // currently transferring
    int       priority = 0;        // -1 low, 0 normal, 1 high
    QDateTime added;
    QDateTime completed;
    QByteArray pieceMap;           // optional coarse availability/progress map: 1 byte per cell,
                                   // 0 = missing, 1 = in progress, 2 = complete (≤ 512 cells)
                                   // [ui-i] cells cover the wanted bytes laid end to end (a bundle:
                                   // its wanted files in file order), cell i = bytes [W*i/n, W*(i+1)/n);
                                   // 2 = every byte of the cell verified (on disk); 1 = some byte of
                                   // the cell is requested from a source and arriving now (never
                                   // merely planned); 0 = neither (it may still hold verified bytes)
    QByteArray pieceFill;          // [ui-i] same length as pieceMap (or empty): share of verified bytes
                                   // per cell, 0 = none ... 255 = all; 1...254 = some but not all (any
                                   // verified byte gives at least 1, a missing byte at most 254)
    UserRef   peer;                // uploads: who is downloading from us
    int       queuePosition = 0;   // [glue-3] uploads in state Queued: place in our upload queue (1 = next)
    // [engine-4] downloads: the effective per-download "AFS - AEQ" values (TransferService::
    // setAutoFindSources / setAutoEnterQueue; the global search/autoFindSourcesMin and
    // search/autoEnterQueue when not set per download). 0 = Never. -1 = not reported (old cores,
    // uploads): show the global setting.
    int       autoFindSourcesMin = -1;
    int       autoEnterQueue = -1;
    // [diag-1] downloads: one plain sentence on why this download is slow or stuck (empty = nothing
    // to say), shown in the status tooltip; level = int(mx::DiagLevel) (diagnostics.h)
    QString   diagnosis;
    int       diagnosisLevel = 0;
    // [dl-1] downloads: the two order flags (TransferService::setSequentialDownload /
    // setFirstLastPiecesFirst; priorities, never conditions) and, for the status tool tip, which
    // networks of this download follow them right now (empty = none of its sources can)
    bool      sequential = false;
    bool      firstLastFirst = false;
    QList<Net> orderFollowedBy;
};

// [dl-1] Preview of a partly downloaded audio / video file (TransferService::previewInfo).
struct PreviewFile {
    int     index = -1;            // bundle file index (-1 = the single file of a download)
    QString name;                  // file name (bundle: path inside the bundle)
    qint64  size = 0;
    qint64  done = 0;              // bytes on disk (progress in the chooser)
    qint64  headBytes = 0;         // contiguous bytes on disk from offset 0
    qint64  tailBytes = 0;         // contiguous bytes on disk that end at the end of the file
    bool    media = false;         // audio or video by the file-type table
    bool    ready = false;         // can be played now (mx::preview::evaluate)
    QString reason;                // why not ("Preview: needs the beginning of the file")
};
struct PreviewInfo {
    bool               supported = false;   // the core can preview this download at all
    QList<PreviewFile> files;               // the media files (single file: one entry)
};

struct DownloadRequest {           // "download this", from a search hit or a pasted link
    QString   name;
    qint64    size = 0;
    FileHashes hashes;
    QList<SearchSource> sources;   // initial sources (optional)
    QString   uri;                 // magnet:/ed2k:/http(s): link, or path to a .torrent file
    QString   saveDir;             // empty = default incoming dir
    QList<int> wantedFiles;        // bundle: indexes to fetch (empty = all)
    bool      startPaused = false;
};

// ---------------------------------------------------------------- library (shared files)
struct SharedFolder {
    QString path;
    bool    recursive = true;
    int     fileCount = 0;
    qint64  totalBytes = 0;
};
struct SharedFile {
    QString   path;                // absolute local path
    QString   name;
    qint64    size = 0;
    QDateTime mtime;
    MediaType type = MediaType::Any;
    int       durationSec = -1;
    int       bitrateKbps = -1;
    int       sampleRateHz = -1;
    FileHashes hashes;             // filled progressively by the hasher
    bool      hashed = false;
    qint64    uploadedBytes = 0;
    int       requests = 0;
};

// ---------------------------------------------------------------- network status
enum class ConnState : int { Disabled = 0, Disconnected, Connecting, Connected, Error };
struct NetworkStatus {
    Net       net = Net::Unknown;
    ConnState state = ConnState::Disabled;
    QString   statusText;          // "Connected to 3 servers", "DHT: 312 nodes"...
    int       peers = 0;           // connected peers / servers / DHT nodes (whatever is meaningful)
    int       primaryLinks = -1;   // [added by core-5] links that are "Primary Connections" (each peer
                                   // once; -1 = the core derives it from peers, see NetworkSummary)
    qint64    usersOnline = -1;    // network population estimate if known
    qint64    filesOnline = -1;
    bool      firewalled = false;  // no incoming connections (LowID / NAT)
    int       listenPort = 0;
    QString   externalAddress;
    int       rateDownBps = 0;
    int       rateUpBps = 0;
};
// A row for the "protocol networks" list (servers, bootstrap nodes, hubs...)
struct ServerEntry {
    Net       net = Net::Unknown;
    QString   id;                  // unique within the network ("host:port")
    QString   name;
    QString   address;
    ConnState state = ConnState::Disconnected;
    qint64    users = -1;          // people (eD2k server users, ...), never DHT nodes
    qint64    files = -1;
    int       pingMs = -1;
    QString   description;
    bool      isStatic = false;    // user-added / keep forever
    qint64    nodes = -1;          // [added by core-5] DHT rows (BitTorrent, Kad, Rats): nodes known
};

// [added by glue-2] the numbers behind the title bar / "Connection Status" box, so the UI does not
// have to parse NetworkService::titleSummary().
struct NetworkSummary {
    ConnState state = ConnState::Disabled;
    // [engine-4] "P=" / "Primary Connections", the original WinMX way: the hubs we search through,
    // connected eD2k servers + Gnutella ultrapeers + OctoMX overlay peers + Rats peers + responding
    // web torrent indexes + 1 for Kad and 1 for the BitTorrent DHT when they are connected
    // (a DHT counts once, not per node). Per network in primaryByNet.
    // [core-5] OctoMX overlay: hello-verified OctoMX peers only, each once (NetworkStatus::primaryLinks);
    // Rats: the rats-search swarm counts once like a DHT (its ~30 peers are index nodes); the web
    // torrent indexes count once when any of them answers (HTTP per search, no standing link).
    // The sum of primaryByNet is always primary.
    int       primary = 0;
    int       sources = 0;             // "S=" until engine-4: known sources of unfinished downloads (= knownSources)
    int       searchesPerMinute = 0;   // incoming search requests answered (last 60 s, all networks)
    int       ownSearchesPerMinute = 0; // searches we started (last 60 s)
    int       networksOnline = 0;      // networks in state Connected
    int       networksEnabled = 0;
    qint64    usersOnline = -1;        // sum of the networks' population estimates (-1 = unknown)
    qint64    filesOnline = -1;
    bool      firewalled = false;      // every connected network reports firewalled / LowID
    // [engine-4] "S=" / "Secondary Connections": peers we exchange data or queue slots with right
    // now: download sources that are connected / remotely queued / transferring, connected
    // BitTorrent peers of running torrents, and uploads (transferring or queued).
    int       secondary = 0;
    int       knownSources = 0;        // [engine-4] every known (not banned) source of unfinished downloads
    QHash<Net, int> primaryByNet;      // [engine-4] the primary figure per connected network
    QHash<Net, int> peersByNet;        // [engine-4] raw NetworkStatus::peers per connected network (DHT nodes, contacts...)
};
// [added by glue-2] a message from a server (eD2k server MOTD / OP_SERVERMESSAGE lines ...)
struct ServerMessage {
    Net       net = Net::Unknown;
    QString   serverId;                // ServerEntry::id
    QString   serverName;
    QString   text;
    QDateTime time;
};
// [added by glue-2] a file in the incomplete folder (Manually Resume dialog)
struct PartialFile {
    QString   path;
    QString   name;
    qint64    size = 0;
    QDateTime modified;
    QString   downloadId;              // non-empty: belongs to this download (cannot be attached again)
};

// ---------------------------------------------------------------- chat
struct ChatRoomInfo {
    QString id;                    // stable room id
    QString name;                  // "Room Name_0A1B2C3D4E5F" style or plain
    QString topic;
    int     users = 0;
    int     limit = 0;
    Net     net = Net::Overlay;
    QString motd;                  // [added by core-5] message of the day, lines separated by '\n'
};
struct ChatUser {
    UserRef user;
    int     files = 0;
    QString connection;            // "Cable", "DSL"...
    int     rank = 0;              // 0 normal, 1 voiced (+), 2 admin (@)
};
enum class ChatKind : int { Message = 0, Action, Join, Part, System, Topic, Motd, Private, Error };
struct ChatMessage {
    QString   roomId;              // empty for private messages
    UserRef   from;
    QString   text;
    ChatKind  kind = ChatKind::Message;
    QDateTime time;
    UserRef   to;                  // [added by core] private messages: the recipient (set on the local
                                   // echo of a PM we sent; from == ourselves then)
    QString   msgId;               // [added by ovl-2] private messages: the message id (our echo of a PM
                                   // we sent, when the backend reports delivery; incoming PMs) or empty
};
// [added by ovl-2] delivery state of a private message we sent (ChatService::privateMessageStatus)
enum class PmDelivery : int {
    Sending = 0,    // handed to the network, waiting for the recipient's receipt
    Sent,           // sent, but no receipt can come (the recipient runs an older version)
    Delivered,      // the recipient's program acknowledged it
    NotDelivered,   // could not be delivered (detail says why); the backend retries when the user is back
    Refused         // the recipient does not accept it (privacy settings); final
};

// ---------------------------------------------------------------- hotlist / browse
struct HotlistEntry {
    UserRef   user;
    bool      online = false;
    int       files = 0;
    QString   connection;
    QDateTime lastSeen;
    QString   note;
};
struct BrowseEntry {               // a file in another user's share
    QString path;                  // remote path using '\\' separators as in the original WinMX
    qint64  size = 0;
    int     durationSec = -1;
    int     bitrateKbps = -1;
    int     sampleRateHz = -1;
    FileHashes hashes;
};

// ---------------------------------------------------------------- bandwidth
struct BandwidthSample {           // one per second
    qint64 timeMs = 0;
    int    downBps = 0;            // everything: payload + protocol / DHT / tracker overhead, all networks
    int    upBps = 0;
    // [added by glue-3]
    int    payloadDownBps = 0;     // file data only (part of downBps)
    int    payloadUpBps = 0;
    QHash<Net, int> netDownBps;    // per network, total incl. overhead (networks with traffic only)
    QHash<Net, int> netUpBps;
};

// [added by glue-3, groups replaced by ui-f] The "Shared File Types" groups (Library ▸ Shared File
// Types), built from the same table as the Search file-type menu: "audio", "video", "pictures",
// "documents", "software", "archives", "torrents", "servers" (.met .wsx), "clr" (.clr).
// The WinMX 3.53 ids of older profiles ("mp3", "ogg", "otheraudio", "archive") are understood
// everywhere and converted by normalizeSharedTypeIds().
QStringList sharedTypeIds();
QStringList sharedTypeExtensions(const QString &typeId);   // lower-case, no dot; legacy ids too
// Old ids -> new ones: mp3 / ogg / otheraudio -> audio, archive (zip iso rar exe ...) -> archives +
// software, unknown ids dropped. Documents and torrents had no group: an old list (one with a legacy
// id) gains them when `shareUnlisted` is on, because that is how those files were shared before.
// A list of new ids comes back unchanged (order of sharedTypeIds()).
QStringList normalizeSharedTypeIds(const QStringList &ids, bool shareUnlisted);
bool isLegacySharedTypeList(const QStringList &ids);
// Is a file shared under that filter? typeIds = checked groups; otherExtensions = space/comma
// separated list ("flac .epub"); shareUnlisted = share files whose extension is in no group.
bool fileMatchesSharedTypes(const QString &fileName, const QStringList &typeIds,
                            const QString &otherExtensions, bool shareUnlisted);

// ---------------------------------------------------------------- [ui-f] search file-type filters
// The Search page file-type menu, stored as a stable code (search/defaultType, the button presets):
//   any                      All Files
//   audio | audio:<op>:<kbps> all audio, op = min (at least) / eq (equal to) / max (at best)
//   video | pictures | documents | software | archives | torrents
//   mp3 | mp3:<op>:<kbps>     MP3 only
//   lossless                 lossless audio (losslessAudioExtensions())
//   discimages               CD / DVD images (discImageExtensions()); networks are asked for every
//                            type (a film or a game can be an .iso), the extension decides
//   servers | clr            server lists (.met .wsx), colour schemes (.clr)
//   custom:<ext>,<ext>...    the user's own extensions
struct FileTypeFilter {
    QString kind;              // "any", "audio", "video", ..., "custom"; empty = invalid
    QString bitrateOp;         // "", "min", "eq", "max" (audio and mp3 only)
    int     kbps = 0;
    QStringList custom;        // kind == "custom": the extensions
    static FileTypeFilter fromCode(const QString &code);   // invalid code -> kind empty
    static QStringList kinds();                             // every kind, in menu order
    QString code() const;      // normalised ("audio:any" -> "audio")
    bool isValid() const { return !kind.isEmpty(); }
    // the general type the networks are asked for (Audio for mp3 / lossless; for custom the type
    // all its extensions share, else Any)
    MediaType mediaType() const;
    // extension-based kinds (mp3, lossless, discimages, torrents, servers, clr, custom): the extensions a hit
    // must have; general types: empty
    QStringList requiredExtensions() const;
    void applyTo(SearchQuery &q) const;   // sets type, extension(s), min/max bitrate
};
// Maps the texts and codes stored by OctoMX <= 4.0 ("MP3 Audio - At least 128 kbps",
// "Video (.avi .asf .mpg .mpeg)", "Any File / Bitrate", "mp3ogg:min:128", "nonmp3" ...) and new codes
// to a valid new code; empty when the text means nothing.
QString fileTypeCodeFromLegacy(const QString &text);

// All extensions a query asks for (extension + extensions), normalised, no duplicates.
QStringList queryExtensions(const SearchQuery &q);
// What a hit is, for the type filter: a non-bundle by its known extension, else by the type its
// network reported; a bundle by the reported type (index category, eD2k / Rats content type), else
// by the type with the most bytes in its file list, else by its name's extension. A torrent / bundle
// still unknown then goes by strong words of its name (typeFromNameWords()). Other = unknown.
MediaType classifyHit(const SearchHit &h);
// "Sintel 872p mp4" -> Video, "Album [FLAC]" -> Audio, "Distro amd64" -> Software: extensions written
// as words and release tags (1080p, x264, S01E02, 320kbps, lossless, epub, x64 ...); Other if none.
MediaType typeFromNameWords(const QString &name);
// The type / extension / bitrate part of a query, as the core, the networks and the UI apply it:
//  * bitrate limits only hide hits whose bitrate is known;
//  * extensions: a file's extension must be listed; a bundle needs a listed file (file list known),
//    else its name's extension or an extension written as a word ("Album [FLAC]"), else a known
//    type of the listed extensions (classifyHit());
//  * type (only without extensions, which decide on their own): classifyHit() must be that type; an
//    unknown type (Other) keeps torrents and bundles (names such as "Big Buck Bunny 1080p" carry no
//    extension) but hides plain files.
bool hitMatchesFileType(const SearchQuery &q, const SearchHit &h);
// The same for a single file name (networks that filter raw results before building hits).
// `reported` = the type the remote side gave (Any = none).
bool fileNameMatchesFileType(const SearchQuery &q, const QString &fileName, MediaType reported = MediaType::Any,
                             int bitrateKbps = -1);

} // namespace mx

Q_DECLARE_METATYPE(mx::Net)
Q_DECLARE_METATYPE(mx::FileHashes)
Q_DECLARE_METATYPE(mx::UserRef)
Q_DECLARE_METATYPE(mx::SearchQuery)
Q_DECLARE_METATYPE(mx::SearchSource)
Q_DECLARE_METATYPE(mx::SearchHit)
Q_DECLARE_METATYPE(mx::TransferInfo)
Q_DECLARE_METATYPE(mx::TransferSource)
Q_DECLARE_METATYPE(mx::DownloadRequest)
Q_DECLARE_METATYPE(mx::SharedFolder)
Q_DECLARE_METATYPE(mx::SharedFile)
Q_DECLARE_METATYPE(mx::NetworkStatus)
Q_DECLARE_METATYPE(mx::ServerEntry)
Q_DECLARE_METATYPE(mx::ChatRoomInfo)
Q_DECLARE_METATYPE(mx::ChatUser)
Q_DECLARE_METATYPE(mx::ChatMessage)
Q_DECLARE_METATYPE(mx::HotlistEntry)
Q_DECLARE_METATYPE(mx::BrowseEntry)
Q_DECLARE_METATYPE(mx::BandwidthSample)
Q_DECLARE_METATYPE(mx::BundleFile)   // [added by core]
Q_DECLARE_METATYPE(mx::NetworkSummary) // [added by glue-2]
Q_DECLARE_METATYPE(mx::ServerMessage)
Q_DECLARE_METATYPE(mx::PartialFile)
