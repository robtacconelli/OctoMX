// SPDX-License-Identifier: GPL-3.0-or-later
#include "gnut_qrp.h"
#include "gnut_util.h"
#include "gnut_zlib.h"

#include <QtCore/QSet>

namespace mx::gnut {

quint32 qrpHash(const QString &s, int bits)
{
    // UTF-16 code units (as LimeWire's Java implementation), each lower-cased on its own
    quint32 x = 0;
    int j = 0;
    for (QChar ch : s) {
        const quint32 b = quint32(ch.isSurrogate() ? ch.unicode() : ch.toLower().unicode()) & 0xff;
        x ^= b << (j * 8);
        j = (j + 1) & 3;
    }
    const quint32 prod = x * 0x4F1BBCDCu;            // low 32 bits of the 64-bit product
    if (bits <= 0) return 0;
    if (bits >= 32) return prod;
    return prod >> (32 - bits);
}

QStringList qrpWordsForName(const QString &fileName)
{
    QStringList out;
    QSet<QString> seen;
    for (const QString &w : splitKeywords(fileName)) {
        if (w.size() > 64) continue;
        if (!seen.contains(w)) { seen.insert(w); out << w; }
        QString cut = w;
        for (int i = 0; i < 2 && cut.size() > 3; ++i) {
            cut.chop(1);
            if (!seen.contains(cut)) { seen.insert(cut); out << cut; }
        }
    }
    return out;
}


static int log2i(int v) { int b = 0; while ((1 << b) < v) ++b; return b; }

QrpTable::QrpTable(int nslots)
{
    m_bits = log2i(qBound(64, nslots, 1 << 21));
    m_slots = 1 << m_bits;
    m_present.resize(m_slots);
}

void QrpTable::clear() { m_present.fill(false); m_filled = 0; }

void QrpTable::addWord(const QString &w)
{
    if (w.isEmpty()) return;
    const quint32 h = qrpHash(w, m_bits);
    if (!m_present.testBit(int(h))) { m_present.setBit(int(h)); ++m_filled; }
}

void QrpTable::addFileName(const QString &name)
{
    for (const QString &w : qrpWordsForName(name)) addWord(w);
}

void QrpTable::addUrn(const QString &urn) { addWord(urn); }

bool QrpTable::matchesQuery(const QString &query) const
{
    const QStringList words = splitKeywords(query);
    if (words.isEmpty()) return false;
    for (const QString &w : words)
        if (!containsWord(w)) return false;
    return true;
}

QList<QByteArray> QrpTable::encode(int infinity, int entryBits, bool compress, int chunkSize) const
{
    QList<QByteArray> msgs;
    QByteArray reset;
    putU8(reset, 0);                                  // RESET
    putU32(reset, quint32(m_slots));
    putU8(reset, quint8(infinity));
    msgs << reset;

    if (entryBits != 8) entryBits = 4;
    const int delta = 1 - infinity;                   // from "infinity" to 1 hop
    QByteArray raw;
    if (entryBits == 4) {
        raw.fill('\0', m_slots / 2);
        const quint8 nib = quint8(delta & 0x0f);
        for (int i = 0; i < m_slots; ++i) {
            if (!m_present.testBit(i)) continue;
            quint8 &byte = reinterpret_cast<quint8 &>(raw[i / 2]);
            if (i & 1) byte |= nib; else byte |= quint8(nib << 4);
        }
    } else {
        raw.fill('\0', m_slots);
        for (int i = 0; i < m_slots; ++i)
            if (m_present.testBit(i)) raw[i] = char(qint8(delta));
    }
    QByteArray data = compress ? deflateAll(raw) : raw;
    if (data.isEmpty()) { data = raw; compress = false; }
    chunkSize = qMax(256, chunkSize);
    int n = int((data.size() + chunkSize - 1) / chunkSize);
    if (n > 255) { chunkSize = int((data.size() + 254) / 255); n = int((data.size() + chunkSize - 1) / chunkSize); }
    for (int i = 0; i < n; ++i) {
        QByteArray p;
        putU8(p, 1);                                  // PATCH
        putU8(p, quint8(i + 1));
        putU8(p, quint8(n));
        putU8(p, compress ? 1 : 0);
        putU8(p, quint8(entryBits));
        p.append(data.mid(i * chunkSize, chunkSize));
        msgs << p;
    }
    return msgs;
}

bool QrpReceiver::handle(const QByteArray &payload, QString *error)
{
    const auto fail = [&](const QString &e) { if (error) *error = e; m_pending.clear(); m_nextSeq = 1; return false; };
    Reader r(payload);
    const int variant = r.u8();
    if (!r.ok()) return fail(QStringLiteral("empty QRP message"));
    if (variant == 0) {
        const quint32 len = r.u32();
        const int inf = r.u8();
        if (!r.ok()) return fail(QStringLiteral("short RESET"));
        if (len < 64 || len > (1u << 21) || (len & (len - 1))) return fail(QStringLiteral("bad table size %1").arg(len));
        if (inf < 1 || inf > 127) return fail(QStringLiteral("bad infinity"));
        m_table = QrpTable(int(len));
        m_infinity = inf;
        m_values.fill(char(inf), int(len));
        m_haveReset = true;
        m_complete = false;
        m_pending.clear();
        m_nextSeq = 1;
        m_seqSize = 0;
        return true;
    }
    if (variant != 1) return fail(QStringLiteral("unknown QRP variant %1").arg(variant));
    if (!m_haveReset) return fail(QStringLiteral("PATCH before RESET"));
    const int seqNo = r.u8(), seqSize = r.u8(), compressor = r.u8(), bits = r.u8();
    if (!r.ok()) return fail(QStringLiteral("short PATCH"));
    if (seqNo != m_nextSeq || seqSize < 1 || (m_seqSize && seqSize != m_seqSize) || seqNo > seqSize)
        return fail(QStringLiteral("bad PATCH sequence %1/%2").arg(seqNo).arg(seqSize));
    if (bits != 1 && bits != 2 && bits != 4 && bits != 8) return fail(QStringLiteral("bad entry bits %1").arg(bits));
    if (compressor > 1) return fail(QStringLiteral("unknown compressor"));
    if (seqNo == 1) { m_seqSize = seqSize; m_entryBits = bits; m_compressor = compressor; }
    m_pending.append(payload.mid(5));
    if (m_pending.size() > 4 * 1024 * 1024) return fail(QStringLiteral("patch too large"));
    if (seqNo < seqSize) { m_nextSeq = seqNo + 1; return true; }
    QByteArray raw = m_pending;
    m_pending.clear();
    m_nextSeq = 1;
    m_seqSize = 0;
    if (m_compressor == 1) {
        bool ok = false;
        raw = inflateAll(raw, m_values.size() + 16, &ok);
        if (!ok) return fail(QStringLiteral("patch inflate failed"));
    }
    return applyPatchData(raw, error);
}

bool QrpReceiver::applyPatchData(const QByteArray &raw, QString *error)
{
    const int nslots = int(m_values.size());
    const qint64 need = qint64(nslots) * m_entryBits / 8;
    if (raw.size() != need) {
        if (error) *error = QStringLiteral("patch size %1, expected %2").arg(raw.size()).arg(need);
        return false;
    }
    const int perByte = 8 / m_entryBits;
    const int mask = (1 << m_entryBits) - 1;
    for (int i = 0; i < nslots; ++i) {
        const quint8 byte = quint8(raw[i / perByte]);
        const int shift = 8 - m_entryBits * (1 + i % perByte);
        int v = (byte >> shift) & mask;
        if (m_entryBits > 1 && (v & (1 << (m_entryBits - 1)))) v -= (1 << m_entryBits);   // signed
        if (m_entryBits == 1) {
            // 1-bit patches: 1 toggles presence
            if (v) m_values[i] = char(quint8(m_values[i]) >= m_infinity ? 1 : m_infinity);
        } else if (v) {
            m_values[i] = char(qBound(0, int(quint8(m_values[i])) + v, 127));
        }
    }
    m_table = QrpTable(nslots);
    for (int i = 0; i < nslots; ++i)
        if (quint8(m_values[i]) < m_infinity) m_table.setSlot(i);
    m_complete = true;
    return true;
}

} // namespace mx::gnut
