// SPDX-License-Identifier: GPL-3.0-or-later
#include "gnut_util.h"

#include "mx/settings_keys.h"

#include <QtCore/QRandomGenerator>
#include <QtCore/QUrl>

namespace mx::gnut {

Q_LOGGING_CATEGORY(lcGnut, "mx.gnutella", QtInfoMsg)

QByteArray randomGuid(bool modern)
{
    QByteArray g(GuidSize, Qt::Uninitialized);
    QRandomGenerator *r = QRandomGenerator::global();
    for (int i = 0; i < GuidSize; ++i) g[i] = char(r->bounded(256));
    if (modern) {
        g[8] = char(0xff);
        g[15] = 0;
    }
    return g;
}

QString guidHex(const QByteArray &g) { return QString::fromLatin1(g.toHex()).toUpper(); }

QByteArray guidFromHex(const QString &s)
{
    if (s.size() != 32) return {};
    for (QChar c : s)
        if (!c.isDigit() && !(c.toLower() >= QLatin1Char('a') && c.toLower() <= QLatin1Char('f'))) return {};
    return QByteArray::fromHex(s.toLatin1());
}

quint8 Reader::u8()
{
    if (!m_ok || m_pos + 1 > m_b.size()) { m_ok = false; return 0; }
    return quint8(m_b[m_pos++]);
}
quint16 Reader::u16()
{
    if (!m_ok || m_pos + 2 > m_b.size()) { m_ok = false; return 0; }
    const quint16 v = quint16(quint8(m_b[m_pos]) | (quint8(m_b[m_pos + 1]) << 8));
    m_pos += 2;
    return v;
}
quint32 Reader::u32()
{
    if (!m_ok || m_pos + 4 > m_b.size()) { m_ok = false; return 0; }
    quint32 v = 0;
    for (int i = 3; i >= 0; --i) v = (v << 8) | quint8(m_b[m_pos + i]);
    m_pos += 4;
    return v;
}
quint32 Reader::ip()
{
    if (!m_ok || m_pos + 4 > m_b.size()) { m_ok = false; return 0; }
    quint32 v = 0;
    for (int i = 0; i < 4; ++i) v = (v << 8) | quint8(m_b[m_pos + i]);
    m_pos += 4;
    return v;
}
QByteArray Reader::bytes(int n)
{
    if (!m_ok || n < 0 || m_pos + qint64(n) > m_b.size()) { m_ok = false; return {}; }
    QByteArray r = m_b.mid(m_pos, n);
    m_pos += n;
    return r;
}
QByteArray Reader::cstr(int maxLen)
{
    if (!m_ok) return {};
    const int end = int(m_b.indexOf('\0', m_pos));
    if (end < 0 || end - m_pos > maxLen) { m_ok = false; return {}; }
    QByteArray r = m_b.mid(m_pos, end - m_pos);
    m_pos = end + 1;
    return r;
}

QString ipToString(quint32 ip)
{
    return QStringLiteral("%1.%2.%3.%4").arg(ip >> 24).arg((ip >> 16) & 0xff).arg((ip >> 8) & 0xff).arg(ip & 0xff);
}

quint32 ipFromString(const QString &s, bool *ok)
{
    const QStringList p = s.trimmed().split(QLatin1Char('.'));
    quint32 ip = 0;
    bool good = p.size() == 4;
    for (int i = 0; good && i < 4; ++i) {
        bool o = false;
        const uint v = p[i].toUInt(&o);
        if (!o || v > 255 || p[i].isEmpty() || p[i].size() > 3) good = false;
        ip = (ip << 8) | v;
    }
    if (ok) *ok = good;
    return good ? ip : 0;
}

bool isPrivateIp(quint32 ip)
{
    const quint8 a = quint8(ip >> 24), b = quint8(ip >> 16);
    return a == 10 || a == 127 || (a == 172 && (b & 0xf0) == 16) || (a == 192 && b == 168) ||
           (a == 169 && b == 254) || (a == 100 && (b & 0xc0) == 64);
}

bool isBogonIp(quint32 ip)
{
    const quint8 a = quint8(ip >> 24);
    return a == 0 || a >= 224 || ip == 0xffffffffu;
}

bool isRoutableIp(quint32 ip) { return !isPrivateIp(ip) && !isBogonIp(ip); }

QString HostPort::toString() const { return ipToString(ip) + QLatin1Char(':') + QString::number(port); }

HostPort HostPort::parse(const QString &s, quint16 defaultPort)
{
    HostPort h;
    QString t = s.trimmed();
    quint16 port = defaultPort;
    const int c = int(t.indexOf(QLatin1Char(':')));
    if (c >= 0) {
        bool ok = false;
        const uint p = t.mid(c + 1).trimmed().toUInt(&ok);
        if (!ok || p == 0 || p > 65535) return {};
        port = quint16(p);
        t = t.left(c);
    }
    bool ok = false;
    const quint32 ip = ipFromString(t, &ok);
    if (!ok) return {};
    h.ip = ip;
    h.port = port;
    return h;
}

QList<HostPort> unpackHosts6(const QByteArray &data, int max)
{
    QList<HostPort> out;
    for (int i = 0; i + 6 <= data.size() && out.size() < max; i += 6) {
        HostPort h;
        h.ip = (quint32(quint8(data[i])) << 24) | (quint32(quint8(data[i + 1])) << 16) |
               (quint32(quint8(data[i + 2])) << 8) | quint32(quint8(data[i + 3]));
        h.port = quint16(quint8(data[i + 4]) | (quint8(data[i + 5]) << 8));
        if (h.isValid()) out << h;
    }
    return out;
}

QByteArray packHost6(const HostPort &h)
{
    QByteArray b;
    putIp(b, h.ip);
    putU16(b, h.port);
    return b;
}

QStringList splitKeywords(const QString &text)
{
    QStringList out;
    QString cur;
    const QList<uint> ucs = text.toUcs4();
    for (uint c : ucs) {
        if (QChar::isLetterOrNumber(c)) {
            const uint l = QChar::toLower(c);
            cur += QString::fromUcs4(reinterpret_cast<const char32_t *>(&l), 1);
        } else if (!cur.isEmpty()) {
            out << cur;
            cur.clear();
        }
    }
    if (!cur.isEmpty()) out << cur;
    return out;
}

namespace {
// representative line speeds (kbit/s) of the wired connection types, slowest first; a speed field
// maps to the nearest one (thresholds halfway on a log-ish scale)
struct SpeedClass { const char *label; quint32 kbps; quint32 upTo; };
const SpeedClass kSpeedClasses[] = {
    {"DSL", 16000, 30000},            // dial-up, ISDN, ADSL (and the old 56K / T1 / "Cable" constants)
    {"VDSL", 50000, 75000},
    {"FTTC", 100000, 175000},
    {"Cable", 250000, 375000},
    {"FTTB", 500000, 750000},
    {"FTTH", 1000000, 1750000},
    {"2.5 Gbit", 2500000, 3750000},
    {"5 Gbit", 5000000, 7500000},
    {"10 Gbit+", 10000000, 0xFFFFFFFFu},
};
} // namespace

QString speedLabel(quint32 kbps)
{
    if (kbps == 0) return QStringLiteral("Unknown");
    for (const SpeedClass &c : kSpeedClasses)
        if (kbps <= c.upTo) return QString::fromLatin1(c.label);
    return QStringLiteral("10 Gbit+");
}

quint32 speedForConnection(const QString &connectionType)
{
    const QString t = mx::migrateConnectionType(connectionType);
    for (const SpeedClass &c : kSpeedClasses)
        if (t == QLatin1String(c.label)) return c.kbps;
    if (t == QLatin1String("4G")) return 30000;          // mobile / satellite: their typical rate
    if (t == QLatin1String("5G")) return 300000;
    if (t == QLatin1String("Satellite")) return 50000;
    return 1000;                                         // Unknown: the classic "broadband" value
}

QString vendorName(const QByteArray &code)
{
    static const QHash<QByteArray, QString> names = {
        {"LIME", QStringLiteral("LimeWire")}, {"GTKG", QStringLiteral("gtk-gnutella")},
        {"BEAR", QStringLiteral("BearShare")}, {"RAZA", QStringLiteral("Shareaza")},
        {"PHEX", QStringLiteral("Phex")}, {"MRPH", QStringLiteral("Morpheus")},
        {"GNUC", QStringLiteral("Gnucleus")}, {"SWAP", QStringLiteral("Swapper")},
        {"MUTE", QStringLiteral("Mutella")}, {"MNAP", QStringLiteral("MyNapster")},
        {"FRST", QStringLiteral("FrostWire")}, {"WSHR", QStringLiteral("WireShare")},
        {"TURB", QStringLiteral("TurboWire")}, {"CULT", QStringLiteral("Cultiv8r")},
        {"OCMX", QStringLiteral("OctoMX")}, {"WMXN", QStringLiteral("OctoMX")}, {"GDNA", QStringLiteral("GnucDNA")},
        {"HSLG", QStringLiteral("Hagelslag")}, {"LIMX", QStringLiteral("LimeWire")},
    };
    const QString n = names.value(code.toUpper());
    if (!n.isEmpty()) return n;
    QString s;
    for (char c : code) s += (c >= 0x20 && c < 0x7f) ? QChar::fromLatin1(c) : QLatin1Char('?');
    return s;
}

QByteArray urlEncodePath(const QString &s) { return QUrl::toPercentEncoding(s); }

} // namespace mx::gnut
