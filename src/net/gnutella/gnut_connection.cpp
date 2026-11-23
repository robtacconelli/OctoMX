// SPDX-License-Identifier: GPL-3.0-or-later
#include "gnut_connection.h"

#include "mx/plugin.h"

#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpSocket>

namespace mx::gnut {

static constexpr qint64 kCongested = 256 * 1024;      // drop low-priority output above this
static constexpr qint64 kMaxBacklog = 2 * 1024 * 1024; // close above this
static constexpr int kIdleTimeoutMs = 180000;
static constexpr int kPingIntervalMs = 45000;

UltrapeerConnection::UltrapeerConnection(const HostPort &addr, const HandshakeConfig &cfg, QObject *parent)
    : QObject(parent), m_addr(addr), m_cfg(cfg)
{
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        if (m_state == Connected) {
            if (m_lastRx.isValid() && m_lastRx.elapsed() > kIdleTimeoutMs) fail(QStringLiteral("idle timeout"));
            else m_timeout.start(15000);
        } else {
            fail(m_state == Connecting ? QStringLiteral("connect timeout") : QStringLiteral("handshake timeout"));
        }
    });
    m_pingTimer.setInterval(kPingIntervalMs);
    connect(&m_pingTimer, &QTimer::timeout, this, &UltrapeerConnection::sendPing);
}

UltrapeerConnection::~UltrapeerConnection()
{
    if (m_sock) {
        m_sock->disconnect(this);
        m_sock->abort();
    }
}

void UltrapeerConnection::start()
{
    if (m_cfg.tls && m_cfg.tlsPolicy != TlsSocket::Plain) m_sock = new TlsSocket(m_cfg.tls, m_cfg.tlsPolicy, this);
    else m_sock = new QTcpSocket(this);
    m_sock->setReadBufferSize(256 * 1024);
    connect(m_sock, &QTcpSocket::connected, this, &UltrapeerConnection::onConnected);
    connect(m_sock, &QTcpSocket::readyRead, this, &UltrapeerConnection::onReadyRead);
    connect(m_sock, &QTcpSocket::disconnected, this, &UltrapeerConnection::onDisconnected);
    connect(m_sock, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        if (m_state != Closed) fail(m_sock->errorString());
    });
    m_started.start();
    m_state = Connecting;
    m_timeout.start(m_cfg.handshakeTimeoutMs);
    m_sock->connectToHost(QHostAddress(m_addr.ip), m_addr.port);
}

void UltrapeerConnection::onConnected()
{
    m_state = Handshaking;
    HeaderBlock h;
    h.firstLine = QStringLiteral("GNUTELLA CONNECT/0.6");
    h.add(QStringLiteral("User-Agent"), m_cfg.userAgent);
    h.add(QStringLiteral("X-Ultrapeer"), QStringLiteral("False"));
    h.add(QStringLiteral("X-Query-Routing"), QStringLiteral("0.2"));
    h.add(QStringLiteral("X-Ultrapeer-Query-Routing"), QStringLiteral("0.1"));
    if (m_cfg.acceptDeflate) h.add(QStringLiteral("Accept-Encoding"), QStringLiteral("deflate"));
    h.add(QStringLiteral("Pong-Caching"), QStringLiteral("0.1"));
    h.add(QStringLiteral("GGEP"), QStringLiteral("0.5"));
    h.add(QStringLiteral("Vendor-Message"), QStringLiteral("0.2"));
    h.add(QStringLiteral("X-Dynamic-Querying"), QStringLiteral("0.1"));
    h.add(QStringLiteral("X-Max-TTL"), QStringLiteral("3"));
    h.add(QStringLiteral("X-Ext-Probes"), QStringLiteral("0.1"));
    h.add(QStringLiteral("Bye-Packet"), QStringLiteral("0.1"));
    h.add(QStringLiteral("Remote-IP"), ipToString(m_addr.ip));
    if (m_cfg.listen.port) {
        if (m_cfg.listen.ip) h.add(QStringLiteral("Listen-IP"), m_cfg.listen.toString());
        else h.add(QStringLiteral("X-Listen-Port"), QString::number(m_cfg.listen.port));
    }
    QStringList features;
    if (m_cfg.browseHost) features << QStringLiteral("browse/1.0");
    if (m_cfg.advertiseTls) features << QStringLiteral("tls/1.0");   // [enc-1] we accept TLS on our port
    if (!features.isEmpty()) h.add(QStringLiteral("X-Features"), features.join(QStringLiteral(", ")));
    const QByteArray b = h.serialize();
    m_bytesOut += b.size(); meter(true, b.size());
    m_sock->write(b);
}

void UltrapeerConnection::onReadyRead()
{
    while (m_sock && m_sock->bytesAvailable() > 0 && m_state != Closed) {
        const QByteArray data = m_sock->read(64 * 1024);
        if (data.isEmpty()) break;
        m_bytesIn += data.size(); meter(false, data.size());
        m_lastRx.start();
        if (m_state == Handshaking) {
            m_hdr.feed(data);
            if (m_hdr.error()) { fail(QStringLiteral("bad handshake: ") + m_hdr.errorText()); return; }
            if (!m_hdr.complete()) continue;
            m_response = m_hdr.block();
            m_vendor = m_response.first(QStringLiteral("User-Agent")).left(80);
            if (m_vendor.isEmpty()) m_vendor = m_response.first(QStringLiteral("Server")).left(80);
            m_remoteIp = m_response.first(QStringLiteral("Remote-IP")).trimmed();
            for (const QString &f : m_response.value(QStringLiteral("X-Features")).split(QLatin1Char(','), Qt::SkipEmptyParts))
                if (f.trimmed().startsWith(QLatin1String("tls/"), Qt::CaseInsensitive)) m_remoteTls = true;   // [enc-1]
            QList<HostPort> tryHosts = parseHostList(m_response.value(QStringLiteral("X-Try-Ultrapeers")));
            if (tryHosts.isEmpty()) tryHosts = parseHostList(m_response.value(QStringLiteral("X-Try")));
            const bool isGnutella = m_response.version.startsWith(QLatin1String("GNUTELLA/"), Qt::CaseInsensitive);
            const bool isUltra = m_response.first(QStringLiteral("X-Ultrapeer")).compare(QLatin1String("true"), Qt::CaseInsensitive) == 0;
            if (!isGnutella || m_response.code != 200 || !isUltra) {
                int code = m_response.code;
                QString reason = m_response.reason;
                if (!isGnutella) { code = 0; reason = QStringLiteral("not a Gnutella 0.6 servent"); }
                else if (code == 200 && !isUltra) {
                    code = 0;
                    reason = QStringLiteral("peer is not an ultrapeer");
                    m_sock->write("GNUTELLA/0.6 503 Ultrapeer needed\r\n\r\n");
                    m_sock->flush();
                }
                const int retry = m_response.first(QStringLiteral("Retry-After")).toInt();
                m_state = Closed;
                m_timeout.stop();
                emit handshakeFailed(code, reason, tryHosts, retry);
                if (m_sock) m_sock->disconnectFromHost();
                emit closed(QStringLiteral("handshake rejected: %1 %2").arg(code).arg(reason));
                return;
            }
            // final step of the three-way handshake
            HeaderBlock fin;
            fin.firstLine = QStringLiteral("GNUTELLA/0.6 200 OK");
            const bool remoteAccepts = m_response.value(QStringLiteral("Accept-Encoding")).contains(QLatin1String("deflate"), Qt::CaseInsensitive);
            if (remoteAccepts && m_cfg.acceptDeflate) fin.add(QStringLiteral("Content-Encoding"), QStringLiteral("deflate"));
            const QByteArray fb = fin.serialize();
            m_sock->write(fb);
            m_bytesOut += fb.size(); meter(true, fb.size());
            if (remoteAccepts && m_cfg.acceptDeflate) m_deflater.reset(new Deflater(6));
            if (m_response.value(QStringLiteral("Content-Encoding")).contains(QLatin1String("deflate"), Qt::CaseInsensitive))
                m_inflater.reset(new Inflater);
            m_dynamicQuerying = m_response.has(QStringLiteral("X-Dynamic-Querying"));
            m_state = Connected;
            m_handshakeMs = m_started.elapsed();
            m_connected.start();
            m_timeout.start(15000);
            m_pingTimer.start();
            if (!tryHosts.isEmpty()) emit handshakeFailed(-1, QString(), tryHosts, 0);   // -1: harvest only
            emit handshakeDone();
            const QByteArray rest = m_hdr.takeRemainder();
            if (!rest.isEmpty()) processStream(rest);
            if (!m_out.isEmpty()) flush();
        } else if (m_state == Connected) {
            processStream(data);
        }
    }
}

void UltrapeerConnection::processStream(const QByteArray &data)
{
    if (m_inflater) {
        QByteArray plain;
        if (!m_inflater->feed(data, &plain, 4 * 1024 * 1024)) { fail(QStringLiteral("inflate error")); return; }
        m_framer.feed(plain);
    } else {
        m_framer.feed(data);
    }
    Message m;
    QPointer<UltrapeerConnection> self(this);
    while (m_framer.next(&m)) {
        ++m_msgsIn;
        if (m.type == MsgPong && m.guid == m_pingGuid && m_pingSent.isValid()) {
            m_pingMs = int(m_pingSent.elapsed());
            m_pingGuid.clear();
        }
        emit messageReceived(m);
        if (!self || m_state != Connected) return;
    }
    if (m_framer.error()) fail(m_framer.errorText());
    else if (m_framer.buffered() > MaxPayload + HeaderSize + 16) fail(QStringLiteral("framing overflow"));
}

void UltrapeerConnection::send(const Message &m, bool lowPriority)
{
    if (m_state == Closed || m_closing) return;
    if (lowPriority && m_sock && m_sock->bytesToWrite() + m_out.size() > kCongested) { ++m_dropped; return; }
    m_out.append(m.encode());
    ++m_msgsOut;
    if (m_state != Connected) return;              // flushed after the handshake
    if (!m_flushScheduled) {
        m_flushScheduled = true;
        QTimer::singleShot(0, this, [this] { m_flushScheduled = false; flush(); });
    }
}

void UltrapeerConnection::flush()
{
    if (!m_sock || m_state != Connected || m_out.isEmpty()) return;
    const QByteArray data = m_deflater ? m_deflater->compress(m_out) : m_out;
    m_out.clear();
    m_bytesOut += data.size(); meter(true, data.size());
    m_sock->write(data);
    if (m_sock->bytesToWrite() > kMaxBacklog) fail(QStringLiteral("output backlog"));
}

void UltrapeerConnection::sendPing()
{
    if (m_state != Connected) return;
    Message p = Message::make(MsgPing, QByteArray(), 1);
    m_pingGuid = p.guid;
    m_pingSent.start();
    send(p);
}

bool UltrapeerConnection::supportsVendor(const QByteArray &vendor, quint16 id, quint16 *version) const
{
    for (const VendorKey &k : m_vendorMsgs)
        if (k.vendor == vendor && k.id == id) { if (version) *version = k.version; return true; }
    return false;
}

void UltrapeerConnection::close(const QString &reason, quint16 byeCode)
{
    if (m_state == Closed) return;
    if (byeCode && m_state == Connected) {
        send(Message::make(MsgBye, buildByePayload(byeCode, reason), 1));
        flush();
        m_closing = true;
    }
    m_state = Closed;
    m_timeout.stop();
    m_pingTimer.stop();
    if (m_sock) m_sock->disconnectFromHost();
    emit closed(reason);
}

void UltrapeerConnection::fail(const QString &reason)
{
    if (m_state == Closed) return;
    const bool wasHandshake = m_state != Connected;
    m_state = Closed;
    m_timeout.stop();
    m_pingTimer.stop();
    if (wasHandshake) emit handshakeFailed(0, reason, {}, 0);
    if (m_sock) m_sock->abort();
    emit closed(reason);
}

void UltrapeerConnection::onDisconnected()
{
    if (m_state == Closed) return;
    fail(m_state == Connected ? QStringLiteral("connection closed by peer") : QStringLiteral("closed during handshake"));
}

bool UltrapeerConnection::encrypted() const
{
    auto *t = qobject_cast<TlsSocket *>(m_sock);
    return t && t->isEncrypted();
}

bool UltrapeerConnection::tlsTried() const { return qobject_cast<TlsSocket *>(m_sock) != nullptr; }

bool UltrapeerConnection::tlsFellBack() const
{
    auto *t = qobject_cast<TlsSocket *>(m_sock);
    return t && t->fellBack();
}

QString UltrapeerConnection::tlsInfo() const
{
    auto *t = qobject_cast<TlsSocket *>(m_sock);
    if (!t) return QString();
    return t->isEncrypted() ? t->cipherName() : t->tlsError();
}

void UltrapeerConnection::meter(bool up, qint64 bytes)
{
    if (m_cfg.meter && bytes > 0)
        m_cfg.meter->accountTraffic(up ? mx::RateLimiter::Up : mx::RateLimiter::Down, mx::Net::Gnutella, bytes, 0);
}

} // namespace mx::gnut
