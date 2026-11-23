// SPDX-License-Identifier: GPL-3.0-or-later
#include "http_index_provider.h"

#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QFutureWatcher>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QLoggingCategory>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <memory>

Q_DECLARE_LOGGING_CATEGORY(lcWeb)

namespace mx::web {

// ---------------------------------------------------------------- SearchBackend helpers
void SearchBackend::markOk(int latencyMs, int results)
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    m_health.state = BackendState::Ok;
    m_health.lastOk = now;
    m_health.lastAttempt = now;
    m_health.lastLatencyMs = latencyMs;
    m_health.consecutiveFailures = 0;
    m_health.backoffUntil = QDateTime();
    m_health.lastError.clear();
    m_health.lastResultCount = results;
    emit healthChanged();
}

void SearchBackend::markFailed(const QString &error, int latencyMs, int minBackoffSec)
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    m_health.lastAttempt = now;
    if (latencyMs >= 0) m_health.lastLatencyMs = latencyMs;
    m_health.lastError = error;
    m_health.consecutiveFailures++;
    // back-off: 30 s, 1 min, 2 min, ... capped at 30 min
    const int f = qMin(m_health.consecutiveFailures, 7);
    const qint64 secs = qMax<qint64>(minBackoffSec, qMin<qint64>(30LL << (f - 1), 1800));
    m_health.backoffUntil = now.addSecs(secs);
    m_health.state = BackendState::BackingOff;
    emit healthChanged();
}

// ---------------------------------------------------------------- HttpIndexProvider
HttpIndexProvider::HttpIndexProvider(const ProviderDef &def, HttpEnvironment *env, QObject *parent)
    : SearchBackend(parent), m_def(def), m_env(env)
{
    m_enabled = def.enabled;
    m_gate.setSingleShot(true);
    connect(&m_gate, &QTimer::timeout, this, &HttpIndexProvider::pumpQueue);
}

HttpIndexProvider::~HttpIndexProvider()
{
    cancelAll();
}

void HttpIndexProvider::setDef(const ProviderDef &def)
{
    m_def = def;
}

QString HttpIndexProvider::description() const
{
    return QUrl(m_def.url).host();
}

QString HttpIndexProvider::address() const
{
    return m_def.homepage.isEmpty() ? QStringLiteral("https://") + QUrl(m_def.url).host() + QLatin1Char('/') : m_def.homepage;
}

QString HttpIndexProvider::encodeQuery(const QString &text, const QString &spaceAs)
{
    QString enc = QString::fromLatin1(QUrl::toPercentEncoding(text));
    if (spaceAs != QLatin1String("%20")) enc.replace(QLatin1String("%20"), spaceAs);
    return enc;
}

QString HttpIndexProvider::categoryValue(const ProviderDef &def, MediaType t)
{
    auto it = def.categories.constFind(int(t));
    if (it != def.categories.constEnd()) return it.value();
    return def.categories.value(int(MediaType::Any));
}

void HttpIndexProvider::tagQueryCategory(const ProviderDef &def, const BackendQuery &q, QList<TorrentRecord> *records)
{
    if (!records || q.type == MediaType::Any || q.type == MediaType::Other) return;
    const auto it = def.categories.constFind(int(q.type));
    if (it == def.categories.constEnd() || it.value().isEmpty()) return;
    if (def.hasAnyCategory && it.value() == def.categories.value(int(MediaType::Any))) return;   // no real narrowing
    for (TorrentRecord &r : *records)
        if (r.type == MediaType::Any || r.type == MediaType::Other) r.type = q.type;
}

bool HttpIndexProvider::accepts(const BackendQuery &q) const
{
    if (q.type == MediaType::Any) return !m_def.strictCategories || m_def.hasAnyCategory || m_def.categories.isEmpty();
    if (m_def.strictCategories) return m_def.categories.contains(int(q.type));
    return true;
}

QString HttpIndexProvider::expandRequest(const QString &tmpl, const ProviderDef &def, const BackendQuery &q, int page, bool urlContext)
{
    QString s = tmpl;
    const QString jsonQ = [&] {
        const QString a = QString::fromUtf8(QJsonDocument(QJsonArray{q.text}).toJson(QJsonDocument::Compact));
        return a.mid(1, a.size() - 2);
    }();
    s.replace(QLatin1String("{query_json}"), jsonQ);
    s.replace(QLatin1String("{query_raw}"), q.text);
    s.replace(QLatin1String("{query}"), urlContext ? encodeQuery(q.text, def.spaceAs) : q.text);
    s.replace(QLatin1String("{page}"), QString::number(page));
    s.replace(QLatin1String("{offset}"), QString::number(qMax(0, page - def.firstPage) * qMax(1, def.pageSize)));
    s.replace(QLatin1String("{pageSize}"), QString::number(def.pageSize));
    s.replace(QLatin1String("{maxResults}"), QString::number(q.maxResults));
    s.replace(QLatin1String("{category}"), categoryValue(def, q.type));
    s.replace(QLatin1String("{adult_hide}"), q.adultFilter ? QStringLiteral("true") : QStringLiteral("false"));
    return s;
}

void HttpIndexProvider::search(const BackendQuery &q)
{
    auto *j = new Job;
    j->serial = m_nextSerial++;
    j->searchId = q.searchId;
    j->q = q;
    j->def = m_def;
    j->page = m_def.firstPage;
    j->started.start();
    m_jobs.insert(j->serial, j);
    const quint64 serial = j->serial;
    enqueue(serial, [this, serial] { if (Job *jb = job(serial)) startPage(jb); });
}

void HttpIndexProvider::probe()
{
    if (m_def.probeQuery.isEmpty()) return;
    for (Job *j : std::as_const(m_jobs))
        if (j->probe) return;   // one at a time
    BackendQuery q;
    q.text = m_def.probeQuery;
    q.maxResults = 5;
    auto *j = new Job;
    j->serial = m_nextSerial++;
    j->probe = true;
    j->q = q;
    j->def = m_def;
    j->def.maxPages = 1;
    j->def.details = DetailsSpec();
    j->page = m_def.firstPage;
    j->started.start();
    m_jobs.insert(j->serial, j);
    const quint64 serial = j->serial;
    enqueue(serial, [this, serial] { if (Job *jb = job(serial)) startPage(jb); });
}

void HttpIndexProvider::cancel(quint32 searchId)
{
    const auto jobs = m_jobs.values();
    for (Job *j : jobs) {
        if (j->probe || j->searchId != searchId) continue;
        m_jobs.remove(j->serial);
        if (j->reply) j->reply->abort();
        for (const auto &r : std::as_const(j->detailReplies)) if (r) r->abort();
        delete j;
    }
}

void HttpIndexProvider::cancelAll()
{
    const auto jobs = m_jobs.values();
    m_jobs.clear();
    m_queue.clear();
    for (Job *j : jobs) {
        if (j->reply) j->reply->abort();
        for (const auto &r : std::as_const(j->detailReplies)) if (r) r->abort();
        delete j;
    }
}

void HttpIndexProvider::enqueue(quint64 serial, std::function<void()> fn)
{
    m_queue.emplace_back(serial, std::move(fn));
    if (!m_gate.isActive()) m_gate.start(0);
}

void HttpIndexProvider::pumpQueue()
{
    // drop entries of cancelled jobs
    while (!m_queue.empty() && !m_jobs.contains(m_queue.front().first)) m_queue.pop_front();
    if (m_queue.empty()) return;
    const qint64 since = m_lastRequest.isValid() ? m_lastRequest.elapsed() : LLONG_MAX;
    if (since < m_def.minIntervalMs) {
        m_gate.start(int(m_def.minIntervalMs - since));
        return;
    }
    auto fn = std::move(m_queue.front().second);
    m_queue.pop_front();
    m_lastRequest.start();
    fn();
    if (!m_queue.empty() && !m_gate.isActive()) m_gate.start(qMax(0, m_def.minIntervalMs));
}

void meterReply(QNetworkReply *reply, mx::RateLimiter *limiter, qint64 requestBytes)
{
    if (!reply || !limiter) return;
    limiter->accountTraffic(mx::RateLimiter::Up, mx::Net::Web, requestBytes, 0);   // request line + headers (estimate)
    auto last = std::make_shared<qint64>(0);
    QObject::connect(reply, &QNetworkReply::downloadProgress, reply, [limiter, last](qint64 got, qint64) {
        if (got > *last) { limiter->accountTraffic(mx::RateLimiter::Down, mx::Net::Web, got - *last, 0); *last = got; }
    });
}

QNetworkReply *HttpIndexProvider::send(const QNetworkRequest &reqIn, const QString &method, const QByteArray &body,
                                       int maxBytes, std::function<void(QNetworkReply *, bool)> done)
{
    QNetworkRequest req = reqIn;
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setMaximumRedirectsAllowed(5);
    if (!m_env->userAgent.isEmpty()) req.setHeader(QNetworkRequest::UserAgentHeader, m_env->userAgent);
    QNetworkReply *reply = method == QLatin1String("POST") ? m_env->nam->post(req, body) : m_env->nam->get(req);
    meterReply(reply, m_env->limiter, req.url().toEncoded().size() + 300 + body.size());
    auto tooLarge = std::make_shared<bool>(false);
    connect(reply, &QNetworkReply::metaDataChanged, reply, [reply, maxBytes, tooLarge] {
        const qint64 len = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
        if (len > maxBytes) { *tooLarge = true; reply->abort(); }
    });
    connect(reply, &QNetworkReply::downloadProgress, reply, [reply, maxBytes, tooLarge](qint64 got, qint64) {
        if (got > maxBytes && !*tooLarge) { *tooLarge = true; reply->abort(); }
    });
    if (!m_env->verifyTls)
        connect(reply, &QNetworkReply::sslErrors, reply, [reply](const QList<QSslError> &) { reply->ignoreSslErrors(); });
    connect(reply, &QNetworkReply::finished, this, [reply, tooLarge, done] {
        done(reply, *tooLarge);
        reply->deleteLater();
    });
    return reply;
}

void HttpIndexProvider::startPage(Job *j)
{
    const QString url = expandRequest(j->def.url, j->def, j->q, j->page, true);
    QNetworkRequest req{QUrl(url, QUrl::TolerantMode)};
    if (!req.url().isValid()) { finishJob(j, false, QStringLiteral("invalid URL")); return; }
    req.setTransferTimeout(j->def.timeoutMs);
    for (auto it = j->def.headers.constBegin(); it != j->def.headers.constEnd(); ++it)
        req.setRawHeader(it.key().toUtf8(), it.value().toUtf8());
    QByteArray body;
    if (j->def.method == QLatin1String("POST")) {
        body = expandRequest(j->def.body, j->def, j->q, j->page, false).toUtf8();
        req.setHeader(QNetworkRequest::ContentTypeHeader,
                      j->def.contentType.isEmpty() ? QStringLiteral("application/json") : j->def.contentType);
    }
    const quint64 serial = j->serial;
    auto timer = std::make_shared<QElapsedTimer>();
    timer->start();
    qCDebug(lcWeb) << m_def.id << "GET" << req.url().toString(QUrl::RemoveUserInfo);
    j->reply = send(req, j->def.method, body, j->def.maxBytes, [this, serial, timer](QNetworkReply *r, bool tooLarge) {
        pageFinished(serial, r, tooLarge, timer->elapsed());
    });
}

void HttpIndexProvider::pageFinished(quint64 serial, QNetworkReply *reply, bool tooLarge, qint64 latency)
{
    Job *j = job(serial);
    if (!j) return;
    j->reply = nullptr;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (tooLarge) {
        const QString err = QStringLiteral("response larger than %1 bytes").arg(j->def.maxBytes);
        markFailed(err, int(latency));
        finishJob(j, j->anyOk, err);
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        const bool timedOut = reply->error() == QNetworkReply::OperationCanceledError
                              || reply->error() == QNetworkReply::TimeoutError;
        QString err = timedOut ? QStringLiteral("timed out")
                          : (status > 0 ? QStringLiteral("HTTP %1").arg(status) : reply->errorString());
        int minBackoff = 0;
        if (status == 429) {   // [diag-1] "too many requests": at least Retry-After, at least 5 minutes
            const int ra = reply->rawHeader("Retry-After").trimmed().toInt();
            minBackoff = qBound(300, ra, 3600);
        }
        markFailed(err, int(latency), minBackoff);
        qCInfo(lcWeb) << m_def.id << "failed:" << err;
        finishJob(j, j->anyOk, err);
        return;
    }
    const QByteArray body = reply->readAll();
    ProviderDef def = j->def;
    auto *watcher = new QFutureWatcher<ParseResult>(this);
    connect(watcher, &QFutureWatcher<ParseResult>::finished, this, [this, watcher, serial, latency] {
        const ParseResult res = watcher->result();
        watcher->deleteLater();
        parsed(serial, res, latency);
    });
    watcher->setFuture(QtConcurrent::run([def, body] { return parseResponse(def, body); }));
}

void HttpIndexProvider::parsed(quint64 serial, const ParseResult &res, qint64 latency)
{
    Job *j = job(serial);
    if (!j) return;
    if (!res.ok) {
        markFailed(res.error, int(latency));
        qCInfo(lcWeb) << m_def.id << "unparsable response:" << res.error;
        finishJob(j, j->anyOk, res.error);
        return;
    }
    j->anyOk = true;
    markOk(int(latency), res.records.size());
    if (res.skipped > 0) qCDebug(lcWeb) << m_def.id << "skipped" << res.skipped << "of" << res.rawItems << "rows";
    if (j->probe) { finishJob(j, true, QString()); return; }
    if (j->def.details.valid() && !res.records.isEmpty() && j->def.details.maxItems > 0) {
        runDetails(serial, res.records, res.contexts, res.rawItems);
        return;
    }
    deliver(j, res.records, res.rawItems);
}

void HttpIndexProvider::runDetails(quint64 serial, QList<TorrentRecord> records, QList<ItemContext> contexts, int rawItems)
{
    Job *j = job(serial);
    if (!j) return;
    struct State {
        QList<TorrentRecord> records;
        int pending = 0;
        int rawItems = 0;
    };
    auto st = std::make_shared<State>();
    st->records = records;
    st->rawItems = rawItems;
    const int n = qMin(j->def.details.maxItems, records.size());
    for (int i = 0; i < n; ++i) {
        const QString url = expandTemplate(j->def.details.url, contexts.at(i));
        QNetworkRequest req{QUrl(url, QUrl::TolerantMode)};
        if (!req.url().isValid()) continue;
        req.setTransferTimeout(j->def.details.timeoutMs);
        st->pending++;
        const ItemContext ctx = contexts.at(i);
        const ProviderDef def = j->def;
        j->detailReplies << send(req, QStringLiteral("GET"), QByteArray(), j->def.maxBytes,
                                 [this, serial, st, i, ctx, def](QNetworkReply *r, bool tooLarge) {
            if (!tooLarge && r->error() == QNetworkReply::NoError) {
                const QByteArray body = r->readAll();
                TorrentRecord rec = st->records.at(i);
                if (applyDetails(def, ctx, body, &rec)) st->records[i] = rec;
            }
            if (--st->pending == 0)
                if (Job *jb = job(serial)) deliver(jb, st->records, st->rawItems);
        });
    }
    if (st->pending == 0) deliver(j, st->records, st->rawItems);
}

void HttpIndexProvider::deliver(Job *j, const QList<TorrentRecord> &records, int rawItems)
{
    j->detailReplies.clear();
    j->pagesDone++;
    j->delivered += records.size();
    if (!records.isEmpty()) {
        QList<TorrentRecord> tagged = records;
        tagQueryCategory(j->def, j->q, &tagged);   // [ui-f]
        emit results(j->searchId, tagged);
    }
    const bool pageFull = j->def.pageSize > 0 ? rawItems >= j->def.pageSize : rawItems > 0;
    if (j->pagesDone < j->def.maxPages && pageFull && j->delivered < j->q.maxResults) {
        j->page++;
        const quint64 serial = j->serial;
        enqueue(serial, [this, serial] { if (Job *jb = job(serial)) startPage(jb); });
        return;
    }
    finishJob(j, true, QString());
}

void HttpIndexProvider::finishJob(Job *j, bool ok, const QString &error)
{
    m_jobs.remove(j->serial);
    const bool probe = j->probe;
    const quint32 id = j->searchId;
    delete j;
    if (!probe) emit finished(id, ok, error);
}

} // namespace mx::web
