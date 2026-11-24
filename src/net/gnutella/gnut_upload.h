// SPDX-License-Identifier: GPL-3.0-or-later
// Gnutella module: HTTP/1.1 upload server session (one per incoming or pushed connection):
// /uri-res/N2R?urn:sha1:..., /get/<index>/<name>, /uri-res/N2X (THEX), browse host ("GET /" with
// Accept: application/x-gnutella-packets), Range, remote queueing (X-Queue / PARQ X-Queued),
// alt-locs, rate limiting through the shared RateLimiter.
#pragma once

#include "gnut_headers.h"
#include "gnut_http.h"
#include "gnut_util.h"
#include "mx/plugin.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QObject>
#include <QtCore/QTimer>

class QTcpSocket;

namespace mx::gnut {

class UploadSession;

class ServeHost {
public:
    virtual ~ServeHost() = default;
    virtual mx::LibraryAccess *library() = 0;
    virtual mx::RateLimiter *limiter() = 0;
    virtual QString userAgent() const = 0;
    virtual bool sharedBySha1(const QByteArray &sha1, mx::SharedFile *out) = 0;
    virtual bool sharedByTth(const QByteArray &tth, mx::SharedFile *out) = 0;
    virtual bool sharedByIndex(quint32 index, mx::SharedFile *out) = 0;
    virtual quint32 indexOf(const mx::SharedFile &f) = 0;
    // Upload slot for `key` (peer ip + file). 1 = granted, 0 = queued (*q filled), -1 = refused.
    virtual int acquireSlot(UploadSession *s, const QString &key, const QString &parqId, QueueInfo *q) = 0;
    virtual void releaseSlot(UploadSession *s) = 0;
    virtual bool browseAllowed(const HostPort &peer) = 0;
    virtual QList<QByteArray> browsePackets() = 0;         // encoded QueryHit messages of our share
    // THEX of a shared file: DIME bytes, or empty + *retryAfter > 0 while it is being computed
    virtual QByteArray thexFor(const mx::SharedFile &f, int *retryAfter) = 0;
    virtual QByteArray tthFor(const mx::SharedFile &f) = 0;  // known TTH root (library or computed)
    virtual QList<HostPort> altLocsFor(const QByteArray &sha1, const HostPort &exclude) = 0;
    virtual void addAltLocs(const QByteArray &sha1, const QList<HostPort> &hosts) = 0;
    virtual void reportUpload(const mx::TransferInfo &t) = 0;
    virtual void reportUploadFinished(const QString &id) = 0;
    virtual void noteIncomingHttp(const HostPort &peer, const QString &what) = 0;   // log / stats
};

class UploadSession : public QObject {
    Q_OBJECT
public:
    // `sock` is connected; `initial` = bytes already read from it (the start of the first request).
    UploadSession(ServeHost *host, QTcpSocket *sock, const QByteArray &initial, bool pushed, QObject *parent = nullptr);
    ~UploadSession() override;
    QString uploadId() const { return m_uploadId; }
    HostPort peer() const { return m_peer; }
    void cancel();                                      // abort (cancelUpload)
    void stopNotShared();                               // [net-3] the file left the shares: stop, say why
    const mx::SharedFile &file() const { return m_file; }   // [net-3] the file of the last request
    bool transferring() const { return m_remaining > 0; }
    const mx::TransferInfo &transferInfo() const { return m_info; }   // [glue-3] for queue rows
    QString slotKey() const { return m_slotKey; }                       // [glue-3] "ip/sha1" of the last request

signals:
    void finished();                                    // connection gone; deleteLater() follows

private:
    void onReadyRead();
    void writeMeta(const QByteArray &bytes);   // [glue-3] metered header write
    void handleRequest(const HeaderBlock &req);
    void serveFile(const HeaderBlock &req, const mx::SharedFile &f, bool head);
    void serveThex(const HeaderBlock &req, const mx::SharedFile &f, bool head);
    void serveBrowse(const HeaderBlock &req, bool head);
    void sendSimple(int code, const QString &reason, const QList<QPair<QString, QString>> &extra = {},
                    const QByteArray &body = QByteArray(), bool close = false);
    void pump();
    void endUpload(mx::TransferState st, const QString &text);
    void report(bool force);
    void closeNow();

    ServeHost *m_host;
    QTcpSocket *m_sock;
    HostPort m_peer;
    bool m_pushed;
    HeaderReader m_hdr;
    QTimer m_idle, m_pumpTimer, m_reportTimer;
    bool m_closeAfter = false;
    bool m_haveSlot = false;
    bool m_queued = false;
    QString m_slotKey;        // [glue-3] last answer was "Queued": the row belongs to the queue entry
    // current body
    mx::SharedFile m_file;
    qint64 m_offset = 0, m_remaining = 0;
    // upload session (per file)
    QString m_uploadId;
    QByteArray m_uploadSha1;
    qint64 m_sentTotal = 0;
    qint64 m_rateBytes = 0;
    int m_rateBps = 0;
    QElapsedTimer m_rateClock;
    QString m_peerAgent;
    mx::TransferInfo m_info;
    bool m_done = false;
};

} // namespace mx::gnut
