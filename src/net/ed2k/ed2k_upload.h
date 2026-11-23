// SPDX-License-Identifier: GPL-3.0-or-later
// Serving our shared files to other eD2k clients: answers file requests, keeps a fair FIFO
// upload queue (longest waiting first), hands out a configurable number of slots, sends
// OP_QUEUERANKING, reads blocks through LibraryAccess and sends them (zlib-compressed when that
// helps and the peer supports it), honouring the shared RateLimiter.
#pragma once

#include "ed2k_io.h"
#include "ed2k_packet.h"
#include "ed2k_util.h"

#include "mx/types.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QThreadPool>
#include <QtCore/QTimer>

namespace mx::ed2k {

class Ed2kCore;
class PeerConnection;

class UploadManager : public QObject {
    Q_OBJECT
public:
    explicit UploadManager(Ed2kCore *core);
    ~UploadManager() override;

    void handlePacket(PeerConnection *pc, const Packet &p);
    // OP_REASKFILEPING from a queued client (client UDP)
    void handleUdpReask(quint32 ip, quint16 port, const QByteArray &payload, bool obfuscated);
    int udpReasksAnswered() const { return m_udpReasks; }
    void peerConnected(PeerConnection *pc);      // handshake done (maybe a queued client came back)
    void peerClosed(PeerConnection *pc);
    void stop();

    int activeUploads() const;
    int queueLength() const { return int(m_waiting.size()); }
    qint64 totalUploaded() const { return m_totalUploaded; }
    int rateBps() const;
    // [glue-3] queued clients are reported as uploads in state Queued (same id while the slot is
    // granted); the Transfers page can force a slot, reorder the queue or drop a client
    bool startUploadNow(const QString &uploadId);        // false: unknown / not waiting
    bool setUploadPriority(const QString &uploadId, int priority);   // -1 back ... 1 front
    bool cancelUpload(const QString &uploadId);          // drop a waiting client or end a session
    QStringList waitingUploadIds() const;                // queue order (tests)
    int packedBlocks() const { return m_packedBlocks; }   // blocks sent zlib-compressed
    int plainBlocks() const { return m_plainBlocks; }

private:
    struct Block { qint64 start = 0, end = 0; };
    struct Client {
        QString key;                 // userhash hex or ip:port
        QPointer<PeerConnection> pc;
        QByteArray userHash;
        quint32 ip = 0;
        quint16 port = 0;
        quint16 udpPort = 0;
        quint8 udpVersion = 0;
        bool cryptUdp = false;
        quint8 cryptOptions = 0;       // [enc-1] supports 0x01 / requests 0x02 / requires 0x04 (hello)
        bool lowId = false;
        QString name, software;
        QByteArray fileHash;
        mx::SharedFile file;
        qint64 waitingSinceMs = 0;
        qint64 lastAskMs = 0;
        bool uploading = false;
        bool addNextConnect = false;
        bool connectingBack = false;
        QList<Block> blocks;          // requested, not yet sent
        QList<Block> sentBlocks;      // recently sent (dedupe of re-requested blocks)
        qint64 queuedBytes = 0;       // payload handed to the socket this session
        bool sessionOver = false;
        qint64 sessionBytes = 0;
        qint64 sessionStartMs = 0;
        qint64 lastRequestMs = 0;
        QString uploadId;
        qint64 rateWindowBytes = 0;
        int rateBps = 0;
        bool compress = false;
        bool i64 = false;
        int counter = 0;
        bool readPending = false;     // a block is being read / packed on the worker thread
        bool partial = false;         // the file is a running download: serve verified parts only
        int priority = 0;             // [glue-3] queue priority (-1, 0, 1)
        bool forced = false;          // [glue-3] "Start Transfer": slot regardless of the slot count
        int reportedRank = -1;        // [glue-3] last rank reported to the core
    };
    struct ReadJob { QString key; int counter = 0; Block block; };
    void startRead(Client &c, const Block &b);
    void onReadDone(const ReadJob &job, const QList<QPair<QByteArray, qint64>> &packets, bool ok, bool packed);

    bool lookupFile(const QByteArray &hash, mx::SharedFile *out) const;
    Client *clientFor(PeerConnection *pc, bool create);
    void onStartUpload(PeerConnection *pc, const QByteArray &hash);
    void onRequestParts(PeerConnection *pc, const Packet &p, bool i64);
    void answerFileName(PeerConnection *pc, const QByteArray &hash, Reader &extInfo, const mx::SharedFile &f);
    void answerFileStatus(PeerConnection *pc, const mx::SharedFile &f);
    void answerHashset(PeerConnection *pc, const QByteArray &hash);
    void answerMultiPacket(PeerConnection *pc, const Packet &p);
    void sendNoFile(PeerConnection *pc, const QByteArray &hash);
    void process();
    void startSession(Client &c);
    void countSentOn(Client &c);   // [up-1] the session's byte counter follows its connection
    void endSession(Client &c, const QString &why, bool requeue);
    void pumpClient(Client &c);
    void sendRanks();
    void enqueue(Client &c);                  // [glue-3] add to m_waiting (priority order) + report Queued
    void dropClient(const QString &key);      // [glue-3] remove a waiting client (+ uploadFinished)
    void sortWaiting();
    void reportQueue();
    Client *clientByUploadId(const QString &uploadId);
    QHash<QString, qint64> m_blocked;         // [glue-3] key → cancelled until (ms), no re-queue
    void report(Client &c, bool finished = false);
    void tick();
    mx::TransferInfo info(const Client &c) const;
    QList<QByteArray> partHashes(const mx::SharedFile &f) const;

    Ed2kCore *m_core;
    QHash<QString, Client> m_clients;    // uploading + waiting, by key
    QList<QString> m_waiting;            // FIFO order of waiting keys
    QTimer m_tick;
    QElapsedTimer m_clock;
    qint64 m_totalUploaded = 0;
    int m_counter = 0;
    int m_packedBlocks = 0, m_plainBlocks = 0;
    int m_udpReasks = 0;
    QThreadPool m_pool;               // disk reads + compression of upload blocks
};

} // namespace mx::ed2k
