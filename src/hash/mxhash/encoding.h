// SPDX-License-Identifier: GPL-3.0-or-later
// Base32 (RFC 4648) and hex helpers for hash values.
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>

namespace mx::hash {

QString toBase32(const QByteArray &raw);                      // upper case, no padding
QByteArray fromBase32(const QString &s, bool *ok = nullptr);  // case-insensitive, ignores '=' padding
QString toHex(const QByteArray &raw);                         // lower case
QByteArray fromHex(const QString &s, bool *ok = nullptr);     // strict: even length, hex digits only
// Accepts a hash written either as hex (2*bytes chars) or base32 (ceil(bytes*8/5) chars) and
// returns the raw value if it has exactly `bytes` bytes, else an empty array.
QByteArray decodeHash(const QString &s, int bytes);

} // namespace mx::hash
