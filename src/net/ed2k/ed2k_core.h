// SPDX-License-Identifier: GPL-3.0-or-later
// Ed2kCore: the shared eD2k engine behind the ED2K plugin (and, next, the Kad plugin):
// identity (user hash, ports), listening socket, UDP socket, server manager, peer connections with
// limits, the upload queue, running downloads (source finding, learned hashes) and the re-ask
// registry that keeps us from being banned.
#pragma once

#include "ed2k_util.h"
#include "kad/kad_wire.h"

#include "mx/plugin.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QTimer>
#include <functional>

class QNetworkAccessManager;
class QTcpServer;
class QUdpSocket;

namespace mx::ed2k {

class ServerManager;
class UploadManager;
class PeerConnection;
class Ed2kRangeSource;
struct PeerDescriptor;

// Hello capabilities we announce (also used by Kad: kadVersion / UDP ports)
struct LocalCaps {
    quint16 udpPort = 0;       // client UDP port (0 = no UDP re-ask support)
    quint16 kadPort = 0;
    quint8  kadVersion = 0;
    quint32 buddyIp = 0;       // Kad buddy (eD2k order) and its UDP port while we are firewalled
    quint16 buddyPort = 0;
    bool    directCallback = false;   // firewalled, but UDP open: OP_DIRECTCALLBACKREQ works
};

// What the Kad node tells the shared engine (hello client ID, which sources we can use)
struct KadState {
    bool running = false;
    bool connected = false;
    bool tcpFirewalled = true;
    quint32 ip = 0;            // eD2k order, 0 = unknown
    bool udpChecked = false;   // [diag-1] the Kad UDP firewall test has a verdict ...
    bool udpOpen = false;      // ... and the shared UDP socket is reachable
};

struct DownloadCtx {
    QString id;
    mx::FileHashes hashes;
    qint64 size = 0;
    QString name;
    bool findingSources = false;
    bool hashsetKnown = false;
    bool aichKnown = false;
    QHash<QByteArray, QSet<quint32>> aichVotes;   // aich -> distinct peer IPs
    qint64 nextServerAskMs = 0;
    QSet<QString> knownSources;                   // peer keys already reported
    QList<QByteArray> partHashes;                 // verified hashset (for serving the partial file)
};

class Ed2kCore : public QObject {
    Q_OBJECT
public:
    explicit Ed2kCore(QObject *parent = nullptr);
    ~Ed2kCore() override;

    // init() is idempotent: the first facade (ED2K or Kad plugin) initialises the shared engine.
    void init(const mx::PluginContext &ctx);
    bool initialized() const { return m_inited; }
    // The engine runs while at least one facade uses it. Servers only run for the ED2K facade.
    enum Facade { FacadeEd2k = 1, FacadeKad = 2 };
    void acquire(int facade);
    void release(int facade);
    bool facadeActive(int facade) const { return (m_facades & facade) != 0; }
    void start() { acquire(FacadeEd2k); }
    void stop() { release(FacadeEd2k); }
    bool running() const { return m_running; }

    // ---- identity / config
    const mx::PluginContext &ctx() const { return m_ctx; }
    const Config &cfg() const { return m_cfg; }
    QString dataDir() const { return m_dataDir; }
    QByteArray userHash() const { return m_userHash; }
    QString nick() const;
    QString clientName() const;
    quint32 emuleVersionTag() const;            // CT_EMULE_VERSION value
    quint16 tcpPort() const { return m_tcpPort; }
    qint64 peerConnectTimeoutMs() const;   // [net-2] connection/connectTimeoutSec
    bool listening() const { return m_listening; }
    QString tcpBindError() const { return m_tcpError; }   // [diag-1]
    QString udpBindError() const { return m_udpError; }
    quint16 wantedUdpPort() const { return m_wantUdp; }
    LocalCaps &caps() { return m_caps; }
    bool cryptSupported() const { return m_cryptSupported; }
    bool cryptRequested() const { return m_cryptRequested; }
    bool cryptRequired() const { return m_cryptRequired; }
    bool allowLan() const { return m_allowLan; }
    int uploadSlots() const;
    QNetworkAccessManager *nam();
    qint64 nowMs() const { return m_clock.elapsed(); }

    // ---- server state (convenience)
    ServerManager *servers() const { return m_servers; }
    UploadManager *uploads() const { return m_uploads; }
    quint32 clientId() const;                   // 0 if not connected
    bool lowId() const;
    quint32 serverIp() const;                   // current server or 0
    quint16 serverPort() const;
    quint32 publicIp() const;                   // learned from server (eD2k order) or 0

    // ---- UDP
    bool sendUdp(quint32 ip, quint16 port, const QByteArray &datagram);
    void accountUdp(int dir, const QByteArray &datagram);   // [glue-3] bandwidth graph
    quint16 udpLocalPort() const;
    // eMule client UDP (0xC5), obfuscated with the receiver's user hash when it is given and we
    // know our public IP
    bool sendClientUdp(quint32 ip, quint16 port, quint8 opcode, const QByteArray &payload, const QByteArray &obfuscateForUserHash);
    // set by the Kad plugin: tries our Kad keys as well (KadID, receiver verify key)
    std::function<kad::Deobfuscated(quint32 ip, const QByteArray &raw)> deobfuscator;
    // set by the Kad plugin: KADEMLIA_CALLBACK_REQ via the source's buddy
    std::function<bool(const PeerDescriptor &peer, const QByteArray &fileHash)> kadBuddyCallback;

    // ---- Kad state
    KadState &kadState() { return m_kad; }
    const KadState &kadState() const { return m_kad; }
    quint32 helloClientId() const;              // client ID we put into hellos (server ID, Kad IP, or 1)
    bool canAcceptIncoming() const;             // others can connect to us (HighID or Kad open)
    bool firewalledForPeers() const { return !canAcceptIncoming(); }
    bool sendDirectCallback(const PeerDescriptor &peer);   // OP_DIRECTCALLBACKREQ to a type-6 Kad source

    // ---- peers
    // Outgoing connection subject to the connection limits (deferred if needed).
    PeerConnection *connectPeer(const PeerDescriptor &d, bool allowObfuscation = true);
    int connectionCount() const { return int(m_peers.size()); }
    void obfuscationCounts(int *obfuscated, int *all) const;   // [enc-1] open peer connections
    int rateDownBps() const { return m_rateDown; }
    int rateUpBps() const { return m_rateUp; }
    void registerSource(Ed2kRangeSource *s);
    void unregisterSource(Ed2kRangeSource *s);
    bool isOwnAddress(quint32 ip, quint16 port) const;
    bool acceptableSourceIp(quint32 ip) const;

    // Re-ask registry (per peer + file). Keeps us below eMule's ban thresholds across
    // RangeSource objects and restarts.
    qint64 msSinceAsk(const QString &peerKey, const QByteArray &fileHash) const;   // -1 = never
    void noteAsk(const QString &peerKey, const QByteArray &fileHash);
    bool sourceExchangeAllowed(const QString &peerKey, const QByteArray &fileHash) const;
    void noteSourceExchange(const QString &peerKey, const QByteArray &fileHash);

    // ---- downloads
    DownloadCtx *ensureDownload(const QString &id, const mx::FileHashes &h, qint64 size, const QString &name);
    DownloadCtx *download(const QString &id);
    QList<DownloadCtx *> downloadsByHash(const QByteArray &ed2k);
    void findSources(const QString &id, const mx::FileHashes &h, qint64 size, const QString &name);
    void stopFindSources(const QString &id);
    void learnHashset(const QByteArray &fileHash, const QList<QByteArray> &parts);
    void learnAich(const QByteArray &fileHash, const QByteArray &aich, quint32 fromIp);
    void addSources(const QByteArray &fileHash, const QList<PeerDescriptor> &list, const QString &origin,
                    mx::Net net = mx::Net::ED2K);
    bool hashsetKnown(const QByteArray &fileHash) const;

    // ---- shared files
    void setSharedFiles(const QList<mx::SharedFile> &files);
    bool sharedFile(const QByteArray &ed2k, mx::SharedFile *out) const;
    int sharedCount() const { return int(m_shared.size()); }
    // Partially downloaded files (the library exposes their verified parts through read()):
    bool isPartialFile(const QByteArray &ed2k) const;
    QList<bool> availableParts(const QByteArray &ed2k, qint64 size);   // cached ~30 s
    // <count 2><bitmap> of our verified parts (eMule part status), or <0 2> if we have nothing
    QByteArray partStatus(const QByteArray &ed2k, qint64 size);
    QList<QByteArray> downloadPartHashes(const QByteArray &ed2k) const;

    void log(const QString &text);

signals:
    void statusChanged();
    void serversChanged();
    void searchHits(quint32 queryId, const QList<mx::SearchHit> &hits);
    void searchDone(quint32 queryId);
    void sourcesFound(const QString &downloadId, const QList<mx::SearchSource> &sources);
    void hashesLearned(const QString &downloadId, const mx::FileHashes &more);
    void hashSetLearned(const QString &downloadId, const QString &kind, qint64 unitSize, const QList<QByteArray> &hashes);
    void uploadChanged(const mx::TransferInfo &upload);
    void uploadFinished(const QString &uploadId);
    void logMessage(const QString &text);
    void serverMessage(const QString &serverId, const QString &text);   // [glue-2]
    // raw UDP datagrams not handled by eD2k server code (eMule client UDP 0xC5, Kad 0xE4/0xE5)
    void udpDatagram(quint32 ip, quint16 port, const QByteArray &datagram);
    // Kad datagram (plain, 0xE4/0xE5 first) with the obfuscation verify keys it carried
    void kadDatagram(quint32 ip, quint16 port, const QByteArray &plain, bool obfuscated, quint32 receiverVerifyKey,
                     quint32 senderVerifyKey);
    // Kad-related TCP packets on peer connections (OP_KAD_FWTCPCHECK_ACK, OP_FWCHECKUDPREQ, OP_CALLBACK, buddy pings)
    void kadTcpPacket(mx::ed2k::PeerConnection *pc, quint8 opcode, const QByteArray &payload);
    // every finished peer handshake (Kad: buddy connections, bootstrap from hellos with a Kad port)
    void peerHandshake(mx::ed2k::PeerConnection *pc);

private:
    void onNewConnection();
    void onUdpReadyRead();
    void handleClientUdp(quint32 ip, quint16 port, const QByteArray &plain, bool obfuscated);
    void startBase();
    void stopBase();
    void adoptPeer(PeerConnection *pc);
    void onPeerHandshake(PeerConnection *pc);
    void onCallbackRequested(quint32 ip, quint16 port, quint8 cryptOptions, const QByteArray &userHash);
    void tick();
    void startPendingConnects();
    void loadState();
    void saveState();
    bool bindListener();

    mx::PluginContext m_ctx;
    Config m_cfg;
    QString m_dataDir;
    QByteArray m_userHash;
    quint16 m_tcpPort = 4662;
    quint16 m_udpPort = 4672;
    bool m_listening = false;
    QString m_tcpError, m_udpError;   // [diag-1]
    quint16 m_wantUdp = 0;
    bool m_running = false;
    bool m_inited = false;
    int m_facades = 0;
    KadState m_kad;
    QHash<quint32, qint64> m_directCallbackLog;   // ip -> last OP_DIRECTCALLBACKREQ accepted (ms)
    bool m_cryptSupported = true, m_cryptRequested = false, m_cryptRequired = false;
    bool m_allowLan = false;
    LocalCaps m_caps;
    QElapsedTimer m_clock;
    QTcpServer *m_listener = nullptr;
    QUdpSocket *m_udp = nullptr;
    QNetworkAccessManager *m_nam = nullptr;
    ServerManager *m_servers = nullptr;
    UploadManager *m_uploads = nullptr;
    QList<QPointer<PeerConnection>> m_peers;
    QList<QPointer<PeerConnection>> m_pendingConnects;
    QList<QPointer<Ed2kRangeSource>> m_sources;
    QHash<QString, qint64> m_askLog;        // "<peer>|<filehex>" -> epoch ms
    QHash<QString, qint64> m_sxLog;
    QHash<QString, DownloadCtx> m_downloads;
    QHash<QByteArray, mx::SharedFile> m_shared;
    struct PartCache { qint64 atMs = -1; QList<bool> parts; };
    QHash<QByteArray, PartCache> m_partCache;
    QTimer m_tick;
    qint64 m_lastHalfOpenWindowMs = 0;
    int m_connectsInWindow = 0;
    qint64 m_closedIn = 0, m_closedOut = 0, m_prevIn = 0, m_prevOut = 0;
    int m_rateDown = 0, m_rateUp = 0;
    int m_tickCount = 0;
};

} // namespace mx::ed2k
