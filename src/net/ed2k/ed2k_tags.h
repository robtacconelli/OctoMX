// SPDX-License-Identifier: GPL-3.0-or-later
// eD2k tags: (name, typed value) pairs used in hello packets, search results, server.met, offers.
// Two encodings exist:
//  * old style:  <type 1><namelen 2><name namelen><value>          (namelen 1 → numeric id)
//  * "new ED2K": <type|0x80 1><id 1><value>, integers shrunk to uint8/16, strings ≤16 bytes use
//                TAGTYPE_STR1..16 (length implied). Only for numeric-id tags.
#pragma once

#include "ed2k_io.h"

#include <QtCore/QList>

namespace mx::ed2k {

struct Tag {
    quint8     type = 0;     // canonical: TAGTYPE_STRING, UINT32 (all ≤32-bit ints), UINT64, FLOAT32, HASH, BLOB, BSOB
    quint8     id = 0;       // numeric name (0 if string-named)
    QByteArray name;         // string name ("users", "bitrate", ...), empty for numeric
    quint64    num = 0;
    float      f = 0;
    QString    str;
    QByteArray blob;         // HASH (16 bytes), BLOB, BSOB

    bool isInt() const { return type == 0x03 || type == 0x0B; }
    bool isStr() const { return type == 0x02; }
    bool isFloat() const { return type == 0x04; }
    bool isHash() const { return type == 0x01; }
    bool is(quint8 tagId) const { return id == tagId && name.isEmpty(); }
    bool is(const char *n) const { return id == 0 && name.compare(QByteArray(n), Qt::CaseInsensitive) == 0; }

    static Tag makeStr(quint8 id, const QString &s);
    static Tag makeStr(const QByteArray &name, const QString &s);
    static Tag makeInt(quint8 id, quint64 v);            // UINT32 or UINT64 if needed
    static Tag makeInt(const QByteArray &name, quint64 v);
    static Tag makeU64(quint8 id, quint64 v);
    static Tag makeHash(quint8 id, const QByteArray &h16);
    static Tag makeFloat(quint8 id, float v);
};

using TagList = QList<Tag>;

// Reads one tag; false (and r.fail()) on malformed input. Unknown types are rejected because the
// following bytes cannot be interpreted.
bool readTag(Reader &r, Tag *out);
// <count 4><tags>; count is capped at maxTags.
bool readTagList(Reader &r, TagList *out, quint32 maxTags = 256);
// <count 1><tags> (used by a few Kad/server structures)
bool readTagList8(Reader &r, TagList *out);

void writeTag(Writer &w, const Tag &t, bool newEd2kFormat = false);
void writeTagList(Writer &w, const TagList &tags, bool newEd2kFormat = false);   // <count 4><tags>

const Tag *findTag(const TagList &tags, quint8 id);
const Tag *findTag(const TagList &tags, const char *name);

} // namespace mx::ed2k
