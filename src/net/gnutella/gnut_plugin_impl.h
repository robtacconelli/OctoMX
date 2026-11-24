// SPDX-License-Identifier: GPL-3.0-or-later
// Gnutella module: the plugin: leaf servent engine behind mx::NetworkPlugin.
//   gnut_plugin.cpp  lifecycle, settings, status, servers list, persistence
//   gnut_net.cpp     ultrapeer connections, bootstrap (GWC/UHC), message dispatch, QRP, vendor msgs
//   gnut_search.cpp  searches, query hits, source finding, downloads (RangeSource), browse
//   gnut_serve.cpp   listener (handshake / HTTP / GIV / connect-back), answering queries, uploads,
//                    upload slots + queue, PUSH handling, THEX cache, browse-host packets
#pragma once

#include "gnut_browse.h"
#include "gnut_connection.h"
#include "gnut_hostcache.h"
#include "gnut_qrp.h"
#include "gnut_source.h"
#include "gnut_upload.h"
#include "gnutella_plugin.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QTimer>
#include <memory>

class QNetworkAccessManager;
class QNetworkReply;
class QTcpServer;
class QTcpSocket;
class QUdpSocket;

namespace mx::gnut {

// Built-in bootstrap lists (each entry answered a live connection test on 2026-11-20).
QStringList defaultGwcs();
QStringList defaultUhcs();

class GnutellaPlugin : public mx::NetworkPlugin, public ServeHost, public SourceHost {
    Q_OBJECT
public:
    explicit GnutellaPlugin(QObject *parent = nullptr);
    ~GnutellaPlugin() override;

    // ---- NetworkPlugin
    mx::Net net() const override { return mx::Net::Gnutella; }
    int capabilities() const override { return CanSearch | CanFindSources | CanFetchRanges | CanServe | CanBrowse; }
    void init(const mx::PluginContext &ctx) override;
    void start() override;
    void stop() override;
    mx::NetworkStatus status() const override;
    QList<mx::ServerEntry> servers() const override;
    void addServer(const QString &address, const QString &name) override;
    void removeServer(const QString &id) override;
    void connectServer(const QString &id) override;
    void disconnectServer(const QString &id) override;
    void search(const mx::SearchQuery &q) override;
    void cancelSearch(quint32 queryId) override;
    void findSources(const QString &downloadId, const mx::FileHashes &h, qint64 size, const QString &name) override;
    void stopFindSources(const QString &downloadId) override;
    mx::RangeSource *openSource(const QString &downloadId, const mx::FileHashes &h, qint64 size,
                                const mx::SearchSource &src) override;
    void setSharedFiles(const QList<mx::SharedFile> &files) override;
    void browse(quint32 requestId, const mx::UserRef &u) override;
    void cancelUpload(const QString &uploadId) override;
    bool startUploadNow(const QString &uploadId) override;              // [glue-3]
    bool setUploadPriority(const QString &uploadId, int priority) override;   // [glue-3]
    QStringList queuedUploadIds() const;                                 // [glue-3] queue order (tests)
    // [diag-1]
    mx::PluginDiagnostics diagnostics() const override;
    void recheckConnectivity() override;

    // ---- introspection for tests / probe
    QVariantMap debugStats() const;
    QByteArray serventGuid() const { return m_guid; }
    quint16 listenPort() const { return m_port; }
    QStringList connectionLines() const;
    int connectedCount() const;

    // ---- ServeHost
    mx::LibraryAccess *library() override { return m_ctx.library; }
    mx::RateLimiter *limiter() override { return m_ctx.limiter; }
    QString userAgent() const override { return m_userAgent; }
    bool sharedBySha1(const QByteArray &sha1, mx::SharedFile *out) override;
    bool sharedByTth(const QByteArray &tth, mx::SharedFile *out) override;
    bool sharedByIndex(quint32 index, mx::SharedFile *out) override;
    quint32 indexOf(const mx::SharedFile &f) override;
    int acquireSlot(UploadSession *s, const QString &key, const QString &parqId, QueueInfo *q) override;
    void releaseSlot(UploadSession *s) override;
    bool browseAllowed(const HostPort &peer) override;
    QList<QByteArray> browsePackets() override;
    QByteArray thexFor(const mx::SharedFile &f, int *retryAfter) override;
    QByteArray tthFor(const mx::SharedFile &f) override;
    QList<HostPort> altLocsFor(const QByteArray &sha1, const HostPort &exclude) override;
    void addAltLocs(const QByteArray &sha1, const QList<HostPort> &hosts) override;
    void reportUpload(const mx::TransferInfo &t) override;
    void reportUploadFinished(const QString &id) override;
    void noteIncomingHttp(const HostPort &peer, const QString &what) override;

    // ---- SourceHost
    HostPort selfAddress() const override;
    bool firewalled() const override;
    int connectTimeoutMs() const override;
    bool requestPush(GnutSource *s, const SourceDesc &d) override;
    void cancelPush(GnutSource *s) override;
    bool wantThex(const QString &downloadId) override;
    void thexLearned(const QString &downloadId, const ThexTree &t) override;
    void altLocsFound(const QString &downloadId, const QByteArray &sha1, const QList<HostPort> &direct,
                      const QList<PushLoc> &pushed, const QList<HostPort> &directTls = {}) override;
    void sourceLog(const QString &line) override;
    QTcpSocket *newSocket(bool peerTls, QObject *parent) override;   // [enc-1]
    QTcpSocket *makeSocket(bool peerTls, QObject *parent);
    void prepareSocket(QTcpSocket *s);   // [net-2] proxy / chosen adapter

    // ---- [enc-1] TLS (gnutella/encryption)
    TlsMode tlsMode() const;
    bool tlsAdvertised() const;             // we accept TLS and say so (handshake, hits, pongs, push)
    TlsStats tlsStats() const { return m_tls ? m_tls->stats : TlsStats(); }
    int encryptedLinks() const;             // connected ultrapeer links over TLS
    QString tlsCertificatePath() const;

private:
    // gnut_plugin.cpp
    void log(const QString &s);
    void emitStatus();
    void saveState();
    QVariant setting(const QString &key, const QVariant &fallback) const;
    int maxUltrapeers() const;
    quint32 ourSpeedKbps() const;   // [core-5] speed field of our hits, from identity/connection
    int uploadSlots() const;
    // gnut_net.cpp
    void maintain();
    void startConnection(const HostPort &h, bool manual = false);
    void onHandshakeDone(UltrapeerConnection *c);
    void onConnectionClosed(UltrapeerConnection *c, const QString &reason);
    void onMessage(UltrapeerConnection *c, const Message &m);
    void onVendor(UltrapeerConnection *c, const Message &m);
    void sendQrp(UltrapeerConnection *c, bool force);
    void bootstrap();
    void queryGwc(const QString &url);
    void onGwcReply(QNetworkReply *r, const QString &url);
    void pingUhc(const QString &hostPort);
    void onUdpReadyRead();
    void learnExternalIp(UltrapeerConnection *c);
    QList<UltrapeerConnection *> connectedUps() const;
    void sendVendor(UltrapeerConnection *c, const QByteArray &vendor, quint16 id, quint16 version,
                    const QByteArray &body, const QByteArray &guid = QByteArray());
    // gnut_search.cpp
    struct SearchState;
    struct Finder;
    void sendSearch(SearchState &s, UltrapeerConnection *only = nullptr);
    void onQueryHit(UltrapeerConnection *c, const Message &m);
    void flushSearchHits();
    void searchTick();
    void sendFinderQuery(Finder &f);
    bool hitPassesFilter(const SearchState &s, const HitResult &r) const;
    void answerQueryStatus(UltrapeerConnection *c, const QByteArray &guid);
    void rememberServent(const SourceDesc &d);
    mx::SearchSource makeSource(const SourceDesc &d, quint32 speed, const QueryHit &qh) const;
    // gnut_serve.cpp
    void onNewConnection();
    void onSniffReadyRead(QTcpSocket *s);
    void handleGiv(QTcpSocket *s, const QByteArray &line);
    void answerIncomingHandshake(QTcpSocket *s, const HeaderBlock &h);
    void onQuery(UltrapeerConnection *c, const Message &m);
    void onPush(UltrapeerConnection *c, const Message &m);
    void rebuildShareIndex();
    void scheduleQrpUpdate();
    void sendPushProxyRequest(const HostPort &proxy, const QByteArray &guid, quint32 index);
    void noteReachable(quint32 peerIp, const QString &how);
    void beginSniff(QTcpSocket *s);                         // [enc-1] watch the first bytes of a connection
    void onTlsIncoming(QTcpSocket *s, const QByteArray &first);
    bool ensureTls();
    TlsSocket::Policy tlsPolicy(bool peerTls) const;
    void applyTlsMode();

    mx::PluginContext m_ctx;
    bool m_inited = false, m_started = false;
    QString m_userAgent;
    QByteArray m_guid;
    quint16 m_port = 0;
    QTcpServer *m_server = nullptr;
    QUdpSocket *m_udp = nullptr;
    QNetworkAccessManager *m_nam = nullptr;
    HostCache m_hosts;
    QList<UltrapeerConnection *> m_conns;
    QSet<quint64> m_manualConnect;
    QTimer m_maintainTimer, m_saveTimer, m_searchTimer, m_hitFlushTimer, m_qrpTimer;
    QElapsedTimer m_uptime;
    qint64 m_lastGwc = 0, m_lastUhc = 0;
    int m_gwcInFlight = 0;
    QHash<QByteArray, QString> m_uhcPings;     // ping guid → UHC host:port
    QHash<QByteArray, qint64> m_udpCbGuids;
    quint32 m_externalIp = 0;
    bool m_reachable = false, m_udpReachable = false;
    QString m_reachableHow;
    QElapsedTimer m_cbAskedAt;             // [diag-1] first connect-back request of this round
    void askConnectBack(UltrapeerConnection *c);
    mx::NetworkStatus m_lastStatus;
    std::shared_ptr<TlsEngine> m_tls;        // [enc-1] created on first use (certificate in dataDir)
    QString m_tlsError;
    QSet<TlsSocket *> m_tlsHandshaking;

    // searches
    struct SearchState {
        mx::SearchQuery q;
        QList<QByteArray> guids;
        QElapsedTimer started, lastHit;
        int hits = 0;            // results kept
        bool done = false;
        QSet<QString> seen;
        QList<mx::SearchHit> pending;
        QSet<quint64> sentTo;    // connection addresses
        QStringList words;
    };
    QHash<quint32, SearchState> m_searches;
    QHash<QByteArray, quint32> m_queryGuids;   // our query GUID → search id
    QList<QByteArray> m_queryGuidOrder;
    struct Finder {
        QString downloadId;
        mx::FileHashes hashes;
        qint64 size = 0;
        QString name;
        qint64 lastQuery = 0;
        int queries = 0;
        QSet<QString> known;     // userId|addr already reported
    };
    QHash<QString, Finder> m_finders;
    QHash<QByteArray, QString> m_finderGuids;  // query GUID → downloadId
    qint64 m_lastFinderQuery = 0;
    QHash<QString, SourceDesc> m_servents;     // guid hex → last known address/proxies
    QList<QString> m_serventOrder;
    QHash<QString, int> m_thexState;           // downloadId → 1 in progress, 2 learned
    QList<QPointer<GnutSource>> m_sources;
    struct PushWait { QPointer<GnutSource> src; QPointer<BrowseClient> browse; qint64 deadline = 0; };
    QMultiHash<QByteArray, PushWait> m_pushWaits;
    QHash<quint32, QPointer<BrowseClient>> m_browses;

    // sharing
    QList<mx::SharedFile> m_shared;
    QList<QString> m_sharedLower;               // normalized names for matching
    QHash<QString, quint32> m_pathIndex;
    quint32 m_nextIndex = 1;
    QHash<quint32, int> m_byIndex;
    QHash<QByteArray, int> m_bySha1, m_byTth;
    QrpTable m_qrp{65536};
    QByteArray m_qrpDigest;
    QList<QByteArray> m_browseCache;
    struct ThexEntry { QByteArray dime; QByteArray root; bool computing = false; qint64 lastUse = 0; };
    QHash<QByteArray, ThexEntry> m_thex;       // by sha1
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    QHash<QByteArray, QList<HostPort>> m_altLocs;
    // uploads
    QSet<UploadSession *> m_slotHolders;
    struct QueueEntry {
        QString key; QString id; QPointer<UploadSession> session; qint64 lastPoll = 0;
        // [glue-3] the queue entry is the upload row while the client waits (across reconnects)
        QString uploadId; mx::TransferInfo info; int priority = 0; bool forced = false; int reportedRank = -1;
    };
    void reportUploadQueue();            // [glue-3]
    void sortUploadQueue();
    QHash<QString, qint64> m_uploadBlocked;   // [glue-3] slot key (ip/file) → refused until (s) after a local cancel
    QList<QueueEntry> m_uploadQueue;
    QHash<QString, QPointer<UploadSession>> m_uploads;   // uploadId → session
    QSet<UploadSession *> m_sessions;
    int m_uploadsDone = 0;
    // incoming sniffing
    QHash<QTcpSocket *, QByteArray> m_sniff;
    QHash<QString, qint64> m_recentPushes;     // "ip:port/index" → when
    int m_pushConnecting = 0;
    // routing / dedupe of incoming queries
    QSet<QByteArray> m_seenQueries;
    QList<QByteArray> m_seenOrder;

    // stats
    struct Stats {
        int handshakesOk = 0, handshakesFailed = 0, qrpSent = 0, queriesSeen = 0, queriesAnswered = 0,
            hitsSent = 0, hitsReceived = 0, incomingHandshakes = 0, incomingHttp = 0, browsesServed = 0,
            pushesReceived = 0, givsReceived = 0, givsSent = 0, connectBacks = 0, gwcOk = 0, gwcFail = 0,
            uhcReplies = 0, pushProxyAcks = 0, tcpConnectBacksRequested = 0, droppedMessages = 0;
        QStringList incomingAgents;
    } m_stats;
};

} // namespace mx::gnut
