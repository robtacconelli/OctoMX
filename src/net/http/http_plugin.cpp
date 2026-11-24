// SPDX-License-Identifier: GPL-3.0-or-later
#include "http_plugin_impl.h"

#include <QtNetwork/QNetworkAccessManager>

#include "http_source.h"
#include "mx/services.h"
#include "mx/settings_keys.h"

namespace mx {

NetworkPlugin *createHttpPlugin(QObject *parent)
{
    return new HttpPlugin(parent);
}

HttpPlugin::HttpPlugin(QObject *parent) : NetworkPlugin(parent)
{
    qRegisterMetaType<mx::HttpProbeResult>();
    qRegisterMetaType<mx::ProbeOutcome>();
    m_status.net = Net::Http;
    m_status.state = ConnState::Disconnected;
    m_status.statusText = QStringLiteral("Offline");
}

HttpPlugin::~HttpPlugin()
{
    // Sources are owned by the engine; make sure none of them touches us any more.
    for (const QPointer<HttpRangeSource> &s : m_sources)
        if (s) s->close();
}

void HttpPlugin::init(const PluginContext &ctx)
{
    m_ctx = ctx;
    if (m_ctx.settings) {
        connect(m_ctx.settings, &Settings::changed, this, [this](const QString &key, const QVariant &) {
            if ((key.startsWith(QLatin1String("connection/proxy")) || key == QLatin1String("connection/bindInterface")
                 || key == QLatin1String("connection/ipv6")) && m_nam)
                configureManager(m_nam, m_ctx.settings);   // [net-2] at once
        });
    }
}

QNetworkAccessManager *HttpPlugin::nam()
{
    if (!m_nam) {
        m_nam = new QNetworkAccessManager(this);
        configureManager(m_nam, m_ctx.settings);
    }
    return m_nam;
}

void HttpPlugin::setStatus(ConnState st, const QString &text)
{
    m_status.state = st;
    m_status.statusText = text;
    emit statusChanged(m_status);
}

void HttpPlugin::start()
{
    nam();
    setStatus(ConnState::Connected, QStringLiteral("Ready"));
}

void HttpPlugin::stop()
{
    setStatus(ConnState::Disconnected, QStringLiteral("Offline"));
}

RangeSource *HttpPlugin::openSource(const QString &downloadId, const FileHashes &h, qint64 size, const SearchSource &src)
{
    if (src.net != Net::Http && src.net != Net::Unknown) return nullptr;
    QUrl url = HttpProbe::normalizeUrl(src.uri);
    if (!url.isValid()) return nullptr;
    auto *s = new HttpRangeSource(this, downloadId, h, size, src, url);
    addSource(s);
    return s;
}

bool HttpPlugin::tryAcquireSlot(const QString &host)
{
    const int cap = config().maxPerHost;
    int &n = m_hostActive[host];
    if (n >= cap) return false;
    ++n;
    return true;
}

void HttpPlugin::releaseSlot(const QString &host)
{
    auto it = m_hostActive.find(host);
    if (it == m_hostActive.end()) return;
    if (--it.value() <= 0) m_hostActive.erase(it);
    // Let waiting sources use the freed slot (queued: never re-enter the caller).
    for (const QPointer<HttpRangeSource> &s : std::as_const(m_sources)) {
        if (!s) continue;
        QPointer<HttpRangeSource> p = s;
        QMetaObject::invokeMethod(s, [p] { if (p) p->kick(); }, Qt::QueuedConnection);
    }
}

void HttpPlugin::addSource(HttpRangeSource *s)
{
    m_sources.removeAll(QPointer<HttpRangeSource>());
    m_sources << s;
}

void HttpPlugin::removeSource(HttpRangeSource *s)
{
    for (int i = m_sources.size() - 1; i >= 0; --i)
        if (!m_sources[i] || m_sources[i] == s) m_sources.removeAt(i);
}

void HttpPlugin::reportHints(const QString &downloadId, const QUrl &self, const QStringList &mirrors, const FileHashes &hashes)
{
    QList<SearchSource> out;
    const QString selfStr = self.toString(QUrl::FullyEncoded);
    for (const QString &m : mirrors) {
        if (m == selfStr) continue;
        SearchSource s;
        s.net = Net::Http;
        s.uri = m;
        s.connection = QStringLiteral("HTTP mirror");
        s.extra.insert(QStringLiteral("mirrorOf"), selfStr);
        out << s;
    }
    if (!out.isEmpty()) {
        emit logMessage(QStringLiteral("%1 mirror(s) announced by %2").arg(out.size()).arg(self.host()));
        emit sourcesFound(downloadId, out);
    }
    if (!hashes.isEmpty()) emit hashesLearned(downloadId, hashes);
}

} // namespace mx
