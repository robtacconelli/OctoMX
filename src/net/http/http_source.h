// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX: HttpRangeSource: one http(s) URL as a mx::RangeSource. Private to mxnet_http.
#pragma once

#include <QtCore/QElapsedTimer>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <memory>
#include <vector>

#include "http_jobs.h"
#include "http_plugin.h"
#include "http_util.h"

class QNetworkReply;

namespace mx {

class HttpPlugin;

class HttpRangeSource : public RangeSource, public HttpSourceInfo {
    Q_OBJECT
    Q_INTERFACES(mx::HttpSourceInfo)
public:
    static constexpr qint64 kMaxChunk = 256 * 1024;        // largest data() emission
    static constexpr qint64 kReadBuffer = 512 * 1024;      // QNetworkReply buffer per request
    static constexpr qint64 kRangedRequestSize = 4 * 1024 * 1024;

    HttpRangeSource(HttpPlugin *plugin, const QString &downloadId, const FileHashes &hashes, qint64 expectedSize,
                    const SearchSource &src, const QUrl &url);
    ~HttpRangeSource() override;

    // RangeSource
    SearchSource descriptor() const override { return m_desc; }
    State state() const override { return m_state; }
    QString stateText() const override { return m_stateText; }
    QString clientName() const override { return m_server; }
    QList<QPair<qint64, qint64>> available() const override;
    qint64 preferredRequestSize() const override;
    int maxPendingRequests() const override;
    void open() override;
    void request(qint64 offset, qint64 length) override;
    void cancelRequests() override;
    void close() override;

    // HttpSourceInfo
    bool supportsRanges() const override { return m_supportsRanges; }
    qint64 remoteSize() const override { return m_remoteSize; }
    QUrl finalUrl() const override { return m_finalUrl; }
    QString etag() const override { return m_etag; }
    QString lastModified() const override { return m_lastModified; }

    // instrumentation (tests)
    qint64 peakBufferedBytes() const { return m_peakBuffered; }
    qint64 maxChunkEmitted() const { return m_maxChunk; }
    int connectionsOpened() const { return m_connectionsOpened; }
    int retriesDone() const { return m_retries; }

    // called by the plugin when a per-host slot frees up
    void kick();

private:
    enum Phase { PIdle, PProbing, PReady, PFailed, PClosed };
    struct Req {
        quint64 id = 0;
        qint64 offset = 0, length = 0, delivered = 0;
        qint64 skip = 0;                  // body bytes to discard before delivering
        QPointer<QNetworkReply> reply;
        bool hasSlot = false;
        QString slotHost;
        bool ranged = false;
        bool headersDone = false;
        bool replyFinished = false;
        bool exactRange = false;          // 206 ending exactly at our end → keep-alive possible
        int status = 0;
        int attempts = 0;
        qint64 startedMs = 0, lastNetMs = 0, notBeforeMs = 0;
        QUrl url;
        RedirectTracker redir;
    };

    Req *find(quint64 id) const;
    qint64 effectiveSize() const;
    qint64 now() const { return m_clock.elapsed(); }
    void updateState();
    void startProbe();
    void onProbeDone(const ProbeOutcome &o);
    void startReq(Req *q);
    void onMeta(quint64 id);
    void onReadyRead(quint64 id);
    void onFinished(quint64 id);
    bool validateHeaders(Req *q);         // false → request dealt with (failed / retried / gone)
    void pump(quint64 id);
    void pumpAll();
    void onReplyDone(Req *q);
    void completeReq(Req *q, qint64 reportedLength);
    void retryReq(Req *q, const QString &why, bool countAttempt = true);
    void busyReq(Req *q, int retryAfterSec);
    void failReq(Req *q, const QString &why);       // one request fails, source stays usable
    void failSource(const QString &why);
    void markNoRanges();
    void releaseReply(Req *q, bool graceful);
    void removeReq(Req *q);
    void tick();
    void deferFail(qint64 offset, qint64 length, const QString &why);
    void updateExtra();
    int concurrencyLimit() const;

    QPointer<HttpPlugin> m_plugin;
    QString m_downloadId;
    FileHashes m_hashes;
    qint64 m_expectedSize;
    SearchSource m_desc;
    QUrl m_origUrl, m_finalUrl;
    HttpCredentials m_cred;
    HttpConfig m_cfg;

    Phase m_phase = PIdle;
    State m_state = Idle;
    QString m_stateText = QStringLiteral("Idle");
    QString m_error, m_connectNote, m_server;

    // learned by the probe
    qint64 m_remoteSize = -1;
    bool m_supportsRanges = false;
    QString m_prevEtag;                   // descriptor().extra["etag"] given to openSource()
    QString m_etag, m_lastModified, m_fileName, m_contentType;
    bool m_isTorrent = false;
    bool m_reresolved = false;

    QPointer<ProbeJob> m_job;
    QTimer m_probeTimer;
    int m_probeAttempts = 0;
    int m_busyStreak = 0;
    qint64 m_busyUntilMs = 0;
    int m_adaptiveLimit = 1 << 20;
    int m_failStreak = 0;

    std::vector<std::unique_ptr<Req>> m_reqs;
    quint64 m_nextId = 1;
    quint64 m_gen = 1;                    // bumped by cancelRequests()/close()/failSource()
    QTimer m_tick;
    QTimer m_pumpTimer;
    QElapsedTimer m_clock;

    qint64 m_peakBuffered = 0;
    qint64 m_maxChunk = 0;
    int m_connectionsOpened = 0;
    int m_retries = 0;
};

} // namespace mx
