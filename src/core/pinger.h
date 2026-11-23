// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX core: "Ping" for search results, sources and users.
// A ping asks the source's network plugin first (NetworkPlugin::ping); if the plugin cannot, the
// round trip is measured as the TCP connect time to the source's address ("ip:port" from
// user.address / user.id, an ed2k-peer:// or http(s):// URI). Sources without an address (DHT
// swarms, firewalled peers) answer -1 at once. At most 32 TCP probes run at the same time; every
// probe gives up after 5 s (-1).
#pragma once

#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QQueue>

#include <functional>

#include "mx/plugin.h"

namespace mx {

class NetworkManager;

class Pinger : public QObject {
    Q_OBJECT
public:
    using Done = std::function<void(int ms)>;
    explicit Pinger(NetworkManager *network, QObject *parent = nullptr);

    void ping(const SearchSource &src, Done done);
    void setTimeoutMs(int ms) { m_timeoutMs = ms; }
    int running() const { return m_running; }

    // "host", port from the source; false if it has no usable address.
    static bool addressOf(const SearchSource &src, QString *host, quint16 *port);

private:
    struct Probe { QString host; quint16 port = 0; Done done; };
    void startProbe(const Probe &p);
    void next();

    NetworkManager *m_network;
    quint32 m_nextToken = 1;
    QHash<quint32, Done> m_pluginWaits;
    QQueue<Probe> m_queue;
    int m_running = 0;
    int m_timeoutMs = 5000;
    static constexpr int kMaxParallel = 32;
};

} // namespace mx
