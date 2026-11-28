// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/engine/part_store.h"

#include "mxhash/hasher.h"
#include "mxhash/multi_hasher.h"
#include "mxhash/tiger_tree.h"
#include "mx/durable_file.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QStorageInfo>
#include <QtCore/QThread>

namespace mx::engine {

namespace {
constexpr qint64 kUnknownEnd = qint64(1) << 60;

bool looksLikeDiskFull(const QString &path, qint64 need)
{
    const QStorageInfo si(QFileInfo(path).absolutePath());
    return si.isValid() && si.bytesAvailable() >= 0 && si.bytesAvailable() < qMax<qint64>(need, 1 << 20);
}

QString uniqueTarget(const QString &dir, const QString &name)
{
    const QFileInfo fi(name);
    QString base = fi.completeBaseName();
    QString ext = fi.suffix();
    if (base.isEmpty()) { base = name; ext.clear(); }
    QString cand = QDir(dir).filePath(name);
    for (int i = 1; QFileInfo::exists(cand) && i < 10000; ++i)
        cand = QDir(dir).filePath(ext.isEmpty() ? QStringLiteral("%1 (%2)").arg(base).arg(i)
                                                : QStringLiteral("%1 (%2).%3").arg(base).arg(i).arg(ext));
    return cand;
}

bool moveFile(const QString &from, const QString &to, QString *err, bool allowCopy = true)
{
    // [safe-1] test seam: MX_TEST_COPY_MOVE=1 behaves as if the target folder were on another volume
    static const bool copyOnly = qEnvironmentVariableIntValue("MX_TEST_COPY_MOVE") > 0;
    if (!copyOnly && QFile::rename(from, to)) {
        durable::noteRenamed(from, to);
        durable::syncDir(QFileInfo(to).absolutePath());
        return true;
    }
    if (!allowCopy) {   // [dl-1] a preview player still holds the file (Windows): wait instead of copying it
        *err = QStringLiteral("the file is open in another program");
        return false;
    }
    // [safe-1] different file system: a synced copy under a temporary name beside the target, renamed
    // when complete, then the source goes (a stop in between leaves no half file under the name)
    if (!durable::copyFile(from, to, err)) return false;
    QFile::remove(from);
    return true;
}

// [safe-1] the move to the target folder, announced beside the part file before it starts:
// "<target>\n<temporary copy>\n" (Download::resolveInterruptedMove finishes it after a stop)
bool writeMoveMarker(const QString &partPath, const QString &target)
{
    QFile m(PartStore::moveMarkerPath(partPath));
    if (!m.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    m.write((target + QLatin1Char('\n') + durable::copyTempName(target) + QLatin1Char('\n')).toUtf8());
    const bool ok = durable::syncFile(m);
    m.close();
    durable::syncDir(QFileInfo(partPath).absolutePath());
    return ok;
}
} // namespace

// [api-2] the test seam (see setTestHold)
namespace {
QMutex g_holdMutex;
PartStore::TestHold g_hold;
void testHold(const QString &step, const std::atomic<bool> *cancel)
{
    PartStore::TestHold h;
    {
        QMutexLocker l(&g_holdMutex);
        h = g_hold;
    }
    if (h) h(step, cancel);
}
} // namespace

void PartStore::setTestHold(TestHold hold)
{
    QMutexLocker l(&g_holdMutex);
    g_hold = std::move(hold);
}

PartStore::PartStore(QThreadPool *pool, const QString &path, qint64 size, bool preallocate, QObject *parent)
    : Store(parent), m_path(path), m_size(size), m_prealloc(preallocate), m_io(pool), m_sh(std::make_shared<Shared>())
{
    m_sh->path = path;
    // [safe-1] a sync at most kSyncMs after the first byte that is not synced yet
    m_syncTimer.setSingleShot(true);
    m_syncTimer.setInterval(kSyncMs);
    connect(&m_syncTimer, &QTimer::timeout, this, &PartStore::flushToDisk);
}

PartStore::~PartStore()
{
    // [safe-1] what is still queued now is claimed by no record (the record only claims what was
    // synced): it is dropped instead of being written; the task that runs is waited for, so no pool
    // thread touches the file or this object afterwards (a hung disk: a last resort of 2 minutes)
    m_syncTimer.stop();
    m_io.discardPending();
    m_io.drain(120000);
    QMutexLocker l(&m_sh->mutex);
    m_sh->file.reset();
}

void PartStore::open()
{
    auto sh = m_sh;
    const qint64 size = m_size;
    const bool prealloc = m_prealloc;
    const RangeSet claimed = m_have;   // [safe-1] what the stored record says is on the disk
    m_io.run([this, sh, size, prealloc, claimed] {
        QString err;
        bool ok = true, full = false;
        RangeSet lost;
        {
            QMutexLocker l(&sh->mutex);
            if (!sh->file) {
                // [safe-1] a record that claims more than the file holds (the file is missing, shorter,
                // or was replaced): those claims are dropped before anything trusts them
                const qint64 onDisk = QFileInfo::exists(sh->path) ? QFileInfo(sh->path).size() : 0;
                if (!claimed.isEmpty() && claimed.last() > onDisk) lost = claimed.intersected(onDisk, kUnknownEnd);
                QDir().mkpath(QFileInfo(sh->path).absolutePath());
                auto f = std::make_unique<QFile>(sh->path);
                if (!f->open(QIODevice::ReadWrite | QIODevice::Unbuffered)) {
                    ok = false;
                    err = f->errorString();
                } else {
                    if (prealloc && size > 0 && f->size() < size && !f->resize(size)) {
                        ok = false;
                        full = looksLikeDiskFull(sh->path, size - f->size());
                        err = full ? QStringLiteral("Disk full") : f->errorString();
                    }
                    sh->file = std::move(f);
                }
            }
        }
        if (!lost.isEmpty()) IoQueue::post(this, [this, lost] { onShortFile(lost); });
        if (!ok) IoQueue::post(this, [this, err, full] { m_error = true; emit storeError(err, full); });
    });
}

void PartStore::onShortFile(const RangeSet &lost)
{
    qWarning("octomx: part file %s holds less than its record claims: %lld bytes are downloaded again", qPrintable(m_path),
             lost.total());
    m_have.remove(lost);
    m_durable.remove(lost);
    m_synced.remove(lost);
    m_verified.remove(lost);
    publishVerified();
    emit changed();
}

void PartStore::setSize(qint64 size)
{
    if (size < 0 || size == m_size) return;
    m_size = size;
    m_have.remove(size, kUnknownEnd);
    m_durable.remove(size, kUnknownEnd);
    m_synced.remove(size, kUnknownEnd);
    m_verified.remove(size, kUnknownEnd);
    publishVerified();
    emit changed();
}

void PartStore::restore(const RangeSet &held, const RangeSet &verified)
{
    const qint64 end = m_size >= 0 ? m_size : kUnknownEnd;
    m_verified = verified.intersected(0, end);
    m_have = held.intersected(0, end);
    m_have.add(m_verified);
    m_durable = m_have;
    m_synced = m_have;   // [safe-1] a record only ever claimed synced bytes
    publishVerified();
}

RangeSet PartStore::persistable() const
{
    return m_synced.intersected(m_have);
}

RangeSet PartStore::persistableVerified() const
{
    return m_synced.intersected(m_verified);
}

RangeSet PartStore::written() const
{
    RangeSet r = m_durable.intersected(m_have);
    r.add(m_verified);
    return r;
}

qint64 PartStore::unsyncedBytes() const
{
    RangeSet r = m_have;
    r.remove(m_synced);
    return r.total();
}

void PartStore::flushToDisk()
{
    m_syncTimer.stop();
    if (m_unsynced.isEmpty() || m_closed) return;
    const RangeSet covered = m_unsynced;
    m_unsynced.clear();
    m_unsyncedBytes = 0;
    ++m_syncsInFlight;
    auto sh = m_sh;
    m_io.run([this, sh, covered] {
        bool ok = false;
        {
            QMutexLocker l(&sh->mutex);
            if (sh->file) {
                ok = durable::syncFile(*sh->file);
                if (ok) durable::noteSynced(sh->path);
            }
        }
        IoQueue::post(this, [this, covered, ok] { onSynced(covered, ok); });
    });
}

void PartStore::onSynced(const RangeSet &covered, bool ok)
{
    m_syncsInFlight = qMax(0, m_syncsInFlight - 1);
    ++m_syncs;
    if (ok) {
        // the writes queued before the sync were all done before it (one queue, in order), and their
        // results came back first: what is durable of them is on the disk now
        const qint64 before = m_synced.total();
        m_synced.add(covered.intersected(m_durable));
        if (m_synced.total() != before) emit changed();   // the record may claim more now: store it soon
        return;
    }
    if (m_closed) return;
    m_have.remove(covered);
    m_durable.remove(covered);
    if (!m_error) {
        m_error = true;
        emit storeError(QStringLiteral("cannot write the part file to the disk"), looksLikeDiskFull(m_path, 1));
    }
}

bool PartStore::syncNow(int timeoutMs)
{
    flushToDisk();
    const bool drained = m_io.drain(timeoutMs);
    QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);   // the results of what ran
    return drained && m_syncsInFlight == 0 && (m_closed || m_unsynced.isEmpty());   // closed: moved or dropped, nothing to claim
}

bool PartStore::readMoveMarker(const QString &partPath, QString *target, QString *tempCopy)
{
    QFile m(moveMarkerPath(partPath));
    if (!m.open(QIODevice::ReadOnly)) return false;
    const QStringList l = QString::fromUtf8(m.read(64 * 1024)).split(QLatin1Char('\n'));
    if (target) *target = l.value(0).trimmed();
    if (tempCopy) *tempCopy = l.value(1).trimmed();
    return true;
}

void PartStore::setEd2kParts(const QList<QByteArray> &parts)
{
    m_scheme = Ed2kParts;
    m_unit = Ed2kPart;
    m_hashes = parts;
    verifyHeldUnits();
}

void PartStore::setTthLeaves(qint64 unit, const QList<QByteArray> &leaves)
{
    if (m_scheme == Ed2kParts) return;   // keep the preferred scheme
    m_scheme = TthLeaves;
    m_unit = unit;
    m_hashes = leaves;
    verifyHeldUnits();
}

void PartStore::clearUnits()
{
    m_scheme = NoUnits;
    m_unit = Ed2kPart;
    m_hashes.clear();
    m_verifying.clear();
    ++m_unitsGen;          // results of checks in flight are ignored
    m_have.add(m_verified);
    m_verified = RangeSet();
    publishVerified();
    emit changed();
}

bool PartStore::fullyHeld() const { return m_size >= 0 && m_have.contains(0, m_size); }
bool PartStore::fullyVerified() const { return m_size >= 0 && m_verified.contains(0, m_size); }

void PartStore::verifyHeldUnits()
{
    if (m_size <= 0 || m_scheme == NoUnits) return;
    checkUnitsIn(0, m_size);
}

void PartStore::discardUnverified()
{
    m_have = m_verified;
    m_durable = m_durable.intersected(m_verified);
    m_synced = m_synced.intersected(m_verified);
    m_unsynced = m_unsynced.intersected(m_verified);
    emit changed();
}

void PartStore::markAllVerified()
{
    if (m_size < 0) return;
    m_verified.add(0, m_size);
    m_have.add(0, m_size);
    m_durable.add(0, m_size);   // [safe-1] the whole-file check synced the file before reading it
    m_synced.add(0, m_size);
    publishVerified();
    emit changed();
}

RangeSet PartStore::fetchTarget() const
{
    if (m_size < 0) return RangeSet(m_have.runEnd(0), kUnknownEnd);
    RangeSet t = m_have.complement(0, m_size);
    return t;
}

Range PartStore::unitAt(qint64 pos) const
{
    const qint64 u = (m_scheme == NoUnits || m_unit <= 0) ? Ed2kPart : m_unit;
    const qint64 s = (pos / u) * u;
    qint64 e = s + u;
    if (m_size >= 0) e = qMin(e, m_size);
    return Range(s, qMax(e, s));
}

bool PartStore::canVerifyUnit(qint64 pos) const
{
    if (m_scheme == NoUnits || m_size <= 0) return false;
    return int(pos / m_unit) < m_hashes.size();
}

RangeSet PartStore::deliver(qint64 off, const QByteArray &bytes)
{
    RangeSet accepted;
    if (m_error || m_closed || bytes.isEmpty() || off < 0) return accepted;
    qint64 end = off + bytes.size();
    if (m_size >= 0) end = qMin(end, m_size);
    if (end <= off) return accepted;
    RangeSet need = m_have.complement(off, end);
    for (qint64 v : std::as_const(m_verifying)) need.remove(unitAt(v).first, unitAt(v).second);
    if (need.isEmpty()) return accepted;
    auto sh = m_sh;
    for (const Range &r : need.ranges()) {
        const QByteArray slice = bytes.mid(int(r.first - off), int(r.second - r.first));
        m_have.add(r.first, r.second);
        accepted.add(r.first, r.second);
        m_unsynced.add(r.first, r.second);   // [safe-1]
        m_unsyncedBytes += slice.size();
        m_pendingWrite += slice.size();
        const qint64 o = r.first;
        m_io.run([this, sh, o, slice] {
            bool ok = false, full = false;
            QString err;
            {
                QMutexLocker l(&sh->mutex);
                if (sh->file) {   // [safe-1] through the test seams (slow disk, power cut journal)
                    ok = durable::journalWrite(*sh->file, sh->path, o, slice) == slice.size();
                    if (!ok) err = sh->file->errorString();
                } else {
                    err = sh->file ? sh->file->errorString() : QStringLiteral("part file not open");
                }
            }
            if (!ok) {
                full = looksLikeDiskFull(sh->path, slice.size());
                if (full) err = QStringLiteral("Disk full");
            }
            const qint64 n = slice.size();
            IoQueue::post(this, [this, o, n, ok, err, full] { onWritten(o, n, ok, err, full); });
        });
    }
    // [safe-1] the sync schedule (a unit verified meanwhile is claimed once its bytes are synced)
    if (m_unsyncedBytes >= kSyncBytes) flushToDisk();
    else if (!m_unsynced.isEmpty() && !m_syncTimer.isActive()) m_syncTimer.start();
    for (const Range &r : accepted.ranges()) checkUnitsIn(r.first, r.second);
    return accepted;
}

void PartStore::onWritten(qint64 off, qint64 n, bool ok, const QString &err, bool diskFull)
{
    m_pendingWrite = qMax<qint64>(0, m_pendingWrite - n);
    if (ok) {
        if (m_have.contains(off, off + n)) m_durable.add(off, off + n);
        return;
    }
    m_have.remove(off, off + n);
    m_durable.remove(off, off + n);
    m_synced.remove(off, off + n);
    if (!m_error) {
        m_error = true;
        emit storeError(err, diskFull);
    }
}

void PartStore::checkUnitsIn(qint64 start, qint64 end)
{
    if (m_scheme == NoUnits || m_size <= 0) return;
    qint64 pos = (start / m_unit) * m_unit;
    for (; pos < end && pos < m_size; pos += m_unit) {
        const Range u = unitAt(pos);
        if (m_verifying.contains(u.first) || m_verified.contains(u.first, u.second)) continue;
        if (!m_have.contains(u.first, u.second)) continue;
        if (!canVerifyUnit(u.first)) continue;
        queueVerify(u.first, u.second);
    }
}

void PartStore::queueVerify(qint64 s, qint64 e)
{
    m_verifying.insert(s);
    const int idx = int(s / m_unit);
    const QByteArray expect = m_hashes.value(idx);
    const Scheme scheme = m_scheme;
    const quint64 gen = m_unitsGen;
    auto sh = m_sh;
    m_io.run([this, sh, s, e, expect, scheme, gen] {
        bool ok = false;
        {
            QMutexLocker l(&sh->mutex);
            if (sh->file && sh->file->seek(s)) {
                std::unique_ptr<hash::Hasher> h;
                if (scheme == Ed2kParts) h = std::make_unique<hash::Md4>();
                else h = std::make_unique<hash::TigerTree>();
                qint64 left = e - s;
                bool readOk = true;
                while (left > 0) {
                    const QByteArray b = sh->file->read(qMin<qint64>(left, 1 << 20));
                    if (b.isEmpty()) { readOk = false; break; }
                    h->update(b);
                    left -= b.size();
                }
                ok = readOk && h->finish() == expect;
            }
        }
        IoQueue::post(this, [this, s, e, ok, gen] { onVerified(s, e, ok, gen); });
    });
}

void PartStore::onVerified(qint64 s, qint64 e, bool ok, quint64 gen)
{
    if (gen != m_unitsGen) return;   // the unit scheme changed meanwhile
    m_verifying.remove(s);
    if (ok) {
        m_verified.add(s, e);
        publishVerified();
        // [safe-1] a verified unit of 1 MiB or more is synced now, so the record can claim it at once
        if (e - s >= (qint64(1) << 20) && m_unsynced.intersects(s, e)) flushToDisk();
        emit unitVerified(s, e);
    } else {
        m_have.remove(s, e);
        m_durable.remove(s, e);
        m_synced.remove(s, e);
        emit unitFailed(s, e);
    }
    emit changed();
}

void PartStore::publishVerified()
{
    QMutexLocker l(&m_sh->mutex);
    m_sh->verified = m_verified;
}

qint64 PartStore::keptBlockUnit(qint64 size)
{
    qint64 u = 256 * 1024;
    while (size / u > 4096) u *= 2;
    return u;
}

void PartStore::finalCheck(std::function<void(const FinalResult &)> done)
{
    auto sh = m_sh;
    const std::atomic<bool> *cancel = m_io.cancelFlag();
    m_io.run([this, sh, cancel, done] {
        testHold(QStringLiteral("final check"), cancel);
        FinalResult r;
        {   // [safe-1] the file is on the disk before it is checked and announced
            QMutexLocker l(&sh->mutex);
            if (sh->file && durable::syncFile(*sh->file)) durable::noteSynced(sh->path);
        }
        hash::MultiHasher h(hash::MultiHasher::Default | hash::MultiHasher::Md5);
        r.ok = hash::MultiHasher::hashFile(sh->path, h, cancel, {}, &r.error);
        if (r.ok) {
            r.hashes = h.hashes();
            r.ed2kParts = h.ed2kPartHashes();
            r.aichParts = h.aichPartHashes();
            r.tthUnit = keptBlockUnit(h.bytesHashed());   // [ver-1]
            r.tthLevel = h.tthLevel(r.tthUnit);
            if (r.tthLevel.isEmpty()) r.tthUnit = 0;
        }
        IoQueue::post(this, [r, done] { done(r); });
    });
}

void PartStore::moveTo(const QString &dir, const QString &name, std::function<void(bool, const QString &, const QString &)> done,
                       bool allowCopy)
{
    m_closed = true;
    auto sh = m_sh;
    m_io.run([this, sh, dir, name, done, allowCopy] {
        testHold(QStringLiteral("move"), nullptr);
        QString err, target;
        bool ok = false;
        {
            QMutexLocker l(&sh->mutex);
            if (sh->file && durable::syncFile(*sh->file)) durable::noteSynced(sh->path);   // [safe-1]
            sh->file.reset();
            sh->verified.clear();
            QDir().mkpath(dir);
            target = uniqueTarget(dir, name);
            const QString part = sh->path;
            if (!writeMoveMarker(part, target)) {   // [safe-1]
                err = QStringLiteral("cannot write to the incomplete folder");
            } else {
                ok = moveFile(part, target, &err, allowCopy);
                if (ok) sh->path = target;
            }
        }
        IoQueue::post(this, [ok, target, err, done] { done(ok, target, err); });
    });
}

void PartStore::moveExact(const QString &target, std::function<void(bool, const QString &)> done)
{
    m_closed = true;
    auto sh = m_sh;
    m_io.run([this, sh, target, done] {
        QString err;
        bool ok = false;
        {
            QMutexLocker l(&sh->mutex);
            if (sh->file && durable::syncFile(*sh->file)) durable::noteSynced(sh->path);   // [safe-1]
            sh->file.reset();
            sh->verified.clear();
            QDir().mkpath(QFileInfo(target).absolutePath());
            if (QFileInfo::exists(target)) {
                err = QStringLiteral("target exists: %1").arg(target);
            } else {
                ok = moveFile(sh->path, target, &err);
                if (ok) sh->path = target;
            }
        }
        IoQueue::post(this, [ok, err, done] { done(ok, err); });
    });
}

void PartStore::afterWrites(std::function<void()> done)
{
    m_io.run([this, done] { IoQueue::post(this, done); });
}

QList<Range> PartStore::completeUnits() const
{
    QList<Range> out;
    if (m_scheme == NoUnits || m_size <= 0 || m_unit <= 0) return out;
    for (qint64 pos = 0; pos < m_size; pos += m_unit) {
        const Range u = unitAt(pos);
        if (m_verifying.contains(u.first) || !canVerifyUnit(u.first)) continue;
        if (m_have.contains(u.first, u.second)) out << u;
    }
    return out;
}

void PartStore::recheckResult(qint64 s, qint64 e, bool ok)
{
    if (ok) {
        m_have.add(s, e);
        m_verified.add(s, e);
    } else {
        m_have.remove(s, e);
        m_durable.remove(s, e);
        m_synced.remove(s, e);   // [safe-1] fetched again: claimed again only once rewritten and synced
        m_verified.remove(s, e);
    }
    publishVerified();
    emit changed();
}

void PartStore::finishInPlace(std::function<void(bool, const QString &, const QString &)> done)
{
    m_closed = true;
    auto sh = m_sh;
    m_io.run([this, sh, done] {
        QString path;
        {
            QMutexLocker l(&sh->mutex);
            if (sh->file && durable::syncFile(*sh->file)) durable::noteSynced(sh->path);   // synced before it is announced
            sh->file.reset();
            sh->verified.clear();
            path = sh->path;
        }
        const bool ok = QFileInfo::exists(path);
        IoQueue::post(this, [ok, path, done] { done(ok, path, ok ? QString() : QStringLiteral("the file is gone")); });
    });
}

void PartStore::close()
{
    m_closed = true;
    m_syncTimer.stop();
    m_io.discardPending();
    auto sh = m_sh;
    QMutexLocker l(&sh->mutex);
    sh->file.reset();
    sh->verified.clear();
}

void PartStore::removeFile()
{
    m_closed = true;
    m_io.discardPending();
    auto sh = m_sh;
    // a fresh queue task would not run after discardPending's cancel flag; do it inline under the lock
    QMutexLocker l(&sh->mutex);
    sh->file.reset();
    sh->verified.clear();
    QFile::remove(sh->path);
}

void PartStore::sha1Range(qint64 s, qint64 e, std::function<void(const QByteArray &)> done)
{
    auto sh = m_sh;
    m_io.run([this, sh, s, e, done] {
        QByteArray out;
        {
            QMutexLocker l(&sh->mutex);
            if (sh->file && sh->file->seek(s)) {
                QCryptographicHash h(QCryptographicHash::Sha1);
                qint64 left = e - s;
                bool ok = true;
                while (left > 0) {
                    const QByteArray b = sh->file->read(qMin<qint64>(left, 1 << 20));
                    if (b.isEmpty()) { ok = false; break; }
                    h.addData(b);
                    left -= b.size();
                }
                if (ok) out = h.result();
            }
        }
        IoQueue::post(this, [out, done] { done(out); });
    });
}

void PartStore::sha1Ranges(const QList<Range> &ranges, std::function<void(const QList<QByteArray> &)> done)
{
    auto sh = m_sh;
    m_io.run([this, sh, ranges, done] {
        QList<QByteArray> out;
        {
            QMutexLocker l(&sh->mutex);
            for (const Range &r : ranges) {
                QByteArray res;
                if (sh->file && sh->file->seek(r.first)) {
                    QCryptographicHash h(QCryptographicHash::Sha1);
                    qint64 left = r.second - r.first;
                    while (left > 0) {
                        const QByteArray b = sh->file->read(qMin<qint64>(left, 1 << 20));
                        if (b.isEmpty()) break;
                        h.addData(b);
                        left -= b.size();
                    }
                    if (left == 0) res = h.result();
                }
                out << res;
            }
        }
        IoQueue::post(this, [out, done] { done(out); });
    });
}

QByteArray PartStore::readVerified(qint64 off, qint64 len) const
{
    return verifiedReader()(off, len);
}

std::function<QByteArray(qint64, qint64)> PartStore::verifiedReader() const
{
    std::shared_ptr<Shared> sh = m_sh;
    return [sh](qint64 off, qint64 len) -> QByteArray {
        QString path;
        {
            QMutexLocker l(&sh->mutex);
            if (len <= 0 || off < 0 || !sh->verified.contains(off, off + len)) return {};
            path = sh->path;
        }
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly) || !f.seek(off)) return {};
        return f.read(qMin<qint64>(len, 16 << 20));
    };
}

void PartStore::pieceMap(int cells, const RangeSet &inFlight, QByteArray *state, QByteArray *fill) const
{
    if (m_size <= 0) {
        state->clear();
        fill->clear();
        return;
    }
    // without fine units nothing can be verified before the whole-file check: the held bytes are
    // what TransferInfo::done counts, so they are the "complete" part of the map
    const RangeSet complete = m_scheme == NoUnits ? m_have : m_verified;
    // in progress: the requested ranges, plus (fine units) the bytes already received for a unit
    // that is still arriving or is being checked now; held bytes of a unit nobody sends any more
    // are neither (not verified, not arriving)
    RangeSet progress = inFlight;
    if (m_scheme != NoUnits) {
        RangeSet pending = m_have;
        pending.remove(m_verified);
        if (!pending.isEmpty()) {
            for (const Range &r : inFlight.ranges()) {
                for (qint64 u = unitAt(r.first).first; u < r.second;) {
                    const Range ur = unitAt(u);
                    if (ur.second <= ur.first) break;
                    progress.add(pending.intersected(ur.first, ur.second));
                    u = ur.second;
                }
            }
            for (qint64 u : m_verifying) {
                const Range ur = unitAt(u);
                progress.add(pending.intersected(ur.first, ur.second));
            }
        }
    }
    condensePieceMap(cells, {Range(0, m_size)}, complete, progress, state, fill);
}

void PartStore::mapOf(int cells, qint64 size, const RangeSet &complete, QByteArray *state, QByteArray *fill)
{
    state->clear();
    fill->clear();
    if (size > 0) condensePieceMap(cells, {Range(0, size)}, complete, RangeSet(), state, fill);
}

void condensePieceMap(int cells, const QList<Range> &spans, const RangeSet &complete, const RangeSet &progress,
                      QByteArray *state, QByteArray *fill)
{
    state->clear();
    fill->clear();
    qint64 total = 0;
    for (const Range &r : spans) total += qMax<qint64>(0, r.second - r.first);
    if (total <= 0 || cells <= 0) return;
    const int n = int(qMin<qint64>(cells, total));
    state->fill(char(0), n);
    fill->fill(char(0), n);
    int si = 0;              // span holding the current axis position
    qint64 spanAxis = 0;     // axis position where span si starts
    for (int i = 0; i < n; ++i) {
        const qint64 s = total * i / n, e = total * (i + 1) / n;   // e > s since n <= total
        qint64 done = 0;
        bool moving = false;
        // walk the spans covering axis [s, e)
        while (si < spans.size() && spanAxis + (spans[si].second - spans[si].first) <= s) {
            spanAxis += qMax<qint64>(0, spans[si].second - spans[si].first);
            ++si;
        }
        qint64 a = spanAxis;
        for (int k = si; k < spans.size() && a < e; ++k) {
            const qint64 len = qMax<qint64>(0, spans[k].second - spans[k].first);
            const qint64 from = qMax(s, a), to = qMin(e, a + len);
            if (to > from) {
                const qint64 bs = spans[k].first + (from - a), be = spans[k].first + (to - a);
                done += complete.coveredIn(bs, be);
                if (!moving && progress.intersects(bs, be)) {
                    RangeSet p = progress.intersected(bs, be);
                    p.remove(complete);
                    moving = !p.isEmpty();
                }
            }
            a += len;
        }
        const qint64 len = e - s;
        int f = int((done * 255 + len / 2) / len);
        if (done > 0 && f < 1) f = 1;
        if (done < len && f > 254) f = 254;
        (*fill)[i] = char(f);
        (*state)[i] = char(done >= len ? 2 : moving ? 1 : 0);
    }
}

} // namespace mx::engine
