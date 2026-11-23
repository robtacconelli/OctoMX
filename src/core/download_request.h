// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX core: helpers that turn search hits and links into DownloadRequests (shared by every
// DownloadEngine implementation).
#pragma once

#include "mx/types.h"

namespace mx {

// Fills hashes / name / size / initial sources from req.uri when it is a magnet: or ed2k:// link
// (via mxhash links), an http(s)/ftp URL (name from the path, an Http source), or a local .torrent
// path (name from the file name; hashes stay empty, the BitTorrent backend parses it).
// Values already present in req win over values from the link. *error is set and false returned
// for an unusable request (no identity, no uri, no sources).
bool resolveDownloadRequest(DownloadRequest &req, QString *error = nullptr);

// Request for a (merged) search hit: name, size, hashes, all sources, bundle files, uri of the first
// source that has one.
DownloadRequest requestFromHit(const SearchHit &hit, const QString &saveDir = QString());

// A short stable id for a new download ("d" + 16 hex chars).
QString newDownloadId();

} // namespace mx
