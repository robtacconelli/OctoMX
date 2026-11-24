// SPDX-License-Identifier: GPL-3.0-or-later
// [diag-1] One-shot outbound checks for the Connection Test: DNS lookups, a BitTorrent UDP tracker
// connect handshake (BEP 15), a DHT ping (BEP 5) from a fresh UDP socket, HTTPS reachability and the
// public TCP port checker. Every check is asynchronous and bounded in time; the callback is
// invoked exactly once on the thread of `ctx` (never after ctx is destroyed).
#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <functional>

class QNetworkAccessManager;

namespace mx { class Settings; }

namespace mx::diag {

// [net-2] the settings whose proxy / adapter the checks follow (mx/net_policy.h): no name lookups
// here when the proxy resolves names, no UDP while a proxy is set, UDP from the chosen adapter
void setPolicySettings(const Settings *s);

struct CheckResult {
    bool    ok = false;
    bool    answered = false;  // port checker: it gave a verdict (ok = open)
    qint64  ms = -1;
    QString detail;            // addresses, HTTP status, "connection id received"
    QString error;
};
using CheckDone = std::function<void(const CheckResult &)>;

void dnsLookup(QObject *ctx, const QString &host, int timeoutMs, CheckDone done);
void udpTrackerHandshake(QObject *ctx, const QString &host, quint16 port, int timeoutMs, CheckDone done);
void dhtPing(QObject *ctx, const QString &host, quint16 port, int timeoutMs, CheckDone done);
void httpReach(QObject *ctx, QNetworkAccessManager *nam, const QString &url, int timeoutMs, CheckDone done);
// urlTemplate with "%1" = port; the service answers "1" (open) or "0" (closed)
void publicPortCheck(QObject *ctx, QNetworkAccessManager *nam, const QString &urlTemplate, int port, int timeoutMs,
                     CheckDone done);
// "udp://host:port/announce" -> host, port; false for other schemes
bool parseUdpTracker(const QString &url, QString *host, quint16 *port);
// "host:port" -> host, port
bool parseHostPortText(const QString &s, QString *host, quint16 *port, quint16 defaultPort);

} // namespace mx::diag
