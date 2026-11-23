// SPDX-License-Identifier: GPL-3.0-or-later
// Compute several file hashes in ONE pass over the data (used by the library hasher and by the
// download engine on completion).
#pragma once

#include "mx/types.h"

#include <QtCore/QList>
#include <QtCore/QString>
#include <atomic>
#include <functional>
#include <memory>

namespace mx::hash {

class Ed2kHasher;
class AichHasher;
class TigerTree;
class BtPieceHasher;
class CryptoHasher;

class MultiHasher {
public:
    enum Kind { Ed2k = 0x01, Aich = 0x02, Sha1 = 0x04, Tth = 0x08, Sha256 = 0x10, Md5 = 0x20, BtPieces = 0x40,
                Default = Ed2k | Aich | Sha1 | Tth | Sha256 };   // what the library computes
    explicit MultiHasher(int kinds = Default, qint64 btPieceLength = 0);
    ~MultiHasher();
    MultiHasher(const MultiHasher &) = delete;
    MultiHasher &operator=(const MultiHasher &) = delete;

    void update(const char *data, qint64 len);
    void update(const QByteArray &b) { update(b.constData(), b.size()); }
    void finish();                                  // finish all; call once
    void reset();
    int kinds() const { return m_kinds; }

    mx::FileHashes hashes() const;                  // ed2k, aich, sha1, tth, sha256, md5 per kinds
    QList<QByteArray> ed2kPartHashes() const;
    QList<QByteArray> aichPartHashes() const;
    QList<QByteArray> tthLevel(qint64 unitBytes) const;
    QList<QByteArray> btPieces() const;
    qint64 bytesHashed() const { return m_bytes; }

    // One-pass hash of a whole file with a 2 MiB read buffer. cancel may be null; progress(bytesDone)
    // is called every ~8 MiB (and at the end) from the calling thread. Returns false on read error
    // or cancel (*error set). finish() has been called when it returns true.
    static bool hashFile(const QString &path, MultiHasher &h, const std::atomic<bool> *cancel = nullptr,
                         const std::function<void(qint64)> &progress = {}, QString *error = nullptr);
private:
    int m_kinds;
    qint64 m_btPieceLength;
    qint64 m_bytes = 0;
    bool m_finished = false;
    std::unique_ptr<Ed2kHasher> m_ed2k;
    std::unique_ptr<AichHasher> m_aich;
    std::unique_ptr<TigerTree> m_tth;
    std::unique_ptr<CryptoHasher> m_sha1, m_sha256, m_md5;
    std::unique_ptr<BtPieceHasher> m_bt;
    mx::FileHashes m_result;
};

} // namespace mx::hash
