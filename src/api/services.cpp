// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX: out-of-line default implementations of service methods added after the first
// contract (so implementations that predate them, e.g. the mock core, keep working).
#include "mx/services.h"

namespace mx {

namespace {
int numberAfter(const QString &s, QLatin1StringView tag)
{
    // tag must start the string or follow a space ("S=" must not match inside "spm=")
    int i = -1;
    for (int from = 0; (i = s.indexOf(tag, from)) >= 0; from = i + 1)
        if (i == 0 || s.at(i - 1) == QLatin1Char(' ') || s.at(i - 1) == QLatin1Char('<')) break;
    if (i < 0) return 0;
    int j = i + int(tag.size());
    int k = j;
    while (k < s.size() && s.at(k).isDigit()) ++k;
    return s.mid(j, k - j).toInt();
}
} // namespace

NetworkSummary NetworkService::summary() const
{
    NetworkSummary out;
    out.state = overallState();
    const QString t = titleSummary();
    out.primary = numberAfter(t, QLatin1StringView("P="));
    out.sources = numberAfter(t, QLatin1StringView("S="));
    out.searchesPerMinute = numberAfter(t, QLatin1StringView("spm="));
    int connected = 0;
    bool anyOpen = false;
    for (const NetworkStatus &st : networks()) {
        if (st.state != ConnState::Disabled) ++out.networksEnabled;
        if (st.state != ConnState::Connected) continue;
        ++out.networksOnline;
        ++connected;
        if (!st.firewalled) anyOpen = true;
        if (st.usersOnline >= 0) out.usersOnline = qMax<qint64>(0, out.usersOnline) + st.usersOnline;
        if (st.filesOnline >= 0) out.filesOnline = qMax<qint64>(0, out.filesOnline) + st.filesOnline;
    }
    out.firewalled = connected > 0 && !anyOpen;
    return out;
}

} // namespace mx
