// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/engine/download.h"

#include "core/dao.h"
#include "core/library.h"
#include "core/log.h"
#include "core/search_merge.h"
#include "mx/settings_keys.h"
#include "mx/trash.h"
#include "mxhash/ed2k.h"
#include "mxhash/encoding.h"
#include "mxhash/links.h"
#include "mxhash/tiger_tree.h"

#include <QtCore/QCborValue>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFileInfo>
#include <QtCore/QMap>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimer>
#include <QtCore/QUrl>

#include <algorithm>
#include <limits>

namespace mx::engine {

namespace {
constexpr qint64 kHugeLen = qint64(1) << 50;

QString hexId(const QByteArray &btih) { return QString::fromLatin1(btih.toHex()); }

// Drop hash fields of the wrong length (plugins are untrusted input).
FileHashes sanitized(const FileHashes &h)
{
    FileHashes o = h;
    auto chk = [](QByteArray &b, int n) { if (!b.isEmpty() && b.size() != n) b.clear(); };
    chk(o.btih, 20); chk(o.btFileRoot, 32); chk(o.ed2k, 16); chk(o.aich, 20); chk(o.sha1, 20);
    chk(o.tth, 24); chk(o.sha256, 32); chk(o.md5, 16);
    if (o.btFileIndex < -1 || o.btFileIndex > 1000000) o.btFileIndex = -1;
    return o;
}

int kindCount(const FileHashes &h)
{
    return int(!h.btih.isEmpty()) + int(!h.ed2k.isEmpty()) + int(!h.aich.isEmpty()) + int(!h.sha1.isEmpty())
           + int(!h.tth.isEmpty()) + int(!h.sha256.isEmpty()) + int(!h.md5.isEmpty());
}

// kinds present in `more` but not in `base`
FileHashes newKinds(const FileHashes &base, const FileHashes &more)
{
    FileHashes n;
    if (base.btih.isEmpty()) { n.btih = more.btih; n.btFileIndex = more.btFileIndex; }
    if (base.ed2k.isEmpty()) n.ed2k = more.ed2k;
    if (base.aich.isEmpty()) n.aich = more.aich;
    if (base.sha1.isEmpty()) n.sha1 = more.sha1;
    if (base.tth.isEmpty()) n.tth = more.tth;
    if (base.sha256.isEmpty()) n.sha256 = more.sha256;
    if (base.md5.isEmpty()) n.md5 = more.md5;
    return n;
}

QString safeFileName(QString n)
{
    static const QRegularExpression bad(QStringLiteral("[\\\\/:*?\"<>|\\x00-\\x1f]"));
    n.replace(bad, QStringLiteral("_"));
    n = n.trimmed();
    while (n.startsWith(QLatin1Char('.'))) n.remove(0, 1);
    if (n.isEmpty()) n = QStringLiteral("download");
    return n.left(200);
}

QString stateTextOf(RangeSource *rs)
{
    QString t = rs ? rs->stateText() : QString();
    return t.left(80);
}

// [engine-4] eD2k server / Kad keyword results name a file but no peer: the network knows sources
// and is asked for them (findSources); such an entry is nothing anyone can connect to.
bool isPlaceholder(const SearchSource &s)
{
    if (s.net != Net::ED2K && s.net != Net::Kad) return false;
    if (!s.user.address.isEmpty() || !s.user.id.isEmpty()) return false;
    return s.uri.isEmpty() || s.uri.startsWith(QLatin1String("ed2k://"), Qt::CaseInsensitive);
}

// [merge-1] "12.3 MB" for status lines
QString humanBytes(qint64 n)
{
    if (n < 1024) return QStringLiteral("%1 bytes").arg(n);
    if (n < 1024 * 1024) return QStringLiteral("%1 KB").arg(n / 1024);
    if (n < (qint64(1) << 30)) return QStringLiteral("%1 MB").arg(double(n) / (1 << 20), 0, 'f', 1);
    return QStringLiteral("%1 GB").arg(double(n) / (qint64(1) << 30), 0, 'f', 2);
}

// one peer reached through two networks of the same protocol family is one source
QString addressKey(const SearchSource &s)
{
    if (s.user.address.isEmpty()) return QString();
    const int fam = (s.net == Net::Kad) ? int(Net::ED2K) : int(s.net);
    return QString::number(fam) + QLatin1Char('|') + s.user.address;
}

} // namespace

// ============================================================================ construction

Download::Download(EngineHost *host, const DownloadRecord &rec, QObject *parent)
    : QObject(parent), m_host(host), m_rec(rec)
{
    m_rec.hashes = sanitized(m_rec.hashes);
    const TransferState st = TransferState(m_rec.state);
    m_userPaused = st == TransferState::Paused;
    m_errorState = st == TransferState::Error;
    if (!m_rec.hashes.btih.isEmpty()) m_torrentId = hexId(m_rec.hashes.btih);
    connect(this, &Download::needsSchedule, this, [this] {
        if (m_schedPending) return;
        m_schedPending = true;
        QTimer::singleShot(0, this, [this] {
            m_schedPending = false;
            schedule();
        });
    });
}

Download::~Download()
{
    for (auto &s : m_slots) {
        if (s->rs) {
            QObject::disconnect(s->rs, nullptr, this, nullptr);
            s->rs->close();
            s->rs->deleteLater();
        }
    }
}

bool Download::isFinished() const
{
    const TransferState s = TransferState(m_rec.state);
    return s == TransferState::Completed || s == TransferState::Seeding || s == TransferState::Cancelled;
}

// [ver-1] not while it is checked (Verify File) or while a restart waits for its old files to go
bool Download::wantsToRun() const
{
    return !isFinished() && !m_userPaused && !m_errorState && !m_verify.running && !m_restartPending
           && !m_checkFirst;   // [safe-1] checked first after an unclean stop
}

QString Download::partPath() const
{
    if (!m_partPath.isEmpty()) return m_partPath;
    return QDir(m_host->incompleteDir()).filePath(m_rec.id + QStringLiteral(".part"));
}

// ============================================================================ persistence

void Download::restoreState(const QByteArray &blob)
{
    if (blob.isEmpty()) return;
    const QVariantMap m = QCborValue::fromCbor(blob).toVariant().toMap();
    if (m.value(QStringLiteral("v")).toInt() != 1) return;
    const qint64 clamp = m_rec.size > 0 ? m_rec.size : -1;
    m_restoreHeld = RangeSet::fromVariant(m.value(QStringLiteral("held")).toList(), clamp);
    m_restoreVerified = RangeSet::fromVariant(m.value(QStringLiteral("verified")).toList(), clamp);
    const QString tid = m.value(QStringLiteral("torrentId")).toString();
    if (tid.size() == 40) m_torrentId = tid;
    m_pendingTorrentUrl = m.value(QStringLiteral("pendingUrl")).toString();
    m_partPath = m.value(QStringLiteral("partPath")).toString();
    m_fallbackTorrent = m.value(QStringLiteral("fallbackTorrent")).toString();
    m_deadEnd = m.value(QStringLiteral("deadEnd")).toBool();
    // [set-1] stored before 5.5: a dead end was a mismatch unless its text says the sources failed
    m_deadEndBanned = m_deadEnd && m.value(QStringLiteral("deadEndBanned"), !m_rec.errorText.startsWith(QLatin1String("File/User Unavailable"))).toBool();
    for (const QVariant &v : m.value(QStringLiteral("ed2kParts")).toList())
        if (v.toByteArray().size() == 16) m_ed2kParts << v.toByteArray();
    m_ed2kProvisional = !m_ed2kParts.isEmpty() && m.value(QStringLiteral("ed2kProvisional")).toBool();
    m_provisionalRoot = m_ed2kProvisional ? m.value(QStringLiteral("ed2kProvRoot")).toByteArray() : QByteArray();
    m_ed2kSetProvider = m.value(QStringLiteral("ed2kSetProvider")).toString();
    m_tthUnit = m.value(QStringLiteral("tthUnit")).toLongLong();
    for (const QVariant &v : m.value(QStringLiteral("tthLeaves")).toList())
        if (v.toByteArray().size() == 24) m_tthLeaves << v.toByteArray();
    if (m_tthLeaves.isEmpty()) m_tthUnit = 0;
    m_hinted = sanitized(FileHashes::fromVariant(m.value(QStringLiteral("hinted")).toMap()));
    for (const QVariant &v : m.value(QStringLiteral("rejected")).toList())
        m_rejected << FileHashes::fromVariant(v.toMap());
    const QVariantMap strikes = m.value(QStringLiteral("strikes")).toMap();
    for (auto it = strikes.constBegin(); it != strikes.constEnd(); ++it) m_strikes.insert(it.key(), it.value().toInt());
    for (const QVariant &v : m.value(QStringLiteral("banned")).toList()) m_banned.insert(v.toString());
    for (const QVariant &v : m.value(QStringLiteral("badSets")).toList()) m_badSetFrom.insert(v.toString());   // [up-1]
    for (const QVariant &v : m.value(QStringLiteral("removed")).toList()) {
        m_userRemoved.insert(v.toString());
        m_banned.insert(v.toString());
    }
    const QVariantMap prio = m.value(QStringLiteral("srcPrio")).toMap();
    for (auto it = prio.constBegin(); it != prio.constEnd(); ++it) m_srcPriority.insert(it.key(), qBound(-1, it.value().toInt(), 1));
    m_singleSource = m.value(QStringLiteral("singleSource")).toBool();
    m_wholeFileFailures = m.value(QStringLiteral("wholeFails")).toInt();
    m_trackers = m.value(QStringLiteral("trackers")).toStringList();
    const QVariantMap vbn = m.value(QStringLiteral("verifiedByNet")).toMap();
    for (auto it = vbn.constBegin(); it != vbn.constEnd(); ++it) m_verifiedByNet.insert(it.key().toInt(), it.value().toLongLong());
    if (m.value(QStringLiteral("errorPaused")).toBool()) m_userPaused = true;
    // [engine-4] per-download AFS / AEQ
    m_afs = m.contains(QStringLiteral("afs")) ? qBound(-1, m.value(QStringLiteral("afs")).toInt(), 1440) : -1;
    m_aeq = m.contains(QStringLiteral("aeq")) ? qBound(-1, m.value(QStringLiteral("aeq")).toInt(), 100000) : -1;
    // [dl-1] download order
    m_seq = m.value(QStringLiteral("seq")).toBool();
    m_firstLast = m.value(QStringLiteral("fl")).toBool();
    // [ui-j] ownership of the files on disk
    if (m.contains(QStringLiteral("own"))) {
        const QVariantMap own = m.value(QStringLiteral("own")).toMap();
        m_ownKnown = true;
        m_preexisting = own.value(QStringLiteral("pre")).toStringList();
        m_topExisted = own.value(QStringLiteral("topExisted")).toBool();
    }
    m_notOwned = m.value(QStringLiteral("notOwned")).toBool();
    // [ver-1] a finished file repaired in place, a damaged one, one without any checksum, a restart
    m_repairPath = m.value(QStringLiteral("repairPath")).toString();
    m_damaged = m.value(QStringLiteral("damaged")).toBool();
    m_noChecksum = m.value(QStringLiteral("noSum")).toBool();
    m_restartPending = m.value(QStringLiteral("restartPending")).toBool();
    m_repairWhole = !m_repairPath.isEmpty() && m.value(QStringLiteral("repairWhole")).toBool();
    m_checkFirst = m.value(QStringLiteral("checkFirst")).toBool();   // [safe-1]
    m_suspect = RangeSet::fromVariant(m.value(QStringLiteral("suspect")).toList(), clamp);
    // [merge-1] networks matched by name and size: confirmed / dropped
    const QVariantMap cn = m.value(QStringLiteral("candNets")).toMap();
    for (auto it = cn.constBegin(); it != cn.constEnd(); ++it) {
        const QVariantMap c = it.value().toMap();
        CandNet x;
        x.failed = c.value(QStringLiteral("failed")).toInt();
        x.confirmed = c.value(QStringLiteral("confirmed")).toBool();
        x.dropped = c.value(QStringLiteral("dropped")).toBool();
        x.fetched = c.value(QStringLiteral("fetched")).toLongLong();
        x.why = c.value(QStringLiteral("why")).toString();
        m_candNets.insert(it.key().toInt(), x);
    }
    m_confirmedClaims = sanitized(FileHashes::fromVariant(m.value(QStringLiteral("confirmedClaims")).toMap()));
    const QVariantMap cf = m.value(QStringLiteral("confirmedFrom")).toMap();
    for (auto it = cf.constBegin(); it != cf.constEnd(); ++it) m_confirmedFrom.insert(it.key(), it.value().toInt());
    m_completionNote = m.value(QStringLiteral("completionNote")).toString();
    m_stalledSwap = m.value(QStringLiteral("stalledSwap")).toBool();
    m_candidateHints = sanitized(FileHashes::fromVariant(m.value(QStringLiteral("candidateHints")).toMap()));
    for (const QVariant &v : m.value(QStringLiteral("btCandidates")).toList())
        if (v.toByteArray().size() == 20) m_btCandidates << v.toByteArray();
    // [eng-1]
    const QVariantMap sw = m.value(QStringLiteral("btSwarm")).toMap();
    for (auto it = sw.constBegin(); it != sw.constEnd(); ++it) m_btSwarm.insert(QByteArray::fromHex(it.key().toLatin1()), it.value().toInt());
    const QVariantMap bn = m.value(QStringLiteral("btNames")).toMap();
    for (auto it = bn.constBegin(); it != bn.constEnd(); ++it) m_btNames.insert(QByteArray::fromHex(it.key().toLatin1()), it.value().toString());
    const QString pt = m.value(QStringLiteral("promoteTorrent")).toString();
    if (pt.size() == 40) {
        m_restorePromote = pt;
        m_restorePromoteTrusted = m.value(QStringLiteral("promoteTrusted")).toBool();
    }
}

QByteArray Download::saveState() const
{
    QVariantMap m;
    m.insert(QStringLiteral("v"), 1);
    m.insert(QStringLiteral("store"), isTorrent() ? QStringLiteral("torrent") : QStringLiteral("part"));
    const RangeSet held = m_part ? m_part->persistable() : m_restoreHeld;
    const RangeSet ver = m_part ? m_part->persistableVerified() : m_restoreVerified;   // [safe-1] synced only
    m.insert(QStringLiteral("held"), held.toVariant());
    m.insert(QStringLiteral("verified"), ver.toVariant());
    if (!m_torrentId.isEmpty()) m.insert(QStringLiteral("torrentId"), m_torrentId);
    if (!m_pendingTorrentUrl.isEmpty()) m.insert(QStringLiteral("pendingUrl"), m_pendingTorrentUrl);
    if (!m_partPath.isEmpty()) m.insert(QStringLiteral("partPath"), m_partPath);
    if (!m_fallbackTorrent.isEmpty()) m.insert(QStringLiteral("fallbackTorrent"), m_fallbackTorrent);
    if (m_deadEnd) m.insert(QStringLiteral("deadEnd"), true);
    if (m_deadEnd) m.insert(QStringLiteral("deadEndBanned"), m_deadEndBanned);   // [set-1]
    QVariantList parts, leaves;
    for (const QByteArray &b : m_ed2kParts) parts << b;
    for (const QByteArray &b : m_tthLeaves) leaves << b;
    if (!parts.isEmpty()) m.insert(QStringLiteral("ed2kParts"), parts);
    if (!parts.isEmpty() && m_ed2kProvisional) {
        m.insert(QStringLiteral("ed2kProvisional"), true);
        m.insert(QStringLiteral("ed2kProvRoot"), m_provisionalRoot);
    }
    if (!parts.isEmpty() && !m_ed2kSetProvider.isEmpty()) m.insert(QStringLiteral("ed2kSetProvider"), m_ed2kSetProvider);
    if (!leaves.isEmpty()) {
        m.insert(QStringLiteral("tthUnit"), m_tthUnit);
        m.insert(QStringLiteral("tthLeaves"), leaves);
    }
    if (!m_hinted.isEmpty()) m.insert(QStringLiteral("hinted"), m_hinted.toVariant());
    QVariantList rej;
    for (const FileHashes &h : m_rejected) rej << h.toVariant();
    if (!rej.isEmpty()) m.insert(QStringLiteral("rejected"), rej);
    QVariantMap strikes;
    for (auto it = m_strikes.constBegin(); it != m_strikes.constEnd(); ++it) strikes.insert(it.key(), it.value());
    if (!strikes.isEmpty()) m.insert(QStringLiteral("strikes"), strikes);
    QVariantList banned;
    for (const QString &k : m_banned) banned << k;
    if (!banned.isEmpty()) m.insert(QStringLiteral("banned"), banned);
    QVariantList badSets;   // [up-1]
    for (const QString &k : m_badSetFrom) badSets << k;
    if (!badSets.isEmpty()) m.insert(QStringLiteral("badSets"), badSets);
    QVariantList removed;
    for (const QString &k : m_userRemoved) removed << k;
    if (!removed.isEmpty()) m.insert(QStringLiteral("removed"), removed);
    QVariantMap prio;
    for (auto it = m_srcPriority.constBegin(); it != m_srcPriority.constEnd(); ++it) prio.insert(it.key(), it.value());
    if (!prio.isEmpty()) m.insert(QStringLiteral("srcPrio"), prio);
    if (m_singleSource) m.insert(QStringLiteral("singleSource"), true);
    if (m_wholeFileFailures) m.insert(QStringLiteral("wholeFails"), m_wholeFileFailures);
    if (!m_trackers.isEmpty()) m.insert(QStringLiteral("trackers"), m_trackers);
    QVariantMap vbn;
    for (auto it = m_verifiedByNet.constBegin(); it != m_verifiedByNet.constEnd(); ++it) vbn.insert(QString::number(it.key()), it.value());
    if (!vbn.isEmpty()) m.insert(QStringLiteral("verifiedByNet"), vbn);
    if (m_userPaused && !m_rec.errorText.isEmpty()) m.insert(QStringLiteral("errorPaused"), true);
    if (m_afs >= 0) m.insert(QStringLiteral("afs"), m_afs);
    if (m_aeq >= 0) m.insert(QStringLiteral("aeq"), m_aeq);
    if (m_seq) m.insert(QStringLiteral("seq"), true);          // [dl-1]
    if (m_firstLast) m.insert(QStringLiteral("fl"), true);
    if (m_ownKnown) {   // [ui-j]
        QVariantMap own;
        own.insert(QStringLiteral("pre"), m_preexisting);
        own.insert(QStringLiteral("topExisted"), m_topExisted);
        m.insert(QStringLiteral("own"), own);
    }
    if (m_notOwned) m.insert(QStringLiteral("notOwned"), true);
    if (!m_repairPath.isEmpty()) m.insert(QStringLiteral("repairPath"), m_repairPath);   // [ver-1]
    if (m_damaged) m.insert(QStringLiteral("damaged"), true);
    if (m_noChecksum) m.insert(QStringLiteral("noSum"), true);
    if (m_restartPending) m.insert(QStringLiteral("restartPending"), true);
    if (m_repairWhole) m.insert(QStringLiteral("repairWhole"), true);
    if (m_checkFirst) m.insert(QStringLiteral("checkFirst"), true);   // [safe-1]
    if (!m_suspect.isEmpty()) m.insert(QStringLiteral("suspect"), m_suspect.toVariant());
    if (!m_candNets.isEmpty()) {   // [merge-1]
        QVariantMap cn;
        for (auto it = m_candNets.constBegin(); it != m_candNets.constEnd(); ++it) {
            QVariantMap c;
            c.insert(QStringLiteral("failed"), it->failed);
            c.insert(QStringLiteral("confirmed"), it->confirmed);
            c.insert(QStringLiteral("dropped"), it->dropped);
            c.insert(QStringLiteral("fetched"), it->fetched);
            if (!it->why.isEmpty()) c.insert(QStringLiteral("why"), it->why);
            cn.insert(QString::number(it.key()), c);
        }
        m.insert(QStringLiteral("candNets"), cn);
    }
    if (!m_confirmedClaims.isEmpty()) m.insert(QStringLiteral("confirmedClaims"), m_confirmedClaims.toVariant());
    if (!m_confirmedFrom.isEmpty()) {
        QVariantMap cf;
        for (auto it = m_confirmedFrom.constBegin(); it != m_confirmedFrom.constEnd(); ++it) cf.insert(it.key(), it.value());
        m.insert(QStringLiteral("confirmedFrom"), cf);
    }
    if (!m_completionNote.isEmpty()) m.insert(QStringLiteral("completionNote"), m_completionNote);
    if (m_stalledSwap) m.insert(QStringLiteral("stalledSwap"), true);
    if (!m_candidateHints.isEmpty()) m.insert(QStringLiteral("candidateHints"), m_candidateHints.toVariant());
    if (!m_btCandidates.isEmpty()) {
        QVariantList bl;
        for (const QByteArray &b : m_btCandidates) bl << b;
        m.insert(QStringLiteral("btCandidates"), bl);
    }
    // [eng-1] the swarm figures rank the candidates again after a restart; a metadata fetch that was
    // running is adopted again (the backend restores its torrent)
    if (!m_btSwarm.isEmpty()) {
        QVariantMap sw;
        for (auto it = m_btSwarm.constBegin(); it != m_btSwarm.constEnd(); ++it) sw.insert(QString::fromLatin1(it.key().toHex()), it.value());
        m.insert(QStringLiteral("btSwarm"), sw);
    }
    if (!m_btNames.isEmpty()) {
        QVariantMap bn;
        for (auto it = m_btNames.constBegin(); it != m_btNames.constEnd(); ++it) bn.insert(QString::fromLatin1(it.key().toHex()), it.value());
        m.insert(QStringLiteral("btNames"), bn);
    }
    const QString pt = !m_promoteTorrent.isEmpty() ? m_promoteTorrent : m_restorePromote;
    if (!pt.isEmpty() && m_fallbackTorrent.isEmpty() && !m_torrent) {
        m.insert(QStringLiteral("promoteTorrent"), pt);
        m.insert(QStringLiteral("promoteTrusted"), !m_promoteTorrent.isEmpty() ? m_promoteTrusted : m_restorePromoteTrusted);
    }
    return QCborValue::fromVariant(m).toCbor();
}

void Download::persist(bool full)
{
    DownloadsDao *dao = m_host->dao();
    m_lastPersistMs = nowMs();
    m_dirtyBlob = false;
    if (Store *s = store()) {
        if (s->kind() == Store::Part)
            m_rec.done = (m_part->scheme() == PartStore::NoUnits) ? m_part->heldBytes() : m_part->verifiedBytes();
        else if (m_torrent->hasMetadata())
            m_rec.done = m_torrent->snap().totalDone;
    }
    m_rec.engineState = saveState();
    if (!dao) return;
    if (full) {
        dao->upsert(m_rec);
    } else {
        dao->updateProgress(m_rec.id, m_rec.state, m_rec.done, m_rec.statusText);
        dao->setEngineState(m_rec.id, m_rec.engineState);
    }
}

void Download::markDirty(bool important)
{
    m_dirtyBlob = true;
    m_host->changed(this, important);
}

// ============================================================================ stores

Store *Download::store() const
{
    if (m_torrent) return m_torrent.get();
    return m_part.get();
}

void Download::ensureStore()
{
    if (store() || !m_pendingTorrentUrl.isEmpty()) return;
    if (!m_repairPath.isEmpty()) {   // [ver-1] a finished file repaired in place: its own file is the part file
        createPartStore();
        return;
    }
    if (!m_fallbackTorrent.isEmpty() && m_host->backend()) {
        // restored while running as a part file next to a torrent that had no metadata yet
        m_promoteTorrent = m_fallbackTorrent;
        m_promoteTrusted = !m_stalledSwap;   // [merge-1]
        m_promoteFile = -1;
        m_torrentId.clear();
        if (m_host->backend()->torrents().contains(m_promoteTorrent)) {
            m_host->backend()->setWantedFiles(m_promoteTorrent, {-1});
            m_host->backend()->resumeTorrent(m_promoteTorrent);
        } else {
            hash::ParsedLink l;
            l.kind = hash::ParsedLink::Magnet;
            l.hashes.btih = hash::fromHex(m_fallbackTorrent);
            l.trackers = m_trackers;
            QString err;
            m_host->backend()->addTorrent(hash::formatMagnet(l), QByteArray(), m_rec.saveDir.isEmpty() ? m_host->incomingDir() : m_rec.saveDir,
                                          false, {-1}, &err);
        }
        m_host->backend()->reannounce(m_promoteTorrent);   // [eng-1] tried again at once after a restart
        m_promoteSinceMs = m_promoteAnnounceMs = nowMs();
        m_promotePeersMax = 0;
        createPartStore();
        return;
    }
    TorrentBackend *be = m_host->backend();
    const QString uri = m_rec.uri.trimmed();
    const bool torrentUri = uri.startsWith(QLatin1String("magnet:"), Qt::CaseInsensitive)
                            || (!uri.contains(QLatin1String("://")) && uri.endsWith(QLatin1String(".torrent"), Qt::CaseInsensitive))
                            || uri.startsWith(QLatin1String("file:"), Qt::CaseInsensitive)
                            || ((uri.startsWith(QLatin1String("http"), Qt::CaseInsensitive)) && uri.endsWith(QLatin1String(".torrent"), Qt::CaseInsensitive));
    if (be && (!m_torrentId.isEmpty() || !m_rec.hashes.btih.isEmpty()
               || (torrentUri && !uri.startsWith(QLatin1String("magnet:"), Qt::CaseInsensitive)))) {
        QString err;
        if (startTorrent(&err)) return;
        if (!err.isEmpty()) {
            qCWarning(lcEngine) << m_rec.id << "cannot add torrent:" << err;
            if (m_rec.hashes.ed2k.isEmpty() && m_rec.hashes.sha1.isEmpty() && m_slots.empty()) {
                setError(err, false);
                return;
            }
        } else {
            return;   // pending .torrent URL fetch
        }
    }
    createPartStore();
    // [eng-1] restored while the metadata of a candidate torrent was being fetched: the backend
    // restored that torrent too; take it again (it was "already in the backend" before, so the
    // download never asked for it again) and announce it at once
    if (!m_restorePromote.isEmpty() && be && m_promoteTorrent.isEmpty()) {
        const QString pt = m_restorePromote;
        m_restorePromote.clear();
        bool rejected = false;
        for (const FileHashes &r : std::as_const(m_rejected)) if (r.btih == hash::fromHex(pt)) rejected = true;
        if (!rejected && be->torrents().contains(pt)) {
            m_promoteTorrent = pt;
            m_promoteTrusted = m_restorePromoteTrusted;
            m_promoteFile = -1;
            m_promoteSinceMs = nowMs();
            m_promoteAnnounceMs = m_promoteSinceMs;
            m_promotePeersMax = 0;
            be->setWantedFiles(pt, {-1});
            be->resumeTorrent(pt);
            be->reannounce(pt);
            qCInfo(lcEngine) << m_rec.id << "promotion: fetching metadata of" << pt.left(8) << "again (restored)";
        }
    }
}

void Download::createPartStore()
{
    const bool prealloc = m_host->settings() && m_host->settings()->b(keys::transfers::Preallocate);
    m_partPath = partPath();   // fixed from now on (a later incompleteDir change does not orphan it)
    m_part = std::make_unique<PartStore>(m_host->pool(), m_partPath, m_rec.size > 0 ? m_rec.size : -1, prealloc);
    m_part->restore(m_restoreHeld, m_restoreVerified);
    m_part->open();
    attachStore(m_part.get());
    applyHashSets();
    mapSourcesToFiles();
}

bool Download::startTorrent(QString *error)
{
    TorrentBackend *be = m_host->backend();
    if (!be) return false;
    QString id = m_torrentId;
    const QString saveDir = m_rec.saveDir.isEmpty() ? m_host->incomingDir() : m_rec.saveDir;
    if (id.isEmpty() || !be->torrents().contains(id)) {
        QString uri = m_rec.uri.trimmed();
        if (!m_pendingTorrentUrl.isEmpty()) uri = m_pendingTorrentUrl;
        // the index offered the .torrent itself: metadata without needing peers (Internet Archive
        // items are often seeded only by web seeds, which cannot serve metadata for a magnet)
        if (!m_torrentUrlHint.isEmpty() && !m_triedTorrentUrl && m_pendingTorrentUrl.isEmpty()
            && (uri.isEmpty() || uri.startsWith(QLatin1String("magnet:"), Qt::CaseInsensitive) || !uri.endsWith(QLatin1String(".torrent")))) {
            m_triedTorrentUrl = true;
            uri = m_torrentUrlHint;
        }
        if (!uri.startsWith(QLatin1String("magnet:"), Qt::CaseInsensitive) && !uri.endsWith(QLatin1String(".torrent"), Qt::CaseInsensitive)
            && !uri.startsWith(QLatin1String("file:"), Qt::CaseInsensitive)) {
            uri.clear();
        }
        if (uri.isEmpty() || (!m_rec.hashes.btih.isEmpty() && !uri.startsWith(QLatin1String("magnet:"))
                              && !QFileInfo::exists(uri) && !uri.startsWith(QLatin1String("http")))) {
            if (m_rec.hashes.btih.isEmpty()) return false;
            hash::ParsedLink l;
            l.kind = hash::ParsedLink::Magnet;
            l.hashes.btih = m_rec.hashes.btih;
            l.name = m_rec.name;
            if (m_rec.size > 0 && !m_rec.isBundle) l.size = m_rec.size;
            l.trackers = m_trackers;
            uri = hash::formatMagnet(l);
        }
        QString err;
        id = be->addTorrent(uri, QByteArray(), saveDir, true, m_rec.wantedFiles, &err);
        if (id.isEmpty()) {
            if (err.isEmpty() && uri.startsWith(QLatin1String("http"), Qt::CaseInsensitive)) {
                m_pendingTorrentUrl = uri;   // the backend fetches it → urlTorrentAdded
                qCInfo(lcEngine) << m_rec.id << "fetching .torrent" << uri;
                markDirty(true);
                return false;
            }
            if (error) *error = err.isEmpty() ? QStringLiteral("cannot add torrent") : err;
            return false;
        }
    }
    m_pendingTorrentUrl.clear();
    m_torrentId = id;
    const QByteArray raw = hash::fromHex(id);
    if (raw.size() == 20 && m_rec.hashes.btih.isEmpty()) m_rec.hashes.btih = raw;
    m_torrent = std::make_unique<TorrentStore>(be, id, m_host->pool(), &m_host->tuning());
    attachStore(m_torrent.get());
    applyTorrentOrder();   // [dl-1] before resuming (the backend does not persist the order flags)
    if (m_active) be->resumeTorrent(id);
    else be->pauseTorrent(id);
    if (m_rec.priority) be->setTorrentPriority(id, m_rec.priority);
    onTorrentChanged();
    if (m_torrent->hasMetadata()) onTorrentMetadata();
    persist(true);
    return true;
}

void Download::attachStore(Store *s)
{
    connect(s, &Store::unitVerified, this, &Download::onUnitVerified);
    connect(s, &Store::unitFailed, this, &Download::onUnitFailed);
    connect(s, &Store::unitAbandoned, this, &Download::onUnitAbandoned);
    connect(s, &Store::storeError, this, &Download::onStoreError);
    connect(s, &Store::changed, this, [this] { markDirty(); });
}

void Download::applyHashSets()
{
    if (!m_part) return;
    if (!m_ed2kParts.isEmpty()) {
        m_part->setEd2kParts(m_ed2kParts);
    } else if (!m_repairPath.isEmpty() && !m_tthLeaves.isEmpty() && m_tthUnit > 0 && m_tthUnit < PartStore::Ed2kPart) {
        // [ver-1] a file repaired in place is checked by its finer block hashes: a block the check did
        // not reach is judged alone (the disk's damage), never together with what sources sent again
        m_part->setTthLeaves(m_tthUnit, m_tthLeaves);
    } else if (!m_rec.hashes.ed2k.isEmpty() && m_rec.size > 0 && m_rec.size < PartStore::Ed2kPart) {
        m_part->setEd2kParts({m_rec.hashes.ed2k});   // single-part file: the file hash is the part hash
    } else if (!m_tthLeaves.isEmpty() && m_tthUnit > 0) {
        m_part->setTthLeaves(m_tthUnit, m_tthLeaves);
    }
}

// ============================================================================ control

void Download::activate()
{
    if (m_active || isFinished()) return;
    m_active = true;
    m_activatedMs = m_lastDataMs = nowMs();
    m_seenSourceSinceActivation = false;
    m_metaStalled = false;
    ensureStore();
    // [ver-1] damage only the whole-file checksum shows: the networks are asked for block hashes
    if (m_repairWhole && !m_awaitSetsUntil) m_awaitSetsUntil = m_activatedMs + m_host->tuning().verifyHashSetWaitMs;
    if (m_torrent) m_host->backend()->resumeTorrent(m_torrentId);
    mapSourcesToFiles();
    findSources(true);
    lookupHashLinks();
    openMoreSources();
    schedule();
    updateStatus();
    maybeComplete();
}

void Download::deactivate()
{
    if (!m_active) {
        updateStatus();
        return;
    }
    m_active = false;
    closeAllSources(m_userPaused ? QStringLiteral("paused") : QStringLiteral("dropped"));
    stopFindSources();
    if (m_torrent) {
        m_torrent->releaseAll();
        m_host->backend()->pauseTorrent(m_torrentId);
    }
    updateStatus();
    persist(true);
}

void Download::setUserPaused(bool paused)
{
    if (paused && m_verify.running) {   // [ver-1] Pause stops a check; what was not read yet keeps its status
        m_verify.stopped = true;
        if (!isFinished()) m_userPaused = true;
        stopVerify();
        if (isFinished()) return;
    }
    if (paused) m_awaitSetsUntil = 0;   // [ver-1] the wait for block hashes starts again on resume
    if (isFinished()) return;
    if (paused == m_userPaused && !(!paused && !m_rec.errorText.isEmpty())) return;
    m_userPaused = paused;
    if (!paused) {
        m_verifyNote.clear();   // [ver-1]
        m_rec.errorText.clear();
        m_errorState = false;
        resetBrokenStore();
    }
    if (paused) deactivate();
    updateStatus();
    persist(true);
}

void Download::resetBrokenStore()
{
    // a part store that hit an I/O error is rebuilt (the incomplete dir may have changed / have
    // space again); what was durable on disk is kept
    if (!m_part || !m_part->hasError()) return;
    m_restoreHeld = m_part->persistable();
    m_restoreVerified = m_part->persistableVerified();
    disconnect(m_part.get(), nullptr, this, nullptr);
    m_part->abandon();
    m_part.reset();
    if (m_restoreHeld.isEmpty()) {
        QFile::remove(partPath());
        m_partPath.clear();   // nothing kept: may move to the (new) incomplete dir
    }
}

void Download::retry()
{
    if (isFinished()) return;
    m_deadEnd = false;
    m_deadEndBanned = false;
    if (m_errorState || m_userPaused) {
        m_errorState = false;
        m_userPaused = false;
        m_rec.errorText.clear();
        resetBrokenStore();
        if (!store() && !m_pendingTorrentUrl.isEmpty()) m_pendingTorrentUrl.clear();
    }
    for (auto &s : m_slots) {
        s->retryAtMs = 0;
        if (!s->banned) s->failures = 0;
    }
    if (m_active) {
        findSources(true);
        lookupHashLinks();
        if (m_torrent) m_host->backend()->reannounce(m_torrentId);
        openMoreSources();
        schedule();
    }
    updateStatus();
    persist(true);
}

void Download::cancel(bool deleteData)
{
    m_active = false;
    if (m_verifyJob) m_verifyJob->stop();   // [ver-1]
    m_verify.running = false;
    dropPreview();   // [dl-1]
    closeAllSources(QStringLiteral("cancelled"));
    stopFindSources();
    if (m_torrent) {
        m_torrent->releaseAll();
        m_host->backend()->removeTorrent(m_torrentId, deleteData && !isFinished());
    }
    if (!m_promoteTorrent.isEmpty() && m_host->backend()) m_host->backend()->removeTorrent(m_promoteTorrent, false);
    if (m_part) {
        if (deleteData && !isFinished()) m_part->removeFile();
        else m_part->abandon();
    } else if (deleteData && !isFinished()) {
        QFile::remove(partPath());
    }
    dropPreview();   // [dl-1] again: on Windows the link goes only once the part file is closed
}

// ============================================================================ [ui-j] removal

namespace {
bool onDisk(const QString &p)
{
    const QFileInfo fi(p);
    return fi.exists() || fi.isSymLink();
}
bool padPath(const QString &p)
{
    return p.startsWith(QLatin1String(".pad/")) || p.contains(QLatin1String("/.pad/"));
}
// every file below `dir` is one of `own` (folders and nothing else)
bool holdsOnly(const QString &dir, const QSet<QString> &own)
{
    QDirIterator it(dir, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QFileInfo f = it.nextFileInfo();
        if (f.isDir() && !f.isSymLink()) continue;
        if (!own.contains(QDir::cleanPath(f.absoluteFilePath()))) return false;
    }
    return true;
}
} // namespace

void Download::noteOwnership(const TorrentSnapshot &sn)
{
    if (m_ownKnown || !sn.hasMetadata || sn.savePath.isEmpty()) return;
    m_ownKnown = true;
    const QDir dir(sn.savePath);
    QString top;
    for (int i = 0; i < sn.files.size(); ++i) {
        const TorrentFile &f = sn.files.at(i);
        if ((m_torrent && m_torrent->isPadFile(i)) || padPath(f.path)) continue;
        if (top.isEmpty() && f.path.contains(QLatin1Char('/'))) top = f.path.section(QLatin1Char('/'), 0, 0);
        if (onDisk(dir.filePath(f.path))) m_preexisting << f.path;
    }
    m_topExisted = !top.isEmpty() && onDisk(dir.filePath(top));
    if (!m_preexisting.isEmpty() || m_topExisted)
        qCInfo(lcEngine) << m_rec.id << "files on the disk before this download (kept on delete):" << m_preexisting.size()
                         << (m_topExisted ? "and its folder" : "");
    m_dirtyBlob = true;
}

DownloadDiskInfo Download::diskInfo(QStringList *pruneDirs) const
{
    DownloadDiskInfo di;
    di.known = true;
    di.finished = isFinished();
    di.total = m_rec.size;
    TorrentBackend *be = m_host->backend();
    const bool hasPart = m_part || !m_partPath.isEmpty() || (!isFinished() && onDisk(partPath()));
    const bool torrent = m_torrent || (!hasPart && !m_torrentId.isEmpty());
    QStringList own, kept;
    if (torrent) {
        di.torrent = true;
        TorrentSnapshot sn;
        if (m_torrent) sn = m_torrent->snap();
        else if (be && be->torrents().contains(m_torrentId)) sn = be->snapshot(m_torrentId);
        QList<TorrentFile> files;
        QString save = sn.savePath;
        if (sn.hasMetadata) {
            for (int i = 0; i < sn.files.size(); ++i)
                if (!(m_torrent && m_torrent->isPadFile(i)) && !padPath(sn.files.at(i).path)) files << sn.files.at(i);
        } else if (m_rec.isBundle && !m_rec.files.isEmpty()) {
            for (const BundleFile &b : m_rec.files) {
                TorrentFile f;
                f.path = b.path;
                f.size = b.size;
                f.done = b.done;
                files << f;
            }
        } else if (!m_rec.savePath.isEmpty()) {
            TorrentFile f;
            f.path = QFileInfo(m_rec.savePath).fileName();
            f.size = m_rec.size;
            f.done = isFinished() ? m_rec.size : m_rec.done;
            files << f;
            save = QFileInfo(m_rec.savePath).absolutePath();
        }
        if (save.isEmpty()) save = m_rec.saveDir.isEmpty() ? m_host->incomingDir() : m_rec.saveDir;
        const QDir dir(save);
        di.folder = dir.absolutePath();
        QString top;
        QSet<QString> ownSet;
        QSet<QString> dirs;
        for (const TorrentFile &f : std::as_const(files)) {
            const QString p = QDir::cleanPath(dir.absoluteFilePath(f.path));
            if (f.path.contains(QLatin1Char('/'))) {
                if (top.isEmpty()) top = f.path.section(QLatin1Char('/'), 0, 0);
                for (QString d = QFileInfo(f.path).path(); !d.isEmpty() && d != QLatin1String("."); d = QFileInfo(d).path())
                    dirs.insert(QDir::cleanPath(dir.absoluteFilePath(d)));
            }
            if (!onDisk(p)) continue;
            if (!m_ownKnown || m_preexisting.contains(f.path)) {
                kept << p;
                continue;
            }
            own << p;
            ownSet.insert(p);
            di.bytes += isFinished() ? QFileInfo(p).size() : qMax<qint64>(0, f.done);
        }
        const QString parts = dir.absoluteFilePath(QLatin1Char('.') + m_torrentId + QStringLiteral(".parts"));
        if (!m_torrentId.isEmpty() && onDisk(parts)) own << parts;
        if (!top.isEmpty() && m_ownKnown && !m_topExisted) {
            const QString topPath = QDir::cleanPath(dir.absoluteFilePath(top));
            if (kept.isEmpty() && QFileInfo(topPath).isDir() && !QFileInfo(topPath).isSymLink() && holdsOnly(topPath, ownSet)) {
                // the folder this download created, holding nothing but its files: it goes as one
                QStringList rest;
                for (const QString &p : std::as_const(own))
                    if (!p.startsWith(topPath + QLatin1Char('/'))) rest << p;
                own = QStringList{topPath} + rest;
            } else if (pruneDirs) {
                // something else is in it: the files go one by one, then the folders they leave empty
                QStringList ds = dirs.values();
                std::sort(ds.begin(), ds.end(), [](const QString &a, const QString &b) { return a.size() > b.size(); });
                *pruneDirs = ds;
            }
        }
        if (!kept.isEmpty())
            di.keptWhy = m_ownKnown ? QStringLiteral("it was on the disk before this download started")
                                    : QStringLiteral("OctoMX cannot tell whether this download created it");
    } else if (isFinished()) {
        const QString p = m_rec.savePath;
        if (!p.isEmpty() && onDisk(p)) {
            di.folder = QFileInfo(p).absolutePath();
            const bool adopted = m_notOwned || m_rec.statusText.contains(QLatin1String("already in your library"));
            if (adopted) {
                kept << p;
                di.keptWhy = QStringLiteral("it was in your library before this download");
            } else if (QFileInfo(p).isFile() && (m_rec.size <= 0 || QFileInfo(p).size() == m_rec.size)) {
                own << p;
                di.bytes = QFileInfo(p).size();
            } else {
                kept << p;
                di.keptWhy = QStringLiteral("it is not the file this download finished (its size changed)");
            }
        }
    } else {
        const QString p = m_part ? m_part->path() : partPath();
        if (onDisk(p)) {
            di.folder = QFileInfo(p).absolutePath();
            own << p;
            const qint64 held = m_part ? m_part->written().total() : m_restoreHeld.total();   // [safe-1] in the file now
            di.bytes = qMin(QFileInfo(p).size(), qMax(m_rec.done, held));
            if (QFileInfo(p).fileName() != m_rec.id + QStringLiteral(".part")) di.inPlaceFile = p;
        }
    }
    di.paths = own;
    di.kept = kept;
    di.trash = trash::availableForAll(own);
    return di;
}

void Download::stopForRemoval(bool keepData)
{
    m_active = false;
    if (m_verifyJob) m_verifyJob->stop();   // [ver-1]
    m_verify.running = false;
    dropPreview();
    closeAllSources(QStringLiteral("cancelled"));   // the uploaders learn that the user removed the download
    stopFindSources();
    TorrentBackend *be = m_host->backend();
    if (m_torrent) m_torrent->releaseAll();
    if (be && !m_torrentId.isEmpty() && be->torrents().contains(m_torrentId)) be->removeTorrent(m_torrentId, false);
    if (be && !m_promoteTorrent.isEmpty()) be->removeTorrent(m_promoteTorrent, false);
    if (m_part) {
        if (keepData) {
            m_part->syncNow(60000);   // [safe-1] what is held reaches the disk, synced, then the kept record
            m_restoreHeld = m_part->persistable();
            m_restoreVerified = m_part->persistableVerified();
        }
        m_part->close();
    }
    dropPreview();   // again: on Windows the link goes only once the part file is closed
}

void Download::setPriority(int p)
{
    m_rec.priority = qBound(-1, p, 1);
    if (m_torrent) m_host->backend()->setTorrentPriority(m_torrentId, m_rec.priority);
    persist(true);
    markDirty(true);
}

void Download::setWantedFiles(const QList<int> &idx)
{
    m_rec.wantedFiles = idx;
    if (m_torrent) m_host->backend()->setWantedFiles(m_torrentId, idx);
    persist(true);
    markDirty(true);
}

void Download::beginShutdown()
{
    m_quitting = true;
    // [ver-1] a check that runs now leaves everything as it was stored (what it found so far is
    // persisted already; a finished file's record stays finished)
    if (m_verifyJob) m_verifyJob->stop();
    for (auto &s : m_slots) {
        if (s->rs) {
            QObject::disconnect(s->rs, nullptr, this, nullptr);
            s->rs->setCloseReason(QStringLiteral("quit"), QString());   // [up-1]
            s->rs->close();
            s->rs->deleteLater();
            s->rs = nullptr;
            s->reqs.clear();
        }
    }
    if (m_torrent) m_torrent->releaseAll();
    stopFindSources();
    if (m_part) {
        // a final check or a copy to the target folder is not waited for: it runs again at the next
        // start (the copy is never seen half done, PartStore::moveTo)
        if (m_completing) m_part->abandon();
        m_part->flushToDisk();
    }
}

bool Download::shutdown(int waitMs)
{
    beginShutdown();
    const bool saved = !m_part || m_part->syncNow(qMax(0, waitMs));
    persist(true);
    return saved;
}

qint64 Download::unsavedBytes() const
{
    return m_part ? m_part->unsyncedBytes() : 0;   // held, not synced yet: queued writes included
}

void Download::setCheckFirst(bool on)
{
    if (m_checkFirst == on) return;
    m_checkFirst = on;
    m_checkFirstSinceMs = on ? nowMs() : 0;
    if (!on) m_checkFirstRun = false;
    if (on) m_suspect = m_part ? m_part->held() : m_restoreHeld;
    qCInfo(lcEngine) << m_rec.id << (on ? "to be checked before its data is trusted (the last session ended unexpectedly)"
                                        : "checked: its data is trusted again");
    markDirty(true);
}

static bool torrentData(bool hasTorrentStore, bool hasPart, const QString &partPath, const QString &torrentId);

bool Download::startCheckFirst()
{
    if (!m_checkFirst || m_verify.running || m_quitting) return false;
    if (isFinished()) {
        setCheckFirst(false);
        return false;
    }
    QString why;
    if (!canVerify(&why)) {
        // busy for a moment (finishing, a promotion, a move, BitTorrent checking its resume data, the
        // backend still restoring the torrent at start): later; else there is nothing to check
        const bool torrentish = torrentData(m_torrent != nullptr, m_part != nullptr, m_partPath, m_torrentId);
        const bool soon = m_completing || m_promoting || !m_movingTo.isEmpty() || m_restartPending
                          || (torrentish && m_rec.done > 0 && nowMs() - m_checkFirstSinceMs < 60000);
        if (soon) return false;
        qCInfo(lcEngine).noquote() << m_rec.id << "check after an unclean stop: nothing to check (" << why << ")";
        setCheckFirst(false);
        persist(true);
        m_host->finished(this);   // it may run now
        return false;
    }
    m_checkFirstRun = true;
    qCInfo(lcEngine) << m_rec.id << "check after an unclean stop: verifying" << (m_userPaused ? "(paused, stays paused)" : "");
    if (!startVerify()) {
        m_checkFirstRun = false;
        return false;
    }
    return m_verify.running || !m_checkFirst;
}

void Download::resolveInterruptedMove()
{
    const QString part = partPath();
    QString target, tmp;
    if (!PartStore::readMoveMarker(part, &target, &tmp)) return;
    if (!tmp.isEmpty()) QFile::remove(tmp);   // a copy that did not finish
    const bool partThere = QFileInfo::exists(part);
    const bool arrived = !isFinished() && !target.isEmpty() && QFileInfo(target).isFile() && (m_rec.size <= 0 || QFileInfo(target).size() == m_rec.size);
    if (arrived) {
        // the file reached the target folder (renamed, or copied, synced and renamed) before the stop;
        // only the record did not know yet
        if (partThere) QFile::remove(part);
        qCInfo(lcEngine) << m_rec.id << "the move to the target folder was interrupted after the file arrived: complete at" << target;
        completeAt(target, m_rec.hashes, {}, {}, false);
    } else if (!isFinished()) {
        qCInfo(lcEngine) << m_rec.id << "the move to the target folder was interrupted before the file arrived: it runs again";
    }
    QFile::remove(PartStore::moveMarkerPath(part));
}

void Download::mergeRequest(const DownloadRequest &req)
{
    learnHashes(req.hashes, true, QStringLiteral("joined request"));
    if (m_rec.size <= 0 && req.size > 0) {
        m_rec.size = req.size;
        if (m_part) m_part->setSize(req.size);
    }
    if (m_rec.uri.isEmpty()) m_rec.uri = req.uri;
    addSources(req.sources, nullptr, true);
    persist(true);
    markDirty(true);
}

// ============================================================================ identities

void Download::learnHashes(const FileHashes &moreIn, bool trusted, const QString &why)
{
    const FileHashes more = sanitized(moreIn);
    if (more.isEmpty()) return;
    for (const FileHashes &r : std::as_const(m_rejected))
        if (r.matches(more) && !m_rec.hashes.matches(more)) return;   // proven wrong before
    if (m_rec.hashes.conflicts(more)) {
        qCWarning(lcEngine) << m_rec.id << "rejecting conflicting hashes from" << why;
        return;
    }
    if (!trusted) {
        const FileHashes fresh = newKinds(m_rec.hashes, more);
        if (fresh.isEmpty() || m_hinted.conflicts(fresh)) return;
        const FileHashes before = m_hinted;
        m_hinted.mergeFrom(fresh);
        if (kindCount(before) != kindCount(m_hinted)) {
            qCInfo(lcEngine) << m_rec.id << "hint from" << why << ":" << m_hinted.toVariant().keys();
            markDirty();
            if (m_active) findSources(true);
            // [eng-1] after the current batch of sources: a better torrent of the same batch may still
            // take the hint's place (addBtCandidate)
            if (!fresh.btih.isEmpty()) QTimer::singleShot(0, this, [this] { if (m_promoteTorrent.isEmpty()) considerPromotion(); });
        }
        return;
    }
    const FileHashes fresh = newKinds(m_rec.hashes, more);
    if (fresh.isEmpty()) return;
    m_rec.hashes.mergeFrom(fresh);
    // a hint that became trusted
    FileHashes h2;
    if (!m_hinted.isEmpty()) {
        if (!m_rec.hashes.btih.isEmpty()) m_hinted.btih.clear();
        if (!m_rec.hashes.ed2k.isEmpty()) m_hinted.ed2k.clear();
        if (!m_rec.hashes.sha1.isEmpty()) m_hinted.sha1.clear();
        if (!m_rec.hashes.tth.isEmpty()) m_hinted.tth.clear();
        if (!m_rec.hashes.aich.isEmpty()) m_hinted.aich.clear();
        if (!m_rec.hashes.sha256.isEmpty()) m_hinted.sha256.clear();
        if (!m_rec.hashes.md5.isEmpty()) m_hinted.md5.clear();
    }
    qCInfo(lcEngine) << m_rec.id << "learned" << fresh.toVariant().keys() << "from" << why;
    if (m_ed2kProvisional && !fresh.ed2k.isEmpty()) {
        if (fresh.ed2k == m_provisionalRoot) {
            m_ed2kProvisional = false;
            qCInfo(lcEngine) << m_rec.id << "provisional ed2k hash set confirmed by the trusted ed2k hash";
        } else {
            dropProvisionalSet(QStringLiteral("its part hash set does not match the trusted ed2k hash"));
        }
    }
    // sources found through these kinds are now trusted
    for (auto &s : m_slots)
        if (!s->trusted && s->claimed.isEmpty() == false && m_rec.hashes.matches(s->claimed) && !m_rec.hashes.conflicts(s->claimed))
            s->trusted = true;
    applyHashSets();
    // [core-5] a tree kept aside for a provisional TTH root that is trusted now
    if (!fresh.tth.isEmpty() && !m_hintTthLeaves.isEmpty()) {
        const QList<QByteArray> leaves = m_hintTthLeaves;
        const qint64 unit = m_hintTthUnit;
        m_hintTthLeaves.clear();
        m_hintTthUnit = 0;
        if (hash::TigerTree::rootFromLevel(leaves) == fresh.tth) learnHashSet(QStringLiteral("tth-leaves"), unit, leaves);
    }
    persist(true);
    markDirty(true);
    if (m_active) findSources(true);
    if (!fresh.btih.isEmpty() && m_part) considerPromotion();
}

// [core-5] After the whole file verified against the trusted kinds: a provisional hint that
// contradicts the real hashes was a lie. Reject it, and ban every source that claimed a hash
// the file does not have (a Gnutella source naming a wrong TTH, ...); the download itself is fine.
void Download::checkClaimsAgainst(const FileHashes &verified)
{
    if (!m_hinted.isEmpty() && m_hinted.conflicts(verified)) {
        qCWarning(lcEngine) << m_rec.id << "provisional hashes proved wrong by the whole-file check:" << m_hinted.toVariant().keys();
        m_rejected << m_hinted;
        m_hinted = FileHashes();
        m_hintTthLeaves.clear();
        m_hintTthUnit = 0;
    }
    for (auto &sp : m_slots) {
        const FileHashes claim = sanitized(FileHashes::fromVariant(sp->desc.extra.value(QStringLiteral("hashes")).toMap()));
        if (!claim.isEmpty() && claim.conflicts(verified))
            ban(sp.get(), QStringLiteral("claimed hashes the file does not have"));
    }
}

void Download::learnHashSet(const QString &kind, qint64 unit, const QList<QByteArray> &hashes)
{
    if (hashes.isEmpty() || hashes.size() > 2000000) return;
    if (kind == QLatin1String("ed2k-parts")) {
        if ((!m_ed2kParts.isEmpty() && !m_ed2kProvisional) || m_rec.hashes.ed2k.isEmpty() || m_rec.size <= 0) return;
        for (const QByteArray &h : hashes) if (h.size() != 16) return;
        const int full = hash::Ed2kHasher::partCount(m_rec.size), data = hash::Ed2kHasher::dataPartCount(m_rec.size);
        if (hashes.size() != full && hashes.size() != data) {
            qCWarning(lcEngine) << m_rec.id << "ed2k hash set with" << hashes.size() << "entries, expected" << full;
            return;
        }
        if (hash::Ed2kHasher::fileHashFromParts(hashes) != m_rec.hashes.ed2k) {
            qCWarning(lcEngine) << m_rec.id << "ed2k hash set does not match the file hash, ignored";
            return;
        }
        if (!adoptEd2kSet(hashes.mid(0, data), QString(), true)) return;
    } else if (kind == QLatin1String("tth-leaves")) {
        if (m_rec.hashes.tth.isEmpty() && !m_hinted.tth.isEmpty() && m_tthLeaves.isEmpty() && hashes.size() <= 2000000
            && hash::TigerTree::rootFromLevel(hashes) == m_hinted.tth) {
            // [core-5] a tree for a provisional root (e.g. a Gnutella source's THEX): kept aside
            m_hintTthUnit = unit;
            m_hintTthLeaves = hashes;
            qCInfo(lcEngine) << m_rec.id << "TTH tree for the provisional root kept aside:" << hashes.size() << "x" << unit;
            return;
        }
        if (!m_tthLeaves.isEmpty() || m_rec.hashes.tth.isEmpty() || m_rec.size <= 0) return;
        if (unit < 1024 || (unit & (unit - 1)) != 0) return;
        for (const QByteArray &h : hashes) if (h.size() != 24) return;
        const qint64 expect = qMax<qint64>(1, (m_rec.size + unit - 1) / unit);
        if (hashes.size() != expect) return;
        if (hash::TigerTree::rootFromLevel(hashes) != m_rec.hashes.tth) {
            qCWarning(lcEngine) << m_rec.id << "TTH leaves do not match the root, ignored";
            return;
        }
        m_tthUnit = unit;
        m_tthLeaves = hashes;
        qCInfo(lcEngine) << m_rec.id << "trusted TTH level:" << hashes.size() << "x" << unit;
    } else {
        return;   // aich-leaves: not used for verification (the trusted ed2k / TTH sets and the whole-file check cover it)
    }
    applyHashSets();
    if (m_awaitHashSetUntil) m_awaitHashSetUntil = 0;
    if (m_repairWhole && m_part && m_part->scheme() != PartStore::NoUnits) {   // [ver-1] the damage can be located now
        m_awaitSetsUntil = 0;
        m_repairWhole = false;
        qCInfo(lcEngine) << m_rec.id << "block hashes arrived: the damaged parts are found and fetched again";
        setVerifyNote(QStringLiteral("block hashes arrived: only the damaged parts are downloaded again"), false);
        emit needsSchedule();
    }
    persist(false);
    markDirty();
}

// [engine-3] A set is trusted when it reduces to the trusted ed2k hash. Without a trusted ed2k
// hash it is used provisionally: parts are checked against it (bad data is caught and blamed early),
// sources whose claims are unconfirmed still get nothing, and the final whole-file check against
// the trusted kinds decides: a set that proves wrong bans its sender and is dropped.
bool Download::adoptEd2kSet(const QList<QByteArray> &data, const QString &providerKey, bool trusted)
{
    if (!m_ed2kParts.isEmpty() && m_ed2kParts != data) {
        if (!m_ed2kProvisional) return false;   // keep the trusted one
        dropProvisionalSet(QStringLiteral("a trusted part hash set contradicts it"));
    }
    const bool wasProvisional = m_ed2kProvisional;
    m_ed2kParts = data;
    m_ed2kProvisional = !trusted;
    m_provisionalFailures = 0;           // [up-1]
    m_provisionalCorroborated = false;
    m_provisionalRoot.clear();   // set by the caller for a provisional set
    if (!providerKey.isEmpty() || !trusted) m_ed2kSetProvider = providerKey;
    if (trusted)
        qCInfo(lcEngine).noquote() << m_rec.id << (wasProvisional ? "provisional ed2k hash set confirmed:" : "trusted ed2k hash set:") << data.size()
                                   << (providerKey.isEmpty() ? QStringLiteral("parts") : QStringLiteral("parts from ") + providerKey);
    else
        qCInfo(lcEngine) << m_rec.id << "provisional ed2k hash set:" << data.size() << "parts from" << providerKey
                         << "(reduces to no trusted hash; checked by the whole-file hashes at the end)";
    return true;
}

void Download::banKey(const QString &key, const QString &why)
{
    if (key.isEmpty()) return;
    if (SourceSlot *s = slotFor(key)) { ban(s, why); return; }
    if (m_banned.contains(key)) return;
    m_banned.insert(key);
    qCInfo(lcEngine) << m_rec.id << "banned source" << key << ":" << why;
    m_dirtyBlob = true;
}

// [up-1] A hash set that proved wrong is dropped, and the source that sent it is never asked to
// judge again, but it is NOT banned for the set: a set is only a claim about the file, the data a
// source sends is checked on its own (a 5.2 node that shared a file it was still downloading sent
// a set computed over the holes of its copy while its bytes were right). Bad data still costs
// strikes and bans as before.
void Download::rejectHashSetOf(const QString &key, const QString &why)
{
    if (key.isEmpty()) return;
    m_badSetFrom.insert(key);
    if (SourceSlot *s = slotFor(key)) s->ignoreHashSets = true;
    qCInfo(lcEngine).noquote() << m_rec.id << "hash sets of" << key << "are not used any more:" << why << "(the source itself is still used, its data is checked)";
    m_dirtyBlob = true;
}

void Download::dropProvisionalSet(const QString &why)
{
    if (!m_ed2kProvisional) return;
    qCWarning(lcEngine) << m_rec.id << "dropping the provisional ed2k hash set from" << m_ed2kSetProvider << ":" << why;
    rejectHashSetOf(m_ed2kSetProvider, why);
    m_provisionalFailures = 0;
    m_provisionalCorroborated = false;
    m_deferredBlackBox.clear();
    m_rejected << FileHashes();
    m_rejected.last().ed2k = m_provisionalRoot;
    m_ed2kParts.clear();
    m_ed2kProvisional = false;
    m_provisionalRoot.clear();
    m_ed2kSetProvider.clear();
    if (m_part) m_part->clearUnits();
    m_blackBox.clear();
    m_isolated.clear();
    m_unitOwner.clear();
    m_failedBy.clear();
    applyHashSets();
    markDirty(true);
}

void Download::onSourceHashSet(SourceSlot *s, const QString &kind, qint64 unit, const QList<QByteArray> &hashes)
{
    if (isFinished() || hashes.isEmpty() || hashes.size() > 2000000) return;
    if (kind == QLatin1String("tth-leaves")) {   // only against the trusted TTH root
        learnHashSet(kind, unit, hashes);
        return;
    }
    if (kind != QLatin1String("ed2k-parts") || m_rec.size < PartStore::Ed2kPart || m_rec.isBundle) return;
    // [up-1] a source whose set proved wrong once is not asked to judge again
    if (s->ignoreHashSets || m_badSetFrom.contains(s->key)) return;
    // [up-1] a torrent download is judged by its piece hashes: an eD2k part set adds nothing, and an
    // unconfirmed one could only do harm later (5.2 banned the sender when it was refuted)
    if (m_torrent && !m_part) {
        qCDebug(lcEngine) << m_rec.id << "ed2k part hash set from" << s->key << "not needed: BitTorrent piece hashes judge this download";
        return;
    }
    for (const QByteArray &h : hashes)
        if (h.size() != 16) { rejectHashSetOf(s->key, QStringLiteral("malformed part hash set")); return; }
    const int full = hash::Ed2kHasher::partCount(m_rec.size), dataParts = hash::Ed2kHasher::dataPartCount(m_rec.size);
    if (hashes.size() != full && hashes.size() != dataParts) { rejectHashSetOf(s->key, QStringLiteral("part hash set of the wrong length")); return; }
    const QByteArray root = hash::Ed2kHasher::fileHashFromParts(hashes);
    const QList<QByteArray> data = hashes.mid(0, dataParts);
    if (!m_rec.hashes.ed2k.isEmpty()) {
        if (root != m_rec.hashes.ed2k) {
            // [up-1] provably wrong and from this source: dropped, never asked again, no ban
            rejectHashSetOf(s->key, QStringLiteral("its part hash set does not match the file's ed2k hash"));
            return;
        }
        if (!m_ed2kParts.isEmpty() && !m_ed2kProvisional) return;   // already have it
        if (!adoptEd2kSet(data, s->key, true)) return;
    } else {
        for (const FileHashes &r : std::as_const(m_rejected))
            if (!r.ed2k.isEmpty() && r.ed2k == root) { rejectHashSetOf(s->key, QStringLiteral("it sent a part hash set proven wrong before")); return; }
        if (!m_ed2kParts.isEmpty()) {
            if (data != m_ed2kParts)
                qCWarning(lcEngine) << m_rec.id << s->key << "sent a part hash set that differs from the provisional one of" << m_ed2kSetProvider;
            return;
        }
        if (!adoptEd2kSet(data, s->key, false)) return;
        m_provisionalRoot = root;
    }
    applyHashSets();
    if (m_awaitHashSetUntil) m_awaitHashSetUntil = 0;
    if (m_repairWhole && m_part && m_part->scheme() != PartStore::NoUnits) {   // [ver-1]
        m_awaitSetsUntil = 0;
        m_repairWhole = false;
        qCInfo(lcEngine) << m_rec.id << "block hashes arrived from" << s->key << ": the damaged parts are found and fetched again";
        setVerifyNote(QStringLiteral("block hashes arrived: only the damaged parts are downloaded again"), false);
    }
    persist(false);
    markDirty();
    emit needsSchedule();
}

// [engine-3] Corruption black box (the idea of eMule's CCorruptionBlackBox): when a unit with
// several contributors fails, remember a SHA-1 of every byte range each of them delivered. When
// the unit later verifies, the same ranges are hashed again from the good data: a contributor
// whose ranges differ provably sent bad bytes (banned at once), one whose ranges match is innocent
// (its strike is forgiven).
void Download::recordBlackBox(qint64 s, qint64 e, const QList<SourceSlot *> &contributors)
{
    QList<BlackBoxEntry> entries;
    QList<Range> all;
    for (SourceSlot *c : contributors) {
        BlackBoxEntry be;
        be.key = c->key;
        const RangeSet part = c->contributed.intersected(s, e);
        for (const Range &r : part.ranges()) be.ranges << r;
        if (be.ranges.isEmpty() || be.ranges.size() > 4096) continue;
        all << be.ranges;
        entries << be;
    }
    if (entries.isEmpty()) return;
    QPointer<Download> self(this);
    m_part->sha1Ranges(all, [self, s, entries](const QList<QByteArray> &hashes) mutable {
        if (!self || hashes.size() < 1) return;
        int i = 0;
        for (BlackBoxEntry &be : entries)
            for (int k = 0; k < be.ranges.size(); ++k) be.sha1 << hashes.value(i++);
        self->m_blackBox[s] << entries;   // a repeated failure adds to what earlier attempts recorded
    });
}

void Download::checkBlackBox(qint64 s, const QSet<QString> &struck)
{
    const QList<BlackBoxEntry> entries = m_blackBox.take(s);
    QList<Range> all;
    for (const BlackBoxEntry &be : entries) all << be.ranges;
    if (all.isEmpty() || !m_part) return;
    QPointer<Download> self(this);
    m_part->sha1Ranges(all, [self, entries, struck, s](const QList<QByteArray> &good) {
        if (!self) return;
        int i = 0;
        QSet<QString> done;
        for (const BlackBoxEntry &be : entries) {
            qint64 badBytes = 0;
            for (int k = 0; k < be.ranges.size(); ++k, ++i)
                if (be.sha1.value(k).isEmpty() || good.value(i).isEmpty() || be.sha1.value(k) != good.value(i))
                    badBytes += be.ranges[k].second - be.ranges[k].first;
            SourceSlot *src = self->slotFor(be.key);
            if (done.contains(be.key) && badBytes == 0) continue;   // judged (and forgiven) already
            done.insert(be.key);
            if (badBytes > 0) {
                qCInfo(lcEngine) << self->m_rec.id << "black box: unit" << s << ":" << be.key << "had sent" << badBytes << "bad bytes";
                self->banKey(be.key, QStringLiteral("File Mismatch: proven to have sent corrupt data"));
            } else if (src && struck.contains(be.key) && src->strikes > 0 && !src->banned) {
                --src->strikes;   // its bytes were right: forgive
                self->m_strikes.insert(src->key, src->strikes);
                qCInfo(lcEngine) << self->m_rec.id << "black box: unit" << s << ":" << be.key << "was innocent";
            }
        }
        self->m_dirtyBlob = true;
    });
}

bool Download::matchesLink(const FileHashes &known, qint64 size) const
{
    if (!m_rec.hashes.matches(known)) return false;
    return size <= 0 || m_rec.size <= 0 || m_rec.isBundle || size == m_rec.size;
}

void Download::hashLinksFound(const FileHashes &all, qint64 size)
{
    if (size > 0 && m_rec.size > 0 && !m_rec.isBundle && size != m_rec.size) return;
    learnHashes(all, false, QStringLiteral("hash link"));
}

// ============================================================================ sources

SourceSlot *Download::slotFor(const QString &key) const { return m_byKey.value(key); }

void Download::addSources(const QList<SearchSource> &sources, NetworkPlugin *from, bool fromRequest)
{
    QList<SearchSource> toStore;
    bool hintsGrew = false;
    int n = 0;
    for (const SearchSource &src : sources) {
        if (++n > 1000) break;
        if (src.net == Net::Unknown) continue;
        const bool candidate = src.extra.value(QStringLiteral("candidate")).toBool();
        if (src.net == Net::BitTorrent) {
            const QString uri = src.uri.trimmed();
            const QString tu = src.extra.value(QStringLiteral("torrentUrl")).toString();
            if (!candidate) {   // [engine-4] what the index said about the swarm (shown while no peer answers)
                const QString pn = src.extra.value(QStringLiteral("providerName")).toString();
                if (src.seeds > m_indexSeeds || (m_indexName.isEmpty() && !pn.isEmpty())) {
                    m_indexSeeds = qMax(m_indexSeeds, src.seeds);
                    m_indexPeers = qMax(m_indexPeers, src.peers);
                    if (!pn.isEmpty()) m_indexName = pn;
                    else if (!src.connection.isEmpty() && m_indexName.isEmpty()) m_indexName = src.connection;
                }
            }
            // [engine-4] the .torrent of a torrent-cache site (itorrents & co.: Cloudflare, dead mirrors, a
            // 60 s timeout) only delays a magnet that has trackers and the DHT; it is preferred only where the
            // magnet alone is known not to work (Internet Archive items, seeded by web seeds) or no
            // info-hash is known at all
            if (m_torrentUrlHint.isEmpty() && !candidate && tu.startsWith(QLatin1String("http"), Qt::CaseInsensitive)) {
                const QString host = QUrl(tu).host().toLower();
                const bool magnetOk = uri.startsWith(QLatin1String("magnet:"), Qt::CaseInsensitive) && uri.contains(QLatin1String("btih:"), Qt::CaseInsensitive);
                if (!magnetOk || host == QLatin1String("archive.org") || host.endsWith(QLatin1String(".archive.org"))) m_torrentUrlHint = tu;
            }
            if (uri.startsWith(QLatin1String("magnet:"), Qt::CaseInsensitive)) {
                const hash::ParsedLink l = hash::parseMagnet(uri);
                if (l.isValid()) {
                    bool newTr = false;
                    for (const QString &t : l.trackers)
                        if (!m_trackers.contains(t) && m_trackers.size() < 50) { m_trackers << t; newTr = true; }
                    if (candidate && !l.hashes.btih.isEmpty() && tu.startsWith(QLatin1String("http"), Qt::CaseInsensitive))
                        m_btTorrentUrls.insert(l.hashes.btih, tu);   // [merge-1] metadata without peers
                    bool rejectedBt = false;
                    for (const FileHashes &r : std::as_const(m_rejected)) if (!r.btih.isEmpty() && r.btih == l.hashes.btih) rejectedBt = true;
                    if (candidate && m_candNets.value(int(Net::BitTorrent)).dropped) rejectedBt = true;
                    if (l.hashes.btih.size() == 20) {
                        // [eng-1] what the index said about this torrent's swarm ranks it among the candidates
                        const QByteArray &bh = l.hashes.btih;
                        if (src.seeds >= 0 || src.peers >= 0) m_btSwarm[bh] = qMax(m_btSwarm.value(bh, -1), qMax(0, src.seeds) + qMax(0, src.peers));
                        else if (!m_btSwarm.contains(bh)) m_btSwarm.insert(bh, -1);
                        QString nm = src.extra.value(QStringLiteral("insideBundle")).toString();
                        if (nm.isEmpty()) nm = l.name;
                        if (!nm.isEmpty() && !m_btNames.contains(bh)) m_btNames.insert(bh, nm.left(80));
                    }
                    if (!l.hashes.btih.isEmpty() && !rejectedBt && l.hashes.btih != m_rec.hashes.btih) {
                        // [merge-1] a further torrent that may hold the file: tried when the first one fails.
                        // [eng-1] also when the download's own identity is a torrent (it may be a dead one),
                        // and the one with the larger swarm is tried first
                        if (candidate && (!m_rec.hashes.btih.isEmpty() || (!m_hinted.btih.isEmpty() && m_hinted.btih != l.hashes.btih))) {
                            addBtCandidate(l.hashes.btih);
                        } else if (m_rec.hashes.btih.isEmpty()) {
                            learnHashes(l.hashes, !candidate, candidate ? QStringLiteral("candidate magnet") : QStringLiteral("BitTorrent source"));
                        }
                    }
                    if (newTr && m_torrent && l.hashes.btih == m_rec.hashes.btih) {
                        QString err;
                        m_host->backend()->addTorrent(uri, QByteArray(), QString(), true, {}, &err);   // merges trackers
                    }
                }
            } else if (uri.startsWith(QLatin1String("http"), Qt::CaseInsensitive) && m_rec.uri.isEmpty()) {
                m_rec.uri = uri;   // .torrent URL
            }
            continue;
        }
        if (isPlaceholder(src)) {
            // [engine-4] the network has the file but named no peer: ask it (findSources), count it
            const int reported = qMax(1, qMax(src.seeds, src.peers));
            m_reportedByNet[int(src.net)] = qMax(m_reportedByNet.value(int(src.net)), reported);
            if (candidate) {
                // [merge-1] a copy matched by name and size that names no peer: its hash is asked for
                // (findSources), what that finds is checked like the copy itself
                const FileHashes claim = sanitized(FileHashes::fromVariant(src.extra.value(QStringLiteral("hashes")).toMap()));
                bool rejected = false;
                for (const FileHashes &r : std::as_const(m_rejected)) if (!claim.isEmpty() && r.matches(claim)) rejected = true;
                const FileHashes fresh = newKinds(m_rec.hashes, claim);
                if (!rejected && !fresh.isEmpty() && !m_rec.hashes.conflicts(claim) && !m_hinted.conflicts(fresh)
                    && !m_candNets.value(netFamily(src.net)).dropped) {
                    const int before = kindCount(m_hinted);
                    m_hinted.mergeFrom(fresh);
                    m_candidateHints.mergeFrom(fresh);
                    if (kindCount(m_hinted) != before) hintsGrew = true;
                }
            }
            continue;
        }
        const QString key = sourceKey(src);
        if (m_banned.contains(key)) continue;
        SourceSlot *known = slotFor(key);
        if (!known) {
            // [engine-4] the same peer under another id (search hit "ip:port" vs. user hash)
            known = slotByAddress(src);
            if (known) {
                m_byKey.insert(key, known);
                if (src.uri.startsWith(QLatin1String("ed2k-peer://")) && !known->desc.uri.startsWith(QLatin1String("ed2k-peer://"))) {
                    known->desc.uri = src.uri;
                    if (!src.user.id.isEmpty()) known->desc.user.id = src.user.id;
                }
            }
        }
        if (SourceSlot *s = known) {
            for (auto it = src.extra.constBegin(); it != src.extra.constEnd(); ++it) s->desc.extra.insert(it.key(), it.value());
            if (s->failures > 0 && !s->rs) s->retryAtMs = qMin(s->retryAtMs, nowMs() + 5000);
            continue;
        }
        auto slot = std::make_unique<SourceSlot>();
        slot->key = key;
        slot->desc = src;
        slot->order = m_nextOrder++;
        slot->candidateFromName = src.extra.value(QStringLiteral("nameSearch")).toBool();
        if (candidate) {
            // [merge-1] a network dropped from this download (its copy failed the check) stays dropped
            // for copies with no hash of their own
            const CandNet cn = m_candNets.value(netFamily(src.net));
            if (cn.dropped && !cn.confirmed && FileHashes::fromVariant(src.extra.value(QStringLiteral("hashes")).toMap()).isEmpty()) continue;
            slot->trusted = false;
            slot->wasCandidate = true;
            slot->claimed = sanitized(FileHashes::fromVariant(src.extra.value(QStringLiteral("hashes")).toMap()));
            bool rejected = false;
            for (const FileHashes &r : std::as_const(m_rejected))
                if (!slot->claimed.isEmpty() && r.matches(slot->claimed)) rejected = true;
            if (rejected || m_rec.hashes.conflicts(slot->claimed)) continue;
            // its hashes become a hint, so the other networks are asked for sources by them too
            const FileHashes fresh = newKinds(m_rec.hashes, slot->claimed);
            if (!fresh.isEmpty() && !m_hinted.conflicts(fresh)) {
                const int before = kindCount(m_hinted);
                m_hinted.mergeFrom(fresh);
                if (kindCount(m_hinted) != before) hintsGrew = true;
            }
        } else if (!fromRequest && from && !netUsesHashOf(src.net, m_rec.hashes)) {
            slot->trusted = false;             // found only through an unconfirmed hint
            slot->claimed = m_hinted;
            // [merge-1] the hint came from a copy matched by name and size: this is that copy
            if (!m_candidateHints.isEmpty() && netUsesHashOf(src.net, m_candidateHints)) slot->wasCandidate = true;
        }
        slot->strikes = m_strikes.value(key);
        slot->ignoreHashSets = m_badSetFrom.contains(key);   // [up-1]
        slot->priority = m_srcPriority.value(key, 0);
        m_byKey.insert(key, slot.get());
        if (const QString ak = addressKey(src); !ak.isEmpty() && !m_byAddr.contains(ak)) m_byAddr.insert(ak, slot.get());
        if (!fromRequest) ++m_foundByNet[int(src.net)];
        m_slots.push_back(std::move(slot));
        toStore << src;
    }
    if (!toStore.isEmpty() && m_host->dao()) m_host->dao()->addSources(m_rec.id, toStore);
    if (hintsGrew && m_active) findSources(true);
    if (!toStore.isEmpty() && m_deadEnd && m_errorState) {
        qCInfo(lcEngine) << m_rec.id << "new sources after giving up, trying again";
        m_deadEnd = false;
        m_errorState = false;
        m_rec.errorText.clear();
        m_host->finished(this);   // → rebalance activates it
    }
    if (!toStore.isEmpty()) {
        m_seenSourceSinceActivation = true;
        mapSourcesToFiles();
        if (m_active) {
            if (!store()) ensureStore();
            openMoreSources();
            emit needsSchedule();
        }
        markDirty(true);
    }
}

void Download::mapSourcesToFiles()
{
    for (auto &s : m_slots) mapSlot(s.get());
}

bool Download::mapSlot(SourceSlot *s)
{
    if (!m_torrent) {
        s->fileIndex = 0;
        s->base = 0;
        s->span = m_rec.size > 0 ? m_rec.size : -1;
        return true;
    }
    if (!m_torrent->hasMetadata()) {
        s->fileIndex = -1;
        return false;
    }
    const QList<TorrentFile> &files = m_torrent->snap().files;
    int idx = -1;
    QString bp = s->desc.extra.value(QStringLiteral("bundlePath")).toString();
    bp.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (!bp.isEmpty()) {
        for (int i = 0; i < files.size(); ++i) {
            QString p = files.at(i).path;
            p.replace(QLatin1Char('\\'), QLatin1Char('/'));
            if (p == bp || p.endsWith(QLatin1Char('/') + bp)) { idx = i; break; }
        }
    } else {
        QList<int> real;
        for (int i = 0; i < files.size(); ++i) if (!m_torrent->isPadFile(i)) real << i;
        if (m_rec.hashes.btFileIndex >= 0 && m_rec.hashes.btFileIndex < files.size()) idx = m_rec.hashes.btFileIndex;
        else if (real.size() == 1) idx = real.first();
        else {
            qint64 want = s->desc.extra.value(QStringLiteral("size")).toLongLong();
            if (want <= 0 && !m_rec.isBundle) want = m_rec.size;
            int hits = 0;
            for (int i : real) if (want > 0 && files.at(i).size == want) { idx = i; ++hits; }
            if (hits != 1) idx = -1;
        }
    }
    if (idx >= 0 && files.at(idx).size <= 0) idx = -1;
    s->fileIndex = idx;
    if (idx < 0) return false;
    s->base = files.at(idx).offset;
    s->span = files.at(idx).size;
    return true;
}

bool Download::slotUsable(const SourceSlot *s) const
{
    if (s->banned || s->strikes >= m_host->tuning().strikesToBan) return false;
    if (!m_host->pluginFor(s->desc.net)) return false;
    if (m_torrent && s->fileIndex < 0) return false;
    if (m_torrent && m_torrent->snap().files.value(s->fileIndex).priority <= 0 && !m_promoting) return false;
    if (!s->trusted) {
        // nothing could confirm its data before the end, unless the engine decided to try the
        // candidates provisionally (no verifiable source; the whole-file check decides) [engine-4]
        if (m_part && m_part->scheme() == PartStore::NoUnits && !provisionalUsable(s)) return false;
        if (!m_part && !m_torrent) return false;
    }
    if (s->aeqParked && s->retryAtMs > nowMs()) return false;   // [engine-4] queue longer than AEQ
    return true;
}

bool Download::slotReady(const SourceSlot *s) const
{
    if (!s->rs || s->banned) return false;
    const RangeSource::State st = s->rs->state();
    return st == RangeSource::Ready || st == RangeSource::Busy;
}

int Download::openSources() const
{
    int n = 0;
    for (const auto &s : m_slots) if (s->rs) ++n;
    return n;
}

int Download::knownSources() const
{
    int n = 0;
    for (const auto &s : m_slots) if (!s->banned) ++n;
    return n;
}

void Download::openMoreSources()
{
    if (!m_active || isFinished() || m_completing || m_quitting || !store()) return;
    if (m_part && !m_part->acceptingData()) return;
    const Tuning &t = m_host->tuning();
    int limit = t.maxOpenSourcesPerDownload;
    if (m_host->settings()) limit = qMin(limit, qMax(1, m_host->settings()->i(keys::transfers::MaxSourcesPerDownload)));
    if (m_singleSource) limit = 1;
    int open = openSources();
    if (open >= limit) return;
    const qint64 now = nowMs();
    QList<SourceSlot *> cands;
    for (auto &s : m_slots)
        if (!s->rs && slotUsable(s.get()) && s->retryAtMs <= now) cands << s.get();
    std::stable_sort(cands.begin(), cands.end(), [](const SourceSlot *a, const SourceSlot *b) {
        if (a->priority != b->priority) return a->priority > b->priority;   // [glue-2] user priority
        if (a->trusted != b->trusted) return a->trusted;
        if (a->strikes != b->strikes) return a->strikes < b->strikes;
        return a->failures < b->failures;
    });
    for (SourceSlot *s : cands) {
        if (open >= limit || m_host->openSourcesGlobal() >= t.maxOpenSourcesGlobal) break;
        openSlot(s);
        if (s->rs) ++open;
    }
}

void Download::openSlot(SourceSlot *s)
{
    NetworkPlugin *p = m_host->pluginFor(s->desc.net);
    if (!p) return;
    FileHashes h = m_rec.hashes;
    qint64 size = m_rec.size > 0 ? m_rec.size : 0;
    if (m_torrent) {
        size = s->span;
        const bool downloadsFile = m_rec.hashes.btFileIndex == s->fileIndex
                                   || (!m_rec.isBundle && m_rec.hashes.btFileIndex < 0);
        if (!downloadsFile) {
            h = FileHashes();
            h.btih = m_rec.hashes.btih;
            h.btFileIndex = s->fileIndex;
        } else {
            h.btFileIndex = s->fileIndex;
        }
    }
    if (!s->trusted && !s->claimed.isEmpty()) {
        FileHashes c = s->claimed;
        c.mergeFrom(h);
        h = c;
    } else if (s->wasCandidate) {
        // [merge-1] a confirmed copy of another network is still found there by its own hash
        FileHashes c = sanitized(FileHashes::fromVariant(s->desc.extra.value(QStringLiteral("hashes")).toMap()));
        if (!c.isEmpty() && !c.conflicts(h)) {
            c.mergeFrom(h);
            h = c;
        }
    }
    // hints help the remote identify the file (our data is verified regardless)
    if (!m_hinted.conflicts(h)) h.mergeFrom(m_hinted);
    RangeSource *rs = p->openSource(m_rec.id, h, size, s->desc);
    const qint64 now = nowMs();
    if (!rs) {
        ++s->failures;
        s->lastError = QStringLiteral("File/User Unavailable (%1 cannot reach it)").arg(netName(s->desc.net));
        s->retryAtMs = now + qMin<qint64>(30LL * 60 * 1000, qint64(m_host->tuning().sourceRetryBaseMs) << qMin(s->failures, 6));
        return;
    }
    if (!rs->parent()) rs->setParent(this);
    s->rs = rs;
    s->plugin = p;
    s->openedMs = now;
    s->reqs.clear();
    connectSource(s);
    rs->open();
}

void Download::connectSource(SourceSlot *s)
{
    RangeSource *rs = s->rs;
    connect(rs, &RangeSource::stateChanged, this, [this, s, rs] { if (s->rs == rs) onSourceState(s); });
    connect(rs, &RangeSource::availabilityChanged, this, [this, s, rs] { if (s->rs == rs) emit needsSchedule(); });
    connect(rs, &RangeSource::data, this, [this, s, rs](qint64 off, const QByteArray &b) { if (s->rs == rs) onSourceData(s, off, b); });
    connect(rs, &RangeSource::requestFinished, this, [this, s, rs](qint64 off, qint64 len, bool ok, const QString &err) {
        if (s->rs == rs) onSourceFinished(s, off, len, ok, err);
    });
    connect(rs, &QObject::destroyed, this, [this, s, rs] {
        if (s->rs == rs || s->rs.isNull()) { s->reqs.clear(); emit needsSchedule(); }
    });
    connect(rs, &RangeSource::hashSetReceived, this, [this, s, rs](const QString &kind, qint64 unit, const QList<QByteArray> &h) {
        if (s->rs == rs) onSourceHashSet(s, kind, unit, h);
    });
}

void Download::closeSlot(SourceSlot *s, bool failed, const QString &why)
{
    if (s->rs) {
        RangeSource *rs = s->rs;
        QObject::disconnect(rs, nullptr, this, nullptr);
        s->rs = nullptr;
        rs->setCloseReason(s->banned ? QStringLiteral("banned") : QStringLiteral("dropped"), why);   // [up-1] told to the uploader
        rs->close();
        rs->deleteLater();
    }
    s->reqs.clear();
    for (auto it = m_unitOwner.begin(); it != m_unitOwner.end();) it = (it.value() == s) ? m_unitOwner.erase(it) : std::next(it);
    if (failed) {
        ++s->failures;
        s->lastError = why.trimmed().isEmpty() ? QStringLiteral("File/User Unavailable") : why.left(80);
        s->retryAtMs = nowMs() + qMin<qint64>(30LL * 60 * 1000, qint64(m_host->tuning().sourceRetryBaseMs) << qMin(s->failures, 6));
        if (m_host->dao()) m_host->dao()->markSourceFailed(m_rec.id, s->desc.net, s->desc.uri, s->desc.user.id);
        qCDebug(lcEngine) << m_rec.id << "source" << s->key << "closed:" << why;
    }
    emit needsSchedule();
    QTimer::singleShot(0, this, [this] { openMoreSources(); });
}

void Download::closeAllSources(const QString &code)
{
    for (auto &s : m_slots) {
        if (!s->rs) continue;
        RangeSource *rs = s->rs;
        QObject::disconnect(rs, nullptr, this, nullptr);
        s->rs = nullptr;
        rs->setCloseReason(code, QString());   // [up-1] the uploader learns why we stopped
        rs->close();
        rs->deleteLater();
        s->reqs.clear();
    }
}

void Download::onSourceState(SourceSlot *s)
{
    RangeSource *rs = s->rs;
    if (!rs) return;
    const RangeSource::State st = rs->state();
    if (st == RangeSource::Failed || st == RangeSource::Closed) {
        closeSlot(s, true, stateTextOf(rs));
        markDirty();
        return;
    }
    if (st == RangeSource::Queued) {   // [engine-4] Auto Enter Queue
        enforceQueueLimit(s);
        if (!s->rs) { markDirty(); return; }
    } else {
        s->queueRank = 0;
    }
    if (st == RangeSource::Ready || st == RangeSource::Busy) {
        m_seenSourceSinceActivation = true;
        const SearchSource d = rs->descriptor();
        if (d.extra != s->desc.extra && !d.extra.isEmpty()) {
            for (auto it = d.extra.constBegin(); it != d.extra.constEnd(); ++it) s->desc.extra.insert(it.key(), it.value());
            if (m_host->dao()) m_host->dao()->addSources(m_rec.id, {s->desc});   // etag & co survive a restart
        }
        // unknown size / name from an HTTP-like source (http plugin contract)
        if (m_part && !m_torrent) {
            if (d.extra.value(QStringLiteral("isTorrentFile")).toBool()) {
                QString url = d.extra.value(QStringLiteral("finalUrl")).toString();
                if (url.isEmpty()) url = d.uri;
                qCInfo(lcEngine) << m_rec.id << "source is a .torrent file, handing" << url << "to BitTorrent";
                closeSlot(s, false, QStringLiteral("torrent file"));
                s->banned = true;
                if (m_host->backend() && m_part->heldBytes() == 0) {
                    m_part->removeFile();
                    m_part.reset();
                    m_rec.uri = url;
                    m_pendingTorrentUrl = url;
                    QString err;
                    if (!startTorrent(&err) && !err.isEmpty()) setError(err, false);
                    persist(true);
                }
                return;
            }
            const QList<QPair<qint64, qint64>> av = rs->available();
            qint64 total = -1;
            if (av.size() == 1 && av.first().first == 0 && av.first().second > 0) total = av.first().second;
            const qint64 xs = d.extra.value(QStringLiteral("size")).toLongLong();
            if (total < 0 && xs > 0) total = xs;
            if (m_rec.size <= 0 && total > 0) {
                m_rec.size = total;
                m_part->setSize(total);
                s->span = total;
                applyHashSets();
                persist(true);
            } else if (m_rec.size > 0 && total > 0 && total != m_rec.size && s->desc.net == Net::Http) {
                qCWarning(lcEngine) << m_rec.id << "source" << s->key << "has size" << total << "expected" << m_rec.size;
                ban(s, QStringLiteral("File Mismatch (size)"));
                return;
            }
            const QString fn = d.extra.value(QStringLiteral("fileName")).toString();
            if (!fn.isEmpty() && m_rec.done == 0 && m_part->heldBytes() == 0 && s->desc.net == Net::Http) {
                const QString urlName = QFileInfo(QUrl(s->desc.uri).path()).fileName();
                if (m_rec.name.isEmpty() || m_rec.name == urlName || m_rec.name == QUrl(s->desc.uri).host()) {
                    if (fn != m_rec.name) {
                        m_rec.name = safeFileName(fn);
                        persist(true);
                    }
                }
            }
        }
        if (s->failures > 0 && s->bytes > 0) s->failures = 0;
        emit needsSchedule();
    }
    markDirty();
}

void Download::cancelSlotRequests(SourceSlot *s, const QString &why)
{
    if (s->reqs.isEmpty()) return;
    s->reqs.clear();
    s->cancelGraceUntilMs = nowMs() + 15000;
    if (s->rs) s->rs->cancelRequests();
    qCDebug(lcEngine) << m_rec.id << "cancelled requests of" << s->key << ":" << why;
    emit needsSchedule();
}

void Download::strike(SourceSlot *s, const QString &why)
{
    ++s->strikes;
    m_strikes.insert(s->key, s->strikes);
    qCInfo(lcEngine) << m_rec.id << "strike" << s->strikes << "for" << s->key << ":" << why;
    if (s->strikes >= m_host->tuning().strikesToBan) ban(s, why);
    m_dirtyBlob = true;
}

void Download::ban(SourceSlot *s, const QString &why)
{
    if (s->banned) return;
    s->banned = true;
    s->lastError = why.left(80);
    m_banned.insert(s->key);
    qCInfo(lcEngine) << m_rec.id << "banned source" << s->key << ":" << why;
    closeSlot(s, false, why);
    m_dirtyBlob = true;
}

void Download::onSourceData(SourceSlot *s, qint64 fileOff, const QByteArray &bytes)
{
    Store *st = store();
    if (!st || bytes.isEmpty()) return;
    const qint64 now = nowMs();
    if (fileOff < 0 || fileOff > (qint64(1) << 52)) {
        strike(s, QStringLiteral("data at an invalid offset"));
        cancelSlotRequests(s, QStringLiteral("bad data"));
        return;
    }
    const qint64 off = s->base + fileOff;
    Request *r = nullptr;
    for (Request &q : s->reqs)
        if (q.off + q.got == off && q.got < q.len) { r = &q; break; }
    if (!r) {
        if (now < s->cancelGraceUntilMs) return;     // late data after a cancel
        strike(s, QStringLiteral("unrequested / out-of-order data"));
        cancelSlotRequests(s, QStringLiteral("protocol violation"));
        return;
    }
    QByteArray b = bytes;
    const bool overlong = r->got + b.size() > r->len;
    if (overlong) b.truncate(int(r->len - r->got));
    r->got += b.size();
    r->lastDataMs = now;
    r = nullptr;   // strike() below may close the slot and clear s->reqs
    s->rate.add(now, b.size());
    m_lastDataMs = now;
    if (s->trusted) m_lastTrustedDataMs = now;   // [engine-4]
    if (s->span >= 0 && fileOff + b.size() > s->span) b.truncate(int(qMax<qint64>(0, s->span - fileOff)));
    const RangeSet acc = st->deliver(off, b);
    if (!acc.isEmpty()) {
        const qint64 n = acc.total();
        s->bytes += n;
        s->contributed.add(acc);
        if (s->wasCandidate && !s->trusted) m_candNets[netFamily(s->desc.net)].fetched += n;   // [merge-1]
    }
    m_dirtyBlob = true;
    if (overlong) strike(s, QStringLiteral("more data than requested"));
}

void Download::onSourceFinished(SourceSlot *s, qint64 fileOff, qint64 len, bool ok, const QString &err)
{
    const qint64 off = s->base + fileOff;
    int idx = -1;
    for (int i = 0; i < s->reqs.size(); ++i)
        if (s->reqs.at(i).off == off) { idx = i; break; }
    if (idx < 0) return;   // cancelled / unknown: ignore
    const Request r = s->reqs.takeAt(idx);
    Q_UNUSED(len)
    if (ok) {
        if (r.got > 0) s->failures = 0;
        if (r.got < r.len) {
            if (m_part && m_part->extent() < 0 && r.sequential) {
                // EOF of an unknown-length transfer: now we know the size
                const qint64 size = r.off + r.got;
                if (size > 0) {
                    qCInfo(lcEngine) << m_rec.id << "unknown-size transfer ended at" << size << "bytes";
                    m_rec.size = size;
                    m_part->setSize(size);
                    s->span = size;
                    persist(true);
                }
            } else if (r.got == 0) {
                ++s->failures;
            }
        }
    } else if (err == RangeSource::rangeNotAvailableError()) {
        // [up-1] a partial source does not hold that range (any more): not its fault; its
        // available() map no longer offers it, the scheduler asks elsewhere
        qCDebug(lcEngine) << m_rec.id << "range not available on" << s->key;
    } else if (err.startsWith(QLatin1String("HTTP server does not support byte ranges"))) {
        // mx::kHttpErrorNoRanges: not a failure, this source only serves from offset 0
        s->desc.extra.insert(QStringLiteral("supportsRanges"), false);
    } else {
        ++s->failures;
        qCDebug(lcEngine) << m_rec.id << "request failed on" << s->key << ":" << err;
        if (s->failures >= 3) {
            closeSlot(s, true, err);
            return;
        }
    }
    emit needsSchedule();
    QTimer::singleShot(0, this, [this] { maybeComplete(); });
}

RangeSet Download::slotAvailability(const SourceSlot *s) const
{
    RangeSet out;
    if (!s->rs) return out;
    const qint64 span = s->span;
    const QList<QPair<qint64, qint64>> av = s->rs->available();
    int n = 0;
    for (const auto &p : av) {
        if (++n > 100000) break;
        qint64 a = p.first, b = p.second;
        if (a < 0 || b <= a) continue;
        if (span >= 0) { if (a >= span) continue; b = qMin(b, span); }
        out.add(s->base + a, s->base + b);
    }
    if (av.isEmpty()) out.add(s->base, s->base + (span >= 0 ? span : kHugeLen));
    return out;
}

// ============================================================================ scheduler

void Download::schedule()
{
    m_lastScheduleMs = nowMs();
    Store *st = store();
    if (!m_active || !st || !st->acceptingData() || m_completing || m_promoting) return;
    if (m_host->bufferedGlobal() > m_host->tuning().maxBufferedBytes) return;
    const RangeSet target = st->fetchTarget();
    RangeSet inflight;
    for (const auto &s : m_slots) inflight.add(s->inflight());
    if (target.isEmpty()) return;
    RangeSet freeSet = target;
    freeSet.remove(inflight);
    const qint64 ext = st->extent();
    const Tuning &t = m_host->tuning();
    qint64 avgChunk = 256 * 1024;
    QList<SourceSlot *> ready;
    for (auto &s : m_slots) {
        if (!slotReady(s.get()) || !slotUsable(s.get())) continue;
        if (s->holdUntilMs > m_lastScheduleMs) continue;
        ready << s.get();
        avgChunk = qMax<qint64>(avgChunk, s->rs->preferredRequestSize());
    }
    if (ready.isEmpty()) return;
    const qint64 left = target.total();
    const bool endgame = freeSet.isEmpty() && !inflight.isEmpty()
                         && (ext > 0 && (left * 100 <= ext * t.endgamePercent || left <= avgChunk * t.endgameChunks));
    const qint64 now = m_lastScheduleMs;
    std::stable_sort(ready.begin(), ready.end(), [now](const SourceSlot *a, const SourceSlot *b) {
        if (a->priority != b->priority) return a->priority > b->priority;   // [glue-2] user priority first
        return a->rate.rate(now, 10) > b->rate.rate(now, 10);
    });
    if (m_singleSource) {
        SourceSlot *keep = ready.first();
        for (SourceSlot *s : ready) if (!s->reqs.isEmpty()) { keep = s; break; }
        ready = {keep};
    }
    for (SourceSlot *s : ready) {
        const int maxReq = qBound(1, s->rs->maxPendingRequests(), 16);
        while (s->reqs.size() < maxReq) {
            Request r;
            if (!pickChunk(s, freeSet, inflight, endgame, &r)) break;
            issue(s, r);
            if (!s->rs) break;   // the source died inside request()
            freeSet.remove(r.off, r.end());
            inflight.add(r.off, r.end());
        }
    }
}

bool Download::pickChunk(SourceSlot *s, const RangeSet &freeSet, const RangeSet &inflight, bool endgame, Request *out)
{
    Store *st = store();
    const RangeSet avail = slotAvailability(s);
    if (avail.isEmpty()) return false;
    RangeSet cand = freeSet.intersected(avail);
    RangeSet foreignOwned;
    for (auto it = m_unitOwner.constBegin(); it != m_unitOwner.constEnd(); ++it)
        if (it.value() != s) foreignOwned.add(st->unitAt(it.key()).first, st->unitAt(it.key()).second);
    cand.remove(foreignOwned);
    // [engine-3] a failed unit is re-fetched from a DIFFERENT source when one is ready
    for (qint64 u : std::as_const(m_isolated)) {
        if (m_unitOwner.contains(u) || !m_failedBy.value(u).contains(s->key)) continue;
        // ... or, when every ready source contributed, from the one with the better track record
        bool other = false;
        for (const auto &o : m_slots) {
            if (o.get() == s || o->banned || !o->trusted || !slotReady(o.get())
                || !slotAvailability(o.get()).contains(st->unitAt(u).first, st->unitAt(u).second)) continue;
            const bool contributed = m_failedBy.value(u).contains(o->key);
            if (!contributed || o->strikes < s->strikes || (o->strikes == s->strikes && o->verifiedBytes > s->verifiedBytes + PartStore::Ed2kPart)) {
                other = true;
                break;
            }
        }
        if (other) cand.remove(st->unitAt(u).first, st->unitAt(u).second);
    }
    auto restrictTrust = [&](RangeSet &set) {
        if (s->trusted) return;
        if (m_part && m_part->scheme() == PartStore::NoUnits && provisionalUsable(s)) return;   // [engine-4]
        if (m_ed2kProvisional && m_part && m_part->scheme() == PartStore::Ed2kParts) { set = RangeSet(); return; }
        RangeSet ok;
        for (const Range &r : set.ranges())
            for (qint64 p = r.first; p < r.second;) {
                const Range u = st->unitAt(p);
                const qint64 e = qMin(r.second, qMax(u.second, p + 1));
                if (st->canVerifyUnit(p)) ok.add(p, e);
                p = e;
            }
        // [merge-1] the waste cap: an unconfirmed network works on a few units at a time
        RangeSet allowed;
        if (candidateCapped(s, &allowed)) ok = ok.intersected(allowed);
        set = ok;
    };
    restrictTrust(cand);
    const qint64 pref = qBound<qint64>(16 * 1024, s->rs->preferredRequestSize(), 32LL << 20);
    const qint64 fileEnd = s->span >= 0 ? s->base + s->span : kHugeLen;

    // unknown size: one sequential request from the end of what we hold
    if (st->extent() < 0) {
        if (cand.isEmpty() || !s->reqs.isEmpty()) return false;
        out->off = cand.first();
        out->len = kHugeLen;
        out->sequential = true;
        return true;
    }
    if (s->sequentialOnly()) {
        if (cand.isEmpty() || !s->reqs.isEmpty()) return false;
        const qint64 first = cand.first();
        const qint64 redundant = first - s->base;
        int others = 0;
        for (const auto &o : m_slots) if (o.get() != s && slotReady(o.get()) && !o->sequentialOnly()) ++others;
        const qint64 allowance = qMax<qint64>(16LL << 20, (s->span > 0 ? s->span : 0) / 10);
        if (redundant > allowance && others > 0) return false;
        out->off = s->base;
        out->len = qMin(cand.runEnd(first), fileEnd) - s->base;
        out->sequential = true;
        return out->len > 0;
    }
    if (cand.isEmpty()) {
        if (!endgame) return false;
        RangeSet dup = inflight;
        dup.remove(s->inflight());
        dup.remove(foreignOwned);
        dup = dup.intersected(avail);
        restrictTrust(dup);
        if (dup.isEmpty()) return false;
        const qint64 start = dup.first();
        const Range u = st->unitAt(start);
        const qint64 end = qMin(qMin(dup.runEnd(start), start + pref), qMin(qMax(u.second, start + 1), fileEnd));
        if (end <= start) return false;
        out->off = start;
        out->len = end - start;
        return true;
    }
    QList<SourceSlot *> peers;
    QList<RangeSet> peerAvail;
    for (const auto &o : m_slots)
        if (o.get() != s && slotReady(o.get())) { peers << o.get(); peerAvail << slotAvailability(o.get()); }
    // [dl-1] download order first (sequential / first and last); a priority, never a condition:
    // nothing of it in this source's candidates → rarest-first below
    qint64 bestPos = -1;
    if ((m_seq || m_firstLast) && orderedStart(s, cand, peerAvail, &bestPos)) {
        const Range u = st->unitAt(bestPos);
        const qint64 end = qMin(qMin(cand.runEnd(bestPos), bestPos + pref), qMin(qMax(u.second, bestPos + 1), fileEnd));
        if (end > bestPos) {
            out->off = bestPos;
            out->len = end - bestPos;
            return true;
        }
        bestPos = -1;
    }
    // rarest-first over units, preferring units already in progress
    const RangeSet target = st->fetchTarget();
    int bestProgress = 2, bestRarity = INT_MAX;
    int examined = 0;
    for (const Range &r : cand.ranges()) {
        for (qint64 p = r.first; p < r.second && examined < 400; ++examined) {
            const Range u = st->unitAt(p);
            const qint64 ue = qMin(r.second, qMax(u.second, p + 1));
            const bool inProgress = !target.contains(u.first, u.second) || inflight.intersects(u.first, u.second);
            const int prog = inProgress ? 0 : 1;
            int rarity = 0;
            for (const RangeSet &pa : peerAvail) if (pa.contains(p)) ++rarity;
            if (prog < bestProgress || (prog == bestProgress && rarity < bestRarity)) {
                bestProgress = prog;
                bestRarity = rarity;
                bestPos = p;
            }
            p = ue;
        }
        if (examined >= 400) break;
    }
    if (bestPos < 0) return false;
    const Range u = st->unitAt(bestPos);
    const qint64 end = qMin(qMin(cand.runEnd(bestPos), bestPos + pref), qMin(qMax(u.second, bestPos + 1), fileEnd));
    if (end <= bestPos) return false;
    out->off = bestPos;
    out->len = end - bestPos;
    return true;
}

void Download::issue(SourceSlot *s, const Request &rIn)
{
    Request r = rIn;
    r.startMs = r.lastDataMs = nowMs();
    if (Store *st = store()) {
        const qint64 u = st->unitAt(r.off).first;
        if (m_isolated.contains(u) && !m_unitOwner.contains(u)) m_unitOwner.insert(u, s);
    }
    s->reqs << r;
    if (s->slowSinceMs == 0) s->slowSinceMs = r.startMs;
    RangeSource *rs = s->rs;
    rs->request(r.off - s->base, r.len);
}

void Download::checkTimeouts(qint64 now)
{
    for (auto &sp : m_slots) {
        SourceSlot *s = sp.get();
        if (!s->rs || s->reqs.isEmpty()) continue;
        for (const Request &r : std::as_const(s->reqs)) {
            if (now - r.lastDataMs > m_host->tuning().requestTimeoutMs) {
                ++s->failures;
                cancelSlotRequests(s, QStringLiteral("request timed out"));
                if (s->failures >= 3) closeSlot(s, true, QStringLiteral("Timed Out"));
                break;
            }
        }
    }
}

void Download::replaceSlowSources(qint64 now)
{
    const Tuning &t = m_host->tuning();
    SourceSlot *fast = nullptr;
    int fastRate = 0;
    for (auto &sp : m_slots) {
        if (!slotReady(sp.get())) continue;
        const int r = sp->rate.rate(now, 5);
        if (r > fastRate) { fastRate = r; fast = sp.get(); }
    }
    if (!fast) return;
    const bool fastStarved = fast->reqs.size() < qBound(1, fast->rs->maxPendingRequests(), 16);
    for (auto &sp : m_slots) {
        SourceSlot *s = sp.get();
        if (s == fast || !s->rs || s->reqs.isEmpty()) { if (s->reqs.isEmpty()) s->slowSinceMs = 0; continue; }
        if (s->priority > 0) continue;   // [glue-2] the user wants this one
        const int r = s->rate.rate(now, 10);
        if (r >= t.slowRateBps) { s->slowSinceMs = 0; continue; }
        if (s->slowSinceMs == 0) s->slowSinceMs = now;
        if (now - s->slowSinceMs < t.slowForMs) continue;
        if (!fastStarved || fastRate < qMax(20 * 1024, 8 * qMax(r, 1))) continue;
        if (s->lastReplacedMs && now - s->lastReplacedMs < 3LL * t.slowForMs) continue;
        qCInfo(lcEngine) << m_rec.id << "replacing slow source" << s->key << "(" << r << "B/s vs" << fastRate << "B/s)";
        cancelSlotRequests(s, QStringLiteral("slow"));
        s->holdUntilMs = now + t.slowHoldMs;
        s->lastReplacedMs = now;
        s->slowSinceMs = 0;
    }
}

void Download::cancelRedundant()
{
    Store *st = store();
    if (!st) return;
    const RangeSet target = st->fetchTarget();
    for (auto &sp : m_slots) {
        SourceSlot *s = sp.get();
        if (s->reqs.isEmpty()) continue;
        bool allRedundant = true;
        for (const Request &r : std::as_const(s->reqs)) {
            if (r.sequential && st->extent() < 0) { allRedundant = false; break; }
            if (target.intersects(r.off + r.got, r.end())) { allRedundant = false; break; }
        }
        if (allRedundant) cancelSlotRequests(s, QStringLiteral("endgame: already received elsewhere"));
    }
}

// ============================================================================ verification results

void Download::onUnitVerified(qint64 s, qint64 e)
{
    if (m_suspect.intersects(s, e)) {   // [safe-1] proven now
        m_suspect.remove(s, e);
        m_dirtyBlob = true;
    }
    const QSet<QString> struck = m_failedBy.take(s);
    m_isolated.remove(s);
    m_unitOwner.remove(s);
    // [up-1] judged by a provisional set: data of sources other than the set's sender that matches it
    // corroborates the set; data of its sender alone proves nothing about the others
    bool providerOnly = m_ed2kProvisional && m_part && m_part->scheme() == PartStore::Ed2kParts;
    if (providerOnly) {
        bool any = false;
        for (auto &sp : m_slots)
            if (sp->contributed.coveredIn(s, e) > 0) {
                any = true;
                if (sp->key != m_ed2kSetProvider) providerOnly = false;
            }
        if (!any) providerOnly = false;
        if (!providerOnly && !m_provisionalCorroborated) {
            m_provisionalCorroborated = true;
            qCInfo(lcEngine) << m_rec.id << "the provisional hash set is corroborated: data of other sources matches it";
            const QList<qint64> deferred = m_deferredBlackBox;   // their good data is on disk: judge them now
            m_deferredBlackBox.clear();
            for (qint64 u : deferred) checkBlackBox(u, {});
        }
    }
    const bool blackBox = m_blackBox.contains(s);   // it decides about the earlier contributors
    if (blackBox && providerOnly && !m_provisionalCorroborated) m_deferredBlackBox << s;
    else if (blackBox) checkBlackBox(s, struck);
    for (auto &sp : m_slots) {
        SourceSlot *src = sp.get();
        const qint64 n = src->contributed.coveredIn(s, e);
        if (n <= 0) continue;
        if (!blackBox && struck.contains(src->key) && src->strikes > 0 && !src->banned) {
            --src->strikes;   // it delivered good data for the same unit: forgive
            m_strikes.insert(src->key, src->strikes);
        }
        src->contributed.remove(s, e);
        src->verifiedBytes += n;
        m_verifiedByNet[int(src->desc.net)] += n;
        src->failures = 0;
        if (!src->trusted) candidateConfirmed(src);
    }
    m_dirtyBlob = true;
    markDirty();
    QTimer::singleShot(0, this, [this] { maybeComplete(); });
}

void Download::onUnitFailed(qint64 s, qint64 e)
{
    QList<SourceSlot *> contributors, untrusted;
    for (auto &sp : m_slots) {
        if (!sp->contributed.intersects(s, e)) continue;
        contributors << sp.get();
        if (!sp->trusted) untrusted << sp.get();
    }
    ++m_unitFailures;
    m_failedUnitBytes += e - s;
    if (m_part && !m_torrent && !contributors.isEmpty() && untrusted.isEmpty()) recordBlackBox(s, e, contributors);
    for (SourceSlot *c : std::as_const(contributors)) c->contributed.remove(s, e);
    qCInfo(lcEngine) << m_rec.id << "unit" << s << "-" << e << "failed verification;" << contributors.size() << "contributors";
    if (m_suspect.intersects(s, e)) {
        // [safe-1] it holds bytes written before an unclean stop that no check had proven: the disk lost
        // them (a sync it did not do); nobody is blamed, the unit is fetched again
        qCInfo(lcEngine) << m_rec.id << "the unit held bytes from before the unclean stop: nobody is blamed";
        m_suspect.remove(s, e);
        m_unitOwner.remove(s);
        m_dirtyBlob = true;
        markDirty();
        emit needsSchedule();
        return;
    }
    if (contributors.isEmpty() && !m_repairPath.isEmpty()) {
        // [ver-1] a finished file repaired in place: these bytes came from the disk, not from a source
        m_unitOwner.remove(s);
        m_dirtyBlob = true;
        markDirty();
        emit needsSchedule();
        return;
    }
    if (m_ed2kProvisional && !m_provisionalCorroborated && m_part && m_part->scheme() == PartStore::Ed2kParts) {
        // [up-1] judged by a set nobody vouches for: the set may be the liar, not the senders. No
        // strikes; the second failure drops the set (its sender's sets are not used again) and the
        // data is checked at the next level (trusted hashes, the whole file)
        ++m_provisionalFailures;
        qCInfo(lcEngine) << m_rec.id << "unit failed against the provisional hash set (" << m_provisionalFailures << "): no strikes";
        if (m_provisionalFailures >= 2) dropProvisionalSet(QStringLiteral("parts of other sources do not match it"));
        m_unitOwner.remove(s);
        m_dirtyBlob = true;
        markDirty();
        emit needsSchedule();
        return;
    }
    if (!untrusted.isEmpty()) {
        // [merge-1] the judge said no: that source is banned; its network is dropped after a few
        for (SourceSlot *c : untrusted) candidateFailed(c, QStringLiteral("Did not match: a block failed the check"));
    } else if (contributors.size() == 1 && m_isolated.contains(s)) {
        ban(contributors.first(), QStringLiteral("File Mismatch (002/004): isolated re-fetch failed again"));
    } else {
        for (SourceSlot *c : contributors) {
            strike(c, QStringLiteral("File Mismatch (002/004)"));
            m_failedBy[s].insert(c->key);
        }
        m_isolated.insert(s);
    }
    m_unitOwner.remove(s);
    m_dirtyBlob = true;
    markDirty();
    emit needsSchedule();
}

void Download::onUnitAbandoned(qint64 s, qint64 e)
{
    for (auto &sp : m_slots) {
        sp->contributed.remove(s, e);
        for (const Request &r : std::as_const(sp->reqs))
            if (r.off < e && r.end() > s) { cancelSlotRequests(sp.get(), QStringLiteral("unit no longer needed")); break; }
    }
    emit needsSchedule();
}

void Download::onStoreError(const QString &text, bool diskFull)
{
    qCWarning(lcEngine) << m_rec.id << "store error:" << text;
    setError(diskFull ? QStringLiteral("Disk full") : text, true);
}

// ============================================================================ discovery

void Download::findSources(bool force)
{
    if (!m_active || isFinished()) return;
    const qint64 now = nowMs();
    // [engine-4] the re-ask interval is the download's Auto Find Sources setting (original WinMX "AFS"):
    // a fixed period, 0 = never by itself (only on start, network up, new hashes, "Find Sources Now")
    qint64 interval = m_host->tuning().findSourcesBaseMs;
    if (interval <= 0) {
        const int afs = effectiveAfs();
        interval = afs > 0 ? qint64(afs) * m_host->tuning().afsMinuteMs : -1;
    }
    FileHashes h = m_rec.hashes;
    h.mergeFrom(m_hinted);
    const QByteArray key = QCborValue::fromVariant(h.toVariant()).toCbor();
    if (!force && (interval < 0 || (m_lastFindMs && now - m_lastFindMs < interval))) return;
    if (force && m_lastFindMs && now - m_lastFindMs < 2000 && key == m_lastFindKey) return;   // debounce bursts
    m_lastFindMs = now;
    m_lastAskMs = now;
    m_lastFindKey = key;
    if (!force) ++m_findRound;
    const qint64 size = m_rec.isBundle ? 0 : m_rec.size;
    for (NetworkPlugin *p : m_host->plugins()) {
        if (!(p->capabilities() & NetworkPlugin::CanFindSources) || !m_host->pluginUsable(p)) continue;
        p->findSources(m_rec.id, h, size, m_rec.name);
        if (netUsesHashOf(p->net(), h) && p->net() != Net::Http) m_askedNets.insert(int(p->net()));
    }
    markDirty();
}

void Download::networkCameUp(NetworkPlugin *p)
{
    if (!m_active || isFinished() || !p || !m_host->pluginUsable(p)) return;
    FileHashes h = m_rec.hashes;
    h.mergeFrom(m_hinted);
    if ((p->capabilities() & NetworkPlugin::CanSearch) && !m_nameSearchRunning && m_nameSearches > 0 && m_nameSearches < 6
        && !m_nameSearchNets.contains(p->net())) {
        // [engine-4] a keyword network that was not up for the last size+name search: search again soon
        m_nextNameSearchMs = qMin(m_nextNameSearchMs, nowMs() + 5000);
    }
    if (m_torrent && p->net() == Net::BitTorrent && m_host->backend()) {
        // [engine-4] the DHT was not up when the torrent was added: announce now instead of in 15 min
        m_host->backend()->reannounce(m_torrentId);
        m_lastReannounceMs = nowMs();
    }
    if (p->capabilities() & NetworkPlugin::CanFindSources) {
        p->findSources(m_rec.id, h, m_rec.isBundle ? 0 : m_rec.size, m_rec.name);
        if (netUsesHashOf(p->net(), h) && p->net() != Net::Http) m_askedNets.insert(int(p->net()));
        m_lastAskMs = nowMs();
    }
    if ((p->capabilities() & NetworkPlugin::CanHashLinks) && !m_rec.hashes.isEmpty()) p->lookupHashLinks(m_rec.hashes, m_rec.size);
}

void Download::stopFindSources()
{
    for (NetworkPlugin *p : m_host->plugins())
        if (p->capabilities() & NetworkPlugin::CanFindSources) p->stopFindSources(m_rec.id);
}

void Download::lookupHashLinks()
{
    if (m_rec.hashes.isEmpty()) return;
    if (HashLinksDao *links = m_host->links()) {
        for (const HashLinkRecord &r : links->find(m_rec.hashes, m_rec.isBundle ? -1 : (m_rec.size > 0 ? m_rec.size : -1))) {
            const bool ours = r.origin == QLatin1String("library") || r.origin == QLatin1String("download");
            learnHashes(r.hashes, ours, QStringLiteral("local hash link (%1)").arg(r.origin));
        }
    }
    for (NetworkPlugin *p : m_host->plugins())
        if ((p->capabilities() & NetworkPlugin::CanHashLinks) && m_host->pluginUsable(p))
            p->lookupHashLinks(m_rec.hashes, m_rec.size);
}

void Download::publishLinks(const FileHashes &all)
{
    if (kindCount(all) < 2) return;
    const QString key = QString::fromLatin1(QCborValue::fromVariant(all.toVariant()).toCbor().toHex());
    if (m_publishedLinks.contains(key)) return;
    m_publishedLinks.insert(key);
    if (HashLinksDao *links = m_host->links()) links->upsert(all, m_rec.isBundle ? -1 : m_rec.size, m_rec.name, QStringLiteral("download"));
    for (NetworkPlugin *p : m_host->plugins())
        if ((p->capabilities() & NetworkPlugin::CanHashLinks) && m_host->pluginUsable(p))
            p->publishHashLinks(all, m_rec.size, m_rec.name);
}

// ============================================================================ completion

void Download::maybeComplete()
{
    if (m_completing || isFinished() || !m_active) return;
    if (m_torrent) {
        if (!m_torrent->hasMetadata() || !m_torrent->snap().finished || m_torrent->reservedCount() != 0) return;
        if (m_checkFirst) return;   // [safe-1] the recheck decides first
        // [merge-1] "finished" alone is not enough: right after a promotion the snapshot can still be the
        // metadata-only one (no file wanted = finished) before the recheck; every wanted file must be done
        const TorrentSnapshot &sn = m_torrent->snap();
        bool wanted = false;
        for (int i = 0; i < sn.files.size(); ++i) {
            if (m_torrent->isPadFile(i) || sn.files.at(i).priority <= 0) continue;
            wanted = true;
            if (sn.files.at(i).done >= 0 && sn.files.at(i).done < sn.files.at(i).size) return;
        }
        if (wanted) completeTorrent();
        return;
    }
    if (!m_part || m_part->extent() < 0 || m_part->hasError()) return;
    if (m_part->bufferedBytes() > 0) return;
    if (m_repairWhole) return;   // [ver-1] damage only the whole-file checksum shows: block hashes first
    if (m_part->fullyVerified()) {
        finalizePart();
        return;
    }
    if (m_part->fullyHeld() && m_part->verifyingCount() == 0) {
        if (m_awaitHashSetUntil && nowMs() < m_awaitHashSetUntil) return;
        finalizePart();
    }
}

void Download::finalizePart()
{
    m_completing = true;
    closeAllSources(QStringLiteral("done"));
    stopFindSources();
    setState(TransferState::Verifying, transferStateText(TransferState::Verifying));
    const bool verify = !m_host->settings() || m_host->settings()->b(keys::transfers::VerifyCompleted);
    const bool fine = m_part->fullyVerified();
    if (fine && !verify) {
        finishPart(m_rec.hashes, {}, {}, false);
        return;
    }
    m_part->finalCheck([this, fine](const PartStore::FinalResult &r) {
        if (m_quitting) return;   // [safe-1] cancelled by the quit: it runs again at the next start
        if (!r.ok) {
            m_completing = false;
            setError(r.error.isEmpty() ? QStringLiteral("Cannot read the downloaded file") : r.error, true);
            return;
        }
        // every known hash kind is checked; [merge-1] a kind that only a confirmed name-and-size
        // candidate brought (not the judge's) that disagrees is reported, it does not fail the file
        QStringList mismatch, foreign;
        const FileHashes &t = m_rec.hashes;
        const FileHashes &cc = m_confirmedClaims;
        auto cmp = [&](const QByteArray &a, const QByteArray &b, const QByteArray &claimed, const char *n) {
            if (a.isEmpty() || b.isEmpty() || a == b) return;
            (!claimed.isEmpty() && claimed == a ? foreign : mismatch) << QLatin1String(n);
        };
        cmp(t.ed2k, r.hashes.ed2k, cc.ed2k, "ed2k");
        cmp(t.sha1, r.hashes.sha1, cc.sha1, "sha1");
        cmp(t.sha256, r.hashes.sha256, cc.sha256, "sha256");
        cmp(t.md5, r.hashes.md5, cc.md5, "md5");
        cmp(t.tth, r.hashes.tth, cc.tth, "tth");
        cmp(t.aich, r.hashes.aich, cc.aich, "aich");
        // the judge's kinds all agree: a disagreeing kind of another network is that copy's fault
        if (mismatch.isEmpty() && !foreign.isEmpty()) {
            m_completionNote = disagreementNote(foreign);
            qCWarning(lcEngine).noquote() << m_rec.id << "complete, but" << m_completionNote;
            forgetKinds(foreign);
            for (const QString &k : std::as_const(foreign)) m_confirmedFrom.remove(k);
        }
        if (!mismatch.isEmpty()) mismatch << foreign;
        if (!mismatch.isEmpty()) {
            if (fine && m_ed2kProvisional) {
                // every part matched a set that reduces to no trusted hash, the file does not: the set lied
                qCWarning(lcEngine) << m_rec.id << "all parts matched the provisional hash set but the file hashes differ:" << mismatch;
                // [up-1] its own data matched its own set and makes another file: that is the data's fault
                if (SourceSlot *p = slotFor(m_ed2kSetProvider); p && p->verifiedBytes > 0)
                    ban(p, QStringLiteral("File Mismatch: the parts it sent and its part hash set describe another file"));
                dropProvisionalSet(QStringLiteral("its part hash set does not describe the wanted file"));
                wholeFileFailed(mismatch);
                return;
            }
            if (fine) {
                setError(QStringLiteral("File Mismatch (002/004)"), true);
                m_completing = false;
                qCWarning(lcEngine) << m_rec.id << "all units verified but whole-file hashes differ:" << mismatch;
                return;
            }
            wholeFileFailed(mismatch);
            return;
        }
        if (!fine) creditWholeFile();   // [engine-4] no units: the whole file verified at once
        m_part->markAllVerified();
        // [ver-1] a file that had no checksum before cannot be verified later (its hashes are those of
        // whatever arrived); one that had: the block hashes of the checked file are kept for Verify File
        const FileHashes &id = m_rec.hashes;
        const bool hadSum = !id.ed2k.isEmpty() || !id.sha1.isEmpty() || !id.sha256.isEmpty() || !id.md5.isEmpty()
                            || !id.tth.isEmpty() || !id.aich.isEmpty();
        if (!hadSum) m_noChecksum = true;
        else if (!m_rec.hashes.conflicts(r.hashes)) keepBlockHashes(r.tthUnit, r.tthLevel, r.ed2kParts, r.hashes);
        setCheckFirst(false);   // [safe-1] every byte was just checked
        finishPart(r.hashes, r.ed2kParts, r.aichParts, true);
    });
}

void Download::wholeFileFailed(const QStringList &mismatch)
{
    m_completing = false;
    ++m_wholeFileFailures;
    qCWarning(lcEngine) << m_rec.id << "whole-file verification failed (" << mismatch.join(QLatin1Char(',')) << ") attempt"
                        << m_wholeFileFailures;
    if (!m_suspect.isEmpty() && m_part) {
        // [safe-1] the file holds bytes from before an unclean stop that no block hash could prove: the
        // disk may have lost them; nobody is blamed, everything not verified is fetched again
        qCInfo(lcEngine) << m_rec.id << "the file held bytes from before the unclean stop: nobody is blamed";
        --m_wholeFileFailures;
        m_suspect.clear();
        for (auto &sp : m_slots) sp->contributed.clear();
        m_part->discardUnverified();
        persist(true);
        updateStatus();
        openMoreSources();
        emit needsSchedule();
        return;
    }
    QList<SourceSlot *> contributors;
    for (auto &sp : m_slots) if (!sp->contributed.isEmpty()) contributors << sp.get();
    // [engine-4] candidates used provisionally are the suspects: ban them, reject their claim and
    // start over without them (their bytes cannot be told apart from the others' without units)
    QList<SourceSlot *> untrusted;
    for (SourceSlot *c : std::as_const(contributors)) if (!c->trusted) untrusted << c;
    if (!untrusted.isEmpty()) {
        for (SourceSlot *c : std::as_const(untrusted)) candidateFailed(c, QStringLiteral("File Mismatch: the candidate has a different file"));
        if (!m_provisionalClaim.isEmpty()) {
            m_rejected << m_provisionalClaim;
            if (m_provisionalClaim.matches(m_hinted)) { m_rejected << m_hinted; m_hinted = FileHashes(); }
        }
        qCWarning(lcEngine) << m_rec.id << "provisional candidates failed the whole-file check:" << untrusted.size() << "banned";
        m_provisionalClaim = FileHashes();
        m_provisional = false;
        m_awaitHashSetUntil = 0;
        for (SourceSlot *c : std::as_const(contributors)) c->contributed.clear();
        m_part->discardUnverified();
        persist(true);
        updateStatus();
        openMoreSources();
        emit needsSchedule();
        return;
    }
    // 1) a finer hash set may still come and locate the bad part: wait for it (once)
    const bool setPossible = (!m_rec.hashes.ed2k.isEmpty() && m_rec.size >= PartStore::Ed2kPart && m_ed2kParts.isEmpty())
                             || (!m_rec.hashes.tth.isEmpty() && m_tthLeaves.isEmpty());
    if (setPossible && m_wholeFileFailures == 1) {
        m_awaitHashSetUntil = nowMs() + 5 * 60 * 1000;
        setState(TransferState::Verifying, QStringLiteral("Waiting for hash set..."));
        findSources(true);
        for (SourceSlot *c : contributors) strike(c, QStringLiteral("whole-file mismatch"));
        markDirty(true);
        openMoreSources();
        return;
    }
    m_awaitHashSetUntil = 0;
    // 2) restart, one source at a time, so the next failure names the culprit
    for (SourceSlot *c : contributors) {
        if (m_singleSource && contributors.size() <= 2) ban(c, QStringLiteral("whole-file mismatch in single-source mode"));
        else strike(c, QStringLiteral("whole-file mismatch"));
        c->contributed.clear();
    }
    m_singleSource = true;
    if (m_wholeFileFailures >= 5) {
        setError(QStringLiteral("File Mismatch (002/004)"), true);
        return;
    }
    m_part->discardUnverified();
    persist(true);
    updateStatus();
    openMoreSources();
    emit needsSchedule();
}

void Download::finishPart(const FileHashes &all, const QList<QByteArray> &ed2kParts, const QList<QByteArray> &aichParts, bool haveAll)
{
    const QString dir = m_rec.saveDir.isEmpty() ? m_host->incomingDir() : m_rec.saveDir;
    // [dl-1] a preview player may hold the file: drop our link first; if the player still has it
    // open without delete sharing (Windows), the rename fails and we wait for it instead of copying
    const bool previewed = !m_previewPath.isEmpty() || m_moveRetries > 0;
    dropPreview();
    if (!m_moveRetries) setState(TransferState::Verifying, QStringLiteral("Moving..."));
    auto done = [this, all, haveAll, ed2kParts, aichParts, previewed](bool ok, const QString &path, const QString &err) {
        if (!ok && previewed && m_moveRetries < 17280) {   // a day of 5 s retries
            ++m_moveRetries;
            setState(TransferState::Verifying, QStringLiteral("Complete, waiting for the preview player to close the file..."));
            QTimer::singleShot(5000, this, [this, all, ed2kParts, aichParts, haveAll] { finishPart(all, ed2kParts, aichParts, haveAll); });
            return;
        }
        m_moveRetries = 0;
        dropPreview();   // [dl-1] Windows: the link could not go while our own handle was open
        if (!ok) {
            if (m_quitting) return;   // [safe-1] a copy cancelled by the quit runs again at the next start
            m_completing = false;
            setError(QStringLiteral("Cannot move to the incoming folder: %1").arg(err), true);
            return;
        }
        completeAt(path, all, ed2kParts, aichParts, haveAll);
    };
    // [ver-1] a finished file repaired in place stays where it is (no move, no "name (1)" copy)
    if (!m_repairPath.isEmpty() && QFileInfo(m_part->path()).absoluteFilePath() == QFileInfo(m_repairPath).absoluteFilePath())
        m_part->finishInPlace(done);
    else
        m_part->moveTo(dir, safeFileName(m_rec.name), done, !previewed);
}

// [safe-1] the finished file is at `path` (synced before the rename or copy)
void Download::completeAt(const QString &path, const FileHashes &all, const QList<QByteArray> &ed2kParts,
                          const QList<QByteArray> &aichParts, bool haveAll)
{
    m_rec.savePath = path;
    if (haveAll) {
        FileHashes h = all;
        h.btih.clear();
        h.btFileIndex = -1;
        checkClaimsAgainst(h);   // [core-5]
        if (!m_rec.hashes.conflicts(h)) m_rec.hashes.mergeFrom(h);
    }
    m_rec.completed = QDateTime::currentDateTime();
    m_rec.errorText.clear();
    m_active = false;
    setState(TransferState::Completed, m_completionNote.isEmpty() ? transferStateText(TransferState::Completed)
                                                                  : QStringLiteral("%1, but %2").arg(transferStateText(TransferState::Completed), m_completionNote));
    qCInfo(lcEngine) << m_rec.id << "complete:" << path;
    logShares();
    if (haveAll) publishLinks(m_rec.hashes);
    if (Library *lib = m_host->library()) {
        m_awaitHashPaths.insert(QFileInfo(path).absoluteFilePath());
        // our final pass already produced every hash: the library must not read the file again
        if (haveAll) lib->addHashed(path, all, ed2kParts, aichParts);
        else lib->requestHash(path);
    }
    m_completing = false;
    m_suspect.clear();   // [safe-1] the whole file is proven now
    if (!m_repairPath.isEmpty()) {   // [ver-1] a repaired file: where it was, with its name
        qCInfo(lcEngine) << m_rec.id << "repaired in place:" << path;
        m_repairPath.clear();
        m_repairWhole = false;
        m_damaged = false;
    }
    persist(true);
    m_host->finished(this);
    m_host->completedFile(this);
    // the move's marker stays until the next start sees this record (resolveInterruptedMove): a power
    // cut may still take the last database commit, never the marker before it
}

void Download::completeTorrent()
{
    m_completing = true;
    closeAllSources(QStringLiteral("done"));
    stopFindSources();
    m_torrent->releaseAll();
    const TorrentSnapshot &sn = m_torrent->snap();
    const bool seed = !m_host->settings() || m_host->settings()->b(keys::bt::SeedCompleted);
    if (!seed) m_host->backend()->pauseTorrent(m_torrentId);
    QStringList paths;
    for (int i = 0; i < sn.files.size(); ++i) {
        const TorrentFile &f = sn.files.at(i);
        if (m_torrent->isPadFile(i) || f.priority <= 0) continue;
        paths << QDir(sn.savePath).filePath(f.path);
    }
    if (m_rec.isBundle || paths.size() != 1) {
        QString top = sn.files.isEmpty() ? sn.name : sn.files.first().path.section(QLatin1Char('/'), 0, 0);
        m_rec.savePath = QDir(sn.savePath).filePath(top.isEmpty() ? sn.name : top);
    } else {
        m_rec.savePath = paths.first();
    }
    // [safe-1] the files are synced to the disk before the download is announced as finished
    m_torrent->syncFiles(paths, [this] { completeTorrentSynced(); });
}

void Download::completeTorrentSynced()
{
    if (!m_torrent || isFinished() || m_quitting) return;
    const TorrentSnapshot &sn = m_torrent->snap();
    QStringList paths;
    for (int i = 0; i < sn.files.size(); ++i) {
        const TorrentFile &f = sn.files.at(i);
        if (m_torrent->isPadFile(i) || f.priority <= 0) continue;
        paths << QDir(sn.savePath).filePath(f.path);
    }
    m_rec.completed = QDateTime::currentDateTime();
    m_active = false;
    setState(TransferState::Completed, transferStateText(TransferState::Completed));
    qCInfo(lcEngine) << m_rec.id << "torrent complete:" << m_rec.savePath << "injected" << m_torrent->injectedCount()
                     << "pieces /" << m_torrent->injectedBytes() << "bytes from other networks";
    logShares();
    if (Library *lib = m_host->library()) {
        for (const QString &p : std::as_const(paths)) {
            m_awaitHashPaths.insert(QFileInfo(p).absoluteFilePath());
            lib->requestHash(p);
        }
    }
    m_completing = false;
    persist(true);
    m_host->finished(this);
    m_host->completedFile(this);
}

void Download::onFileHashed(const SharedFile &f)
{
    const QString p = QFileInfo(f.path).absoluteFilePath();
    if (!m_awaitHashPaths.remove(p)) return;
    FileHashes all = sanitized(f.hashes);
    if (m_torrent) {
        // map the file back to its index in the torrent
        const TorrentSnapshot &sn = m_torrent->snap();
        for (int i = 0; i < sn.files.size(); ++i) {
            if (QFileInfo(QDir(sn.savePath).filePath(sn.files.at(i).path)).absoluteFilePath() == p) {
                all.btih = m_rec.hashes.btih;
                int real = 0;
                for (int k = 0; k < sn.files.size(); ++k) if (!m_torrent->isPadFile(k)) ++real;
                all.btFileIndex = real == 1 ? -1 : i;   // single-file torrent: the torrent IS the file
                break;
            }
        }
        if (!m_rec.isBundle) {
            FileHashes h = all;
            if (m_rec.hashes.conflicts(h)) {
                // [merge-1] the torrent's pieces are the judge and they verified every byte: a kind
                // another network brought that differs means that copy was not the same file
                QStringList kinds;
                auto diff = [&](const QByteArray &a, const QByteArray &b, const char *n) {
                    if (!a.isEmpty() && !b.isEmpty() && a != b) kinds << QLatin1String(n);
                };
                diff(m_rec.hashes.ed2k, h.ed2k, "ed2k");
                diff(m_rec.hashes.sha1, h.sha1, "sha1");
                diff(m_rec.hashes.tth, h.tth, "tth");
                diff(m_rec.hashes.aich, h.aich, "aich");
                diff(m_rec.hashes.sha256, h.sha256, "sha256");
                diff(m_rec.hashes.md5, h.md5, "md5");
                m_completionNote = disagreementNote(kinds);
                qCWarning(lcEngine).noquote() << m_rec.id << "torrent complete, but" << m_completionNote;
                forgetKinds(kinds);
                for (const QString &k : std::as_const(kinds)) m_confirmedFrom.remove(k);
                if (isFinished())
                    setState(TransferState(m_rec.state), QStringLiteral("%1, but %2").arg(transferStateText(TransferState::Completed), m_completionNote));
            }
            if (!m_rec.hashes.conflicts(h)) m_rec.hashes.mergeFrom(h);
        }
        FileHashes pub = all;
        if (pub.btih.isEmpty()) return;
        if (!m_rec.isBundle) pub.mergeFrom(m_rec.hashes);
        publishLinks(pub);
    } else {
        if (m_rec.hashes.conflicts(all)) {
            qCWarning(lcEngine) << m_rec.id << "library hashes of the completed file differ from the download identity!";
            return;
        }
        m_rec.hashes.mergeFrom(all);
        publishLinks(m_rec.hashes);
    }
    persist(true);
    markDirty();
}

// ============================================================================ promotion part → torrent

bool Download::canFallBackToPart() const
{
    if (m_rec.isBundle || !m_torrent) return false;
    if (m_rec.size <= 0) {
        // [engine-3] size unknown (magnet without xl): an HTTP(S) source tells it (Content-Length /
        // EOF): the part store handles an unknown size; promotion matches the file by that size
        for (const auto &sl : m_slots) if (!sl->banned && sl->desc.net == Net::Http) return true;
        return false;
    }
    for (const auto &sl : m_slots) if (!sl->banned) return true;   // HTTP / eD2k / overlay ... sources
    FileHashes other = m_rec.hashes;
    other.mergeFrom(m_hinted);
    other.btih.clear();
    other.btFileRoot.clear();
    return !other.isEmpty();                                         // other networks can look it up
}

void Download::fallBackToPart()
{
    TorrentBackend *be = m_host->backend();
    const QString id = m_torrentId;
    qCInfo(lcEngine) << m_rec.id << "no torrent metadata yet, continuing as a part-file download from the other"
                     << "sources; it is promoted back to the torrent when the metadata arrives";
    disconnect(m_torrent.get(), nullptr, this, nullptr);
    m_torrent->releaseAll();
    m_torrent.reset();
    m_torrentId.clear();
    m_fallbackTorrent = id;
    m_promoteTorrent = id;
    m_promoteTrusted = true;
    m_promoteFile = -1;
    // [eng-1] its metadata has been asked for since the activation: another candidate torrent may get
    // its turn at once (watchPromotion)
    m_promoteSinceMs = m_activatedMs ? m_activatedMs : nowMs();
    m_promoteAnnounceMs = nowMs();
    m_promotePeersMax = 0;
    if (be) {
        be->setWantedFiles(id, {-1});   // keep fetching metadata, download nothing meanwhile
        be->resumeTorrent(id);
    }
    createPartStore();
    // [merge-1] a torrent that gives no file list cannot judge anything: when the other copies are
    // only matched by name and size, their hashes judge the part file now and the torrent becomes the
    // copy to check (its piece must match our data before it is taken)
    const FileHashes &t = m_rec.hashes;
    if (t.ed2k.isEmpty() && t.sha1.isEmpty() && t.tth.isEmpty() && t.sha256.isEmpty() && t.md5.isEmpty() && t.aich.isEmpty()) {
        FileHashes claim = m_candidateHints;
        if (claim.isEmpty()) {
            QList<QPair<FileHashes, int>> claims;
            for (const auto &sp : m_slots) {
                if (sp->banned || !sp->wasCandidate || sp->claimed.isEmpty()) continue;
                bool grouped = false;
                for (auto &c : claims) if (c.first.matches(sp->claimed)) { ++c.second; grouped = true; break; }
                if (!grouped) claims << qMakePair(sp->claimed, 1);
            }
            std::stable_sort(claims.begin(), claims.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
            if (!claims.isEmpty()) claim = claims.first().first;
        }
        claim.btih.clear();
        claim.btFileIndex = -1;
        if (!claim.isEmpty() && !m_rec.hashes.conflicts(claim)) {
            m_promoteTrusted = false;
            m_stalledSwap = true;
            learnHashes(claim, true, QStringLiteral("roles changed: the torrent gives no file list, the other copy's hashes check the blocks"));
            qCInfo(lcEngine) << m_rec.id << "roles changed: the torrent gives no file list; it must match a held piece before it is used";
        }
    }
    mapSourcesToFiles();
    findSources(true);
    openMoreSources();
    persist(true);
    markDirty(true);
    emit needsSchedule();
}

void Download::considerPromotion()
{
    TorrentBackend *be = m_host->backend();
    if (!be || !m_part || m_promoting || !m_promoteTorrent.isEmpty() || !m_promoteUrl.isEmpty() || m_completing || isFinished()) return;
    QByteArray btih = m_rec.hashes.btih;
    bool trusted = true;
    if (btih.isEmpty()) { btih = m_hinted.btih; trusted = false; }
    if (btih.size() != 20 || m_rec.size <= 0) return;
    const QString id = hexId(btih);
    for (const FileHashes &r : std::as_const(m_rejected)) if (r.btih == btih) return;
    if (m_promoteBlocked == btih) return;   // [merge-1] asked before (the tick retries)
    const bool existed = be->torrents().contains(id);
    if (existed) {
        m_promoteBlocked = btih;
        // the torrent is already managed (another download / seeding): do not take it over
        qCInfo(lcEngine) << m_rec.id << "torrent identity" << id.left(8) << "already exists in the backend; not promoting";
        return;
    }
    hash::ParsedLink l;
    l.kind = hash::ParsedLink::Magnet;
    l.hashes.btih = btih;
    l.trackers = m_trackers;
    l.name = m_btNames.value(btih);
    QString err;
    const QString saveDir = m_rec.saveDir.isEmpty() ? m_host->incomingDir() : m_rec.saveDir;
    m_promoteSinceMs = m_promoteAnnounceMs = nowMs();   // [eng-1] the fetch clock (watchPromotion)
    m_promotePeersMax = 0;
    // [merge-1] an index that offers the .torrent itself: the metadata without peers (web-seeded items)
    const QString url = m_btTorrentUrls.value(btih);
    if (!url.isEmpty() && m_promoteUrl.isEmpty()) {
        const QString got = be->addTorrent(url, QByteArray(), saveDir, false, {-1}, &err);
        if (got.isEmpty() && err.isEmpty()) {
            m_promoteUrl = url;
            m_promoteExpect = btih;
            m_promoteTrusted = trusted;
            qCInfo(lcEngine) << m_rec.id << "promotion: fetching the .torrent of" << id.left(8) << url;
            return;
        }
        m_btTorrentUrls.remove(btih);
        if (!got.isEmpty()) {
            m_promoteTorrent = got;
            m_promoteTrusted = trusted;
            m_promoteFile = -1;
            be->setWantedFiles(got, {-1});
            if (be->snapshot(got).hasMetadata) tryPromote();
            return;
        }
    }
    // wanted = {-1}: fetch metadata only, download nothing (bt plugin: invalid indexes are ignored)
    const QString got = be->addTorrent(hash::formatMagnet(l), QByteArray(), saveDir, false, {-1}, &err);
    if (got.isEmpty()) {
        qCInfo(lcEngine) << m_rec.id << "promotion: cannot add torrent:" << err;
        m_promoteBlocked = btih;
        return;
    }
    m_promoteTorrent = got;
    m_promoteTrusted = trusted;
    m_promoteFile = -1;
    const int sw = m_btSwarm.value(btih, -1);
    qCInfo(lcEngine) << m_rec.id << "promotion: fetching metadata of" << got.left(8) << (trusted ? "(trusted)" : "(hint)")
                     << "(" << btLabel(btih) << ", swarm" << (sw < 0 ? QStringLiteral("unknown") : QString::number(sw)) << "by the index,"
                     << m_trackers.size() << "trackers," << m_btCandidates.size() << "other candidate torrents)";
    markDirty(true);
    if (be->snapshot(got).hasMetadata) tryPromote();
}

void Download::tryPromote()
{
    TorrentBackend *be = m_host->backend();
    if (!be || m_promoteTorrent.isEmpty() || !m_part || m_promoting) return;
    const TorrentSnapshot sn = be->snapshot(m_promoteTorrent);
    if (!sn.hasMetadata || sn.pieceLength <= 0) return;
    auto reject = [this, be](const QString &why) {
        qCInfo(lcEngine) << m_rec.id << "promotion rejected:" << why;
        FileHashes r;
        r.btih = hash::fromHex(m_promoteTorrent);
        m_btNote = QStringLiteral("The file of %1 does not match (%2), dropped").arg(btLabel(r.btih), why);   // [eng-1]
        m_btCandidates.removeAll(r.btih);
        m_rejected << r;
        if (m_hinted.btih == r.btih) m_hinted.btih.clear();
        if (!m_promoteTrusted && m_rec.hashes.btih == r.btih) {   // [merge-1] the torrent was the other copy
            m_rec.hashes.btih.clear();
            m_rec.hashes.btFileIndex = -1;
        }
        be->removeTorrent(m_promoteTorrent, false);
        m_promoteTorrent.clear();
        m_fallbackTorrent.clear();
        m_dirtyBlob = true;
        if (!m_promoteTrusted) QTimer::singleShot(0, this, [this] { nextTorrentCandidate(); });   // [merge-1]
    };
    // the file of the torrent with our size (name as tie-breaker)
    int idx = -1, hits = 0;
    for (int i = 0; i < sn.files.size(); ++i) {
        if (sn.files.at(i).size != m_rec.size) continue;
        if (QFileInfo(sn.files.at(i).path).fileName() == m_rec.name || hits == 0) idx = i;
        ++hits;
    }
    if (m_rec.hashes.btFileIndex >= 0 && m_rec.hashes.btFileIndex < sn.files.size()
        && sn.files.at(m_rec.hashes.btFileIndex).size == m_rec.size)
        idx = m_rec.hashes.btFileIndex;
    if (idx < 0) {
        reject(QStringLiteral("no file of size %1 in the torrent").arg(m_rec.size));
        return;
    }
    const TorrentFile f = sn.files.at(idx);
    // check one piece we already hold against the torrent's piece hash
    const qint64 L = sn.pieceLength;
    int checkPiece = -1;
    const RangeSet held = m_part->durable();
    for (qint64 p = (f.offset + L - 1) / L; (p + 1) * L <= f.offset + f.size; ++p) {
        if (held.contains(p * L - f.offset, (p + 1) * L - f.offset)) { checkPiece = int(p); break; }
    }
    bool roleSwap = false;
    if (checkPiece < 0) {
        if (m_promoteTrusted && m_part->heldBytes() == 0) {
            // nothing to check and nothing to lose: just switch
        } else if (!m_promoteTrusted && m_part->heldBytes() == 0) {
            // [merge-1] a torrent matched by name and size, nothing held yet: its piece hashes are the
            // finest judge known completely, so the roles change: the torrent judges, the copy the
            // download started from is now the one "with the same name and size" (checked per piece)
            roleSwap = true;
        } else {
            m_promoteFile = idx;   // wait until we hold a whole piece (re-checked on tick)
            return;
        }
    }
    m_promoting = true;
    m_promoteFile = idx;
    auto doPromote = [this, be, sn, f, idx, roleSwap]() {
        const QString target = QDir(sn.savePath).filePath(f.path);
        for (auto &s : m_slots) cancelSlotRequests(s.get(), QStringLiteral("promotion"));
        setState(TransferState::Verifying, QStringLiteral("Verifying..."));
        m_part->moveExact(target, [this, be, idx, target, roleSwap](bool ok, const QString &err) {
            if (!ok) {
                qCWarning(lcEngine) << m_rec.id << "promotion: move failed:" << err << "- staying a part-file download";
                m_promoting = false;
                be->removeTorrent(m_promoteTorrent, false);
                m_promoteTorrent.clear();
                m_restoreHeld = m_part->written();   // [safe-1] moveExact synced the file before it tried
                m_restoreVerified = m_part->verified();
                QFile::rename(target, partPath());
                m_part.reset();
                createPartStore();
                emit needsSchedule();
                return;
            }
            const QString id = m_promoteTorrent;
            qCInfo(lcEngine) << m_rec.id << "promoted to torrent" << id.left(8) << "file" << idx << "→" << target;
            if (!m_promoteTrusted && !roleSwap) {   // [merge-1] a torrent matched by name and size: its pieces matched our data
                CandNet &cn = m_candNets[int(Net::BitTorrent)];
                cn.confirmed = true;
                cn.dropped = false;
                qCInfo(lcEngine) << m_rec.id << "the BitTorrent copy is the same file: a piece matched; the judge is now BitTorrent piece hashes";
            } else if (roleSwap) {
                // [merge-1] the sources of the identity the download started with become copies
                // "with the same name and size": capped, confirmed or dropped by the pieces
                FileHashes claim = m_rec.hashes;
                claim.btih.clear();
                claim.btFileIndex = -1;
                int swapped = 0;
                for (auto &sp : m_slots) {
                    SourceSlot *x = sp.get();
                    if (!x->trusted || x->wasCandidate || x->banned || x->desc.net == Net::BitTorrent) continue;
                    x->trusted = false;
                    x->wasCandidate = true;
                    x->claimed = claim;
                    if (!x->desc.extra.contains(QStringLiteral("hashes"))) x->desc.extra.insert(QStringLiteral("hashes"), claim.toVariant());
                    ++swapped;
                }
                m_confirmedClaims.mergeFrom(claim);
                for (const QString &k : claim.toVariant().keys())
                    if (k != QLatin1String("btFileIndex")) m_confirmedFrom.insert(k, -1);
                qCInfo(lcEngine) << m_rec.id << "roles changed: BitTorrent piece hashes judge now," << swapped
                                 << "source(s) of the first identity are checked per piece";
            }
            m_rec.hashes.btih = hash::fromHex(id);
            m_rec.hashes.btFileIndex = idx;
            m_hinted.btih.clear();
            m_torrentId = id;
            m_promoteTorrent.clear();
            m_fallbackTorrent.clear();
            m_rec.wantedFiles = {idx};
            m_rec.savePath = target;
            m_restoreHeld.clear();
            m_restoreVerified.clear();
            m_part.reset();
            be->setWantedFiles(id, {idx});
            be->recheck(id);
            m_torrent = std::make_unique<TorrentStore>(be, id, m_host->pool(), &m_host->tuning());
            attachStore(m_torrent.get());
            if (m_active) be->resumeTorrent(id); else be->pauseTorrent(id);
            m_promoting = false;
            mapSourcesToFiles();
            publishLinks(m_rec.hashes);
            persist(true);
            markDirty(true);
            emit needsSchedule();
        });
    };
    if (checkPiece < 0) {
        doPromote();
        return;
    }
    const QByteArray expect = be->pieceHash(m_promoteTorrent, checkPiece);
    const qint64 s = qint64(checkPiece) * L - f.offset;
    m_part->sha1Range(s, s + L, [this, expect, doPromote, reject, checkPiece](const QByteArray &got) {
        if (got.isEmpty() || expect.size() != 20 || got != expect) {
            m_promoting = false;
            reject(QStringLiteral("piece %1 does not match our data").arg(checkPiece));
            return;
        }
        qCInfo(lcEngine) << m_rec.id << "promotion: piece" << checkPiece << "matches our data";
        doPromote();
    });
}

// ============================================================================ torrent events

void Download::onTorrentMetadata()
{
    if (!m_torrent) {
        tryPromote();
        return;
    }
    m_torrent->refresh();
    const TorrentSnapshot &sn = m_torrent->snap();
    if (!sn.hasMetadata) return;
    noteOwnership(sn);   // [ui-j] before libtorrent writes: which of its files were there already
    int real = 0;
    QList<BundleFile> files;
    for (int i = 0; i < sn.files.size(); ++i) {
        if (m_torrent->isPadFile(i)) continue;
        ++real;
        BundleFile b;
        b.path = sn.files.at(i).path;
        b.size = sn.files.at(i).size;
        b.index = i;
        files << b;
    }
    bool changedRec = false;
    if (real > 1 && !m_rec.isBundle) { m_rec.isBundle = true; changedRec = true; }
    if (real == 1 && (m_rec.isBundle || QFileInfo(m_rec.name).suffix().toLower() != QFileInfo(files.first().path).suffix().toLower()
                      || (m_rec.size > 0 && files.first().size > 0 && m_rec.size != files.first().size))) {
        // [merge-1] an index flagged it as a folder ("Sintel 2010 720p mkv"): the metadata says one
        // file, so it is a plain file download (name and size of that file: the background search
        // by name and size can start, sources of other networks map onto it)
        m_rec.isBundle = false;
        m_rec.files.clear();
        const BundleFile &only = files.first();
        const QString base = QFileInfo(only.path).fileName();
        if (!base.isEmpty() && QFileInfo(m_rec.name).suffix().toLower() != QFileInfo(base).suffix().toLower()) m_rec.name = base;
        if (only.size > 0) m_rec.size = only.size;
        qCInfo(lcEngine) << m_rec.id << "the torrent holds one file:" << m_rec.name << m_rec.size << "bytes";
        changedRec = true;
    }
    if (m_rec.isBundle && m_rec.files.size() != files.size()) { m_rec.files = files; changedRec = true; }
    if (!sn.name.isEmpty() && (m_rec.name.isEmpty() || m_rec.name.size() == 40 || m_rec.isBundle) && m_rec.name != sn.name) {
        m_rec.name = sn.name;
        changedRec = true;
    }
    if (m_rec.size <= 0 || m_rec.isBundle) {
        const qint64 sz = sn.totalSize > 0 ? sn.totalSize : m_torrent->payloadSize();
        if (sz != m_rec.size) { m_rec.size = sz; changedRec = true; }
    }
    if (m_rec.savePath.isEmpty()) {
        m_rec.savePath = QDir(sn.savePath).filePath(real == 1 && !sn.files.isEmpty() ? sn.files.first().path : sn.name);
        changedRec = true;
    }
    mapSourcesToFiles();
    if (changedRec) persist(true);
    applyTorrentOrder();   // [dl-1] the band pieces are known now
    openMoreSources();
    emit needsSchedule();
    markDirty(true);
}

void Download::onTorrentChanged()
{
    if (!m_torrent) return;
    const bool had = m_torrent->hasMetadata();
    m_torrent->refresh();
    if (!had && m_torrent->hasMetadata()) onTorrentMetadata();
    const TorrentSnapshot &sn = m_torrent->snap();
    if (m_btBytesAtStart < 0 && sn.hasMetadata) m_btBytesAtStart = sn.totalDone;
    if (sn.rateDownBps > 0) m_lastDataMs = nowMs();
    markDirty();
    if (m_verify.running && m_verify.torrent) pollTorrentVerify();   // [ver-1]
    if (sn.finished) QTimer::singleShot(0, this, [this] { maybeComplete(); });
}

void Download::onTorrentPieceFinished(int piece)
{
    if (m_torrent) m_torrent->onPieceFinished(piece);
}

void Download::onTorrentPieceFailed(int piece)
{
    if (m_torrent) m_torrent->onPieceFailed(piece);
}

void Download::onTorrentFinished()
{
    if (!m_torrent) return;
    m_torrent->refresh();
    QTimer::singleShot(0, this, [this] { maybeComplete(); });
}

void Download::onTorrentError(const QString &text)
{
    qCWarning(lcEngine) << m_rec.id << "torrent error:" << text;
    m_rec.errorText = text.left(300);
    markDirty(true);
}

// [ui-j] the torrent of a download removed earlier left the backend after this download took the same
// torrent up again: add it anew (nothing is wrong with this download)
void Download::torrentReAdd()
{
    if (m_torrent) {
        disconnect(m_torrent.get(), nullptr, this, nullptr);
        m_torrent.reset();
    }
    qCInfo(lcEngine) << m_rec.id << "the torrent of the earlier removal is gone: adding it again";
    if (m_active) {
        ensureStore();
        emit needsSchedule();
    }
    markDirty(true);
}

void Download::onTorrentRemoved()
{
    if (!m_torrent) return;
    qCWarning(lcEngine) << m_rec.id << "torrent removed from the backend behind our back";
    m_torrent.reset();
    if (!isFinished()) setError(QStringLiteral("Torrent removed"), true);
}

void Download::onUrlTorrentAdded(const QString &url, const QString &id)
{
    if (!waitsForUrl(url)) return;
    if (!m_promoteUrl.isEmpty() && url == m_promoteUrl) {   // [merge-1] the .torrent of a candidate torrent
        m_promoteUrl.clear();
        TorrentBackend *be = m_host->backend();
        if (!be) return;
        if (id != hexId(m_promoteExpect)) {
            qCInfo(lcEngine) << m_rec.id << "promotion: the .torrent is another torrent (" << id.left(8) << "), trying its magnet";
            be->removeTorrent(id, false);
            m_btTorrentUrls.remove(m_promoteExpect);
            considerPromotion();
            return;
        }
        m_promoteTorrent = id;
        m_promoteFile = -1;
        be->setWantedFiles(id, {-1});
        qCInfo(lcEngine) << m_rec.id << "promotion: .torrent of" << id.left(8) << "added";
        if (be->snapshot(id).hasMetadata) tryPromote();
        return;
    }
    m_pendingTorrentUrl.clear();
    if (!m_rec.hashes.btih.isEmpty() && id != hexId(m_rec.hashes.btih)) {
        qCWarning(lcEngine) << m_rec.id << ".torrent from" << url << "is a different torrent (" << id.left(8) << "), using the magnet";
        if (m_host->backend()) m_host->backend()->removeTorrent(id, false);
        QString err;
        if (!startTorrent(&err) && !err.isEmpty()) setError(err, false);
        return;
    }
    m_torrentId = id;
    QString err;
    if (!startTorrent(&err)) setError(err.isEmpty() ? QStringLiteral("cannot add torrent") : err, false);
    if (m_active) {
        openMoreSources();
        emit needsSchedule();
    }
}

void Download::onUrlTorrentFailed(const QString &url, const QString &error)
{
    if (!waitsForUrl(url)) return;
    if (!m_promoteUrl.isEmpty() && url == m_promoteUrl) {   // [merge-1] fall back to the magnet
        qCInfo(lcEngine) << m_rec.id << "promotion: .torrent fetch failed (" << error << "), using the magnet";
        m_promoteUrl.clear();
        m_btTorrentUrls.remove(m_promoteExpect);
        considerPromotion();
        return;
    }
    m_pendingTorrentUrl.clear();
    if (url == m_torrentUrlHint && !m_rec.hashes.btih.isEmpty()) {
        qCInfo(lcEngine) << m_rec.id << ".torrent fetch failed (" << error << "), using the magnet";
        QString err;
        if (!startTorrent(&err) && !err.isEmpty()) setError(err, false);
        return;
    }
    setError(QStringLiteral("Cannot fetch .torrent: %1").arg(error), false);
}

// ============================================================================ tick + status

void Download::tick(qint64 now)
{
    if (m_errorState && !(m_deadEnd && m_deadEndBanned) && !isFinished() && m_host->settings() && m_host->settings()->b(keys::transfers::AutoRetry)) {
        const qint64 interval = qint64(qMax(5, m_host->settings()->i(keys::transfers::RetryIntervalSec))) * 1000;
        const int maxAttempts = m_host->settings()->i(keys::transfers::RetryMaxAttempts);
        if (!m_errorAtMs) m_errorAtMs = now;
        if (now - m_errorAtMs >= interval && (maxAttempts <= 0 || m_retryAttempts < maxAttempts)) {
            ++m_retryAttempts;
            m_errorAtMs = 0;
            qCInfo(lcEngine) << m_rec.id << "auto-retry" << m_retryAttempts << "after:" << m_rec.errorText;
            retry();
            m_host->finished(this);   // → rebalance (activates it again)
            return;
        }
    } else {
        m_errorAtMs = 0;
    }
    if (m_torrent && m_active && !m_torrent->hasMetadata() && !m_metaStalled) {
        const int sec = m_host->settings() && m_host->settings()->value(QString(keys::transfers::MetadataTimeoutSec)).isValid()
                            ? m_host->settings()->i(keys::transfers::MetadataTimeoutSec) : 90;
        // [eng-1] a torrent nobody serves while the search knows a better one holding the same file
        // (the row "Big Buck Bunny.mp4" was one dead single-file torrent and a folder with 500 peers):
        // the part file and that candidate after a third of the time
        QByteArray better;
        const TorrentSnapshot &sn0 = m_torrent->snap();
        const bool betterKnown = sn0.peers + sn0.seeds == 0 && btCandidateReady(now, &better)
                                 && btRank(better, now) > btRank(m_rec.hashes.btih, now);
        const qint64 limitMs = qint64(qMax(1, sec)) * 1000 / (betterKnown ? 3 : 1);
        if (now - m_activatedMs > limitMs) {
            if (betterKnown)
                qCInfo(lcEngine) << m_rec.id << "torrent" << m_torrentId.left(8) << "has no peers after" << (now - m_activatedMs) / 1000
                                 << "s; the search knows" << btLabel(better) << "with a larger swarm";
            m_metaStalled = true;
            if (canFallBackToPart()) {
                fallBackToPart();
                return;
            }
            qCInfo(lcEngine) << m_rec.id << "no torrent metadata after" << sec << "s and nothing else to try, still looking";
            markDirty(true);
        }
    }
    if (m_torrent && m_active && m_host->backend()) {
        // [engine-4] a torrent nobody serves is announced again (DHT + trackers): every 2 min for the
        // first 10 min, then every Auto-Find-Sources interval (libtorrent alone waits up to 30 min)
        const TorrentSnapshot &sn0 = m_torrent->snap();
        if (sn0.peers + sn0.seeds == 0 && !sn0.finished) {
            const int afs = effectiveAfs();
            const qint64 every = now - m_activatedMs < 600000 ? 120000 : qint64(afs > 0 ? afs : 30) * m_host->tuning().afsMinuteMs;
            if (now - qMax(m_lastReannounceMs, m_activatedMs) >= every) {
                m_lastReannounceMs = now;
                m_host->backend()->reannounce(m_torrentId);
                qCDebug(lcEngine) << m_rec.id << "no peers: announcing the torrent again";
            }
        }
    }
    if (m_verify.running && m_verify.torrent && m_torrent) pollTorrentVerify();   // [ver-1]
    if (m_repairWhole && m_awaitSetsUntil && now > m_awaitSetsUntil) {   // [ver-1] no block hashes came
        giveUpRepair(QStringLiteral("no network gave block hashes to locate the damage"));
        return;
    }
    if (m_verifyNoteUntilMs && now > m_verifyNoteUntilMs) {
        m_verifyNoteUntilMs = 0;
        m_verifyNote.clear();
    }
    if (m_torrent) {
        m_torrent->refresh();
        if (m_active && m_torrent->hasMetadata()) {
            QHash<int, TorrentStore::Lane> lanes;
            for (const auto &s : m_slots) {
                if (s->fileIndex < 0 || !slotReady(s.get()) || !slotUsable(s.get())) continue;
                TorrentStore::Lane &l = lanes[s->fileIndex];
                l.file = s->fileIndex;
                ++l.readySources;
                l.rateBps += s->rate.rate(now, 10);
            }
            m_torrent->plan(lanes.values(), now);
        }
    }
    if (m_active) {
        checkTimeouts(now);
        replaceSlowSources(now);
        cancelRedundant();
        findSources(false);
        considerProvisional(now);   // [engine-4]
        openMoreSources();
        if (!m_promoteTorrent.isEmpty()) tryPromote();
        // [merge-1] a torrent hinted before the part file existed (or the next candidate torrent)
        else if (m_part && !m_promoting && (!m_hinted.btih.isEmpty() || !m_rec.hashes.btih.isEmpty())) considerPromotion();
        watchPromotion(now);   // [eng-1] no file list: announce again, the next candidate, set-aside ones back
        if (checkDeadEnd(now)) return;
        if (m_awaitHashSetUntil && now > m_awaitHashSetUntil) {
            m_awaitHashSetUntil = 0;
            wholeFileFailed({QStringLiteral("no hash set arrived")});
        }
        maybeComplete();
        emit needsSchedule();
    }
    // rate smoothing
    int raw = 0;
    for (const auto &s : m_slots) if (s->rs) raw += s->rate.rate(now, 5);
    if (m_torrent) raw += m_torrent->snap().rateDownBps;
    m_rateEma = m_lastRateSampleMs ? 0.6 * m_rateEma + 0.4 * raw : raw;
    m_lastRateSampleMs = now;
    updateStatus();
    if (m_dirtyBlob && now - m_lastPersistMs >= m_host->tuning().persistIntervalMs) persist(false);
}

void Download::setState(TransferState st, const QString &text)
{
    const bool changed = int(st) != m_rec.state || text != m_rec.statusText;
    m_rec.state = int(st);
    m_rec.statusText = text;
    if (changed) {
        if (m_host->dao()) m_host->dao()->updateProgress(m_rec.id, m_rec.state, m_rec.done, m_rec.statusText);
        markDirty(true);
    }
}

void Download::setError(const QString &text, bool pauseIt)
{
    m_rec.errorText = text;
    if (pauseIt) {
        m_userPaused = true;
        m_errorState = false;
    } else {
        m_errorState = true;
    }
    if (m_active) {
        m_active = false;
        closeAllSources();
        stopFindSources();
        if (m_torrent) {
            m_torrent->releaseAll();
            m_host->backend()->pauseTorrent(m_torrentId);
        }
    }
    updateStatus();
    persist(true);
    m_host->finished(this);   // frees the queue slot
}

void Download::updateStatus()
{
    if (isFinished() || m_completing) return;
    if (m_userPaused) {
        QString why = m_rec.errorText.isEmpty() ? m_verifyNote : m_rec.errorText;   // [ver-1] the check's outcome
        if (why.isEmpty() && m_repairWhole) why = QStringLiteral("damaged; block hashes are asked for when it runs");
        setState(TransferState::Paused, why.isEmpty() ? transferStateText(TransferState::Paused) : QStringLiteral("Paused (%1)").arg(why));
        return;
    }
    if (m_errorState) {
        setState(TransferState::Error, m_rec.errorText.isEmpty() ? transferStateText(TransferState::Error) : m_rec.errorText);
        return;
    }
    if (!m_active) {
        setState(TransferState::Queued, transferStateText(TransferState::Queued));
        return;
    }
    if (m_promoting) {
        setState(TransferState::Verifying, transferStateText(TransferState::Verifying));
        return;
    }
    if (m_awaitHashSetUntil) return;
    if (m_repairWhole && m_awaitSetsUntil) {   // [ver-1]
        setState(TransferState::Verifying, QStringLiteral("Damaged: asking the networks for block hashes to find the damaged parts..."));
        return;
    }
    if (!m_movingTo.isEmpty()) {   // [engine-3] torrent move_storage running
        setState(TransferState(m_rec.state), QStringLiteral("Moving..."));
        return;
    }
    const qint64 now = nowMs();
    bool transferring = false, queued = false, connecting = false;
    int bestRank = INT_MAX;
    for (const auto &s : m_slots) {
        if (!s->rs) continue;
        if (s->rate.lastActivityMs() && now - s->rate.lastActivityMs() < 5000) transferring = true;
        const RangeSource::State st = s->rs->state();
        if (st == RangeSource::Queued) {
            queued = true;
            const int r = s->rs->descriptor().extra.value(QStringLiteral("queueRank")).toInt();
            if (r > 0) bestRank = qMin(bestRank, r);
            else {
                static const QRegularExpression num(QStringLiteral("(\\d+)"));
                const auto mm = num.match(s->rs->stateText());
                if (mm.hasMatch()) bestRank = qMin(bestRank, mm.captured(1).toInt());
            }
        } else if (st == RangeSource::Connecting || st == RangeSource::Idle || st == RangeSource::Ready || st == RangeSource::Busy) {
            connecting = true;
        }
    }
    if (m_torrent) {
        const TorrentSnapshot &sn = m_torrent->snap();
        if (!m_torrent->hasMetadata()) {
            // [engine-4] say what we wait for: peers to ask for the torrent's file list
            const int conn = sn.peers + sn.seeds;
            QString what;
            if (conn > 0) what = QStringLiteral("getting the file list from %1 peer%2").arg(conn).arg(conn == 1 ? "" : "s");
            else what = QStringLiteral("looking for peers (DHT, trackers)");
            if (m_metaStalled && conn == 0)
                setState(TransferState::FindingSources, QStringLiteral("No sources found yet, still looking%1%2").arg(indexNote(), trackerNote()));
            else if (!m_indexName.isEmpty())   // [core-5] a hit from a torrent index: the original WinMX wording
                setState(TransferState::FindingSources, fetchingFromIndexText());
            else
                setState(TransferState::FindingSources, QStringLiteral("Finding sources... %1%2").arg(what, indexNote()));
            return;
        }
        if (sn.state == TransferState::Verifying) {
            setState(TransferState::Verifying, transferStateText(TransferState::Verifying));
            return;
        }
        if (sn.rateDownBps > 0 || transferring) {
            setState(TransferState::Transferring, transferStateText(TransferState::Transferring));
            return;
        }
        if (sn.peers + sn.seeds > 0 || connecting) {
            // [engine-4] peers connected but nobody sends: say so (a swarm of stuck leechers looks
            // like "Connecting..." forever otherwise)
            if (sn.seeds == 0 && sn.peers > 0 && !connecting && now - m_activatedMs > 60000)
                setState(TransferState::Connecting, QStringLiteral("Connected to %1 peer%2, none has the missing parts yet%3%4")
                                                        .arg(sn.peers).arg(sn.peers == 1 ? "" : "s").arg(indexNote(), trackerNote()));
            else
                setState(TransferState::Connecting, transferStateText(TransferState::Connecting));
            return;
        }
        if (queued) {
            setState(TransferState::RemotelyQueued, bestRank < INT_MAX ? QStringLiteral("Remotely Queued (%1)").arg(bestRank)
                                                                       : transferStateText(TransferState::RemotelyQueued));
            return;
        }
        // [engine-4] metadata known, nobody connected
        if (now - m_activatedMs > 180000)
            setState(TransferState::FindingSources, QStringLiteral("No peers found yet%1%2, asking again every few minutes").arg(indexNote(), trackerNote()));
        else
            setState(TransferState::FindingSources, QStringLiteral("Finding sources... looking for peers (DHT, trackers)%1").arg(indexNote()));
        return;
    }
    if (m_pendingTorrentUrl.size()) {
        setState(TransferState::FindingSources, fetchingFromIndexText());
        return;
    }
    if (transferring) {
        setState(TransferState::Transferring, transferStateText(TransferState::Transferring));
    } else if (queued) {
        setState(TransferState::RemotelyQueued, bestRank < INT_MAX ? QStringLiteral("Remotely Queued (%1)").arg(bestRank)
                                                                   : transferStateText(TransferState::RemotelyQueued));
    } else if (connecting) {
        setState(TransferState::Connecting, transferStateText(TransferState::Connecting));
    } else {
        const int timeout = m_host->settings() ? m_host->settings()->i(keys::transfers::PendingDownloadTimeoutSec) : 300;
        const int waiting = waitingUnverified();
        // [engine-4] what source discovery is doing right now, so a waiting download is not a mystery
        const QString disc = discoveryText(now);
        const bool btihOnly = !m_rec.hashes.btih.isEmpty() && m_rec.hashes.ed2k.isEmpty() && m_rec.hashes.sha1.isEmpty()
                              && m_rec.hashes.tth.isEmpty() && m_rec.hashes.sha256.isEmpty() && m_rec.hashes.md5.isEmpty();
        if (waiting > 0)
            setState(TransferState::FindingSources,
                     QStringLiteral("Finding sources... (%1 unverified source%2 wait for %3) %4").arg(waiting).arg(waiting == 1 ? "" : "s")
                         .arg(btihOnly ? QStringLiteral("the torrent's file list") : QStringLiteral("a part hash set"), disc).trimmed());
        else if (now - qMax(m_lastDataMs, m_activatedMs) > qint64(timeout) * 1000 && openSources() == 0)
            setState(TransferState::TimedOut, QStringLiteral("Timed Out (Waiting for Network Reply) %1").arg(disc).trimmed());
        else
            setState(TransferState::FindingSources, QStringLiteral("%1 %2").arg(transferStateText(TransferState::FindingSources), disc).trimmed());
    }
}

bool Download::checkDeadEnd(qint64 now)
{
    // Every source we ever had is banned (fake / corrupt) or failed over and over, and nothing came
    // in for a while: stop with a clear error instead of retrying forever (manual Retry or new
    // sources revive it). BitTorrent swarms are libtorrent's business and never count as dead.
    if (m_torrent || !m_promoteTorrent.isEmpty() || m_slots.empty() || isFinished()) return false;
    int banned = 0, dead = 0, considered = 0;
    for (const auto &s : m_slots) {
        if (s->rs) return false;
        if (s->removedByUser) continue;   // [glue-2] the user dropped it: neither good nor bad
        ++considered;
        if (s->banned) ++banned;
        else if (s->failures >= 5) ++dead;
        else return false;
    }
    if (considered == 0) return false;
    const bool allBanned = banned == considered;
    if (!allBanned && now - qMax(m_lastDataMs, m_activatedMs) < m_host->tuning().deadEndMs) return false;
    // [set-1] only a download whose every source sent the wrong file stays stopped; one whose sources
    // all failed is tried again by "Automatically retry failed downloads" too (sources come back
    // online), so Retry every / Retry attempts mean something (they applied to torrent errors only)
    m_deadEnd = true;            // new sources revive it
    m_deadEndBanned = allBanned;
    qCWarning(lcEngine) << m_rec.id << "giving up:" << banned << "sources banned," << dead << "failed repeatedly";
    setError(allBanned ? QStringLiteral("File Mismatch: every source sent bad data or the wrong file")
                       : QStringLiteral("File/User Unavailable: every source failed"), false);
    return true;
}

int Download::waitingUnverified() const
{
    // untrusted (candidate / hint-only) sources that cannot be used because nothing could verify
    // their data before the whole file is there (no trusted part hashes / TTH leaves yet)
    if (!m_part || m_part->scheme() != PartStore::NoUnits) return 0;
    int n = 0;
    for (const auto &s : m_slots) if (!s->trusted && !s->banned && !provisionalUsable(s.get())) ++n;
    return n;
}

qint64 Download::bufferedBytes() const
{
    const Store *s = store();
    return s ? s->bufferedBytes() : 0;
}

TransferInfo Download::info() const
{
    TransferInfo t;
    const qint64 now = nowMs();
    t.id = m_rec.id;
    t.name = m_rec.name;
    t.savePath = m_rec.savePath;
    if (t.savePath.isEmpty() || (!isFinished() && m_part)) t.savePath = isFinished() ? m_rec.savePath : (m_part ? m_part->path() : m_rec.savePath);
    t.size = m_rec.size;
    t.done = m_rec.done;
    t.state = TransferState(m_rec.state);
    t.statusText = m_rec.statusText.isEmpty() ? transferStateText(t.state) : m_rec.statusText;
    t.errorText = m_rec.errorText;
    t.hashes = m_rec.hashes;
    t.isBundle = m_rec.isBundle;
    t.files = m_rec.files;
    t.priority = m_rec.priority;
    t.added = m_rec.added;
    t.completed = m_rec.completed;
    // [engine-4] every known source is a row (truthful state, original WinMX wording), not only open sessions
    QSet<int> nets;
    const bool showRows = !isFinished();
    int active = 0;
    for (const auto &s : m_slots) if (!s->banned) ++t.sourcesTotal;
    if (showRows) sourceRows(t.sources, nets, &active, now);
    else for (const auto &s : m_slots) if (s->rs && s->rate.lastActivityMs() && now - s->rate.lastActivityMs() < 5000) ++active;
    t.sourcesActive = active;
    if (m_torrent) {
        const TorrentSnapshot &sn = m_torrent->snap();
        if (m_torrent->hasMetadata() && !isFinished()) {
            t.size = sn.totalSize;
            t.done = sn.totalDone;
        }
        t.sourcesTotal += qMax(sn.peers + sn.seeds, qMax(0, sn.peersTotal) + qMax(0, sn.seedsTotal));
        int peersActive = 0;
        if (showRows) {
            torrentRows(t.sources, nets, &peersActive);
        } else {
            for (const TransferSource &p : sn.sources) if (p.rateBps > 0) ++peersActive;
        }
        t.sourcesActive += peersActive;
        if (sn.rateDownBps > 0 || peersActive > 0) nets.insert(int(Net::BitTorrent));
        if (m_torrent->hasMetadata()) {
            QList<BundleFile> files;
            for (int i = 0; i < sn.files.size(); ++i) {
                if (m_torrent->isPadFile(i)) continue;
                BundleFile b;
                b.path = sn.files.at(i).path;
                b.size = sn.files.at(i).size;
                b.index = i;
                b.done = sn.files.at(i).done;
                b.wanted = sn.files.at(i).priority > 0;
                files << b;
            }
            if (m_rec.isBundle) t.files = files;
        }
        t.rateUpBps = sn.rateUpBps;
    }
    if (m_part && !isFinished()) {
        t.done = (m_part->scheme() == PartStore::NoUnits) ? m_part->heldBytes() : m_part->verifiedBytes();
        if (m_part->extent() < 0) t.size = 0;
        // [eng-1] a torrent that holds this file is known: BitTorrent has its line from that moment on
        if (showRows && btCandidateKnown()) {
            TransferSource r;
            r.net = r.user.net = Net::BitTorrent;
            QByteArray cur = hash::fromHex(m_promoteTorrent);
            if (cur.isEmpty() && !m_promoteExpect.isEmpty() && !m_promoteUrl.isEmpty()) cur = m_promoteExpect;
            if (cur.isEmpty()) cur = !m_hinted.btih.isEmpty() ? m_hinted.btih : m_rec.hashes.btih;
            const QString n = m_btNames.value(cur);
            r.user.name = cur.isEmpty() ? QStringLiteral("BitTorrent") : n.isEmpty() ? QStringLiteral("Torrent %1").arg(hexId(cur).left(8)) : QStringLiteral("Torrent \"%1\"").arg(n);
            // no connection text: the Network column says "BT", not "BT BitTorrent"
            r.statusText = btPartStatus(now);
            t.sources.prepend(r);
        }
    }
    if (isFinished() && t.size > 0) t.done = t.size;
    if (showRows && m_active) discoveryRows(t.sources);
    t.autoFindSourcesMin = effectiveAfs();   // [engine-4]
    t.autoEnterQueue = effectiveAeq();
    t.sequential = m_seq;                // [dl-1]
    t.firstLastFirst = m_firstLast;
    t.orderFollowedBy = orderFollowers();
    // [core-5] the Network column of the parent row: the networks feeding the download now plus
    // every network it has a usable (not banned) source on, also while nothing is transferring
    if (!isFinished()) {
        for (const auto &s : m_slots)
            if (!s->banned && !s->removedByUser && s->desc.net != Net::Unknown) nets.insert(int(s->desc.net));
        if (m_torrent || !m_pendingTorrentUrl.isEmpty()) nets.insert(int(Net::BitTorrent));
        if (m_part && btCandidateKnown()) nets.insert(int(Net::BitTorrent));   // [eng-1] a candidate already
    }
    nets.remove(int(Net::Unknown));
    for (int n : nets) t.networks << Net(n);
    std::sort(t.networks.begin(), t.networks.end(), [](Net a, Net b) { return int(a) < int(b); });
    t.rateDownBps = (m_active && !isFinished()) ? int(m_rateEma) : 0;
    if (t.rateDownBps > 0 && t.size > t.done) t.etaSec = int(qMin<qint64>((t.size - t.done) / qMax(1, t.rateDownBps), 99 * 3600));
    if (!isFinished()) pieceMapOf(&t.pieceMap, &t.pieceFill);   // [ui-i]
    if (!isFinished()) {   // [diag-1]
        t.diagnosis = m_diagnosis;
        t.diagnosisLevel = m_diagnosisLevel;
    }
    if (!m_verify.running && !m_verifyNote.isEmpty() && m_verifyNoteUntilMs > now && !isFinished() && !m_userPaused)
        t.statusText += QStringLiteral(" (verified: %1)").arg(m_verifyNote);   // [ver-1] the check's outcome, a moment
    if (m_verify.running) {   // [ver-1] Verify File: the state of the row while it is checked
        t.state = TransferState::Verifying;
        t.statusText = verifyStatusText();
        t.rateDownBps = 0;
        t.etaSec = -1;
    }
    if (m_checkFirst && !isFinished())   // [safe-1]
        t.statusText = m_verify.running ? t.statusText + QStringLiteral(" (OctoMX was not closed properly)")
                                        : (m_userPaused ? QStringLiteral("Paused, waiting to be checked (OctoMX was not closed properly)")
                                                        : QStringLiteral("Waiting to be checked (OctoMX was not closed properly)"));
    return t;
}

// [ui-i] the map of the file for the Transfers bar. In progress = the ranges requested from open
// sources right now (and what the store receives / checks now); a restored download whose store does
// not exist yet shows what its stored state says it has.
void Download::pieceMapOf(QByteArray *state, QByteArray *fill) const
{
    state->clear();
    fill->clear();
    if (m_torrent && !m_active) {
        // paused / queued by us: libtorrent may not have reported the pause yet, nothing arrives
        TorrentSnapshot sn = m_torrent->snap();
        sn.downloading.clear();
        TorrentStore::mapOf(sn, {}, 512, state, fill);
        return;
    }
    if (const Store *st = store()) {
        RangeSet inFlight;
        for (const auto &s : m_slots) {
            if (!s->rs) continue;
            for (const Request &q : s->reqs) inFlight.add(q.off, q.end());
        }
        st->pieceMap(512, inFlight, state, fill);
        return;
    }
    if (!m_torrentId.isEmpty()) {
        TorrentBackend *be = m_host->backend();
        if (be && be->torrents().contains(m_torrentId)) {
            TorrentSnapshot sn = be->snapshot(m_torrentId);
            sn.downloading.clear();   // not running in this session: nothing is arriving
            TorrentStore::mapOf(sn, {}, 512, state, fill);
        }
        return;
    }
    if (m_rec.size <= 0 || !m_pendingTorrentUrl.isEmpty()) return;
    // the same choice as PartStore: with fine units only verified bytes count, else the held ones
    const bool units = !m_ed2kParts.isEmpty() || (!m_tthLeaves.isEmpty() && m_tthUnit > 0)
                       || (!m_rec.hashes.ed2k.isEmpty() && m_rec.size < PartStore::Ed2kPart);
    RangeSet complete = m_restoreVerified;
    if (!units) complete.add(m_restoreHeld);
    PartStore::mapOf(512, m_rec.size, complete, state, fill);
}

// [diag-1] Connection Test counters (cheap: walks the slots once)
diag::DownloadFacts Download::diagFacts() const
{
    diag::DownloadFacts f;
    const qint64 now = nowMs();
    f.id = m_rec.id;
    f.name = m_rec.name;
    f.state = TransferState(m_rec.state);
    f.active = m_active;
    f.paused = m_userPaused || f.state == TransferState::Paused;
    f.finished = isFinished();
    f.error = hasError();
    f.isTorrent = m_torrent != nullptr;
    f.torrentId = m_torrentId;
    f.fetchingTorrentFile = !m_pendingTorrentUrl.isEmpty();
    f.indexSeeds = m_indexSeeds;
    f.indexPeers = m_indexPeers;
    f.indexName = m_indexName;
    f.size = m_rec.size;
    f.done = m_rec.done;
    f.rateBps = m_active ? int(m_rateEma) : 0;
    f.activeSec = m_active && m_activatedMs > 0 ? (now - m_activatedMs) / 1000 : 0;
    for (const auto &sp : m_slots) {
        const SourceSlot *s = sp.get();
        if (s->isTorrentLink || s->removedByUser) continue;
        if (s->banned) {
            ++f.sourcesBanned;
            continue;
        }
        ++f.sourcesKnown;
        f.knownByNet[int(s->desc.net)] += 1;
        if (s->rs) {
            const RangeSource::State st = s->rs->state();
            if (st == RangeSource::Queued) {
                ++f.sourcesQueued;
                if (s->queueRank > 0 && (f.bestQueueRank == 0 || s->queueRank < f.bestQueueRank)) f.bestQueueRank = s->queueRank;
            }
            if (st != RangeSource::Failed && st != RangeSource::Closed) ++f.sourcesOpen;
            if (s->rate.lastActivityMs() && now - s->rate.lastActivityMs() < 5000) ++f.sourcesTransferring;
        } else if (s->failures > 0 || !s->lastError.isEmpty()) {
            ++f.sourcesFailed;
            if (!s->lastError.isEmpty()) f.failReasons[s->lastError.left(80)] += 1;
        }
    }
    return f;
}

bool Download::setDiagnosis(const QString &text, int level)
{
    if (text == m_diagnosis && level == m_diagnosisLevel) return false;
    m_diagnosis = text;
    m_diagnosisLevel = level;
    return true;
}

TransferInfo Download::briefInfo() const
{
    TransferInfo t;
    t.id = m_rec.id;
    t.name = m_rec.name;
    t.size = m_rec.size;
    t.done = m_rec.done;
    t.state = TransferState(m_rec.state);
    t.statusText = m_rec.statusText;
    t.hashes = m_rec.hashes;
    t.isBundle = m_rec.isBundle;
    t.priority = m_rec.priority;
    t.savePath = m_rec.savePath;
    t.sequential = m_seq;                // [dl-1]
    t.firstLastFirst = m_firstLast;
    if (m_verify.running) {              // [ver-1]
        t.state = TransferState::Verifying;
        t.statusText = verifyStatusText();
    }
    return t;
}

QVariantMap Download::debugInfo() const
{
    QVariantMap m;
    m.insert(QStringLiteral("store"), m_torrent ? QStringLiteral("torrent") : m_part ? QStringLiteral("part") : QStringLiteral("none"));
    m.insert(QStringLiteral("torrentId"), m_torrentId);
    m.insert(QStringLiteral("active"), m_active);
    m.insert(QStringLiteral("checkFirst"), m_checkFirst);   // [safe-1]
    if (m_part) {
        m.insert(QStringLiteral("syncedBytes"), m_part->synced().total());
        m.insert(QStringLiteral("writtenBytes"), m_part->written().total());
        m.insert(QStringLiteral("syncs"), m_part->syncCount());
        m.insert(QStringLiteral("verifyingUnits"), m_part->verifyingCount());
    }
    QVariantMap byNet;
    for (auto it = m_verifiedByNet.constBegin(); it != m_verifiedByNet.constEnd(); ++it) byNet.insert(netName(Net(it.key())), it.value());
    QVariantMap recvNet;
    for (const auto &s : m_slots) {
        const QString n = netName(s->desc.net);
        recvNet[n] = recvNet.value(n).toLongLong() + s->bytes;
    }
    m.insert(QStringLiteral("verifiedByNet"), byNet);
    m.insert(QStringLiteral("receivedByNet"), recvNet);
    if (m_torrent) {
        m.insert(QStringLiteral("injectedPieces"), m_torrent->injectedCount());
        m.insert(QStringLiteral("injectedBytes"), m_torrent->injectedBytes());
        m.insert(QStringLiteral("reservedPieces"), m_torrent->reservedCount());
        m.insert(QStringLiteral("piecePreverifyFailures"), m_torrent->pieceFailures());
        m.insert(QStringLiteral("totalDone"), m_torrent->snap().totalDone);
        m.insert(QStringLiteral("btPeers"), m_torrent->snap().peers + m_torrent->snap().seeds);
        m.insert(QStringLiteral("btBytesEstimate"), qMax<qint64>(0, m_torrent->snap().totalDone - m_torrent->injectedBytes()
                                                                          - qMax<qint64>(0, m_btBytesAtStart)));
    }
    if (m_part) {
        m.insert(QStringLiteral("held"), m_part->heldBytes());
        m.insert(QStringLiteral("verified"), m_part->verifiedBytes());
        m.insert(QStringLiteral("scheme"), int(m_part->scheme()));
        m.insert(QStringLiteral("hashSetProvider"), m_ed2kSetProvider);
        m.insert(QStringLiteral("hashSetProvisional"), m_ed2kProvisional);
        m.insert(QStringLiteral("partPath"), m_part->path());
    }
    int banned = 0, strikes = 0;
    QVariantList srcList;
    for (const auto &s : m_slots) {
        if (s->banned) ++banned;
        strikes += s->strikes;
        QVariantMap sm;
        sm.insert(QStringLiteral("key"), s->key);
        sm.insert(QStringLiteral("bytes"), s->bytes);
        sm.insert(QStringLiteral("verified"), s->verifiedBytes);
        sm.insert(QStringLiteral("strikes"), s->strikes);
        sm.insert(QStringLiteral("banned"), s->banned);
        sm.insert(QStringLiteral("trusted"), s->trusted);
        sm.insert(QStringLiteral("open"), s->isOpen());
        sm.insert(QStringLiteral("file"), s->fileIndex);
        sm.insert(QStringLiteral("candidate"), s->wasCandidate);
        sm.insert(QStringLiteral("net"), netName(s->desc.net));
        if (!s->lastError.isEmpty()) sm.insert(QStringLiteral("why"), s->lastError);
        srcList << sm;
    }
    m.insert(QStringLiteral("sources"), srcList);
    m.insert(QStringLiteral("banned"), banned);
    m.insert(QStringLiteral("strikes"), strikes);
    m.insert(QStringLiteral("singleSource"), m_singleSource);
    m.insert(QStringLiteral("wholeFileFailures"), m_wholeFileFailures);
    m.insert(QStringLiteral("unitFailures"), m_unitFailures);
    if (!m_movingTo.isEmpty()) m.insert(QStringLiteral("movingTo"), m_movingTo);
    if (!m_moveError.isEmpty()) m.insert(QStringLiteral("moveError"), m_moveError);
    m.insert(QStringLiteral("saveDir"), m_rec.saveDir);
    m.insert(QStringLiteral("failedUnitBytes"), m_failedUnitBytes);
    m.insert(QStringLiteral("hinted"), m_hinted.toVariant());
    // [merge-1] the judge and the networks matched only by name and size
    m.insert(QStringLiteral("judge"), judgeText());
    QVariantMap cn;
    for (auto it = m_candNets.constBegin(); it != m_candNets.constEnd(); ++it) {
        QVariantMap c;
        c.insert(QStringLiteral("failed"), it->failed);
        c.insert(QStringLiteral("confirmed"), it->confirmed);
        c.insert(QStringLiteral("dropped"), it->dropped);
        c.insert(QStringLiteral("fetched"), it->fetched);
        cn.insert(netName(Net(it.key())), c);
    }
    m.insert(QStringLiteral("candNets"), cn);
    if (!m_completionNote.isEmpty()) m.insert(QStringLiteral("completionNote"), m_completionNote);
    m.insert(QStringLiteral("torrentCandidates"), int(m_btCandidates.size()));
    // [eng-1] the torrent whose metadata is fetched, the ranked candidates and the BitTorrent row
    m.insert(QStringLiteral("promoteTorrent"), m_promoteTorrent);
    m.insert(QStringLiteral("hintedBtih"), hexId(m_hinted.btih));
    QStringList cands;
    for (const QByteArray &c : m_btCandidates) cands << hexId(c);
    m.insert(QStringLiteral("btCandidateList"), cands);
    if (m_part) m.insert(QStringLiteral("btPartStatus"), btCandidateKnown() ? btPartStatus(nowMs()) : QString());
    if (m_torrent) m.insert(QStringLiteral("btSwarmStatus"), btSwarmStatus());
    // [dl-1] the block map in store coordinates ([[start, end], ...]: held bytes / complete pieces)
    m.insert(QStringLiteral("sequential"), m_seq);
    m.insert(QStringLiteral("firstLast"), m_firstLast);
    if (m_part) {
        m.insert(QStringLiteral("heldRanges"), m_part->held().toVariant());
    } else if (m_torrent && m_torrent->hasMetadata()) {
        const TorrentSnapshot &sn = m_torrent->snap();
        RangeSet have;
        for (int p = 0; p < sn.have.size(); ++p)
            if (sn.have.testBit(p)) have.add(qint64(p) * sn.pieceLength, qMin(m_torrent->payloadSize(), qint64(p + 1) * sn.pieceLength));
        m.insert(QStringLiteral("heldRanges"), have.toVariant());
        m.insert(QStringLiteral("orderBackend"), m_orderBackendOk);
    }
    // [ui-i] what the map of the Transfers bar is made of (tests and mxcli maplog compare it with the disk)
    {
        RangeSet inFlight;
        for (const auto &s : m_slots) {
            if (!s->rs) continue;
            for (const Request &q : s->reqs) inFlight.add(q.off, q.end());
        }
        m.insert(QStringLiteral("inFlightRanges"), inFlight.toVariant());
        if (m_part) {
            m.insert(QStringLiteral("verifiedRanges"), m_part->verified().toVariant());
            m.insert(QStringLiteral("partSize"), m_part->size());
        } else if (m_torrent && m_torrent->hasMetadata()) {
            const TorrentSnapshot &sn = m_torrent->snap();
            m.insert(QStringLiteral("pieceLength"), sn.pieceLength);
            QVariantList dl, files;
            for (int p = 0; p < sn.downloading.size(); ++p)
                if (sn.downloading.testBit(p)) dl << p;
            m.insert(QStringLiteral("downloadingPieces"), dl);
            QVariantList moving;
            for (int p : m_torrent->reservedPieces()) moving << p;
            m.insert(QStringLiteral("reservedPieceList"), moving);
            for (int i = 0; i < sn.files.size(); ++i) {
                const TorrentFile &f = sn.files.at(i);
                files << QVariantMap{{QStringLiteral("path"), f.path}, {QStringLiteral("offset"), f.offset},
                                     {QStringLiteral("size"), f.size}, {QStringLiteral("priority"), f.priority}};
            }
            m.insert(QStringLiteral("files"), files);
            m.insert(QStringLiteral("savePath"), sn.savePath);
        } else if (!m_torrent && !m_part && !m_torrentId.isEmpty() && m_host->backend()
                   && m_host->backend()->torrents().contains(m_torrentId)) {
            // restored, not resumed: the map comes from the backend's snapshot (pieceMapOf)
            const TorrentSnapshot sn = m_host->backend()->snapshot(m_torrentId);
            m.insert(QStringLiteral("mapFrom"), QStringLiteral("snapshot"));
            m.insert(QStringLiteral("pieceLength"), sn.pieceLength);
            RangeSet have;
            qint64 end = 0;
            for (const TorrentFile &f : sn.files) end = qMax(end, f.offset + f.size);
            for (int p = 0; p < sn.have.size(); ++p)
                if (sn.have.testBit(p)) have.add(qint64(p) * sn.pieceLength, qMin(end, qint64(p + 1) * sn.pieceLength));
            m.insert(QStringLiteral("heldRanges"), have.toVariant());
            QVariantList files;
            for (const TorrentFile &f : sn.files)
                files << QVariantMap{{QStringLiteral("path"), f.path}, {QStringLiteral("offset"), f.offset},
                                     {QStringLiteral("size"), f.size}, {QStringLiteral("priority"), f.priority}};
            m.insert(QStringLiteral("files"), files);
            m.insert(QStringLiteral("savePath"), sn.savePath);
        } else if (!m_torrent && !m_part && m_torrentId.isEmpty()) {
            // restored part file, not resumed: the map comes from the stored ranges (pieceMapOf)
            const bool units = !m_ed2kParts.isEmpty() || (!m_tthLeaves.isEmpty() && m_tthUnit > 0)
                               || (!m_rec.hashes.ed2k.isEmpty() && m_rec.size < PartStore::Ed2kPart);
            m.insert(QStringLiteral("mapFrom"), QStringLiteral("stored"));
            m.insert(QStringLiteral("scheme"), units ? 1 : 0);
            m.insert(QStringLiteral("verifiedRanges"), m_restoreVerified.toVariant());
            RangeSet held = m_restoreHeld;
            held.add(m_restoreVerified);
            m.insert(QStringLiteral("heldRanges"), held.toVariant());
            m.insert(QStringLiteral("partSize"), m_rec.size);
            m.insert(QStringLiteral("partPath"), partPath());
        }
    }
    return m;
}

// ============================================================================ [glue-2] user actions

void Download::findMoreSources()
{
    if (isFinished()) return;
    m_findRound = 0;                       // back to the short re-ask interval
    if (!m_active) return;                 // Locally Queued / paused: asked when it starts
    findSources(true);
    lookupHashLinks();
    if (m_torrent) m_host->backend()->reannounce(m_torrentId);
    markDirty(true);
}

void Download::enterQueue()
{
    if (isFinished()) return;
    if (m_errorState || m_deadEnd) {
        retry();                           // revives errored downloads (and reopens everything)
        m_host->finished(this);            // → rebalance activates it
        return;
    }
    m_queueOverride = true;                // [engine-4] the user wants to wait in every remote queue
    for (auto &s : m_slots) {
        if (s->banned) continue;
        s->retryAtMs = 0;
        s->failures = 0;
        s->holdUntilMs = 0;
        s->aeqParked = false;
    }
    if (m_active) {
        openMoreSources();
        emit needsSchedule();
    }
    updateStatus();
    markDirty(true);
}

bool Download::removeSource(const QString &key)
{
    SourceSlot *s = slotFor(key);
    if (!s || s->removedByUser) return false;
    s->removedByUser = true;
    m_userRemoved.insert(key);
    qCInfo(lcEngine) << m_rec.id << "source removed by the user:" << key;
    ban(s, QStringLiteral("removed by the user"));
    m_dirtyBlob = true;
    markDirty(true);
    return true;
}

bool Download::setSourcePriority(const QString &key, int priority)
{
    SourceSlot *s = slotFor(key);
    priority = qBound(-1, priority, 1);
    if (!s) return false;
    s->priority = priority;
    if (priority == 0) m_srcPriority.remove(key);
    else m_srcPriority.insert(key, priority);
    if (priority > 0 && !s->rs && m_active) {
        s->retryAtMs = 0;
        s->holdUntilMs = 0;
        openMoreSources();
    }
    emit needsSchedule();
    m_dirtyBlob = true;
    markDirty(true);
    return true;
}

void Download::onTorrentStorageMoved(const QString &path, bool ok, const QString &error)
{
    if (m_movingTo.isEmpty() && ok && QDir::cleanPath(path) == QDir::cleanPath(m_rec.saveDir)) return;
    const QString wanted = m_movingTo;
    m_movingTo.clear();
    if (ok) {
        m_rec.saveDir = QDir::cleanPath(path.isEmpty() ? wanted : path);
        m_moveError.clear();
        qCInfo(lcEngine) << m_rec.id << "torrent storage moved to" << m_rec.saveDir;
    } else {
        m_moveError = error.isEmpty() ? QStringLiteral("move failed") : error;
        qCWarning(lcEngine) << m_rec.id << "moving the torrent to" << wanted << "failed:" << m_moveError;
    }
    persist(true);
    if (m_torrent) onTorrentChanged();
    updateStatus();
    markDirty(true);
}

bool Download::setTarget(const QString &dir, const QString &name)
{
    if (!m_repairPath.isEmpty() || m_verify.running) return false;   // [ver-1] repaired where it is
    if (isFinished() || m_completing) return false;
    if (m_torrent) {
        // [engine-3] libtorrent owns the files: move them with move_storage (async; the
        // directory changes when the backend confirms). A torrent keeps its own file names.
        const QString cur = m_torrent->snap().name;
        if (!name.trimmed().isEmpty() && name.trimmed() != cur && safeFileName(name.trimmed()) != cur) return false;
        if (dir.trimmed().isEmpty() || !m_movingTo.isEmpty()) return false;
        const QString d = QDir::cleanPath(QDir(dir.trimmed()).absolutePath());
        const QString now = m_torrent->snap().savePath.isEmpty() ? m_rec.saveDir : m_torrent->snap().savePath;
        if (!now.isEmpty() && QDir::cleanPath(QDir(now).absolutePath()) == d) return true;
        TorrentBackend *be = m_host->backend();
        if (!be || !be->moveStorage(m_torrentId, d)) return false;
        m_movingTo = d;
        m_moveError.clear();
        qCInfo(lcEngine) << m_rec.id << "moving torrent storage to" << d;
        setState(TransferState(m_rec.state), QStringLiteral("Moving..."));
        markDirty(true);
        return true;
    }
    const QString n = safeFileName(name.trimmed());
    if (dir.trimmed().isEmpty() && n.isEmpty()) return false;
    if (!dir.trimmed().isEmpty()) m_rec.saveDir = QDir::cleanPath(dir.trimmed());
    if (!n.isEmpty()) m_rec.name = n;
    qCInfo(lcEngine) << m_rec.id << "new target:" << m_rec.saveDir << m_rec.name;
    persist(true);
    markDirty(true);
    return true;
}

QByteArray Download::stateForExistingFile(const QString &path, qint64 held)
{
    QVariantMap m;
    m.insert(QStringLiteral("v"), 1);
    m.insert(QStringLiteral("store"), QStringLiteral("part"));
    RangeSet h;
    if (held > 0) h.add(0, held);
    m.insert(QStringLiteral("held"), h.toVariant());
    m.insert(QStringLiteral("verified"), QVariantList());
    m.insert(QStringLiteral("partPath"), path);
    return QCborValue::fromVariant(m).toCbor();
}

// ============================================================================ [engine-4]

// [engine-4] completion evidence: verified bytes per network ("eD2k 41% · Gnutella 59%")
void Download::logShares() const
{
    QHash<int, qint64> by = m_verifiedByNet;
    if (m_torrent) {
        const qint64 bt = qMax<qint64>(0, m_torrent->snap().totalDone - m_torrent->injectedBytes() - qMax<qint64>(0, m_btBytesAtStart));
        if (bt > 0) by[int(Net::BitTorrent)] += bt;
    }
    qint64 total = 0;
    for (auto it = by.constBegin(); it != by.constEnd(); ++it) total += it.value();
    QStringList parts;
    for (auto it = by.constBegin(); it != by.constEnd(); ++it)
        if (it.value() > 0)
            parts << QStringLiteral("%1 %2 bytes (%3%)").arg(netName(Net(it.key()))).arg(it.value()).arg(total > 0 ? it.value() * 100 / total : 0);
    qCInfo(lcEngine).noquote() << m_rec.id << "bytes by network:" << (parts.isEmpty() ? QStringLiteral("(none this session)") : parts.join(QStringLiteral(", ")));
}

QString Download::trackerNote() const
{
    // what the trackers themselves report (an index's seed count may be years old)
    if (!m_torrent) return QString();
    static const QRegularExpression re(QStringLiteral("^Working \\((\\d+) seeds"));
    int best = -1, working = 0;
    for (const TransferSource &c : m_torrent->snap().channels) {
        const auto m = re.match(c.statusText);
        if (!m.hasMatch()) continue;
        ++working;
        best = qMax(best, m.captured(1).toInt());
    }
    if (working == 0) return QString();
    return best <= 0 ? QStringLiteral(", %1 tracker%2 report no seeds").arg(working).arg(working == 1 ? "" : "s")
                     : QStringLiteral(", trackers report %1").arg(countNoun(best, QStringLiteral("seed")));
}

// [core-5] "Fetching torrent from LimeTorrents..." while the torrent's metadata (or its .torrent
// file) is not there yet: the index that listed it, else the host the .torrent comes from
QString Download::fetchingFromIndexText() const
{
    QString who = m_indexName;
    if (who.isEmpty() && !m_pendingTorrentUrl.isEmpty()) {
        who = QUrl(m_pendingTorrentUrl).host();
        if (who.isEmpty()) who = m_pendingTorrentUrl.left(40);
    }
    if (who.isEmpty()) who = QStringLiteral("the index");
    return QStringLiteral("Fetching torrent from %1...").arg(who);
}

QString Download::indexNote() const
{
    if (m_indexSeeds <= 0 && m_indexPeers <= 0) return QString();
    const QString who = m_indexName.isEmpty() ? QStringLiteral("the index") : m_indexName;
    return QStringLiteral(" (%1 reported %2, %3)").arg(who, countNoun(qMax(0, m_indexSeeds), QStringLiteral("seed")),
                                                       countNoun(qMax(0, m_indexPeers), QStringLiteral("peer")));
}

SourceSlot *Download::slotByAddress(const SearchSource &src) const
{
    const QString ak = addressKey(src);
    return ak.isEmpty() ? nullptr : m_byAddr.value(ak);
}

int Download::effectiveAfs() const
{
    if (m_afs >= 0) return m_afs;
    return m_host->settings() ? qMax(0, m_host->settings()->i(keys::search::AutoFindSourcesMin)) : 10;
}

int Download::effectiveAeq() const
{
    if (m_aeq >= 0) return m_aeq;
    return m_host->settings() ? qMax(0, m_host->settings()->i(keys::search::AutoEnterQueue)) : 100;
}

void Download::setAutoFindSources(int minutes)
{
    m_afs = minutes < 0 ? -1 : qMin(minutes, 1440);
    qCInfo(lcEngine) << m_rec.id << "Auto Find Sources:" << (m_afs < 0 ? QStringLiteral("global") : QString::number(m_afs) + QStringLiteral(" min"));
    persist(false);
    markDirty(true);
}

void Download::setAutoEnterQueue(int n)
{
    m_aeq = n < 0 ? -1 : qMin(n, 100000);
    qCInfo(lcEngine) << m_rec.id << "Auto Enter Queue:" << (m_aeq < 0 ? QStringLiteral("global") : QString::number(m_aeq));
    const int aeq = effectiveAeq();
    // parked sources a looser limit admits again; queued sources a stricter one sends away
    for (auto &sp : m_slots) {
        SourceSlot *s = sp.get();
        if (s->aeqParked && aeq > 0 && (s->queueRank <= 0 || s->queueRank <= aeq)) {
            s->aeqParked = false;
            s->retryAtMs = 0;
            s->lastError.clear();
        }
    }
    QList<SourceSlot *> queued;
    for (auto &sp : m_slots) if (sp->rs && sp->rs->state() == RangeSource::Queued) queued << sp.get();
    for (SourceSlot *s : std::as_const(queued)) if (s->rs) enforceQueueLimit(s);
    if (m_active) openMoreSources();
    persist(false);
    markDirty(true);
}

void Download::enforceQueueLimit(SourceSlot *s)
{
    if (!s->rs) return;
    int rank = s->rs->descriptor().extra.value(QStringLiteral("queueRank")).toInt();
    if (rank <= 0) {
        static const QRegularExpression num(QStringLiteral("(\\d+)"));
        const auto mm = num.match(s->rs->stateText());
        if (mm.hasMatch()) rank = mm.captured(1).toInt();
    }
    s->queueRank = rank;
    if (m_queueOverride || s->priority > 0) return;   // "Enter Queue" / high priority: always wait
    const int aeq = effectiveAeq();
    const qint64 now = nowMs();
    if (aeq > 0) {
        if (rank <= 0 || rank <= aeq) return;
        // a long queue is only left when the download has something better to do
        bool better = m_torrent && m_torrent->snap().rateDownBps > 0;
        for (const auto &o : m_slots) {
            if (better) break;
            if (o.get() == s || o->banned || o->removedByUser) continue;
            if (o->rs && o->rate.lastActivityMs() && now - o->rate.lastActivityMs() < 10000) better = true;
            else if (!o->rs && !o->aeqParked && o->failures == 0 && o->retryAtMs <= now && slotUsable(o.get())) better = true;
        }
        if (!better) return;
    }
    const int afs = effectiveAfs();
    const QString why = aeq > 0 ? QStringLiteral("Remote queue too long (%1 > %2)").arg(rank).arg(aeq)
                                : QStringLiteral("Not entering remote queues (Auto Enter Queue: Never)");
    qCInfo(lcEngine) << m_rec.id << "leaving the remote queue of" << s->key << ":" << why;
    closeSlot(s, false, why);
    s->aeqParked = true;
    s->lastError = why;
    s->retryAtMs = now + qint64(afs > 0 ? afs : 30) * m_host->tuning().afsMinuteMs;
}

// ============================================================================ [merge-1] name-and-size candidates

void Download::candidateConfirmed(SourceSlot *src)
{
    const int fam = netFamily(src->desc.net);
    qCInfo(lcEngine) << m_rec.id << "candidate/hinted source" << src->key << "confirmed by a verified unit";
    src->trusted = true;
    CandNet &cn = m_candNets[fam];
    if (src->wasCandidate && !cn.confirmed) {
        cn.confirmed = true;
        cn.dropped = false;
        qCInfo(lcEngine).noquote() << m_rec.id << QStringLiteral("the %1 copy is the same file: confirmed by a verified unit (%2)")
                                                      .arg(netName(Net(fam)), judgeText());
    }
    const FileHashes claim = src->claimed;
    src->claimed = FileHashes();
    // a bundle's inner file has its own identity: never the download's
    if (m_rec.isBundle || claim.isEmpty() || m_rec.hashes.conflicts(claim)) return;
    const FileHashes fresh = newKinds(m_rec.hashes, claim);
    if (!fresh.isEmpty()) {
        m_confirmedClaims.mergeFrom(fresh);
        for (const QString &k : fresh.toVariant().keys()) if (k != QLatin1String("btFileIndex")) m_confirmedFrom.insert(k, fam);
    }
    learnHashes(claim, true, QStringLiteral("confirmed link"));
    publishLinks(m_rec.hashes);
}

void Download::candidateFailed(SourceSlot *c, const QString &why)
{
    const int fam = netFamily(c->desc.net);
    CandNet &cn = m_candNets[fam];
    const bool counts = c->wasCandidate && !cn.confirmed && !c->banned;
    // a hint proven wrong by this source: the hinted kinds it stood for go (findSources stops using them)
    if (!c->claimed.isEmpty() && c->claimed.matches(m_hinted) && !cn.confirmed) {
        m_rejected << m_hinted;
        m_hinted = FileHashes();
    }
    if (!c->wasCandidate && !c->claimed.isEmpty()) m_rejected << c->claimed;   // a hash-link hint: as before
    ban(c, why);
    if (!counts) return;
    ++cn.failed;
    int left = 0;
    for (const auto &sp : m_slots)
        if (!sp->banned && !sp->trusted && sp->wasCandidate && netFamily(sp->desc.net) == fam) ++left;
    qCInfo(lcEngine) << m_rec.id << netName(Net(fam)) << "candidate" << c->key << "failed the check (" << cn.failed
                     << "of that network," << left << "left)";
    if (cn.failed >= qMax(1, m_host->tuning().candidateNetFailures) || left == 0)
        dropNetwork(fam, QStringLiteral("%1 failed the check").arg(countNoun(cn.failed, QStringLiteral("source"))));
}

void Download::dropNetwork(int fam, const QString &why)
{
    CandNet &cn = m_candNets[fam];
    if (cn.dropped || cn.confirmed) return;
    cn.dropped = true;
    cn.why = why;
    qCWarning(lcEngine).noquote() << m_rec.id << QStringLiteral("the %1 copy is a different file: dropped (%2, %3 fetched from it)")
                                                     .arg(netName(Net(fam)), why).arg(cn.fetched);
    for (auto &sp : m_slots) {
        SourceSlot *s = sp.get();
        if (s->banned || s->trusted || !s->wasCandidate || netFamily(s->desc.net) != fam) continue;
        if (!s->claimed.isEmpty()) m_rejected << s->claimed;
        ban(s, QStringLiteral("The %1 copy is a different file").arg(netName(Net(fam))));
    }
    m_dirtyBlob = true;
    markDirty(true);
}

// The waste cap: a network no verified unit confirmed yet may have at most
// Tuning::candidateUnitsInFlight units of the judge in flight or unverified at once. Returns true
// when `s` is capped; *allowed = the units it may still work on (the ones already started).
bool Download::candidateCapped(const SourceSlot *s, RangeSet *allowed) const
{
    if (s->trusted || !s->wasCandidate) return false;
    const int fam = netFamily(s->desc.net);
    if (m_candNets.value(fam).confirmed) return false;
    const Store *st = store();
    if (!st) return false;
    QSet<qint64> units;
    RangeSet touched;
    auto addUnits = [&](qint64 a, qint64 b) {
        for (qint64 p = a; p < b;) {
            const Range u = st->unitAt(p);
            if (u.second <= p) break;
            if (!units.contains(u.first)) { units.insert(u.first); touched.add(u.first, u.second); }
            p = u.second;
        }
    };
    for (const auto &sp : m_slots) {
        const SourceSlot *o = sp.get();
        if (o->trusted || !o->wasCandidate || netFamily(o->desc.net) != fam) continue;
        for (const Request &r : o->reqs) addUnits(r.off + r.got, r.end());
        for (const Range &r : o->contributed.ranges()) addUnits(r.first, r.second);
    }
    if (units.size() < qMax(1, m_host->tuning().candidateUnitsInFlight)) return false;
    if (allowed) *allowed = touched;
    return true;
}

QString Download::judgeText() const
{
    if (m_torrent && m_torrent->hasMetadata()) return QStringLiteral("BitTorrent piece hashes");
    if (m_torrent) return QStringLiteral("BitTorrent piece hashes, once the torrent's file list is there");
    if (m_part) {
        switch (m_part->scheme()) {
        case PartStore::Ed2kParts: return m_ed2kProvisional ? QStringLiteral("eD2k part hashes (not confirmed yet)") : QStringLiteral("eD2k part hashes");
        case PartStore::TthLeaves: return QStringLiteral("the TTH tree");
        default: break;
        }
    }
    const FileHashes &t = m_rec.hashes;
    if (!t.ed2k.isEmpty() && !m_confirmedClaims.ed2k.size()) return QStringLiteral("the eD2k file hash (part hashes not there yet)");
    if (!t.sha1.isEmpty() && !m_confirmedClaims.sha1.size()) return QStringLiteral("the whole-file SHA-1");
    if (!t.btih.isEmpty()) return QStringLiteral("BitTorrent piece hashes, once the torrent's file list is there");
    if (!t.sha256.isEmpty() || !t.md5.isEmpty() || !t.tth.isEmpty()) return QStringLiteral("the whole-file hash");
    return QStringLiteral("nothing yet (no hash known)");
}

QString Download::disagreementNote(const QStringList &kinds) const
{
    QStringList nets, names;
    for (const QString &k : kinds) {
        const int fam = m_confirmedFrom.value(k, -1);
        const QString n = fam >= 0 ? netName(Net(fam))
                          : (k == QLatin1String("ed2k") || k == QLatin1String("aich")) ? netName(Net::ED2K)
                          : (k == QLatin1String("sha1") || k == QLatin1String("tth")) ? netName(Net::Gnutella)
                          : QStringLiteral("other network's");
        if (!nets.contains(n)) nets << n;
        names << (k == QLatin1String("ed2k") ? QStringLiteral("eD2k") : k.toUpper());
    }
    return QStringLiteral("the %1 copy was not the same file (its %2 hash differs; this file is complete by %3)")
        .arg(nets.join(QStringLiteral(" / ")), names.join(QStringLiteral(", ")), judgeText());
}

void Download::forgetKinds(const QStringList &kinds)
{
    FileHashes r;
    for (const QString &k : kinds) {
        if (k == QLatin1String("ed2k")) { r.ed2k = m_rec.hashes.ed2k; m_rec.hashes.ed2k.clear(); }
        else if (k == QLatin1String("sha1")) { r.sha1 = m_rec.hashes.sha1; m_rec.hashes.sha1.clear(); }
        else if (k == QLatin1String("tth")) { r.tth = m_rec.hashes.tth; m_rec.hashes.tth.clear(); }
        else if (k == QLatin1String("aich")) { r.aich = m_rec.hashes.aich; m_rec.hashes.aich.clear(); }
        else if (k == QLatin1String("sha256")) { r.sha256 = m_rec.hashes.sha256; m_rec.hashes.sha256.clear(); }
        else if (k == QLatin1String("md5")) { r.md5 = m_rec.hashes.md5; m_rec.hashes.md5.clear(); }
    }
    if (!r.isEmpty()) m_rejected << r;
    for (const QString &k : kinds) {
        const int fam = m_confirmedFrom.value(k, -1);
        if (fam >= 0) {
            CandNet &cn = m_candNets[fam];
            cn.why = QStringLiteral("not the same file (whole-file hash)");
        }
    }
}

void Download::nextTorrentCandidate()
{
    const qint64 now = nowMs();
    // [eng-1] the best ranked candidate that is not set aside right now
    QByteArray next;
    for (auto it = m_btCandidates.begin(); it != m_btCandidates.end();) {
        bool rejected = false;
        for (const FileHashes &r : std::as_const(m_rejected)) if (r.btih == *it) rejected = true;
        if (rejected || *it == m_rec.hashes.btih) it = m_btCandidates.erase(it);
        else ++it;
    }
    if (btCandidateReady(now, &next)) {
        m_btCandidates.removeAll(next);
        qCInfo(lcEngine) << m_rec.id << "trying the next torrent that may hold this file:" << hexId(next).left(8)
                         << "(" << btLabel(next) << ")";
        m_hinted.btih = next;
        markDirty(true);
        considerPromotion();
        return;
    }
    if (!m_btCandidates.isEmpty()) return;   // set aside for a while: watchPromotion() comes back to them
    // every torrent matched by name and size proved to be another file
    if (m_rec.hashes.btih.isEmpty()) dropNetwork(int(Net::BitTorrent), QStringLiteral("its pieces do not match the data"));
}

// [eng-1] ---------------------------------------------------------------- candidate torrents

void Download::addBtCandidate(const QByteArray &btih)
{
    if (btih.size() != 20 || btih == m_rec.hashes.btih) return;
    const qint64 now = nowMs();
    // a hint whose metadata is not being fetched yet gives way to a torrent with a larger swarm
    const QByteArray hint = m_hinted.btih;
    const bool hintIdle = !hint.isEmpty() && m_promoteTorrent != hexId(hint) && m_promoteUrl.isEmpty() && !m_promoting;
    if (hintIdle && btih != hint && btRank(btih, now) > btRank(hint, now)) {
        m_hinted.btih = btih;
        m_btCandidates.removeAll(btih);
        if (!m_btCandidates.contains(hint)) m_btCandidates.prepend(hint);
        qCInfo(lcEngine) << m_rec.id << "torrent" << hexId(btih).left(8) << "(" << btLabel(btih) << ") is tried before"
                         << hexId(hint).left(8) << "(" << btLabel(hint) << "): larger swarm";
        markDirty();
        return;
    }
    if (btih == hint || m_btCandidates.contains(btih) || m_btCandidates.size() >= 20) return;
    m_btCandidates << btih;
    markDirty();
}

int Download::btRank(const QByteArray &btih, qint64 now) const
{
    Q_UNUSED(now)
    // seeds + peers an index reported first, unknown (web-seeded archives, indexes without figures)
    // next, a swarm reported empty last; every fetch that gave no file list costs it a rank
    const int sw = m_btSwarm.value(btih, -1);
    const int base = sw > 0 ? 2000000 + qMin(sw, 999999) : sw < 0 ? 1000000 : 0;
    return base - 3000000 * m_btTries.value(btih);
}

bool Download::btCandidateReady(qint64 now, QByteArray *best) const
{
    QByteArray b;
    int br = std::numeric_limits<int>::min();
    for (const QByteArray &c : m_btCandidates) {
        if (c == m_rec.hashes.btih || hexId(c) == m_promoteTorrent) continue;
        bool rejected = false;
        for (const FileHashes &r : std::as_const(m_rejected)) if (r.btih == c) rejected = true;
        if (rejected || m_btRetryAt.value(c) > now) continue;
        const int r = btRank(c, now);
        if (r > br) { br = r; b = c; }
    }
    if (best) *best = b;
    return !b.isEmpty();
}

QString Download::btLabel(const QByteArray &btih) const
{
    const QString n = m_btNames.value(btih);
    return n.isEmpty() ? QStringLiteral("torrent %1").arg(hexId(btih).left(8)) : QStringLiteral("\"%1\"").arg(n);
}

bool Download::btCandidateKnown() const
{
    if (m_candNets.value(int(Net::BitTorrent)).dropped && m_rec.hashes.btih.isEmpty()) return false;
    return !m_promoteTorrent.isEmpty() || !m_promoteUrl.isEmpty() || m_promoting || !m_hinted.btih.isEmpty()
           || !m_rec.hashes.btih.isEmpty() || !m_btCandidates.isEmpty();
}

void Download::setAsideTorrent(qint64 now, const QString &why)
{
    TorrentBackend *be = m_host->backend();
    const QString id = m_promoteTorrent;
    const QByteArray bh = hash::fromHex(id);
    const int tries = ++m_btTries[bh];
    static const int backoffMin[] = {2, 5, 10, 20, 30};
    const int mins = backoffMin[qBound(0, tries - 1, 4)];
    m_btRetryAt.insert(bh, now + qint64(mins) * m_host->tuning().afsMinuteMs);
    if (!m_btCandidates.contains(bh)) m_btCandidates << bh;
    if (m_hinted.btih == bh) m_hinted.btih.clear();
    if (!m_promoteTrusted && m_rec.hashes.btih == bh) {   // the copy the row started from: a candidate now
        m_rec.hashes.btih.clear();
        m_rec.hashes.btFileIndex = -1;
    }
    if (be) be->removeTorrent(id, false);
    m_promoteTorrent.clear();
    m_fallbackTorrent.clear();
    m_promoteFile = -1;
    m_btNote = QStringLiteral("%1 %2, tried again in %3 min").arg(btLabel(bh), why).arg(mins);
    qCInfo(lcEngine) << m_rec.id << "promotion: torrent" << id.left(8) << why << "- set aside for" << mins << "min";
    m_dirtyBlob = true;
    markDirty(true);
    nextTorrentCandidate();
}

// The metadata fetch of a candidate torrent while the part file runs: announce it again while
// nobody answers, and give another candidate its turn when this one gives no file list in time.
void Download::watchPromotion(qint64 now)
{
    TorrentBackend *be = m_host->backend();
    if (!be || !m_part || m_promoting || m_completing) return;
    if (m_promoteTorrent.isEmpty()) {
        // nothing fetched right now: a set-aside candidate whose time has come, or a hint not asked for yet
        if (!m_promoteUrl.isEmpty()) return;
        if (!m_hinted.btih.isEmpty()) {   // a hint promotion cannot take (in the backend already, rejected) gives way
            bool rejected = m_promoteBlocked == m_hinted.btih;
            for (const FileHashes &r : std::as_const(m_rejected)) if (r.btih == m_hinted.btih) rejected = true;
            if (rejected && btCandidateReady(now)) m_hinted.btih.clear();
        }
        if (m_hinted.btih.isEmpty() &&(m_rec.hashes.btih.isEmpty() || m_promoteBlocked == m_rec.hashes.btih) && btCandidateReady(now))
            nextTorrentCandidate();
        return;
    }
    if (!m_promoteSinceMs) m_promoteSinceMs = now;
    const TorrentSnapshot sn = be->snapshot(m_promoteTorrent);
    if (sn.hasMetadata) return;   // tryPromote() takes it from here
    const int connected = sn.peers + sn.seeds;
    m_promotePeersMax = qMax(m_promotePeersMax, connected);
    const qint64 ran = now - m_promoteSinceMs;
    if (connected == 0) {
        const int afs = effectiveAfs();
        const qint64 am = m_host->tuning().afsMinuteMs;
        const qint64 every = ran < 10 * am ? 2 * am : qint64(afs > 0 ? afs : 30) * m_host->tuning().afsMinuteMs;
        if (now - qMax(m_promoteAnnounceMs, m_promoteSinceMs) >= every) {
            m_promoteAnnounceMs = now;
            be->reannounce(m_promoteTorrent);
            qCInfo(lcEngine) << m_rec.id << "promotion: no peers for torrent" << m_promoteTorrent.left(8) << "after" << ran / 1000
                             << "s, announcing it again (DHT and trackers)";
        }
    }
    if (ran >= promoteLimitMs(now) && !m_promoteTrusted && btCandidateReady(now)) {
        setAsideTorrent(now, m_promotePeersMax > 0
                                 ? QStringLiteral("gave no file list in %1 s (%2 connected)").arg(ran / 1000).arg(countNoun(m_promotePeersMax, QStringLiteral("peer")))
                                 : QStringLiteral("had no peers in %1 s").arg(ran / 1000));
    }
}

// transfers/metadataTimeoutSec; a third of it while the torrent never had a peer and a candidate the
// search ranks higher is waiting (the dead single-file torrent before the folder with 500 peers)
qint64 Download::promoteLimitMs(qint64 now) const
{
    const int sec = m_host->settings() && m_host->settings()->value(QString(keys::transfers::MetadataTimeoutSec)).isValid()
                        ? m_host->settings()->i(keys::transfers::MetadataTimeoutSec) : 90;
    const qint64 full = qint64(qMax(1, sec)) * 1000;
    QByteArray better;
    if (m_promotePeersMax == 0 && btCandidateReady(now, &better) && btRank(better, now) > btRank(hash::fromHex(m_promoteTorrent), now))
        return full / 3;
    return full;
}

bool Download::provisionalUsable(const SourceSlot *s) const
{
    if (!m_provisional || s->trusted || s->banned) return false;
    if (m_provisionalClaim.isEmpty() || s->claimed.isEmpty()) return true;
    return !s->claimed.conflicts(m_provisionalClaim);
}

void Download::considerProvisional(qint64 now)
{
    if (m_provisional || !m_part || m_part->scheme() != PartStore::NoUnits || m_completing || m_promoting) return;
    if (now - m_activatedMs < qMax(1000, m_host->tuning().provisionalGraceMs)) return;
    // the whole-file check must be able to decide: a trusted whole-file hash is required (a torrent
    // identity alone cannot check a part file: its pieces need the metadata)
    const FileHashes &t = m_rec.hashes;
    if (t.ed2k.isEmpty() && t.sha1.isEmpty() && t.sha256.isEmpty() && t.md5.isEmpty() && t.tth.isEmpty() && t.aich.isEmpty()) return;
    if (m_lastTrustedDataMs > m_activatedMs && now - m_lastTrustedDataMs < 30000) return;   // something verifiable flows
    QList<QPair<FileHashes, int>> claims;
    for (const auto &sp : m_slots) {
        const SourceSlot *s = sp.get();
        if (s->banned || s->removedByUser) continue;
        if (s->trusted) {
            if (s->rs && (s->rs->state() == RangeSource::Ready || s->rs->state() == RangeSource::Busy)) return;
            continue;
        }
        if (!m_host->pluginFor(s->desc.net)) continue;
        bool grouped = false;
        for (auto &c : claims)
            if (c.first.matches(s->claimed) && !c.first.conflicts(s->claimed)) { ++c.second; grouped = true; break; }
        if (!grouped) claims << qMakePair(s->claimed, 1);
    }
    if (claims.isEmpty()) return;
    std::stable_sort(claims.begin(), claims.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
    m_provisional = true;
    m_provisionalClaim = claims.first().first;
    qCInfo(lcEngine).noquote() << m_rec.id << "no verifiable source served anything:" << claims.first().second
                               << "unverified candidate source(s) claiming" << m_provisionalClaim.toVariant().keys().join(QLatin1Char('+'))
                               << "are used provisionally, the whole-file check against the trusted hashes decides";
    markDirty(true);
    openMoreSources();
    emit needsSchedule();
}

void Download::creditWholeFile()
{
    for (auto &sp : m_slots) {
        SourceSlot *s = sp.get();
        const qint64 n = s->contributed.total();
        if (n <= 0) continue;
        s->verifiedBytes += n;
        m_verifiedByNet[int(s->desc.net)] += n;
        s->contributed.clear();
        if (!s->trusted) {
            qCInfo(lcEngine) << m_rec.id << "candidate source" << s->key << "confirmed by the whole-file check";
            s->trusted = true;
            s->claimed = FileHashes();
            if (s->wasCandidate) {   // [merge-1]
                CandNet &cn = m_candNets[netFamily(s->desc.net)];
                cn.confirmed = true;
                cn.dropped = false;
            }
        }
    }
    m_provisional = false;
    m_dirtyBlob = true;
}

void Download::addHitSources(const SearchHit &hit)
{
    if (isFinished()) return;
    if (!hit.hashes.isEmpty() && !m_rec.hashes.conflicts(hit.hashes)) learnHashes(hit.hashes, true, QStringLiteral("search hit"));
    if (m_rec.size <= 0 && hit.size > 0 && !hit.isBundle) {
        m_rec.size = hit.size;
        if (m_part) m_part->setSize(hit.size);
    }
    addSources(hit.sources, nullptr, true);
}

int Download::connectedSources() const
{
    if (!m_active || isFinished()) return 0;
    int n = 0;
    for (const auto &s : m_slots) {
        if (!s->rs) continue;
        const RangeSource::State st = s->rs->state();
        if (st == RangeSource::Queued || st == RangeSource::Ready || st == RangeSource::Busy) ++n;
    }
    if (m_torrent) n += m_torrent->snap().peers + m_torrent->snap().seeds;
    return n;
}

// ---- background size+name search ([merge-1] per file, every network, the one name rule)
QList<Download::NameSubject> Download::nameSubjects() const
{
    QList<NameSubject> out;
    const qint64 minSize = 1 << 20;
    // a torrent without its file list: what it holds (one file or a folder) is not known yet
    if (m_torrent && !m_torrent->hasMetadata()) return out;
    if (!m_rec.isBundle) {
        if (m_rec.size >= minSize && !m_rec.name.isEmpty() && !SearchMerger::nameWords(m_rec.name).isEmpty()
            && !SearchMerger::probableExtension(m_rec.name).isEmpty())
            out << NameSubject{-1, m_rec.name, QString(), m_rec.size};
        return out;
    }
    // a folder torrent: its large files, one search each (the largest first)
    QList<NameSubject> all;
    qint64 total = 0;
    if (m_torrent && m_torrent->hasMetadata()) {
        const QList<TorrentFile> &files = m_torrent->snap().files;
        for (int i = 0; i < files.size(); ++i) {
            if (m_torrent->isPadFile(i) || files.at(i).priority <= 0) continue;
            total += files.at(i).size;
            all << NameSubject{i, QFileInfo(files.at(i).path).fileName(), files.at(i).path, files.at(i).size};
        }
    } else {
        for (const BundleFile &f : m_rec.files) {
            if (SearchMerger::isPadFile(f.path) || !f.wanted) continue;
            total += f.size;
            all << NameSubject{f.index, QFileInfo(f.path).fileName(), f.path, f.size};
        }
    }
    std::stable_sort(all.begin(), all.end(), [](const NameSubject &a, const NameSubject &b) { return a.size > b.size; });
    qint64 covered = 0;
    for (const NameSubject &n : std::as_const(all)) {
        if (out.size() >= 4 || (total > 0 && covered * 10 >= total * 9)) break;
        if (n.size < minSize || SearchMerger::probableExtension(n.name).isEmpty() || SearchMerger::nameWords(n.name).isEmpty()) continue;
        out << n;
        covered += n.size;
    }
    return out;
}

bool Download::lacksSources(qint64 now) const
{
    Q_UNUSED(now)
    // "more peers as possible": search again while the download is slow or has few sources
    int usable = 0;
    for (const auto &s : m_slots) if (!s->banned && !s->removedByUser) ++usable;
    if (m_torrent) usable += m_torrent->snap().peers + m_torrent->snap().seeds;
    const qint64 left = qMax<qint64>(0, m_rec.size - m_rec.done);
    const qint64 eta = m_rateEma > 1 ? qint64(double(left) / m_rateEma) : std::numeric_limits<qint64>::max();
    return usable < 20 || eta > 15 * 60;
}

bool Download::wantsNameSearch(qint64 now) const
{
    if (!m_active || isFinished() || m_completing || m_promoting || m_nameSearchRunning) return false;
    if (m_nameSearches >= qMax(1, m_host->tuning().nameSearchMax) || now - m_activatedMs < m_host->tuning().nameSearchDelayMs) return false;
    if (nameSubjects().isEmpty()) return false;
    if (m_nextNameSearchMs != 0 && now < m_nextNameSearchMs) return false;
    return m_nameSearches == 0 || lacksSources(now);
}

SearchQuery Download::nameSearchQuery() const
{
    SearchQuery q;
    const QList<NameSubject> subs = nameSubjects();
    if (subs.isEmpty()) return q;
    const NameSubject &sub = subs.at(m_subjectTurn % subs.size());
    QStringList words = SearchMerger::nameWords(sub.name);
    while (words.size() > 6) words.removeLast();
    while (words.size() > 1 && words.join(QLatin1Char(' ')).size() > 60) words.removeLast();
    q.text = words.join(QLatin1Char(' '));
    const QString ext = SearchMerger::probableExtension(sub.name);
    if (!ext.isEmpty() && ext.size() <= 5) q.extension = ext;
    // exactly this size for a plain file; a folder torrent that holds it is a little larger
    q.minSize = sub.size;
    q.maxSize = sub.size + sub.size / 2 + (64LL << 20);
    q.maxResults = 300;
    const bool haveTorrent = m_torrent || !m_rec.hashes.btih.isEmpty();
    for (NetworkPlugin *p : m_host->plugins()) {
        if (!(p->capabilities() & NetworkPlugin::CanSearch) || !m_host->pluginUsable(p)) continue;
        if (p->status().state != ConnState::Connected) continue;
        // a network that indexes files by a hash we trust finds the same file by findSources()
        if ((p->net() == Net::ED2K || p->net() == Net::Kad) && sub.file < 0 && !m_rec.hashes.ed2k.isEmpty()) continue;
        // a torrent download cannot take a second torrent: torrent-only networks have nothing for it
        if (p->net() == Net::Rats && haveTorrent) continue;
        if (!q.networks.contains(p->net())) q.networks << p->net();
    }
    q.extensions.clear();
    return q;
}

void Download::nameSearchStarted(qint64 now, const QList<Net> &nets)
{
    m_nameSearchRunning = true;
    m_nameSearchNets = nets;
    m_lastNameSearchMs = now;
    ++m_nameSearches;
    const QList<NameSubject> subs = nameSubjects();
    m_curSubject = subs.isEmpty() ? NameSubject() : subs.at(m_subjectTurn % subs.size());
    ++m_subjectTurn;
    // what each network is doing in this round (asked, or skipped and why)
    const bool haveTorrent = m_torrent || !m_rec.hashes.btih.isEmpty();
    for (NetworkPlugin *p : m_host->plugins()) {
        if (!(p->capabilities() & NetworkPlugin::CanSearch)) continue;
        NameNetStat &st = m_nameStats[int(p->net())];
        if (nets.contains(p->net())) {
            ++st.rounds;
            st.skipped.clear();
        } else if (!m_host->pluginUsable(p)) {
            st.skipped = QStringLiteral("switched off");
        } else if (p->status().state != ConnState::Connected) {
            st.skipped = QStringLiteral("not connected");
        } else if ((p->net() == Net::ED2K || p->net() == Net::Kad) && !m_rec.hashes.ed2k.isEmpty()) {
            st.skipped = QStringLiteral("not needed, looked up by the eD2k hash");
        } else if (p->net() == Net::Rats && haveTorrent) {
            st.skipped = QStringLiteral("not needed, only torrents there");
        }
    }
    qCInfo(lcEngine).noquote() << m_rec.id << "searching by size+name:" << nameSearchQuery().text << "size" << m_curSubject.size
                               << (m_curSubject.file >= 0 ? QStringLiteral("(file %1 of the torrent)").arg(m_curSubject.file) : QString())
                               << "on" << nets.size() << "network(s)";
    markDirty(true);
}

void Download::nameSearchHits(const QList<SearchHit> &hits)
{
    if (isFinished() || m_curSubject.size <= 0) return;
    const NameSubject sub = m_curSubject;
    const bool haveTorrent = m_torrent || !m_rec.hashes.btih.isEmpty();
    const qint64 minSize = 1 << 20;
    QList<SearchSource> add;
    auto rejectedClaim = [this](const FileHashes &hh) {
        for (const FileHashes &r : std::as_const(m_rejected)) if (!hh.isEmpty() && r.matches(hh)) return true;
        return false;
    };
    for (const SearchHit &h : hits) {
        // a folder torrent of another index: one of its files may be ours
        if (h.isBundle) {
            if (haveTorrent || h.hashes.btih.isEmpty()) continue;
            if (h.files.isEmpty()) {
                if (h.size >= sub.size && SearchMerger::sameName(h.fileName, sub.name) && m_wantFileLists.size() < 10
                    && !m_wantFileLists.contains(h.key))
                    m_wantFileLists << h.key;
                continue;
            }
            for (const BundleFile &f : h.files) {
                if (!SearchMerger::sameFile(QFileInfo(f.path).fileName(), f.size, sub.name, sub.size, minSize)) continue;
                FileHashes inner;
                inner.btih = h.hashes.btih;
                inner.btFileIndex = f.index;
                if (rejectedClaim(inner)) break;
                for (const SearchSource &bs : h.sources) {
                    if (bs.net != Net::BitTorrent) continue;
                    SearchSource s = bs;
                    if (f.index >= 0 && s.uri.startsWith(QLatin1String("magnet:"))) s.uri += QStringLiteral("&so=%1").arg(f.index);
                    s.extra.insert(QStringLiteral("nameSearch"), true);
                    s.extra.insert(QStringLiteral("candidate"), true);
                    s.extra.insert(QStringLiteral("hashes"), inner.toVariant());
                    add << s;
                    break;
                }
                ++m_nameStats[int(Net::BitTorrent)].copies;
                break;
            }
            continue;
        }
        const bool sizeOk = h.size == sub.size;
        if (!SearchMerger::sameFile(h.fileName, h.size, sub.name, sub.size, minSize)) {
            // (identical words only: the loose rule would count every file of that film)
            if (!sizeOk && SearchMerger::nameWords(h.fileName) == SearchMerger::nameWords(sub.name)) {
                QSet<int> nets;
                for (const SearchSource &s : h.sources) nets.insert(int(s.net));
                for (int n : nets) ++m_nameStats[n].otherSize;
            }
            continue;
        }
        const FileHashes hh = sanitized(h.hashes);
        if (sub.file < 0 && m_rec.hashes.conflicts(hh)) continue;
        if (rejectedClaim(hh)) continue;
        const bool same = sub.file < 0 && m_rec.hashes.matches(hh);   // the same file by hash: a real source
        QSet<int> nets;
        for (SearchSource s : h.sources) {
            if (s.net == Net::BitTorrent && (haveTorrent || m_rec.isBundle)) continue;   // one torrent per download
            nets.insert(int(s.net));
            ++m_nameStats[int(s.net)].sources;
            s.extra.insert(QStringLiteral("nameSearch"), true);
            const bool srcCandidate = !same || s.extra.value(QStringLiteral("candidate")).toBool();
            if (srcCandidate) {
                s.extra.insert(QStringLiteral("candidate"), true);
                if (!s.extra.contains(QStringLiteral("hashes"))) s.extra.insert(QStringLiteral("hashes"), hh.toVariant());
            }
            if (sub.file >= 0) {   // an inner file of our bundle
                s.extra.insert(QStringLiteral("bundlePath"), sub.path);
                s.extra.insert(QStringLiteral("size"), sub.size);
            }
            add << s;
        }
        for (int n : nets) ++m_nameStats[n].copies;
    }
    if (add.isEmpty()) {
        markDirty();
        return;
    }
    const size_t before = m_slots.size();
    addSources(add, nullptr, false);
    const int n = int(m_slots.size() - before);
    if (n > 0) {
        m_nameCandidates += n;
        qCInfo(lcEngine) << m_rec.id << "size+name search:" << n << "new candidate source(s)";
    }
    markDirty(true);
}

QStringList Download::takeFileListWants()
{
    QStringList w = m_wantFileLists;
    m_wantFileLists.clear();
    return w;
}

void Download::nameSearchFinished()
{
    if (!m_nameSearchRunning) return;
    m_nameSearchRunning = false;
    const int afs = effectiveAfs();
    const qint64 now = nowMs();
    const int subjects = int(nameSubjects().size());
    if (afs == 0) {
        // Auto Find Sources off: each file once
        m_nextNameSearchMs = m_subjectTurn < subjects ? now + 15000 : std::numeric_limits<qint64>::max();
    } else if (m_subjectTurn < subjects) {
        m_nextNameSearchMs = now + 15000;   // the other large files of a bundle first
    } else {
        // back-off: 2, 5, 10, 20, 30 min, then hourly (only while the download lacks speed or sources)
        static const int steps[] = {2, 5, 10, 20, 30, 60};
        const int round = qMax(0, m_nameSearches / qMax(1, subjects) - 1);
        m_nextNameSearchMs = now + qint64(steps[qMin(round, 5)]) * m_host->tuning().afsMinuteMs;
    }
    // a keyword network that came up while this search ran (typical right after start): ask it soon
    for (Net n : nameSearchQuery().networks)
        if (!m_nameSearchNets.contains(n)) { m_nextNameSearchMs = qMin(m_nextNameSearchMs, now + 5000); break; }
    markDirty(true);
}

QString Download::nameSearchNote(int net, qint64 now) const
{
    Q_UNUSED(now)
    const NameNetStat st = m_nameStats.value(net);
    if (st.rounds == 0) return st.skipped.isEmpty() ? QString() : QStringLiteral("Skipped: %1").arg(st.skipped);
    int found = 0, delivering = 0, confirmed = 0, failed = 0, waiting = 0;
    for (const auto &sp : m_slots) {
        const SourceSlot *s = sp.get();
        if (!s->candidateFromName || int(s->desc.net) != net) continue;
        ++found;
        if (s->banned) { ++failed; continue; }
        if (s->trusted && s->wasCandidate) ++confirmed;
        if (s->rs && s->rate.lastActivityMs() && nowMs() - s->rate.lastActivityMs() < 5000) ++delivering;
        else if (!s->trusted) ++waiting;
    }
    if (net == int(Net::BitTorrent) && st.copies > 0 && found == 0) {
        const CandNet cn = m_candNets.value(int(Net::BitTorrent));
        QString t = QStringLiteral("%1 with the same size and name").arg(countNoun(st.copies, QStringLiteral("torrent")));
        if (cn.confirmed) t += QStringLiteral(", confirmed, its swarm delivers");
        else if (cn.dropped) t += QStringLiteral(", did not match, dropped");
        else if (!m_promoteTorrent.isEmpty()) t += QStringLiteral(", fetching its file list to check it");
        return t;
    }
    if (st.copies == 0 && found == 0) {
        QString t = QStringLiteral("none (%1)").arg(st.rounds == 1 ? QStringLiteral("searched once") : QStringLiteral("searched %1 times").arg(st.rounds));
        if (st.otherSize > 0) t += QStringLiteral(", %1 with this name but another size").arg(st.otherSize);
        return t;
    }
    QStringList parts;
    parts << QStringLiteral("%1 with the same size and name").arg(countNoun(qMax(st.copies, found), QStringLiteral("copy"), QStringLiteral("copies")));
    if (delivering) parts << QStringLiteral("%1 delivering").arg(delivering);
    if (confirmed) parts << QStringLiteral("%1 confirmed").arg(confirmed);
    if (waiting) parts << QStringLiteral("%1 waiting").arg(waiting);
    if (failed) parts << QStringLiteral("%1 did not match, dropped").arg(failed);
    if (found == 0 && st.copies > 0) parts << QStringLiteral("already known");
    return parts.join(QStringLiteral(", "));
}

// ---- reporting: every known source is a row
QString Download::sourceStatus(const SourceSlot *s, qint64 now, int *rank) const
{
    // [merge-1] a source matched only by name and size says so until a verified unit confirms it
    const QString unv = s->trusted ? (s->wasCandidate ? QStringLiteral(" (confirmed)") : QString())
                        : s->wasCandidate ? QStringLiteral(" (same name and size, checked while downloading)")
                                          : QStringLiteral(" (unverified)");
    auto retryIn = [now](qint64 at) {
        const qint64 left = at - now;
        return left >= 60000 ? QStringLiteral("retry in %1 min").arg((left + 59999) / 60000) : QStringLiteral("retry in %1 s").arg(qMax<qint64>(1, (left + 999) / 1000));
    };
    if (s->banned) {
        *rank = 8;
        return QStringLiteral("Banned (%1)").arg(s->lastError.isEmpty() ? QStringLiteral("File Mismatch") : s->lastError);
    }
    if (s->rs) {
        const RangeSource::State st = s->rs->state();
        if (s->rate.lastActivityMs() && now - s->rate.lastActivityMs() < 5000) {
            *rank = 0;
            // [ovl-2] an OctoMX source through a relayed circuit says so (another user carries the bytes)
            const bool relayed = stateTextOf(s->rs).contains(QLatin1String("relayed"));
            return QStringLiteral("Transferring") + (relayed ? QStringLiteral(" (relayed)") : QString()) + unv;
        }
        if (st == RangeSource::Queued) {
            *rank = 2;
            if (s->queueRank > 0) return QStringLiteral("Remotely Queued %1").arg(s->queueRank) + unv;
            const QString t = stateTextOf(s->rs);
            return (t.startsWith(QLatin1String("Remotely Queued"), Qt::CaseInsensitive) ? t : QStringLiteral("Remotely Queued")) + unv;
        }
        if (st == RangeSource::Ready || st == RangeSource::Busy) {
            *rank = 1;
            if (!s->reqs.isEmpty()) return QStringLiteral("Connected, Negotiating...") + unv;
            if (const Store *store = this->store()) {
                if (!slotAvailability(s).intersected(store->fetchTarget()).isEmpty() || store->fetchTarget().isEmpty())
                    return QStringLiteral("Connected") + unv;
                return QStringLiteral("Connected, No Needed Parts") + unv;
            }
            return QStringLiteral("Connected") + unv;
        }
        *rank = 3;
        const QString t = stateTextOf(s->rs);
        return (t.isEmpty() ? QStringLiteral("Connecting to Remote User") : t) + unv;
    }
    if (!m_active) {
        *rank = 4;
        return m_userPaused ? QStringLiteral("Paused") : QStringLiteral("Not connected (download locally queued)");
    }
    if (!m_host->pluginFor(s->desc.net)) {
        *rank = 7;
        return QStringLiteral("Unavailable (%1 is offline)").arg(netName(s->desc.net));
    }
    if (m_torrent && s->fileIndex < 0) {
        *rank = 7;
        return m_torrent->hasMetadata() ? QStringLiteral("Not used (no matching file in the torrent)")
                                        : QStringLiteral("Waiting for the torrent's file list");
    }
    if (!s->trusted && m_part && m_part->scheme() == PartStore::NoUnits && !provisionalUsable(s)) {
        *rank = 6;
        if (m_provisional) return QStringLiteral("Not used: claims a different file");
        if (!m_rec.hashes.btih.isEmpty() && m_rec.hashes.ed2k.isEmpty() && m_rec.hashes.sha1.isEmpty() && m_rec.hashes.tth.isEmpty()
            && m_rec.hashes.sha256.isEmpty() && m_rec.hashes.md5.isEmpty())
            return QStringLiteral("Not used yet: unverified (waiting for the torrent's file list to check it)");
        return QStringLiteral("Not used yet: unverified (needs a part hash set)");
    }
    if (s->aeqParked && s->retryAtMs > now) {
        *rank = 5;
        return QStringLiteral("%1, %2").arg(s->lastError, retryIn(s->retryAtMs));
    }
    if (s->failures > 0 && s->retryAtMs > now) {
        *rank = 5;
        QString e = s->lastError;
        if (!e.startsWith(QLatin1String("Timed Out"), Qt::CaseInsensitive) && !e.startsWith(QLatin1String("Busy"), Qt::CaseInsensitive)
            && !e.startsWith(QLatin1String("File"), Qt::CaseInsensitive) && !e.startsWith(QLatin1String("Remote"), Qt::CaseInsensitive))
            e = e.isEmpty() ? QStringLiteral("File/User Unavailable") : QStringLiteral("File/User Unavailable (%1)").arg(e);
        return QStringLiteral("%1, %2").arg(e, retryIn(s->retryAtMs));
    }
    *rank = 4;
    int limit = m_host->tuning().maxOpenSourcesPerDownload;
    if (m_host->settings()) limit = qMin(limit, qMax(1, m_host->settings()->i(keys::transfers::MaxSourcesPerDownload)));
    if (m_singleSource) limit = 1;
    if (openSources() >= limit) return QStringLiteral("Waiting (%1 source%2 already open)").arg(openSources()).arg(openSources() == 1 ? "" : "s");
    return QStringLiteral("Waiting for Network Reply...") + unv;
}

void Download::sourceRows(QList<TransferSource> &out, QSet<int> &nets, int *active, qint64 now) const
{
    struct Row { int rank = 0; qint64 bytes = 0; int order = 0; TransferSource ts; };
    QList<Row> rows;
    rows.reserve(int(m_slots.size()));
    for (const auto &sp : m_slots) {
        const SourceSlot *s = sp.get();
        if (s->removedByUser) continue;
        const bool act = s->rs && s->rate.lastActivityMs() && now - s->rate.lastActivityMs() < 5000;
        if (act) { ++*active; nets.insert(int(s->desc.net)); }
        Row r;
        r.ts.net = s->desc.net;
        r.ts.user = s->desc.user;
        if (r.ts.user.name.isEmpty()) r.ts.user.name = s->desc.user.id.isEmpty() ? QUrl(s->desc.uri).host() : s->desc.user.id;
        if (r.ts.user.name.isEmpty()) r.ts.user.name = s->desc.user.address;
        if (r.ts.user.net == Net::Unknown) r.ts.user.net = s->desc.net;
        r.ts.statusText = sourceStatus(s, now, &r.rank);
        r.ts.bytes = s->bytes;
        r.ts.rateBps = s->rs ? s->rate.rate(now, 5) : 0;
        r.ts.connection = s->desc.connection.isEmpty() ? netName(s->desc.net) : s->desc.connection;
        r.ts.client = s->rs ? s->rs->clientName() : QString();
        r.ts.id = s->key;
        r.ts.priority = s->priority;
        r.bytes = s->bytes;
        r.order = s->order;
        rows << r;
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
        if (a.rank != b.rank) return a.rank < b.rank;
        if (a.bytes != b.bytes) return a.bytes > b.bytes;
        return a.order < b.order;
    });
    constexpr int kMaxRows = 60;   // the rest becomes one summary row (a tree of 2000 rows helps nobody)
    for (int i = 0; i < rows.size() && i < kMaxRows; ++i) out << rows.at(i).ts;
    if (rows.size() > kMaxRows) {
        TransferSource more;
        QHash<QString, int> byState;
        QSet<int> moreNets;
        for (int i = kMaxRows; i < rows.size(); ++i) {
            const TransferSource &x = rows.at(i).ts;
            more.bytes += x.bytes;
            more.rateBps += x.rateBps;
            moreNets.insert(int(x.net));
            const int rk = rows.at(i).rank;
            ++byState[rk <= 1 ? QStringLiteral("connected") : rk == 2 ? QStringLiteral("queued") : rk <= 4 ? QStringLiteral("waiting")
                     : rk == 8 ? QStringLiteral("banned") : QStringLiteral("unavailable")];
        }
        more.net = moreNets.size() == 1 ? Net(*moreNets.begin()) : Net::Unknown;
        more.user.net = more.net;
        more.user.name = QStringLiteral("+ %1 more sources").arg(rows.size() - kMaxRows);
        QStringList parts;
        for (const char *k : {"connected", "queued", "waiting", "unavailable", "banned"})
            if (byState.value(QLatin1String(k))) parts << QStringLiteral("%1 %2").arg(byState.value(QLatin1String(k))).arg(QLatin1String(k));
        more.statusText = parts.join(QStringLiteral(", "));
        more.connection = more.net == Net::Unknown ? QString() : netName(more.net);
        out << more;
    }
}

namespace {
QString inTime(qint64 ms)
{
    const qint64 s = qMax<qint64>(1, (ms + 999) / 1000);
    return s < 90 ? QStringLiteral("%1 s").arg(s) : QStringLiteral("%1 min").arg((s + 59) / 60);
}
QString rateText(int bps)
{
    return bps >= 1024 * 1024 ? QStringLiteral("%1 MB/s").arg(bps / 1048576.0, 0, 'f', 1) : QStringLiteral("%1 KB/s").arg(bps / 1024);
}
} // namespace

// [eng-1] the BitTorrent row of a torrent download: what the swarm does, in plain words
QString Download::btSwarmStatus() const
{
    const TorrentSnapshot &sn = m_torrent->snap();
    const int connected = sn.peers + sn.seeds;
    const qint64 now = nowMs();
    if (!m_torrent->hasMetadata()) {
        if (connected > 0) return QStringLiteral("Getting the torrent's file list (%1)...").arg(countNoun(connected, QStringLiteral("peer")));
        if (!m_active) return QStringLiteral("Waiting for the torrent's file list");
        QByteArray better;   // the same rule as tick(): a third of the timeout, then the better torrent
        if (!m_metaStalled && btCandidateReady(now, &better) && btRank(better, now) > btRank(m_rec.hashes.btih, now)) {
            const int sec = m_host->settings() && m_host->settings()->value(QString(keys::transfers::MetadataTimeoutSec)).isValid()
                                ? m_host->settings()->i(keys::transfers::MetadataTimeoutSec) : 90;
            return QStringLiteral("No peers for this torrent yet, trying another torrent in %1")
                .arg(inTime(m_activatedMs + qint64(qMax(1, sec)) * 1000 / 3 - now));
        }
        const qint64 next =qMax(m_lastReannounceMs, m_activatedMs) + (now - m_activatedMs < 600000 ? 120000 : qint64(qMax(1, effectiveAfs() > 0 ? effectiveAfs() : 30)) * m_host->tuning().afsMinuteMs);
        return QStringLiteral("No peers for this torrent yet, trying again in %1").arg(inTime(next - now));
    }
    TorrentBackend *be = m_host->backend();
    const TorrentDiag dg = be ? be->torrentDiagnostics(m_torrentId) : TorrentDiag();
    const int known = qMax(qMax(dg.knownPeers, 0), qMax(0, sn.seedsTotal) + qMax(0, sn.peersTotal));
    if (connected == 0) {
        if (!m_active || sn.paused) return QStringLiteral("Idle");
        return known > 0 ? QStringLiteral("Connecting to the swarm (%1 known)...").arg(countNoun(known, QStringLiteral("peer")))
                         : QStringLiteral("No peers for this torrent yet, asking DHT and trackers");
    }
    QString t = known > connected ? QStringLiteral("%1 of %2 connected").arg(connected).arg(countNoun(known, QStringLiteral("peer")))
                                  : QStringLiteral("%1 connected").arg(countNoun(connected, QStringLiteral("peer")));
    if (sn.rateDownBps > 0) t += QStringLiteral(", ") + rateText(sn.rateDownBps);
    return t;
}

// [eng-1] the BitTorrent row while the download runs as a part file and a torrent that holds the
// same file is being looked at (metadata, the piece check) or waits for its next turn
QString Download::btPartStatus(qint64 now) const
{
    TorrentBackend *be = m_host->backend();
    if (m_promoting) return QStringLiteral("Got the file list, checking that the torrent's file is this one...");
    if (!m_promoteUrl.isEmpty())
        return QStringLiteral("Fetching the .torrent file from %1...").arg(QUrl(m_promoteUrl).host());
    if (!m_promoteTorrent.isEmpty() && be) {
        const TorrentSnapshot sn = be->snapshot(m_promoteTorrent);
        const int connected = sn.peers + sn.seeds;
        if (sn.hasMetadata) {
            if (m_promoteFile >= 0)
                return QStringLiteral("Got the file list; the torrent is compared with this file as soon as a whole piece is here (%1 held)")
                    .arg(humanBytes(m_part ? m_part->heldBytes() : 0));
            return QStringLiteral("Got the file list, looking for this file in it...");
        }
        if (connected > 0) return QStringLiteral("Getting the torrent's file list (%1)...").arg(countNoun(connected, QStringLiteral("peer")));
        const qint64 ran = now - (m_promoteSinceMs ? m_promoteSinceMs : now);
        if (!m_promoteTrusted && btCandidateReady(now))
            return QStringLiteral("No peers for this torrent yet, trying another torrent in %1").arg(inTime(promoteLimitMs(now) - ran));
        const qint64 am = m_host->tuning().afsMinuteMs;
        const qint64 every = ran < 10 * am ? 2 * am : qint64(effectiveAfs() > 0 ? effectiveAfs() : 30) * m_host->tuning().afsMinuteMs;
        return QStringLiteral("No peers for this torrent yet, trying again in %1").arg(inTime(qMax(m_promoteAnnounceMs, m_promoteSinceMs) + every - now));
    }
    // between two tries: the earliest set-aside candidate
    qint64 next = 0;
    for (const QByteArray &c : m_btCandidates) {
        const qint64 at = m_btRetryAt.value(c);
        if (at > now && (!next || at < next)) next = at;
    }
    if (next) {
        const QString again = QStringLiteral("next torrent tried in %1").arg(inTime(next - now));
        return m_btNote.isEmpty() ? QStringLiteral("No torrent gave its file list yet, %1").arg(again) : m_btNote.section(QStringLiteral(", tried again"), 0, 0) + QStringLiteral("; ") + again;
    }
    if (!m_btNote.isEmpty()) return m_btNote;
    const QByteArray cur = !m_hinted.btih.isEmpty() ? m_hinted.btih : m_rec.hashes.btih;
    if (!cur.isEmpty() && cur == m_promoteBlocked)
        return QStringLiteral("This torrent is another transfer already; it is not joined");
    return QStringLiteral("Waiting to ask for the torrent's file list");
}

void Download::torrentRows(QList<TransferSource> &out, QSet<int> &nets, int *active) const
{
    const TorrentSnapshot &sn = m_torrent->snap();
    QList<TransferSource> peers, seeds;
    for (TransferSource p : sn.sources) {
        if (p.net == Net::Unknown) p.net = Net::BitTorrent;
        if (p.user.net == Net::Unknown) p.user.net = p.net;
        if (p.rateBps > 0) { ++*active; nets.insert(int(p.net)); }
        if (p.net == Net::Http) seeds << p;   // web seeds are listed one by one
        else peers << p;
    }
    // [eng-1] one line that says what BitTorrent does, then where the peers come from: DHT, trackers,
    // PEX, web seeds (from the backend)
    {
        TransferSource sw;
        sw.net = sw.user.net = Net::BitTorrent;
        sw.user.name = QStringLiteral("BitTorrent swarm");
        // no connection text: the Network column says "BT"
        sw.statusText = btSwarmStatus();
        sw.rateBps = sn.rateDownBps;   // delivering: the list's colour for a sending source
        out << sw;
    }
    for (TransferSource c : sn.channels) {
        if (c.net == Net::Unknown) c.net = Net::BitTorrent;
        if (c.user.net == Net::Unknown) c.user.net = c.net;
        out << c;
    }
    out << seeds;
    std::stable_sort(peers.begin(), peers.end(), [](const TransferSource &a, const TransferSource &b) {
        if (a.rateBps != b.rateBps) return a.rateBps > b.rateBps;
        return a.bytes > b.bytes;
    });
    constexpr int kTopPeers = 8;   // a big swarm is a few busy peers plus one summary row
    for (int i = 0; i < peers.size() && i < kTopPeers; ++i) out << peers.at(i);
    if (peers.size() > kTopPeers) {
        TransferSource more;
        more.net = more.user.net = Net::BitTorrent;
        for (int i = kTopPeers; i < peers.size(); ++i) {
            const TransferSource &p = peers.at(i);
            more.bytes += p.bytes;
            more.rateBps += p.rateBps;
        }
        more.user.name = QStringLiteral("+ %1 more peers").arg(peers.size() - kTopPeers);
        more.connection = QStringLiteral("BitTorrent");
        // [core-5] one clean status (original WinMX wording) for the summary row: empty while some of them send (the
        // speed column shows it), "Queued" when most of them keep us choked, else negotiating
        more.statusText = summaryPeerStatus(peers.mid(kTopPeers));
        out << more;
    }
}

QString Download::summaryPeerStatus(const QList<TransferSource> &rest)
{
    int sending = 0, queued = 0;
    for (const TransferSource &p : rest) {
        if (p.rateBps > 0) ++sending;
        else if (p.statusText.startsWith(QLatin1String("Remotely Queued")) || p.statusText == QLatin1String("Queued")) ++queued;
    }
    if (sending > 0) return QString();
    if (!rest.isEmpty() && queued * 2 >= rest.size()) return QStringLiteral("Queued");
    return QStringLiteral("Connected, Negotiating...");
}

void Download::discoveryRows(QList<TransferSource> &out) const
{
    const qint64 now = nowMs();
    QSet<int> listed;
    for (const TransferSource &s : std::as_const(out)) listed.insert(int(s.net));
    QSet<int> nets = m_askedNets;
    for (auto it = m_reportedByNet.constBegin(); it != m_reportedByNet.constEnd(); ++it) nets.insert(it.key());
    QList<int> sorted = nets.values();
    std::sort(sorted.begin(), sorted.end());
    const int afs = effectiveAfs();
    auto nextAsk = [&]() {
        if (afs == 0) return QStringLiteral("Auto Find Sources is off");
        if (afs < 0 || !m_lastFindMs) return QString();
        const qint64 left = m_lastFindMs + qint64(afs) * m_host->tuning().afsMinuteMs - now;
        return left > 60000 ? QStringLiteral("asking again in %1 min").arg((left + 59999) / 60000) : QStringLiteral("asking again in less than a minute");
    };
    for (int n : std::as_const(sorted)) {
        if (listed.contains(n) || n == int(Net::BitTorrent) || n == int(Net::Http) || n == int(Net::Unknown)) continue;
        TransferSource r;
        r.net = r.user.net = Net(n);
        r.user.name = QStringLiteral("%1 network").arg(netName(Net(n)));
        r.connection = netName(Net(n));
        const int reported = m_reportedByNet.value(n);
        const qint64 ago = m_lastAskMs ? (now - m_lastAskMs) / 1000 : -1;
        const bool asking = m_lastAskMs && now - m_lastAskMs < qMin(60000, m_host->tuning().afsMinuteMs);
        // [merge-1] say what happened, never an endless "Waiting for Network Reply..."
        if (reported > 0)
            r.statusText = QStringLiteral("Search hit (%1 source%2): asking for addresses").arg(reported).arg(reported == 1 ? "" : "s");
        else if (asking)
            r.statusText = QStringLiteral("Asked %1 s ago, waiting for answers").arg(qMax<qint64>(0, ago));
        else {
            const QString next = nextAsk();
            r.statusText = QStringLiteral("Asked, nobody has this file yet%1").arg(next.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(next));
        }
        out << r;
    }
    // [merge-1] networks whose copy matched only by name and size
    QList<int> fams = m_candNets.keys();
    std::sort(fams.begin(), fams.end());
    for (int f : std::as_const(fams)) {
        const CandNet &cn = m_candNets[f];
        if (!cn.dropped && !cn.confirmed) continue;
        TransferSource r;
        r.net = r.user.net = Net(f);
        r.user.name = QStringLiteral("%1 copy").arg(netName(Net(f)));
        r.connection = netName(Net(f));
        if (cn.dropped)
            r.statusText = QStringLiteral("The %1 copy is a different file: dropped (%2, %3 fetched from it)")
                               .arg(netName(Net(f)), cn.why, humanBytes(cn.fetched));
        else
            r.statusText = QStringLiteral("The %1 copy is the same file: confirmed by %2").arg(netName(Net(f)), judgeText());
        out << r;
    }
    if (m_nameSearches > 0 || m_nameSearchRunning) {
        TransferSource r;
        r.user.name = QStringLiteral("Search by name and size");
        r.connection = QStringLiteral("Search");
        QStringList ns;
        for (Net n : m_nameSearchNets) ns << netShortName(n);
        const QString what = m_curSubject.file >= 0 ? QStringLiteral(" (%1)").arg(m_curSubject.name) : QString();
        if (m_nameSearchRunning) {
            r.statusText = QStringLiteral("Searching %1 for the same size and name%2 now...").arg(ns.join(QStringLiteral(", ")), what);
        } else {
            QString when;
            if (m_nextNameSearchMs == std::numeric_limits<qint64>::max() || m_nameSearches >= m_host->tuning().nameSearchMax)
                when = QStringLiteral("not again in this session");
            else if (m_nextNameSearchMs > now)
                when = QStringLiteral("next in %1 min%2").arg((m_nextNameSearchMs - now + 59999) / 60000)
                           .arg(lacksSources(now) ? QString() : QStringLiteral(" if the download is still slow"));
            else
                when = lacksSources(now) ? QStringLiteral("again shortly") : QStringLiteral("again when the download slows down");
            r.statusText = QStringLiteral("Searched %1, %2; blocks are checked by %3")
                               .arg(m_nameSearches == 1 ? QStringLiteral("once") : QStringLiteral("%1 times").arg(m_nameSearches), when, judgeText());
        }
        out << r;
        // one row per network: what it gave (or why it was skipped)
        QList<int> keys = m_nameStats.keys();
        std::sort(keys.begin(), keys.end());
        for (int n : std::as_const(keys)) {
            const QString note = nameSearchNote(n, now);
            if (note.isEmpty()) continue;
            TransferSource x;
            x.net = x.user.net = Net(n);
            x.user.name = QStringLiteral("By name and size: %1").arg(netName(Net(n)));
            x.connection = QStringLiteral("Search");
            x.statusText = note;
            out << x;
        }
    }
}

QString Download::discoveryText(qint64 now) const
{
    QMap<int, int> byNet;
    for (const auto &s : m_slots) if (!s->banned && !s->removedByUser) ++byNet[int(s->desc.net)];
    QStringList known;
    int total = 0;
    for (auto it = byNet.constBegin(); it != byNet.constEnd(); ++it) {
        known << QStringLiteral("%1 %2").arg(netShortName(Net(it.key()))).arg(it.value());
        total += it.value();
    }
    QString out = total > 0 ? QStringLiteral("%1 known (%2)").arg(total).arg(known.join(QStringLiteral(", "))) : QStringLiteral("none known yet");
    QStringList asked;
    QList<int> a = m_askedNets.values();
    std::sort(a.begin(), a.end());
    for (int n : std::as_const(a)) asked << netShortName(Net(n));
    if (m_nameSearchRunning) {
        out += QStringLiteral(", searching by name");
    } else if (m_lastAskMs && now - m_lastAskMs < qMin(60000, m_host->tuning().afsMinuteMs) && !asked.isEmpty()) {
        out += QStringLiteral(", asking %1").arg(asked.join(QStringLiteral(", ")));
    } else {
        const int afs = effectiveAfs();
        if (afs > 0 && m_lastFindMs) {
            const qint64 left = m_lastFindMs + qint64(afs) * m_host->tuning().afsMinuteMs - now;
            out += left > 60000 ? QStringLiteral(", next search in %1 min").arg((left + 59999) / 60000) : QStringLiteral(", next search in <1 min");
        } else if (afs == 0) {
            out += QStringLiteral(", Auto Find Sources off");
        }
    }
    return QStringLiteral("- ") + out;
}


// ============================================================================ [up-1] serving a running download

Download::ServeView Download::serveView() const
{
    ServeView v;
    if (isFinished() || m_completing || m_promoting || m_rec.isBundle || m_rec.size <= 0 || m_rec.hashes.isEmpty()) return v;
    if (m_checkFirst) return v;   // [safe-1] nothing of it is served before its check after an unclean stop
    if (m_part) {
        // only units checked against trusted hashes: a provisional eD2k set (from a source nobody
        // vouches for) does not make data servable
        if (m_part->scheme() == PartStore::NoUnits) return v;
        if (m_part->scheme() == PartStore::Ed2kParts && m_ed2kProvisional) return v;
        v.verified = m_part->verified();
        if (v.verified.isEmpty() || m_part->extent() != m_rec.size) return v;
        v.hashes = m_rec.hashes;
        v.size = m_rec.size;
        v.name = m_rec.name;
        v.path = m_part->path();
        v.reader = m_part->verifiedReader();
        if (!m_ed2kProvisional && !m_ed2kParts.isEmpty() && !m_rec.hashes.ed2k.isEmpty()
            && m_ed2kParts.size() == hash::Ed2kHasher::dataPartCount(m_rec.size)) {
            QList<QByteArray> full = m_ed2kParts;
            if (full.size() != hash::Ed2kHasher::partCount(m_rec.size)) full << hash::Ed2kHasher::fileHashFromParts({});   // MD4 of nothing
            if (hash::Ed2kHasher::fileHashFromParts(full) == m_rec.hashes.ed2k) v.ed2kParts = full;
        }
        v.ok = true;
        return v;
    }
    if (m_torrent && m_torrent->hasMetadata()) {
        // libtorrent checked every piece it has against the torrent's piece hashes
        const TorrentSnapshot &sn = m_torrent->snap();
        int idx = m_rec.hashes.btFileIndex;
        if (idx < 0) {
            int real = 0;
            for (int i = 0; i < sn.files.size(); ++i)
                if (!m_torrent->isPadFile(i)) { idx = i; ++real; }
            if (real != 1) return v;
        }
        if (idx < 0 || idx >= sn.files.size() || sn.pieceLength <= 0) return v;
        const TorrentFile &f = sn.files.at(idx);
        if (f.size != m_rec.size || f.priority <= 0) return v;
        const qint64 pl = sn.pieceLength;
        const int p0 = int(f.offset / pl), p1 = int((f.offset + f.size - 1) / pl);
        for (int p = p0; p <= p1 && p < sn.have.size(); ++p) {
            if (!sn.have.testBit(p)) continue;
            const qint64 a = qMax<qint64>(0, qint64(p) * pl - f.offset), b = qMin<qint64>(f.size, qint64(p + 1) * pl - f.offset);
            if (b > a) v.verified.add(a, b);
        }
        if (v.verified.isEmpty()) return v;
        v.hashes = m_rec.hashes;
        v.size = f.size;
        v.name = m_rec.name;
        v.path = QDir(sn.savePath).filePath(f.path);
        v.btFile = idx;
        const QString path = v.path;
        const RangeSet ok = v.verified;   // a snapshot: pieces never stop being verified
        v.reader = [path, ok](qint64 off, qint64 len) -> QByteArray {
            if (len <= 0 || off < 0 || !ok.contains(off, off + len)) return {};
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly) || !file.seek(off)) return {};
            return file.read(qMin<qint64>(len, 16 << 20));
        };
        v.ok = true;
    }
    return v;
}

QStringList Download::busyPaths() const
{
    // [ver-1] a finished file found damaged is not shared as a complete file (nor while it is repaired)
    const bool damagedNow = m_damaged || (m_verify.running && m_verify.finishedFile && m_verify.badUnits > 0);
    if (isFinished()) {
        if (!damagedNow) return {};
        QStringList out;
        if (!m_rec.savePath.isEmpty()) out << m_rec.savePath;
        if (m_verify.torrent && m_torrent && m_torrent->hasMetadata()) {
            const TorrentSnapshot &sn = m_torrent->snap();
            for (const TorrentFile &f : sn.files) out << QDir(sn.savePath).filePath(f.path);
        }
        return out;
    }
    QStringList out;
    if (!m_repairPath.isEmpty()) out << m_repairPath;   // [ver-1] also before its part store exists
    if (m_part) out << m_part->path();
    if (m_torrent || !m_torrentId.isEmpty() || !m_promoteTorrent.isEmpty()) {
        if (!m_rec.savePath.isEmpty()) out << m_rec.savePath;
        if (m_torrent && m_torrent->hasMetadata()) {
            const TorrentSnapshot &sn = m_torrent->snap();
            for (const TorrentFile &f : sn.files) out << QDir(sn.savePath).filePath(f.path);
        }
    }
    return out;
}

// ============================================================================ [ver-1] Verify File

namespace {
QString quotedName(const QString &n) { return QStringLiteral("'%1'").arg(n); }
bool anyChecksum(const FileHashes &h)
{
    return !h.ed2k.isEmpty() || !h.sha1.isEmpty() || !h.sha256.isEmpty() || !h.md5.isEmpty() || !h.tth.isEmpty() || !h.aich.isEmpty();
}
} // namespace

// A download whose data is a torrent's (not a part file): the backend checks it
static bool torrentData(bool hasTorrentStore, bool hasPart, const QString &partPath, const QString &torrentId)
{
    return hasTorrentStore || (!hasPart && partPath.isEmpty() && !torrentId.isEmpty());
}

bool Download::canVerify(QString *why) const
{
    auto no = [why](const QString &t) {
        if (why) *why = t;
        return false;
    };
    const TransferState st = TransferState(m_rec.state);
    if (m_verify.running) return no(QStringLiteral("It is being verified now."));
    if (m_restartPending) return no(QStringLiteral("It is starting again from zero now."));
    if (st == TransferState::Cancelled) return no(QStringLiteral("It was cancelled."));
    if (m_completing) return no(QStringLiteral("It is being finished now; try again in a moment."));
    if (m_promoting) return no(QStringLiteral("A torrent is taking it over now; try again in a moment."));
    if (!m_movingTo.isEmpty()) return no(QStringLiteral("Its files are being moved now; try again in a moment."));
    if (m_repairWhole && m_awaitSetsUntil) return no(QStringLiteral("It is damaged and waits for block hashes from the networks to find the damage."));
    if (torrentData(m_torrent != nullptr, m_part != nullptr, m_partPath, m_torrentId)) {
        TorrentBackend *be = m_host->backend();
        if (!be || !be->torrents().contains(m_torrentId))
            return no(QStringLiteral("The torrent is no longer in the BitTorrent session; Restart Download fetches it again."));
        const TorrentSnapshot sn = m_torrent ? m_torrent->snap() : be->snapshot(m_torrentId);
        if (!sn.hasMetadata) return no(QStringLiteral("Its file list (with the piece hashes) has not arrived yet."));
        if (sn.state == TransferState::Verifying) return no(QStringLiteral("BitTorrent is checking it now."));
        return true;
    }
    if (isFinished()) {
        if (m_rec.savePath.isEmpty()) return no(QStringLiteral("Where its file is, is not known."));
        return true;
    }
    if (!m_pendingTorrentUrl.isEmpty()) return no(QStringLiteral("Nothing of it has been received yet."));
    const qint64 held = m_part ? m_part->heldBytes() : m_restoreHeld.total();
    if (held <= 0) return no(QStringLiteral("Nothing of it has been received yet."));
    return true;
}

void Download::setVerifyNote(const QString &text, bool sticky)
{
    m_verifyNote = text;
    m_verifyNoteUntilMs = sticky ? 0 : nowMs() + 20000;
}

QString Download::verifyStatusText() const
{
    if (m_verify.total <= 0) return QStringLiteral("Verifying...");
    const int pct = int(qBound<qint64>(0, m_verify.done * 100 / m_verify.total, qint64(100)));
    QString t = QStringLiteral("Verifying %1%...").arg(pct);
    if (m_verify.badUnits > 0)
        t += QStringLiteral(" (%1 damaged so far)").arg(countNoun(m_verify.badUnits, m_verify.unitWord.isEmpty() ? QStringLiteral("part") : m_verify.unitWord));
    return t;
}

bool Download::startVerify()
{
    QString why;
    if (!canVerify(&why)) return false;
    m_verify = VerifyRun();
    m_verify.startedMs = nowMs();
    m_verifyNote.clear();
    m_verifyNoteUntilMs = 0;
    const QString n = quotedName(m_rec.name);
    auto answer = [this](const QString &notice, const QString &status) {
        // nothing is changed: the answer only; [safe-1] the check after an unclean stop ends here too
        const bool firstRun = m_checkFirstRun;
        m_checkFirstRun = false;
        if (firstRun) {
            setCheckFirst(false);
            qCInfo(lcEngine).noquote() << m_rec.id << "check after an unclean stop:" << notice;
            persist(true);
            m_host->finished(this);
            markDirty(true);
            return true;
        }
        if (isFinished() && !status.isEmpty()) setState(TransferState(m_rec.state), status);
        else if (!isFinished()) {
            setVerifyNote(status, m_userPaused);
            updateStatus();
        }
        persist(true);
        m_host->notice(this, notice);
        markDirty(true);
        return true;
    };
    if (torrentData(m_torrent != nullptr, m_part != nullptr, m_partPath, m_torrentId)) return startTorrentVerify();
    if (isFinished()) {
        const QString path = m_rec.savePath;
        const QFileInfo fi(path);
        if (m_noChecksum || (!anyChecksum(m_rec.hashes) && m_ed2kParts.isEmpty() && m_tthLeaves.isEmpty()))
            return answer(QStringLiteral("%1 cannot be verified: no checksum is known for this file.").arg(n),
                          QStringLiteral("%1, cannot be verified: no checksum known").arg(transferStateText(TransferState::Completed)));
        if (!fi.exists() || !fi.isFile())
            return answer(QStringLiteral("Cannot verify %1: the finished file is no longer at %2 (moved, renamed or deleted). Nothing was "
                                         "changed; Restart Download fetches it again.").arg(n, QDir::toNativeSeparators(path)),
                          QStringLiteral("%1, but the file is gone: Restart Download fetches it again").arg(transferStateText(TransferState::Completed)));
        if (m_rec.size > 0 && fi.size() != m_rec.size)
            return answer(QStringLiteral("Cannot verify %1: the file at %2 was changed (it has %3, the download %4). Nothing was changed; "
                                         "Restart Download fetches it again.")
                              .arg(n, QDir::toNativeSeparators(path), humanBytes(fi.size()), humanBytes(m_rec.size)),
                          QStringLiteral("%1, but the file was changed: Restart Download fetches it again").arg(transferStateText(TransferState::Completed)));
        m_verify.running = true;
        m_verify.finishedFile = true;
        qCInfo(lcEngine) << m_rec.id << "Verify File: the finished file" << path;
        beginFileCheck();
        markDirty(true);
        return true;
    }
    if (!anyChecksum(m_rec.hashes) && m_ed2kParts.isEmpty() && m_tthLeaves.isEmpty())
        return answer(QStringLiteral("%1 cannot be verified: no checksum is known for this file.").arg(n),
                      QStringLiteral("cannot be verified: no checksum known"));
    m_verify.running = true;
    m_verify.resumeAfter = m_active;
    qCInfo(lcEngine) << m_rec.id << "Verify File: the partial file" << (m_active ? "(running: stops fetching for the check)" : "");
    if (m_active) deactivate();
    if (!m_part) createPartStore();
    m_part->afterWrites([this] {
        if (m_verify.running && !m_verifyJob) beginPartCheck();
    });
    markDirty(true);
    return true;
}

// the block hashes of a finished file, the finest trusted ones
QList<VerifyJob::Block> Download::verifyBlocks(QString *unitWord) const
{
    QList<VerifyJob::Block> out;
    const qint64 size = m_rec.size;
    if (size <= 0) return out;
    bool ed2kOk = !m_ed2kParts.isEmpty() && m_ed2kParts.size() == hash::Ed2kHasher::dataPartCount(size);
    if (ed2kOk && !m_rec.hashes.ed2k.isEmpty()) {
        QList<QByteArray> full = m_ed2kParts;
        if (full.size() != hash::Ed2kHasher::partCount(size)) full << hash::Ed2kHasher::fileHashFromParts({});
        ed2kOk = hash::Ed2kHasher::fileHashFromParts(full) == m_rec.hashes.ed2k
                 || hash::Ed2kHasher::fileHashFromParts(m_ed2kParts) == m_rec.hashes.ed2k;
    } else if (ed2kOk && m_ed2kProvisional) {
        ed2kOk = false;   // a set nobody vouches for does not judge the user's file
    }
    const bool tthOk = !m_tthLeaves.isEmpty() && m_tthUnit > 0 && !m_rec.hashes.tth.isEmpty()
                       && m_tthLeaves.size() == qMax<qint64>(1, (size + m_tthUnit - 1) / m_tthUnit)
                       && hash::TigerTree::rootFromLevel(m_tthLeaves) == m_rec.hashes.tth;
    const bool single = !m_rec.hashes.ed2k.isEmpty() && size < PartStore::Ed2kPart;
    if (tthOk && (!ed2kOk || m_tthUnit < PartStore::Ed2kPart)) {
        *unitWord = QStringLiteral("block");
        for (int i = 0; i < m_tthLeaves.size(); ++i) {
            VerifyJob::Block b;
            b.start = qint64(i) * m_tthUnit;
            b.end = qMin(size, b.start + m_tthUnit);
            b.expect = m_tthLeaves.at(i);
            b.kind = VerifyJob::Tiger;
            out << b;
        }
    } else if (ed2kOk) {
        *unitWord = QStringLiteral("part");
        for (int i = 0; i < m_ed2kParts.size(); ++i) {
            VerifyJob::Block b;
            b.start = qint64(i) * PartStore::Ed2kPart;
            b.end = qMin(size, b.start + PartStore::Ed2kPart);
            b.expect = m_ed2kParts.at(i);
            b.kind = VerifyJob::Md4;
            if (b.end > b.start) out << b;
        }
    } else if (single) {
        *unitWord = QStringLiteral("part");
        VerifyJob::Block b;
        b.start = 0;
        b.end = size;
        b.expect = m_rec.hashes.ed2k;
        b.kind = VerifyJob::Md4;
        out << b;
    }
    return out;
}

void Download::beginFileCheck()
{
    QString word;
    const QList<VerifyJob::Block> blocks = verifyBlocks(&word);
    m_verifyJob = std::make_unique<VerifyJob>(m_host->pool(), m_rec.savePath);
    connect(m_verifyJob.get(), &VerifyJob::blockChecked, this, &Download::onVerifyBlock);
    connect(m_verifyJob.get(), &VerifyJob::wholeChecked, this, &Download::onVerifyWhole);
    connect(m_verifyJob.get(), &VerifyJob::finished, this, &Download::onVerifyFinished);
    connect(m_verifyJob.get(), &VerifyJob::progress, this, [this](qint64 done, qint64) {
        m_verify.done = done;
        markDirty();
    });
    if (!blocks.isEmpty()) {
        m_verify.unitWord = word;
        for (const VerifyJob::Block &b : blocks) m_verify.total += b.end - b.start;
        m_verifyJob->checkBlocks(blocks, m_host->tuning().verifyBlockPauseMs);
    } else {
        m_verify.whole = true;
        m_verify.total = m_rec.size;
        m_verifyJob->checkWhole(m_rec.hashes, PartStore::keptBlockUnit(m_rec.size));
    }
}

void Download::beginPartCheck()
{
    if (!m_part) return;
    const QString n = quotedName(m_rec.name);
    if (m_part->scheme() == PartStore::NoUnits || (m_part->scheme() == PartStore::Ed2kParts && m_ed2kProvisional)) {
        const QString what = m_part->scheme() == PartStore::NoUnits ? QStringLiteral("only a checksum of the whole file is known")
                                                                    : QStringLiteral("only a block hash set nobody vouches for is known");
        endVerify(QStringLiteral("%1 cannot be verified yet: %2, and the whole file is checked when it is complete.").arg(n, what),
                  QStringLiteral("cannot be verified before it is complete"));
        return;
    }
    QList<VerifyJob::Block> blocks;
    const QList<Range> units = m_part->completeUnits();
    const VerifyJob::Kind kind = m_part->scheme() == PartStore::Ed2kParts ? VerifyJob::Md4 : VerifyJob::Tiger;
    for (const Range &u : units) {
        VerifyJob::Block b;
        b.start = u.first;
        b.end = u.second;
        b.expect = m_part->unitHash(u.first);
        b.kind = kind;
        blocks << b;
    }
    // units with some bytes but not all: they cannot be checked (they keep their bytes)
    const RangeSet held = m_part->held();
    for (qint64 pos = 0; pos < m_part->size(); pos += m_part->unitSize()) {
        const Range u = m_part->unitAt(pos);
        if (u.second <= u.first) break;
        if (held.intersects(u.first, u.second) && !held.contains(u.first, u.second)) ++m_verify.partialUnits;
    }
    m_verify.unitWord = m_part->scheme() == PartStore::Ed2kParts ? QStringLiteral("part") : QStringLiteral("block");
    if (blocks.isEmpty()) {
        endVerify(QStringLiteral("Nothing of %1 can be checked yet: no %2 has arrived completely (a %2 is checked once all of it is there).")
                      .arg(n, m_verify.unitWord),
                  QStringLiteral("nothing complete to check yet"));
        return;
    }
    for (const VerifyJob::Block &b : std::as_const(blocks)) m_verify.total += b.end - b.start;
    m_verifyJob = std::make_unique<VerifyJob>(m_host->pool(), m_part->path());
    connect(m_verifyJob.get(), &VerifyJob::blockChecked, this, &Download::onVerifyBlock);
    connect(m_verifyJob.get(), &VerifyJob::finished, this, &Download::onVerifyFinished);
    connect(m_verifyJob.get(), &VerifyJob::progress, this, [this](qint64 done, qint64) {
        m_verify.done = done;
        markDirty();
    });
    m_verifyJob->checkBlocks(blocks, m_host->tuning().verifyBlockPauseMs);
    markDirty(true);
}

void Download::onVerifyBlock(qint64 s, qint64 e, bool ok, bool readable)
{
    if (!m_verify.running) return;
    const bool good = ok && readable;
    if (!good) {
        ++m_verify.badUnits;
        m_verify.badBytes += e - s;
        qCInfo(lcEngine) << m_rec.id << "Verify File:" << m_verify.unitWord << s << "-" << e << (readable ? "is damaged" : "cannot be read");
    }
    if (m_verify.finishedFile) {
        if (!readable && !QFileInfo::exists(m_rec.savePath)) {   // the file went away during the check
            m_verify.bad = m_verify.good = RangeSet();
            m_verify.badUnits = 0;
            if (m_verifyJob) m_verifyJob->stop();
            endVerify(QStringLiteral("Cannot verify %1: the finished file is no longer at %2. Nothing was changed; Restart Download "
                                     "fetches it again.").arg(quotedName(m_rec.name), QDir::toNativeSeparators(m_rec.savePath)),
                      QStringLiteral("%1, but the file is gone: Restart Download fetches it again").arg(transferStateText(TransferState::Completed)));
            return;
        }
        (good ? m_verify.good : m_verify.bad).add(s, e);
        if (!good && m_verify.badUnits == 1) m_host->sharingChanged();   // not shared as a complete file from now on
    } else if (m_part) {
        // what was read has its new status at once (a Pause or a crash keeps it): a damaged block is
        // no longer held, so it is fetched again; nobody is blamed, the damage is the disk's
        m_part->recheckResult(s, e, good);
        m_suspect.remove(s, e);   // [safe-1] checked: proven good, or fetched again
        if (!good) {
            for (auto &sp : m_slots) sp->contributed.remove(s, e);
            persist(false);
        }
    }
    markDirty();
}

void Download::onVerifyFinished(bool stopped)
{
    Q_UNUSED(stopped)
    if (!m_verify.running || m_verify.whole || m_verify.torrent) return;
    const QString n = quotedName(m_rec.name);
    const QString word = m_verify.unitWord.isEmpty() ? QStringLiteral("part") : m_verify.unitWord;
    const QString damage = QStringLiteral("%1 of %2 (%3)").arg(countNoun(m_verify.badUnits, word), n, humanBytes(m_verify.badBytes));
    const bool stop = m_verify.stopped;
    const qint64 checked = m_verify.done;
    if (m_verify.finishedFile) {
        if (m_verify.badUnits == 0) {
            if (stop) {
                endVerify(QStringLiteral("Verification of %1 stopped: the %2 checked so far are intact; nothing was changed.").arg(n, humanBytes(checked)),
                          transferStateText(TransferState::Completed));
            } else {
                m_damaged = false;
                endVerify(QStringLiteral("Verified: %1 is intact.").arg(n),
                          QStringLiteral("%1, verified intact").arg(transferStateText(TransferState::Completed)));
            }
            return;
        }
        const RangeSet bad = m_verify.bad, good = m_verify.good;
        const QString found = QStringLiteral("%1 (%2)").arg(countNoun(m_verify.badUnits, word), humanBytes(m_verify.badBytes));
        const QString text = stop ? QStringLiteral("Verification of %1 stopped: %2 %3 damaged; %4 downloaded again when you resume it.")
                                        .arg(n, found, m_verify.badUnits == 1 ? QStringLiteral("is") : QStringLiteral("are"),
                                             m_verify.badUnits == 1 ? QStringLiteral("it will be") : QStringLiteral("they will be"))
                                  : QStringLiteral("Verified: %1 %2 damaged; %3 being downloaded again.")
                                        .arg(damage, m_verify.badUnits == 1 ? QStringLiteral("is") : QStringLiteral("are"),
                                             m_verify.badUnits == 1 ? QStringLiteral("it is") : QStringLiteral("they are"));
        enterRepair(bad, good, stop, false);
        endVerify(text, QString());
        return;
    }
    // a partial file
    if (m_verify.badUnits == 0) {
        QString text;
        if (stop) text = QStringLiteral("Verification of %1 stopped: the %2 checked so far are intact.").arg(n, humanBytes(checked));
        else if (m_part && m_part->fullyHeld()) text = QStringLiteral("Verified: %1 is intact.").arg(n);
        else text = QStringLiteral("Verified: the %1 of %2 received so far are intact.").arg(humanBytes(checked), n);
        if (m_verify.partialUnits > 0)
            text += QStringLiteral(" %1 that %2 not arrived completely cannot be checked yet.")
                        .arg(countNoun(m_verify.partialUnits, word), m_verify.partialUnits == 1 ? QStringLiteral("has") : QStringLiteral("have"));
        endVerify(text, QStringLiteral("intact"));
        return;
    }
    const bool paused = m_userPaused;
    const QString verb = m_verify.badUnits == 1 ? QStringLiteral("was") : QStringLiteral("were");
    const QString text = paused ? QStringLiteral("Verified: %1 %2 damaged; %3 downloaded again when you resume it.")
                                      .arg(damage, verb, m_verify.badUnits == 1 ? QStringLiteral("it will be") : QStringLiteral("they will be"))
                                : QStringLiteral("Verified: %1 %2 damaged; %3 being downloaded again.")
                                      .arg(damage, verb, m_verify.badUnits == 1 ? QStringLiteral("it is") : QStringLiteral("they are"));
    endVerify(text, paused ? QStringLiteral("%1 damaged, downloaded again on resume").arg(countNoun(m_verify.badUnits, word))
                           : QStringLiteral("%1 damaged, downloading again").arg(countNoun(m_verify.badUnits, word)));
}

void Download::onVerifyWhole(const VerifyJob::Whole &w)
{
    if (!m_verify.running || !m_verify.whole) return;
    const QString n = quotedName(m_rec.name);
    if (!w.readable) {
        endVerify(QStringLiteral("Cannot verify %1: the file cannot be read (%2). Nothing was changed.").arg(n, w.error),
                  QStringLiteral("%1, but its file cannot be read").arg(transferStateText(TransferState::Completed)));
        return;
    }
    if (w.mismatch.isEmpty()) {
        // an old record without block hashes: the checked file gives them, from now on
        if (!m_rec.hashes.conflicts(w.hashes)) {
            keepBlockHashes(PartStore::keptBlockUnit(m_rec.size), w.tthLevel, w.ed2kParts, w.hashes);
            FileHashes h = w.hashes;
            h.btih.clear();
            h.btFileIndex = -1;
            m_rec.hashes.mergeFrom(h);
        }
        m_damaged = false;
        endVerify(QStringLiteral("Verified: %1 is intact.").arg(n), QStringLiteral("%1, verified intact").arg(transferStateText(TransferState::Completed)));
        return;
    }
    qCWarning(lcEngine) << m_rec.id << "Verify File: the whole file is damaged (" << w.mismatch << "), no block hashes: asking the networks";
    m_verify.badUnits = 1;
    enterRepair(RangeSet(), RangeSet(), false, true);
    endVerify(QStringLiteral("Verified: %1 is damaged (its %2 checksum differs), and only checksums of the whole file are known, so the damage "
                             "cannot be located yet. OctoMX asks the networks for block hashes (an eD2k part hash set, a tiger tree) to "
                             "download only the damaged parts again; if none can be had, Restart Download fetches it again.")
                  .arg(n, w.mismatch.join(QStringLiteral(", "))),
              QString());
}

void Download::endVerify(const QString &text, const QString &status)
{
    m_verify.running = false;
    // [safe-1] the check after an unclean stop: the flag goes only when it ran to its end; no notice of
    // its own (the program says once that the downloads are checked), the status text tells the outcome
    const bool firstRun = m_checkFirstRun;
    m_checkFirstRun = false;
    if (firstRun && !m_verify.stopped) setCheckFirst(false);
    if (m_verifyJob) {
        disconnect(m_verifyJob.get(), nullptr, this, nullptr);
        m_verifyJob->stop();
        m_verifyJob.release()->deleteLater();   // may be inside one of its signals
    }
    if (isFinished()) {
        if (!status.isEmpty()) setState(TransferState(m_rec.state), status);
    } else {
        if (!status.isEmpty()) setVerifyNote(status, m_userPaused);
        updateStatus();
    }
    qCInfo(lcEngine).noquote() << m_rec.id << (firstRun ? "check after an unclean stop:" : "Verify File:") << text;
    persist(true);
    if (!firstRun) m_host->notice(this, text);
    m_host->sharingChanged();
    m_host->finished(this);   // a download that was running goes on (rebalance)
    markDirty(true);
}

void Download::stopVerify()
{
    if (!m_verify.running) return;
    m_verify.stopped = true;
    if (m_verify.torrent) {
        finishTorrentVerify(true);
        return;
    }
    if (m_verifyJob) m_verifyJob->stop();
    if (m_verify.whole || !m_verifyJob) {
        endVerify(QStringLiteral("Verification of %1 stopped; nothing was changed.").arg(quotedName(m_rec.name)),
                  isFinished() ? transferStateText(TransferState::Completed) : QString());
        return;
    }
    onVerifyFinished(true);
}

void Download::keepBlockHashes(qint64 tthUnit, const QList<QByteArray> &tthLevel, const QList<QByteArray> &ed2kParts, const FileHashes &file)
{
    if (!m_ed2kParts.isEmpty() || !m_tthLeaves.isEmpty() || m_rec.size <= 0) return;
    const qint64 expect = qMax<qint64>(1, (m_rec.size + qMax<qint64>(1, tthUnit) - 1) / qMax<qint64>(1, tthUnit));
    if (tthUnit > 0 && tthLevel.size() == expect && !file.tth.isEmpty() && hash::TigerTree::rootFromLevel(tthLevel) == file.tth) {
        m_tthUnit = tthUnit;
        m_tthLeaves = tthLevel;
        qCInfo(lcEngine) << m_rec.id << "kept the block hashes of the checked file:" << tthLevel.size() << "x" << tthUnit;
        return;
    }
    const int full = hash::Ed2kHasher::partCount(m_rec.size), data = hash::Ed2kHasher::dataPartCount(m_rec.size);
    if (data > 1 && (ed2kParts.size() == full || ed2kParts.size() == data) && !file.ed2k.isEmpty()
        && hash::Ed2kHasher::fileHashFromParts(ed2kParts) == file.ed2k) {
        m_ed2kParts = ed2kParts.mid(0, data);
        m_ed2kProvisional = false;
        qCInfo(lcEngine) << m_rec.id << "kept the eD2k part hashes of the checked file:" << data;
    }
}

// A finished file with damage becomes an unfinished download again, repaired where it is: the damaged
// blocks are missing (fetched again from the sources and a source search), the good ones verified.
// awaitSets: damage only the whole-file checksum shows: everything held, nothing verified, block hashes
// are asked for first (giveUpRepair when none come).
void Download::enterRepair(const RangeSet &bad, const RangeSet &good, bool paused, bool awaitSets)
{
    const qint64 size = m_rec.size;
    m_repairPath = m_rec.savePath;
    m_partPath = m_repairPath;
    m_restoreHeld = RangeSet(0, size);
    m_restoreHeld.remove(bad);
    m_restoreVerified = good;
    m_repairWhole = awaitSets;
    m_awaitSetsUntil = 0;
    if (m_part) {
        disconnect(m_part.get(), nullptr, this, nullptr);
        m_part->abandon();
        m_part.reset();
    }
    m_rec.completed = QDateTime();
    m_rec.done = good.total();
    m_completionNote.clear();
    m_damaged = false;
    m_rec.errorText.clear();
    m_errorState = false;
    m_userPaused = paused;
    m_active = false;
    for (auto &sp : m_slots) sp->contributed.clear();
    m_isolated.clear();
    m_unitOwner.clear();
    m_failedBy.clear();
    m_blackBox.clear();
    m_deadEnd = m_deadEndBanned = false;
    m_wholeFileFailures = 0;
    m_awaitHashSetUntil = 0;
    m_lastFindMs = 0;
    m_rec.state = int(paused ? TransferState::Paused : TransferState::Queued);
    m_rec.statusText = transferStateText(TransferState(m_rec.state));
    if (!awaitSets)
        setVerifyNote(QStringLiteral("%1 damaged, %2").arg(countNoun(m_verify.badUnits, m_verify.unitWord.isEmpty() ? QStringLiteral("part") : m_verify.unitWord),
                                                           paused ? QStringLiteral("downloaded again on resume") : QStringLiteral("downloading again")),
                      paused);
    qCInfo(lcEngine) << m_rec.id << "repair in place:" << m_repairPath << "missing" << bad.total() << "bytes," << (awaitSets ? "waiting for block hashes" : "");
    persist(true);
}

void Download::giveUpRepair(const QString &why)
{
    qCWarning(lcEngine) << m_rec.id << "repair given up:" << why;
    closeAllSources(QStringLiteral("done"));
    stopFindSources();
    m_active = false;
    if (m_part) {
        disconnect(m_part.get(), nullptr, this, nullptr);
        m_part->close();
        m_part.reset();
    }
    m_repairPath.clear();
    m_repairWhole = false;
    m_awaitSetsUntil = 0;
    m_partPath.clear();
    m_restoreHeld = m_restoreVerified = RangeSet();
    m_damaged = true;
    m_userPaused = false;
    m_rec.completed = QDateTime::currentDateTime();
    m_rec.done = m_rec.size;
    setState(TransferState::Completed, QStringLiteral("%1, but damaged: Restart Download fetches it again").arg(transferStateText(TransferState::Completed)));
    persist(true);
    m_host->notice(this, QStringLiteral("%1 is damaged, and %2, so nothing can be repaired; the file is not shared. Restart Download fetches it again.")
                             .arg(quotedName(m_rec.name), why));
    m_host->sharingChanged();
    m_host->finished(this);
    markDirty(true);
}

// ---- torrents: the backend checks every piece; the engine compares what it had before

bool Download::startTorrentVerify()
{
    TorrentBackend *be = m_host->backend();
    if (!be) return false;
    if (!m_torrent) {
        m_torrent = std::make_unique<TorrentStore>(be, m_torrentId, m_host->pool(), &m_host->tuning());
        attachStore(m_torrent.get());
    }
    m_torrent->refresh();
    const TorrentSnapshot sn = m_torrent->snap();
    const QString n = quotedName(m_rec.name);
    if (isFinished()) {
        QStringList gone, changed;
        for (int i = 0; i < sn.files.size(); ++i) {
            const TorrentFile &f = sn.files.at(i);
            if (m_torrent->isPadFile(i) || f.priority <= 0) continue;
            const QFileInfo fi(QDir(sn.savePath).filePath(f.path));
            if (!fi.isFile()) gone << QDir::toNativeSeparators(fi.filePath());
            else if (fi.size() != f.size) changed << QDir::toNativeSeparators(fi.filePath());
        }
        if (!gone.isEmpty() || !changed.isEmpty()) {
            const QString what = !gone.isEmpty() ? QStringLiteral("%1 no longer there (%2)").arg(gone.size() == 1 ? QStringLiteral("its file is") : QStringLiteral("files of it are"), gone.first())
                                                 : QStringLiteral("%1 changed (%2)").arg(changed.size() == 1 ? QStringLiteral("its file was") : QStringLiteral("files of it were"), changed.first());
            setState(TransferState(m_rec.state), QStringLiteral("%1, but %2: Restart Download fetches it again").arg(transferStateText(TransferState::Completed),
                                                                                                                       gone.isEmpty() ? QStringLiteral("a file was changed") : QStringLiteral("a file is gone")));
            persist(true);
            m_host->notice(this, QStringLiteral("Cannot verify %1: %2. Nothing was changed; Restart Download fetches it again.").arg(n, what));
            markDirty(true);
            return true;
        }
    }
    m_verify.running = true;
    m_verify.torrent = true;
    m_verify.finishedFile = isFinished();
    m_verify.unitWord = QStringLiteral("piece");
    m_verify.haveBefore = sn.have;
    m_verify.checksBefore = sn.checksDone;
    m_verify.total = sn.totalSize;
    m_verify.resumeAfter = m_active;
    qCInfo(lcEngine) << m_rec.id << "Verify File: torrent" << m_torrentId.left(8) << "rechecked by the backend;" << sn.have.count(true)
                     << "of" << sn.numPieces << "pieces," << sn.totalDone << "bytes before";
    if (m_active) deactivate();
    else m_torrent->releaseAll();
    be->recheck(m_torrentId);
    QTimer::singleShot(0, this, [this] {
        if (m_verify.running && m_verify.torrent) pollTorrentVerify();
    });
    markDirty(true);
    return true;
}

void Download::pollTorrentVerify()
{
    if (!m_torrent || !m_verify.running) return;
    m_torrent->refresh();
    const TorrentSnapshot &sn = m_torrent->snap();
    if (sn.checkPpm >= 0) m_verify.done = m_verify.total * qint64(sn.checkPpm) / 1000000;
    if (sn.checksDone > m_verify.checksBefore && sn.state != TransferState::Verifying) {
        finishTorrentVerify(false);
        return;
    }
    markDirty();
}

void Download::finishTorrentVerify(bool stopped)
{
    TorrentBackend *be = m_host->backend();
    const QString n = quotedName(m_rec.name);
    if (stopped) {
        // libtorrent forgot what it had when the check began and checks the rest when it runs again
        if (be) be->pauseTorrent(m_torrentId);
        if (isFinished()) {
            m_rec.completed = QDateTime();
            m_rec.state = int(TransferState::Paused);
            m_userPaused = true;
        }
        m_verify.running = false;
        endVerify(QStringLiteral("Verification of %1 stopped: BitTorrent checks the rest when you resume it (it does not keep the earlier result "
                                 "of the pieces it has not read again yet).").arg(n),
                  QStringLiteral("check goes on when resumed"));
        return;
    }
    const TorrentSnapshot &sn = m_torrent->snap();
    const qint64 pl = sn.pieceLength;
    const qint64 payload = m_torrent->payloadSize();
    auto wanted = [&sn, pl, this](int p) {
        const qint64 s = qint64(p) * pl, e = s + pl;
        for (int i = 0; i < sn.files.size(); ++i) {
            const TorrentFile &f = sn.files.at(i);
            if (f.priority <= 0 || m_torrent->isPadFile(i)) continue;
            if (f.offset < e && f.offset + f.size > s) return true;
        }
        return false;
    };
    int lost = 0;
    qint64 lostBytes = 0;
    for (int p = 0; p < m_verify.haveBefore.size() && p < sn.have.size(); ++p) {
        if (!m_verify.haveBefore.testBit(p) || sn.have.testBit(p) || !wanted(p)) continue;
        ++lost;
        lostBytes += qMin(pl, payload - qint64(p) * pl);
    }
    m_verify.badUnits = lost;
    m_verify.badBytes = lostBytes;
    qCInfo(lcEngine) << m_rec.id << "Verify File: torrent check done," << lost << "pieces lost," << lostBytes << "bytes;" << sn.have.count(true)
                     << "pieces," << sn.totalDone << "bytes now";
    if (lost == 0) {
        const bool all = sn.finished || m_verify.finishedFile;
        endVerify(all ? QStringLiteral("Verified: %1 is intact.").arg(n)
                      : QStringLiteral("Verified: the %1 of %2 received so far are intact.").arg(humanBytes(sn.totalDone), n),
                  m_verify.finishedFile ? QStringLiteral("%1, verified intact").arg(transferStateText(TransferState::Completed)) : QStringLiteral("intact"));
        return;
    }
    const QString damage = QStringLiteral("%1 of %2 (%3)").arg(countNoun(lost, QStringLiteral("piece")), n, humanBytes(lostBytes));
    const QString verb = lost == 1 ? QStringLiteral("was") : QStringLiteral("were");
    if (m_verify.finishedFile) {
        // unfinished again: the swarm (and the other networks, through the engine) fetch the pieces
        m_rec.completed = QDateTime();
        m_rec.state = int(TransferState::Queued);
        m_rec.statusText = transferStateText(TransferState::Queued);
        m_userPaused = false;
        m_damaged = false;
        setVerifyNote(QStringLiteral("%1 damaged, downloading again").arg(countNoun(lost, QStringLiteral("piece"))), false);
        endVerify(QStringLiteral("Verified: %1 %2 damaged; %3 being downloaded again.").arg(damage, verb, lost == 1 ? QStringLiteral("it is") : QStringLiteral("they are")),
                  QString());
        return;
    }
    const bool paused = m_userPaused;
    endVerify(paused ? QStringLiteral("Verified: %1 %2 damaged; %3 downloaded again when you resume it.").arg(damage, verb, lost == 1 ? QStringLiteral("it will be") : QStringLiteral("they will be"))
                     : QStringLiteral("Verified: %1 %2 damaged; %3 being downloaded again.").arg(damage, verb, lost == 1 ? QStringLiteral("it is") : QStringLiteral("they are")),
              QStringLiteral("%1 damaged, %2").arg(countNoun(lost, QStringLiteral("piece")), paused ? QStringLiteral("downloaded again on resume") : QStringLiteral("downloading again")));
}

// ============================================================================ [ver-1] Restart Download

bool Download::canRestart(QString *why) const
{
    auto no = [why](const QString &t) {
        if (why) *why = t;
        return false;
    };
    if (m_restartPending) return no(QStringLiteral("It is starting again from zero now."));
    if (TransferState(m_rec.state) == TransferState::Cancelled) return no(QStringLiteral("It was cancelled."));
    if (m_completing) return no(QStringLiteral("It is being finished now; try again in a moment."));
    if (!m_movingTo.isEmpty()) return no(QStringLiteral("Its files are being moved now; try again in a moment."));
    return true;
}

DownloadRecord Download::restartRecord() const
{
    DownloadRecord r;
    r.id = m_rec.id;
    r.name = m_rec.name;
    r.size = m_rec.size;
    // the hashes of a file that had no checksum came from that data: not trusted
    r.hashes = m_noChecksum ? FileHashes() : m_rec.hashes;
    r.uri = m_rec.uri;
    r.saveDir = m_rec.saveDir;
    r.wantedFiles = m_rec.wantedFiles;
    r.priority = m_rec.priority;
    r.isBundle = m_rec.isBundle;
    r.files = m_rec.files;
    for (BundleFile &b : r.files) b.done = 0;
    r.uploaded = m_rec.uploaded;
    r.added = m_rec.added;
    r.sortOrder = m_rec.sortOrder;
    r.state = int(TransferState::Queued);
    r.statusText = transferStateText(TransferState::Queued);
    // a torrent's place comes from its metadata again; a part file's when it completes
    if (torrentData(m_torrent != nullptr, m_part != nullptr, m_partPath, m_torrentId)) r.savePath = m_rec.savePath;
    return r;
}

QByteArray Download::restartBlob(bool keepTorrent) const
{
    QVariantMap m;
    m.insert(QStringLiteral("v"), 1);
    const bool torrent = torrentData(m_torrent != nullptr, m_part != nullptr, m_partPath, m_torrentId);
    m.insert(QStringLiteral("store"), torrent ? QStringLiteral("torrent") : QStringLiteral("part"));
    if (torrent && keepTorrent && !m_torrentId.isEmpty()) m.insert(QStringLiteral("torrentId"), m_torrentId);
    // what is known about the file (not about the data): the trusted block hashes, proven-wrong claims
    if (!m_noChecksum) {
        QVariantList parts, leaves;
        if (!m_ed2kProvisional)
            for (const QByteArray &b : m_ed2kParts) parts << b;
        for (const QByteArray &b : m_tthLeaves) leaves << b;
        if (!parts.isEmpty()) m.insert(QStringLiteral("ed2kParts"), parts);
        if (!leaves.isEmpty()) {
            m.insert(QStringLiteral("tthUnit"), m_tthUnit);
            m.insert(QStringLiteral("tthLeaves"), leaves);
        }
    }
    QVariantList rej;
    for (const FileHashes &h : m_rejected) rej << h.toVariant();
    if (!rej.isEmpty()) m.insert(QStringLiteral("rejected"), rej);
    if (!m_trackers.isEmpty()) m.insert(QStringLiteral("trackers"), m_trackers);
    // the user's choices: per-source priorities, removed sources, AFS / AEQ, the download order
    QVariantMap prio;
    for (auto it = m_srcPriority.constBegin(); it != m_srcPriority.constEnd(); ++it) prio.insert(it.key(), it.value());
    if (!prio.isEmpty()) m.insert(QStringLiteral("srcPrio"), prio);
    QVariantList removed;
    for (const QString &k : m_userRemoved) removed << k;
    if (!removed.isEmpty()) m.insert(QStringLiteral("removed"), removed);
    if (m_afs >= 0) m.insert(QStringLiteral("afs"), m_afs);
    if (m_aeq >= 0) m.insert(QStringLiteral("aeq"), m_aeq);
    if (m_seq) m.insert(QStringLiteral("seq"), true);
    if (m_firstLast) m.insert(QStringLiteral("fl"), true);
    // files that were on the disk before the download stay "not its own" (never deleted with it)
    if (m_ownKnown) {
        QVariantMap own;
        own.insert(QStringLiteral("pre"), m_preexisting);
        own.insert(QStringLiteral("topExisted"), m_topExisted);
        m.insert(QStringLiteral("own"), own);
    }
    return QCborValue::fromVariant(m).toCbor();
}

void Download::stopForRestart(bool keepTorrent)
{
    m_active = false;
    if (m_verifyJob) {
        disconnect(m_verifyJob.get(), nullptr, this, nullptr);
        m_verifyJob->stop();
    }
    m_verify.running = false;
    dropPreview();
    closeAllSources(QStringLiteral("cancelled"));
    stopFindSources();
    TorrentBackend *be = m_host->backend();
    if (m_torrent) m_torrent->releaseAll();
    if (be && !m_torrentId.isEmpty() && be->torrents().contains(m_torrentId)) {
        if (keepTorrent) be->pauseTorrent(m_torrentId);
        else be->removeTorrent(m_torrentId, false);
    }
    if (be && !m_promoteTorrent.isEmpty()) be->removeTorrent(m_promoteTorrent, false);
    if (m_part) m_part->close();
    QFile::remove(PartStore::moveMarkerPath(partPath()));   // [safe-1] a move of the old data is history now
    dropPreview();
}

void Download::setRestartPending(bool on)
{
    m_restartPending = on;
    persist(true);
    markDirty(true);
}

} // namespace mx::engine
