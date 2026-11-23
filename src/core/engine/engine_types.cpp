// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/engine/engine_types.h"

#include <QtCore/QElapsedTimer>

namespace mx::engine {

qint64 nowMs()
{
    static QElapsedTimer clock = [] { QElapsedTimer t; t.start(); return t; }();
    return clock.elapsed() + 1;   // never 0 (0 means "unset" in the bookkeeping)
}

void RateMeter::add(qint64 now, qint64 bytes)
{
    const qint64 sec = now / 1000;
    const int i = int(sec % N);
    if (m_sec[i] != sec) {
        m_sec[i] = sec;
        m_bytes[i] = 0;
    }
    m_bytes[i] += bytes;
    if (bytes > 0) m_lastMs = now;
}

int RateMeter::rate(qint64 now, int windowSec) const
{
    windowSec = qBound(1, windowSec, N - 2);
    const qint64 sec = now / 1000;
    qint64 sum = 0;
    // the current second is partial: count the previous `windowSec` complete seconds + the current
    for (int k = 0; k <= windowSec; ++k) {
        const qint64 s = sec - k;
        const int i = int(((s % N) + N) % N);
        if (m_sec[i] == s) sum += m_bytes[i];
    }
    const double span = windowSec + double(now % 1000) / 1000.0;
    return int(qMin<double>(sum / qMax(0.5, span), 2e9));
}

bool SourceSlot::sequentialOnly() const
{
    const QVariant v = desc.extra.value(QStringLiteral("supportsRanges"));
    if (rs) {
        const QVariant live = rs->descriptor().extra.value(QStringLiteral("supportsRanges"));
        if (live.isValid()) return !live.toBool();
    }
    return v.isValid() && !v.toBool();
}

RangeSet SourceSlot::inflight() const
{
    RangeSet r;
    for (const Request &q : reqs) r.add(q.off + q.got, q.end());
    return r;
}

QString sourceKey(const SearchSource &s)
{
    const QString net = QString::number(int(s.net));
    if (!s.user.id.isEmpty()) return net + QLatin1Char('|') + s.user.id;
    if (!s.user.address.isEmpty()) return net + QLatin1Char('|') + s.user.address;
    return net + QLatin1Char('|') + s.uri;
}

} // namespace mx::engine
