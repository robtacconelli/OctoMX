// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX core: best-effort audio metadata for the original WinMX "Time / Bitrate / Freq" columns.
// MP3 (MPEG 1/2/2.5 layer I-III frame headers, Xing/Info and VBRI VBR headers, ID3v2/ID3v1 skipped),
// WAV (RIFF fmt/data), FLAC (STREAMINFO) and Ogg Vorbis/Opus (id header + last granule).
// Anything else (or unparsable) stays -1: video containers are not parsed, and a bitrate is never
// derived from the file size and a guessed length. Values an audio file cannot have (a bitrate
// below 8 or above 9216 kbps, a length of more than a week) are dropped as parse errors.
// Reads at most a few hundred KiB; safe in worker threads.
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>

#include "mx/types.h"

namespace mx {

struct MediaInfo {
    int durationSec = -1;
    int bitrateKbps = -1;
    int sampleRateHz = -1;
    bool isValid() const { return durationSec >= 0 || bitrateKbps >= 0 || sampleRateHz >= 0; }
};

MediaInfo readMediaInfo(const QString &path);
// Parse an MP3 given its first bytes and total file size (exposed for tests).
MediaInfo parseMp3(const QByteArray &head, qint64 fileSize, bool hasId3v1 = false);
// Parse an Ogg file from its first and last bytes, with the same bounds as readMediaInfo (tests).
MediaInfo parseOggForTest(const QByteArray &head, const QByteArray &tail, qint64 fileSize);
// Clears length / bitrate / sample rate of a file whose type cannot carry them (anything but
// audio: the scanner only reads audio files, older versions may have cached other values).
void dropNonAudioMedia(MediaType type, int *durationSec, int *bitrateKbps, int *sampleRateHz);

} // namespace mx
