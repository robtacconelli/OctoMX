// SPDX-License-Identifier: GPL-3.0-or-later
#include "gnut_message.h"
#include "gnut_huge.h"
#include "gnut_zlib.h"

#include <QtCore/QXmlStreamReader>

namespace mx::gnut {

QString msgTypeName(quint8 t)
{
    switch (t) {
    case MsgPing: return QStringLiteral("Ping");
    case MsgPong: return QStringLiteral("Pong");
    case MsgBye: return QStringLiteral("Bye");
    case MsgQrp: return QStringLiteral("QRP");
    case MsgVendor: return QStringLiteral("Vendor");
    case MsgStdVendor: return QStringLiteral("StdVendor");
    case MsgPush: return QStringLiteral("Push");
    case MsgQuery: return QStringLiteral("Query");
    case MsgQueryHit: return QStringLiteral("QueryHit");
    default: return QStringLiteral("0x%1").arg(t, 2, 16, QLatin1Char('0'));
    }
}

QByteArray Message::encode() const
{
    QByteArray b;
    b.reserve(HeaderSize + payload.size());
    QByteArray g = guid.left(GuidSize);
    if (g.size() < GuidSize) g.append(QByteArray(GuidSize - g.size(), '\0'));
    b.append(g);
    putU8(b, type);
    putU8(b, ttl);
    putU8(b, hops);
    putU32(b, quint32(payload.size()));
    b.append(payload);
    return b;
}

Message Message::make(quint8 type, const QByteArray &payload, quint8 ttl, const QByteArray &guid)
{
    Message m;
    m.type = type;
    m.ttl = ttl;
    m.hops = 0;
    m.payload = payload;
    m.guid = guid.size() == GuidSize ? guid : randomGuid(true);
    return m;
}

bool Framer::next(Message *out)
{
    if (m_error || m_buf.size() < HeaderSize) return false;
    const auto *p = reinterpret_cast<const quint8 *>(m_buf.constData());
    const quint32 len = quint32(p[19]) | (quint32(p[20]) << 8) | (quint32(p[21]) << 16) | (quint32(p[22]) << 24);
    if (len > quint32(m_max)) {
        m_error = true;
        m_errorText = QStringLiteral("descriptor too large (%1 bytes, type 0x%2)").arg(len).arg(p[16], 2, 16, QLatin1Char('0'));
        return false;
    }
    if (m_buf.size() < HeaderSize + qint64(len)) return false;
    out->guid = m_buf.left(GuidSize);
    out->type = p[16];
    out->ttl = p[17];
    out->hops = p[18];
    out->payload = m_buf.mid(HeaderSize, int(len));
    m_buf.remove(0, HeaderSize + int(len));
    return true;
}

// ---------------------------------------------------------------- Pong
bool parsePong(const QByteArray &payload, Pong *out)
{
    Reader r(payload);
    out->port = r.u16();
    out->ip = r.ip();
    out->files = r.u32();
    out->kbytes = r.u32();
    if (!r.ok()) return false;
    if (r.left() > 0 && quint8(payload[r.pos()]) == GgepMagic) {
        int pos = r.pos();
        if (!parseGgep(payload, &pos, &out->ggep)) return true;   // keep the base pong
        if (const GgepExt *e = out->ggep.find("IPP")) out->ipp = unpackHosts6(e->data, 512);
        out->tls = out->ggep.has("TLS") || out->ggep.has("GTKG.TLS");   // LimeWire / gtk-gnutella names
        if (const GgepExt *e = out->ggep.find("IPP_TLS")) {
            const QList<bool> f = decodeTlsBits(e->data, int(out->ipp.size()));
            for (int i = 0; i < f.size(); ++i) if (f[i]) out->ippTls << out->ipp[i];
        }
        if (const GgepExt *e = out->ggep.find("UP")) {
            out->ultrapeer = true;
            if (e->data.size() >= 2) out->freeLeafSlots = quint8(e->data[1]);
        }
        if (const GgepExt *e = out->ggep.find("VC")) out->vendor = e->data.left(4);
        if (const GgepExt *e = out->ggep.find("DU")) out->dailyUptime = qint64(ggepUint(e->data));
        if (const GgepExt *e = out->ggep.find("PHC")) {
            const QList<QByteArray> lines = e->data.split('\n');
            for (const QByteArray &l : lines) {
                const QString s = QString::fromUtf8(l).trimmed();
                if (s.startsWith(QLatin1String("http://")) && s.size() < 300 && out->hostCaches.size() < 20)
                    out->hostCaches << s.section(QLatin1Char('\t'), 0, 0);
            }
        }
    }
    return true;
}

QByteArray buildPongPayload(const HostPort &self, quint32 files, quint32 kbytes, const GgepBlock &ggep)
{
    QByteArray b;
    putU16(b, self.port);
    putIp(b, self.ip);
    putU32(b, files);
    putU32(b, kbytes);
    b.append(encodeGgep(ggep));
    return b;
}

QList<bool> decodeTlsBits(const QByteArray &bits, int count)
{
    QList<bool> out;
    for (int i = 0; i < count; ++i) {
        const int byte = i / 8;
        out << (byte < bits.size() && (quint8(bits[byte]) & (0x80 >> (i % 8))));
    }
    return out;
}

QByteArray encodeTlsBits(const QList<bool> &flags)
{
    QByteArray b((int(flags.size()) + 7) / 8, '\0');
    for (int i = 0; i < flags.size(); ++i) if (flags[i]) b[i / 8] = char(quint8(b[i / 8]) | (0x80 >> (i % 8)));
    while (!b.isEmpty() && b.endsWith('\0')) b.chop(1);   // trailing zero bytes are left out
    return b;
}

QByteArray buildUhcPingPayload()
{
    GgepBlock g;
    g.add("SCP", QByteArray(1, '\0'));        // flags 0: we are a leaf; return ultrapeers
    QByteArray vc(VendorCode, 4);
    vc.append(char(0x40));                      // version 4.0
    g.add("VC", vc);
    return encodeGgep(g);
}

// ---------------------------------------------------------------- Bye
QByteArray buildByePayload(quint16 code, const QString &reason)
{
    QByteArray b;
    putU16(b, code);
    b.append(reason.toUtf8().left(200));
    b.append('\0');
    return b;
}

bool parseBye(const QByteArray &payload, quint16 *code, QString *reason)
{
    Reader r(payload);
    *code = r.u16();
    if (!r.ok()) return false;
    QByteArray rest = payload.mid(2, 512);
    const int nul = int(rest.indexOf('\0'));
    if (nul >= 0) rest.truncate(nul);
    *reason = QString::fromUtf8(rest).trimmed();
    return true;
}

// ---------------------------------------------------------------- Vendor
bool parseVendor(const QByteArray &payload, VendorMsg *out)
{
    Reader r(payload);
    out->vendor = r.bytes(4);
    out->id = r.u16();
    out->version = r.u16();
    if (!r.ok()) return false;
    out->body = payload.mid(8);
    return true;
}

QByteArray buildVendorPayload(const QByteArray &vendor, quint16 id, quint16 version, const QByteArray &body)
{
    QByteArray b = vendor.left(4);
    if (b.size() < 4) b.append(QByteArray(4 - b.size(), '\0'));
    putU16(b, id);
    putU16(b, version);
    b.append(body);
    return b;
}

QList<VendorKey> parseMessagesSupported(const QByteArray &body)
{
    QList<VendorKey> out;
    Reader r(body);
    const int n = r.u16();
    for (int i = 0; i < n && i < 256 && r.ok(); ++i) {
        VendorKey k;
        k.vendor = r.bytes(4);
        k.id = r.u16();
        k.version = r.u16();
        if (r.ok()) out << k;
    }
    return out;
}

QByteArray buildMessagesSupportedBody(const QList<VendorKey> &keys)
{
    QByteArray b;
    putU16(b, quint16(keys.size()));
    for (const VendorKey &k : keys) {
        b.append(k.vendor.left(4));
        putU16(b, k.id);
        putU16(b, k.version);
    }
    return b;
}

// ---------------------------------------------------------------- Push
bool parsePush(const QByteArray &payload, PushMsg *out)
{
    Reader r(payload);
    out->serventGuid = r.bytes(16);
    out->index = r.u32();
    out->ip = r.ip();
    out->port = r.u16();
    if (!r.ok()) return false;
    // [enc-1] optional GGEP block after the 26 bytes: "TLS" = the requester takes TLS
    out->tls = false;
    int pos = 26;
    if (payload.size() > pos && quint8(payload[pos]) == GgepMagic) {
        GgepBlock g;
        if (parseGgep(payload, &pos, &g)) out->tls = g.has("TLS");
    }
    return true;
}

QByteArray buildPushPayload(const PushMsg &p)
{
    QByteArray b = p.serventGuid.left(16);
    putU32(b, p.index);
    putIp(b, p.ip);
    putU16(b, p.port);
    if (p.tls) {   // [enc-1]
        GgepBlock g;
        g.add("TLS", QByteArray());
        b.append(encodeGgep(g));
    }
    return b;
}

// ---------------------------------------------------------------- extension areas
namespace {
// Parses a HUGE/GGEP extension area starting at *pos, ending at a NUL (consumed) or `end`.
// Calls onUrn(string), onGgep(block), onXml(bytes). Returns false only on gross malformation.
template <typename FUrn, typename FGgep, typename FXml>
bool parseExtArea(const QByteArray &b, int *pos, int end, FUrn onUrn, FGgep onGgep, FXml onXml)
{
    int p = *pos;
    int guard = 0;
    while (p < end && ++guard < 64) {
        const quint8 c = quint8(b[p]);
        if (c == 0) { ++p; break; }
        if (c == 0x1C) { ++p; continue; }
        if (c == GgepMagic) {
            GgepBlock g;
            int q = p;
            if (parseGgep(b, &q, &g) && q <= end) {
                onGgep(g);
                p = q;
                continue;
            }
            // malformed GGEP: skip to the next separator / NUL
            int nul = int(b.indexOf('\0', p));
            p = (nul < 0 || nul >= end) ? end : nul;
            continue;
        }
        int q = p;
        while (q < end && b[q] != '\0' && quint8(b[q]) != 0x1C) ++q;
        const QByteArray elem = b.mid(p, q - p);
        if (elem.startsWith("urn:") || elem.startsWith("URN:")) onUrn(elem);
        else if (elem.startsWith('<') || elem.startsWith('{')) onXml(elem);
        p = q;
    }
    *pos = p;
    return true;
}
} // namespace

// ---------------------------------------------------------------- Query
int metaMaskForType(mx::MediaType t)
{
    switch (t) {
    case MediaType::Audio: return 0x04;
    case MediaType::Video: return 0x08;
    case MediaType::Document: return 0x10;
    case MediaType::Image: return 0x20;
    case MediaType::Software: return 0x40 | 0x80;
    case MediaType::Archive: return 0x40 | 0x80;   // [ui-f] LimeWire files zip / rar / 7z ... under "programs"
    default: return 0;
    }
}

bool parseQuery(const QByteArray &payload, QueryMsg *out)
{
    Reader r(payload);
    out->flags = r.u16();
    if (!r.ok()) return false;
    int nul = int(payload.indexOf('\0', 2));
    const int textEnd = nul < 0 ? int(payload.size()) : nul;
    if (textEnd - 2 > 4096) return false;
    out->text = QString::fromUtf8(payload.mid(2, textEnd - 2));
    int pos = nul < 0 ? int(payload.size()) : nul + 1;
    parseExtArea(
        payload, &pos, int(payload.size()),
        [&](const QByteArray &u) {
            const QString s = QString::fromLatin1(u);
            if (s.compare(QLatin1String("urn:"), Qt::CaseInsensitive) == 0 ||
                s.compare(QLatin1String("urn:sha1:"), Qt::CaseInsensitive) == 0) { out->wantsAnyUrn = true; return; }
            FileHashes h;
            if (parseUrn(s, &h) && !h.sha1.isEmpty() && out->sha1s.size() < 8) out->sha1s << h.sha1;
        },
        [&](const GgepBlock &g) {
            out->ggep.exts += g.exts;
            if (const GgepExt *e = g.find("XQ")) out->extendedText = QString::fromUtf8(e->data.left(512));
            if (const GgepExt *e = g.find("M")) out->metaMask = int(ggepUint(e->data) & 0xff);
            if (const GgepExt *e = g.find("H")) {
                FileHashes h;
                if (parseGgepH(e->data, &h) && !h.sha1.isEmpty() && out->sha1s.size() < 8) out->sha1s << h.sha1;
            }
        },
        [&](const QByteArray &x) { if (out->xml.isEmpty()) out->xml = x.left(4096); });
    return true;
}

QByteArray buildQueryPayload(const QueryMsg &q)
{
    QByteArray b;
    putU16(b, q.flags);
    b.append(q.text.toUtf8());
    b.append('\0');
    QByteArray ext;
    for (const QByteArray &s : q.sha1s) {
        if (!ext.isEmpty()) ext.append(char(0x1C));
        ext.append(sha1Urn(s).toLatin1());
    }
    GgepBlock g = q.ggep;
    if (!q.extendedText.isEmpty()) g.add("XQ", q.extendedText.toUtf8());
    if (q.metaMask) g.add("M", ggepUintEncode(quint64(q.metaMask)));
    const QByteArray gg = encodeGgep(g, true);
    if (!gg.isEmpty()) {
        if (!ext.isEmpty()) ext.append(char(0x1C));
        ext.append(gg);
    }
    if (!ext.isEmpty()) {
        b.append(ext);
        b.append('\0');
    }
    return b;
}

// ---------------------------------------------------------------- QueryHit
void applyXmlMetadata(const QByteArray &xmlIn, QList<HitResult> *results)
{
    if (xmlIn.isEmpty() || results->isEmpty()) return;
    QXmlStreamReader x(xmlIn);
    x.setNamespaceProcessing(false);      // LimeWire uses an undeclared xsi: prefix
    int seen = 0;
    while (!x.atEnd() && seen < 1024) {
        x.readNext();
        if (!x.isStartElement()) continue;
        ++seen;
        const QXmlStreamAttributes a = x.attributes();
        if (!a.hasAttribute(QLatin1String("index"))) continue;
        bool ok = false;
        const int idx = a.value(QLatin1String("index")).toInt(&ok);
        if (!ok || idx < 0 || idx >= results->size()) continue;
        HitResult &r = (*results)[idx];
        const auto num = [&](const char *name) {
            bool o = false;
            const int v = int(a.value(QLatin1String(name)).toString().section(QLatin1Char('.'), 0, 0).toInt(&o));
            return o ? v : -1;
        };
        int v = num("bitrate");
        if (v > 0 && v < 100000) r.bitrateKbps = v;
        for (const char *n : {"seconds", "length", "duration"}) {
            v = num(n);
            if (v > 0 && v < 1000000) { r.durationSec = v; break; }
        }
        v = num("sampleRate");
        if (v <= 0) v = num("samplerate");
        if (v > 0 && v < 1000000) r.sampleRateHz = v;
    }
}

bool parseQueryHit(const QByteArray &payload, QueryHit *out, QString *error)
{
    const auto fail = [&](const char *why) { if (error) *error = QString::fromLatin1(why); return false; };
    if (payload.size() < 11 + 16) return fail("too short");
    Reader r(payload);
    const int count = r.u8();
    out->port = r.u16();
    out->ip = r.ip();
    out->speed = r.u32();
    const int guidStart = int(payload.size()) - 16;
    out->serventGuid = payload.mid(guidStart, 16);
    int pos = r.pos();
    for (int i = 0; i < count; ++i) {
        if (pos + 8 > guidStart) return fail("truncated result");
        HitResult h;
        Reader rr(payload, pos);
        h.index = rr.u32();
        h.size = rr.u32();
        pos = rr.pos();
        const int nul = int(payload.indexOf('\0', pos));
        if (nul < 0 || nul >= guidStart || nul - pos > 1024) return fail("bad file name");
        h.name = QString::fromUtf8(payload.mid(pos, nul - pos));
        pos = nul + 1;
        // extension area until NUL
        parseExtArea(
            payload, &pos, guidStart,
            [&](const QByteArray &u) { parseUrn(QString::fromLatin1(u), &h.hashes); },
            [&](const GgepBlock &g) {
                if (const GgepExt *e = g.find("H")) parseGgepH(e->data, &h.hashes);
                if (const GgepExt *e = g.find("TT")) if (e->data.size() == 24 && h.hashes.tth.isEmpty()) h.hashes.tth = e->data;
                if (const GgepExt *e = g.find("LF")) {
                    bool ok = false;
                    const quint64 v = ggepUint(e->data, &ok);
                    if (ok && v > 0 && v < (quint64(1) << 50)) h.size = v;
                }
                if (const GgepExt *e = g.find("ALT")) h.altLocs = unpackHosts6(e->data, 50);
                if (const GgepExt *e = g.find("ALT_TLS")) {
                    const QList<bool> f = decodeTlsBits(e->data, int(h.altLocs.size()));
                    for (int i = 0; i < f.size(); ++i) if (f[i]) h.altLocsTls << h.altLocs[i];
                }
                if (const GgepExt *e = g.find("PATH")) h.path = QString::fromUtf8(e->data.left(1024));
                if (const GgepExt *e = g.find("CT")) h.createTime = qint64(ggepUint(e->data));
            },
            [&](const QByteArray &) {});
        if (h.name.isEmpty()) continue;
        out->results << h;
    }
    // trailer (QHD) between pos and guidStart
    const int trailerLen = guidStart - pos;
    if (trailerLen >= 5) {
        Reader t(payload, pos);
        out->vendor = t.bytes(4);
        const int openSize = t.u8();
        if (t.ok() && openSize <= trailerLen - 5) {
            out->hasQhd = true;
            const QByteArray open = payload.mid(t.pos(), openSize);
            int xmlSize = 0;
            if (openSize == 1) {
                out->push = open[0] & 0x01;
                out->pushKnown = true;
            } else if (openSize >= 2) {
                const quint8 b0 = quint8(open[0]), b1 = quint8(open[1]);
                out->pushKnown = b1 & 0x01;
                out->push = out->pushKnown && (b0 & 0x01);
                out->busyKnown = b0 & 0x04;
                out->busy = out->busyKnown && (b1 & 0x04);
                out->uploadedKnown = b0 & 0x08;
                out->uploaded = out->uploadedKnown && (b1 & 0x08);
                out->measuredKnown = b0 & 0x10;
                out->measured = out->measuredKnown && (b1 & 0x10);
                out->ggepInTrailer = (b0 & 0x20) && (b1 & 0x20);
                if (openSize >= 4) xmlSize = quint8(open[2]) | (quint8(open[3]) << 8);
            }
            const int privStart = t.pos() + openSize;
            int privEnd = guidStart;
            if (xmlSize > 0 && xmlSize <= privEnd - privStart) {
                QByteArray xml = payload.mid(privEnd - xmlSize, xmlSize);
                privEnd -= xmlSize;
                while (xml.endsWith('\0')) xml.chop(1);
                if (xml.startsWith("{deflate}")) {
                    bool ok = false;
                    xml = inflateAll(xml.mid(9), 65536, &ok);
                    if (!ok) xml.clear();
                    while (xml.endsWith('\0')) xml.chop(1);
                } else if (xml.startsWith("{plaintext}")) {
                    xml = xml.mid(11);
                } else if (xml.startsWith("{}")) {
                    xml = xml.mid(2);
                }
                out->xml = xml;
            }
            // GGEP in the private area
            int gp = int(payload.indexOf(char(GgepMagic), privStart));
            if (gp >= 0 && gp < privEnd && gp - privStart < 64) {
                GgepBlock g;
                if (parseGgep(payload.left(privEnd), &gp, &g)) {
                    if (const GgepExt *e = g.find("PUSH")) out->pushProxies = unpackHosts6(e->data, 16);
                    if (const GgepExt *e = g.find("PUSH_TLS")) {
                        const QList<bool> f = decodeTlsBits(e->data, int(out->pushProxies.size()));
                        for (int i = 0; i < f.size(); ++i) if (f[i]) out->pushProxiesTls << out->pushProxies[i];
                    }
                    out->browseHost = g.has("BH");
                    out->tls = g.has("TLS") || g.has("GTKG.TLS");
                }
            }
        }
    }
    if (!out->xml.isEmpty()) applyXmlMetadata(out->xml, &out->results);
    return true;
}

QByteArray buildQueryHitPayload(const HostPort &self, quint32 speedKbps, const QList<HitResult> &results,
                                const HitTrailer &t)
{
    QByteArray b;
    const int n = qMin(int(results.size()), 255);
    putU8(b, quint8(n));
    putU16(b, self.port);
    putIp(b, self.ip);
    putU32(b, speedKbps);
    for (int i = 0; i < n; ++i) {
        const HitResult &r = results[i];
        putU32(b, r.index);
        putU32(b, r.size > 0xFFFFFFFFull ? 0xFFFFFFFFu : quint32(r.size));
        QByteArray name = r.name.toUtf8();
        name.replace('\0', '_');
        b.append(name.left(1000));
        b.append('\0');
        QByteArray ext;
        if (r.hashes.sha1.size() == 20) ext.append(sha1Urn(r.hashes.sha1).toLatin1());
        GgepBlock g;
        if (r.hashes.tth.size() == 24) g.add("TT", r.hashes.tth);
        if (r.size > 0xFFFFFFFFull) g.add("LF", ggepUintEncode(r.size));
        if (!r.altLocs.isEmpty()) {
            QByteArray alt;
            for (const HostPort &h : r.altLocs.mid(0, 10)) alt.append(packHost6(h));
            g.add("ALT", alt);
        }
        const QByteArray gg = encodeGgep(g, true);
        if (!gg.isEmpty()) {
            if (!ext.isEmpty()) ext.append(char(0x1C));
            ext.append(gg);
        }
        b.append(ext);
        b.append('\0');
    }
    // QHD
    b.append(t.vendor.left(4));
    GgepBlock tg;
    if (t.browseHost) tg.add("BH", QByteArray());
    if (t.tls) tg.add("TLS", QByteArray());   // [enc-1]
    if (t.firewalled && !t.pushProxies.isEmpty()) {
        QByteArray pp;
        for (const HostPort &h : t.pushProxies.mid(0, 4)) pp.append(packHost6(h));
        tg.add("PUSH", pp);
    }
    const QByteArray tgg = encodeGgep(tg);
    putU8(b, 2);
    quint8 b0 = 0x04 | 0x08 | (tgg.isEmpty() ? 0 : 0x20);
    quint8 b1 = 0x01;
    if (t.firewalled) b0 |= 0x01;
    if (t.busy) b1 |= 0x04;
    if (t.uploaded) b1 |= 0x08;
    if (!tgg.isEmpty()) b1 |= 0x20;
    putU8(b, b0);
    putU8(b, b1);
    b.append(tgg);
    QByteArray g = t.serventGuid.left(16);
    if (g.size() < 16) g.append(QByteArray(16 - g.size(), '\0'));
    b.append(g);
    return b;
}

} // namespace mx::gnut
