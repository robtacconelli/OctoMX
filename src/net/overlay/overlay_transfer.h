// SPDX-License-Identifier: GPL-3.0-or-later
// Net::Overlay direct transfers. Private to mxnet_overlay.
//  * OverlaySource: the download side: a RangeSource talking to one OctoMX user.
//  * UploadSession: the serve side: one remote downloader of one of our files (slot or queue).
#pragma once

#include "mx/plugin.h"

#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QTimer>

#include <deque>

namespace mx::ovl {

class OverlayPlugin;

inline constexpr int kDataHeader = 18;            // 'D' ver sid:u32 rq:u32 off:u64
inline constexpr int kMaxFramePayload = 64 * 1024;
inline constexpr qint64 kMaxRequest = 4 * 1024 * 1024;
inline constexpr qint64 kReadChunk = 256 * 1024;

QByteArray makeDataFrame(quint32 sid, quint32 rq, qint64 off, const char *data, int len);
bool parseDataFrame(const QByteArray &f, quint32 *sid, quint32 *rq, qint64 *off, int *payloadPos);

class OverlaySource : public RangeSource {
    Q_OBJECT
public:
    OverlaySource(OverlayPlugin *plugin, const QString &downloadId, const FileHashes &h, qint64 size,
                  const SearchSource &src, const QString &userId, const QString &nodeId);
    ~OverlaySource() override;

    SearchSource descriptor() const override { return m_src; }
    State state() const override { return m_state; }
    QString stateText() const override;
    QString clientName() const override { return m_client; }
    QList<QPair<qint64, qint64>> available() const override;
    qint64 preferredRequestSize() const override;   // [bw-1] smaller while a download limit is on
    int maxPendingRequests() const override { return 2; }
    void open() override;
    void request(qint64 offset, qint64 length) override;
    void cancelRequests() override;
    void close() override;

    // from the plugin
    quint32 sid() const { return m_sid; }
    QString nodeId() const { return m_nodeId; }
    void onSlot(const QString &st, int pos, const QString &reason, const QStringList &hashSets = {});
    void onHashSet(const QJsonObject &o);   // [engine-3] answer to hs_req
    void onData(quint32 rq, qint64 off, const char *bytes, int len);
    void onGot(quint32 rq, bool ok, const QString &err);
    void peerGone();
    void linkMoved();   // [ovl-2] librats moved the peer to another link: what was in flight may be lost
    void fail(const QString &why);

private:
    void setState(State s);
    void sendOpen();
    void check();
    struct Req { quint32 rq; qint64 off; qint64 len; qint64 got; qint64 lastMs; };
    QPointer<OverlayPlugin> m_plugin;
    QString m_downloadId;
    FileHashes m_h;
    qint64 m_size;
    SearchSource m_src;
    QString m_userId, m_nodeId, m_client;
    quint32 m_sid;
    quint32 m_rqSeq = 0;
    State m_state = Idle;
    int m_queuePos = 0;
    QString m_error;
    std::deque<Req> m_reqs;
    QTimer m_timer;
    qint64 m_lastOpenMs = 0;
    bool m_closed = false;
    bool m_hsAsked = false;                       // [engine-3] hs_req sent (once per session)
    // [bw-1] download limit: "get" messages wait until the shared budget has their bytes
    void sendPaced();
    std::deque<QJsonObject> m_unsent;
    qint64 m_downCredit = 0;
    QTimer m_paceTimer;
};

struct UploadSession {
    QString key;            // "<node>/<sid>"
    QString uploadId;       // reported via uploadChanged
    QString nodeId, userId, userName;
    quint32 sid = 0;
    FileHashes hashes;
    qint64 size = 0;
    QString name;
    bool active = false;
    bool hotlist = false;
    qint64 queuedMs = 0, startedMs = 0, lastActivityMs = 0;
    struct Req { quint32 rq; qint64 off; qint64 len; qint64 sent; };
    std::deque<Req> reqs;
    QByteArray buf;          // read-ahead for reqs.front(), sent from bufPos
    int bufPos = 0;
    bool reading = false;
    bool throttled = false;  // waiting for limiter tokens
    quint64 serial = 0;
    qint64 bytesSent = 0;
    qint64 rateWindowBytes = 0, rateWindowStart = 0;
    int rateBps = 0;
    qint64 lastReportMs = 0;
    int lastQueuePos = -1;
    QStringList hashSets;    // [engine-3] kinds we can answer hs_req with ("ed2k-parts")
    int hashSetsServed = 0;
};

// [engine-3] hash-set exchange limits
inline constexpr qint64 kEd2kPart = 9728000;
inline constexpr int kMaxHashSetParts = 120000;   // ~1.1 TB; 3.8 MB of hex, under the 4 MiB message cap

} // namespace mx::ovl
