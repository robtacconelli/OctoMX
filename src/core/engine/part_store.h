// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX engine: PartFileStore: a sparse <incomplete>/<id>.part file with a range map.
//
// Range states: missing → (requested: tracked by the Download) → held (received; written or queued
// for writing) → verified. "durable" = held bytes whose write has completed; [safe-1] "synced" =
// durable bytes the disk confirmed (fsync), the only ones the stored record may claim
// (persistable()). The part file is synced every kSyncBytes / kSyncMs of new data, before the final
// check and before the move to the target folder.
// Verification units, by preference: eD2k part hashes (9 728 000 bytes), TTH leaves, else the
// whole file at completion (finalCheck). All file I/O and hashing run on the store's serial
// IoQueue; results are posted back to the main thread.
#pragma once

#include <QtCore/QMutex>
#include <QtCore/QSet>
#include <QtCore/QTimer>

#include <functional>
#include <memory>

#include "core/engine/io_queue.h"
#include "core/engine/store.h"
#include "mx/types.h"

class QFile;

namespace mx::engine {

class PartStore : public Store {
    Q_OBJECT
public:
    enum Scheme { NoUnits = 0, Ed2kParts = 1, TthLeaves = 2 };
    static constexpr qint64 Ed2kPart = 9728000;

    PartStore(QThreadPool *pool, const QString &path, qint64 size, bool preallocate, QObject *parent = nullptr);
    ~PartStore() override;

    void open();                                  // async; storeError on failure
    QString path() const { return m_path; }
    qint64 size() const { return m_size; }
    void setSize(qint64 size);                    // unknown size became known
    void restore(const RangeSet &held, const RangeSet &verified);
    RangeSet held() const { return m_have; }
    RangeSet verified() const { return m_verified; }
    RangeSet durable() const { return m_durable; }
    RangeSet persistable() const;                 // [safe-1] held bytes that are synced to the disk
    RangeSet persistableVerified() const;         // [safe-1] verified bytes that are synced to the disk
    RangeSet synced() const { return m_synced; }
    RangeSet written() const;                     // [safe-1] held bytes in the file now (synced or not)
    // [safe-1] the sync schedule: after this much new data or this long after the first unsynced byte
    static constexpr qint64 kSyncBytes = qint64(16) << 20;
    static constexpr int kSyncMs = 1000;
    void flushToDisk();                           // queue a sync of everything written so far (no wait)
    // Wait (≤ timeoutMs) until every queued write is on the disk and synced and the results are in;
    // true when nothing is left. What is not synced then stays unclaimed (downloaded again).
    bool syncNow(int timeoutMs);
    qint64 unsyncedBytes() const;                 // held bytes not yet synced (queued + written)
    int syncCount() const { return m_syncs; }
    // [safe-1] the move to the target folder is announced in "<part>.moving" until the record knows
    // where the file went (Download::resolveInterruptedMove)
    static QString moveMarkerPath(const QString &partPath) { return partPath + QStringLiteral(".moving"); }
    static bool readMoveMarker(const QString &partPath, QString *target, QString *tempCopy);

    // Fine verification (the caller validated the set against the trusted root first).
    void setEd2kParts(const QList<QByteArray> &parts);
    void setTthLeaves(qint64 unit, const QList<QByteArray> &leaves);
    // [engine-3] drop the fine units (a provisional hash set proved wrong): verified bytes become
    // held-but-unverified again; the caller may install another set afterwards.
    void clearUnits();
    Scheme scheme() const { return m_scheme; }
    qint64 unitSize() const { return m_unit; }
    QList<QByteArray> unitHashes() const { return m_hashes; }

    bool fullyHeld() const;
    bool fullyVerified() const;
    void verifyHeldUnits();                       // queue a check for every complete, unverified unit
    void discardUnverified();                     // whole-file failure: drop everything not verified
    void markAllVerified();                       // whole-file check passed
    void clearError() { m_error = false; }
    bool hasError() const { return m_error; }
    int verifyingCount() const { return m_verifying.size(); }

    struct FinalResult {
        bool ok = false;
        QString error;
        FileHashes hashes;
        QList<QByteArray> ed2kParts, aichParts;
        // [ver-1] the file's tiger tree level of tthUnit bytes (kept as its block hashes: Verify File)
        qint64 tthUnit = 0;
        QList<QByteArray> tthLevel;
    };
    // [ver-1] the unit of the block hashes a finished file keeps: 256 KiB or more, at most 4096 blocks
    static qint64 keptBlockUnit(qint64 size);
    void finalCheck(std::function<void(const FinalResult &)> done);
    // Close the file and move it to dir/name (collision-safe "name (1).ext"). done(ok, path, error).
    // [dl-1] allowCopy = false: only a rename (same volume); fails while another program keeps the file
    // open without delete sharing (a preview player on Windows) instead of copying it
    void moveTo(const QString &dir, const QString &name, std::function<void(bool, const QString &, const QString &)> done,
                bool allowCopy = true);
    // Close the file and hand it over at exactly `target` (promotion to a torrent). Overwrites
    // nothing: fails if target exists.
    void moveExact(const QString &target, std::function<void(bool, const QString &)> done);
    void removeFile();                            // delete (cancel)
    void close();                                 // [ui-j] drop pending writes and close the file (no delete)
    // SHA-1 of [s, e) as written so far (promotion check against a torrent piece hash).
    void sha1Range(qint64 s, qint64 e, std::function<void(const QByteArray &)> done);
    // [engine-3] SHA-1 of each range as it is on disk right now (queued behind earlier writes,
    // ahead of later ones). An unreadable range yields an empty hash.
    void sha1Ranges(const QList<Range> &ranges, std::function<void(const QList<QByteArray> &)> done);
    bool drain(int timeoutMs) { return m_io.drain(timeoutMs); }
    void abandon() { m_io.discardPending(); }
    // [ver-1] Verify File. done() runs on the main thread once every write queued so far is on disk.
    void afterWrites(std::function<void()> done);
    // the units that are held completely (verified or not) and are not being checked right now
    QList<Range> completeUnits() const;
    QByteArray unitHash(qint64 unitStart) const { return m_hashes.value(int(unitStart / qMax<qint64>(1, m_unit))); }
    // the outcome of reading a unit again: ok = verified; else its bytes are no longer held (they are
    // fetched again) and it is not verified; nobody is blamed (no unitFailed)
    void recheckResult(qint64 s, qint64 e, bool ok);
    // completion of a file repaired in place: close it where it is (pending writes first)
    void finishInPlace(std::function<void(bool, const QString &, const QString &)> done);

    // Thread-safe read of verified bytes (partial sharing). Empty unless fully verified.
    QByteArray readVerified(qint64 off, qint64 len) const;
    // The same as a free-standing, thread-safe function that stays valid after the store is gone.
    std::function<QByteArray(qint64, qint64)> verifiedReader() const;

    // Store
    Kind kind() const override { return Part; }
    qint64 extent() const override { return m_size; }
    RangeSet fetchTarget() const override;
    Range unitAt(qint64 pos) const override;
    bool canVerifyUnit(qint64 pos) const override;
    RangeSet deliver(qint64 off, const QByteArray &bytes) override;
    qint64 bufferedBytes() const override { return m_pendingWrite; }
    bool acceptingData() const override { return !m_error && !m_closed; }
    qint64 verifiedBytes() const override { return m_verified.total(); }
    qint64 heldBytes() const override { return m_have.total(); }
    void pieceMap(int cells, const RangeSet &inFlight, QByteArray *state, QByteArray *fill) const override;
    // [ui-i] the same map from stored ranges, for a download whose store does not exist yet
    // (restored after a restart, not resumed): complete = verified (with fine units) or held
    static void mapOf(int cells, qint64 size, const RangeSet &complete, QByteArray *state, QByteArray *fill);

private:
    struct Shared {
        mutable QMutex mutex;
        std::unique_ptr<QFile> file;          // used only by IoQueue tasks
        RangeSet verified;                    // copy for readVerified()
        QString path;
    };
    void checkUnitsIn(qint64 start, qint64 end);
    void queueVerify(qint64 s, qint64 e);
    void onWritten(qint64 off, qint64 n, bool ok, const QString &err, bool diskFull);
    void onVerified(qint64 s, qint64 e, bool ok, quint64 gen);
    void publishVerified();
    void onSynced(const RangeSet &covered, bool ok);   // [safe-1]
    void onShortFile(const RangeSet &lost);            // [safe-1] claims beyond the end of the file on disk

    QString m_path;
    qint64 m_size;
    bool m_prealloc;
    IoQueue m_io;
    std::shared_ptr<Shared> m_sh;
    RangeSet m_have, m_durable, m_verified;
    RangeSet m_synced, m_unsynced;            // [safe-1] synced; accepted since the last queued sync
    qint64 m_unsyncedBytes = 0;
    int m_syncsInFlight = 0, m_syncs = 0;
    QTimer m_syncTimer;
    QSet<qint64> m_verifying;                 // unit starts being checked
    Scheme m_scheme = NoUnits;
    qint64 m_unit = Ed2kPart;
    QList<QByteArray> m_hashes;
    quint64 m_unitsGen = 0;                   // [engine-3] bumped by clearUnits(): stale checks are ignored
    qint64 m_pendingWrite = 0;
    bool m_error = false;
    bool m_closed = false;
};

} // namespace mx::engine
