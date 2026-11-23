// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/bandwidth.h"
#include "core/dao.h"
#include "core/database.h"
#include "core/rate_limiter.h"

#include <QtCore/QDateTime>

namespace mx {

namespace {
const QString kDown = QStringLiteral("allTimeDownloaded");
const QString kUp = QStringLiteral("allTimeUploaded");
constexpr int kMaxHistory = 3600;
}

CoreBandwidth::CoreBandwidth(CoreRateLimiter *limiter, Database *db, QObject *parent)
    : BandwidthService(parent)
    , m_limiter(limiter)
    , m_db(db)
{
    m_timer.setInterval(1000);
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &CoreBandwidth::tickNow);
}

void CoreBandwidth::start()
{
    if (m_db && m_db->isOpen()) {
        StatsDao stats(m_db);
        m_baseDown = stats.get(kDown);
        m_baseUp = stats.get(kUp);
    }
    m_startDown = m_lastDown = m_limiter->total(RateLimiter::Down);
    m_startUp = m_lastUp = m_limiter->total(RateLimiter::Up);
    m_lastPayDown = m_limiter->payloadTotal(RateLimiter::Down);
    m_lastPayUp = m_limiter->payloadTotal(RateLimiter::Up);
    m_lastNet.clear();
    for (int d = 0; d < 2; ++d)
        for (int i = 0; i < CoreRateLimiter::kNets; ++i)
            m_lastNet.append(m_limiter->total(d ? RateLimiter::Up : RateLimiter::Down, Net(i)));
    m_lastTickMs = QDateTime::currentMSecsSinceEpoch();
    m_timer.start();
}

void CoreBandwidth::shutdown()
{
    m_timer.stop();
    persist();
}

void CoreBandwidth::persist()
{
    if (!m_db || !m_db->isOpen()) return;
    StatsDao stats(m_db);
    stats.set(kDown, allTimeDownloaded());
    stats.set(kUp, allTimeUploaded());
}

void CoreBandwidth::tickNow()
{
    m_limiter->tick();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 down = m_limiter->total(RateLimiter::Down);
    const qint64 up = m_limiter->total(RateLimiter::Up);
    const qint64 payDown = m_limiter->payloadTotal(RateLimiter::Down);
    const qint64 payUp = m_limiter->payloadTotal(RateLimiter::Up);
    const double secs = qMax<qint64>(1, now - m_lastTickMs) / 1000.0;
    auto rate = [secs](qint64 bytes) { return int(qBound<double>(0, bytes / secs, INT_MAX)); };
    BandwidthSample s;
    s.timeMs = now;
    s.downBps = rate(down - m_lastDown);
    s.upBps = rate(up - m_lastUp);
    s.payloadDownBps = qMin(s.downBps, rate(payDown - m_lastPayDown));
    s.payloadUpBps = qMin(s.upBps, rate(payUp - m_lastPayUp));
    const int nets = CoreRateLimiter::kNets;
    if (m_lastNet.size() != 2 * nets) m_lastNet = QList<qint64>(2 * nets, 0);
    for (int d = 0; d < 2; ++d) {
        for (int i = 0; i < nets; ++i) {
            const qint64 t = m_limiter->total(d ? RateLimiter::Up : RateLimiter::Down, Net(i));
            const qint64 delta = t - m_lastNet[d * nets + i];
            m_lastNet[d * nets + i] = t;
            if (delta > 0) (d ? s.netUpBps : s.netDownBps).insert(Net(i), rate(delta));
        }
    }
    m_lastDown = down;
    m_lastUp = up;
    m_lastPayDown = payDown;
    m_lastPayUp = payUp;
    m_lastTickMs = now;
    m_history.append(s);
    if (m_history.size() > kMaxHistory) m_history.remove(0, m_history.size() - kMaxHistory);
    if (++m_ticks % 60 == 0) persist();
    emit sample(s);
}

QList<BandwidthSample> CoreBandwidth::history(int seconds) const
{
    if (seconds <= 0 || seconds >= m_history.size()) return m_history;
    return m_history.mid(m_history.size() - seconds);
}

BandwidthSample CoreBandwidth::current() const
{
    if (!m_history.isEmpty()) return m_history.last();
    BandwidthSample s;
    s.timeMs = QDateTime::currentMSecsSinceEpoch();
    return s;
}

qint64 CoreBandwidth::totalDownloaded() const { return m_limiter->total(RateLimiter::Down) - m_startDown; }
qint64 CoreBandwidth::totalUploaded() const { return m_limiter->total(RateLimiter::Up) - m_startUp; }
qint64 CoreBandwidth::allTimeDownloaded() const { return m_baseDown + totalDownloaded(); }
qint64 CoreBandwidth::allTimeUploaded() const { return m_baseUp + totalUploaded(); }

} // namespace mx
