// SPDX-License-Identifier: GPL-3.0-or-later
// [bw-1] The speed limits in force, computed the same way by the core (CoreRateLimiter) and the UI
// (Bandwidth page row, tray menu, title, Settings). Header only: no build-file change needed.
//
// Keys (mx::keys::bandwidth): downLimitKBs / upLimitKBs = the normal ("manual") limits, 0 = off,
// lastDown/lastUpLimitKBs = the values shown and restored while a limit is off; scheduleEnabled +
// scheduleStart / scheduleEnd / scheduleDays + scheduleDown/UpKBs = other limits at set times;
// scheduleOverrideUntil = "the user changed the normal limits while the schedule was in force":
// until that moment (the end of that schedule window) the normal limits win.
//
// Who wins: the most recent action. Inside a schedule window the scheduled limits apply, unless
// the user changed a normal limit (Bandwidth page row, tray menu, Settings, script) during that
// window: then the normal limits apply until the window ends, and the next window applies the
// schedule again. Changing the schedule itself ends such an override at once.
//
// Upload: a limit that is on is never below kMinUploadKBs. BitTorrent and eD2k give download
// bandwidth to those who upload (tit for tat, eD2k credits); below about 10 KB/s downloads starve.
#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QString>
#include <QtNetwork/QHostAddress>

#include "mx/services.h"
#include "mx/settings_keys.h"

namespace mx::bwlimit {

inline constexpr int kMinUploadKBs = 10;

inline int clampUp(int kbs) { return kbs > 0 ? qMax(kbs, kMinUploadKBs) : 0; }

// schedule window logic; dayOfWeek 1 = Monday ... 7 = Sunday, daysMask bit 0 = Monday. For an
// overnight window (start > end) the day the window starts counts.
inline bool windowActive(const QTime &now, int dayOfWeek, const QString &start, const QString &end, int daysMask)
{
    const QTime s = QTime::fromString(start, QStringLiteral("HH:mm"));
    const QTime e = QTime::fromString(end, QStringLiteral("HH:mm"));
    if (!s.isValid() || !e.isValid())
        return false;
    auto dayOn = [daysMask](int dow) { return (daysMask >> (dow - 1)) & 1; };
    if (s <= e)
        return dayOn(dayOfWeek) && now >= s && now < e;
    if (now >= s)
        return dayOn(dayOfWeek);
    if (now < e)
        return dayOn(dayOfWeek == 1 ? 7 : dayOfWeek - 1);
    return false;
}

// End of the window that contains `now` (call only while windowActive()).
inline QDateTime windowEnd(const QDateTime &now, const QString &start, const QString &end)
{
    const QTime s = QTime::fromString(start, QStringLiteral("HH:mm"));
    const QTime e = QTime::fromString(end, QStringLiteral("HH:mm"));
    QDateTime t(now.date(), e);
    if (s > e && now.time() >= s)
        t = t.addDays(1);
    return t;
}

// Next moment a window starts after `now` (invalid: no day selected / bad times).
inline QDateTime nextStart(const QDateTime &now, const QString &start, const QString &end, int daysMask)
{
    const QTime s = QTime::fromString(start, QStringLiteral("HH:mm"));
    if (!s.isValid() || !QTime::fromString(end, QStringLiteral("HH:mm")).isValid() || !(daysMask & 127))
        return QDateTime();
    for (int d = 0; d <= 7; ++d) {
        const QDate day = now.date().addDays(d);
        const QDateTime t(day, s);
        if (t > now && ((daysMask >> (day.dayOfWeek() - 1)) & 1))
            return t;
    }
    return QDateTime();
}

struct InForce {
    enum Source { Normal = 0, Schedule, Manual };
    int downKBs = 0;          // in force, 0 = unlimited
    int upKBs = 0;            // in force, 0 = unlimited, else >= kMinUploadKBs
    Source source = Normal;   // Manual = the normal limits, chosen during a schedule window
    QDateTime until;          // Schedule / Manual: when the window ends; Normal: next window start (if any)
    bool scheduleEnabled = false;
};

inline bool scheduleWindowNow(const Settings *s, const QDateTime &now)
{
    return s->b(keys::bandwidth::ScheduleEnabled) &&
           windowActive(now.time(), now.date().dayOfWeek(), s->s(keys::bandwidth::ScheduleStart), s->s(keys::bandwidth::ScheduleEnd),
                        s->i(keys::bandwidth::ScheduleDays));
}

inline QDateTime overrideUntil(const Settings *s)
{
    const QString v = s->s(QString(keys::bandwidth::ScheduleOverrideUntil));
    return v.isEmpty() ? QDateTime() : QDateTime::fromString(v, Qt::ISODate);
}

inline InForce inForce(const Settings *s, const QDateTime &now = QDateTime::currentDateTime())
{
    InForce f;
    if (!s)
        return f;
    f.downKBs = qMax(0, s->i(keys::bandwidth::DownLimitKBs));
    f.upKBs = clampUp(s->i(keys::bandwidth::UpLimitKBs));
    f.scheduleEnabled = s->b(keys::bandwidth::ScheduleEnabled);
    if (!f.scheduleEnabled)
        return f;
    const QString a = s->s(keys::bandwidth::ScheduleStart), b = s->s(keys::bandwidth::ScheduleEnd);
    if (!scheduleWindowNow(s, now)) {
        f.until = nextStart(now, a, b, s->i(keys::bandwidth::ScheduleDays));
        return f;
    }
    f.until = windowEnd(now, a, b);
    const QDateTime ov = overrideUntil(s);
    if (ov.isValid() && now < ov) {
        f.source = InForce::Manual;
        return f;
    }
    f.source = InForce::Schedule;
    f.downKBs = qMax(0, s->i(keys::bandwidth::ScheduleDownKBs));
    f.upKBs = clampUp(s->i(keys::bandwidth::ScheduleUpKBs));
    return f;
}

// A normal limit was changed: inside a schedule window it wins until the window ends.
inline void noteManualChange(Settings *s, const QDateTime &now = QDateTime::currentDateTime())
{
    if (!s || !scheduleWindowNow(s, now))
        return;
    const QString until = windowEnd(now, s->s(keys::bandwidth::ScheduleStart), s->s(keys::bandwidth::ScheduleEnd)).toString(Qt::ISODate);
    if (s->s(QString(keys::bandwidth::ScheduleOverrideUntil)) != until)
        s->setValue(QString(keys::bandwidth::ScheduleOverrideUntil), until);
}

inline bool isScheduleKey(const QString &key)
{
    return key == QLatin1String(keys::bandwidth::ScheduleEnabled) || key == QLatin1String(keys::bandwidth::ScheduleStart) ||
           key == QLatin1String(keys::bandwidth::ScheduleEnd) || key == QLatin1String(keys::bandwidth::ScheduleDays) ||
           key == QLatin1String(keys::bandwidth::ScheduleDownKBs) || key == QLatin1String(keys::bandwidth::ScheduleUpKBs);
}

// Follow Settings::changed: a normal-limit change during a window starts an override, a schedule
// change ends it. Installed by the core and by the UI (idempotent: once per Settings object).
inline void installOverrideRule(Settings *s)
{
    if (!s || s->property("mxBwOverrideRule").toBool())
        return;
    s->setProperty("mxBwOverrideRule", true);
    QObject::connect(s, &Settings::changed, s, [s](const QString &key, const QVariant &) {
        if (key == QLatin1String(keys::bandwidth::DownLimitKBs) || key == QLatin1String(keys::bandwidth::UpLimitKBs))
            noteManualChange(s);
        else if (isScheduleKey(key) && !s->s(QString(keys::bandwidth::ScheduleOverrideUntil)).isEmpty())
            s->setValue(QString(keys::bandwidth::ScheduleOverrideUntil), QString());
    });
}

// LAN peers (exempt from the limits unless bandwidth/limitLan): loopback, private IPv4, link-local,
// IPv6 unique-local; the same ranges libtorrent puts in its local peer class.
inline bool isLanAddress(const QHostAddress &a0)
{
    bool v4 = false;
    const quint32 ip = a0.toIPv4Address(&v4);   // also IPv4-mapped IPv6
    if (v4) {
        return (ip >> 24) == 10 || (ip >> 24) == 127 || (ip >> 20) == ((172u << 4) | 1u) || (ip >> 16) == ((192u << 8) | 168u) ||
               (ip >> 16) == ((169u << 8) | 254u);
    }
    if (a0.protocol() != QAbstractSocket::IPv6Protocol)
        return false;
    if (a0.isLoopback())
        return true;
    const Q_IPV6ADDR b = a0.toIPv6Address();
    return (b[0] & 0xfe) == 0xfc || (b[0] == 0xfe && (b[1] & 0xc0) == 0x80);
}

} // namespace mx::bwlimit
