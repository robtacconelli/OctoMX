// SPDX-License-Identifier: GPL-3.0-or-later
// [diag-1] Windows Defender Firewall, read only: the state of the active profile(s) and whether an
// inbound rule for this program exists (INetFwPolicy2 COM, no elevation, no changes). Blocking
// (call it from a worker thread). On other systems checked = false.
#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>

namespace mx::diag {

struct FirewallState {
    bool    checked = false;
    QString error;                // why it could not be checked
    QString profiles;             // "Private", "Public, Domain"
    bool    enabled = false;      // firewall on for an active profile
    bool    blockAllInbound = false;   // "Block all incoming connections" (shields up)
    bool    inboundDefaultBlock = true;
    int     allowRules = 0;       // enabled inbound allow rules for this program
    int     blockRules = 0;       // enabled inbound block rules for this program
    QStringList ruleNames;
    // [ren-1] enabled inbound allow rules for winmx.exe in the same folder (the program file of copies
    // installed before 5.0): Windows matches rules by program file, so they do not cover octomx.exe
    QStringList oldProgramRules;
};

FirewallState checkWindowsFirewall(const QString &programPath);

} // namespace mx::diag
