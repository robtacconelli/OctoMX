// SPDX-License-Identifier: GPL-3.0-or-later
// [dl-1] OctoMX engine: download order (sequential / first and last pieces first) and preview of
// a partly downloaded audio / video file. The policy lives here, once, for every network:
//   * range sources (eD2k, Kad, Gnutella, HTTP, OctoMX overlay, Rats ...): Download::pickChunk asks
//     orderedStart() before rarest-first, so each source is asked for the lowest missing range it
//     has (sequential) or first for the head and tail band of the file (first and last);
//   * BitTorrent: libtorrent's own picker through TorrentBackend::setDownloadOrder, and the pieces
//     the engine reserves for other networks (TorrentStore::plan) in the same order.
// The flags are priorities: a source without anything in the preferred region gives what it has.
#include "core/engine/download.h"

#include "core/log.h"
#include "mx/download_order.h"
#include "mx/settings_keys.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace mx::engine {

namespace {

QString previewName(QString n)
{
    static const QString bad = QStringLiteral("\\/:*?\"<>|");
    for (QChar &c : n)
        if (bad.contains(c) || c.unicode() < 32) c = QLatin1Char('_');
    n = n.trimmed();
    while (n.endsWith(QLatin1Char('.'))) n.chop(1);
    return n.isEmpty() ? QStringLiteral("preview") : n;
}

bool makeHardLink(const QString &from, const QString &to)
{
#ifdef Q_OS_WIN
    const QString f = QDir::toNativeSeparators(from), t = QDir::toNativeSeparators(to);
    return CreateHardLinkW(reinterpret_cast<LPCWSTR>(t.utf16()), reinterpret_cast<LPCWSTR>(f.utf16()), nullptr) != 0;
#else
    return ::link(QFile::encodeName(from).constData(), QFile::encodeName(to).constData()) == 0;
#endif
}

// a temporary copy of what a player needs: the contiguous head (at most 256 MiB) and the needed tail,
// both at their offsets (the gap stays a hole)
bool copyForPreview(const QString &from, const QString &to, qint64 size, qint64 head, qint64 tail, QString *err)
{
    QFile in(from), out(to);
    if (!in.open(QIODevice::ReadOnly)) { *err = in.errorString(); return false; }
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) { *err = out.errorString(); return false; }
    auto copy = [&](qint64 off, qint64 len) {
        if (!in.seek(off) || !out.seek(off)) return false;
        while (len > 0) {
            const QByteArray b = in.read(qMin<qint64>(len, 4 << 20));
            if (b.isEmpty() || out.write(b) != b.size()) return false;
            len -= b.size();
        }
        return true;
    };
    const qint64 h = qMin<qint64>(head, 256LL << 20);
    bool ok = copy(0, h);
    if (ok && tail > 0 && size - tail >= h) ok = copy(size - tail, tail);
    if (ok && tail > 0 && out.size() < size) ok = out.resize(size);
    if (!ok) {
        *err = out.errorString();
        out.close();
        QFile::remove(to);
    }
    return ok;
}

} // namespace

QString Download::previewDirOf(const QString &incompleteDir)
{
    return QDir(incompleteDir).filePath(QStringLiteral("Preview"));
}

// ---------------------------------------------------------------------------------------- order

void Download::setOrder(bool sequential, bool firstLast, bool notify)
{
    if (sequential == m_seq && firstLast == m_firstLast) return;
    m_seq = sequential;
    m_firstLast = firstLast;
    qCInfo(lcEngine) << m_rec.id << "download order:" << (m_seq ? "sequential" : "normal") << (m_firstLast ? "+ first and last first" : "");
    applyTorrentOrder();
    m_dirtyBlob = true;
    if (!notify) return;   // a new download: transferAdded carries the flags
    markDirty(true);
    emit needsSchedule();
}

void Download::applyTorrentOrder()
{
    if (!m_torrent || m_torrentId.isEmpty() || !m_host->backend()) return;
    m_orderBackendOk = m_host->backend()->setDownloadOrder(m_torrentId, m_seq, m_firstLast);
    m_torrent->setOrder(m_seq, m_firstLast);
}

QList<Range> Download::orderFiles() const
{
    QList<Range> out;
    if (m_torrent) {
        if (!m_torrent->hasMetadata()) return out;
        const TorrentSnapshot &sn = m_torrent->snap();
        for (int i = 0; i < sn.files.size(); ++i) {
            if (m_torrent->isPadFile(i) || sn.files.at(i).priority <= 0 || sn.files.at(i).size <= 0) continue;
            const Range r = m_torrent->fileSpan(i);
            if (r.second > r.first) out << r;
        }
    } else if (m_part && m_part->extent() > 0) {
        out << Range(0, m_part->extent());
    }
    return out;
}

bool Download::orderedStart(SourceSlot *s, const RangeSet &cand, const QList<RangeSet> &peerAvail, qint64 *pos)
{
    if (cand.isEmpty()) return false;
    const QList<Range> files = orderFiles();
    if (files.isEmpty()) return false;
    // rare-part protection (qBittorrent semantics: sequential weakens rarest-first, it does not end
    // it): a partial source that alone has some of the missing data gives one request in four from
    // that data, so the swarm does not lose it when that peer leaves
    if (m_seq) {
        const RangeSet mine = slotAvailability(s);
        bool partial = false;
        for (const Range &f : files)
            if (!mine.contains(f.first, f.second)) partial = true;
        if (partial && (++s->orderedPicks % 4) == 0) {
            RangeSet unique = cand;
            for (const RangeSet &pa : peerAvail) unique.remove(pa);
            if (!unique.isEmpty()) return false;
        }
    }
    if (m_firstLast) {
        for (const Range &f : files) {
            const qint64 len = f.second - f.first;
            const RangeSet head = cand.intersected(f.first, f.first + order::headBandBytes(len));
            if (!head.isEmpty()) { *pos = head.first(); return true; }
            const RangeSet tail = cand.intersected(f.second - order::tailBandBytes(len), f.second);
            if (!tail.isEmpty()) { *pos = tail.first(); return true; }
        }
    }
    if (m_seq) {
        *pos = cand.first();
        return true;
    }
    return false;
}

QList<Net> Download::orderFollowers() const
{
    QList<Net> out;
    if (!m_seq && !m_firstLast) return out;
    QSet<int> nets;
    for (const auto &sp : m_slots) {
        const SourceSlot *s = sp.get();
        if (!s->rs || s->banned || s->isTorrentLink) continue;
        // a source without ranges always sends the file from its start: sequential by nature
        if (s->sequentialOnly() && !m_seq) continue;
        nets.insert(int(s->desc.net));
    }
    if (m_torrent && m_orderBackendOk) nets.insert(int(Net::BitTorrent));
    for (int n : std::as_const(nets)) out << Net(n);
    std::sort(out.begin(), out.end(), [](Net a, Net b) { return int(a) < int(b); });
    return out;
}

// ---------------------------------------------------------------------------------------- preview

PreviewFile Download::previewOf(int fileIndex, QString *path) const
{
    PreviewFile pf;
    pf.index = fileIndex;
    std::function<QByteArray(qint64, qint64)> read;
    std::function<bool(qint64, qint64)> have;
    QString file;
    if (m_torrent) {
        if (!m_torrent->hasMetadata()) return pf;
        const TorrentSnapshot &sn = m_torrent->snap();
        if (fileIndex < 0 || fileIndex >= sn.files.size() || m_torrent->isPadFile(fileIndex)) return pf;
        const TorrentFile &f = sn.files.at(fileIndex);
        pf.name = f.path;
        pf.size = f.size;
        pf.done = f.done;
        file = QDir(sn.savePath).filePath(f.path);
        const qint64 L = sn.pieceLength;
        const Range span = m_torrent->fileSpan(fileIndex);
        auto pieceHave = [&sn](qint64 p) { return p >= 0 && p < sn.have.size() && sn.have.testBit(int(p)); };
        if (L > 0 && span.second > span.first) {
            qint64 e = span.first;
            while (e < span.second && pieceHave(e / L)) e = qMin(span.second, (e / L + 1) * L);
            pf.headBytes = e - span.first;
            qint64 b = span.second;
            while (b > span.first && pieceHave((b - 1) / L)) b = qMax(span.first, ((b - 1) / L) * L);
            pf.tailBytes = span.second - b;
        }
        have = [span, L, pieceHave](qint64 off, qint64 len) {
            if (L <= 0) return false;
            for (qint64 p = (span.first + off) / L; p <= (span.first + off + len - 1) / L; ++p)
                if (!pieceHave(p)) return false;
            return true;
        };
    } else {
        pf.name = m_rec.name;
        pf.size = m_rec.size;
        if (isFinished()) {
            file = m_rec.savePath;
            pf.done = pf.headBytes = pf.tailBytes = pf.size;
        } else {
            file = m_part ? m_part->path() : partPath();
            const RangeSet disk = m_part ? m_part->persistable() : m_restoreHeld;
            pf.done = disk.total();
            pf.headBytes = disk.contains(0) ? disk.runEnd(0) : 0;
            if (pf.size > 0 && disk.contains(pf.size - 1)) {
                const QList<Range> rs = disk.ranges();
                pf.tailBytes = pf.size - rs.last().first;
            }
            have = [disk](qint64 off, qint64 len) { return disk.contains(off, off + len); };
        }
    }
    if (!have) have = [](qint64, qint64) { return true; };
    read = [file](qint64 off, qint64 len) {
        QFile f(file);
        if (!f.open(QIODevice::ReadOnly) || !f.seek(off)) return QByteArray();
        return f.read(len);
    };
    preview::Mp4Layout mp4;
    pf.media = preview::isMediaFile(pf.name);
    if (pf.media && preview::indexKind(pf.name) == preview::Index::Mp4 && pf.headBytes > 0)
        mp4 = preview::scanIsoBmff(pf.size, read, have);
    const preview::Readiness r = preview::evaluate(pf.name, pf.size, pf.headBytes, pf.tailBytes, mp4);
    pf.ready = r.ready;
    pf.reason = r.reason;
    if (path) *path = file;
    return pf;
}

PreviewInfo Download::previewInfo() const
{
    PreviewInfo info;
    if (m_torrent) {
        if (!m_torrent->hasMetadata()) {
            info.supported = true;   // nothing known yet: no files
            return info;
        }
        const TorrentSnapshot &sn = m_torrent->snap();
        for (int i = 0; i < sn.files.size(); ++i) {
            if (m_torrent->isPadFile(i) || sn.files.at(i).priority <= 0) continue;
            const PreviewFile pf = previewOf(i, nullptr);
            if (pf.media) info.files << pf;
        }
        info.supported = true;
        return info;
    }
    if (m_rec.isBundle) return info;   // a bundle without its torrent: nothing to look at
    info.supported = true;
    info.files << previewOf(-1, nullptr);
    return info;
}

QString Download::preparePreview(int fileIndex, QString *error)
{
    auto fail = [error](const QString &why) {
        if (error) *error = why;
        return QString();
    };
    if (m_host->settings() && !m_host->settings()->b(keys::launch::PreviewIncomplete) && !isFinished())
        return fail(QStringLiteral("Previewing incomplete files is switched off (Settings, File Launching, Launch Options)."));
    QString path;
    if (!m_torrent && fileIndex >= 0) fileIndex = -1;
    const PreviewFile pf = previewOf(fileIndex, &path);
    if (!pf.media) return fail(pf.reason.isEmpty() ? QStringLiteral("Preview: not an audio or video file") : pf.reason);
    if (!pf.ready) return fail(pf.reason);
    if (path.isEmpty() || !QFileInfo::exists(path)) return fail(QStringLiteral("The partial file is not on disk yet."));
    // BitTorrent writes every file at its final place and offsets (sparse / preallocated), and a
    // complete file is complete: the player reads it in place
    if (m_torrent || isFinished()) return path;
    // a part file is the file at its final offsets, but named <id>.part (players and the system's
    // file associations go by the extension): a hard link with the real name in
    // <incomplete>/Preview/<id>/, so the player sees the file grow; else a copy of what is there
    const QString dir = QDir(previewDirOf(m_host->incompleteDir())).filePath(m_rec.id);
    QDir().mkpath(dir);
    const QString target = QDir(dir).filePath(previewName(QFileInfo(pf.name).fileName()));
    if (QFileInfo::exists(target) && !QFile::remove(target)) {
        // still open in a player (Windows): it is the same file, use it again
        m_previewPath = target;
        return target;
    }
    if (makeHardLink(path, target)) {
        m_previewPath = target;
        qCInfo(lcEngine) << m_rec.id << "preview: hard link" << target;
        return target;
    }
    QString err;
    if (!copyForPreview(path, target, pf.size, pf.headBytes, qMin<qint64>(pf.tailBytes, 256LL << 20), &err))
        return fail(QStringLiteral("Cannot prepare the preview: %1").arg(err));
    m_previewPath = target;
    qCInfo(lcEngine) << m_rec.id << "preview: temporary copy (no hard link possible)" << target;
    return target;
}

void Download::dropPreview()
{
    if (m_previewPath.isEmpty()) return;
    if (!QFileInfo::exists(m_previewPath) || QFile::remove(m_previewPath)) {
        QDir().rmdir(QFileInfo(m_previewPath).absolutePath());
        m_previewPath.clear();
    }
}

} // namespace mx::engine
