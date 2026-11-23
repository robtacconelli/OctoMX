// SPDX-License-Identifier: GPL-3.0-or-later
// Kad (eMule Kademlia 2) tests: IDs, wire codecs, obfuscation vectors, routing rules, nodes.dat,
// keywords, index, lookups on a simulated 200-node network, and real plugin instances on loopback.
#include "ed2k_core.h"
#include "ed2k_opcodes.h"
#include "ed2k_peer.h"
#include "ed2k_plugin_impl.h"
#include "ed2k_search.h"
#include "ed2k_source.h"
#include "ed2k_upload.h"
#include "kad/kad_index.h"
#include "kad/kad_node.h"
#include "kad/kad_plugin.h"
#include "kad/kad_routing.h"
#include "kad/kad_search.h"
#include "kad/kad_wire.h"
#include "test_support.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QElapsedTimer>
#include <QtCore/QRandomGenerator>
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>
#include <algorithm>
#include <map>

using namespace mx::ed2k;
using namespace mx::ed2k::kad;

class TstKad : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void kadId();
    void obfuscationVectors();
    void obfuscationRoundTrip();
    void framing();
    void tags();
    void messages();
    void keywords_data();
    void keywords();
    void searchPlan();
    void routingRules();
    void routingSplitAndAging();
    void nodesDat();
    void index();
    void fuzzParsers();
    void simulatedNetwork();
    void loopbackPlugins_data();
    void loopbackPlugins();
    void udpReask();
    void partialFileSharing();
};

void TstKad::initTestCase()
{
    mx::registerMetaTypes();
}

static QByteArray hx(const char *s) { return QByteArray::fromHex(s); }

void TstKad::kadId()
{
    // MD4("ubuntu") = a5d7852a264d406b2d682a4aa88fb6d1 (pure-Python MD4); Kad reads it big-endian
    const KadId k = kadKeywordHash(QStringLiteral("ubuntu"));
    QCOMPARE(k.toHex(), QStringLiteral("A5D7852A264D406B2D682A4AA88FB6D1"));
    QCOMPARE(k.toWire(), hx("2a85d7a56b404d264a2a682dd1b68fa8"));
    QCOMPARE(KadId::fromWire(k.toWire()), k);
    QCOMPARE(KadId::fromHash(k.toHash()), k);
    QCOMPARE(k.toHash(), hx("a5d7852a264d406b2d682a4aa88fb6d1"));
    const KadId a(0x80000000u, 0, 0, 1), b(0, 0, 0, 3);
    QCOMPARE(a.bit(0), 1);
    QCOMPARE(a.bit(1), 0);
    QCOMPARE(a.bit(127), 1);
    QCOMPARE((a ^ b), KadId(0x80000000u, 0, 0, 2));
    QVERIFY(b < a);
    QCOMPARE(KadId(0, 0, 0, 1).shiftedLeft(127), KadId(0x80000000u, 0, 0, 0));
    QCOMPARE(KadId(0, 0, 0, 0xFFFFFFFFu).plus(1), KadId(0, 0, 1, 0));
    QCOMPARE(KadId(0, 0x00010000, 0, 0).leadingZeros(), 47);
    QCOMPARE(KadId().leadingZeros(), 128);
    const KadId p = KadId::randomWithPrefix(KadId(0xF0000000u, 0, 0, 0), 4);
    QCOMPARE(p.chunk(0) >> 28, 0xFu);
    QCOMPARE((KadId::allOnes() ^ KadId::allOnes()), KadId());
}

void TstKad::obfuscationVectors()
{
    // vectors from an independent Python implementation of eMule's EncryptedDatagramSocket
    QCOMPARE(udpVerifyKey(0x12345678, ipFromString(QStringLiteral("1.2.3.4"))), 0xf2e6c8b0u);
    ObfuscationParams prm;
    prm.fixed = true;
    prm.randomKeyPart = 0x1234;
    prm.marker = 0x5C;
    const KadId tid = KadId::fromWire(hx("000102030405060708090a0b0c0d0e0f"));
    QCOMPARE(obfuscateKad(hx("e460"), &tid, 0, 0x11223344, prm).toHex(), QByteArray("5c34128b6debd68df443b64836e2c0c88992"));
    prm.randomKeyPart = 0xBEEF;
    prm.marker = 0x40;
    QCOMPARE(obfuscateKad(hx("e4613412"), nullptr, 0xA1B2C3D4, 0x01020304, prm).toHex(),
             QByteArray("42efbe3759f2a7ac98d1594745d75076ea434b72"));
    prm.randomKeyPart = 0x0102;
    prm.marker = 0x80;
    QCOMPARE(obfuscateEd2k(hx("c590a0a1a2a3a4a5a6a7a8a9aaabacadaeaf"), hx("101112131415161718191a1b1c1d1e1f"),
                           ipFromString(QStringLiteral("1.2.3.4")), prm).toHex(),
             QByteArray("8102019f9cc5e7d2273e9ec9af60e115e75717348e7be3d243b4"));
}

void TstKad::obfuscationRoundTrip()
{
    const KadId me = KadId::random();
    const QByteArray uh = generateUserHash();
    const quint32 sender = ipFromString(QStringLiteral("5.6.7.8"));
    const quint32 base = 0x5555AAAA;
    const QByteArray plain = kadFrame(KADEMLIA2_PING, QByteArray());
    // by NodeID
    Deobfuscated d = deobfuscate(obfuscateKad(plain, &me, 0, 77), &me, uh, sender, udpVerifyKey(base, sender));
    QVERIFY(d.obfuscated && d.kad && !d.viaReceiverKey);
    QCOMPARE(d.plain, plain);
    QCOMPARE(d.senderVerifyKey, 77u);
    // by receiver key (the key we gave the sender earlier = our verify key for its IP)
    const quint32 rk = udpVerifyKey(base, sender);
    d = deobfuscate(obfuscateKad(plain, nullptr, rk, 99), &me, uh, sender, rk);
    QVERIFY(d.obfuscated && d.kad && d.viaReceiverKey);
    QCOMPARE(d.receiverVerifyKey, rk);
    QCOMPARE(d.plain, plain);
    // eD2k client datagram keyed by our user hash and the sender's IP
    const QByteArray c5 = QByteArray::fromHex("c590") + QByteArray(16, 'x');
    d = deobfuscate(obfuscateEd2k(c5, uh, sender), &me, uh, sender, rk);
    QVERIFY(d.obfuscated && !d.kad);
    QCOMPARE(d.plain, c5);
    // wrong key / plain / garbage pass through untouched
    const KadId other = KadId::random();
    d = deobfuscate(obfuscateKad(plain, &other, 0, 1), &me, uh, sender, rk);
    QVERIFY(!d.obfuscated);
    d = deobfuscate(plain, &me, uh, sender, rk);
    QVERIFY(!d.obfuscated);
    QCOMPARE(d.plain, plain);
    for (int i = 0; i < 2000; ++i) {
        QByteArray junk(int(QRandomGenerator::global()->bounded(40)), '\0');
        for (auto &c : junk) c = char(QRandomGenerator::global()->bounded(256));
        deobfuscate(junk, &me, uh, sender, rk);
    }
    for (int m = 0; m < 256; ++m) QVERIFY(!isProtocolMarker(quint8(obfuscateKad(plain, &me, 0, 0)[0])));
}

void TstKad::framing()
{
    quint8 op = 0;
    QByteArray pl;
    QVERIFY(kadUnframe(kadFrame(0x21, QByteArray(10, 'a')), &op, &pl));
    QCOMPARE(op, quint8(0x21));
    QCOMPARE(pl, QByteArray(10, 'a'));
    const QByteArray big(3000, 'z');
    const QByteArray packed = kadFrame(0x3B, big);
    QCOMPARE(quint8(packed[0]), PROT_KADPACK);
    QVERIFY(packed.size() < 200);
    QVERIFY(kadUnframe(packed, &op, &pl));
    QCOMPARE(pl, big);
    // zip bomb: 4 MB of zeros packed into a small datagram is refused
    QByteArray bomb;
    bomb.append(char(PROT_KADPACK)).append(char(0x3B)).append(zlibCompress(QByteArray(4 * 1024 * 1024, '\0'), 9));
    QVERIFY(bomb.size() < KAD_MAX_DATAGRAM);
    QVERIFY(!kadUnframe(bomb, &op, &pl));
    QVERIFY(!kadUnframe(QByteArray("\xE4"), &op, &pl));
    QVERIFY(!kadUnframe(QByteArray("\xC5\x01"), &op, &pl));
}

void TstKad::tags()
{
    TagList t;
    t << Tag::makeStr(TAG_FILENAME, QStringLiteral("Größe test.avi")) << Tag::makeInt(TAG_FILESIZE, 200) << Tag::makeInt(TAG_SOURCES, 70000)
      << Tag::makeInt(TAG_MEDIA_LENGTH, 0x1FFFFFFFFull);
    Tag b; b.type = TAGTYPE_BSOB; b.id = TAG_KADAICHHASHPUB; b.blob = QByteArray(20, 'A');
    t << b;
    Writer w;
    writeKadTagList(w, t);
    // known encoding of the first two tags: <type><len 2><name><value>, ints shrunk
    QVERIFY(w.data().startsWith(QByteArray::fromHex("05") + QByteArray::fromHex("020100") + QByteArray("\x01", 1)));
    QVERIFY(w.data().contains(QByteArray::fromHex("09010002c8")));   // UINT8 FILESIZE 200
    Reader r(w.data());
    TagList back;
    QVERIFY(readKadTagList(r, &back));
    QCOMPARE(back.size(), 5);
    QCOMPARE(kadTagStr(back, TAG_FILENAME), QStringLiteral("Größe test.avi"));
    QCOMPARE(kadTagInt(back, TAG_FILESIZE), quint64(200));
    QCOMPARE(kadTagInt(back, TAG_SOURCES), quint64(70000));
    QCOMPARE(kadTagInt(back, TAG_MEDIA_LENGTH), 0x1FFFFFFFFull);
    QCOMPARE(kadTag(back, TAG_KADAICHHASHPUB)->blob, QByteArray(20, 'A'));
    // 64-bit size as an 8-byte BSOB
    Tag s; s.type = TAGTYPE_BSOB; s.id = TAG_FILESIZE;
    Writer w8; w8.u64(5000000000ull); s.blob = w8.take();
    QCOMPARE(kadTagFileSize({s}), 5000000000ull);
}

void TstKad::messages()
{
    const KadId a = KadId::random(), b = KadId::random();
    KadPeer p; p.id = b; p.ip = 0x01020304; p.udpPort = 4672; p.tcpPort = 4662; p.version = 9;
    {
        HelloMsg h; h.id = a; h.tcpPort = 4662; h.version = 9; h.hasMisc = true; h.misc = 5; h.udpPortTag = 5555;
        HelloMsg o; QVERIFY(decodeHello(encodeHello(h), &o));
        QCOMPARE(o.id, a); QCOMPARE(o.tcpPort, quint16(4662)); QCOMPARE(o.version, quint8(9));
        QVERIFY(o.hasMisc); QCOMPARE(o.misc, quint8(5)); QCOMPARE(o.udpPortTag, quint16(5555));
        QVERIFY(!decodeHello(encodeHello(h).left(10), &o));
    }
    {
        BootstrapRes r; r.id = a; r.tcpPort = 1; r.version = 9; r.contacts = {p, p};
        BootstrapRes o; QVERIFY(decodeBootstrapRes(encodeBootstrapRes(r), &o));
        QCOMPARE(o.contacts.size(), 2); QCOMPARE(o.contacts[1].ip, p.ip); QCOMPARE(o.contacts[1].id, b);
        QVERIFY(!decodeBootstrapRes(encodeBootstrapRes(r) + "x", &o));
    }
    {
        KadReq q; q.type = 11; q.target = a; q.receiver = b;
        KadReq o; QVERIFY(decodeKadReq(encodeKadReq(q), &o)); QCOMPARE(o.target, a); QCOMPARE(o.receiver, b); QCOMPARE(o.type, quint8(11));
        QCOMPARE(encodeKadReq(q).size(), 33);
        KadRes r; r.target = a; r.contacts = {p};
        KadRes ro; QVERIFY(decodeKadRes(encodeKadRes(r), &ro)); QCOMPARE(ro.contacts.size(), 1);
        QVERIFY(!decodeKadRes(encodeKadRes(r).left(30), &ro));
    }
    {
        SearchKeyReq q; q.target = a; q.tree = QByteArray::fromHex("0104006d696e74");
        const QByteArray e = encodeSearchKeyReq(q);
        QCOMPARE(quint8(e[17]) & 0x80, 0x80);
        SearchKeyReq o; QVERIFY(decodeSearchKeyReq(e, &o)); QCOMPARE(o.tree, q.tree);
        SearchSourceReq s; s.target = b; s.size = 5000000000ull;
        SearchSourceReq so; QVERIFY(decodeSearchSourceReq(encodeSearchSourceReq(s), &so)); QCOMPARE(so.size, s.size);
    }
    {
        SearchRes r; r.sender = a; r.target = b;
        r.results.append({a, {Tag::makeStr(TAG_FILENAME, QStringLiteral("x.iso")), Tag::makeInt(TAG_FILESIZE, 1234)}});
        SearchRes o; QVERIFY(decodeSearchRes(encodeSearchRes(r), &o));
        QCOMPARE(o.results.size(), 1); QCOMPARE(kadTagStr(o.results[0].tags, TAG_FILENAME), QStringLiteral("x.iso"));
    }
    {
        PublishKeyReq q; q.keyword = a; q.entries.append({b, {Tag::makeStr(TAG_FILENAME, QStringLiteral("n"))}});
        PublishKeyReq o; QVERIFY(decodePublishKeyReq(encodePublishKeyReq(q), &o)); QCOMPARE(o.entries[0].file, b);
        PublishSourceReq s; s.file = a; s.source = b; s.tags = {Tag::makeInt(TAG_SOURCETYPE, 1)};
        PublishSourceReq so; QVERIFY(decodePublishSourceReq(encodePublishSourceReq(s), &so)); QCOMPARE(so.source, b);
        PublishRes r; r.target = a; r.load = 42;
        PublishRes ro; QVERIFY(decodePublishRes(encodePublishRes(r), &ro)); QCOMPARE(ro.load, quint8(42)); QVERIFY(!ro.hasOptions);
    }
    {
        Firewalled2Req f; f.tcpPort = 4662; f.userHash = a; f.connectOptions = 3;
        Firewalled2Req o; QVERIFY(decodeFirewalled2Req(encodeFirewalled2Req(f), &o)); QCOMPARE(o.userHash, a);
        QVERIFY(decodeFirewalled2Req(QByteArray::fromHex("3612"), &o)); QCOMPARE(o.tcpPort, quint16(0x1236));
        FindBuddyReq fb; fb.buddyId = a; fb.userHash = b; fb.tcpPort = 7;
        FindBuddyReq fo; QVERIFY(decodeFindBuddyReq(encodeFindBuddyReq(fb), &fo)); QCOMPARE(fo.tcpPort, quint16(7));
        FindBuddyRes br; br.check = a; br.userHash = b; br.tcpPort = 9; br.hasOptions = true; br.connectOptions = 1;
        FindBuddyRes bo; QVERIFY(decodeFindBuddyRes(encodeFindBuddyRes(br), &bo)); QVERIFY(bo.hasOptions);
        CallbackReq c; c.buddyId = a; c.fileId = b; c.tcpPort = 11;
        CallbackReq co; QVERIFY(decodeCallbackReq(encodeCallbackReq(c), &co)); QCOMPARE(co.fileId, b);
        FirewallUdpMsg m; m.error = 1; m.port = 4672;
        FirewallUdpMsg mo; QVERIFY(decodeFirewallUdp(encodeFirewallUdp(m), &mo)); QCOMPARE(mo.port, quint16(4672));
    }
}

void TstKad::keywords_data()
{
    QTest::addColumn<QString>("in");
    QTest::addColumn<QStringList>("words");
    QTest::newRow("iso") << QStringLiteral("Ubuntu-22.04 Desktop.iso") << QStringList{QStringLiteral("ubuntu"), QStringLiteral("desktop")};
    QTest::newRow("two") << QStringLiteral("linux mint") << QStringList{QStringLiteral("linux"), QStringLiteral("mint")};
    QTest::newRow("short") << QStringLiteral("a.b.c") << QStringList{};
    QTest::newRow("dup") << QStringLiteral("fooo bar fooo") << QStringList{QStringLiteral("bar"), QStringLiteral("fooo")};
    // eMule quirk: a trailing 3-character word counts as extension even when it is a repeated word
    QTest::newRow("dup3") << QStringLiteral("foo bar foo") << QStringList{QStringLiteral("bar")};
    QTest::newRow("ext") << QStringLiteral("song.mp3") << QStringList{QStringLiteral("song")};
    QTest::newRow("single3") << QStringLiteral("mp3") << QStringList{QStringLiteral("mp3")};
    QTest::newRow("utf8") << QStringLiteral("Über äb ab") << QStringList{QStringLiteral("über"), QStringLiteral("äb")};
    QTest::newRow("trailing") << QStringLiteral("abc def.") << QStringList{QStringLiteral("abc"), QStringLiteral("def")};
    QTest::newRow("brackets") << QStringLiteral("[Group] Some_Show (2020) {x265}!?") << QStringList{QStringLiteral("group"), QStringLiteral("some"), QStringLiteral("show"), QStringLiteral("2020"), QStringLiteral("x265")};
}

void TstKad::keywords()
{
    QFETCH(QString, in);
    QFETCH(QStringList, words);
    QCOMPARE(kadGetWords(in), words);
}

void TstKad::searchPlan()
{
    mx::SearchQuery q;
    q.text = QStringLiteral("linux mint");
    KadQueryPlan p = planKadSearch(q);
    QVERIFY(p.error.isEmpty());
    QCOMPARE(p.lookups.size(), 2);
    QCOMPARE(p.lookups[0].keyword, QStringLiteral("linux"));   // longest first
    QCOMPARE(p.lookups[0].target, kadKeywordHash(QStringLiteral("linux")));
    QCOMPARE(p.lookups[0].tree, QByteArray::fromHex("010400") + QByteArray("mint"));   // remaining word only
    QCOMPARE(p.lookups[1].keyword, QStringLiteral("mint"));
    QCOMPARE(p.lookups[1].tree, QByteArray::fromHex("010500") + QByteArray("linux"));
    q.text = QStringLiteral("ubuntu");
    q.type = mx::MediaType::Video;
    q.minSize = 1000;
    p = planKadSearch(q);
    QCOMPARE(p.lookups.size(), 1);
    // AND(type=Video, size > 999)
    QCOMPARE(p.lookups[0].tree.toHex(), QByteArray("0000") + QByteArray("02") + QByteArray("0500") + QByteArray("Video").toHex()
             + QByteArray("010003") + QByteArray("03") + QByteArray("e7030000") + QByteArray("01") + QByteArray("010002"));
    q = mx::SearchQuery();
    q.text = QStringLiteral("velorine OR gooseberry");
    p = planKadSearch(q);
    QCOMPARE(p.lookups.size(), 2);
    q.text = QStringLiteral("ab");
    p = planKadSearch(q);
    QVERIFY(!p.error.isEmpty());
    // local filter
    q = mx::SearchQuery();
    q.text = QStringLiteral("linux -beta");
    q.extension = QStringLiteral("iso");
    p = planKadSearch(q);
    QVERIFY(kadResultMatches(q, p.expr, QStringLiteral("Linux-Mint 22.iso"), 100, QString(), -1));
    QVERIFY(!kadResultMatches(q, p.expr, QStringLiteral("Linux Mint beta.iso"), 100, QString(), -1));
    QVERIFY(!kadResultMatches(q, p.expr, QStringLiteral("Linux Mint.zip"), 100, QString(), -1));
    // [ui-f] the shared type rule: the extension decides, several extensions, bitrate limits
    q = mx::SearchQuery();
    q.text = QStringLiteral("sintel");
    p = planKadSearch(q);
    mx::FileTypeFilter::fromCode(QStringLiteral("video")).applyTo(q);
    QVERIFY(kadResultMatches(q, p.expr, QStringLiteral("Sintel.2010.720p.mkv"), 100, QStringLiteral("Video"), -1));
    QVERIFY(!kadResultMatches(q, p.expr, QStringLiteral("Sintel.exe"), 100, QStringLiteral("Video"), -1));   // a fake "Video"
    QVERIFY(kadResultMatches(q, p.expr, QStringLiteral("Sintel trailer"), 100, QStringLiteral("Video"), -1));  // no extension: the tag
    mx::FileTypeFilter::fromCode(QStringLiteral("software")).applyTo(q);
    QVERIFY(kadResultMatches(q, p.expr, QStringLiteral("Sintel DVD.iso"), 100, QStringLiteral("Pro"), -1));
    QVERIFY(!kadResultMatches(q, p.expr, QStringLiteral("Sintel.zip"), 100, QStringLiteral("Pro"), -1));   // an archive
    mx::FileTypeFilter::fromCode(QStringLiteral("lossless")).applyTo(q);
    QVERIFY(kadResultMatches(q, p.expr, QStringLiteral("Sintel Fan Remix.flac"), 100, QStringLiteral("Audio"), -1));
    QVERIFY(!kadResultMatches(q, p.expr, QStringLiteral("Sintel Fan Remix.mp3"), 100, QStringLiteral("Audio"), -1));
    mx::FileTypeFilter::fromCode(QStringLiteral("audio:max:128")).applyTo(q);
    QVERIFY(kadResultMatches(q, p.expr, QStringLiteral("Sintel Fan Remix.mp3"), 100, QStringLiteral("Audio"), 128));
    QVERIFY(!kadResultMatches(q, p.expr, QStringLiteral("Sintel Fan Remix.mp3"), 100, QStringLiteral("Audio"), 320));
    // index-side tree evaluation agrees
    bool valid = false;
    QVERIFY(KadIndex::treeMatches(QByteArray::fromHex("010400") + "mint", QStringLiteral("linux mint 22"), {}, &valid));
    QVERIFY(valid);
    QVERIFY(!KadIndex::treeMatches(QByteArray::fromHex("010400") + "mint", QStringLiteral("linux 22"), {}, &valid));
}

static KadPeer peerAt(quint32 ip, const KadId &id, quint16 port = 4672)
{
    KadPeer p; p.id = id; p.ip = ip; p.udpPort = port; p.tcpPort = 4662; p.version = 9; return p;
}

void TstKad::routingRules()
{
    const KadId me = KadId::random();
    RoutingTable t(me);
    const qint64 now = 1700000000;
    bool v = true;
    QVERIFY(t.add(peerAt(0x14010101, KadId::random()), 0, 0, v, false, false, now));
    v = true;
    // a second KadID on the same IP is refused (1 contact per IP)
    QVERIFY(!t.add(peerAt(0x14010101, KadId::random(), 5000), 0, 0, v, false, false, now));
    // private and bad addresses are refused without LAN mode
    v = true;
    QVERIFY(!t.add(peerAt(0x0A000001, KadId::random()), 0, 0, v, false, false, now));
    QVERIFY(!t.add(peerAt(0x7F000001, KadId::random()), 0, 0, v, false, false, now));
    QVERIFY(!t.add(peerAt(0x14010102, KadId::random(), 0), 0, 0, v, false, false, now));
    // our own ID and Kad1 contacts are refused
    QVERIFY(!t.add(peerAt(0x14010103, me), 0, 0, v, false, false, now));
    KadPeer k1 = peerAt(0x14010104, KadId::random()); k1.version = 1;
    QVERIFY(!t.add(k1, 0, 0, v, false, false, now));
    // ≤ 2 per /24 in one bin (root bin holds everything for now): the first was 20.1.1.1
    QVERIFY(t.add(peerAt(0x14010105, KadId::random()), 0, 0, v, false, false, now));
    QVERIFY(!t.add(peerAt(0x14010106, KadId::random()), 0, 0, v, false, false, now));
    // ≤ 10 per /24 globally: fill many bins with contacts of 30.0.0.x spread over the ID space
    RoutingTable g(me);
    int added = 0;
    for (int i = 1; i <= 200; ++i) {
        // IDs at increasing distance so they land in different bins
        KadId id = me ^ KadId(0, 0, 0, quint32(i)).shiftedLeft(127 - (i % 120));
        bool vv = true;
        if (g.add(peerAt(0x1E000000u | quint32(i), id), 0, 0, vv, false, false, now)) ++added;
    }
    QVERIFY2(added <= 10, qPrintable(QString::number(added)));
    // LAN mode: subnet rules do not apply, same-IP switch for loopback tests
    RoutingOptions lan;
    lan.allowLan = true;
    lan.allowSameIp = true;
    RoutingTable l(me, lan);
    int lanAdded = 0;
    for (int i = 0; i < 30; ++i) { bool vv = true; if (l.add(peerAt(0x7F000001, KadId::random(), quint16(5000 + i)), 0, 0, vv, false, false, now)) ++lanAdded; }
    QVERIFY(lanAdded >= 10);
    // updates need the sender key we know
    const KadId kid = KadId::random();
    bool vk = true;
    QVERIFY(t.add(peerAt(0x15010101, kid), 1234, 0x01020304, vk, false, true, now, 0x01020304));
    bool vu = false;
    QVERIFY(!t.add(peerAt(0x15010102, kid), 9999, 0x01020304, vu, true, true, now + 5, 0x01020304));   // wrong key: no hijack
    QCOMPARE(t.find(kid)->ip, 0x15010101u);
    QVERIFY(t.add(peerAt(0x15010103, kid), 1234, 0x01020304, vu, true, true, now + 5, 0x01020304));     // right key: IP update
    QCOMPARE(t.find(kid)->ip, 0x15010103u);
    QVERIFY(t.verify(kid, 0x15010103));
    QVERIFY(!t.verify(kid, 0x15010101));
}

void TstKad::routingSplitAndAging()
{
    const KadId me = KadId::random();
    RoutingTable t(me);
    qint64 now = 1700000000;
    int added = 0;
    for (int i = 0; i < 3000; ++i) {
        bool v = true;
        if (t.add(peerAt(0x20000001u + (quint32(i) << 8), KadId::random()), 0, 0, v, false, false, now)) ++added;
    }
    // random IDs: bins split only near our ID (index < KK or level < KBASE), the table stays small
    QVERIFY2(t.count() > 60 && t.count() < 400, qPrintable(QString::number(t.count())));
    QCOMPARE(t.count(), added);
    QVERIFY(t.zoneCount() > 8);
    QVERIFY(t.maxDepth() <= 20);
    // contacts very close to us are always accepted (deep splits around our own ID)
    int close = 0;
    for (int i = 0; i < 20; ++i) {
        KadId id = me ^ KadId(0, 0, 0, quint32(i + 1));
        bool v = true;
        if (t.add(peerAt(0x40000001u + (quint32(i) << 8), id), 0, 0, v, false, false, now)) ++close;
    }
    QVERIFY2(close >= 10, qPrintable(QString::number(close)));
    QVERIFY(t.estimateUsers() > 100);
    // closest() is sorted by distance and returns verified contacts
    const KadId target = KadId::random();
    const QList<KadPeer> c = t.closest(target, 3, 10);
    QCOMPARE(c.size(), 10);
    for (int i = 1; i < c.size(); ++i) QVERIFY((c[i - 1].id ^ target) < (c[i].id ^ target));
    QVERIFY(t.bootstrapContacts(20).size() == 20);
    // aging: a silent contact gets probed by HELLO, becomes type 4 and is dropped
    RoutingTable a(me);
    bool v = true;
    const KadId x = KadId::random();
    a.add(peerAt(0x50000001, x), 0, 0, v, false, false, now);
    QCOMPARE(a.find(x)->type, 3);
    int hellos = 0;
    auto hello = [&](const Contact &) { ++hellos; };
    auto lookup = [&](const KadId &) {};
    for (int s = 0; s < 600; ++s) a.onTimers(now + s, false, hello, lookup);
    QVERIFY(hellos >= 1);
    QCOMPARE(a.count(), 0);
    // an answering contact (update) becomes type 2 and stays
    RoutingTable b(me);
    v = true;
    b.add(peerAt(0x50000002, x), 0, 0, v, false, false, now);
    for (int s = 0; s < 600; ++s) {
        b.onTimers(now + s, false, [&](const Contact &ct) { bool vv = true; b.add(ct.peer(), 0, 0, vv, true, true, now + s); }, lookup);
    }
    QCOMPARE(b.count(), 1);
    QVERIFY(b.find(x)->type <= 2);
}

void TstKad::nodesDat()
{
    QList<NodesDatEntry> in;
    for (int i = 0; i < 25; ++i) {
        NodesDatEntry e;
        e.peer = peerAt(0x60000001u + quint32(i), KadId::random(), quint16(4000 + i));
        e.udpKey = quint32(i * 7 + 1);
        e.udpKeyIp = 0x01020304;
        e.verified = i % 2;
        in << e;
    }
    const QByteArray v2 = writeNodesDat(in);
    QCOMPARE(v2.size(), 12 + 34 * 25);
    NodesDat nd;
    QVERIFY(parseNodesDat(v2, &nd));
    QCOMPARE(nd.version, 2);
    QCOMPARE(nd.entries.size(), 25);
    for (int i = 0; i < 25; ++i) {
        QCOMPARE(nd.entries[i].peer.id, in[i].peer.id);
        QCOMPARE(nd.entries[i].peer.udpPort, in[i].peer.udpPort);
        QCOMPARE(nd.entries[i].udpKey, in[i].udpKey);
        QCOMPARE(nd.entries[i].verified, in[i].verified);
    }
    // v0: <count><id ip udp tcp type>
    Writer w0;
    w0.u32(2);
    for (int i = 0; i < 2; ++i) { writePeer(w0, in[i].peer); }
    QVERIFY(parseNodesDat(w0.data(), &nd));
    QCOMPARE(nd.version, 0);
    QCOMPARE(nd.entries[1].peer.ip, in[1].peer.ip);
    QCOMPARE(nd.entries[1].type, quint8(9));   // the version byte position is the type in v0
    // v1
    Writer w1;
    w1.u32(0).u32(1).u32(3);
    for (int i = 0; i < 3; ++i) writePeer(w1, in[i].peer);
    QVERIFY(parseNodesDat(w1.data(), &nd));
    QCOMPARE(nd.version, 1);
    QCOMPARE(nd.entries[2].peer.version, quint8(9));
    // v3 bootstrap edition
    Writer w3;
    w3.u32(0).u32(3).u32(1).u32(4);
    for (int i = 0; i < 4; ++i) writePeer(w3, in[i].peer);
    QVERIFY(parseNodesDat(w3.data(), &nd));
    QVERIFY(nd.bootstrapOnly);
    QCOMPARE(nd.entries.size(), 4);
    // broken files
    QVERIFY(!parseNodesDat(v2.left(40), &nd));
    QVERIFY(!parseNodesDat(QByteArray(), &nd));
    QVERIFY(!parseNodesDat(w3.data().left(w3.size() - 1), &nd));
    Writer bad; bad.u32(0).u32(7).u32(1);
    QVERIFY(!parseNodesDat(bad.data(), &nd));
}

void TstKad::index()
{
    KadIndex ix;
    const KadId kw = kadKeywordHash(QStringLiteral("ubuntu")), f1 = KadId::random(), f2 = KadId::random();
    const qint64 now = 1700000000;
    quint8 load = 0;
    QVERIFY(ix.addKeyword(kw, f1, 0x01020304, {Tag::makeStr(TAG_FILENAME, QStringLiteral("ubuntu desktop.iso")), Tag::makeInt(TAG_FILESIZE, 1000),
                                               Tag::makeStr(TAG_FILETYPE, QStringLiteral("Pro"))}, now, &load));
    QVERIFY(ix.addKeyword(kw, f2, 0x05060708, {Tag::makeStr(TAG_FILENAME, QStringLiteral("ubuntu server.iso")), Tag::makeInt(TAG_FILESIZE, 2000)}, now, &load));
    QVERIFY(!ix.addKeyword(kw, f2, 0x05060708, {Tag::makeStr(TAG_FILENAME, QStringLiteral("no size"))}, now, &load));
    QCOMPARE(ix.keywordEntries(), 2);
    QList<SearchResEntry> r = ix.keywordResults(kw, QByteArray(), 0, 300, now);
    QCOMPARE(r.size(), 2);
    const quint32 info = quint32(kadTagInt(r[0].tags, TAG_PUBLISHINFO));
    QCOMPARE((info >> 16) & 0xFF, 1u);         // one publisher
    QCOMPARE(info & 0xFFFF, 1000u);             // trust 10.0 (only entry of its /24)
    // restrictive search: "desktop" and size > 500
    Writer tree;
    tree.u8(0).u8(0).u8(1).str16(QStringLiteral("desktop")).u8(3).u32(500).u8(1).u16(1).u8(TAG_FILESIZE);
    r = ix.keywordResults(kw, tree.data(), 0, 300, now);
    QCOMPARE(r.size(), 1);
    QCOMPARE(r[0].answer, f1);
    // sources
    QVERIFY(ix.addSource(f1, KadId::random(), 0x01020304, 4672, {Tag::makeInt(TAG_SOURCETYPE, 1), Tag::makeInt(TAG_SOURCEPORT, 4662)}, now, &load));
    QVERIFY(!ix.addSource(f1, KadId::random(), 0x01020305, 4672, {Tag::makeInt(TAG_SOURCEPORT, 4662)}, now, &load));   // no type
    r = ix.sourceResults(f1, 0, 0, 300, now);
    QCOMPARE(r.size(), 1);
    QCOMPARE(kadTagInt(r[0].tags, TAG_SOURCEIP), quint64(0x01020304));
    QCOMPARE(kadTagInt(r[0].tags, TAG_SOURCEUPORT), quint64(4672));
    // expiry
    ix.clean(now + 25 * 3600);
    QCOMPARE(ix.keywordEntries(), 0);
    QCOMPARE(ix.sourceEntries(), 0);
    // limits: full index still answers (load 100) but stores nothing more
    KadIndexLimits lim;
    lim.maxPerKeyword = 3;
    KadIndex small(lim);
    for (int i = 0; i < 6; ++i)
        small.addKeyword(kw, KadId::random(), quint32(i + 1), {Tag::makeStr(TAG_FILENAME, QStringLiteral("f")), Tag::makeInt(TAG_FILESIZE, 1)}, now, &load);
    QCOMPARE(small.keywordEntries(), 3);
    QCOMPARE(load, quint8(100));
}

void TstKad::fuzzParsers()
{
    QRandomGenerator rng(4242);
    const KadId a = KadId::random();
    KadPeer p = peerAt(0x01020304, a);
    QList<QByteArray> seeds;
    { HelloMsg h; h.id = a; h.version = 9; h.hasMisc = true; seeds << encodeHello(h); }
    { BootstrapRes r; r.id = a; r.contacts = {p, p}; seeds << encodeBootstrapRes(r); }
    { KadRes r; r.target = a; r.contacts = {p}; seeds << encodeKadRes(r); }
    { SearchRes r; r.sender = a; r.target = a; r.results.append({a, {Tag::makeStr(TAG_FILENAME, QStringLiteral("x")), Tag::makeInt(TAG_FILESIZE, 7)}}); seeds << encodeSearchRes(r); }
    { PublishKeyReq q; q.keyword = a; q.entries.append({a, {Tag::makeStr(TAG_FILENAME, QStringLiteral("n"))}}); seeds << encodePublishKeyReq(q); }
    { Writer w; w.u32(0).u32(2).u32(1); writePeer(w, p); w.u32(1).u32(2).u8(1); seeds << w.take(); }
    seeds << QByteArray::fromHex("000001040061626364") << QByteArray::fromHex("0000010300616263030a0000000101000002");
    int ok = 0;
    for (int it = 0; it < 30000; ++it) {
        QByteArray in = seeds[it % seeds.size()];
        const int mode = int(rng.bounded(4));
        if (mode == 0 && !in.isEmpty()) in.truncate(int(rng.bounded(in.size())));
        else if (mode == 1) { for (int k = 0; k < 4 && !in.isEmpty(); ++k) in[int(rng.bounded(in.size()))] = char(rng.bounded(256)); }
        else if (mode == 2) { in.resize(int(rng.bounded(300))); for (auto &c : in) c = char(rng.bounded(256)); }
        else in += QByteArray(int(rng.bounded(20)), char(rng.bounded(256)));
        { HelloMsg x; ok += decodeHello(in, &x); }
        { BootstrapRes x; ok += decodeBootstrapRes(in, &x); }
        { KadReq x; ok += decodeKadReq(in, &x); }
        { KadRes x; ok += decodeKadRes(in, &x); }
        { SearchKeyReq x; ok += decodeSearchKeyReq(in, &x); }
        { SearchSourceReq x; ok += decodeSearchSourceReq(in, &x); }
        { SearchRes x; ok += decodeSearchRes(in, &x); }
        { PublishKeyReq x; ok += decodePublishKeyReq(in, &x); }
        { PublishSourceReq x; ok += decodePublishSourceReq(in, &x); }
        { PublishRes x; ok += decodePublishRes(in, &x); }
        { Firewalled2Req x; ok += decodeFirewalled2Req(in, &x); }
        { FindBuddyReq x; ok += decodeFindBuddyReq(in, &x); }
        { FindBuddyRes x; ok += decodeFindBuddyRes(in, &x); }
        { CallbackReq x; ok += decodeCallbackReq(in, &x); }
        { FirewallUdpMsg x; ok += decodeFirewallUdp(in, &x); }
        { NodesDat x; ok += parseNodesDat(in, &x); }
        { quint8 op; QByteArray pl; ok += kadUnframe(in, &op, &pl); }
        { bool v; KadIndex::treeMatches(in, QStringLiteral("a b c"), {}, &v); }
        { Reader r(in); TagList t; ok += readKadTagList(r, &t); }
    }
    QVERIFY(ok > 0);
}

// ---------------------------------------------------------------- simulated network
namespace {
struct SimNet;
struct SimNode : KadHost {
    SimNet *net = nullptr;
    quint32 ip = 0;          // eD2k order
    quint16 port = 4672;
    QByteArray uh;
    QTemporaryDir dir;
    std::unique_ptr<KadNode> node;
    qint64 kadNowMs() const override;
    qint64 kadNowSecs() const override;
    void kadSendDatagram(quint32 ed2kIp, quint16 p, const QByteArray &bytes) override;
    QByteArray kadUserHash() const override { return uh; }
    quint16 kadTcpPort() const override { return 4662; }
    quint16 kadUdpPort() const override { return port; }
    quint8 kadCryptOptions() const override { return 3; }
};
struct SimNet {
    qint64 nowMs = 0;
    struct Msg { quint32 fromIp; quint16 fromPort; quint32 toIp; quint16 toPort; QByteArray data; };
    QList<Msg> queue;
    QHash<quint64, SimNode *> byAddr;
    quint64 delivered = 0, obfuscated = 0;
    void deliver()
    {
        int guard = 0;
        while (!queue.isEmpty() && ++guard < 2000000) {
            const Msg m = queue.takeFirst();
            SimNode *n = byAddr.value((quint64(m.toIp) << 16) | m.toPort);
            if (!n || !n->node->running() || m.data.size() < 2) continue;
            ++delivered;
            const quint8 b0 = quint8(m.data[0]);
            if (b0 == PROT_KAD || b0 == PROT_KADPACK) { n->node->handleDatagram(m.fromIp, m.fromPort, m.data, false, false, 0); continue; }
            const Deobfuscated d = n->node->deobfuscate(m.fromIp, m.data);
            if (!d.obfuscated || !d.kad) continue;
            ++obfuscated;
            const bool valid = d.receiverVerifyKey && d.receiverVerifyKey == n->node->verifyKeyFor(m.fromIp);
            n->node->handleDatagram(m.fromIp, m.fromPort, d.plain, true, valid, d.senderVerifyKey);
        }
    }
    void run(int seconds, const std::function<bool()> &until = {})
    {
        for (int s = 0; s < seconds; ++s) {
            for (int k = 0; k < 10; ++k) { nowMs += 100; deliver(); }
            for (SimNode *n : std::as_const(byAddr)) n->node->tick();
            deliver();
            if (until && until()) return;
        }
    }
};
qint64 SimNode::kadNowMs() const { return net->nowMs; }
qint64 SimNode::kadNowSecs() const { return 1700000000 + net->nowMs / 1000; }
void SimNode::kadSendDatagram(quint32 ed2kIp, quint16 p, const QByteArray &bytes) { net->queue.append({ip, port, ed2kIp, p, bytes}); }
} // namespace

void TstKad::simulatedNetwork()
{
    SimNet net;
    std::vector<std::unique_ptr<SimNode>> nodes;
    const int N = 200;
    for (int i = 0; i < N; ++i) {
        auto n = std::make_unique<SimNode>();
        n->net = &net;
        n->ip = ipFromString(QStringLiteral("20.%1.%2.1").arg(i / 200).arg(i % 200));   // one /24 per node
        n->uh = md4(QByteArray("user") + QByteArray::number(i));
        // deterministic KadIDs: preferencesKad.dat written before the node loads it
        {
            QFile f(QDir(n->dir.path()).filePath(QStringLiteral("preferencesKad.dat")));
            QVERIFY(f.open(QIODevice::WriteOnly));
            Writer w; w.u32(0).u16(0).bytes(KadId::fromHash(md4(QByteArray("node") + QByteArray::number(i))).toWire()).u8(0);
            f.write(w.data());
        }
        KadConfig cfg;
        cfg.dataDir = n->dir.path();
        cfg.firewallChecks = false;
        cfg.maxPacketsPerSec = 200;
        cfg.searchTolerance = 0xFFFFFFFFu;   // 200 nodes: often nobody shares the target's first 8 bits
        n->node = std::make_unique<KadNode>(n.get(), cfg);
        net.byAddr.insert((quint64(n->ip) << 16) | n->port, n.get());
        nodes.push_back(std::move(n));
    }
    QCOMPARE(nodes[3]->node->id(), KadId::fromHash(md4("node3")));
    for (auto &n : nodes) n->node->start();
    const quint32 seed = kadIpFromEd2k(nodes[0]->ip);
    for (int i = 1; i < N; ++i) nodes[size_t(i)]->node->addBootstrapNode(seed, 4672, true);
    net.run(240);
    int minC = 1 << 30, sum = 0, conn = 0;
    for (auto &n : nodes) { minC = qMin(minC, n->node->contacts()); sum += n->node->contacts(); conn += n->node->connected(); }
    qInfo("simulated: %d nodes, avg %d contacts (min %d), %d connected, %llu datagrams (%llu obfuscated) in 240 virtual s",
          N, sum / N, minC, conn, net.delivered, net.obfuscated);
    QCOMPARE(conn, N);
    QVERIFY2(minC >= 10, qPrintable(QString::number(minC)));
    QVERIFY(sum / N >= 40);
    QVERIFY(net.obfuscated > net.delivered / 2);   // Kad v9 talks obfuscated
    // no two contacts with the same IP in any table
    for (auto &n : nodes) {
        QSet<quint32> ips;
        for (const Contact *c : n->node->routing()->all()) { QVERIFY(!ips.contains(c->ip)); ips.insert(c->ip); }
    }
    // node 7 shares a file: keyword + source publishing
    mx::SharedFile f;
    f.name = QStringLiteral("Simulated Kademlia Testfile.bin");
    f.size = 12345678;
    f.hashes.ed2k = md4("the simulated file");
    nodes[7]->node->setSharedFiles({f});
    net.run(400, [&] { return nodes[7]->node->counters().keywordsPublished >= 3 && nodes[7]->node->counters().filesPublished >= 1; });
    net.run(30);   // let the last store lookup reach all of its nodes
    const KadCounters &c7 = nodes[7]->node->counters();
    qInfo("publish: %llu keyword lookups and %llu file lookups answered (%llu/%llu PUBLISH_RES)", c7.keywordsPublished, c7.filesPublished,
          c7.keywordPublishes, c7.sourcePublishes);
    QVERIFY(c7.keywordsPublished >= 3);
    QVERIFY(c7.filesPublished >= 1);
    // the keyword is stored on nodes close to its hash
    const KadId target = kadKeywordHash(QStringLiteral("kademlia"));
    std::multimap<KadId, int> byDist;
    for (int i = 0; i < N; ++i) byDist.emplace(nodes[size_t(i)]->node->id() ^ target, i);
    int closeStored = 0, k = 0;
    for (auto it = byDist.begin(); it != byDist.end() && k < 10; ++it, ++k)
        if (!nodes[size_t(it->second)]->node->index().keywordResults(target, QByteArray(), 0, 10, net.nowMs / 1000 + 1700000000).isEmpty()) ++closeStored;
    qInfo("keyword \"kademlia\" stored on %d of the 10 closest nodes", closeStored);
    for (const char *w : {"simulated", "kademlia", "testfile"}) {
        const KadId t = kadKeywordHash(QString::fromLatin1(w));
        std::multimap<KadId, int> d;
        for (int i = 0; i < N; ++i) d.emplace(nodes[size_t(i)]->node->id() ^ t, i);
        int rank = 0, inTop = 0, total = 0;
        QStringList ranks;
        for (auto it = d.begin(); it != d.end(); ++it, ++rank) {
            if (nodes[size_t(it->second)]->node->index().keywordEntries() == 0) continue;
            if (nodes[size_t(it->second)]->node->index().keywordResults(t, QByteArray(), 0, 1, net.nowMs / 1000 + 1700000000).isEmpty()) continue;
            ++total;
            if (rank < 10) ++inTop;
            ranks << QString::number(rank);
        }
        qInfo("  %s: stored on %d nodes, %d of them among the 10 closest; distance ranks %s", w, total, inTop, qPrintable(ranks.join(QLatin1Char(','))));
    }
    QVERIFY(closeStored >= 6);
    // keyword search from another node
    QList<mx::SearchHit> hits;
    bool done = false;
    KadNode *searcher = nodes[150]->node.get();
    connect(searcher, &KadNode::searchHits, this, [&](quint32 id, const QList<mx::SearchHit> &h) { if (id == 77) hits << h; });
    connect(searcher, &KadNode::searchDone, this, [&](quint32 id) { if (id == 77) done = true; });
    mx::SearchQuery q;
    q.id = 77;
    q.text = QStringLiteral("kademlia testfile");
    int incoming = 0;   // [engine-3] answered keyword requests are reported for the SPM figure
    for (auto &n : nodes) connect(n->node.get(), &KadNode::incomingSearches, this, [&](int c) { incoming += c; });
    QVERIFY(searcher->search(q));
    net.run(60, [&] { return done; });
    QVERIFY(done);
    QCOMPARE(hits.size(), 1);
    qInfo("incoming keyword searches reported by the network: %d", incoming);
    QVERIFY(incoming >= 1);
    QCOMPARE(hits[0].hashes.ed2k, f.hashes.ed2k);
    QCOMPARE(hits[0].size, f.size);
    QCOMPARE(hits[0].sources[0].net, mx::Net::Kad);
    // source search from a third node
    QList<KadSource> srcs;
    KadNode *dl = nodes[120]->node.get();
    connect(dl, &KadNode::sourcesFound, this, [&](const QString &, const QByteArray &, const QList<KadSource> &s) { srcs << s; });
    dl->findSources(QStringLiteral("dl1"), f.hashes.ed2k, f.size, f.name);
    net.run(60, [&] { return !srcs.isEmpty(); });
    QCOMPARE(srcs.size(), 1);
    QCOMPARE(srcs[0].userHash, nodes[7]->uh);
    QCOMPARE(srcs[0].ip, nodes[7]->ip);
    QCOMPARE(srcs[0].type, quint8(1));
    // hostile datagrams do not hurt
    QRandomGenerator rng(7);
    const quint64 before = nodes[9]->node->counters().droppedMalformed + nodes[9]->node->counters().droppedUnsolicited;
    for (int i = 0; i < 5000; ++i) {
        QByteArray d;
        d.append(char(PROT_KAD)).append(char(rng.bounded(256)));
        QByteArray pl(int(rng.bounded(120)), '\0');
        for (auto &c : pl) c = char(rng.bounded(256));
        nodes[9]->node->handleDatagram(ipFromString(QStringLiteral("33.1.%1.2").arg(i % 250)), 4672, d + pl, false, false, 0);
    }
    net.deliver();
    QVERIFY(nodes[9]->node->counters().droppedMalformed + nodes[9]->node->counters().droppedUnsolicited > before);
    QVERIFY(nodes[9]->node->running());
    // nodes.dat persistence round trip
    QVERIFY(nodes[11]->node->saveNodesDat());
    QFile nf(QDir(nodes[11]->dir.path()).filePath(QStringLiteral("nodes.dat")));
    QVERIFY(nf.open(QIODevice::ReadOnly));
    NodesDat nd;
    QVERIFY(parseNodesDat(nf.readAll(), &nd));
    QCOMPARE(nd.entries.size(), nodes[11]->node->contacts());
}

// ---------------------------------------------------------------- loopback plugin instances
namespace {
struct KadInstance {
    mxtest::MemSettings settings;
    mxtest::FileLibrary lib;
    mxtest::NullLimiter limiter;
    QTemporaryDir dir;
    std::shared_ptr<Ed2kCore> core = makeSharedCore();
    std::unique_ptr<KadPlugin> kad;
    explicit KadInstance(bool firewalled = false, bool udpFirewalled = false)
    {
        settings.setValue(QStringLiteral("ed2k/tcpPort"), 0);
        settings.setValue(QStringLiteral("ed2k/udpPort"), 0);
        settings.setValue(QStringLiteral("ed2k/allowLan"), true);
        settings.setValue(QStringLiteral("ed2k/autoConnect"), false);
        settings.setValue(QStringLiteral("ed2k/autoUpdateServerList"), false);
        settings.setValue(QStringLiteral("ed2k/udpPing"), false);
        settings.setValue(QStringLiteral("kad/allowLan"), true);
        settings.setValue(QStringLiteral("kad/allowSameIp"), true);
        settings.setValue(QStringLiteral("kad/firewallChecks"), false);
        settings.setValue(QStringLiteral("kad/autoBootstrap"), false);
        settings.setValue(QStringLiteral("kad/forceFirewalled"), firewalled);
        settings.setValue(QStringLiteral("kad/forceUdpFirewalled"), udpFirewalled);
        settings.setValue(QStringLiteral("kad/findBuddyDelay"), 5);
        kad = std::make_unique<KadPlugin>(core);
        mx::PluginContext ctx = mxtest::makeContext(&settings, &lib, &limiter, dir.path());
        ctx.dataDir = QDir(dir.path()).filePath(QStringLiteral("kad"));
        kad->init(ctx);
        kad->start();
    }
    ~KadInstance() { kad->stop(); }
    quint16 udp() const { return core->udpLocalPort(); }
};

bool waitUntil(const std::function<bool()> &f, int ms)
{
    QElapsedTimer t; t.start();
    while (!f()) {
        if (t.elapsed() > ms) return false;
        QTest::qWait(100);
    }
    return true;
}

QString makeRandomFile(const QString &path, qint64 size)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return QString();
    QRandomGenerator rng{quint32(size)};
    QByteArray buf(1 << 20, '\0');
    while (f.size() < size) {
        for (auto &c : buf) c = char(rng.bounded(256));
        f.write(buf.left(int(qMin<qint64>(buf.size(), size - f.size()))));
    }
    return path;
}
} // namespace

void TstKad::loopbackPlugins_data()
{
    QTest::addColumn<int>("mode");   // 0 open source, 1 firewalled source (direct UDP callback), 2 firewalled source with buddy
    QTest::newRow("open-source") << 0;
    QTest::newRow("firewalled-direct-callback") << 1;
    QTest::newRow("firewalled-buddy") << 2;
}

void TstKad::loopbackPlugins()
{
    QFETCH(int, mode);
    std::vector<std::unique_ptr<KadInstance>> inst;
    const int N = 6, SHARER = 2, GETTER = 5;
    for (int i = 0; i < N; ++i) inst.push_back(std::make_unique<KadInstance>(i == SHARER && mode >= 1, i == SHARER && mode == 2));
    // mesh-ish bootstrap: everyone knows instance 0, instance 0 knows instance 1
    for (int i = 1; i < N; ++i) inst[size_t(i)]->kad->addServer(QStringLiteral("127.0.0.1:%1").arg(inst[0]->udp()), QString());
    inst[0]->kad->addServer(QStringLiteral("127.0.0.1:%1").arg(inst[1]->udp()), QString());
    QVERIFY(waitUntil([&] {
        for (auto &x : inst) if (x->kad->node()->contacts() < N - 2 || !x->kad->node()->connected()) return false;
        return true;
    }, 60000));
    KadNode *sharer = inst[SHARER]->kad->node();
    if (mode == 1) QVERIFY(sharer->directCallbackPossible());
    if (mode == 2) {
        QVERIFY(waitUntil([&] { return sharer->hasBuddy(); }, 90000));
        qInfo("buddy found");
    }
    // instance SHARER shares a file
    const QString path = makeRandomFile(QDir(inst[SHARER]->dir.path()).filePath(QStringLiteral("OctoMX Kad Loopback Sample.bin")),
                                        2 * 9728000 + 4321);
    const mx::SharedFile sf = inst[SHARER]->lib.addFile(path);
    QVERIFY(sf.hashes.ed2k.size() == 16);
    inst[SHARER]->kad->setSharedFiles(inst[SHARER]->lib.sharedFiles());
    QVERIFY2(waitUntil([&] { return sharer->counters().keywordsPublished >= 2 && sharer->counters().filesPublished >= 1; }, 180000),
             qPrintable(sharer->statusText()));
    // keyword search
    KadPlugin *getter = inst[GETTER]->kad.get();
    QList<mx::SearchHit> hits;
    bool done = false;
    connect(getter, &mx::NetworkPlugin::searchHits, this, [&](quint32 id, const QList<mx::SearchHit> &h) { if (id == 9) hits << h; });
    connect(getter, &mx::NetworkPlugin::searchDone, this, [&](quint32 id) { if (id == 9) done = true; });
    mx::SearchQuery q;
    q.id = 9;
    q.text = QStringLiteral("loopback sample");
    getter->search(q);
    QVERIFY(waitUntil([&] { return done; }, 90000));
    QCOMPARE(hits.size(), 1);
    QCOMPARE(hits[0].hashes.ed2k, sf.hashes.ed2k);
    QCOMPARE(hits[0].size, sf.size);
    // source search → download through the eD2k RangeSource
    QList<mx::SearchSource> sources;
    connect(getter, &mx::NetworkPlugin::sourcesFound, this, [&](const QString &id, const QList<mx::SearchSource> &s) { if (id == QLatin1String("dl")) sources << s; });
    getter->findSources(QStringLiteral("dl"), hits[0].hashes, hits[0].size, hits[0].fileName);
    QVERIFY(waitUntil([&] { return !sources.isEmpty(); }, 90000));
    QCOMPARE(sources[0].net, mx::Net::Kad);
    qInfo("source: %s", qPrintable(sources[0].uri));
    if (mode == 1) QVERIFY(sources[0].uri.contains(QLatin1String("dcb=1")));
    if (mode == 2) QVERIFY(sources[0].uri.contains(QLatin1String("bid=")));
    const QString out = QDir(inst[GETTER]->dir.path()).filePath(QStringLiteral("download.bin"));
    mxtest::SimpleDownloader dl(out, sf.size, sf.hashes.ed2k);
    mx::RangeSource *rs = getter->openSource(QStringLiteral("dl"), hits[0].hashes, hits[0].size, sources[0]);
    QVERIFY(rs);
    connect(getter, &mx::NetworkPlugin::hashSetLearned, this, [&](const QString &, const QString &, qint64, const QList<QByteArray> &h) { dl.setPartHashes(h); });
    connect(inst[GETTER]->core.get(), &Ed2kCore::hashSetLearned, this, [&](const QString &, const QString &, qint64, const QList<QByteArray> &h) { dl.setPartHashes(h); });
    dl.addSource(rs);
    QVERIFY2(waitUntil([&] { return dl.done(); }, 120000), qPrintable(dl.summary() + QLatin1Char(' ') + rs->stateText()));
    QVERIFY(dl.verifiedParts() >= 3);
    QFile a(path), b(out);
    QVERIFY(a.open(QIODevice::ReadOnly) && b.open(QIODevice::ReadOnly));
    QVERIFY(a.readAll() == b.readAll());
    qInfo("mode %d: downloaded %lld bytes via Kad-found source, byte-identical", mode, sf.size);
    rs->close();
    delete rs;   // before the engines go away
}

void TstKad::udpReask()
{
    // two eD2k engines on loopback: B is queued at A (1 slot taken by C) and re-asks over UDP
    // (OP_REASKFILEPING → OP_REASKACK with the queue rank). Kad marks both as reachable.
    KadInstance a, b, c;
    for (KadInstance *x : {&b, &c}) x->kad->addServer(QStringLiteral("127.0.0.1:%1").arg(a.udp()), QString());
    a.kad->addServer(QStringLiteral("127.0.0.1:%1").arg(b.udp()), QString());
    QVERIFY(waitUntil([&] { return a.kad->node()->connected() && b.kad->node()->connected() && c.kad->node()->connected(); }, 30000));
    a.settings.setValue(QStringLiteral("ed2k/uploadSlots"), 1);
    const QString path = makeRandomFile(QDir(a.dir.path()).filePath(QStringLiteral("reask.bin")), 3 * 9728000);
    const mx::SharedFile sf = a.lib.addFile(path);
    a.core->setSharedFiles(a.lib.sharedFiles());
    auto src = [&](KadInstance &x) {
        mx::SearchSource s;
        s.net = mx::Net::Kad;
        s.uri = QStringLiteral("ed2k-peer://127.0.0.1:%1?uid=%2&crypt=3").arg(a.core->tcpPort()).arg(toHex(a.core->userHash()));
        return x.kad->openSource(QStringLiteral("r"), sf.hashes, sf.size, s);
    };
    // C takes the only slot and keeps it (never requests data → slot held until idle timeout)
    mx::RangeSource *rc = src(c);
    rc->open();
    QVERIFY(waitUntil([&] { return rc->state() == mx::RangeSource::Ready; }, 20000));
    // B gets queued
    auto *rb = static_cast<Ed2kRangeSource *>(src(b));
    rb->open();
    QVERIFY(waitUntil([&] { return rb->state() == mx::RangeSource::Queued && rb->queueRank() == 1; }, 20000));
    // the 29-minute timer would fire much later: trigger the UDP re-ask directly
    const int answeredBefore = a.core->uploads()->udpReasksAnswered();
    QCOMPARE(rb->udpReasks(), 0);
    QVERIFY(rb->debugForceReask());
    QVERIFY(waitUntil([&] { return a.core->uploads()->udpReasksAnswered() > answeredBefore; }, 10000));
    QVERIFY(waitUntil([&] { return !rb->udpPending(); }, 10000));
    QCOMPARE(rb->udpReasks(), 1);
    QCOMPARE(rb->queueRank(), 1);
    qInfo("UDP re-ask answered: rank %d", rb->queueRank());
    rb->close(); rc->close();
    delete rb;
    delete rc;
}

namespace {
// A library that exposes one running download: only the verified parts can be read.
class PartialLibrary : public mx::LibraryAccess {
public:
    QString path;
    mx::SharedFile file;
    QSet<int> verified;
    QList<mx::SharedFile> sharedFiles() const override { return {}; }
    bool findByHash(const mx::FileHashes &h, mx::SharedFile *out) const override
    {
        if (h.ed2k != file.hashes.ed2k) return false;
        if (out) *out = file;
        return true;
    }
    QByteArray read(const mx::FileHashes &h, qint64 off, qint64 len) override
    {
        if (h.ed2k != file.hashes.ed2k || off < 0 || len <= 0) return {};
        for (qint64 p = off / 9728000; p <= (off + len - 1) / 9728000; ++p) if (!verified.contains(int(p))) return {};
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly) || !f.seek(off)) return {};
        return f.read(len);
    }
    QList<QByteArray> ed2kPartHashes(const mx::FileHashes &) const override { return {}; }
};
} // namespace

void TstKad::partialFileSharing()
{
    QTemporaryDir dir;
    const QString path = makeRandomFile(dir.filePath(QStringLiteral("partial.bin")), 3 * 9728000 + 1000);
    bool okHash = false;
    const mxtest::Ed2kFileHash eh = mxtest::hashFile(path, &okHash);
    QVERIFY(okHash);
    PartialLibrary plib;
    plib.path = path;
    plib.file.path = path;
    plib.file.name = QStringLiteral("partial.bin");
    plib.file.size = QFileInfo(path).size();
    plib.file.hashes.ed2k = eh.fileHash;
    plib.verified = {0, 2};
    // A: downloading the file (has parts 0 and 2), serves them
    mxtest::MemSettings sa, sb;
    mxtest::NullLimiter la, lb;
    mxtest::FileLibrary libB;
    for (mxtest::MemSettings *s : {&sa, &sb}) {
        s->setValue(QStringLiteral("ed2k/tcpPort"), 0);
        s->setValue(QStringLiteral("ed2k/udpPort"), 0);
        s->setValue(QStringLiteral("ed2k/allowLan"), true);
        s->setValue(QStringLiteral("ed2k/autoConnect"), false);
        s->setValue(QStringLiteral("ed2k/autoUpdateServerList"), false);
        s->setValue(QStringLiteral("ed2k/udpPing"), false);
    }
    Ed2kPlugin a, b;
    a.init(mxtest::makeContext(&sa, &plib, &la, dir.filePath(QStringLiteral("a"))));
    b.init(mxtest::makeContext(&sb, &libB, &lb, dir.filePath(QStringLiteral("b"))));
    a.start();
    b.start();
    a.core()->ensureDownload(QStringLiteral("running"), plib.file.hashes, plib.file.size, plib.file.name);
    a.core()->learnHashset(eh.fileHash, eh.parts);
    QVERIFY(a.core()->isPartialFile(eh.fileHash));
    QCOMPARE(a.core()->availableParts(eh.fileHash, plib.file.size), (QList<bool>{true, false, true, false}));
    QCOMPARE(a.core()->partStatus(eh.fileHash, plib.file.size).toHex(), QByteArray("040005"));   // 4 parts, bits 0 and 2
    // B downloads part 0 from A
    mx::SearchSource s;
    s.net = mx::Net::ED2K;
    s.uri = QStringLiteral("ed2k-peer://127.0.0.1:%1?uid=%2&crypt=3").arg(a.core()->tcpPort()).arg(toHex(a.core()->userHash()));
    mx::RangeSource *rs = b.openSource(QStringLiteral("d"), plib.file.hashes, plib.file.size, s);
    QVERIFY(rs);
    mxtest::SimpleDownloader dl(dir.filePath(QStringLiteral("out.bin")), plib.file.size, eh.fileHash);
    dl.setWanted(0, 9728000);
    connect(&b, &mx::NetworkPlugin::hashSetLearned, this, [&](const QString &, const QString &, qint64, const QList<QByteArray> &h) { dl.setPartHashes(h); });
    dl.addSource(rs);
    QVERIFY2(waitUntil([&] { return dl.done(); }, 60000), qPrintable(dl.summary() + QLatin1Char(' ') + rs->stateText()));
    QCOMPARE(dl.verifiedParts(), 1);
    // the peer saw only the verified parts
    const auto av = rs->available();
    QCOMPARE(av.size(), 2);
    QCOMPARE(av[0], (QPair<qint64, qint64>(0, 9728000)));
    QCOMPARE(av[1], (QPair<qint64, qint64>(2 * 9728000, 3 * 9728000)));
    qInfo("partial file: peer saw parts 0 and 2, part 0 downloaded and verified");
    rs->close();
    delete rs;
    b.stop();
    a.stop();
}

QTEST_GUILESS_MAIN(TstKad)
#include "tst_kad.moc"
