// SPDX-License-Identifier: GPL-3.0-or-later
#include "mxhash/multi_hasher.h"

#include "mxhash/aich.h"
#include "mxhash/bt_pieces.h"
#include "mxhash/ed2k.h"
#include "mxhash/hasher.h"
#include "mxhash/tiger_tree.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QFile>

namespace mx::hash {

MultiHasher::MultiHasher(int kinds, qint64 btPieceLength)
    : m_kinds(kinds)
    , m_btPieceLength(btPieceLength > 0 ? btPieceLength : 256 * 1024)
{
    reset();
}

MultiHasher::~MultiHasher() = default;

void MultiHasher::reset()
{
    m_bytes = 0;
    m_finished = false;
    m_result = mx::FileHashes();
    m_ed2k.reset((m_kinds & Ed2k) ? new Ed2kHasher : nullptr);
    m_aich.reset((m_kinds & Aich) ? new AichHasher : nullptr);
    m_tth.reset((m_kinds & Tth) ? new TigerTree : nullptr);
    m_sha1.reset((m_kinds & Sha1) ? new CryptoHasher(QCryptographicHash::Sha1) : nullptr);
    m_sha256.reset((m_kinds & Sha256) ? new CryptoHasher(QCryptographicHash::Sha256) : nullptr);
    m_md5.reset((m_kinds & Md5) ? new CryptoHasher(QCryptographicHash::Md5) : nullptr);
    m_bt.reset((m_kinds & BtPieces) ? new BtPieceHasher(m_btPieceLength) : nullptr);
}

void MultiHasher::update(const char *data, qint64 len)
{
    if (len <= 0 || m_finished) return;
    m_bytes += len;
    if (m_ed2k) m_ed2k->update(data, len);
    if (m_aich) m_aich->update(data, len);
    if (m_tth) m_tth->update(data, len);
    if (m_sha1) m_sha1->update(data, len);
    if (m_sha256) m_sha256->update(data, len);
    if (m_md5) m_md5->update(data, len);
    if (m_bt) m_bt->update(data, len);
}

void MultiHasher::finish()
{
    if (m_finished) return;
    m_finished = true;
    if (m_ed2k) m_result.ed2k = m_ed2k->finish();
    if (m_aich) m_result.aich = m_aich->finish();
    if (m_tth) m_result.tth = m_tth->finish();
    if (m_sha1) m_result.sha1 = m_sha1->finish();
    if (m_sha256) m_result.sha256 = m_sha256->finish();
    if (m_md5) m_result.md5 = m_md5->finish();
    if (m_bt) m_bt->finish();
}

mx::FileHashes MultiHasher::hashes() const { return m_result; }

QList<QByteArray> MultiHasher::ed2kPartHashes() const
{
    return m_ed2k ? m_ed2k->partHashes() : QList<QByteArray>();
}

QList<QByteArray> MultiHasher::aichPartHashes() const
{
    return m_aich ? m_aich->partHashes() : QList<QByteArray>();
}

QList<QByteArray> MultiHasher::tthLevel(qint64 unitBytes) const
{
    return m_tth ? m_tth->levelForUnit(unitBytes) : QList<QByteArray>();
}

QList<QByteArray> MultiHasher::btPieces() const
{
    return m_bt ? m_bt->pieces() : QList<QByteArray>();
}

bool MultiHasher::hashFile(const QString &path, MultiHasher &h, const std::atomic<bool> *cancel,
                           const std::function<void(qint64)> &progress, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    constexpr qint64 BufSize = 2 * 1024 * 1024;
    constexpr qint64 ProgressEvery = 8 * 1024 * 1024;
    QByteArray buf(BufSize, Qt::Uninitialized);
    qint64 done = 0, lastProgress = 0;
    for (;;) {
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            if (error) *error = QStringLiteral("cancelled");
            return false;
        }
        const qint64 n = f.read(buf.data(), BufSize);
        if (n < 0) {
            if (error) *error = f.errorString();
            return false;
        }
        if (n == 0) break;
        h.update(buf.constData(), n);
        done += n;
        if (progress && done - lastProgress >= ProgressEvery) {
            lastProgress = done;
            progress(done);
        }
    }
    h.finish();
    if (progress) progress(done);
    return true;
}

} // namespace mx::hash
