// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX core: the shared-files library: LibraryService (UI) + LibraryAccess (plugins).
//
// * Shared folders and exclusions are persisted in the DB (shared_folders, library_excluded).
//   The incoming directory is shared implicitly while transfers/shareCompleted is on.
// * Scanning walks the folders in a worker thread; the hash cache (library_files, keyed by
//   path+size+mtime) is consulted there, so unchanged files are never re-hashed.
// * New/changed files are hashed by mxhash::MultiHasher (ed2k + part hashes, AICH, SHA-1, TTH,
//   SHA-256) on a low-priority thread pool (library/hashThreads), with best-effort audio metadata.
// * QFileSystemWatcher + a periodic timer (library/rescanIntervalMin) trigger rescans.
// * sharedFilesUpdated() (debounced 2 s) carries the list of HASHED files; the NetworkManager
//   forwards it to every plugin's setSharedFiles().
// * LibraryAccess methods are thread-safe (plugins may call read() from their own threads).
#pragma once

#include <QtCore/QFileSystemWatcher>
#include <QtCore/QFutureWatcher>
#include <QtCore/QHash>
#include <QtCore/QReadWriteLock>
#include <QtCore/QSet>
#include <QtCore/QThreadPool>
#include <QtCore/QTimer>

#include <atomic>
#include <functional>

#include "mx/plugin.h"
#include "mx/services.h"

namespace mx {

class Database;
class LibraryDao;

class Library : public LibraryService, public LibraryAccess {
    Q_OBJECT
public:
    Library(Settings *settings, Database *db, QObject *parent = nullptr);
    ~Library() override;

    void start();                       // load folders from the DB, first scan, timers, watcher
    void stop(int waitMs = 3000);       // cancel hashing and wait for the workers
    // Test helper: process events until no scan/hash work is pending (or timeout). True if idle.
    bool waitIdle(int timeoutMs);

    // ---- LibraryService
    QList<SharedFolder> folders() const override;
    void addFolder(const QString &path, bool recursive = true) override;
    void removeFolder(const QString &path) override;
    void rescan() override;
    QList<SharedFile> files(const QString &folder = QString()) const override;
    int fileCount() const override;
    qint64 totalBytes() const override;
    bool isScanning() const override;
    QString scanStatus() const override;
    void unshareFile(const QString &path) override;   // a file or a whole sub-directory
    void forgetPaths(const QStringList &paths) override;   // [ui-j] deleted from the disk
    // [glue-3] Shared File Types filter (settings library/sharedTypes, otherExtensions,
    // shareUnlistedTypes), sub-folder exclusion, media folder suggestions
    QStringList sharedTypes() const override;
    void setSharedTypes(const QStringList &typeIds) override;
    QString otherExtensions() const override;
    void setOtherExtensions(const QString &extensions) override;
    void excludeFolder(const QString &path, bool excluded) override;
    QStringList excludedFolders() const override;
    QStringList suggestMediaFolders() const override;
    void setSuggestHomeFolders(bool on) { m_suggestHome = on; }   // [core-5] off for an isolated profile
    static QStringList standardMediaFolders();        // candidates (existing or not)
    bool typeAllowed(const QString &fileName) const;  // current Shared File Types filter

    // ---- LibraryAccess (thread-safe)
    QList<SharedFile> sharedFiles() const override;   // hashed files only
    bool findByHash(const FileHashes &h, SharedFile *out) const override;
    QByteArray read(const FileHashes &h, qint64 offset, qint64 length) override;
    QList<QByteArray> ed2kPartHashes(const FileHashes &h) const override;

    // ---- extras for the core / engine
    QList<QByteArray> aichPartHashes(const FileHashes &h) const;
    QStringList excludedPaths() const;
    void reshareFile(const QString &path);            // undo unshareFile
    // Hash a file now (front of the queue), whether shared or not; the result is cached in the DB
    // and reported with fileHashed(). Used by the engine on download completion. If the file lies in
    // a shared folder (or the shared incoming dir) it becomes shared.
    void requestHash(const QString &path);
    // [engine] The caller already hashed `path` (e.g. the download engine's final verification
    // pass): store the result in the hash cache (path+size+mtime) and index it like requestHash()
    // would, without reading the file again.
    void addHashed(const QString &path, const FileHashes &hashes, const QList<QByteArray> &ed2kParts,
                   const QList<QByteArray> &aichParts);
    // Number of files actually read + hashed by this Library (cache hits not counted). For tests.
    int hashRuns() const { return m_hashRuns.load(); }
    void recordUpload(const FileHashes &h, qint64 bytes);   // per-file statistics
    // The engine can expose the verified part of running downloads for serving:
    using PartialFinder = std::function<bool(const FileHashes &, SharedFile *)>;
    using PartialReader = std::function<QByteArray(const FileHashes &, qint64, qint64)>;
    void setPartialProvider(PartialFinder finder, PartialReader reader);

signals:
    void sharedFilesUpdated(const QList<mx::SharedFile> &files);
    void fileHashed(const mx::SharedFile &file);
    void scanFinished();

private:
    struct Entry {
        SharedFile file;
        QList<QByteArray> ed2kParts;
        QList<QByteArray> aichParts;
        bool shared = true;            // false: hashed on request only (not in a shared folder)
    };
    struct ScanItem;
    struct ScanResult;
    struct HashResult;

    QList<SharedFolder> effectiveFolders() const;
    bool isUnderSharedFolder(const QString &path) const;
    void startScan();
    void onScanDone();
    void pumpHashQueue();
    bool deliverFromCache(const QString &path, bool priority);
    void onHashed(const HashResult &r);
    void indexEntryLocked(const Entry &e);
    void unindexLocked(const Entry &e);
    void scheduleUpdate();
    void emitProgress(const QString &current);
    void updateWatcher(const QStringList &dirs);
    QString findPathLocked(const FileHashes &h) const;

    Settings *m_settings;
    Database *m_db;
    LibraryDao *m_dao;

    mutable QReadWriteLock m_lock;     // guards m_entries + m_index + partial provider
    QHash<QString, Entry> m_entries;   // path -> entry (shared or hashed-on-request)
    QHash<QByteArray, QString> m_index;// kind byte + hash -> path
    PartialFinder m_partialFinder;
    PartialReader m_partialReader;

    QList<SharedFolder> m_folders;     // user folders (main thread)
    QSet<QString> m_excluded;

    QThreadPool m_scanPool;
    QThreadPool m_hashPool;
    QFutureWatcher<ScanResult *> m_scanWatcher;
    bool m_scanning = false;
    bool m_rescanPending = false;
    QStringList m_hashQueue;
    QSet<QString> m_queued;
    QSet<QString> m_priority;          // requested via requestHash (always hashed)
    int m_inFlight = 0;
    int m_hashTotal = 0;
    int m_hashDone = 0;
    QString m_current;
    std::atomic<bool> m_cancel{false};
    std::atomic<int> m_hashRuns{0};
    bool m_started = false;
    bool m_stopped = false;

    QTimer m_updateTimer;              // debounced filesChanged (500 ms)
    QTimer m_pushTimer;                // debounced sharedFilesUpdated (2 s)
    QTimer m_rescanTimer;              // periodic
    QTimer m_watchDebounce;
    QFileSystemWatcher *m_watcher = nullptr;
    qint64 m_lastProgressMs = 0;
    bool m_suggestHome = true;
};

} // namespace mx
