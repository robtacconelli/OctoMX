// SPDX-License-Identifier: GPL-3.0-or-later
// eD2k TCP packet framing:  <proto 1><size 4 = 1 + payload><opcode 1><payload>
// proto 0xE3 (eDonkey), 0xC5 (eMule extended), 0xD4 (zlib-packed eMule packet: payload is deflated).
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>

namespace mx::ed2k {

struct Packet {
    quint8 proto = 0xE3;
    quint8 opcode = 0;
    QByteArray payload;
    bool wasPacked = false;    // arrived as 0xD4 (proto is then reported as 0xC5 for clients)
};

QByteArray encodePacket(quint8 proto, quint8 opcode, const QByteArray &payload);
// Same, but compresses the payload (proto 0xD4) when that makes it smaller. Only use towards
// peers/servers that support it.
QByteArray encodePacketMaybePacked(quint8 proto, quint8 opcode, const QByteArray &payload);

// zlib helpers with hard output limits (hostile input).
QByteArray zlibCompress(const QByteArray &in, int level = 6);
bool zlibUncompress(const QByteArray &in, qsizetype maxOut, QByteArray *out);

// Incremental decoder for a TCP byte stream.
class PacketParser {
public:
    explicit PacketParser(quint32 maxPacket = 2 * 1024 * 1024, quint32 maxUnpacked = 4 * 1024 * 1024)
        : m_max(maxPacket), m_maxUnpacked(maxUnpacked) {}
    void feed(const QByteArray &bytes) { m_buf.append(bytes); }
    void feed(const char *d, qsizetype n) { m_buf.append(d, n); }
    // Returns true and fills *out when a full packet is buffered. On a framing error sets error()
    // and returns false forever after.
    bool next(Packet *out);
    bool error() const { return !m_error.isEmpty(); }
    QString errorString() const { return m_error; }
    qsizetype buffered() const { return m_buf.size(); }
    // When a big data packet is still arriving: bytes of it received so far (for timeouts)
    qint64 pendingPacketSize() const;
private:
    QByteArray m_buf;
    qsizetype m_off = 0;
    quint32 m_max, m_maxUnpacked;
    QString m_error;
};

} // namespace mx::ed2k
