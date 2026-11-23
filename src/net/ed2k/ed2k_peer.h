// SPDX-License-Identifier: GPL-3.0-or-later
// Client-to-client connection: hello handshake, eMule info, and routing of packets to the
// download role (a RangeSource that requested a file from this peer) and the upload role
// (UploadManager serving our shared files). Shared by the ED2K plugin and (next) Kad.
#pragma once

#include "ed2k_connection.h"
#include "ed2k_util.h"

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QTimer>

namespace mx::ed2k {

class Ed2kCore;
class PeerConnection;

struct PeerInfo {
    QByteArray userHash;
    quint32 clientId = 0;        // as sent in hello
    quint16 tcpPort = 0;
    QString name;
    quint32 ed2kVersion = 0;
    bool    emuleProtocol = false;  // CT_EMULE_VERSION or OP_EMULEINFO seen
    quint8  compatClient = 0;
    quint32 clientVersion = 0;      // packed major<<17 | minor<<10 | update<<7
    quint8  emuleInfoVersion = 0;   // from OP_EMULEINFO (old clients)
    QString modVersion;
    quint16 udpPort = 0, kadPort = 0;
    quint8  aichVersion = 0, udpVersion = 0, dataCompVersion = 0, secIdentVersion = 0;
    quint8  sourceExchange1Version = 0, extendedRequestsVersion = 0, acceptCommentVersion = 0;
    bool    unicode = false, noViewSharedFiles = false, multiPacket = false, supportsPreview = false;
    quint8  kadVersion = 0;
    bool    largeFiles = false, extMultiPacket = false, supportsCrypt = false, requestsCrypt = false, requiresCrypt = false;
    bool    sourceExchange2 = false, captcha = false, directUdpCallback = false, fileIdentifiers = false;
    quint32 serverIp = 0;
    quint16 serverPort = 0;
    quint32 buddyIp = 0;
    quint16 buddyPort = 0;
    bool    mlDonkey = false, hybrid = false;
    QString software() const;    // "eMule v0.50a", "aMule v2.3.3", ...
};

// Implemented by Ed2kRangeSource: receives the download-role packets of the connection it owns.
class DownloadSink {
public:
    virtual ~DownloadSink() = default;
    virtual void peerReady(PeerConnection *pc) = 0;                       // handshake done
    virtual void peerPacket(PeerConnection *pc, const Packet &p) = 0;     // download-role packet
    virtual void peerClosed(PeerConnection *pc, const QString &reason) = 0;
};

class PeerConnection : public QObject {
    Q_OBJECT
public:
    PeerConnection(Ed2kCore *core, Ed2kConnection *conn, bool outgoing, QObject *parent = nullptr);
    ~PeerConnection() override;

    // outgoing: decides on obfuscation, connects (core calls this when a connect slot is free)
    void startOutgoing(const PeerDescriptor &d, bool allowObfuscation = true);
    bool started() const { return m_started; }
    const PeerDescriptor &target() const { return m_target; }
    void startIncoming();

    bool outgoing() const { return m_outgoing; }
    bool handshakeDone() const { return m_handshakeDone; }
    bool obfuscated() const { return m_conn && m_conn->obfuscated(); }
    bool triedObfuscation() const { return m_triedObf; }
    const PeerInfo &info() const { return m_info; }
    quint32 ip() const { return m_conn ? m_conn->peerIp() : 0; }
    quint16 listenPort() const { return m_info.tcpPort ? m_info.tcpPort : m_target.port; }
    QString peerKey() const;     // "ip:listenport"
    QString describe() const;    // "1.2.3.4:4662 (eMule v0.50a)"
    bool isOpen() const { return m_conn && m_conn->isOpen() && !m_closed; }
    Ed2kConnection *conn() const { return m_conn; }

    void send(quint8 proto, quint8 opcode, const QByteArray &payload = QByteArray(), bool allowPacked = false);
    void queueData(const QByteArray &framed, qint64 payloadBytes) { if (m_conn) m_conn->queueData(framed, payloadBytes); }
    void close(const QString &reason);
    void setIdleTimeoutMs(qint64 ms) { if (m_conn) m_conn->setIdleTimeoutMs(ms); }

    void setDownloadSink(DownloadSink *s, QObject *owner);
    DownloadSink *downloadSink() const { return m_sinkOwner ? m_sink : nullptr; }
    bool uploadRole = false;     // UploadManager tracks a session on this connection

    static QByteArray buildHelloPayload(Ed2kCore *core, bool answer);
    static bool parseHelloPayload(const QByteArray &payload, bool isAnswer, PeerInfo *out);

signals:
    void handshakeComplete(mx::ed2k::PeerConnection *pc);
    void closed(mx::ed2k::PeerConnection *pc, const QString &reason);
    void dataSent(qint64 payloadBytes);

private:
    void onPacket(const Packet &p);
    void onConnClosed(const QString &reason);
    void finishHandshake();
    bool handleMisc(const Packet &p);
    void sendMuleInfo(bool answer);

    Ed2kCore *m_core;
    Ed2kConnection *m_conn;
    bool m_outgoing;
    bool m_started = false;
    bool m_handshakeDone = false;
    bool m_helloSent = false;
    bool m_closed = false;
    bool m_triedObf = false;
    PeerDescriptor m_target;
    PeerInfo m_info;
    DownloadSink *m_sink = nullptr;
    QPointer<QObject> m_sinkOwner;
    QTimer m_handshakeTimeout;
};

bool isDownloadRolePacket(const Packet &p);
bool isUploadRolePacket(const Packet &p);

} // namespace mx::ed2k
