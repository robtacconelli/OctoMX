// SPDX-License-Identifier: GPL-3.0-or-later
// Parts of this file are adapted from eMule 0.50a (Copyright (C) the eMule team,
// https://www.emule-project.net; kademlia/kademlia/Search.cpp, SearchManager.cpp), GPL-2.0-or-later;
// this file is distributed under GPL-3.0-or-later. See NOTICE.
#include "kad_search.h"

#include "../ed2k_opcodes.h"
#include "../ed2k_util.h"

#include <QtCore/QSet>

namespace mx::ed2k::kad {

const char *const kInvKadKeywordChars = " ()[]{}<>,._-!?:;\\/\"";

static bool isInvKadChar(QChar c)
{
    if (c.unicode() > 0x7F) return false;
    for (const char *p = kInvKadKeywordChars; *p; ++p) if (c == QLatin1Char(*p)) return true;
    return false;
}

QString kadLower(const QString &s)
{
    QString r = s;
    for (QChar &c : r) c = c.toLower();
    return r;
}

QStringList kadGetWords(const QString &text)
{
    QStringList words;
    int lastChars = 0, lastBytes = 0;
    QString seg;
    auto flush = [&] {
        lastChars = int(seg.size());
        lastBytes = int(seg.toUtf8().size());
        if (lastBytes >= 3) {
            const QString w = kadLower(seg);
            words.removeAll(w);
            words.append(w);
        }
        seg.clear();
    };
    for (QChar c : text) {
        if (isInvKadChar(c)) flush();
        else seg.append(c);
    }
    flush();
    if (words.size() > 1 && lastChars == 3 && lastBytes == 3) words.removeLast();
    return words;
}

KadId kadKeywordHash(const QString &word)
{
    return KadId::fromHash(md4(word.toUtf8()));
}

// ---------------------------------------------------------------- query planning
static void flatten(const SearchNodePtr &n, SearchNode::Kind k, QList<SearchNodePtr> &out)
{
    if (!n) return;
    if (n->kind == k) { flatten(n->a, k, out); flatten(n->b, k, out); }
    else out.append(n);
}

// positive keyword nodes of the top-level AND chain (NOT: only its left side)
static void positiveKeywords(const SearchNodePtr &n, QList<SearchNodePtr> &out, bool *pure)
{
    QList<SearchNodePtr> ops;
    flatten(n, SearchNode::And, ops);
    for (const SearchNodePtr &o : ops) {
        if (o->kind == SearchNode::Keyword) out.append(o);
        else if (o->kind == SearchNode::Not) { *pure = false; positiveKeywords(o->a, out, pure); }
        else *pure = false;
    }
}

static QStringList tokens(const QString &text)
{
    return kadLower(text).split(QLatin1Char(' '), Qt::SkipEmptyParts);
}

KadQueryPlan planKadSearch(const mx::SearchQuery &q, int maxLookups)
{
    KadQueryPlan plan;
    QString text = q.text.trimmed();
    if (!q.text2.trimmed().isEmpty())
        text = text.isEmpty() ? q.text2.trimmed() : QStringLiteral("(%1) (%2)").arg(text, q.text2.trimmed());
    QString err;
    plan.expr = parseSearchExpression(text, &err);
    if (!plan.expr) { plan.error = err; return plan; }
    QList<SearchNodePtr> branches;
    flatten(plan.expr, SearchNode::Or, branches);
    SearchEncodeOptions opt;
    QSet<KadId> used;
    auto addLookup = [&](const QString &word, const SearchNodePtr &treeExpr) {
        const KadId t = kadKeywordHash(word);
        if (used.contains(t) || plan.lookups.size() >= maxLookups) return;
        used.insert(t);
        KadLookupPlan lp;
        lp.keyword = word;
        lp.target = t;
        lp.tree = encodeSearchTree(treeExpr, q, opt, false, false);
        plan.lookups.append(lp);
    };
    for (const SearchNodePtr &b : branches) {
        bool pure = true;
        QList<SearchNodePtr> kws;
        positiveKeywords(b, kws, &pure);
        // candidate words: each space-separated token split by eMule's rules (no extension rule
        // across query words: "abc mp3" keeps "mp3")
        QStringList all;
        QStringList toks;
        for (const SearchNodePtr &k : kws) {
            for (const QString &t : tokens(k->text)) {
                toks.append(t);
                QStringList w = kadGetWords(t + QLatin1Char(' '));   // trailing delimiter: never "extension"
                for (const QString &x : w) if (!all.contains(x)) all.append(x);
            }
        }
        if (all.isEmpty()) continue;
        QStringList byLen = all;
        std::stable_sort(byLen.begin(), byLen.end(), [](const QString &a, const QString &c) {
            return a.toUtf8().size() > c.toUtf8().size();
        });
        const int nTargets = (pure && branches.size() == 1) ? qMin<int>(2, int(byLen.size())) : 1;
        for (int i = 0; i < nTargets; ++i) {
            const QString word = byLen[i];
            SearchNodePtr treeExpr;
            if (pure) {
                QStringList rest = toks;
                rest.removeOne(word);
                if (!rest.isEmpty()) {
                    treeExpr = std::make_shared<SearchNode>();
                    treeExpr->kind = SearchNode::Keyword;
                    treeExpr->text = rest.join(QLatin1Char(' '));
                }
            } else {
                treeExpr = b;
            }
            addLookup(word, treeExpr);
        }
    }
    if (plan.lookups.isEmpty()) plan.error = QStringLiteral("no keyword of at least 3 characters");
    return plan;
}

static bool evalNode(const SearchNodePtr &n, const QString &lname)
{
    if (!n) return true;
    switch (n->kind) {
    case SearchNode::Keyword: {
        QString t = kadLower(n->text);
        for (QChar &c : t) if (isInvKadChar(c)) c = QLatin1Char(' ');
        const QStringList parts = t.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        for (const QString &p : parts) if (!lname.contains(p)) return false;
        return true;
    }
    case SearchNode::And: return evalNode(n->a, lname) && evalNode(n->b, lname);
    case SearchNode::Or: return evalNode(n->a, lname) || evalNode(n->b, lname);
    case SearchNode::Not: return evalNode(n->a, lname) && !evalNode(n->b, lname);
    }
    return true;
}

bool kadResultMatches(const mx::SearchQuery &q, const SearchNodePtr &expr, const QString &fileName, qint64 size,
                      const QString &typeText, int bitrateKbps)
{
    if (fileName.isEmpty() || size <= 0) return false;
    if (!evalNode(expr, kadLower(fileName))) return false;
    if (q.minSize > 0 && size < q.minSize) return false;
    if (q.maxSize > 0 && size > q.maxSize) return false;
    // [ui-f] type, extensions, bitrate: the rule of the core and every network (the name's
    // extension decides; Kad's "Pro" covers software and archives alike)
    return mx::fileNameMatchesFileType(q, fileName, mediaTypeFromEd2k(typeText, fileName), bitrateKbps);
}

// ---------------------------------------------------------------- lookups
QString lookupTypeName(LookupType t)
{
    switch (t) {
    case LookupType::Node: return QStringLiteral("node");
    case LookupType::NodeComplete: return QStringLiteral("self");
    case LookupType::File: return QStringLiteral("sources");
    case LookupType::Keyword: return QStringLiteral("keyword");
    case LookupType::StoreFile: return QStringLiteral("publish-source");
    case LookupType::StoreKeyword: return QStringLiteral("publish-keyword");
    case LookupType::FindBuddy: return QStringLiteral("find-buddy");
    case LookupType::FindSource: return QStringLiteral("find-source");
    case LookupType::NodeFwCheckUdp: return QStringLiteral("fwcheck-udp");
    }
    return QString();
}

Lookup::Lookup(LookupType type, const KadId &target, const KadId &me, qint64 now, quint32 id)
    : m_type(type), m_target(target), m_me(me), m_id(id), m_created(now), m_lastResponse(now)
{
}

quint8 Lookup::requestCount() const
{
    switch (m_type) {
    case LookupType::Node: case LookupType::NodeComplete: case LookupType::NodeFwCheckUdp:
        return KADEMLIA_FIND_NODE;
    case LookupType::File: case LookupType::Keyword: case LookupType::FindSource:
        return KADEMLIA_FIND_VALUE;
    case LookupType::FindBuddy: case LookupType::StoreFile: case LookupType::StoreKeyword:
        return KADEMLIA_STORE;
    }
    return KADEMLIA_FIND_NODE;
}

int Lookup::lifetime() const
{
    switch (m_type) {
    case LookupType::StoreFile: case LookupType::StoreKeyword: return 140;
    case LookupType::FindBuddy: return 100;
    default: return 45;
    }
}

int Lookup::total() const
{
    switch (m_type) {
    case LookupType::File: case LookupType::Keyword: return 300;
    case LookupType::StoreFile: case LookupType::StoreKeyword: case LookupType::NodeComplete: case LookupType::FindBuddy: return 10;
    case LookupType::FindSource: return 20;
    default: return 1 << 30;
    }
}

void Lookup::start(LookupHost *host, const QList<KadPeer> &closest)
{
    for (const KadPeer &p : closest) m_possible.emplace(p.id ^ m_target, p);
    if (m_possible.empty()) return;
    const int n = m_type == LookupType::Node ? 1 : qMin<int>(ALPHA_QUERY, int(m_possible.size()));
    auto it = m_possible.begin();
    for (int i = 0; i < n && it != m_possible.end(); ++i, ++it) {
        m_tried[it->first] = it->second;
        sendRequest(host, it->second);
    }
}

void Lookup::sendRequest(LookupHost *host, const KadPeer &p, bool reAskMore)
{
    if (m_stopping) return;
    quint8 count = requestCount();
    if (reAskMore && !m_askedMore) {
        m_askedMore = true;
        m_askedMoreId = p.id;
        count = KADEMLIA_FIND_VALUE_MORE;
    }
    ++m_requests;
    host->lookupSendRequest(*this, p, count);
}

bool Lookup::triedPeer(quint32 ip, quint16 port) const { return triedPeerByIp(ip, port) != nullptr; }

const KadPeer *Lookup::triedPeerByIp(quint32 ip, quint16 port) const
{
    for (const auto &kv : m_tried) if (kv.second.ip == ip && kv.second.udpPort == port) return &kv.second;
    return nullptr;
}

bool Lookup::processResponse(LookupHost *host, quint32 ip, quint16 port, const QList<KadPeer> &contacts)
{
    const KadPeer *from = triedPeerByIp(ip, port);
    if (!from) return false;
    const KadId fromDist = from->id ^ m_target;
    const KadId fromId = from->id;
    const quint32 fromIp = from->ip;
    if (contacts.size() > requestCount() && !(m_askedMore && fromId == m_askedMoreId && contacts.size() <= KADEMLIA_FIND_VALUE_MORE))
        return false;
    m_lastResponseSeen = true;
    if (m_type == LookupType::NodeFwCheckUdp) { ++m_answers; return true; }
    if (m_type == LookupType::Node) {
        ++m_answers;
        m_possible.clear();
        return true;
    }
    bool closer = false;
    QSet<quint32> ips{fromIp};
    QHash<quint32, int> subnets{{fromIp & 0xFFFFFF00u, 1}};
    for (const KadPeer &c : contacts) {
        const KadId d = c.id ^ m_target;
        if (d < fromDist) closer = true;
        if (m_possible.count(d) || m_tried.count(d)) continue;
        if (ips.contains(c.ip)) continue;
        ips.insert(c.ip);
        const quint32 sn = c.ip & 0xFFFFFF00u;
        if (subnets.contains(sn) && !isLanIp(c.ip)) {
            if (subnets.value(sn) >= 2) continue;
            subnets[sn] += 1;
        } else {
            subnets[sn] = 1;
        }
        m_possible[d] = c;
        if (d < fromDist) {
            bool top = false;
            if (int(m_best.size()) < ALPHA_QUERY) {
                top = true;
                m_best[d] = c;
            } else {
                auto last = std::prev(m_best.end());
                if (d < last->first) { m_best.erase(last); m_best[d] = c; top = true; }
            }
            if (top) {
                m_tried[d] = c;
                sendRequest(host, c);
            }
        }
    }
    m_responded[fromDist] = closer;
    if (m_type == LookupType::NodeComplete) ++m_answers;
    return true;
}

void Lookup::storePacket(LookupHost *host)
{
    if (m_possible.empty()) return;
    const KadId fromDist = m_possible.begin()->first;
    const KadPeer p = m_possible.begin()->second;
    if (!m_hasClosest || fromDist < m_closestDistance) { m_closestDistance = fromDist; m_hasClosest = true; }
    if (fromDist.chunk(0) > host->lookupTolerance() && !(host->lookupLanAllowed() && isLanIp(p.ip))) return;
    host->lookupReached(*this, p);
}

void Lookup::jumpStart(LookupHost *host, qint64 now)
{
    if (m_lastResponseSeen) { m_lastResponse = now; m_lastResponseSeen = false; }
    if (m_lastResponse + 3 > now) return;
    if (m_possible.empty()) { prepareToStop(now); return; }
    // the best two FIND_VALUE candidates are dead: ask the closest responder for more contacts
    if (!m_askedMore && requestCount() == KADEMLIA_FIND_VALUE && m_tried.size() >= 3 * KADEMLIA_FIND_VALUE) {
        auto it = m_tried.begin();
        bool lookCloser = true;
        for (int i = 0; i < KADEMLIA_FIND_VALUE && it != m_tried.end(); ++i, ++it)
            if (m_responded.count(it->first)) { lookCloser = false; break; }
        if (lookCloser) {
            for (; it != m_tried.end(); ++it) {
                if (m_responded.count(it->first)) { sendRequest(host, it->second, true); return; }
            }
        }
    }
    while (!m_possible.empty()) {
        const auto front = m_possible.begin();
        if (m_tried.count(front->first)) {
            if (m_responded.count(front->first)) storePacket(host);
            if (m_possible.empty()) break;
            m_possible.erase(m_possible.begin());
        } else {
            m_tried[front->first] = front->second;
            sendRequest(host, front->second);
            break;
        }
    }
}

void Lookup::prepareToStop(qint64 now)
{
    if (m_stopping) return;
    m_created = now - lifetime() + 15;
    m_stopping = true;
}

int Lookup::maintain(LookupHost *host, qint64 now)
{
    const int life = lifetime();
    switch (m_type) {
    case LookupType::Node:
    case LookupType::NodeFwCheckUdp:
        if (m_created + life < now) return 2;
        jumpStart(host, now);
        return 0;
    case LookupType::NodeComplete:
        if (m_created + life < now) return 2;
        if (m_created + 10 < now && m_answers >= 10) return 2;
        jumpStart(host, now);
        return 0;
    default:
        if (m_created + life < now) return 2;
        if (m_answers >= total() || m_created + life - 20 < now) { prepareToStop(now); return 1; }
        jumpStart(host, now);
        return 0;
    }
}

} // namespace mx::ed2k::kad
