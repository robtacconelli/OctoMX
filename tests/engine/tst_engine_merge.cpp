// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX engine tests [merge-1]: copies matched only by name and size. One judge verifies every
// block (BitTorrent pieces, else eD2k part hashes, else the whole-file hash); the first verified
// unit confirms a network, failed units ban the source and drop the network after a few, the
// waste cap keeps an unconfirmed network to a few units, completion checks every known hash kind
// and reports a disagreement in plain words, the plain-file side promotes to the torrent (role
// change), folder torrents take other networks' copies of their inner files.
#include <QtTest/QtTest>

#include "core/network_manager.h"
#include "core/search_manager.h"
#include "engine_env.h"

using namespace mx;
using namespace mx::etest;

namespace {

QString magnetFor(const QString &id, const QString &name)
{
    return QStringLiteral("magnet:?xt=urn:btih:%1&dn=%2").arg(id, name);
}

DownloadRequest torrentReq(const QString &id, const QString &name, qint64 size, const QList<SearchSource> &extra)
{
    DownloadRequest r;
    r.name = name;
    r.size = size;
    r.hashes.btih = QByteArray::fromHex(id.toLatin1());
    r.uri = magnetFor(id, name);
    SearchSource bt;
    bt.net = Net::BitTorrent;
    bt.uri = r.uri;
    r.sources << bt << extra;
    return r;
}

SearchSource candidate(SearchSource s, const FileHashes &claim)
{
    s.extra.insert(QStringLiteral("candidate"), true);
    s.extra.insert(QStringLiteral("hashes"), claim.toVariant());
    return s;
}

QString dump(const TransferInfo &t)
{
    QStringList l;
    for (const TransferSource &s : t.sources) l << QStringLiteral("[%1|%2|%3]").arg(netShortName(s.net), s.user.name, s.statusText);
    return t.statusText + QStringLiteral(" :: ") + l.join(QLatin1Char(' '));
}

QString rowStatus(const TransferInfo &t, const QString &userName)
{
    for (const TransferSource &s : t.sources)
        if (s.user.name == userName) return s.statusText;
    return QStringLiteral("<no row>");
}

QVariantMap srcDebug(const QVariantMap &dbg, const QString &keyPart)
{
    for (const QVariant &v : dbg.value(QStringLiteral("sources")).toList())
        if (v.toMap().value(QStringLiteral("key")).toString().contains(keyPart)) return v.toMap();
    return {};
}

} // namespace

class TstEngineMerge : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { registerMetaTypes(); }
    void torrentJudgeConfirmsAndBans();
    void networkDroppedAfterFailures();
    void wasteCap();
    void completionAgreesOrReportsDisagreement();
    void partStoreDisagreementReported();
    void roleChangePlainToTorrent();
    void nextTorrentWhenTheFirstIsAnotherFile();
    void folderTorrentTakesInnerFileCopies();
    void bundleFlagCorrectedByMetadata();
    void deadTorrentHandsOverToTheOtherCopy();
};

// Judge = BitTorrent piece hashes (metadata there, no BT peer): the eD2k copy matched by name and
// size delivers; its first verified piece confirms eD2k (its ed2k hash is learned), a corrupt one is banned.
void TstEngineMerge::torrentJudgeConfirmsAndBans()
{
    FakeTorrentBackend be;   // outlives the engine
    Env env(QStringLiteral("m-judge"));
    auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(4 * 1024 * 1024, 501);
    const QString tid = be.makeTorrent(QStringLiteral("judge.mkv"), {{QStringLiteral("judge.mkv"), data}}, 256 * 1024);
    SourceConfig good, bad;
    good.data = bad.data = data;
    good.bytesPerSec = 2 << 20;
    bad.bytesPerSec = 4 << 20;
    bad.corruptAll = true;
    const FileHashes claim = hashesOf(data, 2);
    const SearchSource g = candidate(ed->addSource(QStringLiteral("ed2k-peer://1.0.0.1:4662"), good, QStringLiteral("goodpeer")), claim);
    const SearchSource b = candidate(ed->addSource(QStringLiteral("ed2k-peer://1.0.0.2:4662"), bad, QStringLiteral("badpeer")), claim);
    env.engineUp(&be);
    const QString id = env.engine->download(torrentReq(tid, QStringLiteral("judge.mkv"), data.size(), {g, b}));
    QTRY_VERIFY2_WITH_TIMEOUT(rowStatus(env.info(id), QStringLiteral("goodpeer")).contains(QLatin1String("checked while downloading"))
                                  || rowStatus(env.info(id), QStringLiteral("goodpeer")).contains(QLatin1String("(confirmed)")),
                              qPrintable(dump(env.info(id))), 10000);
    QVERIFY2(env.waitDone(id, 60000), qPrintable(dump(env.info(id))));
    QCOMPARE(readFile(env.info(id).savePath), data);
    QVERIFY2(be.injectedBad.isEmpty(), "unverified data reached the file");
    const QVariantMap dbg = env.engine->debugInfo(id);
    QCOMPARE(dbg.value(QStringLiteral("judge")).toString(), QStringLiteral("BitTorrent piece hashes"));
    // the corrupt peer names the same (confirmed) hash: it is a source of this file whose data is bad,
    // struck per failed piece (banned after three), never trusted with unverified bytes
    const QVariantMap badDbg = srcDebug(dbg, QStringLiteral("badpeer"));
    QVERIFY2(badDbg.value(QStringLiteral("banned")).toBool() || badDbg.value(QStringLiteral("strikes")).toInt() > 0, qPrintable(dump(env.info(id))));
    QVERIFY(srcDebug(dbg, QStringLiteral("goodpeer")).value(QStringLiteral("trusted")).toBool());
    QVERIFY(dbg.value(QStringLiteral("piecePreverifyFailures")).toInt() >= 1);
    QVERIFY(dbg.value(QStringLiteral("candNets")).toMap().value(QStringLiteral("eD2k")).toMap().value(QStringLiteral("confirmed")).toBool());
    QCOMPARE(env.info(id).hashes.ed2k, claim.ed2k);   // the confirmed copy's hash is the file's now
}

// Three different eD2k copies, all another file: each fails one piece, then eD2k is dropped from
// this download with a status line; BitTorrent completes the file, nothing unverified is used.
void TstEngineMerge::networkDroppedAfterFailures()
{
    Env env(QStringLiteral("m-drop"));
    env.tuning.sourceRetryBaseMs = 60000;
    FakeTorrentBackend be;
    auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
    auto *kad = env.addPlugin(Net::Kad, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(6 * 1024 * 1024, 502);
    const QByteArray other = randomBytes(6 * 1024 * 1024, 503);
    const QString tid = be.makeTorrent(QStringLiteral("drop.mkv"), {{QStringLiteral("drop.mkv"), data}}, 256 * 1024);
    be.known[tid].peerPiecesPerTick = 1;
    be.known[tid].peers = 3;
    SourceConfig wrong;
    wrong.data = other;
    wrong.bytesPerSec = 3 << 20;
    const FileHashes claim = hashesOf(other, 2);
    QList<SearchSource> cands;
    for (int i = 1; i <= 3; ++i)
        cands << candidate(ed->addSource(QStringLiteral("ed2k-peer://2.0.0.%1:4662").arg(i), wrong, QStringLiteral("ed%1").arg(i)), claim);
    cands << candidate(kad->addSource(QStringLiteral("ed2k-peer://2.0.0.9:4662"), wrong, QStringLiteral("kad1")), claim);
    env.engineUp(&be);
    const QString id = env.engine->download(torrentReq(tid, QStringLiteral("drop.mkv"), data.size(), cands));
    QTRY_VERIFY2_WITH_TIMEOUT(rowStatus(env.info(id), QStringLiteral("eD2k copy")).startsWith(QLatin1String("The eD2k copy is a different file: dropped")),
                              qPrintable(dump(env.info(id))), 30000);
    const QVariantMap c = env.engine->debugInfo(id).value(QStringLiteral("candNets")).toMap().value(QStringLiteral("eD2k")).toMap();
    QVERIFY(c.value(QStringLiteral("dropped")).toBool());
    QCOMPARE(c.value(QStringLiteral("failed")).toInt(), 3);
    // every unconfirmed copy of that network (Kad is the same eD2k copy) is banned at once
    QVERIFY(srcDebug(env.engine->debugInfo(id), QStringLiteral("kad1")).value(QStringLiteral("banned")).toBool());
    qInfo("fetched from the wrong eD2k copy before it was dropped: %lld bytes", c.value(QStringLiteral("fetched")).toLongLong());
    QVERIFY2(env.waitDone(id, 90000), qPrintable(dump(env.info(id))));
    QCOMPARE(readFile(env.info(id).savePath), data);
    QVERIFY(be.injectedBad.isEmpty());
    QVERIFY(env.info(id).hashes.ed2k.isEmpty());   // the wrong hash was never learned
}

// The waste cap: one wrong copy, fast, many requests allowed: it works on at most two pieces before
// the judge answers (and is then banned / its network dropped).
void TstEngineMerge::wasteCap()
{
    FakeTorrentBackend be;   // outlives the engine
    Env env(QStringLiteral("m-cap"));
    auto *gn = env.addPlugin(Net::Gnutella, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(8 * 1024 * 1024, 504);
    const QByteArray other = randomBytes(8 * 1024 * 1024, 505);
    const QString tid = be.makeTorrent(QStringLiteral("cap.mkv"), {{QStringLiteral("cap.mkv"), data}}, 256 * 1024);
    be.known[tid].peerPiecesPerTick = 1;
    be.known[tid].peers = 2;
    SourceConfig wrong;
    wrong.data = other;
    wrong.pref = 256 * 1024;
    wrong.maxPending = 8;          // could ask for 8 pieces at once without the cap
    wrong.bytesPerSec = 512 * 1024;
    const SearchSource w = candidate(gn->addSource(QStringLiteral("gnut://3.0.0.1/cap.mkv"), wrong, QStringLiteral("gnut")), hashesOf(other, 1));
    env.engineUp(&be);
    const QString id = env.engine->download(torrentReq(tid, QStringLiteral("cap.mkv"), data.size(), {w}));
    QTRY_VERIFY2_WITH_TIMEOUT(srcDebug(env.engine->debugInfo(id), QStringLiteral("|gnut")).value(QStringLiteral("banned")).toBool(),
                              qPrintable(dump(env.info(id))), 30000);
    const SourceStats *st = gn->source(QStringLiteral("gnut://3.0.0.1/cap.mkv"));
    QVERIFY(st);
    QSet<qint64> pieces;
    for (const auto &r : st->requests)
        for (qint64 p = r.first / (256 * 1024); p * 256 * 1024 < r.first + r.second; ++p) pieces.insert(p);
    qInfo("the wrong copy was asked for %lld piece(s), served %lld bytes", qint64(pieces.size()), st->served);
    QVERIFY2(pieces.size() <= 2, "more than two units in flight from an unconfirmed network");
    QVERIFY(st->served <= 2 * 256 * 1024);
    QCOMPARE(rowStatus(env.info(id), QStringLiteral("Gnutella copy")).left(42), QStringLiteral("The Gnutella copy is a different file: dro"));
}

// Completion by the judge (BitTorrent pieces); the library hashes the file: an eD2k claim that
// agrees becomes part of the identity, one that differs is reported and dropped (not published).
void TstEngineMerge::completionAgreesOrReportsDisagreement()
{
    for (int lie = 0; lie < 2; ++lie) {
        FakeTorrentBackend be;   // outlives the engine
        Env env(QStringLiteral("m-complete-%1").arg(lie), true);
        auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
        const QByteArray data = randomBytes(3 * 1024 * 1024 + 11, 506 + lie);
        const QString tid = be.makeTorrent(QStringLiteral("done.mkv"), {{QStringLiteral("done.mkv"), data}}, 256 * 1024);
        SourceConfig good;
        good.data = data;
        const FileHashes truth = hashesOf(data, 2);
        FileHashes claim = truth;
        if (lie) claim.ed2k = QByteArray(16, '\x42');   // the network names another hash for the same bytes
        const SearchSource s = candidate(ed->addSource(QStringLiteral("ed2k-peer://4.0.0.1:4662"), good, QStringLiteral("peer")), claim);
        env.engineUp(&be);
        const QString id = env.engine->download(torrentReq(tid, QStringLiteral("done.mkv"), data.size(), {s}));
        QVERIFY2(env.waitDone(id, 60000), qPrintable(dump(env.info(id))));
        QCOMPARE(readFile(env.info(id).savePath), data);
        QTRY_VERIFY_WITH_TIMEOUT(!env.info(id).hashes.sha1.isEmpty(), 20000);   // the library hashed it
        const TransferInfo t = env.info(id);
        if (!lie) {
            QCOMPARE(t.hashes.ed2k, truth.ed2k);
            QVERIFY2(!t.statusText.contains(QLatin1String("not the same")), qPrintable(t.statusText));
        } else {
            QVERIFY2(t.statusText.contains(QLatin1String("the eD2k copy was not the same file")), qPrintable(t.statusText));
            QVERIFY2(t.statusText.contains(QLatin1String("BitTorrent piece hashes")), qPrintable(t.statusText));
            QCOMPARE(t.hashes.ed2k, truth.ed2k);    // the real hash replaced the wrong one
            QCOMPARE(t.state, TransferState::Completed);
        }
    }
}

// Part-file judge (eD2k part hashes from the trusted eD2k identity): a Gnutella copy delivers good
// parts but names a wrong SHA-1; the file completes by the parts, the SHA-1 disagreement is reported.
void TstEngineMerge::partStoreDisagreementReported()
{
    Env env(QStringLiteral("m-part"));
    auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
    auto *gn = env.addPlugin(Net::Gnutella, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(12 * 1000 * 1000, 508);
    SourceConfig slow, fast;
    slow.data = fast.data = data;
    slow.bytesPerSec = 200 * 1024;
    slow.hashSet = ed2kParts(data);
    fast.bytesPerSec = 6 << 20;
    FileHashes wrongSha1;
    wrongSha1.sha1 = QByteArray(20, '\x17');
    const SearchSource e = ed->addSource(QStringLiteral("ed2k-peer://5.0.0.1:4662"), slow, QStringLiteral("edpeer"));
    const SearchSource g = candidate(gn->addSource(QStringLiteral("gnut://5.0.0.2/f.mkv"), fast, QStringLiteral("gnutpeer")), wrongSha1);
    env.engineUp();
    DownloadRequest r;
    r.name = QStringLiteral("part.mkv");
    r.size = data.size();
    r.hashes = hashesOf(data, 2);
    r.sources = {e, g};
    const QString id = env.engine->download(r);
    QVERIFY2(env.waitDone(id, 60000), qPrintable(dump(env.info(id))));
    QCOMPARE(readFile(env.info(id).savePath), data);
    const TransferInfo t = env.info(id);
    QVERIFY2(t.statusText.contains(QLatin1String("the Gnutella copy was not the same file")), qPrintable(t.statusText));
    QVERIFY2(t.statusText.contains(QLatin1String("eD2k part hashes")), qPrintable(t.statusText));
    QVERIFY(t.hashes.sha1 != wrongSha1.sha1);
}

// Started from the plain-file side (eD2k identity, part file) with a torrent matched by name and size:
// the torrent's metadata is fetched in parallel, a held piece matches → promoted, the judge becomes
// BitTorrent piece hashes and BitTorrent is confirmed.
void TstEngineMerge::roleChangePlainToTorrent()
{
    FakeTorrentBackend be;   // outlives the engine
    Env env(QStringLiteral("m-role"));
    auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(5 * 1000 * 1000, 509);
    const QString tid = be.makeTorrent(QStringLiteral("role.mkv"), {{QStringLiteral("role.mkv"), data}}, 128 * 1024);
    be.known[tid].peerPiecesPerTick = 1;
    be.known[tid].peers = 2;
    SourceConfig c;
    c.data = data;
    c.bytesPerSec = 400 * 1024;
    const SearchSource s = ed->addSource(QStringLiteral("ed2k-peer://6.0.0.1:4662"), c, QStringLiteral("edpeer"));
    SearchSource bt;
    bt.net = Net::BitTorrent;
    bt.uri = magnetFor(tid, QStringLiteral("role.mkv"));
    FileHashes btClaim;
    btClaim.btih = QByteArray::fromHex(tid.toLatin1());
    env.engineUp(&be);
    DownloadRequest r;
    r.name = QStringLiteral("role.mkv");
    r.size = data.size();
    r.hashes = hashesOf(data, 2);
    r.sources = {s, candidate(bt, btClaim)};
    const QString id = env.engine->download(r);
    QTRY_COMPARE_WITH_TIMEOUT(env.engine->debugInfo(id).value(QStringLiteral("store")).toString(), QStringLiteral("torrent"), 20000);
    QTRY_COMPARE_WITH_TIMEOUT(env.engine->debugInfo(id).value(QStringLiteral("judge")).toString(), QStringLiteral("BitTorrent piece hashes"), 10000);
    QVERIFY(env.engine->debugInfo(id).value(QStringLiteral("candNets")).toMap().value(QStringLiteral("BitTorrent")).toMap().value(QStringLiteral("confirmed")).toBool());
    QVERIFY2(env.waitDone(id, 60000), qPrintable(dump(env.info(id))));
    QCOMPARE(readFile(env.info(id).savePath), data);
}

// Two torrents matched by name and size: the first holds another file (its piece does not match our
// data, rejected), the second is tried next and promoted.
void TstEngineMerge::nextTorrentWhenTheFirstIsAnotherFile()
{
    FakeTorrentBackend be;   // outlives the engine
    Env env(QStringLiteral("m-next"));
    auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(5 * 1000 * 1000, 510);
    const QByteArray other = randomBytes(5 * 1000 * 1000, 511);
    const QString wrongId = be.makeTorrent(QStringLiteral("next.mkv"), {{QStringLiteral("next.mkv"), other}}, 128 * 1024);
    const QString rightId = be.makeTorrent(QStringLiteral("next.mkv "), {{QStringLiteral("next.mkv"), data}}, 128 * 1024);
    be.known[rightId].peerPiecesPerTick = 1;
    be.known[rightId].peers = 1;
    SourceConfig c;
    c.data = data;
    c.bytesPerSec = 400 * 1024;
    const SearchSource s = ed->addSource(QStringLiteral("ed2k-peer://7.0.0.1:4662"), c, QStringLiteral("edpeer"));
    auto btSource = [](const QString &tid) {
        SearchSource bt;
        bt.net = Net::BitTorrent;
        bt.uri = magnetFor(tid, QStringLiteral("next.mkv"));
        FileHashes h;
        h.btih = QByteArray::fromHex(tid.toLatin1());
        return candidate(bt, h);
    };
    env.engineUp(&be);
    DownloadRequest r;
    r.name = QStringLiteral("next.mkv");
    r.size = data.size();
    r.hashes = hashesOf(data, 2);
    r.sources = {s, btSource(wrongId), btSource(rightId)};
    const QString id = env.engine->download(r);
    QTRY_COMPARE_WITH_TIMEOUT(env.engine->debugInfo(id).value(QStringLiteral("torrentId")).toString(), rightId, 30000);
    QVERIFY2(env.waitDone(id, 60000), qPrintable(dump(env.info(id))));
    QCOMPARE(readFile(env.info(id).savePath), data);
}

// A folder torrent (two files): an eD2k copy of its large inner file (same name and size) serves
// that file, every block checked by the pieces; the download's identity stays the torrent's.
void TstEngineMerge::folderTorrentTakesInnerFileCopies()
{
    FakeTorrentBackend be;   // outlives the engine
    Env env(QStringLiteral("m-inner"));
    auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
    const QByteArray movie = randomBytes(3 * 1024 * 1024, 512);
    const QByteArray subs = randomBytes(300 * 1024, 513);
    const QString tid = be.makeTorrent(QStringLiteral("Folder"), {{QStringLiteral("Movie.mkv"), movie}, {QStringLiteral("Subs.srt"), subs}}, 256 * 1024);
    be.known[tid].peerPiecesPerTick = 1;   // the swarm is slow; the eD2k copy helps with Movie.mkv
    be.known[tid].peers = 1;
    SourceConfig c;
    c.data = movie;
    c.bytesPerSec = 4 << 20;
    SearchSource e = candidate(ed->addSource(QStringLiteral("ed2k-peer://8.0.0.1:4662"), c, QStringLiteral("edpeer")), hashesOf(movie, 2));
    e.extra.insert(QStringLiteral("bundlePath"), QStringLiteral("Folder/Movie.mkv"));
    e.extra.insert(QStringLiteral("size"), movie.size());
    env.engineUp(&be);
    DownloadRequest r = torrentReq(tid, QStringLiteral("Folder"), 0, {e});
    const QString id = env.engine->download(r);
    QVERIFY2(env.waitDone(id, 60000), qPrintable(dump(env.info(id))));
    const QVariantMap dbg = env.engine->debugInfo(id);
    QVERIFY2(dbg.value(QStringLiteral("verifiedByNet")).toMap().value(QStringLiteral("eD2k")).toLongLong() > 0, qPrintable(dump(env.info(id))));
    QVERIFY(srcDebug(dbg, QStringLiteral("edpeer")).value(QStringLiteral("trusted")).toBool());
    QCOMPARE(srcDebug(dbg, QStringLiteral("edpeer")).value(QStringLiteral("file")).toInt(), 0);
    QVERIFY(be.injectedBad.isEmpty());
    QVERIFY(env.info(id).hashes.ed2k.isEmpty());   // the inner file's hash is not the folder's
    QVERIFY(env.info(id).isBundle);
}

// A torrent an index flagged as a folder ("judge 2010 mkv", no file count) whose metadata shows one
// file: corrected to a plain file download (name and size of that file), so the background
// search by name and size runs for it.
void TstEngineMerge::bundleFlagCorrectedByMetadata()
{
    FakeTorrentBackend be;   // outlives the engine
    Env env(QStringLiteral("m-flag"));
    const QByteArray data = randomBytes(2 * 1024 * 1024, 514);
    const QString tid = be.makeTorrent(QStringLiteral("Flag.Film.2010.mkv"), {{QStringLiteral("Flag.Film.2010.mkv"), data}}, 256 * 1024);
    env.engineUp(&be);
    DownloadRequest r = torrentReq(tid, QStringLiteral("Flag Film 2010 mkv"), data.size() + 5, {});
    const QString id = env.engine->download(r);
    QTRY_VERIFY_WITH_TIMEOUT(env.engine->debugInfo(id).value(QStringLiteral("store")).toString() == QLatin1String("torrent"), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(env.info(id).name == QLatin1String("Flag.Film.2010.mkv"), 10000);
    const TransferInfo t = env.info(id);
    QVERIFY(!t.isBundle);
    QCOMPARE(t.size, qint64(data.size()));
}

// The owner's case: a torrent from an index (no peer ever gives its file list) merged with a Kad
// keyword hit (no address, its eD2k hash only). The hash is asked for at once; when the metadata
// times out the roles change: the eD2k copy's hashes judge the part file and it completes from Kad.
void TstEngineMerge::deadTorrentHandsOverToTheOtherCopy()
{
    FakeTorrentBackend be;   // outlives the engine
    Env env(QStringLiteral("m-dead"));
    env.settings.setValue(QString(keys::transfers::MetadataTimeoutSec), 1);
    auto *kad = env.addPlugin(Net::Kad, NetworkPlugin::CanFetchRanges | NetworkPlugin::CanFindSources);
    const QByteArray data = randomBytes(3 * 1000 * 1000, 515);
    SourceConfig c;
    c.data = data;
    c.bytesPerSec = 2 << 20;
    const FileHashes claim = hashesOf(data, 2);
    // findSources by that eD2k hash names a peer (Kad source search)
    kad->findFilter = [claim](const FileHashes &h) { return h.ed2k == claim.ed2k; };
    kad->onFind = {kad->addSource(QStringLiteral("ed2k-peer://9.0.0.1:4662"), c, QStringLiteral("kadpeer"))};
    SearchSource placeholder;   // a Kad keyword result: the file's hash, no peer
    placeholder.net = Net::Kad;
    placeholder.uri = QStringLiteral("ed2k://|file|Dead.Film.2010.mkv|%1|%2|/").arg(data.size()).arg(QString::fromLatin1(claim.ed2k.toHex()));
    placeholder.peers = 1;
    env.engineUp(&be);
    const QString unknownTorrent = QStringLiteral("00112233445566778899aabbccddeeff00112233");   // nobody has it
    const QString id = env.engine->download(torrentReq(unknownTorrent, QStringLiteral("Dead Film 2010 mkv"), data.size(), {candidate(placeholder, claim)}));
    QTRY_VERIFY2_WITH_TIMEOUT(env.engine->debugInfo(id).value(QStringLiteral("store")).toString() == QLatin1String("part"),
                              qPrintable(dump(env.info(id))), 15000);
    QVERIFY2(env.waitDone(id, 60000), qPrintable(dump(env.info(id))));
    QCOMPARE(readFile(env.info(id).savePath), data);
    QCOMPARE(env.info(id).hashes.ed2k, claim.ed2k);
}

QTEST_GUILESS_MAIN(TstEngineMerge)
#include "tst_engine_merge.moc"
