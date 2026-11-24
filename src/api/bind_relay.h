// SPDX-License-Identifier: GPL-3.0-or-later
// [net-2] OctoMX: a SOCKS5 relay on this computer (127.0.0.1, random port, random password) for
// QNetworkAccessManager, which cannot bind its connections to an adapter. Each connection the
// relay opens is bound to the chosen adapter's address (and keeps to IPv4 when IPv6 is off); when
// the adapter is missing it refuses, so a web request fails instead of using another adapter.
// The policy travels in the SOCKS5 user name, so one relay serves every setting. It runs in a
// thread of its own and is stopped when the application ends. Private to mxapi.
#pragma once

#include <QtNetwork/QNetworkProxy>

namespace mx {
namespace netpolicy {
namespace relay {

QNetworkProxy proxyFor(const QString &bind, bool ipv6);

// tests: connections the relay made since the start (and those it refused)
int connectionsMade();
int connectionsRefused();

} // namespace relay
} // namespace netpolicy
} // namespace mx
