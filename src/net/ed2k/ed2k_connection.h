// SPDX-License-Identifier: GPL-3.0-or-later
// One TCP connection speaking eD2k framing, optionally obfuscated, with bounded buffers, timeouts
// and rate limiting through mx::RateLimiter. Used for server and peer connections alike.
#pragma once

#include "ed2k_crypto.h"
#include "ed2k_packet.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QQueue>
#include <QtCore/QTimer>
#include <QtNetwork/QTcpSocket>

namespace mx { class RateLimiter; }

namespace mx::ed2k {

class Ed2kConnection : public QObject {
    Q_OBJECT
public:
    // Outgoing (call connectTo) or wrapping an accepted socket (takes ownership).
    explicit Ed2kConnection(QObject *parent = nullptr);
    explicit Ed2kConnection(QTcpSocket *accepted, QObject *parent = nullptr);
    ~Ed2kConnection() override;

    void setLimiter(mx::RateLimiter *l) { m_limiter = l; }
    // [glue-3] bandwidth-graph accounting only (no throttling), e.g. server connections
    void setMeter(mx::RateLimiter *l) { m_meter = l; }
    void setMaxPacket(quint32 maxPacket);
    void setIdleTimeoutMs(qint64 ms) { m_idleTimeoutMs = ms; }
    void setConnectTimeoutMs(qint64 ms) { m_connectTimeoutMs = ms; }

    // crypt setup: call before connectTo (outgoing) or right after construction (incoming)
    void useClientObfuscation(const QByteArray &targetUserHash);
    void useServerObfuscation();
    void useIncomingDetection(const QByteArray &ourUserHash, bool requireObfuscation);

    void connectTo(quint32 ed2kIp, quint16 port);
    void connectToHost(const QString &host, quint16 port);

    // control packets go out before queued data packets
    void sendPacket(quint8 proto, quint8 opcode, const QByteArray &payload, bool allowPacked = false);
    void sendRaw(const QByteArray &framed);
    // throttled bulk data (already framed); payloadBytes is what counts as "uploaded"
    void queueData(const QByteArray &framed, qint64 payloadBytes);
    qint64 queuedDataBytes() const { return m_dataQueued; }
    void clearDataQueue();

    void close(const QString &reason);           // emits closed() once (queued)
    bool isOpen() const { return m_state == Open || m_state == Connecting; }
    bool isConnected() const { return m_state == Open; }
    bool obfuscated() const { return m_crypt.obfuscated(); }
    bool incoming() const { return m_incoming; }

    quint32 peerIp() const;       // eD2k order
    quint16 peerPort() const { return m_peerPort; }
    QString peerAddress() const;
    qint64 bytesIn() const { return m_bytesIn; }
    qint64 bytesOut() const { return m_bytesOut; }
    qint64 msSinceActivity() const { return m_activity.isValid() ? m_activity.elapsed() : 0; }
    void touch() { m_activity.restart(); }

    // pump throttled data (also called by a shared timer)
    void pump();

signals:
    void connected();                               // TCP established (outgoing)
    void packetReceived(const mx::ed2k::Packet &p);
    void closed(const QString &reason);
    void dataSent(qint64 payloadBytes);

private:
    void init();
    void onReadyRead();
    void onBytesWritten(qint64);
    void onSocketError();
    void onTick();
    void writeWire(const QByteArray &plain, qint64 payloadBytes = 0);
    void meterOut(qint64 payloadBytes);   // [glue-3] accounts m_bytesOut growth
    bool exemptPeer() const;              // [bw-1] LAN peer while bandwidth/limitLan is off
    void flushCryptOutput();
    void finish(const QString &reason, bool graceful = false);

    enum State { Idle, Connecting, Open, Closed };
    QTcpSocket *m_sock = nullptr;
    State m_state = Idle;
    StreamCrypt m_crypt;
    PacketParser m_parser;
    mx::RateLimiter *m_limiter = nullptr;
    mx::RateLimiter *m_meter = nullptr;
    qint64 m_bytesOutMetered = 0;
    QQueue<QPair<QByteArray, qint64>> m_dataQ;
    qint64 m_dataQueued = 0;
    qint64 m_upCredit = 0;
    qint64 m_bytesIn = 0, m_bytesOut = 0;
    qint64 m_idleTimeoutMs = 60000;
    qint64 m_connectTimeoutMs = 30000;
    QElapsedTimer m_activity, m_connectClock;
    QTimer m_tick;
    QTimer m_readRetry;
    QTimer m_pumpRetry;
    quint32 m_peerIpCached = 0;
    quint16 m_peerPort = 0;
    bool m_incoming = false;
    bool m_closeEmitted = false;
};

} // namespace mx::ed2k
