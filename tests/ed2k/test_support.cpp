// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_support.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>

namespace mxtest {

static constexpr qint64 PART = 9728000;
static constexpr qint64 CHUNK = 512000;

Ed2kFileHash hashFile(const QString &path, bool *ok)
{
    Ed2kFileHash r;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { if (ok) *ok = false; return r; }
    const qint64 size = f.size();
    QCryptographicHash part(QCryptographicHash::Md4);
    qint64 inPart = 0;
    QByteArray buf;
    while (!(buf = f.read(1 << 20)).isEmpty()) {
        qsizetype pos = 0;
        while (pos < buf.size()) {
            const qint64 n = qMin<qint64>(buf.size() - pos, PART - inPart);
            part.addData(QByteArrayView(buf.constData() + pos, n));
            inPart += n;
            pos += n;
            if (inPart == PART) { r.parts.append(part.result()); part.reset(); inPart = 0; }
        }
    }
    if (size < PART) {
        r.parts = {part.result()};
        r.fileHash = r.parts[0];
    } else {
        r.parts.append(part.result());   // last partial part, or MD4("") for exact multiples
        QByteArray cat;
        for (const QByteArray &h : r.parts) cat.append(h);
        r.fileHash = QCryptographicHash::hash(cat, QCryptographicHash::Md4);
    }
    if (ok) *ok = true;
    return r;
}

mx::SharedFile FileLibrary::addFile(const QString &path)
{
    bool ok = false;
    const Ed2kFileHash h = hashFile(path, &ok);
    mx::SharedFile f;
    if (!ok) return f;
    QFileInfo fi(path);
    f.path = fi.absoluteFilePath();
    f.name = fi.fileName();
    f.size = fi.size();
    f.mtime = fi.lastModified();
    f.type = mx::mediaTypeForFileName(f.name);
    f.hashes.ed2k = h.fileHash;
    f.hashed = true;
    QMutexLocker l(&m_mx);
    m_files.insert(h.fileHash, Entry{f, h.parts});
    return f;
}

QList<mx::SharedFile> FileLibrary::sharedFiles() const
{
    QMutexLocker l(&m_mx);
    QList<mx::SharedFile> out;
    for (const Entry &e : m_files) out.append(e.f);
    return out;
}

bool FileLibrary::findByHash(const mx::FileHashes &h, mx::SharedFile *out) const
{
    QMutexLocker l(&m_mx);
    auto it = m_files.constFind(h.ed2k);
    if (it == m_files.cend()) return false;
    if (out) *out = it->f;
    return true;
}

QByteArray FileLibrary::read(const mx::FileHashes &h, qint64 offset, qint64 length)
{
    QString path;
    {
        QMutexLocker l(&m_mx);
        auto it = m_files.constFind(h.ed2k);
        if (it == m_files.cend()) return QByteArray();
        path = it->f.path;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || !f.seek(offset)) return QByteArray();
    const QByteArray b = f.read(length);
    QMutexLocker l(&m_mx);
    m_read += b.size();
    return b;
}

QList<QByteArray> FileLibrary::ed2kPartHashes(const mx::FileHashes &h) const
{
    QMutexLocker l(&m_mx);
    auto it = m_files.constFind(h.ed2k);
    return it == m_files.cend() ? QList<QByteArray>() : it->parts;
}

mx::PluginContext makeContext(mx::Settings *s, mx::LibraryAccess *lib, mx::RateLimiter *lim, const QString &profile)
{
    mx::PluginContext c;
    c.settings = s;
    c.library = lib;
    c.limiter = lim;
    c.profileDir = profile;
    c.dataDir = QDir(profile).filePath(QStringLiteral("ed2k"));
    c.screenName = QStringLiteral("OctoMX-test");
    c.clientName = QStringLiteral("OctoMX");
    c.clientVersion = QStringLiteral("4.0");
    return c;
}

// ---------------------------------------------------------------- SimpleDownloader
SimpleDownloader::SimpleDownloader(const QString &path, qint64 size, const QByteArray &ed2k, QObject *parent)
    : QObject(parent), m_file(path), m_size(size), m_hash(ed2k)
{
    m_clock.start();
    if (!m_file.open(QIODevice::ReadWrite)) qWarning("cannot open %s", qPrintable(path));
    m_file.resize(size);
    for (qint64 a = 0; a < size; a += CHUNK) {
        Chunk c;
        c.start = a;
        c.end = qMin(size, a + CHUNK);
        m_chunks.append(c);
    }
    m_wantFrom = 0;
    m_wantTo = size;
}

SimpleDownloader::~SimpleDownloader()
{
    for (auto &s : m_sources) if (s) { s->disconnect(this); s->close(); s->deleteLater(); }
}

void SimpleDownloader::setWanted(qint64 from, qint64 to)
{
    m_wantFrom = qMax<qint64>(0, from);
    m_wantTo = qMin(m_size, to);
}

void SimpleDownloader::setPartHashes(const QList<QByteArray> &h)
{
    if (!m_partHashes.isEmpty()) return;
    m_partHashes = h;
    for (int p = 0; p * PART < m_size; ++p) checkPart(p);
}

void SimpleDownloader::addSource(mx::RangeSource *s)
{
    m_sources.append(s);
    connect(s, &mx::RangeSource::stateChanged, this, [this] { schedule(); });
    connect(s, &mx::RangeSource::availabilityChanged, this, [this] { schedule(); });
    connect(s, &mx::RangeSource::data, this, [this, s](qint64 off, const QByteArray &b) { onData(s, off, b); });
    connect(s, &mx::RangeSource::requestFinished, this, [this, s](qint64 off, qint64 len, bool ok, const QString &err) {
        onFinished(s, off, len, ok, err);
    });
    s->open();
}

QList<mx::RangeSource *> SimpleDownloader::sources() const
{
    QList<mx::RangeSource *> out;
    for (const auto &s : m_sources) if (s) out.append(s.data());
    return out;
}

bool SimpleDownloader::sourceHas(mx::RangeSource *s, qint64 a, qint64 b) const
{
    for (const auto &r : s->available()) if (r.first <= a && r.second >= b) return true;
    return false;
}

void SimpleDownloader::schedule()
{
    for (const auto &sp : m_sources) {
        mx::RangeSource *s = sp.data();
        if (!s || s->state() != mx::RangeSource::Ready || m_active.contains(s)) continue;
        int pick = -1;
        for (int i = 0; i < m_chunks.size(); ++i) {
            const Chunk &c = m_chunks[i];
            if (c.state != 0 || c.end <= m_wantFrom || c.start >= m_wantTo) continue;
            if (!sourceHas(s, c.start, c.end)) continue;
            pick = i;
            break;
        }
        if (pick < 0) continue;
        Chunk &c = m_chunks[pick];
        c.state = 1;
        c.owner = s;
        m_active.insert(s, pick);
        s->request(c.start + c.got, c.end - c.start - c.got);
    }
}

void SimpleDownloader::onData(mx::RangeSource *s, qint64 off, const QByteArray &b)
{
    auto it = m_active.find(s);
    if (it == m_active.end()) return;
    Chunk &c = m_chunks[it.value()];
    if (off != c.start + c.got || off + b.size() > c.end) {
        emit logLine(QStringLiteral("unexpected data at %1 (+%2) for chunk %3-%4").arg(off).arg(b.size()).arg(c.start).arg(c.end));
        return;
    }
    m_file.seek(off);
    m_file.write(b);
    c.got += b.size();
    m_received += b.size();
}

void SimpleDownloader::onFinished(mx::RangeSource *s, qint64, qint64, bool ok, const QString &err)
{
    auto it = m_active.find(s);
    if (it == m_active.end()) return;
    const int idx = it.value();
    m_active.erase(it);
    Chunk &c = m_chunks[idx];
    c.owner = nullptr;
    if (c.got == c.end - c.start) {
        c.state = 2;
        m_partClient.insert(int(c.start / PART), s->clientName());
        checkPart(int(c.start / PART));
    } else {
        c.state = 0;
        if (!ok) emit logLine(QStringLiteral("request failed (%1): %2").arg(s->clientName(), err));
    }
    QMetaObject::invokeMethod(this, [this] { schedule(); }, Qt::QueuedConnection);
    if (done() && !m_finishedEmitted) { m_finishedEmitted = true; emit finished(); }
}

void SimpleDownloader::checkPart(int p)
{
    const qint64 a = qint64(p) * PART, b = qMin(m_size, a + PART);
    for (const Chunk &c : m_chunks) if (c.start >= a && c.end <= b && c.state != 2) return;
    if (m_partHashes.isEmpty() || m_verifiedParts.contains(p)) return;
    m_file.flush();
    m_file.seek(a);
    const QByteArray data = m_file.read(b - a);
    const QByteArray h = QCryptographicHash::hash(data, QCryptographicHash::Md4);
    if (p < m_partHashes.size() && h == m_partHashes[p]) {
        m_verifiedParts.insert(p);
        if (m_firstVerifiedMs < 0) m_firstVerifiedMs = m_clock.elapsed();
        emit partVerified(p, m_partClient.value(p));
    } else {
        ++m_failedParts;
        for (Chunk &c : m_chunks) if (c.start >= a && c.end <= b) { c.state = 0; c.got = 0; }
        emit partFailed(p);
    }
    if (done() && !m_finishedEmitted) { m_finishedEmitted = true; emit finished(); }
}

bool SimpleDownloader::done() const
{
    for (const Chunk &c : m_chunks) {
        if (c.end <= m_wantFrom || c.start >= m_wantTo) continue;
        if (c.state != 2) return false;
    }
    if (m_partHashes.isEmpty()) return m_size < PART;   // a one-part file is checked by its file hash
    for (int p = int(m_wantFrom / PART); qint64(p) * PART < m_wantTo; ++p)
        if (!m_verifiedParts.contains(p)) return false;
    return true;
}

QString SimpleDownloader::summary() const
{
    int done = 0, active = 0;
    for (const Chunk &c : m_chunks) { if (c.state == 2) ++done; else if (c.state == 1) ++active; }
    return QStringLiteral("%1/%2 chunks, %3 active, %4 bytes received, %5 part(s) verified, %6 failed")
        .arg(done).arg(m_chunks.size()).arg(active).arg(m_received).arg(m_verifiedParts.size()).arg(m_failedParts);
}

QStringList SimpleDownloader::sourceLines() const
{
    QStringList out;
    for (const auto &s : m_sources) {
        if (!s) continue;
        const mx::SearchSource d = s->descriptor();
        qint64 avail = 0;
        for (const auto &r : s->available()) avail += r.second - r.first;
        out << QStringLiteral("  %1  %2  [%3]  has %4%")
                   .arg(d.user.address.isEmpty() ? d.user.id : d.user.address, -21)
                   .arg(s->clientName().isEmpty() ? QStringLiteral("?") : s->clientName(), -28)
                   .arg(s->stateText())
                   .arg(m_size ? avail * 100 / m_size : 0);
    }
    return out;
}

} // namespace mxtest
