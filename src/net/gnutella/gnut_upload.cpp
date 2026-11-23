// SPDX-License-Identifier: GPL-3.0-or-later
#include "gnut_upload.h"
#include "gnut_tls.h"
#include "gnut_huge.h"
#include "gnut_zlib.h"

#include "mxhash/encoding.h"

#include <QtCore/QUrl>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpSocket>

namespace mx::gnut {

static int s_uploadCounter = 0;
static constexpr int kChunk = 64 * 1024;
static constexpr qint64 kWriteHigh = 128 * 1024;

UploadSession::UploadSession(ServeHost *host, QTcpSocket *sock, const QByteArray &initial, bool pushed, QObject *parent)
    : QObject(parent), m_host(host), m_sock(sock), m_pushed(pushed), m_hdr(8192, 64, 4096)
{
    m_sock->setParent(this);
    m_peer.ip = m_sock->peerAddress().toIPv4Address();
    m_peer.port = m_sock->peerPort();
    m_idle.setSingleShot(true);
    m_idle.setInterval(90000);
    connect(&m_idle, &QTimer::timeout, this, [this] { closeNow(); });
    m_pumpTimer.setSingleShot(true);
    connect(&m_pumpTimer, &QTimer::timeout, this, &UploadSession::pump);
    m_reportTimer.setInterval(1000);
    connect(&m_reportTimer, &QTimer::timeout, this, [this] { report(true); });
    connect(m_sock, &QTcpSocket::readyRead, this, &UploadSession::onReadyRead);
    connect(m_sock, &QTcpSocket::bytesWritten, this, [this](qint64) { if (m_remaining > 0) pump(); else if (m_closeAfter && m_sock->bytesToWrite() == 0) closeNow(); });
    connect(m_sock, &QTcpSocket::disconnected, this, [this] { closeNow(); });
    connect(m_sock, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) { closeNow(); });
    m_idle.start();
    if (!initial.isEmpty()) {
        m_hdr.feed(initial);
        QTimer::singleShot(0, this, &UploadSession::onReadyRead);
    }
}

UploadSession::~UploadSession()
{
    if (m_haveSlot) m_host->releaseSlot(this);
    if (m_sock) m_sock->disconnect(this);
}

void UploadSession::cancel()
{
    endUpload(mx::TransferState::Cancelled, QStringLiteral("Cancelled"));
    closeNow();
}

void UploadSession::closeNow()
{
    if (m_done) return;
    m_done = true;
    if (!m_uploadId.isEmpty() && !m_queued)   // [glue-3] a queued row lives on in the plugin's queue
        endUpload(m_remaining > 0 ? mx::TransferState::Error : mx::TransferState::Completed,
                  m_remaining > 0 ? QStringLiteral("Connection closed") : QStringLiteral("Finished"));
    if (m_haveSlot) { m_host->releaseSlot(this); m_haveSlot = false; }
    m_idle.stop();
    m_pumpTimer.stop();
    m_reportTimer.stop();
    if (m_sock) {
        m_sock->disconnect(this);
        if (m_sock->state() == QAbstractSocket::ConnectedState) m_sock->disconnectFromHost();
    }
    emit finished();
    deleteLater();
}

void UploadSession::writeMeta(const QByteArray &bytes)
{
    // [glue-3] response headers / small bodies (THEX, 503 queue answers) count as protocol traffic
    if (m_host->limiter()) m_host->limiter()->accountTraffic(mx::RateLimiter::Up, mx::Net::Gnutella, bytes.size(), 0);
    m_sock->write(bytes);
}

void UploadSession::onReadyRead()
{
    if (m_done) return;
    if (m_sock->bytesAvailable() > 0) {
        const QByteArray in = m_sock->read(16384);
        if (m_host->limiter()) m_host->limiter()->accountTraffic(mx::RateLimiter::Down, mx::Net::Gnutella, in.size(), 0);
        m_hdr.feed(in);
    }
    if (m_hdr.error()) {
        sendSimple(400, QStringLiteral("Bad Request"), {}, QByteArray(), true);
        return;
    }
    if (!m_hdr.complete()) return;
    if (m_remaining > 0) return;                     // pipelined request: handled after this body
    HeaderBlock req = m_hdr.block();
    const QByteArray rest = m_hdr.takeRemainder();
    m_hdr.reset();
    if (!rest.isEmpty()) m_hdr.feed(rest);
    m_idle.start();
    handleRequest(req);
}

static QString reasonFor(int code)
{
    switch (code) {
    case 200: return QStringLiteral("OK");
    case 206: return QStringLiteral("Partial Content");
    case 400: return QStringLiteral("Bad Request");
    case 403: return QStringLiteral("Forbidden");
    case 404: return QStringLiteral("Not Found");
    case 406: return QStringLiteral("Not Acceptable");
    case 416: return QStringLiteral("Requested Range Not Satisfiable");
    case 501: return QStringLiteral("Not Implemented");
    case 503: return QStringLiteral("Service Unavailable");
    default: return QStringLiteral("Error");
    }
}

void UploadSession::sendSimple(int code, const QString &reason, const QList<QPair<QString, QString>> &extra,
                               const QByteArray &body, bool close)
{
    HeaderBlock h;
    h.firstLine = QStringLiteral("HTTP/1.1 %1 %2").arg(code).arg(reason.isEmpty() ? reasonFor(code) : reason);
    h.add(QStringLiteral("Server"), m_host->userAgent());
    for (const auto &e : extra) h.add(e.first, e.second);
    h.add(QStringLiteral("Content-Length"), QString::number(body.size()));
    if (!body.isEmpty()) h.add(QStringLiteral("Content-Type"), QStringLiteral("text/plain"));
    h.add(QStringLiteral("Connection"), close ? QStringLiteral("close") : QStringLiteral("Keep-Alive"));
    writeMeta(h.serialize());
    if (!body.isEmpty()) writeMeta(body);
    if (close) {
        m_closeAfter = true;
        if (m_sock->bytesToWrite() == 0) closeNow();
    }
}

void UploadSession::handleRequest(const HeaderBlock &req)
{
    const bool head = req.method == QLatin1String("HEAD");
    if (!req.isRequest || (req.method != QLatin1String("GET") && !head)) {
        sendSimple(501, QString(), {}, QByteArray(), true);
        return;
    }
    const QString agent = req.first(QStringLiteral("User-Agent")).left(80);
    if (!agent.isEmpty()) m_peerAgent = agent;
    {   // [enc-1] the Transfers row tool tip shows "[TLS]" on an encrypted connection
        auto *t = qobject_cast<TlsSocket *>(m_sock);
        if (t && t->isEncrypted() && !m_peerAgent.endsWith(QLatin1String("[TLS]")))
            m_peerAgent = m_peerAgent.isEmpty() ? QStringLiteral("[TLS]") : m_peerAgent + QStringLiteral(" [TLS]");
    }
    const bool http10 = req.version == QLatin1String("HTTP/1.0");
    const QString conn = req.value(QStringLiteral("Connection"));
    m_closeAfter = conn.contains(QLatin1String("close"), Qt::CaseInsensitive) ||
                   (http10 && !conn.contains(QLatin1String("keep-alive"), Qt::CaseInsensitive));
    // the downloader's own address (alt-loc mesh)
    const HostPort node = HostPort::parse(req.first(QStringLiteral("X-Node")).isEmpty() ? req.first(QStringLiteral("X-Listen-IP")) : req.first(QStringLiteral("X-Node")), 6346);

    const QString target = req.target;
    QString path = target;
    QString query;
    const int qm = int(target.indexOf(QLatin1Char('?')));
    if (qm >= 0) { path = target.left(qm); query = target.mid(qm + 1); }

    if (path == QLatin1String("/uri-res/N2R") || path == QLatin1String("/uri-res/N2X")) {
        const QString urn = QUrl::fromPercentEncoding(query.toLatin1()).section(QLatin1Char(';'), 0, 0);
        FileHashes h;
        mx::SharedFile f;
        const bool known = parseUrn(urn, &h) &&
                           ((!h.sha1.isEmpty() && m_host->sharedBySha1(h.sha1, &f)) ||
                            (!h.tth.isEmpty() && m_host->sharedByTth(h.tth, &f)));
        m_host->noteIncomingHttp(m_peer, QStringLiteral("%1 %2").arg(req.method, target.left(100)));
        if (!known) { sendSimple(404, QStringLiteral("Not Found"), {}, QByteArray(), m_closeAfter); return; }
        if (node.isValid() && !f.hashes.sha1.isEmpty() && req.first(QStringLiteral("X-Downloaded")).isEmpty())
            Q_UNUSED(node);                         // partial-file sharing downloaders are not advertised
        const QList<HostPort> alts = parseAltLocs(req.value(QStringLiteral("X-Alt")));
        if (!alts.isEmpty() && !f.hashes.sha1.isEmpty()) m_host->addAltLocs(f.hashes.sha1, alts);
        if (path.endsWith(QLatin1String("N2X"))) serveThex(req, f, head);
        else serveFile(req, f, head);
        return;
    }
    if (path.startsWith(QLatin1String("/get/"))) {
        const QString rest = path.mid(5);
        bool ok = false;
        const quint32 idx = rest.section(QLatin1Char('/'), 0, 0).toUInt(&ok);
        mx::SharedFile f;
        m_host->noteIncomingHttp(m_peer, QStringLiteral("%1 %2").arg(req.method, target.left(100)));
        if (!ok || !m_host->sharedByIndex(idx, &f)) { sendSimple(404, QString(), {}, QByteArray(), m_closeAfter); return; }
        serveFile(req, f, head);
        return;
    }
    if (path == QLatin1String("/") || path.isEmpty()) {
        m_host->noteIncomingHttp(m_peer, QStringLiteral("browse %1").arg(req.first(QStringLiteral("Accept")).left(60)));
        serveBrowse(req, head);
        return;
    }
    if (path.startsWith(QLatin1String("/gnutella/push-proxy")) || path.startsWith(QLatin1String("/gnet/push-proxy"))) {
        sendSimple(400, QStringLiteral("Not A Push Proxy"), {}, QByteArray(), true);
        return;
    }
    sendSimple(404, QString(), {}, QByteArray(), m_closeAfter);
}

void UploadSession::serveThex(const HeaderBlock &req, const mx::SharedFile &f, bool head)
{
    Q_UNUSED(req)
    int retry = 0;
    const QByteArray dime = m_host->thexFor(f, &retry);
    if (dime.isEmpty()) {
        QList<QPair<QString, QString>> ex;
        if (retry > 0) ex << qMakePair(QStringLiteral("Retry-After"), QString::number(retry));
        sendSimple(retry > 0 ? 503 : 404, retry > 0 ? QStringLiteral("Tree Being Computed") : QStringLiteral("No Tree"), ex,
                   QByteArray(), m_closeAfter);
        return;
    }
    HeaderBlock h;
    h.firstLine = QStringLiteral("HTTP/1.1 200 OK");
    h.add(QStringLiteral("Server"), m_host->userAgent());
    h.add(QStringLiteral("Content-Type"), QStringLiteral("application/dime"));
    h.add(QStringLiteral("Content-Length"), QString::number(dime.size()));
    if (!f.hashes.sha1.isEmpty()) h.add(QStringLiteral("X-Gnutella-Content-URN"), sha1Urn(f.hashes.sha1));
    h.add(QStringLiteral("Connection"), m_closeAfter ? QStringLiteral("close") : QStringLiteral("Keep-Alive"));
    writeMeta(h.serialize());
    if (!head) writeMeta(dime);
    if (m_closeAfter) { if (m_sock->bytesToWrite() == 0) closeNow(); }
}

void UploadSession::serveBrowse(const HeaderBlock &req, bool head)
{
    const QString accept = req.value(QStringLiteral("Accept"));
    if (!accept.contains(QLatin1String("application/x-gnutella-packets"), Qt::CaseInsensitive)) {
        const QByteArray body = "OctoMX Gnutella servent. Browse with a Gnutella client.\r\n";
        sendSimple(406, QString(), {}, body, true);
        return;
    }
    if (!m_host->browseAllowed(m_peer)) {
        sendSimple(403, QStringLiteral("Browse Host Disabled"), {}, QByteArray(), true);
        return;
    }
    QByteArray body;
    for (const QByteArray &p : m_host->browsePackets()) body.append(p);
    HeaderBlock h;
    h.firstLine = QStringLiteral("HTTP/1.1 200 OK");
    h.add(QStringLiteral("Server"), m_host->userAgent());
    h.add(QStringLiteral("Content-Type"), QStringLiteral("application/x-gnutella-packets"));
    if (req.value(QStringLiteral("Accept-Encoding")).contains(QLatin1String("deflate"), Qt::CaseInsensitive) && !body.isEmpty()) {
        body = deflateAll(body, 6);
        h.add(QStringLiteral("Content-Encoding"), QStringLiteral("deflate"));
    }
    h.add(QStringLiteral("Content-Length"), QString::number(body.size()));
    h.add(QStringLiteral("Connection"), QStringLiteral("close"));
    writeMeta(h.serialize());
    if (!head) writeMeta(body);
    m_closeAfter = true;
    if (m_sock->bytesToWrite() == 0) closeNow();
}

void UploadSession::serveFile(const HeaderBlock &req, const mx::SharedFile &f, bool head)
{
    const qint64 size = f.size;
    QList<Span> spans;
    const QString rangeHdr = req.first(QStringLiteral("Range"));
    bool partial = false;
    qint64 first = 0, end = size;
    if (!rangeHdr.isEmpty()) {
        if (!parseRangeHeader(rangeHdr, size, &spans)) {
            sendSimple(416, QString(), {qMakePair(QStringLiteral("Content-Range"), QStringLiteral("bytes */%1").arg(size))},
                       QByteArray(), m_closeAfter);
            return;
        }
        first = spans.first().first;
        end = spans.first().second;
        partial = !(first == 0 && end == size);
    }
    // a new file on this connection = a new upload entry
    if (m_uploadSha1 != f.hashes.sha1 || m_uploadId.isEmpty()) {
        if (!m_uploadId.isEmpty() && !m_queued) endUpload(mx::TransferState::Completed, QStringLiteral("Finished"));
        m_queued = false;
        m_uploadId = QStringLiteral("gnut-up-%1").arg(++s_uploadCounter);
        m_uploadSha1 = f.hashes.sha1;
        m_sentTotal = 0;
        m_info = mx::TransferInfo();
        m_info.id = m_uploadId;
        m_info.isUpload = true;
        m_info.name = f.name;
        m_info.size = f.size;
        m_info.hashes = f.hashes;
        m_info.networks = {mx::Net::Gnutella};
        m_info.added = QDateTime::currentDateTime();
        m_info.peer.net = mx::Net::Gnutella;
        m_info.peer.id = m_peer.toString();
        m_info.peer.address = m_peer.toString();
        m_info.peer.name = ipToString(m_peer.ip);
        mx::TransferSource ts;
        ts.net = mx::Net::Gnutella;
        ts.user = m_info.peer;
        ts.client = m_peerAgent;
        m_info.sources = {ts};
    }
    // slot / queue
    QueueInfo q;
    const QString key = ipToString(m_peer.ip) + QLatin1Char('/') + mx::hash::toHex(f.hashes.sha1.isEmpty() ? f.path.toUtf8() : f.hashes.sha1);
    m_slotKey = key;
    QueueInfo inQ;
    parseXQueued(req.first(QStringLiteral("X-Queued")), &inQ);
    if (!m_haveSlot) {
        const int r = m_host->acquireSlot(this, key, inQ.id, &q);
        if (!q.rowId.isEmpty() && q.rowId != m_uploadId) {   // [glue-3] one row per queue entry
            m_uploadId = q.rowId;
            m_info.id = q.rowId;
        }
        m_queued = r == 0;
        if (r == 1) {
            m_haveSlot = true;
        } else {
            QList<QPair<QString, QString>> ex;
            const QString xqv = req.first(QStringLiteral("X-Queue"));
            if (r == 0) {
                ex << qMakePair(QStringLiteral("X-Queue"), QStringLiteral("position=%1, length=%2, limit=%3, pollMin=%4, pollMax=%5")
                                    .arg(q.position).arg(q.length).arg(q.limit).arg(q.pollMin).arg(q.pollMax));
                if (xqv.trimmed().startsWith(QLatin1Char('1')))
                    ex << qMakePair(QStringLiteral("X-Queued"), QStringLiteral("ID=%1; position=%2; length=%3; lifetime=%4")
                                        .arg(q.id).arg(q.position).arg(q.length).arg(q.lifetime));
                ex << qMakePair(QStringLiteral("Retry-After"), QString::number(q.pollMin));
                if (!f.hashes.sha1.isEmpty()) ex << qMakePair(QStringLiteral("X-Gnutella-Content-URN"), sha1Urn(f.hashes.sha1));
                m_info.state = mx::TransferState::Queued;
                m_info.statusText = QStringLiteral("Queued (%1 of %2)").arg(q.position).arg(q.length);
                m_info.queuePosition = q.position;   // [glue-3]
                report(true);
                sendSimple(503, QStringLiteral("Queued"), ex, QByteArray(), m_closeAfter);
                // LimeWire-style clients keep the connection; drop it if they do not poll in time
                m_idle.start((q.pollMax + 30) * 1000);
            } else {
                ex << qMakePair(QStringLiteral("Retry-After"), QStringLiteral("300"));
                m_info.state = mx::TransferState::Error;
                m_info.statusText = QStringLiteral("Busy (queue full)");
                sendSimple(503, QStringLiteral("Busy"), ex, QByteArray(), true);
            }
            return;
        }
    }
    HeaderBlock h;
    h.firstLine = partial ? QStringLiteral("HTTP/1.1 206 Partial Content") : QStringLiteral("HTTP/1.1 200 OK");
    h.add(QStringLiteral("Server"), m_host->userAgent());
    h.add(QStringLiteral("Content-Type"), QStringLiteral("application/binary"));
    h.add(QStringLiteral("Accept-Ranges"), QStringLiteral("bytes"));
    h.add(QStringLiteral("Content-Length"), QString::number(end - first));
    if (partial || !rangeHdr.isEmpty())
        h.add(QStringLiteral("Content-Range"), QStringLiteral("bytes %1-%2/%3").arg(first).arg(end - 1).arg(size));
    if (!f.hashes.sha1.isEmpty()) {
        h.add(QStringLiteral("X-Gnutella-Content-URN"), sha1Urn(f.hashes.sha1));
        const QByteArray tth = m_host->tthFor(f);
        if (tth.size() == 24) {
            int retry = 0;
            m_host->thexFor(f, &retry);                 // warm the tree cache for the THEX request
        }
        if (tth.size() == 24)
            h.add(QStringLiteral("X-Thex-URI"), QStringLiteral("/uri-res/N2X?%1;%2").arg(sha1Urn(f.hashes.sha1), mx::hash::toBase32(tth)));
        const QList<HostPort> alts = m_host->altLocsFor(f.hashes.sha1, m_peer);
        if (!alts.isEmpty()) h.add(QStringLiteral("X-Alt"), formatAltLocs(alts.mid(0, 10)));
    }
    h.add(QStringLiteral("X-Features"), QStringLiteral("browse/1.0, queue/0.1"));
    h.add(QStringLiteral("Connection"), m_closeAfter ? QStringLiteral("close") : QStringLiteral("Keep-Alive"));
    writeMeta(h.serialize());
    if (head) {
        if (m_closeAfter && m_sock->bytesToWrite() == 0) closeNow();
        return;
    }
    m_file = f;
    m_offset = first;
    m_remaining = end - first;
    m_info.state = mx::TransferState::Transferring;
    m_info.statusText = QStringLiteral("Uploading");
    if (!m_rateClock.isValid()) m_rateClock.start();
    m_reportTimer.start();
    report(true);
    pump();
}

void UploadSession::pump()
{
    if (m_done || !m_sock) return;
    while (m_remaining > 0 && m_sock->bytesToWrite() < kWriteHigh) {
        qint64 want = qMin<qint64>(kChunk, m_remaining);
        const bool exempt = m_host->limiter() && m_host->limiter()->exempt(m_sock->peerAddress());   // [bw-1] LAN peer
        qint64 allowed = m_host->limiter() && !exempt ? m_host->limiter()->acquire(mx::RateLimiter::Up, want) : want;
        if (allowed <= 0) { m_pumpTimer.start(50); return; }
        const QByteArray data = m_host->library()->read(m_file.hashes, m_offset, allowed);
        if (data.isEmpty()) {
            endUpload(mx::TransferState::Error, QStringLiteral("Read error"));
            closeNow();
            return;
        }
        m_sock->write(data);
        if (m_host->limiter() && exempt) m_host->limiter()->accountExempt(mx::RateLimiter::Up, mx::Net::Gnutella, data.size(), data.size());
        else if (m_host->limiter()) m_host->limiter()->accountTraffic(mx::RateLimiter::Up, mx::Net::Gnutella, data.size(), data.size());
        m_offset += data.size();
        m_remaining -= data.size();
        m_sentTotal += data.size();
        m_rateBytes += data.size();
        m_idle.start();
        // [bw-1] a partial grant: the shared bucket is empty now, continue from the timer (looping on
        // would write the few bytes that trickle in per pass and keep the event loop busy)
        if (allowed < want && m_remaining > 0) { m_pumpTimer.start(25); return; }
    }
    if (m_remaining == 0) {
        report(true);
        if (m_closeAfter) {
            if (m_sock->bytesToWrite() == 0) closeNow();
            return;
        }
        // next pipelined/keep-alive request may already be buffered
        if (m_hdr.complete() || m_sock->bytesAvailable() > 0) QTimer::singleShot(0, this, &UploadSession::onReadyRead);
    }
}

void UploadSession::report(bool force)
{
    if (m_uploadId.isEmpty()) return;
    if (m_rateClock.isValid() && m_rateClock.elapsed() >= 1000) {
        m_rateBps = int(m_rateBytes * 1000 / qMax<qint64>(1, m_rateClock.elapsed()));
        m_rateBytes = 0;
        m_rateClock.restart();
    }
    if (!force) return;
    m_info.done = m_sentTotal;
    m_info.rateUpBps = m_remaining > 0 ? m_rateBps : 0;
    if (!m_info.sources.isEmpty()) {
        m_info.sources[0].bytes = m_sentTotal;
        m_info.sources[0].rateBps = m_info.rateUpBps;
        m_info.sources[0].statusText = m_info.statusText;
        m_info.sources[0].client = m_peerAgent;
    }
    m_host->reportUpload(m_info);
}

void UploadSession::endUpload(mx::TransferState st, const QString &text)
{
    if (m_uploadId.isEmpty()) return;
    m_info.state = st;
    m_info.statusText = text;
    m_info.rateUpBps = 0;
    m_info.done = m_sentTotal;
    m_info.completed = QDateTime::currentDateTime();
    m_host->reportUpload(m_info);
    m_host->reportUploadFinished(m_uploadId);
    m_uploadId.clear();
    m_reportTimer.stop();
}

} // namespace mx::gnut
