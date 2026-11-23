// SPDX-License-Identifier: GPL-3.0-or-later
#include "ed2k_util.h"
#include "ed2k_io.h"
#include "ed2k_opcodes.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QRandomGenerator>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>

Q_LOGGING_CATEGORY(lcEd2k, "mx.ed2k", QtInfoMsg)

namespace mx::ed2k {

QByteArray generateUserHash()
{
    QByteArray h(16, '\0');
    for (int i = 0; i < 16; ++i) h[i] = char(QRandomGenerator::system()->bounded(256));
    h[5] = char(14);
    h[14] = char(111);
    return h;
}

QByteArray md4(const QByteArray &data) { return QCryptographicHash::hash(data, QCryptographicHash::Md4); }

QVariant Config::get(const QString &key, const QVariant &def) const
{
    if (!s) return def;
    const QVariant v = s->value(key);
    if (!v.isValid() || v.isNull()) return def;
    if (v.typeId() == QMetaType::QString && v.toString().isEmpty() && def.typeId() != QMetaType::QString) return def;
    return v;
}

QString PeerDescriptor::key() const
{
    if (needsKadCallback() && userHash.size() == 16) return QStringLiteral("kad:") + toHex(userHash);
    if (lowId) return QStringLiteral("%1@%2:%3").arg(lowId).arg(ipToString(serverIp)).arg(serverPort);
    return QStringLiteral("%1:%2").arg(ipToString(ip)).arg(port);
}

QString formatPeerUri(const PeerDescriptor &d)
{
    QString s = QStringLiteral("ed2k-peer://%1:%2").arg(ipToString(d.ip)).arg(d.port);
    QStringList q;
    if (d.fileHash.size() == 16) q << QStringLiteral("file=") + toHex(d.fileHash);
    if (d.userHash.size() == 16) q << QStringLiteral("uid=") + toHex(d.userHash);
    if (d.cryptOptions) q << QStringLiteral("crypt=%1").arg(d.cryptOptions);
    if (d.lowId) q << QStringLiteral("lowid=%1").arg(d.lowId);
    if (d.serverIp && d.serverPort) q << QStringLiteral("server=%1:%2").arg(ipToString(d.serverIp)).arg(d.serverPort);
    if (d.hasBuddy()) q << QStringLiteral("buddy=%1:%2").arg(ipToString(d.buddyIp)).arg(d.buddyPort) << QStringLiteral("bid=") + toHex(d.buddyId);
    if (d.kadPort) q << QStringLiteral("kport=%1").arg(d.kadPort);
    if (d.directCallback) q << QStringLiteral("dcb=1");
    if (!q.isEmpty()) s += QLatin1Char('?') + q.join(QLatin1Char('&'));
    return s;
}

static bool parseHostPort(const QString &hp, quint32 *ip, quint16 *port)
{
    const int c = int(hp.lastIndexOf(QLatin1Char(':')));
    if (c <= 0) return false;
    bool ok1 = false, ok2 = false;
    const quint32 a = ipFromString(hp.left(c), &ok1);
    const uint p = hp.mid(c + 1).toUInt(&ok2);
    if (!ok1 || !ok2 || p == 0 || p > 65535) return false;
    *ip = a;
    *port = quint16(p);
    return true;
}

bool parsePeerUri(const QString &uri, PeerDescriptor *out)
{
    if (!uri.startsWith(QLatin1String("ed2k-peer://"), Qt::CaseInsensitive)) return false;
    QString rest = uri.mid(12);
    QString query;
    const int qm = int(rest.indexOf(QLatin1Char('?')));
    if (qm >= 0) { query = rest.mid(qm + 1); rest = rest.left(qm); }
    if (rest.endsWith(QLatin1Char('/'))) rest.chop(1);
    PeerDescriptor d;
    if (!parseHostPort(rest, &d.ip, &d.port)) return false;
    const QUrlQuery q(query);
    for (const auto &kv : q.queryItems()) {
        const QString &k = kv.first;
        const QString &v = kv.second;
        if (k == QLatin1String("file")) d.fileHash = fromHex(v, 16);
        else if (k == QLatin1String("uid")) d.userHash = fromHex(v, 16);
        else if (k == QLatin1String("crypt")) d.cryptOptions = quint8(v.toUInt());
        else if (k == QLatin1String("lowid")) d.lowId = v.toUInt();
        else if (k == QLatin1String("server")) { if (!parseHostPort(v, &d.serverIp, &d.serverPort)) { d.serverIp = 0; d.serverPort = 0; } }
        else if (k == QLatin1String("buddy")) { if (!parseHostPort(v, &d.buddyIp, &d.buddyPort)) { d.buddyIp = 0; d.buddyPort = 0; } }
        else if (k == QLatin1String("bid")) d.buddyId = fromHex(v, 16);
        else if (k == QLatin1String("kport")) d.kadPort = quint16(qMin(v.toUInt(), 65535u));
        else if (k == QLatin1String("dcb")) d.directCallback = v == QLatin1String("1");
    }
    if ((d.hasBuddy() || d.directCallback) && d.userHash.size() != 16) return false;   // callbacks are matched by user hash
    if (d.lowId && !isLowId(d.lowId)) { d.ip = d.lowId; d.lowId = 0; }   // was a HighID after all
    if (!d.isValid()) return false;
    if (out) *out = d;
    return true;
}

QList<Range> planBlocks(qint64 offset, qint64 length, qint64 fileSize)
{
    QList<Range> out;
    if (offset < 0 || length <= 0) return out;
    qint64 end = offset + length;
    if (fileSize > 0) end = qMin(end, fileSize);
    qint64 pos = offset;
    while (pos < end) {
        const qint64 partEnd = (pos / PARTSIZE + 1) * PARTSIZE;
        const qint64 e = qMin(qMin(end, partEnd), pos + EMBLOCKSIZE);
        out.append({pos, e});
        pos = e;
    }
    return out;
}

bool verifyHashset(const QByteArray &fileHash, qint64 size, const QList<QByteArray> &parts)
{
    if (fileHash.size() != 16) return false;
    for (const QByteArray &p : parts) if (p.size() != 16) return false;
    if (size < PARTSIZE) return parts.size() <= 1 && (parts.isEmpty() || parts[0] == fileHash);
    if (parts.size() != ed2kHashsetCount(size)) return false;
    QByteArray cat;
    for (const QByteArray &p : parts) cat.append(p);
    return md4(cat) == fileHash;
}

QList<Range> rangesFromParts(const QList<bool> &parts, qint64 size)
{
    QList<Range> out;
    for (int i = 0; i < parts.size(); ++i) {
        if (!parts[i]) continue;
        const qint64 a = qint64(i) * PARTSIZE;
        const qint64 b = qMin(size, a + PARTSIZE);
        if (a >= b) continue;
        if (!out.isEmpty() && out.last().second == a) out.last().second = b;
        else out.append({a, b});
    }
    return out;
}

QString formatBytes(qint64 b)
{
    if (b < 1024) return QStringLiteral("%1 B").arg(b);
    if (b < 1024 * 1024) return QStringLiteral("%1 KiB").arg(b / 1024.0, 0, 'f', 1);
    if (b < 1024LL * 1024 * 1024) return QStringLiteral("%1 MiB").arg(b / 1048576.0, 0, 'f', 1);
    return QStringLiteral("%1 GiB").arg(b / 1073741824.0, 0, 'f', 2);
}

} // namespace mx::ed2k
