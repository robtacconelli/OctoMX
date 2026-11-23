// SPDX-License-Identifier: GPL-3.0-or-later
// Tiger Tree Hash (THEX, as used by Gnutella urn:tree:tiger / DC++ TTH).
//  * leaves: Tiger(0x00 || up to 1024 data bytes); an empty input has one leaf Tiger(0x00);
//  * inner nodes: Tiger(0x01 || left || right); a node without a right sibling is promoted
//    unchanged to the next level.
// Memory: the hasher keeps one level of the tree (initially the leaves). When that level would
// exceed `maxStoredNodes` it is folded one level up, so for huge files the finest available level
// covers 2^k KiB per node. level()/levelForUnit() return an empty list for levels finer than that.
#pragma once

#include "mxhash/hasher.h"

#include <QtCore/QList>

namespace mx::hash {

class TigerTree : public Hasher {
public:
    static constexpr int LeafSize = 1024;
    explicit TigerTree(int maxStoredNodes = 65536);
    void update(const char *data, qint64 len) override;
    using Hasher::update;
    QByteArray finish() override;                 // 24-byte root
    void reset() override;

    // After finish(). Level 0 = root (1 hash), level 1 = up to 2 hashes, ...; levels() = depth + 1.
    int levels() const;
    QList<QByteArray> level(int depthFromRoot) const;
    // The level whose nodes each cover `unitBytes` (1024 * 2^k) of data; empty if not available.
    QList<QByteArray> levelForUnit(qint64 unitBytes) const;
    // Height (0 = leaves) of the finest level kept in memory, i.e. its unit is 1024 << height.
    int storedHeight() const { return m_storedHeight; }
    qint64 leafCount() const { return m_leafCount; }

    static QByteArray leafHash(const QByteArray &data);                     // Tiger(0x00 + data)
    static QByteArray leafHash(const char *data, qint64 len);
    static QByteArray nodeHash(const QByteArray &a, const QByteArray &b);   // Tiger(0x01 + a + b)
    static QByteArray rootFromLevel(QList<QByteArray> nodes);               // fold a level to the root
    static QList<QByteArray> foldLevel(const QList<QByteArray> &nodes);     // one level up
private:
    void addLeaf(const QByteArray &h);
    void pushAtHeight(QByteArray h, int height);
    int m_maxStored;
    Tiger m_leaf;              // current leaf, already fed with the 0x00 prefix
    int m_inLeaf = 0;
    qint64 m_leafCount = 0;
    // binary-counter stack of complete subtrees below m_storedHeight: m_stack[j] is a node of height j
    QList<QByteArray> m_stack;
    QList<bool> m_stackUsed;
    QList<QByteArray> m_stored;   // nodes of height m_storedHeight, in order
    int m_storedHeight = 0;
    QByteArray m_root;
    bool m_finished = false;
};

} // namespace mx::hash
