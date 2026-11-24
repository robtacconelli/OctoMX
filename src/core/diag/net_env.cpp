// SPDX-License-Identifier: GPL-3.0-or-later
// [diag-1] local network environment (see net_env.h)
#include "core/diag/net_env.h"
#include "core/diag/diag_rules.h"

#include <QtCore/QFile>
#include <QtCore/QHash>
#include <climits>
#include <QtNetwork/QNetworkInterface>
#include <QtNetwork/QUdpSocket>

#ifdef Q_OS_WIN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#endif
#ifdef Q_OS_MACOS
#include <arpa/inet.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <algorithm>
#include <cstddef>
#endif

namespace mx::diag {

#ifdef Q_OS_MACOS
namespace {
// [mac-1] the IPv4 default routes from the kernel's routing table (sysctl NET_RT_FLAGS, no /proc on
// macOS): gateway (host order) and interface index, the unscoped (primary) route first
struct MacRoute { quint32 gateway = 0; int ifIndex = 0; bool scoped = false; };
QList<MacRoute> macDefaultRoutes()
{
    QList<MacRoute> out;
    int mib[6] = {CTL_NET, PF_ROUTE, 0, AF_INET, NET_RT_FLAGS, RTF_GATEWAY};
    size_t len = 0;
    if (sysctl(mib, 6, nullptr, &len, nullptr, 0) < 0 || len == 0)
        return out;
    QByteArray buf(int(len), '\0');
    if (sysctl(mib, 6, buf.data(), &len, nullptr, 0) < 0)
        return out;
    const char *end = buf.constData() + len;
    for (const char *p = buf.constData(); p + sizeof(rt_msghdr) <= end;) {
        const auto *rtm = reinterpret_cast<const rt_msghdr *>(p);
        if (rtm->rtm_msglen == 0)
            break;
        const sockaddr *addrs[RTAX_MAX] = {};
        const char *sp = p + sizeof(rt_msghdr);
        for (int i = 0; i < RTAX_MAX && sp < p + rtm->rtm_msglen; ++i) {
            if (!(rtm->rtm_addrs & (1 << i)))
                continue;
            const auto *sa = reinterpret_cast<const sockaddr *>(sp);
            addrs[i] = sa;
            // Darwin rounds each address up to 4 bytes (ROUNDUP in route.c)
            const size_t l = sa->sa_len ? ((sa->sa_len + sizeof(uint32_t) - 1) & ~(sizeof(uint32_t) - 1)) : sizeof(uint32_t);
            sp += l;
        }
        const sockaddr *dst = addrs[RTAX_DST], *gw = addrs[RTAX_GATEWAY], *mask = addrs[RTAX_NETMASK];
        const bool defaultDst = dst && dst->sa_family == AF_INET && reinterpret_cast<const sockaddr_in *>(dst)->sin_addr.s_addr == 0;
        const bool zeroMask = !mask || mask->sa_len <= offsetof(sockaddr_in, sin_addr) ||
                              reinterpret_cast<const sockaddr_in *>(mask)->sin_addr.s_addr == 0;
        if (defaultDst && zeroMask && gw && gw->sa_family == AF_INET && (rtm->rtm_flags & RTF_UP)) {
            MacRoute r;
            r.gateway = ntohl(reinterpret_cast<const sockaddr_in *>(gw)->sin_addr.s_addr);
            r.ifIndex = rtm->rtm_index;
#ifdef RTF_IFSCOPE
            r.scoped = (rtm->rtm_flags & RTF_IFSCOPE) != 0;
#endif
            if (r.gateway)
                out << r;
        }
        p += rtm->rtm_msglen;
    }
    std::stable_sort(out.begin(), out.end(), [](const MacRoute &a, const MacRoute &b) { return !a.scoped && b.scoped; });
    return out;
}
} // namespace
#endif

QHostAddress localAddressTowards(const QHostAddress &to, quint16 port)
{
    QUdpSocket s;
    s.connectToHost(to, port);   // UDP: only selects the route, no datagram is sent
    if (!s.waitForConnected(200)) return QHostAddress();
    QHostAddress a = s.localAddress();
    s.close();
    if (a.isNull() || a == QHostAddress::AnyIPv4 || a == QHostAddress::Any) return QHostAddress();
    return a;
}

QHostAddress defaultGateway()
{
#ifdef Q_OS_WIN
    DWORD idx = 0;
    MIB_IPFORWARDROW row;
    if (GetBestRoute(inet_addr("8.8.8.8"), 0, &row) == NO_ERROR && row.dwForwardNextHop != 0) {
        Q_UNUSED(idx)
        return QHostAddress(ntohl(row.dwForwardNextHop));
    }
    return QHostAddress();
#elif defined(Q_OS_MACOS)
    const QList<MacRoute> r = macDefaultRoutes();
    return r.isEmpty() ? QHostAddress() : QHostAddress(r.first().gateway);
#else
    QFile f(QStringLiteral("/proc/net/route"));
    if (!f.open(QIODevice::ReadOnly)) return QHostAddress();
    const QList<QByteArray> lines = f.readAll().split('\n');
    quint32 best = 0;
    int bestMetric = INT_MAX;
    for (int i = 1; i < lines.size(); ++i) {
        const QList<QByteArray> c = lines.at(i).simplified().split(' ');
        if (c.size() < 8) continue;
        bool ok1 = false, ok2 = false;
        const quint32 dest = c.at(1).toUInt(&ok1, 16), gw = c.at(2).toUInt(&ok2, 16);
        const int flags = c.at(3).toInt(nullptr, 16), metric = c.at(6).toInt();
        if (!ok1 || !ok2 || dest != 0 || gw == 0 || !(flags & 0x2)) continue;
        if (metric < bestMetric) {
            bestMetric = metric;
            best = gw;
        }
    }
    if (!best) return QHostAddress();
    // the kernel prints the address in host byte order of a little-endian machine
    const quint32 v = ((best & 0xFF) << 24) | ((best & 0xFF00) << 8) | ((best >> 8) & 0xFF00) | (best >> 24);
    return QHostAddress(v);
#endif
}

namespace {
struct AdapterExtra { QString description; bool hasGateway = false; };
// per interface index: description and whether it has a default gateway
QHash<int, AdapterExtra> adapterExtras()
{
    QHash<int, AdapterExtra> out;
#ifdef Q_OS_WIN
    ULONG size = 32 * 1024;
    QByteArray buf;
    for (int attempt = 0; attempt < 3; ++attempt) {
        buf.resize(int(size));
        const ULONG rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_MULTICAST,
                                              nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buf.data()), &size);
        if (rc == ERROR_BUFFER_OVERFLOW) continue;
        if (rc != NO_ERROR) return out;
        for (auto *a = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buf.data()); a; a = a->Next) {
            AdapterExtra x;
            x.description = a->Description ? QString::fromWCharArray(a->Description) : QString();
            x.hasGateway = a->FirstGatewayAddress != nullptr;
            out.insert(int(a->IfIndex), x);
        }
        break;
    }
#elif defined(Q_OS_MACOS)
    for (const MacRoute &r : macDefaultRoutes())
        out[r.ifIndex].hasGateway = true;
#else
    QFile f(QStringLiteral("/proc/net/route"));
    if (f.open(QIODevice::ReadOnly)) {
        const QList<QByteArray> lines = f.readAll().split('\n');
        for (int i = 1; i < lines.size(); ++i) {
            const QList<QByteArray> c = lines.at(i).simplified().split(' ');
            if (c.size() < 4 || c.at(1) != "00000000" || c.at(2) == "00000000") continue;
            const QNetworkInterface ni = QNetworkInterface::interfaceFromName(QString::fromLatin1(c.at(0)));
            if (ni.isValid()) out[ni.index()].hasGateway = true;
        }
    }
#endif
    return out;
}

QString typeText(QNetworkInterface::InterfaceType t)
{
    switch (t) {
    case QNetworkInterface::Wifi: return QStringLiteral("Wi-Fi");
    case QNetworkInterface::Ethernet: return QStringLiteral("Ethernet");
    case QNetworkInterface::Virtual: return QStringLiteral("virtual");
    case QNetworkInterface::Ppp: return QStringLiteral("PPP / VPN");
    case QNetworkInterface::Ieee80216: return QStringLiteral("mobile broadband");
    case QNetworkInterface::Ieee802154: case QNetworkInterface::SixLoWPAN: return QStringLiteral("low-power radio");
    default: return QString();
    }
}
} // namespace

NetEnv probeNetEnv()
{
    NetEnv env;
    const QHostAddress src = localAddressTowards(QHostAddress(QStringLiteral("8.8.8.8")), 53);
    if (!src.isNull()) env.defaultSource = src.toString();
    env.gateway = defaultGateway();
    const QHash<int, AdapterExtra> extras = adapterExtras();
    for (const QNetworkInterface &ni : QNetworkInterface::allInterfaces()) {
        const auto fl = ni.flags();
        // wine reports no flags at all: then the addresses decide
        if (fl != QNetworkInterface::InterfaceFlags() && !(fl & QNetworkInterface::IsUp)) continue;
        AdapterInfo a;
        a.name = ni.name();
        a.humanName = ni.humanReadableName();
        a.up = true;
        a.loopback = (fl & QNetworkInterface::IsLoopBack) || ni.type() == QNetworkInterface::Loopback;
        if (a.loopback) continue;
        for (const QNetworkAddressEntry &e : ni.addressEntries()) {
            const QHostAddress ip = e.ip();
            if (ip.isLoopback()) continue;
            if (ip.protocol() == QAbstractSocket::IPv4Protocol) {
                a.ipv4 << ip.toString();
                if (ip == src) a.isDefault = true;
            } else if (ip.protocol() == QAbstractSocket::IPv6Protocol && !ip.isLinkLocal()) {
                QHostAddress plain(ip);
                plain.setScopeId(QString());
                a.ipv6 << plain.toString();
                if (!ip.isUniqueLocalUnicast() && !ip.isLoopback()) env.hasIpv6 = true;
            }
        }
        if (a.ipv4.isEmpty() && a.ipv6.isEmpty()) continue;
        const AdapterExtra ex = extras.value(ni.index());
        a.description = ex.description == a.humanName ? QString() : ex.description;
        a.hasGateway = ex.hasGateway;
        a.typeName = typeText(ni.type());
        QString why;
        a.virtualOrVpn = looksVirtualAdapter(a.name, a.humanName + QLatin1Char(' ') + a.description, &why)
                         || ni.type() == QNetworkInterface::Virtual || ni.type() == QNetworkInterface::Ppp;
        if (a.virtualOrVpn) a.kindNote = why.isEmpty() ? (ni.type() == QNetworkInterface::Ppp ? QStringLiteral("VPN") : QStringLiteral("virtual")) : why;
        // not clearly virtual, but no way to the internet: say exactly that
        if (!a.virtualOrVpn && !a.hasGateway && !a.ipv4.contains(src.toString())) a.kindNote = QStringLiteral("no internet route");
        if (a.isDefault) {
            env.defaultAdapter = a.humanName;
            env.defaultIsVpn = a.virtualOrVpn && a.kindNote == QLatin1String("VPN");
        }
        env.adapters << a;
    }
    return env;
}

} // namespace mx::diag
