// SPDX-License-Identifier: GPL-3.0-or-later
// Gnutella plugin: incoming connections, answering queries, uploads, PUSH, THEX cache.
#include "gnut_plugin_impl.h"
#include "gnut_huge.h"

#include "mx/settings_keys.h"
#include "mxhash/encoding.h"
#include "mxhash/tiger_tree.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QRunnable>
#include <QtCore/QThreadPool>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

#include <algorithm>

namespace mx::gnut {

static constexpr int kMaxSniffing = 64;
static constexpr int kMaxSessions = 200;
static constexpr int kQueueMax = 32;

// ---------------------------------------------------------------- reachability
bool GnutellaPlugin::firewalled() const
{
    const QString mode = setting(QString(keys::connection::Firewalled), QStringLiteral("auto")).toString();
    if (!setting(QString(keys::connection::AcceptIncoming), true).toBool()) return true;
    if (mode == QLatin1String("open")) return false;
    if (mode == QLatin1String("firewalled")) return true;
    return !m_reachable;
}

HostPort GnutellaPlugin::selfAddress() const
{
    HostPort h;
    h.ip = m_externalIp;
    h.port = m_port;
    return h;
}

void GnutellaPlugin::noteReachable(quint32 peerIp, const QString &how)
{
    if (m_reachable) return;
    // a connection from the outside proves we are reachable; a private peer only counts when our
    // ultrapeers are private too (LAN / loopback tests)
    bool privateUps = false;
    for (UltrapeerConnection *c : m_conns) if (!isRoutableIp(c->address().ip)) privateUps = true;
    if (!isRoutableIp(peerIp) && !privateUps) return;
    m_reachable = true;
    m_reachableHow = how;
    m_browseCache.clear();
    log(QStringLiteral("incoming connection (%1) from %2: not firewalled").arg(how, ipToString(peerIp)));
    emitStatus();
}

void GnutellaPlugin::onNewConnection()
{
    while (m_server && m_server->hasPendingConnections()) {
        QTcpSocket *s = m_server->nextPendingConnection();
        if (!s) break;
        if (m_sniff.size() + m_tlsHandshaking.size() >= kMaxSniffing || m_sessions.size() >= kMaxSessions ||
            !setting(QString(keys::connection::AcceptIncoming), true).toBool()) {
            s->abort();
            s->deleteLater();
            continue;
        }
        s->setParent(this);
        beginSniff(s);
    }
}

void GnutellaPlugin::beginSniff(QTcpSocket *s)
{
    {
        m_sniff.insert(s, QByteArray());
        connect(s, &QTcpSocket::readyRead, this, [this, s] { onSniffReadyRead(s); });
        connect(s, &QTcpSocket::disconnected, this, [this, s] {
            if (!m_sniff.contains(s)) return;
            // connect-back probes sometimes close right after "\n\n"
            const QByteArray b = m_sniff.take(s);
            if (b.trimmed().isEmpty() || b.startsWith("CONNECT BACK")) {
                ++m_stats.connectBacks;
                noteReachable(s->peerAddress().toIPv4Address(), QStringLiteral("TCP connect-back"));
            }
            s->deleteLater();
        });
        QTimer::singleShot(20000, s, [this, s] {
            if (m_sniff.remove(s)) { s->abort(); s->deleteLater(); }
        });
    }
}

// [enc-1] an incoming TLS ClientHello: finish the handshake, then sniff the decrypted stream
void GnutellaPlugin::onTlsIncoming(QTcpSocket *s, const QByteArray &first)
{
    m_sniff.remove(s);
    s->disconnect(this);
    auto *t = new TlsSocket(m_tls, TlsSocket::TlsOnly, this);
    m_tlsHandshaking.insert(t);
    connect(t, &TlsSocket::encrypted, this, [this, t] {
        if (!m_tlsHandshaking.remove(t)) return;
        t->disconnect(this);
        qCDebug(lcGnut).noquote() << "incoming TLS from" << t->peerAddress().toString() << t->cipherName();
        beginSniff(t);
        if (t->bytesAvailable() > 0) onSniffReadyRead(t);
    });
    connect(t, &QTcpSocket::errorOccurred, this, [this, t](QAbstractSocket::SocketError) {
        if (!m_tlsHandshaking.remove(t)) return;
        qCDebug(lcGnut).noquote() << "incoming TLS from" << t->peerAddress().toString() << "failed:" << t->tlsError();
        t->disconnect(this);
        t->deleteLater();
    });
    t->startServer(s, first);
}

void GnutellaPlugin::onSniffReadyRead(QTcpSocket *s)
{
    auto it = m_sniff.find(s);
    if (it == m_sniff.end()) return;
    const QByteArray in = s->read(8192 - it->size());
    if (m_ctx.limiter) m_ctx.limiter->accountExempt(mx::RateLimiter::Down, mx::Net::Gnutella, in.size(), 0);   // [glue-3] [bw-2] first bytes of a connection (handshake or request): not charged
    // [enc-1] TLS on the same port: a ClientHello as the very first bytes
    const bool overTls = qobject_cast<TlsSocket *>(s) != nullptr;
    if (it->isEmpty() && !overTls && looksLikeTlsClientHello(in)) {
        if (tlsMode() == TlsMode::Off || !ensureTls()) {
            if (m_tls) ++m_tls->stats.inRefused;
            m_sniff.remove(s); s->disconnect(this); s->abort(); s->deleteLater();
            return;
        }
        noteReachable(s->peerAddress().toIPv4Address(), QStringLiteral("TLS"));
        onTlsIncoming(s, in);
        return;
    }
    it->append(in);
    const QByteArray b = *it;           // copy: the map entry is removed below
    const quint32 peerIp = s->peerAddress().toIPv4Address();
    const auto dropSock = [this, s] { m_sniff.remove(s); s->disconnect(this); s->abort(); s->deleteLater(); };
    if (!overTls && tlsMode() == TlsMode::Require &&
        (b.startsWith("GNUTELLA") || b.startsWith("GET ") || b.startsWith("HEAD ") || b.startsWith("GIV "))) {
        if (m_tls) ++m_tls->stats.inRefused;
        noteReachable(peerIp, QStringLiteral("plain, refused"));
        dropSock();                                    // [enc-1] Require encryption: no plain connections
        return;
    }
    if (b.startsWith("GNUTELLA CONNECT/")) {
        HeaderReader r(8192, 100, 4096);
        r.feed(b);
        if (r.error()) { dropSock(); return; }
        if (!r.complete()) { if (b.size() >= 8192) dropSock(); return; }
        m_sniff.remove(s);
        s->disconnect(this);
        noteReachable(peerIp, QStringLiteral("Gnutella handshake"));
        answerIncomingHandshake(s, r.block());
        return;
    }
    if (b.startsWith("GET ") || b.startsWith("HEAD ")) {
        m_sniff.remove(s);
        s->disconnect(this);
        noteReachable(peerIp, QStringLiteral("HTTP"));
        auto *u = new UploadSession(this, s, b, false, this);
        m_sessions.insert(u);
        connect(u, &UploadSession::finished, this, [this, u] { m_sessions.remove(u); });
        return;
    }
    if (b.startsWith("GIV ")) {
        const int end = int(b.indexOf("\n\n")) >= 0 ? int(b.indexOf("\n\n")) : int(b.indexOf("\r\n\r\n"));
        if (end < 0) { if (b.size() > 2048) dropSock(); return; }
        m_sniff.remove(s);
        s->disconnect(this);
        noteReachable(peerIp, QStringLiteral("GIV"));
        handleGiv(s, b.left(end));
        return;
    }
    if (b.startsWith("CONNECT BACK") || b.startsWith("\n") || b.startsWith("\r\n")) {
        ++m_stats.connectBacks;
        noteReachable(peerIp, QStringLiteral("TCP connect-back"));
        dropSock();
        return;
    }
    if (b.size() >= 16 && !b.startsWith("GNUTELLA") && !b.startsWith("GE") && !b.startsWith("HE") && !b.startsWith("GI")) {
        dropSock();
    }
}

void GnutellaPlugin::answerIncomingHandshake(QTcpSocket *s, const HeaderBlock &h)
{
    ++m_stats.incomingHandshakes;
    const QString agent = h.first(QStringLiteral("User-Agent")).left(80);
    if (m_stats.incomingAgents.size() < 50) m_stats.incomingAgents << agent;
    HeaderBlock r;
    r.firstLine = QStringLiteral("GNUTELLA/0.6 503 I am a leaf");
    r.add(QStringLiteral("User-Agent"), m_userAgent);
    r.add(QStringLiteral("X-Ultrapeer"), QStringLiteral("False"));
    r.add(QStringLiteral("Remote-IP"), s->peerAddress().toString());
    QStringList ups;
    for (UltrapeerConnection *c : connectedUps()) if (isRoutableIp(c->address().ip)) ups << c->id();
    for (const HostPort &g : m_hosts.goodHosts(10)) if (!ups.contains(g.toString()) && ups.size() < 10) ups << g.toString();
    if (!ups.isEmpty()) r.add(QStringLiteral("X-Try-Ultrapeers"), ups.join(QLatin1Char(',')));
    s->write(r.serialize());
    log(QStringLiteral("answered incoming handshake from %1 (%2) with 503 + %3 ultrapeers").arg(s->peerAddress().toString(), agent).arg(ups.size()));
    connect(s, &QTcpSocket::bytesWritten, s, [s] { if (s->bytesToWrite() == 0) s->disconnectFromHost(); });
    connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
    QTimer::singleShot(5000, s, [s] { s->abort(); s->deleteLater(); });
}

void GnutellaPlugin::handleGiv(QTcpSocket *s, const QByteArray &lineIn)
{
    ++m_stats.givsReceived;
    // "GIV <index>:<servent guid hex>/<file name>"
    const QByteArray line = lineIn.trimmed().mid(4);
    const int colon = int(line.indexOf(':'));
    const int slash = int(line.indexOf('/', colon + 1));
    QByteArray g;
    if (colon > 0) g = guidFromHex(QString::fromLatin1(line.mid(colon + 1, (slash > 0 ? slash : int(line.size())) - colon - 1)));
    if (g.size() != 16) { s->abort(); s->deleteLater(); return; }
    // the GIV may carry bytes after the blank line? (no: the pusher waits for our request)
    auto it = m_pushWaits.find(g);
    while (it != m_pushWaits.end() && it.key() == g) {
        PushWait w = *it;
        it = m_pushWaits.erase(it);
        if (w.src) { w.src->acceptPushedSocket(s); return; }
        if (w.browse) { w.browse->startWithSocket(s); return; }
    }
    qCDebug(lcGnut).noquote() << "unexpected GIV from" << s->peerAddress().toString();
    s->abort();
    s->deleteLater();
}

// ---------------------------------------------------------------- shared files
void GnutellaPlugin::setSharedFiles(const QList<mx::SharedFile> &files)
{
    m_shared.clear();
    for (const mx::SharedFile &f : files) {
        if (f.size <= 0 || f.name.isEmpty()) continue;
        m_shared << f;
    }
    rebuildShareIndex();
    scheduleQrpUpdate();
}

void GnutellaPlugin::rebuildShareIndex()
{
    m_sharedLower.clear();
    m_byIndex.clear();
    m_bySha1.clear();
    m_byTth.clear();
    m_browseCache.clear();
    m_qrp.clear();
    for (int i = 0; i < m_shared.size(); ++i) {
        const mx::SharedFile &f = m_shared[i];
        quint32 idx = m_pathIndex.value(f.path, 0);
        if (!idx) { idx = m_nextIndex++; m_pathIndex.insert(f.path, idx); }
        m_byIndex.insert(idx, i);
        if (f.hashes.sha1.size() == 20) {
            m_bySha1.insert(f.hashes.sha1, i);
            m_qrp.addUrn(sha1Urn(f.hashes.sha1));
        }
        if (f.hashes.tth.size() == 24) m_byTth.insert(f.hashes.tth, i);
        m_sharedLower << splitKeywords(f.name).join(QLatin1Char(' '));
        m_qrp.addFileName(f.name);
    }
    m_qrpDigest = QCryptographicHash::hash(QByteArray(reinterpret_cast<const char *>(m_qrp.present().bits()), (m_qrp.nslots() + 7) / 8),
                                           QCryptographicHash::Sha1);
}

void GnutellaPlugin::scheduleQrpUpdate() { if (!m_qrpTimer.isActive()) m_qrpTimer.start(); }

bool GnutellaPlugin::sharedBySha1(const QByteArray &sha1, mx::SharedFile *out)
{
    auto it = m_bySha1.constFind(sha1);
    if (it != m_bySha1.constEnd()) { *out = m_shared[*it]; return true; }
    // a running download's verified data (partial file sharing) is served by the library too
    if (m_ctx.library) {
        mx::FileHashes h;
        h.sha1 = sha1;
        return m_ctx.library->findByHash(h, out) && out->size > 0;
    }
    return false;
}

bool GnutellaPlugin::sharedByTth(const QByteArray &tth, mx::SharedFile *out)
{
    auto it = m_byTth.constFind(tth);
    if (it == m_byTth.constEnd()) return false;
    *out = m_shared[*it];
    return true;
}

bool GnutellaPlugin::sharedByIndex(quint32 index, mx::SharedFile *out)
{
    auto it = m_byIndex.constFind(index);
    if (it == m_byIndex.constEnd()) return false;
    *out = m_shared[*it];
    return true;
}

quint32 GnutellaPlugin::indexOf(const mx::SharedFile &f) { return m_pathIndex.value(f.path, 0); }

bool GnutellaPlugin::browseAllowed(const HostPort &peer)
{
    Q_UNUSED(peer)
    if (!setting(QString(keys::gnutella::BrowseHost), true).toBool()) return false;
    // "hotlist" cannot be mapped to anonymous Gnutella peers: only "everyone" allows browsing
    const QString who = setting(QString(keys::messages::AllowBrowse), QStringLiteral("everyone")).toString();
    return who.isEmpty() || who == QLatin1String("everyone");
}

QList<QByteArray> GnutellaPlugin::browsePackets()
{
    ++m_stats.browsesServed;
    if (!m_browseCache.isEmpty() || m_shared.isEmpty()) return m_browseCache;
    HitTrailer t;
    t.serventGuid = m_guid;
    t.firewalled = firewalled();
    t.busy = m_slotHolders.size() >= uploadSlots();
    t.uploaded = m_uploadsDone > 0;
    t.browseHost = true;
    t.tls = tlsAdvertised();   // [enc-1]
    for (UltrapeerConnection *c : connectedUps()) if (c->pushProxyAddr.isValid()) t.pushProxies << c->pushProxyAddr;
    const QByteArray guid = randomGuid(true);
    QList<HitResult> batch;
    const auto flush = [&] {
        if (batch.isEmpty()) return;
        Message m = Message::make(MsgQueryHit, buildQueryHitPayload(selfAddress(), ourSpeedKbps(), batch, t), 1, guid);
        m_browseCache << m.encode();
        batch.clear();
    };
    for (int i = 0; i < m_shared.size() && i < 20000; ++i) {
        const mx::SharedFile &f = m_shared[i];
        HitResult r;
        r.index = m_pathIndex.value(f.path);
        r.size = quint64(f.size);
        r.name = f.name;
        r.hashes.sha1 = f.hashes.sha1;
        r.hashes.tth = f.hashes.tth;
        batch << r;
        if (batch.size() >= 16) flush();
    }
    flush();
    return m_browseCache;
}

// ---------------------------------------------------------------- answering queries
void GnutellaPlugin::onQuery(UltrapeerConnection *c, const Message &m)
{
    if (m_seenQueries.contains(m.guid)) return;         // duplicate (several ultrapeers route the same query)
    ++m_stats.queriesSeen;
    emit incomingSearches(1);                           // [glue-2] SPM figure
    m_seenQueries.insert(m.guid);
    m_seenOrder << m.guid;
    while (m_seenOrder.size() > 20000) m_seenQueries.remove(m_seenOrder.takeFirst());
    if (m_shared.isEmpty() || m_queryGuids.contains(m.guid)) return;
    QueryMsg q;
    if (!parseQuery(m.payload, &q)) return;
    if (m_stats.queriesSeen <= 30)
        log(QStringLiteral("query routed to us by %1 (hops %2): \"%3\"%4").arg(c->id()).arg(m.hops).arg(q.effectiveText().left(60))
                .arg(q.sha1s.isEmpty() ? QString() : QStringLiteral(" + ") + sha1Urn(q.sha1s.first())));
    if (q.firewalled() && firewalled()) return;          // neither side could connect
    QList<int> matches;
    for (const QByteArray &sha1 : q.sha1s) {
        auto it = m_bySha1.constFind(sha1);
        if (it != m_bySha1.constEnd() && !matches.contains(*it)) matches << *it;
    }
    if (matches.isEmpty()) {
        const QString text = q.effectiveText();
        if (text.trimmed().isEmpty() || text == QLatin1String("\\")) return;   // index / urn-only queries
        const QStringList words = splitKeywords(text);
        int total = 0;
        for (const QString &w : words) total += int(w.size());
        if (words.isEmpty() || total < 2 || words.size() > 16) return;
        for (int i = 0; i < m_sharedLower.size() && matches.size() < 64; ++i) {
            const QString &n = m_sharedLower[i];
            bool all = true;
            for (const QString &w : words) if (!n.contains(w)) { all = false; break; }
            if (all) matches << i;
        }
    }
    if (matches.isEmpty()) return;
    HitTrailer t;
    t.serventGuid = m_guid;
    t.firewalled = firewalled();
    t.busy = m_slotHolders.size() >= uploadSlots();
    t.uploaded = m_uploadsDone > 0;
    t.browseHost = browseAllowed(HostPort());
    t.tls = tlsAdvertised();   // [enc-1]
    for (UltrapeerConnection *u : connectedUps()) if (u->pushProxyAddr.isValid()) t.pushProxies << u->pushProxyAddr;
    HostPort self = selfAddress();
    if (!self.ip) {
        bool ok = false;
        self.ip = ipFromString(c->remoteIpSeen(), &ok);
    }
    QList<HitResult> batch;
    int sent = 0;
    const quint8 ttl = quint8(qBound(1, int(m.hops) + 2, 10));
    const auto flush = [&] {
        if (batch.isEmpty()) return;
        c->send(Message::make(MsgQueryHit, buildQueryHitPayload(self, ourSpeedKbps(), batch, t), ttl, m.guid), true);
        sent += int(batch.size());
        batch.clear();
    };
    for (int i : matches) {
        const mx::SharedFile &f = m_shared[i];
        HitResult r;
        r.index = m_pathIndex.value(f.path);
        r.size = quint64(f.size);
        r.name = f.name;
        r.hashes.sha1 = f.hashes.sha1;
        r.hashes.tth = f.hashes.tth;
        if (!f.hashes.sha1.isEmpty()) {
            const QList<HostPort> alts = altLocsFor(f.hashes.sha1, HostPort());
            r.altLocs = alts.mid(0, 5);
        }
        batch << r;
        if (batch.size() >= 10) flush();
    }
    flush();
    ++m_stats.queriesAnswered;
    m_stats.hitsSent += sent;
    log(QStringLiteral("answered query \"%1\" from %2 with %3 results").arg(q.effectiveText().left(60), c->id()).arg(sent));
    qCDebug(lcGnut).noquote() << "answered query" << q.effectiveText().left(60) << "with" << sent << "results via" << c->id();
}

// ---------------------------------------------------------------- PUSH (we are the target)
void GnutellaPlugin::onPush(UltrapeerConnection *c, const Message &m)
{
    Q_UNUSED(c)
    PushMsg p;
    if (!parsePush(m.payload, &p) || p.serventGuid != m_guid) return;
    ++m_stats.pushesReceived;
    const HostPort target{p.ip, p.port};
    if (!target.isValid() || isBogonIp(target.ip)) return;
    if (!isRoutableIp(target.ip) && isRoutableIp(c->address().ip)) return;   // private address from the Internet
    const QString key = target.toString() + QLatin1Char('/') + QString::number(p.index);
    if (m_recentPushes.contains(key) || m_pushConnecting >= 8) return;
    m_recentPushes.insert(key, nowSecs());
    mx::SharedFile f;
    QString name = QStringLiteral("browse");
    if (sharedByIndex(p.index, &f)) name = f.name;
    log(QStringLiteral("PUSH from %1 for index %2: connecting back%3").arg(target.toString()).arg(p.index)
            .arg(p.tls && tlsMode() != TlsMode::Off ? QStringLiteral(" with TLS") : QString()));
    QTcpSocket *s = newSocket(p.tls, this);   // [enc-1]
    ++m_pushConnecting;
    const QByteArray giv = "GIV " + QByteArray::number(p.index) + ':' + guidHex(m_guid).toLatin1() + '/' + name.toUtf8() + "\n\n";
    connect(s, &QTcpSocket::connected, this, [this, s, giv] {
        --m_pushConnecting;
        s->disconnect(this);
        s->write(giv);
        ++m_stats.givsSent;
        if (m_sessions.size() >= kMaxSessions) { s->abort(); s->deleteLater(); return; }
        auto *u = new UploadSession(this, s, QByteArray(), true, this);
        m_sessions.insert(u);
        connect(u, &UploadSession::finished, this, [this, u] { m_sessions.remove(u); });
    });
    connect(s, &QTcpSocket::errorOccurred, this, [this, s](QAbstractSocket::SocketError) {
        --m_pushConnecting;
        s->disconnect(this);
        s->deleteLater();
    });
    s->connectToHost(QHostAddress(target.ip), target.port);
    QTimer::singleShot(20000, s, [s] { if (s->state() != QAbstractSocket::ConnectedState) s->abort(); });
}

// ---------------------------------------------------------------- upload slots / queue
int GnutellaPlugin::acquireSlot(UploadSession *s, const QString &key, const QString &parqId, QueueInfo *q)
{
    const qint64 now = nowSecs();
    bool changed = false;
    for (int i = int(m_uploadQueue.size()) - 1; i >= 0; --i)
        if (now - m_uploadQueue[i].lastPoll > 180) {
            const QString row = m_uploadQueue[i].uploadId;
            const bool live = m_uploadQueue[i].session && m_uploadQueue[i].session->uploadId() == row;
            m_uploadQueue.removeAt(i);
            if (!row.isEmpty() && !live) reportUploadFinished(row);   // [glue-3] gone: drop the Queued row
            changed = true;
        }
    if (m_slotHolders.contains(s)) return 1;
    {   // [glue-3] cancelled locally a moment ago: refuse
        const auto b = m_uploadBlocked.constFind(key);
        if (b != m_uploadBlocked.constEnd()) {
            if (*b > now) return -1;
            m_uploadBlocked.remove(key);
        }
    }
    int pos = -1;
    for (int i = 0; i < m_uploadQueue.size(); ++i)
        if (m_uploadQueue[i].key == key || (!parqId.isEmpty() && m_uploadQueue[i].id == parqId)) { pos = i; break; }
    const int nslots = uploadSlots();
    const int free = nslots - int(m_slotHolders.size());
    const bool forced = pos >= 0 && m_uploadQueue[pos].forced;   // [glue-3] "Start Transfer"
    if (forced || (free > 0 && (pos < 0 ? m_uploadQueue.size() < free : pos < free))) {
        if (pos >= 0) {
            q->rowId = m_uploadQueue[pos].uploadId;   // the session continues the queued row
            m_uploadQueue.removeAt(pos);
            for (QueueEntry &o : m_uploadQueue) o.reportedRank = -1;
            changed = true;
        }
        m_slotHolders.insert(s);
        if (changed) reportUploadQueue();
        return 1;
    }
    if (pos < 0) {
        if (m_uploadQueue.size() >= setting(QStringLiteral("gnutella/uploadQueueSize"), kQueueMax).toInt()) {
            if (changed) reportUploadQueue();
            return -1;
        }
        QueueEntry e;
        e.key = key;
        e.id = QString::fromLatin1(randomGuid(false).toHex()).toUpper();
        e.uploadId = s->uploadId();   // the asking session's row becomes the queue row
        m_uploadQueue << e;
        sortUploadQueue();
        for (int i = 0; i < m_uploadQueue.size(); ++i)
            if (m_uploadQueue[i].id == e.id) pos = i;
        changed = true;
    }
    QueueEntry &e = m_uploadQueue[pos];
    e.session = s;
    e.lastPoll = now;
    if (e.uploadId.isEmpty()) e.uploadId = s->uploadId();
    q->rowId = e.uploadId;
    e.info = s->transferInfo();
    e.info.id = e.uploadId;
    e.reportedRank = pos + 1;   // the session reports its own row right now
    q->present = true;
    q->position = pos + 1;
    q->length = int(m_uploadQueue.size());
    q->limit = nslots;
    q->pollMin = qBound(1, setting(QStringLiteral("gnutella/uploadQueuePollSec"), 30).toInt(), 40);
    q->pollMax = q->pollMin * 4;
    q->lifetime = q->pollMax;
    q->id = e.id;
    if (changed) reportUploadQueue();
    return 0;
}

// ---------------------------------------------------------------- [glue-3] queue rows + control
void GnutellaPlugin::sortUploadQueue()
{
    std::stable_sort(m_uploadQueue.begin(), m_uploadQueue.end(),
                     [](const QueueEntry &a, const QueueEntry &b) { return a.priority > b.priority; });
}

void GnutellaPlugin::reportUploadQueue()
{
    const int n = int(m_uploadQueue.size());
    for (int i = 0; i < n; ++i) {
        QueueEntry &e = m_uploadQueue[i];
        if (e.uploadId.isEmpty() || e.reportedRank == i + 1) continue;
        e.reportedRank = i + 1;
        mx::TransferInfo t = e.info;
        t.id = e.uploadId;
        t.isUpload = true;
        t.state = mx::TransferState::Queued;
        t.statusText = QStringLiteral("Queued (%1 of %2)").arg(i + 1).arg(n);
        t.queuePosition = i + 1;
        t.priority = e.priority;
        t.rateUpBps = 0;
        if (!t.sources.isEmpty()) t.sources[0].statusText = t.statusText;
        reportUpload(t);
    }
}

QStringList GnutellaPlugin::queuedUploadIds() const
{
    QStringList out;
    for (const QueueEntry &e : m_uploadQueue) out << e.uploadId;
    return out;
}

bool GnutellaPlugin::startUploadNow(const QString &uploadId)
{
    for (QueueEntry &e : m_uploadQueue) {
        if (e.uploadId != uploadId) continue;
        // HTTP: the client must ask again; it gets the slot at its next poll (≤ pollMin seconds)
        e.forced = true;
        e.priority = 1;
        sortUploadQueue();
        for (QueueEntry &o : m_uploadQueue) o.reportedRank = -1;
        reportUploadQueue();
        return true;
    }
    const QPointer<UploadSession> s = m_uploads.value(uploadId);
    return s && s->transferring();
}

bool GnutellaPlugin::setUploadPriority(const QString &uploadId, int priority)
{
    for (QueueEntry &e : m_uploadQueue) {
        if (e.uploadId != uploadId) continue;
        e.priority = qBound(-1, priority, 1);
        sortUploadQueue();
        for (QueueEntry &o : m_uploadQueue) o.reportedRank = -1;
        reportUploadQueue();
        return true;
    }
    return m_uploads.value(uploadId) != nullptr;   // a running upload: nothing to reorder
}

void GnutellaPlugin::releaseSlot(UploadSession *s)
{
    if (m_slotHolders.remove(s)) ++m_uploadsDone;
    for (int i = int(m_uploadQueue.size()) - 1; i >= 0; --i)
        if (m_uploadQueue[i].session == s && m_uploadQueue[i].session) { /* keep: PARQ clients reconnect */ }
}

void GnutellaPlugin::cancelUpload(const QString &uploadId)
{
    for (int i = 0; i < m_uploadQueue.size(); ++i) {   // [glue-3] a queued client
        if (m_uploadQueue[i].uploadId != uploadId) continue;
        const QueueEntry e = m_uploadQueue.takeAt(i);
        m_uploadBlocked.insert(e.key, nowSecs() + 600);
        if (e.session && e.session->uploadId() == uploadId) e.session->cancel();
        else reportUploadFinished(uploadId);
        for (QueueEntry &o : m_uploadQueue) o.reportedRank = -1;
        reportUploadQueue();
        return;
    }
    const QPointer<UploadSession> s = m_uploads.value(uploadId);
    if (s) {
        if (!s->slotKey().isEmpty()) m_uploadBlocked.insert(s->slotKey(), nowSecs() + 600);
        s->cancel();
    }
}

void GnutellaPlugin::reportUploadFinished(const QString &id)
{
    m_uploads.remove(id);
    emit uploadFinished(id);
}

void GnutellaPlugin::noteIncomingHttp(const HostPort &peer, const QString &what)
{
    ++m_stats.incomingHttp;
    qCInfo(lcGnut).noquote() << "HTTP from" << peer.toString() << what;
}

void GnutellaPlugin::reportUpload(const mx::TransferInfo &t)
{
    if (!m_uploads.contains(t.id)) {
        for (UploadSession *s : std::as_const(m_sessions))
            if (s->uploadId() == t.id) { m_uploads.insert(t.id, s); break; }
    }
    emit uploadChanged(t);
}

QList<HostPort> GnutellaPlugin::altLocsFor(const QByteArray &sha1, const HostPort &exclude)
{
    QList<HostPort> out;
    for (const HostPort &h : m_altLocs.value(sha1)) if (h.ip != exclude.ip) out << h;
    return out;
}

void GnutellaPlugin::addAltLocs(const QByteArray &sha1, const QList<HostPort> &hosts)
{
    if (!m_bySha1.contains(sha1)) return;
    QList<HostPort> &l = m_altLocs[sha1];
    for (const HostPort &h : hosts) {
        if (!isRoutableIp(h.ip) || l.contains(h)) continue;
        l.prepend(h);
    }
    while (l.size() > 20) l.removeLast();
    if (m_altLocs.size() > 1000) m_altLocs.clear();
}

// ---------------------------------------------------------------- THEX cache
namespace {
class ThexJob : public QRunnable {
public:
    ThexJob(mx::LibraryAccess *lib, mx::SharedFile f, std::function<void(QByteArray, QByteArray)> done)
        : m_lib(lib), m_f(std::move(f)), m_done(std::move(done)) {}
    void run() override
    {
        mx::hash::TigerTree tree(1 << 17);
        qint64 off = 0;
        const qint64 chunk = 1024 * 1024;
        while (off < m_f.size) {
            const QByteArray d = m_lib->read(m_f.hashes, off, qMin(chunk, m_f.size - off));
            if (d.isEmpty()) { m_done(QByteArray(), QByteArray()); return; }
            tree.update(d);
            off += d.size();
        }
        const QByteArray root = tree.finish();
        const int depth = thexGoodDepth(m_f.size);
        QList<QList<QByteArray>> levels;
        for (int dd = 0; dd <= depth; ++dd) {
            const QList<QByteArray> l = tree.level(dd);
            if (l.isEmpty()) break;
            levels << l;
        }
        m_done(root, buildThex(m_f.size, levels));
    }
private:
    mx::LibraryAccess *m_lib;
    mx::SharedFile m_f;
    std::function<void(QByteArray, QByteArray)> m_done;
};
} // namespace

QByteArray GnutellaPlugin::tthFor(const mx::SharedFile &f)
{
    if (f.hashes.tth.size() == 24) return f.hashes.tth;
    auto it = m_thex.constFind(f.hashes.sha1);
    return it == m_thex.constEnd() ? QByteArray() : it->root;
}

QByteArray GnutellaPlugin::thexFor(const mx::SharedFile &f, int *retryAfter)
{
    *retryAfter = 0;
    if (f.hashes.sha1.isEmpty() || !m_ctx.library) return {};
    ThexEntry &e = m_thex[f.hashes.sha1];
    e.lastUse = nowSecs();
    if (!e.dime.isEmpty()) return e.dime;
    if (!e.computing) {
        e.computing = true;
        std::shared_ptr<bool> alive = m_alive;
        QPointer<GnutellaPlugin> self(this);
        const QByteArray sha1 = f.hashes.sha1;
        auto *job = new ThexJob(m_ctx.library, f, [alive, self, sha1](QByteArray root, QByteArray dime) {
            if (!*alive) return;
            QMetaObject::invokeMethod(self.data(), [self, alive, sha1, root, dime] {
                if (!self || !*alive) return;
                ThexEntry &x = self->m_thex[sha1];
                x.computing = false;
                x.root = root;
                x.dime = dime;
                qCInfo(lcGnut).noquote() << "THEX tree ready for" << sha1Urn(sha1) << dime.size() << "bytes";
            }, Qt::QueuedConnection);
        });
        QThreadPool::globalInstance()->start(job);
        // keep the cache small
        if (m_thex.size() > 64) {
            auto oldest = m_thex.begin();
            for (auto it = m_thex.begin(); it != m_thex.end(); ++it)
                if (!it->computing && it->lastUse < oldest->lastUse) oldest = it;
            if (!oldest->computing && oldest.key() != f.hashes.sha1) m_thex.erase(oldest);
        }
    }
    *retryAfter = int(qBound<qint64>(1LL, 1LL + f.size / (50LL * 1024 * 1024), 60LL));   // ~50 MB/s hashing
    return {};
}

} // namespace mx::gnut
