// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX core: global up/down token buckets (mx::RateLimiter) with per-network accounting.
// Limits: mx::bwlimit::inForce() (mx/bandwidth_limits.h): bandwidth/downLimitKBs + upLimitKBs, or
// the scheduled ones inside a schedule window (unless the user changed the normal limits during
// that window); re-evaluated on every bandwidth/ setting change and at each schedule boundary.
// Thread-safe: acquire()/account() may be called from any thread.
//
// [bw-1] One shared budget per direction:
//   * acquire() / acquireFor(): file transfers of eD2k, Gnutella, HTTP and the overlay take tokens
//     before they move bytes (and remember them as "granted");
//   * accountTraffic(): wire bytes beyond what was granted (Kad / UDP packets, protocol messages,
//     anything not throttled) are charged to the same bucket, which may go into debt (at most two
//     seconds), so throttled transfers back off and the total stays within the limit;
//   * BitTorrent (libtorrent limits itself) gets budgetBps(); its accounted bytes are charged up to
//     that budget (what goes beyond it is LAN traffic in libtorrent's unlimited local class, or
//     DHT / tracker packets);
//   * exempt(peer) / accountExempt(): LAN peers while bandwidth/limitLan is off.
//   * [eng-1] never starved: while the bucket is empty, acquire() still grants a small amount
//     (kTrickleBytes at most every kTrickleMs per direction) into the bucket's debt, so the protocol
//     messages of a network that had no traffic yet (a hello, a hash set request, a file list) get
//     through while another network uses the whole limit; the debt keeps the long-run total at it.
// [bw-2] Second by second, not only on average:
//   * the bucket refills smoothly and holds at most kSliceMs (100 ms) of the limit, so nobody can
//     take a whole second's allowance at once after a pause (it held one second before: a network
//     that came back got a second's worth in one read, on top of what the others moved);
//   * every 100 ms (a "slice", computed when the limiter is next used) each network's rate is
//     measured (granted + charged bytes, averaged over about half a second) and the limit is shared
//     max-min fairly: a network that wants more than it gets ("hungry": acquire() had to refuse it in
//     the last kHungryMs, or BitTorrent uses at least 85 % of its budget) may get an equal share, one
//     that is satisfied keeps what it uses plus a margin, the rest goes to the hungry ones;
//   * while another network is hungry, acquireFor() gives a network no more than its share (its own
//     small bucket refilled at its share); while nobody else wants more, the whole bucket is open to
//     it, so no throughput is lost;
//   * BitTorrent's budget moves halfway to its share every slice (a stable share that changes
//     gradually, a joining network gets its share within about half a second).
#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QMutex>
#include <QtCore/QObject>
#include <QtCore/QTime>
#include <QtCore/QTimer>

#include <QtCore/QElapsedTimer>
#include <array>
#include <atomic>

#include "mx/bandwidth_limits.h"
#include "mx/plugin.h"

namespace mx {

class CoreRateLimiter : public QObject, public RateLimiter {
    Q_OBJECT
public:
    explicit CoreRateLimiter(Settings *settings, QObject *parent = nullptr);

    qint64 acquire(Dir d, qint64 bytes) override;
    qint64 acquireFor(Dir d, Net net, qint64 bytes) override;   // [bw-2] fair share per network
    void reportWaiting(Dir d, Net net, qint64 bytes) override;    // [bw-2] libtorrent's queued bytes
    void account(Dir d, Net net, qint64 bytes) override;
    void accountTraffic(Dir d, Net net, qint64 wireBytes, qint64 payloadBytes) override;
    int limitBps(Dir d) const override;
    int budgetBps(Dir d, Net net) const override;
    bool exempt(const QHostAddress &peer) const override;
    void accountExempt(Dir d, Net net, qint64 wireBytes, qint64 payloadBytes) override;

    void setLimits(int downBps, int upBps);       // 0 = unlimited (tests / manual override)
    void setLimitLan(bool on) { m_limitLan = on; }
    void reloadFromSettings();                    // applies limits + schedule now
    static bool scheduleActive(const QTime &now, int dayOfWeek, const QString &start, const QString &end, int daysMask);
    bwlimit::InForce inForce() const;             // what reloadFromSettings() applied last

    // Totals of everything accounted (bytes since construction).
    qint64 total(Dir d) const;
    qint64 total(Dir d, Net net) const;
    // File-data part of the totals (accountTraffic payloadBytes).
    qint64 payloadTotal(Dir d) const;
    qint64 payloadTotal(Dir d, Net net) const;
    // [bw-1] bytes charged to the shared budget (acquired + charged), and exempt (LAN) bytes
    qint64 chargedTotal(Dir d) const;
    qint64 exemptTotal(Dir d) const;
    static constexpr int kNets = 16;              // > every mx::Net value
    static constexpr int kTrickleBytes = 1500;    // [eng-1] see the head of this file
    static constexpr int kTrickleMs = 250;
    static constexpr int kSliceMs = 100;          // [bw-2] bucket depth and share update interval
    static constexpr int kHungryMs = 300;         // [bw-2] "wants more" lasts this long after a refusal
    // Called once per second by the BandwidthService: rolls the per-network "last second" counters.
    void tick();
    // [bw-2] the share of every network now (bytes/s; tests, diagnostics); runs the slice first
    int shareBps(Dir d, Net net);

signals:
    void limitsChanged(int downBps, int upBps);

private:
    struct NetState {             // [bw-2] one network in one direction
        double rate = 0;          // bytes/s it was granted or charged, averaged over ~0.5 s
        qint64 slice = 0;         // bytes granted / charged in the current slice
        qint64 wantNs = -(qint64(1) << 60);   // last time acquireFor() could not give all it asked
        double share = 0;         // its fair share (bytes/s), set every slice
        double tokens = 0;        // its own bucket, refilled at `share`
        qint64 tokensNs = 0;
        bool active = false;      // took part in the last share computation
    };
    struct Bucket {
        qint64 rate = 0;      // bytes/s, 0 = unlimited
        double tokens = 0;    // may be negative (debt of charged traffic), >= -maxDebt()
        qint64 lastNs = 0;
        double granted = 0;   // acquired, not yet seen in accountTraffic()
        // BitTorrent
        double btBudget = 0;        // its budget (moves towards its share every slice)
        double btPeak = 0;          // [bw-2] the highest budget of the last 500 ms (charged at that rate)
        qint64 btPeakNs = 0;
        double btReserved = 0;      // [bw-2] reserved for libtorrent's bytes, not yet reported
        qint64 btActualSlice = 0;   // [bw-2] all of libtorrent's bytes in this slice (not capped)
        double btActualRate = 0;    //        averaged over about a second
        double btTrim = 1.0;        //        what libtorrent is handed: budget x trim (0.9 to 1)
        double btAllow = 0;         // how much BitTorrent traffic may still be charged
        qint64 btAllowNs = 0;
        qint64 charged = 0;         // cumulative bytes charged (acquire + charge)
        qint64 btCharged = 0;       // the BitTorrent part of it
        qint64 denied = 0;          // cumulative bytes acquire() could not give
        qint64 trickleNs = 0;       // [eng-1] last small grant made while the bucket was empty
        qint64 sliceNs = 0;         // [bw-2] start of the current slice
        std::array<NetState, kNets> nets{};
        double capacity() const { return qMax<double>(double(rate) * kSliceMs / 1000.0, 3000.0); }   // [bw-2] 100 ms
        double maxDebt() const { return 2.0 * qMax<double>(double(rate), 4096.0); }                 // two seconds
    };
    void refill(Bucket &b, qint64 nowNs);
    void slice(Bucket &b, qint64 nowNs) const;         // [bw-2] rates, hungry flags, shares
    qint64 take(Bucket &b, int net, qint64 bytes, qint64 nowNs);
    void addTotals(Dir d, Net net, qint64 wireBytes, qint64 payloadBytes);
    void scheduleNextReload(const bwlimit::InForce &f);

    Settings *m_settings;
    mutable QMutex m_mutex;
    mutable Bucket m_buckets[2];   // mutable: budgetBps() runs a due slice first
    QElapsedTimer m_clock;
    std::array<std::atomic<qint64>, 2 * kNets> m_totals{};
    std::array<std::atomic<qint64>, 2 * kNets> m_payload{};
    std::array<qint64, 2 * kNets> m_lastTick{};
    std::array<std::atomic<qint64>, 2 * kNets> m_lastSecond{};
    std::array<std::atomic<qint64>, 2> m_exempt{};
    std::atomic<bool> m_limitLan{false};
    bwlimit::InForce m_inForce;
    QTimer m_scheduleTimer;
};

} // namespace mx
