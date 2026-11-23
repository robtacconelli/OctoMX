// SPDX-License-Identifier: GPL-3.0-or-later
// Gnutella module: HUGE (Hash/URN Gnutella Extensions) URN strings and GGEP "H" values.
#pragma once

#include "mx/types.h"

namespace mx::gnut {

// Parses "urn:sha1:<32 base32>", "urn:bitprint:<sha1 b32>.<tth b32>", "urn:tree:tiger/:<39 b32>",
// "urn:tree:tiger:<b32>", "urn:ttroot:<b32>" (case-insensitive; "sha1:<..>" also accepted).
// Merges what it finds into *h; returns true if at least one hash was recognised.
bool parseUrn(const QString &urn, FileHashes *h);
QString sha1Urn(const QByteArray &sha1);         // "urn:sha1:XXXX" (base32)
QString tthUrn(const QByteArray &tth);           // "urn:tree:tiger/:XXXX"
QString bitprintUrn(const QByteArray &sha1, const QByteArray &tth);

// GGEP "H": first byte = type (1 = SHA-1, 2 = bitprint SHA-1 + TTH, 3 = MD5, 4 = UUID, 5 = MD4).
bool parseGgepH(const QByteArray &data, FileHashes *h);
QByteArray encodeGgepH(const QByteArray &sha1, const QByteArray &tth = QByteArray());

} // namespace mx::gnut
