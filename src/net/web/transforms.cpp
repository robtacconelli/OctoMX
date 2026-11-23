// SPDX-License-Identifier: GPL-3.0-or-later
#include "transforms.h"

#include <QtCore/QHash>
#include <QtCore/QRegularExpression>
#include <QtCore/QStringDecoder>
#include <QtCore/QTimeZone>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>

#include <cmath>

namespace mx::web {

qint64 parseSize(const QString &text, bool decimal)
{
    QString s = text.trimmed();
    if (s.isEmpty()) return -1;
    static const QRegularExpression re(QStringLiteral(
        "^([0-9][0-9.,\\s]*(?:[eE][+-]?[0-9]+)?)\\s*([kKmMgGtTpP]?)(i?)([bB]?)(?:ytes?)?\\.?$"));
    const auto m = re.match(s);
    if (!m.hasMatch()) return -1;
    QString num = m.captured(1);
    num.remove(QRegularExpression(QStringLiteral("\\s")));
    // "1,234.5" → thousands separator; "1,5" (European decimal) → decimal comma
    if (num.contains(QLatin1Char(',')) && num.contains(QLatin1Char('.'))) num.remove(QLatin1Char(','));
    else if (num.count(QLatin1Char(',')) == 1 && num.section(QLatin1Char(','), 1).size() != 3) num.replace(QLatin1Char(','), QLatin1Char('.'));
    else num.remove(QLatin1Char(','));
    bool ok = false;
    const double v = num.toDouble(&ok);
    if (!ok || v < 0 || !std::isfinite(v)) return -1;
    const QChar unit = m.captured(2).isEmpty() ? QChar() : m.captured(2).at(0).toUpper();
    const bool binary = !m.captured(3).isEmpty() || !decimal;
    const double base = binary ? 1024.0 : 1000.0;
    int power = 0;
    if (unit == QLatin1Char('K')) power = 1;
    else if (unit == QLatin1Char('M')) power = 2;
    else if (unit == QLatin1Char('G')) power = 3;
    else if (unit == QLatin1Char('T')) power = 4;
    else if (unit == QLatin1Char('P')) power = 5;
    const double bytes = v * std::pow(base, power);
    if (bytes > 9.0e18) return -1;
    return qint64(std::llround(bytes));
}

QByteArray base32Decode(const QString &text)
{
    QByteArray out;
    quint64 buffer = 0;
    int bits = 0;
    for (QChar c : text) {
        int v;
        const char ch = c.toUpper().toLatin1();
        if (ch >= 'A' && ch <= 'Z') v = ch - 'A';
        else if (ch >= '2' && ch <= '7') v = ch - '2' + 26;
        else if (ch == '=') break;
        else return QByteArray();
        buffer = (buffer << 5) | quint64(v);
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            out.append(char((buffer >> bits) & 0xff));
        }
    }
    return out;
}

QByteArray parseInfoHash(const QString &textIn)
{
    QString text = textIn.trimmed();
    if (text.startsWith(QLatin1String("urn:btih:"), Qt::CaseInsensitive)) text = text.mid(9);
    QByteArray raw;
    if (text.size() == 40) {
        for (QChar c : text)
            if (!isxdigit(c.toLatin1())) return QByteArray();
        raw = QByteArray::fromHex(text.toLatin1());
    } else if (text.size() == 32) {
        raw = base32Decode(text);
    }
    if (raw.size() != 20) return QByteArray();
    if (raw.count('\0') == 20) return QByteArray();
    return raw;
}

static QList<QPair<QString, QString>> magnetItems(const QString &magnet)
{
    QList<QPair<QString, QString>> out;
    const QString m = magnet.trimmed();
    if (!m.startsWith(QLatin1String("magnet:"), Qt::CaseInsensitive)) return out;
    int q = m.indexOf(QLatin1Char('?'));
    if (q < 0) return out;
    const QStringList parts = m.mid(q + 1).split(QLatin1Char('&'), Qt::SkipEmptyParts);
    for (const QString &p : parts) {
        const int eq = p.indexOf(QLatin1Char('='));
        QString k = eq < 0 ? p : p.left(eq);
        QString v = eq < 0 ? QString() : p.mid(eq + 1);
        v.replace(QLatin1Char('+'), QLatin1Char(' '));
        out.append({k.toLower(), QUrl::fromPercentEncoding(v.toUtf8())});
    }
    return out;
}

QStringList magnetParams(const QString &magnet, const QString &key)
{
    QStringList out;
    const QString k = key.toLower();
    for (const auto &kv : magnetItems(magnet))
        if (kv.first == k || kv.first.startsWith(k + QLatin1Char('.'))) out << kv.second;
    return out;
}

QString magnetParam(const QString &magnet, const QString &key)
{
    const QStringList v = magnetParams(magnet, key);
    return v.isEmpty() ? QString() : v.first();
}

QByteArray infoHashFromMagnet(const QString &magnet)
{
    for (const QString &xt : magnetParams(magnet, QStringLiteral("xt"))) {
        if (xt.startsWith(QLatin1String("urn:btih:"), Qt::CaseInsensitive)) {
            const QByteArray h = parseInfoHash(xt.mid(9));
            if (!h.isEmpty()) return h;
        }
    }
    return QByteArray();
}

static QDateTime utc(const QDateTime &dt)
{
    return dt.isValid() ? dt.toUTC() : QDateTime();
}

QDateTime parseDate(const QString &textIn, const QString &format)
{
    const QString text = textIn.simplified();
    if (text.isEmpty()) return QDateTime();
    if (!format.isEmpty()) {
        QDateTime dt = QDateTime::fromString(text, format);
        if (dt.isValid()) {
            if (dt.timeSpec() == Qt::LocalTime) dt.setTimeZone(QTimeZone::UTC);
            return utc(dt);
        }
    }
    // numbers: unix seconds or milliseconds
    bool ok = false;
    const qint64 n = text.toLongLong(&ok);
    if (ok) {
        if (n <= 0) return QDateTime();
        if (n > 100000000000LL) return QDateTime::fromMSecsSinceEpoch(n, QTimeZone::UTC);
        return QDateTime::fromSecsSinceEpoch(n, QTimeZone::UTC);
    }
    // ISO 8601 (also "YYYY-MM-DD HH:MM:SS")
    {
        QString iso = text;
        if (iso.size() >= 19 && iso.at(10) == QLatin1Char(' ')) iso[10] = QLatin1Char('T');
        QDateTime dt = QDateTime::fromString(iso, Qt::ISODateWithMs);
        if (!dt.isValid()) dt = QDateTime::fromString(iso, Qt::ISODate);
        if (dt.isValid()) {
            if (dt.timeSpec() == Qt::LocalTime) dt.setTimeZone(QTimeZone::UTC);
            return utc(dt);
        }
    }
    // RFC 2822 family. Normalise "Tue,17 Nov" → "Tue, 17 Nov" and drop the weekday.
    {
        static const QRegularExpression re(QStringLiteral(
            "^(?:[A-Za-z]{3},?\\s*)?(\\d{1,2})\\s+([A-Za-z]{3})[a-z]*\\s+(\\d{4})(?:\\s+(\\d{1,2}):(\\d{2})(?::(\\d{2}))?)?"
            "(?:\\s*([+-]\\d{4}|GMT|UTC|UT|Z))?"));
        const auto m = re.match(text);
        if (m.hasMatch()) {
            static const QStringList months = {QStringLiteral("jan"), QStringLiteral("feb"), QStringLiteral("mar"),
                QStringLiteral("apr"), QStringLiteral("may"), QStringLiteral("jun"), QStringLiteral("jul"),
                QStringLiteral("aug"), QStringLiteral("sep"), QStringLiteral("oct"), QStringLiteral("nov"),
                QStringLiteral("dec")};
            const int mon = months.indexOf(m.captured(2).toLower()) + 1;
            const QDate d(m.captured(3).toInt(), mon, m.captured(1).toInt());
            const QTime t(m.captured(4).toInt(), m.captured(5).toInt(), m.captured(6).toInt());
            if (mon > 0 && d.isValid() && t.isValid()) {
                int offset = 0;
                const QString tz = m.captured(7);
                if (tz.size() == 5 && (tz[0] == QLatin1Char('+') || tz[0] == QLatin1Char('-'))) {
                    offset = (tz.mid(1, 2).toInt() * 60 + tz.mid(3, 2).toInt()) * 60;
                    if (tz[0] == QLatin1Char('-')) offset = -offset;
                }
                return utc(QDateTime(d, t, QTimeZone::fromSecondsAheadOfUtc(offset)));
            }
        }
    }
    // "Oct 25, 2026" / "October 25, 2026"
    {
        static const QRegularExpression re(QStringLiteral("^([A-Za-z]{3})[a-z]*\\.?\\s+(\\d{1,2}),?\\s+(\\d{4})$"));
        const auto m = re.match(text);
        if (m.hasMatch()) {
            const QDate d = QDate::fromString(m.captured(1) + QLatin1Char(' ') + m.captured(2) + QLatin1Char(' ') + m.captured(3),
                                              QStringLiteral("MMM d yyyy"));
            if (d.isValid()) return QDateTime(d, QTime(0, 0), QTimeZone::UTC);
        }
    }
    return QDateTime();
}

QString decodeHtmlEntities(const QString &textIn)
{
    if (!textIn.contains(QLatin1Char('&'))) return textIn;
    static const QHash<QString, QString> named = {
        {QStringLiteral("amp"), QStringLiteral("&")}, {QStringLiteral("lt"), QStringLiteral("<")},
        {QStringLiteral("gt"), QStringLiteral(">")}, {QStringLiteral("quot"), QStringLiteral("\"")},
        {QStringLiteral("apos"), QStringLiteral("'")}, {QStringLiteral("nbsp"), QStringLiteral(" ")},
        {QStringLiteral("middot"), QStringLiteral("·")}, {QStringLiteral("ndash"), QString(QChar(0x2013))},
        // decoded as they are: names from the index sites are their data, not our wording
        {QStringLiteral("mdash"), QString(QChar(0x2014))}, {QStringLiteral("hellip"), QString(QChar(0x2026))},
    };
    static const QRegularExpression re(QStringLiteral("&(#x[0-9a-fA-F]{1,6}|#[0-9]{1,7}|[a-zA-Z]{2,8});"));
    QString out;
    out.reserve(textIn.size());
    int last = 0;
    auto it = re.globalMatch(textIn);
    while (it.hasNext()) {
        const auto m = it.next();
        out += QStringView(textIn).mid(last, m.capturedStart() - last);
        const QString e = m.captured(1);
        QString rep;
        if (e.startsWith(QLatin1String("#x"), Qt::CaseInsensitive)) {
            const uint cp = e.mid(2).toUInt(nullptr, 16);
            if (cp > 0 && cp <= 0x10FFFF) rep = QString::fromUcs4(reinterpret_cast<const char32_t *>(&cp), 1);
        } else if (e.startsWith(QLatin1Char('#'))) {
            const uint cp = e.mid(1).toUInt();
            if (cp > 0 && cp <= 0x10FFFF) rep = QString::fromUcs4(reinterpret_cast<const char32_t *>(&cp), 1);
        } else {
            rep = named.value(e.toLower(), m.captured(0));
        }
        out += rep;
        last = m.capturedEnd();
    }
    out += QStringView(textIn).mid(last);
    return out;
}

QString stripHtml(const QString &html)
{
    static const QRegularExpression tags(QStringLiteral("<[^>]*>"));
    static const QRegularExpression cdata(QStringLiteral("<!\\[CDATA\\[|\\]\\]>"));
    QString s = html;
    s.remove(cdata);
    s.replace(QRegularExpression(QStringLiteral("<br\\s*/?>"), QRegularExpression::CaseInsensitiveOption), QStringLiteral(" "));
    s.replace(tags, QStringLiteral(" "));
    return decodeHtmlEntities(s).simplified();
}

QString buildMagnet(const QByteArray &btih, const QString &name, qint64 size, const QStringList &trackers)
{
    QString m = QStringLiteral("magnet:?xt=urn:btih:") + QString::fromLatin1(btih.toHex());
    if (!name.isEmpty()) m += QStringLiteral("&dn=") + QString::fromLatin1(QUrl::toPercentEncoding(name));
    if (size > 0) m += QStringLiteral("&xl=%1").arg(size);
    QStringList seen;
    for (const QString &t : trackers) {
        const QString tr = t.trimmed();
        if (tr.isEmpty() || seen.contains(tr)) continue;
        seen << tr;
        m += QStringLiteral("&tr=") + QString::fromLatin1(QUrl::toPercentEncoding(tr));
    }
    return m;
}

bool guessIsBundle(int numFiles, const QString &name)
{
    if (numFiles > 1) return true;
    if (numFiles == 1) return false;
    return mediaTypeForFileName(name.trimmed()) == MediaType::Other;
}

QString repairMojibake(const QString &text)
{
    // quick reject: double-encoded UTF-8 always shows a lead byte as Â Ã Ä ... ß (0xC2..0xDF) or
    // â ã ... (0xE2..0xEF) followed by a char from the 0x80..0xBF range (or its cp1252 replacement)
    bool suspicious = false;
    for (int i = 0; i + 1 < text.size(); ++i) {
        const ushort a = text.at(i).unicode();
        if ((a >= 0xC2 && a <= 0xDF) || (a >= 0xE2 && a <= 0xEF)) {
            const ushort b = text.at(i + 1).unicode();
            if ((b >= 0x80 && b <= 0xBF) || b > 0xFF) { suspicious = true; break; }
        }
    }
    if (!suspicious) return text;
    // cp1252 0x80..0x9F
    static const ushort cp1252[32] = {0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030,
                                      0x0160, 0x2039, 0x0152, 0, 0x017D, 0, 0, 0x2018, 0x2019, 0x201C, 0x201D,
                                      0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178};
    QByteArray bytes;
    bytes.reserve(text.size());
    for (QChar c : text) {
        const ushort u = c.unicode();
        if (u < 0x80 || (u >= 0xA0 && u <= 0xFF) || (u >= 0x80 && u <= 0x9F)) { bytes.append(char(u)); continue; }
        int found = -1;
        for (int k = 0; k < 32; ++k)
            if (cp1252[k] == u) { found = k; break; }
        if (found < 0) return text;
        bytes.append(char(0x80 + found));
    }
    QStringDecoder dec(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    const QString fixed = dec(bytes);
    if (dec.hasError() || fixed.contains(QChar::ReplacementCharacter) || fixed.size() >= text.size()) return text;
    return fixed;
}

QString normalizeForMatch(const QString &text)
{
    QString out;
    out.reserve(text.size());
    for (QChar c : text.toLower()) out += c.isLetterOrNumber() ? c : QLatin1Char(' ');
    return out.simplified();
}

QStringList queryWords(const QString &text)
{
    return normalizeForMatch(text).split(QLatin1Char(' '), Qt::SkipEmptyParts);
}

} // namespace mx::web
