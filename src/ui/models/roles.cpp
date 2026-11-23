// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/models/roles.h"

namespace mx::ui {

QVariant makeSegments(const QList<Segment> &segs)
{
    QVariantList l;
    l.reserve(segs.size());
    for (const Segment &s : segs)
        l << QVariant::fromValue(s);
    return l;
}

QList<Segment> segmentsFrom(const QVariant &v)
{
    QList<Segment> out;
    const QVariantList l = v.toList();
    out.reserve(l.size());
    for (const QVariant &e : l)
        out << e.value<Segment>();
    return out;
}

} // namespace mx::ui
