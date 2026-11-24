// SPDX-License-Identifier: GPL-3.0-or-later
// [mac-1] the macOS application firewall, read only and without privileges: socketfilterfw
// (/usr/libexec/ApplicationFirewall) answers --getglobalstate, --getblockall, --getstealthmode and
// --getappblocked <app> for any user. Blocking (call it from a worker thread). Fills the same
// FirewallState as the Windows check: enabled, blockAllInbound, allowRules = 1 when the firewall
// lets OctoMX accept connections, blockRules = 1 when it blocks it, profiles = "stealth mode on".
// On other systems (and when socketfilterfw cannot be run) checked = false.
#pragma once

#include "core/diag/win_firewall.h"

namespace mx::diag {

FirewallState checkMacFirewall(const QString &programPath);
// the parsing part (tests): the outputs of the four socketfilterfw questions
FirewallState parseMacFirewall(const QString &global, const QString &blockAll, const QString &stealth, const QString &app);

} // namespace mx::diag
