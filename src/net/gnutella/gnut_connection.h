// SPDX-License-Identifier: GPL-3.0-or-later
// Gnutella module: one outgoing leaf→ultrapeer connection: 0.6 three-way handshake, optional
// deflate in both directions, descriptor framing, keep-alive pings, bounded output queue.
#pragma once

#include <functional>

#include "gnut_headers.h"
#include "gnut_message.h"
#include "gnut_tls.h"
#include "gnut_util.h"
#include "gnut_zlib.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <memory>

class QTcpSocket;

namespace mx { class RateLimiter; }

namespace mx::gnut {

struct HandshakeConfig {
    QString userAgent;              // "OctoMX/5.1"
    HostPort listen;                // our externally visible address if known (ip may be 0)
    bool acceptDeflate = true;
    bool browseHost = true;
    int handshakeTimeoutMs = 20000;
    mx::RateLimiter *meter = nullptr;   // [glue-3] bandwidth graph accounting (not throttled)
    // [enc-1] TLS: engine (null = plain only), how to connect, and whether we announce "tls/1.0"
    std::shared_ptr<TlsEngine> tls;
    TlsSocket::Policy tlsPolicy = TlsSocket::Plain;
    bool advertiseTls = false;
    // [net-2] proxy / chosen adapter for the link (mx/net_policy.h)
    std::function<void(QTcpSocket *)> prepare;
};

class UltrapeerConnection : public QObject {
    Q_OBJECT
public:
    enum State { Connecting, Handshaking, Connected, Closed };
    UltrapeerConnection(const HostPort &addr, const HandshakeConfig &cfg, QObject *parent = nullptr);
    ~UltrapeerConnection() override;

    void start();
    // Queue a message. lowPriority messages are dropped while the socket is congested.
    void send(const Message &m, bool lowPriority = false);
    void close(const QString &reason, quint16 byeCode = 0);

    State state() const { return m_state; }
    HostPort address() const { return m_addr; }
    QString id() const { return m_addr.toString(); }
    QString vendor() const { return m_vendor; }
    QString remoteIpSeen() const { return m_remoteIp; }         // our IP as the ultrapeer sees it
    const HeaderBlock &response() const { return m_response; }
    qint64 connectedMs() const { return m_connected.isValid() ? m_connected.elapsed() : 0; }
    qint64 handshakeMs() const { return m_handshakeMs; }
    bool compressedIn() const { return bool(m_inflater); }
    bool compressedOut() const { return bool(m_deflater); }
    bool supportsVendor(const QByteArray &vendor, quint16 id, quint16 *version = nullptr) const;
    void setSupportedVendorMessages(const QList<VendorKey> &k) { m_vendorMsgs = k; }
    bool vendorMessagesKnown() const { return !m_vendorMsgs.isEmpty(); }
    int pingMs() const { return m_pingMs; }
    qint64 bytesIn() const { return m_bytesIn; }
    qint64 bytesOut() const { return m_bytesOut; }
    int messagesIn() const { return m_msgsIn; }
    int messagesOut() const { return m_msgsOut; }
    int droppedOut() const { return m_dropped; }
    bool dynamicQuerying() const { return m_dynamicQuerying; }
    // [enc-1]
    bool encrypted() const;                 // the link runs over TLS
    bool tlsTried() const;                  // a TLS handshake was attempted
    bool tlsFellBack() const;               // ... failed, and the link is plain
    QString tlsInfo() const;                // "TLSv1.3 TLS_AES_256_GCM_SHA384" or the TLS error
    bool remoteOffersTls() const { return m_remoteTls; }   // X-Features: tls/x.y in its handshake
    // hops flow from the ultrapeer (BEAR/4); 0 = do not send queries
    int hopsFlow() const { return m_hopsFlow; }
    void setHopsFlow(int v) { m_hopsFlow = v; }
    // QRP state, managed by the plugin
    QByteArray qrpSentDigest;
    bool qrpAcked = false;
    // misc per-connection flags managed by the plugin
    bool connectBackSent = false;
    bool pushProxyRequested = false;
    HostPort pushProxyAddr;         // set when the UP acked our push-proxy request

signals:
    void handshakeDone();
    void handshakeFailed(int code, const QString &reason, const QList<mx::gnut::HostPort> &tryHosts, int retryAfterSec);
    void messageReceived(const mx::gnut::Message &m);
    void closed(const QString &reason);

private:
    void onConnected();
    void onReadyRead();
    void onDisconnected();
    void processStream(const QByteArray &data);
    void flush();
    void sendPing();
    void fail(const QString &reason);

    HostPort m_addr;
    HandshakeConfig m_cfg;
    QTcpSocket *m_sock = nullptr;
    State m_state = Connecting;
    HeaderReader m_hdr;
    HeaderBlock m_response;
    QString m_vendor, m_remoteIp;
    std::unique_ptr<Inflater> m_inflater;
    std::unique_ptr<Deflater> m_deflater;
    Framer m_framer;
    QByteArray m_out;               // uncompressed pending output
    bool m_flushScheduled = false;
    QTimer m_timeout, m_pingTimer;
    QElapsedTimer m_started, m_connected, m_lastRx;
    qint64 m_handshakeMs = 0;
    QByteArray m_pingGuid;
    QElapsedTimer m_pingSent;
    int m_pingMs = -1;
    qint64 m_bytesIn = 0, m_bytesOut = 0;
    void meter(bool up, qint64 bytes);   // [glue-3]
    int m_msgsIn = 0, m_msgsOut = 0, m_dropped = 0;
    QList<VendorKey> m_vendorMsgs;
    bool m_dynamicQuerying = false;
    int m_hopsFlow = 255;
    bool m_closing = false;
    bool m_remoteTls = false;
};

} // namespace mx::gnut
