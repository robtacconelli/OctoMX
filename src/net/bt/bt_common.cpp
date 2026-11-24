// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX: BitTorrent plugin internals: settings mapping, small helpers.
#include "bt_common.h"
#include "bt_pacing.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QRandomGenerator>
#include <QtCore/QSaveFile>
#include <QtNetwork/QHostAddress>

#include <libtorrent/alert.hpp>
#include <libtorrent/fingerprint.hpp>
#include <libtorrent/version.hpp>

#include "mx/net_policy.h"
#include "mx/services.h"
#include "mx/settings_keys.h"

Q_LOGGING_CATEGORY(lcBt, "mx.bt")

namespace mx {
namespace bt {

namespace {
QVariant get(const Settings *s, QLatin1StringView key, const QVariant &fallback)
{
    if (!s)
        return fallback;
    const QVariant v = s->value(QString(key));
    return v.isValid() ? v : fallback;
}
QVariant get(const Settings *s, const char *key, const QVariant &fallback)
{
    return get(s, QLatin1StringView(key), fallback);
}
} // namespace

Config Config::fromSettings(const Settings *s)
{
    namespace k = mx::keys;
    Config c;
    c.listenPort = get(s, k::bt::ListenPort, c.listenPort).toInt();
    c.randomizePorts = get(s, k::connection::RandomizePorts, c.randomizePorts).toBool();
    c.bindInterface = get(s, k::connection::BindInterface, QString()).toString().trimmed();
    c.ipv6 = get(s, k::connection::Ipv6, c.ipv6).toBool();
    if (!c.bindInterface.isEmpty()) {
        // [net-2] an interface name, a friendly name or an address: bind to its addresses (IPv4,
        // and IPv6 only when connection/ipv6 is on)
        const mx::netpolicy::Adapter a = mx::netpolicy::resolveAdapter(c.bindInterface);
        for (const QHostAddress &h : a.addresses)
            if (h.protocol() == QAbstractSocket::IPv4Protocol || c.ipv6) c.bindAddresses << h.toString();
    }
    c.dht = get(s, k::bt::Dht, c.dht).toBool();
    c.pex = get(s, k::bt::Pex, c.pex).toBool();
    c.lsd = get(s, k::bt::Lsd, c.lsd).toBool();
    c.utp = get(s, k::bt::Utp, c.utp).toBool();
    c.upnp = get(s, k::connection::Upnp, c.upnp).toBool();
    c.natpmp = get(s, k::connection::NatPmp, c.natpmp).toBool();
    c.encryption = get(s, k::bt::Encryption, c.encryption).toString().toLower();
    c.maxConnections = get(s, k::bt::MaxConnections, c.maxConnections).toInt();
    c.maxConnectionsPerTorrent = get(s, k::bt::MaxConnectionsPerTorrent, c.maxConnectionsPerTorrent).toInt();
    c.maxUploads = get(s, k::bt::MaxUploads, c.maxUploads).toInt();
    c.maxUploadsPerTorrent = get(s, k::bt::MaxUploadsPerTorrent, c.maxUploadsPerTorrent).toInt();
    c.maxActiveDownloads = get(s, k::bt::MaxActiveDownloads, c.maxActiveDownloads).toInt();
    c.maxActiveSeeds = get(s, k::bt::MaxActiveSeeds, c.maxActiveSeeds).toInt();
    c.seedRatioLimit = get(s, k::bt::SeedRatioLimit, c.seedRatioLimit).toDouble();
    c.seedTimeLimitMin = get(s, k::bt::SeedTimeLimitMin, c.seedTimeLimitMin).toInt();
    c.seedLimitAction = get(s, k::bt::SeedLimitAction, c.seedLimitAction).toString().toLower();
    c.seedCompleted = get(s, k::bt::SeedCompleted, c.seedCompleted).toBool();
    c.announceToAllTrackers = get(s, k::bt::AnnounceToAllTrackers, c.announceToAllTrackers).toBool();
    c.extraTrackers = get(s, k::bt::ExtraTrackers, QStringList()).toStringList();
    c.bootstrapNodes = get(s, k::bt::DhtBootstrapNodes,
                           QStringList{QStringLiteral("router.bittorrent.com:6881"),
                                       QStringLiteral("dht.transmissionbt.com:6881"),
                                       QStringLiteral("router.utorrent.com:6881"),
                                       QStringLiteral("dht.libtorrent.org:25401")}).toStringList();
    c.anonymousMode = get(s, k::bt::AnonymousMode, c.anonymousMode).toBool();
    c.superSeeding = get(s, k::bt::SuperSeeding, c.superSeeding).toBool();
    c.preallocate = get(s, k::bt::Preallocate, c.preallocate).toBool();
    c.webSeeds = get(s, k::bt::WebSeeds, c.webSeeds).toBool();
    {   // [net-2] the program's one proxy policy (SOCKS4 is used as SOCKS5; every peer goes through it)
        const mx::netpolicy::Proxy px = mx::netpolicy::fromSettings(s).proxy;
        c.proxyType = px.kind == mx::netpolicy::Proxy::Socks5 ? QStringLiteral("socks5")
                      : px.kind == mx::netpolicy::Proxy::Http ? QStringLiteral("http")
                                                              : QStringLiteral("none");
        c.proxyHost = px.host;
        c.proxyPort = px.port;
        c.proxyAuth = !px.user.isEmpty();
        c.proxyUser = px.user;
        c.proxyPassword = px.password;
        c.proxyPeers = true;
        c.proxyHostnames = px.remoteDns;
    }
    c.limitLan = get(s, k::bandwidth::LimitLan, false).toBool();
    // keys not in the schema (yet): read with fallbacks
    c.allowMultipleConnectionsPerIp = get(s, "bt/allowMultipleConnectionsPerIp", false).toBool();
    c.validateHttpsTrackers = get(s, "bt/validateHttpsTrackers", true).toBool();
    return c;
}

QVariantMap Record::toVariant() const
{
    QVariantMap m;
    m.insert(QStringLiteral("id"), id);
    if (!uri.isEmpty())
        m.insert(QStringLiteral("uri"), uri);
    m.insert(QStringLiteral("savePath"), savePath);
    if (!name.isEmpty())
        m.insert(QStringLiteral("name"), name);
    m.insert(QStringLiteral("added"), added.toUTC().toString(Qt::ISODateWithMs));
    m.insert(QStringLiteral("priority"), priority);
    if (!seededPath.isEmpty())
        m.insert(QStringLiteral("seededPath"), seededPath);
    if (seedLimitReached)
        m.insert(QStringLiteral("seedLimitReached"), true);
    return m;
}

Record Record::fromVariant(const QVariantMap &m)
{
    Record r;
    r.id = m.value(QStringLiteral("id")).toString();
    r.uri = m.value(QStringLiteral("uri")).toString();
    r.savePath = m.value(QStringLiteral("savePath")).toString();
    r.name = m.value(QStringLiteral("name")).toString();
    r.added = QDateTime::fromString(m.value(QStringLiteral("added")).toString(), Qt::ISODateWithMs);
    r.priority = m.value(QStringLiteral("priority")).toInt();
    r.seededPath = m.value(QStringLiteral("seededPath")).toString();
    r.seedLimitReached = m.value(QStringLiteral("seedLimitReached")).toBool();
    return r;
}

QString toHex(const lt::sha1_hash &h)
{
    return QString::fromLatin1(QByteArray(h.data(), int(h.size())).toHex());
}

QString hexId(const lt::info_hash_t &ih)
{
    return toHex(ih.get_best());
}

QString endpointString(const QString &host, int port)
{
    if (host.contains(QLatin1Char(':')))
        return QStringLiteral("[%1]:%2").arg(host).arg(port);
    return QStringLiteral("%1:%2").arg(host).arg(port);
}

bool parseHostPort(const QString &in, QString *host, int *port, int defaultPort)
{
    QString s = in.trimmed();
    if (s.isEmpty())
        return false;
    int p = defaultPort;
    QString h;
    if (s.startsWith(QLatin1Char('['))) {
        const int close = s.indexOf(QLatin1Char(']'));
        if (close < 0)
            return false;
        h = s.mid(1, close - 1);
        const QString rest = s.mid(close + 1);
        if (rest.startsWith(QLatin1Char(':')))
            p = rest.mid(1).toInt();
    } else if (s.count(QLatin1Char(':')) == 1) {
        h = s.section(QLatin1Char(':'), 0, 0);
        bool ok = false;
        p = s.section(QLatin1Char(':'), 1).toInt(&ok);
        if (!ok)
            return false;
    } else {
        h = s;   // bare host or bare IPv6 address
    }
    if (h.isEmpty() || p <= 0 || p > 65535)
        return false;
    *host = h;
    *port = p;
    return true;
}

// [bw-2] libtorrent's settings under a speed limit (see bt_pacing.h for the rule). Without a limit in
// either direction exactly libtorrent's defaults, as before bw-2.
// - tick_interval 100 ms (default 500): quota handed out in smaller steps;
// - download: request_queue_time 1 s of each peer's own rate (default 3), capped per connection by
//   the pacing plugin at its part of kInflightSec of the budget (max_out_request_queue itself stays
//   at its default: the peer's reqq would replace it anyway);
// - upload: send buffer (one lump of writing) kInflightSec of the budget split among the peers we
//   upload to, at least one block.
void applyRatePacing(lt::settings_pack &p, int downBps, int upBps, int upPeers)
{
    const lt::settings_pack def = lt::default_settings();
    const bool limited = downBps > 0 || upBps > 0;
    p.set_int(lt::settings_pack::tick_interval, limited ? 100 : def.get_int(lt::settings_pack::tick_interval));
    p.set_int(lt::settings_pack::request_queue_time, downBps > 0 ? 1 : def.get_int(lt::settings_pack::request_queue_time));
    p.set_int(lt::settings_pack::max_out_request_queue, def.get_int(lt::settings_pack::max_out_request_queue));
    const int sendBuf = pacedSendBuffer(upBps, upPeers, def.get_int(lt::settings_pack::send_buffer_watermark));
    p.set_int(lt::settings_pack::send_buffer_watermark, sendBuf > 0 ? sendBuf : def.get_int(lt::settings_pack::send_buffer_watermark));
}

lt::settings_pack makeSettingsPack(const Config &c, bool online, int port, const QStringList &userNodes,
                                   int downBps, int upBps)
{
    lt::settings_pack p;
    using sp = lt::settings_pack;
    // identity
    const QString ua = QStringLiteral("%1/%2 libtorrent/%3")
                           .arg(c.clientName, c.clientVersion, QString::fromLatin1(lt::version_str));
    p.set_str(sp::user_agent, ua.toStdString());
    const QStringList ver = c.clientVersion.split(QLatin1Char('.'));
    auto vpart = [&](int i) { return i < ver.size() ? qBound(0, ver.at(i).toInt(), 9) : 0; };
    p.set_str(sp::peer_fingerprint, lt::generate_fingerprint("OX", vpart(0), vpart(1), vpart(2), vpart(3)));
    p.set_str(sp::handshake_client_version, ua.toStdString());
    p.set_bool(sp::anonymous_mode, c.anonymousMode);

    p.set_int(sp::alert_mask, int(lt::alert_category::status | lt::alert_category::error
                                  | lt::alert_category::storage | lt::alert_category::piece_progress
                                  | lt::alert_category::port_mapping | lt::alert_category::tracker
                                  | lt::alert_category::performance_warning
                                  | lt::alert_category::file_progress
                                  // [diag-1] connection test counters (peer connects / failures, DHT replies)
                                  | lt::alert_category::connect | lt::alert_category::peer | lt::alert_category::dht));
    p.set_int(sp::alert_queue_size, 20000);

    // listening
    // [net-2] a chosen adapter that is missing: offline (no listen socket, no DHT, no peers)
    if (!c.bindInterface.isEmpty() && c.bindAddresses.isEmpty())
        online = false;
    // [net-2] a proxy chosen without an address: offline too (never direct)
    if (c.proxyType != QLatin1String("none") && (c.proxyHost.isEmpty() || c.proxyPort <= 0))
        online = false;
    QString ifaces;
    if (online) {
        QStringList parts;
        if (c.bindInterface.isEmpty()) {
            parts << QStringLiteral("0.0.0.0:%1").arg(port);
            if (c.ipv6)
                parts << QStringLiteral("[::]:%1").arg(port);
        } else {
            for (const QString &a : c.bindAddresses)
                parts << QStringLiteral("%1:%2").arg(a.contains(QLatin1Char(':')) ? QStringLiteral("[%1]").arg(a) : a).arg(port);
        }
        ifaces = parts.join(QLatin1Char(','));
    }
    p.set_str(sp::listen_interfaces, ifaces.toStdString());
    p.set_int(sp::max_retry_port_bind, 10);
    // outgoing connections from the adapter's addresses (an address no longer there fails: no fallback)
    p.set_str(sp::outgoing_interfaces, c.bindInterface.isEmpty() ? std::string()
                                       : (c.bindAddresses.isEmpty() ? std::string("0.0.0.1") : c.bindAddresses.join(QLatin1Char(',')).toStdString()));

    // discovery
    // [net-2] an HTTP proxy carries no UDP: no DHT then (libtorrent would only fail to send)
    p.set_bool(sp::enable_dht, online && c.dht && c.proxyType != QLatin1String("http"));
    p.set_bool(sp::enable_lsd, online && c.lsd && c.proxyType == QLatin1String("none"));   // [net-2] no LAN multicast through a proxy
    // [net-2] no port mapping while a proxy is set (nobody reaches us through it)
    const bool mapPorts = online && !c.coreMapsPorts && c.proxyType == QLatin1String("none");
    p.set_bool(sp::enable_upnp, mapPorts && c.upnp);
    p.set_bool(sp::enable_natpmp, mapPorts && c.natpmp);
    p.set_int(sp::announce_port, c.announcePort);   // [diag-1] trackers / DHT get the mapped external port
    QStringList nodes;
    const bool proxied = c.proxyType != QLatin1String("none");
    for (const QString &n : c.bootstrapNodes + userNodes) {
        QString h;
        int pt = 0;
        if (!parseHostPort(n, &h, &pt, 6881))
            continue;
        // [net-2] libtorrent looks DHT router names up itself, with the system's DNS: behind a
        // proxy only routers given by address (the DHT finds the rest through its saved nodes)
        if (proxied && QHostAddress(h).isNull())
            continue;
        nodes << QStringLiteral("%1:%2").arg(h).arg(pt);
    }
    p.set_str(sp::dht_bootstrap_nodes, nodes.join(QLatin1Char(',')).toStdString());
    p.set_bool(sp::enable_incoming_utp, c.utp && c.proxyType != QLatin1String("http"));
    p.set_bool(sp::enable_outgoing_utp, c.utp && c.proxyType != QLatin1String("http"));

    // encryption
    int pol = sp::pe_enabled;
    if (c.encryption == QLatin1String("forced"))
        pol = sp::pe_forced;
    else if (c.encryption == QLatin1String("disabled"))
        pol = sp::pe_disabled;
    p.set_int(sp::out_enc_policy, pol);
    p.set_int(sp::in_enc_policy, pol);
    // [net-2] "Require encryption" means the whole stream: RC4 only, not a plain stream after an
    // encrypted handshake (libtorrent's pe_both would let a peer pick that)
    p.set_int(sp::allowed_enc_level, c.encryption == QLatin1String("forced") ? sp::pe_rc4 : sp::pe_both);
    p.set_bool(sp::prefer_rc4, c.encryption == QLatin1String("forced"));

    // limits
    p.set_int(sp::connections_limit, qMax(10, c.maxConnections));
    p.set_int(sp::unchoke_slots_limit, qMax(1, c.maxUploads));
    p.set_int(sp::active_downloads, qMax(1, c.maxActiveDownloads));
    p.set_int(sp::active_seeds, qMax(0, c.maxActiveSeeds));
    p.set_int(sp::active_limit, qMax(500, c.maxActiveDownloads + c.maxActiveSeeds));
    p.set_bool(sp::dont_count_slow_torrents, true);
    p.set_int(sp::download_rate_limit, qMax(0, downBps));
    p.set_int(sp::upload_rate_limit, qMax(0, upBps));
    applyRatePacing(p, qMax(0, downBps), qMax(0, upBps), 1);   // [bw-2]
    // [eng-1] a limit caps payload and protocol messages, not the estimated TCP/IP overhead:
    // libtorrent's default drains the acknowledgements of a download from the UPLOAD limiter
    // (about 5 KB/s at 400 KB/s down), which ate the whole upload budget at the 10 KB/s minimum,
    // so requests waited and the download fell to a third
    p.set_bool(sp::rate_limit_ip_overhead, false);
    // seeding limits are enforced by the plugin itself (pause | remove); keep libtorrent's
    // queueing heuristics out of the way.
    p.set_int(sp::share_ratio_limit, 1000000);
    p.set_int(sp::seed_time_ratio_limit, 1000000);
    p.set_int(sp::seed_time_limit, 0x7fffffff);
    p.set_bool(sp::allow_multiple_connections_per_ip, c.allowMultipleConnectionsPerIp);
    p.set_bool(sp::announce_to_all_trackers, c.announceToAllTrackers);
    p.set_bool(sp::announce_to_all_tiers, c.announceToAllTrackers);
    p.set_bool(sp::validate_https_trackers, c.validateHttpsTrackers);
    p.set_int(sp::stop_tracker_timeout, 1);

    // proxy
    int ptype = sp::none;
    const bool auth = c.proxyAuth && !c.proxyUser.isEmpty();
    if (!c.proxyHost.isEmpty()) {
        if (c.proxyType == QLatin1String("socks5"))
            ptype = auth ? sp::socks5_pw : sp::socks5;
        else if (c.proxyType == QLatin1String("socks4"))
            ptype = sp::socks4;
        else if (c.proxyType == QLatin1String("http"))
            ptype = auth ? sp::http_pw : sp::http;
    }
    p.set_int(sp::proxy_type, ptype);
    p.set_str(sp::proxy_hostname, c.proxyHost.toStdString());
    p.set_int(sp::proxy_port, c.proxyPort);
    p.set_str(sp::proxy_username, auth ? c.proxyUser.toStdString() : std::string());
    p.set_str(sp::proxy_password, auth ? c.proxyPassword.toStdString() : std::string());
    p.set_bool(sp::proxy_peer_connections, c.proxyPeers);
    p.set_bool(sp::proxy_tracker_connections, true);
    p.set_bool(sp::proxy_hostnames, c.proxyHostnames);

    // disk
    p.set_int(sp::aio_threads, 4);
    p.set_int(sp::hashing_threads, 2);
    p.set_int(sp::file_pool_size, 200);
    p.set_int(sp::max_queued_disk_bytes, 8 * 1024 * 1024);
    p.set_int(sp::send_buffer_watermark, 1024);   // KiB
    p.set_int(sp::checking_mem_usage, 256);       // 16 KiB blocks = 4 MiB
    return p;
}

QString setupCaBundle()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("Windows ROOT certificate store (loaded by libtorrent)");
#else
    if (!qEnvironmentVariableIsEmpty("SSL_CERT_FILE"))
        return QString::fromLocal8Bit(qgetenv("SSL_CERT_FILE"));
    static const char *const bundles[] = {
        "/etc/ssl/certs/ca-certificates.crt",                 // Debian/Ubuntu/Gentoo/Arch
        "/etc/pki/tls/certs/ca-bundle.crt",                   // Fedora/RHEL
        "/etc/ssl/ca-bundle.pem",                             // openSUSE
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",  // CentOS/RHEL 7+
        "/etc/ssl/cert.pem",                                  // Alpine, macOS, BSD
        "/usr/local/share/certs/ca-root-nss.crt",             // FreeBSD
    };
    for (const char *b : bundles) {
        if (QFileInfo(QString::fromLatin1(b)).isReadable()) {
            qputenv("SSL_CERT_FILE", b);
            if (qEnvironmentVariableIsEmpty("SSL_CERT_DIR") && QFileInfo(QStringLiteral("/etc/ssl/certs")).isDir())
                qputenv("SSL_CERT_DIR", "/etc/ssl/certs");
            return QString::fromLatin1(b);
        }
    }
    return QString();
#endif
}

bool writeFileAtomic(const QString &path, const QByteArray &data, QString *error)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    f.write(data);
    if (!f.commit()) {
        if (error)
            *error = f.errorString();
        return false;
    }
    return true;
}

QByteArray readFile(const QString &path, qint64 maxSize)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.size() > maxSize)
        return QByteArray();
    return f.readAll();
}

} // namespace bt
} // namespace mx
