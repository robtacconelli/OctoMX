// SPDX-License-Identifier: GPL-3.0-or-later
// eMule AICH (Advanced Intelligent Corruption Handling) root hash: a SHA-1 hash tree.
//
// Own implementation (a stateless recursive builder) of the AICH tree layout that eMule defines
// (CAICHHashTree, SHAHashSet.cpp, 0.50a); only the interop-mandated layout below is shared:
//  * leaves are SHA-1s of 184 320-byte blocks (EMBLOCKSIZE); blocks never cross a 9 728 000-byte
//    part boundary, so the last block of every part is 143 360 bytes (or the file remainder);
//  * a tree node covering `size` bytes has a "base size": PARTSIZE if size > PARTSIZE, else
//    EMBLOCKSIZE. A node with size <= base is a leaf; otherwise with n = ceil(size/base) units the
//    left child gets ((isLeftBranch ? n + 1 : n) / 2) * base bytes, the right child the rest; the
//    left child is a left branch, the right child a right branch; the root is a left branch;
//  * inner node hash = SHA-1(left hash || right hash).
// Consequences: a file of one block has root = SHA-1(data); files <= one part are a tree over
// blocks; larger files are a tree over parts whose leaves are per-part block trees.
// eMule does not hash 0-byte files; here an empty input yields SHA-1("") (a single empty leaf).
#pragma once

#include "mxhash/hasher.h"

#include <QtCore/QList>

namespace mx::hash {

class AichHasher : public Hasher {
public:
    static constexpr qint64 BlockSize = 184320;
    static constexpr qint64 PartSize = 9728000;
    AichHasher() : m_sha(QCryptographicHash::Sha1) { reset(); }
    void update(const char *data, qint64 len) override;
    using Hasher::update;
    QByteArray finish() override;                 // 20-byte AICH root
    void reset() override;
    // After finish(): the AICH hash of each 9 728 000-byte part (tree nodes at part level; for a
    // file of at most one part this is the root). Note the eMule quirk: the left/right branch flag
    // is inherited by a part's block subtree, so a part that is a right branch is NOT equal to the
    // AICH root of the same bytes hashed standalone (its 53 blocks split 26|27 instead of 27|26).
    QList<QByteArray> partHashes() const { return m_partHashes; }
    // After finish(): the block (leaf) hashes, all parts concatenated in order.
    QList<QByteArray> blockHashes() const;
    // Builds the root from per-part block hashes (exposed for tests and for verifying hash sets).
    static QByteArray rootFromBlocks(const QList<QList<QByteArray>> &blocksPerPart, qint64 fileSize,
                                     QList<QByteArray> *partHashesOut = nullptr);
private:
    void closeBlock();
    QCryptographicHash m_sha;
    qint64 m_total = 0;
    qint64 m_inPart = 0;     // bytes into the current part
    qint64 m_inBlock = 0;    // bytes into the current block
    QList<QList<QByteArray>> m_blocks;   // per part
    QList<QByteArray> m_partHashes;
};

} // namespace mx::hash
