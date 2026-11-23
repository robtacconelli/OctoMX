// SPDX-License-Identifier: GPL-3.0-or-later
#include "gnut_http.h"

#include "mxhash/encoding.h"

#include <QtCore/QDateTime>
#include <QtCore/QLocale>
#include <QtCore/QTimeZone>
#include <functional>
#include <algorithm>

namespace mx::gnut {

static bool toInt64(const QString &s, qint64 *v)
{
    bool ok = false;
    const QString t = s.trimmed();
    if (t.isEmpty() || t.size() > 19) return false;
    for (QChar c : t) if (!c.isDigit()) return false;
    *v = t.toLongLong(&ok);
    return ok && *v >= 0;
}

QList<Span> mergeSpans(QList<Span> spans)
{
    std::sort(spans.begin(), spans.end());
    QList<Span> out;
    for (const Span &s : spans) {
        if (s.second <= s.first) continue;
        if (!out.isEmpty() && s.first <= out.last().second) out.last().second = qMax(out.last().second, s.second);
        else out << s;
    }
    return out;
}

bool parseRangeHeader(const QString &vIn, qint64 size, QList<Span> *out)
{
    QString v = vIn.trimmed();
    const int eq = int(v.indexOf(QLatin1Char('=')));
    if (eq < 0 || v.left(eq).trimmed().compare(QLatin1String("bytes"), Qt::CaseInsensitive) != 0) return false;
    const QStringList items = v.mid(eq + 1).split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (items.isEmpty() || items.size() > 32) return false;
    QList<Span> spans;
    for (const QString &it : items) {
        const QString t = it.trimmed();
        const int d = int(t.indexOf(QLatin1Char('-')));
        if (d < 0) return false;
        const QString a = t.left(d).trimmed(), b = t.mid(d + 1).trimmed();
        qint64 first = 0, last = 0;
        if (a.isEmpty()) {                            // suffix: last N bytes
            if (!toInt64(b, &last) || last == 0) return false;
            first = qMax<qint64>(0, size - last);
            last = size - 1;
        } else {
            if (!toInt64(a, &first)) return false;
            if (b.isEmpty()) last = size - 1;
            else if (!toInt64(b, &last) || last < first) return false;
        }
        if (first >= size) continue;
        last = qMin(last, size - 1);
        spans << Span(first, last + 1);
    }
    if (spans.isEmpty()) return false;
    *out = spans;
    return true;
}

bool parseContentRange(const QString &vIn, qint64 *first, qint64 *lastIncl, qint64 *total)
{
    QString v = vIn.trimmed();
    if (!v.startsWith(QLatin1String("bytes"), Qt::CaseInsensitive)) return false;
    v = v.mid(5).trimmed();
    if (v.startsWith(QLatin1Char('=')) || v.startsWith(QLatin1Char(':'))) v = v.mid(1).trimmed();
    const int slash = int(v.indexOf(QLatin1Char('/')));
    const int dash = int(v.indexOf(QLatin1Char('-')));
    if (dash < 0 || slash < 0 || dash > slash) return false;
    qint64 a = 0, b = 0, t = -1;
    if (!toInt64(v.left(dash), &a) || !toInt64(v.mid(dash + 1, slash - dash - 1), &b) || b < a) return false;
    const QString ts = v.mid(slash + 1).trimmed();
    if (ts != QLatin1String("*")) {
        if (!toInt64(ts, &t) || b >= t) return false;
    }
    *first = a;
    *lastIncl = b;
    *total = t;
    return true;
}

QList<Span> parseAvailableRanges(const QString &vIn, qint64 size)
{
    QString v = vIn.trimmed();
    if (v.startsWith(QLatin1String("bytes"), Qt::CaseInsensitive)) v = v.mid(5);
    if (v.startsWith(QLatin1Char('='))) v = v.mid(1);
    QList<Span> spans;
    const QStringList items = v.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString &it : items) {
        if (spans.size() > 4096) break;
        const QString t = it.trimmed();
        const int d = int(t.indexOf(QLatin1Char('-')));
        qint64 a = 0, b = 0;
        if (d <= 0 || !toInt64(t.left(d), &a) || !toInt64(t.mid(d + 1), &b) || b < a) continue;
        if (a >= size) continue;
        spans << Span(a, qMin(b + 1, size));
    }
    return mergeSpans(spans);
}

QString formatAvailableRanges(const QList<Span> &spans)
{
    QStringList parts;
    for (const Span &s : spans) parts << QStringLiteral("%1-%2").arg(s.first).arg(s.second - 1);
    return QStringLiteral("bytes ") + parts.join(QStringLiteral(", "));
}

static void parseKv(const QString &v, QChar sep, const std::function<void(const QString &, const QString &)> &f)
{
    const QStringList items = v.split(sep, Qt::SkipEmptyParts);
    for (const QString &it : items.mid(0, 32)) {
        const int eq = int(it.indexOf(QLatin1Char('=')));
        if (eq < 0) continue;
        f(it.left(eq).trimmed().toLower(), it.mid(eq + 1).trimmed());
    }
}

void parseXQueue(const QString &v, QueueInfo *q)
{
    if (!v.contains(QLatin1Char('='))) return;        // "X-Queue: 0.1" is a version, not queue info
    q->present = true;
    parseKv(v, QLatin1Char(','), [&](const QString &k, const QString &val) {
        bool ok = false;
        const int n = val.toInt(&ok);
        if (!ok) return;
        if (k == QLatin1String("position")) q->position = n;
        else if (k == QLatin1String("length")) q->length = n;
        else if (k == QLatin1String("limit")) q->limit = n;
        else if (k == QLatin1String("pollmin")) q->pollMin = n;
        else if (k == QLatin1String("pollmax")) q->pollMax = n;
    });
}

void parseXQueued(const QString &v, QueueInfo *q)
{
    if (v.trimmed().isEmpty()) return;
    q->present = true;
    parseKv(v, QLatin1Char(';'), [&](const QString &k, const QString &val) {
        if (k == QLatin1String("id")) { q->id = val.left(64); return; }
        bool ok = false;
        const int n = val.toInt(&ok);
        if (!ok) return;
        if (k == QLatin1String("position")) q->position = n;
        else if (k == QLatin1String("length")) q->length = n;
        else if (k == QLatin1String("lifetime")) q->lifetime = n;
        else if (k == QLatin1String("eta")) q->eta = n;
    });
}

int parseRetryAfter(const QString &vIn)
{
    const QString v = vIn.trimmed();
    if (v.isEmpty()) return -1;
    bool ok = false;
    const qint64 n = v.toLongLong(&ok);
    if (ok) return int(qBound<qint64>(0LL, n, 86400LL));
    QDateTime d = QLocale::c().toDateTime(v, QStringLiteral("ddd, dd MMM yyyy HH:mm:ss 'GMT'"));
    if (!d.isValid()) return -1;
    d.setTimeZone(QTimeZone::UTC);
    return int(qBound<qint64>(0LL, QDateTime::currentDateTimeUtc().secsTo(d), 86400LL));
}

QList<HostPort> parseAltLocs(const QString &v, int max, QList<HostPort> *tls)
{
    QList<HostPort> out;
    QByteArray tlsBits;
    int entry = 0;                              // index among the host entries (for tls=)
    const QStringList items = v.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (QString it : items) {
        if (out.size() >= max) break;
        it = it.trimmed();
        if (it.startsWith(QLatin1String("http://"), Qt::CaseInsensitive)) {
            it = it.mid(7);
            it = it.section(QLatin1Char('/'), 0, 0);
        }
        it = it.section(QLatin1Char(' '), 0, 0);
        if (it.startsWith(QLatin1String("tls="))) { tlsBits = QByteArray::fromHex(it.mid(4).toLatin1()); continue; }
        const HostPort h = HostPort::parse(it, 6346);
        const int i = entry++;
        const bool flagged = i / 8 < tlsBits.size() && (quint8(tlsBits[i / 8]) & (0x80 >> (i % 8)));
        if (h.isValid() && !out.contains(h)) {
            out << h;
            if (flagged && tls) *tls << h;
        }
    }
    return out;
}

QString formatAltLocs(const QList<HostPort> &hosts)
{
    QStringList parts;
    for (const HostPort &h : hosts) parts << (h.port == 6346 ? ipToString(h.ip) : h.toString());
    return parts.join(QStringLiteral(", "));
}

QList<PushLoc> parsePushAltLocs(const QString &v, int max)
{
    QList<PushLoc> out;
    const QStringList items = v.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString &it : items) {
        if (out.size() >= max) break;
        const QStringList f = it.trimmed().split(QLatin1Char(';'), Qt::SkipEmptyParts);
        if (f.isEmpty()) continue;
        PushLoc p;
        const QString g = f[0].trimmed();
        if (g.size() == 32) p.guid = guidFromHex(g);
        else if (g.size() == 26) { bool ok = false; p.guid = mx::hash::fromBase32(g, &ok); if (!ok) p.guid.clear(); }
        if (p.guid.size() != 16) continue;
        for (int i = 1; i < f.size() && p.proxies.size() < 8; ++i) {
            const HostPort h = HostPort::parse(f[i].trimmed(), 0);
            if (h.isValid()) p.proxies << h;
        }
        out << p;
    }
    return out;
}

QString formatPushAltLoc(const PushLoc &p)
{
    QStringList f;
    f << guidHex(p.guid);
    for (const HostPort &h : p.proxies) f << h.toString();
    return f.join(QLatin1Char(';'));
}

} // namespace mx::gnut
