// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX: the HTTP NetworkPlugin. Private to mxnet_http.
#pragma once

#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QPointer>

#include "http_plugin.h"
#include "http_util.h"

class QNetworkAccessManager;

namespace mx {

class HttpRangeSource;

class HttpPlugin : public NetworkPlugin {
    Q_OBJECT
public:
    explicit HttpPlugin(QObject *parent = nullptr);
    ~HttpPlugin() override;

    Net net() const override { return Net::Http; }
    int capabilities() const override { return CanFetchRanges; }
    void init(const PluginContext &ctx) override;
    void start() override;
    void stop() override;
    NetworkStatus status() const override { return m_status; }
    RangeSource *openSource(const QString &downloadId, const FileHashes &h, qint64 size,
                            const SearchSource &src) override;

    // --- for HttpRangeSource
    QNetworkAccessManager *nam();
    HttpConfig config() const { return HttpConfig::load(m_ctx.settings); }
    RateLimiter *limiter() const { return m_ctx.limiter; }
    bool tryAcquireSlot(const QString &host);
    void releaseSlot(const QString &host);
    void addSource(HttpRangeSource *s);
    void removeSource(HttpRangeSource *s);
    void reportHints(const QString &downloadId, const QUrl &self, const QStringList &mirrors, const FileHashes &hashes);
    void log(const QString &text) { emit logMessage(text); }

private:
    void setStatus(ConnState st, const QString &text);
    PluginContext m_ctx;
    NetworkStatus m_status;
    QNetworkAccessManager *m_nam = nullptr;
    QHash<QString, int> m_hostActive;
    QList<QPointer<HttpRangeSource>> m_sources;
};

} // namespace mx
