// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/rate_limiter.h"

#include "mx/settings_keys.h"

#include <QtNetwork/QHostAddress>

namespace mx {

CoreRateLimiter::CoreRateLimiter(Settings *settings, QObject *parent)
    : QObject(parent)
    , m_settings(settings)
{
    m_clock.start();
    for (auto &a : m_totals) a = 0;
    for (auto &a : m_payload) a = 0;
    for (auto &a : m_lastSecond) a = 0;
    for (auto &a : m_exempt) a = 0;
    m_lastTick.fill(0);
    if (m_settings) {
        bwlimit::installOverrideRule(m_settings);   // normal limits changed during a schedule window win
        connect(m_settings, &Settings::changed, this, [this](const QString &key, const QVariant &) {
            if (key.startsWith(QLatin1String("bandwidth/"))) reloadFromSettings();
        });
        m_scheduleTimer.setSingleShot(true);
        connect(&m_scheduleTimer, &QTimer::timeout, this, &CoreRateLimiter::reloadFromSettings);
        reloadFromSettings();
    }
}

bool CoreRateLimiter::scheduleActive(const QTime &now, int dayOfWeek, const QString &start, const QString &end, int daysMask)
{
    return bwlimit::windowActive(now, dayOfWeek, start, end, daysMask);
}

void CoreRateLimiter::reloadFromSettings()
{
    if (!m_settings) return;
    const QDateTime now = QDateTime::currentDateTime();
    const bwlimit::InForce f = bwlimit::inForce(m_settings, now);
    m_limitLan = m_settings->b(keys::bandwidth::LimitLan);
    {
        QMutexLocker l(&m_mutex);
        m_inForce = f;
    }
    setLimits(int(qMin<qint64>(qint64(f.downKBs) * 1024, INT_MAX)), int(qMin<qint64>(qint64(f.upKBs) * 1024, INT_MAX)));
    scheduleNextReload(f);
}

void CoreRateLimiter::scheduleNextReload(const bwlimit::InForce &f)
{
    // at the next schedule boundary (window start / end, end of an override), at least every 30 s
    // (clock changes, sleep / resume)
    qint64 ms = 30 * 1000;
    if (f.scheduleEnabled && f.until.isValid())
        ms = qBound<qint64>(200, QDateTime::currentDateTime().msecsTo(f.until) + 200, ms);
    m_scheduleTimer.start(int(ms));
}

bwlimit::InForce CoreRateLimiter::inForce() const
{
    QMutexLocker l(&m_mutex);
    return m_inForce;
}

void CoreRateLimiter::setLimits(int downBps, int upBps)
{
    bool changed = false;
    {
        QMutexLocker l(&m_mutex);
        const qint64 rates[2] = {qMax(0, downBps), qMax(0, upBps)};
        const qint64 now = m_clock.nsecsElapsed();
        for (int i = 0; i < 2; ++i) {
            Bucket &b = m_buckets[i];
            if (b.rate != rates[i]) {
                b.rate = rates[i];
                b.tokens = qBound(0.0, b.tokens, double(rates[i]));   // a debt of the old limit is forgiven
                b.lastNs = now;
                b.granted = 0;
                b.btBudget = int(rates[i]);
                b.btAllow = 0;
                b.btAllowNs = now;
                changed = true;
            }
        }
    }
    if (changed) emit limitsChanged(downBps, upBps);
}

void CoreRateLimiter::refill(Bucket &b, qint64 nowNs)
{
    b.tokens = qMin(b.capacity(), b.tokens + double(b.rate) * double(nowNs - b.lastNs) / 1e9);
    b.lastNs = nowNs;
}

qint64 CoreRateLimiter::acquire(Dir d, qint64 bytes)
{
    if (bytes <= 0) return 0;
    QMutexLocker l(&m_mutex);
    Bucket &b = m_buckets[d == Up ? 1 : 0];
    if (b.rate <= 0) return bytes;
    const qint64 nowNs = m_clock.nsecsElapsed();
    refill(b, nowNs);
    qint64 n = qBound<qint64>(0, qint64(b.tokens), bytes);
    if (n == 0 && nowNs - b.trickleNs >= qint64(kTrickleMs) * 1000000 && b.tokens > -2 * b.capacity()) {
        // [eng-1] the bucket is empty: a little anyway (protocol messages must not starve), as debt
        n = qMin<qint64>(bytes, kTrickleBytes);
        b.trickleNs = nowNs;
    }
    if (n < bytes) b.denied += bytes - n;
    b.tokens -= double(n);
    b.granted = qMin(2 * b.capacity(), b.granted + double(n));
    b.charged += n;
    return n;
}

void CoreRateLimiter::addTotals(Dir d, Net net, qint64 wireBytes, qint64 payloadBytes)
{
    const int i = (d == Up ? kNets : 0) + qBound(0, int(net), kNets - 1);
    if (wireBytes > 0) m_totals[i] += wireBytes;
    if (payloadBytes > 0) m_payload[i] += payloadBytes;
}

void CoreRateLimiter::account(Dir d, Net net, qint64 bytes)
{
    accountTraffic(d, net, bytes, 0);
}

void CoreRateLimiter::accountTraffic(Dir d, Net net, qint64 wireBytes, qint64 payloadBytes)
{
    addTotals(d, net, wireBytes, payloadBytes);
    if (wireBytes <= 0) return;
    QMutexLocker l(&m_mutex);
    Bucket &b = m_buckets[d == Up ? 1 : 0];
    if (b.rate <= 0) return;
    const qint64 now = m_clock.nsecsElapsed();
    double charge = 0;
    if (net == Net::BitTorrent) {
        // what libtorrent moved within the budget it was given (LAN peers in its local class and
        // DHT / tracker packets beyond that are not charged)
        // libtorrent's counters arrive in batches (a stats alert may carry two seconds): allow up to
        // three seconds of budget to be charged at once, at the budget's rate on average
        const double cap = qMax(4096.0, 3.0 * double(b.btBudget));
        b.btAllow = qMin(cap, b.btAllow + double(b.btBudget) * double(now - b.btAllowNs) / 1e9);
        b.btAllowNs = now;
        charge = qMin(double(wireBytes), b.btAllow);
        b.btAllow -= charge;
        b.btCharged += qint64(charge);
    } else {
        const double use = qMin(b.granted, double(wireBytes));
        b.granted -= use;
        charge = double(wireBytes) - use;
    }
    if (charge <= 0) return;
    refill(b, now);
    b.tokens = qMax(-2 * b.capacity(), b.tokens - charge);   // at most two seconds of debt
    b.charged += qint64(charge);
}

bool CoreRateLimiter::exempt(const QHostAddress &peer) const
{
    return !m_limitLan && !peer.isNull() && bwlimit::isLanAddress(peer);
}

void CoreRateLimiter::accountExempt(Dir d, Net net, qint64 wireBytes, qint64 payloadBytes)
{
    addTotals(d, net, wireBytes, payloadBytes);
    if (wireBytes > 0) m_exempt[d == Up ? 1 : 0] += wireBytes;
    // payload whose wire bytes are accounted elsewhere (the overlay: RatsNode meters the frames):
    // those wire bytes must not be charged either, so they get a grant
    const qint64 extra = payloadBytes - qMax<qint64>(0, wireBytes);
    if (extra <= 0) return;
    m_exempt[d == Up ? 1 : 0] += extra;
    QMutexLocker l(&m_mutex);
    Bucket &b = m_buckets[d == Up ? 1 : 0];
    if (b.rate > 0) b.granted = qMin(2 * b.capacity(), b.granted + double(extra));
}

qint64 CoreRateLimiter::payloadTotal(Dir d) const
{
    qint64 t = 0;
    const int base = d == Up ? kNets : 0;
    for (int i = 0; i < kNets; ++i) t += m_payload[base + i];
    return t;
}

qint64 CoreRateLimiter::payloadTotal(Dir d, Net net) const
{
    return m_payload[(d == Up ? kNets : 0) + qBound(0, int(net), kNets - 1)];
}

qint64 CoreRateLimiter::chargedTotal(Dir d) const
{
    QMutexLocker l(&m_mutex);
    return m_buckets[d == Up ? 1 : 0].charged;
}

qint64 CoreRateLimiter::exemptTotal(Dir d) const
{
    return m_exempt[d == Up ? 1 : 0];
}

int CoreRateLimiter::limitBps(Dir d) const
{
    QMutexLocker l(&m_mutex);
    return int(m_buckets[d == Up ? 1 : 0].rate);
}

int CoreRateLimiter::budgetBps(Dir d, Net net) const
{
    QMutexLocker l(&m_mutex);
    const Bucket &b = m_buckets[d == Up ? 1 : 0];
    if (b.rate <= 0 || net != Net::BitTorrent) return int(b.rate);
    return b.btBudget;   // set in tick()
}

qint64 CoreRateLimiter::total(Dir d) const
{
    qint64 t = 0;
    const int base = d == Up ? kNets : 0;
    for (int i = 0; i < kNets; ++i) t += m_totals[base + i];
    return t;
}

qint64 CoreRateLimiter::total(Dir d, Net net) const
{
    const int n = qBound(0, int(net), kNets - 1);
    return m_totals[(d == Up ? kNets : 0) + n];
}

void CoreRateLimiter::tick()
{
    for (int i = 0; i < 2 * kNets; ++i) {
        const qint64 now = m_totals[i];
        m_lastSecond[i] = now - m_lastTick[i];
        m_lastTick[i] = now;
    }
    QMutexLocker l(&m_mutex);
    for (Bucket &b : m_buckets) {
        const qint64 others = b.charged - b.btCharged;
        b.others1s = others - b.othersAtTick;
        b.othersAtTick = others;
        const qint64 denied1s = b.denied - b.deniedAtTick;
        b.deniedAtTick = b.denied;
        const qint64 limit = b.rate;
        if (limit <= 0) continue;
        // libtorrent's budget: what the others left while they were satisfied; while they were
        // refused tokens, give way a fifth of the limit per second, never below half of it (the others
        // then get the rest through the shared bucket, see accountTraffic())
        qint64 budget = limit - b.others1s;
        if (denied1s > 0) budget = qMin<qint64>(budget, qint64(b.btBudget) - limit / 5);
        b.btBudget = int(qBound<qint64>(limit / 2, budget, limit));
    }
}

} // namespace mx
