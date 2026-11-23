// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX core: global up/down token buckets (mx::RateLimiter) with per-network accounting.
// Limits: mx::bwlimit::inForce() (mx/bandwidth_limits.h): bandwidth/downLimitKBs + upLimitKBs, or
// the scheduled ones inside a schedule window (unless the user changed the normal limits during
// that window); re-evaluated on every bandwidth/ setting change and at each schedule boundary.
// Thread-safe: acquire()/account() may be called from any thread.
//
// [bw-1] One shared budget per direction:
//   * acquire(): file transfers of eD2k, Gnutella, HTTP and the overlay take tokens before they
//     move bytes (and remember them as "granted");
//   * accountTraffic(): wire bytes beyond what was granted (Kad / UDP packets, protocol messages,
//     anything not throttled) are charged to the same bucket, which may go into debt (at most two
//     seconds), so throttled transfers back off and the total stays within the limit;
//   * BitTorrent (libtorrent limits itself) gets budgetBps(), set once per second in tick(): the
//     limit minus what the others were charged in the last second while they got all they asked
//     for; while acquire() had to refuse them, a fifth of the limit less each second, never below
//     half of it (both busy: about half each); its accounted bytes are charged up to
//     that budget (what goes beyond it is LAN traffic in libtorrent's unlimited local class, or
//     DHT / tracker packets);
//   * exempt(peer) / accountExempt(): LAN peers while bandwidth/limitLan is off.
//   * [eng-1] never starved: while the bucket is empty, acquire() still grants a small amount
//     (kTrickleBytes at most every kTrickleMs per direction) into the bucket's debt, so the protocol
//     messages of a network that had no traffic yet (a hello, a hash set request, a file list) get
//     through while another network uses the whole limit; the debt keeps the long-run total at it.
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
    // Called once per second by the BandwidthService: rolls the per-network "last second" counters
    // used by budgetBps().
    void tick();

signals:
    void limitsChanged(int downBps, int upBps);

private:
    struct Bucket {
        qint64 rate = 0;      // bytes/s, 0 = unlimited
        double tokens = 0;    // may be negative (debt of charged traffic), >= -2 * capacity
        qint64 lastNs = 0;
        double granted = 0;   // acquired, not yet seen in accountTraffic()
        // BitTorrent
        mutable int btBudget = 0;   // last budget handed out
        double btAllow = 0;         // how much BitTorrent traffic may still be charged
        qint64 btAllowNs = 0;
        qint64 charged = 0;         // cumulative bytes charged (acquire + charge)
        qint64 btCharged = 0;       // the BitTorrent part of it
        qint64 othersAtTick = 0;
        qint64 others1s = 0;        // non-BitTorrent bytes charged in the last second
        qint64 denied = 0;          // cumulative bytes acquire() could not give
        qint64 deniedAtTick = 0;
        qint64 trickleNs = 0;       // [eng-1] last small grant made while the bucket was empty
        double capacity() const { return qMax<double>(double(rate), 4096.0); }   // <= 1 s burst
    };
    void refill(Bucket &b, qint64 nowNs);
    void addTotals(Dir d, Net net, qint64 wireBytes, qint64 payloadBytes);
    void scheduleNextReload(const bwlimit::InForce &f);

    Settings *m_settings;
    mutable QMutex m_mutex;
    Bucket m_buckets[2];
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
