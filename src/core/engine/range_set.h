// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX engine: a set of half-open byte ranges [start, end), kept merged and sorted.
// Used for range maps (have / verified / in-flight), per-source contribution tracking and
// availability. Not thread-safe (copy it to hand it to another thread).
#pragma once

#include <QtCore/QList>
#include <QtCore/QMap>
#include <QtCore/QPair>
#include <QtCore/QVariantList>

namespace mx::engine {

using Range = QPair<qint64, qint64>;   // [first, second)

class RangeSet {
public:
    RangeSet() = default;
    RangeSet(qint64 start, qint64 end) { add(start, end); }
    static RangeSet fromList(const QList<Range> &l);

    void add(qint64 start, qint64 end);
    void add(const RangeSet &o);
    void remove(qint64 start, qint64 end);
    void remove(const RangeSet &o);
    void clear() { m_map.clear(); }

    bool isEmpty() const { return m_map.isEmpty(); }
    bool contains(qint64 start, qint64 end) const;     // [start,end) fully covered (empty range → true)
    bool contains(qint64 pos) const { return contains(pos, pos + 1); }
    bool intersects(qint64 start, qint64 end) const;
    qint64 coveredIn(qint64 start, qint64 end) const;  // bytes of [start,end) covered
    RangeSet intersected(qint64 start, qint64 end) const;
    RangeSet intersected(const RangeSet &o) const;
    RangeSet complement(qint64 start, qint64 end) const;   // [start,end) minus this
    qint64 total() const;
    int count() const { return int(m_map.size()); }
    qint64 first() const { return m_map.isEmpty() ? -1 : m_map.firstKey(); }
    qint64 last() const { return m_map.isEmpty() ? -1 : m_map.last(); }   // end of the last range
    // End of the contiguous run that starts at `pos` (pos itself if pos is not covered).
    qint64 runEnd(qint64 pos) const;
    QList<Range> ranges() const;

    QVariantList toVariant() const;                   // [[s,e],...] (CBOR/JSON friendly)
    static RangeSet fromVariant(const QVariantList &v, qint64 clampEnd = -1);

    bool operator==(const RangeSet &o) const { return m_map == o.m_map; }
    bool operator!=(const RangeSet &o) const { return !(*this == o); }

private:
    QMap<qint64, qint64> m_map;   // start -> end, non-overlapping, non-adjacent
};

} // namespace mx::engine
