// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX: BitTorrent plugin internals shared by the facade and the session worker.
// Private to mxnet_bt.
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QDateTime>
#include <QtCore/QLoggingCategory>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVariantMap>

#include <libtorrent/info_hash.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/sha1_hash.hpp>

#include "mx/types.h"

Q_DECLARE_LOGGING_CATEGORY(lcBt)

namespace mx {
class Settings;
namespace bt {

// Everything the session needs from mx::Settings, read on the main thread and handed to the worker.
struct Config {
    // network
    int listenPort = 6881;          // 0 = let the OS pick
    bool randomizePorts = false;    // pick a random port in [20000, 60000) once per process start
    QString bindInterface;          // empty = all
    bool ipv6 = true;
    bool dht = true, pex = true, lsd = true, utp = true, upnp = true, natpmp = true;
    bool coreMapsPorts = false;     // [diag-1] the core's PortMapper forwards our ports: libtorrent's UPnP / NAT-PMP off
    int announcePort = 0;           // [diag-1] external port the router gave us (0 = the listen port)
    QString encryption = QStringLiteral("enabled");   // enabled | forced | disabled
    int maxConnections = 300, maxConnectionsPerTorrent = 80;
    int maxUploads = 16, maxUploadsPerTorrent = 4;
    int maxActiveDownloads = 5, maxActiveSeeds = 10;
    double seedRatioLimit = 2.0;    // 0 = unlimited
    int seedTimeLimitMin = 0;       // 0 = unlimited
    QString seedLimitAction = QStringLiteral("pause");
    bool seedCompleted = true;
    bool announceToAllTrackers = true;
    QStringList extraTrackers;
    QStringList bootstrapNodes;     // "host:port" (settings), user-added ones live in dataDir
    bool anonymousMode = false;
    bool superSeeding = false;
    bool preallocate = false;
    bool webSeeds = true;
    // proxy
    QString proxyType = QStringLiteral("none");       // none | socks4 | socks5 | http
    QString proxyHost;
    int proxyPort = 1080;
    bool proxyAuth = false;
    QString proxyUser, proxyPassword;
    bool proxyPeers = true, proxyHostnames = true;
    // bandwidth: the limits themselves come from ctx.limiter->budgetBps() (or setRateLimits())
    bool limitLan = false;          // bandwidth/limitLan: LAN peers are limited too
    // not (yet) in the settings schema: fixed defaults until keys for them are added
    bool allowMultipleConnectionsPerIp = false;
    bool validateHttpsTrackers = true;
    // identity
    QString clientName = QStringLiteral("OctoMX");
    QString clientVersion = QStringLiteral(MX_VERSION_STRING);

    static Config fromSettings(const Settings *s);
};

// Persisted per-torrent record (<dataDir>/torrents/<id>.json); libtorrent's own state is in
// <id>.fastresume, the metadata in <id>.torrent.
struct Record {
    QString id;
    QString uri;            // magnet (if added by magnet)
    QString savePath;
    QString name;
    QDateTime added;
    int priority = 0;       // -1 / 0 / 1
    QString seededPath;     // seedLocalFile(): the local file this torrent shares
    bool seedLimitReached = false;
    QVariantMap toVariant() const;
    static Record fromVariant(const QVariantMap &m);
};

QString hexId(const lt::info_hash_t &ih);          // v1 hex, else truncated v2 hex
QString toHex(const lt::sha1_hash &h);
QString endpointString(const QString &host, int port);
bool parseHostPort(const QString &s, QString *host, int *port, int defaultPort);

// libtorrent settings for the config. online=false → no listen socket, no DHT/LSD/UPnP/NAT-PMP.
lt::settings_pack makeSettingsPack(const Config &c, bool online, int effectiveListenPort,
                                   const QStringList &userNodes, int downBps, int upBps);

// Linux/BSD: locate the system CA bundle and export SSL_CERT_FILE / SSL_CERT_DIR so the
// statically linked OpenSSL inside libtorrent verifies HTTPS trackers. No-op on Windows (libtorrent
// loads the Windows ROOT store itself) and when the variables are already set. Returns what it used.
QString setupCaBundle();

// Atomic file helpers (write to temp + rename).
bool writeFileAtomic(const QString &path, const QByteArray &data, QString *error = nullptr);
QByteArray readFile(const QString &path, qint64 maxSize = 64 * 1024 * 1024);

} // namespace bt
} // namespace mx
