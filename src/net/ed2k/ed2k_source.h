// SPDX-License-Identifier: GPL-3.0-or-later
// mx::RangeSource for one eD2k peer and one file.
//
// Life cycle (eMule download protocol):
//   Idle → Connecting (TCP [+obfuscation], hello, OP_REQUESTFILENAME/OP_SETREQFILEID, hashset,
//   OP_STARTUPLOADREQ) → Queued (OP_QUEUERANKING; re-asked over TCP every 29 min, never more
//   often) → Ready (OP_ACCEPTUPLOADREQ: we hold an upload slot) ⇄ Busy (request() in flight:
//   OP_REQUESTPARTS with ≤3 blocks of ≤180 KiB that never cross a 9.28 MB part, data via
//   OP_SENDINGPART / OP_COMPRESSEDPART [_I64]) → back to Queued on OP_OUTOFPARTREQS.
//   LowID peers are reached through a server callback (only when we are HighID on their server).
//   A peer may also connect to us (slot offer / callback); Ed2kCore attaches that connection.
#pragma once

#include "ed2k_io.h"
#include "ed2k_peer.h"
#include "ed2k_util.h"

#include "mx/plugin.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QPointer>
#include <QtCore/QTimer>

struct z_stream_s;

namespace mx::ed2k {

class Ed2kCore;

class Ed2kRangeSource : public mx::RangeSource, public DownloadSink {
    Q_OBJECT
public:
    Ed2kRangeSource(Ed2kCore *core, const QString &downloadId, const mx::FileHashes &h, qint64 size,
                    const mx::SearchSource &src, const PeerDescriptor &peer, QObject *parent = nullptr);
    ~Ed2kRangeSource() override;

    // mx::RangeSource
    mx::SearchSource descriptor() const override;
    State state() const override { return m_state; }
    QString stateText() const override;
    QString clientName() const override;
    QList<QPair<qint64, qint64>> available() const override { return m_available; }
    qint64 preferredRequestSize() const override { return PREFERRED_REQUEST; }
    int maxPendingRequests() const override { return 1; }
    void open() override;
    void request(qint64 offset, qint64 length) override;
    void cancelRequests() override;
    void close() override;

    // DownloadSink
    void peerReady(PeerConnection *pc) override;
    void peerPacket(PeerConnection *pc, const Packet &p) override;
    void peerClosed(PeerConnection *pc, const QString &reason) override;

    // used by Ed2kCore to hand over connections the peer opened to us
    bool matchesIncoming(const PeerInfo &info, quint32 ip) const;
    bool wantsConnection() const;
    void attach(PeerConnection *pc);
    const PeerDescriptor &peer() const { return m_peer; }
    QByteArray fileHash() const { return m_hashes.ed2k; }
    QString downloadId() const { return m_downloadId; }
    int queueRank() const { return m_rank; }
    // UDP re-ask (OP_REASKFILEPING): Ed2kCore routes OP_REASKACK / OP_FILENOTFOUND / OP_QUEUEFULL here
    bool awaitingUdpAnswer(quint32 ip, quint16 port) const { return m_udpPending && ip == m_peer.ip && port == m_peerUdpPort; }
    void udpAnswer(quint8 opcode, const QByteArray &payload);
    int udpReasks() const { return m_udpReasks; }
    bool udpPending() const { return m_udpPending; }
    // tests: re-ask a queued peer over UDP now (ignores the 29-minute timer); false if UDP is not possible
    bool debugForceReask() { if (!udpReaskPossible() || m_udpPending) return false; sendUdpReask(); return true; }

private:
    enum Phase { PIdle, PConnecting, PWaitCallback, PFileRequest, PHashset, PUploadRequest, PQueued,
                 PQueuedOffline, PNoParts, PDownloading, PFailed, PClosed };
    struct Block {
        qint64 start = 0, end = 0;
        QByteArray buf;          // received, not yet emitted bytes
        qint64 got = 0;          // contiguous bytes received
        qint64 emitted = 0;
        bool requested = false;
        bool done = false;
        z_stream_s *z = nullptr;
        quint32 zTotal = 0, zGot = 0;
    };

    void setPhase(Phase p, const QString &text = QString());
    void updateState();
    void connectNow(bool allowObfuscation);
    void sendFileRequest();
    void afterFileInfo();
    void sendStartUpload();
    void sendBlockRequests();
    void onData(qint64 start, qint64 end, const QByteArray &data);
    void onCompressed(qint64 blockStart, quint32 packedTotal, const QByteArray &data);
    void flushEmits();
    void finishRequest(bool ok, const QString &error);
    void resetBlocks();
    void freeZ(Block &b);
    void fail(const QString &why);
    void scheduleReask();
    void tick();
    void handleFileStatus(Reader &r);
    void handleAnswerSources(const Packet &p);
    void releaseConnection(const QString &why);
    bool udpReaskPossible() const;
    void sendUdpReask();

    Ed2kCore *m_core;
    QString m_downloadId;
    mx::FileHashes m_hashes;
    qint64 m_size;
    mx::SearchSource m_src;
    PeerDescriptor m_peer;
    QPointer<PeerConnection> m_pc;
    State m_state = Idle;
    Phase m_phase = PIdle;
    QString m_text;
    QString m_remoteName;
    QString m_software;
    QList<QPair<qint64, qint64>> m_available;
    bool m_complete = false;
    bool m_gotName = false, m_gotStatus = false;
    bool m_triedPlain = false;
    bool m_everQueued = false;
    bool m_wasObfuscated = false;
    int m_rank = 0;
    int m_failedConnects = 0;
    qint64 m_nextReaskMs = 0;
    qint64 m_phaseSinceMs = 0;
    qint64 m_lastDataMs = 0;
    qint64 m_idleSinceMs = 0;
    // current request
    bool m_reqActive = false;
    qint64 m_reqOffset = 0, m_reqLength = 0;
    QList<Block> m_blocks;
    qint64 m_bytesReceived = 0;
    // client UDP of the peer (from its hello) for UDP re-asks
    quint16 m_peerUdpPort = 0;
    quint8 m_peerUdpVersion = 0;
    bool m_udpPending = false;
    qint64 m_udpSentMs = 0;
    int m_udpFails = 0;
    int m_udpReasks = 0;
    QTimer m_tick;
    QElapsedTimer m_clock;
};

} // namespace mx::ed2k
