// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX engine: [ver-1] "Verify File": read the blocks of a file on disk again and check each one
// against its known hash (an eD2k part = MD4, a tiger tree leaf of the download's unit size), or the
// whole file against every checksum known for it. One block per task on the job's own serial queue:
// the shared worker pool is free for the other downloads' disk writes between two blocks, and the
// window stays responsive (results come back on the main thread). The file is opened read-only for
// each block and closed again (nothing is written; another program may keep it open meanwhile).
#pragma once

#include <QtCore/QObject>

#include <atomic>
#include <memory>

#include "core/engine/io_queue.h"
#include "mx/types.h"

namespace mx::engine {

class VerifyJob : public QObject {
    Q_OBJECT
public:
    enum Kind { Md4 = 0, Tiger = 1 };
    struct Block {
        qint64 start = 0, end = 0;
        QByteArray expect;
        Kind kind = Md4;
    };
    struct Whole {                     // the outcome of checkWhole()
        bool readable = false;
        QString error;
        QStringList mismatch;          // hash kinds that differ ("sha1", "ed2k" ...)
        FileHashes hashes;             // what the file has now
        QList<QByteArray> ed2kParts;   // its eD2k part hashes
        QList<QByteArray> tthLevel;    // its tiger tree level of the unit asked for
    };

    VerifyJob(QThreadPool *pool, const QString &path, QObject *parent = nullptr);
    ~VerifyJob() override;

    // the blocks in this order; pauseMs between two blocks (tests)
    void checkBlocks(const QList<Block> &blocks, int pauseMs);
    // the whole file against every kind in `expect`; tthUnit > 0: its tree level of that unit too
    void checkWhole(const FileHashes &expect, qint64 tthUnit);
    void stop();                       // no further block; a block being read is dropped
    bool running() const { return m_running; }
    qint64 total() const { return m_total; }
    qint64 done() const { return m_done; }
    QString path() const { return m_path; }

signals:
    // readable = false: the block could not be read (file gone, shorter, locked)
    void blockChecked(qint64 start, qint64 end, bool ok, bool readable);
    void wholeChecked(const mx::engine::VerifyJob::Whole &result);
    void progress(qint64 done, qint64 total);
    void finished(bool stopped);

private:
    void next();
    QString m_path;
    IoQueue m_io;
    std::shared_ptr<std::atomic<bool>> m_cancel;
    QList<Block> m_blocks;
    int m_index = 0;
    int m_pauseMs = 0;
    qint64 m_total = 0, m_done = 0;
    bool m_running = false;
};

} // namespace mx::engine
