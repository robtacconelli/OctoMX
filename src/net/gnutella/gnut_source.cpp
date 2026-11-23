// SPDX-License-Identifier: GPL-3.0-or-later
#include "gnut_source.h"
#include "gnut_huge.h"
#include "gnut_tls.h"

#include "mxhash/encoding.h"

#include <QtCore/QDateTime>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpSocket>

namespace mx::gnut {

// ---------------------------------------------------------------- descriptor
QString SourceDesc::toUri() const
{
    QUrl u;
    u.setScheme(QStringLiteral("gnutella"));
    u.setHost(ipToString(addr.ip));
    u.setPort(addr.port);
    u.setPath(QStringLiteral("/%1").arg(index));
    QUrlQuery q;
    if (guid.size() == 16) q.addQueryItem(QStringLiteral("guid"), guidHex(guid));
    if (firewalled) q.addQueryItem(QStringLiteral("fw"), QStringLiteral("1"));
    if (browse) q.addQueryItem(QStringLiteral("bh"), QStringLiteral("1"));
    if (!proxies.isEmpty()) {
        QStringList p;
        for (const HostPort &h : proxies) p << h.toString();
        q.addQueryItem(QStringLiteral("pp"), p.join(QLatin1Char(',')));
    }
    if (tls) q.addQueryItem(QStringLiteral("tls"), QStringLiteral("1"));
    if (!vendor.isEmpty()) q.addQueryItem(QStringLiteral("v"), QString::fromLatin1(QUrl::toPercentEncoding(vendor)));
    if (sha1.size() == 20) q.addQueryItem(QStringLiteral("urn"), sha1Urn(sha1));
    if (!name.isEmpty()) q.addQueryItem(QStringLiteral("name"), QString::fromLatin1(QUrl::toPercentEncoding(name)));
    u.setQuery(q);
    return u.toString(QUrl::FullyEncoded);
}

bool SourceDesc::fromUri(const QString &uri, SourceDesc *out)
{
    if (uri.size() > 4096) return false;
    const QUrl u(uri, QUrl::StrictMode);
    if (!u.isValid() || u.scheme() != QLatin1String("gnutella")) return false;
    SourceDesc d;
    bool ok = false;
    d.addr.ip = ipFromString(u.host(), &ok);
    if (!ok) d.addr.ip = 0;
    d.addr.port = quint16(qBound(0, u.port(6346), 65535));
    d.index = u.path().mid(1).toUInt();
    const QUrlQuery q(u);
    d.guid = guidFromHex(q.queryItemValue(QStringLiteral("guid")));
    d.firewalled = q.queryItemValue(QStringLiteral("fw")) == QLatin1String("1");
    d.browse = q.queryItemValue(QStringLiteral("bh")) == QLatin1String("1");
    d.tls = q.queryItemValue(QStringLiteral("tls")) == QLatin1String("1");
    for (const QString &p : q.queryItemValue(QStringLiteral("pp")).split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const HostPort h = HostPort::parse(p, 0);
        if (h.isValid() && d.proxies.size() < 8) d.proxies << h;
    }
    d.vendor = QUrl::fromPercentEncoding(q.queryItemValue(QStringLiteral("v"), QUrl::FullyEncoded).toLatin1()).left(80);
    FileHashes h;
    if (parseUrn(q.queryItemValue(QStringLiteral("urn")), &h)) d.sha1 = h.sha1;
    d.name = QUrl::fromPercentEncoding(q.queryItemValue(QStringLiteral("name"), QUrl::FullyEncoded).toLatin1()).left(1024);
    if (!d.addr.isValid() && d.guid.isEmpty()) return false;
    *out = d;
    return true;
}

QTcpSocket *SourceHost::newSocket(bool peerTls, QObject *parent)
{
    Q_UNUSED(peerTls)
    return new QTcpSocket(parent);
}

QString SourceDesc::userId() const { return guid.size() == 16 ? guidHex(guid) : addr.toString(); }

// ---------------------------------------------------------------- source
GnutSource::GnutSource(SourceHost *host, const QString &downloadId, const mx::FileHashes &h, qint64 size,
                       const SourceDesc &d, const mx::SearchSource &orig, std::shared_ptr<bool> hostAlive, QObject *parent)
    : RangeSource(parent), m_host(host), m_hostAlive(std::move(hostAlive)), m_downloadId(downloadId), m_hashes(h), m_size(size), m_desc(d), m_orig(orig),
      m_hdr(16384, 100, 4096)
{
    if (m_hashes.sha1.isEmpty()) m_hashes.sha1 = d.sha1;
    m_useGetIndex = m_hashes.sha1.isEmpty();
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        switch (m_phase) {
        case PConnecting: onSocketGone(QStringLiteral("connect timeout")); break;
        case PWaitPush: fail(QStringLiteral("push timeout (no GIV)")); break;
        case PHeaders: case PBody: fail(QStringLiteral("timeout")); break;
        case PThexHeaders: case PThexBody: onSocketGone(QStringLiteral("THEX timeout")); break;
        default: break;
        }
    });
    m_poll.setSingleShot(true);
    connect(&m_poll, &QTimer::timeout, this, &GnutSource::pollQueue);
    m_pumpTimer.setSingleShot(true);
    connect(&m_pumpTimer, &QTimer::timeout, this, &GnutSource::onReadyRead);
}

GnutSource::~GnutSource()
{
    if (m_hostAlive && *m_hostAlive) m_host->cancelPush(this);
    if (m_sock) { m_sock->disconnect(this); m_sock->abort(); }
}

void GnutSource::setState(State s, const QString &text)
{
    if (m_state == s && m_stateText == text) return;
    m_state = s;
    m_stateText = text;
    emit stateChanged();
}

void GnutSource::open()
{
    if (m_closed || m_sock || m_phase == PWaitPush) return;
    const bool canDirect = m_desc.addr.isValid();
    const bool canPush = m_desc.guid.size() == 16 && !m_host->firewalled();
    if (canDirect && (!m_desc.firewalled || !canPush)) connectDirect();
    else if (canPush) {
        m_triedPush = true;
        if (m_host->requestPush(this, m_desc)) {
            m_phase = PWaitPush;
            m_timeout.start(40000);
            setState(Connecting, QStringLiteral("Waiting for push connection..."));
        } else if (canDirect) connectDirect();
        else fail(QStringLiteral("Cannot push (no route)"));
    } else {
        fail(m_desc.firewalled ? QStringLiteral("Firewalled source (we are firewalled too)") : QStringLiteral("No address"));
    }
}

void GnutSource::connectDirect()
{
    m_triedDirect = true;
    QTcpSocket *s = m_host->newSocket(m_desc.tls, this);
    s->setReadBufferSize(512 * 1024);
    m_sock = s;
    m_phase = PConnecting;
    connect(s, &QTcpSocket::connected, this, [this, s] {
        if (s != m_sock) return;
        attachSocket(s);
    });
    connect(s, &QTcpSocket::errorOccurred, this, [this, s](QAbstractSocket::SocketError) {
        if (s == m_sock) onSocketGone(s->errorString());
    });
    connect(s, &QTcpSocket::disconnected, this, [this, s] { if (s == m_sock) onSocketGone(QStringLiteral("closed by peer")); });
    connect(s, &QTcpSocket::readyRead, this, [this, s] { if (s == m_sock) onReadyRead(); });
    m_timeout.start(m_host->connectTimeoutMs());
    if (m_state != Busy) setState(Connecting, QStringLiteral("Connecting..."));
    s->connectToHost(QHostAddress(m_desc.addr.ip), m_desc.addr.port);
}

void GnutSource::acceptPushedSocket(QTcpSocket *sock)
{
    if (m_closed || m_phase != PWaitPush || m_sock) { sock->deleteLater(); return; }
    sock->setParent(this);
    sock->disconnect();
    m_sock = sock;
    connect(sock, &QTcpSocket::errorOccurred, this, [this, sock](QAbstractSocket::SocketError) {
        if (sock == m_sock) onSocketGone(sock->errorString());
    });
    connect(sock, &QTcpSocket::disconnected, this, [this, sock] { if (sock == m_sock) onSocketGone(QStringLiteral("closed by peer")); });
    connect(sock, &QTcpSocket::readyRead, this, [this, sock] { if (sock == m_sock) onReadyRead(); });
    m_host->sourceLog(QStringLiteral("push connection from %1 for %2").arg(sock->peerAddress().toString(), m_desc.name));
    attachSocket(sock);
}

QString GnutSource::clientName() const
{
    if (!m_tls) return m_server;
    return m_server.isEmpty() ? QStringLiteral("[TLS]") : m_server + QStringLiteral(" [TLS]");
}

void GnutSource::attachSocket(QTcpSocket *s)
{
    auto *t = qobject_cast<TlsSocket *>(s);
    m_tls = t && t->isEncrypted();
    if (t && t->fellBack() && m_desc.tls) m_host->sourceLog(QStringLiteral("TLS to %1 failed (%2), plain connection").arg(m_desc.addr.toString(), t->tlsError()));
    m_timeout.stop();
    m_phase = PIdle;
    m_hdr.reset();
    m_keepAlive = true;
    if (m_hasReq) { sendRequest(); return; }
    if (m_poll.property("pending").toBool()) { m_poll.setProperty("pending", false); sendRequest(); return; }
    if (m_state != Queued) setState(Ready, QStringLiteral("Connected"));
}

void GnutSource::request(qint64 offset, qint64 length)
{
    if (m_closed || length <= 0) return;
    if (m_hasReq) {
        emit requestFinished(offset, length, false, QStringLiteral("request already pending"));
        return;
    }
    m_hasReq = true;
    m_reqOff = offset;
    m_reqLen = length;
    m_lastOff = offset;
    m_lastLen = length;
    // served from data granted while we were polling the queue?
    if (m_prefetchOff >= 0 && offset >= m_prefetchOff && offset + length <= m_prefetchOff + m_prefetch.size() && m_phase != PBody) {
        const QByteArray d = m_prefetch.mid(int(offset - m_prefetchOff), int(length));
        m_prefetch.clear();
        m_prefetchOff = -1;
        setState(Busy, QStringLiteral("Transferring"));
        QPointer<GnutSource> self(this);
        QTimer::singleShot(0, this, [this, self, d, offset] {
            if (!self || m_closed || !m_hasReq || m_reqOff != offset) return;
            m_received += d.size();
            emit data(offset, d);
            if (!self) return;
            finishCurrent(true, QString());
        });
        return;
    }
    if (m_phase != PBody) { m_prefetch.clear(); m_prefetchOff = -1; }
    setState(Busy, QStringLiteral("Requesting"));
    if (m_phase == PThexHeaders || m_phase == PThexBody) { m_pendingAfterThex = true; return; }
    if (m_phase == PBody) return;                       // prefetch/discard body still arriving; sent afterwards
    if (m_sock && m_phase == PIdle) { sendRequest(); return; }
    if (!m_sock && m_phase != PWaitPush) {
        if (m_desc.addr.isValid() && !(m_desc.firewalled && m_triedPush && !m_triedDirect)) connectDirect();
        else open();
    }
}

void GnutSource::sendRequest()
{
    if (!m_sock || m_phase != PIdle) return;
    const qint64 off = m_hasReq ? m_reqOff : m_lastOff;
    const qint64 len = m_hasReq ? m_reqLen : m_lastLen;
    QString path;
    if (!m_useGetIndex && m_hashes.sha1.size() == 20) path = QStringLiteral("/uri-res/N2R?") + sha1Urn(m_hashes.sha1);
    else path = QStringLiteral("/get/%1/%2").arg(m_desc.index).arg(QString::fromLatin1(urlEncodePath(m_desc.name)));
    HeaderBlock h;
    h.firstLine = QStringLiteral("GET %1 HTTP/1.1").arg(path);
    h.add(QStringLiteral("Host"), m_desc.addr.isValid() ? m_desc.addr.toString() : m_sock->peerAddress().toString());
    h.add(QStringLiteral("User-Agent"), m_host->userAgent());
    if (len > 0) h.add(QStringLiteral("Range"), QStringLiteral("bytes=%1-%2").arg(off).arg(off + len - 1));
    h.add(QStringLiteral("Connection"), QStringLiteral("Keep-Alive"));
    h.add(QStringLiteral("X-Queue"), QStringLiteral("0.1"));
    if (!m_queue.id.isEmpty())
        h.add(QStringLiteral("X-Queued"), QStringLiteral("position=%1; ID=%2").arg(qMax(0, m_queue.position)).arg(m_queue.id));
    h.add(QStringLiteral("X-Features"), QStringLiteral("browse/1.0, queue/0.1"));
    const HostPort self = m_host->selfAddress();
    if (!m_host->firewalled() && self.isValid()) h.add(QStringLiteral("X-Node"), self.toString());
    meterWrite(h.serialize());
    m_phase = PHeaders;
    m_hdr.reset();
    m_timeout.start(60000);
}

void GnutSource::meterWrite(const QByteArray &bytes)
{
    // [glue-3] request headers count as Gnutella protocol traffic in the bandwidth graph
    if (m_host->limiter()) m_host->limiter()->accountTraffic(mx::RateLimiter::Up, mx::Net::Gnutella, bytes.size(), 0);
    m_sock->write(bytes);
}

void GnutSource::sendThexRequest()
{
    QString uri = m_thexUri.trimmed();
    if (uri.startsWith(QLatin1String("http://"), Qt::CaseInsensitive)) uri = QLatin1Char('/') + uri.mid(7).section(QLatin1Char('/'), 1);
    if (!uri.startsWith(QLatin1Char('/')) || uri.size() > 400 || uri.contains(QLatin1Char(' '))) { m_thexDone = true; return; }
    HeaderBlock h;
    h.firstLine = QStringLiteral("GET %1 HTTP/1.1").arg(uri);
    h.add(QStringLiteral("Host"), m_desc.addr.toString());
    h.add(QStringLiteral("User-Agent"), m_host->userAgent());
    h.add(QStringLiteral("Connection"), QStringLiteral("Keep-Alive"));
    meterWrite(h.serialize());
    m_phase = PThexHeaders;
    m_hdr.reset();
    m_thexBuf.clear();
    m_timeout.start(60000);
}

void GnutSource::onReadyRead()
{
    if (!m_sock || m_closed) return;
    QPointer<GnutSource> self(this);
    for (int guard = 0; guard < 64 && self && m_sock; ++guard) {
        if (m_phase == PHeaders || m_phase == PThexHeaders) {
            if (m_sock->bytesAvailable() <= 0) return;
            const QByteArray head = m_sock->read(4096);
            if (m_host->limiter()) m_host->limiter()->accountTraffic(mx::RateLimiter::Down, mx::Net::Gnutella, head.size(), 0);
            m_hdr.feed(head);
            if (m_hdr.error()) { fail(QStringLiteral("bad HTTP response: ") + m_hdr.errorText()); return; }
            if (!m_hdr.complete()) continue;
            const HeaderBlock h = m_hdr.block();
            if (m_phase == PHeaders) handleResponse(h);
            else handleThexResponse(h);
            continue;
        }
        if (m_phase == PBody || m_phase == PThexBody) {
            deliverBody();
            return;
        }
        // unexpected bytes while idle
        if (m_phase == PIdle && m_sock->bytesAvailable() > 0) {
            m_sock->readAll();
            dropSocket();
        }
        return;
    }
}

void GnutSource::harvestAltLocs(const HeaderBlock &h)
{
    QList<HostPort> tls;
    QList<HostPort> direct = parseAltLocs(h.value(QStringLiteral("X-Alt")), 100, &tls);
    direct += parseAltLocs(h.value(QStringLiteral("Alt-Location")));
    direct += parseAltLocs(h.value(QStringLiteral("X-Gnutella-Alternate-Location")));
    const QList<PushLoc> pushed = parsePushAltLocs(h.value(QStringLiteral("X-Falt")));
    QList<HostPort> clean;
    for (const HostPort &d : direct)
        if (d != m_desc.addr && !clean.contains(d) && clean.size() < 100) clean << d;
    if (!clean.isEmpty() || !pushed.isEmpty()) m_host->altLocsFound(m_downloadId, m_hashes.sha1, clean, pushed, tls);
}

void GnutSource::handleResponse(const HeaderBlock &h)
{
    m_timeout.start(60000);
    QString server = h.first(QStringLiteral("Server"));
    if (server.isEmpty()) server = h.first(QStringLiteral("User-Agent"));
    if (!server.isEmpty()) m_server = server.left(80);
    m_keepAlive = !h.value(QStringLiteral("Connection")).contains(QLatin1String("close"), Qt::CaseInsensitive) &&
                  !h.version.endsWith(QLatin1String("1.0"));
    bool clOk = false;
    const qint64 contentLength = h.first(QStringLiteral("Content-Length")).toLongLong(&clOk);
    const QString availHdr = h.first(QStringLiteral("X-Available-Ranges"));
    if (!availHdr.isEmpty()) {
        const QList<Span> a = parseAvailableRanges(availHdr, m_size);
        if (a != m_avail) { m_avail = a; emit availabilityChanged(); }
    }
    harvestAltLocs(h);
    const QString urnHdr = h.first(QStringLiteral("X-Gnutella-Content-URN"));
    if (!urnHdr.isEmpty() && m_hashes.sha1.size() == 20) {
        FileHashes uh;
        if (parseUrn(urnHdr.section(QLatin1Char(','), 0, 0), &uh) && !uh.sha1.isEmpty() && uh.sha1 != m_hashes.sha1) {
            fail(QStringLiteral("Wrong file (URN mismatch)"));
            return;
        }
    }
    const auto skipBody = [&](bool keep) {
        const qint64 len = clOk ? contentLength : 0;
        if (len > 65536 || !keep) { dropSocket(); return; }
        m_bodyDiscard = true;
        m_bodyIsPrefetch = false;
        m_bodyLeft = len;
        m_bodyGot = 0;
        m_phase = len > 0 ? PBody : PIdle;
        if (len > 0) { QTimer::singleShot(0, this, &GnutSource::onReadyRead); }
    };
    const int code = h.code;
    if (code == 200 || code == 206) {
        qint64 first = 0, last = -1, total = -1;
        const QString cr = h.first(QStringLiteral("Content-Range"));
        const qint64 off = m_hasReq ? m_reqOff : m_lastOff;
        const qint64 len = m_hasReq ? m_reqLen : m_lastLen;
        qint64 bodyLen = 0;
        m_closeAfterBody = !m_keepAlive;
        if (!cr.isEmpty()) {
            if (!parseContentRange(cr, &first, &last, &total)) { fail(QStringLiteral("bad Content-Range")); return; }
            if (total >= 0 && total != m_size) { fail(QStringLiteral("size mismatch (%1 != %2)").arg(total).arg(m_size)); return; }
            if (first != off || last >= off + len) { fail(QStringLiteral("Content-Range %1 does not match the request").arg(cr)); return; }
            bodyLen = last - first + 1;
            if (clOk && contentLength != bodyLen) { fail(QStringLiteral("Content-Length does not match Content-Range")); return; }
        } else {
            if (!clOk || contentLength != m_size) { fail(QStringLiteral("server ignored Range / unexpected length")); return; }
            if (off != 0) { fail(QStringLiteral("server ignored Range")); return; }
            bodyLen = qMin(len, m_size);
            if (bodyLen < contentLength) m_closeAfterBody = true;   // read the prefix, then drop the rest
        }
        if (availHdr.isEmpty()) {
            const QList<Span> full{Span(0, m_size)};
            if (m_avail != full) { m_avail = full; emit availabilityChanged(); }
        }
        if (!m_thexDone && m_thexUri.isEmpty()) {
            const QString t = h.first(QStringLiteral("X-Thex-URI"));
            if (!t.isEmpty() && m_host->wantThex(m_downloadId)) m_thexUri = t;
            else if (t.isEmpty()) m_thexDone = true;
            else m_thexDone = true;
        }
        m_queue = QueueInfo{};
        m_busyCount = 0;
        m_reconnects = 0;
        m_bodyDiscard = false;
        m_bodyOff = first == 0 && cr.isEmpty() ? 0 : first;
        m_bodyLeft = bodyLen;
        m_bodyGot = 0;
        m_bodyIsPrefetch = !m_hasReq;
        if (m_bodyIsPrefetch) {
            if (bodyLen > 1024 * 1024) { dropSocket(); setState(Ready, QStringLiteral("Upload slot granted")); return; }
            m_prefetch.clear();
            m_prefetchOff = m_bodyOff;
        }
        m_phase = PBody;
        setState(Busy, QStringLiteral("Transferring"));
        const QByteArray rest = m_hdr.takeRemainder();
        if (!rest.isEmpty()) {
            // header reader over-read a little: process it as body before the socket data
            QByteArray chunk = rest.left(int(qMin<qint64>(rest.size(), m_bodyLeft)));
            // [glue-3] these bytes were accounted as wire when the header was read: payload part only
            if (m_host->limiter() && !chunk.isEmpty() && !m_bodyDiscard)
                m_host->limiter()->accountTraffic(mx::RateLimiter::Down, mx::Net::Gnutella, 0, chunk.size());
            if (m_bodyIsPrefetch) m_prefetch.append(chunk);
            else if (!chunk.isEmpty()) { m_received += chunk.size(); emit data(m_bodyOff + m_bodyGot, chunk); }
            m_bodyGot += chunk.size();
            m_bodyLeft -= chunk.size();
        }
        QTimer::singleShot(0, this, &GnutSource::onReadyRead);
        if (m_bodyLeft == 0) deliverBody();
        return;
    }
    if (code == 503) {
        QueueInfo q;
        parseXQueue(h.first(QStringLiteral("X-Queue")), &q);
        parseXQueued(h.first(QStringLiteral("X-Queued")), &q);
        const int retry = parseRetryAfter(h.first(QStringLiteral("Retry-After")));
        int interval;
        QString text;
        if (q.present) {
            if (q.id.isEmpty()) q.id = m_queue.id;
            m_queue = q;
            interval = q.pollMin > 0 ? q.pollMin : (retry > 0 ? retry : 45);
            if (q.lifetime > 0) interval = qMin(interval, qMax(5, q.lifetime * 4 / 5));
            if (q.pollMax > 0) interval = qMin(interval, q.pollMax);
            text = q.position > 0 ? (q.length > 0 ? QStringLiteral("Remotely Queued (%1 of %2)").arg(q.position).arg(q.length)
                                                  : QStringLiteral("Remotely Queued (%1)").arg(q.position))
                                  : QStringLiteral("Remotely Queued");
        } else {
            ++m_busyCount;
            interval = retry > 0 ? retry : qMin(60 * m_busyCount, 600);
            text = QStringLiteral("Remote busy, retry in %1 s").arg(interval);
        }
        interval = qBound(5, interval, 900);
        skipBody(m_keepAlive);
        QPointer<GnutSource> self(this);
        finishCurrent(false, q.present ? QStringLiteral("Remotely queued") : QStringLiteral("Busy"));
        if (!self) return;
        setState(Queued, text);
        m_poll.start(interval * 1000);
        return;
    }
    if (code == 416) {
        skipBody(m_keepAlive);
        QPointer<GnutSource> self(this);
        finishCurrent(false, QStringLiteral("Range not available"));
        if (!self) return;
        setState(Ready, QStringLiteral("Connected"));
        return;
    }
    if ((code == 400 || code == 501) && !m_useGetIndex && m_desc.index > 0 && !m_desc.name.isEmpty()) {
        m_useGetIndex = true;                       // old servent without /uri-res
        skipBody(m_keepAlive);
        if (m_phase == PIdle && m_sock) sendRequest();
        else dropSocket();
        return;
    }
    if (code == 404 || code == 410) { fail(QStringLiteral("File not found")); return; }
    fail(QStringLiteral("HTTP %1 %2").arg(code).arg(h.reason.left(60)));
}

void GnutSource::handleThexResponse(const HeaderBlock &h)
{
    bool ok = false;
    const qint64 len = h.first(QStringLiteral("Content-Length")).toLongLong(&ok);
    if (h.code != 200 || !ok || len <= 0 || len > 4 * 1024 * 1024) {
        const int retry = parseRetryAfter(h.first(QStringLiteral("Retry-After")));
        if (h.code == 503 && ++m_thexTries < 6) {
            const int waitSec = qBound(1, retry > 0 ? retry : 10, 120);
            m_thexRetryAtMs = QDateTime::currentMSecsSinceEpoch() + waitSec * 1000;
            QTimer::singleShot(waitSec * 1000 + 50, this, [this] {
                if (m_sock && m_phase == PIdle && !m_hasReq && !m_thexDone && !m_closed) sendThexRequest();
            });
        } else {
            m_thexDone = true;
            m_host->thexLearned(m_downloadId, ThexTree());
        }
        if (ok && len >= 0 && len <= 65536) {
            m_bodyDiscard = true;
            m_bodyLeft = len;
            m_bodyGot = 0;
            m_phase = len > 0 ? PBody : PIdle;
        } else {
            dropSocket();
        }
        if (m_phase == PIdle && m_pendingAfterThex) { m_pendingAfterThex = false; if (m_sock) sendRequest(); }
        if (m_phase == PBody) QTimer::singleShot(0, this, &GnutSource::onReadyRead);
        else if (!m_sock && m_pendingAfterThex) { m_pendingAfterThex = false; request(m_reqOff, m_reqLen); }
        return;
    }
    m_thexLeft = len;
    m_thexBuf = m_hdr.takeRemainder().left(int(len));
    m_thexLeft -= m_thexBuf.size();
    m_phase = PThexBody;
    if (m_thexLeft == 0) deliverBody();
}

void GnutSource::deliverBody()
{
    QPointer<GnutSource> self(this);
    if (m_phase == PThexBody) {
        while (m_sock && m_thexLeft > 0 && m_sock->bytesAvailable() > 0) {
            const QByteArray d = m_sock->read(qMin<qint64>(m_thexLeft, 65536));
            if (m_host->limiter()) m_host->limiter()->accountTraffic(mx::RateLimiter::Down, mx::Net::Gnutella, d.size(), 0);
            m_thexBuf.append(d);
            m_thexLeft -= d.size();
        }
        if (m_thexLeft > 0) { m_timeout.start(60000); return; }
        ThexTree t;
        QString err;
        QByteArray expected = m_hashes.tth;
        if (expected.isEmpty()) {
            const int semi = int(m_thexUri.lastIndexOf(QLatin1Char(';')));
            if (semi > 0) { bool o = false; const QByteArray r = mx::hash::fromBase32(m_thexUri.mid(semi + 1), &o); if (o && r.size() == 24) expected = r; }
        }
        if (parseThex(m_thexBuf, m_size, expected, &t, &err)) {
            m_host->sourceLog(QStringLiteral("THEX from %1: depth %2, %3 nodes of %4 bytes").arg(m_desc.addr.toString()).arg(t.depth).arg(t.deepest.size()).arg(t.unitSize));
            m_host->thexLearned(m_downloadId, t);
        } else {
            m_host->sourceLog(QStringLiteral("THEX from %1 rejected: %2").arg(m_desc.addr.toString(), err));
            m_host->thexLearned(m_downloadId, ThexTree());
        }
        if (!self) return;
        m_thexDone = true;
        m_thexBuf.clear();
        m_phase = PIdle;
        m_timeout.stop();
        if (!m_keepAlive) dropSocket();
        if (m_pendingAfterThex) {
            m_pendingAfterThex = false;
            if (m_sock) sendRequest();
            else { m_hasReq = false; request(m_reqOff, m_reqLen); }
        } else if (m_state == Busy && !m_hasReq) {
            setState(Ready, QStringLiteral("Connected"));
        }
        return;
    }
    if (m_phase != PBody || !m_sock) return;
    bool starved = false;   // [bw-1] a partial grant: the shared bucket is empty, wait for the timer
    while (!starved && m_bodyLeft > 0 && m_sock && m_sock->bytesAvailable() > 0) {
        qint64 want = qMin<qint64>(qMin<qint64>(m_bodyLeft, m_sock->bytesAvailable()), 65536);
        const bool exempt = m_host->limiter() && m_host->limiter()->exempt(m_sock->peerAddress());   // [bw-1] LAN peer
        if (!m_bodyDiscard && m_host->limiter() && !exempt) {
            const qint64 asked = want;
            want = m_host->limiter()->acquireFor(mx::RateLimiter::Down, mx::Net::Gnutella, want);
            if (want <= 0) { m_pumpTimer.start(50); return; }
            starved = want < asked;
        }
        const QByteArray d = m_sock->read(want);
        if (d.isEmpty()) break;
        if (m_host->limiter() && exempt) m_host->limiter()->accountExempt(mx::RateLimiter::Down, mx::Net::Gnutella, d.size(), m_bodyDiscard ? 0 : d.size());
        else if (m_host->limiter()) m_host->limiter()->accountTraffic(mx::RateLimiter::Down, mx::Net::Gnutella, d.size(), m_bodyDiscard ? 0 : d.size());
        const qint64 at = m_bodyOff + m_bodyGot;
        m_bodyGot += d.size();
        m_bodyLeft -= d.size();
        if (m_bodyDiscard) continue;
        if (m_bodyIsPrefetch) { m_prefetch.append(d); continue; }
        m_received += d.size();
        emit data(at, d);
        if (!self) return;
        if (m_closed) return;
    }
    if (m_bodyLeft > 0) {
        if (starved && !m_pumpTimer.isActive()) m_pumpTimer.start(50);
        m_timeout.start(60000);
        return;
    }
    m_timeout.stop();
    // body complete
    if (m_bodyDiscard) {
        m_bodyDiscard = false;
        m_phase = PIdle;
        if (m_closeAfterBody || !m_keepAlive) dropSocket();
        if (m_hasReq && m_state == Busy) { if (m_sock) sendRequest(); else { m_hasReq = false; request(m_reqOff, m_reqLen); } }
        return;
    }
    if (m_bodyIsPrefetch) {
        m_bodyIsPrefetch = false;
        m_phase = PIdle;
        if (m_closeAfterBody || !m_keepAlive) dropSocket();
        if (m_hasReq) {                                 // the engine asked meanwhile
            const qint64 o = m_reqOff, l = m_reqLen;
            m_hasReq = false;
            request(o, l);
        } else {
            setState(Ready, QStringLiteral("Upload slot granted"));
        }
        return;
    }
    const bool fullLen = m_bodyGot == m_reqLen;
    m_phase = PIdle;
    if (m_closeAfterBody || !m_keepAlive) dropSocket();
    const bool doThex = m_sock && !m_thexDone && !m_thexUri.isEmpty() && QDateTime::currentMSecsSinceEpoch() >= m_thexRetryAtMs;
    if (doThex) sendThexRequest();
    const qint64 o = m_reqOff, l = m_reqLen;
    m_hasReq = false;
    setState(Ready, QStringLiteral("Connected"));
    emit requestFinished(o, l, fullLen, fullLen ? QString() : QStringLiteral("short response (%1 of %2 bytes)").arg(m_bodyGot).arg(l));
}

void GnutSource::finishCurrent(bool ok, const QString &err)
{
    if (!m_hasReq) return;
    m_hasReq = false;
    emit requestFinished(m_reqOff, m_reqLen, ok, err);
}

void GnutSource::dropSocket()
{
    if (!m_sock) { m_phase = PNone; return; }
    QTcpSocket *s = m_sock;
    m_sock = nullptr;
    s->disconnect(this);
    s->abort();
    s->deleteLater();
    m_phase = PNone;
    m_timeout.stop();
}

void GnutSource::onSocketGone(const QString &why)
{
    if (m_closed) return;
    const Phase ph = m_phase;
    dropSocket();
    switch (ph) {
    case PConnecting:
        if (m_desc.guid.size() == 16 && !m_triedPush && !m_host->firewalled()) {
            m_triedPush = true;
            if (m_host->requestPush(this, m_desc)) {
                m_phase = PWaitPush;
                m_timeout.start(40000);
                setState(m_state == Busy ? Busy : Connecting, QStringLiteral("Direct connect failed, waiting for push..."));
                return;
            }
        }
        fail(QStringLiteral("Connect failed: %1").arg(why));
        return;
    case PIdle:
    case PNone:
        return;                                  // keep-alive connection closed: reconnect lazily
    case PHeaders:
        if (++m_reconnects <= 2) {
            // the peer closed a kept-alive connection just as we reused it: retry once
            if (m_hasReq || m_poll.property("pending").toBool() || true) {
                if (!m_hasReq) m_poll.setProperty("pending", true);
                if (m_desc.addr.isValid() && !m_desc.firewalled) connectDirect();
                else open();
                return;
            }
        }
        fail(QStringLiteral("Connection lost: %1").arg(why));
        return;
    case PBody: {
        QPointer<GnutSource> self(this);
        if (++m_reconnects > 5) { fail(QStringLiteral("Connection lost: %1").arg(why)); return; }
        if (m_bodyIsPrefetch || m_bodyDiscard) { m_bodyIsPrefetch = m_bodyDiscard = false; m_prefetch.clear(); m_prefetchOff = -1; return; }
        finishCurrent(false, QStringLiteral("connection lost: %1").arg(why));
        if (!self) return;
        setState(Ready, QStringLiteral("Disconnected"));
        return;
    }
    case PThexHeaders:
    case PThexBody:
        m_thexDone = true;
        m_host->thexLearned(m_downloadId, ThexTree());
        if (m_pendingAfterThex) { m_pendingAfterThex = false; m_hasReq = false; request(m_reqOff, m_reqLen); }
        return;
    case PWaitPush:
        return;
    }
}

void GnutSource::pollQueue()
{
    if (m_closed) return;
    if (m_sock && m_phase == PIdle) { sendRequest(); return; }
    if (m_sock && m_phase != PNone) { m_poll.start(5000); return; }
    m_poll.setProperty("pending", true);
    if (m_desc.addr.isValid() && !(m_desc.firewalled && m_triedPush)) connectDirect();
    else if (m_phase != PWaitPush) {
        m_triedPush = false;
        open();
    }
}

void GnutSource::fail(const QString &why)
{
    if (m_closed) return;
    m_host->cancelPush(this);
    m_host->sourceLog(QStringLiteral("source %1 failed: %2").arg(m_desc.addr.toString(), why));
    dropSocket();
    m_poll.stop();
    m_timeout.stop();
    QPointer<GnutSource> self(this);
    finishCurrent(false, why);
    if (!self) return;
    setState(Failed, why);
}

void GnutSource::cancelRequests()
{
    if (!m_hasReq) return;
    m_hasReq = false;
    if (m_phase == PHeaders || m_phase == PBody) dropSocket();   // HTTP cannot abort a response otherwise
    if (m_state == Busy) setState(Ready, QStringLiteral("Connected"));
}

void GnutSource::close()
{
    if (m_closed) return;
    m_closed = true;
    if (m_hostAlive && *m_hostAlive) m_host->cancelPush(this);
    dropSocket();
    m_poll.stop();
    m_timeout.stop();
    m_pumpTimer.stop();
    setState(Closed, QStringLiteral("Closed"));
}

} // namespace mx::gnut
