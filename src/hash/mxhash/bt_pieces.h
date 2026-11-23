// SPDX-License-Identifier: GPL-3.0-or-later
// BitTorrent v1 piece hashes of a single file (SHA-1 per `pieceLength` bytes, last piece short).
// Used to pre-verify blocks fetched from other networks and to create single-file torrents.
#pragma once

#include "mxhash/hasher.h"

#include <QtCore/QList>

namespace mx::hash {

class BtPieceHasher : public Hasher {
public:
    explicit BtPieceHasher(qint64 pieceLength);
    void update(const char *data, qint64 len) override;
    using Hasher::update;
    QByteArray finish() override;                 // concatenation of all 20-byte piece SHA-1s
    void reset() override;
    QList<QByteArray> pieces() const { return m_pieces; }
    qint64 pieceLength() const { return m_pieceLength; }
    static QByteArray hashPiece(const QByteArray &data);   // SHA-1
    // libtorrent-like default piece size for a file of `size` bytes (16 KiB ... 16 MiB, ~1000-2000 pieces).
    static qint64 suggestedPieceLength(qint64 size);
private:
    qint64 m_pieceLength;
    qint64 m_inPiece = 0;
    QCryptographicHash m_sha;
    QList<QByteArray> m_pieces;
};

} // namespace mx::hash
