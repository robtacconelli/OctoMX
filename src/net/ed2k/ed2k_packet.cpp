// SPDX-License-Identifier: GPL-3.0-or-later
#include "ed2k_packet.h"
#include "ed2k_io.h"
#include "ed2k_opcodes.h"

#include <QtCore/QtEndian>
#include <zlib.h>

namespace mx::ed2k {

QByteArray encodePacket(quint8 proto, quint8 opcode, const QByteArray &payload)
{
    QByteArray out;
    out.reserve(payload.size() + 6);
    out.append(char(proto));
    char len[4];
    qToLittleEndian<quint32>(quint32(payload.size() + 1), len);
    out.append(len, 4);
    out.append(char(opcode));
    out.append(payload);
    return out;
}

QByteArray encodePacketMaybePacked(quint8 proto, quint8 opcode, const QByteArray &payload)
{
    if (payload.size() > 64) {
        const QByteArray z = zlibCompress(payload, 9);
        if (!z.isEmpty() && z.size() + 8 < payload.size())
            return encodePacket(PROT_PACKED, opcode, z);
    }
    return encodePacket(proto, opcode, payload);
}

QByteArray zlibCompress(const QByteArray &in, int level)
{
    uLongf outLen = compressBound(uLong(in.size()));
    QByteArray out(qsizetype(outLen), Qt::Uninitialized);
    int rc = compress2(reinterpret_cast<Bytef *>(out.data()), &outLen,
                       reinterpret_cast<const Bytef *>(in.constData()), uLong(in.size()), level);
    if (rc != Z_OK) return QByteArray();
    out.truncate(qsizetype(outLen));
    return out;
}

bool zlibUncompress(const QByteArray &in, qsizetype maxOut, QByteArray *out)
{
    z_stream zs{};
    if (inflateInit(&zs) != Z_OK) return false;
    zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(in.constData()));
    zs.avail_in = uInt(in.size());
    QByteArray result;
    char buf[16384];
    int rc = Z_OK;
    while (rc != Z_STREAM_END) {
        zs.next_out = reinterpret_cast<Bytef *>(buf);
        zs.avail_out = sizeof(buf);
        rc = inflate(&zs, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) { inflateEnd(&zs); return false; }
        const qsizetype got = qsizetype(sizeof(buf) - zs.avail_out);
        if (result.size() + got > maxOut) { inflateEnd(&zs); return false; }
        result.append(buf, got);
        if (rc == Z_OK && zs.avail_in == 0 && got == 0) { inflateEnd(&zs); return false; } // truncated
    }
    inflateEnd(&zs);
    if (out) *out = std::move(result);
    return true;
}

qint64 PacketParser::pendingPacketSize() const
{
    if (m_buf.size() - m_off < 5) return 0;
    return qFromLittleEndian<quint32>(m_buf.constData() + m_off + 1);
}

bool PacketParser::next(Packet *out)
{
    if (error()) return false;
    const qsizetype avail = m_buf.size() - m_off;
    if (avail < 6) {
        if (avail >= 1) {
            const quint8 p = quint8(m_buf[m_off]);
            if (p != PROT_EDONKEY && p != PROT_EMULE && p != PROT_PACKED) {
                m_error = QStringLiteral("bad protocol byte 0x%1").arg(p, 2, 16, QLatin1Char('0'));
                return false;
            }
        }
        return false;
    }
    const char *h = m_buf.constData() + m_off;
    const quint8 proto = quint8(h[0]);
    if (proto != PROT_EDONKEY && proto != PROT_EMULE && proto != PROT_PACKED) {
        m_error = QStringLiteral("bad protocol byte 0x%1").arg(proto, 2, 16, QLatin1Char('0'));
        return false;
    }
    const quint32 len = qFromLittleEndian<quint32>(h + 1);
    if (len < 1 || len > m_max) {
        m_error = QStringLiteral("bad packet length %1").arg(len);
        return false;
    }
    if (avail < qsizetype(5) + qsizetype(len)) return false;
    Packet p;
    p.proto = proto;
    p.opcode = quint8(h[5]);
    p.payload = QByteArray(h + 6, qsizetype(len) - 1);
    m_off += 5 + qsizetype(len);
    if (m_off > 256 * 1024 || m_off == m_buf.size()) { m_buf.remove(0, m_off); m_off = 0; }
    if (proto == PROT_PACKED) {
        QByteArray un;
        if (!zlibUncompress(p.payload, m_maxUnpacked, &un)) {
            m_error = QStringLiteral("corrupt packed packet (opcode 0x%1)").arg(p.opcode, 2, 16, QLatin1Char('0'));
            return false;
        }
        p.payload = std::move(un);
        p.proto = PROT_EMULE;
        p.wasPacked = true;
    }
    if (out) *out = std::move(p);
    return true;
}

} // namespace mx::ed2k
