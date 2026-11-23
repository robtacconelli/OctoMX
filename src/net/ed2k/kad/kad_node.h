// SPDX-License-Identifier: GPL-3.0-or-later
// KadNode: one Kad2 node: identity (KadID, UDP verify base key), routing table, packet
// processing for every Kad2 opcode, flood protection and request tracking, iterative lookups,
// keyword / source searches, publishing of our shared files, TCP/UDP firewall checks, external
// port detection, buddies, a small storing index and nodes.dat persistence.
//
// The node owns no sockets and no timers. Everything outside (UDP send, TCP connections for
// firewall checks / buddies, clock, logging) goes through KadHost, and the owner calls tick() once
// per second. That keeps the node testable with a simulated network and a virtual clock.
#pragma once

#include "kad_index.h"
#include "kad_routing.h"
#include "kad_search.h"

#include "mx/types.h"

#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QSet>
#include <memory>

namespace mx::ed2k::kad {

struct KadConfig {
    QString dataDir;                 // nodes.dat, preferencesKad.dat, kad.json (empty = no persistence)
    bool allowLan = false;           // accept private / loopback contacts (LAN mode, tests)
    bool allowSameIp = false;        // test switch: several contacts per IP
    bool storeIndex = true;          // store what others publish to us (bounded index)
    bool firewallChecks = true;      // TCP firewall check, UDP firewall test, external port lookup
    bool forceTcpFirewalled = false; // tests: never consider ourselves TCP-open
    bool forceUdpFirewalled = false; // tests: behave as UDP firewalled (needs a buddy)
    int warmStartHellos = 10;        // HELLO_REQ to this many known contacts right after start
    int findBuddyDelaySecs = 300;    // first buddy search after start (eMule: 5 min)
    quint32 searchTolerance = SEARCHTOLERANCE;   // simulations of small networks widen this
    bool buddies = true;
    bool publish = true;
    bool fastStart = true;           // self lookup 5 s after start (eMule: 3 min)
    int maxPacketsPerSec = 80;       // outgoing Kad datagrams (token bucket, burst 2 s)
    int maxSearchResults = 2000;
    KadIndexLimits index;
};

struct KadSource {                   // a file source from KADEMLIA2_SEARCH_RES
    quint8 type = 0;                 // 1/4 HighID, 3/5 firewalled with buddy, 6 direct UDP callback
    QByteArray userHash;             // 16 bytes
    quint32 ip = 0;                  // eD2k order (0 for buddy sources)
    quint16 tcpPort = 0, udpPort = 0;
    quint32 buddyIp = 0;             // eD2k order
    quint16 buddyPort = 0;
    QByteArray buddyId;              // 16 wire bytes
    quint8 cryptOptions = 0;         // 0x01 supports, 0x02 requests, 0x04 requires, 0x08 direct callback
};

struct KadCounters {
    quint64 packetsIn = 0, packetsOut = 0, bytesIn = 0, bytesOut = 0;
    quint64 obfuscatedIn = 0, plainIn = 0, obfuscatedOut = 0;
    quint64 droppedFlood = 0, droppedUnsolicited = 0, droppedMalformed = 0, droppedRate = 0;
    quint64 answeredHello = 0, answeredReq = 0, answeredBootstrap = 0, answeredPing = 0, answeredSearch = 0;
    quint64 answeredPublish = 0, answeredFirewall = 0, answeredBuddy = 0, answeredOther = 0;
    quint64 keywordPublishes = 0, sourcePublishes = 0;   // PUBLISH_RES received for our publishes
    quint64 keywordsPublished = 0, filesPublished = 0;   // keyword / file lookups that got ≥ 1 PUBLISH_RES
    quint64 buddyRequests = 0, buddyOffers = 0;          // FINDBUDDY_REQ sent / FINDBUDDY_RES received
};

class KadHost {
public:
    virtual ~KadHost() = default;
    virtual qint64 kadNowMs() const = 0;     // monotonic
    virtual qint64 kadNowSecs() const = 0;   // wall clock seconds (contact ages, persistence)
    virtual void kadSendDatagram(quint32 ed2kIp, quint16 port, const QByteArray &bytes) = 0;
    virtual QByteArray kadUserHash() const = 0;
    virtual quint16 kadTcpPort() const = 0;
    virtual quint16 kadUdpPort() const = 0;          // our (internal) UDP port
    virtual quint8 kadCryptOptions() const = 0;      // 0x01/0x02/0x04 of our TCP obfuscation settings
    virtual quint32 kadPublicIp() const { return 0; } // eD2k order, from servers etc. (0 = unknown)
    virtual void kadLog(const QString &text) { Q_UNUSED(text) }
    // TCP side (all asynchronous; return false if impossible right now)
    virtual bool kadTcpFirewallCheck(quint32 ed2kIp, quint16 tcpPort, const QByteArray &userHash, quint8 cryptOptions)
    { Q_UNUSED(ed2kIp) Q_UNUSED(tcpPort) Q_UNUSED(userHash) Q_UNUSED(cryptOptions) return false; }
    virtual bool kadUdpFirewallCheck(quint32 ed2kIp, quint16 tcpPort, quint16 internPort, quint16 externPort, quint32 senderKey)
    { Q_UNUSED(ed2kIp) Q_UNUSED(tcpPort) Q_UNUSED(internPort) Q_UNUSED(externPort) Q_UNUSED(senderKey) return false; }
    virtual bool kadRequestBuddy(quint32 ed2kIp, quint16 tcpPort, quint16 udpPort, const QByteArray &userHash, quint8 cryptOptions)
    { Q_UNUSED(ed2kIp) Q_UNUSED(tcpPort) Q_UNUSED(udpPort) Q_UNUSED(userHash) Q_UNUSED(cryptOptions) return false; }
    virtual bool kadIncomingBuddy(quint32 ed2kIp, quint16 tcpPort, const QByteArray &userHash)
    { Q_UNUSED(ed2kIp) Q_UNUSED(tcpPort) Q_UNUSED(userHash) return false; }
    // we are someone's buddy: forward a KADEMLIA_CALLBACK_REQ as OP_CALLBACK over the buddy connection
    virtual bool kadRelayCallback(const QByteArray &checkWire, const QByteArray &fileWire, quint32 requesterHostIp, quint16 requesterTcpPort)
    { Q_UNUSED(checkWire) Q_UNUSED(fileWire) Q_UNUSED(requesterHostIp) Q_UNUSED(requesterTcpPort) return false; }
    virtual bool kadHasServedBuddy() const { return false; }
};

class KadNode : public QObject, public LookupHost {
    Q_OBJECT
public:
    KadNode(KadHost *host, const KadConfig &cfg, QObject *parent = nullptr);
    ~KadNode() override;

    void start();
    void stop();                     // persists nodes.dat / preferences
    bool running() const { return m_running; }
    void tick();                     // once per second

    // ---- identity / state
    const KadId &id() const { return m_id; }
    quint32 udpBaseKey() const { return m_udpKey; }
    quint32 verifyKeyFor(quint32 ed2kIp) const { return udpVerifyKey(m_udpKey, ed2kIp); }
    int contacts() const { return m_routing ? m_routing->count() : 0; }
    bool connected() const;
    bool firewalled() const;          // TCP
    bool udpFirewalled() const;
    bool udpVerified() const { return m_udpVerified; }
    bool directCallbackPossible() const { return firewalled() && !udpFirewalled() && m_udpVerified; }
    quint32 kadIp() const { return m_kadIp; }          // host order, learned from FIREWALLED_RES
    quint16 externalPort() const { return m_externPort; }
    quint32 estimatedUsers() const;
    bool hasBuddy() const { return m_buddyIp != 0; }
    quint32 buddyIp() const { return m_buddyIp; }      // eD2k order
    quint16 buddyPort() const { return m_buddyPort; }
    KadId buddyId() const { return m_id ^ KadId::allOnes(); }
    quint8 connectOptions(bool withCallback) const;
    const KadCounters &counters() const { return m_ctr; }
    int runningLookups() const { return int(m_lookups.size()); }
    int bootstrapPending() const { return int(m_bootstrap.size()); }
    RoutingTable *routing() const { return m_routing.get(); }
    KadIndex &index() { return m_index; }
    const KadConfig &config() const { return m_cfg; }
    QString statusText() const;
    // [diag-1] Connection Test: the state of the TCP / UDP firewall checks
    struct FirewallInfo {
        int tcpAcks = 0;            // TCP connect-backs that arrived this round (2 = open)
        int tcpAnswers = 0;         // FIREWALLED_RES received this round
        int tcpNeeded = 0;          // answers needed for a verdict
        bool tcpChecked = false;    // a verdict exists (this round or the last one)
        bool udpVerified = false;   // a UDP firewall test packet arrived: UDP open
        bool udpTimedOut = false;
        int udpFinished = 0, udpNeeded = 0;
        bool checksOn = true;
    };
    FirewallInfo firewallInfo() const;
    void recheckFirewall();         // start a new check round on the next tick

    // ---- datagrams (from the shared UDP socket)
    Deobfuscated deobfuscate(quint32 ed2kIp, const QByteArray &raw) const;
    // plain Kad datagram (0xE4/0xE5 first)
    void handleDatagram(quint32 ed2kIp, quint16 port, const QByteArray &plain, bool obfuscated, bool validReceiverKey,
                        quint32 senderVerifyKey);

    // ---- bootstrap
    void addBootstrapNode(quint32 hostIp, quint16 port, bool sendNow = true);
    int loadNodesDat(const QByteArray &data, const QString &origin);   // contacts added / queued
    void bootstrapFromPeer(quint32 ed2kIp, quint16 kadPort, quint8 kadVersion);   // eD2k hello with a Kad port
    QByteArray nodesDatSnapshot() const;
    bool saveNodesDat() const;

    // ---- searches
    bool search(const mx::SearchQuery &q, QString *error = nullptr);
    void cancelSearch(quint32 queryId);
    void findSources(const QString &downloadId, const QByteArray &ed2kHash, qint64 size, const QString &name);
    void stopFindSources(const QString &downloadId);
    void setSharedFiles(const QList<mx::SharedFile> &files);
    int publishedKeywordCount() const { return int(m_keywords.size()); }

    // ---- TCP-side events (via the host)
    void onTcpFirewallAck(quint32 ed2kIp);
    // a peer asked us (OP_FWCHECKUDPREQ) to test its UDP ports
    void answerUdpFirewallCheck(quint32 ed2kIp, quint16 internPort, quint16 externPort, quint32 senderKey, bool alreadyKnown);
    void onUdpFirewallCheckFailed(quint32 ed2kIp);
    void buddyConnected(quint32 ed2kIp, quint16 udpPort);
    void buddyLost();
    // OP_CALLBACK from our buddy: is it for us and for a file we have?
    bool checkCallback(const QByteArray &checkWire, const QByteArray &fileWire) const;
    // download from a firewalled source: ask its buddy to make it connect to us
    bool sendCallbackRequest(quint32 buddyEd2kIp, quint16 buddyPort, const QByteArray &buddyIdWire, const QByteArray &fileHash);

    // LookupHost
    void lookupSendRequest(Lookup &l, const KadPeer &peer, quint8 count) override;
    void lookupReached(Lookup &l, const KadPeer &peer) override;
    bool lookupLanAllowed() const override { return m_cfg.allowLan; }
    quint32 lookupTolerance() const override { return m_cfg.searchTolerance; }

signals:
    void searchHits(quint32 queryId, const QList<mx::SearchHit> &hits);
    void searchDone(quint32 queryId);
    void sourcesFound(const QString &downloadId, const QByteArray &fileHash, const QList<mx::ed2k::kad::KadSource> &sources);
    void statusChanged();
    void logMessage(const QString &text);
    void incomingSearches(int count);   // [engine-3] keyword search requests we answered (SPM figure)

private:
    struct SenderKey { quint32 key = 0, ip = 0; };
    struct Ctx { quint32 ip; quint16 port; bool obfuscated; bool validKey; SenderKey sk; };

    // sending
    bool sendKad(quint32 hostIp, quint16 port, quint8 opcode, const QByteArray &payload, const KadId *targetId,
                 quint32 receiverKey, bool request);
    bool sendToPeer(const KadPeer &p, quint8 opcode, const QByteArray &payload, bool request);
    void sendMyDetails(quint8 opcode, quint32 ip, quint16 port, quint8 version, const SenderKey &sk, const KadId *targetId,
                       bool requestAck);
    void firewalledCheck(quint32 ip, quint16 port, const SenderKey &sk, quint8 version);
    quint32 keyValue(const SenderKey &sk) const;
    quint32 ourEd2kIp() const;

    // tracking
    void trackOut(quint32 ip, quint8 opcode);
    bool onOutTrackList(quint32 ip, quint8 opcode, bool remove = true);
    bool inFloodAllowed(quint32 ip, quint8 opcode);
    bool takeToken();

    // handlers
    void process(const Ctx &c, quint8 opcode, const QByteArray &p);
    bool addContactFromHello(const Ctx &c, const HelloMsg &h, quint16 udpPort, bool fromHelloReq, bool *requestsAck);
    void onBootstrapReq(const Ctx &c);
    void onBootstrapRes(const Ctx &c, const QByteArray &p);
    void onHelloReq(const Ctx &c, const QByteArray &p);
    void onHelloRes(const Ctx &c, const QByteArray &p);
    void onHelloResAck(const Ctx &c, const QByteArray &p);
    void onReq(const Ctx &c, const QByteArray &p);
    void onRes(const Ctx &c, const QByteArray &p);
    void onSearchKeyReq(const Ctx &c, const QByteArray &p);
    void onSearchSourceReq(const Ctx &c, const QByteArray &p);
    void onSearchRes(const Ctx &c, const QByteArray &p);
    void onPublishKeyReq(const Ctx &c, const QByteArray &p);
    void onPublishSourceReq(const Ctx &c, const QByteArray &p);
    void onPublishRes(const Ctx &c, const QByteArray &p);
    void onFirewalledReq(const Ctx &c, const QByteArray &p, bool v2);
    void onFirewalledRes(const Ctx &c, const QByteArray &p);
    void onFindBuddyReq(const Ctx &c, const QByteArray &p);
    void onFindBuddyRes(const Ctx &c, const QByteArray &p);
    void onCallbackReq(const Ctx &c, const QByteArray &p);
    void onPing(const Ctx &c);
    void onPong(const Ctx &c, const QByteArray &p);
    void onFirewallUdp(const Ctx &c, const QByteArray &p);
    void sendSearchResults(const Ctx &c, const KadId &target, const QList<SearchResEntry> &results);

    // lookups
    Lookup *startLookup(LookupType type, const KadId &target);
    void lookupFinished(Lookup *l);
    void processKeywordResult(Lookup &l, const KadPeer *from, const SearchResEntry &e);
    void processSourceResult(Lookup &l, const SearchResEntry &e);
    void publishTick(qint64 now);
    void sourceSearchTick(qint64 now);
    void firewallTick(qint64 now);
    void udpFwQueryNext();
    TagList keywordTagsFor(const mx::SharedFile &f, quint8 targetVersion) const;

    // persistence
    void loadPrefs();
    void savePrefs() const;
    void loadNodesFile();

    KadHost *m_host;
    KadConfig m_cfg;
    bool m_running = false;
    KadId m_id;
    quint32 m_udpKey = 0;
    std::unique_ptr<RoutingTable> m_routing;
    KadIndex m_index;
    KadCounters m_ctr;

    // connection state
    qint64 m_lastContact = 0;        // s
    qint64 m_startedAt = 0;
    bool m_publishAllowed = false;
    qint64 m_nextSelfLookup = 0, m_nextFirewallCheck = 0, m_nextFindBuddy = 0, m_nextExternPortLookup = 0;
    qint64 m_nextSave = 0, m_nextBootstrap = 0, m_nextSparseLookup = 0, m_nextPublishKey = 0, m_nextPublishSrc = 0, m_nextSourceSearch = 0;
    quint32 m_kadIp = 0;             // host order
    // TCP firewall check
    int m_recheckIp = 0;             // FIREWALLED_RES received this round
    int m_fwAcks = 0;                // TCP ACKs received this round
    bool m_lastFirewallState = true;
    bool m_fwHadVerdict = false;     // [diag-1] a check round finished with a verdict
    QHash<quint32, qint64> m_fwAsked;   // host ip -> ms (FIREWALLED(2)_REQ sent)
    // UDP firewall test
    bool m_udpFirewalled = false, m_udpLastState = false, m_udpVerified = false, m_udpTimedOut = false;
    int m_udpChecksRunning = 0, m_udpChecksFinished = 0;
    qint64 m_udpTestStart = 0;
    bool m_udpTestNeedsLookup = true;
    QList<KadPeer> m_udpCandidates;
    QHash<quint32, bool> m_udpTesters;   // host ip -> answered
    QSet<quint32> m_contactedIps;        // host IPs we ever sent UDP to (bounded)
    // external port
    QList<quint32> m_externAskIps;
    QList<quint16> m_externPorts;
    quint16 m_externPort = 0;
    // buddy
    quint32 m_buddyIp = 0;           // eD2k order
    quint16 m_buddyPort = 0;
    // bootstrap
    struct BootNode { quint32 ip; quint16 port; KadId id; quint8 version; };
    QList<BootNode> m_bootstrap;
    qint64 m_lastPeerBootstrap = 0;
    // packet tracking
    QHash<quint64, QList<qint64>> m_outTrack;   // (ip << 8 | opcode) -> send times (ms)
    qint64 m_lastOutCleanup = 0;
    struct InTrack { int count = 0; qint64 firstMs = 0; };
    QHash<quint64, InTrack> m_inTrack;    // (ip << 8 | opcode) -> counter
    QHash<quint32, qint64> m_banned;      // host ip -> until ms
    qint64 m_lastInCleanup = 0;
    double m_tokens = 0;
    qint64 m_tokenMs = 0;

    // lookups
    QHash<KadId, Lookup *> m_lookups;
    quint32 m_nextLookupId = 1;
    QHash<quint32, QSet<quint32>> m_searchAsked;   // lookup id -> host ips sent a search/publish request

    // keyword searches
    struct Query {
        mx::SearchQuery q;
        KadQueryPlan plan;
        QSet<quint32> lookups;            // lookup ids still running
        QSet<QByteArray> seen;            // ed2k hashes reported
        int hits = 0;
        bool done = false;
    };
    QHash<quint32, Query> m_queries;
    QList<QPair<quint32, KadLookupPlan>> m_pendingKeywordLookups;   // started when the target is free

    // source searches
    struct Download {
        QString id;
        QByteArray hash;
        qint64 size = 0;
        QString name;
        qint64 nextSearch = 0;            // s
        QSet<QByteArray> known;           // user hashes reported
        quint32 lookupId = 0;
    };
    QHash<QString, Download> m_downloads;

    // publishing
    struct PubKeyword { QString word; KadId target; QList<QByteArray> files; int rotate = 0; qint64 next = 0; };
    QHash<KadId, PubKeyword> m_keywords;
    QList<KadId> m_keywordOrder;
    int m_keywordCursor = 0;
    QHash<QByteArray, mx::SharedFile> m_shared;     // ed2k hash -> file
    QHash<QByteArray, qint64> m_nextSrcPublish;     // ed2k hash -> s
    QList<QByteArray> m_sharedOrder;
    int m_srcCursor = 0;
};

} // namespace mx::ed2k::kad

Q_DECLARE_METATYPE(mx::ed2k::kad::KadSource)
