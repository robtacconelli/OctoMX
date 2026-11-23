// SPDX-License-Identifier: GPL-3.0-or-later
// Gnutella plugin: keyword searches, query hits, source finding, downloads, browse host.
#include "gnut_plugin_impl.h"
#include "gnut_huge.h"

#include "mxhash/encoding.h"

#include <QtCore/QFileInfo>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpSocket>

namespace mx::gnut {

static constexpr int kSearchMinMs = 40000;    // a search runs at least this long...
static constexpr int kSearchQuietMs = 20000;  // ...then ends after this much silence
static constexpr int kSearchMaxMs = 180000;
static constexpr qint64 kFinderIntervalSec = 30 * 60;   // gtk-gnutella's SEARCH_MIN_RETRY etiquette
static constexpr qint64 kFinderGlobalGapSec = 30;

// LimeWire drops queries over 30 characters: keep whole words up to that, the rest goes in GGEP XQ.
static QString shortQueryText(const QStringList &words, QString *extended)
{
    QString s;
    for (const QString &w : words) {
        const QString cand = s.isEmpty() ? w : s + QLatin1Char(' ') + w;
        if (cand.size() > 30) break;
        s = cand;
    }
    const QString full = words.join(QLatin1Char(' '));
    if (extended && full != s && full.toUtf8().size() <= 255) *extended = full;
    return s;
}

static void rememberGuid(QHash<QByteArray, quint32> &map, QList<QByteArray> &order, const QByteArray &g, quint32 id)
{
    map.insert(g, id);
    order << g;
    while (order.size() > 512) map.remove(order.takeFirst());
}

void GnutellaPlugin::search(const mx::SearchQuery &q)
{
    SearchState s;
    s.q = q;
    s.words = splitKeywords(q.text + QLatin1Char(' ') + q.text2);
    s.started.start();
    s.lastHit.start();
    if (s.words.isEmpty()) {
        m_searches.insert(q.id, s);
        QTimer::singleShot(0, this, [this, id = q.id] {
            auto it = m_searches.find(id);
            if (it != m_searches.end() && !it->done) { it->done = true; emit searchDone(id); }
        });
        return;
    }
    const QByteArray guid = randomGuid(true);
    s.guids << guid;
    rememberGuid(m_queryGuids, m_queryGuidOrder, guid, q.id);
    auto it = m_searches.insert(q.id, s);
    sendSearch(*it);
    log(QStringLiteral("search #%1 \"%2\" sent to %3 ultrapeers").arg(q.id).arg(s.words.join(QLatin1Char(' '))).arg(it->sentTo.size()));
}

void GnutellaPlugin::sendSearch(SearchState &s, UltrapeerConnection *only)
{
    QueryMsg qm;
    quint16 flags = QfMark | QfXml | QfGgepH | QfLeafGuided;
    if (firewalled()) flags |= QfFirewalled;
    qm.flags = flags;
    qm.text = shortQueryText(s.words, &qm.extendedText);
    qm.metaMask = metaMaskForType(s.q.type);
    const QByteArray payload = buildQueryPayload(qm);
    const QList<UltrapeerConnection *> ups = only ? QList<UltrapeerConnection *>{only} : connectedUps();
    for (UltrapeerConnection *c : ups) {
        if (c->hopsFlow() == 0) continue;
        Message m = Message::make(MsgQuery, payload, 4, s.guids.last());
        c->send(m);
        s.sentTo.insert(c->address().key());
    }
}

void GnutellaPlugin::cancelSearch(quint32 queryId)
{
    auto it = m_searches.find(queryId);
    if (it == m_searches.end()) return;
    if (!it->done) { it->done = true; emit searchDone(queryId); }
}

bool GnutellaPlugin::hitPassesFilter(const SearchState &s, const HitResult &r) const
{
    // all keywords must appear in the name (drops spam / unrelated "catch-all" results)
    const QString norm = splitKeywords(r.name).join(QLatin1Char(' '));
    for (const QString &w : s.words)
        if (!norm.contains(w)) return false;
    const mx::SearchQuery &q = s.q;
    if (q.minSize > 0 && qint64(r.size) < q.minSize) return false;
    if (q.maxSize > 0 && qint64(r.size) > q.maxSize) return false;
    // [ui-f] type, extension(s), bitrate: the shared rule (Gnutella hits are files: the extension decides)
    return mx::fileNameMatchesFileType(q, r.name, mx::MediaType::Any, r.bitrateKbps);
}

void GnutellaPlugin::rememberServent(const SourceDesc &d)
{
    if (d.guid.size() != 16) return;
    const QString k = guidHex(d.guid);
    if (!m_servents.contains(k)) {
        m_serventOrder << k;
        while (m_serventOrder.size() > 5000) m_servents.remove(m_serventOrder.takeFirst());
    }
    m_servents.insert(k, d);
}

mx::SearchSource GnutellaPlugin::makeSource(const SourceDesc &d, quint32 speed, const QueryHit &qh) const
{
    mx::SearchSource src;
    src.net = mx::Net::Gnutella;
    src.user.net = mx::Net::Gnutella;
    src.user.id = d.userId();
    src.user.address = d.addr.isValid() ? d.addr.toString() : QString();
    src.user.name = d.addr.isValid() ? d.addr.toString() : (d.vendor.isEmpty() ? QStringLiteral("Gnutella user") : d.vendor);
    src.uri = d.toUri();
    src.connection = speedLabel(speed);
    if (qh.busyKnown) src.freeSlots = qh.busy ? 0 : 1;
    src.extra[QStringLiteral("vendor")] = d.vendor;
    src.extra[QStringLiteral("firewalled")] = d.firewalled;
    src.extra[QStringLiteral("browse")] = d.browse;
    if (!d.proxies.isEmpty()) src.extra[QStringLiteral("pushProxies")] = int(d.proxies.size());
    if (qh.measuredKnown) src.extra[QStringLiteral("measuredSpeed")] = qh.measured;
    if (qh.uploadedKnown) src.extra[QStringLiteral("hasUploaded")] = qh.uploaded;
    return src;
}

void GnutellaPlugin::onQueryHit(UltrapeerConnection *c, const Message &m)
{
    Q_UNUSED(c)
    ++m_stats.hitsReceived;
    const quint32 searchId = m_queryGuids.value(m.guid, 0);
    const QString finderId = m_finderGuids.value(m.guid);
    const bool anyFinders = !m_finders.isEmpty();
    if (!searchId && finderId.isEmpty() && !anyFinders) return;
    QueryHit qh;
    QString err;
    if (!parseQueryHit(m.payload, &qh, &err)) {
        qCDebug(lcGnut).noquote() << "bad query hit from" << c->id() << err;
        return;
    }
    SourceDesc base;
    base.addr = HostPort{qh.ip, qh.port};
    if (!isRoutableIp(base.addr.ip) && isRoutableIp(c->address().ip)) base.addr = HostPort();   // private address: push only
    base.guid = qh.serventGuid;
    base.firewalled = qh.push || !base.addr.isValid();
    base.browse = qh.browseHost;
    base.proxies = qh.pushProxies;
    base.tls = qh.tls;   // [enc-1] QHD GGEP "TLS"
    base.vendor = qh.vendor.isEmpty() ? QString() : vendorName(qh.vendor);
    rememberServent(base);

    // source finding: any hit with a SHA-1 we are looking for
    if (anyFinders) {
        for (const HitResult &r : qh.results) {
            if (r.hashes.sha1.isEmpty()) continue;
            for (auto it = m_finders.begin(); it != m_finders.end(); ++it) {
                Finder &f = *it;
                if (f.hashes.sha1 != r.hashes.sha1) continue;
                if (f.size > 0 && qint64(r.size) != f.size) continue;
                SourceDesc d = base;
                d.index = r.index;
                d.name = r.name;
                d.sha1 = r.hashes.sha1;
                const QString key = d.userId() + QLatin1Char('|') + d.addr.toString();
                if (!f.known.contains(key)) {
                    f.known.insert(key);
                    mx::SearchSource src = makeSource(d, qh.speed, qh);
                    if (!r.hashes.tth.isEmpty()) {
                        // [core-5] what this servent claims about the file: the core checks the
                        // claim against the verified file and bans the source if it lied
                        mx::FileHashes claim;
                        claim.sha1 = r.hashes.sha1;
                        claim.tth = r.hashes.tth;
                        src.extra[QStringLiteral("hashes")] = claim.toVariant();
                    }
                    emit sourcesFound(f.downloadId, {src});
                }
                // [core-5] a provisional hint for the core (Engine: Gnutella hashes are never trusted
                // on their own), which can ask other networks by it
                if (!r.hashes.tth.isEmpty() && f.hashes.tth.isEmpty()) {
                    f.hashes.tth = r.hashes.tth;
                    mx::FileHashes more;
                    more.tth = r.hashes.tth;
                    emit hashesLearned(f.downloadId, more);
                }
                // alt-locs carried in the hit
                QList<mx::SearchSource> alts;
                for (const HostPort &a : r.altLocs) {
                    SourceDesc ad;
                    ad.addr = a;
                    ad.tls = r.altLocsTls.contains(a);   // [enc-1] ALT_TLS
                    ad.sha1 = r.hashes.sha1;
                    ad.name = r.name;
                    const QString ak = ad.userId() + QLatin1Char('|') + a.toString();
                    if (f.known.contains(ak) || !isRoutableIp(a.ip)) continue;
                    f.known.insert(ak);
                    alts << makeSource(ad, 0, QueryHit());
                }
                if (!alts.isEmpty()) emit sourcesFound(f.downloadId, alts);
            }
        }
    }
    if (!searchId) return;
    auto sit = m_searches.find(searchId);
    if (sit == m_searches.end() || sit->done) return;
    SearchState &s = *sit;
    for (const HitResult &r : qh.results) {
        if (s.hits >= s.q.maxResults) break;
        if (!hitPassesFilter(s, r)) continue;
        SourceDesc d = base;
        d.index = r.index;
        d.name = r.name;
        d.sha1 = r.hashes.sha1;
        const QString key = d.userId() + QLatin1Char('|') + QString::number(r.index) + QLatin1Char('|') + r.name;
        if (s.seen.contains(key)) continue;
        s.seen.insert(key);
        mx::SearchHit h;
        h.queryId = searchId;
        h.fileName = QFileInfo(r.name).fileName();
        if (h.fileName.isEmpty()) h.fileName = r.name;
        h.path = r.path.isEmpty() ? QString() : r.path + QLatin1Char('/') + r.name;
        h.size = qint64(r.size);
        h.type = mx::mediaTypeForFileName(r.name);
        h.durationSec = r.durationSec;
        h.bitrateKbps = r.bitrateKbps;
        h.sampleRateHz = r.sampleRateHz;
        h.hashes.sha1 = r.hashes.sha1;
        h.hashes.tth = r.hashes.tth;
        if (r.createTime > 0 && r.createTime < 4102444800LL) h.published = QDateTime::fromSecsSinceEpoch(r.createTime);
        h.sources << makeSource(d, qh.speed, qh);
        if (!r.hashes.sha1.isEmpty()) h.key = QStringLiteral("sha1:") + mx::hash::toHex(r.hashes.sha1);
        s.pending << h;
        ++s.hits;
        s.lastHit.restart();
    }
    if (!s.pending.isEmpty() && !m_hitFlushTimer.isActive()) m_hitFlushTimer.start();
}

void GnutellaPlugin::flushSearchHits()
{
    for (auto it = m_searches.begin(); it != m_searches.end(); ++it) {
        if (it->pending.isEmpty()) continue;
        const QList<mx::SearchHit> batch = it->pending;
        it->pending.clear();
        if (!it->done) emit searchHits(it.key(), batch);
    }
}

void GnutellaPlugin::answerQueryStatus(UltrapeerConnection *c, const QByteArray &guid)
{
    quint16 count = 0xFFFF;
    const quint32 id = m_queryGuids.value(guid, 0);
    if (id) {
        auto it = m_searches.constFind(id);
        if (it != m_searches.constEnd() && !it->done && it->hits < it->q.maxResults) count = quint16(qMin(it->hits, 0xFFFE));
    } else {
        const QString fid = m_finderGuids.value(guid);
        auto it = m_finders.constFind(fid);
        if (!fid.isEmpty() && it != m_finders.constEnd() && it->known.size() < 50) count = quint16(it->known.size());
    }
    QByteArray body;
    putU16(body, count);
    sendVendor(c, QByteArray("BEAR", 4), 12, 1, body, guid);
}

void GnutellaPlugin::searchTick()
{
    // searches: done after a quiet period
    for (auto it = m_searches.begin(); it != m_searches.end();) {
        SearchState &s = *it;
        if (!s.done) {
            const qint64 el = s.started.elapsed();
            const bool quiet = el > kSearchMinMs && s.lastHit.elapsed() > kSearchQuietMs;
            const bool noNet = el > 30000 && s.sentTo.isEmpty();
            if (quiet || el > kSearchMaxMs || noNet || s.hits >= s.q.maxResults) {
                if (!s.pending.isEmpty()) flushSearchHits();
                s.done = true;
                log(QStringLiteral("search #%1 done: %2 results").arg(it.key()).arg(s.hits));
                emit searchDone(it.key());
            }
            ++it;
        } else if (s.started.elapsed() > 15 * 60 * 1000) {
            for (const QByteArray &g : s.guids) m_queryGuids.remove(g);
            it = m_searches.erase(it);
        } else {
            ++it;
        }
    }
    // source finding: one urn query at a time, each download at most every 30 min
    const qint64 now = nowSecs();
    if (!m_finders.isEmpty() && now - m_lastFinderQuery >= kFinderGlobalGapSec && !connectedUps().isEmpty()) {
        Finder *best = nullptr;
        for (Finder &f : m_finders) {
            if (f.lastQuery && now - f.lastQuery < kFinderIntervalSec) continue;
            if (!best || f.lastQuery < best->lastQuery) best = &f;
        }
        if (best) sendFinderQuery(*best);
    }
}

void GnutellaPlugin::findSources(const QString &downloadId, const mx::FileHashes &h, qint64 size, const QString &name)
{
    if (h.sha1.size() != 20) return;                // Gnutella locates files by SHA-1
    Finder &f = m_finders[downloadId];
    const bool fresh = f.downloadId.isEmpty();
    f.downloadId = downloadId;
    f.hashes.mergeFrom(h);
    f.size = size;
    f.name = name;
    if (fresh) log(QStringLiteral("looking for sources of %1 (%2)").arg(name, sha1Urn(h.sha1)));
}

void GnutellaPlugin::stopFindSources(const QString &downloadId)
{
    m_finders.remove(downloadId);
    for (auto it = m_finderGuids.begin(); it != m_finderGuids.end();) {
        if (it.value() == downloadId) it = m_finderGuids.erase(it);
        else ++it;
    }
    m_thexState.remove(downloadId);
}

void GnutellaPlugin::sendFinderQuery(Finder &f)
{
    QueryMsg qm;
    qm.flags = QfMark | QfGgepH | QfLeafGuided | (firewalled() ? QfFirewalled : 0);
    QStringList words = splitKeywords(QFileInfo(f.name).completeBaseName());
    qm.text = shortQueryText(words, nullptr);
    if (qm.text.size() < 3) qm.text = QStringLiteral("\\");   // URN-only query
    qm.sha1s << f.hashes.sha1;
    const QByteArray guid = randomGuid(true);
    if (f.queries > 0) { QByteArray g = guid; g[15] = char(g[15] | 0x01); }   // (requery mark is informational)
    m_finderGuids.insert(guid, f.downloadId);
    if (m_finderGuids.size() > 256) m_finderGuids.erase(m_finderGuids.begin());
    const QByteArray payload = buildQueryPayload(qm);
    for (UltrapeerConnection *c : connectedUps())
        if (c->hopsFlow() != 0) c->send(Message::make(MsgQuery, payload, 4, guid));
    f.lastQuery = nowSecs();
    ++f.queries;
    m_lastFinderQuery = f.lastQuery;
    log(QStringLiteral("source query #%1 for %2 (\"%3\" + %4)").arg(f.queries).arg(f.name, qm.text, sha1Urn(f.hashes.sha1)));
}

mx::RangeSource *GnutellaPlugin::openSource(const QString &downloadId, const mx::FileHashes &h, qint64 size,
                                            const mx::SearchSource &src)
{
    if (src.net != mx::Net::Gnutella && src.net != mx::Net::Unknown) return nullptr;
    SourceDesc d;
    if (!SourceDesc::fromUri(src.uri, &d)) {
        // fall back to user.address "ip:port" + N2R by SHA-1
        d.addr = HostPort::parse(src.user.address, 6346);
        if (!d.addr.isValid() || h.sha1.isEmpty()) return nullptr;
        d.sha1 = h.sha1;
    }
    if (!h.sha1.isEmpty() && !d.sha1.isEmpty() && h.sha1 != d.sha1) return nullptr;
    if (h.sha1.isEmpty() && (d.index == 0 || d.name.isEmpty()) && d.sha1.isEmpty()) return nullptr;
    if (d.guid.size() == 16) {
        // newer push proxies learned since the hit
        auto it = m_servents.constFind(guidHex(d.guid));
        if (it != m_servents.constEnd() && !it->proxies.isEmpty()) d.proxies = it->proxies;
    }
    auto *s = new GnutSource(this, downloadId, h, size, d, src, m_alive, nullptr);
    m_sources << s;
    for (int i = int(m_sources.size()) - 1; i >= 0; --i) if (!m_sources[i]) m_sources.removeAt(i);
    return s;
}

bool GnutellaPlugin::requestPush(GnutSource *s, const SourceDesc &d)
{
    if (firewalled() || d.guid.size() != 16) return false;
    const HostPort self = selfAddress();
    if (!self.isValid()) return false;
    const QList<UltrapeerConnection *> ups = connectedUps();
    if (ups.isEmpty() && d.proxies.isEmpty()) return false;
    PushWait w;
    w.src = s;
    w.deadline = QDateTime::currentMSecsSinceEpoch() + 45000;
    m_pushWaits.insert(d.guid, w);
    PushMsg p;
    p.serventGuid = d.guid;
    p.index = d.index;
    p.ip = self.ip;
    p.port = self.port;
    p.tls = tlsAdvertised();   // [enc-1] the pushing host may connect back with TLS
    const QByteArray payload = buildPushPayload(p);
    for (UltrapeerConnection *c : ups) c->send(Message::make(MsgPush, payload, 6));
    for (const HostPort &pp : d.proxies.mid(0, 3)) sendPushProxyRequest(pp, d.guid, d.index);
    log(QStringLiteral("PUSH request for %1 (%2) via %3 ultrapeers + %4 push proxies").arg(guidHex(d.guid), d.name.left(60))
            .arg(ups.size()).arg(qMin(3, int(d.proxies.size()))));
    return true;
}

void GnutellaPlugin::cancelPush(GnutSource *s)
{
    for (auto it = m_pushWaits.begin(); it != m_pushWaits.end();) {
        if (it->src == s || (!it->src && !it->browse)) it = m_pushWaits.erase(it);
        else ++it;
    }
}

void GnutellaPlugin::sendPushProxyRequest(const HostPort &proxy, const QByteArray &guid, quint32 index)
{
    QTcpSocket *sock = newSocket(false, this);   // [enc-1] plain in prefer mode, TLS only when required
    const HostPort self = selfAddress();
    const QByteArray req = "GET /gnutella/push-proxy?ServerId=" + mx::hash::toBase32(guid).toLatin1() +
                           "&file=" + QByteArray::number(index) + " HTTP/1.1\r\n"
                           "Host: " + proxy.toString().toLatin1() + "\r\n"
                           "User-Agent: " + m_userAgent.toUtf8() + "\r\n"
                           "X-Node: " + self.toString().toLatin1() + "\r\n"
                           "Connection: close\r\n\r\n";
    connect(sock, &QTcpSocket::connected, sock, [sock, req] { sock->write(req); });
    connect(sock, &QTcpSocket::readyRead, sock, [this, sock, proxy] {
        const QByteArray line = sock->readLine(256).trimmed();
        qCDebug(lcGnut).noquote() << "push-proxy" << proxy.toString() << "replied" << line;
        Q_UNUSED(this)
        sock->deleteLater();
    });
    connect(sock, &QTcpSocket::errorOccurred, sock, [sock](QAbstractSocket::SocketError) { sock->deleteLater(); });
    QTimer::singleShot(15000, sock, [sock] { sock->deleteLater(); });
    sock->connectToHost(QHostAddress(proxy.ip), proxy.port);
}

bool GnutellaPlugin::wantThex(const QString &downloadId)
{
    const int st = m_thexState.value(downloadId, 0);
    if (st != 0) return false;
    m_thexState.insert(downloadId, 1);
    return true;
}

void GnutellaPlugin::thexLearned(const QString &downloadId, const ThexTree &t)
{
    if (t.deepest.isEmpty()) { if (m_thexState.value(downloadId) == 1) m_thexState.remove(downloadId); return; }
    m_thexState.insert(downloadId, 2);
    mx::FileHashes more;
    more.tth = t.root;
    emit hashesLearned(downloadId, more);
    emit hashSetLearned(downloadId, QStringLiteral("tth-leaves"), t.unitSize, t.deepest);
}

void GnutellaPlugin::altLocsFound(const QString &downloadId, const QByteArray &sha1, const QList<HostPort> &direct,
                                  const QList<PushLoc> &pushed, const QList<HostPort> &directTls)
{
    Finder *f = m_finders.contains(downloadId) ? &m_finders[downloadId] : nullptr;
    QList<mx::SearchSource> out;
    static QHash<QString, QSet<QString>> s_known;     // for downloads without a finder
    QSet<QString> &known = f ? f->known : s_known[downloadId];
    for (const HostPort &h : direct) {
        if (!isRoutableIp(h.ip) && isRoutableIp(m_externalIp)) continue;
        SourceDesc d;
        d.addr = h;
        d.tls = directTls.contains(h);   // [enc-1] X-Alt "tls="
        d.sha1 = sha1;
        if (f) d.name = f->name;
        const QString k = d.userId() + QLatin1Char('|') + h.toString();
        if (known.contains(k)) continue;
        known.insert(k);
        out << makeSource(d, 0, QueryHit());
    }
    for (const PushLoc &p : pushed) {
        SourceDesc d;
        d.guid = p.guid;
        d.firewalled = true;
        d.proxies = p.proxies;
        d.sha1 = sha1;
        if (f) d.name = f->name;
        const QString k = d.userId() + QLatin1Char('|');
        if (known.contains(k)) continue;
        known.insert(k);
        out << makeSource(d, 0, QueryHit());
    }
    if (known.size() > 2000) known.clear();
    if (!out.isEmpty()) emit sourcesFound(downloadId, out);
}

void GnutellaPlugin::browse(quint32 requestId, const mx::UserRef &u)
{
    SourceDesc d;
    const QByteArray g = guidFromHex(u.id);
    if (g.size() == 16 && m_servents.contains(guidHex(g))) d = m_servents.value(guidHex(g));
    if (!d.addr.isValid()) d.addr = HostPort::parse(u.address.isEmpty() ? u.id : u.address, 6346);
    if (d.guid.isEmpty()) d.guid = g;
    if (!d.addr.isValid() && d.guid.size() != 16) { emit browseFailed(requestId, u, QStringLiteral("unknown user")); return; }
    auto *b = new BrowseClient(d.addr, m_userAgent, this);
    const bool peerTls = d.tls;
    b->setSocketFactory([this, peerTls](QObject *parent) { return newSocket(peerTls, parent); });   // [enc-1]
    m_browses.insert(requestId, b);
    connect(b, &BrowseClient::entries, this, [this, requestId, u, b](const QList<mx::BrowseEntry> &batch, bool finished) {
        emit browseResults(requestId, u, batch, finished);
        if (finished) { m_browses.remove(requestId); b->deleteLater(); }
    });
    connect(b, &BrowseClient::failed, this, [this, requestId, u, b](const QString &why) {
        emit browseFailed(requestId, u, why);
        m_browses.remove(requestId);
        b->deleteLater();
    });
    const bool direct = d.addr.isValid() && !d.firewalled;
    if (direct || firewalled() || d.guid.size() != 16) {
        if (!d.addr.isValid()) { b->abort(); return; }
        b->start();
        return;
    }
    // firewalled host: push, then browse on the GIV connection
    const HostPort self = selfAddress();
    if (!self.isValid() || connectedUps().isEmpty()) { b->start(); return; }
    PushWait w;
    w.browse = b;
    w.deadline = QDateTime::currentMSecsSinceEpoch() + 45000;
    m_pushWaits.insert(d.guid, w);
    PushMsg p;
    p.serventGuid = d.guid;
    p.index = 0;
    p.ip = self.ip;
    p.port = self.port;
    p.tls = tlsAdvertised();   // [enc-1]
    for (UltrapeerConnection *c : connectedUps()) c->send(Message::make(MsgPush, buildPushPayload(p), 6));
    for (const HostPort &pp : d.proxies.mid(0, 3)) sendPushProxyRequest(pp, d.guid, 0);
    QPointer<BrowseClient> bp(b);
    QTimer::singleShot(45000, this, [bp] { if (bp && bp->received() == 0) bp->abort(); });
}

} // namespace mx::gnut
