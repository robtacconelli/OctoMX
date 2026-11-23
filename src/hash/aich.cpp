// SPDX-License-Identifier: GPL-3.0-or-later
#include "mxhash/aich.h"

namespace mx::hash {

void AichHasher::reset()
{
    m_sha.reset();
    m_total = 0;
    m_inPart = 0;
    m_inBlock = 0;
    m_blocks.clear();
    m_partHashes.clear();
}

void AichHasher::closeBlock()
{
    m_blocks.last().append(m_sha.result());
    m_sha.reset();
    m_inBlock = 0;
}

void AichHasher::update(const char *data, qint64 len)
{
    while (len > 0) {
        if (m_inPart == 0 && m_inBlock == 0)
            m_blocks.append(QList<QByteArray>());
        const qint64 blockStart = m_inPart - m_inBlock;
        const qint64 limit = qMin(BlockSize, PartSize - blockStart);
        const qint64 take = qMin(len, limit - m_inBlock);
        m_sha.addData(QByteArrayView(data, take));
        m_inBlock += take;
        m_inPart += take;
        m_total += take;
        data += take;
        len -= take;
        if (m_inBlock == limit) {
            closeBlock();
            if (m_inPart == PartSize)
                m_inPart = 0;
        }
    }
}

QByteArray AichHasher::finish()
{
    if (m_inBlock > 0)
        closeBlock();
    if (m_total == 0) {
        m_blocks = {{QCryptographicHash::hash(QByteArray(), QCryptographicHash::Sha1)}};
    }
    m_partHashes.clear();
    const QByteArray root = rootFromBlocks(m_blocks, m_total, &m_partHashes);
    m_inPart = 0;
    return root;
}

QList<QByteArray> AichHasher::blockHashes() const
{
    QList<QByteArray> out;
    for (const auto &p : m_blocks)
        out += p;
    return out;
}

namespace {
struct TreeBuilder {
    const QList<QList<QByteArray>> &blocks;
    QList<QByteArray> *parts;
    bool ok = true;

    QByteArray leaf(qint64 offset)
    {
        const qint64 part = offset / AichHasher::PartSize;
        const qint64 blk = (offset % AichHasher::PartSize) / AichHasher::BlockSize;
        if (part >= blocks.size() || blk >= blocks.at(int(part)).size()) {
            ok = false;
            return QByteArray(20, '\0');
        }
        return blocks.at(int(part)).at(int(blk));
    }

    QByteArray node(qint64 offset, qint64 size, bool isLeft, bool partLevel)
    {
        const qint64 base = size > AichHasher::PartSize ? AichHasher::PartSize : AichHasher::BlockSize;
        QByteArray h;
        if (size <= base) {
            h = leaf(offset);
        } else {
            const qint64 n = (size + base - 1) / base;
            const qint64 left = ((isLeft ? n + 1 : n) / 2) * base;
            const bool childPartLevel = (base == AichHasher::PartSize);
            const QByteArray l = node(offset, left, true, childPartLevel);
            const QByteArray r = node(offset + left, size - left, false, childPartLevel);
            QCryptographicHash sha(QCryptographicHash::Sha1);
            sha.addData(l);
            sha.addData(r);
            h = sha.result();
        }
        if (partLevel && parts && size <= AichHasher::PartSize)
            parts->append(h);
        return h;
    }
};
} // namespace

QByteArray AichHasher::rootFromBlocks(const QList<QList<QByteArray>> &blocksPerPart, qint64 fileSize,
                                      QList<QByteArray> *partHashesOut)
{
    if (fileSize <= 0)
        fileSize = 0;
    TreeBuilder b{blocksPerPart, partHashesOut};
    // The root is a part-level node exactly when the whole file fits in one part.
    const QByteArray root = b.node(0, qMax<qint64>(fileSize, 1), true, true);
    if (fileSize == 0 && partHashesOut) {
        partHashesOut->clear();
        partHashesOut->append(root);
    }
    return b.ok ? root : QByteArray();
}

} // namespace mx::hash
