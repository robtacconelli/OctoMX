// SPDX-License-Identifier: GPL-3.0-or-later
// [mac-1] core parts of the macOS port: the application firewall answers (parsed on every
// platform, read for real on a Mac), the default route without /proc (macOS sysctl), and names in
// the decomposed Unicode form macOS file systems hand out (NFD) matching composed ones (NFC).
#include <QtTest/QtTest>

#include "core/diag/mac_firewall.h"
#include "core/diag/net_env.h"
#include "core/search_merge.h"

using namespace mx;

class TstMacCore : public QObject {
    Q_OBJECT
private slots:
    void firewallParsing()
    {
        // what socketfilterfw prints (macOS 14 / 15 wording)
        diag::FirewallState off = diag::parseMacFirewall(QStringLiteral("Firewall is disabled. (State = 0)\n"),
                                                         QStringLiteral("Firewall has block all state set to disabled.\n"),
                                                         QStringLiteral("Firewall stealth mode is off\n"), QString());
        QVERIFY(off.checked);
        QVERIFY(!off.enabled);
        QVERIFY(!off.blockAllInbound);
        QCOMPARE(off.profiles, QStringLiteral("stealth mode off"));

        diag::FirewallState on = diag::parseMacFirewall(QStringLiteral("Firewall is enabled. (State = 1)\n"),
                                                        QStringLiteral("Firewall has block all state set to disabled.\n"),
                                                        QStringLiteral("Firewall stealth mode is on\n"),
                                                        QStringLiteral("The application /Applications/OctoMX.app is permitted\n"));
        QVERIFY(on.checked && on.enabled && !on.blockAllInbound);
        QCOMPARE(on.allowRules, 1);
        QCOMPARE(on.blockRules, 0);
        QCOMPARE(on.profiles, QStringLiteral("stealth mode on"));

        diag::FirewallState blocked = diag::parseMacFirewall(QStringLiteral("Firewall is enabled. (State = 1)"),
                                                             QStringLiteral("Firewall has block all state set to enabled."), QString(),
                                                             QStringLiteral("The application /Applications/OctoMX.app is blocked"));
        QVERIFY(blocked.blockAllInbound);
        QCOMPARE(blocked.blockRules, 1);

        diag::FirewallState old = diag::parseMacFirewall(QStringLiteral("Firewall is enabled. (State = 2)"), QString(), QString(), QString());
        QVERIFY(old.enabled && old.blockAllInbound);   // older systems: state 2 = block all

        diag::FirewallState none = diag::parseMacFirewall(QString(), QString(), QString(), QString());
        QVERIFY(!none.checked);
        QVERIFY(!none.error.isEmpty());
    }

    void firewallOnThisMac()
    {
#ifdef Q_OS_MACOS
        const diag::FirewallState s = diag::checkMacFirewall(QCoreApplication::applicationFilePath());
        qInfo("macOS firewall: checked %d, enabled %d, block all %d, %s %s", int(s.checked), int(s.enabled), int(s.blockAllInbound),
              qPrintable(s.profiles), qPrintable(s.error));
        QVERIFY2(s.checked, qPrintable(s.error));   // socketfilterfw answers without privileges
#else
        QVERIFY(!diag::checkMacFirewall(QStringLiteral("/x")).checked);
#endif
    }

    void defaultRoute()
    {
        const QHostAddress gw = diag::defaultGateway();
        qInfo("default gateway: %s", gw.isNull() ? "none" : "found");   // the address itself is not printed
#ifdef Q_OS_MACOS
        // a CI Mac reaches the internet through a default route
        QVERIFY(!gw.isNull());
        QVERIFY(gw.protocol() == QAbstractSocket::IPv4Protocol);
        const diag::NetEnv env = diag::probeNetEnv();
        bool anyDefault = false;
        for (const diag::AdapterInfo &a : env.adapters)
            anyDefault = anyDefault || a.isDefault;
        QVERIFY(anyDefault);
#endif
    }

    void decomposedNames()
    {
        const QString nfc = QStringLiteral("Big Buck Bunny - Café scène.mp4");
        const QString nfd = nfc.normalized(QString::NormalizationForm_D);
        QVERIFY(nfc != nfd);
        QCOMPARE(SearchMerger::normalizedName(nfd), SearchMerger::normalizedName(nfc));
        QCOMPARE(SearchMerger::nameWords(nfd), SearchMerger::nameWords(nfc));
        QVERIFY(SearchMerger::sameName(nfd, nfc));
        QVERIFY(SearchMerger::nameWords(nfd).contains(QStringLiteral("café")));
    }
};

QTEST_MAIN(TstMacCore)
#include "tst_mac_core.moc"
