// SPDX-License-Identifier: GPL-3.0-or-later
// Net::Rats: see rats_search_plugin.h and docs/research-librats.md §2 for the wire format.
#include "rats_search_plugin.h"

#include "ovl_util.h"

#include <QtCore/QDir>
#include <QtCore/QJsonArray>

#include "mx/settings_keys.h"

namespace mx::ovl {

namespace {

constexpr int kMaxActiveSearches = 6;
constexpr int kQueryLimit = 50;             // per peer, per query type (rats GUI sends 50)
constexpr int kMaxHashPeers = 20000;

QString ratsContentType(MediaType t)
{
    switch (t) {
    case MediaType::Audio: return QStringLiteral("audio");
    case MediaType::Video: return QStringLiteral("video");
    case MediaType::Image: return QStringLiteral("pictures");
    case MediaType::Document: return QStringLiteral("books");
    case MediaType::Software: return QStringLiteral("software");
    case MediaType::Archive: return QStringLiteral("archive");
    default: return QString();
    }
}

MediaType mediaFromContentType(const QString &ct, const QString &name)
{
    if (ct == QLatin1String("video")) return MediaType::Video;
    if (ct == QLatin1String("audio")) return MediaType::Audio;
    if (ct == QLatin1String("books")) return MediaType::Document;
    if (ct == QLatin1String("pictures")) return MediaType::Image;
    if (ct == QLatin1String("software") || ct == QLatin1String("games")) return MediaType::Software;
    if (ct == QLatin1String("archive")) return MediaType::Archive;
    const MediaType m = mediaTypeForFileName(name);
    return m == MediaType::Any ? MediaType::Other : m;
}

QString shortCount(qint64 n)
{
    if (n >= 1000000) return QStringLiteral("%1M").arg(n / 1e6, 0, 'f', 1);
    if (n >= 1000) return QStringLiteral("%1k").arg(n / 1e3, 0, 'f', 0);
    return QString::number(n);
}

} // namespace

RatsSearchPlugin::RatsSearchPlugin(QObject *parent) : RatsSearchApi(parent)
{
    m_flushTimer.setInterval(250);
    m_flushTimer.setSingleShot(true);
    connect(&m_flushTimer, &QTimer::timeout, this, &RatsSearchPlugin::flush);
    m_tickTimer.setInterval(1000);
    connect(&m_tickTimer, &QTimer::timeout, this, &RatsSearchPlugin::tick);
    m_lastStatus.net = Net::Rats;
    m_lastStatus.state = ConnState::Disconnected;
    // [core glue] the generic NetworkPlugin::bundleFiles answer mirrors torrentFilesReceived
    connect(this, &RatsSearchApi::torrentFilesReceived, this,
            [this](const QByteArray &btih, const QList<BundleFile> &files, qint64) {
                FileHashes h;
                h.btih = btih;
                Q_EMIT bundleFiles(h, files);
            });
    m_lastStatus.statusText = tr("Not connected");
}

RatsSearchPlugin::~RatsSearchPlugin()
{
    if (m_node) {
        m_node->stop();
        m_node->waitStopped(3000);
    }
}

QVariant RatsSearchPlugin::setting(const char *key, const QVariant &def) const
{
    if (!m_ctx.settings) return def;
    const QVariant v = m_ctx.settings->value(QString::fromLatin1(key));
    return v.isValid() && !(v.typeId() == QMetaType::QString && v.toString().isEmpty()) ? v : def;
}

bool RatsSearchPlugin::safeSearch() const
{
    // Safe search defaults to ON for the rats network (it carries a lot of adult content);
    // search/adultFilter = true forces it on as well.
    const bool adult = m_ctx.settings && m_ctx.settings->b(QString(keys::search::AdultFilter));
    return adult || setting(ratskeys::SafeSearch, true).toBool();
}

void RatsSearchPlugin::init(const PluginContext &ctx)
{
    m_ctx = ctx;
    if (m_dataDir.isEmpty()) {
        m_dataDir = ctx.dataDir;
        if (m_dataDir.isEmpty() && !ctx.profileDir.isEmpty()) m_dataDir = ctx.profileDir + QStringLiteral("/rats");
    }
}

void RatsSearchPlugin::start()
{
    if (m_running) return;
    RatsNodeConfig cfg;
    cfg.protocol = setting(ratskeys::Protocol, QString::fromLatin1(kDefaultRatsProtocol)).toString();
    cfg.listenPort = static_cast<quint16>(setting(ratskeys::ListenPort, 4444).toInt());
    cfg.dhtPort = static_cast<quint16>(setting(ratskeys::DhtPort, 0).toInt());
    cfg.dataDir = m_dataDir;
    cfg.maxPeers = setting(ratskeys::MaxPeers, 30).toInt();
    cfg.peerTarget = cfg.maxPeers;
    // [diag-1] the core's port mapper forwards our ports when it can (PluginContext::coreMapsPorts)
    cfg.portMapping = !m_ctx.coreMapsPorts && m_ctx.settings && m_ctx.settings->b(QString(keys::connection::Upnp));
    m_wantPort = cfg.listenPort;
    cfg.jsonTypes = {QStringLiteral("client_info"), QStringLiteral("searchTorrent_response"),
                     QStringLiteral("searchFiles_response"), QStringLiteral("torrent_response"),
                     QStringLiteral("searchTorrent"), QStringLiteral("searchFiles"), QStringLiteral("torrent"),
                     QStringLiteral("feed"), QStringLiteral("randomTorrents"), QStringLiteral("topTorrents")};
    if (!m_node) {
        m_node = new RatsNode(this);
        m_node->setMeter(m_ctx.limiter, Net::Rats);   // [glue-3] bandwidth graph
        connect(m_node, &RatsNode::peerConnected, this, &RatsSearchPlugin::onPeerConnected);
        connect(m_node, &RatsNode::peerDisconnected, this, &RatsSearchPlugin::onPeerDisconnected);
        connect(m_node, &RatsNode::peerRerouted, this, [this](const QString &id) {
            // messages sent over the link that lost a duplicate-connection race are lost: resend
            auto it = m_peers.find(id);
            if (it == m_peers.end()) return;
            it->queriesSent.clear();
            sendClientInfo(id);
            sendQueries(id);
        });
        connect(m_node, &RatsNode::jsonReceived, this, &RatsSearchPlugin::onJson);
    }
    QString err;
    if (!m_node->start(cfg, &err)) {
        m_lastError = err;
        m_lastStatus.state = ConnState::Error;
        m_lastStatus.statusText = tr("Cannot start: %1").arg(err);
        Q_EMIT statusChanged(m_lastStatus);
        Q_EMIT logMessage(QStringLiteral("Rats: cannot start: %1").arg(err));
        return;
    }
    m_running = true;
    m_startedAt.start();
    m_tickTimer.start();
    // [ui-h] Settings > Rats Search "Bootstrap peers": dialled at start, besides what the DHT finds
    for (const QString &b : setting("rats/bootstrapPeers", QStringList()).toStringList())
        if (b.contains(QLatin1Char(':')))
            m_node->connectTo(b.trimmed());
    qCInfo(lcRats) << "rats node" << m_node->nodeId().left(8) << "protocol" << cfg.protocol << "port"
                   << m_node->listenPort();
    Q_EMIT logMessage(QStringLiteral("Rats: listening on port %1 (%2)").arg(m_node->listenPort()).arg(cfg.protocol));
    updateStatus(true);
}

void RatsSearchPlugin::stop()
{
    if (!m_running) return;
    m_running = false;
    m_tickTimer.stop();
    if (m_node) m_node->stop();
    m_peers.clear();
    // finish every running search
    const QList<quint32> ids = m_searches.keys();
    for (quint32 id : ids) finish(id);
    m_searches.clear();
    updateStatus(true);
    Q_EMIT serversChanged();
}

// [diag-1] Connection Test: librats listens on one port for TCP and UDP, plus its DHT port
PluginDiagnostics RatsSearchPlugin::diagnostics() const
{
    PluginDiagnostics d;
    d.valid = true;
    if (!m_running || !m_node) return d;
    const int port = m_node->listenPort();
    for (const char *proto : {"TCP", "UDP"}) {
        PortBinding b;
        b.proto = QLatin1String(proto);
        b.port = port ? port : m_wantPort;
        b.ok = port != 0;
        if (port && m_wantPort && port != m_wantPort) b.error = QStringLiteral("port %1 busy, using %2").arg(m_wantPort).arg(port);
        b.purpose = QStringLiteral("rats-search peers");
        d.bindings << b;
    }
    const int dht = m_node->dhtPort();
    if (dht > 0 && dht != port) {
        PortBinding b;
        b.proto = QStringLiteral("UDP");
        b.port = dht;
        b.ok = true;
        b.purpose = QStringLiteral("rats DHT, random port");
        b.mapByCore = false;   // a new random port every start: nothing to forward
        d.bindings << b;
    }
    int inTcp = 0, inUdp = 0;
    for (const RatsPeer &p : m_node->peers()) {
        if (!p.inbound) continue;
        if (p.transport == QLatin1String("tcp")) ++inTcp;
        else if (p.transport == QLatin1String("udp")) ++inUdp;
    }
    if (inTcp) {
        d.tcpIn = Reach::Yes;
        d.tcpEvidence = QStringLiteral("%1 connected to us over TCP").arg(countNoun(inTcp, QStringLiteral("rats-search peer")));
    }
    if (inUdp) {
        d.udpIn = Reach::Yes;
        d.udpEvidence = QStringLiteral("%1 connected to us over UDP (may be a hole punch)").arg(countNoun(inUdp, QStringLiteral("rats-search peer")));
    }
    d.externalAddress = m_node->externalAddress();
    if (!d.externalAddress.isEmpty()) d.externalSeenBy = QStringLiteral("rats DHT / STUN");
    d.dhtNodes = m_node->dhtNodes();
    // [enc-1]
    d.items << DiagItem{QStringLiteral("rats.encryption"), QStringLiteral("Encryption"), DiagLevel::Ok, QStringLiteral("Always on"),
                        QStringLiteral("peer links use the Noise protocol; the rats DHT is plain UDP"), QString()};
    return d;
}

NetworkStatus RatsSearchPlugin::status() const
{
    return m_lastStatus;
}

void RatsSearchPlugin::updateStatus(bool force)
{
    NetworkStatus st;
    st.net = Net::Rats;
    if (!m_running) {
        st.state = m_lastError.isEmpty() ? ConnState::Disconnected : ConnState::Error;
        st.statusText = m_lastError.isEmpty() ? tr("Not connected") : m_lastError;
    } else {
        const int n = m_node ? m_node->peerCount() : 0;
        st.peers = n;
        st.listenPort = m_node ? m_node->listenPort() : 0;
        st.externalAddress = m_node ? m_node->externalAddress() : QString();
        qint64 torrents = 0;
        bool anyInbound = false;
        for (const RatsPeer &p : m_node->peers()) {
            if (p.inbound) anyInbound = true;
            auto it = m_peers.constFind(p.id);
            if (it != m_peers.constEnd() && it->torrents > 0) torrents += it->torrents;
        }
        st.filesOnline = torrents > 0 ? torrents : -1;
        st.usersOnline = -1;   // [core-5] rats-search peers are index nodes, not people online
        st.firewalled = !anyInbound && m_startedAt.elapsed() > 120000;
        if (n == 0) {
            st.state = ConnState::Connecting;
            st.statusText = tr("Connecting... (looking for rats-search peers)");
        } else {
            st.state = ConnState::Connected;
            st.statusText = torrents > 0 ? tr("%n peer(s), ~%1 torrents indexed", nullptr, n).arg(shortCount(torrents))
                                         : tr("%n peer(s)", nullptr, n);
        }
    }
    const bool changed = st.state != m_lastStatus.state || st.statusText != m_lastStatus.statusText ||
                         st.peers != m_lastStatus.peers || st.firewalled != m_lastStatus.firewalled;
    m_lastStatus = st;
    if (changed || force) Q_EMIT statusChanged(st);
}

QList<ServerEntry> RatsSearchPlugin::servers() const
{
    QList<ServerEntry> out;
    if (!m_node || !m_running) return out;
    for (const RatsPeer &p : m_node->peers()) {
        ServerEntry e;
        e.net = Net::Rats;
        e.id = p.id;
        const auto it = m_peers.constFind(p.id);
        const QString ver = it != m_peers.constEnd() ? it->version : QString();
        e.name = ver.isEmpty() ? QStringLiteral("rats peer %1").arg(p.id.left(8))
                               : QStringLiteral("Rats Search %1").arg(ver);
        e.address = p.address;
        e.state = ConnState::Connected;
        if (it != m_peers.constEnd()) {
            if (it->files >= 0) e.files = it->files;   // [core-5] files, not torrents
            e.description = QStringLiteral("%1 torrents, %2 files, %3%4")
                                .arg(it->torrents >= 0 ? shortCount(it->torrents) : QStringLiteral("?"))
                                .arg(it->files >= 0 ? shortCount(it->files) : QStringLiteral("?"))
                                .arg(p.inbound ? QStringLiteral("in") : QStringLiteral("out"))
                                .arg(p.transport.isEmpty() ? QString() : QStringLiteral("/") + p.transport);
        }
        out << e;
    }
    return out;
}

void RatsSearchPlugin::connectServer(const QString &id)
{
    if (m_node && id.contains(QLatin1Char(':'))) m_node->connectTo(id);
}

void RatsSearchPlugin::disconnectServer(const QString &id)
{
    if (m_node) m_node->disconnectPeer(id);
}

void RatsSearchPlugin::addServer(const QString &address, const QString &)
{
    if (m_node) m_node->connectTo(address);
}

// ------------------------------------------------------------------------------------- peers
void RatsSearchPlugin::sendClientInfo(const QString &peerId)
{
    QJsonObject info;
    const QString ver = m_ctx.clientVersion.isEmpty() ? QStringLiteral(MX_VERSION_STRING) : m_ctx.clientVersion;
    info[QStringLiteral("clientVersion")] = QStringLiteral("OctoMX %1").arg(ver);
    info[QStringLiteral("torrents")] = 0;
    info[QStringLiteral("files")] = 0;
    info[QStringLiteral("totalSize")] = 0;
    info[QStringLiteral("peersConnected")] = m_node ? m_node->peerCount() : 0;
    info[QStringLiteral("connectedAt")] = 0;
    info[QStringLiteral("databaseSharing")] = false;
    m_node->sendJson(peerId, QStringLiteral("client_info"), info);
}

void RatsSearchPlugin::onPeerConnected(const RatsPeer &p)
{
    PeerMeta &m = m_peers[p.id];
    m.address = p.address;
    m.connectedMs = nowMs();
    m.queriesSent.clear();
    sendClientInfo(p.id);
    sendQueries(p.id);   // peers trickle in: a broadcast only reached those connected at that moment
    m_serversDirty = true;   // status + servers are re-emitted by tick() (≤ 1 Hz)
}

void RatsSearchPlugin::onPeerDisconnected(const QString &id, const QString &)
{
    m_peers.remove(id);
    m_serversDirty = true;
}

void RatsSearchPlugin::sendQuery(const QString &peerId, const ActiveSearch &s)
{
    QJsonObject q;
    q[QStringLiteral("text")] = s.text;
    q[QStringLiteral("query")] = s.text;
    q[QStringLiteral("limit")] = kQueryLimit;
    q[QStringLiteral("index")] = 0;
    q[QStringLiteral("orderBy")] = QString();
    q[QStringLiteral("orderDesc")] = true;
    q[QStringLiteral("safeSearch")] = safeSearch();
    if (!s.contentType.isEmpty()) q[QStringLiteral("type")] = s.contentType;
    if (s.q.minSize > 0 || s.q.maxSize > 0) {
        QJsonObject size;
        if (s.q.minSize > 0) size[QStringLiteral("min")] = static_cast<double>(s.q.minSize);
        if (s.q.maxSize > 0) size[QStringLiteral("max")] = static_cast<double>(s.q.maxSize);
        q[QStringLiteral("size")] = size;
    }
    m_node->sendJson(peerId, QStringLiteral("searchTorrent"), q);
    // searchFiles matches paths inside torrents: useful for music/extension searches where the
    // torrent name rarely carries the words. Skip it for very generic one-word searches.
    m_node->sendJson(peerId, QStringLiteral("searchFiles"), q);
}

void RatsSearchPlugin::sendQueries(const QString &peerId)
{
    auto pit = m_peers.find(peerId);
    if (pit == m_peers.end() || !m_node) return;
    for (auto it = m_searches.cbegin(); it != m_searches.cend(); ++it) {
        if (it->done || pit->queriesSent.contains(it.key())) continue;
        pit->queriesSent.insert(it.key());
        sendQuery(peerId, it.value());
    }
}

// ------------------------------------------------------------------------------------ search
void RatsSearchPlugin::search(const SearchQuery &q)
{
    QString text = (q.text + QLatin1Char(' ') + q.text2).simplified();
    ActiveSearch s;
    s.q = q;
    // [ui-h] Settings > Rats Search "Maximum results": a cap of its own below the search's limit
    const int cap = setting("rats/maxResults", 500).toInt();
    if (cap > 0 && (s.q.maxResults <= 0 || s.q.maxResults > cap))
        s.q.maxResults = cap;
    s.text = text;
    s.words = queryWords(text);
    for (const QString &e : queryExtensions(q)) s.words.removeAll(e);
    s.contentType = ratsContentType(q.type);
    s.started.start();
    if (text.size() <= 2 || s.words.isEmpty() || !m_running) {
        // rats peers ignore queries of <= 2 characters
        const quint32 id = q.id;
        QTimer::singleShot(0, this, [this, id] { Q_EMIT searchDone(id); });
        return;
    }
    // bound concurrent searches: retire the oldest
    while (m_searches.size() >= kMaxActiveSearches) {
        quint32 oldest = 0;
        qint64 age = -1;
        for (auto it = m_searches.cbegin(); it != m_searches.cend(); ++it)
            if (it->started.elapsed() > age) { age = it->started.elapsed(); oldest = it.key(); }
        finish(oldest);
    }
    m_searches.insert(q.id, s);
    for (auto it = m_peers.begin(); it != m_peers.end(); ++it) {
        it->queriesSent.insert(q.id);
        sendQuery(it.key(), m_searches[q.id]);
    }
    qCInfo(lcRats) << "search" << q.id << text << "sent to" << m_peers.size() << "peers";
}

void RatsSearchPlugin::cancelSearch(quint32 queryId)
{
    m_searches.remove(queryId);
}

void RatsSearchPlugin::finish(quint32 qid)
{
    auto it = m_searches.find(qid);
    if (it == m_searches.end()) return;
    if (!it->pending.isEmpty()) {
        Q_EMIT searchHits(qid, it->pending);
        it->pending.clear();
    }
    qCInfo(lcRats) << "search" << qid << "done:" << it->emitted << "hits from" << it->peersAnswered << "peers,"
                   << it->replies << "raw replies in" << it->started.elapsed() << "ms";
    m_searches.erase(it);
    Q_EMIT searchDone(qid);
}

void RatsSearchPlugin::onJson(const QString &peerId, const QString &type, const QJsonObject &o)
{
    if (type == QLatin1String("searchTorrent_response")) return onHit(peerId, false, o);
    if (type == QLatin1String("searchFiles_response")) return onHit(peerId, true, o);
    if (type == QLatin1String("client_info")) {
        auto it = m_peers.find(peerId);
        if (it == m_peers.end()) return;
        it->gotInfo = true;
        it->version = cleanText(jStr(o, "clientVersion", 64), 64);
        const QJsonValue t = o.value(QStringLiteral("torrents"));
        it->torrents = t.isDouble() ? jInt(o, "torrents", -1) : jInt(o, "torrentsCount", -1);
        it->files = o.value(QStringLiteral("files")).isDouble() ? jInt(o, "files", -1) : jInt(o, "filesCount", -1);
        m_serversDirty = true;
        return;
    }
    if (type == QLatin1String("torrent_response")) {
        const QString hash = jStr(o, "hash", 64).toLower();
        if (!isHex(hash, 40)) return;
        QList<BundleFile> files;
        qint64 total = 0;
        const QJsonArray list = jArr(o, "files_list", 50000);
        int idx = 0;
        for (const QJsonValue &v : list) {
            if (!v.isObject()) continue;
            const QJsonObject f = v.toObject();
            BundleFile bf;
            bf.path = cleanText(jStr(f, "path", 1024), 1024);
            bf.size = qMax<qint64>(0, jInt(f, "size", 0));
            bf.index = idx++;
            total += bf.size;
            files << bf;
        }
        if (files.isEmpty()) return;
        Q_EMIT torrentFilesReceived(QByteArray::fromHex(hash.toLatin1()), files,
                                    jInt(o, "size", total) > 0 ? jInt(o, "size", total) : total);
        return;
    }
    // searchTorrent / searchFiles / torrent / feed / randomTorrents / topTorrents from peers:
    // we keep no index, so, exactly like a rats-search node with zero hits, we send nothing.
}

void RatsSearchPlugin::onHit(const QString &peerId, bool filesHit, const QJsonObject &o)
{
    if (m_searches.isEmpty()) return;
    QString hash = jStr(o, "hash", 64).toLower();
    if (hash.isEmpty()) hash = jStr(o, "info_hash", 64).toLower();
    if (!isHex(hash, 40)) return;
    const QString name = cleanText(jStr(o, "name", 512), 512);
    if (name.isEmpty()) return;
    const qint64 size = jInt(o, "size", 0);
    if (size < 0) return;
    const int files = static_cast<int>(qBound(qint64(0), jInt(o, "files", 1), qint64(10000000)));
    const QString ct = jStr(o, "contentType", 32).toLower();
    const QString cat = jStr(o, "contentCategory", 32).toLower();
    const QByteArray btih = QByteArray::fromHex(hash.toLatin1());

    // matching paths of a searchFiles hit (legacy "path", newer "matchingPaths")
    QStringList paths;
    if (filesHit) {
        for (const char *k : {"path", "matchingPaths"}) {
            for (const QJsonValue &v : jArr(o, k, 200))
                if (v.isString()) paths << cleanText(v.toString(), 1024);
        }
    }

    // remember who has it (for requestTorrentFiles)
    QStringList &hp = m_hashPeers[btih];
    if (hp.isEmpty()) {
        m_hashOrder << btih;
        if (m_hashOrder.size() > kMaxHashPeers) m_hashPeers.remove(m_hashOrder.takeFirst());
    }
    if (!hp.contains(peerId) && hp.size() < 8) hp << peerId;

    const bool safe = safeSearch();
    if (safe && (ct == QLatin1String("bad") || cat == QLatin1String("xxx") || looksAdult(name))) return;
    const MediaType mt = mediaFromContentType(ct, name);

    // Replies do not echo the query: attribute the hit to every running search whose words all
    // occur in the torrent name (searchTorrent) or in a matching path (searchFiles). Stray
    // replies (late answers to an earlier search) match nothing and are dropped.
    bool any = false;
    for (auto it = m_searches.begin(); it != m_searches.end(); ++it) {
        ActiveSearch &s = it.value();
        if (s.done) continue;
        ++s.replies;
        bool match = matchesWords(name, s.words);
        QString matchedPath;
        if (!match && filesHit) {
            for (const QString &p : paths)
                if (matchesWords(p, s.words)) { match = true; matchedPath = p; break; }
        }
        if (!match) continue;
        if (s.q.minSize > 0 && size < s.q.minSize) continue;
        if (s.q.maxSize > 0 && size > s.q.maxSize) continue;
        {
            // [ui-f] type and extension(s) by the shared rule: rats' content type (from the file list)
            // classifies the torrent, a matched path counts as a known file of it
            SearchHit probe;
            probe.fileName = name;
            probe.type = mt;
            probe.isBundle = files > 1;
            probe.hashes.btih = btih;
            if (!matchedPath.isEmpty() && probe.isBundle) probe.files << BundleFile{matchedPath, 0, -1};
            if (!hitMatchesFileType(s.q, probe)) {
                // a bundle may still hold a wanted file besides the matched one
                if (!(probe.isBundle && !probe.files.isEmpty())) continue;
                probe.files.clear();
                if (!hitMatchesFileType(s.q, probe)) continue;
            }
        }
        if (s.seen.contains(btih)) continue;
        if (s.emitted + s.pending.size() >= s.q.maxResults) continue;
        s.seen.insert(btih);
        if (!s.answeredBy.contains(peerId)) { s.answeredBy.insert(peerId); ++s.peersAnswered; }
        s.lastHitMs = s.started.elapsed();

        SearchHit h;
        h.queryId = it.key();
        h.fileName = name;
        h.size = size;
        h.type = mt;
        h.hashes.btih = btih;
        h.isBundle = files > 1;
        if (!matchedPath.isEmpty()) h.path = matchedPath;
        const qint64 added = jInt(o, "added", 0);
        if (added > 0) h.published = QDateTime::fromMSecsSinceEpoch(added);
        SearchSource src;
        src.net = Net::BitTorrent;
        src.uri = h.hashes.toMagnet(name, size);
        src.seeds = static_cast<int>(qBound(qint64(-1), jInt(o, "seeders", -1), qint64(10000000)));
        src.peers = static_cast<int>(qBound(qint64(-1), jInt(o, "leechers", -1), qint64(10000000)));
        src.connection = QStringLiteral("Rats");
        src.extra.insert(QStringLiteral("provider"), QStringLiteral("rats"));
        src.extra.insert(QStringLiteral("ratsPeer"), peerId);
        src.extra.insert(QStringLiteral("files"), files);
        if (!ct.isEmpty()) src.extra.insert(QStringLiteral("contentType"), ct);
        if (!cat.isEmpty()) src.extra.insert(QStringLiteral("contentCategory"), cat);
        const qint64 checked = jInt(o, "trackersChecked", 0);
        if (checked > 0) src.extra.insert(QStringLiteral("trackersChecked"), QDateTime::fromMSecsSinceEpoch(checked));
        h.sources << src;
        s.pending << h;
        any = true;
    }
    if (any && !m_flushTimer.isActive()) m_flushTimer.start();
}

void RatsSearchPlugin::flush()
{
    QList<quint32> full;
    for (auto it = m_searches.begin(); it != m_searches.end(); ++it) {
        if (it->pending.isEmpty()) continue;
        it->emitted += it->pending.size();
        Q_EMIT searchHits(it.key(), it->pending);
        it->pending.clear();
        if (it->emitted >= it->q.maxResults) full << it.key();
    }
    for (quint32 id : full) finish(id);
}

void RatsSearchPlugin::tick()
{
    // search completion: quiet period after the peers had time to answer, or the hard timeout
    const int timeoutMs = qBound(10, m_ctx.settings ? m_ctx.settings->i(QString(keys::search::TimeoutSec)) : 90, 1800) * 1000;
    const int peers = m_node ? m_node->peerCount() : 0;
    QList<quint32> done;
    for (auto it = m_searches.cbegin(); it != m_searches.cend(); ++it) {
        const qint64 el = it->started.elapsed();
        const qint64 quiet = el - it->lastHitMs;
        const bool enoughPeers = peers >= 5 || el > 30000;
        if (el >= timeoutMs || (el >= 12000 && quiet >= 8000 && enoughPeers && peers > 0) ||
            (peers == 0 && el >= 45000))
            done << it.key();
    }
    for (quint32 id : done) finish(id);
    updateStatus();
    if (m_serversDirty) {
        m_serversDirty = false;
        Q_EMIT serversChanged();
    }
}

void RatsSearchPlugin::requestBundleFiles(const FileHashes &h)
{
    if (!h.btih.isEmpty()) requestTorrentFiles(h.btih);
}

bool RatsSearchPlugin::requestTorrentFiles(const QByteArray &btihIn)
{
    if (!m_node || !m_running || m_node->peerCount() == 0) return false;
    QByteArray btih = btihIn.size() == 40 ? QByteArray::fromHex(btihIn) : btihIn;
    if (btih.size() != 20) return false;
    QJsonObject req;
    req[QStringLiteral("hash")] = QString::fromLatin1(btih.toHex());
    QJsonObject opt;
    opt[QStringLiteral("files")] = true;
    req[QStringLiteral("options")] = opt;
    int sent = 0;
    for (const QString &p : m_hashPeers.value(btih)) {
        if (sent >= 3) break;
        if (!m_peers.contains(p)) continue;
        m_node->sendJson(p, QStringLiteral("torrent"), req);
        ++sent;
    }
    if (sent == 0) {
        for (auto it = m_peers.cbegin(); it != m_peers.cend() && sent < 5; ++it, ++sent)
            m_node->sendJson(it.key(), QStringLiteral("torrent"), req);
    }
    return sent > 0;
}

} // namespace mx::ovl

mx::NetworkPlugin *mx::createRatsSearchPlugin(QObject *parent)
{
    return new mx::ovl::RatsSearchPlugin(parent);
}
