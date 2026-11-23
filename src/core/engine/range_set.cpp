// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/engine/range_set.h"

namespace mx::engine {

RangeSet RangeSet::fromList(const QList<Range> &l)
{
    RangeSet s;
    for (const Range &r : l) s.add(r.first, r.second);
    return s;
}

void RangeSet::add(qint64 start, qint64 end)
{
    if (start < 0) start = 0;
    if (end <= start) return;
    // find the first range that could touch [start,end): the one before start (if it reaches start)
    auto it = m_map.upperBound(start);
    if (it != m_map.begin()) {
        auto prev = std::prev(it);
        if (prev.value() >= start) {
            start = prev.key();
            end = qMax(end, prev.value());
            it = m_map.erase(prev);
        }
    }
    while (it != m_map.end() && it.key() <= end) {
        end = qMax(end, it.value());
        it = m_map.erase(it);
    }
    m_map.insert(start, end);
}

void RangeSet::add(const RangeSet &o)
{
    for (auto it = o.m_map.constBegin(); it != o.m_map.constEnd(); ++it) add(it.key(), it.value());
}

void RangeSet::remove(qint64 start, qint64 end)
{
    if (end <= start || m_map.isEmpty()) return;
    auto it = m_map.upperBound(start);
    if (it != m_map.begin()) {
        auto prev = std::prev(it);
        if (prev.value() > start) {
            const qint64 ps = prev.key(), pe = prev.value();
            m_map.erase(prev);
            if (ps < start) m_map.insert(ps, start);
            if (pe > end) m_map.insert(end, pe);
            it = m_map.upperBound(start);
        }
    }
    while (it != m_map.end() && it.key() < end) {
        const qint64 e = it.value();
        it = m_map.erase(it);
        if (e > end) {
            m_map.insert(end, e);
            break;
        }
    }
}

void RangeSet::remove(const RangeSet &o)
{
    for (auto it = o.m_map.constBegin(); it != o.m_map.constEnd(); ++it) remove(it.key(), it.value());
}

bool RangeSet::contains(qint64 start, qint64 end) const
{
    if (end <= start) return true;
    auto it = m_map.upperBound(start);
    if (it == m_map.begin()) return false;
    --it;
    return it.key() <= start && it.value() >= end;
}

bool RangeSet::intersects(qint64 start, qint64 end) const
{
    if (end <= start) return false;
    auto it = m_map.upperBound(start);
    if (it != m_map.begin() && std::prev(it).value() > start) return true;
    return it != m_map.end() && it.key() < end;
}

qint64 RangeSet::coveredIn(qint64 start, qint64 end) const
{
    qint64 n = 0;
    if (end <= start) return 0;
    auto it = m_map.upperBound(start);
    if (it != m_map.begin()) --it;
    for (; it != m_map.end() && it.key() < end; ++it) {
        const qint64 s = qMax(start, it.key()), e = qMin(end, it.value());
        if (e > s) n += e - s;
    }
    return n;
}

RangeSet RangeSet::intersected(qint64 start, qint64 end) const
{
    RangeSet out;
    if (end <= start) return out;
    auto it = m_map.upperBound(start);
    if (it != m_map.begin()) --it;
    for (; it != m_map.end() && it.key() < end; ++it) {
        const qint64 s = qMax(start, it.key()), e = qMin(end, it.value());
        if (e > s) out.m_map.insert(s, e);
    }
    return out;
}

RangeSet RangeSet::intersected(const RangeSet &o) const
{
    RangeSet out;
    for (auto it = o.m_map.constBegin(); it != o.m_map.constEnd(); ++it) out.add(intersected(it.key(), it.value()));
    return out;
}

RangeSet RangeSet::complement(qint64 start, qint64 end) const
{
    RangeSet out;
    if (end <= start) return out;
    qint64 pos = start;
    auto it = m_map.upperBound(start);
    if (it != m_map.begin()) --it;
    for (; it != m_map.end() && it.key() < end; ++it) {
        if (it.value() <= pos) continue;
        if (it.key() > pos) out.m_map.insert(pos, it.key());
        pos = qMax(pos, it.value());
    }
    if (pos < end) out.m_map.insert(pos, end);
    return out;
}

qint64 RangeSet::total() const
{
    qint64 n = 0;
    for (auto it = m_map.constBegin(); it != m_map.constEnd(); ++it) n += it.value() - it.key();
    return n;
}

qint64 RangeSet::runEnd(qint64 pos) const
{
    auto it = m_map.upperBound(pos);
    if (it == m_map.begin()) return pos;
    --it;
    return it.value() > pos ? it.value() : pos;
}

QList<Range> RangeSet::ranges() const
{
    QList<Range> out;
    out.reserve(m_map.size());
    for (auto it = m_map.constBegin(); it != m_map.constEnd(); ++it) out << Range(it.key(), it.value());
    return out;
}

QVariantList RangeSet::toVariant() const
{
    QVariantList l;
    for (auto it = m_map.constBegin(); it != m_map.constEnd(); ++it)
        l << QVariant(QVariantList{it.key(), it.value()});
    return l;
}

RangeSet RangeSet::fromVariant(const QVariantList &v, qint64 clampEnd)
{
    RangeSet s;
    for (const QVariant &e : v) {
        const QVariantList p = e.toList();
        if (p.size() != 2) continue;
        qint64 a = p.at(0).toLongLong(), b = p.at(1).toLongLong();
        if (clampEnd >= 0) b = qMin(b, clampEnd);
        s.add(a, b);
    }
    return s;
}

} // namespace mx::engine
