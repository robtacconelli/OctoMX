// SPDX-License-Identifier: GPL-3.0-or-later
// RatsNode: a Qt-friendly wrapper around one librats node. Private to mxnet_overlay.
// No librats header leaks out of rats_node.cpp (pimpl).
//
// * start(cfg) builds a librats::Node with the requested subsystems and starts it (fast, ~ms).
//   If the configured listen port is taken it falls back to a free port (two nodes in one process).
// * Every librats callback (reactor / worker threads) is marshalled to the thread RatsNode lives in
//   (the Qt main thread) with a queued call; signals are therefore always emitted there.
// * stop() is asynchronous: the librats node is torn down on a helper thread (librats needs ~1 s to
//   persist peers.json / DHT tables and join its reactors). waitStopped() / waitAllStopped() join.
//   Nothing is delivered after stop() returns.
// * send*/publish/connectTo are thread-safe and non-blocking (librats queues them).
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include "mx/plugin.h"

#include <functional>
#include <memory>

namespace mx::ovl {

struct RatsNodeConfig {
   QString protocol = QStringLiteral("winmx-ng/1");   // Noise prologue + DHT rendezvous key (wire identifier kept for compatibility)
    quint16 listenPort = 0;            // 0 = ephemeral
    bool    portFallback = true;       // listenPort busy → ephemeral port
    QString bindAddress;               // empty = all interfaces
    QString dataDir;                   // identity.key, peers.json, dht_routing*.json (empty = nothing kept)
    int     maxPeers = 30;             // inbound admission cap (0 = unlimited)
    // discovery / connectivity subsystems
    bool    dht = true;                // mainline-DHT rendezvous on SHA1(protocol)
    quint16 dhtPort = 0;
    QStringList dhtBootstrap;          // "host:port"; empty = librats' default internet routers
    int     dhtSearchIntervalMs = 30000;
    bool    mdns = true;               // LAN discovery
    bool    pex = true;                // peer exchange
    int     peerTarget = 30;           // PEX stops dialing above this many peers (0 = no target)
    bool    reconnect = true;          // persisted peer book + re-dial with backoff
    bool    holePunch = true;          // hole punch + relayed circuits (client; serving: relayServe)
    bool    punch = true;              // [ovl-2] false: relayed circuits only (tests: a NAT punching cannot open)
    bool    relayServe = false;        // serve relayed circuits for others
    bool    portMapping = false;       // UPnP-IGD / NAT-PMP
    // messaging
    bool    json = true;               // librats MessageJson (typed JSON: rats-search wire)
    QStringList jsonTypes;             // MessageJson types to receive (more via addJsonType)
    QStringList channels;              // raw byte channels to receive (registered before start)
    bool    pubsub = false;            // GossipSub
    int     sendQueueLimit = 0;        // bytes; 0 = librats default (8 MiB)
};

struct RatsPeer {
    QString id;            // 64 hex chars (SHA-256 of the peer's Noise static key)
    QString address;       // "ip:port" of the live connection (may be empty)
    QStringList addresses; // dialable addresses the peer advertised
    bool    inbound = false;
    QString transport;     // "tcp" / "udp" / "relay"
    QString agent;
};

class RatsNode : public QObject {
    Q_OBJECT
public:
    explicit RatsNode(QObject *parent = nullptr);
    ~RatsNode() override;     // stops (async) if running; never blocks for long

    bool start(const RatsNodeConfig &cfg, QString *error = nullptr);
    void stop();              // async teardown; signals stop immediately
    bool waitStopped(int timeoutMs = 3000);   // join this node's teardown
    static bool waitAllStopped(int timeoutMs = 5000);  // join every node's teardown (app exit)
    bool isRunning() const;

    QString nodeId() const;                 // 64 hex
    QString protocol() const;
    quint16 listenPort() const;
    quint16 dhtPort() const;
    int     peerCount() const;
    QList<RatsPeer> peers() const;          // snapshot
    bool    isConnected(const QString &peerId) const;
    QStringList observedAddresses() const;  // our address as peers see it ("ip:port")
    QString externalAddress() const;        // STUN / DHT reflexive IP if known
    QString natType() const;                // librats NAT classification ("endpoint-independent", ...)
    int     dhtNodes() const;               // DHT routing-table size (0 when no DHT)
    QStringList knownPeerAddresses(int max = 64) const; // from the reconnection peer book

    void connectTo(const QString &host, quint16 port);
    bool connectTo(const QString &hostPort);         // "host:port" / "[v6]:port"
    void disconnectPeer(const QString &peerId);
    // [ovl-2] reach a peer we know by id but cannot dial (firewalled / NAT): ask librats to punch a
    // hole through a shared peer and to open a relayed circuit through a shared peer that serves
    // relays. Non-blocking; success shows up as peerConnected (transport "relay" for a circuit).
    // Returns whether at least one attempt started.
    bool reachPeer(const QString &peerId);
    QString peerTransport(const QString &peerId) const;   // "tcp" / "udp" / "relay", empty: not connected

    // typed JSON (librats MessageJson; the rats-search wire format)
    void addJsonType(const QString &type);
    void sendJson(const QString &peerId, const QString &type, const QJsonObject &obj);
    void broadcastJson(const QString &type, const QJsonObject &obj);

    // raw bytes on a named channel. Returns false when the peer's send queue is past its low-water
    // mark ("stop": wait for peerWritable) or the peer is not connected.
    bool sendBytes(const QString &peerId, const QString &channel, const QByteArray &bytes);
    bool sendBytesUrgent(const QString &peerId, const QString &channel, const QByteArray &bytes);   // [up-1] ahead of queued data
    bool broadcastBytes(const QString &channel, const QByteArray &bytes);
    bool isWritable(const QString &peerId) const;
    // [glue-3] account every message (JSON, channel bytes, pubsub) for the bandwidth graph as
    // protocol traffic of `net` (librats has no byte counters; DHT/handshakes are not included)
    // [bw-2] only the bytes of `transferChannel` (file data) count against the transfer limits; JSON,
    // pub-sub and other channels (search, chat, presence, control) are counted, never charged
    void setMeter(mx::RateLimiter *limiter, mx::Net net, const QString &transferChannel = QString())
    { m_meter = limiter; m_meterNet = net; m_meterTransfer = transferChannel; }
    void meter(bool up, qint64 bytes, const QString &channel = QString())
    {
        if (!m_meter || bytes <= 0) return;
        const mx::RateLimiter::Dir d = up ? mx::RateLimiter::Up : mx::RateLimiter::Down;
        if (!m_meterTransfer.isEmpty() && channel == m_meterTransfer) m_meter->accountTraffic(d, m_meterNet, bytes, 0);
        else m_meter->accountExempt(d, m_meterNet, bytes, 0);
    }

    // pub-sub (GossipSub). The validator runs on a librats thread (must be thread-safe, fast);
    // rejected messages are neither delivered nor relayed. Our own publications are not delivered.
    using Validator = std::function<bool(const QString &topic, const QByteArray &data)>;
    void subscribe(const QString &topic, Validator validator = {});
    void unsubscribe(const QString &topic);
    void publish(const QString &topic, const QByteArray &data);
    QStringList topicPeers(const QString &topic) const;   // known subscribers

    static void setLibratsLogLevel(int level);   // 0 debug, 1 info, 2 warn (default), 3 error

signals:
    void peerConnected(const mx::ovl::RatsPeer &peer);
    void peerDisconnected(const QString &peerId, const QString &reason);
    void peerRerouted(const QString &peerId);      // messages sent just before may be lost: resend
    void peerWritable(const QString &peerId);
    void dialFailed(const QString &address);
    void jsonReceived(const QString &peerId, const QString &type, const QJsonObject &obj);
    void bytesReceived(const QString &peerId, const QString &channel, const QByteArray &bytes);
    void topicMessage(const QString &topic, const QString &originId, const QByteArray &data);

private:
    struct Shared;
    std::shared_ptr<Shared> d;
    std::weak_ptr<Shared> m_lastStopping;
    mx::RateLimiter *m_meter = nullptr;
    mx::Net m_meterNet = mx::Net::Overlay;
    QString m_meterTransfer;   // [bw-2]
};

} // namespace mx::ovl

Q_DECLARE_METATYPE(mx::ovl::RatsPeer)
