// SPDX-License-Identifier: GPL-3.0-or-later
// [mac-1] macOS application firewall check (see mac_firewall.h). Read only.
#include "core/diag/mac_firewall.h"

#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>

#include "mx/app_paths.h"

namespace mx::diag {

namespace {
// "... (State = 1)" or the words enabled / disabled / on / off; -1 = not understood
int stateOf(const QString &text)
{
    const QRegularExpressionMatch m = QRegularExpression(QStringLiteral("State\\s*=\\s*(\\d+)")).match(text);
    if (m.hasMatch())
        return m.captured(1).toInt() > 0 ? 1 : 0;
    const QString t = text.toLower();
    if (t.contains(QLatin1String("disabled")) || t.contains(QLatin1String(" is off")))
        return 0;
    if (t.contains(QLatin1String("enabled")) || t.contains(QLatin1String(" is on")))
        return 1;
    return -1;
}
} // namespace

FirewallState parseMacFirewall(const QString &global, const QString &blockAll, const QString &stealth, const QString &app)
{
    FirewallState s;
    const int g = stateOf(global);
    if (g < 0) {
        s.error = global.trimmed().isEmpty() ? QStringLiteral("no answer from socketfilterfw") : global.trimmed().left(200);
        return s;
    }
    s.checked = true;
    s.enabled = g == 1;
    // --getglobalstate says "State = 2" when "Block all incoming connections" is on (older systems)
    const QRegularExpressionMatch gm = QRegularExpression(QStringLiteral("State\\s*=\\s*(\\d+)")).match(global);
    s.blockAllInbound = (gm.hasMatch() && gm.captured(1).toInt() == 2) || stateOf(blockAll) == 1;
    s.inboundDefaultBlock = false;   // the application firewall asks for each new program
    s.profiles = stateOf(stealth) == 1 ? QStringLiteral("stealth mode on") : QStringLiteral("stealth mode off");
    const QString a = app.toLower();
    if (a.contains(QLatin1String("blocked")) && !a.contains(QLatin1String("not blocked")))
        s.blockRules = 1;
    else if (a.contains(QLatin1String("permitted")) || a.contains(QLatin1String("allowed")) || a.contains(QLatin1String("not blocked")))
        s.allowRules = 1;
    if (s.allowRules || s.blockRules)
        s.ruleNames << QStringLiteral("OctoMX");
    return s;
}

FirewallState checkMacFirewall(const QString &programPath)
{
#ifdef Q_OS_MACOS
    const QString tool = QStringLiteral("/usr/libexec/ApplicationFirewall/socketfilterfw");
    if (!QFileInfo(tool).isExecutable()) {
        FirewallState s;
        s.error = QStringLiteral("socketfilterfw is not on this Mac");
        return s;
    }
    auto ask = [&tool](const QStringList &args) {
        QProcess p;
        p.start(tool, args);
        if (!p.waitForFinished(4000)) {
            p.kill();
            p.waitForFinished(500);
            return QString();
        }
        return QString::fromLocal8Bit(p.readAllStandardOutput() + p.readAllStandardError());
    };
    // the firewall knows applications by their bundle
    const QString bundle = mx::paths::bundleOf(QFileInfo(programPath).absolutePath());
    return parseMacFirewall(ask({QStringLiteral("--getglobalstate")}), ask({QStringLiteral("--getblockall")}),
                            ask({QStringLiteral("--getstealthmode")}),
                            ask({QStringLiteral("--getappblocked"), bundle.isEmpty() ? programPath : bundle}));
#else
    Q_UNUSED(programPath)
    FirewallState s;
    s.error = QStringLiteral("not checked on this system");
    return s;
#endif
}

} // namespace mx::diag
