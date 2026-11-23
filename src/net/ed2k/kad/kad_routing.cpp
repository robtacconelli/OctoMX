// SPDX-License-Identifier: GPL-3.0-or-later
// Parts of this file are adapted from eMule 0.50a (Copyright (C) the eMule team,
// https://www.emule-project.net; kademlia/routing/Contact.cpp, RoutingZone.cpp, RoutingBin.cpp), GPL-2.0-or-later;
// this file is distributed under GPL-3.0-or-later. See NOTICE.
#include "kad_routing.h"

#include <QtCore/QRandomGenerator>
#include <algorithm>
#include <cmath>
#include <map>

namespace mx::ed2k::kad {

static constexpr int MAX_CONTACTS_SUBNET = 10;
static constexpr int MAX_CONTACTS_IP = 1;

void Contact::checkingType(qint64 now)
{
    if (now - lastTypeSet < 10 || type == 4) return;
    lastTypeSet = now;
    expires = now + 120;
    ++type;
}

void Contact::updateType(qint64 now)
{
    const qint64 hours = (now - created) / 3600;
    if (hours <= 0) { type = 2; expires = now + 3600; }
    else if (hours == 1) { type = 1; expires = now + 5400; }
    else { type = 0; expires = now + 7200; }
}

struct RoutingTable::Zone {
    Zone *parent = nullptr;
    std::unique_ptr<Zone> sub[2];
    int level = 0;
    KadId index;
    bool leaf = true;
    std::vector<std::unique_ptr<Contact>> bin;   // front = oldest / least recently seen
    qint64 nextSmall = 0, nextBig = 0;
};

static bool indexBelow(const KadId &idx, quint32 v)
{
    return idx.chunk(0) == 0 && idx.chunk(1) == 0 && idx.chunk(2) == 0 && idx.chunk(3) < v;
}

RoutingTable::RoutingTable(const KadId &me, const RoutingOptions &opt) : m_me(me), m_opt(opt)
{
    m_root = std::make_unique<Zone>();
    initZone(m_root.get(), 0);
}

RoutingTable::~RoutingTable() = default;

void RoutingTable::initZone(Zone *z, qint64 now)
{
    z->nextSmall = now + qMin<quint32>(z->index.chunk(3), 60);
    z->nextBig = now + 10;
}

void RoutingTable::clear()
{
    m_root = std::make_unique<Zone>();
    initZone(m_root.get(), 0);
    m_ipCount.clear();
    m_subnetCount.clear();
    m_count = 0;
}

void RoutingTable::track(quint32 ip, int delta)
{
    auto adj = [delta](QHash<quint32, int> &h, quint32 k) {
        const int v = h.value(k) + delta;
        if (v <= 0) h.remove(k); else h.insert(k, v);
    };
    adj(m_ipCount, ip);
    adj(m_subnetCount, ip & 0xFFFFFF00u);
    m_count = qMax(0, m_count + delta);
}

bool RoutingTable::checkGlobalIpLimits(quint32 ip) const
{
    if (!m_opt.allowSameIp && m_ipCount.value(ip) >= MAX_CONTACTS_IP) return false;
    if (m_subnetCount.value(ip & 0xFFFFFF00u) >= MAX_CONTACTS_SUBNET && !isLanIp(ip)) return false;
    return true;
}

RoutingTable::Zone *RoutingTable::leafFor(const KadId &distance) const
{
    Zone *z = m_root.get();
    while (!z->leaf) z = z->sub[distance.bit(z->level)].get();
    return z;
}

Contact *RoutingTable::find(const KadId &id) const
{
    const Zone *z = leafFor(id ^ m_me);
    for (const auto &c : z->bin) if (c->id == id) return c.get();
    return nullptr;
}

Contact *RoutingTable::findByIp(quint32 ip, quint16 port, bool tcpPort) const
{
    QList<Zone *> leaves;
    collectLeaves(m_root.get(), leaves);
    for (Zone *z : leaves)
        for (const auto &c : z->bin)
            if (c->ip == ip && (port == 0 || (tcpPort ? c->tcpPort : c->udpPort) == port)) return c.get();
    return nullptr;
}

bool RoutingTable::canSplit(const Zone *z) const
{
    if (z->level >= 127) return false;
    return (indexBelow(z->index, KK) || z->level < KBASE) && int(z->bin.size()) == K;
}

bool RoutingTable::binAdd(Zone *z, std::unique_ptr<Contact> &c)
{
    int sameSubnet = 0;
    for (const auto &e : z->bin) {
        if (e->id == c->id) return false;
        if ((e->ip & 0xFFFFFF00u) == (c->ip & 0xFFFFFF00u)) ++sameSubnet;
    }
    if (!checkGlobalIpLimits(c->ip)) return false;
    if (sameSubnet >= 2 && !isLanIp(c->ip)) return false;
    if (int(z->bin.size()) >= K) return false;
    track(c->ip, +1);
    z->bin.push_back(std::move(c));
    return true;
}

void RoutingTable::split(Zone *z)
{
    std::vector<std::unique_ptr<Contact>> entries;
    entries.swap(z->bin);
    for (const auto &c : entries) track(c->ip, -1);
    const qint64 now = m_now;
    for (int side = 0; side < 2; ++side) {
        auto s = std::make_unique<Zone>();
        s->parent = z;
        s->level = z->level + 1;
        s->index = z->index.shiftedLeft(1).plus(quint32(side));
        initZone(s.get(), now);
        z->sub[side] = std::move(s);
    }
    z->leaf = false;
    for (auto &c : entries) binAdd(z->sub[c->distance.bit(z->level)].get(), c);   // dropped if the bin rules fail
}

bool RoutingTable::changeIp(Zone *z, Contact *c, quint32 newIp)
{
    if (c->ip == newIp) return true;
    if (!m_opt.allowSameIp && m_ipCount.value(newIp) >= MAX_CONTACTS_IP) return false;
    if ((c->ip & 0xFFFFFF00u) != (newIp & 0xFFFFFF00u)) {
        if (m_subnetCount.value(newIp & 0xFFFFFF00u) >= MAX_CONTACTS_SUBNET && !isLanIp(newIp)) return false;
        int same = 0;
        for (const auto &e : z->bin) if ((e->ip & 0xFFFFFF00u) == (newIp & 0xFFFFFF00u)) ++same;
        if (same >= 2 && !isLanIp(newIp)) return false;
    }
    track(c->ip, -1);
    c->ip = newIp;
    track(c->ip, +1);
    return true;
}

static void pushToBottom(std::vector<std::unique_ptr<Contact>> &bin, Contact *c)
{
    for (size_t i = 0; i < bin.size(); ++i) {
        if (bin[i].get() != c) continue;
        std::unique_ptr<Contact> keep = std::move(bin[i]);
        bin.erase(bin.begin() + qsizetype(i));
        bin.push_back(std::move(keep));
        return;
    }
}

bool RoutingTable::addToZone(Zone *z, std::unique_ptr<Contact> &c, bool &update, bool &outVerified, quint32 ourIp)
{
    while (!z->leaf) z = z->sub[c->distance.bit(z->level)].get();
    Contact *ex = nullptr;
    for (const auto &e : z->bin) if (e->id == c->id) { ex = e.get(); break; }
    if (ex) {
        if (!update) return false;
        const quint32 exKey = ex->keyFor(ourIp);
        if (exKey != 0 && exKey != c->keyFor(ourIp)) {
            update = false;   // hijack attempt: the update does not carry the key we know
        } else if (ex->version >= 1 && ex->version < KADEMLIA_VERSION6_49aBETA && ex->receivedHello) {
            if (ex->ip == c->ip && ex->tcpPort == c->tcpPort && ex->version == c->version && ex->udpPort == c->udpPort) {
                outVerified = ex->ipVerified;
                ex->updateType(c->lastTypeSet);
                pushToBottom(z->bin, ex);
            } else {
                update = false;
            }
        } else if (changeIp(z, ex, c->ip) && c->version >= ex->version) {
            ex->udpPort = c->udpPort;
            ex->tcpPort = c->tcpPort;
            ex->version = c->version;
            ex->udpKey = c->udpKey;
            ex->udpKeyIp = c->udpKeyIp;
            if (!ex->ipVerified) ex->ipVerified = c->ipVerified;
            outVerified = ex->ipVerified;
            ex->updateType(c->lastTypeSet);
            if (c->receivedHello) ex->receivedHello = true;
            pushToBottom(z->bin, ex);
        } else {
            update = false;
        }
        return false;
    }
    if (int(z->bin.size()) < K) {
        update = false;
        if (m_count >= m_opt.maxContacts) return false;
        return binAdd(z, c);
    }
    if (canSplit(z)) {
        split(z);
        return addToZone(z, c, update, outVerified, ourIp);
    }
    update = false;
    return false;
}

bool RoutingTable::add(const KadPeer &p, quint32 udpKey, quint32 udpKeyIp, bool &ipVerified, bool update, bool fromHello,
                       qint64 now, quint32 ourEd2kIp)
{
    m_now = now;
    if (p.id == m_me || p.version <= 1) return false;
    if (!isGoodKadIpPort(p.ip, p.udpPort, m_opt.allowLan)) return false;
    if (p.udpPort == 53 && p.version <= KADEMLIA_VERSION5_48a) return false;
    auto c = std::make_unique<Contact>();
    c->id = p.id;
    c->distance = p.id ^ m_me;
    c->ip = p.ip;
    c->udpPort = p.udpPort;
    c->tcpPort = p.tcpPort;
    c->version = p.version;
    c->udpKey = udpKey;
    c->udpKeyIp = udpKey ? udpKeyIp : 0;
    c->ipVerified = ipVerified;
    c->receivedHello = fromHello;
    c->created = c->lastTypeSet = now;
    if (!ourEd2kIp) ourEd2kIp = udpKeyIp;
    bool upd = update;
    if (addToZone(m_root.get(), c, upd, ipVerified, ourEd2kIp)) return true;
    return upd;
}

bool RoutingTable::verify(const KadId &id, quint32 ip)
{
    Contact *c = find(id);
    if (!c || c->ip != ip) return false;
    c->ipVerified = true;
    return true;
}

void RoutingTable::setAllVerified()
{
    QList<Zone *> leaves;
    collectLeaves(m_root.get(), leaves);
    for (Zone *z : leaves) for (auto &c : z->bin) c->ipVerified = true;
}

void RoutingTable::remove(const KadId &id)
{
    Zone *z = leafFor(id ^ m_me);
    for (size_t i = 0; i < z->bin.size(); ++i) {
        if (z->bin[i]->id != id) continue;
        track(z->bin[i]->ip, -1);
        z->bin.erase(z->bin.begin() + qsizetype(i));
        return;
    }
}

void RoutingTable::collectLeaves(Zone *z, QList<Zone *> &out) const
{
    if (z->leaf) { out.append(z); return; }
    collectLeaves(z->sub[0].get(), out);
    collectLeaves(z->sub[1].get(), out);
}

QList<const Contact *> RoutingTable::closestContacts(const KadId &target, int maxType, int count, bool verifiedOnly) const
{
    // the table is small (≤ a few thousand contacts): a full scan is simpler than eMule's recursion
    std::multimap<KadId, const Contact *> best;
    QList<Zone *> leaves;
    collectLeaves(m_root.get(), leaves);
    for (Zone *z : leaves) {
        for (const auto &c : z->bin) {
            if (c->type > maxType || (verifiedOnly && !c->ipVerified)) continue;
            best.emplace(c->id ^ target, c.get());
            if (int(best.size()) > count) best.erase(std::prev(best.end()));
        }
    }
    QList<const Contact *> out;
    for (const auto &kv : best) out.append(kv.second);
    return out;
}

QList<KadPeer> RoutingTable::closest(const KadId &target, int maxType, int count, bool verifiedOnly) const
{
    QList<KadPeer> out;
    for (const Contact *c : closestContacts(target, maxType, count, verifiedOnly)) out.append(c->peer());
    return out;
}

void RoutingTable::randomBin(const Zone *z, QList<const Contact *> &out) const
{
    if (z->leaf) { for (const auto &c : z->bin) out.append(c.get()); return; }
    randomBin(z->sub[QRandomGenerator::global()->bounded(2)].get(), out);
}

void RoutingTable::topDepth(const Zone *z, int depth, QList<const Contact *> &out) const
{
    if (z->leaf) { for (const auto &c : z->bin) out.append(c.get()); return; }
    if (depth <= 0) { randomBin(z, out); return; }
    topDepth(z->sub[0].get(), depth - 1, out);
    topDepth(z->sub[1].get(), depth - 1, out);
}

QList<KadPeer> RoutingTable::bootstrapContacts(int maxCount) const
{
    QList<const Contact *> top;
    topDepth(m_root.get(), LOG_BASE_EXPONENT, top);
    QList<KadPeer> out;
    for (const Contact *c : top) {
        out.append(c->peer());
        if (out.size() >= maxCount) break;
    }
    return out;
}

QList<const Contact *> RoutingTable::all() const
{
    QList<Zone *> leaves;
    collectLeaves(m_root.get(), leaves);
    QList<const Contact *> out;
    for (Zone *z : leaves) for (const auto &c : z->bin) out.append(c.get());
    return out;
}

const Contact *RoutingTable::randomContact(int maxType, int minVersion) const
{
    QList<const Contact *> fit;
    for (const Contact *c : all()) if (c->type <= maxType && c->version >= minVersion) fit.append(c);
    if (fit.isEmpty()) return nullptr;
    return fit[QRandomGenerator::global()->bounded(int(fit.size()))];
}

bool RoutingTable::isAcceptable(const KadPeer &p) const
{
    if (p.version <= 1) return false;
    if (const Contact *d = find(p.id)) return !(d->ipVerified && (d->ip != p.ip || d->udpPort != p.udpPort));
    return checkGlobalIpLimits(p.ip);
}

int RoutingTable::zoneCount() const
{
    QList<Zone *> leaves;
    collectLeaves(m_root.get(), leaves);
    return int(leaves.size());
}

int RoutingTable::maxDepth() const
{
    int best = 0;
    QList<Zone *> leaves;
    collectLeaves(m_root.get(), leaves);
    for (const Zone *z : leaves) best = qMax(best, z->level);
    return best;
}

int RoutingTable::zoneContacts(const Zone *z) const
{
    if (z->leaf) return int(z->bin.size());
    return zoneContacts(z->sub[0].get()) + zoneContacts(z->sub[1].get());
}

quint32 RoutingTable::estimateZone(const Zone *z) const
{
    if (!z->leaf) return 0;
    if (z->level < KBASE) return quint32(std::pow(2.0, z->level) * K);
    const Zone *cur = z->parent ? z->parent->parent ? z->parent->parent->parent : nullptr : nullptr;
    if (!cur) return 0;
    const double modify = double(zoneContacts(cur)) / double(K * 2);
    // eMule multiplies by a firewalled-users correction of 1.2 .. 1.9; we use its old default 1.2
    return quint32(std::pow(2.0, z->level - 2) * K * modify * 1.2);
}

quint32 RoutingTable::estimateUsers() const
{
    QList<Zone *> leaves;
    collectLeaves(m_root.get(), leaves);
    quint32 best = 0;
    for (Zone *z : leaves) best = qMax(best, estimateZone(z));
    return best;
}

void RoutingTable::onTimers(qint64 now, bool longNoContact, const std::function<void(const Contact &)> &sendHello,
                            const std::function<void(const KadId &)> &randomLookup)
{
    m_now = now;
    QList<Zone *> leaves;
    collectLeaves(m_root.get(), leaves);
    for (Zone *z : leaves) {
        if (m_bigTimer <= now && (z->nextBig <= now || longNoContact)) {
            if (indexBelow(z->index, KK) || z->level < KBASE || (K - int(z->bin.size())) >= int(K * 0.8)) {
                // random target inside this zone: prefix = zone index, then XOR with our ID
                const KadId prefix = z->index.shiftedLeft(128 - z->level);
                const KadId target = KadId::randomWithPrefix(prefix, z->level) ^ m_me;
                z->nextBig = now + 3600;
                m_bigTimer = now + 10;
                randomLookup(target);
            }
        }
        if (z->nextSmall <= now) {
            z->nextSmall = now + 60;
            // drop dead contacts
            for (size_t i = 0; i < z->bin.size();) {
                Contact *c = z->bin[i].get();
                if (c->type == 4 && c->expires > 0 && c->expires <= now) {
                    track(c->ip, -1);
                    z->bin.erase(z->bin.begin() + qsizetype(i));
                    continue;
                }
                if (c->expires == 0) c->expires = now;
                ++i;
            }
            if (!z->bin.empty()) {
                Contact *c = z->bin.front().get();
                if (c->expires >= now || c->type == 4) {
                    pushToBottom(z->bin, c);
                } else {
                    c->checkingType(now);
                    sendHello(*c);
                }
            }
        }
    }
    if (m_consolidate == 0) m_consolidate = now + 45 * 60;
    if (now >= m_consolidate) { m_consolidate = now + 45 * 60; consolidate(); }
}

int RoutingTable::consolidate(Zone *z)
{
    if (z->leaf) return 0;
    int merged = consolidate(z->sub[0].get()) + consolidate(z->sub[1].get());
    if (z->sub[0]->leaf && z->sub[1]->leaf && zoneContacts(z) < K / 2) {
        std::vector<std::unique_ptr<Contact>> entries;
        for (int s = 0; s < 2; ++s) {
            for (auto &c : z->sub[s]->bin) { track(c->ip, -1); entries.push_back(std::move(c)); }
            z->sub[s].reset();
        }
        z->leaf = true;
        z->bin.clear();
        for (auto &c : entries) binAdd(z, c);
        ++merged;
    }
    return merged;
}

int RoutingTable::consolidate() { return consolidate(m_root.get()); }

// ---------------------------------------------------------------- nodes.dat
bool parseNodesDat(const QByteArray &data, NodesDat *out, int maxEntries)
{
    Reader r(data);
    NodesDat nd;
    quint32 n = r.u32();
    int version = 0;
    if (!r.ok()) return false;
    if (n == 0) {
        if (data.size() < 8) return false;
        version = int(r.u32());
        if (version == 3) {
            const quint32 edition = r.u32();
            if (edition == 1) {
                nd.bootstrapOnly = true;
                n = r.u32();
                if (!r.ok() || n == 0 || qint64(n) * 25 != r.remaining()) return false;
                nd.version = 3;
                for (quint32 i = 0; i < n && int(nd.entries.size()) < maxEntries; ++i) {
                    NodesDatEntry e;
                    e.peer = readPeer(r);
                    nd.entries.append(e);
                }
                if (!r.ok()) return false;
                if (out) *out = nd;
                return true;
            }
        }
        if (version < 1 || version > 3) return false;
        n = r.u32();
    }
    if (!r.ok()) return false;
    const int rec = version >= 2 ? 34 : 25;
    if (n == 0 || qint64(n) * rec > r.remaining()) return false;
    nd.version = version;
    for (quint32 i = 0; i < n && int(nd.entries.size()) < maxEntries; ++i) {
        NodesDatEntry e;
        e.peer.id = KadId::fromWire(r.bytes(16));
        e.peer.ip = r.u32();
        e.peer.udpPort = r.u16();
        e.peer.tcpPort = r.u16();
        if (version >= 1) e.peer.version = r.u8();
        else e.type = r.u8();
        if (version >= 2) {
            e.udpKey = r.u32();
            e.udpKeyIp = r.u32();
            e.verified = r.u8() != 0;
        }
        if (!r.ok()) return false;
        nd.entries.append(e);
    }
    if (out) *out = nd;
    return true;
}

QByteArray writeNodesDat(const QList<NodesDatEntry> &entries)
{
    Writer w(12 + 34 * entries.size());
    w.u32(0).u32(2).u32(quint32(entries.size()));
    for (const NodesDatEntry &e : entries) {
        writePeer(w, e.peer);
        w.u32(e.udpKey).u32(e.udpKeyIp).u8(e.verified ? 1 : 0);
    }
    return w.take();
}

} // namespace mx::ed2k::kad
