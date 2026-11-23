// SPDX-License-Identifier: GPL-3.0-or-later
// Stand-ins for the OctoMX core used by the Gnutella tests and gnutella_probe: in-memory
// Settings, a LibraryAccess over explicit files (SHA-1 + TTH via mxhash), a counting
// RateLimiter, and a tiny single-file downloader driving RangeSources.
#pragma once

#include "mx/plugin.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QMap>
#include <QtCore/QMutex>
#include <QtCore/QPointer>
#include <QtCore/QTimer>

namespace gnuttest {

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

class CountingLimiter : public mx::RateLimiter {
public:
    qint64 acquire(Dir, qint64 bytes) override { return bytes; }
    void account(Dir d, mx::Net, qint64 bytes) override { QMutexLocker l(&m_mx); m_total[d] += bytes; }
    int limitBps(Dir) const override { return 0; }
    qint64 total(Dir d) const { QMutexLocker l(&m_mx); return m_total[d]; }
private:
    mutable QMutex m_mx;
    qint64 m_total[2] = {0, 0};
};

class FileLibrary : public mx::LibraryAccess {
public:
    mx::SharedFile addFile(const QString &path);          // hashes it (blocking): sha1 + tth
    QList<mx::SharedFile> sharedFiles() const override;
    bool findByHash(const mx::FileHashes &h, mx::SharedFile *out) const override;
    QByteArray read(const mx::FileHashes &h, qint64 offset, qint64 length) override;
    QList<QByteArray> ed2kPartHashes(const mx::FileHashes &) const override { return {}; }
    qint64 bytesRead() const { QMutexLocker l(&m_mx); return m_read; }
private:
    QHash<QByteArray, mx::SharedFile> m_files;        // by sha1
    mutable QMutex m_mx;
    qint64 m_read = 0;
};

mx::PluginContext makeContext(mx::Settings *s, mx::LibraryAccess *lib, mx::RateLimiter *lim, const QString &profile);
QByteArray makeTestData(qint64 size, quint32 seed);
bool writeFile(const QString &path, const QByteArray &data);

// Downloads [from, to) of one file from RangeSources into memory (chunks of preferredRequestSize).
class MemDownloader : public QObject {
    Q_OBJECT
public:
    MemDownloader(qint64 size, qint64 from = 0, qint64 to = -1, QObject *parent = nullptr);
    void addSource(mx::RangeSource *s);
    bool done() const { return m_missing.isEmpty() && m_inflight.isEmpty(); }
    QByteArray bytes() const { return m_data; }
    qint64 received() const { return m_received; }
    QStringList log() const { return m_log; }
    QList<int> statesSeen() const { return m_states; }
    int failedRequests() const { return m_failed; }
signals:
    void finished();
private:
    void schedule();
    QByteArray m_data;
    qint64 m_from, m_to;
    QList<QPair<qint64, qint64>> m_missing;
    QHash<mx::RangeSource *, QPair<qint64, qint64>> m_inflight;
    QList<QPointer<mx::RangeSource>> m_sources;
    qint64 m_received = 0;
    int m_failed = 0;
    QStringList m_log;
    QList<int> m_states;
    QTimer m_retry;
    bool m_emitted = false;
};

} // namespace gnuttest
