// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX: UI-facing service interfaces.
// The UI talks ONLY to these (obtained from mx::Core). Two implementations exist:
//   * the real one (src/core + network plugins)
//   * the mock one (src/mock) with canned WinMX-like data, used for --demo and UI screenshots.
// Threading: every method is called on the main (GUI) thread and every signal is emitted on the
// main thread. Implementations must never block: long work is asynchronous.
// Owner: coordinator. Additive changes only.
#pragma once

#include <QtCore/QObject>
#include "mx/diagnostics.h"
#include "mx/types.h"

namespace mx {

// ---------------------------------------------------------------- settings
// Typed key/value store with defaults, persisted in <profile>/settings.json (or ini).
// Keys are slash-separated paths, e.g. "transfers/maxDownloads". The catalogue of keys, types,
// defaults and ranges lives in mx/settings_keys.h + src/api/settings_schema.cpp so the Settings dialog, the engine and
// the plugins agree.
class Settings : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual QVariant value(const QString &key) const = 0;            // falls back to schema default
    virtual void setValue(const QString &key, const QVariant &v) = 0; // persists (debounced), emits changed
    virtual QVariant defaultValue(const QString &key) const = 0;
    virtual QStringList keys(const QString &prefix = QString()) const = 0;
    virtual void resetToDefaults(const QString &prefix = QString()) = 0;
    virtual void sync() = 0;                                          // flush to disk now
    // convenience
    bool    b(const QString &k) const { return value(k).toBool(); }
    int     i(const QString &k) const { return value(k).toInt(); }
    qint64  i64(const QString &k) const { return value(k).toLongLong(); }
    QString s(const QString &k) const { return value(k).toString(); }
    QStringList sl(const QString &k) const { return value(k).toStringList(); }
signals:
    void changed(const QString &key, const QVariant &value);
};

// ---------------------------------------------------------------- networks page
class NetworkService : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual QList<NetworkStatus> networks() const = 0;       // one per protocol, fixed order
    virtual NetworkStatus status(Net n) const = 0;
    virtual ConnState overallState() const = 0;              // Connected if any network is usable
    virtual QString overallHeadline() const = 0;             // "Ready" / "Connecting..." / "Offline"
    virtual QString overallDetail() const = 0;               // "You can search and download now."
    virtual void connectAll() = 0;
    virtual void disconnectAll() = 0;
    virtual void setEnabled(Net n, bool on) = 0;             // persisted
    virtual bool isEnabled(Net n) const = 0;
    virtual void connectNetwork(Net n) = 0;
    virtual void disconnectNetwork(Net n) = 0;
    // servers / bootstrap nodes / hubs known per network (the black "protocol networks" list)
    virtual QList<ServerEntry> servers(Net n = Net::Unknown) const = 0;
    virtual void addServer(Net n, const QString &address, const QString &name = QString()) = 0;
    virtual void removeServer(Net n, const QString &id) = 0;
    virtual void connectServer(Net n, const QString &id) = 0;
    virtual void disconnectServer(Net n, const QString &id) = 0;
    // identity
    virtual QString screenName() const = 0;
    virtual void setScreenName(const QString &name) = 0;
    // title-bar numbers: P = connected peers/servers, S = known sources, plus any others
    virtual QString titleSummary() const = 0;                // "Online P=5 S=12"

    // ---- [added by glue-2] (all have defaults so the mock core keeps compiling)
    // The title-bar / status-box numbers as fields. Default: parsed from titleSummary()
    // ("P=", "S=", "spm=") and networks().
    virtual NetworkSummary summary() const;
    // Change a server's address / name (Networks ▸ Edit). Default: remove + add. Returns false if
    // the network has no such server.
    virtual bool editServer(Net n, const QString &id, const QString &address, const QString &name = QString())
    {
        removeServer(n, id);
        addServer(n, address, name);
        return true;
    }
    // Import a server list file (eD2k server.met, binary, or a text file with one "host:port"
    // [name] per line). Returns the number of servers added/updated, or -1 if unreadable/unsupported.
    virtual int importServerList(Net n, const QString &filePath) { Q_UNUSED(n) Q_UNUSED(filePath) return -1; }
    // Recent server messages (newest last; ≤ 200 per network). Net::Unknown = all networks.
    virtual QList<ServerMessage> serverMessages(Net n = Net::Unknown) const { Q_UNUSED(n) return {}; }
    // Measure the round trip to a user (TCP connect time to its address, or the network's own ping).
    // Answer: userPinged(u, ms) with ms = -1 when unreachable / not pingable.
    virtual void pingUser(const UserRef &u) { Q_UNUSED(u) }
signals:
    void networkChanged(const mx::NetworkStatus &st);
    void overallChanged();
    void serversChanged(mx::Net n);
    void screenNameChanged(const QString &name);
    // [added by glue-2]
    void summaryChanged();                                   // summary() numbers changed (≤ 1/s)
    void serverMessageReceived(const mx::ServerMessage &m);
    void userPinged(const mx::UserRef &u, int ms);
};

// ---------------------------------------------------------------- search page
class SearchService : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual quint32 startSearch(SearchQuery q) = 0;          // returns the id; results arrive async
    virtual void stopSearch(quint32 id) = 0;                 // stop receiving (keeps results)
    virtual void closeSearch(quint32 id) = 0;                // forget it
    virtual bool isRunning(quint32 id) const = 0;
    virtual QList<SearchHit> hits(quint32 id) const = 0;     // merged, current state
    virtual QStringList history() const = 0;                 // recent search strings (persisted)
    virtual void clearHistory() = 0;
    // [added by core] Fetch the file list of a bundle hit (torrent) lazily; when it arrives the hit's
    // `files` are filled and hitsChanged is emitted for it. No-op if unsupported.
    virtual void requestBundleFiles(quint32 id, const QString &hitKey) { Q_UNUSED(id) Q_UNUSED(hitKey) }
    // [added by glue-2] Search by hash: `linkOrHash` is a magnet:, ed2k:// link, or a raw hash
    // (32 hex = eD2k, 40 hex / 32 base32 = BitTorrent info-hash or SHA-1, 39 base32 = TTH,
    // 64 hex = SHA-256, optionally prefixed "ed2k:", "sha1:", "btih:", "tth:", "urn:..."). The
    // result is ONE hit for that file whose sources grow as networks answer. Returns the search
    // id like startSearch(). Default: a keyword search for the text.
    virtual quint32 searchHash(const QString &linkOrHash)
    {
        SearchQuery q;
        q.text = linkOrHash;
        return startSearch(q);
    }
    // [added by glue-2] Ping the sources of these hits; every answer updates SearchSource::pingMs
    // (-1 = unreachable) and sets extra["pinged"]=true, delivered through hitsChanged.
    virtual void pingHits(quint32 id, const QStringList &hitKeys) { Q_UNUSED(id) Q_UNUSED(hitKeys) }
signals:
    // A hit is new or was updated (more sources merged in). Identified by (queryId, key).
    void hitsChanged(quint32 id, const QList<mx::SearchHit> &addedOrUpdated);
    void searchProgress(quint32 id, mx::Net net, const QString &text); // "Kad: 37 results"
    void searchFinished(quint32 id);                         // every network done or timed out
    // [added by core] Rows absorbed into another hit (a later hit proved two rows are the same file):
    // remove these keys; the surviving row arrives in a hitsChanged right after.
    void hitsRemoved(quint32 id, const QStringList &keys);
    // [added by engine-4] answer to requestBundleFiles() when no network can deliver the file list
    // (index-only torrent nobody answers for, no info-hash, timeout ~20 s). The UI should then offer
    // the whole bundle (downloadHit() with no file selection downloads every file).
    void bundleFilesUnavailable(quint32 id, const QString &hitKey, const QString &reason);
};

// ---------------------------------------------------------------- transfers page
class TransferService : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    // Starts (or joins, if the same file is already downloading) a download. Returns transfer id.
    virtual QString download(const DownloadRequest &req) = 0;
    virtual QString downloadHit(const SearchHit &hit, const QString &saveDir = QString()) = 0;
    virtual QString downloadUri(const QString &uri, const QString &saveDir = QString()) = 0;
    virtual QList<TransferInfo> downloads() const = 0;
    virtual QList<TransferInfo> uploads() const = 0;
    virtual TransferInfo transfer(const QString &id) const = 0;
    virtual void pause(const QString &id) = 0;
    virtual void resume(const QString &id) = 0;
    virtual void pauseAll() = 0;
    virtual void resumeAll() = 0;
    virtual void cancel(const QString &id, bool deleteData = true) = 0;   // remove + drop partial data
    virtual void retry(const QString &id) = 0;                // "Find more sources" / restart errored
    virtual void setPriority(const QString &id, int priority) = 0;
    virtual void clearCompleted() = 0;                        // remove finished rows (keeps files)
    virtual void clearUpload(const QString &id) = 0;
    virtual void cancelUpload(const QString &id) = 0;
    virtual void setWantedFiles(const QString &id, const QList<int> &indexes) = 0; // bundles
    virtual void openFile(const QString &id) = 0;             // launch with the OS handler
    virtual void openFolder(const QString &id) = 0;
    virtual QString incomingDir() const = 0;

    // ---- [added by glue-2] (defaults keep the mock core compiling)
    // "Find More Sources": ask every network for sources again now (no state reset).
    virtual void findMoreSources(const QString &id) { retry(id); }
    // "Enter Queue / Retry": reconnect to every known source now (gets us into remote queues),
    // clearing back-off timers; errored downloads are revived like retry().
    virtual void enterQueue(const QString &id) { retry(id); }
    // Per source row (TransferSource::id): drop this source from the download (it is not re-added
    // when found again), or change its priority (-1 low, 0 normal, 1 high: order in which sources
    // are opened and get chunks).
    virtual void cancelSource(const QString &id, const QString &sourceId) { Q_UNUSED(id) Q_UNUSED(sourceId) }
    virtual void setSourcePriority(const QString &id, const QString &sourceId, int priority)
    { Q_UNUSED(id) Q_UNUSED(sourceId) Q_UNUSED(priority) }
    // Uploads: "Start Transfer" for a queued upload (give it a slot now) and its priority (queue
    // order). false = the network cannot do it for this upload.
    virtual bool startUploadNow(const QString &id) { Q_UNUSED(id) return false; }
    virtual bool setUploadPriority(const QString &id, int priority) { Q_UNUSED(id) Q_UNUSED(priority) return false; }
    // Manually Resume: start downloading `req` using `existingFile` (an incomplete file from an
    // earlier session / another client) as the data already received. Unverified data is checked
    // with the file's part hashes or at completion. The file is used in place (it becomes the
    // download's part file). Returns the transfer id, or empty on failure (unsupported, the file
    // is larger than the download, the same file is already downloading, ...).
    virtual QString resumeDownload(const DownloadRequest &req, const QString &existingFile)
    { Q_UNUSED(req) Q_UNUSED(existingFile) return QString(); }
    // Pick a different target: where the finished file goes (dir, file name; empty = keep).
    // false if the download is finished or its store cannot move (running torrents).
    virtual bool setDownloadTarget(const QString &id, const QString &dir, const QString &name = QString())
    { Q_UNUSED(id) Q_UNUSED(dir) Q_UNUSED(name) return false; }
    // Files in the incomplete folder (candidates for resumeDownload; ones in use carry downloadId).
    virtual QList<PartialFile> partialFiles() const { return {}; }
    virtual QString incompleteDir() const { return QString(); }

    // ---- [added by engine-4] per-download "Auto Find Sources" / "Auto Enter Queue" (the original WinMX
    // "AFS - AEQ" column and the two Transfers buttons act on the selected downloads).
    // minutes: look for more sources on every network every `minutes` (0 = Never: only on start,
    // when a network comes up and on "Find Sources Now"). n: wait in remote queues up to rank n
    // (0 = Never enter remote queues by itself; "Enter Queue" still does). -1 = back to the global
    // default (search/autoFindSourcesMin, search/autoEnterQueue). Persisted per download; the
    // effective values are reported in TransferInfo::autoFindSourcesMin / autoEnterQueue.
    virtual void setAutoFindSources(const QString &id, int minutes) { Q_UNUSED(id) Q_UNUSED(minutes) }
    virtual void setAutoEnterQueue(const QString &id, int n) { Q_UNUSED(id) Q_UNUSED(n) }

    // ---- [added by dl-1] download order (qBittorrent's "Download In Sequential Order" and
    // "Download First And Last Pieces First"): priorities, never conditions. Every network that
    // can follow does (libtorrent's sequential mode and piece priorities, the engine's range
    // choice for eD2k / Gnutella / HTTP / OctoMX); a source without the wanted range gives what it
    // has. Persisted per download, effective at once; reported in TransferInfo::sequential /
    // firstLastFirst. New downloads take transfers/sequentialDefault / transfers/firstLastDefault.
    virtual void setSequentialDownload(const QString &id, bool on) { Q_UNUSED(id) Q_UNUSED(on) }
    virtual void setFirstLastPiecesFirst(const QString &id, bool on) { Q_UNUSED(id) Q_UNUSED(on) }
    // Preview: the media files of a download, how much of each is on disk and whether a player can
    // start on it now (see mx/download_order.h for the rules).
    virtual PreviewInfo previewInfo(const QString &id) const { Q_UNUSED(id) return {}; }
    // The path a player should open for that file (fileIndex -1 = the single file): the partial
    // file itself when it is playable in place, else a read-only view or a temporary copy of what
    // is there. Empty + *error when it cannot be previewed now.
    virtual QString preparePreview(const QString &id, int fileIndex, QString *error)
    {
        Q_UNUSED(id) Q_UNUSED(fileIndex)
        if (error) *error = tr("Preview is not supported by this core.");
        return QString();
    }

    // ---- [added by ui-j] removing a download from the list, keeping or deleting its file.
    // What the download has on disk now (what FileRemoval::Trash would remove, what always stays).
    // Default: nothing known (known = false), so an old core never deletes anything.
    virtual DownloadDiskInfo diskInfo(const QString &id) const { Q_UNUSED(id) return {}; }
    // Remove the download from the list; a running one is stopped first.
    //   Keep: a finished file stays where it is; partial data stays on disk with what is known about
    //     it, so Load Incomplete (partialFiles(): kept = true), the "matches incomplete local file"
    //     mark of Search and downloading the same file again continue it.
    //   Trash / TrashOrDelete: diskInfo(id).paths go to the trash / Recycle Bin (mx/trash.h), with
    //     every trace in the profile (resume data, the kept record); what the download did not create
    //     is never touched. TrashOrDelete deletes for good what cannot go to the trash.
    //   Delete [api-2]: the same paths deleted for good, never to the trash (the local API's files=delete).
    // The outcome of a delete arrives in downloadFilesDeleted (after the files were closed).
    // Default: cancel(id, mode != Keep).
    virtual void removeDownload(const QString &id, FileRemoval mode) { cancel(id, mode != FileRemoval::Keep); }

    // ---- [added by ver-1] "Verify File" and "Restart Download" (Transfers context menu).
    // Verify File: every block of the download that is on disk is read again and checked against
    // the block hashes the download knows (torrent pieces, eD2k parts, tiger tree leaves) or, when
    // only a checksum of the whole file is known, the whole file. A running download stops fetching
    // while it is checked and goes on by itself afterwards. Damaged blocks, and only those, are
    // downloaded again (a finished download becomes unfinished again until they are back; it is not
    // shared as a complete file meanwhile). The outcome, in plain words, arrives in downloadNotice
    // and in the row's status text; Pause stops a check. canVerifyDownload: false (with *why for the
    // tool tip) when it cannot be done right now. Default: not supported.
    virtual bool canVerifyDownload(const QString &id, QString *why = nullptr) const
    {
        Q_UNUSED(id)
        if (why) *why = tr("This core cannot verify downloads.");
        return false;
    }
    virtual bool verifyDownload(const QString &id) { Q_UNUSED(id) return false; }
    // Restart Download: what the download received (or the finished file) is disposed of by the
    // rules of removeDownload(id, TrashOrDelete) (diskInfo(id) says what and where), then the same
    // download (same id, sources, folder, name, priority, wanted files of a bundle and its own
    // options) starts again from zero, running even if it was paused; nothing of the old data is
    // trusted or reused. The UI asks first. Default: not supported.
    virtual bool canRestartDownload(const QString &id, QString *why = nullptr) const
    {
        Q_UNUSED(id)
        if (why) *why = tr("This core cannot restart downloads.");
        return false;
    }
    virtual bool restartDownload(const QString &id) { Q_UNUSED(id) return false; }
signals:
    void transferAdded(const mx::TransferInfo &t);
    void transferChanged(const mx::TransferInfo &t);          // throttled to ~1/s per transfer
    void transferRemoved(const QString &id, bool wasUpload);
    void downloadCompleted(const mx::TransferInfo &t);
    // [added by engine-4] a download()/downloadHit()/downloadUri()/resumeDownload() request that did
    // not create (or join) a download: `what` = the hit key (downloadHit), the URI, or the request's
    // name; `reason` is human-readable ("Nothing to download: the index gave no link or hash", ...).
    // Emitted before the call returns its empty id.
    void downloadNotStarted(const QString &what, const QString &reason);
    // [added by ui-j] the files of a removeDownload(id, Trash / TrashOrDelete): moved to the trash,
    // deleted for good, and the ones still there with one reason per failed path
    void downloadFilesDeleted(const QString &id, const QString &name, const QStringList &trashed, const QStringList &deleted,
                              const QStringList &failed, const QStringList &errors);
    // [added by ver-1] the outcome of verifyDownload() / restartDownload() in one plain sentence
    // ("Verified: 'name' is intact."), for the Transfers page's notice
    void downloadNotice(const QString &id, const QString &text);
};

// ---------------------------------------------------------------- shared files page
class LibraryService : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual QList<SharedFolder> folders() const = 0;
    virtual void addFolder(const QString &path, bool recursive = true) = 0;
    virtual void removeFolder(const QString &path) = 0;
    virtual void rescan() = 0;
    virtual QList<SharedFile> files(const QString &folder = QString()) const = 0;
    virtual int fileCount() const = 0;
    virtual qint64 totalBytes() const = 0;
    virtual bool isScanning() const = 0;
    virtual QString scanStatus() const = 0;                   // "Hashing 12 of 340: foo.mp3"
    virtual void unshareFile(const QString &path) = 0;        // exclude one file (persisted)
    // [added by glue-3] "Shared File Types" dialog: checked group ids (see mx::sharedTypeIds()) and the
    // "Other file extensions" line. The core shares / announces / serves / lets browse only matching
    // files (persisted as library/sharedTypes + library/otherExtensions).
    virtual QStringList sharedTypes() const { return sharedTypeIds(); }
    virtual void setSharedTypes(const QStringList &typeIds) { Q_UNUSED(typeIds) }
    virtual QString otherExtensions() const { return QString(); }
    virtual void setOtherExtensions(const QString &extensions) { Q_UNUSED(extensions) }
    // [added by glue-3] exclude / re-include a sub folder of a shared folder (persisted; files below
    // it disappear from files()/the networks). "Share without sub folders" = addFolder(path, false).
    virtual void excludeFolder(const QString &path, bool excluded) { if (excluded) unshareFile(path); }
    virtual QStringList excludedFolders() const { return {}; }
    // [added by glue-3] standard Music / Videos / Pictures / Downloads folders that exist ("Scan For Media").
    virtual QStringList suggestMediaFolders() const { return {}; }
    // [added by ui-j] these files / folders were deleted from the disk (Shared Files ▸ Delete File...):
    // drop them (and everything below a folder) from the library now, so the page and what other
    // users browse or find update at once. Not an exclusion: a new file at the same place is shared
    // again. Default: rescan().
    virtual void forgetPaths(const QStringList &paths) { Q_UNUSED(paths) rescan(); }
signals:
    void foldersChanged();
    void filesChanged();
    void scanProgress(int done, int total, const QString &currentFile);
};

// ---------------------------------------------------------------- chat page
class ChatService : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void refreshRoomList() = 0;                       // async → roomListChanged
    // [added by glue-3] networks that offer chat rooms (running ChatBackend plugins; "List Channels"
    // network choice) and a room-list refresh on one of them (async → roomListChanged; roomList()
    // keeps the rooms of every network, ChatRoomInfo::net tells them apart).
    virtual QList<Net> chatNetworks() const { return {Net::Overlay}; }
    virtual void refreshRoomList(Net net) { Q_UNUSED(net) refreshRoomList(); }
    virtual QList<ChatRoomInfo> roomList() const = 0;
    virtual void joinRoom(const QString &roomIdOrName) = 0;   // creates the room if it does not exist
    virtual void leaveRoom(const QString &roomId) = 0;
    virtual QList<ChatRoomInfo> joinedRooms() const = 0;
    virtual QList<ChatUser> roomUsers(const QString &roomId) const = 0;
    virtual QList<ChatMessage> backlog(const QString &roomId) const = 0;
    virtual void sendMessage(const QString &roomId, const QString &text) = 0; // handles "/me", "/topic"...
    virtual void sendPrivateMessage(const UserRef &to, const QString &text) = 0;
    virtual void setTopic(const QString &roomId, const QString &topic) = 0;
    virtual QStringList favouriteRooms() const = 0;           // auto-join list (persisted)
    virtual void setFavouriteRoom(const QString &roomIdOrName, bool fav) = 0;
signals:
    void roomListChanged();
    void roomJoined(const mx::ChatRoomInfo &room);
    void roomLeft(const QString &roomId);
    void roomUpdated(const mx::ChatRoomInfo &room);           // topic / user count
    void roomUsersChanged(const QString &roomId);
    void messageReceived(const mx::ChatMessage &msg);         // room messages AND private messages
    void backlogCleared(const QString &roomId);               // [added by core] "/clear": wipe the view
    // [added by ovl-2] delivery of a PM we sent (msgId as in the echoed ChatMessage, status a PmDelivery,
    // detail: why it was not delivered / refused, empty otherwise)
    void privateMessageStatus(const mx::UserRef &to, const QString &msgId, int status, const QString &detail);
};

// ---------------------------------------------------------------- hotlist, browse, ignore
class UserService : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual QList<HotlistEntry> hotlist() const = 0;
    virtual void addToHotlist(const UserRef &u) = 0;
    virtual void removeFromHotlist(const UserRef &u) = 0;
    virtual void setNote(const UserRef &u, const QString &note) = 0;
    virtual QList<UserRef> ignored() const = 0;
    virtual void ignore(const UserRef &u, bool on) = 0;
    virtual bool isIgnored(const UserRef &u) const = 0;
    // Browse a user's shared files (overlay, eD2k "view shared files", Gnutella browse-host).
    // Returns a request id; results arrive in chunks.
    virtual quint32 browse(const UserRef &u) = 0;
    virtual void cancelBrowse(quint32 requestId) = 0;
    virtual void whois(const UserRef &u) = 0;                 // → whoisResult
    // [added by glue-3] whois with a request id (never 0) → whoisReply(requestId, ...) exactly once
    // (info["error"] set when the user could not be asked / timed out). whoisResult is still emitted.
    virtual quint32 requestWhois(const UserRef &u) { whois(u); return 0; }
    // [added by ovl-2] the user is us (our own row in a room, a search result, a Hotlist entry), by id
    virtual bool isSelf(const UserRef &u) const { Q_UNUSED(u) return false; }
signals:
    void hotlistChanged();
    void ignoredChanged();
    void browseResults(quint32 requestId, const mx::UserRef &u, const QList<mx::BrowseEntry> &files, bool finished);
    void browseFailed(quint32 requestId, const mx::UserRef &u, const QString &reason);
    void whoisResult(const mx::UserRef &u, const QVariantMap &info);
    void whoisReply(quint32 requestId, const mx::UserRef &u, const QVariantMap &info); // [glue-3]
};

// ---------------------------------------------------------------- bandwidth page
class BandwidthService : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual QList<BandwidthSample> history(int seconds = 600) const = 0;
    virtual BandwidthSample current() const = 0;
    virtual qint64 totalDownloaded() const = 0;               // session
    virtual qint64 totalUploaded() const = 0;
    virtual qint64 allTimeDownloaded() const = 0;             // persisted
    virtual qint64 allTimeUploaded() const = 0;
signals:
    void sample(const mx::BandwidthSample &s);                // once per second
};

// ---------------------------------------------------------------- connection test [added by diag-1]
// "Connection Test": ports, port mapping, reachability, outbound health and a "why is this slow"
// diagnosis per active download. report() is cheap (built from cached facts); startTest() runs the
// active checks (bind checks, port mapping, DNS, a UDP tracker handshake, a DHT ping, waiting a
// bounded time for passive evidence) and reports progress until testFinished.
class DiagnosticsService : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual DiagReport report() const = 0;
    virtual void startTest() = 0;
    virtual void cancelTest() = 0;
    virtual bool isTesting() const = 0;
    // The public TCP port checker asked by checkPortsOnline() (host name shown in the UI; empty =
    // none configured, the button is then disabled). Only ever used when the user asks.
    virtual QString portCheckService() const { return QString(); }
    // Ask the public port checker (and OctoMX overlay peers, if any are connected) whether our
    // TCP ports can be reached from the internet. Results arrive in the report (reportChanged).
    virtual void checkPortsOnline() {}
    // The one-sentence diagnosis of a download (empty = nothing to say); also in TransferInfo::diagnosis.
    virtual QString downloadDiagnosis(const QString &transferId) const { Q_UNUSED(transferId) return QString(); }
signals:
    void reportChanged();                                    // <= 1/s
    void testProgress(int percent, const QString &step);
    void testFinished(bool cancelled);
};

// ---------------------------------------------------------------- [str-1] the local streaming API
// What the Settings page shows of it (src/core/stream, docs/API.md).
struct StreamApiState {
    bool        available = false;   // this core has the API
    bool        enabled = false;     // api/enabled
    bool        listening = false;
    int         port = 0;
    QStringList addresses;           // where it listens ("127.0.0.1", "::1", "192.168.1.20")
    QString     baseUrl;             // "http://127.0.0.1:17580/api/v1/"
    QString     error;               // why it does not listen ("Port 17580 is in use by another program ...")
    int         streams = 0;         // open streams
    int         players = 0;         // player connections reading now
};

// ---------------------------------------------------------------- the facade
class Core : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual Settings *settings() = 0;
    virtual NetworkService *network() = 0;
    virtual SearchService *search() = 0;
    virtual TransferService *transfers() = 0;
    virtual LibraryService *library() = 0;
    virtual ChatService *chat() = 0;
    virtual UserService *users() = 0;
    virtual BandwidthService *bandwidth() = 0;
    virtual QString profileDir() const = 0;
    virtual QString versionString() const = 0;                // MX_VERSION_STRING (title bar)
    virtual void start() = 0;                                 // load state, start enabled networks
    virtual void shutdown() = 0;                              // save everything, stop networks
    // [safe-1] the safe quit in steps, so the program can show "Closing: saving downloads...":
    // beginShutdown() stops the transfers and starts writing and syncing what was received (returns
    // at once); shutdownBusy() = something is still being saved, shutdownBytesLeft() = how much is
    // still on its way to the disk (bytes); closeNow() = do not wait any more (what is not saved yet
    // is downloaded again). shutdown() finishes (and waits for what is left unless closeNow()).
    virtual void beginShutdown() {}
    virtual bool shutdownBusy() const { return false; }
    virtual qint64 shutdownBytesLeft() const { return 0; }
    virtual void closeNow() {}
    // [safe-1] the last session ended without the safe quit (a crash, a kill, a power cut)
    virtual bool lastStopWasUnclean() const { return false; }
    virtual bool isMock() const { return false; }
    // [added by diag-1] the Connection Test service; nullptr = not available (old cores)
    virtual DiagnosticsService *diagnostics() { return nullptr; }
    // [str-1] the local streaming API (off by default; settings api/*): its state for the Settings page
    virtual StreamApiState streamApiState() const { return {}; }
signals:
    void statusMessage(const QString &text);                  // status-bar text
    void shutdownFinished();
    void streamApiChanged();                                  // [str-1] streamApiState() changed
};

// Factories. createRealCore is defined in src/core (target mxcore); it takes ownership of the
// plugins (the app creates them: see each src/net/<name>/<name>_plugin.h factory function).
// createMockCore is defined in src/mock (target mxmock).
class NetworkPlugin;
Core *createRealCore(const QString &profileDir, const QList<NetworkPlugin *> &plugins, QObject *parent = nullptr);
Core *createMockCore(const QString &profileDir, QObject *parent = nullptr);

} // namespace mx
