// SPDX-License-Identifier: GPL-3.0-or-later
#include "mxhash/ed2k.h"

namespace mx::hash {

void Ed2kHasher::reset()
{
    m_md4.reset();
    m_inPart = 0;
    m_parts.clear();
}

void Ed2kHasher::update(const char *data, qint64 len)
{
    while (len > 0) {
        const qint64 take = qMin(len, PartSize - m_inPart);
        m_md4.update(data, take);
        m_inPart += take;
        data += take;
        len -= take;
        if (m_inPart == PartSize) {
            m_parts.append(m_md4.finish());
            m_md4.reset();
            m_inPart = 0;
        }
    }
}

QByteArray Ed2kHasher::finish()
{
    // Always close the current part, even when empty: that yields MD4("") for 0-byte files and
    // the trailing empty-part hash eMule appends for exact multiples of PartSize.
    m_parts.append(m_md4.finish());
    m_md4.reset();
    m_inPart = 0;
    return fileHashFromParts(m_parts);
}

QByteArray Ed2kHasher::fileHashFromParts(const QList<QByteArray> &parts)
{
    if (parts.isEmpty()) return Md4::hash(QByteArray());
    if (parts.size() == 1) return parts.first();
    Md4 h;
    for (const QByteArray &p : parts)
        h.update(p);
    return h.finish();
}

int Ed2kHasher::partCount(qint64 fileSize)
{
    return int(fileSize / PartSize) + 1;
}

int Ed2kHasher::dataPartCount(qint64 fileSize)
{
    if (fileSize <= 0) return 0;
    return int((fileSize + PartSize - 1) / PartSize);
}

} // namespace mx::hash
