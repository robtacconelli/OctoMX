// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX: BitTorrent plugin facade (main thread side).
#include "bt_plugin_impl.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <libtorrent/bencode.hpp>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/hasher.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/torrent_info.hpp>

#include "mx/net_policy.h"
#include "mx/settings_keys.h"

namespace mx {

NetworkPlugin *createBitTorrentPlugin(QObject *parent)
{
    return new bt::BtPlugin(parent);
}

namespace bt {

namespace {
constexpr qint64 kMaxTorrentFile = 32 * 1024 * 1024;

QString canonicalPath(const QString &path)
{
    const QFileInfo fi(path);
    const QString c = fi.canonicalFilePath();
    return c.isEmpty() ? QDir::cleanPath(fi.absoluteFilePath()) : c;
}
} // namespace

BtPlugin::BtPlugin(QObject *parent) : NetworkPlugin(parent)
{
    m_events = new BitTorrentEvents(this);
    m_status.net = Net::BitTorrent;
    m_status.state = ConnState::Disconnected;
    m_status.statusText = QStringLiteral("Offline");
    m_clock.start();
    m_hashPool.setMaxThreadCount(1);
    m_changedTimer.setInterval(250);
    connect(&m_changedTimer, &QTimer::timeout, this, &BtPlugin::flushChanged);
    m_cfgTimer.setSingleShot(true);
    m_cfgTimer.setInterval(300);
    connect(&m_cfgTimer, &QTimer::timeout, this, &BtPlugin::reloadConfig);
    m_rateTimer.setInterval(200);   // [bw-2] the budget moves every 100 ms (was polled once a second)
    connect(&m_rateTimer, &QTimer::timeout, this, &BtPlugin::pollRates);
}

BtPlugin::~BtPlugin()
{
    m_cancelHashing->store(true);
    m_hashPool.waitForDone();
    if (m_worker) {
        Worker *w = m_worker;
        m_worker = nullptr;
        QMetaObject::invokeMethod(w, [w] {
            w->teardown(3000);
            delete w;
        }, Qt::BlockingQueuedConnection);
    }
    if (m_thread) {
        m_thread->quit();
        m_thread->wait();
        delete m_thread;
    }
}

template <typename F> void BtPlugin::toWorker(F &&fn)
{
    if (m_worker)
        QMetaObject::invokeMethod(m_worker, std::forward<F>(fn), Qt::QueuedConnection);
}

template <typename F> void BtPlugin::toWorkerBlocking(F &&fn)
{
    if (m_worker)
        QMetaObject::invokeMethod(m_worker, std::forward<F>(fn), Qt::BlockingQueuedConnection);
}

// ------------------------------------------------------------------------------------------------ lifecycle
void BtPlugin::init(const PluginContext &ctx)
{
    if (m_inited)
        return;
    m_ctx = ctx;
    m_dataDir = !ctx.dataDir.isEmpty() ? ctx.dataDir : ctx.profileDir + QStringLiteral("/bt");
    QDir().mkpath(m_dataDir);
    m_cfg = Config::fromSettings(ctx.settings);
    applyCtxToConfig();
    const QString ca = setupCaBundle();
    qCInfo(lcBt) << "HTTPS tracker CA source:" << (ca.isEmpty() ? QStringLiteral("(none found)") : ca);
    loadNodes();

    m_thread = new QThread;
    m_thread->setObjectName(QStringLiteral("mx.bt.session"));
    m_worker = new Worker(this, this, m_dataDir);
    m_worker->moveToThread(m_thread);
    m_thread->start();
    Worker *w = m_worker;
    const Config cfg = m_cfg;
    const QStringList nodes = m_userNodes, trackers = m_userTrackers;
    m_inited = true;
    toWorkerBlocking([w, cfg, nodes, trackers] { w->init(cfg, nodes, trackers); });
    // deliver the restored torrents now, so torrents()/snapshot() are valid when init() returns
    m_restoring = true;
    QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
    m_restoring = false;

    if (ctx.settings) {
        connect(ctx.settings, &Settings::changed, this, [this](const QString &key, const QVariant &) {
            if (key.startsWith(QLatin1String("bt/")) || key.startsWith(QLatin1String("connection/"))
                || key.startsWith(QLatin1String("bandwidth/")))
                m_cfgTimer.start();
        });
    }
    m_changedTimer.start();
    m_rateTimer.start();
    pollRates();
}

void BtPlugin::start()
{
    if (!m_inited || m_started)
        return;
    m_started = true;
    m_cfg = Config::fromSettings(m_ctx.settings);
    applyCtxToConfig();
    Worker *w = m_worker;
    const Config cfg = m_cfg;
    toWorker([w, cfg] { w->goOnline(cfg); });
    m_status.state = ConnState::Connecting;
    m_status.statusText = QStringLiteral("Starting...");
    emit statusChanged(m_status);
}

void BtPlugin::stop()
{
    if (!m_started)
        return;
    m_started = false;
    Worker *w = m_worker;
    toWorkerBlocking([w] { w->goOffline(3000); });
    QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
}

void BtPlugin::applyCtxToConfig()
{
    if (!m_ctx.clientName.isEmpty())
        m_cfg.clientName = m_ctx.clientName;
    if (!m_ctx.clientVersion.isEmpty())
        m_cfg.clientVersion = m_ctx.clientVersion;
    m_cfg.coreMapsPorts = m_ctx.coreMapsPorts;
    m_cfg.announcePort = m_announcePort;
}

// [diag-1] the core's port mapper forwarded (or lost) one of our ports: trackers and the DHT must
// get the external port when the router gave a different one
void BtPlugin::portMappingChanged(const PortMapping &m)
{
    if (m.proto != QLatin1String("TCP")) return;
    const int want = (m.result == PortMapping::Mapped && m.externalPort > 0 && m.externalPort != m.internalPort) ? m.externalPort : 0;
    if (want == m_announcePort) return;
    m_announcePort = want;
    qCInfo(lcBt) << "announcing port" << (want ? want : m.internalPort) << "(router mapping: internal" << m.internalPort
                 << "external" << m.externalPort << ")";
    if (m_inited) reloadConfig();
}

void BtPlugin::recheckConnectivity()
{
    for (const QString &id : m_order) reannounce(id);
}

void BtPlugin::wDiag(const QHash<QString, TorrentDiag> &torrents, const PluginDiagnostics &session)
{
    m_torrentDiag = torrents;
    m_sessionDiag = session;
    if (!m_started) m_sessionDiag.bindings.clear();
}

void BtPlugin::reloadConfig()
{
    const QStringList oldNodes = m_cfg.bootstrapNodes;
    m_cfg = Config::fromSettings(m_ctx.settings);
    applyCtxToConfig();
    Worker *w = m_worker;
    const Config cfg = m_cfg;
    toWorker([w, cfg] { w->applyConfig(cfg); });
    if (oldNodes != m_cfg.bootstrapNodes)
        emit serversChanged();
    pollRates();
}

void BtPlugin::pollRates()
{
    if (m_manualRates || !m_inited)
        return;
    int d = 0, u = 0;
    if (m_ctx.limiter) {
        d = m_ctx.limiter->budgetBps(RateLimiter::Down, Net::BitTorrent);
        u = m_ctx.limiter->budgetBps(RateLimiter::Up, Net::BitTorrent);
    }   // without a limiter (tests, tools): unlimited unless setRateLimits() is used
    if (d == m_appliedDown && u == m_appliedUp)
        return;
    m_appliedDown = d;
    m_appliedUp = u;
    Worker *w = m_worker;
    toWorker([w, d, u] { w->setRateLimits(d, u); });
}

void BtPlugin::setRateLimits(int downBps, int upBps)
{
    m_manualRates = true;
    downBps = qMax(0, downBps);
    upBps = qMax(0, upBps);
    if (downBps == m_appliedDown && upBps == m_appliedUp)
        return;
    m_appliedDown = downBps;
    m_appliedUp = upBps;
    Worker *w = m_worker;
    toWorker([w, downBps, upBps] { w->setRateLimits(downBps, upBps); });
}

// ------------------------------------------------------------------------------------------------ servers
void BtPlugin::loadNodes()
{
    const QJsonObject o = QJsonDocument::fromJson(readFile(m_dataDir + QStringLiteral("/nodes.json"))).object();
    m_userNodes.clear();
    m_userTrackers.clear();
    for (const QJsonValue &v : o.value(QStringLiteral("nodes")).toArray())
        m_userNodes << v.toString();
    for (const QJsonValue &v : o.value(QStringLiteral("trackers")).toArray())
        m_userTrackers << v.toString();
}

void BtPlugin::saveNodes()
{
    QJsonObject o;
    o.insert(QStringLiteral("nodes"), QJsonArray::fromStringList(m_userNodes));
    o.insert(QStringLiteral("trackers"), QJsonArray::fromStringList(m_userTrackers));
    writeFileAtomic(m_dataDir + QStringLiteral("/nodes.json"), QJsonDocument(o).toJson());
}

QList<ServerEntry> BtPlugin::servers() const
{
    QList<ServerEntry> out;
    const ConnState dhtState = !m_started || !m_cfg.dht ? ConnState::Disconnected
                               : m_status.peers > 0     ? ConnState::Connected
                                                        : ConnState::Connecting;
    auto add = [&](const QString &addr, const QString &desc, bool isStatic, ConnState st) {
        ServerEntry e;
        e.net = Net::BitTorrent;
        e.id = addr;
        e.address = addr;
        QString host;
        int port = 0;
        e.name = parseHostPort(addr, &host, &port, 6881) ? host : addr;
        e.state = st;
        e.description = desc;
        e.isStatic = isStatic;
        // [core-5] a DHT row counts nodes, not users
        if (st == ConnState::Connected && !addr.contains(QLatin1String("://"))) {
            e.nodes = m_status.peers;
            e.description += QStringLiteral(", %1").arg(countNoun(m_status.peers, QStringLiteral("node")));
        }
        out << e;
    };
    for (const QString &n : m_cfg.bootstrapNodes)
        add(n, QStringLiteral("DHT bootstrap router"), false, dhtState);
    for (const QString &n : m_userNodes)
        add(n, QStringLiteral("DHT node (user-added)"), true, dhtState);
    for (const QString &t : m_userTrackers)
        add(t, QStringLiteral("Tracker (added to all public torrents)"), true,
            m_started ? ConnState::Connected : ConnState::Disconnected);
    return out;
}

void BtPlugin::addServer(const QString &address, const QString &name)
{
    Q_UNUSED(name)
    const QString a = address.trimmed();
    if (a.startsWith(QLatin1String("udp://")) || a.startsWith(QLatin1String("http://"))
        || a.startsWith(QLatin1String("https://"))) {
        if (m_userTrackers.contains(a))
            return;
        m_userTrackers << a;
    } else {
        QString host;
        int port = 0;
        if (!parseHostPort(a, &host, &port, 6881))
            return;
        const QString norm = endpointString(host, port);
        if (m_userNodes.contains(norm) || m_cfg.bootstrapNodes.contains(norm))
            return;
        m_userNodes << norm;
        Worker *w = m_worker;
        toWorker([w, host, port] { w->addDhtNode(host, port); });
    }
    saveNodes();
    Worker *w = m_worker;
    const QStringList nodes = m_userNodes, trackers = m_userTrackers;
    toWorker([w, nodes, trackers] { w->setUserNodes(nodes, trackers); });
    emit serversChanged();
}

void BtPlugin::removeServer(const QString &id)
{
    if (m_userNodes.removeAll(id) || m_userTrackers.removeAll(id)) {
        saveNodes();
        Worker *w = m_worker;
        const QStringList nodes = m_userNodes, trackers = m_userTrackers;
        toWorker([w, nodes, trackers] { w->setUserNodes(nodes, trackers); });
        emit serversChanged();
        return;
    }
    if (m_cfg.bootstrapNodes.contains(id) && m_ctx.settings) {
        QStringList l = m_cfg.bootstrapNodes;
        l.removeAll(id);
        m_ctx.settings->setValue(QString(keys::bt::DhtBootstrapNodes), l);
        m_cfg.bootstrapNodes = l;
        emit serversChanged();
    }
}

void BtPlugin::connectServer(const QString &id)
{
    QString host;
    int port = 0;
    if (!parseHostPort(id, &host, &port, 6881))
        return;
    Worker *w = m_worker;
    toWorker([w, host, port] { w->addDhtNode(host, port); });
}

void BtPlugin::setSharedFiles(const QList<SharedFile> &files)
{
    const bool seedLibrary = m_ctx.settings && m_ctx.settings->value(QString(keys::bt::SeedLibrary)).toBool();
    if (!seedLibrary)
        return;
    for (const SharedFile &f : files) {
        if (f.size <= 0)
            continue;
        const QString c = canonicalPath(f.path);
        if (m_seedPathToId.contains(c) || m_hashing.contains(c))
            continue;
        QString err;
        seedLocalFile(f.path, &err);
    }
}

void BtPlugin::cancelUpload(const QString &uploadId)
{
    // "bt:<infohash>:<ip:port>"
    const QString ep = uploadId.section(QLatin1Char(':'), 2);
    QString host;
    int port = 0;
    if (parseHostPort(ep, &host, &port, 0)) {
        Worker *w = m_worker;
        toWorker([w, host] { w->banPeerAddress(host); });
    }
    emit uploadFinished(uploadId);
}

// ------------------------------------------------------------------------------------------------ adding
QString BtPlugin::defaultSaveDir() const
{
    QString d;
    if (m_ctx.settings)
        d = m_ctx.settings->value(QString(keys::transfers::IncomingDir)).toString();
    if (d.isEmpty())
        d = m_ctx.profileDir.isEmpty() ? m_dataDir + QStringLiteral("/downloads") : m_ctx.profileDir + QStringLiteral("/downloads");
    return d;
}

QString BtPlugin::addTorrent(const QString &uri, const QByteArray &torrentFile, const QString &saveDir, bool paused,
                             const QList<int> &wantedFiles, QString *error)
{
    auto fail = [&](const QString &e) {
        if (error)
            *error = e;
        return QString();
    };
    if (!m_inited)
        return fail(QStringLiteral("BitTorrent plugin not initialised"));
    auto atp = std::make_shared<lt::add_torrent_params>();
    Record rec;
    rec.savePath = QDir::cleanPath(saveDir.isEmpty() ? defaultSaveDir() : saveDir);
    QByteArray bytes = torrentFile;
    const QString u = uri.trimmed();
    if (bytes.isEmpty() && !u.isEmpty() && !u.startsWith(QLatin1String("magnet:"), Qt::CaseInsensitive)) {
        if (u.startsWith(QLatin1String("http://"), Qt::CaseInsensitive) || u.startsWith(QLatin1String("https://"), Qt::CaseInsensitive)) {
            fetchUrl(u, saveDir, paused, wantedFiles);
            if (error)
                error->clear();
            return QString();   // pending → urlTorrentAdded / urlTorrentFailed
        }
        const QUrl url(u);
        const QString local = url.isLocalFile() ? url.toLocalFile() : u;
        if (!QFileInfo(local).isFile())
            return fail(QStringLiteral("Unsupported link or file not found: %1").arg(u));
        bytes = readFile(local, kMaxTorrentFile);
        if (bytes.isEmpty())
            return fail(QStringLiteral("Cannot read %1").arg(local));
    }
    if (!bytes.isEmpty()) {
        lt::error_code ec;
        auto ti = std::make_shared<lt::torrent_info>(lt::span<char const>(bytes.constData(), bytes.size()), ec, lt::from_span);
        if (ec)
            return fail(QStringLiteral("Invalid .torrent: %1").arg(QString::fromStdString(ec.message())));
        atp->ti = ti;
    } else if (u.startsWith(QLatin1String("magnet:"), Qt::CaseInsensitive)) {
        lt::error_code ec;
        lt::parse_magnet_uri(u.toStdString(), *atp, ec);
        if (ec)
            return fail(QStringLiteral("Invalid magnet link: %1").arg(QString::fromStdString(ec.message())));
        rec.uri = u;
        rec.name = QString::fromStdString(atp->name);
        int tier = 0;
        for (int t : atp->tracker_tiers)
            tier = qMax(tier, t + 1);
        for (const QString &tr : m_cfg.extraTrackers) {
            const std::string s = tr.trimmed().toStdString();
            if (s.empty() || std::find(atp->trackers.begin(), atp->trackers.end(), s) != atp->trackers.end())
                continue;
            atp->tracker_tiers.resize(atp->trackers.size(), 0);
            atp->trackers.push_back(s);
            atp->tracker_tiers.push_back(tier);
        }
    } else {
        return fail(QStringLiteral("Nothing to add (no magnet link and no .torrent data)"));
    }
    if (paused)
        atp->flags = (lt::torrent_flags::default_flags | lt::torrent_flags::paused) & ~lt::torrent_flags::auto_managed;
    else
        atp->flags = (lt::torrent_flags::default_flags | lt::torrent_flags::auto_managed) & ~lt::torrent_flags::paused;
    return addParams(atp, rec, torrentFile.isEmpty() ? bytes : torrentFile, wantedFiles, error);
}

QString BtPlugin::addParams(std::shared_ptr<lt::add_torrent_params> atp, Record rec, const QByteArray &torrentBytes,
                            const QList<int> &wanted, QString *error)
{
    const lt::info_hash_t ih = atp->ti ? atp->ti->info_hashes() : atp->info_hashes;
    if (!ih.has_v1() && !ih.has_v2()) {
        if (error)
            *error = QStringLiteral("No info-hash");
        return QString();
    }
    const QString id = hexId(ih);
    if (m_snap.contains(id)) {
        // duplicate: merge trackers, keep everything else
        std::vector<std::string> tr = atp->trackers;
        if (atp->ti)
            for (const lt::announce_entry &ae : atp->ti->trackers())
                tr.push_back(ae.url);
        if (!tr.empty()) {
            Worker *w = m_worker;
            toWorker([w, id, tr] { w->mergeTrackers(id, tr); });
        }
        return id;
    }
    rec.id = id;
    rec.added = QDateTime::currentDateTime();
    if (rec.name.isEmpty() && atp->ti)
        rec.name = QString::fromStdString(atp->ti->name());
    atp->save_path = rec.savePath.toStdString();
    atp->flags &= ~lt::torrent_flags::duplicate_is_error;
    if (!m_cfg.pex)
        atp->flags |= lt::torrent_flags::disable_pex;
    if (m_cfg.superSeeding)
        atp->flags |= lt::torrent_flags::super_seeding;
    atp->storage_mode = m_cfg.preallocate ? lt::storage_mode_allocate : lt::storage_mode_sparse;
    atp->max_connections = m_cfg.maxConnectionsPerTorrent;
    atp->max_uploads = m_cfg.maxUploadsPerTorrent;
    if (!wanted.isEmpty()) {
        int n = 0;
        if (atp->ti)
            n = atp->ti->num_files();
        else
            for (int i : wanted)
                n = qMax(n, i + 1);
        atp->file_priorities.assign(std::size_t(n), lt::dont_download);
        for (int i : wanted)
            if (i >= 0 && i < n && !(atp->ti && atp->ti->files().pad_file_at(lt::file_index_t(i))))
                atp->file_priorities[std::size_t(i)] = lt::default_priority;
        atp->flags |= lt::torrent_flags::default_dont_download;
    }
    TorrentSnapshot snap = snapshotFromParams(*atp, rec);
    if (m_started && !(atp->flags & lt::torrent_flags::paused)) {
        snap.state = atp->ti ? TransferState::FindingSources : TransferState::FindingSources;
        snap.statusText = atp->ti ? QStringLiteral("Starting") : QStringLiteral("Fetching metadata");
    }
    m_snap.insert(id, snap);
    m_order.append(id);
    if (atp->ti)
        m_pieceHashes.insert(id, v1PieceHashes(*atp->ti));
    if (!rec.seededPath.isEmpty())
        m_seedPathToId.insert(rec.seededPath, id);
    AddRequest req;
    req.id = id;
    req.atp = atp;
    req.rec = rec;
    req.torrentFile = torrentBytes;
    req.wantedFiles = wanted;
    Worker *w = m_worker;
    toWorker([w, req] { w->addTorrent(req); });
    if (error)
        error->clear();
    return id;
}

void BtPlugin::fetchUrl(const QString &url, const QString &saveDir, bool paused, const QList<int> &wanted)
{
    if (!m_nam)
        m_nam = new QNetworkAccessManager(this);
    mx::netpolicy::applyTo(m_nam, m_ctx.settings);   // [net-2] .torrent files: through the proxy / the adapter
    QNetworkRequest req{QUrl(url)};
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("%1/%2").arg(m_cfg.clientName, m_cfg.clientVersion));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(60000);
    QNetworkReply *reply = m_nam->get(req);
    connect(reply, &QNetworkReply::downloadProgress, reply, [reply](qint64 got, qint64 total) {
        if (got > kMaxTorrentFile || total > kMaxTorrentFile)
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, url, saveDir, paused, wanted] {
        reply->deleteLater();
        QString err;
        if (reply->error() != QNetworkReply::NoError) {
            err = reply->errorString();
        } else {
            const QByteArray data = reply->readAll();
            const QString id = addTorrent(QString(), data, saveDir, paused, wanted, &err);
            if (!id.isEmpty()) {
                emit m_events->urlTorrentAdded(url, id);
                return;
            }
            if (err.isEmpty())
                err = QStringLiteral("not a .torrent file");
        }
        qCInfo(lcBt) << "fetching" << url << "failed:" << err;
        emit m_events->urlTorrentFailed(url, err);
        emit m_events->torrentError(url, err);
    });
}

// ------------------------------------------------------------------------------------------------ commands
void BtPlugin::removeTorrent(const QString &id, bool deleteFiles)
{
    if (!m_snap.contains(id))
        return;
    m_snap.remove(id);
    m_order.removeAll(id);
    m_pieceHashes.remove(id);
    m_confirmed.remove(id);
    m_dirty.remove(id);
    for (auto it = m_seedPathToId.begin(); it != m_seedPathToId.end();)
        it = it.value() == id ? m_seedPathToId.erase(it) : std::next(it);
    Worker *w = m_worker;
    toWorker([w, id, deleteFiles] { w->removeTorrent(id, deleteFiles); });
}

void BtPlugin::pauseTorrent(const QString &id)
{
    auto it = m_snap.find(id);
    if (it == m_snap.end())
        return;
    it->paused = true;
    if (!it->finished) {
        it->state = TransferState::Paused;
        it->statusText = QStringLiteral("Paused");
    }
    it->rateDownBps = it->rateUpBps = 0;
    noteChanged(id);
    Worker *w = m_worker;
    toWorker([w, id] { w->pauseTorrent(id); });
}

void BtPlugin::resumeTorrent(const QString &id)
{
    auto it = m_snap.find(id);
    if (it == m_snap.end())
        return;
    it->paused = false;
    if (it->state == TransferState::Paused || it->state == TransferState::Error) {
        it->state = TransferState::FindingSources;
        it->statusText = QStringLiteral("Resuming");
    }
    noteChanged(id);
    Worker *w = m_worker;
    toWorker([w, id] { w->resumeTorrent(id); });
}

void BtPlugin::setWantedFiles(const QString &id, const QList<int> &indexes)
{
    if (!m_snap.contains(id))
        return;
    Worker *w = m_worker;
    toWorker([w, id, indexes] { w->setWantedFiles(id, indexes); });
}

void BtPlugin::setTorrentPriority(const QString &id, int priority)
{
    if (!m_snap.contains(id))
        return;
    Worker *w = m_worker;
    toWorker([w, id, priority] { w->setPriority(id, priority); });
}

void BtPlugin::recheck(const QString &id)
{
    if (!m_snap.contains(id))
        return;
    Worker *w = m_worker;
    toWorker([w, id] { w->recheck(id); });
}

// [engine-3] libtorrent move_storage (never overwrites: fail_if_exist) → torrentStorageMoved
bool BtPlugin::moveStorage(const QString &id, const QString &newSaveDir)
{
    if (!m_snap.contains(id) || newSaveDir.trimmed().isEmpty())
        return false;
    const QString dir = QDir::cleanPath(QDir(newSaveDir).absolutePath());
    if (!QDir().mkpath(dir))
        return false;
    Worker *w = m_worker;
    toWorker([w, id, dir] { w->moveStorage(id, dir); });
    return true;
}

void BtPlugin::wStorageMoved(const QString &id, const QString &path, bool ok, const QString &error)
{
    auto it = m_snap.find(id);
    if (ok && it != m_snap.end())
        it->savePath = path;
    qCInfo(lcBt).noquote() << "move storage" << id.left(8) << (ok ? "done:" : "failed:") << (ok ? path : error);
    emit m_events->torrentStorageMoved(id, path, ok, error);
}

void BtPlugin::reannounce(const QString &id)
{
    if (!m_snap.contains(id))
        return;
    Worker *w = m_worker;
    toWorker([w, id] { w->reannounce(id); });
}

QByteArray BtPlugin::pieceHash(const QString &id, int piece) const
{
    const QByteArray &h = m_pieceHashes.value(id);
    if (piece < 0 || qint64(piece) * 20 + 20 > h.size())
        return QByteArray();
    return h.mid(piece * 20, 20);
}

void BtPlugin::reservePieces(const QString &id, const QList<int> &pieces, bool reserve)
{
    if (!m_snap.contains(id) || pieces.isEmpty())
        return;
    Worker *w = m_worker;
    toWorker([w, id, pieces, reserve] { w->reserve(id, pieces, reserve); });
}

// [dl-1] libtorrent's sequential_download flag + top priority for the first / last band pieces
bool BtPlugin::setDownloadOrder(const QString &id, bool sequential, bool firstLast)
{
    if (!m_snap.contains(id))
        return false;
    Worker *w = m_worker;
    toWorker([w, id, sequential, firstLast] { w->setOrder(id, sequential, firstLast); });
    return true;
}

void BtPlugin::injectPiece(const QString &id, int piece, const QByteArray &data)
{
    Worker *w = m_worker;
    toWorker([w, id, piece, data] { w->inject(id, piece, data); });
}

void BtPlugin::addPeer(const QString &id, const QString &endpoint)
{
    Worker *w = m_worker;
    toWorker([w, id, endpoint] { w->addPeer(id, endpoint); });
}

// ------------------------------------------------------------------------------------------------ seeding local files
bool BtPlugin::seedLocalFile(const QString &path, QString *error)
{
    const QFileInfo fi(path);
    auto fail = [&](const QString &e) {
        if (error)
            *error = e;
        return false;
    };
    if (!m_inited)
        return fail(QStringLiteral("BitTorrent plugin not initialised"));
    if (!fi.exists() || !fi.isFile())
        return fail(QStringLiteral("File not found: %1").arg(path));
    if (!fi.isReadable())
        return fail(QStringLiteral("File not readable: %1").arg(path));
    if (fi.size() <= 0)
        return fail(QStringLiteral("Cannot seed an empty file"));
    {
        QFile f(fi.absoluteFilePath());
        if (!f.open(QIODevice::ReadOnly))
            return fail(QStringLiteral("Cannot open %1: %2").arg(path, f.errorString()));
    }
    const QString c = canonicalPath(path);
    if (error)
        error->clear();
    const QString existing = m_seedPathToId.value(c);
    if (!existing.isEmpty() && m_snap.contains(existing)) {
        QMetaObject::invokeMethod(this, [this, path, existing] { emit m_events->seedReady(path, existing); }, Qt::QueuedConnection);
        return true;
    }
    if (m_hashing.contains(c))
        return true;
    m_hashing.insert(c);
    const QString name = fi.fileName();
    const qint64 size = fi.size();
    auto cancel = m_cancelHashing;
    const QString creator = QStringLiteral("%1 %2").arg(m_cfg.clientName, m_cfg.clientVersion);
    m_hashPool.start([this, c, name, size, cancel, creator] {
        QString err;
        QByteArray out;
        try {
            QFile f(c);
            if (!f.open(QIODevice::ReadOnly))
                throw std::runtime_error(f.errorString().toStdString());
            lt::file_storage fs;
            fs.add_file(name.toStdString(), size);
            lt::create_torrent ct(fs, 0, lt::create_torrent::v1_only);
            const int n = ct.num_pieces();
            QByteArray buf;
            QElapsedTimer report;
            report.start();
            qint64 hashed = 0;
            for (int i = 0; i < n; ++i) {
                if (cancel->load())
                    return;
                const int len = ct.piece_size(lt::piece_index_t(i));
                buf.resize(len);
                if (f.read(buf.data(), len) != len)
                    throw std::runtime_error("short read (file changed while hashing?)");
                lt::hasher h(buf.constData(), len);
                ct.set_hash(lt::piece_index_t(i), h.final());
                hashed += len;
                if (report.elapsed() >= 250 || i == n - 1) {
                    report.restart();
                    QMetaObject::invokeMethod(this, [this, c, hashed, size] { emit m_events->seedProgress(c, hashed, size); },
                                              Qt::QueuedConnection);
                }
            }
            ct.set_creator(creator.toStdString().c_str());
            const lt::entry e = ct.generate();
            std::vector<char> v;
            lt::bencode(std::back_inserter(v), e);
            out = QByteArray(v.data(), int(v.size()));
        } catch (const std::exception &e) {
            err = QString::fromLocal8Bit(e.what());
        }
        if (cancel->load())
            return;
        QMetaObject::invokeMethod(this, [this, c, out, err] { onSeedHashed(c, out, err); }, Qt::QueuedConnection);
    });
    return true;
}

void BtPlugin::onSeedHashed(const QString &path, const QByteArray &torrent, const QString &error)
{
    m_hashing.remove(path);
    QString err = error;
    if (err.isEmpty()) {
        auto atp = std::make_shared<lt::add_torrent_params>();
        lt::error_code ec;
        atp->ti = std::make_shared<lt::torrent_info>(lt::span<char const>(torrent.constData(), torrent.size()), ec, lt::from_span);
        if (ec) {
            err = QString::fromStdString(ec.message());
        } else {
            atp->flags = (lt::torrent_flags::default_flags | lt::torrent_flags::auto_managed | lt::torrent_flags::seed_mode)
                         & ~lt::torrent_flags::paused;
            Record rec;
            rec.savePath = QFileInfo(path).absolutePath();
            rec.seededPath = path;
            const QString id = addParams(atp, rec, torrent, {}, &err);
            if (!id.isEmpty()) {
                m_seedPathToId.insert(path, id);
                qCInfo(lcBt) << "seeding" << path << "as" << id;
                emit m_events->seedReady(path, id);
                return;
            }
        }
    }
    qCInfo(lcBt) << "seedLocalFile" << path << "failed:" << err;
    emit m_events->seedFailed(path, err);
}

// ------------------------------------------------------------------------------------------------ worker → main
void BtPlugin::noteChanged(const QString &id)
{
    const qint64 now = m_clock.elapsed();
    const qint64 last = m_lastChanged.value(id, -100000);
    if (now - last >= 1000) {
        m_lastChanged.insert(id, now);
        m_dirty.remove(id);
        emit m_events->torrentChanged(id);
    } else {
        m_dirty.insert(id);
    }
}

void BtPlugin::flushChanged()
{
    if (m_dirty.isEmpty())
        return;
    const qint64 now = m_clock.elapsed();
    const QSet<QString> dirty = m_dirty;
    for (const QString &id : dirty) {
        if (!m_snap.contains(id)) {
            m_dirty.remove(id);
            continue;
        }
        if (now - m_lastChanged.value(id, -100000) >= 1000) {
            m_dirty.remove(id);
            m_lastChanged.insert(id, now);
            emit m_events->torrentChanged(id);
        }
    }
}

void BtPlugin::wTorrentAdded(const TorrentSnapshot &snap, const QString &seededPath)
{
    if (!m_snap.contains(snap.id)) {
        if (!m_restoring)
            return;   // removed in the meantime
        m_order.append(snap.id);
    }
    TorrentSnapshot &s = m_snap[snap.id];
    s = snap;
    if (!seededPath.isEmpty())
        m_seedPathToId.insert(seededPath, snap.id);
    if (!m_confirmed.contains(snap.id)) {
        m_confirmed.insert(snap.id);
        emit m_events->torrentAdded(snap.id);
    } else {
        noteChanged(snap.id);
    }
}

void BtPlugin::wTorrentMetadata(const QString &id, const QByteArray &pieceHashes, const TorrentSnapshot &snap)
{
    if (!m_snap.contains(id))
        return;
    m_pieceHashes.insert(id, pieceHashes);
    m_snap.insert(id, snap);
    if (!m_metaEmitted.contains(id)) {
        m_metaEmitted.insert(id);
        emit m_events->torrentMetadata(id);
    }
    noteChanged(id);
}

void BtPlugin::wSnapshots(const QList<TorrentSnapshot> &snaps)
{
    for (const TorrentSnapshot &s : snaps) {
        auto it = m_snap.find(s.id);
        if (it == m_snap.end())
            continue;
        *it = s;
        noteChanged(s.id);
    }
}

void BtPlugin::wPieces(const QString &id, const QList<int> &finished, const QList<int> &failed)
{
    auto it = m_snap.find(id);
    if (it != m_snap.end())
        for (int p : finished)
            if (p >= 0 && p < it->have.size())
                it->have.setBit(p);
    for (int p : finished)
        emit m_events->torrentPieceFinished(id, p);
    for (int p : failed)
        emit m_events->torrentPieceFailed(id, p);
}

void BtPlugin::wTorrentFinished(const TorrentSnapshot &snap)
{
    auto it = m_snap.find(snap.id);
    if (it == m_snap.end())
        return;
    *it = snap;
    it->finished = true;
    emit m_events->torrentFinished(snap.id);
}

void BtPlugin::wTorrentRemoved(const QString &id)
{
    m_snap.remove(id);
    m_order.removeAll(id);
    m_pieceHashes.remove(id);
    m_metaEmitted.remove(id);
    m_confirmed.remove(id);
    emit m_events->torrentRemoved(id);
}

void BtPlugin::wTorrentError(const QString &id, const QString &text)
{
    auto it = m_snap.find(id);
    if (it != m_snap.end())
        it->errorText = text;
    emit m_events->torrentError(id, text);
}

void BtPlugin::wStatus(const NetworkStatus &st, int dhtNodes)
{
    Q_UNUSED(dhtNodes)
    NetworkStatus s = st;
    if (!m_started && s.state != ConnState::Disconnected)
        s.state = ConnState::Disconnected;
    const bool serversToo = (m_status.peers > 0) != (s.peers > 0);
    m_status = s;
    emit statusChanged(m_status);
    if (serversToo)
        emit serversChanged();
}

void BtPlugin::wUploads(const QList<TransferInfo> &changed, const QStringList &finished)
{
    for (const TransferInfo &t : changed)
        emit uploadChanged(t);
    for (const QString &id : finished)
        emit uploadFinished(id);
}

void BtPlugin::wLog(const QString &text)
{
    emit logMessage(text);
}

void BtPlugin::wAccountDiscovery(qint64 down, qint64 up)
{
    if (!m_ctx.limiter) return;
    if (down > 0) m_ctx.limiter->accountExempt(RateLimiter::Down, Net::BitTorrent, down, 0);
    if (up > 0) m_ctx.limiter->accountExempt(RateLimiter::Up, Net::BitTorrent, up, 0);
}

void BtPlugin::wWaiting(qint64 down, qint64 up)
{
    if (!m_ctx.limiter || m_manualRates) return;
    if (down > 0) m_ctx.limiter->reportWaiting(RateLimiter::Down, Net::BitTorrent, down);
    if (up > 0) m_ctx.limiter->reportWaiting(RateLimiter::Up, Net::BitTorrent, up);
}

void BtPlugin::wAccount(qint64 wireDown, qint64 wireUp, qint64 payloadDown, qint64 payloadUp)
{
    if (!m_ctx.limiter)
        return;
    if (wireDown > 0)
        m_ctx.limiter->accountTraffic(RateLimiter::Down, Net::BitTorrent, wireDown, payloadDown);
    if (wireUp > 0)
        m_ctx.limiter->accountTraffic(RateLimiter::Up, Net::BitTorrent, wireUp, payloadUp);
}

} // namespace bt
} // namespace mx
