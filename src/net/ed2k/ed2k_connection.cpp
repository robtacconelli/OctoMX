// SPDX-License-Identifier: GPL-3.0-or-later
#include "ed2k_connection.h"
#include "ed2k_io.h"
#include "ed2k_util.h"

#include "mx/plugin.h"

#include <QtNetwork/QHostAddress>

namespace mx::ed2k {

static constexpr qint64 kMaxDataQueue = 3 * 1024 * 1024;
static constexpr qint64 kMaxSocketBacklog = 256 * 1024;

Ed2kConnection::Ed2kConnection(QObject *parent) : QObject(parent)
{
    m_sock = new QTcpSocket(this);
    init();
}

Ed2kConnection::Ed2kConnection(QTcpSocket *accepted, QObject *parent) : QObject(parent)
{
    m_sock = accepted;
    m_sock->setParent(this);
    m_incoming = true;
    m_state = Open;
    m_peerIpCached = ipFromHostOrder(m_sock->peerAddress().toIPv4Address());
    m_peerPort = m_sock->peerPort();
    init();
    m_activity.start();
}

Ed2kConnection::~Ed2kConnection()
{
    if (m_sock) {
        m_sock->disconnect(this);
        m_sock->abort();
    }
}

void Ed2kConnection::init()
{
    m_sock->setReadBufferSize(256 * 1024);
    m_sock->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
    connect(m_sock, &QTcpSocket::readyRead, this, &Ed2kConnection::onReadyRead);
    connect(m_sock, &QTcpSocket::bytesWritten, this, &Ed2kConnection::onBytesWritten);
    connect(m_sock, &QTcpSocket::errorOccurred, this, &Ed2kConnection::onSocketError);
    connect(m_sock, &QTcpSocket::disconnected, this, [this] { finish(QStringLiteral("connection closed by peer")); });
    connect(m_sock, &QTcpSocket::connected, this, [this] {
        m_state = Open;
        m_activity.start();
        m_peerIpCached = ipFromHostOrder(m_sock->peerAddress().toIPv4Address());
        flushCryptOutput();
        emit connected();
    });
    m_tick.setInterval(2000);
    connect(&m_tick, &QTimer::timeout, this, &Ed2kConnection::onTick);
    m_tick.start();
    m_pumpRetry.setSingleShot(true);
    m_pumpRetry.setInterval(40);
    connect(&m_pumpRetry, &QTimer::timeout, this, &Ed2kConnection::pump);
    m_readRetry.setSingleShot(true);
    m_readRetry.setInterval(50);
    connect(&m_readRetry, &QTimer::timeout, this, &Ed2kConnection::onReadyRead);
}

void Ed2kConnection::setMaxPacket(quint32 maxPacket) { m_parser = PacketParser(maxPacket, maxPacket * 2); }

void Ed2kConnection::useClientObfuscation(const QByteArray &target) { m_crypt.startClient(target, 128); }
void Ed2kConnection::useServerObfuscation() { m_crypt.startServer(); }
void Ed2kConnection::useIncomingDetection(const QByteArray &ourHash, bool require)
{
    m_crypt.startIncoming(ourHash, require, 128);
}

void Ed2kConnection::connectTo(quint32 ip, quint16 port)
{
    m_state = Connecting;
    m_peerIpCached = ip;
    m_peerPort = port;
    m_connectClock.start();
    m_sock->connectToHost(QHostAddress(ipToHostOrder(ip)), port);
}

void Ed2kConnection::connectToHost(const QString &host, quint16 port)
{
    m_state = Connecting;
    m_peerPort = port;
    m_connectClock.start();
    m_sock->connectToHost(host, port, QIODevice::ReadWrite, QAbstractSocket::IPv4Protocol);
}

quint32 Ed2kConnection::peerIp() const { return m_peerIpCached; }
QString Ed2kConnection::peerAddress() const { return QStringLiteral("%1:%2").arg(ipToString(m_peerIpCached)).arg(m_peerPort); }

void Ed2kConnection::flushCryptOutput()
{
    if (!m_sock || m_sock->state() != QAbstractSocket::ConnectedState) return;
    if (m_crypt.hasOutput()) {
        const QByteArray o = m_crypt.takeOutput();
        m_bytesOut += o.size();
        m_sock->write(o);
    }
    meterOut(0);
}

void Ed2kConnection::meterOut(qint64 payloadBytes)
{
    mx::RateLimiter *m = m_meter ? m_meter : m_limiter;
    const qint64 wire = m_bytesOut - m_bytesOutMetered;
    m_bytesOutMetered = m_bytesOut;
    if (!m || (wire <= 0 && payloadBytes <= 0)) return;
    if (exemptPeer()) m->accountExempt(mx::RateLimiter::Up, mx::Net::ED2K, wire, payloadBytes);   // [bw-1] LAN peer
    else m->accountTraffic(mx::RateLimiter::Up, mx::Net::ED2K, wire, payloadBytes);
}

bool Ed2kConnection::exemptPeer() const
{
    // [bw-1] LAN peers are exempt from the limits unless bandwidth/limitLan (asked each time: the
    // setting applies at once)
    mx::RateLimiter *m = m_meter ? m_meter : m_limiter;
    return m && m_sock && m_state == Open && m->exempt(m_sock->peerAddress());
}

void Ed2kConnection::writeWire(const QByteArray &plain, qint64 payloadBytes)
{
    if (m_state == Closed) return;
    const QByteArray w = m_crypt.send(plain);
    if (m_sock->state() == QAbstractSocket::ConnectedState) {
        flushCryptOutput();
        if (!w.isEmpty()) { m_bytesOut += w.size(); m_sock->write(w); }
    } else if (!w.isEmpty()) {
        // not connected yet and not obfuscating: hold in crypt-less pending by writing later
        // (QTcpSocket buffers writes issued while connecting)
        m_bytesOut += w.size();
        m_sock->write(w);
    }
    meterOut(payloadBytes);
}

void Ed2kConnection::sendPacket(quint8 proto, quint8 opcode, const QByteArray &payload, bool allowPacked)
{
    if (m_state == Closed) return;
    sendRaw(allowPacked ? encodePacketMaybePacked(proto, opcode, payload) : encodePacket(proto, opcode, payload));
}

void Ed2kConnection::sendRaw(const QByteArray &framed)
{
    if (m_state == Closed) return;
    if (m_sock->bytesToWrite() > 4 * 1024 * 1024) { close(QStringLiteral("send buffer overflow")); return; }
    writeWire(framed);
}

void Ed2kConnection::queueData(const QByteArray &framed, qint64 payloadBytes)
{
    if (m_state == Closed) return;
    if (m_dataQueued + framed.size() > kMaxDataQueue) { close(QStringLiteral("data queue overflow")); return; }
    m_dataQ.enqueue({framed, payloadBytes});
    m_dataQueued += framed.size();
    pump();
}

void Ed2kConnection::clearDataQueue()
{
    m_dataQ.clear();
    m_dataQueued = 0;
}

void Ed2kConnection::pump()
{
    if (m_state != Open || !m_crypt.ready()) return;
    while (!m_dataQ.isEmpty() && m_sock->bytesToWrite() < kMaxSocketBacklog) {
        const qint64 need = m_dataQ.head().first.size();
        if (m_limiter && m_upCredit < need && !exemptPeer()) {
            m_upCredit += m_limiter->acquire(mx::RateLimiter::Up, need - m_upCredit);
            if (m_upCredit < need) {
                if (!m_pumpRetry.isActive()) m_pumpRetry.start();   // limiter is empty: try again shortly
                break;
            }
        }
        auto item = m_dataQ.dequeue();
        m_dataQueued -= item.first.size();
        if (m_limiter) m_upCredit = qMax<qint64>(0, m_upCredit - need);
        writeWire(item.first, item.second);   // [glue-3] accounted (wire + payload) in writeWire
        m_activity.restart();
        emit dataSent(item.second);
        if (m_state == Closed) return;
    }
}

void Ed2kConnection::onBytesWritten(qint64)
{
    if (!m_dataQ.isEmpty()) pump();
}

void Ed2kConnection::onReadyRead()
{
    if (m_state == Closed || !m_sock) return;
    QPointer<Ed2kConnection> self(this);
    qint64 avail = m_sock->bytesAvailable();
    if (avail <= 0) return;
    qint64 grant = avail;
    const bool exempt = exemptPeer();   // [bw-1]
    if (m_limiter && !exempt) {
        grant = m_limiter->acquire(mx::RateLimiter::Down, avail);
        if (grant <= 0) { m_readRetry.start(); return; }
    }
    const QByteArray raw = m_sock->read(grant);
    mx::RateLimiter *meter = m_meter ? m_meter : m_limiter;   // [glue-3] wire bytes; payload below
    if (meter && exempt) meter->accountExempt(mx::RateLimiter::Down, mx::Net::ED2K, raw.size(), 0);
    else if (meter) meter->accountTraffic(mx::RateLimiter::Down, mx::Net::ED2K, raw.size(), 0);
    m_bytesIn += raw.size();
    m_activity.restart();
    const QByteArray plain = m_crypt.receive(raw);
    if (m_crypt.failed()) { close(QStringLiteral("obfuscation: ") + m_crypt.error()); return; }
    flushCryptOutput();
    if (!m_dataQ.isEmpty()) pump();
    if (!plain.isEmpty()) m_parser.feed(plain);
    Packet p;
    while (self && m_state != Closed && m_parser.next(&p)) {
        if (meter) {   // [glue-3] file data part of what was accounted above (header: hash + offsets)
            qint64 head = -1;
            if (p.proto == 0xE3 && p.opcode == 0x46) head = 24;          // OP_SENDINGPART
            else if (p.proto == 0xC5 && p.opcode == 0xA2) head = 32;     // OP_SENDINGPART_I64
            else if (p.proto == 0xC5 && p.opcode == 0x40) head = 24;     // OP_COMPRESSEDPART
            else if (p.proto == 0xC5 && p.opcode == 0xA1) head = 28;     // OP_COMPRESSEDPART_I64
            if (head >= 0 && p.payload.size() > head)
                meter->accountTraffic(mx::RateLimiter::Down, mx::Net::ED2K, 0, p.payload.size() - head);
        }
        emit packetReceived(p);
    }
    if (!self || m_state == Closed) return;
    if (m_parser.error()) { close(QStringLiteral("protocol error: ") + m_parser.errorString()); return; }
    if (m_sock->bytesAvailable() > 0) m_readRetry.start();
}

void Ed2kConnection::onSocketError()
{
    if (m_state == Closed) return;
    finish(m_sock->errorString());
}

void Ed2kConnection::onTick()
{
    if (m_state == Connecting && m_connectClock.isValid() && m_connectClock.elapsed() > m_connectTimeoutMs) {
        close(QStringLiteral("connect timeout"));
        return;
    }
    if (m_state == Open && m_idleTimeoutMs > 0 && m_activity.isValid() && m_activity.elapsed() > m_idleTimeoutMs) {
        close(QStringLiteral("idle timeout"));
        return;
    }
    if (m_state == Open && !m_dataQ.isEmpty()) pump();
}

void Ed2kConnection::close(const QString &reason)
{
    if (m_state == Closed) return;
    finish(reason, /*graceful=*/true);
}

void Ed2kConnection::finish(const QString &reason, bool graceful)
{
    if (m_state == Closed) return;
    m_state = Closed;
    m_tick.stop();
    m_readRetry.stop();
    m_pumpRetry.stop();
    clearDataQueue();
    if (m_sock) {
        m_sock->disconnect(this);
        if (graceful && m_sock->state() == QAbstractSocket::ConnectedState) {
            // let already written control packets (e.g. OP_OUTOFPARTREQS) drain; the socket is
            // detached so it can outlive us for a few seconds
            QTcpSocket *s = m_sock;
            m_sock = nullptr;
            s->setParent(nullptr);
            connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
            QTimer::singleShot(5000, s, [s] { s->abort(); s->deleteLater(); });
            s->disconnectFromHost();
        } else {
            m_sock->abort();
        }
    }
    if (!m_closeEmitted) {
        m_closeEmitted = true;
        // queued so that handlers may safely delete us
        QMetaObject::invokeMethod(this, [this, reason] { emit closed(reason); }, Qt::QueuedConnection);
    }
}

} // namespace mx::ed2k
