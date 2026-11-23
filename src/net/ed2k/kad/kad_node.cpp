// SPDX-License-Identifier: GPL-3.0-or-later
// Parts of this file are adapted from eMule 0.50a (Copyright (C) the eMule team,
// https://www.emule-project.net; kademlia/net/PacketTracking.cpp, KademliaUDPListener.cpp), GPL-2.0-or-later;
// this file is distributed under GPL-3.0-or-later. See NOTICE.
#include "kad_node.h"

#include "../ed2k_opcodes.h"
#include "../ed2k_search.h"
#include "../ed2k_util.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRandomGenerator>
#include <QtCore/QSaveFile>
#include <algorithm>

namespace mx::ed2k::kad {

static constexpr qint64 KADEMLIADISCONNECTDELAY = 20 * 60;
static constexpr int KADEMLIAFIREWALLCHECKS = 4;
static constexpr qint64 KADEMLIAREPUBLISHTIMEK = 24 * 3600;
static constexpr qint64 KADEMLIAREPUBLISHTIMES = 5 * 3600;
static constexpr qint64 KADEMLIAREASKTIME = 3600;
static constexpr int KADEMLIATOTALSTOREKEY = 2, KADEMLIATOTALSTORESRC = 3, KADEMLIATOTALFILE = 5;
static constexpr int UDP_FIREWALLTEST_CLIENTSTOASK = 2;
static constexpr int EXTERNAL_PORT_ASKIPS = 3;

static bool isTrackedRequest(quint8 op)
{
    switch (op) {
    case KADEMLIA2_BOOTSTRAP_REQ: case KADEMLIA2_HELLO_REQ: case KADEMLIA2_HELLO_RES: case KADEMLIA2_REQ:
    case KADEMLIA2_SEARCH_NOTES_REQ: case KADEMLIA2_PUBLISH_KEY_REQ: case KADEMLIA2_PUBLISH_SOURCE_REQ:
    case KADEMLIA2_PUBLISH_NOTES_REQ: case KADEMLIA_FINDBUDDY_REQ: case KADEMLIA_CALLBACK_REQ: case KADEMLIA2_PING:
        return true;
    default:
        return false;
    }
}

KadNode::KadNode(KadHost *host, const KadConfig &cfg, QObject *parent)
    : QObject(parent), m_host(host), m_cfg(cfg), m_index(cfg.index)
{
    qRegisterMetaType<mx::ed2k::kad::KadSource>();
    qRegisterMetaType<QList<mx::ed2k::kad::KadSource>>();
    loadPrefs();
}

KadNode::~KadNode()
{
    if (m_running) stop();
    qDeleteAll(m_lookups);
}

// ---------------------------------------------------------------- lifecycle
void KadNode::start()
{
    if (m_running) return;
    m_running = true;
    const qint64 now = m_host->kadNowSecs();
    m_startedAt = now;
    RoutingOptions ro;
    ro.allowLan = m_cfg.allowLan;
    ro.allowSameIp = m_cfg.allowSameIp;
    m_routing = std::make_unique<RoutingTable>(m_id, ro);
    loadNodesFile();
    m_nextSelfLookup = now + (m_cfg.fastStart ? 5 : 180);
    m_nextFirewallCheck = now + 3600;
    m_nextFindBuddy = now + m_cfg.findBuddyDelaySecs;
    m_nextSave = now + 600;
    m_nextBootstrap = now;
    m_nextPublishKey = m_nextPublishSrc = now;
    m_recheckIp = 0;
    m_fwAcks = 0;
    m_lastFirewallState = true;
    m_udpFirewalled = m_udpLastState = m_udpTimedOut = false;
    m_udpVerified = !m_cfg.firewallChecks || m_cfg.forceUdpFirewalled;
    m_udpChecksRunning = m_udpChecksFinished = 0;
    m_udpTestNeedsLookup = m_cfg.firewallChecks;
    m_tokens = m_cfg.maxPacketsPerSec;
    m_tokenMs = m_host->kadNowMs();
    emit logMessage(QStringLiteral("started: KadID %1, %2 contacts from nodes.dat").arg(m_id.toHex()).arg(contacts()));
    // warm start: tell a few known contacts that we are back (refreshes them and us in their tables)
    if (m_cfg.warmStartHellos > 0) {
        QList<const Contact *> all = m_routing->all();
        std::stable_sort(all.begin(), all.end(), [](const Contact *a, const Contact *b) { return a->type < b->type; });
        int n = 0;
        for (const Contact *c : all) {
            if (n >= m_cfg.warmStartHellos) break;
            if (!c->ipVerified) continue;
            sendMyDetails(KADEMLIA2_HELLO_REQ, c->ip, c->udpPort, c->version, SenderKey{c->udpKey, c->udpKeyIp},
                          c->version >= KADEMLIA_VERSION6_49aBETA ? &c->id : nullptr, false);
            ++n;
        }
    }
    emit statusChanged();
}

void KadNode::stop()
{
    if (!m_running) return;
    saveNodesDat();
    savePrefs();
    m_running = false;
    for (auto it = m_queries.begin(); it != m_queries.end(); ++it)
        if (!it->done) { it->done = true; emit searchDone(it.key()); }
    m_queries.clear();
    m_pendingKeywordLookups.clear();
    qDeleteAll(m_lookups);
    m_lookups.clear();
    m_searchAsked.clear();
    m_routing.reset();
    m_bootstrap.clear();
    m_outTrack.clear();
    m_inTrack.clear();
    m_buddyIp = 0;
    m_buddyPort = 0;
    m_lastContact = 0;
    emit statusChanged();
}

bool KadNode::connected() const
{
    if (!m_running || !m_lastContact || contacts() == 0) return false;
    return m_host->kadNowSecs() - m_lastContact < KADEMLIADISCONNECTDELAY;
}

bool KadNode::firewalled() const
{
    if (m_cfg.forceTcpFirewalled) return true;
    if (!m_cfg.firewallChecks) return false;
    if (m_fwAcks < 2) {
        if (m_recheckIp < KADEMLIAFIREWALLCHECKS) return m_lastFirewallState;
        return true;
    }
    return false;
}

// [diag-1]
KadNode::FirewallInfo KadNode::firewallInfo() const
{
    FirewallInfo f;
    f.checksOn = m_cfg.firewallChecks;
    f.tcpAcks = m_fwAcks;
    f.tcpAnswers = m_recheckIp;
    f.tcpNeeded = KADEMLIAFIREWALLCHECKS;
    f.tcpChecked = m_fwAcks >= 2 || m_recheckIp >= KADEMLIAFIREWALLCHECKS || m_fwHadVerdict;
    f.udpVerified = m_udpVerified;
    f.udpTimedOut = m_udpTimedOut;
    f.udpFinished = m_udpChecksFinished;
    f.udpNeeded = UDP_FIREWALLTEST_CLIENTSTOASK;
    return f;
}

void KadNode::recheckFirewall()
{
    m_nextFirewallCheck = 0;
}

bool KadNode::udpFirewalled() const
{
    if (m_cfg.forceUdpFirewalled) return true;
    if (!m_cfg.firewallChecks) return false;
    const bool running = m_udpChecksFinished < UDP_FIREWALLTEST_CLIENTSTOASK;
    if (running && m_udpTimedOut) return true;
    if (running) return m_udpLastState;
    return m_udpFirewalled;
}

quint8 KadNode::connectOptions(bool withCallback) const
{
    quint8 o = m_host->kadCryptOptions() & 0x07;
    if (withCallback && directCallbackPossible()) o |= 0x08;
    return o;
}

quint32 KadNode::estimatedUsers() const
{
    if (!m_routing) return 0;
    if (m_cfg.allowLan && m_cfg.allowSameIp) return quint32(m_routing->count());   // LAN mode: count what we see
    return m_routing->estimateUsers();
}

quint32 KadNode::ourEd2kIp() const
{
    if (m_kadIp) return kadIpToEd2k(m_kadIp);
    return m_host->kadPublicIp();
}

quint32 KadNode::keyValue(const SenderKey &sk) const
{
    return sk.ip == ourEd2kIp() ? sk.key : 0;
}

QString KadNode::statusText() const
{
    if (!m_running) return QStringLiteral("Offline");
    if (!connected()) {
        if (contacts() > 0) return QStringLiteral("Bootstrapping... (%1)").arg(mx::countNoun(contacts(), QStringLiteral("contact")));
        return QStringLiteral("Bootstrapping...");
    }
    QString fw;
    if (!firewalled()) fw = QStringLiteral("open");
    else if (hasBuddy()) fw = QStringLiteral("firewalled, buddy");
    else if (directCallbackPossible()) fw = QStringLiteral("firewalled, UDP open");
    else fw = QStringLiteral("firewalled");
    return QStringLiteral("Connected: %1, %2").arg(mx::countNoun(contacts(), QStringLiteral("contact"))).arg(fw);
}

// ---------------------------------------------------------------- persistence
void KadNode::loadPrefs()
{
    bool haveId = false;
    if (!m_cfg.dataDir.isEmpty()) {
        QFile f(QDir(m_cfg.dataDir).filePath(QStringLiteral("preferencesKad.dat")));
        if (f.open(QIODevice::ReadOnly)) {
            const QByteArray d = f.read(64);
            Reader r(d);
            const quint32 ip = r.u32();
            r.u16();
            const QByteArray id = r.bytes(16);
            if (r.ok()) {
                m_id = KadId::fromWire(id);
                haveId = !m_id.isZero();
                m_kadIp = 0;
                Q_UNUSED(ip)
            }
        }
        QFile j(QDir(m_cfg.dataDir).filePath(QStringLiteral("kad.json")));
        if (j.open(QIODevice::ReadOnly)) {
            const QJsonObject o = QJsonDocument::fromJson(j.read(65536)).object();
            m_udpKey = quint32(o.value(QStringLiteral("udpKey")).toDouble());
        }
    }
    if (!haveId) m_id = KadId::random();
    while (m_udpKey == 0) m_udpKey = QRandomGenerator::system()->generate();
    savePrefs();
}

void KadNode::savePrefs() const
{
    if (m_cfg.dataDir.isEmpty()) return;
    QDir().mkpath(m_cfg.dataDir);
    {
        QSaveFile f(QDir(m_cfg.dataDir).filePath(QStringLiteral("preferencesKad.dat")));
        if (f.open(QIODevice::WriteOnly)) {
            Writer w(23);
            w.u32(m_kadIp).u16(0).bytes(m_id.toWire()).u8(0);
            f.write(w.data());
            f.commit();
        }
    }
    QSaveFile j(QDir(m_cfg.dataDir).filePath(QStringLiteral("kad.json")));
    if (j.open(QIODevice::WriteOnly)) {
        QJsonObject o;
        o.insert(QStringLiteral("udpKey"), double(m_udpKey));
        o.insert(QStringLiteral("kadId"), m_id.toHex());
        j.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
        j.commit();
    }
}

void KadNode::loadNodesFile()
{
    if (m_cfg.dataDir.isEmpty()) return;
    QFile f(QDir(m_cfg.dataDir).filePath(QStringLiteral("nodes.dat")));
    if (!f.open(QIODevice::ReadOnly) || f.size() > 4 * 1024 * 1024) return;
    loadNodesDat(f.readAll(), QStringLiteral("nodes.dat"));
}

int KadNode::loadNodesDat(const QByteArray &data, const QString &origin)
{
    NodesDat nd;
    if (!parseNodesDat(data, &nd)) {
        emit logMessage(QStringLiteral("%1 is not a valid nodes.dat (%2 bytes)").arg(origin).arg(data.size()));
        return 0;
    }
    const qint64 now = m_host->kadNowSecs();
    int n = 0;
    if (nd.bootstrapOnly || !m_routing) {
        // bootstrap edition: the 50 contacts closest to us become the bootstrap list
        std::multimap<KadId, BootNode> best;
        for (const NodesDatEntry &e : nd.entries) {
            if (e.peer.version <= 1 || !isGoodKadIpPort(e.peer.ip, e.peer.udpPort, m_cfg.allowLan)) continue;
            best.emplace(e.peer.id ^ m_id, BootNode{e.peer.ip, e.peer.udpPort, e.peer.id, e.peer.version});
            if (best.size() > 50) best.erase(std::prev(best.end()));
        }
        for (const auto &kv : best) { m_bootstrap.append(kv.second); ++n; }
        emit logMessage(QStringLiteral("%1 (bootstrap edition): %2 bootstrap contacts").arg(origin).arg(n));
        return n;
    }
    bool anyVerified = false;
    for (const NodesDatEntry &e : nd.entries) {
        if (nd.version == 0 && e.type >= 4) continue;
        KadPeer p = e.peer;
        if (nd.version == 0) p.version = 2;   // v0 files carry no version: assume Kad2
        bool verified = e.verified;
        if (m_routing->add(p, e.udpKey, e.udpKeyIp, verified, false, false, now, ourEd2kIp())) ++n;
        if (e.verified) anyVerified = true;
    }
    if (!anyVerified) m_routing->setAllVerified();   // old files: trust them once to bootstrap faster
    emit logMessage(QStringLiteral("%1 (v%2): %3 of %4 contacts added").arg(origin).arg(nd.version).arg(n).arg(nd.entries.size()));
    emit statusChanged();
    return n;
}

QByteArray KadNode::nodesDatSnapshot() const
{
    QList<const Contact *> list = m_routing ? m_routing->all() : QList<const Contact *>();
    std::stable_sort(list.begin(), list.end(), [](const Contact *a, const Contact *b) {
        if (a->ipVerified != b->ipVerified) return a->ipVerified;
        return a->type < b->type;
    });
    QList<NodesDatEntry> entries;
    for (const Contact *c : list) {
        if (c->type >= 4) continue;
        NodesDatEntry e;
        e.peer = c->peer();
        e.udpKey = c->udpKey;
        e.udpKeyIp = c->udpKeyIp;
        e.verified = c->ipVerified;
        entries.append(e);
        if (entries.size() >= 1000) break;
    }
    return writeNodesDat(entries);
}

bool KadNode::saveNodesDat() const
{
    if (m_cfg.dataDir.isEmpty() || !m_routing || m_routing->count() == 0) return false;
    QDir().mkpath(m_cfg.dataDir);
    QSaveFile f(QDir(m_cfg.dataDir).filePath(QStringLiteral("nodes.dat")));
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(nodesDatSnapshot());
    return f.commit();
}

// ---------------------------------------------------------------- bootstrap
void KadNode::addBootstrapNode(quint32 hostIp, quint16 port, bool sendNow)
{
    if (!isGoodKadIpPort(hostIp, port, m_cfg.allowLan)) return;
    for (const BootNode &b : std::as_const(m_bootstrap)) if (b.ip == hostIp && b.port == port) return;
    if (m_bootstrap.size() < 200) m_bootstrap.prepend(BootNode{hostIp, port, KadId(), 0});
    if (sendNow && m_running) {
        m_bootstrap.removeFirst();
        sendKad(hostIp, port, KADEMLIA2_BOOTSTRAP_REQ, QByteArray(), nullptr, 0, true);
    }
}

void KadNode::bootstrapFromPeer(quint32 ed2kIp, quint16 kadPort, quint8 kadVersion)
{
    if (!m_running || connected() || kadVersion <= 1 || !kadPort) return;
    const qint64 now = m_host->kadNowSecs();
    if (now - m_lastPeerBootstrap < 10) return;
    m_lastPeerBootstrap = now;
    const quint32 ip = kadIpFromEd2k(ed2kIp);
    if (!isGoodKadIpPort(ip, kadPort, m_cfg.allowLan)) return;
    sendKad(ip, kadPort, KADEMLIA2_BOOTSTRAP_REQ, QByteArray(), nullptr, 0, true);
}

// ---------------------------------------------------------------- sending
bool KadNode::takeToken()
{
    const qint64 ms = m_host->kadNowMs();
    const double rate = qMax(1, m_cfg.maxPacketsPerSec);
    m_tokens = qMin(rate * 2, m_tokens + double(ms - m_tokenMs) * rate / 1000.0);
    m_tokenMs = ms;
    if (m_tokens < 1.0) return false;
    m_tokens -= 1.0;
    return true;
}

bool KadNode::sendKad(quint32 hostIp, quint16 port, quint8 opcode, const QByteArray &payload, const KadId *targetId,
                      quint32 receiverKey, bool request)
{
    if (!m_running || !port || !hostIp) return false;
    if (!takeToken()) { ++m_ctr.droppedRate; return false; }
    Q_UNUSED(request)
    const quint32 ed2kIp = kadIpToEd2k(hostIp);
    QByteArray wire = kadFrame(opcode, payload);
    const bool useId = targetId && !targetId->isZero();
    if (useId || receiverKey) {
        const QByteArray obf = obfuscateKad(wire, useId ? targetId : nullptr, receiverKey, verifyKeyFor(ed2kIp));
        if (!obf.isEmpty()) { wire = obf; ++m_ctr.obfuscatedOut; }
    }
    if (isTrackedRequest(opcode)) trackOut(hostIp, opcode);
    if (m_contactedIps.size() < 100000) m_contactedIps.insert(hostIp);
    ++m_ctr.packetsOut;
    m_ctr.bytesOut += quint64(wire.size());
    m_host->kadSendDatagram(ed2kIp, port, wire);
    return true;
}

bool KadNode::sendToPeer(const KadPeer &p, quint8 opcode, const QByteArray &payload, bool request)
{
    quint32 rk = 0;
    if (const Contact *c = m_routing ? m_routing->find(p.id) : nullptr) rk = c->keyFor(ourEd2kIp());
    if (p.version >= KADEMLIA_VERSION6_49aBETA) return sendKad(p.ip, p.udpPort, opcode, payload, &p.id, rk, request);
    return sendKad(p.ip, p.udpPort, opcode, payload, nullptr, 0, request);
}

void KadNode::sendMyDetails(quint8 opcode, quint32 ip, quint16 port, quint8 version, const SenderKey &sk, const KadId *targetId,
                            bool requestAck)
{
    if (version <= 1) return;
    HelloMsg h;
    h.id = m_id;
    h.tcpPort = m_host->kadTcpPort();
    h.version = KADEMLIA_VERSION;
    const bool tcpFw = firewalled(), udpFw = udpFirewalled();
    if (version >= KADEMLIA_VERSION8_49b && (requestAck || tcpFw || udpFw)) {
        h.hasMisc = true;
        h.misc = quint8((requestAck ? 4 : 0) | (tcpFw ? 2 : 0) | (udpFw ? 1 : 0));
    }
    const QByteArray payload = encodeHello(h);
    if (version >= KADEMLIA_VERSION6_49aBETA)
        sendKad(ip, port, opcode, payload, (targetId && !targetId->isZero()) ? targetId : nullptr, keyValue(sk), opcode == KADEMLIA2_HELLO_REQ);
    else
        sendKad(ip, port, opcode, payload, nullptr, 0, opcode == KADEMLIA2_HELLO_REQ);
}

void KadNode::firewalledCheck(quint32 ip, quint16 port, const SenderKey &sk, quint8 version)
{
    if (!m_cfg.firewallChecks || m_cfg.forceTcpFirewalled || m_recheckIp >= KADEMLIAFIREWALLCHECKS) return;
    if (m_fwAsked.contains(ip) || m_fwAsked.size() >= 3 * KADEMLIAFIREWALLCHECKS) return;
    m_fwAsked.insert(ip, m_host->kadNowMs());
    if (version > KADEMLIA_VERSION6_49aBETA) {
        Firewalled2Req r;
        r.tcpPort = m_host->kadTcpPort();
        r.userHash = KadId::fromHash(m_host->kadUserHash());
        r.connectOptions = connectOptions(false);
        sendKad(ip, port, KADEMLIA_FIREWALLED2_REQ, encodeFirewalled2Req(r), nullptr, keyValue(sk), true);
    } else {
        Writer w; w.u16(m_host->kadTcpPort());
        sendKad(ip, port, KADEMLIA_FIREWALLED_REQ, w.data(), nullptr, keyValue(sk), true);
    }
}

// ---------------------------------------------------------------- tracking
void KadNode::trackOut(quint32 ip, quint8 opcode)
{
    QList<qint64> &l = m_outTrack[(quint64(ip) << 8) | opcode];
    l.append(m_host->kadNowMs());
    if (l.size() > 64) l.removeFirst();
}

bool KadNode::onOutTrackList(quint32 ip, quint8 opcode, bool remove)
{
    auto it = m_outTrack.find((quint64(ip) << 8) | opcode);
    if (it == m_outTrack.end()) return false;
    const qint64 now = m_host->kadNowMs();
    while (!it->isEmpty() && now - it->first() > 180000) it->removeFirst();
    if (it->isEmpty()) { m_outTrack.erase(it); return false; }
    if (remove) {
        it->removeFirst();
        if (it->isEmpty()) m_outTrack.erase(it);
    }
    return true;
}

bool KadNode::inFloodAllowed(quint32 ip, quint8 opcode)
{
    int perMinute = 0;
    switch (opcode) {
    case KADEMLIA2_BOOTSTRAP_REQ: perMinute = 2; break;
    case KADEMLIA2_HELLO_REQ: perMinute = 3; break;
    case KADEMLIA2_REQ: perMinute = 10; break;
    case KADEMLIA2_SEARCH_NOTES_REQ: case KADEMLIA2_SEARCH_KEY_REQ: case KADEMLIA2_SEARCH_SOURCE_REQ:
    case KADEMLIA2_PUBLISH_KEY_REQ: perMinute = 3; break;
    case KADEMLIA2_PUBLISH_SOURCE_REQ: case KADEMLIA2_PUBLISH_NOTES_REQ: perMinute = 2; break;
    case KADEMLIA_FIREWALLED2_REQ: opcode = KADEMLIA_FIREWALLED_REQ; perMinute = 2; break;
    case KADEMLIA_FIREWALLED_REQ: perMinute = 2; break;
    case KADEMLIA_FINDBUDDY_REQ: perMinute = 2; break;
    case KADEMLIA_CALLBACK_REQ: perMinute = 1; break;
    case KADEMLIA2_PING: perMinute = 2; break;
    default: return true;   // responses are checked against the out track list
    }
    if (m_cfg.allowLan && isLanIp(ip)) return true;   // no flood detection in LAN mode (as eMule)
    const qint64 now = m_host->kadNowMs();
    const qint64 per = 60000 / perMinute;
    if (m_inTrack.size() > 50000) m_inTrack.clear();   // spoofed-address floods: bounded memory
    if (m_banned.size() > 20000) m_banned.clear();
    InTrack &t = m_inTrack[(quint64(ip) << 8) | opcode];
    if (t.count > 0 && now - t.firstMs > per) {
        const qint64 rm = (now - t.firstMs) / per;
        if (rm > t.count) { t.count = 0; t.firstMs = now; }
        else { t.count -= int(rm); t.firstMs += per * rm; }
    }
    if (t.count == 0) t.firstMs = now;
    ++t.count;
    if (t.count > perMinute * 5) {
        m_banned.insert(ip, now + 3600 * 1000);
        emit logMessage(QStringLiteral("banned %1 for an hour (flooding opcode 0x%2)").arg(kadIpToString(ip)).arg(opcode, 2, 16, QLatin1Char('0')));
        return false;
    }
    return t.count <= perMinute;
}

// ---------------------------------------------------------------- receiving
Deobfuscated KadNode::deobfuscate(quint32 ed2kIp, const QByteArray &raw) const
{
    return kad::deobfuscate(raw, m_running ? &m_id : nullptr, m_host->kadUserHash(), ed2kIp, verifyKeyFor(ed2kIp));
}

void KadNode::handleDatagram(quint32 ed2kIp, quint16 port, const QByteArray &plain, bool obfuscated, bool validReceiverKey,
                             quint32 senderVerifyKey)
{
    if (!m_running) return;
    const quint32 ip = kadIpFromEd2k(ed2kIp);
    const qint64 nowMs = m_host->kadNowMs();
    auto ban = m_banned.constFind(ip);
    if (ban != m_banned.cend() && ban.value() > nowMs) return;
    if (port == 53 && senderVerifyKey == 0) return;   // no unencrypted packets from the DNS port
    quint8 op = 0;
    QByteArray payload;
    if (!kadUnframe(plain, &op, &payload)) { ++m_ctr.droppedMalformed; return; }
    ++m_ctr.packetsIn;
    m_ctr.bytesIn += quint64(plain.size());
    if (obfuscated) ++m_ctr.obfuscatedIn; else ++m_ctr.plainIn;
    const bool hadContact = connected();
    m_lastContact = m_host->kadNowSecs();
    if (!inFloodAllowed(ip, op)) { ++m_ctr.droppedFlood; return; }
    Ctx c{ip, port, obfuscated, validReceiverKey, SenderKey{senderVerifyKey, ourEd2kIp()}};
    process(c, op, payload);
    if (!hadContact && connected()) {
        emit logMessage(QStringLiteral("connected (%1 contacts)").arg(contacts()));
        emit statusChanged();
    }
}

void KadNode::process(const Ctx &c, quint8 op, const QByteArray &p)
{
    switch (op) {
    case KADEMLIA2_BOOTSTRAP_REQ: onBootstrapReq(c); break;
    case KADEMLIA2_BOOTSTRAP_RES: onBootstrapRes(c, p); break;
    case KADEMLIA2_HELLO_REQ: onHelloReq(c, p); break;
    case KADEMLIA2_HELLO_RES: onHelloRes(c, p); break;
    case KADEMLIA2_HELLO_RES_ACK: onHelloResAck(c, p); break;
    case KADEMLIA2_REQ: onReq(c, p); break;
    case KADEMLIA2_RES: onRes(c, p); break;
    case KADEMLIA2_SEARCH_KEY_REQ: onSearchKeyReq(c, p); break;
    case KADEMLIA2_SEARCH_SOURCE_REQ: onSearchSourceReq(c, p); break;
    case KADEMLIA2_SEARCH_NOTES_REQ: ++m_ctr.answeredOther; break;   // we store no notes: no results
    case KADEMLIA2_SEARCH_RES: onSearchRes(c, p); break;
    case KADEMLIA2_PUBLISH_KEY_REQ: onPublishKeyReq(c, p); break;
    case KADEMLIA2_PUBLISH_SOURCE_REQ: onPublishSourceReq(c, p); break;
    case KADEMLIA2_PUBLISH_NOTES_REQ: ++m_ctr.answeredOther; break;  // notes are not stored (eMule: answer only if stored)
    case KADEMLIA2_PUBLISH_RES: onPublishRes(c, p); break;
    case KADEMLIA2_PUBLISH_RES_ACK: break;
    case KADEMLIA_FIREWALLED_REQ: onFirewalledReq(c, p, false); break;
    case KADEMLIA_FIREWALLED2_REQ: onFirewalledReq(c, p, true); break;
    case KADEMLIA_FIREWALLED_RES: onFirewalledRes(c, p); break;
    case KADEMLIA_FIREWALLED_ACK_RES:
        if (p.isEmpty() && m_fwAsked.contains(c.ip)) ++m_fwAcks;
        break;
    case KADEMLIA_FINDBUDDY_REQ: onFindBuddyReq(c, p); break;
    case KADEMLIA_FINDBUDDY_RES: onFindBuddyRes(c, p); break;
    case KADEMLIA_CALLBACK_REQ: onCallbackReq(c, p); break;
    case KADEMLIA2_PING: onPing(c); break;
    case KADEMLIA2_PONG: onPong(c, p); break;
    case KADEMLIA2_FIREWALLUDP: onFirewallUdp(c, p); break;
    default: ++m_ctr.droppedMalformed; break;   // Kad1 and unknown opcodes
    }
}

bool KadNode::addContactFromHello(const Ctx &c, const HelloMsg &h, quint16 udpPort, bool fromHelloReq, bool *requestsAck)
{
    Q_UNUSED(fromHelloReq)
    if (requestsAck) *requestsAck = h.hasMisc && (h.misc & 0x04) && h.version >= KADEMLIA_VERSION8_49b;
    if (h.hasMisc && (h.misc & 0x01)) return false;   // UDP firewalled nodes are not added
    KadPeer p;
    p.id = h.id;
    p.ip = c.ip;
    p.udpPort = udpPort;
    p.tcpPort = h.tcpPort;
    p.version = h.version;
    bool verified = c.validKey;
    return m_routing->add(p, c.sk.key, c.sk.ip, verified, true, true, m_host->kadNowSecs(), ourEd2kIp());
}

void KadNode::onBootstrapReq(const Ctx &c)
{
    BootstrapRes r;
    r.id = m_id;
    r.tcpPort = m_host->kadTcpPort();
    r.version = KADEMLIA_VERSION;
    r.contacts = m_routing->bootstrapContacts(20);
    sendKad(c.ip, c.port, KADEMLIA2_BOOTSTRAP_RES, encodeBootstrapRes(r), nullptr, keyValue(c.sk), false);
    ++m_ctr.answeredBootstrap;
}

void KadNode::onBootstrapRes(const Ctx &c, const QByteArray &p)
{
    if (!onOutTrackList(c.ip, KADEMLIA2_BOOTSTRAP_REQ)) { ++m_ctr.droppedUnsolicited; return; }
    BootstrapRes r;
    if (!decodeBootstrapRes(p, &r)) { ++m_ctr.droppedMalformed; return; }
    const qint64 now = m_host->kadNowSecs();
    const bool assumeVerified = m_routing->count() == 0;
    if (m_bootstrap.isEmpty()) {
        bool v = c.validKey;
        KadPeer self;
        self.id = r.id; self.ip = c.ip; self.udpPort = c.port; self.tcpPort = r.tcpPort; self.version = r.version;
        m_routing->add(self, c.sk.key, c.sk.ip, v, true, false, now, ourEd2kIp());
    }
    int added = 0;
    for (const KadPeer &k : r.contacts) {
        bool v = assumeVerified;
        if (m_routing->add(k, 0, 0, v, false, false, now, ourEd2kIp())) ++added;
    }
    emit logMessage(QStringLiteral("bootstrap answer from %1:%2, %3 contacts (%4 new)").arg(kadIpToString(c.ip)).arg(c.port)
                        .arg(r.contacts.size()).arg(added));
    if (added) m_nextSelfLookup = qMin(m_nextSelfLookup, now + 1);
    // announce ourselves to the node that answered (eMule waits for the zone timers; in a sparse
    // network that takes minutes)
    sendMyDetails(KADEMLIA2_HELLO_REQ, c.ip, c.port, r.version, c.sk, r.version >= KADEMLIA_VERSION6_49aBETA ? &r.id : nullptr, false);
    emit statusChanged();
}

void KadNode::onHelloReq(const Ctx &c, const QByteArray &p)
{
    HelloMsg h;
    if (!decodeHello(p, &h) || h.version < 2) { ++m_ctr.droppedMalformed; return; }
    const quint16 udpPort = h.udpPortTag ? h.udpPortTag : c.port;
    const bool added = addContactFromHello(c, h, udpPort, true, nullptr);
    sendMyDetails(KADEMLIA2_HELLO_RES, c.ip, udpPort, h.version, c.sk, &h.id, added && !c.validKey);
    ++m_ctr.answeredHello;
    if (m_cfg.firewallChecks && m_externAskIps.size() < EXTERNAL_PORT_ASKIPS && h.version > KADEMLIA_VERSION5_48a)
        sendKad(c.ip, udpPort, KADEMLIA2_PING, QByteArray(), nullptr, keyValue(c.sk), true);
    if (m_recheckIp < KADEMLIAFIREWALLCHECKS) firewalledCheck(c.ip, udpPort, c.sk, h.version);
}

void KadNode::onHelloRes(const Ctx &c, const QByteArray &p)
{
    if (!onOutTrackList(c.ip, KADEMLIA2_HELLO_REQ)) { ++m_ctr.droppedUnsolicited; return; }
    HelloMsg h;
    if (!decodeHello(p, &h) || h.version < 2) { ++m_ctr.droppedMalformed; return; }
    const quint16 udpPort = h.udpPortTag ? h.udpPortTag : c.port;
    bool wantsAck = false;
    addContactFromHello(c, h, udpPort, false, &wantsAck);
    if (wantsAck && c.sk.key) {
        Writer w(17);
        w.bytes(m_id.toWire()).u8(0);
        sendKad(c.ip, udpPort, KADEMLIA2_HELLO_RES_ACK, w.data(), nullptr, keyValue(c.sk), false);
    }
    if (m_cfg.firewallChecks && m_externAskIps.size() < EXTERNAL_PORT_ASKIPS && h.version > KADEMLIA_VERSION5_48a)
        sendKad(c.ip, udpPort, KADEMLIA2_PING, QByteArray(), nullptr, keyValue(c.sk), true);
    if (m_recheckIp < KADEMLIAFIREWALLCHECKS) firewalledCheck(c.ip, udpPort, c.sk, h.version);
}

void KadNode::onHelloResAck(const Ctx &c, const QByteArray &p)
{
    if (p.size() < 17) { ++m_ctr.droppedMalformed; return; }
    if (!onOutTrackList(c.ip, KADEMLIA2_HELLO_RES)) { ++m_ctr.droppedUnsolicited; return; }
    if (!c.validKey) return;
    m_routing->verify(KadId::fromWire(p.left(16)), c.ip);
}

void KadNode::onReq(const Ctx &c, const QByteArray &p)
{
    KadReq q;
    if (!decodeKadReq(p, &q)) { ++m_ctr.droppedMalformed; return; }
    if (q.receiver != m_id) return;   // they think we are someone else (fresh KadID)
    KadRes r;
    r.target = q.target;
    const int want = qMin<int>(q.type, 32);
    r.contacts = m_routing->closest(q.target, 2, want);
    if (r.contacts.size() < want) {
        // a young table has few type ≤ 2 contacts yet: fill up with verified new ones (eMule: type ≤ 2 only)
        QSet<KadId> have;
        for (const KadPeer &p : std::as_const(r.contacts)) have.insert(p.id);
        for (const KadPeer &p : m_routing->closest(q.target, 3, want)) {
            if (r.contacts.size() >= want) break;
            if (!have.contains(p.id)) r.contacts.append(p);
        }
        std::sort(r.contacts.begin(), r.contacts.end(), [&](const KadPeer &a, const KadPeer &b) { return (a.id ^ q.target) < (b.id ^ q.target); });
    }
    sendKad(c.ip, c.port, KADEMLIA2_RES, encodeKadRes(r), nullptr, keyValue(c.sk), false);
    ++m_ctr.answeredReq;
}

void KadNode::onRes(const Ctx &c, const QByteArray &p)
{
    if (!onOutTrackList(c.ip, KADEMLIA2_REQ)) { ++m_ctr.droppedUnsolicited; return; }
    KadRes r;
    if (!decodeKadRes(p, &r)) { ++m_ctr.droppedMalformed; return; }
    Lookup *l = m_lookups.value(r.target);
    if (!l) return;   // lookup already gone
    const qint64 now = m_host->kadNowSecs();
    QList<KadPeer> results;
    for (const KadPeer &k : std::as_const(r.contacts)) {
        if (k.version <= 1) continue;
        if (!isGoodKadIpPort(k.ip, k.udpPort, m_cfg.allowLan)) continue;
        if (k.udpPort == 53 && k.version <= KADEMLIA_VERSION5_48a) continue;
        if (l->type() == LookupType::NodeFwCheckUdp) {
            if (m_udpCandidates.size() < 64) m_udpCandidates.prepend(k);
            continue;
        }
        bool v = false;
        const bool added = m_routing->add(k, 0, 0, v, false, false, now, ourEd2kIp());
        if (added || m_routing->isAcceptable(k)) results.append(k);
    }
    if (!l->processResponse(this, c.ip, c.port, results)) { ++m_ctr.droppedUnsolicited; return; }
    if (l->type() == LookupType::NodeFwCheckUdp) udpFwQueryNext();
}

void KadNode::sendSearchResults(const Ctx &c, const KadId &target, const QList<SearchResEntry> &results)
{
    SearchRes chunk;
    chunk.sender = m_id;
    chunk.target = target;
    int size = 2 + 34;
    auto flush = [&] {
        if (chunk.results.isEmpty()) return;
        sendKad(c.ip, c.port, KADEMLIA2_SEARCH_RES, encodeSearchRes(chunk), nullptr, keyValue(c.sk), false);
        chunk.results.clear();
        size = 2 + 34;
    };
    for (const SearchResEntry &e : results) {
        Writer w;
        writeKadTagList(w, e.tags);
        const int esz = 16 + int(w.size());
        if (size + esz > UDP_KAD_MAXFRAGMENT && !chunk.results.isEmpty()) flush();
        chunk.results.append(e);
        size += esz;
    }
    flush();
}

void KadNode::onSearchKeyReq(const Ctx &c, const QByteArray &p)
{
    SearchKeyReq q;
    if (!decodeSearchKeyReq(p, &q)) { ++m_ctr.droppedMalformed; return; }
    ++m_ctr.answeredSearch;
    emit incomingSearches(1);
    if (!m_cfg.storeIndex) return;
    if (!q.tree.isEmpty()) {
        bool valid = false;
        KadIndex::treeMatches(q.tree, QString(), {}, &valid);
        if (!valid) { ++m_ctr.droppedMalformed; return; }
    }
    const QList<SearchResEntry> res = m_index.keywordResults(q.target, q.tree, q.start, 300, m_host->kadNowSecs());
    if (!res.isEmpty()) sendSearchResults(c, q.target, res);
}

void KadNode::onSearchSourceReq(const Ctx &c, const QByteArray &p)
{
    SearchSourceReq q;
    if (!decodeSearchSourceReq(p, &q)) { ++m_ctr.droppedMalformed; return; }
    ++m_ctr.answeredSearch;
    if (!m_cfg.storeIndex) return;
    const QList<SearchResEntry> res = m_index.sourceResults(q.target, q.size, q.start, 300, m_host->kadNowSecs());
    if (!res.isEmpty()) sendSearchResults(c, q.target, res);
}

void KadNode::onSearchRes(const Ctx &c, const QByteArray &p)
{
    SearchRes r;
    if (!decodeSearchRes(p, &r)) { ++m_ctr.droppedMalformed; return; }
    Lookup *l = m_lookups.value(r.target);
    if (!l || !(l->type() == LookupType::Keyword || l->type() == LookupType::File)
        || !m_searchAsked.value(l->id()).contains(c.ip)) {
        ++m_ctr.droppedUnsolicited;
        return;
    }
    const KadPeer *from = l->triedPeerByIp(c.ip, c.port);
    for (const SearchResEntry &e : std::as_const(r.results)) {
        if (l->type() == LookupType::Keyword) processKeywordResult(*l, from, e);
        else processSourceResult(*l, e);
    }
}

void KadNode::onPublishKeyReq(const Ctx &c, const QByteArray &p)
{
    if (udpFirewalled()) return;   // we must not index: give no (false) report
    PublishKeyReq q;
    if (!decodePublishKeyReq(p, &q)) { ++m_ctr.droppedMalformed; return; }
    const KadId dist = m_id ^ q.keyword;
    if (dist.chunk(0) > m_cfg.searchTolerance && !(m_cfg.allowLan && isLanIp(c.ip))) return;
    quint8 load = 100;
    const qint64 now = m_host->kadNowSecs();
    for (const PublishKeyEntry &e : std::as_const(q.entries)) {
        quint8 l = 100;
        if (m_cfg.storeIndex) m_index.addKeyword(q.keyword, e.file, c.ip, e.tags, now, &l);
        load = l;
    }
    PublishRes r;
    r.target = q.keyword;
    r.load = load;
    sendKad(c.ip, c.port, KADEMLIA2_PUBLISH_RES, encodePublishRes(r), nullptr, keyValue(c.sk), false);
    ++m_ctr.answeredPublish;
}

void KadNode::onPublishSourceReq(const Ctx &c, const QByteArray &p)
{
    if (udpFirewalled()) return;
    PublishSourceReq q;
    if (!decodePublishSourceReq(p, &q)) { ++m_ctr.droppedMalformed; return; }
    const KadId dist = m_id ^ q.file;
    if (dist.chunk(0) > m_cfg.searchTolerance && !(m_cfg.allowLan && isLanIp(c.ip))) return;
    quint8 load = 100;
    bool stored = !m_cfg.storeIndex;   // not storing: answer as eMule does under full load
    if (m_cfg.storeIndex) stored = m_index.addSource(q.file, q.source, c.ip, c.port, q.tags, m_host->kadNowSecs(), &load);
    if (!stored) return;
    PublishRes r;
    r.target = q.file;
    r.load = load;
    sendKad(c.ip, c.port, KADEMLIA2_PUBLISH_RES, encodePublishRes(r), nullptr, keyValue(c.sk), false);
    ++m_ctr.answeredPublish;
}

void KadNode::onPublishRes(const Ctx &c, const QByteArray &p)
{
    const bool key = onOutTrackList(c.ip, KADEMLIA2_PUBLISH_KEY_REQ);
    const bool src = !key && onOutTrackList(c.ip, KADEMLIA2_PUBLISH_SOURCE_REQ);
    if (!key && !src) { ++m_ctr.droppedUnsolicited; return; }
    PublishRes r;
    if (!decodePublishRes(p, &r)) { ++m_ctr.droppedMalformed; return; }
    if (Lookup *l = m_lookups.value(r.target)) {
        if (l->type() == LookupType::StoreKeyword || l->type() == LookupType::StoreFile) {
            if (l->answers() == 0) {
                if (l->type() == LookupType::StoreKeyword) ++m_ctr.keywordsPublished; else ++m_ctr.filesPublished;
            }
            l->addAnswers();
            l->load += r.load;
            ++l->loadResponses;
        }
    }
    if (key) ++m_ctr.keywordPublishes; else ++m_ctr.sourcePublishes;
    if (r.hasOptions && (r.options & 0x01) && c.sk.key)
        sendKad(c.ip, c.port, KADEMLIA2_PUBLISH_RES_ACK, QByteArray(), nullptr, keyValue(c.sk), false);
}

void KadNode::onFirewalledReq(const Ctx &c, const QByteArray &p, bool v2)
{
    Firewalled2Req q;
    if ((v2 && p.size() < 19) || (!v2 && p.size() != 2) || !decodeFirewalled2Req(p, &q)) { ++m_ctr.droppedMalformed; return; }
    const QByteArray uh = v2 ? q.userHash.toHash() : QByteArray();
    if (!m_host->kadTcpFirewallCheck(kadIpToEd2k(c.ip), q.tcpPort, uh, q.connectOptions)) return;
    Writer w(4);
    w.u32(c.ip);
    sendKad(c.ip, c.port, KADEMLIA_FIREWALLED_RES, w.data(), nullptr, keyValue(c.sk), false);
    ++m_ctr.answeredFirewall;
}

void KadNode::onFirewalledRes(const Ctx &c, const QByteArray &p)
{
    if (p.size() != 4) { ++m_ctr.droppedMalformed; return; }
    if (!m_fwAsked.contains(c.ip)) { ++m_ctr.droppedUnsolicited; return; }
    Reader r(p);
    const quint32 ip = r.u32();
    if (ip && ip != m_kadIp) {
        m_kadIp = ip;
        emit logMessage(QStringLiteral("our public IP is %1 (from %2)").arg(kadIpToString(ip), kadIpToString(c.ip)));
        emit statusChanged();
    }
    ++m_recheckIp;
}

void KadNode::onTcpFirewallAck(quint32 ed2kIp)
{
    const quint32 ip = kadIpFromEd2k(ed2kIp);
    if (!m_fwAsked.contains(ip)) return;
    m_fwAsked[ip] = -1;   // counted (an ACK per asked IP)
    const bool was = firewalled();
    ++m_fwAcks;
    if (was != firewalled()) {
        emit logMessage(QStringLiteral("TCP firewall check passed, we are open"));
        emit statusChanged();
    }
}

void KadNode::onFindBuddyReq(const Ctx &c, const QByteArray &p)
{
    FindBuddyReq q;
    if (!decodeFindBuddyReq(p, &q)) { ++m_ctr.droppedMalformed; return; }
    if (!m_cfg.buddies || firewalled() || udpFirewalled() || !m_udpVerified) return;
    if (m_host->kadHasServedBuddy()) return;
    if (!m_host->kadIncomingBuddy(kadIpToEd2k(c.ip), q.tcpPort, q.userHash.toHash())) return;
    FindBuddyRes r;
    r.check = q.buddyId;
    r.userHash = KadId::fromHash(m_host->kadUserHash());
    r.tcpPort = m_host->kadTcpPort();
    if (c.sk.key) { r.hasOptions = true; r.connectOptions = connectOptions(false); }
    sendKad(c.ip, c.port, KADEMLIA_FINDBUDDY_RES, encodeFindBuddyRes(r), nullptr, keyValue(c.sk), false);
    ++m_ctr.answeredBuddy;
}

void KadNode::onFindBuddyRes(const Ctx &c, const QByteArray &p)
{
    if (!onOutTrackList(c.ip, KADEMLIA_FINDBUDDY_REQ)) { ++m_ctr.droppedUnsolicited; return; }
    FindBuddyRes r;
    if (!decodeFindBuddyRes(p, &r)) { ++m_ctr.droppedMalformed; return; }
    if ((r.check ^ KadId::allOnes()) != m_id || hasBuddy()) return;
    ++m_ctr.buddyOffers;
    emit logMessage(QStringLiteral("buddy offer from %1:%2").arg(kadIpToString(c.ip)).arg(r.tcpPort));
    m_host->kadRequestBuddy(kadIpToEd2k(c.ip), r.tcpPort, c.port, r.userHash.toHash(), r.connectOptions);
}

void KadNode::onCallbackReq(const Ctx &c, const QByteArray &p)
{
    CallbackReq q;
    if (!decodeCallbackReq(p, &q)) { ++m_ctr.droppedMalformed; return; }
    if (m_host->kadRelayCallback(q.buddyId.toWire(), q.fileId.toWire(), c.ip, q.tcpPort)) ++m_ctr.answeredBuddy;
}

void KadNode::onPing(const Ctx &c)
{
    Writer w(2);
    w.u16(c.port);
    sendKad(c.ip, c.port, KADEMLIA2_PONG, w.data(), nullptr, keyValue(c.sk), false);
    ++m_ctr.answeredPing;
}

void KadNode::onPong(const Ctx &c, const QByteArray &p)
{
    if (p.size() < 2) { ++m_ctr.droppedMalformed; return; }
    if (!onOutTrackList(c.ip, KADEMLIA2_PING)) { ++m_ctr.droppedUnsolicited; return; }
    if (m_externAskIps.size() >= EXTERNAL_PORT_ASKIPS || m_externAskIps.contains(c.ip)) return;
    Reader r(p);
    const quint16 port = r.u16();
    m_externAskIps.append(c.ip);
    if (m_externPorts.contains(port)) {
        m_externPort = port;
        while (m_externAskIps.size() < EXTERNAL_PORT_ASKIPS) m_externAskIps.append(0);
        emit logMessage(QStringLiteral("external UDP port %1 (internal %2)").arg(port).arg(m_host->kadUdpPort()));
    } else {
        m_externPorts.append(port);
        if (m_externAskIps.size() >= EXTERNAL_PORT_ASKIPS) m_externPort = 0;   // unreliable
    }
    udpFwQueryNext();
}

void KadNode::onFirewallUdp(const Ctx &c, const QByteArray &p)
{
    FirewallUdpMsg m;
    if (!decodeFirewallUdp(p, &m)) { ++m_ctr.droppedMalformed; return; }
    auto it = m_udpTesters.find(c.ip);
    if (it == m_udpTesters.end()) { ++m_ctr.droppedUnsolicited; return; }
    if (it.value()) return;   // second answer of the same test
    it.value() = true;
    if (m_udpChecksFinished >= UDP_FIREWALLTEST_CLIENTSTOASK) return;
    m_udpChecksRunning = qMax(0, m_udpChecksRunning - 1);
    const quint16 intern = m_host->kadUdpPort();
    if (m.error == 0 && m.port && (m.port == intern || m.port == m_externPort)) {
        m_udpFirewalled = false;
        m_udpVerified = true;
        m_udpTimedOut = false;
        m_udpChecksFinished = UDP_FIREWALLTEST_CLIENTSTOASK;
        m_udpCandidates.clear();
        emit logMessage(QStringLiteral("UDP firewall test passed (port %1 reachable)").arg(m.port));
        emit statusChanged();
        return;
    }
    if (m.error == 0) {
        if (++m_udpChecksFinished >= UDP_FIREWALLTEST_CLIENTSTOASK) {
            m_udpFirewalled = true;
            m_udpVerified = true;
            emit statusChanged();
        }
    }
    udpFwQueryNext();
}

void KadNode::answerUdpFirewallCheck(quint32 ed2kIp, quint16 internPort, quint16 externPort, quint32 senderKey, bool alreadyKnown)
{
    if (!m_running || internPort == 0) return;
    const quint32 ip = kadIpFromEd2k(ed2kIp);
    const bool known = alreadyKnown || (m_routing && m_routing->findByIp(ip));
    FirewallUdpMsg m;
    m.error = known ? 1 : 0;
    m.port = internPort;
    sendKad(ip, internPort, KADEMLIA2_FIREWALLUDP, encodeFirewallUdp(m), nullptr, senderKey, false);
    if (externPort && externPort != internPort) {
        m.port = externPort;
        sendKad(ip, externPort, KADEMLIA2_FIREWALLUDP, encodeFirewallUdp(m), nullptr, senderKey, false);
    }
    ++m_ctr.answeredFirewall;
}

void KadNode::onUdpFirewallCheckFailed(quint32 ed2kIp)
{
    const quint32 ip = kadIpFromEd2k(ed2kIp);
    auto it = m_udpTesters.find(ip);
    if (it == m_udpTesters.end() || it.value()) return;
    it.value() = true;
    m_udpChecksRunning = qMax(0, m_udpChecksRunning - 1);
    udpFwQueryNext();
}

void KadNode::udpFwQueryNext()
{
    if (!m_running || !m_routing) return;   // [glue-2] a peer connection closing after stop() (crash at exit)
    if (!m_cfg.firewallChecks || m_udpChecksFinished >= UDP_FIREWALLTEST_CLIENTSTOASK) return;
    if (m_udpChecksRunning + m_udpChecksFinished >= UDP_FIREWALLTEST_CLIENTSTOASK) return;
    if (m_externAskIps.size() < EXTERNAL_PORT_ASKIPS) return;   // wait until we know our external port
    while (!m_udpCandidates.isEmpty()) {
        const KadPeer k = m_udpCandidates.takeFirst();
        if (k.version <= KADEMLIA_VERSION5_48a || k.id == m_id || k.ip == m_kadIp || !k.tcpPort) continue;
        if (m_udpTesters.contains(k.ip) || m_contactedIps.contains(k.ip) || m_routing->findByIp(k.ip)) continue;
        if (m_host->kadUdpFirewallCheck(kadIpToEd2k(k.ip), k.tcpPort, m_host->kadUdpPort(), m_externPort,
                                        verifyKeyFor(kadIpToEd2k(k.ip)))) {
            m_udpTesters.insert(k.ip, false);
            ++m_udpChecksRunning;
            emit logMessage(QStringLiteral("UDP firewall test requested from %1:%2").arg(kadIpToString(k.ip)).arg(k.tcpPort));
            break;
        }
    }
}

void KadNode::buddyConnected(quint32 ed2kIp, quint16 udpPort)
{
    m_buddyIp = ed2kIp;
    m_buddyPort = udpPort;
    emit logMessage(QStringLiteral("buddy %1 (UDP %2), we can be reached through it").arg(ipToString(ed2kIp)).arg(udpPort));
    for (auto it = m_nextSrcPublish.begin(); it != m_nextSrcPublish.end(); ++it) it.value() = 0;   // republish sources
    emit statusChanged();
}

void KadNode::buddyLost()
{
    if (!m_buddyIp) return;
    m_buddyIp = 0;
    m_buddyPort = 0;
    m_nextFindBuddy = m_host->kadNowSecs() + 30;
    emit logMessage(QStringLiteral("buddy lost"));
    emit statusChanged();
}

bool KadNode::checkCallback(const QByteArray &checkWire, const QByteArray &fileWire) const
{
    if (checkWire.size() != 16 || fileWire.size() != 16) return false;
    if ((KadId::fromWire(checkWire) ^ KadId::allOnes()) != m_id) return false;
    return m_shared.contains(KadId::fromWire(fileWire).toHash());
}

bool KadNode::sendCallbackRequest(quint32 buddyEd2kIp, quint16 buddyPort, const QByteArray &buddyIdWire, const QByteArray &fileHash)
{
    if (!connected() || buddyIdWire.size() != 16 || fileHash.size() != 16) return false;
    CallbackReq q;
    q.buddyId = KadId::fromWire(buddyIdWire);
    q.fileId = KadId::fromHash(fileHash);
    q.tcpPort = m_host->kadTcpPort();
    return sendKad(kadIpFromEd2k(buddyEd2kIp), buddyPort, KADEMLIA_CALLBACK_REQ, encodeCallbackReq(q), nullptr, 0, true);
}

// ---------------------------------------------------------------- lookups
Lookup *KadNode::startLookup(LookupType type, const KadId &target)
{
    if (!m_running || m_lookups.contains(target) || m_lookups.size() >= 64) return nullptr;
    QList<KadPeer> start = m_routing->closest(target, 3, 50, true);
    if (start.isEmpty()) start = m_routing->closest(target, 3, 50, false);
    if (start.isEmpty()) return nullptr;
    auto *l = new Lookup(type, target, m_id, m_host->kadNowSecs(), m_nextLookupId++);
    m_lookups.insert(target, l);
    l->start(this, start);
    return l;
}

void KadNode::lookupSendRequest(Lookup &l, const KadPeer &peer, quint8 count)
{
    KadReq q;
    q.type = count;
    q.target = l.target();
    q.receiver = peer.id;
    sendToPeer(peer, KADEMLIA2_REQ, encodeKadReq(q), true);
}

void KadNode::lookupReached(Lookup &l, const KadPeer &peer)
{
    auto asked = [&] { m_searchAsked[l.id()].insert(peer.ip); };
    switch (l.type()) {
    case LookupType::Keyword: {
        if (peer.version < 3) return;
        SearchKeyReq q;
        q.target = l.target();
        q.tree = l.searchTree;
        asked();
        sendToPeer(peer, KADEMLIA2_SEARCH_KEY_REQ, encodeSearchKeyReq(q), true);
        return;
    }
    case LookupType::File: {
        if (peer.version < 3) return;
        SearchSourceReq q;
        q.target = l.target();
        q.size = quint64(l.fileSize);
        asked();
        sendToPeer(peer, KADEMLIA2_SEARCH_SOURCE_REQ, encodeSearchSourceReq(q), true);
        return;
    }
    case LookupType::StoreKeyword: {
        if (l.answers() > l.total()) { l.prepareToStop(m_host->kadNowSecs()); return; }
        if (peer.version < 2) return;
        PublishKeyReq q;
        q.keyword = l.target();
        auto flush = [&] {
            if (q.entries.isEmpty()) return;
            sendToPeer(peer, KADEMLIA2_PUBLISH_KEY_REQ, encodePublishKeyReq(q), true);
            q.entries.clear();
        };
        int bytes = 18;
        for (const KadId &fid : std::as_const(l.fileIds)) {
            auto it = m_shared.constFind(fid.toHash());
            if (it == m_shared.cend()) continue;
            PublishKeyEntry e;
            e.file = fid;
            e.tags = keywordTagsFor(*it, peer.version);
            Writer w;
            writeKadTagList(w, e.tags);
            const int esz = 16 + int(w.size());
            // eMule receives into a 5000-byte buffer: keep every datagram well below that
            if ((bytes + esz > 3800 || q.entries.size() >= 50) && !q.entries.isEmpty()) { flush(); bytes = 18; }
            q.entries.append(e);
            bytes += esz;
        }
        flush();
        return;
    }
    case LookupType::StoreFile: {
        if (l.answers() > l.total()) { l.prepareToStop(m_host->kadNowSecs()); return; }
        if (peer.version < 4) return;
        auto it = m_shared.constFind(l.target().toHash());
        if (it == m_shared.cend()) { l.prepareToStop(m_host->kadNowSecs()); return; }
        const mx::SharedFile &f = *it;
        TagList tags;
        const bool big = f.size > OLD_MAX_FILE_SIZE;
        if (!firewalled()) {
            tags << Tag::makeInt(TAG_SOURCETYPE, big ? 4 : 1) << Tag::makeInt(TAG_SOURCEPORT, m_host->kadTcpPort());
        } else if (directCallbackPossible()) {
            tags << Tag::makeInt(TAG_SOURCETYPE, 6) << Tag::makeInt(TAG_SOURCEPORT, m_host->kadTcpPort());
        } else if (hasBuddy()) {
            tags << Tag::makeInt(TAG_SOURCETYPE, big ? 5 : 3) << Tag::makeInt(TAG_SERVERIP, m_buddyIp)
                 << Tag::makeInt(TAG_SERVERPORT, m_buddyPort)
                 << Tag::makeStr(TAG_BUDDYHASH, QString::fromLatin1(buddyId().toWire().toHex()))
                 << Tag::makeInt(TAG_SOURCEPORT, m_host->kadTcpPort());
        } else {
            l.prepareToStop(m_host->kadNowSecs());
            return;
        }
        tags << Tag::makeInt(TAG_FILESIZE, quint64(f.size)) << Tag::makeInt(TAG_ENCRYPTION, connectOptions(true));
        PublishSourceReq q;
        q.file = l.target();
        q.source = KadId::fromHash(m_host->kadUserHash());
        q.tags = tags;
        sendToPeer(peer, KADEMLIA2_PUBLISH_SOURCE_REQ, encodePublishSourceReq(q), true);
        return;
    }
    case LookupType::FindBuddy: {
        if (l.answers() > l.total()) { l.prepareToStop(m_host->kadNowSecs()); return; }
        FindBuddyReq q;
        q.buddyId = l.target();
        q.userHash = KadId::fromHash(m_host->kadUserHash());
        q.tcpPort = m_host->kadTcpPort();
        sendToPeer(peer, KADEMLIA_FINDBUDDY_REQ, encodeFindBuddyReq(q), true);
        ++m_ctr.buddyRequests;
        l.addAnswers();
        return;
    }
    default:
        return;
    }
}

void KadNode::lookupFinished(Lookup *l)
{
    const qint64 now = m_host->kadNowSecs();
    m_searchAsked.remove(l->id());
    switch (l->type()) {
    case LookupType::NodeComplete:
        if (!m_publishAllowed) {
            m_publishAllowed = true;
            emit logMessage(QStringLiteral("self lookup done (%1 answers), publishing enabled").arg(l->answers()));
        }
        break;
    case LookupType::Keyword: {
        auto it = m_queries.find(l->queryId);
        if (it != m_queries.end()) {
            it->lookups.remove(l->id());
            bool pending = false;
            for (const auto &pl : std::as_const(m_pendingKeywordLookups)) if (pl.first == l->queryId) pending = true;
            if (it->lookups.isEmpty() && !pending && !it->done) {
                it->done = true;
                const quint32 qid = it.key();
                emit logMessage(QStringLiteral("search \"%1\" done, %2 result(s)").arg(it->q.text).arg(it->hits));
                m_queries.erase(it);
                emit searchDone(qid);
            }
        }
        break;
    }
    case LookupType::File: {
        for (Download &d : m_downloads) {
            if (d.lookupId != l->id()) continue;
            d.lookupId = 0;
            d.nextSearch = now + (l->answers() > 0 ? KADEMLIAREASKTIME : 600);
            emit logMessage(QStringLiteral("source lookup for %1 done, %2 source(s), %3 nodes answered")
                                .arg(d.name.isEmpty() ? toHex(d.hash) : d.name).arg(l->answers()).arg(l->responses()));
        }
        break;
    }
    case LookupType::NodeFwCheckUdp:
        if (m_udpChecksFinished < UDP_FIREWALLTEST_CLIENTSTOASK && m_udpCandidates.isEmpty() && m_udpChecksRunning == 0)
            m_udpTestNeedsLookup = true;   // try another random target later
        break;
    default:
        break;
    }
}

TagList KadNode::keywordTagsFor(const mx::SharedFile &f, quint8 targetVersion) const
{
    TagList tags;
    tags << Tag::makeStr(TAG_FILENAME, f.name);
    if (f.size > OLD_MAX_FILE_SIZE) {
        Tag t; t.type = TAGTYPE_BSOB; t.id = TAG_FILESIZE;
        Writer w; w.u64(quint64(f.size)); t.blob = w.take();
        tags << t;
    } else {
        tags << Tag::makeInt(TAG_FILESIZE, quint64(f.size));
    }
    tags << Tag::makeInt(TAG_SOURCES, 1);
    if (targetVersion >= KADEMLIA_VERSION9_50a && f.hashes.aich.size() == 20) {
        Tag t; t.type = TAGTYPE_BSOB; t.id = TAG_KADAICHHASHPUB; t.blob = f.hashes.aich;
        tags << t;
    }
    const QString type = ed2kTypeForMedia(f.type != mx::MediaType::Any ? f.type : mx::mediaTypeForFileName(f.name));
    if (!type.isEmpty()) tags << Tag::makeStr(TAG_FILETYPE, type);
    if (f.durationSec > 0) tags << Tag::makeInt(TAG_MEDIA_LENGTH, quint64(f.durationSec));
    if (f.bitrateKbps > 0) tags << Tag::makeInt(TAG_MEDIA_BITRATE, quint64(f.bitrateKbps));
    return tags;
}

// ---------------------------------------------------------------- results
void KadNode::processKeywordResult(Lookup &l, const KadPeer *from, const SearchResEntry &e)
{
    auto qit = m_queries.find(l.queryId);
    if (qit == m_queries.end()) return;
    Query &q = *qit;
    const quint8 fromVersion = from ? from->version : 0;
    QString name, type, codec, artist, album, title;
    quint64 size = 0;
    int length = -1, bitrate = -1, avail = -1, complete = -1;
    quint32 publishInfo = 0;
    QList<QPair<int, QByteArray>> aichs;
    for (const Tag &t : e.tags) {
        if (!t.name.isEmpty()) continue;
        switch (t.id) {
        case TAG_FILENAME: if (t.isStr()) name = t.str; break;
        case TAG_FILESIZE: { TagList one{t}; size = kadTagFileSize(one); break; }
        case TAG_FILETYPE: if (t.isStr()) type = t.str.left(16); break;
        case TAG_MEDIA_ARTIST: if (t.isStr()) artist = t.str.left(128); break;
        case TAG_MEDIA_ALBUM: if (t.isStr()) album = t.str.left(128); break;
        case TAG_MEDIA_TITLE: if (t.isStr()) title = t.str.left(128); break;
        case TAG_MEDIA_LENGTH: if (t.isInt()) length = int(qMin<quint64>(t.num, 0x7FFFFFFF)); break;
        case TAG_MEDIA_BITRATE: if (t.isInt()) bitrate = int(qMin<quint64>(t.num, 100000)); break;
        case TAG_MEDIA_CODEC: if (t.isStr()) codec = t.str.left(32); break;
        case TAG_SOURCES: if (t.isInt()) avail = t.num > 65500 ? 0 : int(t.num); break;
        case TAG_COMPLETE_SOURCES: if (t.isInt()) complete = t.num > 65500 ? 0 : int(t.num); break;
        case TAG_PUBLISHINFO: if (t.isInt() && fromVersion >= KADEMLIA_VERSION6_49aBETA) publishInfo = quint32(t.num); break;
        case TAG_KADAICHHASHRESULT:
            if (fromVersion >= KADEMLIA_VERSION9_50a && (t.type == TAGTYPE_BSOB || t.type == TAGTYPE_BLOB)) {
                Reader r(t.blob);
                const quint8 n = r.u8();
                for (int i = 0; i < n && r.ok(); ++i) {
                    const quint8 pop = r.u8();
                    const QByteArray h = r.bytes(20);
                    if (r.ok() && pop > 0) aichs.append({pop, h});
                }
                if (!r.ok()) aichs.clear();
            }
            break;
        default: break;
        }
    }
    if (name.trimmed().isEmpty() || size == 0 || size > quint64(MAX_FILE_SIZE)) return;
    // eMule: the keyword we searched for must be one of the file name's words
    if (!kadGetWords(name).contains(l.name)) return;
    if (!kadResultMatches(q.q, q.plan.expr, name, qint64(size), type, bitrate)) return;
    const int pubs = int((publishInfo >> 16) & 0xFF), names = int(publishInfo >> 24);
    const double trust = double(publishInfo & 0xFFFF) / 100.0;
    // spam heuristic: a single publisher whose /24 flooded the indexing node (trust < 0.1)
    if (publishInfo && pubs <= 1 && trust < 0.1) return;
    const QByteArray hash = e.answer.toHash();
    l.addAnswers();
    if (q.seen.contains(hash) || q.hits >= qMin(q.q.maxResults > 0 ? q.q.maxResults : 2000, m_cfg.maxSearchResults)) return;
    q.seen.insert(hash);
    ++q.hits;
    mx::SearchHit h;
    h.queryId = l.queryId;
    h.fileName = name;
    h.size = qint64(size);
    h.type = mediaTypeFromEd2k(type, name);
    h.durationSec = length > 0 ? length : -1;
    h.bitrateKbps = bitrate > 0 ? bitrate : -1;
    h.hashes.ed2k = hash;
    if (aichs.size() == 1 && pubs > 0 && pubs / aichs[0].first <= 3) h.hashes.aich = aichs[0].second;
    h.key = QStringLiteral("ed2k:") + toHex(hash);
    mx::SearchSource s;
    s.net = mx::Net::Kad;
    s.uri = formatEd2kFileLink(name, qint64(size), hash);
    s.connection = QStringLiteral("Kad");
    s.peers = avail;
    s.seeds = complete;
    if (publishInfo) {
        s.extra.insert(QStringLiteral("trust"), trust);
        s.extra.insert(QStringLiteral("publishers"), pubs);
        s.extra.insert(QStringLiteral("names"), names);
    }
    if (!codec.isEmpty()) s.extra.insert(QStringLiteral("codec"), codec);
    if (!artist.isEmpty()) s.extra.insert(QStringLiteral("artist"), artist);
    if (!album.isEmpty()) s.extra.insert(QStringLiteral("album"), album);
    if (!title.isEmpty()) s.extra.insert(QStringLiteral("title"), title);
    h.sources.append(s);
    emit searchHits(l.queryId, {h});
    if (q.hits >= qMin(q.q.maxResults > 0 ? q.q.maxResults : 2000, m_cfg.maxSearchResults))
        for (quint32 lid : std::as_const(q.lookups))
            for (Lookup *x : std::as_const(m_lookups)) if (x->id() == lid) x->prepareToStop(m_host->kadNowSecs());
}

void KadNode::processSourceResult(Lookup &l, const SearchResEntry &e)
{
    Download *d = nullptr;
    for (Download &x : m_downloads) if (x.lookupId == l.id()) d = &x;
    KadSource s;
    s.userHash = e.answer.toHash();
    quint32 hostIp = 0;
    for (const Tag &t : e.tags) {
        if (!t.name.isEmpty()) continue;
        switch (t.id) {
        case TAG_SOURCETYPE: if (t.isInt()) s.type = quint8(t.num); break;
        case TAG_SOURCEIP: if (t.isInt()) hostIp = quint32(t.num); break;
        case TAG_SOURCEPORT: if (t.isInt()) s.tcpPort = quint16(t.num); break;
        case TAG_SOURCEUPORT: if (t.isInt()) s.udpPort = quint16(t.num); break;
        case TAG_SERVERIP: if (t.isInt()) s.buddyIp = quint32(t.num); break;
        case TAG_SERVERPORT: if (t.isInt()) s.buddyPort = quint16(t.num); break;
        case TAG_BUDDYHASH:
            if (t.isStr()) { const QByteArray b = QByteArray::fromHex(t.str.toLatin1()); if (b.size() == 16) s.buddyId = b; }
            break;
        case TAG_ENCRYPTION: if (t.isInt()) s.cryptOptions = quint8(t.num); break;
        default: break;
        }
    }
    s.ip = hostIp ? kadIpToEd2k(hostIp) : 0;
    bool usable = false;
    switch (s.type) {
    case 1: case 4: usable = s.tcpPort && hostIp && isGoodKadIpPort(hostIp, s.tcpPort, m_cfg.allowLan); break;
    case 3: case 5:
        // firewalled with buddy: only useful if the source can connect to us
        usable = !firewalled() && s.tcpPort && s.buddyIp && s.buddyPort && s.buddyId.size() == 16;
        s.ip = 0;
        break;
    case 6: usable = !firewalled() && (s.cryptOptions & 0x08) && hostIp && s.udpPort && s.tcpPort; break;
    default: break;
    }
    if (!usable || s.userHash == m_host->kadUserHash()) return;
    l.addAnswers();
    if (!d || d->known.contains(s.userHash) || d->known.size() >= 2000) return;
    d->known.insert(s.userHash);
    emit sourcesFound(d->id, d->hash, {s});
}

// ---------------------------------------------------------------- searches API
bool KadNode::search(const mx::SearchQuery &q, QString *error)
{
    if (!m_running || contacts() == 0) { if (error) *error = QStringLiteral("Kad is not connected"); return false; }
    KadQueryPlan plan = planKadSearch(q);
    if (!plan.error.isEmpty()) { if (error) *error = plan.error; return false; }
    Query query;
    query.q = q;
    query.plan = plan;
    m_queries.insert(q.id, query);
    int started = 0;
    for (const KadLookupPlan &lp : std::as_const(plan.lookups)) {
        Lookup *l = startLookup(LookupType::Keyword, lp.target);
        if (!l) { m_pendingKeywordLookups.append({q.id, lp}); continue; }
        l->queryId = q.id;
        l->name = lp.keyword;
        l->searchTree = lp.tree;
        m_queries[q.id].lookups.insert(l->id());
        ++started;
    }
    QStringList kws;
    for (const KadLookupPlan &lp : std::as_const(plan.lookups)) kws << lp.keyword;
    emit logMessage(QStringLiteral("searching \"%1\" (keyword lookup%2 %3)").arg(q.text, plan.lookups.size() > 1 ? QStringLiteral("s") : QString(),
                                                                                       kws.join(QStringLiteral(", "))));
    if (!started && m_pendingKeywordLookups.isEmpty()) { m_queries.remove(q.id); if (error) *error = QStringLiteral("no contacts"); return false; }
    return true;
}

void KadNode::cancelSearch(quint32 queryId)
{
    auto it = m_queries.find(queryId);
    if (it == m_queries.end()) return;
    for (quint32 lid : std::as_const(it->lookups))
        for (Lookup *l : std::as_const(m_lookups)) if (l->id() == lid) { l->queryId = 0; l->prepareToStop(m_host->kadNowSecs()); }
    for (int i = int(m_pendingKeywordLookups.size()) - 1; i >= 0; --i)
        if (m_pendingKeywordLookups[i].first == queryId) m_pendingKeywordLookups.removeAt(i);
    m_queries.erase(it);
}

void KadNode::findSources(const QString &downloadId, const QByteArray &ed2kHash, qint64 size, const QString &name)
{
    if (ed2kHash.size() != 16 || size <= 0) return;
    auto it = m_downloads.find(downloadId);
    if (it == m_downloads.end()) {
        Download d;
        d.id = downloadId;
        d.hash = ed2kHash;
        d.size = size;
        d.name = name;
        d.nextSearch = 0;
        m_downloads.insert(downloadId, d);
    } else if (!name.isEmpty()) {
        it->name = name;
    }
}

void KadNode::stopFindSources(const QString &downloadId)
{
    auto it = m_downloads.find(downloadId);
    if (it == m_downloads.end()) return;
    for (Lookup *l : std::as_const(m_lookups)) if (l->id() == it->lookupId) l->prepareToStop(m_host->kadNowSecs());
    m_downloads.erase(it);
}

void KadNode::setSharedFiles(const QList<mx::SharedFile> &files)
{
    QHash<QByteArray, mx::SharedFile> shared;
    for (const mx::SharedFile &f : files)
        if (f.hashes.ed2k.size() == 16 && f.size > 0 && !f.name.isEmpty()) shared.insert(f.hashes.ed2k, f);
    m_shared = shared;
    // keyword index of our files (eMule CPublishKeywordList)
    QHash<KadId, PubKeyword> kw;
    for (auto it = m_shared.cbegin(); it != m_shared.cend(); ++it) {
        for (const QString &w : kadGetWords(it->name)) {
            const KadId t = kadKeywordHash(w);
            PubKeyword &k = kw[t];
            if (k.word.isEmpty()) {
                k.word = w;
                k.target = t;
                k.next = m_keywords.contains(t) ? m_keywords.value(t).next : 0;
            }
            if (k.files.size() < 1000) k.files.append(it.key());
        }
    }
    m_keywords = kw;
    m_keywordOrder = m_keywords.keys();
    std::sort(m_keywordOrder.begin(), m_keywordOrder.end());
    m_keywordCursor = 0;
    m_sharedOrder = m_shared.keys();
    std::sort(m_sharedOrder.begin(), m_sharedOrder.end());
    m_srcCursor = 0;
    for (auto it = m_nextSrcPublish.begin(); it != m_nextSrcPublish.end();)
        if (!m_shared.contains(it.key())) it = m_nextSrcPublish.erase(it); else ++it;
}

// ---------------------------------------------------------------- periodic
void KadNode::publishTick(qint64 now)
{
    if (!m_cfg.publish || !m_publishAllowed || !connected() || m_shared.isEmpty()) return;
    if (firewalled() && !hasBuddy() && !directCallbackPossible()) return;   // nobody could download from us
    int storeKey = 0, storeSrc = 0;
    for (Lookup *l : std::as_const(m_lookups)) {
        if (l->type() == LookupType::StoreKeyword) ++storeKey;
        else if (l->type() == LookupType::StoreFile) ++storeSrc;
    }
    if (storeKey < KADEMLIATOTALSTOREKEY && now >= m_nextPublishKey && !m_keywordOrder.isEmpty()) {
        m_nextPublishKey = now + 2;
        for (int n = 0; n < m_keywordOrder.size(); ++n) {
            const KadId t = m_keywordOrder[m_keywordCursor % m_keywordOrder.size()];
            m_keywordCursor = (m_keywordCursor + 1) % int(m_keywordOrder.size());
            auto it = m_keywords.find(t);
            if (it == m_keywords.end() || it->next > now) continue;
            if (m_lookups.contains(t)) continue;
            Lookup *l = startLookup(LookupType::StoreKeyword, t);
            if (!l) break;
            l->name = it->word;
            // ≤ 150 files per keyword publish, rotating through longer lists
            const int nFiles = int(it->files.size());
            for (int i = 0; i < qMin(150, nFiles); ++i) l->fileIds.append(KadId::fromHash(it->files[(it->rotate + i) % nFiles]));
            it->rotate = nFiles > 150 ? (it->rotate + 150) % nFiles : 0;
            it->next = now + KADEMLIAREPUBLISHTIMEK;
            break;
        }
    }
    if (storeSrc < KADEMLIATOTALSTORESRC && now >= m_nextPublishSrc && !m_sharedOrder.isEmpty()) {
        m_nextPublishSrc = now + 2;
        for (int n = 0; n < m_sharedOrder.size(); ++n) {
            const QByteArray h = m_sharedOrder[m_srcCursor % m_sharedOrder.size()];
            m_srcCursor = (m_srcCursor + 1) % int(m_sharedOrder.size());
            if (m_nextSrcPublish.value(h) > now) continue;
            const KadId t = KadId::fromHash(h);
            if (m_lookups.contains(t)) continue;
            Lookup *l = startLookup(LookupType::StoreFile, t);
            if (!l) break;
            l->name = m_shared.value(h).name;
            m_nextSrcPublish.insert(h, now + KADEMLIAREPUBLISHTIMES);
            break;
        }
    }
}

void KadNode::sourceSearchTick(qint64 now)
{
    if (!connected() || now < m_nextSourceSearch) return;
    int running = 0;
    for (Lookup *l : std::as_const(m_lookups)) if (l->type() == LookupType::File) ++running;
    if (running >= KADEMLIATOTALFILE) return;
    for (Download &d : m_downloads) {
        if (d.lookupId || d.nextSearch > now) continue;
        Lookup *l = startLookup(LookupType::File, KadId::fromHash(d.hash));
        if (!l) { d.nextSearch = now + 60; continue; }
        l->downloadId = d.id;
        l->fileSize = d.size;
        l->name = d.name;
        d.lookupId = l->id();
        d.nextSearch = now + KADEMLIAREASKTIME;
        m_nextSourceSearch = now + 1;
        emit logMessage(QStringLiteral("looking for sources of %1").arg(d.name.isEmpty() ? toHex(d.hash) : d.name));
        break;
    }
}

void KadNode::firewallTick(qint64 now)
{
    if (!m_cfg.firewallChecks) return;
    if (now >= m_nextFirewallCheck) {
        m_nextFirewallCheck = now + 3600;
        if (m_fwAcks >= 2 || m_recheckIp >= KADEMLIAFIREWALLCHECKS) m_fwHadVerdict = true;   // [diag-1]
        m_lastFirewallState = (m_fwAcks < 2);
        m_fwAcks = 0;
        m_recheckIp = 0;
        m_fwAsked.clear();
        // UDP recheck
        m_udpLastState = m_udpFirewalled;
        m_udpChecksRunning = m_udpChecksFinished = 0;
        m_udpTimedOut = false;
        m_udpTestNeedsLookup = true;
        m_externAskIps.clear();
        m_externPorts.clear();
        m_nextFindBuddy = qMax(m_nextFindBuddy, now + 5 * 60);
    }
    for (auto it = m_fwAsked.begin(); it != m_fwAsked.end();) {
        if (it.value() >= 0 && m_host->kadNowMs() - it.value() > 10 * 60 * 1000) it = m_fwAsked.erase(it); else ++it;
    }
    const bool udpRunning = m_udpChecksFinished < UDP_FIREWALLTEST_CLIENTSTOASK;
    if (udpRunning && connected()) {
        if (m_udpTestNeedsLookup) {
            m_udpTestNeedsLookup = false;
            if (!m_udpTestStart || m_udpChecksFinished == 0) m_udpTestStart = now;
            startLookup(LookupType::NodeFwCheckUdp, KadId::random());
        }
        if (!m_udpTimedOut && !m_udpFirewalled && firewalled() && m_udpTestStart && now - m_udpTestStart > 6 * 60 && !m_udpVerified) {
            m_udpTimedOut = true;
            emit logMessage(QStringLiteral("UDP firewall test timed out, assuming UDP firewalled"));
            emit statusChanged();
        }
        if (m_externAskIps.size() < EXTERNAL_PORT_ASKIPS && now >= m_nextExternPortLookup) {
            m_nextExternPortLookup = now + 15;
            if (const Contact *c = m_routing->randomContact(3, KADEMLIA_VERSION6_49aBETA))
                sendKad(c->ip, c->udpPort, KADEMLIA2_PING, QByteArray(), &c->id, c->keyFor(ourEd2kIp()), true);
        }
        udpFwQueryNext();
    }
}

void KadNode::tick()
{
    if (!m_running) return;
    const qint64 now = m_host->kadNowSecs();
    const qint64 nowMs = m_host->kadNowMs();
    const bool wasConnected = connected();
    // bootstrap while not connected
    if (!connected() && !m_bootstrap.isEmpty() && now >= m_nextBootstrap) {
        const BootNode b = m_bootstrap.takeFirst();
        m_nextBootstrap = now + (contacts() == 0 ? 2 : 15);
        sendKad(b.ip, b.port, KADEMLIA2_BOOTSTRAP_REQ, QByteArray(), (b.version >= KADEMLIA_VERSION6_49aBETA && !b.id.isZero()) ? &b.id : nullptr, 0, true);
    }
    // routing table timers (HELLO probes, random lookups)
    const bool longNoContact = m_lastContact && (now - m_lastContact) > (KADEMLIADISCONNECTDELAY - 5 * 60);
    m_routing->onTimers(now, longNoContact,
        [this](const Contact &c) {
            sendMyDetails(KADEMLIA2_HELLO_REQ, c.ip, c.udpPort, c.version, SenderKey{c.udpKey, c.udpKeyIp},
                          c.version >= KADEMLIA_VERSION6_49aBETA ? &c.id : nullptr, false);
        },
        [this](const KadId &t) { startLookup(LookupType::Node, t); });
    if (now >= m_nextSelfLookup && contacts() > 0) {
        if (startLookup(LookupType::NodeComplete, m_id)) m_nextSelfLookup = now + (contacts() < 30 ? 60 : 4 * 3600);
        else m_nextSelfLookup = now + 10;
    }
    // sparse table (fresh network, LAN, just bootstrapped): a random lookup every 10 s; the zone
    // big timers alone would do only one per zone and hour
    if (contacts() > 0 && contacts() < 30 && now >= m_nextSparseLookup) {
        m_nextSparseLookup = now + 10;
        startLookup(LookupType::Node, KadId::random());
    }
    // lookups
    const QList<KadId> targets = m_lookups.keys();
    for (const KadId &t : targets) {
        Lookup *l = m_lookups.value(t);
        if (!l) continue;
        if (l->maintain(this, now) == 2) {
            m_lookups.remove(t);
            lookupFinished(l);
            delete l;
        }
    }
    // pending keyword lookups (target was busy)
    for (int i = 0; i < m_pendingKeywordLookups.size();) {
        const auto pl = m_pendingKeywordLookups[i];
        auto qit = m_queries.find(pl.first);
        if (qit == m_queries.end()) { m_pendingKeywordLookups.removeAt(i); continue; }
        if (m_lookups.contains(pl.second.target)) { ++i; continue; }
        Lookup *l = startLookup(LookupType::Keyword, pl.second.target);
        m_pendingKeywordLookups.removeAt(i);
        if (!l) {
            if (qit->lookups.isEmpty()) { const quint32 id = qit.key(); m_queries.erase(qit); emit searchDone(id); }
            continue;
        }
        l->queryId = pl.first;
        l->name = pl.second.keyword;
        l->searchTree = pl.second.tree;
        qit->lookups.insert(l->id());
    }
    firewallTick(now);
    // buddy search when we are firewalled on both TCP and UDP
    if (m_cfg.buddies && connected() && firewalled() && udpFirewalled() && !hasBuddy() && now >= m_nextFindBuddy) {
        m_nextFindBuddy = now + 20 * 60;
        if (startLookup(LookupType::FindBuddy, m_id ^ KadId::allOnes()))
            emit logMessage(QStringLiteral("firewalled, looking for a buddy"));
    }
    sourceSearchTick(now);
    publishTick(now);
    m_index.clean(now);
    // housekeeping
    if (nowMs - m_lastInCleanup > 12 * 60 * 1000) {
        m_lastInCleanup = nowMs;
        for (auto it = m_inTrack.begin(); it != m_inTrack.end();) {
            if (nowMs - it->firstMs > 10 * 60 * 1000) it = m_inTrack.erase(it); else ++it;
        }
        for (auto it = m_banned.begin(); it != m_banned.end();) {
            if (it.value() < nowMs) it = m_banned.erase(it); else ++it;
        }
        if (m_contactedIps.size() > 50000) m_contactedIps.clear();
    }
    if (nowMs - m_lastOutCleanup > 60 * 1000) {
        m_lastOutCleanup = nowMs;
        for (auto it = m_outTrack.begin(); it != m_outTrack.end();) {
            while (!it->isEmpty() && nowMs - it->first() > 180000) it->removeFirst();
            if (it->isEmpty()) it = m_outTrack.erase(it); else ++it;
        }
    }
    if (now >= m_nextSave) {
        m_nextSave = now + 600;
        saveNodesDat();
        savePrefs();
    }
    if (wasConnected != connected()) emit statusChanged();
}

} // namespace mx::ed2k::kad
