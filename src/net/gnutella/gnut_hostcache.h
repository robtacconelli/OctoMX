// SPDX-License-Identifier: GPL-3.0-or-later
// Gnutella module: persisted host cache: ultrapeers we heard of (X-Try-Ultrapeers, pongs,
// UHC/GWC replies) and how well they worked (last seen / last success / counters / vendor),
// user-added static hosts, plus the GWebCache / UDP host cache lists with their own stats.
// Stored as JSON in <dataDir>/hosts.json.
#pragma once

#include "gnut_util.h"

#include <QtCore/QDateTime>
#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QSet>
#include <QtCore/QString>

namespace mx::gnut {

struct HostEntry {
    HostPort addr;
    QString hostName;            // static hosts may be given by DNS name
    qint64 added = 0;            // unix seconds
    qint64 lastSeen = 0;         // last time someone told us about it
    qint64 lastAttempt = 0;
    qint64 lastSuccess = 0;      // last successful handshake (leaf accepted)
    int successes = 0;
    int failures = 0;            // consecutive failures since the last success
    int totalFailures = 0;
    qint64 retryAfter = 0;       // unix seconds; not before
    QString vendor;              // User-Agent seen
    QString source;              // "gwc", "uhc", "x-try", "pong", "static", "ghc"
    bool isStatic = false;
    bool tls = false;            // [enc-1] accepts TLS (X-Features tls, GGEP TLS / IPP_TLS)
    double score(qint64 now) const;
};

struct CacheEntry {              // a GWebCache URL or a UHC "host:port"
    QString url;
    qint64 lastAttempt = 0;
    qint64 lastSuccess = 0;
    int successes = 0;
    int failures = 0;
    bool builtin = false;
};

class HostCache {
public:
    static constexpr int MaxHosts = 2000;

    void setPath(const QString &path) { m_path = path; }
    bool load();
    bool save() const;
    bool dirty() const { return m_dirty; }

    // add or refresh; returns true if new
    bool add(const HostPort &h, const QString &source, qint64 now = 0);
    void addStatic(const HostPort &h, const QString &name);
    bool remove(const QString &id);           // "ip:port"
    void markAttempt(const HostPort &h, qint64 now = 0);
    void markSuccess(const HostPort &h, const QString &vendor, qint64 now = 0);
    void markFailure(const HostPort &h, int retryAfterSec, qint64 now = 0);
    // a host told us it is a leaf / not a Gnutella node: drop it (unless static)
    void markUseless(const HostPort &h);
    // [enc-1] the host takes TLS connections (true) / a TLS handshake with it failed (false)
    void setTls(const HostPort &h, bool tls);
    bool supportsTls(const HostPort &h) const;
    int tlsCount() const;
    // best `n` candidates not in `exclude` whose retryAfter has passed
    QList<HostPort> pick(int n, const QSet<quint64> &exclude, qint64 now = 0) const;
    // good hosts for X-Try-Ultrapeers (recent successes first)
    QList<HostPort> goodHosts(int n) const;
    const HostEntry *find(const HostPort &h) const;
    QList<HostEntry> entries() const { return m_hosts.values(); }
    int size() const { return int(m_hosts.size()); }
    int usableCount(qint64 now = 0) const;

    // GWebCaches / UHCs
    void setBuiltinCaches(const QStringList &gwcs, const QStringList &uhcs);
    void addGwc(const QString &url);
    void addUhc(const QString &hostPort);
    QList<CacheEntry> &gwcs() { return m_gwcs; }
    QList<CacheEntry> &uhcs() { return m_uhcs; }
    CacheEntry *nextGwc(qint64 now, int minIntervalSec);
    CacheEntry *nextUhc(qint64 now, int minIntervalSec);
    void touch() { m_dirty = true; }

private:
    void trim();
    QString m_path;
    QHash<quint64, HostEntry> m_hosts;
    QList<CacheEntry> m_gwcs, m_uhcs;
    mutable bool m_dirty = false;
};

qint64 nowSecs();

} // namespace mx::gnut
