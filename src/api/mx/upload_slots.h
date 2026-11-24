// SPDX-License-Identifier: GPL-3.0-or-later
// [net-3] One pool of upload slots for the networks that queue uploads: the OctoMX network, eD2k and
// Gnutella. "Max total uploads" (transfers/maxUploads) is one total over them, as the label reads; a network
// whose own page sets a slot count (ed2k/uploadSlots, gnutella/uploadSlots above 0) keeps that count
// to itself and does not ask the pool. BitTorrent has its own unchoke slots (bt/maxUploads).
//
// Fair between networks: a free slot goes first to a network that waits and holds fewer slots than
// the one asking. Such a network has a grace time to take it (it may need to poll or to connect back);
// when it does not, the slot goes to the next one, so no network can block the others and none
// starves. "Start Transfer" and the small file bypass take a slot beyond the total (counted).
// Per-user limits stay with the network (transfers/maxUploadsPerUser on the OctoMX network).
#pragma once

#include "mx/types.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QTimer>

namespace mx {

class Settings;

class UploadSlots : public QObject {
    Q_OBJECT
public:
    explicit UploadSlots(Settings *settings, QObject *parent = nullptr);

    int total() const;                  // transfers/maxUploads (0: no uploads on these networks)
    int used() const { return int(m_holders.size()); }
    int usedBy(Net net) const;
    // uploads `net` may start now: the free slots less those held for networks that wait with fewer
    void setWaiting(Net net, int waiting, int graceMs);
    int available(Net net);
    bool acquire(Net net, const QString &id, bool forced = false);   // false: no slot for it now
    void release(const QString &id);
    bool holds(const QString &id) const { return m_holders.contains(id); }
    int waiting(Net net) const { return m_nets.value(int(net)).waiting; }

signals:
    // a slot became free, the total grew or a held slot's grace ran out: queues may promote
    void slotsFreed();

private:
    struct NetState {
        int waiting = 0;
        int graceMs = 5000;
        qint64 preferredSinceMs = -1;   // a free slot has been held for it since
    };
    qint64 now() const { return m_clock.elapsed(); }
    void notifySoon(int ms = 0);
    void notifyAfterGrace(int ms);

    Settings *m_settings;
    QHash<QString, Net> m_holders;      // upload id -> network
    QHash<int, NetState> m_nets;
    QElapsedTimer m_clock;
    QTimer m_notify;
    QTimer m_grace;   // a held slot's grace runs out (its own timer: an earlier "soon" must not swallow it)
};

} // namespace mx
