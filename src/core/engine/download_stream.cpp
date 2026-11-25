// SPDX-License-Identifier: GPL-3.0-or-later
// [str-1] OctoMX engine: streaming. A player reads the file through the local API (src/core/stream)
// in its own order: the beginning, the index at the end, sequentially, any place after a skip. The
// stream service tells the download where every open read is (setStreamReads); the download then
//   * range sources (eD2k, Kad, Gnutella, HTTP, OctoMX ...): fetches only [pos, end) of every read
//     (a stream download: streamTarget() limits the scheduler to it) and asks each source first for
//     the byte nearest after a read position it has (streamStart(), consulted by orderedStart());
//   * BitTorrent: gives those pieces to libtorrent with deadlines and the top priority, every other
//     piece priority 0 (TorrentBackend::setStreamPieces), and the engine's own piece reservations for
//     other networks are made among them (TorrentStore::setStreamPieces);
//   * a seek: requests nobody waits for any more are cancelled unless little of them is left.
// Only right data reaches a player: verified bytes (torrent pieces, tiger tree leaves, eD2k parts)
// or, where the only check is a large unit (an eD2k part of 9.28 MB, no fine hash at all), the
// received bytes at once (streamTrustsReceived(); the status says so). A torrent piece is verified
// in the backend's memory and written to its file afterwards, so a player gets it only once the
// backend has compared the file with it (streamPieceReadable()).
#include "core/engine/download.h"

#include "core/dao.h"
#include "core/log.h"
#include "mx/download_order.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QTimer>

#include <algorithm>

namespace mx::engine {

namespace {
// a unit larger than this is not waited for: its bytes are served as they arrive (unverified)
constexpr qint64 kStreamWaitUnitMax = qint64(2) << 20;
// a request nobody waits for any more is left to finish when this little of it is left
constexpr qint64 kStreamFinishLeft = 256 * 1024;
// the backend is asked again whether a finished piece is in its file when it did not answer for this long
constexpr qint64 kStreamAskAgainMs = 3000;
} // namespace

DownloadsDao *Download::recordDao() const { return streamRestricts() ? nullptr : m_host->dao(); }

void Download::setStream(bool on, bool restrict, int fileIndex, const QString &partPath)
{
    if (!partPath.isEmpty() && !m_part) m_partPath = partPath;   // the cache file of a stream download
    m_streamOn = on;
    m_streamRestrict = restrict;
    m_streamFile = fileIndex;
    m_streamHave.clear();   // asked again: the files may have changed since (a restart, a check)
    m_streamAsked.clear();
    if (!on) {
        m_streamReads.clear();
        m_streamGiven.clear();
        m_streamWant.clear();
    }
}

Range Download::streamSpan() const
{
    if (m_torrent) {
        if (!m_torrent->hasMetadata()) return Range(0, 0);
        const TorrentSnapshot &sn = m_torrent->snap();
        int f = m_streamFile;
        if (f < 0 || f >= sn.files.size()) {   // no choice (a single file, or a bundle selected before its
            qint64 best = -1;                   // file list was known): the largest audio / video file
            bool media = false;
            for (int i = 0; i < sn.files.size(); ++i) {
                if (m_torrent->isPadFile(i) || sn.files.at(i).priority <= 0) continue;
                const bool m = preview::isMediaFile(sn.files.at(i).path);
                if ((m && !media) || (m == media && sn.files.at(i).size > best)) {
                    best = sn.files.at(i).size;
                    f = i;
                    media = m;
                }
            }
        }
        return f >= 0 ? m_torrent->fileSpan(f) : Range(0, 0);
    }
    const qint64 size = m_part && m_part->extent() > 0 ? m_part->extent() : m_rec.size;
    return size > 0 ? Range(0, size) : Range(0, 0);
}

bool Download::streamTrustsReceived() const
{
    if (!m_part || m_torrent) return false;
    if (m_part->scheme() == PartStore::NoUnits) return true;   // no fine hash: the whole-file check comes last
    return m_part->unitSize() > kStreamWaitUnitMax;            // an eD2k part of 9.28 MB is not waited for
}

RangeSet Download::streamServableSet() const
{
    RangeSet out;
    const Range span = streamSpan();
    if (span.second <= span.first) return out;
    if (streamWholeFileReadable()) {
        out.add(span.first, span.second);
        return out;
    }
    if (m_torrent) {
        const TorrentSnapshot &sn = m_torrent->snap();
        const qint64 L = sn.pieceLength;
        if (L <= 0) return out;
        for (qint64 p = span.first / L; p * L < span.second; ++p)
            if (streamPieceReadable(p)) out.add(qMax(span.first, p * L), qMin(span.second, (p + 1) * L));
        return out;
    }
    if (!m_part) return out;
    out = m_part->verified();
    if (streamTrustsReceived()) out.add(m_part->written());
    return out.intersected(span.first, span.second);
}

Download::StreamServe Download::streamServable(qint64 pos) const
{
    StreamServe sv;
    const Range span = streamSpan();
    if (span.second <= span.first || pos < span.first || pos >= span.second) return sv;
    sv.fileBase = span.first;
    if (m_torrent) {
        const TorrentSnapshot &sn = m_torrent->snap();
        for (int i = 0; i < sn.files.size(); ++i)
            if (m_torrent->fileSpan(i) == span) { sv.path = QDir(sn.savePath).filePath(sn.files.at(i).path); break; }
    } else if (isFinished()) {
        sv.path = m_rec.savePath;
    } else if (m_part) {
        sv.path = m_part->path();
    }
    if (streamWholeFileReadable()) {
        sv.avail = span.second - pos;
        return sv;
    }
    // the run of servable bytes from pos (torrent: whole pieces; part file: verified, or received)
    if (m_torrent) {
        const TorrentSnapshot &sn = m_torrent->snap();
        const qint64 L = sn.pieceLength;
        if (L <= 0) return sv;
        qint64 e = pos;
        for (qint64 p = pos / L; e < span.second; ++p) {
            if (!streamPieceReadable(p)) break;
            e = qMin(span.second, (p + 1) * L);
        }
        sv.avail = e - pos;
        return sv;
    }
    if (!m_part) return sv;
    const RangeSet ver = m_part->verified();
    if (ver.contains(pos)) {
        sv.avail = qMin(span.second, ver.runEnd(pos)) - pos;
        return sv;
    }
    if (streamTrustsReceived()) {
        RangeSet all = m_part->written();
        all.add(ver);
        if (all.contains(pos)) {
            sv.avail = qMin(span.second, all.runEnd(pos)) - pos;
            sv.unverified = true;
        }
    }
    return sv;
}

QString Download::streamFileName() const
{
    if (m_torrent && m_torrent->hasMetadata()) {
        const Range span = streamSpan();
        const TorrentSnapshot &sn = m_torrent->snap();
        for (int i = 0; i < sn.files.size(); ++i)
            if (span.second > span.first && m_torrent->fileSpan(i) == span) return QFileInfo(sn.files.at(i).path).fileName();
    }
    return m_rec.isBundle ? QString() : m_rec.name;
}

qint64 Download::streamDiskBytes() const
{
    if (m_torrent) return m_torrent->hasMetadata() ? m_torrent->snap().totalDone : 0;
    return m_part ? m_part->heldBytes() : 0;
}

void Download::setStreamReads(const QList<StreamRead> &reads)
{
    if (!m_streamOn) return;
    bool same = reads.size() == m_streamGiven.size();
    for (int i = 0; same && i < reads.size(); ++i)
        same = reads.at(i).pos == m_streamGiven.at(i).pos && reads.at(i).end == m_streamGiven.at(i).end
               && reads.at(i).urgentEnd == m_streamGiven.at(i).urgentEnd && reads.at(i).waiting == m_streamGiven.at(i).waiting;
    if (same) {
        // the same reads, but a torrent that was not told yet (a part file promoted to a torrent, the
        // backend restarted) or a while ago: told (again)
        if (m_torrent && (m_streamSentFor != m_torrentId || nowMs() - m_streamSentMs > 5000)) {
            m_streamSentMs = 0;
            applyStreamToTorrent();
        }
        askStreamPiecesOnDisk();
        return;
    }
    m_streamGiven = reads;
    m_streamReads = reads;
    // a block is served once it is checked: its whole unit is fetched, also the bytes before the
    // read position (a player that starts in the middle of a block)
    if (m_part && !m_torrent && !streamTrustsReceived() && m_part->scheme() != PartStore::NoUnits) {
        const qint64 ext = m_part->extent();
        for (StreamRead &r : m_streamReads) {
            if (r.end <= r.pos) continue;
            r.pos = m_part->unitAt(r.pos).first;
            if (ext > 0) r.end = qMin(ext, qMax(r.end, m_part->unitAt(r.end - 1).second));
        }
    }
    m_streamWant.clear();
    m_streamUrgentWaiting.clear();
    for (const StreamRead &r : std::as_const(m_streamReads)) {
        if (r.end > r.pos) m_streamWant.add(r.pos, r.end);
        if (r.waiting && r.urgentEnd > r.pos) m_streamUrgentWaiting.add(r.pos, r.urgentEnd);
    }
    dropStaleStreamRequests();
    applyStreamToTorrent();
    askStreamPiecesOnDisk();
    emit needsSchedule();
}

RangeSet Download::streamTarget(const RangeSet &target) const
{
    if (!streamRestricts()) return target;
    return target.intersected(m_streamWant);
}

bool Download::streamStart(SourceSlot *s, const RangeSet &cand, qint64 *pos)
{
    Q_UNUSED(s)
    // the reads come most urgent first (a player that waits before one that has its buffer)
    for (const StreamRead &r : std::as_const(m_streamReads)) {
        const RangeSet sub = cand.intersected(r.pos, r.end);
        if (!sub.isEmpty()) {
            *pos = sub.first();
            return true;
        }
    }
    return false;
}

void Download::dropStaleStreamRequests()
{
    if (!streamRestricts()) return;
    for (auto &sp : m_slots) {
        SourceSlot *s = sp.get();
        if (!s->rs || s->reqs.isEmpty()) continue;
        bool stale = true;
        for (const Request &r : std::as_const(s->reqs)) {
            const qint64 from = r.off + r.got;
            if (r.sequential || m_streamWant.intersects(from, r.end()) || r.end() - from <= kStreamFinishLeft) {
                stale = false;   // still wanted, or nearly done: finishing it is faster than a new request
                break;
            }
        }
        if (stale) cancelSlotRequests(s, QStringLiteral("no player waits for it any more"));
    }
}

void Download::applyStreamToTorrent()
{
    if (!m_torrent || !m_torrent->hasMetadata() || m_torrentId.isEmpty() || !m_host->backend()) return;
    const TorrentSnapshot &sn = m_torrent->snap();
    const qint64 L = sn.pieceLength;
    if (L <= 0) return;
    QHash<int, int> deadline;   // piece -> ms from now
    QList<int> order;
    if (m_streamOn) {
        for (const StreamRead &r : std::as_const(m_streamReads)) {
            if (r.end <= r.pos) continue;
            // the urgent part is due within about 4 s of playing: its size gives the pace
            const qint64 pace = qMax<qint64>(256 * 1024, (r.urgentEnd - r.pos) / 4);
            for (qint64 p = r.pos / L; p * L < r.end && p < sn.numPieces; ++p) {
                if (streamPieceGot(p)) continue;
                const qint64 ahead = qMax<qint64>(0, p * L - r.pos);
                const int ms = int(qMin<qint64>(60000, 50 + ahead * 1000 / pace));
                auto it = deadline.find(int(p));
                if (it == deadline.end()) {
                    deadline.insert(int(p), ms);
                    order << int(p);
                } else if (ms < it.value()) {
                    it.value() = ms;
                }
            }
        }
        std::stable_sort(order.begin(), order.end(), [&deadline](int a, int b) { return deadline.value(a) < deadline.value(b); });
    }
    QList<QPair<int, int>> list;
    for (int p : std::as_const(order)) list << qMakePair(p, deadline.value(p));
    m_torrent->setStreamPieces(order, streamRestricts());
    // the backend keeps its deadlines: it is told again only when the pieces changed
    bool same = list.size() == m_streamSent.size() && m_streamSentMs > 0;
    for (int i = 0; same && i < list.size(); ++i) same = list.at(i).first == m_streamSent.at(i).first;
    if (same) return;
    m_streamSent = list;
    m_streamSentMs = nowMs();
    m_streamSentFor = m_torrentId;
    // mode 0 (libtorrent's piece deadlines on top of the top priority) is the default. Against one local
    // seed of 3 MB/s (tst_stream_bt) the top priority alone, mode 1, was a little faster after a seek
    // (median 72 / 81 ms against 121 / 173 ms); in the real swarm (Big Buck Bunny, about 100 peers)
    // the deadlines avoided the long stalls: ffprobe 9.8 / 4.5 s against 34.2 / 4.1 s, the longest wait
    // of a player 3.5 / 1.0 s against 30.1 / 1.8 s (the deadlines ask the fastest peers and ask again
    // for a late block). MX_STREAM_BT_MODE=1 selects the other one.
    const int mode = qEnvironmentVariableIsSet("MX_STREAM_BT_MODE") ? qEnvironmentVariableIntValue("MX_STREAM_BT_MODE") : 0;
    m_host->backend()->setStreamPieces(m_torrentId, list, m_streamOn && streamRestricts(), mode);
}

void Download::setStreamIdle(bool idle)
{
    if (!streamRestricts() || idle == m_streamIdle) return;
    m_streamIdle = idle;
    if (idle) {
        qCInfo(lcEngine) << m_rec.id << "stream: no player, its sources are let go";
        closeAllSources(QStringLiteral("dropped"));
        if (m_torrent && m_host->backend()) {
            m_torrent->releaseAll();
            m_host->backend()->pauseTorrent(m_torrentId);
        }
    } else {
        qCInfo(lcEngine) << m_rec.id << "stream: a player is back";
        if (m_torrent && m_host->backend() && m_active) m_host->backend()->resumeTorrent(m_torrentId);
        openMoreSources();
        emit needsSchedule();
    }
    markDirty(true);
}

void Download::streamPieceDone(int piece)
{
    if (!m_streamOn || piece < 0) return;
    if (m_streamGot.size() <= piece) m_streamGot.resize(piece + 1);
    m_streamGot.setBit(piece);
    if (m_torrent) applyStreamToTorrent();   // that piece leaves the deadline list
    askStreamPiecesOnDisk();
}

bool Download::streamPieceGot(qint64 p) const
{
    if (!m_torrent || p < 0) return false;
    const TorrentSnapshot &sn = m_torrent->snap();
    return (p < sn.have.size() && sn.have.testBit(int(p))) || (p < m_streamGot.size() && m_streamGot.testBit(int(p)));
}

bool Download::streamPieceReadable(qint64 p) const
{
    if (!streamPieceGot(p)) return false;
    return m_streamTrustHave || (p < m_streamHave.size() && m_streamHave.testBit(int(p)));
}

// a finished download is served whole; one that a torrent backend still holds piece by piece, as
// its last pieces may be verified and not written yet
bool Download::streamWholeFileReadable() const
{
    return isFinished() && (!m_torrent || !m_torrent->hasMetadata() || m_streamTrustHave);
}

void Download::streamPieceOnDisk(int piece)
{
    if (piece < 0) return;
    if (m_streamHave.size() <= piece) m_streamHave.resize(piece + 1);
    m_streamHave.setBit(piece);
    m_streamAsked.remove(piece);
}

// the finished pieces the players read or will read soon that the backend did not confirm in the
// file yet: it is asked (the backend also tells by itself for a piece that finishes while a stream
// is on; this covers the pieces that were there before, and a backend that was restarted)
void Download::askStreamPiecesOnDisk()
{
    if (!m_streamOn || !m_torrent || !m_torrent->hasMetadata() || m_torrentId.isEmpty()) return;
    const TorrentSnapshot &sn = m_torrent->snap();
    const qint64 L = sn.pieceLength;
    if (L <= 0) return;
    const qint64 now = nowMs();
    QList<int> ask;
    for (const StreamRead &r : std::as_const(m_streamReads)) {
        for (qint64 p = r.pos / L; p * L < r.end && p < sn.numPieces; ++p) {
            if (!streamPieceGot(p) || (p < m_streamHave.size() && m_streamHave.testBit(int(p)))) continue;
            const auto it = m_streamAsked.constFind(int(p));
            if (it != m_streamAsked.constEnd() && now - it.value() < kStreamAskAgainMs) continue;
            m_streamAsked.insert(int(p), now);
            ask << int(p);
        }
    }
    if (ask.isEmpty()) return;
    TorrentBackend *be = m_host->backend();
    m_streamTrustHave = !be || !be->confirmPiecesOnDisk(m_torrentId, ask);
}

void Download::endStream(bool keep, const QString &dir)
{
    if (!m_streamOn) return;
    const bool owned = m_streamRestrict;
    m_streamOn = false;
    m_streamReads.clear();
    m_streamGiven.clear();
    m_streamWant.clear();
    m_streamNote.clear();
    m_streamSentMs = 0;
    m_streamSent.clear();
    if (m_torrent) {
        m_torrent->setStreamPieces({}, false);
        if (m_host->backend() && !m_torrentId.isEmpty()) m_host->backend()->setStreamPieces(m_torrentId, {}, false, 0);
    }
    if (owned && keep) {
        // a normal download from now on: it fetches the rest and completes into the incoming folder
        m_streamRestrict = false;
        if (!dir.isEmpty()) setTarget(dir, QString());
        qCInfo(lcEngine) << m_rec.id << "the stream is kept as a download into" << dir;
        persist(true);
        if (DownloadsDao *dao = m_host->dao()) {   // its sources from now on too
            QList<SearchSource> srcs;
            for (const auto &sp : m_slots)
                if (!sp->removedByUser) srcs << sp->desc;
            if (!srcs.isEmpty()) dao->addSources(m_rec.id, srcs);
        }
        updateStatus();
        markDirty(true);
        emit needsSchedule();
        QTimer::singleShot(0, this, [this] { maybeComplete(); });
        return;
    }
    markDirty(true);
    emit needsSchedule();
}

} // namespace mx::engine
