// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX engine: internal bookkeeping types shared by Download, the stores and the scheduler.
#pragma once

#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QPointer>
#include <QtCore/QString>

#include "core/engine/range_set.h"
#include "mx/plugin.h"

namespace mx::engine {

// Tunables. Defaults are the production values; tests shrink the timeouts through
// DownloadEngine::setTuning() (engine.h).
struct Tuning {
    int     requestTimeoutMs = 60000;      // no data for this long on an outstanding request → cancel
    int     slowRateBps = 2048;            // "slow" source ...
    int     slowForMs = 30000;             // ... for this long while others are much faster
    int     slowHoldMs = 60000;            // a replaced slow source gets no new chunk for this long
    int     reservationIdleMs = 45000;     // a reserved piece without progress for this long → returned
    int     reservationMaxMs = 180000;     // hard cap for a reservation
    int     sourceRetryBaseMs = 30000;     // reopen a failed source after base·2^n (capped 30 min)
    int     maxOpenSourcesPerDownload = 40;
    int     maxOpenSourcesGlobal = 400;
    int     persistIntervalMs = 10000;     // engine-state blob written at least this often when dirty
    int     findSourcesBaseMs = 0;         // 0 = transfers/sourceSearchIntervalMin
    qint64  maxBufferedBytes = 96LL << 20; // write backlog + inject assemblies (all downloads)
    double  maxReservedShare = 0.30;       // of the missing pieces, while libtorrent has peers
    int     reserveHorizonSec = 30;        // reserve what the foreign sources deliver in this time
    int     endgamePercent = 2;
    int     endgameChunks = 4;
    int     strikesToBan = 3;
    // [engine-4] candidates (size+name / hint-only sources) of a download without a fine hash set
    // are tried provisionally when no verifiable source served anything for this long
    int     provisionalGraceMs = 45000;
    int     nameSearchDelayMs = 3000;      // first background size+name search after activation
    int     afsMinuteMs = 60000;           // one "minute" of Auto Find Sources (tests shrink it)
    int     deadEndMs = 5 * 60 * 1000;     // [set-1] no data for this long and every source failed: an error
    // [merge-1] sources matched only by name and size (candidates) of a network that no verified unit
    // confirmed yet: at most this many units of the judge in flight / unverified at once (the waste
    // cap), and that network is dropped from the download after this many different sources failed
    // the check (or when every candidate of it failed)
    int     candidateUnitsInFlight = 2;
    int     candidateNetFailures = 3;
    int     nameSearchMax = 12;            // background size+name searches per session
    // [ver-1] Verify File: a pause between two checked blocks (0 = none; tests slow a check down to stop
    // it in the middle), and how long a damaged file whose only checksum covers the whole file waits for
    // the networks to send block hashes that locate the damage
    int     verifyBlockPauseMs = 0;
    int     verifyHashSetWaitMs = 3 * 60 * 1000;
    // [str-1] a stream whose torrent gives no file list goes on as a part file from the other networks
    // after this (a normal download waits transfers/metadataTimeoutSec)
    int     streamMetadataFallbackMs = 10000;
};

// [merge-1] eD2k and Kad serve the same copy (one hash family); every other network is its own
inline int netFamily(Net n) { return n == Net::Kad ? int(Net::ED2K) : int(n); }

// Hash kinds a network can look sources up with (used to decide whether a source found by a
// plugin was found through a trusted hash or only through an unconfirmed hint).
inline bool netUsesHashOf(Net n, const FileHashes &h)
{
    switch (n) {
    case Net::ED2K:
    case Net::Kad:        return !h.ed2k.isEmpty();
    case Net::Gnutella:   return !h.sha1.isEmpty() || !h.tth.isEmpty();
    case Net::BitTorrent: return !h.btih.isEmpty();
    case Net::Overlay:    // libraries are indexed by file hashes; a btih alone finds nothing there
        return !h.ed2k.isEmpty() || !h.sha1.isEmpty() || !h.tth.isEmpty() || !h.sha256.isEmpty()
               || !h.md5.isEmpty() || !h.aich.isEmpty();
    default:              return true;   // HTTP & co: the URL is the identity
    }
}

struct Request {
    qint64 off = 0;          // store coordinates
    qint64 len = 0;
    qint64 got = 0;          // contiguous bytes received from off
    qint64 startMs = 0;
    qint64 lastDataMs = 0;
    bool   sequential = false;   // whole-prefix request of a no-range source (data before the gap is ignored)
    qint64 end() const { return off + len; }
};

// Bytes per second over a sliding window of whole seconds.
class RateMeter {
public:
    void add(qint64 nowMs, qint64 bytes);
    int rate(qint64 nowMs, int windowSec = 10) const;   // bytes/s
    qint64 lastActivityMs() const { return m_lastMs; }
private:
    static constexpr int N = 32;
    qint64 m_sec[N] = {};
    qint64 m_bytes[N] = {};
    qint64 m_lastMs = 0;
};

// One known source of a download (open or not).
struct SourceSlot {
    QString      key;                 // dedupe key (net + user id / address / uri)
    SearchSource desc;
    QPointer<RangeSource> rs;         // open RangeSource (owned by the engine) or null
    NetworkPlugin *plugin = nullptr;
    int          fileIndex = -1;      // torrent store: file the source serves (-1 = unmapped)
    qint64       base = 0;            // store offset of the source's file start
    qint64       span = -1;           // file size in store coordinates (-1 = whole/unknown)
    bool         trusted = true;      // false: candidate / found only through a hash-link hint
    FileHashes   claimed;             // what an untrusted source claims (confirmed on a verified unit)
    bool         banned = false;
    int          strikes = 0;
    int          failures = 0;        // consecutive open / request failures
    qint64       retryAtMs = 0;       // earliest reopen
    qint64       openedMs = 0;
    QList<Request> reqs;
    RateMeter    rate;
    qint64       bytes = 0;           // accepted bytes (this session)
    qint64       verifiedBytes = 0;   // bytes of verified units this source contributed
    RangeSet     contributed;         // store coordinates, not yet verified
    qint64       cancelGraceUntilMs = 0;   // late data after cancelRequests() is ignored until then
    qint64       slowSinceMs = 0;
    qint64       holdUntilMs = 0;
    qint64       lastReplacedMs = 0;
    bool         isTorrentLink = false;    // Net::BitTorrent: contributes the magnet only
    int          priority = 0;             // [glue-2] user priority -1/0/1 (open + chunk order)
    bool         removedByUser = false;    // [glue-2] TransferService::cancelSource
    // [engine-4] reporting (every known source is a row in the Transfers tree)
    int          order = 0;                // insertion order (stable rows)
    QString      lastError;                // why it was last closed / banned (original WinMX wording)
    int          queueRank = 0;            // last remote queue rank seen (0 = none)
    bool         aeqParked = false;        // left a remote queue longer than the AEQ limit
    bool         candidateFromName = false; // found by the background size+name search
    bool         wasCandidate = false;      // [merge-1] matched only by name and size (shown until confirmed)
    bool         ignoreHashSets = false;    // [up-1] it sent a hash set that proved wrong: never use its sets again
    int          orderedPicks = 0;         // [dl-1] requests chosen by the download order (rare-part protection)
    bool         sequentialOnly() const;
    bool         isOpen() const { return !rs.isNull(); }
    RangeSet     inflight() const;
};

QString sourceKey(const SearchSource &s);
qint64 nowMs();                       // monotonic milliseconds (process-wide clock)

} // namespace mx::engine
