// SPDX-License-Identifier: GPL-3.0-or-later
// Net::Overlay: keyword search among OctoMX users, find-sources, hash-link registry.
#include "overlay_plugin_impl.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QSaveFile>

#include "mx/settings_keys.h"

namespace mx::ovl {

namespace {
constexpr int kMaxAnswerHits = 100;
constexpr int kMaxHashLinks = 5000;
constexpr int kMaxShared = 200000;

int hashKinds(const FileHashes &h)
{
    return !h.btih.isEmpty() + !h.ed2k.isEmpty() + !h.aich.isEmpty() + !h.sha1.isEmpty() + !h.tth.isEmpty() +
           !h.sha256.isEmpty() + !h.md5.isEmpty() + !h.btFileRoot.isEmpty();
}
bool hasMoreThan(const FileHashes &all, const FileHashes &known)
{
    FileHashes m = known;
    m.mergeFrom(all);
    return hashKinds(m) > hashKinds(known);
}
QString displayPath(const SharedFile &f)
{
    const QFileInfo fi(f.path);
    const QString dir = fi.dir().dirName();
    const QString name = f.name.isEmpty() ? fi.fileName() : f.name;
    return dir.isEmpty() ? QStringLiteral("\\") + name : QStringLiteral("\\%1\\%2").arg(dir, name);
}
// [ui-f] type / extensions / bitrate by the shared rule (mx::fileNameMatchesFileType); `q` carries
// type, extension(s), sizes and bitrate limits
bool fileMatches(const SharedFile &f, const QString &lowerName, const QStringList &words, const SearchQuery &q)
{
    if (!matchesWords(lowerName, words)) return false;
    if (q.minSize > 0 && f.size < q.minSize) return false;
    if (q.maxSize > 0 && f.size > q.maxSize) return false;
    return fileNameMatchesFileType(q, f.name, f.type, f.bitrateKbps);
}
} // namespace

QJsonObject OverlayPlugin::fileToJson(const SharedFile &f, bool withPath) const
{
    QJsonObject o;
    o[QStringLiteral("n")] = f.name.isEmpty() ? QFileInfo(f.path).fileName() : f.name;
    if (withPath) o[QStringLiteral("p")] = displayPath(f);   // never the full local path
    o[QStringLiteral("s")] = static_cast<double>(f.size);
    if (f.durationSec >= 0) o[QStringLiteral("d")] = f.durationSec;
    if (f.bitrateKbps >= 0) o[QStringLiteral("b")] = f.bitrateKbps;
    if (f.sampleRateHz >= 0) o[QStringLiteral("r")] = f.sampleRateHz;
    o[QStringLiteral("m")] = static_cast<int>(f.type == MediaType::Any ? mediaTypeForFileName(f.name) : f.type);
    const QJsonObject h = hashesToJson(f.hashes);
    if (!h.isEmpty()) o[QStringLiteral("h")] = h;
    return o;
}

void OverlayPlugin::setSharedFiles(const QList<SharedFile> &files)
{
    const int before = m_shared.size();
    m_shared = files.mid(0, kMaxShared);
    m_sharedLower.clear();
    m_sharedLower.reserve(m_shared.size());
    for (const SharedFile &f : m_shared)
        m_sharedLower << (f.name.isEmpty() ? QFileInfo(f.path).fileName() : f.name).toLower();
    m_haveSharedList = true;
    if (m_running && before != m_shared.size() && nowMs() - m_lastPresenceMs > 5000) announcePresence();
}

// ------------------------------------------------------------------------------------ search
void OverlayPlugin::search(const SearchQuery &q)
{
    const QString text = (q.text + QLatin1Char(' ') + q.text2).simplified();
    const QStringList words = queryWords(text);
    if (!m_running || words.isEmpty()) {
        const quint32 id = q.id;
        QTimer::singleShot(0, this, [this, id] { Q_EMIT searchDone(id); });
        return;
    }
    OvSearch s;
    s.id = q.id;
    s.q = q;
    s.started.start();
    const QString qid = randomId();
    m_searches.insert(qid, s);
    QJsonObject b = newBody("q");
    b[QStringLiteral("qid")] = qid;
    b[QStringLiteral("text")] = cleanText(text, 256);
    b[QStringLiteral("type")] = static_cast<int>(q.type);
    {
        const QStringList exts = queryExtensions(q);   // [ui-f] one: ext (old peers); several: exts as well
        if (exts.size() == 1) b[QStringLiteral("ext")] = cleanText(exts.first(), 16);
        if (exts.size() > 1) b[QStringLiteral("exts")] = QJsonArray::fromStringList(exts.mid(0, 32));
    }
    if (q.minSize > 0) b[QStringLiteral("min")] = static_cast<double>(q.minSize);
    if (q.maxSize > 0) b[QStringLiteral("max")] = static_cast<double>(q.maxSize);
    b[QStringLiteral("node")] = m_node->nodeId();
    QJsonArray addrs;
    for (const QString &a : ourAddresses()) addrs << a;
    b[QStringLiteral("addrs")] = addrs;
    publishSigned(QString::fromLatin1(kTopicSearch), b);
}

void OverlayPlugin::cancelSearch(quint32 queryId)
{
    for (auto it = m_searches.begin(); it != m_searches.end();) {
        if (it->id == queryId) it = m_searches.erase(it);
        else ++it;
    }
}

void OverlayPlugin::finishSearches(bool all)
{
    QList<quint32> done;
    for (auto it = m_searches.begin(); it != m_searches.end();) {
        const qint64 el = it->started.elapsed();
        const bool fin = all || el >= m_t.searchMaxMs || (m_peers.isEmpty() && el >= 2000) ||
                         (el >= m_t.searchQuietMs && el - it->lastHitMs >= m_t.searchQuietMs) ||
                         it->emitted >= it->q.maxResults;
        if (fin) {
            done << it->id;
            it = m_searches.erase(it);
        } else {
            ++it;
        }
    }
    for (quint32 id : done) Q_EMIT searchDone(id);
}

void OverlayPlugin::onSearchEnvelope(const QString &uid, const QJsonObject &b)
{
    const QString t = jStr(b, "t", 16);
    const QString qid = jStr(b, "qid", 64);
    const QString node = jStr(b, "node", 64).toLower();
    if (!isHex(qid) || !isHex(node, 64)) return;
    if (!m_peerGate.allow(QStringLiteral("q:") + uid, 1.0)) return;   // per-user query rate
    QStringList addrs;
    for (const QJsonValue &v : jArr(b, "addrs", 8))
        if (v.isString()) addrs << cleanText(v.toString(), 64);
    touchUser(uid, b, node);
    if (m_users[uid].addrs.isEmpty()) m_users[uid].addrs = addrs;

    if (t == QLatin1String("q")) {
        Q_EMIT incomingSearches(1);   // [glue-2] SPM figure
        const QStringList words = queryWords(jStr(b, "text", 256));
        if (words.isEmpty()) return;
        const MediaType type = static_cast<MediaType>(qBound(qint64(0), jInt(b, "type", 0), qint64(7)));
        SearchQuery fq;   // the asker's filters
        fq.type = type;
        fq.extension = jStr(b, "ext", 16);
        for (const QJsonValue &e : jArr(b, "exts", 32))
            if (e.isString()) fq.extensions << cleanText(e.toString(), 16);
        fq.minSize = jInt(b, "min", 0);
        fq.maxSize = jInt(b, "max", 0);
        QJsonArray items;
        for (int i = 0; i < m_shared.size() && items.size() < kMaxAnswerHits; ++i)
            if (fileMatches(m_shared[i], m_sharedLower[i], words, fq))
                items << fileToJson(m_shared[i], true);
        if (items.isEmpty()) return;
        QJsonObject a;
        a[QStringLiteral("t")] = QStringLiteral("hits");
        a[QStringLiteral("v")] = kWireVersion;
        a[QStringLiteral("qid")] = qid;
        a[QStringLiteral("items")] = items;
        a[QStringLiteral("name")] = m_displayName;
        a[QStringLiteral("conn")] = connectionLabel();
        a[QStringLiteral("free")] = freeSlots();
        a[QStringLiteral("total")] = totalSlots();
        sendToNode(node, a, addrs);
        return;
    }
    if (t == QLatin1String("src_q") || t == QLatin1String("hl_q")) {
        const FileHashes h = hashesFromJson(jObj(b, "h"));
        const qint64 size = jInt(b, "size", 0);
        if (h.isEmpty()) return;
        if (t == QLatin1String("src_q")) {
            QList<SharedFile> files = m_shared;
            // [up-1] a file we are still downloading answers too, by its trusted identity, but only
            // askers that understand partial sources ("part") hear about it
            SharedFile pf;
            QList<QPair<qint64, qint64>> have;
            if (jBool(b, "part", false) && m_ctx.library && setting(QString(keys::library::ShareIncomplete), true).toBool()
                && m_ctx.library->partialRanges(h, &have) && !have.isEmpty() && m_ctx.library->findByHash(h, &pf)
                && !pf.hashes.conflicts(h))
                files.prepend(pf);
            for (const SharedFile &f : std::as_const(files)) {
                const bool partialMatch = !pf.path.isEmpty() && f.path == pf.path;   // matched by the library
                if ((!partialMatch && !f.hashes.matches(h)) || f.hashes.conflicts(h) || (size > 0 && f.size != size)) continue;
                QJsonObject a;
                a[QStringLiteral("t")] = QStringLiteral("src");
                a[QStringLiteral("v")] = kWireVersion;
                a[QStringLiteral("qid")] = qid;
                a[QStringLiteral("h")] = hashesToJson(f.hashes);
                a[QStringLiteral("size")] = static_cast<double>(f.size);
                a[QStringLiteral("n")] = f.name;
                a[QStringLiteral("name")] = m_displayName;
                a[QStringLiteral("conn")] = connectionLabel();
                a[QStringLiteral("free")] = freeSlots();
                a[QStringLiteral("total")] = totalSlots();
                sendToNode(node, a, addrs);
                break;
            }
            return;
        }
        if (!setting(QString(keys::overlay::ShareHashLinks), true).toBool()) return;
        FileHashes all;
        qint64 sz = 0;
        QString name;
        if (!lookupLocal(h, size, &all, &sz, &name) || !hasMoreThan(all, h)) return;
        QJsonObject a;
        a[QStringLiteral("t")] = QStringLiteral("hl");
        a[QStringLiteral("v")] = kWireVersion;
        a[QStringLiteral("qid")] = qid;
        a[QStringLiteral("h")] = hashesToJson(all);
        a[QStringLiteral("size")] = static_cast<double>(sz);
        sendToNode(node, a, addrs);
    }
}

void OverlayPlugin::onHits(const QString &nodeId, const QJsonObject &o)
{
    auto it = m_searches.find(jStr(o, "qid", 64));
    if (it == m_searches.end()) return;
    OvSearch &s = it.value();
    const QString uid = m_nodeUser.value(nodeId);
    const UserRef who = userRefFor(uid);
    const QStringList words = queryWords(s.q.text + QLatin1Char(' ') + s.q.text2);
    QList<SearchHit> hits;
    for (const QJsonValue &v : jArr(o, "items", kMaxAnswerHits)) {
        if (s.emitted + hits.size() >= s.q.maxResults) break;
        if (!v.isObject()) continue;
        const QJsonObject f = v.toObject();
        SharedFile sf;
        sf.name = cleanText(jStr(f, "n", 512), 512);
        sf.size = jInt(f, "s", -1);
        sf.type = static_cast<MediaType>(qBound(qint64(0), jInt(f, "m", 0), qint64(7)));
        if (sf.name.isEmpty() || sf.size < 0) continue;
        // never trust the responder's matching: filter locally with the same rules
        sf.bitrateKbps = static_cast<int>(jInt(f, "b", -1));
        if (!fileMatches(sf, sf.name.toLower(), words, s.q)) continue;
        SearchHit h;
        h.queryId = s.id;
        h.fileName = sf.name;
        h.path = cleanText(jStr(f, "p", 1024), 1024);
        h.size = sf.size;
        h.type = sf.type;
        h.durationSec = static_cast<int>(jInt(f, "d", -1));
        h.bitrateKbps = static_cast<int>(jInt(f, "b", -1));
        h.sampleRateHz = static_cast<int>(jInt(f, "r", -1));
        h.hashes = hashesFromJson(jObj(f, "h"));
        const QString key = uid + QLatin1Char('/') + (h.hashes.isEmpty() ? h.fileName + QString::number(h.size)
                                                                         : QString::fromLatin1(h.hashes.sha1.toHex() + h.hashes.ed2k.toHex() + h.hashes.sha256.toHex()));
        if (s.seen.contains(key)) continue;
        s.seen.insert(key);
        SearchSource src;
        src.net = Net::Overlay;
        src.user = who;
        const QString nm = cleanText(jStr(o, "name", 48), 48);
        if (!nm.isEmpty()) src.user.name = nm;
        // "winmx:<user id>": a source descriptor kept for compatibility (stored downloads carry it,
        // so the scheme stays)
        src.uri = QStringLiteral("winmx:%1").arg(uid);
        src.connection = cleanText(jStr(o, "conn", 24), 24);
        src.freeSlots = static_cast<int>(qBound(qint64(-1), jInt(o, "free", -1), qint64(1000)));
        src.totalSlots = static_cast<int>(qBound(qint64(-1), jInt(o, "total", -1), qint64(1000)));
        src.extra.insert(QStringLiteral("node"), nodeId);
        h.sources << src;
        hits << h;
    }
    if (hits.isEmpty()) return;
    s.emitted += hits.size();
    s.lastHitMs = s.started.elapsed();
    Q_EMIT searchHits(s.id, hits);
}

// ------------------------------------------------------------------------------------ sources
void OverlayPlugin::findSources(const QString &downloadId, const FileHashes &h, qint64 size, const QString &name)
{
    if (h.isEmpty()) return;
    FindReq r;
    r.downloadId = downloadId;
    r.h = h;
    r.size = size;
    r.name = name;
    m_find.insert(downloadId, r);
    if (m_running) askSources();
}

void OverlayPlugin::stopFindSources(const QString &downloadId)
{
    m_find.remove(downloadId);
}

void OverlayPlugin::askSources()
{
    if (!m_running || m_peers.isEmpty()) return;
    const qint64 now = nowMs();
    for (FindReq &r : m_find) {
        if (r.lastAskMs && now - r.lastAskMs < m_t.sourcesRepeatMs) continue;
        r.lastAskMs = now;
        r.qid = randomId();
        QJsonObject b = newBody("src_q");
        b[QStringLiteral("qid")] = r.qid;
        b[QStringLiteral("h")] = hashesToJson(r.h);
        b[QStringLiteral("size")] = static_cast<double>(r.size);
        b[QStringLiteral("part")] = true;   // [up-1] partial sources welcome (we read "have" maps)
        b[QStringLiteral("node")] = m_node->nodeId();
        QJsonArray addrs;
        for (const QString &a : ourAddresses()) addrs << a;
        b[QStringLiteral("addrs")] = addrs;
        publishSigned(QString::fromLatin1(kTopicSearch), b);
    }
}

void OverlayPlugin::onSources(const QString &nodeId, const QJsonObject &o)
{
    const QString qid = jStr(o, "qid", 64);
    FindReq *req = nullptr;
    for (FindReq &r : m_find)
        if (r.qid == qid) { req = &r; break; }
    if (!req) return;
    const FileHashes all = hashesFromJson(jObj(o, "h"));
    const qint64 size = jInt(o, "size", 0);
    if (!all.matches(req->h) || all.conflicts(req->h)) return;
    if (req->size > 0 && size > 0 && size != req->size) return;
    const QString uid = m_nodeUser.value(nodeId);
    if (uid.isEmpty() || req->reported.contains(uid)) return;
    req->reported.insert(uid);
    SearchSource src;
    src.net = Net::Overlay;
    src.user = userRefFor(uid);
    const QString nm = cleanText(jStr(o, "name", 48), 48);
    if (!nm.isEmpty()) src.user.name = nm;
    src.uri = QStringLiteral("winmx:%1").arg(uid);
    src.connection = cleanText(jStr(o, "conn", 24), 24);
    src.freeSlots = static_cast<int>(qBound(qint64(-1), jInt(o, "free", -1), qint64(1000)));
    src.totalSlots = static_cast<int>(qBound(qint64(-1), jInt(o, "total", -1), qint64(1000)));
    src.extra.insert(QStringLiteral("node"), nodeId);
    const QString downloadId = req->downloadId;
    if (hasMoreThan(all, req->h)) {
        req->h.mergeFrom(all);
        Q_EMIT hashesLearned(downloadId, all);
    }
    Q_EMIT sourcesFound(downloadId, {src});
}

// -------------------------------------------------------------------------------- hash links
bool OverlayPlugin::lookupLocal(const FileHashes &h, qint64 size, FileHashes *all, qint64 *outSize, QString *name) const
{
    for (const SharedFile &f : m_shared) {
        if (!f.hashes.matches(h) || (size > 0 && f.size != size)) continue;
        *all = f.hashes;
        *outSize = f.size;
        *name = f.name;
        return true;
    }
    if (!m_haveSharedList && m_ctx.library) {
        SharedFile sf;
        if (m_ctx.library->findByHash(h, &sf) && (size <= 0 || sf.size == size) && sf.hashes.matches(h) &&
            !sf.hashes.conflicts(h)) {
            *all = sf.hashes;
            *outSize = sf.size;
            *name = sf.name;
            return true;
        }
    }
    for (int i = m_hashLinks.size() - 1; i >= 0; --i) {
        const HashLink &l = m_hashLinks[i];
        if (!l.h.matches(h) || (size > 0 && l.size > 0 && l.size != size)) continue;
        *all = l.h;
        *outSize = l.size;
        *name = l.name;
        return true;
    }
    return false;
}

void OverlayPlugin::cacheHashLink(const FileHashes &h, qint64 size, const QString &name)
{
    if (hashKinds(h) < 2) return;
    for (int i = m_hashLinks.size() - 1; i >= 0; --i) {
        HashLink &l = m_hashLinks[i];
        if (l.h.conflicts(h) || !l.h.matches(h) || (l.size > 0 && size > 0 && l.size != size)) continue;
        l.h.mergeFrom(h);
        if (l.size <= 0) l.size = size;
        if (l.name.isEmpty()) l.name = name;
        l.lastMs = nowMs();
        m_hashLinks.move(i, m_hashLinks.size() - 1);
        m_hashLinksDirty = true;
        return;
    }
    m_hashLinks.append(HashLink{h, size, cleanText(name, 256), nowMs()});
    while (m_hashLinks.size() > kMaxHashLinks) m_hashLinks.removeFirst();
    m_hashLinksDirty = true;
}

void OverlayPlugin::lookupHashLinks(const FileHashes &known, qint64 size)
{
    if (known.isEmpty()) return;
    FileHashes all;
    qint64 sz = 0;
    QString name;
    if (lookupLocal(known, size, &all, &sz, &name) && hasMoreThan(all, known) && !all.conflicts(known)) {
        QTimer::singleShot(0, this, [this, known, all, sz] { Q_EMIT hashLinksFound(known, all, sz); });
    }
    if (!m_running) return;
    const QString qid = randomId();
    m_hlReqs.insert(qid, HlReq{known, size, nowMs()});
    QJsonObject b = newBody("hl_q");
    b[QStringLiteral("qid")] = qid;
    b[QStringLiteral("h")] = hashesToJson(known);
    b[QStringLiteral("size")] = static_cast<double>(size);
    b[QStringLiteral("node")] = m_node->nodeId();
    QJsonArray addrs;
    for (const QString &a : ourAddresses()) addrs << a;
    b[QStringLiteral("addrs")] = addrs;
    publishSigned(QString::fromLatin1(kTopicSearch), b);
}

void OverlayPlugin::onHashLinkAnswer(const QString &, const QJsonObject &o)
{
    auto it = m_hlReqs.find(jStr(o, "qid", 64));
    if (it == m_hlReqs.end()) return;
    const FileHashes all = hashesFromJson(jObj(o, "h"));
    const qint64 size = jInt(o, "size", 0);
    // answers are hints (the engine verifies data), but drop anything that contradicts what we know
    if (all.conflicts(it->known) || !all.matches(it->known)) return;
    if (it->size > 0 && size > 0 && size != it->size) return;
    if (!hasMoreThan(all, it->known)) return;
    const FileHashes known = it->known;
    const qint64 sz = size > 0 ? size : it->size;
    cacheHashLink(all, sz, QString());
    Q_EMIT hashLinksFound(known, all, sz);
}

void OverlayPlugin::publishHashLinks(const FileHashes &all, qint64 size, const QString &name)
{
    if (hashKinds(all) < 2) return;
    cacheHashLink(all, size, name);
    if (!m_running || !setting(QString(keys::overlay::ShareHashLinks), true).toBool()) return;
    QJsonObject b = newBody("hl_pub");
    b[QStringLiteral("h")] = hashesToJson(all);
    b[QStringLiteral("size")] = static_cast<double>(size);
    b[QStringLiteral("n")] = cleanText(name, 256);
    publishSigned(QString::fromLatin1(kTopicHashLinks), b);
}

void OverlayPlugin::onHashLinkPublish(const QJsonObject &b)
{
    if (jStr(b, "t", 16) != QLatin1String("hl_pub")) return;
    if (!setting(QString(keys::overlay::ShareHashLinks), true).toBool()) return;
    const FileHashes h = hashesFromJson(jObj(b, "h"));
    const qint64 size = jInt(b, "size", 0);
    if (hashKinds(h) < 2 || size <= 0) return;
    cacheHashLink(h, size, cleanText(jStr(b, "n", 256), 256));
}

void OverlayPlugin::loadHashLinks()
{
    m_hashLinks.clear();
    if (m_dataDir.isEmpty()) return;
    QFile f(m_dataDir + QStringLiteral("/hashlinks.json"));
    if (!f.open(QIODevice::ReadOnly) || f.size() > 16 * 1024 * 1024) return;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    for (const QJsonValue &v : doc.array()) {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        HashLink l;
        l.h = hashesFromJson(jObj(o, "h"));
        l.size = jInt(o, "s", 0);
        l.name = jStr(o, "n", 256);
        l.lastMs = jInt(o, "t", 0);
        if (hashKinds(l.h) >= 2) m_hashLinks << l;
        if (m_hashLinks.size() >= kMaxHashLinks) break;
    }
}

void OverlayPlugin::saveHashLinks()
{
    if (!m_hashLinksDirty || m_dataDir.isEmpty()) return;
    QJsonArray arr;
    for (const HashLink &l : m_hashLinks) {
        QJsonObject o;
        o[QStringLiteral("h")] = hashesToJson(l.h);
        o[QStringLiteral("s")] = static_cast<double>(l.size);
        if (!l.name.isEmpty()) o[QStringLiteral("n")] = l.name;
        o[QStringLiteral("t")] = static_cast<double>(l.lastMs);
        arr << o;
    }
    QDir().mkpath(m_dataDir);
    QSaveFile f(m_dataDir + QStringLiteral("/hashlinks.json"));
    if (f.open(QIODevice::WriteOnly) && f.write(QJsonDocument(arr).toJson(QJsonDocument::Compact)) >= 0 && f.commit())
        m_hashLinksDirty = false;
}

} // namespace mx::ovl
