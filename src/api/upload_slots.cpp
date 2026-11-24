// SPDX-License-Identifier: GPL-3.0-or-later
// [net-3] the shared upload slot pool (see mx/upload_slots.h)
#include "mx/upload_slots.h"

#include "mx/services.h"
#include "mx/settings_keys.h"

namespace mx {

UploadSlots::UploadSlots(Settings *settings, QObject *parent) : QObject(parent), m_settings(settings)
{
    m_clock.start();
    m_notify.setSingleShot(true);
    connect(&m_notify, &QTimer::timeout, this, &UploadSlots::slotsFreed);
    m_grace.setSingleShot(true);
    connect(&m_grace, &QTimer::timeout, this, &UploadSlots::slotsFreed);
    if (m_settings)
        connect(m_settings, &Settings::changed, this, [this](const QString &key, const QVariant &) {
            if (key == keys::transfers::MaxUploads) notifySoon();
        });
}

int UploadSlots::total() const
{
    if (!m_settings) return 3;
    const QVariant v = m_settings->value(QString(keys::transfers::MaxUploads));
    return qBound(0, v.isValid() ? v.toInt() : 3, 500);
}

int UploadSlots::usedBy(Net net) const
{
    int n = 0;
    for (auto it = m_holders.cbegin(); it != m_holders.cend(); ++it) n += it.value() == net;
    return n;
}

void UploadSlots::setWaiting(Net net, int waiting, int graceMs)
{
    NetState &s = m_nets[int(net)];
    const bool more = waiting > s.waiting;
    s.waiting = qMax(0, waiting);
    s.graceMs = qMax(1000, graceMs);
    if (s.waiting == 0) s.preferredSinceMs = -1;
    if (more && used() < total()) notifySoon();   // a free slot and someone new to take it
}

int UploadSlots::available(Net net)
{
    const int free = total() - used();
    if (free <= 0) return 0;
    const int mine = usedBy(net);
    const qint64 t = now();
    int held = 0;
    qint64 nextExpiry = -1;
    for (auto it = m_nets.begin(); it != m_nets.end(); ++it) {
        if (it.key() == int(net) || it->waiting <= 0) continue;
        const int theirs = usedBy(Net(it.key()));
        if (theirs >= mine) continue;
        if (it->preferredSinceMs < 0) it->preferredSinceMs = t;
        const qint64 left = it->preferredSinceMs + it->graceMs - t;
        if (left <= 0) continue;   // did not take it in time: the others may
        held += qMin(it->waiting, mine - theirs);
        if (nextExpiry < 0 || left < nextExpiry) nextExpiry = left;
    }
    if (held > 0 && nextExpiry > 0) notifyAfterGrace(int(nextExpiry) + 50);   // ask again when the grace runs out
    return qMax(0, free - held);
}

bool UploadSlots::acquire(Net net, const QString &id, bool forced)
{
    if (id.isEmpty()) return false;
    if (m_holders.contains(id)) return true;
    if (!forced && available(net) <= 0) return false;
    m_holders.insert(id, net);
    const qint64 t = now();
    for (auto it = m_nets.begin(); it != m_nets.end(); ++it) {
        if (it.key() == int(net)) it->preferredSinceMs = -1;   // it got one
        else if (it->preferredSinceMs >= 0 && t - it->preferredSinceMs > it->graceMs)
            it->preferredSinceMs = -1;   // its grace ran out: a fresh one at the next free slot
    }
    return true;
}

void UploadSlots::release(const QString &id)
{
    if (m_holders.remove(id) > 0) notifySoon();
}

void UploadSlots::notifyAfterGrace(int ms)
{
    if (m_grace.isActive() && m_grace.remainingTime() <= ms) return;
    m_grace.start(ms);
}

void UploadSlots::notifySoon(int ms)
{
    if (m_notify.isActive() && m_notify.remainingTime() <= ms) return;
    m_notify.start(ms);
}

} // namespace mx
