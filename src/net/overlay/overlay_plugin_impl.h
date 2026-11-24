// SPDX-License-Identifier: GPL-3.0-or-later
// Net::Overlay: the OctoMX peer network (kOverlayProtocol below). Private to mxnet_overlay.
// Wire format: PROTOCOL.md. Split over several .cpp files:
//   overlay_plugin.cpp    lifecycle, peers, hello, control dispatch, signed envelopes, presence, status
//   overlay_chat.cpp      rooms (ChatBackend), directory, history, flood control
//   overlay_users.cpp     private messages, browse, whois, hotlist presence
//   overlay_search.cpp    overlay keyword search, find-sources, hash links (registry + LRU)
//   overlay_transfer.cpp  RangeSource (download side) + upload slots/queue (serve side)
#pragma once

#include "overlay_plugin.h"
#include "ovl_crypto.h"
#include "ovl_util.h"
#include "rats_node.h"

#include "mx/upload_slots.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QJsonArray>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QThreadPool>
#include <QtCore/QTimer>

#include <deque>
#include <functional>

namespace mx::ovl {

class OverlaySource;
struct UploadSession;

// the protocol identifier (kept for compatibility): Noise prologue and DHT rendezvous key
// SHA1(kOverlayProtocol), so nodes of every version find and talk to each other; never rename it
inline constexpr const char *kOverlayProtocol = "winmx-ng/1";
inline constexpr int kWireVersion = 1;
inline constexpr const char *kCtlChannel = "mx";    // control messages ('J'/'Z' + JSON)
inline constexpr const char *kDataChannel = "mxd";  // binary data frames
inline constexpr const char *kTopicPresence = "mx/presence";
inline constexpr const char *kTopicRooms = "mx/rooms";
inline constexpr const char *kTopicSearch = "mx/search";
inline constexpr const char *kTopicRelay = "mx/relay";
inline constexpr const char *kTopicHashLinks = "mx/hashlinks";

// Timers; tests shorten them through OverlayPlugin::setTimings().
struct OverlayTimings {
    int presenceMs = 60000;        // presence re-announce ([ovl-2] was 120 s; announced as "pi")
    int presenceTimeoutMs = 330000;   // users that announce no "pi" (4.2 and older): 2.75 x their 120 s
    int presenceGraceMs = 30000;   // users with "pi": offline after 2 x pi + this without presence
    int pingWaitMs = 4000;         // [ovl-2] lost the direct link: wait this long for a pong via the mesh
    int linkIdlePingMs = 4000;     // [ovl-2] a verified link silent this long gets a control "ping"
    int linkDeadMs = 12000;        // [ovl-2] ...and is dropped after this long without any byte (a crashed
                                   // peer's UDP link would otherwise linger for librats' 45 s idle timeout)
    int pmAckMs = 6000;            // [ovl-2] direct PM: receipt wait before also relaying it
    int pmRelayAckMs = 15000;      // [ovl-2] relayed PM: receipt wait before "not delivered"
    int pmRetryMs = 120000;        // [ovl-2] undelivered PM to a user who is online: try again this often
    int pmGiveUpMs = 3600000;      // [ovl-2] stop retrying an undelivered PM after this long
    int heartbeatMs = 30000;       // room member heartbeat
    int memberTimeoutMs = 95000;
    int directoryMs = 60000;       // room directory announcement
    int directoryTimeoutMs = 180000;
    int historyDelayMs = 1500;     // after joining, ask for recent history
    int searchQuietMs = 6000;      // overlay search done after this long without hits...
    int searchMaxMs = 20000;       // ...or this long in total
    int sourcesRepeatMs = 300000;  // find-sources re-ask
    int dialWaitMs = 15000;        // connect-on-demand wait (dial, connect-back, relay circuit) before failing
    int requestTimeoutMs = 60000;  // range request without progress
    int uploadIdleMs = 60000;      // active upload slot without requests
    int aloneAfterMs = 15000;      // "Connecting..." → "Connected (no other OctoMX users found yet)"
};

class OverlayPlugin : public OverlayApi {
    Q_OBJECT
public:
    explicit OverlayPlugin(QObject *parent = nullptr);
    ~OverlayPlugin() override;

    // ---- NetworkPlugin
    Net net() const override { return Net::Overlay; }
    int capabilities() const override;
    void init(const PluginContext &ctx) override;
    void start() override;
    void stop() override;
    NetworkStatus status() const override { return m_status; }
    QList<ServerEntry> servers() const override;
    void addServer(const QString &address, const QString &name) override;
    void removeServer(const QString &id) override;
    void connectServer(const QString &id) override;
    void disconnectServer(const QString &id) override;
    void search(const SearchQuery &q) override;
    void cancelSearch(quint32 queryId) override;
    void findSources(const QString &downloadId, const FileHashes &h, qint64 size, const QString &name) override;
    void stopFindSources(const QString &downloadId) override;
    RangeSource *openSource(const QString &downloadId, const FileHashes &h, qint64 size,
                            const SearchSource &src) override;
    void setSharedFiles(const QList<SharedFile> &files) override;
    void lookupHashLinks(const FileHashes &known, qint64 size) override;
    void publishHashLinks(const FileHashes &all, qint64 size, const QString &name) override;
    void browse(quint32 requestId, const UserRef &u) override;
    void whois(const UserRef &u) override;
    void cancelUpload(const QString &uploadId) override;
    bool startUploadNow(const QString &uploadId) override;                 // [glue-2]
    bool setUploadPriority(const QString &uploadId, int priority) override; // [glue-2]
    // [diag-1] Connection Test (overlay_probe.cpp)
    PluginDiagnostics diagnostics() const override;
    bool probePorts(quint32 token, const QList<PortProbe> &ports) override;

    // ---- ChatBackend
    void refreshRoomList() override;
    QList<ChatRoomInfo> roomList() const override;
    void joinRoom(const QString &roomIdOrName) override;
    void leaveRoom(const QString &roomId) override;
    QList<ChatUser> roomUsers(const QString &roomId) const override;
    void sendRoomMessage(const QString &roomId, const QString &text, ChatKind kind) override;
    void sendPrivateMessage(const UserRef &to, const QString &text) override;
    bool sendPrivateMessageWithId(const UserRef &to, const QString &text, const QString &msgId) override;
    void refusePrivateMessage(const ChatMessage &msg, const QString &reason) override;
    void setTopic(const QString &roomId, const QString &topic) override;
    void setMotd(const QString &roomId, const QString &motd) override;
    QObject *chatEvents() override { return m_events; }

    // ---- OverlayApi
    UserRef localUser() const override;
    bool isUserOnline(const UserRef &u) const override;
    void setHotlist(const QList<UserRef> &users) override;
    void setIgnored(const QList<UserRef> &users) override;   // [net-3]
    void connectPeer(const QString &hostPort) override;

    // ---- tests / probes
    struct TestOptions {
        bool dht = true, mdns = true, pex = true, reconnect = true;
        int listenPort = -1;              // -1 = connection/tcpPort setting
        bool advertise = true;            // false: announce no dialable addresses (forces relaying)
        QString publicFirst;              // [ovl-2] announce this address first (tests: the shared public
                                          // address of two PCs behind one NAT that does no hairpinning)
        bool firewalled = false;          // [ovl-2] refuse direct inbound links (as behind a NAT without a
                                          // forwarded port); relayed circuits are still accepted
        QString protocol;                 // empty = kOverlayProtocol
    };
    void setTestOptions(const TestOptions &o) { m_testOpts = o; }
    void setTimings(const OverlayTimings &t) { m_t = t; }
    RatsNode *node() const { return m_node; }
    QString nodeId() const { return m_node ? m_node->nodeId() : QString(); }
    quint16 listenPort() const { return m_node ? m_node->listenPort() : 0; }
    int connectedUsers() const;       // peers whose hello was verified
    bool helloVerified(const QString &nodeId) const   // [bw-2] tests: this node's hello was verified
    { auto it = m_peers.constFind(nodeId); return it != m_peers.constEnd() && it->hello; }

    // ---- used by OverlaySource / uploads (same module)
    struct PeerState {
        QString address;
        bool inbound = false;
        QString transport;            // [diag-1] "tcp" / "udp" / "relay"
        bool hello = false;
        QString userId;
        qint64 connectedMs = 0;
        QList<QByteArray> early;      // control messages received before hello (bounded)
        int dropped = 0;
        qint64 lastRxMs = 0;          // [ovl-2] last byte from this peer (liveness)
        qint64 lastPingMs = 0;
    };
    void dropPeer(const QString &nodeId, const QString &reason);   // [ovl-2] forget a link (after the checks)
    void checkLinks();
    struct UserInfo {
        QString name;                 // display name ("Name_12345")
        QString nodeId;
        QStringList addrs;
        QByteArray xpub;
        int files = 0;
        QString conn;
        QString client;
        qint64 lastSeenMs = 0;
        qint64 sinceMs = 0;           // their reported online-since
        bool online = false;
        bool direct = false;          // hello on a live connection
        QStringList caps;             // [ovl-2] capabilities from hello / presence ("pm_ack", "reach", "ping")
        int presenceIntervalSec = 0;  // [ovl-2] "pi" from their presence (0: an older node, 120 s)
        qint64 pingSentMs = 0;        // [ovl-2] liveness ping via the mesh after the direct link went
        QString pingId;
        qint64 offlineSinceMs = 0;    // [ovl-2] when they went offline (bye, lost link, timeout)
        QString offlineWhy;
        bool has(const char *cap) const { return caps.contains(QLatin1String(cap)); }
    };
    // Send a control message to a user/node; dials (known addresses) and queues it for up to
    // dialWaitMs if not connected. Returns false if the target is unknown and not connected.
    bool sendToUser(const QString &userId, const QJsonObject &msg, std::function<void(bool)> done = {});
    bool sendToNode(const QString &nodeId, const QJsonObject &msg, const QStringList &addrs = {},
                    std::function<void(bool)> done = {});
    bool sendDataFrame(const QString &nodeId, const QByteArray &frame);
    bool isWritable(const QString &nodeId) const;
    const UserInfo *user(const QString &userId) const;
    QString nodeForUser(const QString &userId) const;
    PluginContext &ctx() { return m_ctx; }
    const OverlayTimings &timings() const { return m_t; }
    bool onHotlist(const QString &userId) const { return m_hotlist.contains(userId); }
    bool isIgnoredUser(const QString &userId) const;   // [net-3] on the user's ignore list (by key or by name)
    QString displayName() const { return m_displayName; }
    QString connectionLabel() const;
    int freeSlots() const;
    int totalSlots() const;
    // [net-3] the shared pool (transfers/maxUploads over the OctoMX network, eD2k and Gnutella) when
    // the core gives one, else this network's own count
    bool slotFree();
    void takeSlot(UploadSession *s, bool forced);
    void giveBackSlot(UploadSession *s);
    void reportWaiting();
    QPointer<UploadSlots> m_slots;
    UserRef userRefFor(const QString &userId) const;
    void unregisterSource(quint32 sid);

private:
    // overlay_plugin.cpp
    QVariant setting(const QString &key, const QVariant &def = QVariant()) const;
    void updateDisplayName();
    void onPeerConnected(const RatsPeer &p);
    void onPeerDisconnected(const QString &nodeId, const QString &reason);
    void onBytes(const QString &nodeId, const QString &channel, const QByteArray &bytes);
    void onTopic(const QString &topic, const QString &origin, const QByteArray &data);
    void handleControl(const QString &nodeId, const QJsonObject &o);
    void sendHello(const QString &nodeId);
    void onHello(const QString &nodeId, const QJsonObject &o);
    void flushPending(const QString &nodeId, const QString &key = QString());   // key: "u:<user id>" queue
    void expirePending();
    void sendRaw(const QString &nodeId, const QJsonObject &msg);
    void sendUrgent(const QString &nodeId, const QJsonObject &msg);   // [up-1] a live copy, ahead of queued data
    QJsonObject newBody(const char *type) const;
    QByteArray makeEnvelope(const QString &topic, const QJsonObject &body) const;
    void publishSigned(const QString &topic, const QJsonObject &body);
    void subscribeTopic(const QString &topic, int maxBytes);
    void announcePresence();
    void onPresence(const QString &userId, const QJsonObject &body, bool bye);
    void touchUser(const QString &userId, const QJsonObject &body, const QString &nodeId);
    void setUserOnline(const QString &userId, bool online);
    void tick();
    void updateStatus(bool force = false);
    void dialBootstrap();
    QStringList ourAddresses() const;

    // overlay_chat.cpp
    struct Member { QString name; int files = 0; QString conn; qint64 lastMs = 0; };
    struct Room {
        QString id;            // normalised name
        QString name;          // display name
        QString topicStr;      // pub-sub topic
        bool joined = false;
        QString topic;
        qint64 topicTs = 0;
        QString topicBy;
        QString motd;          // message of the day, lines separated by '\n'
        qint64 motdTs = 0;
        QString motdBy;
        bool greeted = false;  // topic / MOTD lines shown since the join
        QString shownTopic, shownMotd;
        QHash<QString, Member> members;   // userId → member (not us)
        std::deque<QByteArray> history;   // raw signed envelopes (msg / action / topic)
        int historyAsks = 0;
        bool historyDone = false;
        qint64 joinedMs = 0;
    };
    struct DirEntry { QString name; QString topic; int users = 0; qint64 lastMs = 0; };
    void chatInit();
    Room *room(const QString &idOrName);
    void onRoomEnvelope(Room &r, const QString &userId, const QJsonObject &body, const QByteArray &raw,
                        bool fromHistory);
    void onDirectory(const QJsonObject &body);
    // [ovl-2] forward the traffic of every room in the directory without joining it: GossipSub relays a
    // topic only among its subscribers, so two members linked only through a non-member never met
    void relayRoomTopics();
    QSet<QString> m_relayTopics;
    void roomHeartbeat(Room &r);
    void greetRoom(const QString &roomId);
    static QString cleanMotd(const QString &raw);
    void showRoomState(Room &r);
    void announceRooms();
    void askHistory(const QString &roomId);
    void onHistoryRequest(const QString &nodeId, const QJsonObject &o);
    void onHistory(const QString &nodeId, const QJsonObject &o);
    void expireMembers();
    void emitRoomMessage(const Room &r, const QString &userId, const QString &name, const QString &text,
                         ChatKind kind, qint64 ts = 0);
    static QString roomTopic(const QString &roomId);
    ChatRoomInfo roomInfo(const Room &r) const;
    bool floodOk(const QString &userId);

    // overlay_users.cpp
    void onPm(const QString &fromUser, const QJsonObject &o, bool relayed, const QString &nodeId = QString());
    void onRelayEnvelope(const QString &fromUser, const QJsonObject &body);
    // [ovl-2] tracked private messages (receipts, retries) and reachability
    struct OutPm {
        QString id, uid;
        UserRef to;
        QJsonObject msg;              // the inner pm message
        enum Stage { WaitDirectAck, WaitRelayAck, WaitUser } stage = WaitUser;
        qint64 createdMs = 0, deadlineMs = 0, lastTryMs = 0;
        int tries = 0;
        bool reported = false;        // NotDelivered already shown for the current failure
        bool tracked = true;          // false: the caller gave no id, failures become Error lines
    };
    void pmTry(OutPm &p);
    bool pmRelay(OutPm &p);
    void pmStatus(const OutPm &p, PmDelivery st, const QString &detail = QString());
    void pmTick();
    void pmRetryFor(const QString &userId);
    void onPmAck(const QString &fromUser, const QString &id);
    void onPmRefused(const QString &fromUser, const QString &id, const QString &reason);
    void answerPm(const QString &fromUser, const QString &id, const QString &refusal, bool relayed, const QString &nodeId);
    void publishRelay(const char *type, const QString &toUser, QJsonObject extra = {});
    QString pmRefusal(const QString &userId) const;
    QString refusalText(const QString &name, const QString &reason) const;
    void requestReach(const QString &nodeId);
    void onReachRequest(const QString &fromUser, const QJsonObject &body);
    void startPing(const QString &userId);
    void userWentOffline(const QString &userId, const QString &why);
    void reconcilePeers();
    qint64 presenceTimeoutFor(const UserInfo &u) const;
    QJsonArray ourCaps() const;
    QString unreachableText(const QString &userId) const;   // why a browse / transfer could not connect
    QHash<QString, OutPm> m_outPms;           // msg id → message we wait to deliver
    QSet<QString> m_pmPartners;               // users we exchanged PMs with (online / offline lines)
    QHash<QString, QString> m_pmVerdicts;     // incoming msg id → "" (accepted) or refusal (re-answer copies)
    QStringList m_pmVerdictOrder;
    QString m_pmInFlight, m_pmInFlightRefusal;   // set while the core looks at an incoming PM
    QHash<QString, qint64> m_reachAskedMs;    // node id → last connect-back request we sent
    QHash<QString, qint64> m_reachServedMs;   // user id → last connect-back we did for them
    qint64 m_lastReconcileMs = 0;
    qint64 m_lastPeerLostMs = 0;
    void onBrowseRequest(const QString &nodeId, const QJsonObject &o);
    void onBrowse(const QString &nodeId, const QJsonObject &o);
    void onWhoisRequest(const QString &nodeId, const QJsonObject &o);
    void onWhois(const QString &nodeId, const QJsonObject &o, bool error);
    bool allowedBy(const QString &settingKey, const QString &userId) const;
    void expireRequests();

    // overlay_search.cpp
    void onSearchEnvelope(const QString &userId, const QJsonObject &body);
    void onHits(const QString &nodeId, const QJsonObject &o);
    void onSources(const QString &nodeId, const QJsonObject &o);
    void onHashLinkAnswer(const QString &nodeId, const QJsonObject &o);
    void onHashLinkPublish(const QJsonObject &body);
    QJsonObject fileToJson(const SharedFile &f, bool withPath) const;
    void finishSearches(bool all = false);
    void askSources();
    void loadHashLinks();
    void saveHashLinks();
    void cacheHashLink(const FileHashes &h, qint64 size, const QString &name);
    bool lookupLocal(const FileHashes &h, qint64 size, FileHashes *all, qint64 *outSize, QString *name) const;

    // overlay_probe.cpp [diag-1] "probe my ports": connect back to the requester's own address
    void onProbeRequest(const QString &nodeId, const QJsonObject &o);
    void onProbeAnswer(const QString &nodeId, const QJsonObject &o);
    void finishProbe(quint32 token);
    struct ProbeReq { QList<PortProbe> ports; QString nonce; int answers = 0; int asked = 0; qint64 deadlineMs = 0; };
    QHash<quint32, ProbeReq> m_probeReqs;
    QHash<QString, qint64> m_probeServedMs;   // requester user id -> last probe we ran for it
    std::deque<qint64> m_probeServedAll;      // all probes we ran (rate limit)
    bool m_inboundTcp = false, m_inboundUdp = false;
    int m_wantPort = 0;

    // overlay_transfer.cpp (serve side)
    void onGetOpen(const QString &nodeId, const QJsonObject &o);
    void onGet(const QString &nodeId, const QJsonObject &o);
    void onGetClose(const QString &nodeId, const QJsonObject &o, bool cancelOnly);
    void onHashSetRequest(const QString &nodeId, const QJsonObject &o);   // [engine-3]
    void onHashSetAnswer(const QString &nodeId, const QJsonObject &o);
    void onSlot(const QString &nodeId, const QJsonObject &o);
    void onHave(const QString &nodeId, const QJsonObject &o);   // [up-1]
    void onGot(const QString &nodeId, const QJsonObject &o);
    void onDataFrame(const QString &nodeId, const QByteArray &frame);
    void pumpUpload(UploadSession *s);
    void promoteQueue();
    void sendQueuePositions();
    // [up-1] endState / endText: what the upload row says at the end (the row is "Complete" only
    // when the whole file was sent; the core's upload tracker enforces that)
    void closeUpload(UploadSession *s, const QString &reason, bool notifyPeer,
                     TransferState endState = TransferState::Completed, const QString &endText = QString());
    void refreshPartial(UploadSession *s, bool force);                // [up-1] tell the downloader what grew
    QList<QByteArray> servableEd2kSet(const FileHashes &h, qint64 size) const;   // [up-1] verified sets only
    void uploadsTick();
    void dropPeerUploads(const QString &nodeId);
    void reportUpload(UploadSession *s, bool force);
    void reportFinal(UploadSession *s, TransferState endState, const QString &endText);   // [up-1]

    friend class OverlaySource;

    PluginContext m_ctx;
    TestOptions m_testOpts;
    OverlayTimings m_t;
    QString m_dataDir;
    Identity m_id;
    QString m_userId;
    QString m_screenName;
    QString m_displayName;
    QPointer<RatsNode> m_node;
    QObject *m_events = nullptr;     // a ChatEvents
    bool m_running = false;
    QElapsedTimer m_startedAt;
    qint64 m_onlineSinceMs = 0;
    bool m_sawInbound = false;
    NetworkStatus m_status;
    QTimer m_tick;
    int m_tickCount = 0;
    qint64 m_lastPresenceMs = 0, m_lastHeartbeatMs = 0, m_lastDirectoryMs = 0, m_lastBootstrapMs = 0;
    QString m_announcedConn;   // [set-1] the connection type last told (presence, room lists)

    QHash<QString, PeerState> m_peers;        // librats node id → state
    QHash<QString, UserInfo> m_users;         // user id → info
    QHash<QString, QString> m_nodeUser;       // node id → user id (verified by hello)
    QSet<QString> m_hotlist;
    QSet<QString> m_ignoredIds;               // [net-3] ignored user keys (lower case hex)
    QStringList m_ignoredNames;               // [net-3] entries added by name only (/ignore Bob)
    struct Pending { QByteArray bytes; qint64 expiresMs; std::function<void(bool)> done; };
    QHash<QString, QList<Pending>> m_pending; // node id → messages waiting for connect + hello
    RateGate m_peerGate{50.0, 200.0};
    RateGate m_floodGate{0.6, 6.0};
    QSet<QString> m_floodNotified;
    SeenSet m_seen{8192};                     // envelope / message ids

    // chat
    QHash<QString, Room> m_rooms;
    QHash<QString, DirEntry> m_directory;

    // users: requests in flight
    struct BrowseReq { UserRef user; QString nodeId; QString rid; qint64 lastMs = 0; int got = 0; bool resent = false; };
    void sendBrowseRequest(quint32 requestId);   // [ovl-2] (re)send, failing with a clear reason
    QHash<quint32, BrowseReq> m_browse;
    struct WhoisReq { UserRef user; QString rid; qint64 sentMs = 0; };
    QHash<QString, WhoisReq> m_whois;         // rid → request
    // [bw-2] answers pushed to a node (search hits, sources) in the last seconds: sent again when
    // librats moves that node to another link (what went out on the replaced link may be lost)
    struct SentAnswer { QString nodeId; QJsonObject msg; qint64 ms = 0; };
    QList<SentAnswer> m_sentAnswers;
    void rememberAnswer(const QString &nodeId, const QJsonObject &msg);

    // search / sources / hash links
    QList<SharedFile> m_shared;
    QStringList m_sharedLower;                // lower-case names, same order as m_shared
    bool m_haveSharedList = false;
    struct OvSearch { quint32 id = 0; SearchQuery q; QElapsedTimer started; qint64 lastHitMs = 0; int emitted = 0;
                      QSet<QString> seen; };
    QHash<QString, OvSearch> m_searches;      // qid string → search
    struct FindReq { QString downloadId; FileHashes h; qint64 size = 0; QString name; QString qid; qint64 lastAskMs = 0;
                     QSet<QString> reported; };
    QHash<QString, FindReq> m_find;           // download id → request
    struct HlReq { FileHashes known; qint64 size = 0; qint64 sentMs = 0; };
    QHash<QString, HlReq> m_hlReqs;           // qid → lookup
    struct HashLink { FileHashes h; qint64 size = 0; QString name; qint64 lastMs = 0; };
    QList<HashLink> m_hashLinks;              // LRU, most recent last
    bool m_hashLinksDirty = false;

    // transfers
    QHash<quint32, QPointer<OverlaySource>> m_sources;   // our downloads by session id
    QHash<QString, UploadSession *> m_uploads;           // "<node>/<sid>" → session
    QList<UploadSession *> m_uploadQueue;                // waiting for a slot
    QThreadPool *m_readPool = nullptr;                   // disk reads for uploads
    quint64 m_uploadSerial = 0;
    qint64 m_corruptStart = 0, m_corruptEnd = 0;          // [engine-3] test hook overlay/testCorruptUploads
    qint64 m_corruptedBytes = 0;
public:
    qint64 corruptedBytes() const { return m_corruptedBytes; }   // tests
};

} // namespace mx::ovl
