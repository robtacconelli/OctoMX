// SPDX-License-Identifier: GPL-3.0-or-later
#include "ed2k_servermet.h"
#include "ed2k_io.h"
#include "ed2k_opcodes.h"
#include "ed2k_tags.h"

#include <QtCore/QRegularExpression>
#include <zlib.h>

namespace mx::ed2k {

QString ServerInfo::address() const
{
    if (ip == 0 && !dynHost.isEmpty()) return QStringLiteral("%1:%2").arg(dynHost).arg(port);
    return QStringLiteral("%1:%2").arg(ipToString(ip)).arg(port);
}

bool parseServerMet(const QByteArray &data, QList<ServerInfo> *out, QString *error)
{
    Reader r(data);
    const quint8 hdr = r.u8();
    if (!r.ok() || (hdr != 0x0E && hdr != 0xE0)) {
        if (error) *error = QStringLiteral("not a server.met (header 0x%1)").arg(hdr, 2, 16, QLatin1Char('0'));
        return false;
    }
    const quint32 count = r.u32();
    if (!r.ok() || count > 100000) { if (error) *error = QStringLiteral("bad server count"); return false; }
    QList<ServerInfo> list;
    for (quint32 i = 0; i < count && r.ok(); ++i) {
        ServerInfo s;
        s.ip = r.u32();
        s.port = r.u16();
        TagList tags;
        if (!readTagList(r, &tags, 512)) break;
        for (const Tag &t : tags) {
            if (t.name.isEmpty()) {
                switch (t.id) {
                case ST_SERVERNAME: if (t.isStr()) s.name = t.str.left(256); break;
                case ST_DESCRIPTION: if (t.isStr()) s.description = t.str.left(512); break;
                case ST_PING: s.ping = quint32(t.num); break;
                case ST_FAIL: s.failCount = quint32(t.num); break;
                case ST_PREFERENCE: s.preference = quint32(t.num); break;
                case ST_DYNIP: if (t.isStr()) s.dynHost = t.str.left(256); break;
                case ST_MAXUSERS: s.maxUsers = quint32(t.num); break;
                case ST_SOFTFILES: s.softFiles = quint32(t.num); break;
                case ST_HARDFILES: s.hardFiles = quint32(t.num); break;
                case ST_LASTPING: s.lastPingTime = quint32(t.num); break;
                case ST_VERSION:
                    if (t.isStr()) s.version = t.str.left(64);
                    else if (t.isInt()) s.version = QStringLiteral("%1.%2").arg((t.num >> 16) & 0xFFFF).arg(t.num & 0xFFFF);
                    break;
                case ST_UDPFLAGS: s.udpFlags = quint32(t.num); break;
                case ST_LOWIDUSERS: s.lowIdUsers = quint32(t.num); break;
                case ST_UDPKEY: s.udpKey = quint32(t.num); break;
                case ST_UDPKEYIP: s.udpKeyIp = quint32(t.num); break;
                case ST_TCPPORTOBFUSCATION: s.obfuscationTcpPort = quint16(t.num); break;
                case ST_UDPPORTOBFUSCATION: s.obfuscationUdpPort = quint16(t.num); break;
                default: break;
                }
            } else if (t.is("users")) { s.users = quint32(t.num); }
            else if (t.is("files")) { s.files = quint32(t.num); }
            else if (t.is("mxstatic")) { s.isStatic = t.num != 0; }
        }
        if (s.port == 0 || (s.ip == 0 && s.dynHost.isEmpty())) continue;
        list.append(s);
    }
    if (list.isEmpty() && count > 0) { if (error) *error = QStringLiteral("no valid server entries"); return false; }
    if (out) *out = list;
    return true;
}

QByteArray writeServerMet(const QList<ServerInfo> &servers)
{
    Writer w;
    w.u8(0xE0).u32(quint32(servers.size()));
    for (const ServerInfo &s : servers) {
        w.u32(s.ip).u16(s.port);
        TagList tags;
        if (!s.name.isEmpty()) tags << Tag::makeStr(ST_SERVERNAME, s.name);
        if (!s.description.isEmpty()) tags << Tag::makeStr(ST_DESCRIPTION, s.description);
        if (!s.dynHost.isEmpty()) tags << Tag::makeStr(ST_DYNIP, s.dynHost);
        if (!s.version.isEmpty()) tags << Tag::makeStr(ST_VERSION, s.version);
        tags << Tag::makeInt(ST_FAIL, s.failCount);
        tags << Tag::makeInt(ST_PREFERENCE, s.preference);
        if (s.ping) tags << Tag::makeInt(ST_PING, s.ping);
        if (s.lastPingTime) tags << Tag::makeInt(ST_LASTPING, s.lastPingTime);
        if (s.maxUsers) tags << Tag::makeInt(ST_MAXUSERS, s.maxUsers);
        if (s.softFiles) tags << Tag::makeInt(ST_SOFTFILES, s.softFiles);
        if (s.hardFiles) tags << Tag::makeInt(ST_HARDFILES, s.hardFiles);
        if (s.udpFlags) tags << Tag::makeInt(ST_UDPFLAGS, s.udpFlags);
        if (s.lowIdUsers) tags << Tag::makeInt(ST_LOWIDUSERS, s.lowIdUsers);
        if (s.obfuscationTcpPort) tags << Tag::makeInt(ST_TCPPORTOBFUSCATION, s.obfuscationTcpPort);
        if (s.obfuscationUdpPort) tags << Tag::makeInt(ST_UDPPORTOBFUSCATION, s.obfuscationUdpPort);
        if (s.udpKey) tags << Tag::makeInt(ST_UDPKEY, s.udpKey);
        if (s.udpKeyIp) tags << Tag::makeInt(ST_UDPKEYIP, s.udpKeyIp);
        if (s.users) tags << Tag::makeInt(QByteArray("users"), s.users);
        if (s.files) tags << Tag::makeInt(QByteArray("files"), s.files);
        if (s.isStatic) tags << Tag::makeInt(QByteArray("mxstatic"), 1);
        writeTagList(w, tags, false);
    }
    return w.take();
}

static bool gunzip(const QByteArray &in, QByteArray *out)
{
    z_stream zs{};
    if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) return false;
    zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(in.constData()));
    zs.avail_in = uInt(in.size());
    QByteArray res;
    char buf[16384];
    int rc = Z_OK;
    while (rc != Z_STREAM_END) {
        zs.next_out = reinterpret_cast<Bytef *>(buf);
        zs.avail_out = sizeof(buf);
        rc = inflate(&zs, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) { inflateEnd(&zs); return false; }
        res.append(buf, qsizetype(sizeof(buf) - zs.avail_out));
        if (res.size() > 8 * 1024 * 1024) { inflateEnd(&zs); return false; }
        if (rc == Z_OK && zs.avail_in == 0 && zs.avail_out != 0) break;
    }
    inflateEnd(&zs);
    *out = res;
    return true;
}

bool parseServerMetDownload(const QByteArray &data, QList<ServerInfo> *out, QString *error)
{
    if (data.size() >= 2 && quint8(data[0]) == 0x1F && quint8(data[1]) == 0x8B) {
        QByteArray plain;
        if (!gunzip(data, &plain)) { if (error) *error = QStringLiteral("corrupt gzip"); return false; }
        return parseServerMet(plain, out, error);
    }
    return parseServerMet(data, out, error);
}

bool isKnownBadServer(const ServerInfo &s)
{
    // Families of fake / spy / spam servers that have circulated in public lists. They answer
    // searches with junk and never give sources. Matching by name is a heuristic; trusted list
    // membership (serverTrustScore) wins in ordering anyway.
    static const QRegularExpression bad(QStringLiteral(
        "^(astra[- ]?\\d|akteon|pentium pilat|da na hui|poor-eserver|gaal\\b|don pedro|holy donkey|"
        "edonkey server (no)?\\s?\\d|sharing kingdom|master server|stormshare|"
        "byte ?devils|sexy-?server|\\!-= porn pit)"), QRegularExpression::CaseInsensitiveOption);
    if (bad.match(s.name.trimmed()).hasMatch()) return true;
    if (s.port == 0) return true;
    if (s.ip && !isRoutableIp(s.ip)) return true;
    return false;
}

int serverTrustScore(const ServerInfo &s)
{
    // Long-running servers listed by the eMule Security project (verified 2026-11 from
    // upd.emule-security.org/server.met). Matched by address, names are not trusted.
    static const char *const good[] = {
        "176.123.5.89:4725",     // eMule Sunrise
        "176.125.242.230:5555",  // emule-qt.org eNode
        "176.116.0.51:6555",     // emule-qt.org BiG BanG eNode
        "85.17.116.222:6082",    // ed2k-rust
        "91.208.162.87:4232",    // Sharing-Devils No.4
        "85.121.5.137:4232",     // Sharing-Devils No.2
        "91.208.162.182:4232",   // MO-Server
        "213.141.198.207:4232",  // Mazinga
        "77.42.68.79:4232",      // Nordic
        "176.116.0.62:7555",     // Razorback 4
        "57.131.35.107:4232",    // MO-ad-free
        "212.95.35.240:4232",    // eMule-Cosmic
        "91.208.162.55:4235",    // Sharing-Devils No.3
        "57.128.161.24:6666",    // Cyberchrist
        "146.19.248.158:4325",   // Sharing-Devils No.1
        "82.192.72.186:4661",    // Drunken Donkey
        "143.246.192.192:4232",  // eMule-Nordic
        "85.137.248.210:56612",  // MO-Tardis
        "91.126.170.253:5687",   // Emule Sentinel
    };
    const QString a = QStringLiteral("%1:%2").arg(ipToString(s.ip)).arg(s.port);
    for (const char *g : good)
        if (a == QLatin1String(g)) return 100;
    return isKnownBadServer(s) ? -1000 : 0;
}

} // namespace mx::ed2k
