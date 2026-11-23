// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX: libtorrent session worker (runs in its own thread).
#include "bt_worker.h"
#include "bt_pacing.h"
#include "mx/download_order.h"   // [dl-1]

#include <QtCore/QDir>
#include <climits>
#include <algorithm>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRandomGenerator>
#include <QtCore/QThread>
#include <QtCore/QUrl>

#include <libtorrent/alert_types.hpp>
#include <libtorrent/announce_entry.hpp>
#include <libtorrent/bencode.hpp>
#include <libtorrent/hasher.hpp>
#include <libtorrent/ip_filter.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/peer_info.hpp>
#include <libtorrent/read_resume_data.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/session_stats.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/write_resume_data.hpp>

namespace mx {
namespace bt {

namespace {

constexpr lt::status_flags_t kStatusFlags = lt::torrent_handle::query_pieces | lt::torrent_handle::query_name
                                            | lt::torrent_handle::query_save_path;

QString qs(const std::string &s) { return QString::fromStdString(s); }

QString sessionFile(const QString &dataDir) { return dataDir + QStringLiteral("/session.dat"); }
QString recordFile(const QString &dataDir, const QString &id) { return Worker::torrentsDir(dataDir) + QLatin1Char('/') + id + QStringLiteral(".json"); }
QString resumeFile(const QString &dataDir, const QString &id) { return Worker::torrentsDir(dataDir) + QLatin1Char('/') + id + QStringLiteral(".fastresume"); }
QString torrentFile(const QString &dataDir, const QString &id) { return Worker::torrentsDir(dataDir) + QLatin1Char('/') + id + QStringLiteral(".torrent"); }

QByteArray toBytes(const std::vector<char> &v) { return QByteArray(v.data(), int(v.size())); }

bool isChecking(lt::torrent_status::state_t s)
{
    return s == lt::torrent_status::checking_files || s == lt::torrent_status::checking_resume_data;
}

} // namespace

// ------------------------------------------------------------------------------------------------
QByteArray v1PieceHashes(const lt::torrent_info &ti)
{
    QByteArray out;
    if (!ti.info_hashes().has_v1() || !ti.is_valid())
        return out;
    const int n = ti.num_pieces();
    out.reserve(n * 20);
    for (int i = 0; i < n; ++i) {
        const lt::sha1_hash h = ti.hash_for_piece(lt::piece_index_t(i));
        out.append(h.data(), int(h.size()));
    }
    return out;
}

TorrentSnapshot snapshotFromParams(const lt::add_torrent_params &atp, const Record &rec)
{
    TorrentSnapshot s;
    s.id = rec.id;
    s.savePath = atp.save_path.empty() ? rec.savePath : qs(atp.save_path);
    s.added = rec.added;
    const auto ti = atp.ti;
    s.name = ti && ti->is_valid() ? qs(ti->name()) : (!atp.name.empty() ? qs(atp.name) : rec.name);
    if (s.name.isEmpty())
        s.name = rec.id;
    const bool userPaused = (atp.flags & lt::torrent_flags::paused) && !(atp.flags & lt::torrent_flags::auto_managed);
    s.paused = userPaused;
    s.state = userPaused ? TransferState::Paused : TransferState::Queued;
    s.statusText = transferStateText(s.state);
    if (ti && ti->is_valid()) {
        s.hasMetadata = true;
        s.pieceLength = ti->piece_length();
        s.numPieces = ti->num_pieces();
        s.have.resize(s.numPieces);
        for (int i = 0; i < s.numPieces && i < int(atp.have_pieces.size()); ++i)
            if (atp.have_pieces[lt::piece_index_t(i)])
                s.have.setBit(i);
        const lt::file_storage &fs = ti->files();
        for (int i = 0; i < fs.num_files(); ++i) {
            const lt::file_index_t fi(i);
            TorrentFile f;
            f.path = qs(fs.file_path(fi));
            f.size = fs.file_size(fi);
            f.offset = fs.file_offset(fi);
            int prio = 4;
            if (fs.pad_file_at(fi))
                prio = 0;
            else if (i < int(atp.file_priorities.size()))
                prio = int(static_cast<std::uint8_t>(atp.file_priorities[std::size_t(i)]));
            else if (atp.flags & lt::torrent_flags::default_dont_download)
                prio = 0;
            f.priority = prio;
            // estimate from the have bitfield (full pieces inside the file)
            if (f.size > 0 && s.pieceLength > 0) {
                const int first = int(f.offset / s.pieceLength);
                const int last = int((f.offset + f.size - 1) / s.pieceLength);
                qint64 done = 0;
                for (int p = first; p <= last && p < s.numPieces; ++p) {
                    if (!s.have.testBit(p))
                        continue;
                    const qint64 ps = qint64(p) * s.pieceLength;
                    const qint64 pe = qMin<qint64>(ps + ti->piece_size(lt::piece_index_t(p)), ti->total_size());
                    done += qMin(pe, f.offset + f.size) - qMax(ps, f.offset);
                }
                f.done = done;
            }
            if (prio > 0) {
                s.totalSize += f.size;
                s.totalDone += f.done;
            }
            s.files.append(f);
        }
    }
    return s;
}

// ------------------------------------------------------------------------------------------------
Worker::Worker(QObject *sinkObject, WorkerSink *sink, const QString &dataDir)
    : m_sinkObj(sinkObject), m_sink(sink), m_dataDir(dataDir)
{
}

Worker::~Worker()
{
    if (m_ses) {
        m_ses->set_alert_notify([] {});
        lt::session_proxy proxy = m_ses->abort();
        m_ses.reset();
    }
}

void Worker::post(std::function<void()> fn)
{
    QObject *obj = m_sinkObj.data();
    if (!obj)
        return;
    QMetaObject::invokeMethod(obj, std::move(fn), Qt::QueuedConnection);
}

Worker::Torrent *Worker::find(const QString &id)
{
    auto it = m_t.find(id);
    return it == m_t.end() ? nullptr : &it->second;
}

bool Worker::defer(Torrent *t, std::function<void()> op)
{
    if (!t || t->inSession || t->removing)
        return false;
    t->pendingOps.push_back(std::move(op));
    return true;
}

Worker::Torrent *Worker::find(const lt::torrent_handle &h)
{
    auto it = m_byHandle.find(h);
    if (it == m_byHandle.end())
        return nullptr;
    return find(it->second);
}

// ------------------------------------------------------------------------------------------------ init
void Worker::init(const Config &cfg, const QStringList &userNodes, const QStringList &userTrackers)
{
    m_clock.start();
    m_cfg = cfg;
    m_userNodes = userNodes;
    m_userTrackers = userTrackers;
    QDir().mkpath(torrentsDir(m_dataDir));
    m_port = cfg.randomizePorts ? int(QRandomGenerator::global()->bounded(20000, 60000)) : cfg.listenPort;

    lt::session_params sp;
    const QByteArray state = readFile(sessionFile(m_dataDir));
    if (!state.isEmpty()) {
        try {
            lt::error_code ec;
            lt::bdecode_node n = lt::bdecode(lt::span<char const>(state.constData(), state.size()), ec);
            if (!ec)
                sp = lt::read_session_params(n, lt::session_handle::save_dht_state);
            else
                qCWarning(lcBt) << "session.dat unreadable:" << qs(ec.message());
        } catch (const std::exception &e) {
            qCWarning(lcBt) << "session.dat rejected:" << e.what();
        }
    }
    sp.settings = makeSettingsPack(m_cfg, false, m_port, m_userNodes, m_downBps, m_upBps);
    m_ses = std::make_unique<lt::session>(std::move(sp), lt::session::paused);
    m_ses->add_extension(makePacingPlugin(m_pacing));   // [bw-2] per-peer queue under a limit
    m_ses->set_alert_notify([this] { scheduleAlerts(); });

    m_idxDhtNodes = lt::find_metric_idx("dht.dht_nodes");
    m_idxRecv = lt::find_metric_idx("net.recv_bytes");
    m_idxSent = lt::find_metric_idx("net.sent_bytes");
    m_idxRecvPayload = lt::find_metric_idx("net.recv_payload_bytes");
    m_idxSentPayload = lt::find_metric_idx("net.sent_payload_bytes");
    m_idxDhtIn = lt::find_metric_idx("dht.dht_bytes_in");
    m_idxDhtOut = lt::find_metric_idx("dht.dht_bytes_out");
    m_idxTrackerIn = lt::find_metric_idx("net.recv_tracker_bytes");
    m_idxTrackerOut = lt::find_metric_idx("net.sent_tracker_bytes");
    m_idxLimDownBytes = lt::find_metric_idx("net.limiter_down_bytes");   // [bw-2]
    m_idxDownPeers = lt::find_metric_idx("peer.num_peers_down_requests");
    m_idxUpPeers = lt::find_metric_idx("peer.num_peers_up_requests");
    m_idxLimUpBytes = lt::find_metric_idx("net.limiter_up_bytes");
    m_idxIncoming = lt::find_metric_idx("net.has_incoming_connections");
    m_idxDhtMsgIn = lt::find_metric_idx("dht.dht_messages_in");   // [diag-1]

    m_tick = new QTimer(this);
    m_tick->setInterval(1000);
    connect(m_tick, &QTimer::timeout, this, &Worker::tick);
    m_tick->start();
    // [bw-2] libtorrent's traffic reaches the shared budget (and the Bandwidth graph) every 100 ms:
    // once a second, its bytes came as one batch, so the graph's samples held 0, 1 or 2 seconds of it
    // and the other networks took the whole limit until the batch was charged, then stopped
    m_statsTimer = new QTimer(this);
    m_statsTimer->setInterval(100);
    m_statsTimer->setTimerType(Qt::PreciseTimer);
    connect(m_statsTimer, &QTimer::timeout, this, [this] { if (m_ses) m_ses->post_session_stats(); });
    m_statsTimer->start();
    m_saveTimer = new QTimer(this);
    m_saveTimer->setInterval(30000);
    connect(m_saveTimer, &QTimer::timeout, this, &Worker::periodicSave);
    m_saveTimer->start();
    m_stateTimer = new QTimer(this);
    m_stateTimer->setInterval(5 * 60000);
    connect(m_stateTimer, &QTimer::timeout, this, &Worker::saveSessionState);
    m_stateTimer->start();

    applyPeerClassFilter();
    loadPersisted();
}

void Worker::loadPersisted()
{
    QDir d(torrentsDir(m_dataDir));
    const QStringList recs = d.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    QList<TorrentSnapshot> snaps;
    int loaded = 0;
    for (const QString &fn : recs) {
        const QJsonDocument doc = QJsonDocument::fromJson(readFile(d.filePath(fn)));
        Record rec = Record::fromVariant(doc.object().toVariantMap());
        if (rec.id.isEmpty())
            rec.id = QFileInfo(fn).completeBaseName();
        lt::add_torrent_params atp;
        bool haveResume = false;
        const QByteArray fr = readFile(resumeFile(m_dataDir, rec.id));
        if (!fr.isEmpty()) {
            lt::error_code ec;
            atp = lt::read_resume_data(lt::span<char const>(fr.constData(), fr.size()), ec);
            haveResume = !ec;
            if (ec)
                qCWarning(lcBt) << "fastresume of" << rec.id << "rejected:" << qs(ec.message());
        }
        if (!atp.ti) {
            const QByteArray tf = readFile(torrentFile(m_dataDir, rec.id));
            if (!tf.isEmpty()) {
                lt::error_code ec;
                auto ti = std::make_shared<lt::torrent_info>(lt::span<char const>(tf.constData(), tf.size()), ec, lt::from_span);
                if (!ec)
                    atp.ti = ti;
            }
        }
        if (!haveResume && !atp.ti) {
            if (rec.uri.startsWith(QLatin1String("magnet:"))) {
                lt::error_code ec;
                lt::parse_magnet_uri(rec.uri.toStdString(), atp, ec);
                if (ec) {
                    qCWarning(lcBt) << "cannot restore" << rec.id << ":" << qs(ec.message());
                    continue;
                }
            } else {
                qCWarning(lcBt) << "cannot restore" << rec.id << ": no resume data, no metadata";
                continue;
            }
        }
        if (!haveResume) {
            atp.flags = (lt::torrent_flags::default_flags | lt::torrent_flags::auto_managed) & ~lt::torrent_flags::paused;
        }
        if (atp.save_path.empty())
            atp.save_path = rec.savePath.toStdString();
        // reservations are never persisted: piece priorities are re-derived from file priorities
        atp.piece_priorities.clear();
        atp.flags &= ~lt::torrent_flags::duplicate_is_error;
        const QString id = hexId(atp.ti ? atp.ti->info_hashes() : atp.info_hashes);
        if (id != rec.id)
            qCWarning(lcBt) << "record" << rec.id << "holds torrent" << id;
        if (m_t.count(id))
            continue;
        rec.id = id;
        Torrent &t = m_t[id];
        t.id = id;
        t.rec = rec;
        t.torrentFileWritten = QFileInfo::exists(torrentFile(m_dataDir, id));
        if (atp.ti == nullptr || !atp.ti->priv()) {
            for (const QString &u : m_userTrackers)
                if (std::find(atp.trackers.begin(), atp.trackers.end(), u.toStdString()) == atp.trackers.end())
                    atp.trackers.push_back(u.toStdString());
        }
        atp.max_connections = m_cfg.maxConnectionsPerTorrent;
        atp.max_uploads = m_cfg.maxUploadsPerTorrent;
        if (!m_cfg.pex)
            atp.flags |= lt::torrent_flags::disable_pex;
        // torrent::init() calls we_have() → piece_finished_alert for every piece in the resume
        // data; those are not new verifications, so they are not reported again
        t.replayed.assign(atp.have_pieces.size(), false);
        for (std::size_t i = 0; i < t.replayed.size(); ++i)
            t.replayed[i] = atp.have_pieces[lt::piece_index_t(int(i))];
        snaps.append(snapshotFromParams(atp, rec));
        if (atp.ti)
            t.ti = atp.ti;
        m_ses->async_add_torrent(std::move(atp));
        ++loaded;
    }
    qCInfo(lcBt) << "restored" << loaded << "torrents from" << torrentsDir(m_dataDir);
    // hand the preliminary snapshots to the facade (it is blocked in init(); it will process them)
    for (const TorrentSnapshot &s : snaps) {
        const Torrent *t = find(s.id);
        const QByteArray hashes = t && t->ti ? v1PieceHashes(*t->ti) : QByteArray();
        const QString seeded = t ? t->rec.seededPath : QString();
        post([sink = m_sink, s, hashes, seeded] {
            sink->wTorrentAdded(s, seeded);
            if (s.hasMetadata)
                sink->wTorrentMetadata(s.id, hashes, s);
        });
    }
}

// ------------------------------------------------------------------------------------------------ persistence
void Worker::persistRecord(const Torrent &t)
{
    const QByteArray json = QJsonDocument(QJsonObject::fromVariantMap(t.rec.toVariant())).toJson(QJsonDocument::Indented);
    QString err;
    if (!writeFileAtomic(recordFile(m_dataDir, t.id), json, &err))
        qCWarning(lcBt) << "cannot write record" << t.id << err;
}

void Worker::deletePersisted(const QString &id)
{
    QFile::remove(recordFile(m_dataDir, id));
    QFile::remove(resumeFile(m_dataDir, id));
    QFile::remove(torrentFile(m_dataDir, id));
}

void Worker::requestResume(Torrent &t, bool infoDict)
{
    if (!t.inSession || t.removing)
        return;
    lt::resume_data_flags_t f = lt::torrent_handle::flush_disk_cache;
    if (infoDict)
        f |= lt::torrent_handle::save_info_dict;
    t.h.save_resume_data(f);
    ++m_resumePending;
    t.lastResumeMs = m_clock.elapsed();
}

void Worker::writeResume(const lt::add_torrent_params &p0)
{
    const QString id = hexId(p0.ti ? p0.ti->info_hashes() : p0.info_hashes);
    Torrent *t = find(id);
    if (!t || t->removing)
        return;
    lt::add_torrent_params p = p0;
    if (p.ti && !t->torrentFileWritten) {
        try {
            const std::vector<char> tf = lt::write_torrent_file_buf(p, {});
            writeFileAtomic(torrentFile(m_dataDir, id), toBytes(tf));
            t->torrentFileWritten = true;
        } catch (const std::exception &e) {
            qCInfo(lcBt) << "cannot export .torrent for" << id << e.what();
            t->torrentFileWritten = true;   // keep it in the fastresume instead
        }
    }
    p.piece_priorities.clear();   // reservations are not persisted
    const std::vector<char> buf = lt::write_resume_data_buf(p);
    QString err;
    if (!writeFileAtomic(resumeFile(m_dataDir, id), toBytes(buf), &err))
        qCWarning(lcBt) << "cannot write fastresume" << id << err;
    t->resumeDirty = false;
}

void Worker::saveSessionState()
{
    if (!m_ses)
        return;
    try {
        const lt::session_params sp = m_ses->session_state(lt::session_handle::save_dht_state);
        const std::vector<char> buf = lt::write_session_params_buf(sp, lt::session_handle::save_dht_state);
        writeFileAtomic(sessionFile(m_dataDir), toBytes(buf));
    } catch (const std::exception &e) {
        qCWarning(lcBt) << "cannot save session state:" << e.what();
    }
}

int Worker::saveAllResume(int timeoutMs, bool all)
{
    if (!m_ses)
        return 0;
    int n = 0;
    for (auto &kv : m_t) {
        Torrent &t = kv.second;
        if (!t.inSession || t.removing)
            continue;
        if (!all && t.haveStatus && !t.st.need_save_resume && !t.resumeDirty)
            continue;
        requestResume(t, true);
        ++n;
    }
    QElapsedTimer el;
    el.start();
    while (m_resumePending > 0 && el.elapsed() < timeoutMs) {
        m_ses->wait_for_alert(lt::milliseconds(100));
        drainAlerts();
    }
    if (m_resumePending > 0)
        qCWarning(lcBt) << m_resumePending << "resume data requests did not finish in" << timeoutMs << "ms";
    m_resumePending = 0;
    return n;
}

void Worker::periodicSave()
{
    for (auto &kv : m_t) {
        Torrent &t = kv.second;
        if (t.inSession && !t.removing && t.haveStatus && (t.st.need_save_resume || t.resumeDirty))
            requestResume(t, true);
    }
}

// ------------------------------------------------------------------------------------------------ online/offline/config
lt::settings_pack Worker::currentPack() const
{
    return makeSettingsPack(m_cfg, m_online, m_port, m_userNodes, m_downBps, m_upBps);
}

void Worker::applySettingsNow()
{
    if (m_ses)
        m_ses->apply_settings(currentPack());
}

void Worker::applyPeerClassFilter()
{
    if (!m_ses)
        return;
    lt::ip_filter f;
    const std::uint32_t global = 1u << static_cast<std::uint32_t>(lt::session::global_peer_class_id);
    const std::uint32_t local = 1u << static_cast<std::uint32_t>(lt::session::local_peer_class_id);
    f.add_rule(lt::make_address("0.0.0.0"), lt::make_address("255.255.255.255"), global);
    f.add_rule(lt::make_address("::"), lt::make_address("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff"), global);
    if (!m_cfg.limitLan) {
        // libtorrent's default: LAN peers are only in the (unlimited) local class
        const char *v4[][2] = {{"10.0.0.0", "10.255.255.255"}, {"172.16.0.0", "172.31.255.255"},
                               {"192.168.0.0", "192.168.255.255"}, {"169.254.0.0", "169.254.255.255"},
                               {"127.0.0.0", "127.255.255.255"}};
        for (auto &r : v4)
            f.add_rule(lt::make_address(r[0]), lt::make_address(r[1]), local);
        f.add_rule(lt::make_address("fe80::"), lt::make_address("febf:ffff:ffff:ffff:ffff:ffff:ffff:ffff"), local);
        f.add_rule(lt::make_address("::1"), lt::make_address("::1"), local);
    }
    m_ses->set_peer_class_filter(f);
}

void Worker::goOnline(const Config &cfg)
{
    if (!m_ses)
        return;
    const bool portChanged = cfg.listenPort != m_cfg.listenPort || cfg.randomizePorts != m_cfg.randomizePorts;
    m_cfg = cfg;
    if (portChanged && !cfg.randomizePorts)
        m_port = cfg.listenPort;
    m_online = true;
    applySettingsNow();
    applyPeerClassFilter();
    m_ses->resume();
    qCInfo(lcBt) << "session online, listen" << qs(currentPack().get_str(lt::settings_pack::listen_interfaces));
    updateStatus();
}

void Worker::goOffline(int timeoutMs)
{
    if (!m_ses)
        return;
    const int n = saveAllResume(timeoutMs, true);
    saveSessionState();
    m_online = false;
    m_ses->pause();
    applySettingsNow();
    m_listen.clear();
    m_listenPort = 0;
    m_dhtNodes = 0;
    qCInfo(lcBt) << "session offline; saved resume data of" << n << "torrents";
    updateStatus();
}

void Worker::teardown(int timeoutMs)
{
    if (!m_ses)
        return;
    if (m_online) {
        goOffline(timeoutMs);
    } else {
        saveAllResume(timeoutMs, false);
        saveSessionState();
    }
    if (m_tick)
        m_tick->stop();
    if (m_saveTimer)
        m_saveTimer->stop();
    if (m_stateTimer)
        m_stateTimer->stop();
    m_ses->set_alert_notify([] {});
    QElapsedTimer el;
    el.start();
    {
        lt::session_proxy proxy = m_ses->abort();
        m_ses.reset();
    }
    qCInfo(lcBt) << "session destroyed in" << el.elapsed() << "ms";
    m_sinkObj = nullptr;
}

void Worker::applyConfig(const Config &cfg)
{
    const bool portChanged = cfg.listenPort != m_cfg.listenPort || cfg.randomizePorts != m_cfg.randomizePorts;
    const bool lanChanged = cfg.limitLan != m_cfg.limitLan;
    const Config old = m_cfg;
    m_cfg = cfg;
    if (portChanged)
        m_port = cfg.randomizePorts ? int(QRandomGenerator::global()->bounded(20000, 60000)) : cfg.listenPort;
    applySettingsNow();
    if (lanChanged)
        applyPeerClassFilter();
    for (auto &kv : m_t) {
        Torrent &t = kv.second;
        if (!t.inSession)
            continue;
        if (old.maxConnectionsPerTorrent != cfg.maxConnectionsPerTorrent)
            t.h.set_max_connections(cfg.maxConnectionsPerTorrent);
        if (old.maxUploadsPerTorrent != cfg.maxUploadsPerTorrent)
            t.h.set_max_uploads(cfg.maxUploadsPerTorrent);
        if (old.pex != cfg.pex) {
            if (cfg.pex)
                t.h.unset_flags(lt::torrent_flags::disable_pex);
            else
                t.h.set_flags(lt::torrent_flags::disable_pex);
        }
    }
}

void Worker::setRateLimits(int downBps, int upBps)
{
    downBps = qMax(0, downBps);
    upBps = qMax(0, upBps);
    m_pacing->downBps.store(downBps);   // [bw-2] the pacing plugin applies it with the next block
    if (downBps == m_downBps && upBps == m_upBps)
        return;
    m_downBps = downBps;
    m_upBps = upBps;
    if (!m_ses)
        return;
    lt::settings_pack p;
    p.set_int(lt::settings_pack::download_rate_limit, downBps);
    p.set_int(lt::settings_pack::upload_rate_limit, upBps);
    applyRatePacing(p, downBps, upBps, m_upPeers);   // [bw-2]
    m_appliedSendBuf = p.get_int(lt::settings_pack::send_buffer_watermark);
    m_ses->apply_settings(std::move(p));
}

// [bw-2] the number of peers we download from / upload to (session counters, every 100 ms): the
// in-flight amount of the limit is split among them
void Worker::updatePacingPeers(int downPeers, int upPeers, qint64 recvBytes)
{
    m_pacing->downPeers.store(qMax(1, downPeers));
    m_upPeers = qMax(1, upPeers);
    // once a second: the in-flight time follows the round trip (bt_pacing.h adaptInflight)
    const qint64 now = m_clock.elapsed();
    if (m_pacingSinceMs < 0 || m_downBps <= 0 || downPeers <= 0) {
        // no limit, or nothing requested from any peer: start again from the default next time
        if (downPeers <= 0) m_pacing->inflightMs.store(int(kInflightSec * 1000));
        m_pacingSinceMs = now;
        m_pacingRecv0 = recvBytes;
    } else {
        if (now - m_pacingSinceMs >= 1000) {
            const double rate = double(qMax<qint64>(0, recvBytes - m_pacingRecv0)) * 1000.0 / double(now - m_pacingSinceMs);
            const double k = adaptInflight(m_pacing->inflightMs.load() / 1000.0, rate, m_downBps);
            m_pacing->inflightMs.store(int(k * 1000));
            m_pacingSinceMs = now;
            m_pacingRecv0 = recvBytes;
        }
    }
    if (!m_ses || m_upBps <= 0)
        return;
    lt::settings_pack p;
    applyRatePacing(p, m_downBps, m_upBps, m_upPeers);
    const int sendBuf = p.get_int(lt::settings_pack::send_buffer_watermark);
    if (sendBuf == m_appliedSendBuf)
        return;
    m_appliedSendBuf = sendBuf;
    lt::settings_pack q;
    q.set_int(lt::settings_pack::send_buffer_watermark, sendBuf);
    m_ses->apply_settings(std::move(q));
}


void Worker::setUserNodes(const QStringList &nodes, const QStringList &trackers)
{
    const QStringList newTrackers = [&] {
        QStringList l;
        for (const QString &t : trackers)
            if (!m_userTrackers.contains(t))
                l << t;
        return l;
    }();
    m_userNodes = nodes;
    m_userTrackers = trackers;
    if (!m_ses)
        return;
    lt::settings_pack p;
    p.set_str(lt::settings_pack::dht_bootstrap_nodes,
              currentPack().get_str(lt::settings_pack::dht_bootstrap_nodes));
    m_ses->apply_settings(std::move(p));
    for (auto &kv : m_t) {
        Torrent &t = kv.second;
        if (!t.inSession || (t.ti && t.ti->priv()))
            continue;
        for (const QString &u : newTrackers)
            t.h.add_tracker(lt::announce_entry(u.toStdString()));
    }
}

void Worker::addDhtNode(const QString &host, int port)
{
    if (m_ses)
        m_ses->add_dht_node({host.toStdString(), port});
}

// ------------------------------------------------------------------------------------------------ torrent commands
void Worker::addTorrent(const AddRequest &req)
{
    if (!m_ses || !req.atp)
        return;
    if (Torrent *old = find(req.id)) {
        if (!old->removing)
            return;   // duplicate: the facade already merged trackers
        // re-added while the old instance is still being removed: forget the old one now
        m_byHandle.erase(old->h);
        m_t.erase(req.id);
    }
    Torrent &t = m_t[req.id];
    t.id = req.id;
    t.rec = req.rec;
    t.ti = req.atp->ti;
    // persist first so that a crash right now still restores the torrent
    persistRecord(t);
    if (!req.torrentFile.isEmpty()) {
        writeFileAtomic(torrentFile(m_dataDir, req.id), req.torrentFile);
        t.torrentFileWritten = true;
    }
    lt::add_torrent_params atp = *req.atp;
    if (!atp.ti || !atp.ti->priv()) {
        for (const QString &u : m_userTrackers)
            if (std::find(atp.trackers.begin(), atp.trackers.end(), u.toStdString()) == atp.trackers.end())
                atp.trackers.push_back(u.toStdString());
    }
    try {
        writeFileAtomic(resumeFile(m_dataDir, req.id), toBytes(lt::write_resume_data_buf(atp)));
    } catch (const std::exception &e) {
        qCWarning(lcBt) << "cannot write initial resume data" << e.what();
    }
    m_ses->async_add_torrent(std::move(atp));
}

void Worker::mergeTrackers(const QString &id, const std::vector<std::string> &trackers)
{
    Torrent *t = find(id);
    if (defer(t, [this, id, trackers] { mergeTrackers(id, trackers); }))
        return;
    if (!t || !t->inSession)
        return;
    for (const std::string &u : trackers)
        t->h.add_tracker(lt::announce_entry(u));
    t->resumeDirty = true;
}

void Worker::removeTorrent(const QString &id, bool deleteFiles)
{
    Torrent *t = find(id);
    if (!t)
        return;
    t->removing = true;
    t->deleteFiles = deleteFiles;
    deletePersisted(id);
    for (auto it = m_up.begin(); it != m_up.end();) {
        if (it->second.info.hashes.btih.toHex() == id.toLatin1()) {
            const QString uid = it->first;
            post([sink = m_sink, uid] { sink->wUploads({}, {uid}); });
            it = m_up.erase(it);
        } else {
            ++it;
        }
    }
    if (t->inSession)
        m_ses->remove_torrent(t->h, deleteFiles ? lt::session::delete_files : lt::remove_flags_t{});
    // else: removed when its add_torrent_alert arrives
}

void Worker::pauseTorrent(const QString &id)
{
    Torrent *t = find(id);
    if (defer(t, [this, id] { pauseTorrent(id); }))
        return;
    if (!t || !t->inSession)
        return;
    t->h.unset_flags(lt::torrent_flags::auto_managed);
    t->h.pause();
    t->resumeDirty = true;
}

void Worker::resumeTorrent(const QString &id)
{
    Torrent *t = find(id);
    if (defer(t, [this, id] { resumeTorrent(id); }))
        return;
    if (!t || !t->inSession)
        return;
    t->h.clear_error();
    t->h.set_flags(lt::torrent_flags::auto_managed);
    t->h.resume();
    t->resumeDirty = true;
}

void Worker::applyWanted(Torrent &t, const QList<int> &indexes)
{
    if (!t.ti)
        return;
    const lt::file_storage &fs = t.ti->files();
    std::vector<lt::download_priority_t> v(std::size_t(fs.num_files()), lt::dont_download);
    for (int i : indexes)
        if (i >= 0 && i < fs.num_files() && !fs.pad_file_at(lt::file_index_t(i)))
            v[std::size_t(i)] = lt::default_priority;
    const bool wasLtFinished = t.haveStatus && t.st.is_finished;
    // newly wanted files that are not complete → the torrent is unfinished again, even if libtorrent
    // completes them before we ever sample an unfinished status (then torrentFinished fires again)
    {
        std::vector<std::int64_t> fp;
        t.h.file_progress(fp, lt::torrent_handle::piece_granularity);
        for (std::size_t i = 0; i < v.size(); ++i) {
            const bool was = i < t.filePrio.size() ? t.filePrio[i] != lt::dont_download : true;
            if (v[i] != lt::dont_download && !was && i < fp.size() && fp[i] < fs.file_size(lt::file_index_t(int(i))))
                t.finishedReported = false;
        }
    }
    t.h.prioritize_files(v);
    t.filePrio = v;
    applyFirstLast(t);      // [dl-1] file priorities overwrite piece priorities: band pieces again
    applyReservations(t);   // file priorities overwrite piece priorities: re-assert reservations
    // status() is executed after the posted priority jobs → is_finished reflects the new wishes
    t.st = t.h.status(kStatusFlags);
    t.haveStatus = true;
    if (wasLtFinished && !t.st.is_finished)
        wakeTorrent(t);
    checkFinished(t);
    refreshSoon(t.id);   // file priorities reach the picker only after a disk job
    t.resumeDirty = true;
}

void Worker::setWantedFiles(const QString &id, const QList<int> &indexes)
{
    Torrent *t = find(id);
    if (!t)
        return;
    if (!t->inSession || !t->ti) {
        t->pendingWanted = indexes;
        return;
    }
    applyWanted(*t, indexes);
    requestResume(*t);
}

void Worker::setPriority(const QString &id, int priority)
{
    Torrent *t = find(id);
    if (!t)
        return;
    t->rec.priority = qBound(-1, priority, 1);
    persistRecord(*t);
    if (!t->inSession)
        return;
    if (priority > 0)
        t->h.queue_position_top();
    else if (priority < 0)
        t->h.queue_position_bottom();
}

void Worker::recheck(const QString &id)
{
    Torrent *t = find(id);
    if (defer(t, [this, id] { recheck(id); }))
        return;
    if (t && t->inSession) {
        t->replayed.clear();
        t->h.force_recheck();
        t->finishedReported = false;
    }
}

void Worker::moveStorage(const QString &id, const QString &dir)
{
    Torrent *t = find(id);
    if (defer(t, [this, id, dir] { moveStorage(id, dir); }))
        return;
    if (!t || !t->inSession) {
        post([sink = m_sink, id, dir] { sink->wStorageMoved(id, dir, false, QStringLiteral("unknown torrent")); });
        return;
    }
    t->h.move_storage(dir.toStdString(), lt::move_flags_t::fail_if_exist);
}

void Worker::reannounce(const QString &id)
{
    Torrent *t = find(id);
    if (defer(t, [this, id] { reannounce(id); }))
        return;
    if (!t || !t->inSession)
        return;
    t->h.force_reannounce(0, -1, lt::torrent_handle::ignore_min_interval);
    if (m_online && m_cfg.dht)
        t->h.force_dht_announce();
}

lt::download_priority_t Worker::naturalPiecePriority(const Torrent &t, int piece) const
{
    if (!t.ti)
        return lt::default_priority;
    const lt::file_storage &fs = t.ti->files();
    const std::vector<lt::file_slice> slices = fs.map_block(lt::piece_index_t(piece), 0, fs.piece_size(lt::piece_index_t(piece)));
    std::uint8_t best = 0;
    for (const lt::file_slice &s : slices) {
        if (fs.pad_file_at(s.file_index) || s.size == 0)
            continue;
        const std::size_t fi = std::size_t(static_cast<int>(s.file_index));
        const std::uint8_t p = fi < t.filePrio.size() ? static_cast<std::uint8_t>(t.filePrio[fi])
                                                      : static_cast<std::uint8_t>(lt::default_priority);
        best = std::max(best, p);
    }
    // [dl-1] first and last pieces first: the band pieces of wanted files go to the top
    if (best > 0 && t.firstLast && t.bandPieces.count(piece))
        best = static_cast<std::uint8_t>(lt::top_priority);
    return lt::download_priority_t(best);
}

// [dl-1] qBittorrent's "Download first and last pieces first", per wanted file: the pieces covering
// the head and the tail band (mx::order: 1 % of the file, at least 4 MiB, at most a quarter) get the
// top priority; with the flag off they go back to the file's priority. Reserved pieces stay reserved.
void Worker::applyFirstLast(Torrent &t)
{
    if (!t.inSession || !t.ti)
        return;
    const lt::file_storage &fs = t.ti->files();
    std::set<int> band;
    if (t.firstLast) {
        const qint64 L = fs.piece_length();
        for (int i = 0; i < fs.num_files(); ++i) {
            const lt::file_index_t fi(i);
            if (fs.pad_file_at(fi) || fs.file_size(fi) <= 0)
                continue;
            if (std::size_t(i) < t.filePrio.size() && t.filePrio[std::size_t(i)] == lt::dont_download)
                continue;
            const qint64 off = fs.file_offset(fi), len = fs.file_size(fi), end = off + len;
            const qint64 he = off + mx::order::headBandBytes(len), ts = end - mx::order::tailBandBytes(len);
            for (qint64 p = off / L; p <= (he - 1) / L; ++p) band.insert(int(p));
            for (qint64 p = ts / L; p <= (end - 1) / L; ++p) band.insert(int(p));
        }
    }
    std::set<int> touch = band;
    touch.insert(t.bandPieces.begin(), t.bandPieces.end());
    t.bandPieces = band;
    std::vector<std::pair<lt::piece_index_t, lt::download_priority_t>> v;
    for (int p : touch) {
        if (p < 0 || p >= t.ti->num_pieces() || t.reserved.count(p))
            continue;
        v.emplace_back(lt::piece_index_t(p), naturalPiecePriority(t, p));
    }
    if (!v.empty())
        t.h.prioritize_pieces(v);
}

void Worker::setOrder(const QString &id, bool sequential, bool firstLast)
{
    Torrent *t = find(id);
    if (defer(t, [this, id, sequential, firstLast] { setOrder(id, sequential, firstLast); }))
        return;
    if (!t || !t->inSession)
        return;
    const bool changed = t->seqOrder != sequential || t->firstLast != firstLast;
    t->seqOrder = sequential;
    t->firstLast = firstLast;
    if (sequential)
        t->h.set_flags(lt::torrent_flags::sequential_download);
    else
        t->h.unset_flags(lt::torrent_flags::sequential_download);
    applyFirstLast(*t);   // no metadata yet: onMetadata() applies it
    if (changed)
        qCInfo(lcBt).noquote() << "download order of" << id.left(8) << (sequential ? "sequential" : "normal")
                               << (firstLast ? "+ first and last pieces first" : "") << "(" << t->bandPieces.size() << "band pieces)";
}

void Worker::applyReservations(Torrent &t)
{
    if (!t.inSession || !t.ti || t.reserved.empty())
        return;
    std::vector<std::pair<lt::piece_index_t, lt::download_priority_t>> v;
    for (int p : t.reserved)
        if (p >= 0 && p < t.ti->num_pieces())
            v.emplace_back(lt::piece_index_t(p), lt::dont_download);
    t.h.prioritize_pieces(v);
}

void Worker::reserve(const QString &id, const QList<int> &pieces, bool on)
{
    Torrent *t = find(id);
    if (defer(t, [this, id, pieces, on] { reserve(id, pieces, on); }))
        return;
    if (!t)
        return;
    const int n = t->ti ? t->ti->num_pieces() : INT_MAX;
    std::vector<std::pair<lt::piece_index_t, lt::download_priority_t>> v;
    for (int p : pieces) {
        if (p < 0 || p >= n)
            continue;
        if (on) {
            if (t->inSession && t->ti && t->h.have_piece(lt::piece_index_t(p)))
                continue;
            if (t->reserved.insert(p).second)
                v.emplace_back(lt::piece_index_t(p), lt::dont_download);
        } else if (t->reserved.erase(p)) {
            v.emplace_back(lt::piece_index_t(p), naturalPiecePriority(*t, p));
        }
    }
    if (!v.empty() && t->inSession && t->ti) {
        const bool wasLtFinished = t->haveStatus && t->st.is_finished;
        t->h.prioritize_pieces(v);
        t->st = t->h.status(kStatusFlags);
        t->haveStatus = true;
        if (wasLtFinished && !t->st.is_finished)
            wakeTorrent(*t);
        refreshSoon(id);
    }
}

void Worker::inject(const QString &id, int piece, const QByteArray &data)
{
    Torrent *t = find(id);
    auto fail = [&] {
        if (t)
            t->pieceFail << piece;
        else
            post([sink = m_sink, id, piece] { sink->wPieces(id, {}, {piece}); });
    };
    if (!t || !t->ti || t->removing) {
        fail();
        flushPieceEvents();
        return;
    }
    const lt::torrent_info &ti = *t->ti;
    if (piece < 0 || piece >= ti.num_pieces() || data.size() != ti.piece_size(lt::piece_index_t(piece))) {
        qCInfo(lcBt) << "inject" << id << piece << ": bad index or size" << data.size();
        fail();
        flushPieceEvents();
        return;
    }
    if (t->inSession && t->h.have_piece(lt::piece_index_t(piece))) {
        t->pieceDone << piece;   // no-op, but settle the caller's bookkeeping
        flushPieceEvents();
        return;
    }
    if (ti.info_hashes().has_v1()) {
        lt::hasher h(data.constData(), data.size());
        if (h.final() != ti.hash_for_piece(lt::piece_index_t(piece))) {
            qCInfo(lcBt) << "inject" << id << piece << ": SHA-1 mismatch, rejected before writing";
            fail();
            flushPieceEvents();
            return;
        }
    }
    if (!t->inSession || isChecking(t->h.status({}).state)) {
        t->held[piece] = data;
        return;
    }
    Inject &inj = t->injected[piece];
    inj.data = data;
    inj.retries = 0;
    t->h.add_piece(lt::piece_index_t(piece), std::vector<char>(data.constBegin(), data.constEnd()));
}

void Worker::flushHeld(Torrent &t)
{
    if (t.held.empty())
        return;
    auto held = std::move(t.held);
    t.held.clear();
    for (auto &kv : held)
        inject(t.id, kv.first, kv.second);
}

void Worker::addPeer(const QString &id, const QString &endpoint)
{
    Torrent *t = find(id);
    QString host;
    int port = 0;
    if (!t || t->removing || !parseHostPort(endpoint, &host, &port, 6881))
        return;
    lt::error_code ec;
    const lt::address a = lt::make_address(host.toStdString(), ec);
    if (ec)
        return;
    const lt::tcp::endpoint ep(a, std::uint16_t(port));
    if (t->inSession)
        t->h.connect_peer(ep);
    else
        t->pendingPeers.push_back(ep);
}

void Worker::banPeerAddress(const QString &address)
{
    if (!m_ses)
        return;
    lt::error_code ec;
    lt::make_address(address.toStdString(), ec);
    if (ec)
        return;
    m_banned.insert(address);
    applyIpFilter();
}

// libtorrent 2.0.15 quirk (read in torrent.cpp / peer_list.cpp, verified by
// tst_bt_inject::unreserveHandsBackToPicker): when a torrent goes from "finished" (only priority-0
// pieces missing) back to "downloading" because priorities changed, the peer list's connect-candidate
// count is only recalculated inside connect_one_peer(), which the session only calls for torrents with
// candidates > 0. Seeds are not candidates while finished, so a torrent whose known peers are all
// seeds never reconnects. Adding one NEW peer entry makes candidates > 0 → connect_one_peer() →
// recalculation → the known seeds become candidates again. We add an RFC 5737 TEST-NET-1 address
// (never routable; the attempt just fails) and also re-announce.
void Worker::wakeTorrent(Torrent &t)
{
    if (!t.inSession)
        return;
    const std::uint32_t n = QRandomGenerator::global()->bounded(1u, 255u);
    const lt::address_v4 dummy(0xC0000200u | n);   // 192.0.2.n
    t.h.connect_peer(lt::tcp::endpoint(dummy, std::uint16_t(QRandomGenerator::global()->bounded(20000, 60000))));
    if (m_online) {
        t.h.force_reannounce(0, -1, lt::torrent_handle::ignore_min_interval);
        if (m_cfg.dht)
            t.h.force_dht_announce();
    }
}

void Worker::refreshSoon(const QString &id)
{
    for (int ms : {300, 1500}) {
        QTimer::singleShot(ms, this, [this, id] {
            Torrent *t = find(id);
            if (!t || !t->inSession || t->removing)
                return;
            const bool wasLtFinished = t->haveStatus && t->st.is_finished;
            t->st = t->h.status(kStatusFlags);
            t->haveStatus = true;
            if (wasLtFinished && !t->st.is_finished)
                wakeTorrent(*t);
            const std::vector<lt::download_priority_t> ltPrio = t->h.get_file_priorities();
            if (!t->filePrio.empty() && ltPrio != t->filePrio) {
                qCInfo(lcBt) << "file priorities of" << id << "not applied by libtorrent yet; re-applying";
                t->h.prioritize_files(t->filePrio);
                applyFirstLast(*t);   // [dl-1]
                applyReservations(*t);
            } else if (t->firstLast) {
                // [dl-1] a file-priority job that landed after the band priorities overwrote them
                applyFirstLast(*t);
                applyReservations(*t);
            }
            checkFinished(*t);
            const QList<TorrentSnapshot> snaps{makeSnapshot(*t)};
            post([sink = m_sink, snaps] { sink->wSnapshots(snaps); });
        });
    }
}

void Worker::applyIpFilter()
{
    if (!m_ses)
        return;
    lt::ip_filter f;
    lt::error_code ec;
    for (const QString &b : std::as_const(m_banned)) {
        const lt::address a = lt::make_address(b.toStdString(), ec);
        if (!ec)
            f.add_rule(a, a, lt::ip_filter::blocked);
    }
    m_ses->set_ip_filter(f);
}

// ------------------------------------------------------------------------------------------------ alerts
void Worker::scheduleAlerts()
{
    // called on a libtorrent thread
    if (!m_alertsPending.exchange(true))
        QMetaObject::invokeMethod(this, [this] { drainAlerts(); }, Qt::QueuedConnection);
}

void Worker::drainAlerts()
{
    m_alertsPending = false;
    if (!m_ses)
        return;
    std::vector<lt::alert *> alerts;
    m_ses->pop_alerts(&alerts);
    for (lt::alert *a : alerts) {
        try {
            handleAlert(a);
        } catch (const std::exception &e) {
            qCWarning(lcBt) << "alert handler:" << e.what();
        }
    }
    flushPieceEvents();
}

void Worker::flushPieceEvents()
{
    for (auto &kv : m_t) {
        Torrent &t = kv.second;
        if (t.pieceDone.isEmpty() && t.pieceFail.isEmpty())
            continue;
        const QList<int> done = std::exchange(t.pieceDone, {});
        const QList<int> fail = std::exchange(t.pieceFail, {});
        post([sink = m_sink, id = t.id, done, fail] { sink->wPieces(id, done, fail); });
    }
}

void Worker::onMetadata(Torrent &t)
{
    if (t.metadataReported || !t.inSession)
        return;
    auto ti = t.h.torrent_file();
    if (!ti || !ti->is_valid())
        return;
    t.ti = ti;
    t.metadataReported = true;
    t.filePrio = t.h.get_file_priorities();
    if (!t.pendingWanted.isEmpty()) {
        applyWanted(t, t.pendingWanted);
        t.pendingWanted.clear();
    }
    if (!m_cfg.webSeeds)
        for (const lt::web_seed_entry &w : ti->web_seeds())
            t.h.remove_url_seed(w.url);
    applyFirstLast(t);   // [dl-1] the band pieces are known now
    applyReservations(t);
    if (t.rec.name.isEmpty()) {
        t.rec.name = qs(ti->name());
        persistRecord(t);
    }
    const QByteArray hashes = v1PieceHashes(*ti);
    const TorrentSnapshot snap = makeSnapshot(t);
    post([sink = m_sink, id = t.id, hashes, snap] { sink->wTorrentMetadata(id, hashes, snap); });
}

bool Worker::reallyFinished(const Torrent &t) const
{
    if (!t.haveStatus || !t.st.has_metadata)
        return false;
    if (!(t.st.is_finished || t.st.is_seeding))
        return false;
    if (isChecking(t.st.state))
        return false;
    // libtorrent's is_finished can be stale right after a priority change (file priorities reach the
    // picker only after a disk job): the files themselves are the ground truth
    if (t.inSession && t.ti && !t.st.is_seeding) {
        std::vector<std::int64_t> fp;
        t.h.file_progress(fp, lt::torrent_handle::piece_granularity);
        const lt::file_storage &fs = t.ti->files();
        for (int i = 0; i < fs.num_files() && std::size_t(i) < fp.size(); ++i) {
            const lt::file_index_t fi(i);
            const bool wanted = !fs.pad_file_at(fi)
                                && (std::size_t(i) >= t.filePrio.size() || t.filePrio[std::size_t(i)] != lt::dont_download);
            if (wanted && fp[std::size_t(i)] < fs.file_size(fi))
                return false;
        }
    }
    for (int p : t.reserved) {
        if (naturalPiecePriority(t, p) == lt::dont_download)
            continue;
        const bool have = int(t.st.pieces.size()) > p ? t.st.pieces[lt::piece_index_t(p)] : false;
        if (!have)
            return false;
    }
    return true;
}

void Worker::checkFinished(Torrent &t)
{
    const bool fin = reallyFinished(t);
    if (fin && !t.finishedReported) {
        t.finishedReported = true;
        post([sink = m_sink, snap = makeSnapshot(t)] { sink->wTorrentFinished(snap); });
        requestResume(t);
        if (!m_cfg.seedCompleted && t.rec.seededPath.isEmpty()) {
            t.h.unset_flags(lt::torrent_flags::auto_managed);
            t.h.pause();
        }
    } else if (!fin && t.finishedReported && t.haveStatus && !(t.st.is_finished || t.st.is_seeding)) {
        t.finishedReported = false;   // more pieces wanted again (wanted files / recheck)
    }
}

void Worker::handleAlert(lt::alert *a)
{
    switch (a->type()) {
    case lt::add_torrent_alert::alert_type: {
        auto *aa = static_cast<lt::add_torrent_alert *>(a);
        const QString id = hexId(aa->params.ti ? aa->params.ti->info_hashes() : aa->params.info_hashes);
        Torrent *t = find(id);
        if (aa->error) {
            const QString msg = qs(aa->error.message());
            qCWarning(lcBt) << "add failed" << id << msg;
            if (t && !t->inSession) {
                deletePersisted(id);
                m_t.erase(id);
            }
            post([sink = m_sink, id, msg] {
                sink->wTorrentError(id, msg);
                sink->wTorrentRemoved(id);
            });
            break;
        }
        if (!t) {   // unknown (should not happen) → drop it
            m_ses->remove_torrent(aa->handle);
            break;
        }
        t->h = aa->handle;
        t->inSession = true;
        m_byHandle[aa->handle] = id;
        if (t->removing) {
            m_ses->remove_torrent(t->h, t->deleteFiles ? lt::session::delete_files : lt::remove_flags_t{});
            break;
        }
        t->st = t->h.status(kStatusFlags);
        t->haveStatus = true;
        if (t->rec.priority > 0)
            t->h.queue_position_top();
        else if (t->rec.priority < 0)
            t->h.queue_position_bottom();
        const TorrentSnapshot snap = makeSnapshot(*t);
        post([sink = m_sink, snap, seeded = t->rec.seededPath] { sink->wTorrentAdded(snap, seeded); });
        if (t->st.has_metadata)
            onMetadata(*t);
        {
            auto ops = std::move(t->pendingOps);
            t->pendingOps.clear();
            const QString tid = t->id;
            for (auto &op : ops)
                op();
            t = find(tid);
            if (!t)
                break;
        }
        for (const lt::tcp::endpoint &ep : t->pendingPeers)
            t->h.connect_peer(ep);
        t->pendingPeers.clear();
        if (!isChecking(t->st.state))
            flushHeld(*t);
        requestResume(*t);
        if (!t->rec.seededPath.isEmpty() && m_online && m_cfg.dht)
            t->h.force_dht_announce();
        break;
    }
    case lt::metadata_received_alert::alert_type: {
        auto *ma = static_cast<lt::metadata_received_alert *>(a);
        if (Torrent *t = find(ma->handle)) {
            t->st = t->h.status(kStatusFlags);
            onMetadata(*t);
            requestResume(*t);
        }
        break;
    }
    case lt::state_update_alert::alert_type: {
        auto *su = static_cast<lt::state_update_alert *>(a);
        QList<TorrentSnapshot> snaps;
        QList<TransferInfo> upChanged;
        QSet<QString> refreshed, seenUp;
        const qint64 now = m_clock.elapsed();
        for (const lt::torrent_status &s : su->status) {
            Torrent *t = find(s.handle);
            if (!t || t->removing)
                continue;
            const bool wasChecking = t->haveStatus && isChecking(t->st.state);
            const bool wasLtFinished = t->haveStatus && t->st.is_finished;
            t->st = s;
            if (wasLtFinished && !s.is_finished && !isChecking(s.state) && !(s.flags & lt::torrent_flags::paused))
                wakeTorrent(*t);   // left "finished" (priorities changed): see wakeTorrent()
            t->haveStatus = true;
            if (s.has_metadata && !t->metadataReported)
                onMetadata(*t);
            if (wasChecking && !isChecking(s.state))
                flushHeld(*t);
            refreshPeers(*t, now, upChanged, seenUp);
            refreshQueue(*t);   // [ui-i]
            refreshed.insert(t->id);
            checkFinished(*t);
            snaps.append(makeSnapshot(*t));
        }
        sweepUploads(now, refreshed, seenUp, upChanged);
        if (!snaps.isEmpty())
            post([sink = m_sink, snaps] { sink->wSnapshots(snaps); });
        postDiag();   // [diag-1]
        break;
    }
    case lt::piece_finished_alert::alert_type: {
        auto *pa = static_cast<lt::piece_finished_alert *>(a);
        if (Torrent *t = find(pa->handle)) {
            const int p = static_cast<int>(pa->piece_index);
            if (std::size_t(p) < t->replayed.size() && t->replayed[std::size_t(p)]) {
                t->replayed[std::size_t(p)] = false;
                break;
            }
            t->pieceDone << p;
            t->injected.erase(p);
            if (int(t->st.pieces.size()) > p)
                t->st.pieces.set_bit(pa->piece_index);
            if (t->reserved.erase(p)) {
                // verified reserved piece: give it its normal priority back so it counts as wanted
                t->h.prioritize_pieces(std::vector<std::pair<lt::piece_index_t, lt::download_priority_t>>{
                    {pa->piece_index, naturalPiecePriority(*t, p)}});
            }
            t->resumeDirty = true;
        }
        break;
    }
    case lt::hash_failed_alert::alert_type: {
        auto *ha = static_cast<lt::hash_failed_alert *>(a);
        if (Torrent *t = find(ha->handle)) {
            const int p = static_cast<int>(ha->piece_index);
            auto it = t->injected.find(p);
            if (it != t->injected.end() && it->second.retries < 2) {
                // our data was pre-verified: a peer's bad block was mixed in. libtorrent resets the
                // piece asynchronously (disk clear): re-inject a little later.
                ++it->second.retries;
                const QString id = t->id;
                QTimer::singleShot(400, this, [this, id, p] {
                    Torrent *tt = find(id);
                    if (!tt || !tt->inSession)
                        return;
                    auto i2 = tt->injected.find(p);
                    if (i2 == tt->injected.end())
                        return;
                    tt->h.add_piece(lt::piece_index_t(p), std::vector<char>(i2->second.data.constBegin(), i2->second.data.constEnd()));
                });
            } else {
                if (it != t->injected.end())
                    t->injected.erase(it);
                t->pieceFail << p;
            }
        }
        break;
    }
    case lt::torrent_finished_alert::alert_type: {
        auto *fa = static_cast<lt::torrent_finished_alert *>(a);
        if (Torrent *t = find(fa->handle)) {
            t->st = t->h.status(kStatusFlags);
            t->haveStatus = true;
            checkFinished(*t);
            requestResume(*t);
        }
        break;
    }
    case lt::torrent_checked_alert::alert_type: {
        auto *ca = static_cast<lt::torrent_checked_alert *>(a);
        if (Torrent *t = find(ca->handle)) {
            t->st = t->h.status(kStatusFlags);
            // [dl-1] the check re-derives piece priorities from the file priorities: band pieces again
            if (t->firstLast) {
                applyFirstLast(*t);
                applyReservations(*t);
            }
            flushHeld(*t);
        }
        break;
    }
    case lt::torrent_paused_alert::alert_type:
    case lt::torrent_resumed_alert::alert_type: {
        auto *ta = static_cast<lt::torrent_alert *>(a);
        if (Torrent *t = find(ta->handle))
            if (t->resumeDirty)
                requestResume(*t);
        break;
    }
    case lt::save_resume_data_alert::alert_type: {
        auto *ra = static_cast<lt::save_resume_data_alert *>(a);
        if (m_resumePending > 0)
            --m_resumePending;
        writeResume(ra->params);
        break;
    }
    case lt::save_resume_data_failed_alert::alert_type: {
        auto *ra = static_cast<lt::save_resume_data_failed_alert *>(a);
        if (m_resumePending > 0)
            --m_resumePending;
        if (ra->error != lt::errors::resume_data_not_modified)
            qCInfo(lcBt) << "save_resume_data failed:" << qs(ra->error.message());
        break;
    }
    case lt::torrent_removed_alert::alert_type: {
        auto *ra = static_cast<lt::torrent_removed_alert *>(a);
        const QString id = hexId(ra->info_hashes);
        m_byHandle.erase(ra->handle);
        auto it = m_t.find(id);
        if (it != m_t.end() && it->second.removing) {
            m_t.erase(it);
            post([sink = m_sink, id] { sink->wTorrentRemoved(id); });
        }
        break;
    }
    case lt::torrent_delete_failed_alert::alert_type: {
        auto *da = static_cast<lt::torrent_delete_failed_alert *>(a);
        if (da->error)   // libtorrent also posts this (with no error) for torrents without metadata
            qCWarning(lcBt) << "deleting files failed:" << qs(da->error.message());
        break;
    }
    case lt::torrent_error_alert::alert_type: {
        auto *ea = static_cast<lt::torrent_error_alert *>(a);
        if (Torrent *t = find(ea->handle)) {
            const QString msg = qs(ea->error.message()) + (ea->filename() && *ea->filename() ? QStringLiteral(" (%1)").arg(QString::fromUtf8(ea->filename())) : QString());
            post([sink = m_sink, id = t->id, msg] { sink->wTorrentError(id, msg); });
            t->resumeDirty = true;
        }
        break;
    }
    case lt::storage_moved_alert::alert_type: {   // [engine-3]
        auto *ma = static_cast<lt::storage_moved_alert *>(a);
        if (Torrent *t = find(ma->handle)) {
            const QString path = QString::fromUtf8(ma->storage_path());
            t->rec.savePath = path;
            persistRecord(*t);
            t->resumeDirty = true;
            requestResume(*t);
            post([sink = m_sink, id = t->id, path] { sink->wStorageMoved(id, path, true, QString()); });
        }
        break;
    }
    case lt::storage_moved_failed_alert::alert_type: {
        auto *ma = static_cast<lt::storage_moved_failed_alert *>(a);
        if (Torrent *t = find(ma->handle)) {
            // fail_if_exist reports a collision with an empty error code ("Success")
            QString msg = !ma->error ? QStringLiteral("a file of that name already exists at the target") : qs(ma->error.message());
            if (ma->file_path() && *ma->file_path()) msg += QStringLiteral(" (%1)").arg(QString::fromUtf8(ma->file_path()));
            post([sink = m_sink, id = t->id, msg] { sink->wStorageMoved(id, QString(), false, msg); });
        }
        break;
    }
    case lt::file_error_alert::alert_type: {
        auto *ea = static_cast<lt::file_error_alert *>(a);
        if (Torrent *t = find(ea->handle)) {
            const QString msg = QStringLiteral("%1: %2").arg(QString::fromUtf8(ea->filename()), qs(ea->error.message()));
            post([sink = m_sink, id = t->id, msg] { sink->wTorrentError(id, msg); });
        }
        break;
    }
    case lt::performance_alert::alert_type:
        qCDebug(lcBt).noquote() << qs(a->message());
        break;
    case lt::metadata_failed_alert::alert_type:
    case lt::fastresume_rejected_alert::alert_type:
    case lt::dht_bootstrap_alert::alert_type:
        qCInfo(lcBt).noquote() << qs(a->message());
        break;
    case lt::listen_succeeded_alert::alert_type: {
        auto *la = static_cast<lt::listen_succeeded_alert *>(a);
        if (la->socket_type == lt::socket_type_t::tcp || la->socket_type == lt::socket_type_t::tcp_ssl) {
            m_listen.insert(QStringLiteral("%1:%2").arg(qs(la->address.to_string())).arg(la->port));
            m_listenPort = la->port;
        }
        qCInfo(lcBt).noquote() << qs(a->message());
        updateStatus();
        break;
    }
    case lt::listen_failed_alert::alert_type: {
        const QString msg = qs(a->message());
        {
            auto *lf = static_cast<lt::listen_failed_alert *>(a);
            m_listenError = qs(lf->error.message());
            m_listenErrorPort = lf->port;
        }
        qCWarning(lcBt).noquote() << msg;
        post([sink = m_sink, msg] { sink->wLog(QStringLiteral("BitTorrent: ") + msg); });
        break;
    }
    case lt::external_ip_alert::alert_type: {
        auto *ea = static_cast<lt::external_ip_alert *>(a);
        m_externalIp = qs(ea->external_address.to_string());
        break;
    }
    case lt::portmap_alert::alert_type:
    case lt::portmap_error_alert::alert_type: {
        const QString msg = qs(a->message());
        diagAlert(a);
        qCInfo(lcBt).noquote() << msg;
        post([sink = m_sink, msg] { sink->wLog(QStringLiteral("BitTorrent: ") + msg); });
        break;
    }
    case lt::tracker_error_alert::alert_type:
    case lt::tracker_warning_alert::alert_type:
    case lt::scrape_failed_alert::alert_type:
        qCDebug(lcBt).noquote() << qs(a->message());
        diagAlert(a);
        break;
    case lt::tracker_reply_alert::alert_type:
        qCDebug(lcBt).noquote() << qs(a->message());
        diagAlert(a);
        break;
    case lt::peer_connect_alert::alert_type:        // [diag-1]
    case lt::peer_disconnected_alert::alert_type:
    case lt::peer_error_alert::alert_type:
    case lt::incoming_connection_alert::alert_type:
    case lt::dht_reply_alert::alert_type:
        diagAlert(a);
        break;
    case lt::session_stats_alert::alert_type: {
        auto *sa = static_cast<lt::session_stats_alert *>(a);
        const auto c = sa->counters();
        const qint64 now = m_clock.elapsed();
        if (m_idxDhtNodes >= 0)
            m_dhtNodes = int(c[m_idxDhtNodes]);
        if (m_idxIncoming >= 0 && c[m_idxIncoming] > 0)
            m_incoming = true;
        if (m_idxDhtMsgIn >= 0)
            m_dhtMsgIn = qint64(c[m_idxDhtMsgIn]);
        const qint64 recv = m_idxRecv >= 0 ? c[m_idxRecv] : 0;
        const qint64 sent = m_idxSent >= 0 ? c[m_idxSent] : 0;
        const qint64 recvP = m_idxRecvPayload >= 0 ? c[m_idxRecvPayload] : 0;
        const qint64 sentP = m_idxSentPayload >= 0 ? c[m_idxSentPayload] : 0;
        // [bw-2] stats come every 100 ms; the rates of the status line stay one-second figures
        const bool statusDue = m_lastRecv < 0 || now - m_lastStatsMs >= 900;
        if (statusDue && m_lastRecv >= 0 && now > m_lastStatsMs) {
            const qint64 dr = qMax<qint64>(0, recv - m_lastRecv), ds = qMax<qint64>(0, sent - m_lastSent);
            const double dt = (now - m_lastStatsMs) / 1000.0;
            m_rateDown = int(dr / dt);
            m_rateUp = int(ds / dt);
        }
        // [glue-3] the Bandwidth graph gets everything libtorrent moved (peers incl. protocol, DHT,
        // trackers) with the payload part, since the last stats alert
        auto metric = [&c](int idx) -> qint64 { return idx >= 0 ? qint64(c[idx]) : 0; };
        const qint64 otherRecv = metric(m_idxDhtIn) + metric(m_idxTrackerIn);
        const qint64 otherSent = metric(m_idxDhtOut) + metric(m_idxTrackerOut);
        const qint64 wireRecv = recv + otherRecv;
        const qint64 wireSent = sent + otherSent;
        if (m_lastWireRecv >= 0) {
            const qint64 wr = qMax<qint64>(0, wireRecv - m_lastWireRecv), ws = qMax<qint64>(0, wireSent - m_lastWireSent);
            const qint64 pr = qMax<qint64>(0, recvP - m_lastRecvPayload), ps = qMax<qint64>(0, sentP - m_lastSentPayload);
            // [bw-2] DHT and trackers find peers: counted for the graph, never charged to the limits
            const qint64 xr = qBound<qint64>(0, otherRecv - m_lastOtherRecv, wr), xs = qBound<qint64>(0, otherSent - m_lastOtherSent, ws);
            if (wr || ws)
                post([sink = m_sink, wr, ws, pr, ps, xr, xs] {
                    sink->wAccount(wr - xr, ws - xs, pr, ps);
                    sink->wAccountDiscovery(xr, xs);
                });
        }
        m_lastOtherRecv = otherRecv;
        m_lastOtherSent = otherSent;
        if (m_idxDownPeers >= 0 && m_idxUpPeers >= 0)   // [bw-2]
            updatePacingPeers(int(metric(m_idxDownPeers)), int(metric(m_idxUpPeers)), recv);
        {   // [bw-2] held back by the limit: the shared budget gives libtorrent an equal share
            const qint64 qd = metric(m_idxLimDownBytes), qu = metric(m_idxLimUpBytes);
            if (qd > 0 || qu > 0)
                post([sink = m_sink, qd, qu] { sink->wWaiting(qd, qu); });
        }
        m_lastWireRecv = wireRecv;
        m_lastWireSent = wireSent;
        m_lastRecvPayload = recvP;
        m_lastSentPayload = sentP;
        if (statusDue) {
            m_lastRecv = recv;
            m_lastSent = sent;
            m_lastStatsMs = now;
            updateStatus();
        }
        break;
    }
    default:
        break;
    }
}

// ------------------------------------------------------------------------------------------------ periodic
void Worker::tick()
{
    if (!m_ses)
        return;
    m_ses->post_torrent_updates(kStatusFlags);   // [bw-2] session stats: m_statsTimer
    const qint64 now = m_clock.elapsed();
    QStringList toRemove;
    for (auto &kv : m_t) {
        Torrent &t = kv.second;
        if (!t.inSession || t.removing)
            continue;
        enforceSeedLimits(t);
        if (t.rec.seedLimitReached && m_cfg.seedLimitAction == QLatin1String("remove") && t.haveStatus
            && t.st.is_seeding && t.rec.seededPath.isEmpty())
            toRemove << t.id;
        // resume data for torrents that changed (piece progress) at most every 30 s via periodicSave;
        // structural changes (pause/priorities/trackers) are saved within ~1 s
        if (t.resumeDirty && now - t.lastResumeMs > 1000 && !(t.haveStatus && t.st.need_save_resume && now - t.lastResumeMs < 30000))
            requestResume(t);
    }
    for (const QString &id : toRemove) {
        qCInfo(lcBt) << "seed limit reached, removing" << id;
        removeTorrent(id, false);
    }
}

void Worker::enforceSeedLimits(Torrent &t)
{
    if (!t.haveStatus || t.rec.seedLimitReached || !t.rec.seededPath.isEmpty())
        return;
    if (!t.st.is_seeding || (t.st.flags & lt::torrent_flags::paused))
        return;
    bool reached = false;
    if (m_cfg.seedRatioLimit > 0) {
        const qint64 base = qMax<qint64>(1, t.st.total_wanted);
        if (double(t.st.all_time_upload) / double(base) >= m_cfg.seedRatioLimit)
            reached = true;
    }
    if (m_cfg.seedTimeLimitMin > 0
        && std::chrono::duration_cast<std::chrono::seconds>(t.st.seeding_duration).count() >= qint64(m_cfg.seedTimeLimitMin) * 60)
        reached = true;
    if (!reached)
        return;
    t.rec.seedLimitReached = true;
    persistRecord(t);
    if (m_cfg.seedLimitAction != QLatin1String("remove")) {
        qCInfo(lcBt) << "seed limit reached, pausing" << t.id;
        t.h.unset_flags(lt::torrent_flags::auto_managed);
        t.h.pause();
        t.resumeDirty = true;
    }
}

// ------------------------------------------------------------------------------------------------ snapshots
// [engine-4] where the swarm's peers come from, for the Transfers tree: DHT, each tracker, PEX, LSD
// and the web seeds of the torrent (TorrentSnapshot::channels)
void Worker::refreshChannels(Torrent &t, qint64 nowMs, const std::vector<lt::peer_info> &peers)
{
    t.channels.clear();
    if (!t.inSession)
        return;
    int fromDht = 0, fromTracker = 0, fromPex = 0, fromLsd = 0, webConnected = 0;
    for (const lt::peer_info &pi : peers) {
        if (pi.source & lt::peer_info::dht) ++fromDht;
        if (pi.source & lt::peer_info::tracker) ++fromTracker;
        if (pi.source & lt::peer_info::pex) ++fromPex;
        if (pi.source & lt::peer_info::lsd) ++fromLsd;
        if (pi.connection_type == lt::peer_info::web_seed || pi.connection_type == lt::peer_info::http_seed) ++webConnected;
    }
    auto row = [](const QString &name, const QString &conn, const QString &status, Net net = Net::BitTorrent) {
        TransferSource r;
        r.net = net;
        r.user.net = net;
        r.user.name = name;
        r.connection = conn;
        r.statusText = status;
        return r;
    };
    const bool paused = (t.st.flags & lt::torrent_flags::paused) != lt::torrent_flags_t{};
    if (m_cfg.dht) {
        QString st;
        if (!m_online || paused) st = QStringLiteral("Idle");
        else if (fromDht > 0) st = QStringLiteral("%1 peer%2 connected").arg(fromDht).arg(fromDht == 1 ? "" : "s");
        else if (m_dhtNodes > 0) st = QStringLiteral("Waiting for Network Reply... (%1)").arg(countNoun(m_dhtNodes, QStringLiteral("node")));
        else st = QStringLiteral("Connecting to the DHT...");
        t.channels << row(QStringLiteral("DHT"), QStringLiteral("DHT"), st);
    }
    // tracker states change slowly; asking libtorrent for them is a round trip to its thread
    if (t.trackerRows.isEmpty() || nowMs - t.lastTrackersMs >= 5000) {
        t.lastTrackersMs = nowMs;
        t.trackerRows.clear();
        t.diag.trackers.clear();   // [diag-1]
        int n = 0;
        for (const lt::announce_entry &ae : t.h.trackers()) {
            if (++n > 12) break;
            const QString url = qs(ae.url);
            QString host = QUrl(url).host();
            if (host.isEmpty()) host = url.left(40);
            bool updating = false, working = false, contacted = false;
            int seeds = -1, leechers = -1, fails = 0;
            QString err;
            for (const lt::announce_endpoint &ep : ae.endpoints) {
                const lt::announce_infohash &ih = ep.info_hashes[lt::protocol_version::V1];
                fails = qMax(fails, int(ih.fails));
                updating = updating || ih.updating;
                if (ih.start_sent) contacted = true;
                if (ih.start_sent && ih.fails == 0) working = true;
                if (ih.last_error && err.isEmpty()) err = qs(ih.message.empty() ? ih.last_error.message() : ih.message);
                seeds = qMax(seeds, ih.scrape_complete);
                leechers = qMax(leechers, ih.scrape_incomplete);
            }
            QString st;
            if (working) {
                st = (seeds >= 0 || leechers >= 0) ? QStringLiteral("Working (%1, %2)").arg(countNoun(qMax(0, seeds), QStringLiteral("seed")), countNoun(qMax(0, leechers), QStringLiteral("peer")))
                                                   : QStringLiteral("Working");
            } else if (updating) {
                st = QStringLiteral("Waiting for Network Reply...");
            } else if (!err.isEmpty()) {
                st = QStringLiteral("Unavailable (%1)").arg(err.left(60));
            } else {
                st = contacted ? QStringLiteral("Not working") : QStringLiteral("Not contacted yet");
            }
            t.trackerRows << row(host, QStringLiteral("Tracker"), st);
            // [diag-1]
            TrackerDiag td = t.trk.value(url);
            td.host = host;
            td.url = url;
            td.contacted = contacted || td.contacted;
            td.working = working;
            td.updating = updating;
            td.fails = fails;
            td.seeds = seeds;
            td.leechers = leechers;
            if (td.error.isEmpty()) td.error = err.left(80);
            if (working) {   // one endpoint answered (an IPv6 one failing is no problem)
                td.dnsFailed = false;
                td.error.clear();
            }
            t.diag.trackers << td;
        }
    }
    t.channels << t.trackerRows;
    if (fromTracker > 0 && !t.trackerRows.isEmpty())
        t.channels.last().statusText += QStringLiteral(", %1 peer%2 via trackers").arg(fromTracker).arg(fromTracker == 1 ? "" : "s");
    if (fromPex > 0)
        t.channels << row(QStringLiteral("Peer exchange"), QStringLiteral("PEX"), QStringLiteral("%1 peer%2 connected").arg(fromPex).arg(fromPex == 1 ? "" : "s"));
    if (fromLsd > 0)
        t.channels << row(QStringLiteral("Local peers"), QStringLiteral("LSD"), QStringLiteral("%1 peer%2 connected").arg(fromLsd).arg(fromLsd == 1 ? "" : "s"));
    if (t.ti && webConnected == 0 && m_cfg.webSeeds) {
        int n = 0;
        for (const lt::web_seed_entry &w : t.ti->web_seeds()) {
            if (++n > 6) break;
            const QString url = qs(w.url);
            const QString host = QUrl(url).host().isEmpty() ? url.left(40) : QUrl(url).host();
            t.channels << row(host, QStringLiteral("Web seed"), paused ? QStringLiteral("Idle") : QStringLiteral("Not connected"), Net::Http);
        }
    }
}

void Worker::refreshPeers(Torrent &t, qint64 nowMs, QList<TransferInfo> &upChanged, QSet<QString> &seenUp)
{
    t.sources.clear();
    std::vector<lt::peer_info> peers;
    if (t.inSession && t.st.num_peers > 0)
        t.h.get_peer_info(peers);
    refreshChannels(t, nowMs, peers);
    {   // [diag-1] what the connected peers do for us
        TorrentDiag &dg = t.diag;
        dg.connectedNow = dg.seedsConnected = dg.chokingUs = dg.interesting = dg.slowPeers = 0;
        dg.fromPex = dg.fromLsd = 0;
        t.encPeers = t.plainPeers = t.utpPeers = 0;
        for (const lt::peer_info &pi : peers) {
            if (pi.flags & (lt::peer_info::connecting | lt::peer_info::handshake)) continue;
            if (pi.connection_type == lt::peer_info::standard_bittorrent) {   // [enc-1] web seeds are HTTP
                if (pi.flags & (lt::peer_info::rc4_encrypted | lt::peer_info::plaintext_encrypted)) ++t.encPeers;
                else ++t.plainPeers;
                if (pi.flags & lt::peer_info::utp_socket) ++t.utpPeers;
            }
            ++dg.connectedNow;
            if (pi.flags & lt::peer_info::seed) ++dg.seedsConnected;
            if (pi.flags & lt::peer_info::interesting) {
                ++dg.interesting;
                if (pi.flags & lt::peer_info::remote_choked) ++dg.chokingUs;
                else if (pi.down_speed < 2048) ++dg.slowPeers;
            }
            if (pi.source & lt::peer_info::pex) ++dg.fromPex;
            if (pi.source & lt::peer_info::lsd) ++dg.fromLsd;
        }
    }
    if (peers.empty())
        return;
    const QByteArray btih = QByteArray::fromHex(t.id.toLatin1());
    for (const lt::peer_info &pi : peers) {
        const QString addr = endpointString(qs(pi.ip.address().to_string()), pi.ip.port());
        TransferSource src;
        src.net = Net::BitTorrent;
        src.user.net = Net::BitTorrent;
        src.user.id = addr;
        src.user.address = addr;
        src.client = qs(pi.client);
        src.user.name = src.client.isEmpty() ? addr : src.client;
        if (pi.connection_type == lt::peer_info::web_seed || pi.connection_type == lt::peer_info::http_seed) {
            src.net = Net::Http;   // [engine-4] a web seed is an HTTP server, listed as such
            src.user.net = Net::Http;
            src.user.name = QStringLiteral("Web seed %1").arg(addr);
        }
        src.bytes = pi.total_download;
        src.rateBps = pi.down_speed;
        QString conn = (pi.flags & lt::peer_info::utp_socket) ? QStringLiteral("uTP") : QStringLiteral("TCP");
        if (pi.flags & (lt::peer_info::rc4_encrypted | lt::peer_info::plaintext_encrypted))
            conn += QStringLiteral(" (encrypted)");
        src.connection = conn;
        // [core-5] the original WinMX wording of the Transfers window (UI_SPEC 8.7), not libtorrent states
        if (pi.flags & lt::peer_info::connecting)
            src.statusText = QStringLiteral("Connecting to Remote User");
        else if (pi.flags & lt::peer_info::handshake)
            src.statusText = QStringLiteral("Connected, Negotiating...");
        else if (pi.down_speed > 0)
            src.statusText = QStringLiteral("Transferring");
        else if ((pi.flags & lt::peer_info::interesting) && (pi.flags & lt::peer_info::remote_choked))
            src.statusText = QStringLiteral("Remotely Queued");
        else if (pi.flags & lt::peer_info::interesting)
            src.statusText = QStringLiteral("Waiting for Network Reply...");
        else if (pi.flags & lt::peer_info::seed)
            src.statusText = QStringLiteral("Connected (complete source)");
        else
            src.statusText = QStringLiteral("Connected, Negotiating...");
        t.sources.append(src);

        // uploads: one row per peer currently receiving data from us
        const QString uid = QStringLiteral("bt:%1:%2").arg(t.id, addr);
        auto it = m_up.find(uid);
        // [engine-4] payload only: protocol chatter (handshake, bitfield, keep-alives) is no upload row
        // [core-5] a reconnect of the same peer starts libtorrent's counter at 0 again
        const bool reset = it != m_up.end() && pi.total_upload < it->second.lastTotal;
        const bool active = pi.payload_up_speed > 0
                            || (it != m_up.end() && (reset ? pi.total_upload > 0 : pi.total_upload > it->second.lastTotal));
        if (it == m_up.end() && !active)
            continue;
        if (it == m_up.end()) {
            it = m_up.emplace(uid, UpRow{}).first;
            TransferInfo &ti = it->second.info;
            ti.id = uid;
            ti.isUpload = true;
            ti.added = QDateTime::currentDateTime();
            ti.networks = {Net::BitTorrent};
            ti.hashes.btih = btih;
        }
        UpRow &row = it->second;
        if (reset)
            row.base += row.lastTotal;
        row.peerSeed = bool(pi.flags & lt::peer_info::seed);
        row.peerProgressPpm = pi.progress_ppm;
        row.peerRateBps = pi.payload_up_speed;   // what we sent it: a lower bound of its download speed
        row.lastSeenMs = nowMs;
        seenUp.insert(uid);
        if (active)
            row.lastActiveMs = nowMs;
        TransferInfo &ti = row.info;
        ti.name = t.st.name.empty() ? t.rec.name : qs(t.st.name);
        ti.savePath = qs(t.st.save_path);
        ti.size = t.ti ? t.ti->total_size() : 0;
        ti.isBundle = t.ti && t.ti->num_files() > 1;
        const qint64 before = ti.done;
        const int beforeRate = ti.rateUpBps;
        ti.done = row.base + pi.total_upload;   // [core-5] every byte sent to this peer, not 0 after a reconnect
        ti.rateUpBps = pi.payload_up_speed;
        ti.state = TransferState::Transferring;
        ti.statusText = pi.payload_up_speed > 0 ? QStringLiteral("Uploading") : QStringLiteral("Uploading (waiting)");
        ti.peer = src.user;
        TransferSource us = src;
        us.bytes = ti.done;
        us.rateBps = pi.payload_up_speed;
        us.statusText = ti.statusText;
        ti.sources = {us};
        ti.sourcesTotal = ti.sourcesActive = 1;
        row.lastTotal = pi.total_upload;
        if (!row.announced || before != ti.done || beforeRate != ti.rateUpBps) {
            row.announced = true;
            upChanged.append(ti);
        }
    }
}

void Worker::sweepUploads(qint64 nowMs, const QSet<QString> &refreshed, const QSet<QString> &seenUp,
                          QList<TransferInfo> &upChanged)
{
    QStringList finished;
    for (auto it = m_up.begin(); it != m_up.end();) {
        const QString &uid = it->first;
        const QString tid = uid.section(QLatin1Char(':'), 1, 1);
        const bool gone = refreshed.contains(tid) && !seenUp.contains(uid);
        const bool idle = nowMs - it->second.lastActiveMs > 15000;
        if (gone || idle) {
            // [core-5] a sensible end state with the real byte count: the peer got (or has) the
            // whole file: "Complete"; it left or stopped asking halfway: "Disconnected" /
            // "Stopped (Idle)". An upload that never sent a byte leaves no row (UploadTracker).
            TransferInfo fin = it->second.info;
            // a peer that finishes drops the link at once (two seeds have nothing to say), often
            // before our next look: it counts as finished when the rest of the torrent fitted, at
            // the speed it was downloading, into the time since we last saw it
            const UpRow &ur = it->second;
            const qint64 remaining = fin.size > 0 ? fin.size * qint64(1000000 - qBound(0, ur.peerProgressPpm, 1000000)) / 1000000 : -1;
            const bool finishedAndLeft = gone && remaining >= 0 && ur.peerRateBps > 0
                                         && remaining <= qint64(ur.peerRateBps) * qMax<qint64>(1000, nowMs - ur.lastSeenMs) / 1000 * 2;
            fin.rateUpBps = 0;
            fin.etaSec = -1;
            if (!fin.sources.isEmpty())
                fin.sources.first().rateBps = 0;
            if ((fin.size > 0 && fin.done >= fin.size) || it->second.peerSeed || finishedAndLeft) {
                fin.state = TransferState::Completed;
                fin.statusText = transferStateText(TransferState::Completed);
            } else {
                fin.state = TransferState::Cancelled;
                fin.statusText = gone ? QStringLiteral("Disconnected") : QStringLiteral("Stopped (Idle)");
            }
            if (!fin.sources.isEmpty())
                fin.sources.first().statusText = fin.statusText;
            upChanged.append(fin);
            finished << uid;
            it = m_up.erase(it);
        } else {
            ++it;
        }
    }
    if (!upChanged.isEmpty() || !finished.isEmpty())
        post([sink = m_sink, upChanged, finished] { sink->wUploads(upChanged, finished); });
    upChanged.clear();
}

// [ui-i] the pieces libtorrent is receiving right now: in its download queue with at least one block
// requested from a peer (or web seed) or being written; a partial piece nobody sends is not
void Worker::refreshQueue(Torrent &t)
{
    const bool running = t.inSession && t.ti && t.st.has_metadata && m_online && !(t.st.flags & lt::torrent_flags::paused)
                         && !t.st.is_seeding && !isChecking(t.st.state);
    if (!running) {
        t.downloading.clear();
        return;
    }
    const int n = t.ti->num_pieces();
    QBitArray bits(n);
    std::vector<lt::partial_piece_info> q;
    t.h.get_download_queue(q);
    for (const lt::partial_piece_info &pp : q) {
        const int p = int(static_cast<int>(pp.piece_index));
        if (p >= 0 && p < n && (pp.requested > 0 || pp.writing > 0))
            bits.setBit(p);
    }
    t.downloading = bits;
}

TorrentSnapshot Worker::makeSnapshot(Torrent &t, bool withFiles)
{
    TorrentSnapshot s;
    const lt::torrent_status &st = t.st;
    s.id = t.id;
    s.added = t.rec.added;
    s.name = !st.name.empty() ? qs(st.name) : (t.ti ? qs(t.ti->name()) : t.rec.name);
    if (s.name.isEmpty())
        s.name = t.id;
    s.savePath = !st.save_path.empty() ? qs(st.save_path) : t.rec.savePath;
    s.hasMetadata = t.ti && t.ti->is_valid();
    if (s.hasMetadata) {
        const lt::torrent_info &ti = *t.ti;
        s.pieceLength = ti.piece_length();
        s.numPieces = ti.num_pieces();
        s.have.resize(s.numPieces);
        if (int(st.pieces.size()) == s.numPieces) {
            for (int i = 0; i < s.numPieces; ++i)
                if (st.pieces[lt::piece_index_t(i)])
                    s.have.setBit(i);
        }
        // [ui-i] what arrives now (nothing while paused or offline)
        if (t.downloading.size() == s.numPieces && m_online && !(st.flags & lt::torrent_flags::paused))
            s.downloading = t.downloading;
        if (withFiles) {
            std::vector<std::int64_t> fp;
            if (t.inSession)
                t.h.file_progress(fp, lt::torrent_handle::piece_granularity);
            if (t.filePrio.empty() && t.inSession)
                t.filePrio = t.h.get_file_priorities();
            const lt::file_storage &fs = ti.files();
            for (int i = 0; i < fs.num_files(); ++i) {
                const lt::file_index_t fi(i);
                TorrentFile f;
                f.path = qs(fs.file_path(fi));
                f.size = fs.file_size(fi);
                f.offset = fs.file_offset(fi);
                f.priority = fs.pad_file_at(fi) ? 0
                             : (std::size_t(i) < t.filePrio.size() ? int(static_cast<std::uint8_t>(t.filePrio[std::size_t(i)])) : 4);
                f.done = std::size_t(i) < fp.size() ? fp[std::size_t(i)] : 0;
                if (f.priority > 0) {
                    s.totalSize += f.size;
                    s.totalDone += f.done;
                }
                s.files.append(f);
            }
        }
    }
    const bool userPaused = (st.flags & lt::torrent_flags::paused) && !(st.flags & lt::torrent_flags::auto_managed);
    const bool queued = (st.flags & lt::torrent_flags::paused) && (st.flags & lt::torrent_flags::auto_managed);
    s.paused = userPaused;
    s.finished = reallyFinished(t);
    // a paused torrent gets no further status updates: do not keep showing its last rate
    const bool moving = m_online && !(st.flags & lt::torrent_flags::paused);
    s.rateDownBps = moving ? st.download_payload_rate : 0;
    s.rateUpBps = moving ? st.upload_payload_rate : 0;
    s.uploadedBytes = st.all_time_upload;
    s.seeds = st.num_seeds;
    s.peers = qMax(0, st.num_peers - st.num_seeds);
    s.seedsTotal = st.num_complete;
    s.peersTotal = st.num_incomplete;
    s.sources = t.sources;
    s.channels = t.channels;   // [engine-4]
    if (st.errc) {
        s.errorText = qs(st.errc.message());
        s.state = TransferState::Error;
        s.statusText = QStringLiteral("Error: %1").arg(s.errorText);
        return s;
    }
    const int pct = s.totalSize > 0 ? int(s.totalDone * 100 / s.totalSize) : int(st.progress_ppm / 10000);
    if (isChecking(st.state)) {
        s.state = TransferState::Verifying;
        s.statusText = QStringLiteral("Checking %1%").arg(int(st.progress_ppm / 10000));
    } else if (userPaused) {
        s.state = s.finished ? TransferState::Completed : TransferState::Paused;
        s.statusText = s.finished ? QStringLiteral("Completed") : QStringLiteral("Paused (%1%)").arg(pct);
    } else if (queued) {
        s.state = s.finished ? TransferState::Completed : TransferState::Queued;
        s.statusText = s.finished ? QStringLiteral("Completed (queued for seeding)") : QStringLiteral("Locally Queued");
    } else if (!m_online) {
        s.state = s.finished ? TransferState::Completed : TransferState::Queued;
        s.statusText = QStringLiteral("Offline");
    } else if (s.finished) {
        s.state = TransferState::Seeding;
        s.statusText = st.num_peers > 0 ? QStringLiteral("Seeding to %1").arg(countNoun(st.num_peers, QStringLiteral("peer"))) : QStringLiteral("Seeding");
    } else if (st.state == lt::torrent_status::downloading_metadata) {
        s.state = TransferState::FindingSources;
        s.statusText = QStringLiteral("Fetching metadata (%1)").arg(countNoun(st.num_peers, QStringLiteral("peer")));
    } else if (st.is_finished && !t.reserved.empty()) {
        s.state = TransferState::Transferring;
        s.statusText = QStringLiteral("%1%, waiting for %2 from other networks").arg(pct).arg(countNoun(t.reserved.size(), QStringLiteral("piece")));
    } else if (st.download_payload_rate > 0) {
        s.state = TransferState::Transferring;
        s.statusText = QStringLiteral("Downloading %1% from %2 (%3)").arg(pct).arg(countNoun(st.num_peers, QStringLiteral("peer")), countNoun(st.num_seeds, QStringLiteral("seed")));
    } else if (st.num_peers > 0) {
        s.state = TransferState::Connecting;
        s.statusText = QStringLiteral("%1%, connected to %2").arg(pct).arg(countNoun(st.num_peers, QStringLiteral("peer")));
    } else {
        s.state = TransferState::FindingSources;
        s.statusText = QStringLiteral("%1%, looking for peers").arg(pct);
    }
    return s;
}

void Worker::updateStatus()
{
    NetworkStatus ns;
    ns.net = Net::BitTorrent;
    int active = 0;
    bool anyPeer = false;
    for (const auto &kv : m_t) {
        if (kv.second.removing)
            continue;
        ++active;
        if (kv.second.haveStatus && kv.second.st.num_peers > 0)
            anyPeer = true;
    }
    if (!m_online) {
        ns.state = ConnState::Disconnected;
        ns.statusText = QStringLiteral("Offline, %1").arg(countNoun(active, QStringLiteral("torrent")));
    } else {
        const bool up = (!m_listen.isEmpty() && (!m_cfg.dht || m_dhtNodes > 0)) || anyPeer;
        ns.state = up ? ConnState::Connected : ConnState::Connecting;
        if (m_cfg.dht)
            ns.statusText = QStringLiteral("DHT: %1, %2").arg(countNoun(m_dhtNodes, QStringLiteral("node")), countNoun(active, QStringLiteral("torrent")));
        else
            ns.statusText = QStringLiteral("DHT off, %1").arg(countNoun(active, QStringLiteral("torrent")));
        if (m_listen.isEmpty())
            ns.statusText += QStringLiteral(", not listening");
    }
    ns.peers = m_dhtNodes;
    ns.firewalled = !m_incoming;
    ns.listenPort = m_listenPort;
    ns.externalAddress = m_externalIp;
    ns.rateDownBps = m_rateDown;
    ns.rateUpBps = m_rateUp;
    post([sink = m_sink, ns, n = m_dhtNodes] { sink->wStatus(ns, n); });
}


// ------------------------------------------------------------------------------------------------ [diag-1] connection test
namespace {
// "timed out" / "refused" / "handshake" / "banned" / "" (not a failure: a normal disconnect)
QString failureKind(const lt::error_code &e, lt::operation_t op)
{
    namespace le = lt::errors;
    if (e.category() == lt::libtorrent_category()) {
        switch (e.value()) {
        case le::timed_out_no_handshake:
        case le::no_incoming_encrypted:
        case le::no_rc4_mode:
        case le::invalid_info_hash:
        case le::sync_hash_not_found:
        case le::invalid_encryption_constant:
        case le::unsupported_encryption_mode:
        case le::unsupported_encryption_mode_selected:
            return QStringLiteral("handshake");
        case le::peer_banned:
        case le::self_connection:
        case le::banned_by_ip_filter:
            return QStringLiteral("banned");
        case le::timed_out:
            return op == lt::operation_t::connect ? QStringLiteral("timed out") : QString();
        default:
            break;
        }
    }
    if (op != lt::operation_t::connect) return QString();
    if (e == boost::asio::error::timed_out || e == boost::asio::error::host_unreachable || e == boost::asio::error::network_unreachable)
        return QStringLiteral("timed out");
    if (e == boost::asio::error::connection_refused || e == boost::asio::error::connection_reset)
        return QStringLiteral("refused");
    return QStringLiteral("other");
}
} // namespace

void Worker::diagAlert(lt::alert *a)
{
    const qint64 now = m_clock.elapsed();
    switch (a->type()) {
    case lt::peer_connect_alert::alert_type: {
        auto *pa = static_cast<lt::peer_connect_alert *>(a);
        Torrent *t = find(pa->handle);
        if (pa->direction == lt::peer_connect_alert::direction_t::out) {
            ++m_connectOk;
            if (t) ++t->diag.connectAttempts;
        } else if (t) {
            ++t->diag.fromIncoming;
        }
        break;
    }
    case lt::peer_disconnected_alert::alert_type:
    case lt::peer_error_alert::alert_type: {
        auto *pa = static_cast<lt::peer_alert *>(a);
        lt::error_code ec;
        lt::operation_t op;
        if (a->type() == lt::peer_disconnected_alert::alert_type) {
            auto *d = static_cast<lt::peer_disconnected_alert *>(a);
            ec = d->error;
            op = d->op;
        } else {
            auto *d = static_cast<lt::peer_error_alert *>(a);
            ec = d->error;
            op = d->op;
        }
        const QString kind = failureKind(ec, op);
        if (kind.isEmpty()) break;
        ++m_connectFailed;
        Torrent *t = find(pa->handle);
        if (!t) break;
        // a failed outgoing attempt is an attempt too (peer_connect_alert only reports successes)
        if (kind != QLatin1String("banned") || op == lt::operation_t::connect) ++t->diag.connectAttempts;
        if (kind == QLatin1String("timed out")) ++t->diag.failedTimeout;
        else if (kind == QLatin1String("refused")) ++t->diag.failedRefused;
        else if (kind == QLatin1String("handshake")) ++t->diag.failedHandshake;
        else if (kind == QLatin1String("banned")) ++t->diag.failedBanned;
        else ++t->diag.failedOther;
        break;
    }
    case lt::incoming_connection_alert::alert_type: {
        auto *ia = static_cast<lt::incoming_connection_alert *>(a);
        // TCP and uTP apart: only a TCP connection proves the TCP port is open
        if (ia->socket_type == lt::socket_type_t::utp || ia->socket_type == lt::socket_type_t::utp_ssl) m_incomingUtpMs.push_back(now);
        else m_incomingMs.push_back(now);
        break;
    }
    case lt::dht_reply_alert::alert_type: {
        auto *da = static_cast<lt::dht_reply_alert *>(a);
        if (Torrent *t = find(da->handle)) {
            ++t->diag.dhtLookups;
            t->diag.dhtPeers += da->num_peers;
            t->diag.fromDht += da->num_peers;
        }
        break;
    }
    case lt::tracker_reply_alert::alert_type: {
        auto *ta = static_cast<lt::tracker_reply_alert *>(a);
        if (Torrent *t = find(ta->handle)) {
            TrackerDiag &td = t->trk[qs(ta->tracker_url())];
            td.contacted = true;
            td.dnsFailed = false;
            td.error.clear();
            td.peersReturned = ta->num_peers;
            t->diag.fromTrackers += ta->num_peers;
        }
        break;
    }
    case lt::tracker_error_alert::alert_type: {
        auto *ta = static_cast<lt::tracker_error_alert *>(a);
        if (Torrent *t = find(ta->handle)) {
            TrackerDiag &td = t->trk[qs(ta->tracker_url())];
            td.contacted = true;
            const lt::error_code &e = ta->error;
            td.dnsFailed = e == boost::asio::error::host_not_found || e == boost::asio::error::host_not_found_try_again
                           || e == boost::asio::error::no_data || e == boost::asio::error::no_recovery
                           || qs(e.message()).contains(QLatin1String("resolve"), Qt::CaseInsensitive)
                           || qs(e.message()).contains(QLatin1String("host not found"), Qt::CaseInsensitive);
            const QString reason = qs(ta->failure_reason());
            td.error = (td.dnsFailed ? QStringLiteral("name does not resolve: ") : QString()) + (reason.isEmpty() ? qs(e.message()) : reason);
        }
        break;
    }
    case lt::portmap_alert::alert_type: {
        auto *pa = static_cast<lt::portmap_alert *>(a);
        PortMapping m;
        m.net = Net::BitTorrent;
        m.proto = pa->map_protocol == lt::portmap_protocol::udp ? QStringLiteral("UDP") : QStringLiteral("TCP");
        m.internalPort = m_listenPort;
        m.externalPort = pa->external_port;
        m.method = pa->map_transport == lt::portmap_transport::upnp ? QStringLiteral("UPnP") : QStringLiteral("NAT-PMP");
        m.result = PortMapping::Mapped;
        m.when = QDateTime::currentDateTime();
        m_ownMaps.insert(QStringLiteral("%1/%2/%3").arg(m.proto).arg(int(static_cast<std::uint8_t>(pa->mapping))).arg(m.method), m);
        break;
    }
    case lt::portmap_error_alert::alert_type: {
        auto *pa = static_cast<lt::portmap_error_alert *>(a);
        if (!m_online) break;   // removal while going offline: not a mapping failure
        PortMapping m;
        m.net = Net::BitTorrent;
        m.proto = QStringLiteral("TCP/UDP");
        m.internalPort = m_listenPort;
        m.method = pa->map_transport == lt::portmap_transport::upnp ? QStringLiteral("UPnP") : QStringLiteral("NAT-PMP");
        m.result = PortMapping::Failed;
        m.error = qs(pa->error.message());
        m.when = QDateTime::currentDateTime();
        m_ownMaps.insert(QStringLiteral("err/%1/%2").arg(int(static_cast<std::uint8_t>(pa->mapping))).arg(m.method), m);
        break;
    }
    default:
        break;
    }
}

void Worker::postDiag()
{
    const qint64 now = m_clock.elapsed();
    auto prune = [now](std::vector<qint64> &v) {
        v.erase(std::remove_if(v.begin(), v.end(), [now](qint64 t) { return now - t > 15 * 60 * 1000; }), v.end());
    };
    prune(m_incomingMs);
    prune(m_incomingUtpMs);
    QHash<QString, TorrentDiag> out;
    bool anyActive = false;
    int encPeers = 0, plainPeers = 0, utpPeers = 0;   // [enc-1]
    for (auto &kv : m_t) {
        Torrent &t = kv.second;
        if (t.removing || !t.haveStatus) continue;
        TorrentDiag d = t.diag;
        d.valid = true;
        d.hasMetadata = t.st.has_metadata;
        const bool paused = (t.st.flags & lt::torrent_flags::paused) != lt::torrent_flags_t{};
        const bool autoManaged = (t.st.flags & lt::torrent_flags::auto_managed) != lt::torrent_flags_t{};
        d.queued = paused && autoManaged && !t.st.is_finished && m_online;
        d.paused = paused && !d.queued;
        d.activeSec = std::chrono::duration_cast<std::chrono::seconds>(t.st.active_duration).count();
        d.knownPeers = t.st.list_peers;
        d.knownSeeds = t.st.list_seeds;
        d.downBps = t.st.download_payload_rate;
        d.dhtNodes = m_cfg.dht ? m_dhtNodes : -1;
        d.dhtEnabled = m_cfg.dht && m_online;
        if (t.st.num_peers == 0) d.connectedNow = d.seedsConnected = d.chokingUs = d.interesting = d.slowPeers = 0;
        if (t.st.num_peers > 0) { encPeers += t.encPeers; plainPeers += t.plainPeers; utpPeers += t.utpPeers; }
        if (!paused && !t.st.is_finished) anyActive = true;
        out.insert(t.id, d);
    }
    PluginDiagnostics s;
    s.valid = true;
    if (m_online) {
        const int port = m_listenPort > 0 ? m_listenPort : m_port;
        PortBinding tcp, udp;
        tcp.proto = QStringLiteral("TCP");
        udp.proto = QStringLiteral("UDP");
        tcp.port = udp.port = port;
        tcp.purpose = QStringLiteral("peers");
        udp.purpose = m_cfg.utp ? QStringLiteral("DHT and uTP peers") : QStringLiteral("DHT");
        tcp.ok = udp.ok = !m_listen.isEmpty();
        if (m_listen.isEmpty()) {
            tcp.error = udp.error = m_listenError.isEmpty() ? QStringLiteral("not listening yet") : m_listenError;
            if (m_listenErrorPort > 0) tcp.port = udp.port = m_listenErrorPort;
        }
        tcp.mapByCore = udp.mapByCore = m_cfg.coreMapsPorts;
        s.bindings << tcp;
        if (m_cfg.dht || m_cfg.utp) s.bindings << udp;
    }
    if (!m_cfg.coreMapsPorts) s.ownMappings = m_ownMaps.values();
    const int in15 = int(m_incomingMs.size());
    s.incomingConnections = in15 + int(m_incomingUtpMs.size());
    if (in15 > 0) {
        s.tcpIn = Reach::Yes;
        s.tcpEvidence = QStringLiteral("%1 in the last 15 min").arg(countNoun(in15, QStringLiteral("incoming TCP peer connection")));
    } else if (m_online && anyActive && m_clock.elapsed() > 15 * 60 * 1000 && m_connectOk > 20) {
        s.tcpIn = Reach::No;
        s.tcpEvidence = QStringLiteral("no incoming peer connection in 15 min while downloading");
    }
    if (!m_incomingUtpMs.empty()) {
        s.udpIn = Reach::Yes;
        s.udpEvidence = QStringLiteral("%1 in the last 15 min").arg(countNoun(qint64(m_incomingUtpMs.size()), QStringLiteral("incoming uTP connection")));
    } else if (m_cfg.dht && m_dhtNodes > 50 && m_dhtMsgIn > 0) {
        s.udpEvidence = QStringLiteral("DHT answers arrive (outgoing UDP works); no incoming uTP connection yet");
    }
    s.externalAddress = m_externalIp;
    s.externalSeenBy = QStringLiteral("BitTorrent peers and trackers");
    s.dhtNodes = m_cfg.dht && m_online ? m_dhtNodes : -1;
    s.dhtReplies = m_idxDhtMsgIn >= 0 ? int(qMin<qint64>(m_dhtMsgIn, INT_MAX)) : -1;
    s.connectAttempts = m_connectOk + m_connectFailed;
    s.connectOk = m_connectOk;
    if (m_cfg.announcePort > 0 && m_cfg.announcePort != m_listenPort)
        s.items << DiagItem{QStringLiteral("bitTorrent.announce"), QStringLiteral("Announced port"), DiagLevel::Ok,
                            QString::number(m_cfg.announcePort), QStringLiteral("the router's external port for %1").arg(m_listenPort), QString()};
    {   // [enc-1] protocol encryption (MSE / PE) of the connected peers
        DiagItem e;
        e.id = QStringLiteral("bitTorrent.encryption");
        e.check = QStringLiteral("Encryption");
        const int all = encPeers + plainPeers;
        const QString mode = m_cfg.encryption == QLatin1String("forced") ? QStringLiteral("required")
                             : m_cfg.encryption == QLatin1String("disabled") ? QStringLiteral("disabled") : QStringLiteral("preferred");
        if (m_cfg.encryption == QLatin1String("disabled")) {
            e.level = DiagLevel::Off;
            e.result = QStringLiteral("Off");
        } else {
            e.level = all > 0 ? DiagLevel::Ok : DiagLevel::Unknown;
            e.result = all > 0 ? QStringLiteral("%1 of %2 encrypted").arg(encPeers).arg(countNoun(all, QStringLiteral("peer"))) : QStringLiteral("No peers connected");
        }
        e.details = QStringLiteral("protocol encryption %1%2").arg(mode, all > 0 ? QStringLiteral(", %1 over uTP").arg(utpPeers) : QString());
        e.explain = QStringLiteral("BitTorrent protocol encryption (MSE) hides the content of peer connections from simple filtering "
                                   "by a provider. It does not hide your IP address from the peers and it is not anonymity. "
                                   "Trackers and DHT are not encrypted.");
        s.items << e;
    }
    post([sink = m_sink, out, s] { sink->wDiag(out, s); });
}

} // namespace bt
} // namespace mx
