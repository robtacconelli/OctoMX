// SPDX-License-Identifier: GPL-3.0-or-later
// Kad keyword handling (eMule CSearchManager::GetWords rules, keyword hash), query planning for
// mx::SearchQuery (target keyword + restrictive search tree, several lookups for OR / multi-word
// queries), local result matching, and the iterative lookup state machine (eMule CSearch:
// alpha = 3 parallel requests, closest-set tracking, jump-start of stalled lookups, per-type
// lifetimes and answer totals, the "best two dead → ask the closest responder for more" rule).
#pragma once

#include "kad_wire.h"

#include "../ed2k_search.h"
#include "mx/types.h"

#include <QtCore/QStringList>
#include <map>

namespace mx::ed2k::kad {

// ---------------------------------------------------------------- keywords
extern const char *const kInvKadKeywordChars;   // " ()[]{}<>,._-!?:;\\/\""
QString kadLower(const QString &s);              // per UTF-16 unit, locale independent
// eMule GetWords: split at the invalid characters, keep words of ≥ 3 UTF-8 bytes, lower case,
// de-duplicated (a repeated word moves to the end), and a trailing 3-character word (the
// extension) is dropped when there is more than one word.
QStringList kadGetWords(const QString &text);
KadId kadKeywordHash(const QString &word);       // MD4(UTF-8 word), big-endian into a KadId

// ---------------------------------------------------------------- query planning
struct KadLookupPlan {
    QString keyword;
    KadId target;
    QByteArray tree;          // restrictive search tree for SEARCH_KEY_REQ (empty = none)
};
struct KadQueryPlan {
    QList<KadLookupPlan> lookups;
    SearchNodePtr expr;       // full expression for local filtering
    QString error;
};
KadQueryPlan planKadSearch(const mx::SearchQuery &q, int maxLookups = 3);

// Does a result (name, size, ed2k type text) satisfy the whole query?
bool kadResultMatches(const mx::SearchQuery &q, const SearchNodePtr &expr, const QString &fileName, qint64 size,
                      const QString &typeText, int bitrateKbps);

// ---------------------------------------------------------------- lookups
enum class LookupType { Node, NodeComplete, File, Keyword, StoreFile, StoreKeyword, FindBuddy, FindSource, NodeFwCheckUdp };
QString lookupTypeName(LookupType t);

class Lookup;
class LookupHost {
public:
    virtual ~LookupHost() = default;
    // send KADEMLIA2_REQ asking `peer` for `count` contacts close to the lookup target
    virtual void lookupSendRequest(Lookup &l, const KadPeer &peer, quint8 count) = 0;
    // the lookup reached a responding node close to the target: issue the actual search / store
    virtual void lookupReached(Lookup &l, const KadPeer &peer) = 0;
    virtual bool lookupLanAllowed() const = 0;
    // a store/search is only sent to nodes within this distance (first 32 bits; eMule 2^24)
    virtual quint32 lookupTolerance() const { return SEARCHTOLERANCE; }
};

class Lookup {
public:
    Lookup(LookupType type, const KadId &target, const KadId &me, qint64 nowSecs, quint32 id);

    LookupType type() const { return m_type; }
    const KadId &target() const { return m_target; }
    quint32 id() const { return m_id; }
    quint8 requestCount() const;           // contacts asked for per KADEMLIA2_REQ
    int answers() const { return m_answers; }
    void addAnswers(int n = 1) { m_answers += n; }
    int requestsSent() const { return m_requests; }
    int responses() const { return int(m_responded.size()); }
    bool stopping() const { return m_stopping; }
    qint64 created() const { return m_created; }
    int lifetime() const;                  // seconds
    int total() const;                     // answers that end the lookup
    KadId closestReachedDistance() const { return m_closestDistance; }
    bool hasClosestReached() const { return m_hasClosest; }

    // free-form data used by the node
    quint32 queryId = 0;                    // keyword search: mx::SearchQuery id
    QString downloadId;                     // file search
    QByteArray searchTree;                  // keyword search
    QList<KadId> fileIds;                   // store keyword (≤ 150) / find source (1)
    qint64 fileSize = 0;
    QString name;
    int load = 0, loadResponses = 0;

    void start(LookupHost *host, const QList<KadPeer> &closest);
    // a KADEMLIA2_RES from ip:port (host order). Returns false if it was not expected / too large.
    bool processResponse(LookupHost *host, quint32 ip, quint16 port, const QList<KadPeer> &contacts);
    // once per second: stalled → ask the next candidate; responded candidates → lookupReached()
    void jumpStart(LookupHost *host, qint64 nowSecs);
    void prepareToStop(qint64 nowSecs);
    // eMule SearchManager::JumpStart rules: 0 = keep going, 1 = stop requested, 2 = delete now
    int maintain(LookupHost *host, qint64 nowSecs);
    bool triedPeer(quint32 ip, quint16 port) const;
    const KadPeer *triedPeerByIp(quint32 ip, quint16 port) const;

private:
    void sendRequest(LookupHost *host, const KadPeer &p, bool reAskMore = false);
    void storePacket(LookupHost *host);

    LookupType m_type;
    KadId m_target, m_me;
    quint32 m_id;
    qint64 m_created;
    qint64 m_lastResponse;
    bool m_lastResponseSeen = false;   // a response arrived since the last jumpStart()
    bool m_stopping = false;
    int m_answers = 0;
    int m_requests = 0;
    std::map<KadId, KadPeer> m_possible, m_tried, m_best;
    std::map<KadId, bool> m_responded;
    bool m_askedMore = false;
    KadId m_askedMoreId;
    KadId m_closestDistance;
    bool m_hasClosest = false;
};

} // namespace mx::ed2k::kad
