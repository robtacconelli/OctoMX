// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/search_merge.h"

#include <QtCore/QFileInfo>
#include <QtCore/QSet>

#include <algorithm>
#include <functional>

namespace mx {

namespace {
QString baseName(const QString &path)
{
    const int slash = qMax(path.lastIndexOf(QLatin1Char('/')), path.lastIndexOf(QLatin1Char('\\')));
    return slash >= 0 ? path.mid(slash + 1) : path;
}

bool knownExtension(const QString &ext)
{
    return !ext.isEmpty() && mediaTypeForFileName(QStringLiteral("x.") + ext) != MediaType::Other;
}

QStringList splitWords(const QString &s)
{
    QStringList out;
    QString cur;
    for (const QChar c : s) {
        if (c.isLetterOrNumber()) {
            cur += c.toLower();
        } else if (!cur.isEmpty()) {
            out << cur;
            cur.clear();
        }
    }
    if (!cur.isEmpty()) out << cur;
    return out;
}

bool sameWholeTorrent(const FileHashes &a, const FileHashes &b)
{
    return !a.btih.isEmpty() && a.btih == b.btih && a.btFileIndex < 0 && b.btFileIndex < 0;
}

int filesCountOf(const SearchHit &h)
{
    int n = 0;
    for (const SearchSource &s : h.sources) n = qMax(n, s.extra.value(QStringLiteral("files")).toInt());
    return n;
}

bool listComplete(const QList<BundleFile> &files)
{
    if (files.isEmpty()) return false;
    for (const BundleFile &f : files) if (f.index < 0) return false;
    return true;
}
} // namespace

// ============================================================================ the name rule

QString SearchMerger::normalizedName(const QString &fileName)
{
    QString name = baseName(fileName).toLower();
    QString ext;
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    if (dot > 0 && name.size() - dot <= 6) {
        ext = name.mid(dot + 1);
        name = name.left(dot);
    }
    QString out;
    out.reserve(name.size());
    bool space = false;
    for (const QChar c : std::as_const(name)) {
        if (c.isLetterOrNumber()) {
            if (space && !out.isEmpty()) out += QLatin1Char(' ');
            out += c;
            space = false;
        } else {
            space = true;
        }
    }
    if (!ext.isEmpty()) out += QLatin1Char('.') + ext;
    return out;
}

QString SearchMerger::probableExtension(const QString &name)
{
    const QString base = baseName(name).trimmed();
    const int dot = base.lastIndexOf(QLatin1Char('.'));
    if (dot > 0) {
        const QString e = base.mid(dot + 1).toLower();
        bool alnum = !e.isEmpty() && e.size() <= 5;
        bool letters = alnum;
        for (const QChar c : e) {
            if (!c.isLetterOrNumber()) alnum = letters = false;
            if (!c.isLetter()) letters = false;
        }
        if (alnum && knownExtension(e)) return e;
        if (letters && e.size() <= 4) return e;
    }
    // a torrent index that lost the dots: "Sintel 2010 720p mkv"
    const QStringList w = splitWords(base);
    if (w.size() >= 2 && knownExtension(w.last())) return w.last();
    return QString();
}

QStringList SearchMerger::nameWords(const QString &name)
{
    QString base = baseName(name).trimmed();
    const QString ext = probableExtension(base);
    QStringList words;
    if (!ext.isEmpty() && base.endsWith(QLatin1Char('.') + ext, Qt::CaseInsensitive)) {
        base.chop(ext.size() + 1);
        words = splitWords(base);
    } else {
        words = splitWords(base);
        if (!ext.isEmpty() && !words.isEmpty() && words.last() == ext) words.removeLast();
    }
    QStringList out;
    for (const QString &w : std::as_const(words))
        if (w.size() >= 2 && !out.contains(w)) out << w;
    return out;
}

bool SearchMerger::sameName(const QString &a, const QString &b)
{
    const QStringList wa = nameWords(a), wb = nameWords(b);
    if (wa.isEmpty() || wb.isEmpty()) {
        const QStringList ra = splitWords(baseName(a)), rb = splitWords(baseName(b));
        return ra == rb;
    }
    const QStringList &shorter = wa.size() <= wb.size() ? wa : wb;
    const QStringList &longer = wa.size() <= wb.size() ? wb : wa;
    int common = 0;
    for (const QString &w : shorter) if (longer.contains(w)) ++common;
    return common * 10 >= int(shorter.size()) * 6;
}

bool SearchMerger::sameFile(const QString &nameA, qint64 sizeA, const QString &nameB, qint64 sizeB, qint64 minSize)
{
    if (sizeA != sizeB || sizeA <= 0 || sizeA < minSize) return false;
    const QString ea = probableExtension(nameA);
    if (ea.isEmpty() || ea != probableExtension(nameB)) return false;
    return sameName(nameA, nameB);
}

bool SearchMerger::isPadFile(const QString &path)
{
    QString p = path;
    p.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return p.startsWith(QLatin1String(".pad/")) || p.contains(QLatin1String("/.pad/"))
           || baseName(p).startsWith(QLatin1String("_____padding_file"));
}

QList<BundleFile> SearchMerger::realFiles(const QList<BundleFile> &files)
{
    QList<BundleFile> out;
    for (const BundleFile &f : files) if (!isPadFile(f.path)) out << f;
    return out;
}

bool SearchMerger::sharesHashKind(const FileHashes &a, const FileHashes &b)
{
    auto both = [](const QByteArray &x, const QByteArray &y) { return !x.isEmpty() && !y.isEmpty(); };
    return both(a.ed2k, b.ed2k) || both(a.sha1, b.sha1) || both(a.tth, b.tth) || both(a.aich, b.aich)
        || both(a.sha256, b.sha256) || both(a.md5, b.md5) || both(a.btFileRoot, b.btFileRoot)
        || (both(a.btih, b.btih) && a.btih == b.btih && a.btFileIndex >= 0 && b.btFileIndex >= 0);
}

QString SearchMerger::baseKey(const SearchHit &h)
{
    if (!h.hashes.ed2k.isEmpty()) return QStringLiteral("ed2k:") + QString::fromLatin1(h.hashes.ed2k.toHex());
    if (!h.hashes.sha1.isEmpty()) return QStringLiteral("sha1:") + QString::fromLatin1(h.hashes.sha1.toHex());
    if (!h.hashes.btih.isEmpty()) {
        QString k = QStringLiteral("btih:") + QString::fromLatin1(h.hashes.btih.toHex());
        if (h.hashes.btFileIndex >= 0) k += QLatin1Char(':') + QString::number(h.hashes.btFileIndex);
        return k;
    }
    return QStringLiteral("size+name:%1:%2").arg(h.size).arg(normalizedName(h.fileName));
}

QStringList SearchMerger::hashKeys(const FileHashes &h) const
{
    QStringList k;
    auto add = [&k](const char *p, const QByteArray &v) {
        if (!v.isEmpty()) k << QLatin1String(p) + QString::fromLatin1(v.toHex());
    };
    add("e:", h.ed2k);
    add("s:", h.sha1);
    add("t:", h.tth);
    add("a:", h.aich);
    add("2:", h.sha256);
    add("m:", h.md5);
    add("r:", h.btFileRoot);
    add("b:", h.btih);
    return k;
}

// ============================================================================ torrent facts

bool SearchMerger::learnTorrentFacts(const SearchHit &h)
{
    if (!m_across || h.hashes.btih.isEmpty() || h.hashes.btFileIndex >= 0) return false;
    const QByteArray &bt = h.hashes.btih;
    bool changed = false;
    const QList<BundleFile> real = realFiles(h.files);
    if (!h.files.isEmpty() && (real.size() > 1 || listComplete(h.files))
        && m_btFiles.value(bt).size() < h.files.size()) {
        m_btFiles.insert(bt, h.files);
        changed = true;
    }
    const int n = filesCountOf(h);
    if (n > 0 && m_btCount.value(bt) != n && !m_btFiles.contains(bt)) {
        m_btCount.insert(bt, n);
        changed = true;
    }
    return changed;
}

SearchHit SearchMerger::normalise(const SearchHit &h) const
{
    SearchHit n = h;
    if (!m_across || n.hashes.btih.isEmpty() || n.hashes.btFileIndex >= 0) return n;
    QList<BundleFile> files = m_btFiles.value(n.hashes.btih);
    if (files.isEmpty() && (listComplete(n.files) || realFiles(n.files).size() > 1)) files = n.files;
    if (!files.isEmpty()) {
        const QList<BundleFile> real = realFiles(files);
        if (real.size() == 1) {
            n.isBundle = false;
            const QString inner = baseName(real.first().path);
            if (probableExtension(n.fileName).isEmpty() || probableExtension(n.fileName) != probableExtension(inner))
                n.fileName = inner;
            if (real.first().size > 0) n.size = real.first().size;
            n.files.clear();
        } else if (real.size() > 1) {
            n.isBundle = true;
            n.files = files;
        }
        return n;
    }
    const int count = m_btCount.value(n.hashes.btih, filesCountOf(n));
    if (count == 1) n.isBundle = false;
    else if (count > 1) n.isBundle = true;
    return n;
}

SearchMerger::Ident SearchMerger::identOf(const SearchHit &h) const
{
    Ident id;
    if (!h.isBundle) {
        id.valid = h.size > 0;
        id.name = h.fileName;
        id.size = h.size;
        return id;
    }
    if (!m_across || !h.files.isEmpty()) return id;
    if (!h.hashes.btih.isEmpty() && m_btCount.value(h.hashes.btih) > 1) return id;
    if (filesCountOf(h) > 1) return id;
    if (probableExtension(h.fileName).isEmpty() || h.size <= 0) return id;
    id.valid = true;
    id.name = h.fileName;
    id.size = h.size;
    id.probableSingle = true;
    return id;
}

bool SearchMerger::nameMatches(int idx, const Ident &id) const
{
    if (!id.valid) return false;
    for (const Part &part : m_parts.at(idx)) {
        const Ident pi = identOf(part.hit);
        if (!pi.valid) continue;
        if (m_across) {
            if (sameFile(pi.name, pi.size, id.name, id.size, m_minCandidate)) return true;
        } else if (pi.size == id.size && normalizedName(pi.name) == normalizedName(id.name)) {
            return true;
        }
    }
    return false;
}

// ============================================================================ indexes

void SearchMerger::indexRow(int idx)
{
    auto put = [](QMultiHash<QString, int> &ix, const QString &k, int v) {
        for (auto it = ix.constFind(k); it != ix.constEnd() && it.key() == k; ++it)
            if (it.value() == v) return;
        ix.insert(k, v);
    };
    for (const QString &k : hashKeys(m_hits.at(idx).hashes)) put(m_hashIndex, k, idx);
    for (const SearchSource &s : m_hits.at(idx).sources) {
        if (!s.extra.value(QStringLiteral("candidate")).toBool() || s.extra.contains(QStringLiteral("innerOf"))) continue;
        const FileHashes c = FileHashes::fromVariant(s.extra.value(QStringLiteral("hashes")).toMap());
        for (const QString &k : hashKeys(c)) put(m_candIndex, k, idx);
    }
    for (const Part &part : m_parts.at(idx)) {
        const Ident id = identOf(part.hit);
        if (!id.valid) continue;
        bool present = false;
        for (auto it = m_bySize.constFind(id.size); it != m_bySize.constEnd() && it.key() == id.size; ++it)
            if (it.value() == idx) { present = true; break; }
        if (!present) m_bySize.insert(id.size, idx);
    }
}

void SearchMerger::rebuildIndexes()
{
    m_hashIndex.clear();
    m_candIndex.clear();
    m_bySize.clear();
    m_keys.clear();
    for (int i = 0; i < m_hits.size(); ++i) {
        m_keys.insert(m_hits.at(i).key, i);
        indexRow(i);
    }
}

QString SearchMerger::uniqueKey(const QString &base) const
{
    if (!m_keys.contains(base)) return base;
    int i = 2;
    while (m_keys.contains(base + QLatin1Char('#') + QString::number(i))) ++i;
    return base + QLatin1Char('#') + QString::number(i);
}

// ============================================================================ add

SearchMerger::Result SearchMerger::add(const SearchHit &hit, bool allowNew)
{
    Result res;
    if (learnTorrentFacts(hit)) {
        // the torrent is not what earlier hits made of it: rebuild its rows with what is known now
        res.removedKeys << rebuildRowsWith(hit.hashes.btih, &res.changedKeys);
    }
    const SearchHit n = normalise(hit);
    addInternal(n, allowNew, QString(), &res);
    if (res.index >= 0 && m_across) {
        QStringList changed;
        relinkAll(&changed);
        const QString self = m_hits.at(res.index).key;
        for (const QString &k : std::as_const(changed))
            if (k != self && !res.changedKeys.contains(k)) res.changedKeys << k;
    }
    for (const QString &k : std::as_const(res.removedKeys)) res.changedKeys.removeAll(k);
    return res;
}

int SearchMerger::addInternal(const SearchHit &h, bool allowNew, const QString &preferKey, Result *res)
{
    // 1. by hash
    QList<int> cands;
    for (const QString &k : hashKeys(h.hashes)) {
        for (auto it = m_hashIndex.constFind(k); it != m_hashIndex.constEnd() && it.key() == k; ++it)
            if (!cands.contains(it.value())) cands << it.value();
    }
    std::sort(cands.begin(), cands.end());
    auto compatible = [this](const SearchHit &m, const SearchHit &x) {
        return m.isBundle == x.isBundle || (m_across && sameWholeTorrent(m.hashes, x.hashes));
    };
    QList<int> matching;
    for (int idx : std::as_const(cands)) {
        const SearchHit &m = m_hits.at(idx);
        if (compatible(m, h) && m.hashes.matches(h.hashes)) matching << idx;
    }
    if (!matching.isEmpty()) {
        const int survivor = matching.first();
        mergeInto(survivor, h, false);
        // the new hit may prove that other rows are the same file: absorb them into the survivor
        QList<int> absorbed;
        for (int i = 1; i < matching.size(); ++i) {
            const int idx = matching.at(i);
            const SearchHit other = m_hits.at(idx);
            if (!compatible(m_hits.at(survivor), other) || !m_hits.at(survivor).hashes.matches(other.hashes)) continue;
            const QList<Part> parts = m_parts.at(idx);
            for (const Part &p : parts) mergeInto(survivor, p.hit, p.candidate);
            res->removedKeys << other.key;
            absorbed << idx;
        }
        if (!absorbed.isEmpty()) {
            std::sort(absorbed.begin(), absorbed.end(), std::greater<int>());
            for (int idx : std::as_const(absorbed)) {
                m_hits.removeAt(idx);
                m_parts.removeAt(idx);
            }
            rebuildIndexes();
        }
        res->index = survivor;   // survivor is the lowest index: unaffected by the removals
        return survivor;
    }
    if (m_across) {
        // 1b. [merge-1] the same file as a candidate already merged into a row (same torrent from
        // another index, same eD2k hash from Kad ...): joins that row as a candidate too
        QList<int> cc;
        for (const QString &k : hashKeys(h.hashes))
            for (auto it = m_candIndex.constFind(k); it != m_candIndex.constEnd() && it.key() == k; ++it)
                if (!cc.contains(it.value())) cc << it.value();
        std::sort(cc.begin(), cc.end());
        for (int idx : std::as_const(cc)) {
            const SearchHit &m = m_hits.at(idx);
            if (m.hashes.conflicts(h.hashes)) continue;
            bool claimMatches = false;
            for (const SearchSource &s : m.sources) {
                if (!s.extra.value(QStringLiteral("candidate")).toBool() || s.extra.contains(QStringLiteral("innerOf"))) continue;
                const FileHashes c = FileHashes::fromVariant(s.extra.value(QStringLiteral("hashes")).toMap());
                if (c.matches(h.hashes)) { claimMatches = true; break; }
            }
            if (!claimMatches) continue;
            mergeInto(idx, h, true);
            res->index = idx;
            res->candidate = true;
            return idx;
        }
    }
    // 2. candidate by exact size + the name rule
    const Ident id = identOf(h);
    if (id.valid && id.size >= m_minCandidate) {
        int best = -1;
        for (auto it = m_bySize.constFind(id.size); it != m_bySize.constEnd() && it.key() == id.size; ++it) {
            const int idx = it.value();
            const SearchHit &m = m_hits.at(idx);
            if (!m_across && m.isBundle) continue;
            if (sharesHashKind(m.hashes, h.hashes) || m.hashes.conflicts(h.hashes)) continue;
            if (sameWholeTorrent(m.hashes, h.hashes)) continue;   // (rule 1 would have taken it)
            if (!nameMatches(idx, id)) continue;
            if (best < 0 || idx < best) best = idx;
        }
        if (best >= 0) {
            mergeInto(best, h, true);
            SearchHit &m = m_hits[best];
            if (m.isBundle && m.files.isEmpty()) {
                // a torrent that is probably one file met a plain file: the row is that one file
                m.isBundle = false;
                if (!h.isBundle) m.type = h.type != MediaType::Any ? h.type : m.type;
            }
            res->index = best;
            res->candidate = true;
            return best;
        }
    }
    // 3. new hit
    if (!allowNew) return -1;
    SearchHit n = h;
    for (SearchSource &s : n.sources) s.extra.remove(QStringLiteral("candidate"));
    n.innerMatches.clear();
    const QString key = (!preferKey.isEmpty() && !m_keys.contains(preferKey)) ? preferKey : uniqueKey(baseKey(n));
    n.key = key;
    const int idx = m_hits.size();
    m_hits << n;
    SearchHit part = h;
    for (SearchSource &s : part.sources) s.extra.remove(QStringLiteral("candidate"));
    m_parts << QList<Part>{Part{part, false}};
    m_keys.insert(key, idx);
    indexRow(idx);
    res->index = idx;
    res->added = true;
    return idx;
}

void SearchMerger::mergeInto(int idx, const SearchHit &h, bool candidate)
{
    SearchHit &m = m_hits[idx];
    for (SearchSource s : h.sources) {
        if (candidate) {
            s.extra.insert(QStringLiteral("candidate"), true);
            if (!h.hashes.isEmpty()) s.extra.insert(QStringLiteral("hashes"), h.hashes.toVariant());
            if (s.extra.value(QStringLiteral("fileName")).toString().isEmpty() && h.fileName != m.fileName)
                s.extra.insert(QStringLiteral("fileName"), h.fileName);
        }
        bool replaced = false;
        for (SearchSource &e : m.sources) {
            if (e.extra.contains(QStringLiteral("innerOf"))) continue;
            const bool sameUser = e.user.isValid() && s.user.isValid() && e.user.key() == s.user.key();
            const bool sameSwarm = !e.user.isValid() && !s.user.isValid() && e.uri == s.uri;
            if (e.net == s.net && (sameUser || sameSwarm)) {
                // refresh the existing source (newer numbers win, keep candidate flag only if both are)
                const bool wasCandidate = e.extra.value(QStringLiteral("candidate")).toBool();
                if (s.seeds >= 0) e.seeds = s.seeds;
                if (s.peers >= 0) e.peers = s.peers;
                if (s.freeSlots >= 0) e.freeSlots = s.freeSlots;
                if (s.totalSlots >= 0) e.totalSlots = s.totalSlots;
                if (s.pingMs >= 0) e.pingMs = s.pingMs;
                if (!s.connection.isEmpty()) e.connection = s.connection;
                if (e.uri.isEmpty()) e.uri = s.uri;
                for (auto it = s.extra.constBegin(); it != s.extra.constEnd(); ++it) e.extra.insert(it.key(), it.value());
                if (!wasCandidate) {
                    e.extra.remove(QStringLiteral("candidate"));
                    e.extra.remove(QStringLiteral("hashes"));
                }
                replaced = true;
                break;
            }
        }
        if (!replaced) m.sources << s;
    }
    SearchHit part = h;
    for (SearchSource &s : part.sources) s.extra.remove(QStringLiteral("candidate"));
    m_parts[idx] << Part{part, candidate};
    if (!candidate) m.hashes.mergeFrom(h.hashes);
    if (h.fileName.size() > m.fileName.size() && !(candidate && h.isBundle && !m.isBundle)) m.fileName = h.fileName;
    if (m.path.isEmpty()) m.path = h.path;
    if (m.size <= 0) m.size = h.size;
    if (m.type == MediaType::Any) m.type = h.type;
    if (m.durationSec < 0) m.durationSec = h.durationSec;
    if (m.bitrateKbps < 0) m.bitrateKbps = h.bitrateKbps;
    if (m.sampleRateHz < 0) m.sampleRateHz = h.sampleRateHz;
    if (m.files.isEmpty() && (!candidate || m.isBundle)) m.files = h.files;
    if (!m.published.isValid()) m.published = h.published;
    if (m.suspect.isEmpty()) m.suspect = h.suspect;   // [glue-2] search hygiene
    indexRow(idx);
}

// ============================================================================ rebuild / remove

QStringList SearchMerger::rebuildRowsWith(const QByteArray &btih, QStringList *changed)
{
    QList<int> rows;
    for (int i = 0; i < m_hits.size(); ++i) {
        for (const Part &p : m_parts.at(i))
            if (p.hit.hashes.btih == btih && p.hit.hashes.btFileIndex < 0) {
                rows << i;
                break;
            }
    }
    if (rows.isEmpty()) return {};
    QStringList oldKeys;
    QList<QList<Part>> parts;
    for (int i : std::as_const(rows)) {
        oldKeys << m_hits.at(i).key;
        parts << m_parts.at(i);
    }
    for (int k = int(rows.size()) - 1; k >= 0; --k) {
        m_hits.removeAt(rows.at(k));
        m_parts.removeAt(rows.at(k));
    }
    rebuildIndexes();
    QStringList now;
    for (int r = 0; r < parts.size(); ++r) {
        for (int j = 0; j < parts.at(r).size(); ++j) {
            Result tmp;
            const int idx = addInternal(normalise(parts.at(r).at(j).hit), true, j == 0 ? oldKeys.at(r) : QString(), &tmp);
            if (idx >= 0 && !now.contains(m_hits.at(idx).key)) now << m_hits.at(idx).key;
            for (const QString &k : std::as_const(tmp.removedKeys)) now.removeAll(k);
        }
    }
    QStringList removed;
    for (const QString &k : std::as_const(oldKeys)) if (!m_keys.contains(k)) removed << k;
    if (changed) for (const QString &k : std::as_const(now)) if (!changed->contains(k)) *changed << k;
    QStringList more;
    relinkAll(&more);
    if (changed) for (const QString &k : std::as_const(more)) if (!changed->contains(k)) *changed << k;
    return removed;
}

bool SearchMerger::remove(const QString &key)
{
    const int i = indexOfKey(key);
    if (i < 0) return false;
    m_hits.removeAt(i);
    m_parts.removeAt(i);
    rebuildIndexes();
    QStringList changed;
    if (m_across) relinkAll(&changed);
    return true;
}

SearchMerger::Update SearchMerger::setBundleFiles(const QByteArray &btih, const QList<BundleFile> &files)
{
    Update u;
    if (btih.isEmpty() || files.isEmpty()) return u;
    if (!m_across) {
        for (int i = 0; i < m_hits.size(); ++i) {
            SearchHit &hit = m_hits[i];
            if (!hit.isBundle || hit.hashes.btih != btih || !hit.files.isEmpty()) continue;
            hit.files = files;
            u.changedKeys << hit.key;
        }
        return u;
    }
    QList<BundleFile> list = files;
    bool anyIndex = false;
    for (const BundleFile &f : list) if (f.index >= 0) anyIndex = true;
    if (!anyIndex) for (int i = 0; i < list.size(); ++i) list[i].index = i;   // a full list in torrent order
    if (m_btFiles.value(btih).size() >= list.size() && listComplete(m_btFiles.value(btih))) return u;
    m_btFiles.insert(btih, list);
    m_btCount.remove(btih);
    u.removedKeys = rebuildRowsWith(btih, &u.changedKeys);
    return u;
}

// ============================================================================ inner files (rule 3)

void SearchMerger::relinkAll(QStringList *changed)
{
    if (!m_across) return;
    // what every plain row gets: torrents of which one inner file is that same file
    QHash<int, QList<QPair<int, int>>> links;   // plain row → (bundle row, position in its files)
    QHash<int, QList<SearchHit::InnerMatch>> matches;   // bundle row → its inner matches
    for (int b = 0; b < m_hits.size(); ++b) {
        const SearchHit &bh = m_hits.at(b);
        if (!bh.isBundle || bh.files.isEmpty() || bh.hashes.btih.isEmpty()) continue;
        for (int pos = 0; pos < bh.files.size(); ++pos) {
            const BundleFile &f = bh.files.at(pos);
            if (f.size < m_minCandidate || isPadFile(f.path)) continue;
            Ident fid;
            fid.valid = true;
            fid.name = baseName(f.path);
            fid.size = f.size;
            for (auto it = m_bySize.constFind(f.size); it != m_bySize.constEnd() && it.key() == f.size; ++it) {
                const int p = it.value();
                if (p == b) continue;
                const SearchHit &ph = m_hits.at(p);
                if (ph.isBundle && !ph.files.isEmpty()) continue;
                FileHashes inner;
                inner.btih = bh.hashes.btih;
                inner.btFileIndex = f.index;
                if (ph.hashes.conflicts(inner) || sameWholeTorrent(ph.hashes, bh.hashes)) continue;
                if (!nameMatches(p, fid)) continue;
                links[p] << qMakePair(b, pos);
                SearchHit::InnerMatch im;
                im.file = f.index;
                im.path = f.path;
                im.size = f.size;
                im.rowKey = ph.key;
                QList<Net> nets;
                for (const SearchSource &s : ph.sources)
                    if (!s.extra.contains(QStringLiteral("innerOf")) && s.net != Net::Unknown && !nets.contains(s.net)) nets << s.net;
                std::sort(nets.begin(), nets.end(), [](Net x, Net y) { return int(x) < int(y); });
                im.networks = nets;
                // a download of the whole folder can take this file from those networks too
                for (const SearchSource &src : ph.sources) {
                    if (src.extra.contains(QStringLiteral("innerOf")) || src.net == Net::BitTorrent) continue;
                    SearchSource c = src;
                    c.extra.insert(QStringLiteral("candidate"), true);
                    if (!c.extra.contains(QStringLiteral("hashes")) && !ph.hashes.isEmpty()) c.extra.insert(QStringLiteral("hashes"), ph.hashes.toVariant());
                    c.extra.insert(QStringLiteral("bundlePath"), f.path);
                    c.extra.insert(QStringLiteral("size"), f.size);
                    if (im.sources.size() < 200) im.sources << c;
                }
                matches[b] << im;
            }
        }
    }
    auto sameMatches = [](const QList<SearchHit::InnerMatch> &a, const QList<SearchHit::InnerMatch> &b) {
        if (a.size() != b.size()) return false;
        for (int i = 0; i < a.size(); ++i)
            if (a[i].file != b[i].file || a[i].path != b[i].path || a[i].rowKey != b[i].rowKey || a[i].networks != b[i].networks
                || a[i].sources.size() != b[i].sources.size())
                return false;
        return true;
    };
    for (int i = 0; i < m_hits.size(); ++i) {
        SearchHit &h = m_hits[i];
        // bundle side
        const QList<SearchHit::InnerMatch> im = matches.value(i);
        if (!sameMatches(im, h.innerMatches)) {
            h.innerMatches = im;
            if (changed && !changed->contains(h.key)) *changed << h.key;
        }
        // plain side: derived sources, one per (torrent, inner file)
        if (!links.contains(i) && !hasDerived(h)) continue;
        QList<SearchSource> derived;
        for (const auto &l : links.value(i)) {
            const SearchHit &bh = m_hits.at(l.first);
            const BundleFile &f = bh.files.at(l.second);
            SearchSource best;
            for (const SearchSource &s : bh.sources)
                if (s.net == Net::BitTorrent && (best.net == Net::Unknown || s.seeds > best.seeds)) best = s;
            if (best.net == Net::Unknown) {
                best.net = Net::BitTorrent;
                best.uri = bh.hashes.toMagnet(bh.fileName, bh.size);
            }
            SearchSource s;
            s.net = Net::BitTorrent;
            s.uri = best.uri;
            if (f.index >= 0 && s.uri.startsWith(QLatin1String("magnet:"), Qt::CaseInsensitive))
                s.uri += QStringLiteral("&so=%1").arg(f.index);
            s.connection = best.connection;
            s.seeds = best.seeds;
            s.peers = best.peers;
            FileHashes inner;
            inner.btih = bh.hashes.btih;
            inner.btFileIndex = f.index;
            s.extra.insert(QStringLiteral("candidate"), true);
            s.extra.insert(QStringLiteral("hashes"), inner.toVariant());
            s.extra.insert(QStringLiteral("innerOf"), bh.key);
            s.extra.insert(QStringLiteral("insideBundle"), bh.fileName);
            s.extra.insert(QStringLiteral("bundlePath"), f.path);
            s.extra.insert(QStringLiteral("btFileIndex"), f.index);
            s.extra.insert(QStringLiteral("size"), f.size);
            s.extra.insert(QStringLiteral("fileName"), baseName(f.path));
            const QString pn = best.extra.value(QStringLiteral("providerName")).toString();
            if (!pn.isEmpty()) s.extra.insert(QStringLiteral("indexName"), pn);
            const QString tu = best.extra.value(QStringLiteral("torrentUrl")).toString();
            if (!tu.isEmpty()) s.extra.insert(QStringLiteral("torrentUrl"), tu);   // the folder's .torrent (metadata without peers)
            derived << s;
        }
        QList<SearchSource> old;
        QList<SearchSource> kept;
        for (const SearchSource &s : std::as_const(h.sources))
            (s.extra.contains(QStringLiteral("innerOf")) ? old : kept) << s;
        bool same = old.size() == derived.size();
        for (int k = 0; same && k < old.size(); ++k)
            same = old.at(k).uri == derived.at(k).uri && old.at(k).seeds == derived.at(k).seeds
                   && old.at(k).peers == derived.at(k).peers && old.at(k).extra == derived.at(k).extra;
        if (same) continue;
        h.sources = kept + derived;
        if (changed && !changed->contains(h.key)) *changed << h.key;
    }
}

bool SearchMerger::hasDerived(const SearchHit &h)
{
    for (const SearchSource &s : h.sources)
        if (s.extra.contains(QStringLiteral("innerOf"))) return true;
    return false;
}

QStringList SearchMerger::bundlesWorthFileList(int idx) const
{
    QStringList out;
    if (!m_across || idx < 0 || idx >= m_hits.size()) return out;
    const SearchHit &h = m_hits.at(idx);
    auto unknownBundle = [this](const SearchHit &b) {
        return b.isBundle && b.files.isEmpty() && !b.hashes.btih.isEmpty() && !m_btFiles.contains(b.hashes.btih);
    };
    // a folder holds the file plus a little more (subtitles, a poster, a sample)
    auto mayHold = [](qint64 bundle, qint64 file) { return bundle >= file && bundle <= file + file / 2 + (64LL << 20); };
    if (unknownBundle(h)) {
        for (int i = 0; i < m_hits.size(); ++i) {
            const SearchHit &p = m_hits.at(i);
            if (i == idx || p.isBundle || p.size < m_minCandidate || !mayHold(h.size, p.size)) continue;
            if (sameName(h.fileName, p.fileName)) { out << h.key; break; }
        }
        return out;
    }
    if (h.isBundle || h.size < m_minCandidate) return out;
    for (int i = 0; i < m_hits.size(); ++i) {
        const SearchHit &b = m_hits.at(i);
        if (i == idx || !unknownBundle(b) || !mayHold(b.size, h.size)) continue;
        if (sameName(b.fileName, h.fileName)) out << b.key;
    }
    return out;
}

} // namespace mx
