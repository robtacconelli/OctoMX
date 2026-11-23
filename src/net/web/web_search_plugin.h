// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX web module: the NetworkPlugin facade (private; tests include it directly).
#pragma once

#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QTimer>

#include <memory>

#include "hit_builder.h"
#include "http_index_provider.h"
#include "mx/plugin.h"
#include "search_backend.h"

class QNetworkAccessManager;

namespace mx::web {

// Settings keys used by this plugin that are not (yet) in the core schema. SimpleSettings stores
// unknown keys as given; the plugin falls back to the defaults below.
namespace wkeys {
inline constexpr QLatin1StringView UserAgent{"web/userAgent"};          // String; empty = built-in browser UA
inline constexpr QLatin1StringView ProviderEnabled{"web/providerEnabled"}; // Map {providerId: bool} (user toggles)
inline constexpr QLatin1StringView AdultKeywords{"web/adultKeywords"};   // StringList; empty = built-in list
inline constexpr QLatin1StringView TimeoutSec{"web/timeoutSec"};         // Int; whole-search deadline (default 25)
inline constexpr QLatin1StringView ProbeOnStart{"web/probeOnStart"};     // Bool; health-check indexes on start (default true)
inline constexpr QLatin1StringView Trackers{"web/trackers"};             // StringList; trackers for magnets (default bt/extraTrackers)
}

QString defaultUserAgent();

class WebSearchPlugin : public NetworkPlugin {
    Q_OBJECT
public:
    explicit WebSearchPlugin(QObject *parent = nullptr);
    ~WebSearchPlugin() override;

    Net net() const override { return Net::Web; }
    int capabilities() const override { return CanSearch; }
    void init(const PluginContext &ctx) override;
    void start() override;
    void stop() override;
    NetworkStatus status() const override;

    QList<ServerEntry> servers() const override;
    void addServer(const QString &address, const QString &name) override;
    void removeServer(const QString &id) override;
    void connectServer(const QString &id) override;
    void disconnectServer(const QString &id) override;

    PluginDiagnostics diagnostics() const override;   // [diag-1] index hosts + their health
    void search(const SearchQuery &q) override;
    void cancelSearch(quint32 queryId) override;

    // --- module-internal API (tests, future backends)
    QList<SearchBackend *> backends() const { return m_backends; }
    SearchBackend *backend(const QString &id) const;
    void addBackend(SearchBackend *b);       // a non-HTTP backend (takes ownership)
    void reloadProviders();
    // Override the built-in provider list (tests); empty = the compiled-in resource.
    void setBuiltinProviders(const QByteArray &json) { m_builtinOverride = json; }
    bool installProviderFile(const QByteArray &json, const QString &suggestedName, QString *error);
    QString providersDir() const;

private:
    struct ActiveSearch {
        SearchQuery q;
        BackendQuery bq;
        QSet<SearchBackend *> pending;
        std::unique_ptr<HitMerger> merger;
        QTimer deadline;
        QElapsedTimer started;
        QHash<QString, int> perBackend;   // results accepted per backend
        QHash<SearchBackend *, int> newHits;  // distinct new hits contributed per backend
        int backendCount = 1;
    };

    void applyNetworkSettings();
    void wireBackend(SearchBackend *b);
    void onResults(SearchBackend *b, quint32 searchId, const QList<TorrentRecord> &records);
    void onFinished(SearchBackend *b, quint32 searchId, bool ok, const QString &error);
    void finishSearch(quint32 searchId);
    void updateStatus();
    void probeStale();
    bool persistedEnabled(const QString &id, bool def) const;
    void persistEnabled(const QString &id, bool on);
    QStringList trackers() const;
    FilterOptions filterFor(SearchBackend *b) const;

    PluginContext m_ctx;
    QNetworkAccessManager *m_nam = nullptr;
    HttpEnvironment m_env;
    QList<SearchBackend *> m_backends;
    QHash<quint32, ActiveSearch *> m_searches;
    QByteArray m_builtinOverride;
    QTimer m_probeTimer;
    QTimer m_statusTimer;
    bool m_running = false;
    bool m_probing = false;
    NetworkStatus m_lastStatus;
};

} // namespace mx::web
