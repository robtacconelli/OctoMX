// SPDX-License-Identifier: GPL-3.0-or-later
// Parts of this file are adapted from eMule 0.50a (Copyright (C) the eMule team,
// https://www.emule-project.net; EncryptedDatagramSocket.cpp), GPL-2.0-or-later;
// this file is distributed under GPL-3.0-or-later. See NOTICE.
#include "kad_wire.h"

#include "../ed2k_crypto.h"
#include "../ed2k_opcodes.h"
#include "../ed2k_packet.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QRandomGenerator>

namespace mx::ed2k::kad {

QString kadIpToString(quint32 ip)
{
    return QStringLiteral("%1.%2.%3.%4").arg(ip >> 24).arg((ip >> 16) & 0xFF).arg((ip >> 8) & 0xFF).arg(ip & 0xFF);
}

bool isLanIp(quint32 ip)
{
    const quint32 a = ip >> 24, b = (ip >> 16) & 0xFF;
    return a == 10 || a == 127 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) || (a == 169 && b == 254)
        || (a == 100 && b >= 64 && b <= 127);
}

bool isGoodKadIpPort(quint32 ip, quint16 port, bool allowLan)
{
    if (port == 0 || ip == 0) return false;
    const quint32 a = ip >> 24;
    if (a == 0 || a >= 224) return false;
    if (isLanIp(ip)) return allowLan;
    return true;
}

// ---------------------------------------------------------------- tags
void writeKadTag(Writer &w, const Tag &t)
{
    auto name = [&](quint8 type) {
        w.u8(type);
        if (t.name.isEmpty()) w.u16(1).u8(t.id);
        else w.raw16(t.name);
    };
    switch (t.type) {
    case TAGTYPE_STRING: name(TAGTYPE_STRING); w.raw16(t.str.toUtf8()); break;
    case TAGTYPE_UINT32:
    case TAGTYPE_UINT64:
        if (t.num <= 0xFF) { name(TAGTYPE_UINT8); w.u8(quint8(t.num)); }
        else if (t.num <= 0xFFFF) { name(TAGTYPE_UINT16); w.u16(quint16(t.num)); }
        else if (t.num <= 0xFFFFFFFFull) { name(TAGTYPE_UINT32); w.u32(quint32(t.num)); }
        else { name(TAGTYPE_UINT64); w.u64(t.num); }
        break;
    case TAGTYPE_FLOAT32: name(TAGTYPE_FLOAT32); w.f32(t.f); break;
    case TAGTYPE_HASH: name(TAGTYPE_HASH); w.hash16(t.blob); break;
    case TAGTYPE_BSOB:
    case TAGTYPE_BLOB: {
        const QByteArray b = t.blob.left(255);
        name(TAGTYPE_BSOB); w.u8(quint8(b.size())); w.bytes(b);
        break;
    }
    default: break;
    }
}

void writeKadTagList(Writer &w, const TagList &tags)
{
    const int n = int(qMin<qsizetype>(tags.size(), 255));
    w.u8(quint8(n));
    for (int i = 0; i < n; ++i) writeKadTag(w, tags[i]);
}

bool readKadTagList(Reader &r, TagList *out, int maxTags)
{
    const quint8 n = r.u8();
    if (!r.ok() || n > maxTags || qint64(n) * 4 > r.remaining()) { r.fail(); return false; }
    TagList tags;
    tags.reserve(n);
    for (int i = 0; i < n; ++i) {
        Tag t;
        if (!readTag(r, &t)) return false;
        if (t.isStr() && t.str.size() > 2048) t.str.truncate(2048);
        tags.append(std::move(t));
    }
    if (out) *out = std::move(tags);
    return true;
}

const Tag *kadTag(const TagList &tags, quint8 id) { return findTag(tags, id); }

quint64 kadTagInt(const TagList &tags, quint8 id, quint64 def)
{
    const Tag *t = findTag(tags, id);
    return (t && t->isInt()) ? t->num : def;
}

QString kadTagStr(const TagList &tags, quint8 id)
{
    const Tag *t = findTag(tags, id);
    return (t && t->isStr()) ? t->str : QString();
}

quint64 kadTagFileSize(const TagList &tags)
{
    const Tag *t = findTag(tags, TAG_FILESIZE);
    if (!t) return 0;
    if (t->isInt()) return t->num;
    if ((t->type == TAGTYPE_BSOB || t->type == TAGTYPE_BLOB) && t->blob.size() == 8) {
        Reader r(t->blob);
        return r.u64();
    }
    return 0;
}

// ---------------------------------------------------------------- framing
QByteArray kadFrame(quint8 opcode, const QByteArray &payload, bool allowPack)
{
    if (allowPack && payload.size() > 200) {
        const QByteArray z = zlibCompress(payload, 9);
        if (!z.isEmpty() && z.size() < payload.size()) {
            QByteArray out;
            out.reserve(z.size() + 2);
            out.append(char(PROT_KADPACK)).append(char(opcode)).append(z);
            return out;
        }
    }
    QByteArray out;
    out.reserve(payload.size() + 2);
    out.append(char(PROT_KAD)).append(char(opcode)).append(payload);
    return out;
}

bool kadUnframe(const QByteArray &d, quint8 *opcode, QByteArray *payload)
{
    if (d.size() < 2 || d.size() > KAD_MAX_DATAGRAM) return false;
    const quint8 proto = quint8(d[0]);
    if (opcode) *opcode = quint8(d[1]);
    if (proto == PROT_KAD) {
        if (payload) *payload = d.mid(2);
        return true;
    }
    if (proto == PROT_KADPACK) {
        QByteArray un;
        if (!zlibUncompress(d.mid(2), KAD_MAX_UNPACKED, &un)) return false;
        if (payload) *payload = std::move(un);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- messages
static void writeId(Writer &w, const KadId &id) { w.bytes(id.toWire()); }
static KadId readId(Reader &r) { const QByteArray b = r.bytes(16); return r.ok() ? KadId::fromWire(b) : KadId(); }

void writePeer(Writer &w, const KadPeer &p)
{
    writeId(w, p.id);
    w.u32(p.ip).u16(p.udpPort).u16(p.tcpPort).u8(p.version);
}

KadPeer readPeer(Reader &r)
{
    KadPeer p;
    p.id = readId(r);
    p.ip = r.u32();
    p.udpPort = r.u16();
    p.tcpPort = r.u16();
    p.version = r.u8();
    return p;
}

QByteArray encodeHello(const HelloMsg &m)
{
    Writer w(48);
    writeId(w, m.id);
    w.u16(m.tcpPort).u8(m.version);
    TagList tags;
    if (m.udpPortTag) tags << Tag::makeInt(TAG_SOURCEUPORT, m.udpPortTag);
    if (m.hasMisc) tags << Tag::makeInt(TAG_KADMISCOPTIONS, m.misc);
    writeKadTagList(w, tags);
    return w.take();
}

bool decodeHello(const QByteArray &p, HelloMsg *m)
{
    Reader r(p);
    HelloMsg h;
    h.id = readId(r);
    h.tcpPort = r.u16();
    h.version = r.u8();
    if (!r.ok()) return false;
    if (r.remaining() > 0) {
        TagList tags;
        if (!readKadTagList(r, &tags, 16)) return false;
        for (const Tag &t : tags) {
            if (t.is(quint8(TAG_SOURCEUPORT)) && t.isInt() && quint16(t.num)) h.udpPortTag = quint16(t.num);
            else if (t.is(quint8(TAG_KADMISCOPTIONS)) && t.isInt()) { h.hasMisc = true; h.misc = quint8(t.num); }
        }
    }
    if (m) *m = h;
    return true;
}

QByteArray encodeBootstrapRes(const BootstrapRes &m)
{
    Writer w(21 + 25 * m.contacts.size());
    writeId(w, m.id);
    w.u16(m.tcpPort).u8(m.version).u16(quint16(m.contacts.size()));
    for (const KadPeer &c : m.contacts) writePeer(w, c);
    return w.take();
}

bool decodeBootstrapRes(const QByteArray &p, BootstrapRes *m)
{
    Reader r(p);
    BootstrapRes b;
    b.id = readId(r);
    b.tcpPort = r.u16();
    b.version = r.u8();
    const quint16 n = r.u16();
    if (!r.ok() || n > 64 || qint64(n) * 25 != r.remaining()) return false;
    for (int i = 0; i < n; ++i) b.contacts.append(readPeer(r));
    if (!r.ok()) return false;
    if (m) *m = b;
    return true;
}

QByteArray encodeKadReq(const KadReq &m)
{
    Writer w(33);
    w.u8(m.type);
    writeId(w, m.target);
    writeId(w, m.receiver);
    return w.take();
}

bool decodeKadReq(const QByteArray &p, KadReq *m)
{
    if (p.size() != 33) return false;
    Reader r(p);
    KadReq q;
    q.type = r.u8() & 0x1F;
    q.target = readId(r);
    q.receiver = readId(r);
    if (!r.ok() || q.type == 0) return false;
    if (m) *m = q;
    return true;
}

QByteArray encodeKadRes(const KadRes &m)
{
    Writer w(17 + 25 * m.contacts.size());
    writeId(w, m.target);
    const int n = int(qMin<qsizetype>(m.contacts.size(), 32));
    w.u8(quint8(n));
    for (int i = 0; i < n; ++i) writePeer(w, m.contacts[i]);
    return w.take();
}

bool decodeKadRes(const QByteArray &p, KadRes *m)
{
    Reader r(p);
    KadRes s;
    s.target = readId(r);
    const quint8 n = r.u8();
    if (!r.ok() || n > 32 || p.size() != 17 + 25 * int(n)) return false;
    for (int i = 0; i < n; ++i) s.contacts.append(readPeer(r));
    if (!r.ok()) return false;
    if (m) *m = s;
    return true;
}

QByteArray encodeSearchKeyReq(const SearchKeyReq &m)
{
    Writer w(18 + m.tree.size());
    writeId(w, m.target);
    w.u16(quint16((m.start & 0x7FFF) | (m.tree.isEmpty() ? 0 : 0x8000)));
    w.bytes(m.tree);
    return w.take();
}

bool decodeSearchKeyReq(const QByteArray &p, SearchKeyReq *m)
{
    Reader r(p);
    SearchKeyReq q;
    q.target = readId(r);
    const quint16 s = r.u16();
    if (!r.ok()) return false;
    q.start = s & 0x7FFF;
    if (s & 0x8000) {
        q.tree = r.rest();
        if (q.tree.isEmpty() || q.tree.size() > 1024) return false;
    }
    if (m) *m = q;
    return true;
}

QByteArray encodeSearchSourceReq(const SearchSourceReq &m)
{
    Writer w(26);
    writeId(w, m.target);
    w.u16(m.start & 0x7FFF).u64(m.size);
    return w.take();
}

bool decodeSearchSourceReq(const QByteArray &p, SearchSourceReq *m)
{
    Reader r(p);
    SearchSourceReq q;
    q.target = readId(r);
    q.start = r.u16() & 0x7FFF;
    q.size = r.u64();
    if (!r.ok()) return false;
    if (m) *m = q;
    return true;
}

QByteArray encodeSearchRes(const SearchRes &m)
{
    Writer w(512);
    writeId(w, m.sender);
    writeId(w, m.target);
    w.u16(quint16(m.results.size()));
    for (const SearchResEntry &e : m.results) {
        writeId(w, e.answer);
        writeKadTagList(w, e.tags);
    }
    return w.take();
}

bool decodeSearchRes(const QByteArray &p, SearchRes *m)
{
    Reader r(p);
    SearchRes s;
    s.sender = readId(r);
    s.target = readId(r);
    const quint16 n = r.u16();
    if (!r.ok() || n > 300 || qint64(n) * 17 > r.remaining()) return false;
    for (int i = 0; i < n; ++i) {
        SearchResEntry e;
        e.answer = readId(r);
        if (!readKadTagList(r, &e.tags, 64)) return false;
        s.results.append(std::move(e));
    }
    if (!r.ok()) return false;
    if (m) *m = std::move(s);
    return true;
}

QByteArray encodePublishKeyReq(const PublishKeyReq &m)
{
    Writer w(1024);
    writeId(w, m.keyword);
    w.u16(quint16(m.entries.size()));
    for (const PublishKeyEntry &e : m.entries) {
        writeId(w, e.file);
        writeKadTagList(w, e.tags);
    }
    return w.take();
}

bool decodePublishKeyReq(const QByteArray &p, PublishKeyReq *m)
{
    Reader r(p);
    PublishKeyReq q;
    q.keyword = readId(r);
    const quint16 n = r.u16();
    if (!r.ok() || n > 200 || qint64(n) * 17 > r.remaining()) return false;
    for (int i = 0; i < n; ++i) {
        PublishKeyEntry e;
        e.file = readId(r);
        if (!readKadTagList(r, &e.tags, 64)) return false;
        q.entries.append(std::move(e));
    }
    if (!r.ok()) return false;
    if (m) *m = std::move(q);
    return true;
}

QByteArray encodePublishSourceReq(const PublishSourceReq &m)
{
    Writer w(128);
    writeId(w, m.file);
    writeId(w, m.source);
    writeKadTagList(w, m.tags);
    return w.take();
}

bool decodePublishSourceReq(const QByteArray &p, PublishSourceReq *m)
{
    Reader r(p);
    PublishSourceReq q;
    q.file = readId(r);
    q.source = readId(r);
    if (!readKadTagList(r, &q.tags, 32) || !r.ok()) return false;
    if (m) *m = std::move(q);
    return true;
}

QByteArray encodePublishRes(const PublishRes &m)
{
    Writer w(18);
    writeId(w, m.target);
    w.u8(m.load);
    if (m.hasOptions) w.u8(m.options);
    return w.take();
}

bool decodePublishRes(const QByteArray &p, PublishRes *m)
{
    Reader r(p);
    PublishRes q;
    q.target = readId(r);
    q.load = r.u8();
    if (!r.ok()) return false;
    if (r.remaining() > 0) { q.hasOptions = true; q.options = r.u8(); }
    if (m) *m = q;
    return true;
}

QByteArray encodeFirewalled2Req(const Firewalled2Req &m)
{
    Writer w(19);
    w.u16(m.tcpPort);
    writeId(w, m.userHash);
    w.u8(m.connectOptions);
    return w.take();
}

bool decodeFirewalled2Req(const QByteArray &p, Firewalled2Req *m)
{
    Reader r(p);
    Firewalled2Req q;
    q.tcpPort = r.u16();
    if (p.size() >= 19) {
        q.userHash = readId(r);
        q.connectOptions = r.u8();
    } else if (p.size() != 2) {
        return false;
    }
    if (!r.ok()) return false;
    if (m) *m = q;
    return true;
}

QByteArray encodeFindBuddyReq(const FindBuddyReq &m)
{
    Writer w(34);
    writeId(w, m.buddyId);
    writeId(w, m.userHash);
    w.u16(m.tcpPort);
    return w.take();
}

bool decodeFindBuddyReq(const QByteArray &p, FindBuddyReq *m)
{
    if (p.size() < 34) return false;
    Reader r(p);
    FindBuddyReq q;
    q.buddyId = readId(r);
    q.userHash = readId(r);
    q.tcpPort = r.u16();
    if (!r.ok()) return false;
    if (m) *m = q;
    return true;
}

QByteArray encodeFindBuddyRes(const FindBuddyRes &m)
{
    Writer w(35);
    writeId(w, m.check);
    writeId(w, m.userHash);
    w.u16(m.tcpPort);
    if (m.hasOptions) w.u8(m.connectOptions);
    return w.take();
}

bool decodeFindBuddyRes(const QByteArray &p, FindBuddyRes *m)
{
    if (p.size() < 34) return false;
    Reader r(p);
    FindBuddyRes q;
    q.check = readId(r);
    q.userHash = readId(r);
    q.tcpPort = r.u16();
    if (r.remaining() > 0) { q.hasOptions = true; q.connectOptions = r.u8(); }
    if (!r.ok()) return false;
    if (m) *m = q;
    return true;
}

QByteArray encodeCallbackReq(const CallbackReq &m)
{
    Writer w(34);
    writeId(w, m.buddyId);
    writeId(w, m.fileId);
    w.u16(m.tcpPort);
    return w.take();
}

bool decodeCallbackReq(const QByteArray &p, CallbackReq *m)
{
    if (p.size() < 34) return false;
    Reader r(p);
    CallbackReq q;
    q.buddyId = readId(r);
    q.fileId = readId(r);
    q.tcpPort = r.u16();
    if (!r.ok()) return false;
    if (m) *m = q;
    return true;
}

QByteArray encodeFirewallUdp(const FirewallUdpMsg &m)
{
    Writer w(3);
    w.u8(m.error).u16(m.port);
    return w.take();
}

bool decodeFirewallUdp(const QByteArray &p, FirewallUdpMsg *m)
{
    if (p.size() < 3) return false;
    Reader r(p);
    FirewallUdpMsg q;
    q.error = r.u8();
    q.port = r.u16();
    if (!r.ok()) return false;
    if (m) *m = q;
    return true;
}

// ---------------------------------------------------------------- obfuscation
static constexpr quint32 MAGICVALUE_UDP_SYNC_CLIENT = 0x395F2EC1;
static constexpr quint8 MAGICVALUE_UDP = 91;
static constexpr int CRYPT_HEADER_WITHOUTPADDING = 8;

static QByteArray md5(const QByteArray &b) { return QCryptographicHash::hash(b, QCryptographicHash::Md5); }

bool isProtocolMarker(quint8 b)
{
    switch (b) {
    case PROT_EMULE: case PROT_KADPACK: case PROT_KAD: case 0xA3: case 0xB2: case PROT_PACKED:
        return true;
    default:
        return false;
    }
}

quint32 udpVerifyKey(quint32 baseKey, quint32 ed2kIp)
{
    Writer w(8);
    w.u32(ed2kIp).u32(baseKey);   // uint64 (key << 32 | ip), little-endian in memory
    const QByteArray h = md5(w.data());
    Reader r(h);
    const quint32 x = r.u32() ^ r.u32() ^ r.u32() ^ r.u32();
    return (x % 0xFFFFFFFEu) + 1;
}

static quint8 pickMarker(bool kad, bool viaReceiverKey, const ObfuscationParams &prm)
{
    for (int i = 0; i < 128; ++i) {
        quint8 m = prm.fixed ? prm.marker : quint8(QRandomGenerator::global()->bounded(256));
        if (kad) m = viaReceiverKey ? quint8((m & 0xFC) | 0x02) : quint8(m & 0xFC);
        else m = quint8(m | 0x01);
        if (!isProtocolMarker(m)) return m;
        if (prm.fixed) break;
    }
    return kad ? (viaReceiverKey ? 0x02 : 0x00) : 0x01;
}

static QByteArray finishObfuscation(const QByteArray &plain, const QByteArray &key, quint8 marker, quint16 rnd,
                                    bool kad, quint32 receiverKey, quint32 senderKey)
{
    Rc4 rc4(md5(key), false);
    Writer hdr(16);
    hdr.u32(MAGICVALUE_UDP_SYNC_CLIENT).u8(0);           // magic, padding length 0
    if (kad) hdr.u32(receiverKey).u32(senderKey);
    QByteArray enc = hdr.take() + plain;
    rc4.process(enc.data(), enc.size());
    Writer out(enc.size() + 3);
    out.u8(marker).u16(rnd).bytes(enc);
    return out.take();
}

QByteArray obfuscateKad(const QByteArray &plain, const KadId *targetId, quint32 receiverVerifyKey,
                        quint32 senderVerifyKey, const ObfuscationParams &prm)
{
    const quint16 rnd = prm.fixed ? prm.randomKeyPart : quint16(QRandomGenerator::global()->bounded(65536));
    const bool useId = targetId && !targetId->isZero();
    if (!useId && receiverVerifyKey == 0) return QByteArray();
    Writer key(18);
    if (useId) key.bytes(targetId->toWire()).u16(rnd);
    else key.u32(receiverVerifyKey).u16(rnd);
    return finishObfuscation(plain, key.data(), pickMarker(true, !useId, prm), rnd, true, receiverVerifyKey, senderVerifyKey);
}

QByteArray obfuscateEd2k(const QByteArray &plain, const QByteArray &targetUserHash, quint32 ourPublicEd2kIp,
                         const ObfuscationParams &prm)
{
    if (targetUserHash.size() != 16) return QByteArray();
    const quint16 rnd = prm.fixed ? prm.randomKeyPart : quint16(QRandomGenerator::global()->bounded(65536));
    Writer key(23);
    key.bytes(targetUserHash).u32(ourPublicEd2kIp).u8(MAGICVALUE_UDP).u16(rnd);
    return finishObfuscation(plain, key.data(), pickMarker(false, false, prm), rnd, false, 0, 0);
}

Deobfuscated deobfuscate(const QByteArray &d, const KadId *ourKadId, const QByteArray &ourUserHash,
                         quint32 senderEd2kIp, quint32 verifyKeyForSender)
{
    Deobfuscated res;
    res.plain = d;
    if (d.size() <= CRYPT_HEADER_WITHOUTPADDING || isProtocolMarker(quint8(d[0]))) return res;
    const quint8 marker = quint8(d[0]);
    const QByteArray rnd = d.mid(1, 2);
    int tryKind = (marker & 0x03) == 3 ? 1 : (marker & 0x03);
    int tries = ourKadId ? 3 : 1;
    if (!ourKadId) tryKind = 1;
    for (; tries > 0; --tries, tryKind = (tryKind + 1) % 3) {
        QByteArray key;
        if (tryKind == 0) {
            if (!ourKadId) continue;
            key = ourKadId->toWire() + rnd;
        } else if (tryKind == 1) {
            if (ourUserHash.size() != 16) continue;
            Writer k(23);
            k.bytes(ourUserHash).u32(senderEd2kIp).u8(MAGICVALUE_UDP).bytes(rnd);
            key = k.take();
        } else {
            if (!ourKadId) continue;
            Writer k(6);
            k.u32(verifyKeyForSender).bytes(rnd);
            key = k.take();
        }
        Rc4 rc4(md5(key), false);
        QByteArray body = d.mid(3);
        rc4.process(body.data(), body.size());
        Reader r(body);
        if (r.u32() != MAGICVALUE_UDP_SYNC_CLIENT) continue;
        const quint8 pad = r.u8();
        qsizetype len = d.size() - CRYPT_HEADER_WITHOUTPADDING;
        if (len <= pad) return res;
        len -= pad;
        r.skip(pad);
        const bool kad = tryKind != 1;
        if (kad) {
            if (len <= 8) return res;
            res.receiverVerifyKey = r.u32();
            res.senderVerifyKey = r.u32();
            len -= 8;
        }
        if (!r.ok()) return res;
        res.obfuscated = true;
        res.kad = kad;
        res.viaReceiverKey = tryKind == 2;
        res.plain = body.right(len);
        return res;
    }
    return res;
}

} // namespace mx::ed2k::kad
