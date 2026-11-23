// SPDX-License-Identifier: GPL-3.0-or-later
// Net::Rats: keyword search over the live rats-search P2P network. Private to mxnet_overlay.
#pragma once

#include "overlay_plugin.h"
#include "rats_node.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QTimer>

namespace mx::ovl {

// Settings this plugin reads that are not (yet) in the schema; defaults used when absent.
namespace ratskeys {
inline constexpr const char *Protocol = "rats/protocol";      // String, "rats-search/4"
inline constexpr const char *ListenPort = "rats/listenPort";  // Int, 4444 (busy → free port)
inline constexpr const char *DhtPort = "rats/dhtPort";        // Int, 0 = ephemeral
inline constexpr const char *SafeSearch = "rats/safeSearch";  // Bool, true
inline constexpr const char *MaxPeers = "rats/maxPeers";      // Int, 30
}
inline constexpr const char *kDefaultRatsProtocol = "rats-search/4";

class RatsSearchPlugin : public RatsSearchApi {
    Q_OBJECT
public:
    explicit RatsSearchPlugin(QObject *parent = nullptr);
    ~RatsSearchPlugin() override;

    Net net() const override { return Net::Rats; }
    int capabilities() const override { return CanSearch; }
    void init(const PluginContext &ctx) override;
    void start() override;
    void stop() override;
    NetworkStatus status() const override;
    QList<ServerEntry> servers() const override;
    void connectServer(const QString &id) override;
    void disconnectServer(const QString &id) override;
    void addServer(const QString &address, const QString &name) override;

    PluginDiagnostics diagnostics() const override;   // [diag-1]
    void search(const SearchQuery &q) override;
    void cancelSearch(quint32 queryId) override;
    bool requestTorrentFiles(const QByteArray &btih) override;
    // generic core hook (NetworkPlugin): forwards to requestTorrentFiles; answer via bundleFiles
    void requestBundleFiles(const FileHashes &h) override;

    // test / probe hooks
    RatsNode *node() const { return m_node; }
    void setDataDirForTest(const QString &d) { m_dataDir = d; }

private:
    struct PeerMeta {
        QString address;
        QString version;
        qint64 torrents = -1;
        qint64 files = -1;
        qint64 connectedMs = 0;
        bool gotInfo = false;
        QSet<quint32> queriesSent;
    };
    struct ActiveSearch {
        SearchQuery q;
        QString text;
        QStringList words;
        QString contentType;          // rats content type for q.type ("" = any)
        QElapsedTimer started;
        qint64 lastHitMs = 0;          // ms since start of last accepted hit
        QSet<QByteArray> seen;         // info-hashes
        int emitted = 0;
        int replies = 0;               // raw replies (before filtering)
        int peersAnswered = 0;
        QSet<QString> answeredBy;
        QList<SearchHit> pending;
        bool done = false;
    };

    QVariant setting(const char *key, const QVariant &def) const;
    void sendClientInfo(const QString &peerId);
    void sendQueries(const QString &peerId);
    void sendQuery(const QString &peerId, const ActiveSearch &s);
    void onPeerConnected(const RatsPeer &p);
    void onPeerDisconnected(const QString &id, const QString &reason);
    void onJson(const QString &peerId, const QString &type, const QJsonObject &o);
    void onHit(const QString &peerId, bool filesHit, const QJsonObject &o);
    void flush();
    void tick();
    void finish(quint32 qid);
    void updateStatus(bool force = false);
    bool safeSearch() const;

    PluginContext m_ctx;
    QString m_dataDir;
    QPointer<RatsNode> m_node;
    bool m_running = false;
    int m_wantPort = 0;   // [diag-1]
    QElapsedTimer m_startedAt;
    QHash<QString, PeerMeta> m_peers;
    QHash<quint32, ActiveSearch> m_searches;
    QHash<QByteArray, QStringList> m_hashPeers;   // btih → peers that reported it (bounded)
    QList<QByteArray> m_hashOrder;
    QTimer m_flushTimer, m_tickTimer;
    NetworkStatus m_lastStatus;
    QString m_lastError;
    bool m_serversDirty = false;
};

} // namespace mx::ovl
