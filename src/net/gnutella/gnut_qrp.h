// SPDX-License-Identifier: GPL-3.0-or-later
// Gnutella module: Query Routing Protocol (QRP 0.1): the keyword hash function, the leaf's
// table built from shared file names, and RESET/PATCH message encoding (plus a decoder used by
// the tests' ultrapeer stub).
#pragma once

#include <QtCore/QBitArray>
#include <QtCore/QByteArray>
#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QStringList>

namespace mx::gnut {

// The QRP hash of a keyword (lower-cased per character, "& 0xff" of each code point).
quint32 qrpHash(const QString &s, int bits);
// Words inserted for a file name: every keyword (>= 1 char) plus its prefixes obtained by cutting
// up to 2 trailing characters while at least 3 remain (our own choice; QRP leaves prefix
// handling to the implementation).
QStringList qrpWordsForName(const QString &fileName);

class QrpTable {
public:
    explicit QrpTable(int nslots = 65536);         // power of two
    int nslots() const { return m_slots; }
    int bits() const { return m_bits; }
    void clear();
    void addWord(const QString &w);
    void addFileName(const QString &name);
    void addUrn(const QString &urn);              // "urn:sha1:XXXX"
    void setSlot(int i) { if (i >= 0 && i < m_slots && !m_present.testBit(i)) { m_present.setBit(i); ++m_filled; } }
    bool containsHash(quint32 h) const { return h < quint32(m_slots) && m_present.testBit(int(h)); }
    bool containsWord(const QString &w) const { return containsHash(qrpHash(w, m_bits)); }
    // true if every keyword of `query` is present (how ultrapeers route to leaves)
    bool matchesQuery(const QString &query) const;
    int filled() const { return m_filled; }
    bool operator==(const QrpTable &o) const { return m_slots == o.m_slots && m_present == o.m_present; }
    bool operator!=(const QrpTable &o) const { return !(*this == o); }

    // Messages (QRP payloads, without the Gnutella header): one RESET then PATCH chunks.
    // entryBits 4 or 8 (signed deltas vs. an empty table); compressed with zlib when `compress`.
    QList<QByteArray> encode(int infinity = 7, int entryBits = 4, bool compress = true, int chunkSize = 4096) const;
    const QBitArray &present() const { return m_present; }

private:
    int m_slots;
    int m_bits;
    QBitArray m_present;
    int m_filled = 0;
};

// Receiving side (ultrapeer view of a leaf's table): RESET + PATCH sequences.
class QrpReceiver {
public:
    // Returns false on a protocol error (bad variant, sequence, sizes, inflate failure, > caps).
    bool handle(const QByteArray &payload, QString *error = nullptr);
    bool complete() const { return m_complete; }   // a full patch sequence was applied
    const QrpTable *table() const { return m_complete ? &m_table : nullptr; }
    int nslots() const { return m_table.nslots(); }
private:
    bool applyPatchData(const QByteArray &raw, QString *error);
    QrpTable m_table{64};
    int m_infinity = 7;
    QByteArray m_values;      // one byte per slot (current hop values)
    int m_seqSize = 0, m_nextSeq = 1, m_entryBits = 0, m_compressor = 0;
    QByteArray m_pending;     // concatenated patch bytes of the current sequence
    bool m_haveReset = false;
    bool m_complete = false;
};

} // namespace mx::gnut
