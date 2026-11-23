// SPDX-License-Identifier: GPL-3.0-or-later
#include "kad_index.h"

#include "kad_search.h"

#include "../ed2k_opcodes.h"

#include <memory>

namespace mx::ed2k::kad {

static constexpr qint64 KEYWORD_LIFETIME = 24 * 3600;
static constexpr qint64 SOURCE_LIFETIME = 5 * 3600;

double KadIndex::trust(const KeyEntry &e) const
{
    double t = 0;
    for (auto it = e.publishers.cbegin(); it != e.publishers.cend(); ++it) {
        const int n = m_publishSubnets.value(it.key() & 0xFFFFFF00u);
        t += n > 0 ? 10.0 / n : 10.0;
    }
    return t;
}

bool KadIndex::addKeyword(const KadId &keyword, const KadId &file, quint32 ip, const TagList &tags, qint64 now, quint8 *load)
{
    *load = 0;
    KeyEntry e;
    e.file = file;
    for (const Tag &t : tags) {
        if (t.is(quint8(TAG_FILENAME)) && t.isStr()) { if (e.name.isEmpty()) e.name = t.str.left(512); }
        else if (t.is(quint8(TAG_FILESIZE))) { if (!e.size) { TagList one{t}; e.size = kadTagFileSize(one); } }
        else if (t.is(quint8(TAG_KADAICHHASHPUB))) { if (t.blob.size() == 20 && e.aich.isEmpty()) e.aich = t.blob; }
        else if (t.is(quint8(TAG_PUBLISHINFO)) || t.is(quint8(TAG_KADAICHHASHRESULT))) continue;   // only we may add these
        else if (e.tags.size() < 16) e.tags.append(t);
    }
    if (e.name.trimmed().isEmpty() || e.size == 0 || e.size > quint64(MAX_FILE_SIZE)) return false;
    if (m_keywordTotal >= m_lim.maxKeywordEntries) { *load = 100; return true; }
    auto &files = m_keywords[keyword];
    if (!files.contains(file) && files.size() >= m_lim.maxPerKeyword) { *load = 100; return true; }
    const bool existed = files.contains(file);
    KeyEntry &cur = files[file];
    if (!existed) {
        cur = std::move(e);
        ++m_keywordTotal;
    } else {
        if (cur.size == e.size) { cur.tags = e.tags; if (!e.name.isEmpty()) cur.name = e.name; }
        if (cur.aich.isEmpty()) cur.aich = e.aich;
    }
    if (!cur.publishers.contains(ip)) m_publishSubnets[ip & 0xFFFFFF00u] += 1;
    cur.publishers.insert(ip, now);
    if (cur.publishers.size() > 64) {   // bounded: forget the oldest publisher
        auto oldest = cur.publishers.begin();
        for (auto it = cur.publishers.begin(); it != cur.publishers.end(); ++it) if (it.value() < oldest.value()) oldest = it;
        m_publishSubnets[oldest.key() & 0xFFFFFF00u] -= 1;
        cur.publishers.erase(oldest);
    }
    if (cur.names.size() < 16 || cur.names.contains(e.name)) cur.names[e.name] += 1;
    if (!e.aich.isEmpty() && e.aich == cur.aich) cur.aichVotes = qMin(cur.aichVotes + 1, 255);
    cur.expires = now + KEYWORD_LIFETIME;
    *load = quint8(qMin<qint64>(100, qint64(files.size()) * 100 / qMax(1, m_lim.maxPerKeyword)));
    return true;
}

bool KadIndex::addSource(const KadId &file, const KadId &source, quint32 ip, quint16 udpPort, const TagList &tags,
                         qint64 now, quint8 *load)
{
    *load = 0;
    SourceEntry e;
    e.source = source;
    e.ip = ip;
    e.udpPort = udpPort;
    bool haveType = false, haveUdp = false;
    for (const Tag &t : tags) {
        if (t.is(quint8(TAG_SOURCETYPE))) { if (haveType) continue; haveType = true; e.tags << Tag::makeInt(TAG_SOURCEIP, ip) << t; }
        else if (t.is(quint8(TAG_FILESIZE))) { if (!e.size) { TagList one{t}; e.size = kadTagFileSize(one); } }
        else if (t.is(quint8(TAG_SOURCEPORT))) { if (!e.tcpPort && t.isInt()) { e.tcpPort = quint16(t.num); e.tags << t; } }
        else if (t.is(quint8(TAG_SOURCEUPORT))) { if (!haveUdp && t.isInt() && t.num) { haveUdp = true; e.udpPort = quint16(t.num); e.tags << t; } }
        else if (t.is(quint8(TAG_SOURCEIP))) continue;
        else if (e.tags.size() < 16) e.tags << t;
    }
    if (!haveUdp) e.tags << Tag::makeInt(TAG_SOURCEUPORT, e.udpPort);
    if (!haveType || !e.ip || !e.tcpPort || !e.udpPort) return false;
    e.expires = now + SOURCE_LIFETIME;
    if (!m_sources.contains(file) && m_sources.size() >= m_lim.maxSourceKeys) { *load = 100; return true; }
    QList<SourceEntry> &list = m_sources[file];
    for (SourceEntry &s : list) {
        if (s.ip == e.ip && (s.tcpPort == e.tcpPort || s.udpPort == e.udpPort)) {
            s = std::move(e);
            *load = quint8(qMin<qint64>(100, qint64(list.size()) * 100 / qMax(1, m_lim.maxSourcesPerFile)));
            return true;
        }
    }
    if (list.size() >= m_lim.maxSourcesPerFile) {
        list.removeLast();
        list.prepend(std::move(e));
        *load = 100;
        return true;
    }
    list.prepend(std::move(e));
    ++m_sourceTotal;
    *load = quint8(qMin<qint64>(100, qint64(list.size()) * 100 / qMax(1, m_lim.maxSourcesPerFile)));
    return true;
}

// ---------------------------------------------------------------- search tree evaluation
namespace {
struct Term {
    enum Kind { And, Or, Not, String, Meta, Num } kind = String;
    std::unique_ptr<Term> l, r;
    QStringList words;
    QString value;
    QByteArray tagName;
    quint64 num = 0;
    quint8 op = 0;
};

std::unique_ptr<Term> parseTerm(Reader &r, int level)
{
    if (level >= 24 || !r.ok()) return nullptr;
    auto t = std::make_unique<Term>();
    const quint8 op = r.u8();
    if (!r.ok()) return nullptr;
    if (op == 0x00) {
        const quint8 b = r.u8();
        if (b > 2) return nullptr;
        t->kind = b == 0 ? Term::And : b == 1 ? Term::Or : Term::Not;
        t->l = parseTerm(r, level + 1);
        if (!t->l) return nullptr;
        t->r = parseTerm(r, level + 1);
        if (!t->r) return nullptr;
        return t;
    }
    if (op == 0x01) {
        t->kind = Term::String;
        QString s = kadLower(r.str16(1024));
        // tokenise like eMule: quoted parts stay together, the rest splits at the invalid characters
        QString cur;
        bool inQuote = false;
        for (QChar c : s) {
            if (c == QLatin1Char('"')) { if (!cur.isEmpty()) t->words << cur; cur.clear(); inQuote = !inQuote; continue; }
            if (!inQuote && QByteArray(kInvKadKeywordChars).contains(char(c.unicode() < 0x80 ? c.unicode() : 1))) {
                if (!cur.isEmpty()) t->words << cur;
                cur.clear();
                continue;
            }
            cur.append(c);
        }
        if (!cur.isEmpty()) t->words << cur;
        return r.ok() ? std::move(t) : nullptr;
    }
    if (op == 0x02) {
        t->kind = Term::Meta;
        t->value = kadLower(r.str16(1024));
        t->tagName = r.raw16(64);
        return r.ok() ? std::move(t) : nullptr;
    }
    if (op == 0x03 || op == 0x08) {
        t->kind = Term::Num;
        t->num = op == 0x03 ? r.u32() : r.u64();
        t->op = r.u8();
        t->tagName = r.raw16(64);
        if (!r.ok() || t->op > 5) return nullptr;
        return t;
    }
    return nullptr;
}

const Tag *tagByName(const TagList &tags, const QByteArray &name)
{
    for (const Tag &t : tags) {
        if (name.size() == 1 && t.name.isEmpty() && t.id == quint8(name[0])) return &t;
        if (name.size() > 1 && t.name == name) return &t;
    }
    return nullptr;
}

bool evalTerm(const Term *t, const QString &lname, const TagList &tags)
{
    switch (t->kind) {
    case Term::And: return evalTerm(t->l.get(), lname, tags) && evalTerm(t->r.get(), lname, tags);
    case Term::Or: return evalTerm(t->l.get(), lname, tags) || evalTerm(t->r.get(), lname, tags);
    case Term::Not: return evalTerm(t->l.get(), lname, tags) && !evalTerm(t->r.get(), lname, tags);
    case Term::String:
        if (t->words.isEmpty()) return false;
        for (const QString &w : t->words) if (!lname.contains(w)) return false;
        return true;
    case Term::Meta: {
        if (t->tagName.size() == 1 && quint8(t->tagName[0]) == TAG_FILEFORMAT) return lname.endsWith(QLatin1Char('.') + t->value);
        const Tag *tg = tagByName(tags, t->tagName);
        return tg && tg->isStr() && kadLower(tg->str) == t->value;
    }
    case Term::Num: {
        quint64 v = 0;
        if (t->tagName.size() == 1 && quint8(t->tagName[0]) == TAG_FILESIZE) v = kadTagFileSize(tags);
        else { const Tag *tg = tagByName(tags, t->tagName); if (!tg || !tg->isInt()) return false; v = tg->num; }
        switch (t->op) {
        case 0: return v == t->num;
        case 1: return v > t->num;
        case 2: return v < t->num;
        case 3: return v >= t->num;
        case 4: return v <= t->num;
        default: return v != t->num;
        }
    }
    }
    return false;
}
} // namespace

bool KadIndex::treeMatches(const QByteArray &tree, const QString &fileName, const TagList &tags, bool *valid)
{
    Reader r(tree);
    auto t = parseTerm(r, 0);
    if (valid) *valid = t != nullptr;
    if (!t) return false;
    return evalTerm(t.get(), kadLower(fileName), tags);
}

QList<SearchResEntry> KadIndex::keywordResults(const KadId &keyword, const QByteArray &tree, int start, int max, qint64 now)
{
    QList<SearchResEntry> out;
    auto it = m_keywords.find(keyword);
    if (it == m_keywords.end()) return out;
    std::unique_ptr<Term> term;
    if (!tree.isEmpty()) {
        Reader r(tree);
        term = parseTerm(r, 0);
        if (!term) return out;
    }
    int skip = start;
    // two passes as in eMule: trusted (≥ 1.0) entries first, then the rest
    for (int pass = 0; pass < 2 && out.size() < max; ++pass) {
        for (auto e = it->cbegin(); e != it->cend() && out.size() < max; ++e) {
            if (e->expires < now) continue;
            const double tv = trust(*e);
            if ((pass == 0) != (tv >= 1.0)) continue;
            TagList all = e->tags;
            all.prepend(e->size > 0xFFFFFFFFull ? [&] { Tag t; t.type = TAGTYPE_BSOB; t.id = TAG_FILESIZE; Writer w; w.u64(e->size); t.blob = w.take(); return t; }()
                                                : Tag::makeInt(TAG_FILESIZE, e->size));
            all.prepend(Tag::makeStr(TAG_FILENAME, e->name));
            if (term && !evalTerm(term.get(), kadLower(e->name), all)) continue;
            if (skip > 0) { --skip; continue; }
            const quint32 info = (quint32(qMin<int>(int(e->names.size()), 255)) << 24) | (quint32(qMin<int>(int(e->publishers.size()), 255)) << 16)
                                 | quint32(qMin(65535.0, tv * 100.0));
            all << Tag::makeInt(TAG_PUBLISHINFO, info);
            if (!e->aich.isEmpty()) {
                Tag a;
                a.type = TAGTYPE_BSOB;
                a.id = TAG_KADAICHHASHRESULT;
                Writer w;
                w.u8(1).u8(quint8(qMax(1, e->aichVotes))).bytes(e->aich);
                a.blob = w.take();
                all << a;
            }
            out.append({e->file, all});
        }
    }
    return out;
}

QList<SearchResEntry> KadIndex::sourceResults(const KadId &file, quint64 size, int start, int max, qint64 now)
{
    QList<SearchResEntry> out;
    auto it = m_sources.find(file);
    if (it == m_sources.end()) return out;
    int skip = start;
    for (const SourceEntry &s : *it) {
        if (out.size() >= max) break;
        if (s.expires < now) continue;
        if (size && s.size && s.size != size) continue;
        if (skip > 0) { --skip; continue; }
        out.append({s.source, s.tags});
    }
    return out;
}

void KadIndex::clean(qint64 now)
{
    if (now - m_lastClean < 60) return;
    m_lastClean = now;
    for (auto k = m_keywords.begin(); k != m_keywords.end();) {
        for (auto e = k->begin(); e != k->end();) {
            if (e->expires < now) {
                for (auto p = e->publishers.cbegin(); p != e->publishers.cend(); ++p) m_publishSubnets[p.key() & 0xFFFFFF00u] -= 1;
                e = k->erase(e);
                --m_keywordTotal;
            } else {
                ++e;
            }
        }
        if (k->isEmpty()) k = m_keywords.erase(k); else ++k;
    }
    for (auto it = m_publishSubnets.begin(); it != m_publishSubnets.end();) {
        if (it.value() <= 0) it = m_publishSubnets.erase(it); else ++it;
    }
    for (auto s = m_sources.begin(); s != m_sources.end();) {
        const qsizetype before = s->size();
        s->erase(std::remove_if(s->begin(), s->end(), [now](const SourceEntry &e) { return e.expires < now; }), s->end());
        m_sourceTotal -= int(before - s->size());
        if (s->isEmpty()) s = m_sources.erase(s); else ++s;
    }
}

} // namespace mx::ed2k::kad
