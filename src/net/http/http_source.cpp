// SPDX-License-Identifier: GPL-3.0-or-later
#include "http_source.h"

#include <QtNetwork/QHostAddress>

#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <algorithm>

#include "http_plugin_impl.h"

namespace mx {

namespace {
QString hostKey(const QUrl &u)
{
    const QString sch = u.scheme().toLower();
    return u.host(QUrl::FullyEncoded).toLower() + QLatin1Char(':')
         + QString::number(u.port(sch == QLatin1String("https") ? 443 : 80));
}
QString fmtBytes(qint64 n) { return QString::number(n); }
} // namespace

HttpRangeSource::HttpRangeSource(HttpPlugin *plugin, const QString &downloadId, const FileHashes &hashes,
                                 qint64 expectedSize, const SearchSource &src, const QUrl &url)
    : RangeSource(nullptr), m_plugin(plugin), m_downloadId(downloadId), m_hashes(hashes),
      m_expectedSize(expectedSize > 0 ? expectedSize : -1), m_desc(src)
{
    m_desc.net = Net::Http;
    m_prevEtag = src.extra.value(QStringLiteral("etag")).toString();
    if (m_desc.connection.isEmpty()) m_desc.connection = QStringLiteral("HTTP");
    m_origUrl = url;
    m_cred = HttpCredentials::take(&m_origUrl);
    m_finalUrl = m_origUrl;
    m_desc.user.net = Net::Http;
    if (m_desc.user.id.isEmpty()) m_desc.user.id = hostKey(m_origUrl);
    if (m_desc.user.name.isEmpty()) m_desc.user.name = m_origUrl.host();
    m_cfg = plugin ? plugin->config() : HttpConfig();
    m_clock.start();
    m_tick.setInterval(500);
    connect(&m_tick, &QTimer::timeout, this, &HttpRangeSource::tick);
    m_pumpTimer.setSingleShot(true);
    connect(&m_pumpTimer, &QTimer::timeout, this, &HttpRangeSource::pumpAll);
    m_probeTimer.setSingleShot(true);
    connect(&m_probeTimer, &QTimer::timeout, this, &HttpRangeSource::startProbe);
}

HttpRangeSource::~HttpRangeSource()
{
    for (auto &q : m_reqs) releaseReply(q.get(), false);
    m_reqs.clear();
    if (m_job) m_job->abort();
    if (m_plugin) m_plugin->removeSource(this);
}

HttpRangeSource::Req *HttpRangeSource::find(quint64 id) const
{
    for (const auto &q : m_reqs)
        if (q->id == id) return q.get();
    return nullptr;
}

qint64 HttpRangeSource::effectiveSize() const
{
    return m_remoteSize >= 0 ? m_remoteSize : m_expectedSize;
}

QList<QPair<qint64, qint64>> HttpRangeSource::available() const
{
    if (m_phase != PReady) return {};
    const qint64 sz = effectiveSize();
    if (sz < 0) return {};
    return {qMakePair(qint64(0), sz)};
}

qint64 HttpRangeSource::preferredRequestSize() const
{
    if (m_supportsRanges) return kRangedRequestSize;
    const qint64 sz = effectiveSize();
    return sz > 0 ? sz : kRangedRequestSize;
}

int HttpRangeSource::concurrencyLimit() const
{
    if (!m_supportsRanges) return 1;
    return qMax(1, std::min({m_cfg.connectionsPerSource, m_cfg.maxPerHost, m_adaptiveLimit}));
}

int HttpRangeSource::maxPendingRequests() const
{
    return concurrencyLimit();
}

void HttpRangeSource::updateExtra()
{
    QVariantMap &x = m_desc.extra;
    x.insert(QStringLiteral("size"), m_remoteSize);
    x.insert(QStringLiteral("fileName"), m_fileName);
    x.insert(QStringLiteral("contentType"), m_contentType);
    x.insert(QStringLiteral("supportsRanges"), m_supportsRanges);
    x.insert(QStringLiteral("etag"), m_etag);
    x.insert(QStringLiteral("lastModified"), m_lastModified);
    x.insert(QStringLiteral("finalUrl"), m_finalUrl.toString(QUrl::FullyEncoded));
    x.insert(QStringLiteral("isTorrentFile"), m_isTorrent);
}

void HttpRangeSource::updateState()
{
    State st;
    QString text;
    const bool busy = m_busyUntilMs > now();
    const int wait = busy ? int((m_busyUntilMs - now() + 999) / 1000) : 0;
    switch (m_phase) {
    case PIdle: st = Idle; text = QStringLiteral("Idle"); break;
    case PClosed: st = Closed; text = QStringLiteral("Closed"); break;
    case PFailed: st = Failed; text = m_error; break;
    case PProbing:
        if (busy) { st = Queued; text = QStringLiteral("Server busy, retry in %1 s").arg(wait); }
        else { st = Connecting; text = m_connectNote.isEmpty() ? QStringLiteral("Connecting...") : m_connectNote; }
        break;
    case PReady:
    default:
        if (busy) { st = Queued; text = QStringLiteral("Server busy, retry in %1 s").arg(wait); }
        else if (m_reqs.empty()) { st = Ready; text = m_supportsRanges ? QStringLiteral("Ready") : QStringLiteral("Ready (no range support)"); }
        else { st = Busy; text = m_supportsRanges ? QStringLiteral("Transferring") : QStringLiteral("Transferring (no range support)"); }
        break;
    }
    if (st == m_state && text == m_stateText) return;
    m_state = st;
    m_stateText = text;
    emit stateChanged();
}

// ---------------------------------------------------------------- open / probe
void HttpRangeSource::open()
{
    if (m_phase != PIdle) return;
    m_phase = PProbing;
    m_tick.start();
    updateState();
    startProbe();
}

void HttpRangeSource::startProbe()
{
    if (m_phase != PProbing || !m_plugin) return;
    m_cfg = m_plugin->config();
    if (m_job) { m_job->abort(); m_job->deleteLater(); }
    m_job = new ProbeJob(m_plugin->nam(), m_cfg, m_cred, this);
    connect(m_job, &ProbeJob::done, this, &HttpRangeSource::onProbeDone);
    // Sniff the first bytes when we do not know what the URL is (pasted link → maybe a .torrent).
    m_job->start(ProbeJob::Probe, m_origUrl, m_expectedSize <= 0);
}

void HttpRangeSource::onProbeDone(const ProbeOutcome &o)
{
    if (m_job) { m_job->deleteLater(); m_job = nullptr; }
    if (m_phase != PProbing) return;
    const HttpProbeResult &r = o.r;
    if (!r.ok) {
        if (o.busy) {
            ++m_busyStreak;
            if (m_busyStreak > 30) { failSource(QStringLiteral("Server busy for too long: %1").arg(r.error)); return; }
            const int wait = r.retryAfterSec >= 0 ? qMax(1, r.retryAfterSec) : qMin(300, 5 << qMin(6, m_busyStreak - 1));
            m_busyUntilMs = now() + wait * 1000LL;
            m_probeTimer.start(wait * 1000);
            updateState();
            return;
        }
        if (o.permanent) { failSource(r.error); return; }
        ++m_probeAttempts;
        ++m_retries;
        if (m_probeAttempts > m_cfg.maxRetries) {
            failSource(QStringLiteral("%1 (after %2 attempts)").arg(r.error).arg(m_probeAttempts));
            return;
        }
        const int delay = qMin(60000, m_cfg.retryBaseMs << qMin(10, m_probeAttempts - 1));
        m_connectNote = QStringLiteral("Connecting... (retry %1: %2)").arg(m_probeAttempts).arg(r.error);
        m_probeTimer.start(delay);
        updateState();
        return;
    }
    m_busyStreak = 0;
    m_busyUntilMs = 0;
    m_finalUrl = r.finalUrl;
    m_remoteSize = r.size;
    m_supportsRanges = r.supportsRanges;
    m_etag = r.etag;
    m_lastModified = r.lastModified;
    m_fileName = r.fileName;
    m_contentType = r.contentType;
    m_isTorrent = r.isTorrentFile;
    updateExtra();

    if (m_expectedSize > 0 && m_remoteSize >= 0 && m_remoteSize != m_expectedSize) {
        failSource(QStringLiteral("Size mismatch: the server's file is %1 bytes, expected %2")
                       .arg(fmtBytes(m_remoteSize), fmtBytes(m_expectedSize)));
        return;
    }
    if (isStrongEtag(m_prevEtag) && isStrongEtag(m_etag) && m_prevEtag != m_etag) {
        failSource(QStringLiteral("File changed on the server since the download started (ETag %1 → %2)").arg(m_prevEtag, m_etag));
        return;
    }
    if (!r.hashes.isEmpty() && m_hashes.conflicts(r.hashes)) {
        failSource(QStringLiteral("The server's file has a different hash than this download"));
        return;
    }
    if (m_plugin && (!r.mirrors.isEmpty() || !r.hashes.isEmpty()))
        m_plugin->reportHints(m_downloadId, m_finalUrl, r.mirrors, r.hashes);
    qCInfo(lcHttp) << "source ready" << m_finalUrl.toString() << "size" << m_remoteSize << "ranges" << m_supportsRanges
                   << "etag" << m_etag;
    m_phase = PReady;
    m_connectNote.clear();
    updateState();
    emit availabilityChanged();
}

// ---------------------------------------------------------------- requests
void HttpRangeSource::deferFail(qint64 offset, qint64 length, const QString &why)
{
    const quint64 gen = m_gen;
    QTimer::singleShot(0, this, [this, gen, offset, length, why] {
        if (gen == m_gen) emit requestFinished(offset, length, false, why);
    });
}

void HttpRangeSource::request(qint64 offset, qint64 length)
{
    if (m_plugin) m_cfg = m_plugin->config();
    if (m_phase != PReady) { deferFail(offset, length, QStringLiteral("Source not ready (%1)").arg(m_stateText)); return; }
    if (offset < 0 || length <= 0) { deferFail(offset, length, QStringLiteral("Invalid range")); return; }
    const qint64 sz = effectiveSize();
    if (sz >= 0 && offset + length > sz) {
        deferFail(offset, length, QStringLiteral("Range %1+%2 outside the file (%3 bytes)").arg(offset).arg(length).arg(sz));
        return;
    }
    if (!m_supportsRanges && offset != 0) {
        deferFail(offset, length, QStringLiteral("%1 (cannot start at offset %2)").arg(QLatin1String(kHttpErrorNoRanges)).arg(offset));
        return;
    }
    if (int(m_reqs.size()) >= maxPendingRequests()) {
        deferFail(offset, length, QStringLiteral("Too many outstanding requests (max %1)").arg(maxPendingRequests()));
        return;
    }
    auto q = std::make_unique<Req>();
    q->id = m_nextId++;
    q->offset = offset;
    q->length = length;
    m_reqs.push_back(std::move(q));
    if (!m_tick.isActive()) m_tick.start();
    kick();
    updateState();
}

void HttpRangeSource::kick()
{
    if (m_phase != PReady || !m_plugin) return;
    if (m_busyUntilMs > now()) return;
    int active = 0;
    for (const auto &q : m_reqs)
        if (q->reply) ++active;
    const int limit = concurrencyLimit();
    const qint64 t = now();
    for (std::size_t i = 0; i < m_reqs.size(); ++i) {
        Req *q = m_reqs[i].get();
        if (q->reply || q->notBeforeMs > t) continue;
        if (active >= limit) break;
        if (!q->hasSlot) {
            const QString host = hostKey(m_finalUrl);
            if (!m_plugin->tryAcquireSlot(host)) break;     // plugin kicks us when one frees up
            q->hasSlot = true;
            q->slotHost = host;
        }
        q->url = m_finalUrl;
        q->redir.reset();
        startReq(q);
        ++active;
    }
}

void HttpRangeSource::startReq(Req *q)
{
    QNetworkRequest req = makeRequest(q->url, m_cfg, m_cred);
    const qint64 pos = q->offset + q->delivered;
    const qint64 end = q->offset + q->length - 1;
    q->ranged = m_supportsRanges;
    if (q->ranged) {
        req.setRawHeader("Range", "bytes=" + QByteArray::number(pos) + '-' + QByteArray::number(end));
        if (isStrongEtag(m_etag)) req.setRawHeader("If-Range", m_etag.toLatin1());
        else if (!m_lastModified.isEmpty()) req.setRawHeader("If-Range", m_lastModified.toLatin1());
        q->skip = 0;
    } else {
        q->skip = pos;          // sequential: re-read from 0 and discard what was already delivered
    }
    q->headersDone = q->replyFinished = q->exactRange = false;
    q->status = 0;
    q->startedMs = q->lastNetMs = now();
    QNetworkReply *r = m_plugin->nam()->get(req);
    ++m_connectionsOpened;
    r->setReadBufferSize(kReadBuffer);
    prepareReply(r, m_cfg);
    q->reply = r;
    const quint64 id = q->id;
    connect(r, &QNetworkReply::metaDataChanged, this, [this, id] { onMeta(id); });
    connect(r, &QNetworkReply::readyRead, this, [this, id] { onReadyRead(id); });
    connect(r, &QNetworkReply::finished, this, [this, id] { onFinished(id); });
}

void HttpRangeSource::releaseReply(Req *q, bool graceful)
{
    if (q->reply) {
        QNetworkReply *r = q->reply;
        q->reply = nullptr;
        if (graceful && r->isRunning()) {
            // All wanted bytes are in; let Qt finish the (exact) response so the connection is
            // reused, but never deliver anything more from it.
            r->disconnect(this);
            QObject::connect(r, &QNetworkReply::finished, r, &QObject::deleteLater);
            QTimer::singleShot(10000, r, [r] { if (r->isRunning()) r->abort(); });
        } else {
            dropReply(r);
        }
    }
    if (q->hasSlot) {
        q->hasSlot = false;
        if (m_plugin) m_plugin->releaseSlot(q->slotHost);
    }
}

void HttpRangeSource::removeReq(Req *q)
{
    releaseReply(q, false);
    m_reqs.erase(std::remove_if(m_reqs.begin(), m_reqs.end(), [q](const std::unique_ptr<Req> &p) { return p.get() == q; }),
                 m_reqs.end());
}

void HttpRangeSource::onMeta(quint64 id)
{
    Req *q = find(id);
    if (!q || !q->reply || q->headersDone) return;
    const int st = statusOf(q->reply);
    if (st == 0) return;
    q->status = st;
    if (m_server.isEmpty()) m_server = QString::fromLatin1(q->reply->rawHeader("Server")).left(60);
    if (isRedirectStatus(st)) { q->headersDone = true; return; }   // followed in onFinished
    validateHeaders(q);
}

bool HttpRangeSource::validateHeaders(Req *q)
{
    q->headersDone = true;
    QNetworkReply *r = q->reply;
    const int st = q->status;
    const QString etag = QString::fromLatin1(r->rawHeader("ETag")).trimmed();
    const qint64 sz = effectiveSize();
    const qint64 pos = q->offset + q->delivered;
    const qint64 end = q->offset + q->length - 1;

    if ((st == 200 || st == 206) && isStrongEtag(m_etag) && isStrongEtag(etag) && etag != m_etag) {
        failSource(QStringLiteral("File changed on the server (ETag %1 → %2)").arg(m_etag, etag));
        return false;
    }
    if ((st == 200 || st == 206) && m_etag.isEmpty() && !m_lastModified.isEmpty()) {
        const QString lm = QString::fromLatin1(r->rawHeader("Last-Modified")).trimmed();
        if (!lm.isEmpty() && lm != m_lastModified) {
            failSource(QStringLiteral("File changed on the server (Last-Modified %1 → %2)").arg(m_lastModified, lm));
            return false;
        }
    }
    if (st == 206) {
        const ContentRange cr = parseContentRange(r->rawHeader("Content-Range"));
        if (!cr.valid || cr.unsatisfied) {
            failSource(QStringLiteral("Server sent an invalid Content-Range: \"%1\"")
                           .arg(QString::fromLatin1(r->rawHeader("Content-Range").left(80))));
            return false;
        }
        if (cr.total >= 0 && sz >= 0 && cr.total != sz) {
            failSource(QStringLiteral("File changed on the server (size %1 → %2)").arg(sz).arg(cr.total));
            return false;
        }
        if (!q->ranged || cr.first > pos || cr.last < pos) {
            failSource(QStringLiteral("Server sent the wrong range (bytes %1-%2, requested %3-%4)")
                           .arg(cr.first).arg(cr.last).arg(pos).arg(end));
            return false;
        }
        q->skip = pos - cr.first;
        q->exactRange = cr.first == pos && cr.last == end;
        return true;
    }
    if (st == 200) {
        const qint64 cl = contentLengthOf(r);
        if (sz >= 0 && cl >= 0 && cl != sz) {
            failSource(QStringLiteral("File changed on the server (size %1 → %2)").arg(sz).arg(cl));
            return false;
        }
        if (q->ranged) {
            // The server ignored our Range (or If-Range did not match without a usable ETag):
            // from now on this source is a sequential, no-range source.
            qCInfo(lcHttp) << "server answered 200 to a range request; no range support:" << m_finalUrl.toString();
            markNoRanges();
            if (pos != 0) {
                failReq(q, QStringLiteral("%1 (server answered a range request with the whole file)")
                               .arg(QLatin1String(kHttpErrorNoRanges)));
                return false;
            }
            q->ranged = false;
            q->skip = 0;
        }
        return true;
    }
    if (st == 416) {
        const ContentRange cr = parseContentRange(r->rawHeader("Content-Range"));
        if (cr.valid && cr.total >= 0 && sz >= 0 && cr.total != sz) {
            failSource(QStringLiteral("File changed on the server (size %1 → %2)").arg(sz).arg(cr.total));
            return false;
        }
        failReq(q, QStringLiteral("Server rejected the range %1-%2 (HTTP 416)").arg(pos).arg(end));
        return false;
    }
    if (st == 429 || st == 503) {
        busyReq(q, parseRetryAfter(r->rawHeader("Retry-After")));
        return false;
    }
    if (st >= 500) {
        retryReq(q, describeError(r));
        return false;
    }
    if ((st == 403 || st == 404 || st == 410) && !m_reresolved && m_finalUrl != m_origUrl) {
        // The redirect target may have expired (signed / per-node CDN URLs): resolve again once.
        qCInfo(lcHttp) << "HTTP" << st << "from" << m_finalUrl.toString() << "→ re-resolving" << m_origUrl.toString();
        m_reresolved = true;
        m_finalUrl = m_origUrl;
        retryReq(q, QStringLiteral("HTTP %1, re-resolving").arg(st), false);
        return false;
    }
    if (st == 404) { failSource(QStringLiteral("File not found (HTTP 404)")); return false; }
    if (st == 410) { failSource(QStringLiteral("File gone (HTTP 410)")); return false; }
    if (st == 403) { failSource(QStringLiteral("Access denied (HTTP 403)")); return false; }
    failSource(st >= 400 ? describeError(r) : QStringLiteral("Unexpected HTTP status %1").arg(st));
    return false;
}

void HttpRangeSource::markNoRanges()
{
    if (!m_supportsRanges) return;
    m_supportsRanges = false;
    updateExtra();
    // Requests that cannot be served any more (offset != 0) fail now.
    std::vector<std::pair<qint64, qint64>> dead;
    for (std::size_t i = 0; i < m_reqs.size();) {
        Req *q = m_reqs[i].get();
        if (q->offset + q->delivered != 0 && !q->reply) {
            dead.emplace_back(q->offset, q->length);
            removeReq(q);
        } else {
            ++i;
        }
    }
    for (const auto &d : dead) deferFail(d.first, d.second, QStringLiteral("%1 (cannot start at offset %2)")
                                                                .arg(QLatin1String(kHttpErrorNoRanges)).arg(d.first));
    emit availabilityChanged();
    updateState();
}

void HttpRangeSource::onReadyRead(quint64 id)
{
    Req *q = find(id);
    if (!q || !q->reply) return;
    q->lastNetMs = now();
    if (!q->headersDone) {
        onMeta(id);
        q = find(id);
        if (!q || !q->reply) return;
    }
    if (isRedirectStatus(q->status) || !q->headersDone) {
        q->reply->readAll();     // body of the redirect: ignore
        return;
    }
    pump(id);
}

void HttpRangeSource::pumpAll()
{
    std::vector<quint64> ids;
    for (const auto &q : m_reqs)
        if (q->reply && q->headersDone) ids.push_back(q->id);
    for (quint64 id : ids) pump(id);
}

void HttpRangeSource::pump(quint64 id)
{
    RateLimiter *lim = m_plugin ? m_plugin->limiter() : nullptr;
    for (;;) {
        Req *q = find(id);
        if (!q || !q->reply || !q->headersDone || isRedirectStatus(q->status)) return;
        QNetworkReply *r = q->reply;
        qint64 buffered = 0;
        for (const auto &o : m_reqs)
            if (o->reply) buffered += o->reply->bytesAvailable();
        m_peakBuffered = qMax(m_peakBuffered, buffered);
        const qint64 avail = r->bytesAvailable();
        if (q->delivered >= q->length) { completeReq(q, q->length); return; }
        if (avail <= 0) {
            if (q->replyFinished) onReplyDone(q);
            return;
        }
        const qint64 remaining = q->length - q->delivered;
        const qint64 want = q->skip > 0 ? std::min({q->skip, avail, kMaxChunk}) : std::min({avail, remaining, kMaxChunk});
        // [bw-1] a LAN server (address literal or localhost) is exempt unless bandwidth/limitLan
        const QString host = q->url.host();
        const bool exempt = lim && lim->exempt(host == QLatin1String("localhost") ? QHostAddress(QHostAddress::LocalHost) : QHostAddress(host));
        const qint64 got = lim && !exempt ? lim->acquire(RateLimiter::Down, want) : want;
        auto waitForTokens = [&] {
            if (!m_pumpTimer.isActive()) {
                const int rate = lim ? lim->limitBps(RateLimiter::Down) : 0;
                const int delay = rate > 0 ? int(qBound(qint64(5), 1000 * std::min<qint64>(want, 32768) / rate, qint64(250))) : 20;
                m_pumpTimer.start(delay);
            }
        };
        if (got <= 0) {
            waitForTokens();
            return;
        }
        // [bw-1] a partial grant means the shared bucket is empty now: deliver it, then wait for the
        // timer (looping on would take the few bytes that trickle in per pass and never return to
        // the event loop)
        const bool starved = got < want;
        const QByteArray bytes = r->read(got);
        if (bytes.isEmpty()) return;
        if (lim && exempt) lim->accountExempt(RateLimiter::Down, Net::Http, bytes.size(), bytes.size());
        else if (lim) lim->accountTraffic(RateLimiter::Down, Net::Http, bytes.size(), bytes.size());   // [glue-3]
        q->lastNetMs = now();
        if (q->skip > 0) {
            q->skip -= bytes.size();
            if (starved) { waitForTokens(); return; }
            continue;
        }
        const qint64 at = q->offset + q->delivered;
        q->delivered += bytes.size();
        m_maxChunk = qMax<qint64>(m_maxChunk, bytes.size());
        m_failStreak = 0;
        m_busyStreak = 0;
        const quint64 gen = m_gen;
        emit data(at, bytes);
        if (gen != m_gen) return;           // cancelled / closed / failed from the slot
        if (starved) {
            if (q->delivered < q->length) waitForTokens();
            else completeReq(q, q->length);
            return;
        }
    }
}

void HttpRangeSource::onFinished(quint64 id)
{
    Req *q = find(id);
    if (!q || !q->reply) return;
    q->replyFinished = true;
    q->lastNetMs = now();
    if (!q->headersDone) {
        onMeta(id);
        q = find(id);
        if (!q || !q->reply) return;
    }
    if (isRedirectStatus(q->status)) {
        QString err;
        const QUrl next = q->redir.next(q->url, q->reply, m_cfg, &err);
        if (!next.isValid()) { failSource(err); return; }
        QNetworkReply *old = q->reply;
        q->reply = nullptr;
        dropReply(old);
        q->url = next;
        startReq(q);            // keeps its host slot
        return;
    }
    if (!q->headersDone) {      // network error before any status line
        onReplyDone(q);
        return;
    }
    pump(id);                   // drains what is buffered; calls onReplyDone() when empty
}

void HttpRangeSource::onReplyDone(Req *q)
{
    if (q->delivered >= q->length) { completeReq(q, q->length); return; }
    QNetworkReply *r = q->reply;
    const auto err = r ? r->error() : QNetworkReply::UnknownNetworkError;
    if (err == QNetworkReply::SslHandshakeFailedError) { failSource(describeError(r)); return; }
    if (err != QNetworkReply::NoError && err != QNetworkReply::OperationCanceledError
        && !isTransientNetworkError(err) && q->status == 0) {
        failSource(describeError(r));
        return;
    }
    if (err == QNetworkReply::NoError && effectiveSize() < 0 && !q->ranged && q->offset == 0) {
        // Size unknown until EOF: the body ended cleanly, that's the whole file.
        m_remoteSize = q->delivered;
        updateExtra();
        completeReq(q, q->delivered);
        emit availabilityChanged();
        return;
    }
    const QString why = err != QNetworkReply::NoError
        ? describeError(r)
        : QStringLiteral("Connection closed early (%1 of %2 bytes)").arg(q->delivered).arg(q->length);
    retryReq(q, why);
}

void HttpRangeSource::completeReq(Req *q, qint64 reportedLength)
{
    const qint64 off = q->offset;
    const qint64 len = reportedLength;
    const bool graceful = q->ranged && q->exactRange;
    releaseReply(q, graceful);
    removeReq(q);
    kick();
    const quint64 gen = m_gen;
    emit requestFinished(off, len, true, QString());
    if (gen == m_gen) updateState();
}

void HttpRangeSource::failReq(Req *q, const QString &why)
{
    const qint64 off = q->offset, len = q->length;
    removeReq(q);
    kick();
    const quint64 gen = m_gen;
    emit requestFinished(off, len, false, why);
    if (gen == m_gen) updateState();
}

void HttpRangeSource::retryReq(Req *q, const QString &why, bool countAttempt)
{
    releaseReply(q, false);
    if (countAttempt) {
        ++q->attempts;
        ++m_retries;
    }
    if (q->attempts > m_cfg.maxRetries) {
        ++m_failStreak;
        const QString err = QStringLiteral("%1 (gave up after %2 attempts)").arg(why).arg(q->attempts);
        if (m_failStreak >= 3) { failSource(err); return; }
        failReq(q, err);
        return;
    }
    const int delay = countAttempt ? qMin(60000, m_cfg.retryBaseMs << qMin(10, q->attempts - 1)) : 0;
    q->notBeforeMs = now() + delay;
    qCInfo(lcHttp) << "retry" << q->attempts << "in" << delay << "ms:" << why << m_finalUrl.toString()
                   << "at" << (q->offset + q->delivered);
    if (delay == 0) QTimer::singleShot(0, this, &HttpRangeSource::kick);
}

void HttpRangeSource::busyReq(Req *q, int retryAfterSec)
{
    releaseReply(q, false);
    int transferring = 0;
    for (const auto &o : m_reqs)
        if (o.get() != q && o->reply && o->headersDone && (o->status == 200 || o->status == 206)) ++transferring;
    if (transferring > 0) {
        // The server limits connections per client: keep what works, retry this one later.
        m_adaptiveLimit = qMax(1, transferring);
        q->notBeforeMs = now() + (retryAfterSec >= 0 ? qMax(1, retryAfterSec) : 5) * 1000LL;
        qCInfo(lcHttp) << "server busy with" << transferring << "connections; limiting to" << m_adaptiveLimit;
        updateState();
        return;
    }
    ++m_busyStreak;
    if (m_busyStreak > 30) { failSource(QStringLiteral("Server busy for too long")); return; }
    const int wait = retryAfterSec >= 0 ? qMax(1, retryAfterSec) : qMin(300, 5 << qMin(6, m_busyStreak - 1));
    m_busyUntilMs = now() + wait * 1000LL;
    q->notBeforeMs = m_busyUntilMs;
    updateState();
}

void HttpRangeSource::failSource(const QString &why)
{
    qCWarning(lcHttp) << "source failed:" << why << m_origUrl.toString();
    ++m_gen;
    std::vector<std::pair<qint64, qint64>> pending;
    for (auto &q : m_reqs) {
        pending.emplace_back(q->offset, q->length);
        releaseReply(q.get(), false);
    }
    m_reqs.clear();
    m_pumpTimer.stop();
    m_probeTimer.stop();
    m_tick.stop();
    if (m_job) { m_job->abort(); m_job->deleteLater(); m_job = nullptr; }
    m_busyUntilMs = 0;
    m_phase = PFailed;
    m_error = why;
    const quint64 gen = m_gen;
    for (const auto &p : pending) {
        emit requestFinished(p.first, p.second, false, why);
        if (gen != m_gen) return;
    }
    updateState();
}

void HttpRangeSource::tick()
{
    if (m_phase == PFailed || m_phase == PClosed) { m_tick.stop(); return; }
    const qint64 t = now();
    if (m_busyUntilMs && m_busyUntilMs <= t) {
        m_busyUntilMs = 0;
        kick();
    }
    std::vector<quint64> ids;
    for (const auto &q : m_reqs) ids.push_back(q->id);
    for (quint64 id : ids) {
        Req *q = find(id);
        if (!q || !q->reply || q->replyFinished) continue;
        if (!q->headersDone) {
            if (t - q->startedMs > m_cfg.timeoutSec * 1000LL)
                retryReq(q, QStringLiteral("No response from the server within %1 s").arg(m_cfg.timeoutSec));
        } else if (q->reply->bytesAvailable() == 0 && t - q->lastNetMs > m_cfg.stallSec * 1000LL) {
            retryReq(q, QStringLiteral("Stalled: no data for %1 s").arg(m_cfg.stallSec));
        }
    }
    kick();
    updateState();
}

void HttpRangeSource::cancelRequests()
{
    ++m_gen;
    for (auto &q : m_reqs) releaseReply(q.get(), false);
    m_reqs.clear();
    m_pumpTimer.stop();
    updateState();
}

void HttpRangeSource::close()
{
    if (m_phase == PClosed) return;
    ++m_gen;
    for (auto &q : m_reqs) releaseReply(q.get(), false);
    m_reqs.clear();
    m_pumpTimer.stop();
    m_probeTimer.stop();
    m_tick.stop();
    if (m_job) { m_job->abort(); m_job->deleteLater(); m_job = nullptr; }
    m_busyUntilMs = 0;
    m_phase = PClosed;
    updateState();
}

} // namespace mx
