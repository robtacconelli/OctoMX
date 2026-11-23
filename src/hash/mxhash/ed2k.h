// SPDX-License-Identifier: GPL-3.0-or-later
// eD2k file hash (eMule convention).
//  * The file is cut into parts of 9 728 000 bytes; each part is MD4-hashed (the "hashset").
//  * A file of one part (size < PartSize, including 0 bytes): file hash = MD4(data) and the
//    hashset is that single hash.
//  * Otherwise file hash = MD4(concatenation of the part hashes).
//  * If the size is an exact non-zero multiple of PartSize, eMule appends the MD4 of an EMPTY part
//    (31d6cfe0...) to the hashset before computing the file hash (so 9 728 000 bytes → 2 entries).
#pragma once

#include "mxhash/hasher.h"

#include <QtCore/QList>

namespace mx::hash {

class Ed2kHasher : public Hasher {
public:
    static constexpr qint64 PartSize = 9728000;
    Ed2kHasher() { reset(); }
    void update(const char *data, qint64 len) override;
    using Hasher::update;
    QByteArray finish() override;                 // 16-byte eD2k file hash
    void reset() override;
    // Valid after finish(): MD4 per part (see header comment; ≥ 1 entry).
    QList<QByteArray> partHashes() const { return m_parts; }
    static QByteArray fileHashFromParts(const QList<QByteArray> &parts); // 1 part → it; else MD4(concat)
    static int partCount(qint64 fileSize);       // fileSize / PartSize + 1 (hashset entries)
    // Number of real data parts (for part maps / availability): ceil(size / PartSize), ≥ 1 if size > 0.
    static int dataPartCount(qint64 fileSize);
private:
    Md4 m_md4;
    qint64 m_inPart = 0;
    QList<QByteArray> m_parts;
};

} // namespace mx::hash
