// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/engine/verify_job.h"

#include "mxhash/hasher.h"
#include "mxhash/multi_hasher.h"
#include "mxhash/tiger_tree.h"

#include <QtCore/QFile>
#include <QtCore/QTimer>

namespace mx::engine {

VerifyJob::VerifyJob(QThreadPool *pool, const QString &path, QObject *parent)
    : QObject(parent), m_path(path), m_io(pool), m_cancel(std::make_shared<std::atomic<bool>>(false))
{
}

VerifyJob::~VerifyJob()
{
    *m_cancel = true;
    m_io.discardPending();
    m_io.drain(2000);
}

void VerifyJob::checkBlocks(const QList<Block> &blocks, int pauseMs)
{
    m_blocks = blocks;
    m_index = 0;
    m_pauseMs = qMax(0, pauseMs);
    m_total = 0;
    m_done = 0;
    for (const Block &b : blocks) m_total += b.end - b.start;
    m_running = true;
    QTimer::singleShot(0, this, [this] { next(); });
}

void VerifyJob::next()
{
    if (!m_running) return;
    if (*m_cancel || m_index >= m_blocks.size()) {
        m_running = false;
        emit finished(*m_cancel);
        return;
    }
    const Block b = m_blocks.at(m_index++);
    const QString path = m_path;
    auto cancel = m_cancel;
    m_io.run([this, b, path, cancel] {
        bool readable = false, ok = false;
        QFile f(path);
        if (!*cancel && f.open(QIODevice::ReadOnly) && f.seek(b.start)) {
            std::unique_ptr<hash::Hasher> h;
            if (b.kind == Md4) h = std::make_unique<hash::Md4>();
            else h = std::make_unique<hash::TigerTree>();
            qint64 left = b.end - b.start;
            readable = true;
            while (left > 0 && !*cancel) {
                const QByteArray chunk = f.read(qMin<qint64>(left, 1 << 20));
                if (chunk.isEmpty()) { readable = false; break; }
                h->update(chunk);
                left -= chunk.size();
            }
            if (*cancel) return;
            ok = readable && h->finish() == b.expect;
        }
        f.close();
        if (*cancel) return;
        IoQueue::post(this, [this, b, ok, readable] {
            if (!m_running || *m_cancel) return;
            m_done += b.end - b.start;
            emit blockChecked(b.start, b.end, ok, readable);
            emit progress(m_done, m_total);
            if (!m_running) return;   // stopped by a slot
            if (m_pauseMs > 0) QTimer::singleShot(m_pauseMs, this, [this] { next(); });
            else next();
        });
    });
}

void VerifyJob::checkWhole(const FileHashes &expect, qint64 tthUnit)
{
    m_running = true;
    m_done = 0;
    m_total = QFile(m_path).size();
    const QString path = m_path;
    auto cancel = m_cancel;
    m_io.run([this, path, cancel, expect, tthUnit] {
        Whole w;
        hash::MultiHasher h(hash::MultiHasher::Default | hash::MultiHasher::Md5);
        // progress is posted at most every ~8 MiB by hashFile
        auto prog = [this](qint64 n) {
            IoQueue::post(this, [this, n] {
                if (!m_running) return;
                m_done = n;
                emit progress(m_done, m_total);
            });
        };
        w.readable = hash::MultiHasher::hashFile(path, h, cancel.get(), prog, &w.error);
        if (*cancel) return;
        if (w.readable) {
            w.hashes = h.hashes();
            w.ed2kParts = h.ed2kPartHashes();
            if (tthUnit > 0) w.tthLevel = h.tthLevel(tthUnit);
            auto cmp = [&](const QByteArray &want, const QByteArray &got, const char *kind) {
                if (!want.isEmpty() && !got.isEmpty() && want != got) w.mismatch << QLatin1String(kind);
            };
            cmp(expect.ed2k, w.hashes.ed2k, "ed2k");
            cmp(expect.sha1, w.hashes.sha1, "sha1");
            cmp(expect.sha256, w.hashes.sha256, "sha256");
            cmp(expect.md5, w.hashes.md5, "md5");
            cmp(expect.tth, w.hashes.tth, "tth");
            cmp(expect.aich, w.hashes.aich, "aich");
        }
        IoQueue::post(this, [this, w] {
            if (!m_running) return;
            m_running = false;
            m_done = m_total;
            emit wholeChecked(w);
            emit finished(false);
        });
    });
}

void VerifyJob::stop()
{
    if (!m_running) return;
    m_running = false;
    *m_cancel = true;
    m_io.discardPending();
}

} // namespace mx::engine
