// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/rate_limiter.h"

#include "mx/settings_keys.h"

#include <QtNetwork/QHostAddress>

#include <cmath>

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
                b.tokens = qBound(0.0, b.tokens, b.capacity());   // a debt of the old limit is forgiven
                b.lastNs = now;
                b.granted = 0;
                b.btBudget = double(rates[i]);
                b.btPeak = b.btBudget;
                b.btPeakNs = now;
                b.btReserved = 0;
                b.btActualSlice = 0;
                b.btActualRate = 0;
                b.btTrim = 1.0;
                b.btAllow = 0;
                b.btAllowNs = now;
                b.sliceNs = now;
                for (NetState &n : b.nets) {   // [bw-2] shares start again from the new limit
                    n.share = 0;
                    n.tokens = 0;
                    n.tokensNs = now;
                }
                changed = true;
            }
        }
    }
    if (changed) emit limitsChanged(downBps, upBps);
}

void CoreRateLimiter::refill(Bucket &b, qint64 nowNs)
{
    // [bw-2] libtorrent's bytes are reported up to 100 ms after they moved: what it is expected to
    // move is reserved as time goes by (at its measured rate, at most its budget), so the others do
    // not take the same tokens in between; its reports are set against the reservation
    const double dt = double(nowNs - b.lastNs) / 1e9;
    const double reserve = qMin(b.nets[int(Net::BitTorrent)].rate, b.btBudget) * dt;
    b.btReserved += reserve;
    b.tokens = qMin(b.capacity(), b.tokens + double(b.rate) * dt - reserve);
    b.lastNs = nowNs;
}

// [bw-2] Every kSliceMs: the rate of every network (granted + charged bytes, exponential average of
// about half a second), who wants more ("hungry"), and a max-min fair share of the limit for each.
void CoreRateLimiter::slice(Bucket &b, qint64 nowNs) const
{
    const qint64 dt = nowNs - b.sliceNs;
    if (b.rate <= 0 || dt < qint64(kSliceMs) * 1000000) return;
    b.sliceNs = nowNs;
    const double secs = double(dt) / 1e9;
    const double alpha = qMin(1.0, secs / 0.5);
    const double limit = double(b.rate);
    const int bt = int(Net::BitTorrent);
    constexpr double kInf = 1e18;
    // demands: a hungry network wants everything, a satisfied one what it uses plus a margin (so it
    // can grow, and so it notices when it gets less than it could use)
    double demand[kNets];
    bool hungry[kNets];
    int participants = 0, hungryCount = 0;
    for (int i = 0; i < kNets; ++i) {
        NetState &n = b.nets[i];
        n.rate += alpha * (double(n.slice) / secs - n.rate);
        n.slice = 0;
        hungry[i] = nowNs - n.wantNs < qint64(kHungryMs) * 1000000;
        if (i == bt) hungry[i] = hungry[i] || (n.rate >= 0.85 * b.btBudget && n.rate > 1024);
        n.active = hungry[i] || n.rate > 512;
        demand[i] = !n.active ? 0 : hungry[i] ? kInf : n.rate * 1.2 + qMax(1024.0, limit * 0.02);
        if (n.active) ++participants;
        if (hungry[i]) ++hungryCount;
    }
    // water filling: the smallest demands first, each at most an equal part of what is left
    double share[kNets] = {};
    double left = limit;
    int open = participants;
    bool done[kNets] = {};
    while (open > 0) {
        int pick = -1;
        for (int i = 0; i < kNets; ++i)
            if (b.nets[i].active && !done[i] && (pick < 0 || demand[i] < demand[pick])) pick = i;
        const double part = left / open;
        share[pick] = qMin(demand[pick], part);
        left -= share[pick];
        done[pick] = true;
        --open;
    }
    // what nobody claimed: to the hungry ones, or to all (they may grow into it)
    if (left > 0 && participants > 0)
        for (int i = 0; i < kNets; ++i)
            if (b.nets[i].active && (hungry[i] || hungryCount == 0)) share[i] += left / (hungryCount ? hungryCount : participants);
    for (int i = 0; i < kNets; ++i) b.nets[i].share = share[i];
    // libtorrent: halfway to its share per slice. While it moves nothing: what the others leave, but
    // at least a newcomer's share (so a torrent that starts gets going at once; what it moves is
    // charged, so the others back off) and never below a twentieth of the limit
    double target = share[bt];
    if (!b.nets[bt].active) {
        int others = 0;
        double claimed = 0;
        for (int i = 0; i < kNets; ++i)
            if (i != bt && b.nets[i].active) {
                ++others;
                claimed += qMin(demand[i], limit);
            }
        target = qMax(limit - claimed, limit / (others + 1));
    }
    target = qMax(target, qMax(limit * 0.05, 4096.0));
    // libtorrent moves a few percent more than its limit with a 100 ms tick (uploads: 103 to 104 %,
    // measured; 100 % with its default 500 ms tick): while it uses its whole budget and moves up to
    // 15 % more, it is handed a little less (at most 10 %), until what it moves matches its budget.
    // Far more than the budget is its unlimited LAN class or DHT: no correction then
    b.btActualRate += qMin(1.0, secs / 1.0) * (double(b.btActualSlice) / secs - b.btActualRate);
    b.btActualSlice = 0;
    if (hungry[bt] && b.btBudget > 0 && b.btActualRate < 1.15 * b.btBudget)
        b.btTrim = qBound(0.9, b.btTrim + 0.3 * secs * (1.0 - b.btActualRate / b.btBudget), 1.0);
    // a reservation libtorrent did not use (it moved less than its rate said) goes back after 300 ms
    const double keep = qMin(b.nets[bt].rate, b.btBudget) * 0.3 + 4096.0;
    if (b.btReserved > keep) {
        b.tokens = qMin(b.capacity(), b.tokens + (b.btReserved - keep));
        b.btReserved = keep;
    }
    const double step = 1.0 - std::pow(0.5, secs * 1000.0 / kSliceMs);   // halfway per 100 ms
    b.btBudget = qBound(qMin(limit, 4096.0), b.btBudget + step * (target - b.btBudget), limit);
    // libtorrent takes a new budget up a few hundred ms later (the plugin polls it every 200 ms,
    // libtorrent hands out quota every 100 ms): until then it may still move the higher one, and
    // that must be charged too, or the others would take the same bytes again
    if (b.btBudget >= b.btPeak || nowNs - b.btPeakNs > 500 * qint64(1000000)) {
        b.btPeak = b.btBudget;
        b.btPeakNs = nowNs;
    }
}

// [bw-2] the grant for one network: the shared bucket, and while another network is hungry, its own
// share bucket; the [eng-1] trickle while the shared bucket is empty
qint64 CoreRateLimiter::take(Bucket &b, int net, qint64 bytes, qint64 nowNs)
{
    refill(b, nowNs);
    slice(b, nowNs);
    NetState &me = b.nets[net];
    const double cap = qMax(me.share * kSliceMs / 1000.0, 3000.0);
    me.tokens = qMin(cap, me.tokens + me.share * double(nowNs - me.tokensNs) / 1e9);
    me.tokensNs = nowNs;
    bool othersHungry = false;
    for (int i = 0; i < kNets && !othersHungry; ++i) {
        if (i == net) continue;
        const NetState &o = b.nets[i];
        othersHungry = nowNs - o.wantNs < qint64(kHungryMs) * 1000000;
        if (i == int(Net::BitTorrent)) othersHungry = othersHungry || (o.rate >= 0.85 * b.btBudget && o.rate > 1024);
    }
    double avail = b.tokens;
    if (othersHungry && me.share > 0) avail = qMin(avail, me.tokens);
    qint64 n = qBound<qint64>(0, qint64(avail), bytes);
    if (n == 0 && nowNs - b.trickleNs >= qint64(kTrickleMs) * 1000000 && b.tokens > -b.maxDebt()) {
        // [eng-1] nothing to give: a little anyway (protocol messages must not starve), as debt
        n = qMin<qint64>(bytes, kTrickleBytes);
        b.trickleNs = nowNs;
    }
    if (n < bytes) {
        b.denied += bytes - n;
        me.wantNs = nowNs;
    }
    b.tokens -= double(n);
    me.tokens = qMax(-cap, me.tokens - double(n));
    me.slice += n;
    b.granted = qMin(b.maxDebt(), b.granted + double(n));
    b.charged += n;
    return n;
}

void CoreRateLimiter::reportWaiting(Dir d, Net net, qint64 bytes)
{
    QMutexLocker l(&m_mutex);
    Bucket &b = m_buckets[d == Up ? 1 : 0];
    if (b.rate <= 0) return;
    // more waiting than a fifth of a second of its budget (at least one request of two blocks): it
    // is held back by its budget, not by its peers
    if (double(bytes) > qMax(16384.0, b.btBudget * 0.2)) b.nets[qBound(0, int(net), kNets - 1)].wantNs = m_clock.nsecsElapsed();
}

qint64 CoreRateLimiter::acquire(Dir d, qint64 bytes)
{
    return acquireFor(d, Net::Unknown, bytes);
}

qint64 CoreRateLimiter::acquireFor(Dir d, Net net, qint64 bytes)
{
    if (bytes <= 0) return 0;
    QMutexLocker l(&m_mutex);
    Bucket &b = m_buckets[d == Up ? 1 : 0];
    if (b.rate <= 0) return bytes;
    return take(b, qBound(0, int(net), kNets - 1), bytes, m_clock.nsecsElapsed());
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
    // [bw-2] the limits are for file transfers: networks that only search and discover (Kad, the web
    // indexes, Rats) are counted for the graph, never charged (their plugins say so too)
    if (net == Net::Kad || net == Net::Web || net == Net::Rats) {
        accountExempt(d, net, wireBytes, payloadBytes);
        return;
    }
    addTotals(d, net, wireBytes, payloadBytes);
    if (wireBytes <= 0) return;
    QMutexLocker l(&m_mutex);
    Bucket &b = m_buckets[d == Up ? 1 : 0];
    if (b.rate <= 0) return;
    const qint64 now = m_clock.nsecsElapsed();
    const int ni = qBound(0, int(net), kNets - 1);
    double charge = 0;
    if (net == Net::BitTorrent) {
        // what libtorrent moved within the budget it was given (LAN peers in its local class and
        // DHT / tracker packets beyond that are not charged). Its counters arrive every 100 ms
        // ([bw-2], they came once a second): up to two seconds of budget may be charged at once,
        // at the budget's rate on average
        const double allowRate = qMax(b.btBudget, b.btPeak);
        const double cap = qMax(65536.0, 2.0 * allowRate);
        b.btAllow = qMin(cap, b.btAllow + allowRate * double(now - b.btAllowNs) / 1e9);
        b.btAllowNs = now;
        b.btActualSlice += wireBytes;
        charge = qMin(double(wireBytes), b.btAllow);
        b.btAllow -= charge;
        b.btCharged += qint64(charge);
        b.nets[ni].slice += qint64(charge);   // [bw-2] its rate, for its share
        refill(b, now);
        const double used = qMin(b.btReserved, charge);   // [bw-2] already reserved
        b.btReserved -= used;
        b.charged += qint64(charge);
        b.tokens = qMax(-b.maxDebt(), b.tokens - (charge - used));
        return;
    } else {
        const double use = qMin(b.granted, double(wireBytes));
        b.granted -= use;
        charge = double(wireBytes) - use;
        b.nets[ni].slice += qint64(charge);   // [bw-2] what it moved without asking (Kad, protocol)
    }
    if (charge <= 0) return;
    refill(b, now);
    b.tokens = qMax(-b.maxDebt(), b.tokens - charge);   // at most two seconds of debt
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
    if (b.rate > 0) b.granted = qMin(b.maxDebt(), b.granted + double(extra));
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
    Bucket &b = m_buckets[d == Up ? 1 : 0];
    if (b.rate <= 0 || net != Net::BitTorrent) return int(b.rate);
    slice(b, m_clock.nsecsElapsed());
    // [bw-2] in KiB steps (libtorrent is not re-set for every byte), never above the limit
    return int(qMin<qint64>(b.rate, qint64(qRound(b.btBudget * b.btTrim / 1024.0)) * 1024));
}

int CoreRateLimiter::shareBps(Dir d, Net net)
{
    QMutexLocker l(&m_mutex);
    Bucket &b = m_buckets[d == Up ? 1 : 0];
    if (b.rate <= 0) return 0;
    slice(b, m_clock.nsecsElapsed());
    return int(b.nets[qBound(0, int(net), kNets - 1)].share);
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
    // [bw-2] the shares are no longer set here once a second: see slice()
    QMutexLocker l(&m_mutex);
    const qint64 now = m_clock.nsecsElapsed();
    for (Bucket &b : m_buckets) slice(b, now);
}

} // namespace mx
