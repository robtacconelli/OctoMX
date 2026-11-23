// SPDX-License-Identifier: GPL-3.0-or-later
// Kad routing table as in eMule: a binary tree of zones over the XOR distance to our KadID, each
// leaf holding a bin of at most K = 10 contacts. Leaves split when full only near our own ID
// (zone index < KK or level < KBASE), so the table keeps ~fine detail around us and a sample of
// the rest. Contacts age through types 0..4 (0 = long known and alive, 3 = new, 4 = about to be
// dropped) by HELLO probes on the per-zone small timer; the big timer runs random lookups in thin
// zones. Anti-sybil limits: one contact per IP, ≤ 10 per /24 globally, ≤ 2 per /24 in one bin
// (LAN addresses exempt from the subnet rules). Also: nodes.dat (v0/v1/v2/bootstrap) codec.
#pragma once

#include "kad_wire.h"

#include <QtCore/QHash>
#include <QtCore/QList>
#include <functional>
#include <memory>
#include <vector>

namespace mx::ed2k::kad {

struct Contact {
    KadId id;
    KadId distance;          // id ^ our id
    quint32 ip = 0;          // host order
    quint16 udpPort = 0, tcpPort = 0;
    quint8 version = 0;
    quint32 udpKey = 0;      // the sender verify key this contact gave us ...
    quint32 udpKeyIp = 0;    // ... valid while our public IP (eD2k order) is this one
    bool ipVerified = false;
    bool receivedHello = false;
    int type = 3;
    qint64 created = 0, lastTypeSet = 0, expires = 0;   // seconds (epoch)

    quint32 keyFor(quint32 ourEd2kIp) const { return (udpKeyIp && ourEd2kIp == udpKeyIp) ? udpKey : 0; }
    KadPeer peer() const { KadPeer p; p.id = id; p.ip = ip; p.udpPort = udpPort; p.tcpPort = tcpPort; p.version = version; return p; }
    void checkingType(qint64 now);
    void updateType(qint64 now);
};

struct RoutingOptions {
    bool allowLan = false;       // accept private / loopback addresses (LAN mode, tests)
    bool allowSameIp = false;    // test switch: several contacts per IP (loopback multi-instance tests)
    int maxContacts = 5000;      // hard cap over the whole table
};

class RoutingTable {
public:
    explicit RoutingTable(const KadId &me, const RoutingOptions &opt = {});
    ~RoutingTable();
    RoutingTable(const RoutingTable &) = delete;
    RoutingTable &operator=(const RoutingTable &) = delete;

    const KadId &me() const { return m_me; }
    const RoutingOptions &options() const { return m_opt; }

    // eMule CRoutingZone::Add semantics: returns true if the contact was added or (when `update`)
    // refreshed; `ipVerified` in/out. Updates of a contact that has a sender key require the same key.
    bool add(const KadPeer &p, quint32 udpKey, quint32 udpKeyIp, bool &ipVerified, bool update, bool fromHello,
             qint64 now, quint32 ourEd2kIp = 0);
    Contact *find(const KadId &id) const;
    Contact *findByIp(quint32 ip, quint16 port = 0, bool tcpPort = false) const;
    bool verify(const KadId &id, quint32 ip);
    void setAllVerified();
    void remove(const KadId &id);
    void clear();

    // closest verified contacts with type <= maxType, nearest first
    QList<KadPeer> closest(const KadId &target, int maxType, int count, bool verifiedOnly = true) const;
    QList<const Contact *> closestContacts(const KadId &target, int maxType, int count, bool verifiedOnly = true) const;
    QList<KadPeer> bootstrapContacts(int maxCount) const;      // TopDepth(LOG_BASE_EXPONENT) sample
    QList<const Contact *> all() const;
    const Contact *randomContact(int maxType, int minVersion) const;
    // KADEMLIA2_RES answers: would this contact pass our routing rules (duplicates allowed)?
    bool isAcceptable(const KadPeer &p) const;
    bool checkGlobalIpLimits(quint32 ip) const;

    int count() const { return m_count; }
    int zoneCount() const;      // leaves
    int maxDepth() const;
    quint32 estimateUsers() const;

    // Timers (called once per second): per-zone small timer (every 60 s: drop expired type-4
    // contacts, probe the oldest one with a HELLO) and the big timer (≤ 1 random lookup per 10 s
    // in a zone that needs contacts). Consolidate merges almost empty sibling leaves (every 45 min).
    void onTimers(qint64 now, bool longNoContact, const std::function<void(const Contact &)> &sendHello,
                  const std::function<void(const KadId &)> &randomLookup);
    int consolidate();

private:
    struct Zone;
    bool addToZone(Zone *z, std::unique_ptr<Contact> &c, bool &update, bool &outVerified, quint32 ourEd2kIp);
    bool binAdd(Zone *z, std::unique_ptr<Contact> &c);
    bool canSplit(const Zone *z) const;
    void split(Zone *z);
    Zone *leafFor(const KadId &distance) const;
    void track(quint32 ip, int delta);
    bool changeIp(Zone *z, Contact *c, quint32 newIp);
    void collectLeaves(Zone *z, QList<Zone *> &out) const;
    void topDepth(const Zone *z, int depth, QList<const Contact *> &out) const;
    void randomBin(const Zone *z, QList<const Contact *> &out) const;
    int consolidate(Zone *z);
    int zoneContacts(const Zone *z) const;
    quint32 estimateZone(const Zone *z) const;
    void initZone(Zone *z, qint64 now);

    KadId m_me;
    RoutingOptions m_opt;
    std::unique_ptr<Zone> m_root;
    QHash<quint32, int> m_ipCount;      // per IP
    QHash<quint32, int> m_subnetCount;  // per /24
    int m_count = 0;
    qint64 m_bigTimer = 0;
    qint64 m_consolidate = 0;
    qint64 m_now = 0;
};

// ---------------------------------------------------------------- nodes.dat
struct NodesDatEntry {
    KadPeer peer;
    quint8 type = 0;             // v0 files only
    quint32 udpKey = 0, udpKeyIp = 0;
    bool verified = false;
};
struct NodesDat {
    int version = -1;            // 0, 1, 2, or 3 (bootstrap edition)
    bool bootstrapOnly = false;  // v3 "bootstrap nodes.dat": contacts to bootstrap from, not to add
    QList<NodesDatEntry> entries;
};
bool parseNodesDat(const QByteArray &data, NodesDat *out, int maxEntries = 5000);
QByteArray writeNodesDat(const QList<NodesDatEntry> &entries);   // version 2

} // namespace mx::ed2k::kad
