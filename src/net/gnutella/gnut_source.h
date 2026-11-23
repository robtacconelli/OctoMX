// SPDX-License-Identifier: GPL-3.0-or-later
// Gnutella module: downloads: the source descriptor carried in SearchSource::uri and the
// RangeSource that fetches byte ranges from one Gnutella host over HTTP/1.1 (persistent
// connection, no pipelining), with remote queueing, PUSH for firewalled hosts, THEX download
// and alt-loc harvesting.
#pragma once

#include "gnut_headers.h"
#include "gnut_http.h"
#include "gnut_thex.h"
#include "gnut_util.h"
#include "mx/plugin.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <memory>

class QTcpSocket;

namespace mx::gnut {

// "gnutella://1.2.3.4:6346/<index>?guid=<hex>&fw=1&pp=ip:port,ip:port&bh=1&v=LIME&name=<pct>&urn=urn:sha1:.."
struct SourceDesc {
    HostPort addr;
    quint32 index = 0;
    QByteArray guid;               // servent GUID
    bool firewalled = false;
    bool browse = false;
    QList<HostPort> proxies;
    QString name;
    QByteArray sha1;
    QString vendor;
    bool tls = false;              // [enc-1] the host takes TLS (QHD / ALT_TLS / X-Alt tls=); "tls=1"
    QString toUri() const;
    static bool fromUri(const QString &uri, SourceDesc *out);
    QString userId() const;        // guid hex, or "ip:port" when unknown
};

class GnutSource;

class SourceHost {
public:
    virtual ~SourceHost() = default;
    virtual mx::RateLimiter *limiter() = 0;
    virtual QString userAgent() const = 0;
    virtual HostPort selfAddress() const = 0;      // ip may be 0 if unknown
    virtual bool firewalled() const = 0;
    virtual int connectTimeoutMs() const = 0;
    // Ask `target` (firewalled) to connect to us. Returns false if impossible (we are firewalled
    // too, or no route). The plugin calls source->acceptPushedSocket() when the GIV arrives.
    virtual bool requestPush(GnutSource *s, const SourceDesc &d) = 0;
    virtual void cancelPush(GnutSource *s) = 0;
    virtual bool wantThex(const QString &downloadId) = 0;
    virtual void thexLearned(const QString &downloadId, const ThexTree &t) = 0;
    virtual void altLocsFound(const QString &downloadId, const QByteArray &sha1, const QList<HostPort> &direct,
                              const QList<PushLoc> &pushed, const QList<HostPort> &directTls = {}) = 0;   // [enc-1] tls=
    virtual void sourceLog(const QString &line) = 0;
    // [enc-1] an outgoing socket (a TlsSocket when Gnutella encryption is on); peerTls = the host
    // announced TLS. The default is a plain QTcpSocket (tests).
    virtual QTcpSocket *newSocket(bool peerTls, QObject *parent);
};

class GnutSource : public mx::RangeSource {
    Q_OBJECT
public:
    // hostAlive: cleared by the plugin when it is destroyed (sources are owned by the engine and
    // may outlive it during shutdown)
    GnutSource(SourceHost *host, const QString &downloadId, const mx::FileHashes &h, qint64 size,
               const SourceDesc &d, const mx::SearchSource &orig, std::shared_ptr<bool> hostAlive,
               QObject *parent = nullptr);
    ~GnutSource() override;

    mx::SearchSource descriptor() const override { return m_orig; }
    State state() const override { return m_state; }
    QString stateText() const override { return m_stateText; }
    QString clientName() const override;   // [enc-1] "+ [TLS]" on an encrypted connection
    QList<QPair<qint64, qint64>> available() const override { return m_avail; }
    qint64 preferredRequestSize() const override { return 256 * 1024; }
    int maxPendingRequests() const override { return 1; }
    void open() override;
    void request(qint64 offset, qint64 length) override;
    void cancelRequests() override;
    void close() override;

    // the plugin hands over the socket after a matching GIV (the GIV line is consumed)
    void acceptPushedSocket(QTcpSocket *sock);
    const SourceDesc &desc() const { return m_desc; }
    QString downloadId() const { return m_downloadId; }
    qint64 bytesReceived() const { return m_received; }

private:
    enum Phase { PNone, PConnecting, PWaitPush, PIdle, PHeaders, PBody, PThexHeaders, PThexBody };
    void setState(State s, const QString &text);
    void connectDirect();
    void attachSocket(QTcpSocket *s);
    void sendRequest();
    void sendThexRequest();
    void meterWrite(const QByteArray &bytes);   // [glue-3]
    void onReadyRead();
    void onSocketGone(const QString &why);
    void handleResponse(const HeaderBlock &h);
    void handleThexResponse(const HeaderBlock &h);
    void deliverBody();
    void finishCurrent(bool ok, const QString &err);
    void fail(const QString &why);
    void pollQueue();
    void dropSocket();
    void harvestAltLocs(const HeaderBlock &h);

    SourceHost *m_host;
    std::shared_ptr<bool> m_hostAlive;
    QString m_downloadId;
    mx::FileHashes m_hashes;
    qint64 m_size;
    SourceDesc m_desc;
    mx::SearchSource m_orig;
    State m_state = Idle;
    QString m_stateText = QStringLiteral("Idle");
    QString m_server;
    QList<QPair<qint64, qint64>> m_avail;

    QTcpSocket *m_sock = nullptr;
    bool m_tls = false;            // [enc-1] the current connection is encrypted
    Phase m_phase = PNone;
    HeaderReader m_hdr;
    QTimer m_timeout, m_poll, m_pumpTimer;
    bool m_triedPush = false, m_triedDirect = false;
    bool m_useGetIndex = false;
    bool m_keepAlive = true;
    // current engine request
    bool m_hasReq = false;
    qint64 m_reqOff = 0, m_reqLen = 0;
    qint64 m_lastOff = 0, m_lastLen = 0;   // for queue polling
    // body being read
    qint64 m_bodyOff = 0, m_bodyLeft = 0, m_bodyGot = 0;
    bool m_bodyIsPrefetch = false;
    QByteArray m_prefetch; qint64 m_prefetchOff = -1;
    bool m_bodyDiscard = false;
    bool m_closeAfterBody = false;
    // queue
    QueueInfo m_queue;
    int m_busyCount = 0;
    // THEX
    QString m_thexUri;
    bool m_thexDone = false;
    QByteArray m_thexBuf;
    qint64 m_thexLeft = 0;
    bool m_pendingAfterThex = false;
    qint64 m_thexRetryAtMs = 0;       // server is still computing the tree (503 + Retry-After)
    int m_thexTries = 0;
    qint64 m_received = 0;
    int m_reconnects = 0;
    bool m_closed = false;
};

} // namespace mx::gnut
