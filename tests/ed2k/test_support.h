// SPDX-License-Identifier: GPL-3.0-or-later
// Tiny stand-ins for the OctoMX core, enough to drive the eD2k plugin in tests and in the
// ed2k_probe tool: in-memory Settings, a LibraryAccess over explicit files (computes eD2k hashes),
// a counting RateLimiter and a minimal multi-source download driver (SimpleDownloader).
#pragma once

#include "mx/plugin.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QHash>
#include <QtCore/QMap>
#include <QtCore/QMutex>
#include <QtCore/QPointer>
#include <QtCore/QTimer>

namespace mxtest {

class MemSettings : public mx::Settings {
    Q_OBJECT
public:
    using mx::Settings::Settings;
    QVariant value(const QString &key) const override { return m_v.value(key); }
    void setValue(const QString &key, const QVariant &v) override { m_v.insert(key, v); emit changed(key, v); }
    QVariant defaultValue(const QString &) const override { return QVariant(); }
    QStringList keys(const QString &prefix = QString()) const override
    {
        QStringList out;
        for (auto it = m_v.cbegin(); it != m_v.cend(); ++it) if (it.key().startsWith(prefix)) out << it.key();
        return out;
    }
    void resetToDefaults(const QString &) override { m_v.clear(); }
    void sync() override {}
private:
    QMap<QString, QVariant> m_v;
};

class NullLimiter : public mx::RateLimiter {
public:
    qint64 acquire(Dir d, qint64 bytes) override
    {
        if (m_limit[d] <= 0) return bytes;
        // crude token bucket: refilled per 100 ms
        const qint64 now = m_clock.isValid() ? m_clock.elapsed() : (m_clock.start(), 0);
        if (now - m_last[d] >= 100) { m_tokens[d] = m_limit[d] / 10; m_last[d] = now; }
        const qint64 g = qMin(bytes, m_tokens[d]);
        m_tokens[d] -= g;
        return g;
    }
    void account(Dir d, mx::Net, qint64 bytes) override { m_total[d] += bytes; }
    int limitBps(Dir d) const override { return int(m_limit[d]); }
    void setLimit(Dir d, qint64 bps) { m_limit[d] = bps; }
    qint64 total(Dir d) const { return m_total[d]; }
private:
    qint64 m_limit[2] = {0, 0}, m_total[2] = {0, 0}, m_tokens[2] = {0, 0}, m_last[2] = {-1000, -1000};
    QElapsedTimer m_clock;
};

// eD2k hashing of a whole file (MD4 per 9 728 000-byte part; eMule conventions)
struct Ed2kFileHash {
    QByteArray fileHash;
    QList<QByteArray> parts;    // hashset as eMule sends it (size/PARTSIZE + 1 entries for size >= PARTSIZE, else 1)
};
Ed2kFileHash hashFile(const QString &path, bool *ok = nullptr);

class FileLibrary : public mx::LibraryAccess {
public:
    mx::SharedFile addFile(const QString &path);       // hashes it (blocking)
    QList<mx::SharedFile> sharedFiles() const override;
    bool findByHash(const mx::FileHashes &h, mx::SharedFile *out) const override;
    QByteArray read(const mx::FileHashes &h, qint64 offset, qint64 length) override;
    QList<QByteArray> ed2kPartHashes(const mx::FileHashes &h) const override;
    qint64 bytesRead() const { return m_read; }
private:
    struct Entry { mx::SharedFile f; QList<QByteArray> parts; };
    QHash<QByteArray, Entry> m_files;
    mutable QMutex m_mx;
    qint64 m_read = 0;
};

mx::PluginContext makeContext(mx::Settings *s, mx::LibraryAccess *lib, mx::RateLimiter *lim, const QString &profile);

// Downloads one file from any number of RangeSources into `path`, verifying every eD2k part
// against the hash set once known. Mirrors (in miniature) what the core download engine does.
class SimpleDownloader : public QObject {
    Q_OBJECT
public:
    SimpleDownloader(const QString &path, qint64 size, const QByteArray &ed2k, QObject *parent = nullptr);
    ~SimpleDownloader() override;
    void setWanted(qint64 from, qint64 to);              // only fetch this range (aligned to parts)
    void setPartHashes(const QList<QByteArray> &h);
    void addSource(mx::RangeSource *s);
    QList<mx::RangeSource *> sources() const;
    bool done() const;                                   // wanted range verified (or received if no hashes)
    qint64 receivedBytes() const { return m_received; }
    int verifiedParts() const { return m_verifiedParts.size(); }
    QList<int> verifiedPartList() const { return m_verifiedParts.values(); }
    int failedParts() const { return m_failedParts; }
    QString summary() const;
    QStringList sourceLines() const;
    qint64 firstVerifiedMs() const { return m_firstVerifiedMs; }

signals:
    void partVerified(int part, const QString &sourceClient);
    void partFailed(int part);
    void finished();
    void logLine(const QString &s);

private:
    struct Chunk { qint64 start = 0, end = 0, got = 0; int state = 0; QPointer<mx::RangeSource> owner; };
    void schedule();
    void onData(mx::RangeSource *s, qint64 off, const QByteArray &b);
    void onFinished(mx::RangeSource *s, qint64 off, qint64 len, bool ok, const QString &err);
    void checkPart(int part);
    bool sourceHas(mx::RangeSource *s, qint64 a, qint64 b) const;

    QFile m_file;
    qint64 m_size;
    QByteArray m_hash;
    QList<QByteArray> m_partHashes;
    QList<Chunk> m_chunks;
    QList<QPointer<mx::RangeSource>> m_sources;
    QHash<mx::RangeSource *, int> m_active;   // source -> chunk index
    QHash<int, QString> m_partClient;
    QSet<int> m_verifiedParts;
    int m_failedParts = 0;
    qint64 m_received = 0;
    qint64 m_wantFrom = 0, m_wantTo = 0;
    QElapsedTimer m_clock;
    qint64 m_firstVerifiedMs = -1;
    bool m_finishedEmitted = false;
};

} // namespace mxtest
