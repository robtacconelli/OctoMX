// SPDX-License-Identifier: GPL-3.0-or-later
#include "mxhash/tiger_tree.h"

namespace mx::hash {

namespace {
const char kLeafPrefix = '\x00';
const char kNodePrefix = '\x01';
} // namespace

TigerTree::TigerTree(int maxStoredNodes)
    : m_maxStored(qMax(2, maxStoredNodes & ~1))
{
    reset();
}

void TigerTree::reset()
{
    m_leaf.reset();
    m_leaf.update(&kLeafPrefix, 1);
    m_inLeaf = 0;
    m_leafCount = 0;
    m_stack.clear();
    m_stackUsed.clear();
    m_stored.clear();
    m_storedHeight = 0;
    m_root.clear();
    m_finished = false;
}

void TigerTree::update(const char *data, qint64 len)
{
    while (len > 0) {
        const int take = int(qMin<qint64>(len, LeafSize - m_inLeaf));
        m_leaf.update(data, take);
        m_inLeaf += take;
        data += take;
        len -= take;
        if (m_inLeaf == LeafSize) {
            addLeaf(m_leaf.finish());
            m_leaf.reset();
            m_leaf.update(&kLeafPrefix, 1);
            m_inLeaf = 0;
        }
    }
}

void TigerTree::addLeaf(const QByteArray &h)
{
    ++m_leafCount;
    pushAtHeight(h, 0);
}

void TigerTree::pushAtHeight(QByteArray h, int height)
{
    while (height < m_storedHeight) {
        while (m_stack.size() <= height) {
            m_stack.append(QByteArray());
            m_stackUsed.append(false);
        }
        if (!m_stackUsed.at(height)) {
            m_stack[height] = h;
            m_stackUsed[height] = true;
            return;
        }
        h = nodeHash(m_stack.at(height), h);
        m_stack[height].clear();
        m_stackUsed[height] = false;
        ++height;
    }
    m_stored.append(h);
    if (m_stored.size() >= m_maxStored && (m_stored.size() % 2) == 0) {
        m_stored = foldLevel(m_stored);
        ++m_storedHeight;
    }
}

QByteArray TigerTree::finish()
{
    if (m_inLeaf > 0 || m_leafCount == 0)
        addLeaf(m_leaf.finish());
    m_leaf.reset();
    m_inLeaf = 0;
    // Remaining complete subtrees below the stored height form the (incomplete) last stored node.
    QByteArray cur;
    for (int j = 0; j < m_stack.size(); ++j) {
        if (!m_stackUsed.at(j)) continue;
        cur = cur.isEmpty() ? m_stack.at(j) : nodeHash(m_stack.at(j), cur);
        m_stackUsed[j] = false;
    }
    m_stack.clear();
    m_stackUsed.clear();
    if (!cur.isEmpty())
        m_stored.append(cur);
    m_root = rootFromLevel(m_stored);
    m_finished = true;
    return m_root;
}

int TigerTree::levels() const
{
    int depth = 0;
    while ((qint64(1) << depth) < m_leafCount)
        ++depth;
    return depth + 1;
}

QList<QByteArray> TigerTree::level(int depthFromRoot) const
{
    if (!m_finished) return {};
    const int depth = levels() - 1;
    const int height = depth - depthFromRoot;
    if (depthFromRoot < 0 || height < m_storedHeight) return {};
    QList<QByteArray> l = m_stored;
    for (int h = m_storedHeight; h < height; ++h)
        l = foldLevel(l);
    return l;
}

QList<QByteArray> TigerTree::levelForUnit(qint64 unitBytes) const
{
    if (unitBytes < LeafSize || (unitBytes % LeafSize) != 0) return {};
    const qint64 leaves = unitBytes / LeafSize;
    if ((leaves & (leaves - 1)) != 0) return {};   // not a power of two
    int k = 0;
    while ((qint64(1) << k) < leaves)
        ++k;
    const int depth = levels() - 1;
    return level(qMax(0, depth - k));
}

QByteArray TigerTree::leafHash(const char *data, qint64 len)
{
    Tiger t;
    t.update(&kLeafPrefix, 1);
    t.update(data, len);
    return t.finish();
}

QByteArray TigerTree::leafHash(const QByteArray &data)
{
    return leafHash(data.constData(), data.size());
}

QByteArray TigerTree::nodeHash(const QByteArray &a, const QByteArray &b)
{
    Tiger t;
    t.update(&kNodePrefix, 1);
    t.update(a);
    t.update(b);
    return t.finish();
}

QList<QByteArray> TigerTree::foldLevel(const QList<QByteArray> &nodes)
{
    QList<QByteArray> up;
    up.reserve((nodes.size() + 1) / 2);
    for (int i = 0; i + 1 < nodes.size(); i += 2)
        up.append(nodeHash(nodes.at(i), nodes.at(i + 1)));
    if (nodes.size() % 2)
        up.append(nodes.last());
    return up;
}

QByteArray TigerTree::rootFromLevel(QList<QByteArray> nodes)
{
    if (nodes.isEmpty()) return leafHash(QByteArray());
    while (nodes.size() > 1)
        nodes = foldLevel(nodes);
    return nodes.first();
}

} // namespace mx::hash
