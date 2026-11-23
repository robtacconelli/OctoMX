// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_support.h"

#include "mxhash/multi_hasher.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QRandomGenerator>

namespace gnuttest {

mx::SharedFile FileLibrary::addFile(const QString &path)
{
    mx::SharedFile f;
    QFileInfo fi(path);
    f.path = fi.absoluteFilePath();
    f.name = fi.fileName();
    f.size = fi.size();
    f.mtime = fi.lastModified();
    f.type = mx::mediaTypeForFileName(f.name);
    mx::hash::MultiHasher h(mx::hash::MultiHasher::Sha1 | mx::hash::MultiHasher::Tth);
    if (!mx::hash::MultiHasher::hashFile(path, h)) return f;
    const mx::FileHashes hs = h.hashes();
    f.hashes.sha1 = hs.sha1;
    f.hashes.tth = hs.tth;
    f.hashed = true;
    QMutexLocker l(&m_mx);
    m_files.insert(f.hashes.sha1, f);
    return f;
}

QList<mx::SharedFile> FileLibrary::sharedFiles() const
{
    QMutexLocker l(&m_mx);
    return m_files.values();
}

bool FileLibrary::findByHash(const mx::FileHashes &h, mx::SharedFile *out) const
{
    QMutexLocker l(&m_mx);
    for (const mx::SharedFile &f : m_files)
        if (f.hashes.matches(h)) { *out = f; return true; }
    return false;
}

QByteArray FileLibrary::read(const mx::FileHashes &h, qint64 offset, qint64 length)
{
    mx::SharedFile f;
    if (!findByHash(h, &f)) return {};
    QFile file(f.path);
    if (!file.open(QIODevice::ReadOnly) || !file.seek(offset)) return {};
    const QByteArray d = file.read(length);
    QMutexLocker l(&m_mx);
    m_read += d.size();
    return d;
}

mx::PluginContext makeContext(mx::Settings *s, mx::LibraryAccess *lib, mx::RateLimiter *lim, const QString &profile)
{
    mx::PluginContext c;
    c.settings = s;
    c.library = lib;
    c.limiter = lim;
    c.profileDir = profile;
    c.dataDir = profile + QStringLiteral("/gnutella");
    QDir().mkpath(c.dataDir);
    c.screenName = QStringLiteral("Tester");
    c.clientName = QStringLiteral("OctoMX");
    c.clientVersion = QStringLiteral(MX_VERSION_STRING);
    return c;
}

QByteArray makeTestData(qint64 size, quint32 seed)
{
    QRandomGenerator r(seed);
    QByteArray d(int(size), Qt::Uninitialized);
    for (int i = 0; i < d.size(); ++i) d[i] = char(r.bounded(256));
    return d;
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    return f.write(data) == data.size();
}

MemDownloader::MemDownloader(qint64 size, qint64 from, qint64 to, QObject *parent)
    : QObject(parent), m_from(from), m_to(to < 0 ? size : to)
{
    m_data = QByteArray(int(m_to - m_from), '\0');
    m_missing << qMakePair(m_from, m_to);
    m_retry.setInterval(300);
    connect(&m_retry, &QTimer::timeout, this, &MemDownloader::schedule);
    m_retry.start();
}

void MemDownloader::addSource(mx::RangeSource *s)
{
    m_sources << s;
    connect(s, &mx::RangeSource::stateChanged, this, [this, s] {
        m_states << int(s->state());
        m_log << QStringLiteral("state %1 %2").arg(int(s->state())).arg(s->stateText());
        schedule();
    });
    connect(s, &mx::RangeSource::data, this, [this](qint64 off, const QByteArray &b) {
        if (off < m_from || off + b.size() > m_to) { m_log << QStringLiteral("data outside range %1").arg(off); return; }
        memcpy(m_data.data() + (off - m_from), b.constData(), size_t(b.size()));
        m_received += b.size();
    });
    connect(s, &mx::RangeSource::requestFinished, this, [this, s](qint64 off, qint64 len, bool ok, const QString &err) {
        m_inflight.remove(s);
        if (!ok) {
            ++m_failed;
            m_log << QStringLiteral("request %1+%2 failed: %3").arg(off).arg(len).arg(err);
            m_missing.prepend(qMakePair(off, off + len));
        }
        QTimer::singleShot(0, this, &MemDownloader::schedule);
    });
    s->open();
}

void MemDownloader::schedule()
{
    if (done()) {
        if (!m_emitted) { m_emitted = true; m_retry.stop(); emit finished(); }
        return;
    }
    for (const QPointer<mx::RangeSource> &sp : m_sources) {
        mx::RangeSource *s = sp.data();
        if (!s || m_missing.isEmpty()) continue;
        if (m_inflight.contains(s)) continue;
        if (s->state() != mx::RangeSource::Ready && s->state() != mx::RangeSource::Busy) continue;
        QPair<qint64, qint64> r = m_missing.takeFirst();
        const qint64 chunk = s->preferredRequestSize();
        if (r.second - r.first > chunk) {
            m_missing.prepend(qMakePair(r.first + chunk, r.second));
            r.second = r.first + chunk;
        }
        m_inflight.insert(s, r);
        s->request(r.first, r.second - r.first);
    }
}

} // namespace gnuttest
