// SPDX-License-Identifier: GPL-3.0-or-later
// Small helpers shared by the eD2k module: user hash, peer descriptors, block planning, part maps.
#pragma once

#include "mx/services.h"

#include <QtCore/QByteArray>
#include <QtCore/QList>
#include <QtCore/QLoggingCategory>
#include <QtCore/QPair>
#include <QtCore/QString>

Q_DECLARE_LOGGING_CATEGORY(lcEd2k)

namespace mx::ed2k {

using Range = QPair<qint64, qint64>;   // [first, second)

// Random 16 bytes with the eMule client markers (byte 5 = 14, byte 14 = 111).
QByteArray generateUserHash();
QByteArray md4(const QByteArray &data);

// Settings with fallbacks (the schema may not define our keys yet).
struct Config {
    mx::Settings *s = nullptr;
    QVariant get(const QString &key, const QVariant &def) const;
    int i(const QString &key, int def) const { return get(key, def).toInt(); }
    bool b(const QString &key, bool def) const { return get(key, def).toBool(); }
    QString str(const QString &key, const QString &def) const { return get(key, def).toString(); }
};

// ---------------------------------------------------------------- peer descriptor
// A RangeSource for an eD2k peer is opened from a SearchSource whose `uri` is
//   ed2k-peer://<ip>:<port>?file=<ed2k hex>&uid=<user hash hex>&crypt=<flags>&lowid=<id>&server=<ip>:<port>
//              [&buddy=<ip>:<udp port>&bid=<buddy id hex>][&kport=<kad udp port>][&dcb=1]
// (only ip:port is required; for a LowID peer ip is 0.0.0.0 and lowid/server are mandatory).
// crypt flags: 0x01 supports, 0x02 requests, 0x04 requires obfuscation (eMule "connect options").
// Kad sources: a firewalled peer with a buddy has lowid=1, buddy + bid (we send KADEMLIA_CALLBACK_REQ
// to its buddy); dcb=1 marks a firewalled peer that accepts OP_DIRECTCALLBACKREQ on its Kad UDP port.
// Alternatively `uri` may be an ed2k:// file link and `user.address` "ip:port".
struct PeerDescriptor {
    quint32 ip = 0;            // eD2k order; 0 for LowID peers
    quint16 port = 0;
    quint32 lowId = 0;         // != 0 → LowID, reachable only via server callback
    quint32 serverIp = 0;
    quint16 serverPort = 0;
    QByteArray userHash;       // 16 bytes if known (needed for obfuscated connect)
    quint8  cryptOptions = 0;
    QByteArray fileHash;       // optional
    quint32 buddyIp = 0;       // Kad buddy (eD2k order) + its UDP port + the 16-byte buddy ID (wire form)
    quint16 buddyPort = 0;
    QByteArray buddyId;
    quint16 kadPort = 0;       // peer's Kad/UDP port (direct callback)
    bool directCallback = false;
    bool isLowId() const { return lowId != 0; }
    bool hasBuddy() const { return buddyIp && buddyPort && buddyId.size() == 16; }
    bool needsKadCallback() const { return hasBuddy() || directCallback; }
    bool isValid() const { return port != 0 && (ip != 0 || lowId != 0); }
    QString key() const;       // "ip:port" or "lowid@server"
};
QString formatPeerUri(const PeerDescriptor &d);
bool parsePeerUri(const QString &uri, PeerDescriptor *out);

// ---------------------------------------------------------------- parts / blocks
inline int dataPartCount(qint64 size) { return size <= 0 ? 0 : int((size + 9728000 - 1) / 9728000); }
inline int ed2kPartCount(qint64 size) { return int(size / 9728000 + 1); }   // count field in OP_FILESTATUS
// hashset entries as eMule sends them: 0 for files < PARTSIZE, else size/PARTSIZE + 1
inline int ed2kHashsetCount(qint64 size) { return size < 9728000 ? 0 : int(size / 9728000 + 1); }

// Split [offset, offset+length) into blocks of at most EMBLOCKSIZE that never cross a part boundary.
QList<Range> planBlocks(qint64 offset, qint64 length, qint64 fileSize);
// The request size we advertise: 512 000 bytes = 9 728 000 / 19, so requests aligned to it never
// straddle parts; it needs three ≤180 KiB blocks (184320 + 184320 + 143360).
constexpr qint64 PREFERRED_REQUEST = 512000;

// MD4(concat(parts)) == fileHash, with the eMule rule that 1-part files hash to MD4(data).
bool verifyHashset(const QByteArray &fileHash, qint64 size, const QList<QByteArray> &parts);

// Part bitmap (one bool per data part) -> available ranges
QList<Range> rangesFromParts(const QList<bool> &parts, qint64 size);

QString formatBytes(qint64 b);

} // namespace mx::ed2k
