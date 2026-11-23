// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX web module: value transforms used by the provider definitions (private).
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QDateTime>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include "mx/types.h"

namespace mx::web {

// "1.4 GiB", "650.55 MB", "4.44GB", "1,234 KB", "123456", "1.2e9" → bytes. Units without "i" are
// binary (1024) unless `decimal` is set: torrent sites almost always mean binary units. -1 if invalid.
qint64 parseSize(const QString &text, bool decimal = false);

// RFC 4648 base32 (case-insensitive, padding optional). Empty on error.
QByteArray base32Decode(const QString &text);

// 40 hex chars or 32 base32 chars (optionally prefixed "urn:btih:") → 20 raw bytes. An all-zero hash
// is rejected (apibay's "No results returned" row). Empty if invalid.
QByteArray parseInfoHash(const QString &text);

// The first urn:btih of a magnet link (20 raw bytes) or empty.
QByteArray infoHashFromMagnet(const QString &magnet);
// A query parameter of a magnet link (first occurrence, percent-decoded): dn, xl, tr...
QString magnetParam(const QString &magnet, const QString &key);
QStringList magnetParams(const QString &magnet, const QString &key);

// Dates: unix seconds / milliseconds, ISO 8601 ("2026-04-25T15:35:00+00:00", "2026-11-05 15:43:48"),
// RFC 2822 ("Tue, 03 Nov 2009 07:03:00 -0000", also without the space after the comma or without
// the weekday), "Oct 25, 2026", "2026-11-05". `format` (Qt date format) is tried first if given.
// Invalid QDateTime on failure. Results are UTC.
QDateTime parseDate(const QString &text, const QString &format = QString());

// Remove tags, decode the common HTML entities, collapse whitespace.
QString stripHtml(const QString &html);
QString decodeHtmlEntities(const QString &text);

// magnet:?xt=urn:btih:<hex>&dn=<name>[&xl=<size>]&tr=... (trackers de-duplicated, order kept).
QString buildMagnet(const QByteArray &btih, const QString &name, qint64 size, const QStringList &trackers);

// The bundle rule: > 1 file → bundle; exactly 1 → single file; unknown (-1/0) → bundle when the name
// has no known file extension (mx::mediaTypeForFileName() == Other), i.e. it looks like a folder name.
bool guessIsBundle(int numFiles, const QString &name);

// Undo the common "UTF-8 decoded as Windows-1252" double encoding ("Ð‘Ð¾" → "Бо"). Returns the
// text unchanged unless it re-encodes to cp1252 bytes that form valid, shorter UTF-8.
QString repairMojibake(const QString &text);

// Lower-case, every non-letter/digit → space, collapsed. Used for keyword matching.
QString normalizeForMatch(const QString &text);
// Keywords of a query (normalised words; empty words dropped).
QStringList queryWords(const QString &text);

} // namespace mx::web
