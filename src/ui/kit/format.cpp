// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/kit/format.h"

#include <QtCore/QDateTime>
#include <QtCore/QLocale>
#include <QtGui/QFontMetrics>

namespace mx::ui::fmt {

namespace {
QChar g_sep;
bool g_sepSet = false;

QChar localeSeparator()
{
    // MX_DIGIT_GROUPING=space|comma|dot overrides the locale (reproducible screenshots / tests)
    const QByteArray env = qgetenv("MX_DIGIT_GROUPING").toLower();
    if (env == "space")
        return QLatin1Char(' ');
    if (env == "comma")
        return QLatin1Char(',');
    if (env == "dot")
        return QLatin1Char('.');
    const QLocale l = QLocale::system();
    if (l.language() == QLocale::C)
        return QLatin1Char(' ');
    const QString g = l.groupSeparator();
    if (g.isEmpty())
        return QLatin1Char(' ');
    const QChar c = g.at(0);
    // Narrow no-break space / no-break space -> a plain space renders identically in Tahoma.
    if (c == QChar(0x202F) || c == QChar(0x00A0))
        return QLatin1Char(' ');
    return c;
}
} // namespace

QChar groupSeparator()
{
    if (!g_sepSet) {
        g_sep = localeSeparator();
        g_sepSet = true;
    }
    return g_sep;
}

void setGroupSeparator(QChar c)
{
    g_sep = c;
    g_sepSet = true;
}

void resetGroupSeparator()
{
    g_sepSet = false;
}

QString number(qint64 n)
{
    const bool neg = n < 0;
    QString digits = QString::number(neg ? -n : n);
    const QChar sep = groupSeparator();
    QString out;
    out.reserve(digits.size() + digits.size() / 3 + 1);
    const int lead = digits.size() % 3;
    for (int i = 0; i < digits.size(); ++i) {
        if (i && (i - lead) % 3 == 0)
            out += sep;
        out += digits.at(i);
    }
    return neg ? QLatin1Char('-') + out : out;
}

QString duration(int s)
{
    if (s < 0)
        return QString();
    const int h = s / 3600, m = (s / 60) % 60, sec = s % 60;
    if (h > 0)
        return QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(sec, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(m).arg(sec, 2, 10, QLatin1Char('0'));
}

QString eta(int s)
{
    return duration(s);
}

QString rateKBs(qint64 bps)
{
    // [qa-2] the locale's decimal mark: with "1.234.567" grouping (Italian, German) "6.51" read as 651
    QString s = QString::number(double(bps) / 1024.0, 'f', 2);
    return s.replace(QLatin1Char('.'), decimalPoint());
}

QString bytesOf(qint64 done, qint64 total)
{
    return QStringLiteral("%1 of %2").arg(number(done), number(total));
}

int percentInt(qint64 done, qint64 total)
{
    if (total <= 0)
        return 0;
    const qint64 p = done * 100 / total;
    return int(qBound(qint64(0), p, qint64(100)));
}

QString percent(qint64 done, qint64 total)
{
    return QStringLiteral("%1%").arg(percentInt(done, total));
}

QString partialOf(int pct, qint64 total)
{
    return QStringLiteral("%1% of %2").arg(pct).arg(number(total));
}

QString bitrate(int kbps)
{
    return kbps > 0 ? QString::number(kbps) : QString();
}

QString frequency(int hz)
{
    return hz > 0 ? QString::number(hz) : QString();
}

QString ping(int ms)
{
    return ms >= 0 ? QString::number(ms) : QStringLiteral("?");
}

// ------------------------------------------------------------------------------------ [ui-e]

namespace {
bool g_exact = false;

QString decimals(double v, int places)
{
    QString s = QString::number(v, 'f', places);
    if (places > 0)
        s.replace(QLatin1Char('.'), decimalPoint());
    return s;
}
} // namespace

bool exactSizes()
{
    return g_exact;
}

void setExactSizes(bool on)
{
    g_exact = on;
}

QChar decimalPoint()
{
    const QByteArray env = qgetenv("MX_DIGIT_GROUPING").toLower();
    if (env == "dot")   // "1.234.567" grouping goes with a decimal comma
        return QLatin1Char(',');
    if (!env.isEmpty())
        return QLatin1Char('.');
    const QLocale l = QLocale::system();
    if (l.language() == QLocale::C)
        return QLatin1Char('.');
    const QString d = l.decimalPoint();
    return d.isEmpty() ? QLatin1Char('.') : d.at(0);
}

QString sizeUnit(qint64 bytes, int *unitIndex)
{
    static const qint64 k = 1024;
    int u = 0;
    double v = double(bytes);
    while (u < 4 && v >= 999.5) {   // 1000 B .. 1023 B read better as 0.98 KB
        v /= k;
        ++u;
    }
    if (unitIndex)
        *unitIndex = u;
    if (u == 0)
        return QString::number(bytes);
    const int places = v < 9.995 ? 2 : v < 99.95 ? 1 : 0;
    return decimals(v, places);
}

static const char *const kUnits[] = {"B", "KB", "MB", "GB", "TB"};

QString size(qint64 bytes)
{
    if (bytes < 0)
        return QString();
    if (g_exact)
        return number(bytes);
    int u = 0;
    const QString n = sizeUnit(bytes, &u);
    return n + QLatin1Char(' ') + QLatin1String(kUnits[u]);
}

QString sizeOf(qint64 done, qint64 total)
{
    if (g_exact)
        return total > 0 ? bytesOf(done, total) : QStringLiteral("%1 of ?").arg(number(done));
    if (total <= 0)
        return QStringLiteral("%1 of ?").arg(size(qMax<qint64>(0, done)));
    int u = 0;
    const QString t = sizeUnit(total, &u);
    // the done part in the unit of the total ("12.4 of 45.7 MB"); 0 stays "0"
    QString d;
    if (done <= 0) {
        d = QStringLiteral("0");
    } else if (u == 0) {
        d = QString::number(done);
    } else {
        double v = double(done);
        for (int i = 0; i < u; ++i)
            v /= 1024.0;
        const int places = v < 9.995 ? 2 : v < 99.95 ? 1 : 0;
        d = decimals(v, places);
    }
    return QStringLiteral("%1 of %2 %3").arg(d, t, QLatin1String(kUnits[u]));
}

QString sizeOfPercent(int pct, qint64 total)
{
    if (g_exact)
        return partialOf(pct, total);
    return QStringLiteral("%1% of %2").arg(pct).arg(size(total));
}

QString exactBytes(qint64 bytes)
{
    if (bytes < 0)
        return QString();
    return bytes == 1 ? QStringLiteral("1 byte") : QStringLiteral("%1 bytes").arg(number(bytes));
}

QString rate(qint64 bps)
{
    if (g_exact)
        return rateKBs(bps);
    if (bps < 1024 * 1000)
        return decimals(bps / 1024.0, bps < 10 * 1024 ? 2 : bps < 100 * 1024 ? 1 : 0) + QStringLiteral(" KB/s");
    const double m = bps / (1024.0 * 1024.0);
    return decimals(m, m < 9.995 ? 2 : m < 99.95 ? 1 : 0) + QStringLiteral(" MB/s");
}

QString rateLong(qint64 bps)
{
    if (g_exact)
        return QStringLiteral("%1 bytes per second").arg(number(bps));
    return rate(bps);
}

QString etaText(int s)
{
    if (s < 0)
        return QString();
    if (g_exact)
        return eta(s);
    if (s < 60)
        return QStringLiteral("%1 s").arg(s);
    const int m = s / 60;
    if (m < 10)
        return QStringLiteral("%1 min %2 s").arg(m).arg(s % 60);
    if (m < 60)
        return QStringLiteral("%1 min").arg(m);
    const int h = m / 60;
    if (h < 24)
        return QStringLiteral("%1 h %2 min").arg(h).arg(m % 60);
    return QStringLiteral("%1 d %2 h").arg(h / 24).arg(h % 24);
}

QString dateTime(const QDateTime &t)
{
    if (!t.isValid())
        return QString();
    return QLocale::system().toString(t.toLocalTime(), QLocale::ShortFormat);
}

QString elide(const QString &text, const QFontMetrics &fm, int width)
{
    if (width <= 0)
        return QString();
    if (fm.horizontalAdvance(text) <= width)
        return text;
    static const QString dots = QStringLiteral("...");
    const int dw = fm.horizontalAdvance(dots);
    if (dw > width)
        return QString();
    // binary search the longest prefix that fits together with the dots
    int lo = 0, hi = text.size();
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        if (fm.horizontalAdvance(text.left(mid)) + dw <= width)
            lo = mid;
        else
            hi = mid - 1;
    }
    // [qa-2] never cut a surrogate pair in half (a lone half draws as a box)
    if (lo > 0 && text.at(lo - 1).isHighSurrogate())
        --lo;
    return text.left(lo) + dots;
}

QString elideMiddle(const QString &text, const QFontMetrics &fm, int width)
{
    if (width <= 0)
        return QString();
    if (fm.horizontalAdvance(text) <= width)
        return text;
    static const QString dots = QStringLiteral("...");
    const int dw = fm.horizontalAdvance(dots);
    if (dw > width)
        return QString();
    // the longest n such that the first n/2 and the last n - n/2 characters fit with the dots
    auto cut = [&text](int n, QString *l, QString *r) {
        int left = (n + 1) / 2, right = n / 2;
        if (left > 0 && text.at(left - 1).isHighSurrogate())   // never half a surrogate pair
            --left;
        if (right > 0 && text.at(text.size() - right).isLowSurrogate())
            --right;
        *l = text.left(left);
        *r = text.right(right);
    };
    int lo = 0, hi = int(text.size()) - 1;
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        QString l, r;
        cut(mid, &l, &r);
        if (fm.horizontalAdvance(l) + dw + fm.horizontalAdvance(r) <= width)
            lo = mid;
        else
            hi = mid - 1;
    }
    QString l, r;
    cut(lo, &l, &r);
    return l + dots + r;
}

} // namespace mx::ui::fmt
