// SPDX-License-Identifier: GPL-3.0-or-later
#include "ovl_util.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonDocument>
#include <QtCore/QRandomGenerator>
#include <QtCore/QRegularExpression>
#include <QtCore/QtEndian>

#include <zlib.h>

Q_LOGGING_CATEGORY(lcOverlay, "mx.overlay")
Q_LOGGING_CATEGORY(lcRats, "mx.rats")

namespace mx::ovl {

QString jStr(const QJsonObject &o, const char *key, int maxLen)
{
    const QJsonValue v = o.value(QLatin1String(key));
    if (!v.isString()) return QString();
    QString s = v.toString();
    if (s.size() > maxLen) s.truncate(maxLen);
    return s;
}

qint64 jInt(const QJsonObject &o, const char *key, qint64 def)
{
    const QJsonValue v = o.value(QLatin1String(key));
    if (!v.isDouble()) return def;
    const double d = v.toDouble();
    if (!(d == d) || d > 9.0e18 || d < -9.0e18) return def;   // NaN / out of range
    return static_cast<qint64>(d);
}

bool jBool(const QJsonObject &o, const char *key, bool def)
{
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isBool() ? v.toBool() : def;
}

QJsonObject jObj(const QJsonObject &o, const char *key)
{
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isObject() ? v.toObject() : QJsonObject();
}

QJsonArray jArr(const QJsonObject &o, const char *key, int maxItems)
{
    const QJsonValue v = o.value(QLatin1String(key));
    if (!v.isArray()) return QJsonArray();
    QJsonArray a = v.toArray();
    while (a.size() > maxItems) a.removeLast();
    return a;
}

bool isHex(const QString &s, int len)
{
    if (len >= 0 && s.size() != len) return false;
    if (s.isEmpty()) return false;
    for (QChar c : s) {
        const ushort u = c.unicode();
        if (!((u >= '0' && u <= '9') || (u >= 'a' && u <= 'f') || (u >= 'A' && u <= 'F'))) return false;
    }
    return true;
}

namespace {
struct HashKind { const char *key; QByteArray FileHashes::*field; int bytes; };
const HashKind kHashKinds[] = {
    {"btih", &FileHashes::btih, 20},     {"btFileRoot", &FileHashes::btFileRoot, 32},
    {"ed2k", &FileHashes::ed2k, 16},     {"aich", &FileHashes::aich, 20},
    {"sha1", &FileHashes::sha1, 20},     {"tth", &FileHashes::tth, 24},
    {"sha256", &FileHashes::sha256, 32}, {"md5", &FileHashes::md5, 16},
};
} // namespace

QJsonObject hashesToJson(const FileHashes &h)
{
    QJsonObject o;
    for (const auto &k : kHashKinds) {
        const QByteArray &v = h.*(k.field);
        if (v.size() == k.bytes) o.insert(QLatin1String(k.key), QString::fromLatin1(v.toHex()));
    }
    if (!h.btih.isEmpty() && h.btFileIndex >= 0) o.insert(QStringLiteral("btFileIndex"), h.btFileIndex);
    return o;
}

FileHashes hashesFromJson(const QJsonObject &o)
{
    FileHashes h;
    for (const auto &k : kHashKinds) {
        const QString s = jStr(o, k.key, 128);
        if (isHex(s, k.bytes * 2)) h.*(k.field) = QByteArray::fromHex(s.toLatin1());
    }
    const qint64 idx = jInt(o, "btFileIndex", -1);
    if (!h.btih.isEmpty() && idx >= 0 && idx < 1000000) h.btFileIndex = static_cast<int>(idx);
    return h;
}

QByteArray encodeControl(const QJsonObject &o, bool allowCompress)
{
    const QByteArray json = QJsonDocument(o).toJson(QJsonDocument::Compact);
    if (allowCompress && json.size() > 2048) {
        uLongf cap = compressBound(static_cast<uLong>(json.size()));
        QByteArray out(5 + static_cast<int>(cap), Qt::Uninitialized);
        out[0] = 'Z';
        qToBigEndian<quint32>(static_cast<quint32>(json.size()), out.data() + 1);
        if (compress2(reinterpret_cast<Bytef *>(out.data() + 5), &cap,
                      reinterpret_cast<const Bytef *>(json.constData()), static_cast<uLong>(json.size()),
                      Z_DEFAULT_COMPRESSION) == Z_OK) {
            out.resize(5 + static_cast<int>(cap));
            if (out.size() < json.size()) return out;
        }
    }
    QByteArray out;
    out.reserve(json.size() + 1);
    out.append('J');
    out.append(json);
    return out;
}

bool decodeControl(const QByteArray &bytes, QJsonObject *out, int maxInflated)
{
    if (bytes.size() < 3) return false;
    QByteArray json;
    if (bytes[0] == 'J') {
        if (bytes.size() - 1 > maxInflated) return false;
        json = bytes.mid(1);
    } else if (bytes[0] == 'Z') {
        if (bytes.size() < 6) return false;
        const quint32 len = qFromBigEndian<quint32>(bytes.constData() + 1);
        if (len == 0 || len > static_cast<quint32>(maxInflated)) return false;
        json.resize(static_cast<int>(len));
        uLongf dlen = len;
        if (uncompress(reinterpret_cast<Bytef *>(json.data()), &dlen,
                       reinterpret_cast<const Bytef *>(bytes.constData() + 5),
                       static_cast<uLong>(bytes.size() - 5)) != Z_OK || dlen != len)
            return false;
    } else {
        return false;
    }
    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) return false;
    *out = doc.object();
    return true;
}

QString normaliseRoomName(const QString &name)
{
    QString s = cleanText(name, 64).simplified().toLower();
    return s;
}

QString cleanText(const QString &s, int maxLen)
{
    QString out;
    out.reserve(qMin<qsizetype>(s.size(), maxLen));
    for (QChar c : s) {
        if (out.size() >= maxLen) break;
        const ushort u = c.unicode();
        if (u < 0x20 && u != '\t') continue;
        if (u == 0x7f || (u >= 0x202a && u <= 0x202e) || (u >= 0x2066 && u <= 0x2069)) continue; // bidi tricks
        out.append(c);
    }
    return out;
}

QString cleanName(const QString &s)
{
    QString n = cleanText(s, 32).trimmed();
    n.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral("_"));
    if (n.isEmpty()) n = QStringLiteral("Guest");
    return n;
}

QString idSuffix(const QString &userIdHex)
{
    const QByteArray h = QCryptographicHash::hash(userIdHex.toLatin1(), QCryptographicHash::Sha1);
    const quint32 v = qFromBigEndian<quint32>(h.constData());
    return QStringLiteral("%1").arg(v % 100000, 5, 10, QLatin1Char('0'));
}

QString randomId()
{
    quint64 a = QRandomGenerator::system()->generate64();
    return QStringLiteral("%1").arg(a, 16, 16, QLatin1Char('0'));
}

QStringList queryWords(const QString &q)
{
    QStringList out;
    static const QRegularExpression sep(QStringLiteral("[\\s\\.\\-_,;:!\\?\\(\\)\\[\\]\\{\\}\"'/\\\\+&]+"));
    for (const QString &w : q.toLower().split(sep, Qt::SkipEmptyParts))
        if (!out.contains(w)) out << w;
    return out;
}

bool matchesWords(const QString &text, const QStringList &words)
{
    if (words.isEmpty()) return false;
    const QString t = text.toLower();
    for (const QString &w : words)
        if (!t.contains(w)) return false;
    return true;
}

bool looksAdult(const QString &name)
{
    static const QStringList bad = {QStringLiteral("xxx"), QStringLiteral("porn"), QStringLiteral("sex"),
                                    QStringLiteral("hentai"), QStringLiteral("milf"), QStringLiteral("nude"),
                                    QStringLiteral("brazzers"), QStringLiteral("onlyfans"), QStringLiteral("erotic"),
                                    QStringLiteral("fuck"), QStringLiteral("anal"), QStringLiteral("blowjob"),
                                    QStringLiteral("pussy"), QStringLiteral("cock"), QStringLiteral("tits"),
                                    QStringLiteral("18+"), QStringLiteral("nsfw"), QStringLiteral("playboy")};
    const QStringList words = queryWords(name);
    const QString low = name.toLower();
    for (const QString &b : bad) {
        if (b.size() <= 4) {
            if (words.contains(b)) return true;   // whole word only ("sex" but not "sussex")
        } else if (low.contains(b)) {
            return true;
        }
    }
    return false;
}

bool RateGate::allow(const QString &key, double cost)
{
    const qint64 now = nowMs();
    auto it = m_b.find(key);
    if (it == m_b.end()) it = m_b.insert(key, B{m_burst, now});
    B &b = it.value();
    b.tokens = qMin(m_burst, b.tokens + (now - b.last) / 1000.0 * m_rate);
    b.last = now;
    if (b.tokens < cost) return false;
    b.tokens -= cost;
    return true;
}

void RateGate::prune()
{
    const qint64 now = nowMs();
    for (auto it = m_b.begin(); it != m_b.end();) {
        if (now - it->last > 600000) it = m_b.erase(it);
        else ++it;
    }
}

bool SeenSet::insert(const QString &id)
{
    if (m_set.contains(id)) return false;
    m_set.insert(id);
    m_order.push_back(id);
    while (static_cast<int>(m_order.size()) > m_cap) {
        m_set.remove(m_order.front());
        m_order.pop_front();
    }
    return true;
}

QString humanSize(qint64 b)
{
    const char *u[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(b);
    int i = 0;
    while (v >= 1024 && i < 4) { v /= 1024; ++i; }
    return QStringLiteral("%1 %2").arg(v, 0, 'f', i ? 1 : 0).arg(QLatin1String(u[i]));
}

} // namespace mx::ovl
