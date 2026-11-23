// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/server_list.h"

#include <QtCore/QRegularExpression>
#include <QtCore/QStringList>

namespace mx {

namespace {

struct Reader {
    const QByteArray &d;
    int pos = 0;
    bool ok = true;
    bool need(int n)
    {
        if (!ok || n < 0 || pos + n > d.size()) ok = false;
        return ok;
    }
    quint32 u(int n)
    {
        if (!need(n)) return 0;
        quint32 v = 0;
        for (int i = n - 1; i >= 0; --i) v = (v << 8) | quint8(d.at(pos + i));   // little endian
        pos += n;
        return v;
    }
    QByteArray bytes(int n)
    {
        if (!need(n)) return {};
        const QByteArray b = d.mid(pos, n);
        pos += n;
        return b;
    }
};

// Reads one eMule tag; returns false on malformed data. *nameId = tag id (0 if a long name),
// *str = string value if it is a string tag.
bool readTag(Reader &r, quint8 *nameId, QString *str, bool *isString)
{
    quint8 type = quint8(r.u(1));
    *nameId = 0;
    *isString = false;
    if (type & 0x80) {
        type &= 0x7F;
        *nameId = quint8(r.u(1));
    } else {
        const int len = int(r.u(2));
        const QByteArray n = r.bytes(len);
        if (len == 1) *nameId = quint8(n.at(0));
    }
    if (!r.ok) return false;
    switch (type) {
    case 0x01: r.bytes(16); break;                         // hash
    case 0x02: {                                           // string
        const int len = int(r.u(2));
        *str = QString::fromUtf8(r.bytes(len));
        *isString = true;
        break;
    }
    case 0x03: r.u(4); break;                              // uint32
    case 0x04: r.u(4); break;                              // float
    case 0x05: r.u(1); break;                              // bool
    case 0x06: { const int n = int(r.u(2)); r.bytes((n + 7) / 8); break; } // bool array
    case 0x07: { const int n = int(r.u(4)); if (n > (1 << 20)) return false; r.bytes(n); break; } // blob
    case 0x08: r.u(2); break;                              // uint16
    case 0x09: r.u(1); break;                              // uint8
    case 0x0A: return false;                               // bsob: not used in server.met
    case 0x0B: r.u(4); r.u(4); break;                      // uint64
    default:
        if (type >= 0x11 && type <= 0x20) {                // compact string of length type-0x10
            *str = QString::fromUtf8(r.bytes(type - 0x10));
            *isString = true;
            break;
        }
        return false;
    }
    return r.ok;
}

} // namespace

bool parseServerMetEntries(const QByteArray &data, QList<ServerListEntry> *out)
{
    if (data.size() < 5) return false;
    const quint8 hdr = quint8(data.at(0));
    if (hdr != 0x0E && hdr != 0xE0) return false;
    Reader r{data, 1};
    const quint32 count = r.u(4);
    if (!r.ok || count > 100000) return false;
    QList<ServerListEntry> list;
    for (quint32 i = 0; i < count && r.ok; ++i) {
        const quint32 ip = r.u(4);
        const quint16 port = quint16(r.u(2));
        const quint32 tags = r.u(4);
        if (!r.ok || tags > 1000) return false;
        ServerListEntry e;
        QString dynHost;
        for (quint32 t = 0; t < tags; ++t) {
            quint8 id = 0;
            QString str;
            bool isString = false;
            if (!readTag(r, &id, &str, &isString)) return false;
            if (id == 0x01 && isString) e.name = str.trimmed();       // ST_SERVERNAME
            if (id == 0x85 && isString) dynHost = str.trimmed();      // ST_DYNIP
        }
        // ip is stored in network byte order read as little endian: a.b.c.d = bytes 0..3
        const QString host = !dynHost.isEmpty() ? dynHost
                                                : QStringLiteral("%1.%2.%3.%4").arg(ip & 0xFF).arg((ip >> 8) & 0xFF)
                                                      .arg((ip >> 16) & 0xFF).arg((ip >> 24) & 0xFF);
        if (port == 0 || (ip == 0 && dynHost.isEmpty())) continue;
        e.address = host + QLatin1Char(':') + QString::number(port);
        list << e;
    }
    if (!r.ok) return false;
    *out = list;
    return true;
}

QList<ServerListEntry> parseServerListFile(const QByteArray &data, QString *error)
{
    QList<ServerListEntry> out;
    if (parseServerMetEntries(data, &out)) return out;
    out.clear();
    if (!data.isEmpty() && (quint8(data.at(0)) == 0x0E || quint8(data.at(0)) == 0xE0)) {
        if (error) *error = QStringLiteral("damaged server.met");
        return {};
    }
    // text: reject binary garbage
    int ctrl = 0;
    for (char c : data.left(4096)) if (c == 0 || (quint8(c) < 0x09)) ++ctrl;
    if (ctrl > 0) {
        if (error) *error = QStringLiteral("not a server list");
        return {};
    }
    static const QRegularExpression link(QStringLiteral("ed2k://\\|server\\|([^|]+)\\|(\\d+)\\|"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression hp(QStringLiteral("^\\s*([A-Za-z0-9.\\-]+|\\[[0-9a-fA-F:]+\\]):(\\d{1,5})(?:\\s+(.*))?$"));
    for (const QString &raw : QString::fromUtf8(data).split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts)) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')) || line.startsWith(QLatin1Char(';'))) continue;
        auto it = link.globalMatch(line);
        bool any = false;
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            const int port = m.captured(2).toInt();
            if (port <= 0 || port > 65535) continue;
            out << ServerListEntry{m.captured(1) + QLatin1Char(':') + m.captured(2), QString()};
            any = true;
        }
        if (any) continue;
        const QRegularExpressionMatch m = hp.match(line);
        if (!m.hasMatch()) continue;
        const int port = m.captured(2).toInt();
        if (port <= 0 || port > 65535) continue;
        out << ServerListEntry{m.captured(1) + QLatin1Char(':') + m.captured(2), m.captured(3).trimmed()};
    }
    if (out.isEmpty() && error) *error = QStringLiteral("no servers found");
    return out;
}

} // namespace mx
