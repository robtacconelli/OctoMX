// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for mxhash: published test vectors + structural checks.
#include "mxhash/aich.h"
#include "mxhash/bt_pieces.h"
#include "mxhash/ed2k.h"
#include "mxhash/encoding.h"
#include "mxhash/hasher.h"
#include "mxhash/links.h"
#include "mxhash/multi_hasher.h"
#include "mxhash/tiger_tree.h"

#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>

using namespace mx::hash;

namespace {
QByteArray pattern(qint64 n)
{
    QByteArray b(n, Qt::Uninitialized);
    for (qint64 i = 0; i < n; ++i)
        b[i] = char((i * 31 + (i >> 13) + (i >> 21)) & 0xff);
    return b;
}
QByteArray sha1(const QByteArray &d) { return QCryptographicHash::hash(d, QCryptographicHash::Sha1); }
QByteArray sha1(const QByteArray &a, const QByteArray &b) { return sha1(a + b); }

// Feed in awkward chunk sizes to exercise the streaming paths.
template <class H>
QByteArray feedChunked(H &h, const QByteArray &data, int chunk = 7777)
{
    for (qint64 off = 0; off < data.size(); off += chunk)
        h.update(data.constData() + off, qMin<qint64>(chunk, data.size() - off));
    return h.finish();
}
QByteArray aichRoot(const QByteArray &data)
{
    AichHasher a;
    a.update(data);
    return a.finish();
}
// Independent reference for a block subtree, phrased in block counts (eMule: n = ceil(size/base),
// left = ((isLeft ? n + 1 : n) / 2) units).
QList<QByteArray> blocksOf(const QByteArray &part)
{
    QList<QByteArray> h;
    for (qint64 off = 0; off < part.size(); off += AichHasher::BlockSize)
        h << QCryptographicHash::hash(part.mid(off, AichHasher::BlockSize), QCryptographicHash::Sha1);
    return h;
}
QByteArray refPartTree(const QList<QByteArray> &h, int lo, int n, bool isLeft)
{
    if (n == 1) return h.at(lo);
    const int left = (isLeft ? n + 1 : n) / 2;
    return QCryptographicHash::hash(refPartTree(h, lo, left, true) + refPartTree(h, lo + left, n - left, false),
                                    QCryptographicHash::Sha1);
}
} // namespace

class TestHash : public QObject {
    Q_OBJECT
private slots:
    void md4Rfc1320_data();
    void md4Rfc1320();
    void md4Streaming();
    void tigerNessie_data();
    void tigerNessie();
    void tigerMillionA();
    void thexVectors_data();
    void thexVectors();
    void tigerTreeStructure();
    void tigerTreeFolding();
    void cryptoHashes();
    void ed2kSmall();
    void ed2kBoundaries_data();
    void ed2kBoundaries();
    void aichStructure();
    void aichParts();
    void btPieces();
    void multiHasherFile();
    void base32Rfc4648_data();
    void base32Rfc4648();
    void hexHelpers();
    void magnetParse();
    void magnetMultiXt();
    void magnetInvalid();
    void magnetRoundTrip();
    void ed2kLinkParse();
    void ed2kLinkFormat();
};

// ---------------------------------------------------------------- MD4
void TestHash::md4Rfc1320_data()
{
    QTest::addColumn<QByteArray>("in");
    QTest::addColumn<QString>("hex");
    QTest::newRow("empty") << QByteArray("") << "31d6cfe0d16ae931b73c59d7e0c089c0";
    QTest::newRow("a") << QByteArray("a") << "bde52cb31de33e46245e05fbdbd6fb24";
    QTest::newRow("abc") << QByteArray("abc") << "a448017aaf21d8525fc10ae87aa6729d";
    QTest::newRow("message digest") << QByteArray("message digest") << "d9130a8164549fe818874806e1c7014b";
    QTest::newRow("a-z") << QByteArray("abcdefghijklmnopqrstuvwxyz") << "d79e1c308aa5bbcdeea8ed63df412da9";
    QTest::newRow("A-Za-z0-9")
        << QByteArray("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789")
        << "043f8582f241db351ce627e153e7f0e4";
    QTest::newRow("8x1234567890")
        << QByteArray("12345678901234567890123456789012345678901234567890123456789012345678901234567890")
        << "e33b4ddc9c38f2199c3e7b164fcc0536";
}

void TestHash::md4Rfc1320()
{
    QFETCH(QByteArray, in);
    QFETCH(QString, hex);
    QCOMPARE(toHex(Md4::hash(in)), hex);
}

void TestHash::md4Streaming()
{
    const QByteArray d = pattern(100000);
    for (int chunk : {1, 3, 63, 64, 65, 1000}) {
        Md4 h;
        QCOMPARE(feedChunked(h, d, chunk), Md4::hash(d));
    }
}

// ---------------------------------------------------------------- Tiger
void TestHash::tigerNessie_data()
{
    QTest::addColumn<QByteArray>("in");
    QTest::addColumn<QString>("hex");
    QTest::newRow("empty") << QByteArray("") << "3293AC630C13F0245F92BBB1766E16167A4E58492DDE73F3";
    QTest::newRow("a") << QByteArray("a") << "77BEFBEF2E7EF8AB2EC8F93BF587A7FC613E247F5F247809";
    QTest::newRow("abc") << QByteArray("abc") << "2AAB1484E8C158F2BFB8C5FF41B57A525129131C957B5F93";
    QTest::newRow("Tiger") << QByteArray("Tiger") << "DD00230799F5009FEC6DEBC838BB6A27DF2B9D6F110C7937";
    QTest::newRow("message digest") << QByteArray("message digest")
                                    << "D981F8CB78201A950DCF3048751E441C517FCA1AA55A29F6";
    QTest::newRow("a-z") << QByteArray("abcdefghijklmnopqrstuvwxyz")
                         << "1714A472EEE57D30040412BFCC55032A0B11602FF37BEEE9";
    QTest::newRow("abcdbcde...")
        << QByteArray("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
        << "0F7BF9A19B9C58F2B7610DF7E84F0AC3A71C631E7B53F78E";
    QTest::newRow("A-Za-z0-9")
        << QByteArray("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789")
        << "8DCEA680A17583EE502BA38A3C368651890FFBCCDC49A8CC";
    QTest::newRow("A-Za-z0-9+-")
        << QByteArray("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-")
        << "F71C8583902AFB879EDFE610F82C0D4786A3A534504486B5";
    QTest::newRow("paper title")
        << QByteArray("Tiger - A Fast New Hash Function, by Ross Anderson and Eli Biham")
        << "8A866829040A410C729AD23F5ADA711603B3CDD357E4C15E";
}

void TestHash::tigerNessie()
{
    QFETCH(QByteArray, in);
    QFETCH(QString, hex);
    QCOMPARE(toHex(Tiger::hash(in)).toUpper(), hex);
    Tiger t;
    QCOMPARE(toHex(feedChunked(t, in, 5)).toUpper(), hex);
}

void TestHash::tigerMillionA()
{
    Tiger t;
    const QByteArray chunk(1000, 'a');
    for (int i = 0; i < 1000; ++i)
        t.update(chunk);
    QCOMPARE(toHex(t.finish()).toUpper(), QStringLiteral("6DB0E2729CBEAD93D715C6A7D36302E9B3CEE0D2BC314B41"));
}

// ---------------------------------------------------------------- Tiger tree (THEX)
void TestHash::thexVectors_data()
{
    QTest::addColumn<QByteArray>("in");
    QTest::addColumn<QString>("b32");
    QTest::newRow("empty") << QByteArray() << "LWPNACQDBZRYXW3VHJVCJ64QBZNGHOHHHZWCLNQ";
    QTest::newRow("one zero byte") << QByteArray(1, '\0') << "VK54ZIEEVTWNAUI5D5RDFIL37LX2IQNSTAXFKSA";
    QTest::newRow("1024 A") << QByteArray(1024, 'A') << "L66Q4YVNAFWVS23X2HJIRA5ZJ7WXR3F26RSASFA";
    QTest::newRow("1025 A") << QByteArray(1025, 'A') << "PZMRYHGY6LTBEH63ZWAHDORHSYTLO4LEFUIKHWY";
}

void TestHash::thexVectors()
{
    QFETCH(QByteArray, in);
    QFETCH(QString, b32);
    TigerTree t;
    t.update(in);
    QCOMPARE(toBase32(t.finish()), b32);
    TigerTree t2;
    QCOMPARE(toBase32(feedChunked(t2, in, 100)), b32);
}

void TestHash::tigerTreeStructure()
{
    // 5 leaves (4097..5120 bytes): level heights 0..3; root = H(H(H(l0,l1),H(l2,l3)), l4)
    const QByteArray d = pattern(4500);
    QList<QByteArray> leaves;
    for (int off = 0; off < d.size(); off += 1024)
        leaves << TigerTree::leafHash(d.mid(off, 1024));
    QCOMPARE(leaves.size(), 5);
    const QByteArray expect = TigerTree::nodeHash(
        TigerTree::nodeHash(TigerTree::nodeHash(leaves[0], leaves[1]), TigerTree::nodeHash(leaves[2], leaves[3])),
        leaves[4]);
    TigerTree t;
    t.update(d);
    QCOMPARE(t.finish(), expect);
    QCOMPARE(t.levels(), 4);
    QCOMPARE(t.level(0), QList<QByteArray>{expect});
    QCOMPARE(t.level(3), leaves);
    QCOMPARE(t.levelForUnit(1024), leaves);
    QCOMPARE(t.levelForUnit(2048).size(), 3);
    QCOMPARE(t.levelForUnit(2048).at(2), leaves[4]);   // promoted
    QCOMPARE(t.levelForUnit(1000), QList<QByteArray>());
    QCOMPARE(t.levelForUnit(1 << 20), QList<QByteArray>{expect});
    QCOMPARE(TigerTree::rootFromLevel(leaves), expect);
}

void TestHash::tigerTreeFolding()
{
    // A tree that keeps at most 4 nodes must give the same root and the same coarse levels.
    for (int n : {0, 1, 1024, 3000, 7 * 1024, 8 * 1024, 9 * 1024 + 5, 33 * 1024, 100000}) {
        const QByteArray d = pattern(n);
        TigerTree full, small(4);
        full.update(d);
        small.update(d);
        QCOMPARE(small.finish(), full.finish());
        QCOMPARE(small.levels(), full.levels());
        QVERIFY(small.storedHeight() >= 0);
        for (int lv = 0; lv < full.levels(); ++lv) {
            const QList<QByteArray> s = small.level(lv);
            if (!s.isEmpty()) QCOMPARE(s, full.level(lv));
        }
        QCOMPARE(small.level(0), full.level(0));
    }
}

// ---------------------------------------------------------------- SHA-1 / SHA-256 / MD5
void TestHash::cryptoHashes()
{
    CryptoHasher s1(QCryptographicHash::Sha1), s256(QCryptographicHash::Sha256), m5(QCryptographicHash::Md5);
    s1.update(QByteArray("abc"));
    s256.update(QByteArray("abc"));
    m5.update(QByteArray("abc"));
    QCOMPARE(toHex(s1.finish()), QStringLiteral("a9993e364706816aba3e25717850c26c9cd0d89d"));
    QCOMPARE(toHex(s256.finish()), QStringLiteral("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    QCOMPARE(toHex(m5.finish()), QStringLiteral("900150983cd24fb0d6963f7d28e17f72"));
}

// ---------------------------------------------------------------- eD2k
void TestHash::ed2kSmall()
{
    Ed2kHasher e;
    QCOMPARE(toHex(e.finish()), QStringLiteral("31d6cfe0d16ae931b73c59d7e0c089c0"));   // MD4("")
    QCOMPARE(e.partHashes().size(), 1);
    e.reset();
    e.update(QByteArray("abc"));
    QCOMPARE(toHex(e.finish()), QStringLiteral("a448017aaf21d8525fc10ae87aa6729d"));   // = MD4 for 1 part
    QCOMPARE(Ed2kHasher::partCount(0), 1);
    QCOMPARE(Ed2kHasher::partCount(9727999), 1);
    QCOMPARE(Ed2kHasher::partCount(9728000), 2);
    QCOMPARE(Ed2kHasher::partCount(9728001), 2);
    QCOMPARE(Ed2kHasher::partCount(2 * 9728000), 3);
    QCOMPARE(Ed2kHasher::dataPartCount(9728000), 1);
    QCOMPARE(Ed2kHasher::dataPartCount(9728001), 2);
}

void TestHash::ed2kBoundaries_data()
{
    QTest::addColumn<qint64>("size");
    QTest::newRow("PART-1") << qint64(9727999);
    QTest::newRow("PART") << qint64(9728000);
    QTest::newRow("PART+1") << qint64(9728001);
    QTest::newRow("2*PART") << qint64(2 * 9728000);
    QTest::newRow("2*PART+12345") << qint64(2 * 9728000 + 12345);
}

void TestHash::ed2kBoundaries()
{
    QFETCH(qint64, size);
    const qint64 P = Ed2kHasher::PartSize;
    const QByteArray d = pattern(size);
    // Expected per the eMule algorithm, composed by hand from MD4 of the slices.
    QList<QByteArray> parts;
    for (qint64 off = 0; off < size; off += P)
        parts << Md4::hash(d.mid(off, qMin(P, size - off)));
    if (size % P == 0) parts << Md4::hash(QByteArray());       // eMule's trailing empty part
    QByteArray expect;
    if (parts.size() == 1) {
        expect = parts.first();
    } else {
        QByteArray cat;
        for (const QByteArray &p : parts) cat += p;
        expect = Md4::hash(cat);
    }
    Ed2kHasher e;
    QCOMPARE(feedChunked(e, d, 1 << 20), expect);
    QCOMPARE(e.partHashes(), parts);
    QCOMPARE(e.partHashes().size(), Ed2kHasher::partCount(size));
    QCOMPARE(Ed2kHasher::fileHashFromParts(parts), expect);
    if (size == P) {
        QCOMPARE(parts.size(), 2);
        QCOMPARE(toHex(parts.at(1)), QStringLiteral("31d6cfe0d16ae931b73c59d7e0c089c0"));
    }
    if (size == P - 1)
        QCOMPARE(expect, Md4::hash(d));   // single part: file hash == part hash == MD4(data)
}

// ---------------------------------------------------------------- AICH
void TestHash::aichStructure()
{
    const qint64 B = AichHasher::BlockSize;
    // one block: root = SHA1(data)
    {
        const QByteArray d = pattern(100);
        QCOMPARE(aichRoot(d), sha1(d));
        const QByteArray full = pattern(B);
        QCOMPARE(aichRoot(full), sha1(full));
    }
    // two blocks
    {
        const QByteArray d = pattern(B + 10);
        QCOMPARE(aichRoot(d), sha1(sha1(d.left(B)), sha1(d.mid(B))));
    }
    // three blocks: left branch root → left gets (3+1)/2 = 2 blocks
    {
        const QByteArray d = pattern(2 * B + 1);
        const QByteArray h0 = sha1(d.left(B)), h1 = sha1(d.mid(B, B)), h2 = sha1(d.mid(2 * B));
        QCOMPARE(aichRoot(d), sha1(sha1(h0, h1), h2));
    }
    // six blocks: root(L) 3|3; left(L) 2|1; right(R) 1|2
    {
        const QByteArray d = pattern(5 * B + 7);
        QList<QByteArray> h;
        for (int i = 0; i < 6; ++i) h << sha1(d.mid(i * B, B));
        const QByteArray expect = sha1(sha1(sha1(h[0], h[1]), h[2]), sha1(h[3], sha1(h[4], h[5])));
        QCOMPARE(aichRoot(d), expect);
        AichHasher a;
        QCOMPARE(feedChunked(a, d, 4097), expect);
        QCOMPARE(a.blockHashes(), h);
        QCOMPARE(a.partHashes(), QList<QByteArray>{expect});
    }
    // a whole part has 53 blocks, the last one 143 360 bytes
    {
        const QByteArray d = pattern(AichHasher::PartSize);
        AichHasher a;
        a.update(d);
        const QByteArray root = a.finish();
        QCOMPARE(a.blockHashes().size(), 53);
        QCOMPARE(a.blockHashes().last(), sha1(d.mid(52 * B)));
        QCOMPARE(d.size() - 52 * B, qint64(143360));
        QCOMPARE(a.partHashes(), QList<QByteArray>{root});
    }
}

void TestHash::aichParts()
{
    const qint64 P = AichHasher::PartSize;
    // PART+1: parts split 1|1, second part is one 1-byte block
    {
        const QByteArray d = pattern(P + 1);
        const QByteArray p0 = aichRoot(d.left(P));
        const QByteArray p1 = sha1(d.mid(P));
        AichHasher a;
        QCOMPARE(feedChunked(a, d, 1 << 20), sha1(p0, p1));
        QCOMPARE(a.partHashes(), (QList<QByteArray>{p0, p1}));
    }
    // six parts (last one short): root(L) 3|3, left(L) 2|1, right(R) 1|2, exercises both branch rules.
    // NOTE (eMule quirk): the branch flag is inherited by the block subtree of a part, so a part that
    // is a RIGHT branch splits its 53 blocks 26|27 instead of 27|26, its hash differs from the AICH
    // root of the same 9.28 MB standalone.
    {
        const qint64 size = 5 * P + 777777;
        const QByteArray d = pattern(size);
        const bool isLeft[6] = {true, false, false, true, true, false};
        QList<QByteArray> p;
        for (int i = 0; i < 6; ++i) {
            const QByteArray part = d.mid(i * P, P);
            p << refPartTree(blocksOf(part), 0, int(blocksOf(part).size()), isLeft[i]);
            if (isLeft[i]) QCOMPARE(p.last(), aichRoot(part));
        }
        QVERIFY(p[1] != aichRoot(d.mid(P, P)));
        const QByteArray expect = sha1(sha1(sha1(p[0], p[1]), p[2]), sha1(p[3], sha1(p[4], p[5])));
        AichHasher a;
        QCOMPARE(feedChunked(a, d, 3 << 20), expect);
        QCOMPARE(a.partHashes(), p);
    }
}

// ---------------------------------------------------------------- BitTorrent pieces
void TestHash::btPieces()
{
    const QByteArray d = pattern(1000);
    BtPieceHasher b(256);
    const QByteArray all = feedChunked(b, d, 100);
    QCOMPARE(b.pieces().size(), 4);
    QCOMPARE(all.size(), 80);
    for (int i = 0; i < 4; ++i)
        QCOMPARE(b.pieces().at(i), BtPieceHasher::hashPiece(d.mid(i * 256, 256)));
    QCOMPARE(BtPieceHasher::suggestedPieceLength(0), qint64(16384));
    QVERIFY(BtPieceHasher::suggestedPieceLength(4LL << 30) >= (2 << 20));
}

// ---------------------------------------------------------------- MultiHasher
void TestHash::multiHasherFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray d = pattern(Ed2kHasher::PartSize + 12345);
    const QString path = dir.filePath(QStringLiteral("f.bin"));
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        QCOMPARE(f.write(d), qint64(d.size()));
    }
    MultiHasher m(MultiHasher::Default | MultiHasher::Md5 | MultiHasher::BtPieces, 1 << 18);
    QList<qint64> progress;
    QString err;
    QVERIFY(MultiHasher::hashFile(path, m, nullptr, [&](qint64 done) { progress << done; }, &err));
    QVERIFY(!progress.isEmpty());
    QCOMPARE(progress.last(), qint64(d.size()));
    QCOMPARE(m.bytesHashed(), qint64(d.size()));

    Ed2kHasher e; e.update(d);
    AichHasher a; a.update(d);
    TigerTree t; t.update(d);
    BtPieceHasher bt(1 << 18); bt.update(d);
    const mx::FileHashes h = m.hashes();
    QCOMPARE(h.ed2k, e.finish());
    QCOMPARE(h.aich, a.finish());
    QCOMPARE(h.tth, t.finish());
    QCOMPARE(h.sha1, sha1(d));
    QCOMPARE(h.sha256, QCryptographicHash::hash(d, QCryptographicHash::Sha256));
    QCOMPARE(h.md5, QCryptographicHash::hash(d, QCryptographicHash::Md5));
    bt.finish();
    QCOMPARE(m.btPieces(), bt.pieces());
    QCOMPARE(m.ed2kPartHashes(), e.partHashes());
    QCOMPARE(m.ed2kPartHashes().size(), 2);
    QCOMPARE(m.aichPartHashes(), a.partHashes());
    QCOMPARE(m.tthLevel(1 << 20), t.levelForUnit(1 << 20));
    QCOMPARE(m.tthLevel(1 << 20).size(), 10);

    // default kinds: no md5
    MultiHasher m2;
    QVERIFY(MultiHasher::hashFile(path, m2));
    QVERIFY(m2.hashes().md5.isEmpty());
    QCOMPARE(m2.hashes().ed2k, h.ed2k);

    // cancel + missing file
    std::atomic<bool> cancel{true};
    MultiHasher m3;
    QVERIFY(!MultiHasher::hashFile(path, m3, &cancel, {}, &err));
    QVERIFY(!MultiHasher::hashFile(dir.filePath(QStringLiteral("missing")), m3, nullptr, {}, &err));
    QVERIFY(!err.isEmpty());
}

// ---------------------------------------------------------------- encodings
void TestHash::base32Rfc4648_data()
{
    QTest::addColumn<QByteArray>("in");
    QTest::addColumn<QString>("out");
    QTest::newRow("empty") << QByteArray("") << "";
    QTest::newRow("f") << QByteArray("f") << "MY";
    QTest::newRow("fo") << QByteArray("fo") << "MZXQ";
    QTest::newRow("foo") << QByteArray("foo") << "MZXW6";
    QTest::newRow("foob") << QByteArray("foob") << "MZXW6YQ";
    QTest::newRow("fooba") << QByteArray("fooba") << "MZXW6YTB";
    QTest::newRow("foobar") << QByteArray("foobar") << "MZXW6YTBOI";
}

void TestHash::base32Rfc4648()
{
    QFETCH(QByteArray, in);
    QFETCH(QString, out);
    QCOMPARE(toBase32(in), out);
    bool ok = false;
    QCOMPARE(fromBase32(out, &ok), in);
    QVERIFY(ok);
    QCOMPARE(fromBase32(out.toLower(), &ok), in);
    // RFC form with padding
    QString padded = out;
    while (padded.size() % 8) padded += QLatin1Char('=');
    QCOMPARE(fromBase32(padded, &ok), in);
}

void TestHash::hexHelpers()
{
    bool ok = false;
    QCOMPARE(toHex(QByteArray("\x01\xab\xff", 3)), QStringLiteral("01abff"));
    QCOMPARE(fromHex(QStringLiteral("01ABff"), &ok), QByteArray("\x01\xab\xff", 3));
    QVERIFY(ok);
    QVERIFY(fromHex(QStringLiteral("abc"), &ok).isEmpty());
    QVERIFY(!ok);
    QVERIFY(fromHex(QStringLiteral("zz"), &ok).isEmpty());
    QVERIFY(!ok);
    QVERIFY(!fromBase32(QStringLiteral("MZ1"), &ok).size());
    QVERIFY(!ok);
    const QByteArray h20 = pattern(20);
    QCOMPARE(decodeHash(toHex(h20), 20), h20);
    QCOMPARE(decodeHash(toBase32(h20), 20), h20);
    QVERIFY(decodeHash(toHex(h20), 16).isEmpty());
}

// ---------------------------------------------------------------- links
void TestHash::magnetParse()
{
    const QString hex = QStringLiteral("c12fe1c06bba254a9dc9f519b335aa7c1367a88a");
    const ParsedLink l = parseLink(
        QStringLiteral("magnet:?xt=urn:btih:") + hex.toUpper()
        + QStringLiteral("&dn=Ubuntu+22.04+Desktop%21&xl=3654957056"
                         "&tr=udp%3A%2F%2Ftracker.example.org%3A1337%2Fannounce&tr=http://t2.example/a"));
    QVERIFY2(l.isValid(), qPrintable(l.error));
    QCOMPARE(l.kind, ParsedLink::Magnet);
    QCOMPARE(toHex(l.hashes.btih), hex);
    QCOMPARE(l.name, QStringLiteral("Ubuntu 22.04 Desktop!"));
    QCOMPARE(l.size, qint64(3654957056LL));
    QCOMPARE(l.trackers, (QStringList{QStringLiteral("udp://tracker.example.org:1337/announce"),
                                      QStringLiteral("http://t2.example/a")}));
    // base32 btih
    const ParsedLink l2 = parseMagnet(QStringLiteral("magnet:?xt=urn:btih:") + toBase32(fromHex(hex)));
    QVERIFY(l2.isValid());
    QCOMPARE(l2.hashes.btih, l.hashes.btih);
}

void TestHash::magnetMultiXt()
{
    const QByteArray ed2k = pattern(16), sha = pattern(20), tth = pattern(24).right(24), aich = pattern(40).right(20),
                     md5 = pattern(16).right(16);
    const QString uri = QStringLiteral("magnet:?xt.1=urn:ed2k:") + toHex(ed2k)
                      + QStringLiteral("&xt.2=urn:bitprint:") + toBase32(sha) + QLatin1Char('.') + toBase32(tth)
                      + QStringLiteral("&xt.3=urn:aich:") + toBase32(aich) + QStringLiteral("&xt.4=urn:md5:") + toHex(md5)
                      + QStringLiteral("&xt.5=urn:btmh:1220abcd&xt.6=urn:foo:bar"
                                       "&so=0,2,4-6&ws=http%3A%2F%2Fmirror.example%2Ff.iso"
                                       "&as=http://as.example/f&xs=http://xs.example/f&x.pe=1.2.3.4:6881"
                                       "&kt=linux+iso&dn=f.iso");
    const ParsedLink l = parseMagnet(uri);
    QVERIFY2(l.isValid(), qPrintable(l.error));
    QCOMPARE(l.hashes.ed2k, ed2k);
    QCOMPARE(l.hashes.sha1, sha);
    QCOMPARE(l.hashes.tth, tth);
    QCOMPARE(l.hashes.aich, aich);
    QCOMPARE(l.hashes.md5, md5);
    QCOMPARE(l.selectOnly, (QList<int>{0, 2, 4, 5, 6}));
    QCOMPARE(l.webSeeds, QStringList{QStringLiteral("http://mirror.example/f.iso")});
    QCOMPARE(l.acceptableSources, QStringList{QStringLiteral("http://as.example/f")});
    QCOMPARE(l.exactSources, QStringList{QStringLiteral("http://xs.example/f")});
    QCOMPARE(l.sources, QStringList{QStringLiteral("1.2.3.4:6881")});
    QCOMPARE(l.extras.value(QStringLiteral("btmh")).toString(), QStringLiteral("1220abcd"));
    QCOMPARE(l.extras.value(QStringLiteral("xt")).toString(), QStringLiteral("urn:foo:bar"));
    QCOMPARE(l.extras.value(QStringLiteral("kt")).toString(), QStringLiteral("linux+iso"));
    // separate sha1 + tree:tiger forms
    const ParsedLink l2 = parseMagnet(QStringLiteral("magnet:?xt=urn:sha1:%1&xt=urn:tree:tiger:%2")
                                          .arg(toBase32(sha), toBase32(tth)));
    QCOMPARE(l2.hashes.sha1, sha);
    QCOMPARE(l2.hashes.tth, tth);
    const ParsedLink l3 = parseMagnet(QStringLiteral("magnet:?xt=urn:tree:tiger/1024:%1").arg(toBase32(tth)));
    QCOMPARE(l3.hashes.tth, tth);
}

void TestHash::magnetInvalid()
{
    QVERIFY(!parseMagnet(QStringLiteral("magnet:?dn=nothing")).isValid());
    QVERIFY(!parseMagnet(QStringLiteral("magnet:?xt=urn:btih:1234")).isValid());
    QVERIFY(!parseLink(QStringLiteral("http://example.org/x")).isValid());
    QVERIFY(!parseLink(QString()).isValid());
    QVERIFY(!parseLink(QStringLiteral("ed2k://|server|1.2.3.4|4661|/")).isValid());
    QVERIFY(!parseLink(QStringLiteral("ed2k://|file|x|abc|0123456789ABCDEF0123456789ABCDEF|/")).isValid());
    QVERIFY(!parseLink(QStringLiteral("ed2k://|file|x|10|0123|/")).isValid());
    QVERIFY(!parseMagnet(QStringLiteral("magnet:?xt=urn:btih:1234")).error.isEmpty());
}

void TestHash::magnetRoundTrip()
{
    ParsedLink l;
    l.kind = ParsedLink::Magnet;
    l.hashes.btih = pattern(20);
    l.hashes.ed2k = pattern(16);
    l.hashes.sha1 = pattern(21).right(20);
    l.hashes.tth = pattern(30).right(24);
    l.hashes.aich = pattern(33).right(20);
    l.hashes.md5 = pattern(17).right(16);
    l.name = QStringLiteral("Some File & [x] 100% é.mkv");
    l.size = 123456789;
    l.trackers << QStringLiteral("udp://t.example:80/announce?x=1&y=2");
    l.webSeeds << QStringLiteral("https://w.example/a b");
    l.acceptableSources << QStringLiteral("http://a.example/");
    l.exactSources << QStringLiteral("urn:btpk:abcd");
    l.selectOnly = {1, 3, 4, 5, 9};
    l.sources << QStringLiteral("10.0.0.1:1234");
    const QString m = formatMagnet(l);
    const ParsedLink r = parseMagnet(m);
    QVERIFY2(r.isValid(), qPrintable(r.error));
    QCOMPARE(r.hashes.btih, l.hashes.btih);
    QCOMPARE(r.hashes.ed2k, l.hashes.ed2k);
    QCOMPARE(r.hashes.sha1, l.hashes.sha1);
    QCOMPARE(r.hashes.tth, l.hashes.tth);
    QCOMPARE(r.hashes.aich, l.hashes.aich);
    QCOMPARE(r.hashes.md5, l.hashes.md5);
    QCOMPARE(r.name, l.name);
    QCOMPARE(r.size, l.size);
    QCOMPARE(r.trackers, l.trackers);
    QCOMPARE(r.webSeeds, l.webSeeds);
    QCOMPARE(r.acceptableSources, l.acceptableSources);
    QCOMPARE(r.exactSources, l.exactSources);
    QCOMPARE(r.selectOnly, l.selectOnly);
    QCOMPARE(r.sources, l.sources);
    QVERIFY(m.contains(QStringLiteral("so=1,3-5,9")));
    // FileHashes::toMagnet (mxapi) output must parse to the same hashes too
    const ParsedLink r2 = parseMagnet(l.hashes.toMagnet(l.name, l.size));
    QVERIFY(r2.isValid());
    QVERIFY(r2.hashes.matches(l.hashes));
    QCOMPARE(r2.name, l.name);
}

void TestHash::ed2kLinkParse()
{
    const QByteArray aich = pattern(20);
    const QString uri = QStringLiteral("ed2k://|file|My%20File%20%28x%29.avi|734003200|0123456789ABCDEF0123456789abcdef|h=")
                      + toBase32(aich)
                      + QStringLiteral("|p=00112233445566778899aabbccddeeff:ffeeddccbbaa99887766554433221100|"
                                       "s=http://web.example/f.avi|/|sources,1.2.3.4:4662,5.6.7.8:4242|/");
    const ParsedLink l = parseLink(uri);
    QVERIFY2(l.isValid(), qPrintable(l.error));
    QCOMPARE(l.kind, ParsedLink::Ed2k);
    QCOMPARE(l.name, QStringLiteral("My File (x).avi"));
    QCOMPARE(l.size, qint64(734003200));
    QCOMPARE(toHex(l.hashes.ed2k), QStringLiteral("0123456789abcdef0123456789abcdef"));
    QCOMPARE(l.hashes.aich, aich);
    QCOMPARE(l.ed2kHashsetParts.size(), 2);
    QCOMPARE(l.webSeeds, QStringList{QStringLiteral("http://web.example/f.avi")});
    QCOMPARE(l.sources, (QStringList{QStringLiteral("1.2.3.4:4662"), QStringLiteral("5.6.7.8:4242")}));
    // %7C-encoded separators
    const ParsedLink l2 = parseLink(QStringLiteral("ed2k://%7Cfile%7Ca.txt%7C3%7C0123456789ABCDEF0123456789ABCDEF%7C/"));
    QVERIFY2(l2.isValid(), qPrintable(l2.error));
    QCOMPARE(l2.name, QStringLiteral("a.txt"));
    QCOMPARE(l2.size, qint64(3));
}

void TestHash::ed2kLinkFormat()
{
    mx::FileHashes h;
    h.ed2k = pattern(16);
    const QString name = QStringLiteral("a|b c%d é.mp3");
    const QString s = formatEd2kLink(name, 42, h);
    QVERIFY(s.startsWith(QStringLiteral("ed2k://|file|")));
    QVERIFY(s.endsWith(QStringLiteral("|/")));
    QVERIFY(!s.contains(QStringLiteral("h=")));
    ParsedLink l = parseEd2kLink(s);
    QVERIFY2(l.isValid(), qPrintable(l.error));
    QCOMPARE(l.name, name);
    QCOMPARE(l.size, qint64(42));
    QCOMPARE(l.hashes.ed2k, h.ed2k);
    h.aich = pattern(20);
    const QString s2 = formatEd2kLink(name, 42, h);
    QVERIFY(s2.contains(QStringLiteral("|h=") + toBase32(h.aich) + QStringLiteral("|/")));
    l = parseEd2kLink(s2);
    QCOMPARE(l.hashes.aich, h.aich);
    // known-good form produced by eMule for a simple name
    mx::FileHashes h2;
    h2.ed2k = fromHex(QStringLiteral("31d6cfe0d16ae931b73c59d7e0c089c0"));
    QCOMPARE(formatEd2kLink(QStringLiteral("empty.txt"), 0, h2),
             QStringLiteral("ed2k://|file|empty.txt|0|31D6CFE0D16AE931B73C59D7E0C089C0|/"));
}

QTEST_GUILESS_MAIN(TestHash)
#include "tst_hash.moc"
