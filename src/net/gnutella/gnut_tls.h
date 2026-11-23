// SPDX-License-Identifier: GPL-3.0-or-later
// Gnutella module: TLS for TCP connections (ultrapeer links, HTTP downloads / uploads, push,
// browse), the way gtk-gnutella and LimeWire do it: a plain TLS session started right after the
// TCP connect, detected on the listening port by the first byte (0x16, a TLS handshake record).
//
// Our own OpenSSL (linked statically) is used instead of QSslSocket so that Windows and Linux
// behave the same and the anonymous Diffie-Hellman suites LimeWire-family clients offer
// (TLS_DH_anon_WITH_AES_128_CBC_SHA) still work. Nobody verifies certificates on Gnutella: the
// session is encrypted, but the other side is not authenticated (a man in the middle could read
// it). We present a self-signed certificate made in the profile on first use, so clients that do
// not offer anonymous suites can connect too.
//
// TlsSocket is a QTcpSocket, so the existing code (UploadSession, GnutSource, BrowseClient,
// UltrapeerConnection) uses it unchanged: it wraps a real QTcpSocket, emits connected() after the
// handshake, and read()/write() move plain text. Policy TryTls falls back to a fresh plain
// connection when the TLS handshake fails; TlsOnly reports the failure; Plain only relays.
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QElapsedTimer>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpSocket>
#include <memory>

struct ssl_ctx_st;
struct ssl_st;
struct bio_st;

namespace mx::gnut {

// gnutella/encryption: "enabled" (prefer TLS, default) | "forced" (TLS only) | "disabled" (plain)
enum class TlsMode { Prefer, Require, Off };
TlsMode tlsModeFromSetting(const QString &v);
QString tlsModeName(TlsMode m);

// Counters of one plugin (shared by its sockets).
struct TlsStats {
    int outAttempts = 0;      // outgoing connections that started a TLS handshake
    int outOk = 0;            // ... and finished it
    int outFallback = 0;      // ... failed and were retried plain (prefer)
    int outFailed = 0;        // ... failed for good (require)
    int inOk = 0;             // incoming TLS sessions accepted
    int inFailed = 0;         // incoming TLS handshakes that failed
    int inRefused = 0;        // incoming TLS refused (encryption disabled) / plain refused (required)
};

// The OpenSSL contexts (client + server) of one plugin, with its certificate.
class TlsEngine {
public:
    // Loads <pemPath> (private key + certificate) or creates a self-signed one there.
    static std::shared_ptr<TlsEngine> create(const QString &pemPath, QString *error);
    ~TlsEngine();
    ssl_ctx_st *clientCtx() const { return m_client; }
    ssl_ctx_st *serverCtx() const { return m_server; }
    QString certificateInfo() const { return m_certInfo; }   // "self-signed, created 2026-11-22"
    bool createdNow() const { return m_createdNow; }
    TlsStats stats;
private:
    TlsEngine() = default;
    ssl_ctx_st *m_client = nullptr;
    ssl_ctx_st *m_server = nullptr;
    QString m_certInfo;
    bool m_createdNow = false;
};

// True when `b` (the first bytes of an incoming connection) starts a TLS handshake record.
bool looksLikeTlsClientHello(const QByteArray &b);

class TlsSocket : public QTcpSocket {
    Q_OBJECT
public:
    enum Policy { Plain, TryTls, TlsOnly };
    TlsSocket(std::shared_ptr<TlsEngine> engine, Policy policy, QObject *parent = nullptr);
    ~TlsSocket() override;

    // client: TCP connect, then (policy != Plain) the TLS handshake; connected() follows it
    void connectToHost(const QString &hostName, quint16 port, OpenMode mode = ReadWrite,
                       NetworkLayerProtocol protocol = AnyIPProtocol) override;
    using QAbstractSocket::connectToHost;
    // server: take over an accepted socket; `alreadyRead` are its first bytes (the ClientHello)
    void startServer(QTcpSocket *plain, const QByteArray &alreadyRead);

    Policy policy() const { return m_policy; }
    bool isEncrypted() const { return m_tlsUp; }
    bool fellBack() const { return m_fellBack; }
    QString cipherName() const { return m_cipher; }      // "TLSv1.3 TLS_AES_256_GCM_SHA384"
    QString tlsError() const { return m_tlsError; }
    static void setHandshakeTimeoutMs(int ms);           // tests

    qint64 bytesAvailable() const override;
    qint64 bytesToWrite() const override;
    bool canReadLine() const override;
    void close() override;
    void disconnectFromHost() override;
    void setReadBufferSize(qint64 size) override;
    bool atEnd() const override;

signals:
    void encrypted();
    void tlsFailed(const QString &why);   // before a fallback, or before errorOccurred (TlsOnly)

protected:
    qint64 readData(char *data, qint64 maxlen) override;
    qint64 writeData(const char *data, qint64 len) override;

private:
    void makeInner();
    void attachInner(QTcpSocket *s);
    void onInnerConnected();
    void onInnerReadyRead();
    void onInnerGone(QAbstractSocket::SocketError e, bool isError);
    void startTls(bool server);
    void pump(const QByteArray &in);
    void flushOut();
    void handshakeFailed(const QString &why);
    void becomeConnected();
    void finishUnconnected(bool emitDisconnected);
    void freeSsl();

    std::shared_ptr<TlsEngine> m_engine;
    Policy m_policy;
    QTcpSocket *m_inner = nullptr;
    ssl_st *m_ssl = nullptr;
    bio_st *m_rbio = nullptr, *m_wbio = nullptr;
    bool m_server = false, m_tlsStarted = false, m_tlsUp = false, m_fellBack = false;
    bool m_tcpUp = false, m_connectedEmitted = false, m_gone = false;
    QByteArray m_plain;          // decrypted input not read yet
    QByteArray m_pendingOut;     // plain output written before the handshake finished
    QString m_host;
    quint16 m_port = 0;
    OpenMode m_mode = ReadWrite;
    QString m_cipher, m_tlsError;
    QTimer m_hsTimer;
    qint64 m_readBufferSize = 0;
};

} // namespace mx::gnut
