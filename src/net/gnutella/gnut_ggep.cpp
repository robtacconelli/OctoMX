// SPDX-License-Identifier: GPL-3.0-or-later
#include "gnut_ggep.h"
#include "gnut_zlib.h"

namespace mx::gnut {

const GgepExt *GgepBlock::find(const char *id) const
{
    for (const GgepExt &e : exts)
        if (e.id == id) return &e;
    return nullptr;
}

QByteArray cobsEncode(const QByteArray &in)
{
    QByteArray out;
    out.reserve(in.size() + in.size() / 254 + 2);
    int codeIdx = int(out.size());
    out.append('\0');
    int code = 1;
    for (char c : in) {
        if (c == 0) {
            out[codeIdx] = char(code);
            codeIdx = int(out.size());
            out.append('\0');
            code = 1;
        } else {
            out.append(c);
            if (++code == 0xFF) {
                out[codeIdx] = char(code);
                codeIdx = int(out.size());
                out.append('\0');
                code = 1;
            }
        }
    }
    out[codeIdx] = char(code);
    return out;
}

QByteArray cobsDecode(const QByteArray &in, bool *ok)
{
    QByteArray out;
    int i = 0;
    const int n = int(in.size());
    while (i < n) {
        const int code = quint8(in[i++]);
        if (code == 0 || i + code - 1 > n) { if (ok) *ok = false; return {}; }
        out.append(in.constData() + i, code - 1);
        i += code - 1;
        if (i < n && code < 0xFF) out.append('\0');
    }
    if (ok) *ok = true;
    return out;
}

quint64 ggepUint(const QByteArray &d, bool *ok)
{
    if (d.isEmpty() || d.size() > 8) { if (ok) *ok = false; return 0; }
    quint64 v = 0;
    for (int i = int(d.size()) - 1; i >= 0; --i) v = (v << 8) | quint8(d[i]);
    if (ok) *ok = true;
    return v;
}

QByteArray ggepUintEncode(quint64 v)
{
    QByteArray b;
    do {
        b.append(char(v & 0xff));
        v >>= 8;
    } while (v);
    return b;
}

bool parseGgep(const QByteArray &buf, int *pos, GgepBlock *out)
{
    int p = *pos;
    const int n = int(buf.size());
    if (p >= n || quint8(buf[p]) != GgepMagic) return false;
    ++p;
    for (int count = 0; count < GgepMaxExtensions; ++count) {
        if (p >= n) return false;
        const quint8 flags = quint8(buf[p++]);
        const int idLen = flags & 0x0f;
        if (idLen == 0 || (flags & 0x10)) return false;
        if (p + idLen > n) return false;
        QByteArray id = buf.mid(p, idLen);
        if (id.contains('\0')) return false;
        p += idLen;
        // data length: up to 3 bytes, 6 bits each; 0x80 = more, 0x40 = last
        int len = 0;
        bool last = false;
        for (int k = 0; k < 3; ++k) {
            if (p >= n) return false;
            const quint8 b = quint8(buf[p++]);
            len = (len << 6) | (b & 0x3f);
            if (b & 0x40) { last = true; break; }
            if (!(b & 0x80)) return false;
        }
        if (!last) return false;
        if (p + len > n) return false;
        QByteArray data = buf.mid(p, len);
        p += len;
        if (flags & 0x40) {
            bool ok = false;
            data = cobsDecode(data, &ok);
            if (!ok) return false;
        }
        if (flags & 0x20) {
            bool ok = false;
            data = inflateAll(data, GgepMaxDataLen, &ok);
            if (!ok) return false;
        }
        GgepExt e;
        e.id = id;
        e.data = data;
        out->exts << e;
        if (flags & 0x80) {
            *pos = p;
            return true;
        }
    }
    return false;                                 // too many extensions
}

QByteArray encodeGgep(const GgepBlock &b, bool noNul)
{
    if (b.exts.isEmpty()) return {};
    QByteArray out;
    out.append(char(GgepMagic));
    for (int i = 0; i < b.exts.size(); ++i) {
        const GgepExt &e = b.exts[i];
        QByteArray data = e.data;
        quint8 flags = quint8(qBound(1, int(e.id.size()), 15));
        if (e.compress && !data.isEmpty()) {
            const QByteArray z = deflateAll(data);
            if (!z.isEmpty() && z.size() < data.size()) { data = z; flags |= 0x20; }
        }
        if (e.cobs || (noNul && data.contains('\0'))) {
            data = cobsEncode(data);
            flags |= 0x40;
        }
        if (i == b.exts.size() - 1) flags |= 0x80;
        out.append(char(flags));
        out.append(e.id.left(15));
        const int len = int(data.size());
        if (len < 0x40) {
            out.append(char(0x40 | len));
        } else if (len < 0x1000) {
            out.append(char(0x80 | (len >> 6)));
            out.append(char(0x40 | (len & 0x3f)));
        } else {
            out.append(char(0x80 | ((len >> 12) & 0x3f)));
            out.append(char(0x80 | ((len >> 6) & 0x3f)));
            out.append(char(0x40 | (len & 0x3f)));
        }
        out.append(data);
    }
    return out;
}

} // namespace mx::gnut
