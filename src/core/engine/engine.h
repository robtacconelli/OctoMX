// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX core: THE SEAM for the multi-network download engine.
//
// The real engine (PartFileStore, TorrentStore, range scheduler, see docs/ARCHITECTURE.md
// §Downloads) is written by a separate task. It replaces engine/engine_stub.cpp with its own
// files under src/core/engine/ (src/core/CMakeLists.txt globs engine/*.cpp; re-run cmake) and may
// extend this header; nothing else in the core needs to change:
//   * RealCore constructs DownloadEngine(EngineContext, parent) in its constructor, calls start()
//     from Core::start() AFTER the plugins were init()ed and started and the library started, and
//     shutdown() from Core::shutdown() BEFORE the plugins are stopped (must return within ~2 s).
//   * knownSourceCount() feeds the title bar "S=" number.
//   * Uploads are NOT the engine's job: TransferServiceBase (core/transfer_base.h) forwards the
//     UploadTracker (core/upload_tracker.h), implement only the download half and use
//     uploadTransfer() in transfer(id).
// Useful core pieces for the engine: core/dao.h (DownloadsDao incl. the opaque engine_state blob,
// HashLinksDao), core/library.h (requestHash()/fileHashed() for completion hashing,
// setPartialProvider() to serve verified parts), core/download_request.h (link parsing),
// core/launcher.h (openFile/openFolder), mxhash (verification), tests/core/fake_plugin.h.
#pragma once

#include <QtCore/QList>
#include <QtCore/QString>

#include <memory>

#include "core/diag/diag_rules.h"
#include "core/engine/engine_types.h"
#include "core/transfer_base.h"
#include "mx/plugin.h"

namespace mx::engine {
class Download;
}

namespace mx {

class Database;
class Library;
class SearchManager;
class UploadTracker;

struct EngineContext {
    Settings       *settings = nullptr;
    Database       *db = nullptr;          // open before the engine is constructed
    Library        *library = nullptr;
    RateLimiter    *limiter = nullptr;
    UploadTracker  *uploads = nullptr;
    QList<NetworkPlugin *> plugins;        // all plugins (enabled or not); check capabilities()
    TorrentBackend *torrents = nullptr;    // the plugin that implements TorrentBackend, if any
    NetworkPlugin  *torrentPlugin = nullptr; // the same object as a NetworkPlugin
    QString         profileDir;
    // [engine-4] the search service: a download started from a search hit keeps every source the
    // hit carries (and gets the ones merged into it later); background size+name searches.
    SearchManager  *search = nullptr;
};

class DownloadEngine : public TransferServiceBase {
    Q_OBJECT
public:
    explicit DownloadEngine(const EngineContext &ctx, QObject *parent = nullptr);
    ~DownloadEngine() override;

    void start();                 // restore the download list from the DB and resume work
    // [safe-1] the safe quit: beginShutdown() stops every source and starts writing and syncing what
    // was received (returns at once); unsavedBytes() is what is still on its way to the disk;
    // shutdown() waits until it is all synced (as long as it takes; a last resort of
    // kShutdownCapMs for a hung disk, nothing after closeNow()), then stores the records. A record
    // never claims what was not synced: what was not saved is downloaded again.
    void beginShutdown();
    qint64 unsavedBytes() const;
    void closeNow();
    void shutdown();
    static constexpr int kShutdownCapMs = 10 * 60 * 1000;
    // [safe-1] the last session did not end with shutdown() (a crash, a kill, a power cut): the
    // downloads that hold data are checked before they are trusted (Download::checkFirst)
    bool uncleanStart() const;
    static QString runningMarkPath(const QString &profileDir);
    int knownSourceCount() const; // sum of known sources of all unfinished downloads
    int connectedSourceCount() const; // [engine-4] sources we exchange data / queue slots with (S=)
    // [engine-4] downloads without their source rows (cheap: search-hit flags, chat checks)
    QList<TransferInfo> downloadsBrief() const;

    // ---- TransferService (download half)
    QString download(const DownloadRequest &req) override;
    QString downloadHit(const SearchHit &hit, const QString &saveDir = QString()) override;
    QString downloadUri(const QString &uri, const QString &saveDir = QString()) override;
    QList<TransferInfo> downloads() const override;
    TransferInfo transfer(const QString &id) const override;
    void pause(const QString &id) override;
    void resume(const QString &id) override;
    void pauseAll() override;
    void resumeAll() override;
    void cancel(const QString &id, bool deleteData = true) override;
    void retry(const QString &id) override;
    void setPriority(const QString &id, int priority) override;
    void clearCompleted() override;
    void setWantedFiles(const QString &id, const QList<int> &indexes) override;
    void openFile(const QString &id) override;
    void openFolder(const QString &id) override;
    QString incomingDir() const override;
    // [glue-2]
    void findMoreSources(const QString &id) override;
    void enterQueue(const QString &id) override;
    void cancelSource(const QString &id, const QString &sourceId) override;
    void setSourcePriority(const QString &id, const QString &sourceId, int priority) override;
    QString resumeDownload(const DownloadRequest &req, const QString &existingFile) override;
    bool setDownloadTarget(const QString &id, const QString &dir, const QString &name = QString()) override;
    QList<PartialFile> partialFiles() const override;
    // [ui-j] removing a download, keeping or deleting its file
    DownloadDiskInfo diskInfo(const QString &id) const override;
    void removeDownload(const QString &id, FileRemoval mode) override;
    // downloads removed with "keep the file" whose partial data is still there (done = bytes held;
    // the "matches incomplete local file" mark of Search)
    QList<TransferInfo> keptDownloads() const;
    // a search hit of a kept download (same hash, or same name and size as RealCore::flagHit matches
    // the downloads in the list): downloading it continues the kept data
    bool matchesKept(const SearchHit &h) const;
    // [engine-4]
    void setAutoFindSources(const QString &id, int minutes) override;
    void setAutoEnterQueue(const QString &id, int n) override;
    // [dl-1]
    void setSequentialDownload(const QString &id, bool on) override;
    void setFirstLastPiecesFirst(const QString &id, bool on) override;
    PreviewInfo previewInfo(const QString &id) const override;
    QString preparePreview(const QString &id, int fileIndex, QString *error) override;
    // [ver-1] Verify File / Restart Download
    bool canVerifyDownload(const QString &id, QString *why = nullptr) const override;
    bool verifyDownload(const QString &id) override;
    bool canRestartDownload(const QString &id, QString *why = nullptr) const override;
    bool restartDownload(const QString &id) override;

    // ---- engine extras (tests, mxcli)
    void setTuning(const engine::Tuning &t);       // before start()
    engine::Tuning tuning() const;
    // Per-download internals: store kind, verified bytes per network, injected pieces, strikes...
    QVariantMap debugInfo(const QString &id) const;
    QString incompleteDir() const override;
    // [diag-1] Connection Test: counters per unfinished download, and the sentence it derived
    // (lands in TransferInfo::diagnosis; emits transferChanged when it changed)
    QList<diag::DownloadFacts> diagFacts() const;
    void setDiagnosis(const QString &id, const QString &text, int level);
    int activeDownloadCount() const;

    // ---- [str-1] streaming (the stream service, src/core/stream)
    engine::Download *downloadObject(const QString &id) const;
    // The next download that download() / downloadHit() creates (not one it joins) is a stream
    // download: it fetches only what players read (Download::setStream), its cache (part file or
    // torrent folder) lies in `cacheDir`, sparse, and it is never stored. One call, one download.
    void setNextStream(int fileIndex, const QString &cacheDir);
    void clearNextStream();
    // Remove a stream download from the list (its sources closed, its torrent out of the session);
    // its cache is NOT moved to the trash: the stream service deletes it for good.
    void dropStream(const QString &id);
    // <incomplete>/Streams: every stream's cache lives below it
    QString streamCacheRoot() const;

private slots:
    void onUrlTorrentAdded(const QString &url, const QString &id);
    void onUrlTorrentFailed(const QString &url, const QString &error);
    void onSeedReady(const QString &path, const QString &id);

private:
    QString downloadHitRequest(const SearchHit &hit, const QString &saveDir);
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace mx
