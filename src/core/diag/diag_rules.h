// SPDX-License-Identifier: GPL-3.0-or-later
// [diag-1] Connection Test rules: pure functions that turn counters into a level and one plain
// sentence (per download, per network, for the report summary). No Qt objects, no I/O: unit tested
// in tests/core/tst_diag.cpp.
#pragma once

#include <QtCore/QHash>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include "mx/diagnostics.h"
#include "mx/plugin.h"

namespace mx::diag {

struct Verdict {
    DiagLevel level = DiagLevel::Unknown;
    QString   sentence;          // empty = nothing to say (healthy, paused, finished)
};

// What the download engine knows about one download (engine/download.cpp fills it).
struct DownloadFacts {
    QString id, name;
    TransferState state = TransferState::Queued;
    bool    active = false;      // has an engine slot (not Locally Queued)
    bool    paused = false, finished = false, error = false;
    bool    isTorrent = false;   // a libtorrent store is attached
    QString torrentId;
    bool    fetchingTorrentFile = false;   // a .torrent URL is being fetched
    int     indexSeeds = -1, indexPeers = -1;
    QString indexName;
    qint64  size = 0, done = 0;
    int     rateBps = 0;
    qint64  activeSec = 0;       // seconds since the download got its engine slot (this session)
    // non-BitTorrent sources (RangeSources)
    int     sourcesKnown = 0, sourcesOpen = 0, sourcesQueued = 0, sourcesTransferring = 0;
    int     sourcesBanned = 0, sourcesFailed = 0, bestQueueRank = 0;
    QHash<QString, int> failReasons;   // lastError text -> count
    QHash<int, int> knownByNet;        // int(Net) -> sources
};

// The environment the rules need.
struct DownloadContext {
    int  activeDownloads = 0;    // downloads holding an engine slot
    int  maxDownloads = 0;       // transfers/maxDownloads
    int  btActiveLimit = 0;      // bt/maxActiveDownloads
    int  downLimitKBs = 0;       // the download limit in force now (0 = unlimited)
    bool weFirewalledEd2k = false;   // eD2k LowID
    bool ed2kOrKadUp = false;
};

Verdict torrentVerdict(const DownloadFacts &f, const TorrentDiag &t, const DownloadContext &c);
Verdict rangeVerdict(const DownloadFacts &f, const DownloadContext &c);
// torrentVerdict when t.valid, else rangeVerdict
Verdict downloadVerdict(const DownloadFacts &f, const TorrentDiag &t, const DownloadContext &c);

// Failure reason buckets for libtorrent / socket errors ("timed out", "refused", "handshake",
// "banned", "other"), case-insensitive on the error message text.
QString failureBucket(const QString &errorText);

// Report summary from the groups (General first, then networks, then downloads).
Verdict reportSummary(const QList<DiagGroup> &groups);
// The router's WAN address (and the addresses other networks see): "", "double NAT" or
// "carrier-grade NAT" with a level and an explanation.
struct NatVerdict {
    DiagLevel level = DiagLevel::Unknown;
    QString   kind;              // "", "double NAT", "carrier-grade NAT", "address mismatch"
    QString   text;
};
NatVerdict natVerdict(const QString &routerWan, const QStringList &seenExternal);

// What the router discovery tried when nothing answered, and the evidence around it.
struct DiscoveryFacts {
    QStringList adapters;            // "WiFi 192.168.1.20" per adapter searched
    QList<int>  answers;             // SSDP answers per adapter (any device)
    QString gateway;
    bool    unicastTried = false;
    bool    searchSkipped = false;
    bool    cacheTried = false;
    bool    notifyListening = false;
    bool    upnpOn = true, natPmpOn = true;
    QString natPmpResult;            // "no answer from 192.168.1.1"
    bool    winTried = false;
    bool    winFoundNone = false;    // the Windows UPnP service says it knows no router
    QString winResult;
    QString defaultAdapter;          // the adapter of the internet route
    bool    defaultIsVirtual = false;
    bool    firewallChecked = false, firewallOn = false;
    QString firewallProfiles;        // "Public", "Private, Domain"
    int     firewallAllowRules = 0;
};
// Plain words: what was tried, the likely reason (only from evidence) and what to do.
QString discoveryFailureText(const DiscoveryFacts &f);

// Looks like a VPN / virtual adapter (by name and description).
bool looksVirtualAdapter(const QString &name, const QString &humanName, QString *why = nullptr);

} // namespace mx::diag
