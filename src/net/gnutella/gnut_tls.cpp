// SPDX-License-Identifier: GPL-3.0-or-later
#include "gnut_tls.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QPointer>
#include <QtCore/QRandomGenerator>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

namespace mx::gnut {

static int s_handshakeTimeoutMs = 12000;

TlsMode tlsModeFromSetting(const QString &v)
{
    if (v == QLatin1String("forced")) return TlsMode::Require;
    if (v == QLatin1String("disabled")) return TlsMode::Off;
    return TlsMode::Prefer;
}

QString tlsModeName(TlsMode m)
{
    switch (m) {
    case TlsMode::Require: return QStringLiteral("required");
    case TlsMode::Off: return QStringLiteral("disabled");
    default: return QStringLiteral("preferred");
    }
}

bool looksLikeTlsClientHello(const QByteArray &b)
{
    // record type 22 (handshake), version 3.x; a ClientHello (type 1) follows the 5-byte header
    if (b.size() < 1 || quint8(b[0]) != 0x16) return false;
    if (b.size() >= 2 && quint8(b[1]) != 0x03) return false;
    if (b.size() >= 6 && quint8(b[5]) != 0x01) return false;
    return true;
}

static QString sslErrorText(int sslErr)
{
    char buf[256];
    const unsigned long e = ERR_get_error();
    ERR_clear_error();
    if (e) {
        ERR_error_string_n(e, buf, sizeof buf);
        return QString::fromLatin1(buf);
    }
    if (sslErr == SSL_ERROR_SYSCALL) return QStringLiteral("connection error");
    return QStringLiteral("TLS error %1").arg(sslErr);
}

// ---------------------------------------------------------------- engine
static QByteArray makeSelfSigned(QString *error)
{
    QByteArray pem;
    EVP_PKEY *pkey = EVP_RSA_gen(2048);
    X509 *x = X509_new();
    bool ok = pkey && x;
    if (ok) {
        X509_set_version(x, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(x), long(QRandomGenerator::global()->bounded(1, 0x7fffffff)));
        X509_gmtime_adj(X509_getm_notBefore(x), -86400L);
        X509_gmtime_adj(X509_getm_notAfter(x), 20L * 365 * 86400);
        X509_set_pubkey(x, pkey);
        X509_NAME *n = X509_get_subject_name(x);
        X509_NAME_add_entry_by_txt(n, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char *>("OctoMX Gnutella"), -1, -1, 0);
        X509_set_issuer_name(x, n);
        ok = X509_sign(x, pkey, EVP_sha256()) > 0;
    }
    if (ok) {
        BIO *b = BIO_new(BIO_s_mem());
        ok = PEM_write_bio_PrivateKey(b, pkey, nullptr, nullptr, 0, nullptr, nullptr) && PEM_write_bio_X509(b, x);
        char *p = nullptr;
        const long len = BIO_get_mem_data(b, &p);
        if (ok && len > 0) pem = QByteArray(p, int(len));
        BIO_free(b);
    }
    if (!ok && error) *error = QStringLiteral("cannot create the TLS certificate: ") + sslErrorText(0);
    X509_free(x);
    EVP_PKEY_free(pkey);
    return pem;
}

static bool usePem(SSL_CTX *ctx, const QByteArray &pem, QString *info)
{
    BIO *b = BIO_new_mem_buf(pem.constData(), int(pem.size()));
    EVP_PKEY *k = PEM_read_bio_PrivateKey(b, nullptr, nullptr, nullptr);
    X509 *x = PEM_read_bio_X509(b, nullptr, nullptr, nullptr);
    BIO_free(b);
    bool ok = k && x && SSL_CTX_use_certificate(ctx, x) == 1 && SSL_CTX_use_PrivateKey(ctx, k) == 1 &&
              SSL_CTX_check_private_key(ctx) == 1;
    if (ok && info) {
        char buf[64] = {0};
        BIO *mb = BIO_new(BIO_s_mem());
        ASN1_TIME_print(mb, X509_get0_notBefore(x));
        const int n = BIO_read(mb, buf, sizeof buf - 1);
        BIO_free(mb);
        *info = QStringLiteral("self-signed, valid from %1").arg(QString::fromLatin1(buf, qMax(0, n)));
    }
    X509_free(x);
    EVP_PKEY_free(k);
    ERR_clear_error();
    return ok;
}

static void commonSetup(SSL_CTX *c)
{
    // old LimeWire-family peers speak TLS 1.0 with anonymous DH only: security level 0 allows it.
    // Authenticated (EC)DHE suites come first, anonymous ones last.
    SSL_CTX_set_min_proto_version(c, TLS1_VERSION);
    SSL_CTX_set_security_level(c, 0);
    SSL_CTX_set_cipher_list(c, "HIGH:!eNULL:!MD5:!3DES:!RC4:!PSK:!SRP:!DSS:+kRSA:+aNULL:@SECLEVEL=0");
    SSL_CTX_set_options(c, SSL_OP_IGNORE_UNEXPECTED_EOF | SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
    SSL_CTX_set_verify(c, SSL_VERIFY_NONE, nullptr);   // Gnutella peers have no verifiable identity
    SSL_CTX_set_session_cache_mode(c, SSL_SESS_CACHE_OFF);
    SSL_CTX_set_mode(c, SSL_MODE_RELEASE_BUFFERS);
}

std::shared_ptr<TlsEngine> TlsEngine::create(const QString &pemPath, QString *error)
{
    std::shared_ptr<TlsEngine> e(new TlsEngine);
    e->m_client = SSL_CTX_new(TLS_client_method());
    e->m_server = SSL_CTX_new(TLS_server_method());
    if (!e->m_client || !e->m_server) {
        if (error) *error = QStringLiteral("OpenSSL: ") + sslErrorText(0);
        return nullptr;
    }
    commonSetup(e->m_client);
    commonSetup(e->m_server);
    SSL_CTX_set_dh_auto(e->m_server, 1);
    SSL_CTX_set_options(e->m_server, SSL_OP_CIPHER_SERVER_PREFERENCE);
    SSL_CTX_set_num_tickets(e->m_server, 0);
    QByteArray pem;
    QFile f(pemPath);
    if (!pemPath.isEmpty() && f.open(QIODevice::ReadOnly)) { pem = f.read(64 * 1024); f.close(); }
    if (pem.isEmpty() || !usePem(e->m_server, pem, &e->m_certInfo)) {
        pem = makeSelfSigned(error);
        if (pem.isEmpty() || !usePem(e->m_server, pem, &e->m_certInfo)) return nullptr;
        e->m_createdNow = true;
        if (!pemPath.isEmpty()) {
            QDir().mkpath(QFileInfo(pemPath).absolutePath());
            QFile out(pemPath);
            if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                out.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
                out.write(pem);
            }
        }
    }
    return e;
}

TlsEngine::~TlsEngine()
{
    SSL_CTX_free(m_client);
    SSL_CTX_free(m_server);
}

// ---------------------------------------------------------------- socket
void TlsSocket::setHandshakeTimeoutMs(int ms) { s_handshakeTimeoutMs = ms; }

TlsSocket::TlsSocket(std::shared_ptr<TlsEngine> engine, Policy policy, QObject *parent)
    : QTcpSocket(parent), m_engine(std::move(engine)), m_policy(m_engine ? policy : Plain)
{
    m_hsTimer.setSingleShot(true);
    connect(&m_hsTimer, &QTimer::timeout, this, [this] { handshakeFailed(QStringLiteral("TLS handshake timeout")); });
}

TlsSocket::~TlsSocket()
{
    m_hsTimer.stop();
    if (m_inner) {
        m_inner->disconnect(this);
        m_inner->abort();
        delete m_inner;
        m_inner = nullptr;
    }
    freeSsl();
    setSocketState(UnconnectedState);   // the base destructor must not run its own abort path
}

void TlsSocket::freeSsl()
{
    if (m_ssl) SSL_free(m_ssl);       // frees both BIOs
    m_ssl = nullptr;
    m_rbio = m_wbio = nullptr;
}

void TlsSocket::attachInner(QTcpSocket *s)
{
    m_inner = s;
    s->setParent(this);
    if (m_readBufferSize) s->setReadBufferSize(m_readBufferSize);
    connect(s, &QTcpSocket::connected, this, &TlsSocket::onInnerConnected);
    connect(s, &QTcpSocket::readyRead, this, &TlsSocket::onInnerReadyRead);
    connect(s, &QTcpSocket::bytesWritten, this, [this](qint64 n) { if (m_connectedEmitted && !m_gone) emit bytesWritten(n); });
    connect(s, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError e) { onInnerGone(e, true); });
    connect(s, &QTcpSocket::disconnected, this, [this] { onInnerGone(QAbstractSocket::RemoteHostClosedError, false); });
}

void TlsSocket::makeInner()
{
    auto *s = new QTcpSocket(this);
    if (m_prepare) m_prepare(s);   // [net-2]
    attachInner(s);
}

void TlsSocket::connectToHost(const QString &hostName, quint16 port, OpenMode mode, NetworkLayerProtocol)
{
    if (m_inner) return;
    m_host = hostName;
    m_port = port;
    m_mode = mode;
    if (!isOpen()) QIODevice::open(mode);
    setPeerName(hostName);
    setSocketState(ConnectingState);
    emit stateChanged(ConnectingState);
    makeInner();
    m_inner->connectToHost(hostName, port, mode);
}

void TlsSocket::startServer(QTcpSocket *plain, const QByteArray &alreadyRead)
{
    m_server = true;
    m_policy = TlsOnly;
    plain->disconnect(this);
    attachInner(plain);
    m_tcpUp = true;
    setPeerAddress(plain->peerAddress());
    setPeerPort(plain->peerPort());
    setLocalAddress(plain->localAddress());
    setLocalPort(plain->localPort());
    if (!isOpen()) QIODevice::open(ReadWrite);
    setSocketState(ConnectingState);
    if (!m_engine) { handshakeFailed(QStringLiteral("TLS not available")); return; }
    startTls(true);
    QPointer<TlsSocket> self(this);
    pump(alreadyRead);
    if (self && m_inner && m_inner->bytesAvailable() > 0) onInnerReadyRead();
}

void TlsSocket::onInnerConnected()
{
    m_tcpUp = true;
    setPeerAddress(m_inner->peerAddress());
    setPeerPort(m_inner->peerPort());
    setLocalAddress(m_inner->localAddress());
    setLocalPort(m_inner->localPort());
    if (m_policy == Plain || m_fellBack) { becomeConnected(); return; }
    startTls(false);
}

void TlsSocket::startTls(bool server)
{
    ERR_clear_error();
    m_ssl = SSL_new(server ? m_engine->serverCtx() : m_engine->clientCtx());
    m_rbio = BIO_new(BIO_s_mem());
    m_wbio = BIO_new(BIO_s_mem());
    SSL_set_bio(m_ssl, m_rbio, m_wbio);
    if (server) SSL_set_accept_state(m_ssl);
    else { SSL_set_connect_state(m_ssl); ++m_engine->stats.outAttempts; }
    m_tlsStarted = true;
    m_hsTimer.start(s_handshakeTimeoutMs);
    if (!server) pump(QByteArray());        // produces the ClientHello
}

void TlsSocket::flushOut()
{
    if (!m_wbio || !m_inner) return;
    while (const size_t pending = BIO_ctrl_pending(m_wbio)) {
        QByteArray b(int(qMin<size_t>(pending, 256 * 1024)), Qt::Uninitialized);
        const int n = BIO_read(m_wbio, b.data(), int(b.size()));
        if (n <= 0) break;
        m_inner->write(b.constData(), n);
    }
}

void TlsSocket::pump(const QByteArray &in)
{
    if (!m_ssl) return;
    ERR_clear_error();
    if (!in.isEmpty()) BIO_write(m_rbio, in.constData(), int(in.size()));
    if (!m_tlsUp) {
        const int r = SSL_do_handshake(m_ssl);
        if (r != 1) {
            const int e = SSL_get_error(m_ssl, r);
            flushOut();                                   // alerts, or the next handshake flight
            if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) return;
            handshakeFailed(sslErrorText(e));
            return;
        }
        m_tlsUp = true;
        m_hsTimer.stop();
        m_cipher = QString::fromLatin1(SSL_get_version(m_ssl)) + QLatin1Char(' ') + QString::fromLatin1(SSL_get_cipher_name(m_ssl));
        if (m_server) ++m_engine->stats.inOk; else ++m_engine->stats.outOk;
        flushOut();
        QPointer<TlsSocket> self(this);
        becomeConnected();
        if (!self || m_gone) return;
        emit encrypted();
        if (!self || m_gone || !m_ssl) return;
    }
    QByteArray got;
    char buf[16384];
    bool fatal = false;
    for (;;) {
        const int n = SSL_read(m_ssl, buf, int(sizeof buf));
        if (n > 0) { got.append(buf, n); continue; }
        const int e = SSL_get_error(m_ssl, n);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE || e == SSL_ERROR_ZERO_RETURN) break;
        m_tlsError = sslErrorText(e);
        fatal = true;
        break;
    }
    flushOut();
    QPointer<TlsSocket> self(this);
    if (!got.isEmpty()) {
        m_plain.append(got);
        emit readyRead();
        if (!self) return;
    }
    if (fatal && m_inner && !m_gone) {
        setErrorString(QStringLiteral("TLS error: ") + m_tlsError);
        m_inner->disconnect(this);
        m_inner->abort();
        onInnerGone(QAbstractSocket::SslInternalError, true);
    }
}

void TlsSocket::onInnerReadyRead()
{
    if (!m_inner || m_gone) return;
    const QByteArray in = m_inner->readAll();
    if (in.isEmpty()) return;
    if (m_tlsStarted) { pump(in); return; }
    if (!m_connectedEmitted) return;   // nothing is expected before connected()
    m_plain.append(in);
    emit readyRead();
}

void TlsSocket::becomeConnected()
{
    setSocketState(ConnectedState);
    m_connectedEmitted = true;
    QPointer<TlsSocket> self(this);
    emit stateChanged(ConnectedState);
    if (!self) return;
    if (!m_pendingOut.isEmpty()) {
        const QByteArray out = m_pendingOut;
        m_pendingOut.clear();
        writeData(out.constData(), out.size());
    }
    if (!m_server) emit connected();
}

void TlsSocket::handshakeFailed(const QString &why)
{
    if (m_gone) return;
    m_hsTimer.stop();
    m_tlsError = why;
    QPointer<TlsSocket> self(this);
    emit tlsFailed(why);
    if (!self) return;
    freeSsl();
    m_tlsStarted = false;
    if (!m_server && m_policy == TryTls && m_tcpUp && !m_fellBack) {
        // prefer mode: the peer does not speak TLS (or broke the handshake): reconnect plain
        ++m_engine->stats.outFallback;
        m_fellBack = true;
        m_tcpUp = false;
        QTcpSocket *old = m_inner;
        m_inner = nullptr;
        old->disconnect(this);
        old->abort();
        old->deleteLater();
        makeInner();
        m_inner->connectToHost(m_host, m_port, m_mode);
        return;
    }
    if (m_server) ++m_engine->stats.inFailed; else ++m_engine->stats.outFailed;
    m_gone = true;
    if (m_inner) { m_inner->disconnect(this); m_inner->abort(); }
    setSocketError(SslHandshakeFailedError);
    setErrorString(QStringLiteral("TLS handshake failed: ") + why);
    setSocketState(UnconnectedState);
    emit stateChanged(UnconnectedState);
    if (!self) return;
    emit errorOccurred(SslHandshakeFailedError);
}

void TlsSocket::onInnerGone(QAbstractSocket::SocketError e, bool isError)
{
    if (m_gone || !m_inner) return;
    QPointer<TlsSocket> self(this);
    if (m_inner->bytesAvailable() > 0) {
        onInnerReadyRead();
        if (!self || m_gone) return;
    }
    if (m_tlsStarted && !m_tlsUp) {
        if (m_tcpUp) { handshakeFailed(QStringLiteral("connection closed during the TLS handshake")); return; }
    }
    m_gone = true;
    m_hsTimer.stop();
    const bool wasConnected = m_connectedEmitted;
    if (isError) {
        setSocketError(e);
        if (e != QAbstractSocket::SslInternalError) setErrorString(m_inner->errorString());
    }
    m_inner->disconnect(this);
    if (isError) {
        if (!wasConnected) { setSocketState(UnconnectedState); emit stateChanged(UnconnectedState); }
        if (!self) return;
        emit errorOccurred(e);
        if (!self) return;
    }
    if (wasConnected) finishUnconnected(true);
}

void TlsSocket::finishUnconnected(bool emitDisconnected)
{
    if (state() == UnconnectedState) return;
    setSocketState(UnconnectedState);
    QPointer<TlsSocket> self(this);
    emit stateChanged(UnconnectedState);
    if (!self) return;
    emit readChannelFinished();
    if (!self) return;
    if (emitDisconnected) emit disconnected();
}

qint64 TlsSocket::readData(char *data, qint64 maxlen)
{
    const qint64 n = qMin<qint64>(maxlen, m_plain.size());
    if (n <= 0) return (m_gone && state() == UnconnectedState) ? -1 : 0;
    memcpy(data, m_plain.constData(), size_t(n));
    m_plain.remove(0, int(n));
    return n;
}

qint64 TlsSocket::writeData(const char *data, qint64 len)
{
    if (m_gone || !m_inner) return -1;
    if (!m_connectedEmitted) { m_pendingOut.append(data, int(len)); return len; }
    if (m_tlsUp && m_ssl) {
        ERR_clear_error();
        qint64 done = 0;
        while (done < len) {
            const int chunk = int(qMin<qint64>(len - done, 64 * 1024));
            const int n = SSL_write(m_ssl, data + done, chunk);
            if (n <= 0) break;
            done += n;
        }
        flushOut();
        return len;
    }
    return m_inner->write(data, len);
}

qint64 TlsSocket::bytesAvailable() const { return m_plain.size() + QIODevice::bytesAvailable(); }

qint64 TlsSocket::bytesToWrite() const { return (m_inner ? m_inner->bytesToWrite() : 0) + m_pendingOut.size(); }

bool TlsSocket::canReadLine() const { return m_plain.contains('\n') || QIODevice::canReadLine(); }

bool TlsSocket::atEnd() const { return m_plain.isEmpty() && QIODevice::atEnd(); }

void TlsSocket::setReadBufferSize(qint64 size)
{
    m_readBufferSize = size;
    if (m_inner) m_inner->setReadBufferSize(size);
}

void TlsSocket::disconnectFromHost()
{
    if (m_gone || !m_inner || !m_connectedEmitted) { close(); return; }
    if (m_tlsUp && m_ssl) { ERR_clear_error(); SSL_shutdown(m_ssl); flushOut(); }
    setSocketState(ClosingState);
    emit stateChanged(ClosingState);
    m_inner->disconnectFromHost();
    if (m_inner && m_inner->state() == UnconnectedState && !m_gone) {
        m_gone = true;
        m_inner->disconnect(this);
        finishUnconnected(true);
    }
}

void TlsSocket::close()
{
    const bool wasConnected = m_connectedEmitted && state() != UnconnectedState;
    m_hsTimer.stop();
    m_gone = true;
    if (m_inner) { m_inner->disconnect(this); m_inner->abort(); }
    m_plain.clear();
    m_pendingOut.clear();
    QIODevice::close();
    if (state() != UnconnectedState) {
        setSocketState(UnconnectedState);
        QPointer<TlsSocket> self(this);
        emit stateChanged(UnconnectedState);
        if (!self) return;
        if (wasConnected) emit disconnected();
    }
}

} // namespace mx::gnut
