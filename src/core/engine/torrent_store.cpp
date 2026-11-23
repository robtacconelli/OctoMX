// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/engine/torrent_store.h"

#include "mx/download_order.h"

#include "core/log.h"

#include <QtCore/QCryptographicHash>

#include <algorithm>
#include <cmath>

namespace mx::engine {

TorrentStore::TorrentStore(TorrentBackend *backend, const QString &id, QThreadPool *pool, const Tuning *tuning,
                           QObject *parent)
    : Store(parent), m_be(backend), m_id(id), m_io(pool), m_tuning(tuning)
{
    refresh();
}

TorrentStore::~TorrentStore()
{
    m_io.discardPending();
}

void TorrentStore::refresh()
{
    if (!m_be) return;
    m_snap = m_be->snapshot(m_id);
    qint64 end = 0;
    for (const TorrentFile &f : std::as_const(m_snap.files)) end = qMax(end, f.offset + f.size);
    if (end <= 0 && m_snap.pieceLength > 0) end = qint64(m_snap.pieceLength) * m_snap.numPieces;
    m_payload = end;
    if (m_snap.have.size() != m_snap.numPieces) m_snap.have.resize(qMax(0, m_snap.numPieces));
    // wanted pieces: one sweep over the files (pieces of wanted, non-pad files)
    m_wanted = QBitArray(qMax(0, m_snap.numPieces));
    const qint64 L = m_snap.pieceLength;
    if (L > 0) {
        for (int i = 0; i < m_snap.files.size(); ++i) {
            const TorrentFile &f = m_snap.files.at(i);
            if (f.size <= 0 || f.priority <= 0 || isPadFile(i)) continue;
            const int a = int(f.offset / L), b = int(qMin<qint64>((f.offset + f.size - 1) / L, m_snap.numPieces - 1));
            for (int p = a; p <= b; ++p) m_wanted.setBit(p);
        }
    }
}

Range TorrentStore::fileSpan(int file) const
{
    if (file < 0 || file >= m_snap.files.size()) return Range(0, 0);
    const TorrentFile &f = m_snap.files.at(file);
    return Range(f.offset, f.offset + f.size);
}

bool TorrentStore::isPadFile(int file) const
{
    if (file < 0 || file >= m_snap.files.size()) return false;
    const QString &p = m_snap.files.at(file).path;
    return p.startsWith(QLatin1String(".pad/")) || p.contains(QLatin1String("/.pad/"));
}

Range TorrentStore::pieceRange(int p) const
{
    const qint64 L = m_snap.pieceLength;
    if (L <= 0 || p < 0 || p >= m_snap.numPieces) return Range(0, 0);
    return Range(qint64(p) * L, qMin(qint64(p + 1) * L, m_payload));
}

bool TorrentStore::pieceWanted(int p) const
{
    return p >= 0 && p < m_wanted.size() && m_wanted.testBit(p);
}

void TorrentStore::fillPads(Piece &pc)
{
    for (int i = 0; i < m_snap.files.size(); ++i) {
        if (!isPadFile(i)) continue;
        const Range fs = fileSpan(i);
        const qint64 s = qMax(fs.first, pc.start), e = qMin(fs.second, pc.end);
        if (e <= s) continue;
        if (pc.buf.isEmpty()) {
            pc.buf = QByteArray(int(pc.end - pc.start), char(0));
            m_buffered += pc.buf.size();
        }
        pc.filled.add(s, e);   // buffer is zero-initialised
    }
}

void TorrentStore::plan(const QList<Lane> &lanes, qint64 nowMs)
{
    if (!hasMetadata()) return;
    // ---- timeouts
    const QList<int> keys = m_res.keys();
    for (int p : keys) {
        const Piece &pc = m_res.value(p);
        if (pc.verifying || pc.injected) {
            if (pc.injected && nowMs - pc.lastMs > 120000) release(p, true);   // backend never answered
            continue;
        }
        // a piece that got data times out on its own silence; one still waiting its turn only when
        // the foreign sources as a whole stopped delivering
        const bool started = pc.lastMs > pc.reservedMs;
        const bool idle = started ? nowMs - pc.lastMs > m_tuning->reservationIdleMs
                                  : (nowMs - pc.reservedMs > m_tuning->reservationIdleMs
                                     && nowMs - m_lastProgressMs > m_tuning->reservationIdleMs);
        const bool tooOld = nowMs - pc.reservedMs > m_tuning->reservationMaxMs;
        if (m_snap.have.testBit(p) || idle || tooOld) {
            if (idle || tooOld) {
                qCInfo(lcEngine) << "torrent" << m_id.left(8) << "reservation of piece" << p << "timed out, returned to libtorrent";
                m_cooldown.insert(p, nowMs + 4LL * m_tuning->reservationIdleMs);   // let libtorrent have it
            }
            release(p, true);
        }
    }
    QHash<int, Lane> laneByFile;
    int ready = 0;
    qint64 rate = 0;
    for (const Lane &l : lanes) {
        if (l.file < 0 || l.file >= m_snap.files.size()) continue;
        laneByFile.insert(l.file, l);
        ready += l.readySources;
        rate += l.rateBps;
    }
    if (ready == 0) return;

    const qint64 L = m_snap.pieceLength;
    int missing = 0;
    for (int p = 0; p < m_snap.numPieces; ++p)
        if (!m_snap.have.testBit(p) && pieceWanted(p)) ++missing;
    if (missing == 0) return;
    const bool btHasPeers = (m_snap.peers + m_snap.seeds) > 0;
    const int shareCap = btHasPeers ? qMax(1, int(std::ceil(m_tuning->maxReservedShare * missing))) : missing;
    const qint64 assumed = rate > 0 ? rate : qint64(ready) * 256 * 1024;
    const int horizon = int(qMax<qint64>(2LL * ready, assumed * m_tuning->reserveHorizonSec / qMax<qint64>(1, L)));
    const int memCap = int(qMax<qint64>(2, (m_tuning->maxBufferedBytes / 2) / qMax<qint64>(1, L)));
    const int target = qMin(shareCap, qMin(horizon, memCap));
    int need = target - int(m_res.size());
    if (need <= 0) return;

    auto coverable = [&](int p) {
        const Range r = pieceRange(p);
        for (const Lane &l : lanes) {   // fast path: inside one lane's file
            const Range fs = fileSpan(l.file);
            if (r.first >= fs.first && r.second <= fs.second) return true;
        }
        for (int i = 0; i < m_snap.files.size(); ++i) {
            const TorrentFile &f = m_snap.files.at(i);
            if (f.offset >= r.second) break;
            if (f.offset + f.size <= r.first || f.size == 0) continue;
            if (!laneByFile.contains(i) && !isPadFile(i)) return false;
        }
        return true;
    };
    QList<int> fresh;
    QSet<int> taken;
    auto consider = [&](int p) {
        if (p < 0 || p >= m_snap.numPieces || need <= 0) return;
        if (m_snap.have.testBit(p) || m_res.contains(p) || m_giveUp.contains(p) || taken.contains(p)) return;
        if (m_cooldown.value(p) > nowMs) return;
        if (!pieceWanted(p) || !coverable(p)) return;
        fresh << p;
        taken.insert(p);
        --need;
    };
    QList<Lane> ordered = lanes;
    if (m_seq || m_firstLast)   // [dl-1] the order flags follow the file order of the torrent
        std::sort(ordered.begin(), ordered.end(), [](const Lane &a, const Lane &b) { return a.file < b.file; });
    if (m_firstLast) {   // [dl-1] the head and tail band of every lane's file first
        for (const Lane &l : std::as_const(ordered)) {
            const Range fs = fileSpan(l.file);
            if (fs.second <= fs.first) continue;
            const qint64 len = fs.second - fs.first;
            const qint64 he = fs.first + order::headBandBytes(len), ts = fs.second - order::tailBandBytes(len);
            for (int p = int(fs.first / L); p <= int((he - 1) / L) && need > 0; ++p) consider(p);
            for (int p = int(ts / L); p <= int((fs.second - 1) / L) && need > 0; ++p) consider(p);
        }
    }
    for (const Lane &l : std::as_const(ordered)) {
        const Range fs = fileSpan(l.file);
        if (fs.second <= fs.first || need <= 0) continue;
        const int pFirst = int(fs.first / L), pLast = int((fs.second - 1) / L);
        if (m_seq) {   // [dl-1] ascending, like libtorrent's sequential picker
            for (int p = pFirst; p <= pLast && need > 0; ++p) consider(p);
            continue;
        }
        // from the end of the file backwards: away from where the picker of a fresh torrent tends
        // to be busy, and contiguous for HTTP sources
        for (int p = pLast; p >= pFirst && need > 0; --p) consider(p);
    }
    if (fresh.isEmpty()) return;
    m_be->reservePieces(m_id, fresh, true);
    for (int p : fresh) {
        Piece pc;
        const Range r = pieceRange(p);
        pc.start = r.first;
        pc.end = r.second;
        pc.reservedMs = pc.lastMs = nowMs;
        fillPads(pc);
        m_res.insert(p, pc);
    }
    qCDebug(lcEngine) << "torrent" << m_id.left(8) << "reserved" << fresh.size() << "pieces for foreign sources ("
                      << m_res.size() << "of" << missing << "missing reserved)";
    emit changed();
}

void TorrentStore::release(int p, bool emitAbandon)
{
    auto it = m_res.find(p);
    if (it == m_res.end()) return;
    const Range r(it->start, it->end);
    m_buffered -= it->buf.size();
    const bool injected = it->injected;
    m_res.erase(it);
    if (!injected) m_be->reservePieces(m_id, {p}, false);
    if (emitAbandon) emit unitAbandoned(r.first, r.second);
}

void TorrentStore::releaseAll()
{
    QList<int> back;
    QList<Range> ranges;
    for (auto it = m_res.begin(); it != m_res.end(); ++it) {
        if (!it->injected) back << it.key();
        ranges << Range(it->start, it->end);
        m_buffered -= it->buf.size();
    }
    m_res.clear();
    m_buffered = 0;
    if (!back.isEmpty()) m_be->reservePieces(m_id, back, false);
    for (const Range &r : ranges) emit unitAbandoned(r.first, r.second);
}

RangeSet TorrentStore::fetchTarget() const
{
    RangeSet t;
    for (auto it = m_res.constBegin(); it != m_res.constEnd(); ++it) {
        if (it->verifying || it->injected) continue;
        t.add(it->filled.complement(it->start, it->end));
    }
    return t;
}

Range TorrentStore::unitAt(qint64 pos) const
{
    const qint64 L = m_snap.pieceLength;
    if (L <= 0) return Range(pos, pos + 1);
    return pieceRange(int(pos / L));
}

bool TorrentStore::canVerifyUnit(qint64 pos) const
{
    return hasMetadata() && pos >= 0 && pos < m_payload;
}

RangeSet TorrentStore::deliver(qint64 off, const QByteArray &bytes)
{
    RangeSet accepted;
    const qint64 L = m_snap.pieceLength;
    if (!hasMetadata() || bytes.isEmpty() || off < 0 || L <= 0) return accepted;
    const qint64 end = qMin(off + bytes.size(), m_payload);
    for (int p = int(off / L); p < m_snap.numPieces && qint64(p) * L < end; ++p) {
        auto it = m_res.find(p);
        if (it == m_res.end() || it->verifying || it->injected) continue;
        const qint64 s = qMax(off, it->start), e = qMin(end, it->end);
        if (e <= s) continue;
        const RangeSet need = it->filled.complement(s, e);
        if (need.isEmpty()) continue;
        if (it->buf.isEmpty()) {
            it->buf = QByteArray(int(it->end - it->start), char(0));
            m_buffered += it->buf.size();
        }
        for (const Range &r : need.ranges()) {
            memcpy(it->buf.data() + (r.first - it->start), bytes.constData() + (r.first - off), size_t(r.second - r.first));
            it->filled.add(r.first, r.second);
            accepted.add(r.first, r.second);
        }
        it->lastMs = m_lastProgressMs = nowMs();
        if (it->filled.contains(it->start, it->end)) startVerify(p);
    }
    return accepted;
}

void TorrentStore::startVerify(int p)
{
    auto it = m_res.find(p);
    if (it == m_res.end()) return;
    it->verifying = true;
    const QByteArray expect = m_be->pieceHash(m_id, p);
    const QByteArray data = it->buf;
    m_io.run([this, p, data, expect] {
        const QByteArray got = QCryptographicHash::hash(data, QCryptographicHash::Sha1);
        const bool ok = expect.size() == 20 && got == expect;
        IoQueue::post(this, [this, p, ok] {
            auto it = m_res.find(p);
            if (it == m_res.end() || !it->verifying) return;
            it->verifying = false;
            const Range r(it->start, it->end);
            if (ok) {
                it->injected = true;
                const QByteArray data = it->buf;
                m_buffered -= it->buf.size();
                it->buf = QByteArray();
                m_be->injectPiece(m_id, p, data);
                qCDebug(lcEngine) << "torrent" << m_id.left(8) << "injected piece" << p;
            } else {
                ++m_failTotal;
                const int n = ++m_failures[p];
                qCInfo(lcEngine) << "torrent" << m_id.left(8) << "foreign piece" << p << "failed the SHA-1 pre-check (" << n << ")";
                m_buffered -= it->buf.size();
                it->buf = QByteArray();
                it->filled.clear();
                fillPads(*it);
                emit unitFailed(r.first, r.second);
                if (n >= 3) {
                    m_giveUp.insert(p);
                    release(p, true);
                }
            }
            emit changed();
        });
    });
}

void TorrentStore::onPieceFinished(int piece)
{
    if (piece >= 0 && piece < m_snap.have.size()) m_snap.have.setBit(piece);
    auto it = m_res.find(piece);
    if (it == m_res.end()) return;
    const Range r(it->start, it->end);
    const bool injected = it->injected;
    m_buffered -= it->buf.size();
    m_res.erase(it);
    if (injected) {
        ++m_injectedCount;
        m_injectedBytes += r.second - r.first;
        emit unitVerified(r.first, r.second);
    } else {
        // a BitTorrent peer had it in flight when we reserved: the plugin un-reserved it
        emit unitAbandoned(r.first, r.second);
    }
    emit changed();
}

void TorrentStore::onPieceFailed(int piece)
{
    auto it = m_res.find(piece);
    if (it == m_res.end() || !it->injected) return;   // a peer's bad piece: libtorrent's business
    const Range r(it->start, it->end);
    ++m_failTotal;
    const int n = ++m_failures[piece];
    it->injected = false;
    it->filled.clear();
    it->lastMs = it->reservedMs = nowMs();
    fillPads(*it);
    emit unitFailed(r.first, r.second);
    if (n >= 2) {
        m_giveUp.insert(piece);
        release(piece, false);
    }
    emit changed();
}

void TorrentStore::pieceMap(int cells, const RangeSet &inFlight, QByteArray *state, QByteArray *fill) const
{
    // pieces from other networks count while a request for them is out, while they are checked
    // and until libtorrent has written them; a reservation alone is only a plan
    QSet<int> moving;
    for (auto it = m_res.constBegin(); it != m_res.constEnd(); ++it)
        if (it->verifying || it->injected || inFlight.intersects(it->start, it->end)) moving.insert(it.key());
    mapOf(m_snap, moving, cells, state, fill);
}

static void addPieceRuns(const QBitArray &bits, int n, qint64 L, qint64 payload, RangeSet *out)
{
    for (int p = 0; p < n && p < bits.size();) {
        if (!bits.testBit(p)) {
            ++p;
            continue;
        }
        int q = p + 1;
        while (q < n && q < bits.size() && bits.testBit(q)) ++q;
        out->add(qint64(p) * L, qMin(qint64(q) * L, payload));
        p = q;
    }
}

void TorrentStore::mapOf(const TorrentSnapshot &sn, const QSet<int> &moving, int cells, QByteArray *state, QByteArray *fill)
{
    state->clear();
    fill->clear();
    const qint64 L = sn.pieceLength;
    const int n = sn.numPieces;
    if (!sn.hasMetadata || L <= 0 || n <= 0) return;
    qint64 payload = 0;
    for (const TorrentFile &f : sn.files) payload = qMax(payload, f.offset + f.size);
    if (payload <= 0) payload = L * n;
    QList<Range> spans;
    for (const TorrentFile &f : sn.files) {
        const bool pad = f.path.startsWith(QLatin1String(".pad/")) || f.path.contains(QLatin1String("/.pad/"));
        if (f.priority > 0 && !pad && f.size > 0) spans << Range(f.offset, f.offset + f.size);
    }
    if (sn.files.isEmpty()) spans << Range(0, payload);
    RangeSet complete, progress;
    addPieceRuns(sn.have, n, L, payload, &complete);
    addPieceRuns(sn.downloading, n, L, payload, &progress);
    for (int p : moving)
        if (p >= 0 && p < n) progress.add(qint64(p) * L, qMin(qint64(p + 1) * L, payload));
    condensePieceMap(cells, spans, complete, progress, state, fill);
}

} // namespace mx::engine
