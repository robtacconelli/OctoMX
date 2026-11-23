// SPDX-License-Identifier: GPL-3.0-or-later
// [dl-1] Download order and preview rules shared by the engine, the BitTorrent plugin, the UI and
// the demo core: one place for the sizes, so every network and every window agrees.
//
// "Download In Sequential Order" and "Download First And Last Pieces First" (qBittorrent's two
// per-torrent flags) are priorities, never conditions: a source that has nothing in the wanted
// region still gives what it has.
//
// First and last: the head and the tail band of every wanted file go first. Each band is 1 % of the
// file, at least 4 MiB (the pieces covering it), and never more than a quarter of the file. 1 % is
// qBittorrent's allowance for an AVI index (idx1) at the end; 4 MiB covers the mp4 "moov" box of a
// feature film that was not written "fast start" (typically 0.1 ... 1 % of the file), the Matroska
// cues and a player's first probe at the head.
//
// Preview: a partly downloaded audio / video file can be played when its beginning is on disk
// (neededHead(): 1 % of the file, at least 4 MiB for video, 512 KiB for audio, capped at the file
// size) and, for containers whose index sits at the end, the end too:
//   * mp4 / m4v / mov / 3gp / m4a / m4b (ISO BMFF): the "moov" box. scanIsoBmff() walks the box
//     headers from offset 0: moov before mdat = "fast start", nothing more needed; mdat first =
//     the bytes from the end of mdat to the end of the file are needed.
//   * avi: the tail band (idx1); players can seek and do not ask to "repair" the file then.
//   * everything else (mkv, webm, mpg, ts, ogg, mp3, flac ...): the beginning is enough.
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>

#include <functional>

namespace mx {

namespace order {
qint64 headBandBytes(qint64 fileSize);
qint64 tailBandBytes(qint64 fileSize);
} // namespace order

namespace preview {

enum class Index { None = 0, Mp4 = 1, AtEnd = 2 };

bool isMediaFile(const QString &name);          // audio or video in the mxapi file-type table
bool isAudioFile(const QString &name);
Index indexKind(const QString &name);
qint64 neededHead(const QString &name, qint64 size);

// ISO BMFF top-level boxes. read(off, len) returns what is on disk (may be shorter); have(off, len)
// says whether those bytes were received. Result: tailFrom = -1 not decided yet (the head is not
// there or the box after mdat is not), 0 = nothing needed beyond the head (moov first), > 0 = the
// bytes [tailFrom, size) are needed.
struct Mp4Layout {
    bool   decided = false;
    qint64 tailFrom = -1;
};
Mp4Layout scanIsoBmff(qint64 size, const std::function<QByteArray(qint64, qint64)> &read,
                      const std::function<bool(qint64, qint64)> &have);

// The verdict for one file. headHeld / tailHeld: contiguous bytes on disk from offset 0 / ending
// at the end of the file. mp4: the layout above (ignored for other types).
struct Readiness {
    bool    media = false;
    bool    ready = false;
    qint64  needHead = 0;
    qint64  needTail = 0;      // bytes at the end that are needed (0 = none)
    QString reason;            // why not: "Preview: needs the beginning of the file" ...
};
Readiness evaluate(const QString &name, qint64 size, qint64 headHeld, qint64 tailHeld, const Mp4Layout &mp4 = {});

} // namespace preview
} // namespace mx
