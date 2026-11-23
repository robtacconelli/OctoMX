// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: number / time / size formatting exactly as the original WinMX draws it.
#pragma once

#include <QtCore/QString>

class QDateTime;
class QFontMetrics;

namespace mx::ui::fmt {

// Thousands separator used by number(). Default: the system locale's group separator, except for
// the "C"/POSIX locale where a space is used (as in the European reference screenshots,
// `4 388 571`). Overridable (tests, Settings).
QChar groupSeparator();
void setGroupSeparator(QChar c);
void resetGroupSeparator();

QString number(qint64 n);                    // 4388571 -> "4 388 571"
QString duration(int seconds);               // 274 -> "4:34", 4448 -> "1:14:08", <0 -> ""
QString eta(int seconds);                    // Time Left: "0:44", "1:02:10", <0 -> ""
QString rateKBs(qint64 bytesPerSecond);      // K/s column: 6666 -> "6.51"
QString bytesOf(qint64 done, qint64 total);  // "583 408 of 6 110 000"
QString percent(qint64 done, qint64 total);  // "49%"
int percentInt(qint64 done, qint64 total);   // 0..100
QString partialOf(int pct, qint64 total);    // "25% of 109 878 998"
QString bitrate(int kbps);                   // "128", "" if unknown
QString frequency(int hz);                   // "44100", "" if unknown
QString ping(int ms);                        // "123", "?" if unknown (<0)

// ---- [ui-e] human-readable sizes, rates and times (Settings ▸ "Show exact byte counts" = classic)
// Sizes use 1024-based units with three significant digits: "512 B", "812 KB", "45.7 MB", "1.16 GB".
bool exactSizes();                           // ui/exactSizes (default false)
void setExactSizes(bool on);
QChar decimalPoint();                        // the locale's ("." for C / English, "," for German ...)
QString size(qint64 bytes);                  // "45.7 MB" (exactSizes: "47 923 412"); <0 -> ""
QString sizeUnit(qint64 bytes, int *unitIndex);   // number part in the unit chosen for `bytes`
QString sizeOf(qint64 done, qint64 total);   // "12.4 of 45.7 MB" (exactSizes: bytesOf())
QString sizeOfPercent(int pct, qint64 total);// "25% of 104 MB"  (exactSizes: partialOf())
QString exactBytes(qint64 bytes);            // tooltip: "47 923 412 bytes"
QString rate(qint64 bytesPerSecond);         // "6.51 KB/s", "1.25 MB/s" (exactSizes: rateKBs())
QString rateLong(qint64 bytesPerSecond);     // status lines: "449 KB/s" / exact: "460 540 bytes per second"
QString etaText(int seconds);                // Time Left: "44 s", "4 min 34 s", "19 min", "3 h 12 min", "2 d 3 h"
QString dateTime(const QDateTime &t);        // the system locale's short date + time

// Elide `text` on the right with three ASCII dots ("..."), the way Win32 DT_END_ELLIPSIS does.
QString elide(const QString &text, const QFontMetrics &fm, int width);
// [post-1] the same in the middle ("C:\Users\...\incoming\"), for paths; Qt's elidedText() would use
// the Unicode ellipsis character
QString elideMiddle(const QString &text, const QFontMetrics &fm, int width);

} // namespace mx::ui::fmt
