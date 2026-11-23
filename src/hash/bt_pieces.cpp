// SPDX-License-Identifier: GPL-3.0-or-later
#include "mxhash/bt_pieces.h"

namespace mx::hash {

BtPieceHasher::BtPieceHasher(qint64 pieceLength)
    : m_pieceLength(pieceLength > 0 ? pieceLength : 256 * 1024)
    , m_sha(QCryptographicHash::Sha1)
{
}

void BtPieceHasher::reset()
{
    m_sha.reset();
    m_inPiece = 0;
    m_pieces.clear();
}

void BtPieceHasher::update(const char *data, qint64 len)
{
    while (len > 0) {
        const qint64 take = qMin(len, m_pieceLength - m_inPiece);
        m_sha.addData(QByteArrayView(data, take));
        m_inPiece += take;
        data += take;
        len -= take;
        if (m_inPiece == m_pieceLength) {
            m_pieces.append(m_sha.result());
            m_sha.reset();
            m_inPiece = 0;
        }
    }
}

QByteArray BtPieceHasher::finish()
{
    if (m_inPiece > 0) {
        m_pieces.append(m_sha.result());
        m_sha.reset();
        m_inPiece = 0;
    }
    QByteArray all;
    all.reserve(m_pieces.size() * 20);
    for (const QByteArray &p : m_pieces)
        all += p;
    return all;
}

QByteArray BtPieceHasher::hashPiece(const QByteArray &data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha1);
}

qint64 BtPieceHasher::suggestedPieceLength(qint64 size)
{
    qint64 len = 16 * 1024;
    while (len < 16 * 1024 * 1024 && size / len > 2000)
        len *= 2;
    return len;
}

} // namespace mx::hash
