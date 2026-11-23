// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX core: search hygiene: recognise eD2k "bridge" entries and likely fakes.
//
// * Bridge entries: some eD2k servers (emule-qt.org eNodes) answer keyword searches with
//   "[torrent emule-qt.org] <name>" / "[kad emule-qt.org] <name>" rows that carry synthetic file
//   hashes (ed2b0111ffff...). No eD2k peer has such a file, so they are dropped (search/dropTorrentBridges).
// * Suspects are only FLAGGED (SearchHit::suspect = reason) unless search/hideSuspects is on:
//   executables / archives wrapped around media names, a network-reported type that contradicts the
//   extension, sizes that cannot hold the claimed media, classic fake markers (WMV/ASF "movies",
//   "password"/"codec required" bait), and, across one search, the same eD2k complete-source
//   count on many different files (server-side fake farms).
#pragma once

#include <QtCore/QHash>
#include <QtCore/QSet>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include "mx/types.h"

namespace mx::hygiene {

// True for an eD2k/Kad bridge entry (tagged name or synthetic hash). Only meaningful for hits
// reported by the ED2K / Kad plugins.
bool isTorrentBridge(const SearchHit &h);
bool isSyntheticEd2kHash(const QByteArray &ed2k);

// Reason why the hit looks fake, or an empty string. `reportedType` is the MediaType the network
// claimed for the file (MediaType::Any = none).
QString suspectReason(const SearchHit &h, MediaType reportedType = MediaType::Any);

// Media columns of a network hit (eD2k / Kad / Gnutella tags, overlay fields): length in seconds,
// bitrate in kbps, sample rate in Hz. Drops values no file can have, and a bitrate on anything but
// audio or video. For a video with a known size and length, a bitrate that is off by more than a
// factor of 8 from the size per second is a unit error (bps, Mbps) and dropped: never replaced by a
// value derived from the size, the cell stays empty.
void sanitizeMedia(SearchHit &h);

// Combines reasons ("a; b"), without duplicates.
QString addReason(const QString &existing, const QString &reason);

// Cross-hit detector for constant eD2k complete-source counts within one search: feed every eD2k
// hit; returns the keys (as given to add()) that are now suspect because the same seeds value
// (>= minSeeds) was reported for >= minFiles different files; when the file count grows all keys of
// that value are returned again (reasonFor() names the count).
class ConstantSourceCountDetector {
public:
    explicit ConstantSourceCountDetector(int minSeeds = 20, int minFiles = 8) : m_minSeeds(minSeeds), m_minFiles(minFiles) {}
    QStringList add(const QString &key, const QByteArray &fileId, int seeds);
    QString reasonFor(int seeds) const;
    int flaggedValue(const QString &key) const { return m_flagged.value(key, -1); }

private:
    int m_minSeeds, m_minFiles;
    QHash<int, QSet<QByteArray>> m_files;             // seeds value → distinct files
    QHash<int, QStringList> m_keys;                   // seeds value → hit keys seen with it
    QHash<QString, int> m_flagged;                    // key → seeds value
};

} // namespace mx::hygiene
