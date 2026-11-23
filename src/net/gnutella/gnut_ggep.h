// SPDX-License-Identifier: GPL-3.0-or-later
// Gnutella module: GGEP (Gnutella Generic Extension Protocol 0.51) blocks: parse and encode,
// including COBS encoding and deflate compression of extension payloads.
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QList>

namespace mx::gnut {

constexpr quint8 GgepMagic = 0xC3;
constexpr int GgepMaxExtensions = 64;
constexpr int GgepMaxDataLen = 65536;          // decoded payload cap (after inflate)

struct GgepExt {
    QByteArray id;                             // 1..15 bytes ("H", "ALT", "PUSH", "LF"...)
    QByteArray data;                           // decoded (COBS-decoded and inflated)
    bool compress = false;                     // encode: deflate the payload
    bool cobs = false;                         // encode: force COBS (auto if data contains NUL and allowCobs)
};

class GgepBlock {
public:
    QList<GgepExt> exts;
    const GgepExt *find(const char *id) const;
    bool has(const char *id) const { return find(id) != nullptr; }
    QByteArray value(const char *id) const { const GgepExt *e = find(id); return e ? e->data : QByteArray(); }
    void add(const QByteArray &id, const QByteArray &data, bool compress = false)
    { GgepExt e; e.id = id; e.data = data; e.compress = compress; exts << e; }
    bool isEmpty() const { return exts.isEmpty(); }
};

// Parse a GGEP block starting at buf[*pos] (must be the 0xC3 magic). On success *pos points just
// past the block. Returns false on malformed input (then *pos is unspecified, *out may hold the
// extensions decoded so far).
bool parseGgep(const QByteArray &buf, int *pos, GgepBlock *out);
// Encode. noNul = the block must not contain 0x00 bytes (query-hit results, HUGE areas): every
// extension whose data contains a NUL is COBS-encoded. Returns empty for an empty block.
QByteArray encodeGgep(const GgepBlock &b, bool noNul = false);

QByteArray cobsEncode(const QByteArray &in);
QByteArray cobsDecode(const QByteArray &in, bool *ok = nullptr);

// Variable-length little-endian unsigned integers (GGEP "LF", "DU", "CT"...): 1..8 bytes.
quint64 ggepUint(const QByteArray &d, bool *ok = nullptr);
QByteArray ggepUintEncode(quint64 v);

} // namespace mx::gnut
