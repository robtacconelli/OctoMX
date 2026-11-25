// SPDX-License-Identifier: GPL-3.0-or-later
// Unit + loopback tests for the eD2k module.
#include "ed2k_core.h"
#include "ed2k_crypto.h"
#include "ed2k_io.h"
#include "ed2k_opcodes.h"
#include "ed2k_packet.h"
#include "ed2k_peer.h"
#include "ed2k_plugin.h"
#include "ed2k_plugin_impl.h"
#include "ed2k_search.h"
#include "ed2k_servermet.h"
#include "ed2k_source.h"
#include "ed2k_tags.h"
#include "ed2k_upload.h"
#include "ed2k_util.h"
#include "test_support.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QRandomGenerator>
#include <QtCore/QTemporaryDir>
#include <QtNetwork/QTcpSocket>
#include <memory>

#include <QtTest/QtTest>
#include <zlib.h>

using namespace mx::ed2k;

static QByteArray hx(const char *s) { return QByteArray::fromHex(QByteArray(s)); }

class TstEd2k : public QObject {
    Q_OBJECT
private slots:
    void tagsRoundTrip_data();
    void tagsRoundTrip();
    void tagsKnownEncoding();
    void packetFraming();
    void packedPacket();
    void parserErrors();
    void serverMet();
    void serverMetGzipAndFilter();
    void searchExpression_data();
    void searchExpression();
    void searchEncoding();
    void searchResults();
    void bigUInt();
    void cryptVectors();
    void cryptHandshake();
    void cryptServerDh();
    void cryptRequire();
    void planBlocks_data();
    void planBlocks();
    void hashset();
    void peerUri();
    void helloRoundTrip();
    void fuzzParsers();
    void loopbackTransfer_data();
    void loopbackTransfer();
    void loopbackQueue();
    void cancelAndStartAgain_data();
    void cancelAndStartAgain();
    void earlyAskBudget();
    void droppedSourceWaits();
    void socketFuzz();
};

// ---------------------------------------------------------------- tags
void TstEd2k::tagsRoundTrip_data()
{
    QTest::addColumn<bool>("newFormat");
    QTest::newRow("old") << false;
    QTest::newRow("new") << true;
}

void TstEd2k::tagsRoundTrip()
{
    QFETCH(bool, newFormat);
    TagList in;
    in << Tag::makeStr(FT_FILENAME, QStringLiteral("Ubuntu 24.04, ü.iso"))
       << Tag::makeStr(FT_FILETYPE, QStringLiteral("Pro"))
       << Tag::makeStr(FT_FILEFORMAT, QString(40, QLatin1Char('x')))
       << Tag::makeInt(FT_SOURCES, 7) << Tag::makeInt(FT_COMPLETE_SOURCES, 300)
       << Tag::makeInt(FT_FILESIZE, 4000000000ull) << Tag::makeInt(FT_MEDIA_LENGTH, 70000)
       << Tag::makeU64(FT_FILESIZE, 0x123456789Aull) << Tag::makeHash(FT_FILEHASH, QByteArray(16, '\x7f'))
       << Tag::makeFloat(0x42, 1.5f) << Tag::makeInt(QByteArray("bitrate"), 192)
       << Tag::makeStr(QByteArray("length"), QStringLiteral("1:02:03"));
    Tag blob; blob.type = TAGTYPE_BLOB; blob.id = 0x30; blob.blob = QByteArray(100, 'b');
    in << blob;
    Writer w;
    writeTagList(w, in, newFormat);
    Reader r(w.data());
    TagList out;
    QVERIFY(readTagList(r, &out));
    QVERIFY(r.atEnd());
    QCOMPARE(out.size(), in.size());
    for (int i = 0; i < in.size(); ++i) {
        QCOMPARE(out[i].id, in[i].id);
        QCOMPARE(out[i].name, in[i].name);
        if (in[i].isStr()) QCOMPARE(out[i].str, in[i].str);
        if (in[i].isInt()) QCOMPARE(out[i].num, in[i].num);
        if (in[i].isHash() || in[i].type == TAGTYPE_BLOB) QCOMPARE(out[i].blob, in[i].blob);
        if (in[i].isFloat()) QCOMPARE(out[i].f, in[i].f);
    }
}

void TstEd2k::tagsKnownEncoding()
{
    // old style: <type><len 2 = 1><id><value>
    Writer w; writeTag(w, Tag::makeInt(CT_VERSION, 0x3C), false);
    QCOMPARE(w.data(), hx("030100113c000000"));
    // new style short string: STR3 (0x13) | 0x80, id 3, "Pro"
    Writer w2; writeTag(w2, Tag::makeStr(FT_FILETYPE, QStringLiteral("Pro")), true);
    QCOMPARE(w2.data(), hx("9303") + QByteArray("Pro"));
    // new style int shrinks to uint8 / uint16
    Writer w3; writeTag(w3, Tag::makeInt(FT_SOURCES, 5), true); writeTag(w3, Tag::makeInt(FT_SOURCES, 300), true);
    QCOMPARE(w3.data(), hx("891505") + hx("88152c01"));
    // string-named old tag
    Writer w4; writeTag(w4, Tag::makeInt(QByteArray("users"), 2), false);
    QCOMPARE(w4.data(), hx("030500") + QByteArray("users") + hx("02000000"));
    // decoding uint16/uint8/bool types normalises to UINT32
    const QByteArray rBytes = hx("880200");   // [mac-1] the Reader keeps a pointer: no temporary
    Reader r(rBytes);   // short-form uint16 with only one value byte -> truncated
    Tag t;
    QVERIFY(!readTag(r, &t));
    const QByteArray r2Bytes = hx("8915" "05");   // [mac-1] the Reader keeps a pointer: no temporary
    Reader r2(r2Bytes);
    QVERIFY(readTag(r2, &t));
    QCOMPARE(t.type, quint8(TAGTYPE_UINT32));
    QCOMPARE(t.num, quint64(5));
    // Latin-1 fallback for invalid UTF-8
    const QByteArray r3Bytes = hx("0201000103004162e9");   // [mac-1] the Reader keeps a pointer: no temporary
    Reader r3(r3Bytes);
    QVERIFY(readTag(r3, &t));
    QCOMPARE(t.str, QStringLiteral("Ab\u00e9"));
}

// ---------------------------------------------------------------- packets
void TstEd2k::packetFraming()
{
    const QByteArray a = encodePacket(PROT_EDONKEY, OP_HELLO, QByteArray("hello"));
    QCOMPARE(a, hx("e30600000001") + QByteArray("hello"));
    const QByteArray b = encodePacket(PROT_EMULE, OP_QUEUERANKING, hx("0700000000000000000000000"));
    PacketParser p;
    const QByteArray stream = a + b + a;
    for (char c : stream) p.feed(&c, 1);   // byte by byte
    Packet x;
    QVERIFY(p.next(&x)); QCOMPARE(x.proto, quint8(PROT_EDONKEY)); QCOMPARE(x.opcode, quint8(OP_HELLO)); QCOMPARE(x.payload, QByteArray("hello"));
    QVERIFY(p.next(&x)); QCOMPARE(x.proto, quint8(PROT_EMULE)); QCOMPARE(x.opcode, quint8(OP_QUEUERANKING));
    QVERIFY(p.next(&x)); QCOMPARE(x.payload, QByteArray("hello"));
    QVERIFY(!p.next(&x));
    QVERIFY(!p.error());
}

void TstEd2k::packedPacket()
{
    QByteArray payload;
    for (int i = 0; i < 2000; ++i) payload += "search result entry ";
    const QByteArray enc = encodePacketMaybePacked(PROT_EDONKEY, OP_SEARCHRESULT, payload);
    QCOMPARE(quint8(enc[0]), quint8(PROT_PACKED));
    QVERIFY(enc.size() < payload.size() / 4);
    PacketParser p;
    p.feed(enc);
    Packet x;
    QVERIFY(p.next(&x));
    QVERIFY(x.wasPacked);
    QCOMPARE(x.opcode, quint8(OP_SEARCHRESULT));
    QCOMPARE(x.payload, payload);
    // small payloads are not packed
    QCOMPARE(quint8(encodePacketMaybePacked(PROT_EMULE, 1, "abc")[0]), quint8(PROT_EMULE));
    // zip bomb guard
    QByteArray zeros(8 * 1024 * 1024, '\0');
    const QByteArray bomb = encodePacket(PROT_PACKED, 0x33, zlibCompress(zeros, 9));
    PacketParser p2(2 * 1024 * 1024, 4 * 1024 * 1024);
    p2.feed(bomb);
    QVERIFY(!p2.next(&x));
    QVERIFY(p2.error());
}

void TstEd2k::parserErrors()
{
    Packet x;
    { PacketParser p; p.feed(hx("17")); QVERIFY(!p.next(&x)); QVERIFY(p.error()); }                 // bad protocol
    { PacketParser p(1000); p.feed(hx("e3ffffff0001")); QVERIFY(!p.next(&x)); QVERIFY(p.error()); } // too long
    { PacketParser p; p.feed(hx("e30000000001")); QVERIFY(!p.next(&x)); QVERIFY(p.error()); }       // length 0
    { PacketParser p; p.feed(hx("d405000000330102")); QVERIFY(!p.next(&x)); }                       // incomplete: waits
    { PacketParser p; p.feed(hx("d40500000033010203ff")); QVERIFY(!p.next(&x)); QVERIFY(p.error()); } // corrupt zlib
}

// ---------------------------------------------------------------- server.met
void TstEd2k::serverMet()
{
    QList<ServerInfo> in;
    ServerInfo a; a.ip = ipFromString(QStringLiteral("176.123.5.89")); a.port = 4725; a.name = QStringLiteral("eMule Sunrise");
    a.description = QStringLiteral("desc"); a.users = 53058; a.files = 23648989; a.obfuscationTcpPort = 4725; a.udpFlags = 0x37B;
    ServerInfo b; b.dynHost = QStringLiteral("server.example.org"); b.port = 4661; b.isStatic = true; b.name = QStringLiteral("dyn");
    in << a << b;
    const QByteArray met = writeServerMet(in);
    QList<ServerInfo> out;
    QString err;
    QVERIFY2(parseServerMet(met, &out, &err), qPrintable(err));
    QCOMPARE(out.size(), 2);
    QCOMPARE(out[0].address(), QStringLiteral("176.123.5.89:4725"));
    QCOMPARE(out[0].name, a.name);
    QCOMPARE(out[0].users, a.users);
    QCOMPARE(out[0].files, a.files);
    QCOMPARE(out[0].obfuscationTcpPort, quint16(4725));
    QCOMPARE(out[0].udpFlags, a.udpFlags);
    QCOMPARE(out[1].address(), QStringLiteral("server.example.org:4661"));
    QVERIFY(out[1].isStatic);
    // hand-made eMule 0x0E file with an old-style ST_SERVERNAME tag and a string-named "users"
    Writer w;
    w.u8(0x0E).u32(1).u32(ipFromString(QStringLiteral("85.17.116.222"))).u16(6082).u32(2);
    w.u8(TAGTYPE_STRING).u16(1).u8(ST_SERVERNAME).raw16("ed2k-rust");
    w.u8(TAGTYPE_UINT32).raw16("users").u32(12315);
    QVERIFY(parseServerMet(w.data(), &out));
    QCOMPARE(out.size(), 1);
    QCOMPARE(out[0].name, QStringLiteral("ed2k-rust"));
    QCOMPARE(out[0].users, 12315u);
    // garbage
    QVERIFY(!parseServerMet(QByteArray("<html>not found</html>"), &out));
    QVERIFY(!parseServerMet(hx("e0ffffff7f"), &out));
}

void TstEd2k::serverMetGzipAndFilter()
{
    ServerInfo good; good.ip = ipFromString(QStringLiteral("91.208.162.87")); good.port = 4232; good.name = QStringLiteral("!! Sharing-Devils No.4 !!");
    ServerInfo fake; fake.ip = ipFromString(QStringLiteral("45.67.231.239")); fake.port = 41237; fake.name = QStringLiteral("Astra-7");
    ServerInfo lan; lan.ip = ipFromString(QStringLiteral("192.168.1.2")); lan.port = 4661;
    const QByteArray met = writeServerMet({good, fake, lan});
    // gzip it
    z_stream zs{};
    QVERIFY(deflateInit2(&zs, 9, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY) == Z_OK);
    QByteArray gz(int(met.size()) + 256, '\0');
    zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(met.constData()));
    zs.avail_in = uInt(met.size());
    zs.next_out = reinterpret_cast<Bytef *>(gz.data());
    zs.avail_out = uInt(gz.size());
    QCOMPARE(deflate(&zs, Z_FINISH), Z_STREAM_END);
    gz.truncate(qsizetype(zs.total_out));
    deflateEnd(&zs);
    QList<ServerInfo> out;
    QVERIFY(parseServerMetDownload(gz, &out));
    QCOMPARE(out.size(), 3);
    QVERIFY(!isKnownBadServer(out[0]));
    QVERIFY(isKnownBadServer(out[1]));
    QVERIFY(isKnownBadServer(out[2]));
    QVERIFY(serverTrustScore(out[0]) > serverTrustScore(out[1]));
}

// ---------------------------------------------------------------- search
void TstEd2k::searchExpression_data()
{
    QTest::addColumn<QString>("in");
    QTest::addColumn<QString>("tree");
    QTest::newRow("words") << "ubuntu desktop iso" << "\"ubuntu desktop iso\"";
    QTest::newRow("or") << "linux OR bsd" << "(\"linux\" OR \"bsd\")";
    QTest::newRow("prec") << "a b OR c NOT d" << "(\"a b\" OR (\"c\" NOT \"d\"))";
    QTest::newRow("minus") << "debian -source" << "(\"debian\" NOT \"source\")";
    QTest::newRow("paren") << "(a OR b) c" << "((\"a\" OR \"b\") AND \"c\")";
    QTest::newRow("phrase") << "\"big buck\" bunny" << "\"big buck bunny\"";
    QTest::newRow("and") << "x AND y" << "(\"x\" AND \"y\")";
    QTest::newRow("err1") << "OR x" << "";
    QTest::newRow("err2") << "(a b" << "";
    QTest::newRow("err3") << "   " << "";
}

void TstEd2k::searchExpression()
{
    QFETCH(QString, in);
    QFETCH(QString, tree);
    QString err;
    SearchNodePtr n = parseSearchExpression(in, &err);
    if (tree.isEmpty()) { QVERIFY(!n); QVERIFY(!err.isEmpty()); return; }
    QVERIFY2(n, qPrintable(err));
    QCOMPARE(searchNodeToString(n), tree);
}

void TstEd2k::searchEncoding()
{
    mx::SearchQuery q;
    q.text = QStringLiteral("ubuntu iso");
    q.type = mx::MediaType::Software;
    q.minSize = 1000;
    const QByteArray enc = encodeSearchRequest(q);
    const QByteArray expect = hx("0000") + hx("010a00") + QByteArray("ubuntu iso")
                            + hx("0000") + hx("020300") + QByteArray("Pro") + hx("010003")
                            + hx("03e7030000" "01" "0100" "02");
    QCOMPARE(enc.toHex(), expect.toHex());
    // boolean tree + extension + 64-bit max size + bitrate
    mx::SearchQuery q2;
    q2.text = QStringLiteral("a OR b");
    q2.extension = QStringLiteral(".ISO");
    q2.maxSize = 5000000000LL;
    q2.minBitrate = 128;
    const QByteArray e2 = encodeSearchRequest(q2);
    const QByteArray x2 = hx("0000") + hx("0001") + hx("010100") + "a" + hx("010100") + "b"
                        + hx("0000") + hx("08" "01f2052a01000000" "02" "0100" "02")
                        + hx("0000") + hx("020300") + "iso" + hx("010004")
                        + hx("037f000000" "01") + hx("0700") + "bitrate";
    QCOMPARE(e2.toHex(), x2.toHex());
    // 32-bit server: large value clamps
    SearchEncodeOptions o; o.largeFiles = false;
    QVERIFY(encodeSearchRequest(q2, o).contains(hx("03ffffffff02")));
    // [ui-f] several extensions ("Lossless audio"): no FT_FILEFORMAT, the type narrows the answer;
    // "Bitrate at best" / "equal to" send an upper bound
    mx::SearchQuery q4;
    q4.text = QStringLiteral("sintel");
    mx::FileTypeFilter::fromCode(QStringLiteral("lossless")).applyTo(q4);
    const QByteArray e4 = encodeSearchRequest(q4);
    QVERIFY(e4.contains(QByteArray("Audio")));
    QVERIFY(!e4.contains(QByteArray("flac")));
    QVERIFY(!e4.contains(hx("010004")));   // no FT_FILEFORMAT term
    mx::SearchQuery q5;
    q5.text = QStringLiteral("sintel");
    mx::FileTypeFilter::fromCode(QStringLiteral("mp3:eq:192")).applyTo(q5);
    const QByteArray e5 = encodeSearchRequest(q5);
    QVERIFY(e5.contains(QByteArray("mp3") + hx("010004")));             // one extension: FT_FILEFORMAT
    QVERIFY(e5.contains(hx("03bf000000" "01") + hx("0700") + "bitrate"));   // > 191
    QVERIFY(e5.contains(hx("03c1000000" "02") + hx("0700") + "bitrate"));   // < 193
    for (const char *code : {"video", "pictures", "documents", "software", "archives", "audio"}) {
        mx::SearchQuery qt;
        qt.text = QStringLiteral("sintel");
        mx::FileTypeFilter::fromCode(QLatin1String(code)).applyTo(qt);
        QVERIFY2(encodeSearchRequest(qt).contains(ed2kTypeForMedia(qt.type).toLatin1()), code);
    }
    QCOMPARE(ed2kTypeForMedia(mx::MediaType::Archive), QStringLiteral("Pro"));   // servers know no "Arc"
    // empty query -> error
    mx::SearchQuery q3;
    QString err;
    QVERIFY(encodeSearchRequest(q3, {}, &err).isEmpty());
}

void TstEd2k::searchResults()
{
    Writer w;
    w.u32(2);
    // entry 1: 64-bit size via FT_FILESIZE + FT_FILESIZE_HI, media length string, sources
    w.hash16(hx("00112233445566778899aabbccddeeff")).u32(0xFBFBFBFB).u16(0xFBFB);
    TagList t1;
    t1 << Tag::makeStr(FT_FILENAME, QStringLiteral("debian-12.iso")) << Tag::makeInt(FT_FILESIZE, 0x00000010)
       << Tag::makeInt(FT_FILESIZE_HI, 1) << Tag::makeStr(FT_FILETYPE, QStringLiteral("Iso"))
       << Tag::makeInt(FT_SOURCES, 42) << Tag::makeInt(FT_COMPLETE_SOURCES, 40)
       << Tag::makeStr(QByteArray("length"), QStringLiteral("1:00:05")) << Tag::makeInt(QByteArray("bitrate"), 256);
    writeTagList(w, t1, true);
    // entry 2: from a HighID client
    w.hash16(hx("ffeeddccbbaa99887766554433221100")).u32(ipFromString(QStringLiteral("80.1.2.3"))).u16(4662);
    TagList t2;
    t2 << Tag::makeStr(FT_FILENAME, QStringLiteral("song.mp3")) << Tag::makeInt(FT_FILESIZE, 4000000)
       << Tag::makeInt(FT_MEDIA_LENGTH, 245) << Tag::makeInt(FT_MEDIA_BITRATE, 320);
    writeTagList(w, t2, false);
    w.u8(1);   // more results available
    QList<SearchResultEntry> res;
    bool more = false;
    QVERIFY(parseSearchResults(w.data(), &res, &more));
    QVERIFY(more);
    QCOMPARE(res.size(), 2);
    QCOMPARE(res[0].size, 0x100000010LL);
    QCOMPARE(res[0].lengthSec, 3605);
    QCOMPARE(res[0].bitrateKbps, 256);
    const mx::SearchHit h0 = toSearchHit(res[0], 9);
    QCOMPARE(h0.queryId, 9u);
    QCOMPARE(h0.hashes.ed2k, hx("00112233445566778899aabbccddeeff"));
    QCOMPARE(h0.sources.size(), 1);
    QCOMPARE(h0.sources[0].peers, 42);
    QCOMPARE(h0.sources[0].seeds, 40);
    QCOMPARE(h0.type, mx::MediaType::Software);   // [ui-f] "Iso": disc images are software
    QCOMPARE(h0.sources[0].uri, QStringLiteral("ed2k://|file|debian-12.iso|4294967312|00112233445566778899AABBCCDDEEFF|/"));
    QVERIFY(!h0.sources[0].user.isValid());
    const mx::SearchHit h1 = toSearchHit(res[1], 9);
    QCOMPARE(h1.durationSec, 245);
    QCOMPARE(h1.bitrateKbps, 320);
    QCOMPARE(h1.type, mx::MediaType::Audio);
    QCOMPARE(h1.sources[0].user.address, QStringLiteral("80.1.2.3:4662"));
    // hostile count
    Writer bad; bad.u32(0x7FFFFFFF);
    QVERIFY(!parseSearchResults(bad.data(), &res, &more));
}

// ---------------------------------------------------------------- crypto
void TstEd2k::bigUInt()
{
    const BigUInt p = BigUInt::fromBigEndian(ed2kDhPrime());
    QCOMPARE(p.bitLength(), 768);
    const BigUInt a = BigUInt::fromBigEndian(hx("80112233445566778899aabbccddeeff"));
    const BigUInt ga = BigUInt::powMod(BigUInt::fromU32(2), a, p);
    // expected value computed with Python's pow(2, a, p)
    QCOMPARE(ga.toBigEndian(96).toHex(),
             QByteArray("4ffc07a2e76360d73e904a67e019d156075abb3f357d95b25bdf724564c6b1ffe1efbaecd6c26962d375f05ddebacf9ba0bbc8a538416ebc1b0535d074c9a9bbffba4426b0d309623ee58f7b1f9d875d945ea72372480964c68ac0ffb8fc0f01"));
    const BigUInt b = BigUInt::fromBigEndian(hx("f0e0d0c0b0a090807060504030201000"));
    const BigUInt gb = BigUInt::powMod(BigUInt::fromU32(2), b, p);
    QCOMPARE(BigUInt::powMod(gb, a, p).toBigEndian(96).toHex(),
             QByteArray("4bb23e780c02b05631ec611dae4c52fe11345dcf2f7e63bd614befc642d4eeed34c6e85388993a0a570e2c10a854b2f29c3679a2687bf92b198e368d841b95acf9d49ad03b3380b29d0551edcd127d603e019f880ebf8270f73fccea4172728e"));
    QCOMPARE(BigUInt::powMod(ga, b, p).toBigEndian(96), BigUInt::powMod(gb, a, p).toBigEndian(96));
}

void TstEd2k::cryptVectors()
{
    // Vectors derived from the reference algorithm (eMule EncryptedStreamSocket): key =
    // MD5(<target hash><34><rand LE>), RC4 with 1024 bytes dropped, magic 0x835E6FC4 LE.
    const QByteArray target = hx("101112131415161718191a1b1c1d1e1f");
    StreamCrypt a;
    a.startClient(target, 0, 0x12345678, true);
    const QByteArray out = a.takeOutput();
    QCOMPARE(out.size(), 1 + 4 + 7);
    const quint8 marker = quint8(out[0]);
    QVERIFY(marker != PROT_EDONKEY && marker != PROT_EMULE && marker != PROT_PACKED);
    QCOMPARE(out.mid(1, 4).toHex(), QByteArray("78563412"));
    QCOMPARE(out.mid(5).toHex(), QByteArray("cc4843e73d0c78"));
    // the answer B would send (magic, method 0, padding 0) under MD5(<hash><203><rand>)
    QCOMPARE(a.state(), StreamCrypt::Negotiating);
    QByteArray app = a.receive(hx("68f8cf970629"));
    QVERIFY(app.isEmpty());
    QCOMPARE(a.state(), StreamCrypt::Encrypting);
    // Rc4 sanity vs. well-known test vector (no drop): key "Key", "Plaintext" -> bbf316e8d940af0ad3
    Rc4 k(QByteArray("Key"), false);
    QCOMPARE(k.process(QByteArray("Plaintext")).toHex(), QByteArray("bbf316e8d940af0ad3"));
}

void TstEd2k::cryptHandshake()
{
    const QByteArray bHash = generateUserHash();
    StreamCrypt a, b;
    a.startClient(bHash, 64);
    b.startIncoming(bHash, false, 64);
    // A sends application data before the handshake is done: it is held back
    QVERIFY(a.send(QByteArray("HELLO-FROM-A")).isEmpty());
    QByteArray toB = a.takeOutput();
    // deliver in odd fragments
    QByteArray atB;
    for (int i = 0; i < toB.size(); i += 3) atB += b.receive(toB.mid(i, 3));
    QVERIFY(atB.isEmpty());
    QCOMPARE(b.state(), StreamCrypt::Encrypting);
    const QByteArray toA = b.takeOutput();
    QVERIFY(a.receive(toA).isEmpty());
    QCOMPARE(a.state(), StreamCrypt::Encrypting);
    const QByteArray held = a.takeOutput();      // released application data, encrypted
    QVERIFY(!held.contains("HELLO"));
    QCOMPARE(b.receive(held), QByteArray("HELLO-FROM-A"));
    // both directions keep working
    const QByteArray big(70000, 'z');
    QCOMPARE(a.receive(b.send(big)), big);
    QCOMPARE(b.receive(a.send(QByteArray("ok"))), QByteArray("ok"));
    // wrong user hash on the B side → magic mismatch
    StreamCrypt a2, b2;
    a2.startClient(generateUserHash(), 0);
    b2.startIncoming(bHash, false);
    b2.receive(a2.takeOutput());
    QVERIFY(b2.failed());
    // plain peer is detected
    StreamCrypt b3;
    b3.startIncoming(bHash, false);
    const QByteArray plain = encodePacket(PROT_EDONKEY, OP_HELLO, "x");
    QCOMPARE(b3.receive(plain), plain);
    QVERIFY(b3.detectedPlain());
}

void TstEd2k::cryptServerDh()
{
    // simulate an eD2k server side of the DH obfuscation
    StreamCrypt c;
    c.startServer();
    const QByteArray req = c.takeOutput();
    QCOMPARE(req.size(), 97);
    const BigUInt p = BigUInt::fromBigEndian(ed2kDhPrime());
    const BigUInt ga = BigUInt::fromBigEndian(req.mid(1, 96));
    const BigUInt bsec = BigUInt::fromBigEndian(hx("c1c2c3c4c5c6c7c8c9cacbcccdcecfd0"));
    const BigUInt gb = BigUInt::powMod(BigUInt::fromU32(2), bsec, p);
    QByteArray s = BigUInt::powMod(ga, bsec, p).toBigEndian(96);
    s.append(char(MAGICVALUE_SERVER));
    Rc4 srvSend(QCryptographicHash::hash(s, QCryptographicHash::Md5));
    s[96] = char(MAGICVALUE_REQUESTER);
    Rc4 srvRecv(QCryptographicHash::hash(s, QCryptographicHash::Md5));
    Writer enc;
    enc.u32(MAGICVALUE_SYNC).u8(0).u8(0).u8(3).bytes("pad");
    QByteArray answer = gb.toBigEndian(96) + srvSend.process(enc.data());
    QVERIFY(c.receive(answer).isEmpty());
    QCOMPARE(c.state(), StreamCrypt::Encrypting);
    const QByteArray reply = srvRecv.process(c.takeOutput());
    Reader r(reply);
    QCOMPARE(r.u32(), MAGICVALUE_SYNC);
    QCOMPARE(r.u8(), quint8(0));
    const quint8 pad = r.u8();
    QVERIFY(r.skip(pad));
    QVERIFY(r.atEnd());
    QCOMPARE(srvRecv.process(c.send("login")), QByteArray("login"));
}

void TstEd2k::cryptRequire()
{
    StreamCrypt b;
    b.startIncoming(generateUserHash(), true);
    b.receive(encodePacket(PROT_EDONKEY, OP_HELLO, "x"));
    QVERIFY(b.failed());
}

// ---------------------------------------------------------------- planner / helpers
void TstEd2k::planBlocks_data()
{
    QTest::addColumn<qint64>("off");
    QTest::addColumn<qint64>("len");
    QTest::addColumn<qint64>("size");
    QTest::addColumn<int>("blocks");
    QTest::newRow("aligned") << qint64(0) << qint64(512000) << qint64(100000000) << 3;
    QTest::newRow("small") << qint64(5) << qint64(10) << qint64(100) << 1;
    QTest::newRow("cross-part") << qint64(PARTSIZE - 1000) << qint64(512000) << qint64(3 * PARTSIZE) << 4;
    QTest::newRow("eof") << qint64(PARTSIZE * 2 - 100) << qint64(512000) << qint64(PARTSIZE * 2) << 1;
    QTest::newRow("big") << qint64(0) << qint64(EMBLOCKSIZE * 5 + 1) << qint64(PARTSIZE) << 6;
}

void TstEd2k::planBlocks()
{
    QFETCH(qint64, off);
    QFETCH(qint64, len);
    QFETCH(qint64, size);
    QFETCH(int, blocks);
    const QList<Range> b = mx::ed2k::planBlocks(off, len, size);
    QCOMPARE(b.size(), blocks);
    qint64 pos = off;
    for (const Range &r : b) {
        QCOMPARE(r.first, pos);
        QVERIFY(r.second > r.first);
        QVERIFY(r.second - r.first <= EMBLOCKSIZE);
        QCOMPARE(r.first / PARTSIZE, (r.second - 1) / PARTSIZE);   // never crosses a part
        pos = r.second;
    }
    QCOMPARE(pos, qMin(off + len, size));
    // the preferred request size tiles parts exactly
    QCOMPARE(PARTSIZE % PREFERRED_REQUEST, qint64(0));
    QCOMPARE(mx::ed2k::planBlocks(0, PREFERRED_REQUEST, PARTSIZE).size(), 3);
}

void TstEd2k::hashset()
{
    // 2 parts + empty-part convention for an exact multiple
    QList<QByteArray> parts = {md4("a"), md4("b"), md4(QByteArray())};
    QByteArray cat; for (auto &p : parts) cat += p;
    const QByteArray fh = md4(cat);
    QVERIFY(verifyHashset(fh, 2 * PARTSIZE, parts));
    QVERIFY(!verifyHashset(fh, 2 * PARTSIZE, parts.mid(0, 2)));
    QVERIFY(!verifyHashset(md4("x"), 2 * PARTSIZE, parts));
    QVERIFY(verifyHashset(md4("tiny"), 4, {md4("tiny")}));
    QCOMPARE(md4(QByteArray()).toHex(), QByteArray("31d6cfe0d16ae931b73c59d7e0c089c0"));
    QCOMPARE(ed2kHashsetCount(PARTSIZE), 2);
    QCOMPARE(ed2kHashsetCount(PARTSIZE - 1), 0);
    QCOMPARE(dataPartCount(PARTSIZE), 1);
    QCOMPARE(dataPartCount(PARTSIZE + 1), 2);
    const QList<Range> r = rangesFromParts({true, false, true, true}, 3 * PARTSIZE + 5);
    QCOMPARE(r.size(), 2);
    QCOMPARE(r[1].first, 2 * PARTSIZE);
    QCOMPARE(r[1].second, 3 * PARTSIZE + 5);
    const QByteArray uh = generateUserHash();
    QCOMPARE(quint8(uh[5]), quint8(14));
    QCOMPARE(quint8(uh[14]), quint8(111));
}

void TstEd2k::peerUri()
{
    PeerDescriptor d;
    d.ip = ipFromString(QStringLiteral("80.1.2.3"));
    d.port = 4662;
    d.userHash = hx("000102030405060708090a0b0c0d0e0f");
    d.cryptOptions = 3;
    d.fileHash = hx("ffeeddccbbaa99887766554433221100");
    const QString u = formatPeerUri(d);
    QCOMPARE(u, QStringLiteral("ed2k-peer://80.1.2.3:4662?file=ffeeddccbbaa99887766554433221100&uid=000102030405060708090a0b0c0d0e0f&crypt=3"));
    PeerDescriptor e;
    QVERIFY(parsePeerUri(u, &e));
    QCOMPARE(e.ip, d.ip); QCOMPARE(e.port, d.port); QCOMPARE(e.userHash, d.userHash); QCOMPARE(e.cryptOptions, d.cryptOptions);
    QVERIFY(parsePeerUri(QStringLiteral("ed2k-peer://0.0.0.0:4662?lowid=1234&server=176.123.5.89:4725"), &e));
    QVERIFY(e.isLowId());
    QCOMPARE(e.lowId, 1234u);
    QCOMPARE(e.serverPort, quint16(4725));
    QVERIFY(!parsePeerUri(QStringLiteral("ed2k-peer://0.0.0.0:4662"), &e));
    QVERIFY(!parsePeerUri(QStringLiteral("ed2k-peer://1.2.3.4:0"), &e));
    QVERIFY(!parsePeerUri(QStringLiteral("http://1.2.3.4:5"), &e));
}

void TstEd2k::helloRoundTrip()
{
    mxtest::MemSettings s;
    QTemporaryDir dir;
    Ed2kCore core;
    core.init(mxtest::makeContext(&s, nullptr, nullptr, dir.path()));
    const QByteArray hello = PeerConnection::buildHelloPayload(&core, false);
    PeerInfo i;
    QVERIFY(PeerConnection::parseHelloPayload(hello, false, &i));
    QCOMPARE(i.userHash, core.userHash());
    QCOMPARE(i.name, QStringLiteral("OctoMX-test"));
    QVERIFY(i.emuleProtocol);
    QCOMPARE(i.compatClient, quint8(0x4D));
    QCOMPARE(i.software(), QStringLiteral("eMule Compat v4.0.0 [OctoMX 4.0]"));
    QVERIFY(i.sourceExchange2);
    QVERIFY(i.largeFiles);
    QVERIFY(i.supportsCrypt);
    QCOMPARE(i.extendedRequestsVersion, quint8(2));
    QCOMPARE(i.dataCompVersion, quint8(1));
    const QByteArray ans = PeerConnection::buildHelloPayload(&core, true);
    QVERIFY(PeerConnection::parseHelloPayload(ans, true, &i));
    QVERIFY(!PeerConnection::parseHelloPayload(ans, false, &i));   // missing hash-size byte
    QVERIFY(!PeerConnection::parseHelloPayload(hello.left(30), false, &i));
    // [set-1] Settings > Other Networks "Default login": the name servers and clients see (empty: the
    // screen name)
    s.setValue(QStringLiteral("other/defaultLogin"), QStringLiteral("BunnyFan"));
    PeerInfo j;
    QVERIFY(PeerConnection::parseHelloPayload(PeerConnection::buildHelloPayload(&core, false), false, &j));
    QCOMPARE(j.name, QStringLiteral("BunnyFan"));
    s.setValue(QStringLiteral("other/defaultLogin"), QString());
    QVERIFY(PeerConnection::parseHelloPayload(PeerConnection::buildHelloPayload(&core, false), false, &j));
    QCOMPARE(j.name, QStringLiteral("OctoMX-test"));
}

// ---------------------------------------------------------------- fuzz
void TstEd2k::fuzzParsers()
{
    QRandomGenerator rng(12345);
    auto rnd = [&](int maxLen) {
        QByteArray b(int(rng.bounded(maxLen + 1)), '\0');
        for (auto &c : b) c = char(rng.bounded(256));
        return b;
    };
    // valid samples to truncate / mutate
    Writer sr; sr.u32(1).hash16(QByteArray(16, 'h')).u32(0).u16(0);
    writeTagList(sr, {Tag::makeStr(FT_FILENAME, QStringLiteral("x.iso")), Tag::makeInt(FT_FILESIZE, 5)}, true);
    ServerInfo si; si.ip = ipFromString(QStringLiteral("1.2.3.4")); si.port = 4661; si.name = QStringLiteral("x");
    const QByteArray samples[] = {sr.data(), writeServerMet({si})};
    int parsed = 0;
    for (int iter = 0; iter < 20000; ++iter) {
        QByteArray in;
        if (iter % 3 == 0) {
            in = samples[iter % 2];
            if (!in.isEmpty()) {
                in.truncate(qsizetype(rng.bounded(int(in.size()) + 1)));
                if (!in.isEmpty() && iter % 2) in[int(rng.bounded(int(in.size())))] = char(rng.bounded(256));
            }
        } else {
            in = rnd(iter % 7 == 0 ? 4000 : 64);
        }
        { Reader r(in); TagList t; readTagList(r, &t); }
        { Reader r(in); Tag t; while (readTag(r, &t)) {} }
        { QList<SearchResultEntry> e; bool more; if (parseSearchResults(in, &e, &more)) ++parsed; }
        { QList<ServerInfo> s; parseServerMet(in, &s); parseServerMetDownload(in, &s); }
        { PeerInfo i; PeerConnection::parseHelloPayload(in, iter & 1, &i); }
        { PacketParser p; p.feed(in); Packet x; while (p.next(&x)) {} }
        { PacketParser p; p.feed(encodePacket(PROT_PACKED, 0x33, in)); Packet x; p.next(&x); }
        { StreamCrypt c; c.startIncoming(QByteArray(16, 'u'), false); c.receive(in); c.receive(in); }
        { StreamCrypt c; c.startClient(QByteArray(16, 'u'), 8); c.takeOutput(); c.receive(in); }
        { PeerDescriptor d; parsePeerUri(QString::fromLatin1(in), &d); }
        { parseSearchExpression(QString::fromLatin1(in)); }
        { QByteArray o; zlibUncompress(in, 1 << 20, &o); }
    }
    QVERIFY(parsed >= 0);
}

// ---------------------------------------------------------------- loopback
namespace {
struct Node {
    mxtest::MemSettings settings;
    mxtest::FileLibrary lib;
    mxtest::NullLimiter limiter;
    QTemporaryDir dir;
    Ed2kPlugin *plugin = nullptr;
    explicit Node(int nSlots = 3, bool obf = true)
    {
        settings.setValue(QStringLiteral("ed2k/tcpPort"), 0);          // any free port
        settings.setValue(QStringLiteral("ed2k/udpPort"), 0);
        settings.setValue(QStringLiteral("ed2k/autoConnect"), false);
        settings.setValue(QStringLiteral("ed2k/autoUpdateServerList"), false);
        settings.setValue(QStringLiteral("ed2k/udpPing"), false);
        settings.setValue(QStringLiteral("ed2k/udpSearch"), false);
        settings.setValue(QStringLiteral("ed2k/udpSources"), false);
        settings.setValue(QStringLiteral("ed2k/allowLan"), true);
        settings.setValue(QStringLiteral("ed2k/uploadSlots"), nSlots);
        settings.setValue(QStringLiteral("ed2k/obfuscation"), obf);
        plugin = new Ed2kPlugin();
        plugin->init(mxtest::makeContext(&settings, &lib, &limiter, dir.path()));
        plugin->start();
    }
    ~Node() { plugin->stop(); delete plugin; }
    quint16 port() const { return plugin->core()->tcpPort(); }
    QString uri(bool withHash) const
    {
        QString u = QStringLiteral("ed2k-peer://127.0.0.1:%1").arg(port());
        if (withHash) u += QStringLiteral("?uid=%1&crypt=3").arg(QString::fromLatin1(plugin->core()->userHash().toHex()));
        return u;
    }
};

QString makeFile(const QString &dir, const QString &name, qint64 size, bool compressible)
{
    const QString path = dir + QLatin1Char('/') + name;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return QString();
    QRandomGenerator rng{quint32(size)};
    QByteArray buf;
    while (f.size() < size) {
        buf.clear();
        if (compressible) {
            for (int i = 0; i < 2000; ++i) buf += QByteArray("line ") + QByteArray::number(int(rng.bounded(100))) + " of the OctoMX eD2k loopback test\n";
        } else {
            buf.resize(1 << 20);
            for (auto &c : buf) c = char(rng.bounded(256));
        }
        f.write(buf.left(int(qMin<qint64>(buf.size(), size - f.size()))));
    }
    return path;
}

bool sameFiles(const QString &a, const QString &b)
{
    QFile x(a), y(b);
    if (!x.open(QIODevice::ReadOnly) || !y.open(QIODevice::ReadOnly) || x.size() != y.size()) return false;
    while (!x.atEnd()) if (x.read(1 << 20) != y.read(1 << 20)) return false;
    return true;
}
} // namespace

void TstEd2k::loopbackTransfer_data()
{
    QTest::addColumn<bool>("obfuscated");
    QTest::addColumn<bool>("compressible");
    QTest::addColumn<qint64>("size");
    QTest::newRow("plain-random-3parts") << false << false << qint64(2 * PARTSIZE + 1234567);
    QTest::newRow("obfuscated-text-1part") << true << true << qint64(3 * 1000 * 1000);
    QTest::newRow("obfuscated-random-exact2parts") << true << false << qint64(2 * PARTSIZE);
}

void TstEd2k::loopbackTransfer()
{
    QFETCH(bool, obfuscated);
    QFETCH(bool, compressible);
    QFETCH(qint64, size);
    Node a, b;
    QVERIFY(a.port() != 0 && b.port() != 0 && a.port() != b.port());
    const QString src = makeFile(a.dir.path(), QStringLiteral("shared.bin"), size, compressible);
    const mx::SharedFile sf = a.lib.addFile(src);
    QVERIFY(sf.hashes.ed2k.size() == 16);
    a.plugin->setSharedFiles(a.lib.sharedFiles());

    QList<QByteArray> learned;
    connect(b.plugin, &mx::NetworkPlugin::hashSetLearned, this,
            [&](const QString &id, const QString &kind, qint64 unit, const QList<QByteArray> &h) {
                QCOMPARE(id, QStringLiteral("dl"));
                QCOMPARE(kind, QStringLiteral("ed2k-parts"));
                QCOMPARE(unit, PARTSIZE);
                learned = h;
            });
    QList<mx::TransferInfo> ups;
    QStringList upDone;
    connect(a.plugin, &mx::NetworkPlugin::uploadChanged, this, [&](const mx::TransferInfo &t) { ups << t; });
    connect(a.plugin, &mx::NetworkPlugin::uploadFinished, this, [&](const QString &id) { upDone << id; });

    mx::SearchSource ss;
    ss.net = mx::Net::ED2K;
    ss.uri = a.uri(obfuscated);
    mx::RangeSource *rs = b.plugin->openSource(QStringLiteral("dl"), sf.hashes, sf.size, ss);
    QVERIFY(rs);
    const QString out = b.dir.path() + QStringLiteral("/download.bin");
    mxtest::SimpleDownloader dl(out, sf.size, sf.hashes.ed2k);
    connect(b.plugin, &mx::NetworkPlugin::hashSetLearned, &dl, [&dl](const QString &, const QString &, qint64, const QList<QByteArray> &h) { dl.setPartHashes(h); });
    QStringList states;
    connect(rs, &mx::RangeSource::stateChanged, this, [&] { if (!states.contains(rs->stateText())) states << rs->stateText(); });
    QElapsedTimer t; t.start();
    dl.addSource(rs);
    QTRY_VERIFY_WITH_TIMEOUT(dl.done(), 60000);
    qInfo() << "loopback" << QTest::currentDataTag() << sf.size << "bytes in" << t.elapsed() << "ms; states:" << states.join(QStringLiteral(" > "));
    QVERIFY(sameFiles(src, out));
    QVERIFY(rs->clientName().contains(QStringLiteral("OctoMX 4.0")));
    QCOMPARE(rs->available().size(), 1);
    QCOMPARE(rs->available().first().second, sf.size);
    if (size >= PARTSIZE) {
        QCOMPARE(learned.size(), ed2kHashsetCount(size));
        QCOMPARE(dl.verifiedParts(), dataPartCount(size));
    } else {
        QCOMPARE(learned, QList<QByteArray>{sf.hashes.ed2k});
    }
    QCOMPARE(rs->descriptor().extra.value(QStringLiteral("obfuscated")).toBool(), obfuscated);
    const UploadManager *um = a.plugin->core()->uploads();
    if (compressible) QVERIFY(um->packedBlocks() > 0);
    else { QVERIFY(um->plainBlocks() > 0); QCOMPARE(um->packedBlocks(), 0); }
    QVERIFY(!ups.isEmpty());
    QCOMPARE(ups.last().peer.id, QString::fromLatin1(b.plugin->core()->userHash().toHex()));
    // closing the source cancels the transfer; the upload session ends
    rs->close();
    delete rs;
    QTRY_VERIFY_WITH_TIMEOUT(!upDone.isEmpty(), 60000);
}

void TstEd2k::cancelAndStartAgain_data()
{
    QTest::addColumn<QString>("code");   // what the engine tells the source when it closes it; empty: the network stops under it
    QTest::addColumn<bool>("sameDownload");
    QTest::newRow("removed, started again") << QStringLiteral("cancelled") << false;   // a new download of the same file
    QTest::newRow("paused, resumed") << QStringLiteral("paused") << true;              // the same download opens its source again
    QTest::newRow("program closed, started again") << QStringLiteral("quit") << true;
    QTest::newRow("network switched off and on") << QString() << true;
}

void TstEd2k::cancelAndStartAgain()
{
    // The user stops a transfer that runs (remove or pause) and starts it again: the source is asked
    // again at once, not after the 29 minutes a queued source waits between two asks.
    QFETCH(QString, code);
    QFETCH(bool, sameDownload);
    Node a, b;
    a.limiter.setLimit(mx::RateLimiter::Up, 3 * 1024 * 1024);   // the first transfer is still running when it is stopped
    const QString src = makeFile(a.dir.path(), QStringLiteral("again.bin"), 2 * PARTSIZE + 4321, false);
    const mx::SharedFile sf = a.lib.addFile(src);
    a.plugin->setSharedFiles(a.lib.sharedFiles());
    mx::SearchSource ss;
    ss.net = mx::Net::ED2K;
    ss.uri = a.uri(true);
    QList<QByteArray> hashes;
    connect(b.plugin, &mx::NetworkPlugin::hashSetLearned, this,
            [&](const QString &, const QString &, qint64, const QList<QByteArray> &h) { hashes = h; });
    {
        mx::RangeSource *rs = b.plugin->openSource(QStringLiteral("first"), sf.hashes, sf.size, ss);
        QVERIFY(rs);
        mxtest::SimpleDownloader dl(b.dir.path() + QStringLiteral("/first.bin"), sf.size, sf.hashes.ed2k);
        dl.addSource(rs);
        QTRY_VERIFY_WITH_TIMEOUT(dl.receivedBytes() > 1024 * 1024, 30000);
        QVERIFY(!dl.done());
        if (code.isEmpty()) {   // the network stops under the running transfer, nobody tells the source why
            b.plugin->stop();
            delete rs;
            b.plugin->start();
        } else {
            rs->setCloseReason(code, QString());
            rs->close();
            delete rs;
            if (code == QLatin1String("quit")) {   // what was noted is read back from the disk
                b.plugin->stop();
                b.plugin->start();
            }
        }
    }
    QTest::qWait(500);
    a.limiter.setLimit(mx::RateLimiter::Up, 0);
    QElapsedTimer t; t.start();
    // (owned here: a failing check below must not leave the source behind its plugin)
    const std::unique_ptr<mx::RangeSource> owner(b.plugin->openSource(sameDownload ? QStringLiteral("first") : QStringLiteral("second"), sf.hashes, sf.size, ss));
    mx::RangeSource *rs = owner.get();
    QVERIFY(rs);
    QStringList states;
    connect(rs, &mx::RangeSource::stateChanged, this, [&] { if (!states.contains(rs->stateText())) states << rs->stateText(); });
    const QString out = b.dir.path() + QStringLiteral("/second.bin");
    mxtest::SimpleDownloader dl(out, sf.size, sf.hashes.ed2k);
    if (!hashes.isEmpty()) dl.setPartHashes(hashes);
    connect(b.plugin, &mx::NetworkPlugin::hashSetLearned, &dl, [&dl](const QString &, const QString &, qint64, const QList<QByteArray> &h) { dl.setPartHashes(h); });
    dl.addSource(rs);
    QTRY_VERIFY2_WITH_TIMEOUT(dl.receivedBytes() > 0, qPrintable(QStringLiteral("no data from the source asked again; its states: %1 (now: %2)")
                                                                     .arg(states.join(QStringLiteral(" > ")), rs->stateText())), 15000);
    qInfo() << "started again: first data after" << t.elapsed() << "ms; states:" << states.join(QStringLiteral(" > "));
    QTRY_VERIFY_WITH_TIMEOUT(dl.done(), 60000);
    QVERIFY(sameFiles(src, out));
    rs->close();
}

void TstEd2k::earlyAskBudget()
{
    // An uploader counts requests for the same file that come sooner than ten minutes (eMule bans at
    // the fourth). We ask early only after the user stopped a running transfer, and at most twice.
    Node n;
    Ed2kCore *c = n.plugin->core();
    const QString peer = QStringLiteral("192.0.2.7:4662");
    const QByteArray file(16, '\x21');
    QVERIFY(!c->takeEarlyAsk(peer, file));          // never asked, never stopped
    c->noteAsk(peer, file);
    QVERIFY(!c->takeEarlyAsk(peer, file));          // asked, but nobody stopped it: the 29 minutes apply
    c->noteUserStop(peer, file);
    QVERIFY(c->takeEarlyAsk(peer, file));           // first early ask
    QVERIFY(!c->takeEarlyAsk(peer, file));          // one stop, one early ask
    c->noteAsk(peer, file);
    c->noteUserStop(peer, file);
    QVERIFY(c->takeEarlyAsk(peer, file));           // second
    c->noteAsk(peer, file);
    c->noteUserStop(peer, file);
    QVERIFY(!c->takeEarlyAsk(peer, file));          // a third would be the uploader's third strike: wait
    // another file of the same source has its own count, and the count survives a restart
    const QByteArray other(16, '\x22');
    c->noteAsk(peer, other);
    c->noteUserStop(peer, other);
    n.plugin->stop();
    n.plugin->start();
    QVERIFY(!c->takeEarlyAsk(peer, file));
    QVERIFY(c->takeEarlyAsk(peer, other));
}

void TstEd2k::droppedSourceWaits()
{
    // The other side of cancelAndStartAgain: a source the engine itself closed (slow, replaced, a
    // retry) is not asked again before its time, or an uploader would count us as a nuisance.
    Node a, b;
    a.limiter.setLimit(mx::RateLimiter::Up, 3 * 1024 * 1024);
    const QString src = makeFile(a.dir.path(), QStringLiteral("wait.bin"), 2 * PARTSIZE + 99, false);
    const mx::SharedFile sf = a.lib.addFile(src);
    a.plugin->setSharedFiles(a.lib.sharedFiles());
    mx::SearchSource ss;
    ss.net = mx::Net::ED2K;
    ss.uri = a.uri(true);
    {
        const std::unique_ptr<mx::RangeSource> rs(b.plugin->openSource(QStringLiteral("dl"), sf.hashes, sf.size, ss));
        QVERIFY(rs);
        mxtest::SimpleDownloader dl(b.dir.path() + QStringLiteral("/first.bin"), sf.size, sf.hashes.ed2k);
        dl.addSource(rs.get());
        QTRY_VERIFY_WITH_TIMEOUT(dl.receivedBytes() > 1024 * 1024, 30000);
        rs->setCloseReason(QStringLiteral("dropped"), QString());
        rs->close();
    }
    const std::unique_ptr<mx::RangeSource> rs(b.plugin->openSource(QStringLiteral("dl"), sf.hashes, sf.size, ss));
    QVERIFY(rs);
    mxtest::SimpleDownloader dl(b.dir.path() + QStringLiteral("/second.bin"), sf.size, sf.hashes.ed2k);
    dl.addSource(rs.get());
    QTest::qWait(2500);
    QCOMPARE(rs->state(), mx::RangeSource::Queued);
    QCOMPARE(dl.receivedBytes(), qint64(0));
    rs->close();
}

void TstEd2k::loopbackQueue()
{
    // one upload slot, two downloaders: the second waits in the queue (OP_QUEUERANKING) and gets
    // the slot when the first one's session ends (session limit ≈ one part).
    Node a(1, true), b1, b2;
    a.limiter.setLimit(mx::RateLimiter::Up, 4 * 1024 * 1024);   // so that the sessions overlap
    const QString src = makeFile(a.dir.path(), QStringLiteral("big.bin"), 2 * PARTSIZE + 777, false);
    const mx::SharedFile sf = a.lib.addFile(src);
    a.plugin->setSharedFiles(a.lib.sharedFiles());
    struct Dl { Node *n; mx::RangeSource *rs; mxtest::SimpleDownloader *dl; bool sawQueued = false; int maxRank = 0; };
    QList<Dl> dls;
    for (Node *n : {&b1, &b2}) {
        mx::SearchSource ss;
        ss.uri = a.uri(true);
        mx::RangeSource *rs = n->plugin->openSource(QStringLiteral("q"), sf.hashes, sf.size, ss);
        QVERIFY(rs);
        auto *dl = new mxtest::SimpleDownloader(n->dir.path() + QStringLiteral("/out.bin"), sf.size, sf.hashes.ed2k, this);
        connect(n->plugin, &mx::NetworkPlugin::hashSetLearned, dl, [dl](const QString &, const QString &, qint64, const QList<QByteArray> &h) { dl->setPartHashes(h); });
        dls.append({n, rs, dl});
    }
    for (int i = 0; i < dls.size(); ++i) {
        mx::RangeSource *rs = dls[i].rs;
        connect(rs, &mx::RangeSource::stateChanged, this, [&dls, i, rs] {
            if (rs->state() == mx::RangeSource::Queued) {
                dls[i].sawQueued = true;
                dls[i].maxRank = qMax(dls[i].maxRank, rs->descriptor().extra.value(QStringLiteral("queueRank")).toInt());
            }
        });
    }
    dls[0].dl->addSource(dls[0].rs);
    QTRY_VERIFY_WITH_TIMEOUT(dls[0].rs->state() == mx::RangeSource::Busy || dls[0].rs->state() == mx::RangeSource::Ready, 20000);
    dls[1].dl->addSource(dls[1].rs);
    QTRY_VERIFY_WITH_TIMEOUT(dls[0].dl->done() && dls[1].dl->done(), 120000);
    QVERIFY(dls[1].sawQueued);
    QVERIFY(dls[1].maxRank >= 1);
    QVERIFY(dls[0].sawQueued);   // lost the slot at the session limit, then got it back
    QVERIFY(sameFiles(src, b1.dir.path() + QStringLiteral("/out.bin")));
    QVERIFY(sameFiles(src, b2.dir.path() + QStringLiteral("/out.bin")));
    for (auto &d : dls) { d.rs->close(); delete d.rs; delete d.dl; }
}

void TstEd2k::socketFuzz()
{
    // hostile peer: garbage, then a valid hello followed by random packets. The node must survive
    // and still serve a normal download afterwards.
    Node a;
    const QString src = makeFile(a.dir.path(), QStringLiteral("f.bin"), 700000, true);
    const mx::SharedFile sf = a.lib.addFile(src);
    a.plugin->setSharedFiles(a.lib.sharedFiles());
    QRandomGenerator rng(7);
    for (int round = 0; round < 30; ++round) {
        QTcpSocket s;
        s.connectToHost(QHostAddress::LocalHost, a.port());
        QVERIFY(s.waitForConnected(5000));
        if (round % 3 == 0) {
            QByteArray g(int(rng.bounded(5000)) + 1, '\0');
            for (auto &c : g) c = char(rng.bounded(256));
            s.write(g);
        } else {
            Writer h;
            h.u8(16).hash16(generateUserHash()).u32(0).u16(4662).u32(0).u32(0).u16(0);
            s.write(encodePacket(PROT_EDONKEY, OP_HELLO, h.data()));
            for (int k = 0; k < 40; ++k) {
                QByteArray pl(int(rng.bounded(300)), '\0');
                for (auto &c : pl) c = char(rng.bounded(256));
                if (k % 5 == 0) pl = sf.hashes.ed2k + pl;   // plausible file hash prefix
                const quint8 proto = (k & 1) ? PROT_EMULE : PROT_EDONKEY;
                s.write(encodePacket(proto, quint8(rng.bounded(256)), pl));
            }
        }
        s.flush();
        QTest::qWait(30);
        s.abort();
    }
    QTest::qWait(300);
    Node b;
    mx::SearchSource ss;
    ss.uri = a.uri(false);
    mx::RangeSource *rs = b.plugin->openSource(QStringLiteral("f"), sf.hashes, sf.size, ss);
    mxtest::SimpleDownloader dl(b.dir.path() + QStringLiteral("/f.bin"), sf.size, sf.hashes.ed2k);
    connect(b.plugin, &mx::NetworkPlugin::hashSetLearned, &dl, [&dl](const QString &, const QString &, qint64, const QList<QByteArray> &h) { dl.setPartHashes(h); });
    dl.addSource(rs);
    QTRY_VERIFY_WITH_TIMEOUT(dl.done(), 30000);
    QVERIFY(sameFiles(src, b.dir.path() + QStringLiteral("/f.bin")));
    rs->close();
    delete rs;
}

QTEST_GUILESS_MAIN(TstEd2k)
#include "tst_ed2k.moc"
