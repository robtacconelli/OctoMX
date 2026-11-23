// SPDX-License-Identifier: GPL-3.0-or-later
#include "ed2k_tags.h"
#include "ed2k_opcodes.h"

namespace mx::ed2k {

Tag Tag::makeStr(quint8 id, const QString &s) { Tag t; t.type = TAGTYPE_STRING; t.id = id; t.str = s; return t; }
Tag Tag::makeStr(const QByteArray &name, const QString &s) { Tag t; t.type = TAGTYPE_STRING; t.name = name; t.str = s; return t; }
Tag Tag::makeInt(quint8 id, quint64 v) { Tag t; t.type = v > 0xFFFFFFFFull ? TAGTYPE_UINT64 : TAGTYPE_UINT32; t.id = id; t.num = v; return t; }
Tag Tag::makeInt(const QByteArray &name, quint64 v) { Tag t = makeInt(quint8(0), v); t.name = name; return t; }
Tag Tag::makeU64(quint8 id, quint64 v) { Tag t; t.type = TAGTYPE_UINT64; t.id = id; t.num = v; return t; }
Tag Tag::makeHash(quint8 id, const QByteArray &h) { Tag t; t.type = TAGTYPE_HASH; t.id = id; t.blob = h.left(16); return t; }
Tag Tag::makeFloat(quint8 id, float v) { Tag t; t.type = TAGTYPE_FLOAT32; t.id = id; t.f = v; return t; }

bool readTag(Reader &r, Tag *out)
{
    Tag t;
    quint8 type = r.u8();
    if (!r.ok()) return false;
    if (type & 0x80) {
        type &= 0x7F;
        t.id = r.u8();
    } else {
        quint16 len = r.u16();
        if (!r.ok()) return false;
        if (len == 1) {
            t.id = r.u8();
        } else {
            if (len > 1024) { r.fail(); return false; }
            t.name = r.bytes(len);
        }
    }
    if (!r.ok()) return false;

    switch (type) {
    case TAGTYPE_STRING:
        t.type = TAGTYPE_STRING;
        t.str = r.str16();
        break;
    case TAGTYPE_UINT32: t.type = TAGTYPE_UINT32; t.num = r.u32(); break;
    case TAGTYPE_UINT16: t.type = TAGTYPE_UINT32; t.num = r.u16(); break;
    case TAGTYPE_UINT8:  t.type = TAGTYPE_UINT32; t.num = r.u8(); break;
    case TAGTYPE_UINT64: t.type = TAGTYPE_UINT64; t.num = r.u64(); break;
    case TAGTYPE_FLOAT32: t.type = TAGTYPE_FLOAT32; t.f = r.f32(); break;
    case TAGTYPE_HASH: t.type = TAGTYPE_HASH; t.blob = r.bytes(16); break;
    case TAGTYPE_BOOL: t.type = TAGTYPE_UINT32; t.num = r.u8(); break;
    case TAGTYPE_BOOLARRAY: {
        quint16 bits = r.u16();
        t.type = TAGTYPE_BLOB;
        t.blob = r.bytes(bits / 8 + 1);
        break;
    }
    case TAGTYPE_BLOB: {
        quint32 len = r.u32();
        if (!r.ok() || len > quint32(r.remaining()) || len > 1024 * 1024) { r.fail(); return false; }
        t.type = TAGTYPE_BLOB;
        t.blob = r.bytes(len);
        break;
    }
    case TAGTYPE_BSOB: {
        quint8 len = r.u8();
        t.type = TAGTYPE_BSOB;
        t.blob = r.bytes(len);
        break;
    }
    default:
        if (type >= TAGTYPE_STR1 && type <= TAGTYPE_STR22) {
            t.type = TAGTYPE_STRING;
            t.str = decodeString(r.bytes(type - TAGTYPE_STR1 + 1));
        } else {
            r.fail();
            return false;
        }
    }
    if (!r.ok()) return false;
    if (out) *out = std::move(t);
    return true;
}

bool readTagList(Reader &r, TagList *out, quint32 maxTags)
{
    quint32 n = r.u32();
    if (!r.ok() || n > maxTags) { r.fail(); return false; }
    // each tag is at least 3 bytes
    if (qint64(n) * 3 > r.remaining()) { r.fail(); return false; }
    TagList tags;
    tags.reserve(int(n));
    for (quint32 i = 0; i < n; ++i) {
        Tag t;
        if (!readTag(r, &t)) return false;
        tags.append(std::move(t));
    }
    if (out) *out = std::move(tags);
    return true;
}

bool readTagList8(Reader &r, TagList *out)
{
    quint8 n = r.u8();
    if (!r.ok()) return false;
    TagList tags;
    for (int i = 0; i < n; ++i) {
        Tag t;
        if (!readTag(r, &t)) return false;
        tags.append(std::move(t));
    }
    if (out) *out = std::move(tags);
    return true;
}

static void writeName(Writer &w, quint8 type, const Tag &t, bool shortForm)
{
    if (t.name.isEmpty()) {
        if (shortForm) { w.u8(type | 0x80).u8(t.id); }
        else { w.u8(type).u16(1).u8(t.id); }
    } else {
        w.u8(type).raw16(t.name);
    }
}

void writeTag(Writer &w, const Tag &t, bool nf)
{
    const bool shortForm = nf && t.name.isEmpty();
    switch (t.type) {
    case TAGTYPE_STRING: {
        const QByteArray u = t.str.toUtf8();
        if (shortForm && u.size() >= 1 && u.size() <= 16) {
            writeName(w, quint8(TAGTYPE_STR1 + u.size() - 1), t, true);
            w.bytes(u);
        } else {
            writeName(w, TAGTYPE_STRING, t, shortForm);
            w.raw16(u);
        }
        break;
    }
    case TAGTYPE_UINT32:
    case TAGTYPE_UINT64:
        if (nf) {
            if (t.num <= 0xFF) { writeName(w, TAGTYPE_UINT8, t, shortForm); w.u8(quint8(t.num)); }
            else if (t.num <= 0xFFFF) { writeName(w, TAGTYPE_UINT16, t, shortForm); w.u16(quint16(t.num)); }
            else if (t.num <= 0xFFFFFFFFull) { writeName(w, TAGTYPE_UINT32, t, shortForm); w.u32(quint32(t.num)); }
            else { writeName(w, TAGTYPE_UINT64, t, shortForm); w.u64(t.num); }
        } else if (t.type == TAGTYPE_UINT64) {
            writeName(w, TAGTYPE_UINT64, t, false); w.u64(t.num);
        } else {
            writeName(w, TAGTYPE_UINT32, t, false); w.u32(quint32(t.num));
        }
        break;
    case TAGTYPE_FLOAT32: writeName(w, TAGTYPE_FLOAT32, t, shortForm); w.f32(t.f); break;
    case TAGTYPE_HASH: writeName(w, TAGTYPE_HASH, t, shortForm); w.hash16(t.blob); break;
    case TAGTYPE_BLOB: writeName(w, TAGTYPE_BLOB, t, shortForm); w.u32(quint32(t.blob.size())); w.bytes(t.blob); break;
    case TAGTYPE_BSOB: {
        const QByteArray b = t.blob.left(255);
        writeName(w, TAGTYPE_BSOB, t, shortForm); w.u8(quint8(b.size())); w.bytes(b);
        break;
    }
    default: break;
    }
}

void writeTagList(Writer &w, const TagList &tags, bool nf)
{
    w.u32(quint32(tags.size()));
    for (const Tag &t : tags) writeTag(w, t, nf);
}

const Tag *findTag(const TagList &tags, quint8 id)
{
    for (const Tag &t : tags) if (t.is(id)) return &t;
    return nullptr;
}
const Tag *findTag(const TagList &tags, const char *name)
{
    for (const Tag &t : tags) if (t.is(name)) return &t;
    return nullptr;
}

} // namespace mx::ed2k
