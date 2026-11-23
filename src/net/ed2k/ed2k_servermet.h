// SPDX-License-Identifier: GPL-3.0-or-later
// server.met (eMule server list file) parse/write and the in-memory server record.
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QList>
#include <QtCore/QString>

namespace mx::ed2k {

struct ServerInfo {
    quint32 ip = 0;            // eD2k byte order (see ed2k_io.h)
    quint16 port = 0;
    QString dynHost;           // ST_DYNIP hostname (resolved at connect time)
    QString name;
    QString description;
    QString version;
    quint32 users = 0, files = 0, maxUsers = 0, softFiles = 0, hardFiles = 0, lowIdUsers = 0;
    quint32 ping = 0;          // ms (last measured)
    quint32 failCount = 0;
    quint32 preference = 0;    // 0 normal, 1 high, 2 low (eMule convention)
    quint32 lastPingTime = 0;  // unix seconds
    quint32 udpFlags = 0;
    quint32 tcpFlags = 0;      // from OP_IDCHANGE (runtime)
    quint16 obfuscationTcpPort = 0, obfuscationUdpPort = 0;
    quint32 udpKey = 0, udpKeyIp = 0;
    bool    isStatic = false;  // user-added: never purged (stored as custom tag "mxstatic")
    // runtime
    qint64  lastConnectAttemptMs = 0;
    qint64  nextRetryMs = 0;
    quint32 udpChallenge = 0;
    qint64  udpPingSentMs = 0;
    int     udpPingTries = 0;
    bool    obfFailed = false;
    bool    pingedThisSession = false;

    QString address() const;   // "ip:port" (or "host:port" for dynamic servers without IP yet)
    QString id() const { return address(); }
};

// Parses a server.met (header 0x0E or 0xE0). Garbage entries are skipped; hostile counts bounded.
bool parseServerMet(const QByteArray &data, QList<ServerInfo> *out, QString *error = nullptr);
QByteArray writeServerMet(const QList<ServerInfo> &servers);

// server list as published on the web: plain server.met or gzip-compressed one (auto-detected)
bool parseServerMetDownload(const QByteArray &data, QList<ServerInfo> *out, QString *error = nullptr);

// Known fake / spy servers (by name or address); also rejects absurd entries.
bool isKnownBadServer(const ServerInfo &s);
// Well-known long-running honest servers get a bonus in auto-connect ordering.
int serverTrustScore(const ServerInfo &s);

} // namespace mx::ed2k
