// SPDX-License-Identifier: GPL-3.0-or-later
#include "http_jobs.h"

#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <chrono>

namespace mx {

ProbeJob::ProbeJob(QNetworkAccessManager *nam, const HttpConfig &cfg, const HttpCredentials &cred, QObject *parent)
    : QObject(parent), m_nam(nam), m_cfg(cfg), m_cred(cred)
{
    m_overall.setSingleShot(true);
    connect(&m_overall, &QTimer::timeout, this, [this] {
        finish(false, QStringLiteral("Timed out after %1 s").arg(m_overall.interval() / 1000));
    });
}

ProbeJob::~ProbeJob()
{
    dropReply(m_reply, this);
}

void ProbeJob::abort()
{
    m_finished = true;
    m_overall.stop();
    dropReply(m_reply, this);
    m_reply = nullptr;
}

QNetworkReply *ProbeJob::track(QNetworkReply *r)
{
    dropReply(m_reply, this);
    m_reply = r;
    prepareReply(r, m_cfg);
    return r;
}

void ProbeJob::start(Mode mode, const QUrl &url, bool sniff, qint64 maxBytes)
{
    m_mode = mode;
    m_wantSniff = sniff;
    m_maxBytes = maxBytes;
    m_origUrl = m_url = url;
    m_out = ProbeOutcome();
    m_out.r.finalUrl = url;
    m_finished = false;
    m_overall.start(qMax(5, m_cfg.timeoutSec * (mode == Fetch ? 6 : 3)) * 1000);
    if (mode == Fetch) sendGet(url);
    else sendHead(url);
}

void ProbeJob::sendHead(const QUrl &u)
{
    m_url = u;
    QNetworkRequest req = makeRequest(u, m_cfg, m_cred);
    req.setRawHeader("Want-Digest", "sha-256, sha;q=0.8, md5;q=0.5");
    req.setRawHeader("Want-Repr-Digest", "sha-256=9, sha=5, md5=3");
    req.setTransferTimeout(std::chrono::seconds(m_cfg.timeoutSec));
    QNetworkReply *r = track(m_nam->head(req));
    connect(r, &QNetworkReply::finished, this, &ProbeJob::onHeadFinished);
}

void ProbeJob::sendGet(const QUrl &u)
{
    m_url = u;
    m_getMetaDone = false;
    m_getStatus = 0;
    m_out.sniff.clear();
    m_out.body.clear();
    QNetworkRequest req = makeRequest(u, m_cfg, m_cred);
    if (m_mode == Probe) {
        const int n = m_wantSniff ? 64 : 1;
        req.setRawHeader("Range", "bytes=0-" + QByteArray::number(n - 1));
        req.setRawHeader("Want-Digest", "sha-256, sha;q=0.8, md5;q=0.5");
    }
    req.setTransferTimeout(std::chrono::seconds(m_cfg.timeoutSec));
    QNetworkReply *r = track(m_nam->get(req));
    r->setReadBufferSize(256 * 1024);
    connect(r, &QNetworkReply::metaDataChanged, this, &ProbeJob::onGetMeta);
    connect(r, &QNetworkReply::readyRead, this, &ProbeJob::onGetData);
    connect(r, &QNetworkReply::finished, this, &ProbeJob::onGetFinished);
}

void ProbeJob::absorb(QNetworkReply *r, bool wholeEntity)
{
    HttpProbeResult &o = m_out.r;
    const QString et = QString::fromLatin1(r->rawHeader("ETag")).trimmed();
    if (!et.isEmpty()) o.etag = et;
    const QString lm = QString::fromLatin1(r->rawHeader("Last-Modified")).trimmed();
    if (!lm.isEmpty()) o.lastModified = lm;
    const QString ct = contentTypeOf(r);
    if (!ct.isEmpty()) o.contentType = ct;
    const QString cd = parseContentDisposition(r->rawHeader("Content-Disposition"));
    if (!cd.isEmpty()) m_cdName = cd;
    for (const QString &m : parseDuplicateLinks(r->rawHeader("Link"), r->url()))
        if (!o.mirrors.contains(m)) o.mirrors << m;
    o.hashes.mergeFrom(parseHashHeaders(r, wholeEntity));
}

bool ProbeJob::handleErrorStatus(int st, QNetworkReply *r)
{
    HttpProbeResult &o = m_out.r;
    o.httpStatus = st;
    if (st == 429 || st == 503) {
        o.retryAfterSec = parseRetryAfter(r->rawHeader("Retry-After"));
        m_out.busy = true;
        finish(false, st == 429 ? QStringLiteral("Server busy (HTTP 429 Too Many Requests)")
                                : QStringLiteral("Server busy (HTTP 503 Service Unavailable)"));
        return true;
    }
    if (st == 404 || st == 410) { finish(false, st == 404 ? QStringLiteral("File not found (HTTP 404)") : QStringLiteral("File gone (HTTP 410)"), true); return true; }
    if (st == 401 || st == 407) { finish(false, QStringLiteral("Authentication required (HTTP %1)").arg(st), true); return true; }
    return false;
}

void ProbeJob::onHeadFinished()
{
    QNetworkReply *r = m_reply;
    if (!r || m_finished) return;
    const int st = statusOf(r);
    if (isRedirectStatus(st)) {
        absorb(r, false);
        QString err;
        const QUrl next = m_redir.next(m_url, r, m_cfg, &err);
        if (!next.isValid()) { finish(false, err, true); return; }
        sendHead(next);
        return;
    }
    if (st >= 200 && st < 300) {
        m_headDone = true;
        HttpProbeResult &o = m_out.r;
        o.httpStatus = st;
        o.finalUrl = m_url;
        absorb(r, true);
        o.size = contentLengthOf(r);
        const QByteArray ar = r->rawHeader("Accept-Ranges").trimmed().toLower();
        if (ar.contains("bytes")) { o.supportsRanges = true; m_rangesKnown = true; }
        else if (ar == "none") { o.supportsRanges = false; m_rangesKnown = true; }
        // A GET is still needed to sniff the content, to confirm range support, or to learn the
        // size (some servers omit Content-Length on HEAD).
        if (m_wantSniff || !m_rangesKnown || o.size < 0) { sendGet(m_url); return; }
        finish(true);
        return;
    }
    if (st > 0 && handleErrorStatus(st, r)) return;
    if (r->error() == QNetworkReply::SslHandshakeFailedError) { finish(false, describeError(r), true); return; }
    // HEAD refused (405/501/403/400, connection closed ...) → try a tiny ranged GET instead.
    qCDebug(lcHttp) << "HEAD refused" << st << r->error() << "→ GET fallback" << m_url;
    sendGet(m_url);
}

void ProbeJob::onGetMeta()
{
    QNetworkReply *r = m_reply;
    if (!r || m_finished || m_getMetaDone) return;
    const int st = statusOf(r);
    if (st == 0) return;
    m_getMetaDone = true;
    m_getStatus = st;
    if (isRedirectStatus(st)) return;                 // handled when finished
    HttpProbeResult &o = m_out.r;
    if (st == 206 || st == 200) {
        o.httpStatus = st;
        o.finalUrl = m_url;
        if (st == 206) {
            const ContentRange cr = parseContentRange(r->rawHeader("Content-Range"));
            o.supportsRanges = true;
            if (cr.valid && cr.total >= 0) o.size = cr.total;
            absorb(r, false);
        } else {
            if (m_mode == Probe) o.supportsRanges = false;
            else o.supportsRanges = r->rawHeader("Accept-Ranges").toLower().contains("bytes");
            const qint64 cl = contentLengthOf(r);
            if (cl >= 0) o.size = cl;
            absorb(r, true);
            if (m_mode == Fetch && m_maxBytes > 0 && cl > m_maxBytes) {
                finish(false, QStringLiteral("Resource too large (%1 bytes, limit %2)").arg(cl).arg(m_maxBytes), true);
                return;
            }
        }
        return;
    }
    if (st == 416 && m_mode == Probe) {
        const ContentRange cr = parseContentRange(r->rawHeader("Content-Range"));
        o.httpStatus = st;
        o.finalUrl = m_url;
        if (cr.valid && cr.total == 0) { o.size = 0; o.supportsRanges = true; absorb(r, true); finish(true); return; }
        finish(false, QStringLiteral("Server rejected the range request (HTTP 416)"), true);
        return;
    }
    if (handleErrorStatus(st, r)) return;
    if (st >= 400) {
        o.httpStatus = st;
        finish(false, describeError(r), st < 500);
    }
}

void ProbeJob::onGetData()
{
    QNetworkReply *r = m_reply;
    if (!r || m_finished) return;
    if (!m_getMetaDone) onGetMeta();
    if (m_finished || isRedirectStatus(m_getStatus)) return;
    if (m_getStatus != 200 && m_getStatus != 206) { r->readAll(); return; }
    if (m_mode == Probe) {
        const int want = m_wantSniff ? 64 : 1;
        m_out.sniff += r->read(want - m_out.sniff.size());
        if (m_out.sniff.size() >= want) finish(true);
        return;
    }
    m_out.body += r->readAll();
    if (m_maxBytes > 0 && m_out.body.size() > m_maxBytes) {
        m_out.body.clear();
        finish(false, QStringLiteral("Resource too large (more than %1 bytes)").arg(m_maxBytes), true);
    }
}

void ProbeJob::onGetFinished()
{
    QNetworkReply *r = m_reply;
    if (!r || m_finished) return;
    if (!m_getMetaDone) onGetMeta();
    if (m_finished) return;
    if (isRedirectStatus(m_getStatus)) {
        absorb(r, false);
        QString err;
        const QUrl next = m_redir.next(m_url, r, m_cfg, &err);
        if (!next.isValid()) { finish(false, err, true); return; }
        sendGet(next);
        return;
    }
    onGetData();
    if (m_finished) return;
    if (r->error() != QNetworkReply::NoError && r->error() != QNetworkReply::OperationCanceledError) {
        const bool perm = r->error() == QNetworkReply::SslHandshakeFailedError || !isTransientNetworkError(r->error());
        finish(false, describeError(r), perm);
        return;
    }
    if (m_getStatus == 200 || m_getStatus == 206) { finish(true); return; }
    finish(false, m_getStatus ? QStringLiteral("Unexpected HTTP %1").arg(m_getStatus) : describeError(r));
}

void ProbeJob::finish(bool ok, const QString &error, bool permanent)
{
    if (m_finished) return;
    m_finished = true;
    m_overall.stop();
    if (m_reply) {
        QNetworkReply *r = m_reply;
        m_reply = nullptr;
        dropReply(r, this);
    }
    HttpProbeResult &o = m_out.r;
    o.ok = ok;
    o.error = ok ? QString() : error;
    m_out.permanent = !ok && permanent;
    QString name = m_cdName;
    if (name.isEmpty()) name = fileNameFromUrl(o.finalUrl);
    if (name.isEmpty()) name = fileNameFromUrl(m_origUrl);
    if (name.isEmpty()) name = sanitizeFileName(o.finalUrl.host());
    o.fileName = name;
    const QByteArray head = m_mode == Fetch ? m_out.body.left(96) : m_out.sniff;
    o.isTorrentFile = o.contentType == QLatin1String("application/x-bittorrent")
                   || name.endsWith(QLatin1String(".torrent"), Qt::CaseInsensitive)
                   || looksLikeTorrentData(head);
    if (m_mode == Fetch && ok && o.size < 0) o.size = m_out.body.size();
    const ProbeOutcome out = m_out;
    emit done(out);
}

// ---------------------------------------------------------------- public HttpProbe
HttpProbe::HttpProbe(Settings *settings, QObject *parent)
    : QObject(parent), m_settings(settings)
{
    m_nam = new QNetworkAccessManager(this);
    configureManager(m_nam, settings);
}

HttpProbe::~HttpProbe() = default;

void HttpProbe::cleanup()
{
    if (m_job) {
        static_cast<ProbeJob *>(m_job.data())->abort();
        m_job->deleteLater();
        m_job = nullptr;
    }
}

void HttpProbe::abort() { cleanup(); }

QUrl HttpProbe::normalizeUrl(const QString &userInput)
{
    const QString t = userInput.trimmed();
    QUrl u = t.contains(QLatin1String("://")) ? parseUrl(t) : QUrl::fromUserInput(t);
    const QString sch = u.scheme().toLower();
    if (!u.isValid() || u.host().isEmpty() || (sch != QLatin1String("http") && sch != QLatin1String("https"))) return QUrl();
    return u.adjusted(QUrl::RemoveFragment);
}

bool HttpProbe::looksLikeTorrent(const QByteArray &head) { return looksLikeTorrentData(head); }

static void startJob(HttpProbe *self, QPointer<QObject> &jobSlot, QNetworkAccessManager *nam, Settings *settings,
                     const QString &url, ProbeJob::Mode mode, qint64 maxBytes, bool autoDelete)
{
    QUrl u = HttpProbe::normalizeUrl(url);
    HttpConfig cfg = HttpConfig::load(settings);
    const HttpCredentials cred = HttpCredentials::take(&u);
    auto *job = new ProbeJob(nam, cfg, cred, self);
    jobSlot = job;
    QObject::connect(job, &ProbeJob::done, self, [self, job, mode, autoDelete](const ProbeOutcome &o) {
        job->deleteLater();
        if (mode == ProbeJob::Fetch) emit self->fetched(o.r, o.body);
        else emit self->finished(o.r);
        if (autoDelete) self->deleteLater();
    });
    if (!u.isValid()) {
        QMetaObject::invokeMethod(job, [job, url] {
            ProbeOutcome o;
            o.r.error = QStringLiteral("Not an http(s) URL: %1").arg(url.left(200));
            o.permanent = true;
            emit job->done(o);
        }, Qt::QueuedConnection);
        return;
    }
    job->start(mode, u, true, maxBytes);
}

void HttpProbe::probe(const QString &url)
{
    cleanup();
    startJob(this, m_job, m_nam, m_settings, url, ProbeJob::Probe, 0, m_autoDelete);
}

void HttpProbe::fetchSmall(const QString &url, qint64 maxBytes)
{
    cleanup();
    startJob(this, m_job, m_nam, m_settings, url, ProbeJob::Fetch, maxBytes, m_autoDelete);
}

} // namespace mx
