// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests of the Gnutella module's codecs and parsers (no network).
#include "gnut_ggep.h"
#include "gnut_headers.h"
#include "gnut_hostcache.h"
#include "gnut_http.h"
#include "gnut_huge.h"
#include "gnut_message.h"
#include "gnut_qrp.h"
#include "gnut_source.h"
#include "gnut_thex.h"
#include "gnut_util.h"
#include "gnut_zlib.h"

#include "mx/settings_keys.h"
#include "mxhash/encoding.h"
#include "mxhash/tiger_tree.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QRandomGenerator>
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>

using namespace mx::gnut;

class TstGnutellaUnit : public QObject {
    Q_OBJECT
private slots:
    void framing();
    void framingTooLarge();
    void cobs();
    void ggepRoundTrip();
    void ggepKnownBytes();
    void ggepUintCodec();
    void hugeUrns();
    void queryRoundTrip();
    void queryHitRoundTrip();
    void queryHitXml();
    void pongIpp();
    void vendorAndPush();
    void qrpHashVectors();
    void qrpPatchRoundTrip();
    void qrpWords();
    void handshakeParser();
    void headerReaderLimits();
    void httpRanges();
    void httpQueueHeaders();
    void altLocs();
    void thexRoundTrip();
    void sourceDescUri();
    void hostCachePersistence();
    void zlibStreaming();
    void fuzzParsers();
    void modernSpeedLabels();
};

void TstGnutellaUnit::framing()
{
    QList<Message> msgs;
    for (int i = 0; i < 50; ++i) {
        Message m = Message::make(quint8(i % 2 ? MsgQuery : MsgPing), QByteArray(i * 37, char('a' + i % 26)), quint8(1 + i % 7));
        m.hops = quint8(i % 3);
        msgs << m;
    }
    QByteArray stream;
    for (const Message &m : msgs) stream += m.encode();
    Framer f;
    QRandomGenerator r(42);
    int pos = 0, got = 0;
    while (pos < stream.size()) {
        const int n = 1 + int(r.bounded(300));
        f.feed(stream.mid(pos, n));
        pos += n;
        Message m;
        while (f.next(&m)) {
            const Message &e = msgs[got++];
            QCOMPARE(m.guid, e.guid);
            QCOMPARE(m.type, e.type);
            QCOMPARE(m.ttl, e.ttl);
            QCOMPARE(m.hops, e.hops);
            QCOMPARE(m.payload, e.payload);
        }
    }
    QCOMPARE(got, msgs.size());
    QVERIFY(!f.error());
}

void TstGnutellaUnit::framingTooLarge()
{
    QByteArray h(16, '\x11');
    putU8(h, MsgQuery);
    putU8(h, 3);
    putU8(h, 0);
    putU32(h, 70000);
    Framer f;
    f.feed(h);
    Message m;
    QVERIFY(!f.next(&m));
    QVERIFY(f.error());
}

void TstGnutellaUnit::cobs()
{
    QCOMPARE(cobsEncode(QByteArray("\x00", 1)), QByteArray("\x01\x01", 2));
    QCOMPARE(cobsEncode(QByteArray("\x11\x22\x00\x33", 4)), QByteArray("\x03\x11\x22\x02\x33", 5));
    QCOMPARE(cobsEncode(QByteArray("\x11\x22\x33\x44", 4)), QByteArray("\x05\x11\x22\x33\x44", 5));
    QByteArray big(254, '\x01');
    const QByteArray e = cobsEncode(big);
    QCOMPARE(e.size(), 256);
    QCOMPARE(quint8(e[0]), quint8(0xFF));
    QCOMPARE(quint8(e[255]), quint8(0x01));
    QRandomGenerator r(7);
    for (int i = 0; i < 300; ++i) {
        QByteArray d(int(r.bounded(700)), Qt::Uninitialized);
        for (char &c : d) c = char(r.bounded(4) == 0 ? 0 : r.bounded(256));
        const QByteArray enc = cobsEncode(d);
        QVERIFY(!enc.contains('\0'));
        bool ok = false;
        QCOMPARE(cobsDecode(enc, &ok), d);
        QVERIFY(ok);
    }
}

void TstGnutellaUnit::ggepRoundTrip()
{
    GgepBlock b;
    b.add("H", QByteArray("\x01", 1) + QByteArray(20, '\x00'));       // NULs → COBS when noNul
    b.add("LF", ggepUintEncode(5000000000ULL));
    b.add("BH", QByteArray());
    b.add("ALT", QByteArray(6 * 100, '\x07'));                          // 600 bytes: 2-byte length
    b.add("XML", QByteArray(5000, 'x'), true);                          // compressed
    for (bool noNul : {false, true}) {
        const QByteArray enc = encodeGgep(b, noNul);
        if (noNul) QVERIFY(!enc.contains('\0'));
        GgepBlock out;
        int pos = 0;
        QVERIFY(parseGgep(enc, &pos, &out));
        QCOMPARE(pos, enc.size());
        QCOMPARE(out.exts.size(), b.exts.size());
        for (int i = 0; i < b.exts.size(); ++i) {
            QCOMPARE(out.exts[i].id, b.exts[i].id);
            QCOMPARE(out.exts[i].data, b.exts[i].data);
        }
        QCOMPARE(ggepUint(out.value("LF")), 5000000000ULL);
    }
    // compression really happened
    GgepBlock c;
    c.add("XML", QByteArray(5000, 'x'), true);
    QVERIFY(encodeGgep(c).size() < 200);
}

void TstGnutellaUnit::ggepKnownBytes()
{
    // "SCP" with one 0x00 byte, as a UHC ping carries it
    const QByteArray raw("\xC3\x83SCP\x41\x00", 7);
    GgepBlock g;
    int pos = 0;
    QVERIFY(parseGgep(raw, &pos, &g));
    QCOMPARE(g.exts.size(), 1);
    QCOMPARE(g.exts[0].id, QByteArray("SCP"));
    QCOMPARE(g.exts[0].data, QByteArray(1, '\0'));
    // malformed: id length 0, reserved bit, truncated length, missing last flag
    for (const QByteArray &bad : {QByteArray("\xC3\x80", 2), QByteArray("\xC3\x91H\x40", 4), QByteArray("\xC3\x81H\x80", 4),
                                  QByteArray("\xC3\x01H\x40", 4), QByteArray("\xC3\x81H\x45" "ab", 6)}) {
        GgepBlock x;
        int p = 0;
        QVERIFY(!parseGgep(bad, &p, &x));
    }
}

void TstGnutellaUnit::ggepUintCodec()
{
    for (quint64 v : {0ULL, 1ULL, 255ULL, 256ULL, 65535ULL, 0x100000000ULL, 0xFFFFFFFFFFFFFFFFULL}) {
        bool ok = false;
        QCOMPARE(ggepUint(ggepUintEncode(v), &ok), v);
        QVERIFY(ok);
    }
    QCOMPARE(ggepUintEncode(4294967296ULL).size(), 5);
}

void TstGnutellaUnit::hugeUrns()
{
    const QByteArray sha1 = QCryptographicHash::hash("hello", QCryptographicHash::Sha1);
    const QByteArray tth = mx::hash::TigerTree::leafHash(QByteArray("hello"));
    mx::FileHashes h;
    QVERIFY(parseUrn(sha1Urn(sha1), &h));
    QCOMPARE(h.sha1, sha1);
    QCOMPARE(sha1Urn(sha1), QStringLiteral("urn:sha1:") + mx::hash::toBase32(sha1));
    mx::FileHashes h2;
    QVERIFY(parseUrn(bitprintUrn(sha1, tth).toUpper().replace(QStringLiteral("URN:BITPRINT:"), QStringLiteral("urn:bitprint:")), &h2));
    QCOMPARE(h2.sha1, sha1);
    QCOMPARE(h2.tth, tth);
    mx::FileHashes h3;
    QVERIFY(parseUrn(tthUrn(tth), &h3));
    QCOMPARE(h3.tth, tth);
    mx::FileHashes h4;
    QVERIFY(!parseUrn(QStringLiteral("urn:sha1:TOOSHORT"), &h4));
    QVERIFY(!parseUrn(QStringLiteral("urn:md5:abc"), &h4));
    // GGEP "H"
    mx::FileHashes g1, g2;
    QVERIFY(parseGgepH(encodeGgepH(sha1), &g1));
    QCOMPARE(g1.sha1, sha1);
    QVERIFY(parseGgepH(encodeGgepH(sha1, tth), &g2));
    QCOMPARE(g2.tth, tth);
    QVERIFY(!parseGgepH(QByteArray("\x01\x02", 2), &g1));
}

void TstGnutellaUnit::queryRoundTrip()
{
    QueryMsg q;
    q.flags = QfMark | QfXml | QfGgepH | QfFirewalled;
    q.text = QStringLiteral("creative commons");
    q.extendedText = QStringLiteral("creative commons very long extended query text beyond thirty");
    q.sha1s << QCryptographicHash::hash("x", QCryptographicHash::Sha1);
    q.metaMask = 0x04;
    const QByteArray p = buildQueryPayload(q);
    QueryMsg o;
    QVERIFY(parseQuery(p, &o));
    QCOMPARE(o.flags, q.flags);
    QCOMPARE(o.text, q.text);
    QCOMPARE(o.extendedText, q.extendedText);
    QCOMPARE(o.sha1s, q.sha1s);
    QCOMPARE(o.metaMask, 4);
    // [ui-f] every type of the Search menu asks for the LimeWire meta type (archives are "programs")
    QCOMPARE(metaMaskForType(mx::MediaType::Video), 0x08);
    QCOMPARE(metaMaskForType(mx::MediaType::Image), 0x20);
    QCOMPARE(metaMaskForType(mx::MediaType::Document), 0x10);
    QCOMPARE(metaMaskForType(mx::MediaType::Software), 0xC0);
    QCOMPARE(metaMaskForType(mx::MediaType::Archive), 0xC0);
    QCOMPARE(metaMaskForType(mx::MediaType::Any), 0);
    QVERIFY(o.firewalled());
    QVERIFY(!o.wantsOob());
    // a classic query with a bare "urn:" request and no GGEP
    QueryMsg c;
    QVERIFY(parseQuery(QByteArray("\x00\x00test\x00urn:\x00", 11), &c));
    QCOMPARE(c.text, QStringLiteral("test"));
    QVERIFY(c.wantsAnyUrn);
}

void TstGnutellaUnit::queryHitRoundTrip()
{
    QList<HitResult> rs;
    for (int i = 0; i < 3; ++i) {
        HitResult r;
        r.index = quint32(100 + i);
        r.size = i == 2 ? 6000000000ULL : quint64(1000 * (i + 1));
        r.name = QStringLiteral("file %1 ünïcode.mp3").arg(i);
        r.hashes.sha1 = QCryptographicHash::hash(QByteArray::number(i), QCryptographicHash::Sha1);
        if (i != 1) r.hashes.tth = QByteArray(24, char(0x10 + i)).replace(3, 1, QByteArray(1, '\0'));   // with a NUL byte
        if (i == 0) r.altLocs = {HostPort::parse(QStringLiteral("1.2.3.4:6346")), HostPort::parse(QStringLiteral("5.6.7.8:1234"))};
        rs << r;
    }
    HitTrailer t;
    t.firewalled = true;
    t.busy = true;
    t.uploaded = true;
    t.browseHost = true;
    t.pushProxies = {HostPort::parse(QStringLiteral("9.9.9.9:6346"))};
    t.serventGuid = randomGuid();
    const HostPort self = HostPort::parse(QStringLiteral("80.1.2.3:6347"));
    const QByteArray p = buildQueryHitPayload(self, 1234, rs, t);
    QueryHit h;
    QString err;
    QVERIFY2(parseQueryHit(p, &h, &err), qPrintable(err));
    QCOMPARE(h.ip, self.ip);
    QCOMPARE(h.port, self.port);
    QCOMPARE(h.speed, 1234u);
    QCOMPARE(h.results.size(), 3);
    for (int i = 0; i < 3; ++i) {
        QCOMPARE(h.results[i].index, rs[i].index);
        QCOMPARE(h.results[i].size, rs[i].size);
        QCOMPARE(h.results[i].name, rs[i].name);
        QCOMPARE(h.results[i].hashes.sha1, rs[i].hashes.sha1);
        QCOMPARE(h.results[i].hashes.tth, rs[i].hashes.tth);
    }
    QCOMPARE(h.results[0].altLocs.size(), 2);
    QCOMPARE(h.vendor, QByteArray("OCMX"));
    QVERIFY(h.hasQhd && h.pushKnown && h.push && h.busyKnown && h.busy && h.uploaded);
    QCOMPARE(h.pushProxies.size(), 1);
    QCOMPARE(h.pushProxies[0].toString(), QStringLiteral("9.9.9.9:6346"));
    QVERIFY(h.browseHost);
    QCOMPARE(h.serventGuid, t.serventGuid);
    // not firewalled, not busy
    HitTrailer t2;
    t2.serventGuid = t.serventGuid;
    t2.browseHost = false;
    QueryHit h2;
    QVERIFY(parseQueryHit(buildQueryHitPayload(self, 10, rs.mid(0, 1), t2), &h2));
    QVERIFY(h2.pushKnown && !h2.push && h2.busyKnown && !h2.busy && !h2.browseHost);
}

void TstGnutellaUnit::queryHitXml()
{
    // LimeWire layout: open data size 4 (flags, flags, xml size LE), XML at the end of the private area
    for (bool deflated : {false, true}) {
        QByteArray p;
        putU8(p, 2);
        putU16(p, 6346);
        putIp(p, 0x01020304);
        putU32(p, 300);
        for (int i = 0; i < 2; ++i) {
            putU32(p, quint32(i));
            putU32(p, 4000000);
            p.append(QByteArray("song") + QByteArray::number(i) + ".mp3");
            p.append('\0');
            p.append('\0');
        }
        const QByteArray xml = "<?xml version=\"1.0\"?><audios xsi:noNamespaceSchemaLocation=\"http://www.limewire.com/schemas/audio.xsd\">"
                               "<audio title=\"a\" bitrate=\"192\" seconds=\"201\" index=\"0\"/>"
                               "<audio bitrate=\"128\" seconds=\"99\" sampleRate=\"44100\" index=\"1\"/></audios>";
        QByteArray xmlBlock = deflated ? QByteArray("{deflate}") + deflateAll(xml) : xml;
        xmlBlock.append('\0');
        p.append("LIME");
        putU8(p, 4);
        putU8(p, 0x1c);                  // meaningful: busy, uploaded, speed
        putU8(p, 0x01);                  // push meaningful, not set
        putU16(p, quint16(xmlBlock.size()));
        p.append(xmlBlock);
        p.append(QByteArray(16, '\x42'));
        QueryHit h;
        QVERIFY(parseQueryHit(p, &h));
        QCOMPARE(h.results.size(), 2);
        QCOMPARE(h.results[0].bitrateKbps, 192);
        QCOMPARE(h.results[0].durationSec, 201);
        QCOMPARE(h.results[1].bitrateKbps, 128);
        QCOMPARE(h.results[1].sampleRateHz, 44100);
        QVERIFY(h.pushKnown && !h.push && h.busyKnown && !h.busy);
        QCOMPARE(vendorName(h.vendor), QStringLiteral("LimeWire"));
    }
}

void TstGnutellaUnit::pongIpp()
{
    GgepBlock g;
    QByteArray ipp;
    for (int i = 1; i <= 5; ++i) ipp.append(packHost6(HostPort{quint32(0x50000000 + i), quint16(6346 + i)}));
    g.add("IPP", ipp);
    g.add("UP", QByteArray("\x01\x05\x02", 3));
    g.add("VC", QByteArray("GTKG\x11", 5));
    const QByteArray payload = buildPongPayload(HostPort::parse(QStringLiteral("80.1.1.1:6346")), 10, 20, g);
    Pong p;
    QVERIFY(parsePong(payload, &p));
    QCOMPARE(p.ipp.size(), 5);
    QCOMPARE(p.ipp[4].port, quint16(6351));
    QVERIFY(p.ultrapeer);
    QCOMPARE(p.freeLeafSlots, 5);
    QCOMPARE(p.vendor, QByteArray("GTKG"));
    QCOMPARE(p.files, 10u);
    // the UHC ping we send
    const QByteArray ping = buildUhcPingPayload();
    GgepBlock pg;
    int pos = 0;
    QVERIFY(parseGgep(ping, &pos, &pg));
    QVERIFY(pg.has("SCP"));
}

void TstGnutellaUnit::vendorAndPush()
{
    const QList<VendorKey> keys = {{QByteArray("BEAR"), 4, 1}, {QByteArray("LIME"), 21, 1}};
    const QByteArray ms = buildVendorPayload(QByteArray(4, '\0'), 0, 0, buildMessagesSupportedBody(keys));
    VendorMsg v;
    QVERIFY(parseVendor(ms, &v));
    QCOMPARE(v.vendor, QByteArray(4, '\0'));
    const QList<VendorKey> back = parseMessagesSupported(v.body);
    QCOMPARE(back.size(), 2);
    QCOMPARE(back[1].vendor, QByteArray("LIME"));
    QCOMPARE(back[1].id, quint16(21));
    PushMsg p;
    p.serventGuid = randomGuid();
    p.index = 77;
    p.ip = 0x7f000001;
    p.port = 4567;
    PushMsg q;
    QVERIFY(parsePush(buildPushPayload(p), &q));
    QCOMPARE(q.serventGuid, p.serventGuid);
    QCOMPARE(q.index, 77u);
    QCOMPARE(q.ip, p.ip);
    QCOMPARE(q.port, p.port);
    quint16 code = 0;
    QString reason;
    QVERIFY(parseBye(buildByePayload(502, QStringLiteral("Shutdown")), &code, &reason));
    QCOMPARE(code, quint16(502));
    QCOMPARE(reason, QStringLiteral("Shutdown"));
}

void TstGnutellaUnit::qrpHashVectors()
{
    // Interop anchors: values printed in the public QRP specification ("Query Routing for the
    // Gnutella Network" 1.0, appendix "Java Implementation of Hash Function", reference unit tests).
    QCOMPARE(qrpHash(QString(), 13), 0u);
    QCOMPARE(qrpHash(QStringLiteral("eb"), 13), 6791u);
    QCOMPARE(qrpHash(QStringLiteral("ndflaleme"), 16), 45559u);
    QCOMPARE(qrpHash(QStringLiteral("zzzzzzzzzzz"), 10), 944u);
    QCOMPARE(qrpHash(QStringLiteral("3NJA9"), 10), 581u);          // case-insensitive
    // Our own inputs. Expected values computed independently from the spec's definition (XOR the
    // low byte of each lower-cased UTF-16 code unit into byte j%4 of a 32-bit word, multiply by
    // 0x4F1BBCDC mod 2^32, keep the top `bits` bits) with a separate Python implementation.
    QCOMPARE(qrpHash(QStringLiteral("octomx"), 16), 33490u);
    QCOMPARE(qrpHash(QStringLiteral("OctoMX"), 16), 33490u);
    QCOMPARE(qrpHash(QStringLiteral("sintel"), 13), 1459u);
    QCOMPARE(qrpHash(QStringLiteral("bunny"), 10), 719u);
    QCOMPARE(qrpHash(QStringLiteral("creative"), 16), 31933u);
    QCOMPARE(qrpHash(QStringLiteral("commons"), 13), 5082u);
    QCOMPARE(qrpHash(QStringLiteral("1080p"), 16), 55700u);
    QCOMPARE(qrpHash(QStringLiteral("mp3"), 10), 453u);
    QCOMPARE(qrpHash(QStringLiteral("a"), 16), 63874u);
    QCOMPARE(qrpHash(QStringLiteral("zz"), 13), 7527u);
    QCOMPARE(qrpHash(QStringLiteral("abcdefghijklmnopqrstuvwxyz"), 16), 19183u);
    // non-ASCII: per UTF-16 code unit, lower-cased, "& 0xff"; surrogates are taken as they are
    const auto u = [](std::initializer_list<char32_t> cps) { return QString::fromUcs4(std::data(cps), int(cps.size())); };
    QCOMPARE(qrpHash(u({0x63, 0x61, 0x66, 0xe9}), 16), 38385u);        // "cafe" with e-acute
    QCOMPARE(qrpHash(u({0x43, 0x41, 0x46, 0xc9}), 16), 38385u);        // upper case of the same
    QCOMPARE(qrpHash(u({0xfc, 0x62, 0x65, 0x72}), 13), 7948u);         // u-umlaut "ber"
    QCOMPARE(qrpHash(u({0x97f3, 0x697d}), 10), 650u);                  // two CJK ideographs
    QCOMPARE(qrpHash(u({0x1f600}), 13), 6963u);                        // outside the BMP (surrogate pair)
    QCOMPARE(qrpHash(u({0x78, 0x1f600, 0x79}), 16), 44288u);
}

void TstGnutellaUnit::qrpPatchRoundTrip()
{
    QrpTable t(65536);
    t.addFileName(QStringLiteral("Big_Buck_Bunny 1080p (Creative Commons).mp4"));
    t.addFileName(QStringLiteral("gutenberg - Alice in Wonderland.txt"));
    t.addUrn(QStringLiteral("urn:sha1:PLSTHIPQGSSZTS5FJUPAKUZWUGYQYPFB"));
    QVERIFY(t.filled() > 10);
    QVERIFY(t.matchesQuery(QStringLiteral("bunny creative")));
    QVERIFY(t.matchesQuery(QStringLiteral("alice wonderland")));
    QVERIFY(t.matchesQuery(QStringLiteral("wonderla")));          // truncated prefix inserted
    QVERIFY(!t.matchesQuery(QStringLiteral("zebra quantum")));
    for (int bits : {4, 8}) {
        for (bool comp : {true, false}) {
            const QList<QByteArray> msgs = t.encode(7, bits, comp, 1024);
            QVERIFY(msgs.size() >= 2);
            QCOMPARE(quint8(msgs[0][0]), quint8(0));
            if (comp) QVERIFY(msgs.size() <= 3);                  // a sparse 64 Ki table compresses well
            QrpReceiver r;
            for (const QByteArray &m : msgs) {
                QString err;
                QVERIFY2(r.handle(m, &err), qPrintable(err));
            }
            QVERIFY(r.complete());
            QCOMPARE(r.table()->nslots(), 65536);
            QVERIFY(*r.table() == t);
        }
    }
    // reset content: 0x00, size 65536 LE, infinity 7
    const QByteArray reset = t.encode().first();
    QCOMPARE(reset, QByteArray("\x00\x00\x00\x01\x00\x07", 6));
    // a patch out of sequence is rejected
    QrpReceiver bad;
    const QList<QByteArray> msgs = t.encode(7, 4, false, 4096);
    QVERIFY(bad.handle(msgs[0]));
    QVERIFY(!bad.handle(msgs[2]));
}

void TstGnutellaUnit::qrpWords()
{
    const QStringList w = qrpWordsForName(QStringLiteral("Ubuntu-24.04_Desktop.ISO"));
    QVERIFY(w.contains(QStringLiteral("ubuntu")));
    QVERIFY(w.contains(QStringLiteral("ubunt")));
    QVERIFY(w.contains(QStringLiteral("ubun")));
    QVERIFY(!w.contains(QStringLiteral("ubu")));
    QVERIFY(w.contains(QStringLiteral("24")));
    QVERIFY(w.contains(QStringLiteral("iso")));
    QVERIFY(w.contains(QStringLiteral("desktop")));
}

void TstGnutellaUnit::handshakeParser()
{
    const QByteArray resp = "GNUTELLA/0.6 200 OK\r\n"
                            "User-Agent: gtk-gnutella/1.2.3 (2025-01-01; GTK2; Linux x86_64)\r\n"
                            "x-ultrapeer: True\r\n"
                            "X-Try-Ultrapeers: 1.2.3.4:6346, 5.6.7.8:1111,\r\n"
                            "  9.9.9.9:2222, bogus, 300.1.1.1:5\r\n"
                            "Accept-Encoding: deflate\r\n"
                            "Content-Encoding: deflate\r\n"
                            "Remote-IP: 80.80.80.80\r\n"
                            "\r\n";
    HeaderReader r;
    for (int i = 0; i < resp.size(); i += 7) r.feed(resp.mid(i, 7));
    QVERIFY(r.complete());
    const HeaderBlock &b = r.block();
    QCOMPARE(b.code, 200);
    QCOMPARE(b.reason, QStringLiteral("OK"));
    QVERIFY(!b.isRequest);
    QCOMPARE(b.first(QStringLiteral("X-Ultrapeer")), QStringLiteral("True"));
    const QList<HostPort> hosts = parseHostList(b.value(QStringLiteral("X-Try-Ultrapeers")));
    QCOMPARE(hosts.size(), 3);
    QCOMPARE(hosts[2].toString(), QStringLiteral("9.9.9.9:2222"));
    QCOMPARE(b.first(QStringLiteral("remote-ip")), QStringLiteral("80.80.80.80"));
    HeaderBlock req;
    QVERIFY(parseHeaderBlock("GNUTELLA CONNECT/0.6\r\nUser-Agent: X\r\n\r\n", &req));
    QVERIFY(req.isRequest);
    QCOMPARE(req.method, QStringLiteral("GNUTELLA"));
    HeaderBlock rej;
    QVERIFY(parseHeaderBlock("GNUTELLA/0.6 503 Too many leaf connections (300 max)\n\n", &rej));
    QCOMPARE(rej.code, 503);
    QCOMPARE(rej.reason, QStringLiteral("Too many leaf connections (300 max)"));
    // remainder after the block is kept
    HeaderReader r2;
    r2.feed("HTTP/1.1 206 Partial Content\r\nContent-Length: 3\r\n\r\nabcEXTRA");
    QVERIFY(r2.complete());
    QCOMPARE(r2.takeRemainder(), QByteArray("abcEXTRA"));
}

void TstGnutellaUnit::headerReaderLimits()
{
    HeaderReader r(1024, 10, 100);
    r.feed(QByteArray("GNUTELLA/0.6 200 OK\r\nX: ") + QByteArray(200, 'a'));
    QVERIFY(r.error());
    HeaderReader r2(1024, 5, 100);
    r2.feed("A\r\nB: 1\r\nC: 1\r\nD: 1\r\nE: 1\r\nF: 1\r\nG: 1\r\n");
    QVERIFY(r2.error());
    HeaderReader r3(100, 50, 100);
    for (int i = 0; i < 20; ++i) r3.feed("X: 12345\r\n");
    QVERIFY(r3.error());
}

void TstGnutellaUnit::httpRanges()
{
    QList<Span> s;
    QVERIFY(parseRangeHeader(QStringLiteral("bytes=0-99"), 1000, &s));
    QCOMPARE(s, (QList<Span>{Span(0, 100)}));
    QVERIFY(parseRangeHeader(QStringLiteral("bytes=900-"), 1000, &s));
    QCOMPARE(s, (QList<Span>{Span(900, 1000)}));
    QVERIFY(parseRangeHeader(QStringLiteral("bytes=-50"), 1000, &s));
    QCOMPARE(s, (QList<Span>{Span(950, 1000)}));
    QVERIFY(parseRangeHeader(QStringLiteral("bytes = 990-2000"), 1000, &s));
    QCOMPARE(s, (QList<Span>{Span(990, 1000)}));
    QVERIFY(!parseRangeHeader(QStringLiteral("bytes=1000-1001"), 1000, &s));
    QVERIFY(!parseRangeHeader(QStringLiteral("bytes=5-3"), 1000, &s));
    QVERIFY(!parseRangeHeader(QStringLiteral("pages=1-2"), 1000, &s));
    QVERIFY(!parseRangeHeader(QStringLiteral("bytes=abc"), 1000, &s));
    qint64 a, b, t;
    QVERIFY(parseContentRange(QStringLiteral("bytes 0-99/1000"), &a, &b, &t));
    QCOMPARE(a, 0);
    QCOMPARE(b, 99);
    QCOMPARE(t, 1000);
    QVERIFY(parseContentRange(QStringLiteral("bytes=5-9/*"), &a, &b, &t));
    QCOMPARE(t, -1);
    QVERIFY(!parseContentRange(QStringLiteral("bytes 0-1000/1000"), &a, &b, &t));
    QVERIFY(!parseContentRange(QStringLiteral("bytes */1000"), &a, &b, &t));
    const QList<Span> av = parseAvailableRanges(QStringLiteral("bytes 0-1023, 2048-4095, 1024-2047, 9000-99999"), 10000);
    QCOMPARE(av, (QList<Span>{Span(0, 4096), Span(9000, 10000)}));
    QCOMPARE(formatAvailableRanges(av), QStringLiteral("bytes 0-4095, 9000-9999"));
}

void TstGnutellaUnit::httpQueueHeaders()
{
    QueueInfo q;
    parseXQueue(QStringLiteral("position=2, length=5, limit=4, pollMin=45, pollMax=120"), &q);
    QVERIFY(q.present);
    QCOMPARE(q.position, 2);
    QCOMPARE(q.length, 5);
    QCOMPARE(q.limit, 4);
    QCOMPARE(q.pollMin, 45);
    QCOMPARE(q.pollMax, 120);
    QueueInfo v;
    parseXQueue(QStringLiteral("0.1"), &v);
    QVERIFY(!v.present);
    QueueInfo p;
    parseXQueued(QStringLiteral("ID=0123456789ABCDEF; position=7; lifetime=300; length=10; ETA=600"), &p);
    QCOMPARE(p.id, QStringLiteral("0123456789ABCDEF"));
    QCOMPARE(p.position, 7);
    QCOMPARE(p.lifetime, 300);
    QCOMPARE(p.eta, 600);
    QCOMPARE(parseRetryAfter(QStringLiteral("120")), 120);
    QCOMPARE(parseRetryAfter(QStringLiteral("junk")), -1);
    QCOMPARE(parseRetryAfter(QStringLiteral("99999999")), 86400);
    const QString future = QLocale::c().toString(QDateTime::currentDateTimeUtc().addSecs(300), QStringLiteral("ddd, dd MMM yyyy HH:mm:ss 'GMT'"));
    const int r = parseRetryAfter(future);
    QVERIFY2(r >= 295 && r <= 301, qPrintable(QString::number(r)));
}

void TstGnutellaUnit::altLocs()
{
    const QList<HostPort> a = parseAltLocs(QStringLiteral("1.2.3.4:6346, 5.6.7.8, tls=C0, http://9.8.7.6:1234/uri-res/N2R?urn:sha1:ABC, bad"));
    QCOMPARE(a.size(), 3);
    QCOMPARE(a[1].toString(), QStringLiteral("5.6.7.8:6346"));
    QCOMPARE(a[2].toString(), QStringLiteral("9.8.7.6:1234"));
    QCOMPARE(formatAltLocs(a), QStringLiteral("1.2.3.4, 5.6.7.8, 9.8.7.6:1234"));
    PushLoc pl;
    pl.guid = randomGuid();
    pl.proxies = {HostPort::parse(QStringLiteral("1.1.1.1:1")), HostPort::parse(QStringLiteral("2.2.2.2:2"))};
    const QList<PushLoc> back = parsePushAltLocs(formatPushAltLoc(pl) + QStringLiteral(", ") +
                                                 mx::hash::toBase32(pl.guid) + QStringLiteral(";pptls=3;3.3.3.3:3"));
    QCOMPARE(back.size(), 2);
    QCOMPARE(back[0].guid, pl.guid);
    QCOMPARE(back[0].proxies.size(), 2);
    QCOMPARE(back[1].guid, pl.guid);
    QCOMPARE(back[1].proxies.size(), 1);
}

void TstGnutellaUnit::thexRoundTrip()
{
    for (qint64 size : {qint64(0), qint64(1000), qint64(300 * 1024 + 17), qint64(3 * 1024 * 1024 + 5)}) {
        QByteArray data(int(size), Qt::Uninitialized);
        QRandomGenerator r{quint32(size)};
        for (char &c : data) c = char(r.bounded(256));
        mx::hash::TigerTree tree;
        tree.update(data);
        const QByteArray root = tree.finish();
        const int depth = thexGoodDepth(size);
        QList<QList<QByteArray>> levels;
        for (int d = 0; d <= depth; ++d) levels << tree.level(d);
        const QByteArray dime = buildThex(size, levels);
        ThexTree t;
        QString err;
        QVERIFY2(parseThex(dime, size, root, &t, &err), qPrintable(err));
        QCOMPARE(t.depth, depth);
        QCOMPARE(t.root, root);
        QCOMPARE(t.deepest, tree.levelForUnit(t.unitSize));
        // wrong root
        QVERIFY(!parseThex(dime, size, QByteArray(24, 'x'), &t, &err));
        if (depth > 0) {
            QByteArray bad = dime;
            bad[bad.size() - 5] = char(bad[bad.size() - 5] ^ 0x55);   // flip a deepest-level byte
            QVERIFY(!parseThex(bad, size, root, &t, &err));
        }
    }
}

void TstGnutellaUnit::sourceDescUri()
{
    SourceDesc d;
    d.addr = HostPort::parse(QStringLiteral("81.2.3.4:6348"));
    d.index = 42;
    d.guid = randomGuid();
    d.firewalled = true;
    d.browse = true;
    d.proxies = {HostPort::parse(QStringLiteral("7.7.7.7:7777"))};
    d.name = QStringLiteral("Big Buck Bunny & friends #1 ?.ogg");
    d.sha1 = QCryptographicHash::hash("q", QCryptographicHash::Sha1);
    d.vendor = QStringLiteral("LimeWire");
    SourceDesc o;
    QVERIFY(SourceDesc::fromUri(d.toUri(), &o));
    QCOMPARE(o.addr, d.addr);
    QCOMPARE(o.index, d.index);
    QCOMPARE(o.guid, d.guid);
    QCOMPARE(o.firewalled, true);
    QCOMPARE(o.browse, true);
    QCOMPARE(o.proxies.size(), 1);
    QCOMPARE(o.name, d.name);
    QCOMPARE(o.sha1, d.sha1);
    QCOMPARE(o.vendor, d.vendor);
    QVERIFY(!SourceDesc::fromUri(QStringLiteral("http://x/"), &o));
}

void TstGnutellaUnit::hostCachePersistence()
{
    QTemporaryDir dir(QDir::currentPath() + QStringLiteral("/hc-XXXXXX"));
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("hosts.json"));
    {
        HostCache c;
        c.setPath(path);
        QVERIFY(c.add(HostPort::parse(QStringLiteral("80.1.1.1:6346")), QStringLiteral("gwc")));
        QVERIFY(!c.add(HostPort::parse(QStringLiteral("80.1.1.1:6346")), QStringLiteral("gwc")));
        c.add(HostPort::parse(QStringLiteral("80.1.1.2:6346")), QStringLiteral("x-try"));
        c.markSuccess(HostPort::parse(QStringLiteral("80.1.1.2:6346")), QStringLiteral("gtk-gnutella/1.2"));
        c.addStatic(HostPort::parse(QStringLiteral("80.1.1.3:7000")), QStringLiteral("my node"));
        c.markFailure(HostPort::parse(QStringLiteral("80.1.1.1:6346")), 0);
        c.addGwc(QStringLiteral("http://example.org/gwc.php"));
        QVERIFY(c.save());
    }
    HostCache c;
    c.setPath(path);
    QVERIFY(c.load());
    QCOMPARE(c.size(), 3);
    const HostEntry *e = c.find(HostPort::parse(QStringLiteral("80.1.1.2:6346")));
    QVERIFY(e);
    QCOMPARE(e->successes, 1);
    QCOMPARE(e->vendor, QStringLiteral("gtk-gnutella/1.2"));
    QVERIFY(c.find(HostPort::parse(QStringLiteral("80.1.1.3:7000")))->isStatic);
    // the failed host is backed off; the good one is picked first
    const QList<HostPort> pick = c.pick(3, {});
    QCOMPARE(pick.size(), 2);
    QCOMPARE(pick[0].toString(), QStringLiteral("80.1.1.2:6346"));
    QCOMPARE(c.gwcs().size(), 1);
}

void TstGnutellaUnit::zlibStreaming()
{
    Deflater d;
    Inflater i;
    QByteArray all, out;
    for (int k = 0; k < 100; ++k) {
        const QByteArray chunk = QByteArray::number(k).repeated(50 + k);
        all += chunk;
        const QByteArray z = d.compress(chunk);
        QByteArray part;
        QVERIFY(i.feed(z, &part, 1 << 20));
        QCOMPARE(part, chunk);            // sync flush: everything decodable immediately
        out += part;
    }
    QCOMPARE(out, all);
    QByteArray capped;
    Inflater i2;
    QVERIFY(!i2.feed(deflateAll(QByteArray(100000, 'a')), &capped, 1000));   // memory cap
    bool ok = true;
    inflateAll(QByteArray("garbage!"), 1000, &ok);
    QVERIFY(!ok);
}

void TstGnutellaUnit::fuzzParsers()
{
    QRandomGenerator r(1234);
    // valid samples to mutate
    QList<HitResult> rs;
    HitResult hr;
    hr.index = 1; hr.size = 5; hr.name = QStringLiteral("a.mp3");
    hr.hashes.sha1 = QByteArray(20, 'S'); hr.hashes.tth = QByteArray(24, 'T');
    hr.altLocs = {HostPort::parse(QStringLiteral("1.2.3.4:5"))};
    rs << hr << hr;
    HitTrailer t; t.serventGuid = randomGuid(); t.pushProxies = {HostPort::parse(QStringLiteral("1.1.1.1:1"))}; t.firewalled = true;
    QueryMsg qm; qm.flags = 0xE000; qm.text = QStringLiteral("abc"); qm.sha1s << QByteArray(20, 'x'); qm.extendedText = QStringLiteral("long");
    GgepBlock gb; gb.add("IPP", QByteArray(12, 'x')); gb.add("Z", QByteArray(300, 'z'), true);
    QrpTable qt(1024); qt.addWord(QStringLiteral("x"));
    mx::hash::TigerTree tt; tt.update(QByteArray(5000, 'a')); tt.finish();
    const QList<QByteArray> samples = {
        buildQueryHitPayload(HostPort::parse(QStringLiteral("1.2.3.4:6346")), 1, rs, t),
        buildQueryPayload(qm),
        buildPongPayload(HostPort::parse(QStringLiteral("1.2.3.4:6346")), 1, 2, gb),
        encodeGgep(gb),
        buildVendorPayload("LIME", 22, 2, QByteArray(6, 'a')),
        buildPushPayload(PushMsg{randomGuid(), 1, 2, 3}),
        QByteArray("HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 0-1/2\r\nX-Queue: position=1, pollMin=2\r\n\r\n"),
        buildThex(5000, {tt.level(0), tt.level(1), tt.level(2), tt.level(3)}),
        qt.encode(7, 4, true, 300).value(1),
    };
    int iterations = 0;
    for (int round = 0; round < 4000; ++round) {
        QByteArray d = samples[int(r.bounded(int(samples.size())))];
        const int mode = int(r.bounded(4));
        if (mode == 0 && !d.isEmpty()) d.truncate(int(r.bounded(int(d.size()))));
        else if (mode == 1) for (int k = 0; k < 1 + int(r.bounded(8)); ++k) { if (!d.isEmpty()) d[int(r.bounded(int(d.size())))] = char(r.bounded(256)); }
        else if (mode == 2) { d = QByteArray(int(r.bounded(600)), Qt::Uninitialized); for (char &c : d) c = char(r.bounded(256)); }
        else d.append(QByteArray(int(r.bounded(50)), char(r.bounded(256))));
        QueryHit h; QueryMsg q; Pong p; VendorMsg v; PushMsg pm; GgepBlock g; HeaderBlock hb; ThexTree th; QrpReceiver rc;
        parseQueryHit(d, &h);
        parseQuery(d, &q);
        parsePong(d, &p);
        parseVendor(d, &v);
        parseMessagesSupported(d);
        parsePush(d, &pm);
        int pos = 0;
        if (!d.isEmpty()) { QByteArray gg = d; gg[0] = char(GgepMagic); parseGgep(gg, &pos, &g); }
        parseHeaderBlock(d, &hb);
        HeaderReader hr2; hr2.feed(d);
        parseThex(d, 5000, QByteArray(), &th);
        QByteArray rq = qt.encode().first();
        rc.handle(rq);
        rc.handle(d);
        Framer f; f.feed(d); Message m; while (f.next(&m)) {}
        bool ok;
        inflateAll(d, 100000, &ok);
        cobsDecode(d, &ok);
        const QString s = QString::fromLatin1(d);
        QList<Span> spans; qint64 a, b, c;
        parseRangeHeader(s, 1000, &spans);
        parseContentRange(s, &a, &b, &c);
        parseAvailableRanges(s, 1000);
        QueueInfo qi; parseXQueue(s, &qi); parseXQueued(s, &qi);
        parseAltLocs(s); parsePushAltLocs(s); parseRetryAfter(s); parseHostList(s);
        mx::FileHashes fh; parseUrn(s, &fh); parseGgepH(d, &fh);
        SourceDesc sd; SourceDesc::fromUri(s, &sd);
        ++iterations;
    }
    QCOMPARE(iterations, 4000);
}


// [core-5] Gnutella speed fields map to the modern connection list at the plugin boundary (no
// "56K" / "T1" / "T3 or Higher" any more); what we send maps back to our own type
void TstGnutellaUnit::modernSpeedLabels()
{
    const QStringList modern = mx::connectionTypes();
    for (quint32 kbps : {0u, 14u, 28u, 56u, 64u, 128u, 768u, 1000u, 1544u, 6000u, 20000u, 44736u, 50000u,
                         100000u, 250000u, 500000u, 1000000u, 2500000u, 5000000u, 10000000u, 0xFFFFFFFFu})
        QVERIFY2(modern.contains(mx::gnut::speedLabel(kbps)), qPrintable(mx::gnut::speedLabel(kbps)));
    QCOMPARE(mx::gnut::speedLabel(0), QStringLiteral("Unknown"));
    QCOMPARE(mx::gnut::speedLabel(56), QStringLiteral("DSL"));       // was "56K"
    QCOMPARE(mx::gnut::speedLabel(1544), QStringLiteral("DSL"));     // was "Cable"
    QCOMPARE(mx::gnut::speedLabel(44736), QStringLiteral("VDSL"));   // was "T1"
    QCOMPARE(mx::gnut::speedLabel(1000000), QStringLiteral("FTTH")); // was "T3 or Higher"
    // monotonic: a faster line never gets a slower label
    int last = -1;
    for (quint32 kbps = 1; kbps < 20000000u; kbps = kbps * 3 / 2 + 1) {
        const int rank = mx::connectionSpeedRank(mx::gnut::speedLabel(kbps));
        QVERIFY2(rank >= last, qPrintable(QString::number(kbps)));
        last = rank;
    }
    for (const char *t : {"DSL", "VDSL", "FTTC", "Cable", "FTTB", "FTTH", "2.5 Gbit", "5 Gbit", "10 Gbit+"})
        QCOMPARE(mx::gnut::speedLabel(mx::gnut::speedForConnection(QString::fromLatin1(t))), QString::fromLatin1(t));
    QVERIFY(modern.contains(mx::gnut::speedLabel(mx::gnut::speedForConnection(QStringLiteral("4G")))));
    QCOMPARE(mx::gnut::speedForConnection(QStringLiteral("T1")), mx::gnut::speedForConnection(QStringLiteral("DSL")));  // old label migrated
}

QTEST_GUILESS_MAIN(TstGnutellaUnit)
#include "tst_gnutella_unit.moc"
