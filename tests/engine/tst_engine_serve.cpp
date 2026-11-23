// SPDX-License-Identifier: GPL-3.0-or-later
// [up-1] Serving files that are still downloading, and the hash-set rules of the downloader:
//  * a file a running download writes (a torrent's payload in the shared incoming folder) is never
//    hashed or shared as a complete file; its pieces are served by its trusted identity instead;
//  * a part-file download serves only units verified against trusted hashes, with its trusted set,
//    never one checked against a provisional set;
//  * a hash set never bans a source: a wrong one is dropped and not asked for again, the source's
//    data keeps being used (it is checked on its own); a set refuted later is dropped quietly;
//  * a torrent download does not take eD2k sets at all;
//  * the engine tells a source why it stops using it (banned, paused, done, cancelled);
//  * "Range not available" from a partial source is not a failure.
#include <QtCore/QDir>
#include <QtTest/QTest>

#include "core/library.h"
#include "engine_env.h"
#include "mxhash/ed2k.h"
#include "mxhash/encoding.h"

using namespace mx;
using namespace mx::etest;

namespace {
constexpr qint64 P = hash::Ed2kHasher::PartSize;

DownloadRequest req(const QString &name, const QByteArray &data, const FileHashes &h, const QList<SearchSource> &src)
{
    DownloadRequest r;
    r.name = name;
    r.size = data.size();
    r.hashes = h;
    r.sources = src;
    return r;
}

struct SrcInfo { bool banned = false; int strikes = 0; qint64 verified = 0; };
SrcInfo srcInfo(const QVariantMap &dbg, const QString &needle)
{
    SrcInfo r;
    for (const QVariant &v : dbg.value(QStringLiteral("sources")).toList()) {
        const QVariantMap m = v.toMap();
        if (!m.value(QStringLiteral("key")).toString().contains(needle)) continue;
        r.banned = m.value(QStringLiteral("banned")).toBool();
        r.strikes = m.value(QStringLiteral("strikes")).toInt();
        r.verified = m.value(QStringLiteral("verified")).toLongLong();
    }
    return r;
}

QString magnetFor(const QString &id, const QString &name)
{
    return QStringLiteral("magnet:?xt=urn:btih:%1&dn=%2").arg(id, name);
}

qint64 covered(const QList<QPair<qint64, qint64>> &l)
{
    qint64 n = 0;
    for (const auto &r : l) n += r.second - r.first;
    return n;
}

QByteArray md4Root(const QList<QByteArray> &parts)
{
    if (parts.size() == 1) return parts.first();
    QCryptographicHash h(QCryptographicHash::Md4);
    for (const QByteArray &p : parts) h.addData(p);
    return h.result();
}
} // namespace

class TstEngineServe : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { registerMetaTypes(); }
    void busyFileNeverShared();
    void torrentPartialServedByPieces();
    void partFileServesTrustedUnitsOnly();
    void provisionalSetNotServed();
    void wrongSetFromGoodSourceNoBan();
    void refutedLaterNoBan();
    void torrentIgnoresEd2kSets();
    void closeReasons();
    void rangeNotAvailableIsNoFailure();
};

// The Library alone: a busy file in a shared folder is skipped by the scan and by every lookup;
// once released it is hashed and shared like any other file.
void TstEngineServe::busyFileNeverShared()
{
    Env env(QStringLiteral("serve-busy"), true);
    const QString dir = env.root + QStringLiteral("/share");
    QDir().mkpath(dir);
    const QByteArray data = randomBytes(2 * 1024 * 1024 + 3, 501);
    const QString path = dir + QStringLiteral("/still-downloading.bin");
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
    f.close();
    env.library->setBusyPaths({path});
    env.library->start();
    env.libraryStarted = true;
    env.library->addFolder(dir, true);
    QVERIFY(env.library->waitIdle(20000));
    QVERIFY(env.library->isBusy(path));
    QVERIFY(env.library->sharedFiles().isEmpty());
    QVERIFY(env.library->files().isEmpty());
    SharedFile sf;
    QVERIFY(!env.library->findByHash(hashesOf(data, 1), &sf));
    QVERIFY(env.library->read(hashesOf(data, 1), 0, 100).isEmpty());
    QCOMPARE(env.library->hashRuns(), 0);   // never read for hashing while busy
    // released (the download finished): the folder is looked at again and the file shared
    env.library->setBusyPaths({});
    QVERIFY(QTest::qWaitFor([&] { return env.library->findByHash(hashesOf(data, 1), &sf); }, 20000));
    QCOMPARE(sf.size, qint64(data.size()));
    QCOMPARE(env.library->sharedFiles().size(), 1);
}

// A torrent download in the shared incoming folder (the owner's 5.2 report): the file is busy, the
// library does not hash it; the engine serves the pieces libtorrent verified by the torrent's
// identity, with no eD2k set; when complete it is shared as a whole file.
void TstEngineServe::torrentPartialServedByPieces()
{
    Env env(QStringLiteral("serve-torrent"), true);
    env.settings.setValue(QString(keys::transfers::ShareCompleted), true);
    FakeTorrentBackend be;
    const QByteArray data = randomBytes(3 * 1024 * 1024 + 77, 502);
    const int pl = 256 * 1024;
    const QString tid = be.makeTorrent(QStringLiteral("big-partial.bin"), {{QStringLiteral("big-partial.bin"), data}}, pl);
    be.known[tid].peerPiecesPerTick = 1;   // a piece every 200 ms: ~3 s for the file
    be.known[tid].peers = 1;
    env.engineUp(&be);
    const QString id = env.engine->downloadUri(magnetFor(tid, QStringLiteral("big-partial.bin")));
    QVERIFY(!id.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(env.info(id).done >= 3 * pl, 15000);
    const TransferInfo t = env.info(id);
    QVERIFY(t.state != TransferState::Completed);
    const QString path = t.savePath;
    QVERIFY2(!path.isEmpty() && QFile::exists(path), qPrintable(path));
    // libtorrent gives the file its full size at once (holes): make the fake do the same
    QFile pf(path);
    QVERIFY(pf.resize(data.size()));
    QTRY_VERIFY_WITH_TIMEOUT(env.library->isBusy(path), 3000);
    env.library->rescan();
    QVERIFY(env.library->waitIdle(20000));
    for (const SharedFile &x : env.library->sharedFiles()) QVERIFY2(x.path != path, "a file still downloading was shared");
    FileHashes h;
    h.btih = hash::fromHex(tid);
    QList<QPair<qint64, qint64>> have;
    QVERIFY(env.library->partialRanges(h, &have));
    QVERIFY(!have.isEmpty());
    FileHashes hf = h;   // asked for the torrent's file 0 (how a torrent download opens a source)
    hf.btFileIndex = 0;
    QVERIFY(env.library->partialRanges(hf, nullptr));
    hf.btFileIndex = 1;   // a file the torrent does not have
    QVERIFY(!env.library->partialRanges(hf, nullptr));
    QVERIFY(covered(have) < data.size());
    for (const auto &r : std::as_const(have)) {
        QCOMPARE(r.first % pl, qint64(0));
        QCOMPARE(env.library->read(h, r.first, 1000), data.mid(int(r.first), 1000));
    }
    const qint64 hole = have.first().first > 0 ? 0 : have.first().second;   // a range nobody verified
    if (hole < data.size()) QVERIFY(env.library->read(h, hole, 1000).isEmpty());
    QVERIFY(env.library->ed2kPartHashes(h).isEmpty());   // no set it could vouch for
    QVERIFY2(env.waitDone(id, 60000), qPrintable(env.info(id).statusText));
    QTRY_VERIFY_WITH_TIMEOUT(!env.library->isBusy(path), 3000);
    QVERIFY(!env.library->partialRanges(h, nullptr));
    SharedFile sf;
    QTRY_VERIFY_WITH_TIMEOUT(env.library->findByHash(hashesOf(data, 1), &sf), 20000);
    QCOMPARE(sf.hashes.ed2k, hashesOf(data, 2).ed2k);   // hashed over the complete file
}

// A part-file download with a trusted ed2k hash and its set: only verified units are served, the
// set offered is the trusted one.
void TstEngineServe::partFileServesTrustedUnitsOnly()
{
    Env env(QStringLiteral("serve-part"), true);
    auto *ov = env.addPlugin(Net::Overlay, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(3 * P + 4321, 503);
    SourceConfig c;
    c.data = data;
    c.bytesPerSec = 6 << 20;
    c.pref = 1 << 20;
    c.hashSet = ed2kParts(data);
    const SearchSource s = ov->addSource(QStringLiteral("ovl://seed"), c, QStringLiteral("seed"));
    env.engineUp();
    const FileHashes h = hashesOf(data, 2);
    const QString id = env.engine->download(req(QStringLiteral("part.bin"), data, h, {s}));
    QList<QPair<qint64, qint64>> have;
    QTRY_VERIFY_WITH_TIMEOUT(env.library->partialRanges(h, &have) && !have.isEmpty(), 20000);
    if (env.info(id).state != TransferState::Completed) {
        for (const auto &r : std::as_const(have)) {
            QVERIFY(r.first % P == 0);
            QVERIFY(r.second % P == 0 || r.second == data.size());
            QCOMPARE(env.library->read(h, r.first, 4096), data.mid(int(r.first), 4096));
        }
        QList<QByteArray> set = env.library->ed2kPartHashes(h);
        QCOMPARE(set.size(), hash::Ed2kHasher::partCount(data.size()));
        QCOMPARE(md4Root(set), h.ed2k);
    }
    QVERIFY2(env.waitDone(id, 60000), qPrintable(env.info(id).statusText));
}

// Only a SHA-1 is trusted; the source's set is provisional: nothing is served from units checked
// against it (it could be wrong), and it is not offered.
void TstEngineServe::provisionalSetNotServed()
{
    Env env(QStringLiteral("serve-prov"), true);
    auto *ov = env.addPlugin(Net::Overlay, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(3 * P + 99, 504);
    SourceConfig c;
    c.data = data;
    c.bytesPerSec = 6 << 20;
    c.pref = 1 << 20;
    c.hashSet = ed2kParts(data);
    const SearchSource s = ov->addSource(QStringLiteral("ovl://seed"), c, QStringLiteral("seed"));
    env.engineUp();
    const FileHashes h = hashesOf(data, 1);
    const QString id = env.engine->download(req(QStringLiteral("prov.bin"), data, h, {s}));
    bool sawPartial = false;
    QVERIFY(QTest::qWaitFor([&] {
        sawPartial = sawPartial || env.library->partialRanges(h, nullptr);
        return env.info(id).state == TransferState::Completed;
    }, 60000));
    QVERIFY2(!sawPartial, "units checked only against a provisional set were offered");
}

// The 5.2 case: a source shares a file it is still downloading and sends a part set computed over
// the holes of its copy, while the bytes it sends are right. Only a SHA-1 is trusted, so the set is
// used provisionally. Nothing may be banned, the download completes with the source's bytes, its
// sets are not used again, and at most two parts are fetched twice.
void TstEngineServe::wrongSetFromGoodSourceNoBan()
{
    Env env(QStringLiteral("serve-52"));
    auto *ov = env.addPlugin(Net::Overlay, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(4 * P + 1234, 505);
    QByteArray holes = data;
    for (qint64 i = P / 2; i < P / 2 + P; ++i) holes[int(i)] = 0;   // its copy: a hole over parts 0 and 1
    holes.replace(int(3 * P), int(P / 3), QByteArray(int(P / 3), '\0'));
    SourceConfig a, g;
    a.data = g.data = data;
    a.hashSet = ed2kParts(holes);   // computed over the copy with holes
    a.bytesPerSec = g.bytesPerSec = 6 << 20;
    a.pref = g.pref = 1 << 20;
    g.queuedMs = 300;
    const SearchSource sa = ov->addSource(QStringLiteral("ovl://alice"), a, QStringLiteral("alice"));
    const SearchSource sg = ov->addSource(QStringLiteral("ovl://good"), g, QStringLiteral("good"));
    env.engineUp();
    const QString id = env.engine->download(req(QStringLiteral("w52.bin"), data, hashesOf(data, 1), {sa, sg}));
    QVERIFY2(env.waitDone(id, 90000), qPrintable(env.info(id).statusText));
    QCOMPARE(readFile(env.info(id).savePath), data);
    const QVariantMap dbg = env.engine->debugInfo(id);
    const qint64 total = ov->served(QStringLiteral("ovl://alice")) + ov->served(QStringLiteral("ovl://good"));
    const SrcInfo al = srcInfo(dbg, QStringLiteral("alice")), go = srcInfo(dbg, QStringLiteral("good"));
    qInfo("5.2 set: served %lld for %lld (+%lld); unit failures %d; whole-file failures %d; alice banned %d strikes %d; good banned %d strikes %d",
          total, qint64(data.size()), total - data.size(), dbg.value(QStringLiteral("unitFailures")).toInt(),
          dbg.value(QStringLiteral("wholeFileFailures")).toInt(), al.banned, al.strikes, go.banned, go.strikes);
    QVERIFY(!al.banned);
    QVERIFY(!go.banned);
    QCOMPARE(go.strikes, 0);
    QCOMPARE(dbg.value(QStringLiteral("wholeFileFailures")).toInt(), 0);
    QVERIFY2(total <= data.size() + 2 * P + (1 << 20), "more than two parts fetched twice");
}

// A set used provisionally is refuted later by a trusted ed2k hash (the owner's ban text): it is
// dropped, its sender is NOT banned and keeps sending.
void TstEngineServe::refutedLaterNoBan()
{
    Env env(QStringLiteral("serve-refuted"));
    auto *ov = env.addPlugin(Net::Overlay, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(3 * P + 10, 506);
    SourceConfig a;
    a.data = data;
    a.hashSet = ed2kParts(randomBytes(data.size(), 507));
    a.bytesPerSec = 3 << 20;
    a.pref = 1 << 20;
    const SearchSource sa = ov->addSource(QStringLiteral("ovl://alice"), a, QStringLiteral("alice"));
    env.engineUp();
    const QString id = env.engine->download(req(QStringLiteral("ref.bin"), data, hashesOf(data, 1), {sa}));
    QTRY_VERIFY_WITH_TIMEOUT(env.engine->debugInfo(id).value(QStringLiteral("hashSetProvisional")).toBool(), 10000);
    DownloadRequest join = req(QStringLiteral("ref.bin"), data, hashesOf(data, 1 | 2), {});
    QCOMPARE(env.engine->download(join), id);   // joins: the ed2k hash is trusted now
    QTRY_VERIFY_WITH_TIMEOUT(!env.engine->debugInfo(id).value(QStringLiteral("hashSetProvisional")).toBool(), 5000);
    QVERIFY(!srcInfo(env.engine->debugInfo(id), QStringLiteral("alice")).banned);
    QVERIFY2(env.waitDone(id, 60000), qPrintable(env.info(id).statusText));
    QCOMPARE(readFile(env.info(id).savePath), data);
    QVERIFY(!srcInfo(env.engine->debugInfo(id), QStringLiteral("alice")).banned);
}

// A torrent download takes no eD2k set from an overlay source: piece hashes judge.
void TstEngineServe::torrentIgnoresEd2kSets()
{
    Env env(QStringLiteral("serve-tset"));
    FakeTorrentBackend be;
    auto *ov = env.addPlugin(Net::Overlay, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(2 * P + 55, 508);
    const QString tid = be.makeTorrent(QStringLiteral("tset.bin"), {{QStringLiteral("tset.bin"), data}}, 256 * 1024);
    SourceConfig a;
    a.data = data;
    a.hashSet = ed2kParts(randomBytes(data.size(), 509));
    a.bytesPerSec = 8 << 20;
    a.pref = 1 << 20;
    const SearchSource sa = ov->addSource(QStringLiteral("ovl://alice"), a, QStringLiteral("alice"));
    env.engineUp(&be);
    DownloadRequest r;
    r.uri = magnetFor(tid, QStringLiteral("tset.bin"));
    r.hashes.btih = hash::fromHex(tid);
    r.size = data.size();
    r.name = QStringLiteral("tset.bin");
    r.sources = {sa};
    const QString id = env.engine->download(r);
    QVERIFY2(env.waitDone(id, 60000), qPrintable(env.info(id).statusText));
    const QVariantMap dbg = env.engine->debugInfo(id);
    QVERIFY(dbg.value(QStringLiteral("hashSetProvider")).toString().isEmpty());
    QVERIFY(!srcInfo(dbg, QStringLiteral("alice")).banned);
    QCOMPARE(readFile(env.info(id).savePath), data);
}

// The engine says why it stops using a source: banned (with the reason), paused, done, cancelled.
void TstEngineServe::closeReasons()
{
    Env env(QStringLiteral("serve-close"));
    auto *ov = env.addPlugin(Net::Overlay, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(2 * P + 1, 510);
    SourceConfig slow, bad;
    slow.data = data;
    slow.bytesPerSec = 512 * 1024;
    slow.pref = 256 * 1024;
    slow.hashSet = ed2kParts(data);   // trusted units: the corrupt source is caught part by part
    bad.data = data;
    bad.corruptAll = true;
    bad.bytesPerSec = 4 << 20;
    const SearchSource ss = ov->addSource(QStringLiteral("ovl://slow"), slow, QStringLiteral("slow"));
    const SearchSource sb = ov->addSource(QStringLiteral("ovl://bad"), bad, QStringLiteral("bad"));
    env.engineUp();
    const QString id = env.engine->download(req(QStringLiteral("close.bin"), data, hashesOf(data, 2), {ss, sb}));
    QTRY_VERIFY_WITH_TIMEOUT(ov->source(QStringLiteral("ovl://bad")) && ov->source(QStringLiteral("ovl://bad"))->closeCode == QLatin1String("banned"), 30000);
    qInfo("banned source was told: %s", qPrintable(ov->source(QStringLiteral("ovl://bad"))->closeText));
    QVERIFY(!ov->source(QStringLiteral("ovl://bad"))->closeText.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(env.info(id).done > 0, 10000);
    env.engine->pause(id);
    QTRY_COMPARE_WITH_TIMEOUT(ov->source(QStringLiteral("ovl://slow"))->closeCode, QStringLiteral("paused"), 5000);
    env.engine->resume(id);
    QVERIFY2(env.waitDone(id, 60000), qPrintable(env.info(id).statusText));
    QCOMPARE(ov->source(QStringLiteral("ovl://slow"))->closeCode, QStringLiteral("done"));
    // cancel
    const QByteArray d2 = randomBytes(P + 3, 511);
    SourceConfig c2;
    c2.data = d2;
    c2.bytesPerSec = 256 * 1024;
    const SearchSource s2 = ov->addSource(QStringLiteral("ovl://c2"), c2, QStringLiteral("c2"));
    const QString id2 = env.engine->download(req(QStringLiteral("c2.bin"), d2, hashesOf(d2, 2), {s2}));
    QTRY_VERIFY_WITH_TIMEOUT(env.info(id2).done > 0, 10000);
    env.engine->cancel(id2, true);
    QTRY_COMPARE_WITH_TIMEOUT(ov->source(QStringLiteral("ovl://c2"))->closeCode, QStringLiteral("cancelled"), 5000);
}

// A source whose map was too generous answers "Range not available": no failure, no strike, the
// rest of the file comes from where it is.
void TstEngineServe::rangeNotAvailableIsNoFailure()
{
    Env env(QStringLiteral("serve-nar"));
    auto *ov = env.addPlugin(Net::Overlay, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(2 * P + 777, 512);
    SourceConfig part, full;
    part.data = full.data = data;
    part.missing = {{P, 2 * P}};
    part.bytesPerSec = 8 << 20;
    part.pref = full.pref = 1 << 20;
    full.bytesPerSec = 2 << 20;
    full.queuedMs = 500;
    const SearchSource sp = ov->addSource(QStringLiteral("ovl://part"), part, QStringLiteral("part"));
    const SearchSource sf = ov->addSource(QStringLiteral("ovl://full"), full, QStringLiteral("full"));
    env.engineUp();
    const QString id = env.engine->download(req(QStringLiteral("nar.bin"), data, hashesOf(data, 2), {sp, sf}));
    QVERIFY2(env.waitDone(id, 60000), qPrintable(env.info(id).statusText));
    QCOMPARE(readFile(env.info(id).savePath), data);
    const QVariantMap dbg = env.engine->debugInfo(id);
    QVERIFY(!srcInfo(dbg, QStringLiteral("part")).banned);
    QCOMPARE(srcInfo(dbg, QStringLiteral("part")).strikes, 0);
    qInfo("not-available answers: %d", ov->source(QStringLiteral("ovl://part"))->notAvailable);
}

QTEST_MAIN(TstEngineServe)
#include "tst_engine_serve.moc"
