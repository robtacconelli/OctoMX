// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX: the mock core (target mxmock). Implements every service of mx/services.h with
// deterministic, lively demo data modelled on the layout of the reference screenshots (the
// "velorine" search has the shape of the search in reference/ref2; transfers in every state, servers,
// chat rooms, hotlist, shared files, bandwidth curves). Every artist, title and user name is invented
// or a free work (public-domain compositions and texts, the Blender open films, Linux ISOs), see
// tools/check-content.sh. No network access at all.
//
// * Deterministic: a fixed random seed; the same actions give the same data.
// * frozen = true: no time-based changes at all (no timers; search results arrive at once), so
//   screenshots are reproducible (`octomx --demo --demo-frozen`, env MX_DEMO_FROZEN=1).
// * Settings: mx::SimpleSettings persisted to <profile>/settings.json (debounced) when a profile
//   directory is given; in-memory otherwise.
//
// Conventions the mock uses (and the UI understands) in SearchSource::extra:
//   "partialPercent" (int)  the source only has N % of the file  -> "25% of 109 878 998"
//   "queue"          (int)  the source's queue length when it has no free slot
//   "server"         (str)  server label for server-based networks ("[eD2k] DonkeyServer No1")
#pragma once

#include "mx/services.h"

namespace mx {

struct MockOptions {
    bool frozen = false;
    quint32 seed = 0x57A1C0DE;
};

Core *createMockCoreWithOptions(const QString &profileDir, const MockOptions &opt, QObject *parent = nullptr);
// [ui-g] tests: a demo Hotlist user (by name) goes online / offline; false when there is no such entry
bool mockSetHotlistOnline(Core *core, const QString &name, bool online);

} // namespace mx
