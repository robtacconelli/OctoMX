// SPDX-License-Identifier: GPL-3.0-or-later
// RatsNode: see rats_node.h. The only translation unit (with nothing else) that includes librats.
#include "rats_node.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QJsonDocument>
#include <QtCore/QMetaObject>
#include <QtCore/QPointer>

// librats has a method called emit() (EventBus): hide Qt's keyword macros while including it.
#pragma push_macro("emit")
#pragma push_macro("slots")
#pragma push_macro("signals")
#undef emit
#undef slots
#undef signals
#include <librats/core/address.h>
#include <librats/dht/dht.h>
#include <librats/node/node.h>
#include <librats/peer/peer.h>
#include <librats/peer/peer_id.h>
#include <librats/subsystems/dht_discovery.h>
#include <librats/subsystems/hole_punch.h>
#include <librats/subsystems/mdns_discovery.h>
#include <librats/subsystems/message_json.h>
#include <librats/subsystems/peer_exchange.h>
#include <librats/subsystems/port_mapping_service.h>
#include <librats/subsystems/pubsub.h>
#include <librats/subsystems/reconnection.h>
#include <librats/subsystems/relay.h>
#include <librats/subsystems/relay_service.h>
#include <librats/subsystems/hole_punch_service.h>
#include <librats/util/json.h>
#include <librats/util/logger.h>
#pragma pop_macro("signals")
#pragma pop_macro("slots")
#pragma pop_macro("emit")

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

namespace mx::ovl {

namespace {

// ---- teardown bookkeeping (node.stop() runs on helper threads; app exit joins them)
struct Stoppers {
    std::mutex m;
    std::condition_variable cv;
    int running = 0;
};
Stoppers &stoppers()
{
    static Stoppers *s = new Stoppers;   // leaked on purpose: usable during static destruction
    return *s;
}
void joinStoppersAtExit()
{
    RatsNode::waitAllStopped(5000);
}

struct LogInit {
    LogInit()
    {
        auto &lg = librats::Logger::getInstance();
        const QByteArray env = qgetenv("MX_RATS_LOG");   // debug | info | warn | error
        if (env.isEmpty()) {
            lg.set_console_logging_enabled(false);
            lg.set_log_level(librats::LogLevel::ERROR);
        } else {
            lg.set_console_logging_enabled(true);
            lg.set_log_level(env == "debug" ? librats::LogLevel::DEBUG
                             : env == "info" ? librats::LogLevel::INFO
                             : env == "error" ? librats::LogLevel::ERROR : librats::LogLevel::WARN);
        }
    }
};
void ensureLogInit()
{
    static LogInit once;
    (void)once;
}

QString addrToQ(const librats::Address &a) { return QString::fromStdString(a.to_string()); }

bool splitHostPort(const QString &hp, QString *host, quint16 *port)
{
    QString s = hp.trimmed();
    int colon = s.lastIndexOf(QLatin1Char(':'));
    if (colon <= 0) return false;
    QString h = s.left(colon);
    bool ok = false;
    const int p = s.mid(colon + 1).toInt(&ok);
    if (!ok || p <= 0 || p > 65535) return false;
    if (h.startsWith(QLatin1Char('[')) && h.endsWith(QLatin1Char(']'))) h = h.mid(1, h.size() - 2);
    if (h.isEmpty()) return false;
    *host = h;
    *port = static_cast<quint16>(p);
    return true;
}

RatsPeer toPeer(const librats::PeerInfo &info)
{
    RatsPeer p;
    p.id = QString::fromStdString(info.id.to_hex());
    if (info.endpoint) p.address = addrToQ(*info.endpoint);
    for (const auto &a : info.addresses) p.addresses << addrToQ(a);
    if (p.address.isEmpty() && !p.addresses.isEmpty()) p.address = p.addresses.front();
    p.inbound = info.direction == librats::ConnRole::Inbound;
    p.transport = QString::fromLatin1(librats::to_string(info.transport)).toLower();
    p.agent = QString::fromStdString(info.agent_version);
    return p;
}

std::optional<librats::PeerId> toId(const QString &hex)
{
    if (hex.size() != 64) return std::nullopt;
    return librats::PeerId::from_hex(hex.toStdString());
}

} // namespace

// ---------------------------------------------------------------------------------------------
struct RatsNode::Shared : std::enable_shared_from_this<RatsNode::Shared> {
    // owner is written only on the main thread (start/stop), under ownerMu; read under ownerMu by
    // librats threads (post) and lock-free on the main thread (delivery).
    std::mutex ownerMu;
    RatsNode *owner = nullptr;

    RatsNodeConfig cfg;
    std::unique_ptr<librats::Node> node;
    librats::MessageJson *json = nullptr;
    librats::PubSub *ps = nullptr;
    librats::DhtDiscovery *dht = nullptr;
    librats::ReconnectionService *rc = nullptr;
    QString id;
    quint16 port = 0;
    std::atomic<bool> running{false};

    std::mutex jsonMu;
    std::set<std::string> jsonTypes;

    // teardown
    std::mutex doneMu;
    std::condition_variable doneCv;
    bool stopping = false;
    bool done = false;

    template <class F>
    void post(F &&f)
    {
        std::lock_guard<std::mutex> l(ownerMu);
        if (!owner) return;
        std::weak_ptr<Shared> w = weak_from_this();
        QMetaObject::invokeMethod(
            owner,
            [w, fn = std::forward<F>(f)]() mutable {
                auto s = w.lock();
                if (!s || !s->owner) return;
                fn(s->owner);
            },
            Qt::QueuedConnection);
    }

    void registerJsonType(const std::string &type)
    {
        if (!json) return;
        {
            std::lock_guard<std::mutex> l(jsonMu);
            if (!jsonTypes.insert(type).second) return;
        }
        std::weak_ptr<Shared> w = weak_from_this();
        json->on(type, [w, type](const librats::PeerId &from, const librats::Json &data) {
            auto s = w.lock();
            if (!s) return;
            if (!data.is_object()) return;
            const std::string text = data.dump();
            if (text.size() > 4 * 1024 * 1024) return;   // hostile input: bounded
            QJsonParseError pe{};
            const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(text), &pe);
            if (!doc.isObject()) return;
            const QString pid = QString::fromStdString(from.to_hex());
            const QString t = QString::fromStdString(type);
            const QJsonObject obj = doc.object();
            const qint64 n = qint64(text.size());
            s->post([pid, t, obj, n](RatsNode *o) { o->meter(false, n); Q_EMIT o->jsonReceived(pid, t, obj); });
        });
    }

    bool build(quint16 port, QString *error);
};

bool RatsNode::Shared::build(quint16 listenPort, QString *error)
{
    librats::NodeConfig nc;
    nc.listen_port = listenPort;
    nc.protocol = cfg.protocol.toStdString();
    nc.security = librats::NodeConfig::Security::Noise;
    nc.data_dir = cfg.dataDir.toStdString();
    nc.max_peers = static_cast<size_t>(qMax(0, cfg.maxPeers));
    nc.bind_address = cfg.bindAddress.toStdString();
    if (cfg.sendQueueLimit > 0) nc.send_queue_limit = static_cast<size_t>(cfg.sendQueueLimit);
    node = std::make_unique<librats::Node>(nc);
    librats::Node &n = *node;

    if (cfg.dht) {
        librats::DhtDiscovery::Config dc;
        dc.dht_port = cfg.dhtPort;
        dc.data_dir = cfg.dataDir.toStdString();
        dc.bind_address = cfg.bindAddress.toStdString();
        dc.search_interval = std::chrono::milliseconds(qMax(5000, cfg.dhtSearchIntervalMs));
        for (const QString &b : cfg.dhtBootstrap) {
            QString h;
            quint16 p = 0;
            if (splitHostPort(b, &h, &p)) dc.bootstrap_nodes.push_back({h.toStdString(), p});
        }
        dht = n.add_subsystem(std::make_unique<librats::DhtDiscovery>(dc));
    }
    if (cfg.mdns) n.add_subsystem(std::make_unique<librats::MdnsDiscovery>());
    if (cfg.json) json = n.add_subsystem(std::make_unique<librats::MessageJson>());
    if (cfg.pubsub) ps = n.add_subsystem(std::make_unique<librats::PubSub>());
    if (cfg.reconnect) {
        librats::ReconnectionService::Config rcc;
        if (!cfg.dataDir.isEmpty()) rcc.store_path = (cfg.dataDir + QStringLiteral("/peers.json")).toStdString();
        rc = n.add_subsystem(std::make_unique<librats::ReconnectionService>(rcc));
    }
    if (cfg.holePunch) {
        if (cfg.punch) {
            librats::HolePunch::Config hp;
            hp.enable_relay = true;
            n.add_subsystem(std::make_unique<librats::HolePunch>(hp));
        }
        librats::Relay::Config rl;
        rl.serve = cfg.relayServe;
        n.add_subsystem(std::make_unique<librats::Relay>(rl));
    }
    if (cfg.pex) {
        librats::PeerExchange::Config px;
        px.peer_target = static_cast<size_t>(qMax(0, cfg.peerTarget));
        n.add_subsystem(std::make_unique<librats::PeerExchange>(px));
    }
    if (cfg.portMapping) n.add_subsystem(std::make_unique<librats::PortMappingService>());

    std::weak_ptr<Shared> w = weak_from_this();
    n.on_peer_connected([w](const librats::Peer &p) {
        auto s = w.lock();
        if (!s) return;
        auto info = p.info();
        RatsPeer rp;
        if (info) rp = toPeer(*info);
        rp.id = QString::fromStdString(p.id().to_hex());
        s->post([rp](RatsNode *o) { Q_EMIT o->peerConnected(rp); });
    });
    n.on_peer_disconnected([w](const librats::PeerId &id, librats::CloseReason r) {
        auto s = w.lock();
        if (!s) return;
        const QString pid = QString::fromStdString(id.to_hex());
        const QString why = QString::fromLatin1(librats::to_string(r));
        s->post([pid, why](RatsNode *o) { Q_EMIT o->peerDisconnected(pid, why); });
    });
    n.on_peer_rerouted([w](const librats::Peer &p) {
        auto s = w.lock();
        if (!s) return;
        const QString pid = QString::fromStdString(p.id().to_hex());
        s->post([pid](RatsNode *o) { Q_EMIT o->peerRerouted(pid); });
    });
    n.on_peer_writable([w](const librats::Peer &p) {
        auto s = w.lock();
        if (!s) return;
        const QString pid = QString::fromStdString(p.id().to_hex());
        s->post([pid](RatsNode *o) { Q_EMIT o->peerWritable(pid); });
    });
    n.on_dial_failed([w](const librats::Address &a) {
        auto s = w.lock();
        if (!s) return;
        const QString as = addrToQ(a);
        s->post([as](RatsNode *o) { Q_EMIT o->dialFailed(as); });
    });
    for (const QString &ch : cfg.channels) {
        const QString chan = ch;
        n.on(ch.toStdString(), [w, chan](const librats::Peer &p, librats::ByteView v) {
            auto s = w.lock();
            if (!s) return;
            const QString pid = QString::fromStdString(p.id().to_hex());
            QByteArray bytes(reinterpret_cast<const char *>(v.data()), static_cast<qsizetype>(v.size()));
            s->post([pid, chan, bytes](RatsNode *o) { o->meter(false, bytes.size()); Q_EMIT o->bytesReceived(pid, chan, bytes); });
        });
    }
    if (json)
        for (const QString &t : cfg.jsonTypes) registerJsonType(t.toStdString());

    if (!n.start()) {
        if (error) *error = QStringLiteral("cannot listen on port %1").arg(listenPort);
        node->stop();
        node.reset();
        json = nullptr;
        ps = nullptr;
        dht = nullptr;
        rc = nullptr;
        std::lock_guard<std::mutex> l(jsonMu);
        jsonTypes.clear();
        return false;
    }
    id = QString::fromStdString(n.local_id().to_hex());
    port = n.listen_port();
    return true;
}

// ---------------------------------------------------------------------------------------------
RatsNode::RatsNode(QObject *parent) : QObject(parent)
{
    static const bool registered = [] {
        qRegisterMetaType<mx::ovl::RatsPeer>();
        qAddPostRoutine(joinStoppersAtExit);
        return true;
    }();
    (void)registered;
}

RatsNode::~RatsNode()
{
    stop();
}

bool RatsNode::start(const RatsNodeConfig &cfg, QString *error)
{
    ensureLogInit();
    if (d && d->running) stop();
    auto s = std::make_shared<Shared>();
    s->cfg = cfg;
    if (!cfg.dataDir.isEmpty()) QDir().mkpath(cfg.dataDir);
    {
        std::lock_guard<std::mutex> l(s->ownerMu);
        s->owner = this;
    }
    QString err;
    bool ok = s->build(cfg.listenPort, &err);
    if (!ok && cfg.portFallback && cfg.listenPort != 0) ok = s->build(0, &err);
    if (!ok) {
        std::lock_guard<std::mutex> l(s->ownerMu);
        s->owner = nullptr;
        if (error) *error = err;
        return false;
    }
    s->running = true;
    d = s;
    return true;
}

void RatsNode::stop()
{
    if (!d) return;
    std::shared_ptr<Shared> s = std::move(d);
    d.reset();
    {
        std::lock_guard<std::mutex> l(s->ownerMu);
        s->owner = nullptr;     // nothing is delivered from now on
    }
    s->running = false;
    {
        std::lock_guard<std::mutex> l(s->doneMu);
        if (s->stopping) return;
        s->stopping = true;
    }
    if (!s->node) {
        std::lock_guard<std::mutex> l(s->doneMu);
        s->done = true;
        return;
    }
    {
        std::lock_guard<std::mutex> l(stoppers().m);
        ++stoppers().running;
    }
    m_lastStopping = s;
    std::thread([s]() mutable {
        s->node->stop();
        s->node.reset();
        s->json = nullptr;
        s->ps = nullptr;
        s->dht = nullptr;
        s->rc = nullptr;
        {
            std::lock_guard<std::mutex> l(s->doneMu);
            s->done = true;
        }
        s->doneCv.notify_all();
        s.reset();
        std::lock_guard<std::mutex> l(stoppers().m);
        --stoppers().running;
        stoppers().cv.notify_all();
    }).detach();
}

bool RatsNode::waitStopped(int timeoutMs)
{
    std::shared_ptr<Shared> s = m_lastStopping.lock();
    if (!s) return true;
    std::unique_lock<std::mutex> l(s->doneMu);
    return s->doneCv.wait_for(l, std::chrono::milliseconds(timeoutMs), [&] { return s->done; });
}

bool RatsNode::waitAllStopped(int timeoutMs)
{
    std::unique_lock<std::mutex> l(stoppers().m);
    return stoppers().cv.wait_for(l, std::chrono::milliseconds(timeoutMs),
                                  [] { return stoppers().running == 0; });
}

bool RatsNode::isRunning() const { return d && d->running; }
QString RatsNode::nodeId() const { return d ? d->id : QString(); }
QString RatsNode::protocol() const { return d ? d->cfg.protocol : QString(); }
quint16 RatsNode::listenPort() const { return d ? d->port : 0; }
quint16 RatsNode::dhtPort() const { return d && d->dht ? d->dht->dht_port() : 0; }
int RatsNode::peerCount() const { return d && d->node ? static_cast<int>(d->node->peer_count()) : 0; }

QList<RatsPeer> RatsNode::peers() const
{
    QList<RatsPeer> out;
    if (!d || !d->node) return out;
    for (const auto &pi : d->node->peers()) out << toPeer(pi);
    return out;
}

bool RatsNode::isConnected(const QString &peerId) const
{
    if (!d || !d->node) return false;
    auto id = toId(peerId);
    return id && d->node->peer(*id).has_value();
}

QStringList RatsNode::observedAddresses() const
{
    QStringList out;
    if (!d || !d->node) return out;
    for (const auto &a : d->node->observed_addresses()) out << addrToQ(a);
    return out;
}

QString RatsNode::externalAddress() const
{
    if (!d || !d->dht) return QString();
    return QString::fromStdString(d->dht->external_address());
}

QString RatsNode::natType() const
{
    if (!d || !d->node) return QString();
    return QString::fromLatin1(librats::to_string(d->node->nat_status().udp_mapping()));
}

int RatsNode::dhtNodes() const
{
    if (!d || !d->dht || !d->dht->dht_client()) return 0;
    return static_cast<int>(d->dht->dht_client()->get_routing_table_size());
}

QStringList RatsNode::knownPeerAddresses(int max) const
{
    QStringList out;
    if (!d || !d->rc) return out;
    for (const auto &a : d->rc->known_peers(static_cast<size_t>(qMax(0, max)))) out << addrToQ(a);
    return out;
}

void RatsNode::connectTo(const QString &host, quint16 port)
{
    if (!d || !d->node || host.isEmpty() || port == 0) return;
    d->node->connect(host.toStdString(), port);
}

bool RatsNode::connectTo(const QString &hostPort)
{
    QString h;
    quint16 p = 0;
    if (!splitHostPort(hostPort, &h, &p)) return false;
    connectTo(h, p);
    return true;
}

void RatsNode::disconnectPeer(const QString &peerId)
{
    if (!d || !d->node) return;
    auto id = toId(peerId);
    if (!id) return;
    if (auto p = d->node->peer(*id)) p->disconnect();
}

bool RatsNode::reachPeer(const QString &peerId)
{
    if (!d || !d->node) return false;
    auto id = toId(peerId);
    if (!id || d->node->peer(*id).has_value()) return false;
    bool started = false;
    if (auto *punch = d->node->services().get<librats::HolePunchService>()) started = punch->punch(*id) || started;
    if (auto *relay = d->node->services().get<librats::RelayService>()) started = relay->connect_via_relay(*id) || started;
    return started;
}

QString RatsNode::peerTransport(const QString &peerId) const
{
    if (!d || !d->node) return QString();
    auto id = toId(peerId);
    if (!id) return QString();
    const auto info = d->node->peer(*id);
    if (!info) return QString();
    for (const auto &pi : d->node->peers())
        if (pi.id == *id) return QString::fromLatin1(librats::to_string(pi.transport)).toLower();
    return QString();
}

void RatsNode::addJsonType(const QString &type)
{
    if (d) d->registerJsonType(type.toStdString());
}

namespace {
librats::Json toRatsJson(const QJsonObject &obj)
{
    const QByteArray text = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    librats::Json j = librats::Json::parse(text.toStdString(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) return librats::Json::object();
    return j;
}
} // namespace

void RatsNode::sendJson(const QString &peerId, const QString &type, const QJsonObject &obj)
{
    if (!d || !d->json) return;
    auto id = toId(peerId);
    if (!id) return;
    const librats::Json j = toRatsJson(obj);
    if (m_meter) meter(true, qint64(j.dump().size()));   // [glue-3]
    d->json->send(*id, type.toStdString(), j);
}

void RatsNode::broadcastJson(const QString &type, const QJsonObject &obj)
{
    if (!d || !d->json) return;
    const librats::Json j = toRatsJson(obj);
    if (m_meter) meter(true, qint64(j.dump().size()) * peerCount());   // [glue-3]
    d->json->send(type.toStdString(), j);
}

bool RatsNode::sendBytes(const QString &peerId, const QString &channel, const QByteArray &bytes)
{
    if (!d || !d->node) return false;
    auto id = toId(peerId);
    if (!id) return false;
    const bool ok = d->node->send(*id, channel.toStdString(),
                                  librats::ByteView(reinterpret_cast<const uint8_t *>(bytes.constData()),
                                                    static_cast<size_t>(bytes.size())));
    meter(true, bytes.size());   // [glue-3] queued even when "stop" is returned
    return ok;
}

bool RatsNode::broadcastBytes(const QString &channel, const QByteArray &bytes)
{
    if (!d || !d->node) return false;
    meter(true, qint64(bytes.size()) * peerCount());   // [glue-3]
    return d->node->broadcast(channel.toStdString(),
                              librats::ByteView(reinterpret_cast<const uint8_t *>(bytes.constData()),
                                                static_cast<size_t>(bytes.size())));
}

bool RatsNode::isWritable(const QString &peerId) const
{
    if (!d || !d->node) return false;
    auto id = toId(peerId);
    return id && d->node->peer_writable(*id);
}

void RatsNode::subscribe(const QString &topic, Validator validator)
{
    if (!d || !d->ps) return;
    const std::string t = topic.toStdString();
    std::weak_ptr<Shared> w = d;
    const std::string self = d->id.toStdString();
    if (validator) {
        d->ps->set_validator(t, [validator](const librats::PeerId &, const std::string &tp, librats::ByteView data) {
            const QByteArray raw = QByteArray::fromRawData(reinterpret_cast<const char *>(data.data()),
                                                           static_cast<qsizetype>(data.size()));
            return validator(QString::fromStdString(tp), raw) ? librats::ValidationResult::Accept
                                                              : librats::ValidationResult::Reject;
        });
    }
    d->ps->subscribe(t, [w, self](const librats::PeerId &from, const std::string &tp, librats::ByteView data) {
        auto s = w.lock();
        if (!s) return;
        const std::string fromHex = from.to_hex();
        if (fromHex == self) return;   // a subscribed publisher hears itself: drop
        const QString origin = QString::fromStdString(fromHex);
        const QString qt = QString::fromStdString(tp);
        QByteArray bytes(reinterpret_cast<const char *>(data.data()), static_cast<qsizetype>(data.size()));
        s->post([qt, origin, bytes](RatsNode *o) { o->meter(false, bytes.size()); Q_EMIT o->topicMessage(qt, origin, bytes); });
    });
}

void RatsNode::unsubscribe(const QString &topic)
{
    if (d && d->ps) d->ps->unsubscribe(topic.toStdString());
}

void RatsNode::publish(const QString &topic, const QByteArray &data)
{
    if (!d || !d->ps) return;
    meter(true, qint64(data.size()) * qMin(6, peerCount()));   // [glue-3] GossipSub mesh degree
    d->ps->publish(topic.toStdString(),
                   librats::ByteView(reinterpret_cast<const uint8_t *>(data.constData()),
                                     static_cast<size_t>(data.size())));
}

QStringList RatsNode::topicPeers(const QString &topic) const
{
    QStringList out;
    if (!d || !d->ps) return out;
    for (const auto &p : d->ps->peers_for_topic(topic.toStdString())) out << QString::fromStdString(p.to_hex());
    return out;
}

void RatsNode::setLibratsLogLevel(int level)
{
    ensureLogInit();
    auto &lg = librats::Logger::getInstance();
    lg.set_console_logging_enabled(level < 3);
    lg.set_log_level(level <= 0 ? librats::LogLevel::DEBUG
                     : level == 1 ? librats::LogLevel::INFO
                     : level == 2 ? librats::LogLevel::WARN : librats::LogLevel::ERROR);
}

} // namespace mx::ovl
