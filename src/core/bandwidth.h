// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX core: BandwidthService: samples the RateLimiter totals once per second, keeps one hour
// of history, session totals, and all-time totals persisted in the stats table
// ("allTimeDownloaded"/"allTimeUploaded"; written every 60 s and on shutdown).
#pragma once

#include <QtCore/QList>
#include <QtCore/QTimer>

#include "mx/services.h"

namespace mx {

class CoreRateLimiter;
class Database;

class CoreBandwidth : public BandwidthService {
    Q_OBJECT
public:
    CoreBandwidth(CoreRateLimiter *limiter, Database *db, QObject *parent = nullptr);

    void start();          // loads all-time totals, starts the 1 Hz timer
    void shutdown();       // persists totals
    void tickNow();        // take one sample immediately (tests)

    QList<BandwidthSample> history(int seconds = 600) const override;
    BandwidthSample current() const override;
    qint64 totalDownloaded() const override;
    qint64 totalUploaded() const override;
    qint64 allTimeDownloaded() const override;
    qint64 allTimeUploaded() const override;

private:
    void persist();
    CoreRateLimiter *m_limiter;
    Database *m_db;
    QTimer m_timer;
    QList<BandwidthSample> m_history;   // newest last, ≤ 3600
    qint64 m_lastDown = 0, m_lastUp = 0;
    qint64 m_lastPayDown = 0, m_lastPayUp = 0;
    QList<qint64> m_lastNet;               // per Net index: down totals then up totals
    qint64 m_baseDown = 0, m_baseUp = 0;   // all-time totals at start
    qint64 m_startDown = 0, m_startUp = 0; // limiter totals at start
    qint64 m_lastTickMs = 0;
    int m_ticks = 0;
};

} // namespace mx
