// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX engine: one download (a file or a torrent bundle): identity, sources, the shared range
// scheduler, the store (PartStore or TorrentStore), its state machine and persistence.
#pragma once

#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QSet>

#include <memory>
#include <vector>

#include "core/dao.h"
#include "core/diag/diag_rules.h"
#include "core/engine/engine_types.h"
#include "core/engine/part_store.h"
#include "core/engine/torrent_store.h"

namespace mx {
class Settings;
class Library;
class DownloadsDao;
class HashLinksDao;
}

namespace mx::engine {

// What a Download needs from the engine (implemented by DownloadEngine::Private).
class EngineHost {
public:
    virtual ~EngineHost() = default;
    virtual Settings *settings() const = 0;
    virtual DownloadsDao *dao() const = 0;
    virtual HashLinksDao *links() const = 0;
    virtual Library *library() const = 0;
    virtual TorrentBackend *backend() const = 0;
    virtual QList<NetworkPlugin *> plugins() const = 0;
    virtual bool pluginUsable(NetworkPlugin *p) const = 0;     // enabled + not Disabled
    virtual NetworkPlugin *pluginFor(Net n) const = 0;        // CanFetchRanges plugin of that net
    virtual QThreadPool *pool() const = 0;
    virtual const Tuning &tuning() const = 0;
    virtual QString incomingDir() const = 0;
    virtual QString incompleteDir() const = 0;
    virtual int openSourcesGlobal() const = 0;
    virtual qint64 bufferedGlobal() const = 0;
    virtual void changed(class Download *d, bool important) = 0;   // → transferChanged (throttled)
    virtual void finished(class Download *d) = 0;                  // completed: frees a queue slot
    virtual void completedFile(class Download *d) = 0;             // downloadCompleted
};

class Download : public QObject {
    Q_OBJECT
public:
    Download(EngineHost *host, const DownloadRecord &rec, QObject *parent = nullptr);
    ~Download() override;

    // ---- identity / record
    QString id() const { return m_rec.id; }
    DownloadRecord &record() { return m_rec; }
    const DownloadRecord &record() const { return m_rec; }
    const FileHashes &hashes() const { return m_rec.hashes; }
    qint64 size() const { return m_rec.size; }
    bool isFinished() const;          // Completed (or Seeding)
    bool isPausedByUser() const { return m_userPaused; }
    bool hasError() const { return !m_rec.errorText.isEmpty() && m_errorState; }
    bool isActive() const { return m_active; }
    bool wantsToRun() const;          // not paused, not finished, not errored
    QString torrentId() const { return m_torrentId; }
    QString originalUri() const { return m_rec.uri; }
    QString partPath() const;

    void restoreState(const QByteArray &blob);
    QByteArray saveState() const;
    void persist(bool full);          // full = upsert record + blob; else progress + blob (deferred)

    // ---- control (called by the engine)
    void activate();
    void deactivate();                // → Locally Queued (engine decides)
    void setUserPaused(bool paused);
    void retry();
    void cancel(bool deleteData);
    // [ui-j] what the download has on disk (what deleting it would remove, what always stays)
    // pruneDirs: folders of a folder torrent to remove afterwards when they are then empty
    DownloadDiskInfo diskInfo(QStringList *pruneDirs = nullptr) const;
    // [ui-j] stop for removal: every source closed, pending writes of the part file written (keep)
    // or dropped (delete), the part file closed; a torrent leaves the backend (its files stay, its
    // resume data in the profile goes). Nothing is deleted here.
    void stopForRemoval(bool keepData);
    // [ui-j] a finished row that points to a file the download did not create (already in the library)
    void setNotOwned(bool on) { m_notOwned = on; }
    void setPriority(int p);
    void setWantedFiles(const QList<int> &idx);
    void shutdown(int drainMs);       // persist, close sources, release reservations (no deletes)
    void tick(qint64 now);            // 1 Hz housekeeping
    void addSources(const QList<SearchSource> &sources, NetworkPlugin *from, bool fromRequest);
    void mergeRequest(const DownloadRequest &req);
    void learnHashes(const FileHashes &more, bool trusted, const QString &why);
    void learnHashSet(const QString &kind, qint64 unit, const QList<QByteArray> &hashes);
    void hashLinksFound(const FileHashes &all, qint64 size);
    bool matchesLink(const FileHashes &known, qint64 size) const;
    void networkCameUp(NetworkPlugin *p);   // a plugin just connected: ask it now
    // [glue-2] per-transfer / per-source actions
    void findMoreSources();                 // ask every network again now (no state reset)
    void enterQueue();                      // reopen every known source now (back-off cleared)
    bool removeSource(const QString &key);  // drop a source for good (not re-added when found again)
    bool setSourcePriority(const QString &key, int priority);
    bool setTarget(const QString &dir, const QString &name);   // final location (part-file downloads)
    // Blob for restoreState(): `path` is an existing partial file whose first `held` bytes are data.
    static QByteArray stateForExistingFile(const QString &path, qint64 held);
    // [engine-4] per-download Auto Find Sources (minutes, 0 = never) / Auto Enter Queue (max remote
    // queue rank, 0 = never); -1 = global default. Persisted in the blob.
    void setAutoFindSources(int minutes);
    void setAutoEnterQueue(int n);
    int effectiveAfs() const;
    int effectiveAeq() const;
    // [engine-4] sources of the search hit the download was started from (and later updates of it)
    void addHitSources(const SearchHit &hit);
    // [engine-4] background size+name search on the keyword networks (candidates, verified before trust)
    bool wantsNameSearch(qint64 now) const;
    SearchQuery nameSearchQuery() const;
    void nameSearchStarted(qint64 now, const QList<Net> &nets);
    void nameSearchHits(const QList<SearchHit> &hits);
    void nameSearchFinished();
    QStringList takeFileListWants();        // [merge-1] bundle rows of the name search worth a file list
    // [engine-4] sources we exchange data / queue slots with right now (S= / secondary connections)
    int connectedSources() const;
    // [dl-1] download order (priorities, never conditions) and preview of the partial file
    void setOrder(bool sequential, bool firstLast, bool notify = true);   // notify = false: before transferAdded
    bool sequential() const { return m_seq; }
    bool firstLast() const { return m_firstLast; }
    PreviewInfo previewInfo() const;
    QString preparePreview(int fileIndex, QString *error);
    void dropPreview();               // remove the preview link / copy (best effort)
    static QString previewDirOf(const QString &incompleteDir);

    // ---- torrent events (routed by the engine by id)
    void onTorrentMetadata();
    void onTorrentChanged();
    void onTorrentPieceFinished(int piece);
    void onTorrentPieceFailed(int piece);
    void onTorrentFinished();
    void onTorrentError(const QString &text);
    void onTorrentRemoved();
    void torrentReAdd();              // [ui-j] the removal of an earlier download of this torrent finished
    void onTorrentStorageMoved(const QString &path, bool ok, const QString &error);   // [engine-3]
    void onUrlTorrentAdded(const QString &url, const QString &id);
    void onUrlTorrentFailed(const QString &url, const QString &error);
    QString promoteTorrentId() const { return m_promoteTorrent; }
    bool waitsForUrl(const QString &url) const
    {
        return (!m_pendingTorrentUrl.isEmpty() && m_pendingTorrentUrl == url) || (!m_promoteUrl.isEmpty() && m_promoteUrl == url);
    }

    // ---- library hand-over
    void onFileHashed(const SharedFile &f);
    bool awaitsHashOf(const QString &path) const { return m_awaitHashPaths.contains(path); }

    // ---- reporting
    TransferInfo info() const;
    TransferInfo briefInfo() const;   // [engine-4] identity + state, no source rows
    void pieceMapOf(QByteArray *state, QByteArray *fill) const;   // [ui-i] TransferInfo::pieceMap / pieceFill
    int knownSources() const;
    int waitingUnverified() const;
    bool checkDeadEnd(qint64 now);    // hint-only / candidate sources we cannot verify yet
    int openSources() const;
    qint64 bufferedBytes() const;
    QVariantMap debugInfo() const;
    // [diag-1] counters for the Connection Test, and the sentence it derived (TransferInfo::diagnosis)
    diag::DownloadFacts diagFacts() const;
    bool setDiagnosis(const QString &text, int level);   // true = changed
    // partial sharing (thread-safe parts are inside PartStore)
    PartStore *partStore() const { return m_part.get(); }

signals:
    void needsSchedule();

private:
    // stores
    void ensureStore();
    void createPartStore();
    void resetBrokenStore();
    bool startTorrent(QString *error);
    void attachStore(Store *s);
    Store *store() const;
    bool isTorrent() const { return m_torrent != nullptr; }
    // sources
    SourceSlot *slotFor(const QString &key) const;
    void openMoreSources();
    void openSlot(SourceSlot *s);
    void closeSlot(SourceSlot *s, bool failed, const QString &why);
    void closeAllSources();
    void connectSource(SourceSlot *s);
    void onSourceState(SourceSlot *s);
    void onSourceData(SourceSlot *s, qint64 off, const QByteArray &bytes);
    void onSourceFinished(SourceSlot *s, qint64 off, qint64 len, bool ok, const QString &err);
    void strike(SourceSlot *s, const QString &why);
    void ban(SourceSlot *s, const QString &why);
    void cancelSlotRequests(SourceSlot *s, const QString &why);
    void mapSourcesToFiles();
    bool mapSlot(SourceSlot *s);
    bool slotUsable(const SourceSlot *s) const;
    bool slotReady(const SourceSlot *s) const;
    RangeSet slotAvailability(const SourceSlot *s) const;
    // scheduling
    void schedule();
    bool pickChunk(SourceSlot *s, const RangeSet &freeSet, const RangeSet &inflight, bool endgame, Request *out);
    void issue(SourceSlot *s, const Request &r);
    void checkTimeouts(qint64 now);
    void replaceSlowSources(qint64 now);
    void cancelRedundant();
    // verification results
    void onUnitVerified(qint64 s, qint64 e);
    void onUnitFailed(qint64 s, qint64 e);
    void onUnitAbandoned(qint64 s, qint64 e);
    void onStoreError(const QString &text, bool diskFull);
    void applyHashSets();
    // [engine-3] per-source hash sets (RangeSource::hashSetReceived) + corruption black box
    void onSourceHashSet(SourceSlot *s, const QString &kind, qint64 unit, const QList<QByteArray> &hashes);
    bool adoptEd2kSet(const QList<QByteArray> &data, const QString &providerKey, bool trusted);
    void dropProvisionalSet(const QString &why);
    void banKey(const QString &key, const QString &why);
    void recordBlackBox(qint64 s, qint64 e, const QList<SourceSlot *> &contributors);
    void checkBlackBox(qint64 s, const QSet<QString> &struck);
    // discovery
    void findSources(bool force);
    void stopFindSources();
    void lookupHashLinks();
    // completion
    void maybeComplete();
    void finalizePart();
    void finishPart(const FileHashes &all, const QList<QByteArray> &ed2kParts, const QList<QByteArray> &aichParts, bool haveAll);
    void wholeFileFailed(const QStringList &mismatch);
    void completeTorrent();
    void publishLinks(const FileHashes &all);
    // promotion part → torrent
    void considerPromotion();
    bool canFallBackToPart() const;
    void fallBackToPart();
    void tryPromote();
    // status
    void updateStatus();
    void setState(TransferState st, const QString &text);
    void setError(const QString &text, bool pauseIt);
    void markDirty(bool important = false);

    EngineHost *m_host;
    DownloadRecord m_rec;
    FileHashes m_hinted;                       // unconfirmed hash-link hints (not trusted)
    // [core-5] a TTH tree that arrived for a hinted (not trusted) TTH root: kept until the root is
    // trusted, never used to judge data before that
    qint64 m_hintTthUnit = 0;
    QList<QByteArray> m_hintTthLeaves;
    void checkClaimsAgainst(const FileHashes &verified);
    QList<FileHashes> m_rejected;              // hints / candidates proven wrong
    QList<QByteArray> m_ed2kParts;             // validated part hashes
    bool m_ed2kProvisional = false;            // [engine-3] m_ed2kParts reduce to no trusted ed2k hash (yet)
    QByteArray m_provisionalRoot;              // the ed2k root the provisional set reduces to
    QString m_ed2kSetProvider;                 // source key that sent m_ed2kParts ("" = plugin-level / restored)
    struct BlackBoxEntry { QString key; QList<Range> ranges; QList<QByteArray> sha1; };
    QHash<qint64, QList<BlackBoxEntry>> m_blackBox;   // failed unit start → what each contributor had sent
    QString m_movingTo;                        // [engine-3] torrent move_storage in progress
    QString m_moveError;                       // last failed move (debugInfo / status)
    int m_unitFailures = 0;                    // this session (debugInfo)
    qint64 m_failedUnitBytes = 0;              // bytes of failed units = what had to be fetched again
    qint64 m_tthUnit = 0;
    QList<QByteArray> m_tthLeaves;             // validated TTH level
    std::unique_ptr<PartStore> m_part;
    std::unique_ptr<TorrentStore> m_torrent;
    QString m_torrentId;
    QString m_pendingTorrentUrl;
    QString m_partPath;
    QString m_fallbackTorrent;                 // part file running beside a torrent that had no metadata
    bool m_metaStalled = false;
    bool m_deadEnd = false;                    // gave up: every source banned / dead (no auto-retry)
    QString m_torrentUrlHint;                  // extra["torrentUrl"] of a BitTorrent source
    bool m_triedTorrentUrl = false;                        // persisted once the part file exists
    QStringList m_trackers;                    // from BitTorrent sources' magnets
    std::vector<std::unique_ptr<SourceSlot>> m_slots;
    QHash<QString, SourceSlot *> m_byKey;
    QHash<QString, int> m_strikes;             // persisted per source key
    QSet<QString> m_banned;
    RangeSet m_restoreHeld, m_restoreVerified;  // from the blob until the part store exists
    bool m_active = false;
    bool m_userPaused = false;
    bool m_errorState = false;
    bool m_completing = false;
    bool m_singleSource = false;               // whole-file failure policy
    int m_wholeFileFailures = 0;
    qint64 m_awaitHashSetUntil = 0;             // whole-file failure: wait for a hash set first
    bool m_promoting = false;
    QString m_promoteTorrent;                  // candidate torrent id while promotion is checked
    int m_promoteFile = -1;
    bool m_promoteTrusted = false;
    QSet<QString> m_awaitHashPaths;            // completed files handed to the library hasher
    qint64 m_lastFindMs = 0;
    QByteArray m_lastFindKey;
    int m_findRound = 0;
    qint64 m_lastPersistMs = 0;
    qint64 m_activatedMs = 0;
    qint64 m_lastDataMs = 0;
    qint64 m_lastScheduleMs = 0;
    bool m_dirtyBlob = false;
    bool m_seenSourceSinceActivation = false;
    double m_rateEma = 0;
    QString m_diagnosis;          // [diag-1]
    int m_diagnosisLevel = 0;
    qint64 m_lastRateSampleMs = 0;
    QHash<int, qint64> m_verifiedByNet;        // verified bytes per mx::Net (foreign + part)
    qint64 m_btBytesAtStart = -1;
    QSet<QString> m_publishedLinks;
    bool m_schedPending = false;
    qint64 m_errorAtMs = 0;
    int m_retryAttempts = 0;
    // failed units with several contributors are re-fetched from ONE source ("isolated"), so a
    // second failure names the culprit; innocent contributors get their strike back on success
    QSet<qint64> m_isolated;                   // unit starts
    QHash<qint64, SourceSlot *> m_unitOwner;   // isolated unit → the source allowed to fill it
    QHash<qint64, QSet<QString>> m_failedBy;   // unit start → keys struck for it
    QHash<QString, int> m_srcPriority;         // [glue-2] persisted per source key
    QSet<QString> m_userRemoved;               // [glue-2] persisted: cancelSource()
    // [engine-4]
    void sourceRows(QList<TransferSource> &out, QSet<int> &nets, int *active, qint64 now) const;
    void torrentRows(QList<TransferSource> &out, QSet<int> &nets, int *active) const;
    void discoveryRows(QList<TransferSource> &out) const;
    QString sourceStatus(const SourceSlot *s, qint64 now, int *rank) const;
    QString discoveryText(qint64 now) const;
    bool provisionalUsable(const SourceSlot *s) const;
    void considerProvisional(qint64 now);
    void enforceQueueLimit(SourceSlot *s);
    void creditWholeFile();
    SourceSlot *slotByAddress(const SearchSource &src) const;
    QHash<QString, SourceSlot *> m_byAddr;     // "<net family>|ip:port" → slot (one peer = one row)
    int m_afs = -1, m_aeq = -1;                // per-download overrides (-1 = global)
    bool m_queueOverride = false;              // "Enter Queue" pressed: stay in every remote queue (session)
    bool m_provisional = false;                // candidates used without a fine hash set (whole-file check decides)
    FileHashes m_provisionalClaim;             // the claim the provisionally used candidates share
    int m_nextOrder = 0;
    QHash<int, int> m_foundByNet;              // new sources per mx::Net this session
    QHash<int, int> m_reportedByNet;           // search-hit counts of networks that gave no address (Kad, eD2k servers)
    QSet<int> m_askedNets;                     // networks asked with findSources()
    int m_nameSearches = 0;                    // background size+name searches this session
    qint64 m_nextNameSearchMs = 0;
    bool m_nameSearchRunning = false;
    QList<Net> m_nameSearchNets;
    int m_nameCandidates = 0;                  // candidate sources the name searches found
    qint64 m_lastAskMs = 0;                    // last findSources round (for "next search in")
    qint64 m_lastTrustedDataMs = 0;
    QString fetchingFromIndexText() const;
    static QString summaryPeerStatus(const QList<TransferSource> &rest);
    QString indexNote() const;
    QString trackerNote() const;               // ", 4 trackers report no seeds"
    void logShares() const;                 // " (LimeTorrents reported 6 seeds, 4 peers)"
    int m_indexSeeds = -1, m_indexPeers = -1;  // swarm numbers a torrent index reported
    QString m_indexName;
    qint64 m_lastReannounceMs = 0;            // data from a trusted source (provisional mode decision)
    // [dl-1]
    bool m_seq = false, m_firstLast = false;
    // [ui-j] which torrent files were on the disk before this download (never deleted with it) and
    // whether it created the top folder of a folder torrent; known once the file list was known
    bool m_ownKnown = false;
    QStringList m_preexisting;                // torrent file paths (TorrentFile::path) that existed before
    bool m_topExisted = false;
    bool m_notOwned = false;                  // finished row of a file that was already in the library
    void noteOwnership(const TorrentSnapshot &sn);
    bool m_orderBackendOk = false;            // the torrent backend follows the flags
    QString m_previewPath;                    // link / copy handed to a player (part files)
    int m_moveRetries = 0;                    // completion waits for a preview player to let go (Windows)
    QList<Range> orderFiles() const;          // wanted file spans (store coordinates), file order
    bool orderedStart(SourceSlot *s, const RangeSet &cand, const QList<RangeSet> &peerAvail, qint64 *pos);
    void applyTorrentOrder();
    QList<Net> orderFollowers() const;
    PreviewFile previewOf(int fileIndex, QString *path) const;
    // [merge-1] sources matched only by name and size: one judge, confirm or drop per network, waste cap
    struct CandNet {
        int     failed = 0;        // different sources of it that failed the check
        bool    confirmed = false; // a verified unit came from it
        bool    dropped = false;   // "the eD2k copy is a different file"
        qint64  fetched = 0;       // bytes fetched from it before it was confirmed / dropped
        QString why;
    };
    QHash<int, CandNet> m_candNets;           // by netFamily(), persisted
    FileHashes m_confirmedClaims;             // hash kinds learned only from confirmed candidates
    QHash<QString, int> m_confirmedFrom;      // kind ("ed2k", "sha1" ...) → netFamily it came from
    QString m_completionNote;                 // a hash disagreement found at completion, in plain words
    QList<QByteArray> m_btCandidates;         // further torrents that may hold this file (tried in turn)
    // [eng-1] which torrent is tried first, and what happens to one that gives no file list: the
    // search's swarm figure ranks them; a fetch without metadata for transfers/metadataTimeoutSec is
    // set aside (back-off 2, 5, 10, 20, 30 min) while another candidate is tried; the row says it all
    QHash<QByteArray, int> m_btSwarm;         // seeds + peers an index reported (-1 = unknown), persisted
    QHash<QByteArray, QString> m_btNames;     // torrent (folder) name for the row, persisted
    QHash<QByteArray, qint64> m_btRetryAt;    // set aside until (this session)
    QHash<QByteArray, int> m_btTries;         // fetches that gave no file list (this session)
    qint64 m_promoteSinceMs = 0;              // the current metadata fetch started (or was adopted)
    qint64 m_promoteAnnounceMs = 0;           // last re-announce of it
    int m_promotePeersMax = 0;                // most peers it had connected
    QString m_btNote;                         // the last thing that happened to a torrent, for the row
    QString m_restorePromote;                 // promotion torrent of the last session (adopted on restore)
    bool m_restorePromoteTrusted = false;
    void addBtCandidate(const QByteArray &btih);
    int btRank(const QByteArray &btih, qint64 now) const;
    bool btCandidateReady(qint64 now, QByteArray *best = nullptr) const;
    void setAsideTorrent(qint64 now, const QString &why);
    void watchPromotion(qint64 now);
    qint64 promoteLimitMs(qint64 now) const;  // how long the current fetch gets before the next candidate
    QString btLabel(const QByteArray &btih) const;
    bool btCandidateKnown() const;
    QString btPartStatus(qint64 now) const;   // the BitTorrent row while the part file runs
    QString btSwarmStatus() const;            // the BitTorrent summary row of a torrent download
    QByteArray m_promoteBlocked;              // a torrent promotion cannot take (exists already / cannot be added)
    QHash<QByteArray, QString> m_btTorrentUrls;   // .torrent URLs of candidate torrents (Internet Archive & co.)
    QString m_promoteUrl;                     // .torrent of a candidate torrent being fetched for promotion
    QByteArray m_promoteExpect;               // its info-hash
    FileHashes m_candidateHints;              // hint kinds that came from copies matched by name and size
    bool m_stalledSwap = false;               // the torrent gave no file list: the other copy's hashes judge
    void candidateFailed(SourceSlot *c, const QString &why);
    void candidateConfirmed(SourceSlot *c);
    void dropNetwork(int fam, const QString &why);
    bool candidateCapped(const SourceSlot *s, RangeSet *allowed) const;
    QString judgeText() const;
    QString disagreementNote(const QStringList &kinds) const;
    void forgetKinds(const QStringList &kinds);
    void nextTorrentCandidate();
    // [merge-1] the background search per file (the file, or each large file of a bundle)
    struct NameSubject {
        int     file = -1;         // torrent file index (-1 = the download itself)
        QString name;
        QString path;              // path inside the bundle
        qint64  size = 0;
    };
    QList<NameSubject> nameSubjects() const;
    NameSubject m_curSubject;
    int m_subjectTurn = 0;
    struct NameNetStat {
        int copies = 0;            // hits with the same size and name (all rounds)
        int sources = 0;           // sources they named
        int otherSize = 0;         // same name, another size (not the same file)
        int rounds = 0;            // rounds this network was searched in
        QString skipped;           // why it was not searched in the last round
    };
    QHash<int, NameNetStat> m_nameStats;      // by Net
    QStringList m_wantFileLists;
    qint64 m_lastNameSearchMs = 0;
    QString nameSearchNote(int net, qint64 now) const;
    bool lacksSources(qint64 now) const;
};

} // namespace mx::engine
