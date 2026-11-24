// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX: the libtorrent session worker. Lives in its own QThread; owns the lt::session and
// makes every (potentially blocking) libtorrent call. Talks to the main-thread facade only through
// queued invocations on the sink object. Private to mxnet_bt.
#pragma once

#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QTimer>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <vector>

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/fwd.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_status.hpp>

#include "bt_common.h"
#include "bt_pacing.h"
#include "mx/plugin.h"

namespace mx {
namespace bt {

// What the worker reports to the facade (all calls arrive queued on the main thread).
class WorkerSink {
public:
    virtual ~WorkerSink() = default;
    virtual void wTorrentAdded(const TorrentSnapshot &snap, const QString &seededPath) = 0;
    virtual void wTorrentMetadata(const QString &id, const QByteArray &pieceHashes, const TorrentSnapshot &snap) = 0;
    virtual void wSnapshots(const QList<TorrentSnapshot> &snaps) = 0;
    virtual void wPieces(const QString &id, const QList<int> &finished, const QList<int> &failed) = 0;
    virtual void wTorrentFinished(const TorrentSnapshot &snap) = 0;   // fresh snapshot
    virtual void wTorrentRemoved(const QString &id) = 0;
    virtual void wTorrentError(const QString &id, const QString &text) = 0;
    // [engine-3] move_storage finished (ok → path is the new save path)
    virtual void wStorageMoved(const QString &id, const QString &path, bool ok, const QString &error)
    { Q_UNUSED(id) Q_UNUSED(path) Q_UNUSED(ok) Q_UNUSED(error) }
    virtual void wStatus(const NetworkStatus &st, int dhtNodes) = 0;
    virtual void wUploads(const QList<TransferInfo> &changed, const QStringList &finished) = 0;
    virtual void wLog(const QString &text) = 0;
    // [glue-3] wire = peer traffic + DHT + tracker bytes since the last call; payload = file data
    virtual void wAccount(qint64 wireDown, qint64 wireUp, qint64 payloadDown, qint64 payloadUp) = 0;
    virtual void wWaiting(qint64 down, qint64 up) { Q_UNUSED(down) Q_UNUSED(up) }   // [bw-2] bytes queued for the rate limit
    virtual void wAccountDiscovery(qint64 down, qint64 up) { Q_UNUSED(down) Q_UNUSED(up) }   // [bw-2] DHT + trackers
    // [diag-1] connection test counters, once a second with the snapshots
    virtual void wDiag(const QHash<QString, TorrentDiag> &torrents, const PluginDiagnostics &session)
    { Q_UNUSED(torrents) Q_UNUSED(session) }
};

// Thread-agnostic helpers (no session calls).
QByteArray v1PieceHashes(const lt::torrent_info &ti);                     // 20 bytes per piece; empty for v2-only
TorrentSnapshot snapshotFromParams(const lt::add_torrent_params &atp, const Record &rec);

struct AddRequest {
    QString id;
    std::shared_ptr<lt::add_torrent_params> atp;   // fully prepared (save_path, flags, priorities...)
    Record rec;
    QByteArray torrentFile;                         // raw .torrent to persist (may be empty)
    QList<int> wantedFiles;
};

class Worker : public QObject {
    Q_OBJECT
public:
    Worker(QObject *sinkObject, WorkerSink *sink, const QString &dataDir);
    ~Worker() override;

    // ---- all of these run in the worker thread (posted by the facade)
    void init(const Config &cfg, const QStringList &userNodes, const QStringList &userTrackers);
    void goOnline(const Config &cfg);
    void goOffline(int timeoutMs);                 // save everything, then take the session offline
    void teardown(int timeoutMs);                  // save everything + destroy the session
    void applyConfig(const Config &cfg);
    void setRateLimits(int downBps, int upBps);
    void setUserNodes(const QStringList &nodes, const QStringList &trackers);
    void addDhtNode(const QString &host, int port);

    void addTorrent(const AddRequest &req);
    void mergeTrackers(const QString &id, const std::vector<std::string> &trackers);
    void removeTorrent(const QString &id, bool deleteFiles);
    // [net-3] the upload rows of a torrent end now with this reason (it stops being offered)
    void endUploads(const QString &id, const QString &why);
    // [net-3] the Library's shared files (canonical paths): a finished torrent whose shared file left
    // the list stops seeding ("not shared"), and seeds again once all of them are back
    void applySharing(const QSet<QString> &sharedPaths);
    void pauseTorrent(const QString &id);
    void resumeTorrent(const QString &id);
    void setWantedFiles(const QString &id, const QList<int> &indexes);
    void setPriority(const QString &id, int priority);
    void recheck(const QString &id);
    void moveStorage(const QString &id, const QString &dir);   // [engine-3]
    void reannounce(const QString &id);
    void reserve(const QString &id, const QList<int> &pieces, bool reserve);
    // [dl-1] sequential_download flag + top priority for the head / tail band pieces of each wanted file
    void setOrder(const QString &id, bool sequential, bool firstLast);
    void inject(const QString &id, int piece, const QByteArray &data);
    void addPeer(const QString &id, const QString &endpoint);
    void banPeerAddress(const QString &address);

    static QString torrentsDir(const QString &dataDir) { return dataDir + QStringLiteral("/torrents"); }

private:
    struct Inject {
        QByteArray data;
        int retries = 0;
    };
    struct UpRow {
        TransferInfo info;
        qint64 lastActiveMs = 0;
        qint64 lastTotal = 0;     // libtorrent's per-connection counter at the last refresh
        qint64 base = 0;          // bytes of earlier connections of the same peer (address)
        bool peerSeed = false;    // the peer has the whole torrent now
        int peerProgressPpm = 0;  // the peer's own completion at the last refresh
        int peerRateBps = 0;      // how fast we were sending to it then
        qint64 lastSeenMs = 0;    // the last refresh that saw the peer connected
        bool announced = false;
    };
    struct Torrent {
        QString id;
        lt::torrent_handle h;
        bool inSession = false;
        Record rec;
        std::shared_ptr<const lt::torrent_info> ti;
        std::vector<lt::download_priority_t> filePrio;
        QList<int> pendingWanted;        // wanted indexes to apply once metadata is known
        std::vector<lt::tcp::endpoint> pendingPeers;   // addPeer() before the torrent was in the session
        std::vector<std::function<void()>> pendingOps; // commands issued before add_torrent_alert
        std::set<int> reserved;
        std::vector<bool> replayed;      // restored from resume data: libtorrent re-posts piece_finished for these at load
        std::map<int, Inject> injected;  // waiting for piece_finished / hash_failed
        std::map<int, QByteArray> held;  // injected while checking files
        lt::torrent_status st;
        bool haveStatus = false;
        bool finishedReported = false;
        bool metadataReported = false;
        bool torrentFileWritten = false;
        bool removing = false;
        bool deleteFiles = false;
        bool sharingChecked = false;     // [net-3] compared with the last shared list
        qint64 lastSeedLookMs = 0;       // [net-3] status refreshed for the seeding time limit
        QList<TransferSource> sources;
        QList<TransferSource> channels;  // [engine-4] DHT / trackers / PEX / web seeds (TorrentSnapshot::channels)
        QList<TransferSource> trackerRows;
        qint64 lastTrackersMs = 0;
        qint64 lastResumeMs = 0;
        bool resumeDirty = false;
        QList<int> pieceDone, pieceFail; // batched for the sink
        // [diag-1] connection test counters
        TorrentDiag diag;
        int encPeers = 0, plainPeers = 0, utpPeers = 0;   // [enc-1] connected peers by encryption / transport
        QHash<QString, TrackerDiag> trk;  // by announce URL: what the tracker alerts said
        // [dl-1] download order (set by the engine each session, not persisted here)
        bool seqOrder = false;
        bool firstLast = false;
        // [ver-1] checks of the files that ended (TorrentSnapshot::checksDone); a paused torrent that is
        // rechecked runs for the check only and is paused again when it ends
        int checksAsked = 0, checksDone = 0;
        bool pauseAfterCheck = false;
        bool recheckOfFinished = false;   // the recheck began on a finished torrent
        std::set<int> bandPieces;         // pieces raised to top priority by firstLast
        // [ui-i] pieces in libtorrent's download queue with a block requested / being written
        // (TorrentSnapshot::downloading), refreshed with every status update
        QBitArray downloading;
    };

    void post(std::function<void()> fn);
    void scheduleAlerts();
    void drainAlerts();
    void handleAlert(lt::alert *a);
    void tick();                                   // 1 Hz
    void periodicSave();                           // 30 s
    void saveSessionState();
    int saveAllResume(int timeoutMs, bool flush);  // returns number saved
    void flushPieceEvents();

    Torrent *find(const QString &id);
    // true if the command was queued because the torrent is not in the session yet
    bool defer(Torrent *t, std::function<void()> op);
    Torrent *find(const lt::torrent_handle &h);
    void loadPersisted();
    void persistRecord(const Torrent &t);
    void deletePersisted(const QString &id);
    void requestResume(Torrent &t, bool infoDict = true);
    void writeResume(const lt::add_torrent_params &p);
    void onMetadata(Torrent &t);
    void applyWanted(Torrent &t, const QList<int> &indexes);
    void applyReservations(Torrent &t);
    void applyFirstLast(Torrent &t);   // [dl-1] after anything that rewrites piece priorities
    lt::download_priority_t naturalPiecePriority(const Torrent &t, int piece) const;
    bool reallyFinished(const Torrent &t) const;
    void checkFinished(Torrent &t);
    void enforceSeedLimits(Torrent &t);
    void flushHeld(Torrent &t);
    TorrentSnapshot makeSnapshot(Torrent &t, bool withFiles = true);
    void refreshQueue(Torrent &t);   // [ui-i]
    void refreshPeers(Torrent &t, qint64 nowMs, QList<TransferInfo> &upChanged, QSet<QString> &seenUp);
    void refreshChannels(Torrent &t, qint64 nowMs, const std::vector<lt::peer_info> &peers);   // [engine-4]
    void sweepUploads(qint64 nowMs, const QSet<QString> &refreshedTorrents, const QSet<QString> &seenUp,
                      QList<TransferInfo> &upChanged);
    void updateStatus();
    void applySettingsNow();
    lt::settings_pack currentPack() const;
    void applyPeerClassFilter();
    void applyIpFilter();
    void wakeTorrent(Torrent &t);
    void refreshSoon(const QString &id);
    void diagAlert(lt::alert *a);                  // [diag-1]
    void postDiag();                               // [diag-1]   // re-read status shortly after async priority changes    // reconnect after leaving libtorrent's finished state (see .cpp)

    QPointer<QObject> m_sinkObj;
    WorkerSink *m_sink;
    QString m_dataDir;
    std::unique_ptr<lt::session> m_ses;
    Config m_cfg;
    bool m_online = false;
    int m_port = 0;              // configured/derived port for listen_interfaces
    int m_downBps = 0, m_upBps = 0;
    QStringList m_userNodes, m_userTrackers;
    std::map<QString, Torrent> m_t;
    QSet<QString> m_sharedPaths;   // [net-3] the last shared list (canonical paths)
    bool m_haveSharing = false;
    void checkSharing(Torrent &t);
    std::unordered_map<lt::torrent_handle, QString> m_byHandle;
    std::map<QString, UpRow> m_up;
    std::atomic<bool> m_alertsPending{false};
    QTimer *m_tick = nullptr;
    QTimer *m_statsTimer = nullptr;   // [bw-2] session stats every 100 ms
    QTimer *m_saveTimer = nullptr;
    QTimer *m_stateTimer = nullptr;
    QElapsedTimer m_clock;
    // network status
    QSet<QString> m_listen;      // "ip:port" tcp listen sockets
    int m_listenPort = 0;
    QString m_externalIp;
    int m_dhtNodes = 0;
    bool m_incoming = false;
    qint64 m_lastRecv = -1, m_lastSent = -1, m_lastStatsMs = 0;
    qint64 m_lastRecvPayload = 0, m_lastSentPayload = 0;
    int m_idxRecvPayload = -1, m_idxSentPayload = -1;
    int m_idxDhtIn = -1, m_idxDhtOut = -1, m_idxTrackerIn = -1, m_idxTrackerOut = -1; // [glue-3]
    int m_idxLimDownBytes = -1, m_idxLimUpBytes = -1;   // [bw-2] bytes waiting for the rate limit
    int m_idxDownPeers = -1, m_idxUpPeers = -1;         // [bw-2] peers with requests outstanding
    std::shared_ptr<PacingState> m_pacing = std::make_shared<PacingState>();
    int m_upPeers = 1;
    qint64 m_lastOtherRecv = 0, m_lastOtherSent = 0;   // [bw-2] DHT + tracker counters
    int m_appliedSendBuf = -1;
    void updatePacingPeers(int downPeers, int upPeers, qint64 recvBytes);
    qint64 m_pacingSinceMs = -1, m_pacingRecv0 = 0;
    qint64 m_lastWireRecv = -1, m_lastWireSent = 0;
    int m_rateDown = 0, m_rateUp = 0;
    int m_idxDhtNodes = -1, m_idxRecv = -1, m_idxSent = -1, m_idxIncoming = -1;
    int m_resumePending = 0;     // outstanding save_resume_data requests
    QSet<QString> m_banned;
    // [diag-1] connection test
    QString m_listenError;
    int m_listenErrorPort = 0;
    std::vector<qint64> m_incomingMs, m_incomingUtpMs;   // accepted incoming connections (last 15 min)
    int m_connectOk = 0, m_connectFailed = 0;
    int m_idxDhtMsgIn = -1;
    qint64 m_dhtMsgIn = 0;
    QHash<QString, PortMapping> m_ownMaps;   // libtorrent's own mappings ("TCP/6881")
};

} // namespace bt
} // namespace mx
