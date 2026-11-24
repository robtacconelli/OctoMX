// SPDX-License-Identifier: GPL-3.0-or-later
#include "web_plugin.h"

#include <QtCore/QDir>
#include <QtCore/QRegularExpression>

#include <algorithm>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QLoggingCategory>
#include <QtCore/QSaveFile>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QNetworkReply>

#include "mx/net_policy.h"
#include "mx/settings_keys.h"
#include "transforms.h"
#include "web_search_plugin.h"

Q_LOGGING_CATEGORY(lcWeb, "mx.web")

static void initWebResources()
{
    Q_INIT_RESOURCE(web);
}

namespace mx {

NetworkPlugin *createWebSearchPlugin(QObject *parent)
{
    return new web::WebSearchPlugin(parent);
}

namespace web {

static constexpr int kRecentOkSecs = 30 * 60;

QString defaultUserAgent()
{
    // Several index sites refuse unknown agents; identify as a current desktop browser.
    return QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:128.0) Gecko/20100101 Firefox/128.0");
}

WebSearchPlugin::WebSearchPlugin(QObject *parent) : NetworkPlugin(parent)
{
    initWebResources();
    qRegisterMetaType<mx::web::TorrentRecord>();
    m_nam = new QNetworkAccessManager(this);
    m_nam->setAutoDeleteReplies(false);
    m_env.nam = m_nam;
    m_env.userAgent = defaultUserAgent();
    m_probeTimer.setInterval(10 * 60 * 1000);
    connect(&m_probeTimer, &QTimer::timeout, this, &WebSearchPlugin::probeStale);
    m_statusTimer.setSingleShot(true);
    m_statusTimer.setInterval(200);
    connect(&m_statusTimer, &QTimer::timeout, this, [this] {
        const NetworkStatus st = status();
        emit statusChanged(st);
        emit serversChanged();
    });
}

WebSearchPlugin::~WebSearchPlugin()
{
    for (ActiveSearch *s : std::as_const(m_searches)) delete s;
    m_searches.clear();
    for (SearchBackend *b : std::as_const(m_backends)) b->cancelAll();
}

void WebSearchPlugin::init(const PluginContext &ctx)
{
    m_ctx = ctx;
    m_env.limiter = ctx.limiter;   // [glue-3]
    if (!m_ctx.dataDir.isEmpty()) QDir().mkpath(m_ctx.dataDir);
    if (m_ctx.settings) {
        connect(m_ctx.settings, &Settings::changed, this, [this](const QString &key, const QVariant &) {
            if (key.startsWith(QLatin1String("connection/proxy")) || key == wkeys::UserAgent
                || key == QLatin1String("connection/bindInterface") || key == QLatin1String("connection/ipv6")
                || key == QLatin1String(keys::http::VerifyTls))
                applyNetworkSettings();
        });
    }
    applyNetworkSettings();
    reloadProviders();
}

void WebSearchPlugin::applyNetworkSettings()
{
    Settings *s = m_ctx.settings;
    if (!s) return;
    const QString ua = s->s(wkeys::UserAgent).trimmed();
    m_env.userAgent = ua.isEmpty() ? defaultUserAgent() : ua;
    const QVariant vt = s->value(keys::http::VerifyTls);
    m_env.verifyTls = vt.isValid() ? vt.toBool() : true;

    // [net-2] the proxy, or the chosen adapter (mx/net_policy.h); never direct when either is set
    mx::netpolicy::applyTo(m_nam, s);
}

QString WebSearchPlugin::providersDir() const
{
    return m_ctx.dataDir.isEmpty() ? QString() : m_ctx.dataDir + QStringLiteral("/providers.d");
}

void WebSearchPlugin::reloadProviders()
{
    QStringList files;
    if (!m_ctx.dataDir.isEmpty()) {
        files << m_ctx.dataDir + QStringLiteral("/providers.json");
        const QDir d(providersDir());
        for (const QString &f : d.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) files << d.filePath(f);
    }
    const ProviderSet set = loadProviders(files, m_builtinOverride);
    for (const QString &m : set.messages) {
        qCWarning(lcWeb) << m;
        emit logMessage(QStringLiteral("Web search: ") + m);
    }
    QList<SearchBackend *> keep;
    QSet<QString> ids;
    for (const ProviderDef &def : set.providers) {
        ids.insert(def.id);
        HttpIndexProvider *p = nullptr;
        for (SearchBackend *b : std::as_const(m_backends))
            if (b->id() == def.id) p = qobject_cast<HttpIndexProvider *>(b);
        if (p) {
            p->setDef(def);
        } else {
            p = new HttpIndexProvider(def, &m_env, this);
            wireBackend(p);
        }
        p->setEnabled(persistedEnabled(def.id, def.enabled));
        keep << p;
    }
    for (SearchBackend *b : std::as_const(m_backends)) {
        if (keep.contains(b)) continue;
        if (!qobject_cast<HttpIndexProvider *>(b)) { keep << b; continue; }   // non-HTTP backends stay
        b->cancelAll();
        for (ActiveSearch *s : std::as_const(m_searches)) s->pending.remove(b);
        b->deleteLater();
    }
    m_backends = keep;
    qCInfo(lcWeb) << "providers:" << m_backends.size();
    m_statusTimer.start();
}

void WebSearchPlugin::wireBackend(SearchBackend *b)
{
    connect(b, &SearchBackend::results, this, [this, b](quint32 id, const QList<TorrentRecord> &recs) { onResults(b, id, recs); });
    connect(b, &SearchBackend::finished, this, [this, b](quint32 id, bool ok, const QString &err) { onFinished(b, id, ok, err); });
    connect(b, &SearchBackend::healthChanged, this, [this] { m_statusTimer.start(); });
}

void WebSearchPlugin::addBackend(SearchBackend *b)
{
    b->setParent(this);
    b->setEnabled(persistedEnabled(b->id(), b->isEnabled()));
    wireBackend(b);
    m_backends << b;
    m_statusTimer.start();
}

SearchBackend *WebSearchPlugin::backend(const QString &id) const
{
    for (SearchBackend *b : m_backends)
        if (b->id() == id) return b;
    return nullptr;
}

bool WebSearchPlugin::persistedEnabled(const QString &id, bool def) const
{
    if (!m_ctx.settings) return def;
    const QVariantMap m = m_ctx.settings->value(wkeys::ProviderEnabled).toMap();
    auto it = m.constFind(id);
    return it == m.constEnd() ? def : it.value().toBool();
}

void WebSearchPlugin::persistEnabled(const QString &id, bool on)
{
    if (!m_ctx.settings) return;
    QVariantMap m = m_ctx.settings->value(wkeys::ProviderEnabled).toMap();
    m.insert(id, on);
    m_ctx.settings->setValue(wkeys::ProviderEnabled, m);
}

QStringList WebSearchPlugin::trackers() const
{
    if (!m_ctx.settings) return {};
    const QStringList own = m_ctx.settings->sl(wkeys::Trackers);
    if (!own.isEmpty()) return own;
    return m_ctx.settings->sl(keys::bt::ExtraTrackers);
}

FilterOptions WebSearchPlugin::filterFor(SearchBackend *b) const
{
    FilterOptions o;
    if (auto *h = qobject_cast<HttpIndexProvider *>(b)) o.matchName = h->def().matchName;
    o.adultFilter = m_ctx.settings && m_ctx.settings->b(keys::search::AdultFilter);
    if (o.adultFilter) {
        o.adultKeywords = m_ctx.settings->sl(wkeys::AdultKeywords);
        if (o.adultKeywords.isEmpty()) o.adultKeywords = defaultAdultKeywords();
    }
    return o;
}

void WebSearchPlugin::start()
{
    m_running = true;
    m_probeTimer.start();
    const bool probe = !m_ctx.settings || !m_ctx.settings->value(wkeys::ProbeOnStart).isValid()
                       || m_ctx.settings->b(wkeys::ProbeOnStart);
    if (probe) {
        m_probing = true;
        // a little later, so an immediate search is not slowed down by the health checks
        QTimer::singleShot(3000, this, [this] {
            if (m_running) probeStale();
        });
    }
    m_statusTimer.start();
}

void WebSearchPlugin::probeStale()
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (SearchBackend *b : std::as_const(m_backends)) {
        if (!b->isEnabled() || b->isBackingOff()) continue;
        const BackendHealth &h = b->health();
        if (h.lastAttempt.isValid() && h.lastAttempt.secsTo(now) < kRecentOkSecs / 2) continue;
        b->probe();
    }
    m_statusTimer.start();
}

void WebSearchPlugin::stop()
{
    m_running = false;
    m_probing = false;
    m_probeTimer.stop();
    const QList<quint32> ids = m_searches.keys();
    for (quint32 id : ids) finishSearch(id);
    for (SearchBackend *b : std::as_const(m_backends)) b->cancelAll();
    m_statusTimer.start();
}

// [diag-1] Connection Test: which index hosts we depend on and how the last requests went
PluginDiagnostics WebSearchPlugin::diagnostics() const
{
    PluginDiagnostics d;
    d.valid = true;
    for (SearchBackend *b : m_backends) {
        if (!b->isEnabled()) continue;
        const QString host = QUrl(b->address().section(QLatin1Char(' '), 0, 0)).host();
        if (!host.isEmpty() && !host.contains(QLatin1Char('{')) && !d.hosts.contains(host)) d.hosts << host;
        const BackendHealth &h = b->health();
        if (!h.lastAttempt.isValid()) continue;
        const bool ok = h.lastOk.isValid() && h.consecutiveFailures == 0;
        d.items << DiagItem{QStringLiteral("web.index.") + b->id(), QStringLiteral("Index %1").arg(b->displayName()),
                            ok ? DiagLevel::Ok : h.consecutiveFailures >= 3 ? DiagLevel::Limited : DiagLevel::Unknown,
                            ok ? QStringLiteral("Answers (%1 ms)").arg(h.lastLatencyMs) : QStringLiteral("Fails"),
                            ok ? (h.lastResultCount >= 0 ? QStringLiteral("%1 results last time").arg(h.lastResultCount) : QString())
                               : QStringLiteral("%1 in a row: %2").arg(countNoun(h.consecutiveFailures, QStringLiteral("failure")),
                                                                        h.lastError.contains(QLatin1String("429")) ? QStringLiteral("asks us to slow down (HTTP 429)")
                                                                                                                   : h.lastError.left(80)),
                            ok ? QString()
                               : h.lastError.contains(QLatin1String("429"))
                                     ? QStringLiteral("This torrent index asks for fewer requests: OctoMX waits a few minutes before asking it again. The other indexes still work.")
                                     : QStringLiteral("This torrent index does not answer: it may be down or blocked by your provider. The other indexes still work.")};
    }
    {   // [enc-1] which indexes are asked over HTTPS
        int https = 0, all = 0;
        for (SearchBackend *b : m_backends) {
            if (!b->isEnabled()) continue;
            ++all;
            if (b->address().startsWith(QLatin1String("https://"), Qt::CaseInsensitive)) ++https;
        }
        if (all > 0)
            d.items << DiagItem{QStringLiteral("web.encryption"), QStringLiteral("Encryption"), https == all ? DiagLevel::Ok : DiagLevel::Unknown,
                                QStringLiteral("HTTPS for %1 of %2").arg(https).arg(countNoun(all, QStringLiteral("index"), QStringLiteral("indexes"))),
                                m_env.verifyTls ? QStringLiteral("certificates are checked") : QStringLiteral("certificate checks are off (http/verifyTls)"), QString()};
    }
    return d;
}

NetworkStatus WebSearchPlugin::status() const
{
    NetworkStatus st;
    st.net = Net::Web;
    int enabled = 0, ok = 0, tried = 0;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (SearchBackend *b : m_backends) {
        if (!b->isEnabled()) continue;
        ++enabled;
        const BackendHealth &h = b->health();
        if (h.lastAttempt.isValid()) ++tried;
        if (h.state == BackendState::Ok && h.lastOk.isValid() && h.lastOk.secsTo(now) < kRecentOkSecs) ++ok;
    }
    st.peers = ok;
    if (!m_running) {
        st.state = ConnState::Disconnected;
        st.statusText = QStringLiteral("Offline (%1)").arg(countNoun(enabled, QStringLiteral("index"), QStringLiteral("indexes")));
    } else if (enabled == 0) {
        st.state = ConnState::Error;
        st.statusText = QStringLiteral("No search indexes enabled");
    } else if (ok > 0) {
        st.state = ConnState::Connected;
        st.statusText = QStringLiteral("%1 of %2 indexes responding").arg(ok).arg(enabled);
    } else if (tried < enabled) {
        st.state = ConnState::Connecting;
        st.statusText = QStringLiteral("Checking %1...").arg(countNoun(enabled, QStringLiteral("index"), QStringLiteral("indexes")));
    } else {
        st.state = ConnState::Error;
        st.statusText = QStringLiteral("0 of %1 indexes responding").arg(enabled);
    }
    return st;
}

QList<ServerEntry> WebSearchPlugin::servers() const
{
    QList<ServerEntry> out;
    for (SearchBackend *b : m_backends) {
        ServerEntry e;
        e.net = Net::Web;
        e.id = b->id();
        e.name = b->displayName();
        e.address = b->address();
        e.description = b->description();
        e.isStatic = b->isUserDefined();
        const BackendHealth &h = b->health();
        e.pingMs = h.lastLatencyMs;
        // [core-5] the last answer's result count is no "files" figure of the index: description only
        if (h.lastResultCount >= 0) e.description += QStringLiteral(", %1 last search").arg(countNoun(h.lastResultCount, QStringLiteral("result")));
        if (!b->isEnabled()) e.state = ConnState::Disconnected;
        else if (!m_running) e.state = ConnState::Disconnected;
        else if (h.state == BackendState::Ok) e.state = ConnState::Connected;
        else if (h.state == BackendState::BackingOff || h.state == BackendState::Failing) e.state = ConnState::Error;
        else e.state = ConnState::Connecting;
        if (e.state == ConnState::Error && !h.lastError.isEmpty()) e.description += QStringLiteral(" (%1)").arg(h.lastError);
        out << e;
    }
    return out;
}

void WebSearchPlugin::connectServer(const QString &id)
{
    SearchBackend *b = backend(id);
    if (!b) return;
    b->setEnabled(true);
    b->resetHealth();
    persistEnabled(id, true);
    if (m_running) b->probe();
    m_statusTimer.start();
}

void WebSearchPlugin::disconnectServer(const QString &id)
{
    SearchBackend *b = backend(id);
    if (!b) return;
    b->setEnabled(false);
    b->cancelAll();
    for (auto it = m_searches.begin(); it != m_searches.end(); ++it) {
        if (it.value()->pending.remove(b) && it.value()->pending.isEmpty()) {
            const quint32 sid = it.key();
            QTimer::singleShot(0, this, [this, sid] { if (m_searches.contains(sid) && m_searches.value(sid)->pending.isEmpty()) finishSearch(sid); });
        }
    }
    persistEnabled(id, false);
    m_statusTimer.start();
}

bool WebSearchPlugin::installProviderFile(const QByteArray &json, const QString &suggestedName, QString *error)
{
    QString err;
    const QList<QJsonObject> objs = providerObjects(json, &err);
    if (objs.isEmpty()) { if (error) *error = err; return false; }
    // validate: each object must parse on its own or as an override of a known provider
    QStringList ids;
    for (const QJsonObject &o : objs) {
        const QString id = o.value(QLatin1String("id")).toString().trimmed();
        if (id.isEmpty()) { if (error) *error = QStringLiteral("provider without id"); return false; }
        ProviderDef d;
        QString perr;
        if (!backend(id) && !parseProviderDef(o, &d, &perr)) { if (error) *error = perr; return false; }
        ids << id;
    }
    if (providersDir().isEmpty()) { if (error) *error = QStringLiteral("no data directory"); return false; }
    QDir().mkpath(providersDir());
    QString base = ids.size() == 1 ? ids.first() : QFileInfo(suggestedName).completeBaseName();
    base.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")), QStringLiteral("_"));
    if (base.isEmpty()) base = QStringLiteral("providers");
    QSaveFile f(providersDir() + QLatin1Char('/') + base + QStringLiteral(".json"));
    QJsonArray arr;
    for (const QJsonObject &o : objs) arr.append(o);
    if (!f.open(QIODevice::WriteOnly)) { if (error) *error = f.errorString(); return false; }
    f.write(QJsonDocument(QJsonObject{{QStringLiteral("providers"), arr}}).toJson(QJsonDocument::Indented));
    if (!f.commit()) { if (error) *error = f.errorString(); return false; }
    // a newly added provider starts enabled unless its definition says otherwise
    if (m_ctx.settings) {
        QVariantMap m = m_ctx.settings->value(wkeys::ProviderEnabled).toMap();
        bool changed = false;
        for (const QString &id : ids) changed |= m.remove(id) > 0;
        if (changed) m_ctx.settings->setValue(wkeys::ProviderEnabled, m);
    }
    reloadProviders();
    if (m_running)
        for (const QString &id : ids)
            if (SearchBackend *b = backend(id); b && b->isEnabled()) b->probe();
    return true;
}

void WebSearchPlugin::addServer(const QString &addressIn, const QString &name)
{
    const QString address = addressIn.trimmed();
    auto report = [this](bool ok, const QString &what) {
        emit logMessage(ok ? QStringLiteral("Web search: installed provider definition %1").arg(what)
                           : QStringLiteral("Web search: cannot add provider: %1").arg(what));
    };
    if (address.startsWith(QLatin1String("http://")) || address.startsWith(QLatin1String("https://"))) {
        QNetworkRequest req{QUrl(address)};
        req.setTransferTimeout(15000);
        req.setHeader(QNetworkRequest::UserAgentHeader, m_env.userAgent);
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply *r = m_nam->get(req);
        meterReply(r, m_env.limiter, address.size() + 300);   // [glue-3]
        connect(r, &QNetworkReply::downloadProgress, r, [r](qint64 got, qint64) { if (got > 1024 * 1024) r->abort(); });
        connect(r, &QNetworkReply::finished, this, [this, r, address, name, report] {
            r->deleteLater();
            if (r->error() != QNetworkReply::NoError) { report(false, address + QStringLiteral(": ") + r->errorString()); return; }
            QString err;
            const QString fileName = name.isEmpty() ? QUrl(address).fileName() : name;
            if (installProviderFile(r->readAll(), fileName, &err)) report(true, address);
            else report(false, err);
        });
        return;
    }
    QString path = address;
    if (path.startsWith(QLatin1String("file:"))) path = QUrl(path).toLocalFile();
    QFile f(path);
    if (!f.exists() || f.size() > 1024 * 1024 || !f.open(QIODevice::ReadOnly)) {
        report(false, path + QStringLiteral(": not a readable JSON file"));
        return;
    }
    QString err;
    if (installProviderFile(f.readAll(), name.isEmpty() ? QFileInfo(path).fileName() : name, &err)) report(true, path);
    else report(false, err);
}

void WebSearchPlugin::removeServer(const QString &id)
{
    SearchBackend *b = backend(id);
    if (!b) return;
    // user-installed definition files that define only this provider are deleted; anything else is
    // disabled (built-in providers cannot be removed, only switched off)
    bool removed = false;
    const QDir d(providersDir());
    if (b->isUserDefined() && !providersDir().isEmpty()) {
        for (const QString &fn : d.entryList({QStringLiteral("*.json")}, QDir::Files)) {
            QFile f(d.filePath(fn));
            if (!f.open(QIODevice::ReadOnly)) continue;
            const QList<QJsonObject> objs = providerObjects(f.readAll(), nullptr);
            f.close();
            if (objs.size() == 1 && objs.first().value(QLatin1String("id")).toString() == id) removed |= f.remove();
        }
    }
    if (removed) reloadProviders();
    if (backend(id)) disconnectServer(id);
    m_statusTimer.start();
}

// ---------------------------------------------------------------- search
void WebSearchPlugin::search(const SearchQuery &q)
{
    const quint32 id = q.id;
    auto doneLater = [this, id] { QTimer::singleShot(0, this, [this, id] { emit searchDone(id); }); };
    if (!q.networks.isEmpty() && !q.networks.contains(Net::Web) && !q.networks.contains(Net::BitTorrent)) { doneLater(); return; }
    BackendQuery bq;
    bq.searchId = id;
    bq.text = (q.text + QLatin1Char(' ') + q.text2).simplified();
    bq.type = q.type;
    int maxResults = q.maxResults > 0 ? q.maxResults : 2000;
    if (m_ctx.settings) {
        const int webMax = m_ctx.settings->i(keys::web::MaxResults);
        if (webMax > 0) maxResults = qMin(maxResults, webMax);
    }
    bq.maxResults = maxResults;
    bq.adultFilter = m_ctx.settings && m_ctx.settings->b(keys::search::AdultFilter);
    if (!m_running || bq.text.isEmpty() || m_searches.contains(id)) { doneLater(); return; }

    auto *s = new ActiveSearch;
    s->q = q;
    s->bq = bq;
    s->merger = std::make_unique<HitMerger>(maxResults);
    s->started.start();
    for (SearchBackend *b : std::as_const(m_backends)) {
        if (!b->isEnabled() || b->isBackingOff() || !b->accepts(bq)) continue;
        s->pending.insert(b);
    }
    if (s->pending.isEmpty()) {
        delete s;
        emit logMessage(QStringLiteral("Web search: no index available (all disabled or backing off)"));
        doneLater();
        return;
    }
    m_searches.insert(id, s);
    int timeout = 25;
    if (m_ctx.settings) {
        const QVariant t = m_ctx.settings->value(wkeys::TimeoutSec);
        if (t.isValid() && t.toInt() > 0) timeout = t.toInt();
        const int global = m_ctx.settings->i(keys::search::TimeoutSec);
        if (global > 0) timeout = qMin(timeout, global);
    }
    s->deadline.setSingleShot(true);
    s->deadline.setInterval(qBound(3, timeout, 300) * 1000);
    connect(&s->deadline, &QTimer::timeout, this, [this, id] {
        if (ActiveSearch *as = m_searches.value(id)) {
            for (SearchBackend *b : std::as_const(as->pending)) {
                qCInfo(lcWeb) << "search" << id << b->id() << "did not finish in time";
                b->cancel(id);
            }
        }
        finishSearch(id);
    });
    s->deadline.start();
    qCInfo(lcWeb) << "search" << id << bq.text << "on" << s->pending.size() << "indexes";
    const QSet<SearchBackend *> targets = s->pending;
    s->backendCount = targets.size();
    for (SearchBackend *b : targets) b->search(bq);
}

void WebSearchPlugin::onResults(SearchBackend *b, quint32 searchId, const QList<TorrentRecord> &records)
{
    ActiveSearch *s = m_searches.value(searchId);
    if (!s) return;
    const FilterOptions opt = filterFor(b);
    const QStringList tr = trackers();
    QList<int> changed;
    int accepted = 0;
    // best rows first, and a fair share of the result limit for every index: a backend may exceed its
    // quota only as far as the quotas still reserved for the indexes that have not answered allow
    QList<TorrentRecord> sorted = records;
    std::stable_sort(sorted.begin(), sorted.end(), [](const TorrentRecord &x, const TorrentRecord &y) { return x.seeders > y.seeders; });
    const int quota = qMax(10, s->merger->maxHits() / qMax(1, s->backendCount));
    for (const TorrentRecord &r : std::as_const(sorted)) {
        if (!recordMatches(r, s->q, opt)) continue;
        const bool isNew = !s->merger->contains(HitMerger::keyFor(r));
        int &mine = s->newHits[b];
        if (isNew && mine >= quota) {
            int reserved = 0;
            for (SearchBackend *o : std::as_const(s->pending))
                if (o != b) reserved += qMax(0, quota - s->newHits.value(o));
            if (s->merger->size() + reserved >= s->merger->maxHits()) continue;
        }
        const int idx = s->merger->add(r, searchId, b->displayName(), tr);
        if (idx < 0) continue;
        if (isNew) ++mine;
        ++accepted;
        if (!changed.contains(idx)) changed << idx;
    }
    s->perBackend[b->id()] += accepted;
    if (changed.isEmpty()) return;
    QList<SearchHit> hits;
    hits.reserve(changed.size());
    for (int idx : std::as_const(changed)) hits << s->merger->hit(idx);
    emit searchHits(searchId, hits);
}

void WebSearchPlugin::onFinished(SearchBackend *b, quint32 searchId, bool ok, const QString &error)
{
    ActiveSearch *s = m_searches.value(searchId);
    if (!s) return;
    s->pending.remove(b);
    qCInfo(lcWeb) << "search" << searchId << b->id() << (ok ? "ok" : "failed") << error
                  << s->perBackend.value(b->id()) << "hits" << s->started.elapsed() << "ms";
    if (s->pending.isEmpty()) finishSearch(searchId);
}

void WebSearchPlugin::finishSearch(quint32 searchId)
{
    ActiveSearch *s = m_searches.take(searchId);
    if (!s) return;
    for (SearchBackend *b : std::as_const(s->pending)) b->cancel(searchId);
    delete s;
    emit searchDone(searchId);
}

void WebSearchPlugin::cancelSearch(quint32 queryId)
{
    ActiveSearch *s = m_searches.take(queryId);
    if (!s) return;
    for (SearchBackend *b : std::as_const(m_backends)) b->cancel(queryId);
    delete s;
}

void WebSearchPlugin::updateStatus()
{
    m_statusTimer.start();
}

} // namespace web
} // namespace mx
