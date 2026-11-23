// SPDX-License-Identifier: GPL-3.0-or-later
// [diag-1] The local network environment: adapters with their addresses (VPN / virtual ones
// flagged), the adapter of the default route and the default gateway. Cheap and synchronous
// (no packets are sent: the default route is found with an unconnected UDP socket "connect").
#pragma once

#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtNetwork/QHostAddress>

namespace mx::diag {

struct AdapterInfo {
    QString name;            // "br0", "{GUID}" on Windows
    QString humanName;       // "Ethernet", "WireGuard Tunnel"
    QStringList ipv4, ipv6;  // without prefix length; IPv6 link-local left out
    bool    up = false;
    bool    loopback = false;
    bool    virtualOrVpn = false;
    QString kindNote;        // "VPN", "virtual", "no internet route" (only what is known)
    bool    isDefault = false;   // carries the default route
    QString description;     // the adapter's description where the system offers one (Windows)
    QString typeName;        // "Wi-Fi", "Ethernet", "virtual", "PPP / VPN", ...
    bool    hasGateway = false;  // the adapter has a default gateway of its own
};

struct NetEnv {
    QList<AdapterInfo> adapters;     // up, non-loopback adapters with an address
    QString defaultSource;           // local IPv4 address used for the internet ("" = no route)
    QString defaultAdapter;          // its adapter (human name)
    bool    defaultIsVpn = false;
    QHostAddress gateway;            // IPv4 default gateway (null = unknown)
    bool    hasIpv6 = false;         // a global IPv6 address exists
};

NetEnv probeNetEnv();
// The local address the OS would use to reach `to` (unconnected UDP; nothing is sent).
QHostAddress localAddressTowards(const QHostAddress &to, quint16 port = 9);
QHostAddress defaultGateway();

} // namespace mx::diag
