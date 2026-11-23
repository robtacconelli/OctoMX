// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX web module: the backend abstraction (private to src/net/web).
// A SearchBackend answers keyword searches asynchronously with TorrentRecords. HttpIndexProvider (a
// data-driven HTTP index API / RSS / HTML scraper) is one implementation; non-HTTP backends (rats-search
// P2P network, a local DHT-crawl index) can implement the same interface and plug into WebSearchPlugin.
#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QVariantMap>

#include "mx/types.h"

namespace mx::web {

struct RecordFile {
    QString path;           // relative path inside the torrent / item
    qint64  size = -1;
    QString url;            // direct http(s) URL of this file, if the index offers one
};

// One result row as reported by a backend, already normalised.
struct TorrentRecord {
    QString    providerId;
    QString    name;
    QByteArray btih;        // 20 raw bytes (empty if unknown)
    qint64     size = -1;
    int        seeders = -1;
    int        leechers = -1;
    int        numFiles = -1;  // -1 unknown
    QDateTime  published;
    QString    category;     // provider's own category text / id
    MediaType  type = MediaType::Any;  // from the provider's category mapping (Any = unknown)
    bool       adult = false;          // provider category is an adult category
    QString    magnet;       // magnet given by the provider (may carry extra trackers)
    QString    torrentUrl;   // .torrent download URL
    QString    httpUrl;      // direct HTTP URL of the whole payload (single-file items)
    QString    detailsUrl;   // human-readable page
    QList<RecordFile> files;
    QVariantMap extra;       // free-form, copied into SearchSource::extra
};

struct BackendQuery {
    quint32   searchId = 0;
    QString   text;          // all keywords (text + text2), simplified
    MediaType type = MediaType::Any;
    int       maxResults = 200;
    bool      adultFilter = false;
};

enum class BackendState { Unknown = 0, Ok, Failing, BackingOff, Disabled };

struct BackendHealth {
    BackendState state = BackendState::Unknown;
    QDateTime    lastOk;            // last successful answer (any number of results)
    QDateTime    lastAttempt;
    int          lastLatencyMs = -1;
    int          consecutiveFailures = 0;
    QDateTime    backoffUntil;      // do not query before this time
    QString      lastError;
    int          lastResultCount = -1;
};

class SearchBackend : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    ~SearchBackend() override = default;

    virtual QString id() const = 0;
    virtual QString displayName() const = 0;
    virtual QString description() const = 0;   // e.g. host name
    virtual QString address() const = 0;       // e.g. URL template / home page
    virtual bool isUserDefined() const { return false; }

    bool isEnabled() const { return m_enabled; }
    void setEnabled(bool on) { if (m_enabled != on) { m_enabled = on; emit healthChanged(); } }

    // Whether the backend wants to take part in this query (category support etc.).
    virtual bool accepts(const BackendQuery &q) const { Q_UNUSED(q) return true; }
    // Start an asynchronous search; → results* then exactly one finished(). Never blocks.
    virtual void search(const BackendQuery &q) = 0;
    virtual void cancel(quint32 searchId) = 0;
    virtual void cancelAll() = 0;
    // Cheap liveness check (a small query); updates health(). Optional.
    virtual void probe() {}

    const BackendHealth &health() const { return m_health; }
    // True while the backend is in back-off after failures.
    bool isBackingOff() const { return m_health.backoffUntil.isValid() && QDateTime::currentDateTimeUtc() < m_health.backoffUntil; }
    void resetHealth() { m_health = BackendHealth(); emit healthChanged(); }

signals:
    void results(quint32 searchId, const QList<mx::web::TorrentRecord> &records);
    void finished(quint32 searchId, bool ok, const QString &error);
    void healthChanged();

protected:
    // Book-keeping helpers for implementations.
    void markOk(int latencyMs, int results);
    // minBackoffSec: wait at least this long (HTTP 429 "slow down", Retry-After) [diag-1]
    void markFailed(const QString &error, int latencyMs = -1, int minBackoffSec = 0);

    BackendHealth m_health;
    bool m_enabled = true;
};

} // namespace mx::web

Q_DECLARE_METATYPE(mx::web::TorrentRecord)
