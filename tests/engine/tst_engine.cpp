// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX engine: PartFileStore / scheduler / lifecycle tests with fake plugins.
#include <QtCore/QDir>
#include <QtTest/QTest>

#include "core/dao.h"
#include "engine_env.h"
#include "mxhash/ed2k.h"

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

// total bytes requested more than once across all sources
qint64 overlapBytes(const QList<SourceStats *> &srcs)
{
    QList<QPair<qint64, qint64>> all;
    for (SourceStats *s : srcs) all << s->requests;
    std::sort(all.begin(), all.end());
    qint64 over = 0, reach = -1;
    for (const auto &r : all) {
        const qint64 e = r.first + r.second;
        if (r.first < reach) over += qMin(reach, e) - r.first;
        reach = qMax(reach, e);
    }
    return over;
}

QList<SourceStats *> live(TestPlugin *p)
{
    QList<SourceStats *> l;
    for (const auto &s : p->stats) l << s.get();
    return l;
}
} // namespace

class TstEngine : public QObject {
    Q_OBJECT
private slots:
    void singleSource();
    void fiveSources();
    void slowSourceReplaced();
    void corruptWithPartHashes();
    void corruptWithoutFineHashes();
    void queuePauseResumeCancelPriority();
    void restartResumes();
    void killRestartLosesLittle();
    void unknownSizeHttp();
    void sequentialOnlySource();
    void misbehavingAndShortSources();
    void candidateConfirmed();
    void candidateRejected();
    void hashLinkMidDownload();
    void dedupeJoin();
    void collisionNamingAndLibrary();
    void diskErrorPauses();
    void partialSharing();
    void completionNotHashedTwice();
    void allSourcesFakeEndsInError();
    void unverifiedSourcesExplained();
    void statusTexts();
    // [engine-3] per-source hash sets + black box
    void sourceHashSetTrustedBlackBox();
    void sourceHashSetProvisionalCorrupt();
    void sourceHashSetProvisionalRefuted();
    void sourceHashSetWrongRootIgnored();
};

namespace {
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
} // namespace

void TstEngine::singleSource()
{
    Env env(QStringLiteral("single"));
    auto *http = env.addPlugin(Net::Http, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(3 * 1024 * 1024 + 17, 1);
    SourceConfig c;
    c.data = data;
    const SearchSource s = http->addSource(QStringLiteral("http://a/file.bin"), c);
    env.engineUp();
    const QString id = env.engine->download(req(QStringLiteral("file.bin"), data, hashesOf(data, 1), {s}));
    QVERIFY(!id.isEmpty());
    QVERIFY2(env.waitDone(id, 20000), qPrintable(env.info(id).statusText));
    const TransferInfo t = env.info(id);
    QCOMPARE(t.statusText, QStringLiteral("Complete"));
    QCOMPARE(t.done, t.size);
    QCOMPARE(readFile(t.savePath), data);
    QVERIFY(t.savePath.startsWith(env.root + QStringLiteral("/incoming/")));
    QCOMPARE(env.completed->size(), 1);
    QVERIFY(!QFile::exists(env.engine->incompleteDir() + QLatin1Char('/') + id + QStringLiteral(".part")));
    // ed2k + sha256 + tth computed at completion and kept in the identity
    QCOMPARE(t.hashes.ed2k.size(), 16);
    QCOMPARE(t.hashes.sha256.size(), 32);
}

void TstEngine::fiveSources()
{
    Env env(QStringLiteral("five"));
    auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges | NetworkPlugin::CanFindSources);
    const QByteArray data = randomBytes(30 * 1000 * 1000 + 4321, 2);
    const FileHashes h = hashesOf(data, 2);
    ed->hashSetOnFind << qMakePair(P, ed2kParts(data));
    QList<SearchSource> srcs;
    const int rates[5] = {4 << 20, 2 << 20, 1 << 20, 512 << 10, 0};
    for (int i = 0; i < 5; ++i) {
        SourceConfig c;
        c.data = data;
        c.bytesPerSec = rates[i];
        c.pref = 180 * 1024 * 2;
        c.maxPending = 1 + (i % 2);
        if (i == 4) c.avail = {{0, 15 * 1000 * 1000}};   // a partial source
        if (i == 3) c.avail = {{9 * 1000 * 1000, data.size()}};
        srcs << ed->addSource(QStringLiteral("ed2k://peer%1").arg(i), c, QStringLiteral("10.0.0.%1:4662").arg(i + 1));
    }
    env.engineUp();
    const QString id = env.engine->download(req(QStringLiteral("five.bin"), data, h, srcs));
    QVERIFY2(QTest::qWaitFor([&] { return env.info(id).sourcesActive >= 3; }, 10000), "sources did not start");
    QVERIFY2(env.waitDone(id, 60000), qPrintable(env.info(id).statusText));
    QCOMPARE(readFile(env.info(id).savePath), data);
    const QList<SourceStats *> l = live(ed);
    QVERIFY(l.size() >= 5);
    int contributing = 0;
    for (SourceStats *s : l) {
        if (s->served > 0) ++contributing;
        for (const auto &r : s->requests) {
            // availability respected
            bool inside = s->cfg.avail.isEmpty();
            for (const auto &a : s->cfg.avail) if (r.first >= a.first && r.first + r.second <= a.second) inside = true;
            QVERIFY2(inside, "request outside the source's availability");
            // never straddles an eD2k part
            QVERIFY(r.first / P == (r.first + r.second - 1) / P);
        }
    }
    QVERIFY(contributing >= 4);
    const qint64 over = overlapBytes(l);
    qInfo("double-assigned bytes: %lld of %lld (endgame only)", over, qint64(data.size()));
    // the endgame may ask for every request that is on its way a second time: the five sources hold
    // at most 1 + 2 + 1 + 2 + 1 = 7 requests of 360 KiB
    QVERIFY2(over <= data.size() * 3 / 100 + 7 * 360 * 1024, "chunks were double-assigned outside the endgame");
    const QVariantMap dbg = env.engine->debugInfo(id);
    QCOMPARE(dbg.value(QStringLiteral("scheme")).toInt(), 1);   // eD2k parts were used
}

void TstEngine::slowSourceReplaced()
{
    Env env(QStringLiteral("slow"));
    auto *http = env.addPlugin(Net::Http, NetworkPlugin::CanFetchRanges);
    // [safe-1] 12 MiB (was 6): the download outlasts the slow detection (slowForMs + slowHoldMs) even
    // on a fast run, so the slow source is always replaced, not just outrun (failed under load)
    const QByteArray data = randomBytes(12 * 1024 * 1024, 3);
    SourceConfig fast, slow;
    fast.data = slow.data = data;
    fast.bytesPerSec = 3 << 20;
    fast.maxPending = 1;
    slow.bytesPerSec = 1000;
    slow.maxPending = 1;
    const SearchSource sf = http->addSource(QStringLiteral("http://fast/x"), fast);
    const SearchSource ss = http->addSource(QStringLiteral("http://slow/x"), slow);
    env.engineUp();
    QElapsedTimer t;
    t.start();
    const QString id = env.engine->download(req(QStringLiteral("x.bin"), data, hashesOf(data, 1), {ss, sf}));
    // the limits are wide for a machine whose timers tick slowly (the test sources then serve at a
    // quarter of their nominal speed); left to the slow source its half would take hours
    QVERIFY2(env.waitDone(id, 90000), qPrintable(env.info(id).statusText));
    QCOMPARE(readFile(env.info(id).savePath), data);
    SourceStats *slowSrc = http->source(QStringLiteral("http://slow/x"));
    QVERIFY(slowSrc);
    qInfo("done in %lld ms; slow source served %lld bytes, cancelled %d times", t.elapsed(), slowSrc->served, slowSrc->cancelCount);
    QVERIFY(slowSrc->cancelCount >= 1);
    QVERIFY(t.elapsed() < 60000);
}

void TstEngine::corruptWithPartHashes()
{
    Env env(QStringLiteral("corrupt-parts"));
    auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges | NetworkPlugin::CanFindSources);
    const QByteArray data = randomBytes(25 * 1000 * 1000, 4);
    ed->hashSetOnFind << qMakePair(P, ed2kParts(data));
    SourceConfig good, bad;
    good.data = bad.data = data;
    good.bytesPerSec = 3 << 20;
    bad.bytesPerSec = 8 << 20;
    bad.corrupt = {{P + 1000, P + 1100}};   // inside part 1 only
    const SearchSource sg = ed->addSource(QStringLiteral("ed2k://good"), good, QStringLiteral("1.1.1.1:4662"));
    const SearchSource sb = ed->addSource(QStringLiteral("ed2k://bad"), bad, QStringLiteral("2.2.2.2:4662"));
    env.engineUp();
    const QString id = env.engine->download(req(QStringLiteral("p.bin"), data, hashesOf(data, 2), {sg, sb}));
    QVERIFY2(env.waitDone(id, 60000), qPrintable(env.info(id).statusText));
    QCOMPARE(readFile(env.info(id).savePath), data);
    qint64 total = 0;
    for (SourceStats *s : live(ed)) total += s->served;
    const QVariantMap dbg = env.engine->debugInfo(id);
    qInfo("served %lld bytes for a %lld-byte file; banned=%d", total, qint64(data.size()), dbg.value(QStringLiteral("banned")).toInt());
    bool badBanned = false, goodBanned = false;
    for (const QVariant &v : dbg.value(QStringLiteral("sources")).toList()) {
        const QVariantMap m = v.toMap();
        if (m.value(QStringLiteral("key")).toString().contains(QLatin1String("2.2.2.2"))) badBanned = m.value(QStringLiteral("banned")).toBool();
        if (m.value(QStringLiteral("key")).toString().contains(QLatin1String("1.1.1.1"))) goodBanned = m.value(QStringLiteral("banned")).toBool();
    }
    QVERIFY(badBanned);
    QVERIFY(!goodBanned);
    // only the bad part (at most twice) was fetched again, never the whole file
    QVERIFY2(total <= data.size() + 3 * P, "more than the bad part was re-downloaded");
}

void TstEngine::corruptWithoutFineHashes()
{
    Env env(QStringLiteral("corrupt-whole"));
    auto *http = env.addPlugin(Net::Http, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(4 * 1024 * 1024 + 99, 5);
    SourceConfig good, bad;
    good.data = bad.data = data;
    good.bytesPerSec = 2 << 20;
    bad.bytesPerSec = 4 << 20;
    bad.corrupt = {{1000, 1001}};
    bad.avail = {};
    const SearchSource sg = http->addSource(QStringLiteral("http://good/f"), good);
    const SearchSource sb = http->addSource(QStringLiteral("http://bad/f"), bad);
    env.engineUp();
    const QString id = env.engine->download(req(QStringLiteral("w.bin"), data, hashesOf(data, 1 | 4), {sb, sg}));
    QVERIFY2(env.waitDone(id, 60000), qPrintable(env.info(id).statusText));
    QCOMPARE(readFile(env.info(id).savePath), data);
    const QVariantMap dbg = env.engine->debugInfo(id);
    qInfo("whole-file failures: %d, single-source mode: %d", dbg.value(QStringLiteral("wholeFileFailures")).toInt(),
          dbg.value(QStringLiteral("singleSource")).toBool());
    QVERIFY(dbg.value(QStringLiteral("wholeFileFailures")).toInt() >= 1);
    QVERIFY(dbg.value(QStringLiteral("singleSource")).toBool());
}

void TstEngine::queuePauseResumeCancelPriority()
{
    Env env(QStringLiteral("queue"));
    env.settings.setValue(QString(keys::transfers::MaxDownloads), 1);
    auto *http = env.addPlugin(Net::Http, NetworkPlugin::CanFetchRanges);
    QList<QByteArray> datas;
    QStringList ids;
    env.engineUp();
    for (int i = 0; i < 3; ++i) {
        datas << randomBytes(2 * 1024 * 1024, 10 + i);
        SourceConfig c;
        c.data = datas.last();
        c.bytesPerSec = 256 * 1024;
        const SearchSource s = http->addSource(QStringLiteral("http://q/%1").arg(i), c);
        ids << env.engine->download(req(QStringLiteral("q%1.bin").arg(i), datas.last(), hashesOf(datas.last(), 1), {s}));
    }
    QTRY_VERIFY_WITH_TIMEOUT(env.info(ids[0]).state != TransferState::Queued, 3000);
    QCOMPARE(env.info(ids[1]).statusText, QStringLiteral("Locally Queued"));
    QCOMPARE(env.info(ids[2]).state, TransferState::Queued);
    // priority: the third jumps the queue and takes the only slot
    env.engine->setPriority(ids[2], 1);
    QTRY_VERIFY_WITH_TIMEOUT(env.info(ids[2]).state != TransferState::Queued, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(env.info(ids[0]).state, TransferState::Queued, 3000);
    // pause the running one → the next in order runs
    env.engine->pause(ids[2]);
    QCOMPARE(env.info(ids[2]).state, TransferState::Paused);
    QTRY_VERIFY_WITH_TIMEOUT(env.info(ids[0]).state != TransferState::Queued, 3000);
    // pauseAll / resumeAll
    env.engine->pauseAll();
    for (const QString &id : ids) QCOMPARE(env.info(id).state, TransferState::Paused);
    env.engine->resumeAll();
    QTRY_VERIFY_WITH_TIMEOUT(env.info(ids[2]).state != TransferState::Paused, 3000);
    // cancel with data → part file gone, row removed
    QSignalSpy removed(env.engine.get(), &TransferService::transferRemoved);
    QTRY_VERIFY_WITH_TIMEOUT(env.info(ids[2]).done > 0 || QFile::exists(env.engine->incompleteDir() + QLatin1Char('/') + ids[2] + QStringLiteral(".part")), 5000);
    env.engine->cancel(ids[2], true);
    QCOMPARE(removed.size(), 1);
    QVERIFY(env.engine->transfer(ids[2]).id.isEmpty());
    QTest::qWait(100);
    QVERIFY(!QFile::exists(env.engine->incompleteDir() + QLatin1Char('/') + ids[2] + QStringLiteral(".part")));
    // the rest completes one after the other; clearCompleted removes the rows, keeps the files
    QVERIFY(env.waitDone(ids[0], 30000));
    QVERIFY(env.waitDone(ids[1], 30000));
    const QString p0 = env.info(ids[0]).savePath;
    env.engine->clearCompleted();
    QVERIFY(env.engine->downloads().isEmpty());
    QCOMPARE(readFile(p0), datas[0]);
}

void TstEngine::restartResumes()
{
    Env env(QStringLiteral("restart"));
    auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges | NetworkPlugin::CanFindSources);
    const QByteArray data = randomBytes(3 * P + 12345, 6);
    ed->hashSetOnFind << qMakePair(P, ed2kParts(data));
    SourceConfig c;
    c.data = data;
    c.bytesPerSec = 8 << 20;
    c.pref = 512 * 1024;
    const SearchSource s = ed->addSource(QStringLiteral("ed2k://r"), c, QStringLiteral("3.3.3.3:4662"));
    env.engineUp();
    const QString id = env.engine->download(req(QStringLiteral("r.bin"), data, hashesOf(data, 2), {s}));
    QVERIFY(QTest::qWaitFor([&] { return env.info(id).done >= 2 * P; }, 20000));
    const qint64 verifiedBefore = env.info(id).done;
    env.engineDown(true);
    // second session: everything verified must not be requested again
    const int before = ed->stats.size();
    env.engineUp();
    QCOMPARE(env.engine->downloads().size(), 1);
    QVERIFY(env.info(id).done >= verifiedBefore);
    QVERIFY2(env.waitDone(id, 30000), qPrintable(env.info(id).statusText));
    QCOMPARE(readFile(env.info(id).savePath), data);
    for (int i = before; i < ed->stats.size(); ++i) {
        for (const auto &r : ed->stats[i]->requests)
            QVERIFY2(r.first >= verifiedBefore / P * P || r.first + r.second <= 0, "verified data was requested again");
    }
    // paused stays paused across a restart
    const QByteArray d2 = randomBytes(1 << 20, 7);
    SourceConfig c2;
    c2.data = d2;
    const SearchSource s2 = ed->addSource(QStringLiteral("ed2k://r2"), c2, QStringLiteral("4.4.4.4:4662"));
    DownloadRequest r2 = req(QStringLiteral("r2.bin"), d2, hashesOf(d2, 2), {s2});
    r2.startPaused = true;
    const QString id2 = env.engine->download(r2);
    env.engineDown(true);
    env.engineUp();
    QCOMPARE(env.info(id2).state, TransferState::Paused);
    QCOMPARE(env.info(id).state, TransferState::Completed);
}

void TstEngine::killRestartLosesLittle()
{
    Env env(QStringLiteral("kill"));
    auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges | NetworkPlugin::CanFindSources);
    const QByteArray data = randomBytes(4 * P, 8);
    ed->hashSetOnFind << qMakePair(P, ed2kParts(data));
    SourceConfig c;
    c.data = data;
    c.bytesPerSec = 6 << 20;
    const SearchSource s = ed->addSource(QStringLiteral("ed2k://k"), c, QStringLiteral("5.5.5.5:4662"));
    env.engineUp();
    const QString id = env.engine->download(req(QStringLiteral("k.bin"), data, hashesOf(data, 2), {s}));
    QVERIFY(QTest::qWaitFor([&] { return env.info(id).done >= 2 * P; }, 30000));
    QTest::qWait(env.tuning.persistIntervalMs * 3);
    const qint64 doneAtKill = env.info(id).done;
    env.engineDown(false);   // no shutdown(): like a crash
    env.engineUp();
    const qint64 restored = env.info(id).done;
    qInfo("verified at kill: %lld, restored: %lld", doneAtKill, restored);
    QVERIFY(restored >= doneAtKill - P);
    QVERIFY2(env.waitDone(id, 30000), qPrintable(env.info(id).statusText));
    QCOMPARE(readFile(env.info(id).savePath), data);
}

void TstEngine::unknownSizeHttp()
{
    Env env(QStringLiteral("unknown"));
    auto *http = env.addPlugin(Net::Http, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(1500 * 1000 + 3, 9);
    // (a) the server reports the length once connected
    SourceConfig c;
    c.data = data;
    c.extra.insert(QStringLiteral("size"), data.size());
    c.extra.insert(QStringLiteral("fileName"), QStringLiteral("real name.iso"));
    c.extra.insert(QStringLiteral("supportsRanges"), true);
    http->addSource(QStringLiteral("http://srv/download?id=1"), c);
    // (b) no length at all: sequential until EOF
    SourceConfig u;
    u.data = data;
    u.unknownLength = true;
    u.sequentialOnly = true;
    http->addSource(QStringLiteral("http://srv/stream"), u);
    env.engineUp();
    const QString a = env.engine->downloadUri(QStringLiteral("http://srv/download?id=1"));
    QVERIFY2(env.waitDone(a, 20000), qPrintable(env.info(a).statusText));
    QCOMPARE(env.info(a).size, qint64(data.size()));
    QCOMPARE(env.info(a).name, QStringLiteral("real name.iso"));
    QCOMPARE(readFile(env.info(a).savePath), data);
    const QString b = env.engine->downloadUri(QStringLiteral("http://srv/stream"));
    QVERIFY2(env.waitDone(b, 20000), qPrintable(env.info(b).statusText));
    QCOMPARE(env.info(b).size, qint64(data.size()));
    QCOMPARE(readFile(env.info(b).savePath), data);
}

void TstEngine::sequentialOnlySource()
{
    Env env(QStringLiteral("seq"));
    auto *http = env.addPlugin(Net::Http, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(5 * 1024 * 1024, 11);
    SourceConfig seq, ranged;
    seq.data = ranged.data = data;
    seq.sequentialOnly = true;
    seq.bytesPerSec = 2 << 20;
    ranged.bytesPerSec = 1 << 20;
    const SearchSource s1 = http->addSource(QStringLiteral("http://seq/f"), seq);
    const SearchSource s2 = http->addSource(QStringLiteral("http://ranged/f"), ranged);
    env.engineUp();
    const QString id = env.engine->download(req(QStringLiteral("s.bin"), data, hashesOf(data, 1), {s1, s2}));
    QVERIFY2(env.waitDone(id, 30000), qPrintable(env.info(id).statusText));
    QCOMPARE(readFile(env.info(id).savePath), data);
    for (const auto &r : http->source(QStringLiteral("http://seq/f"))->requests) QCOMPARE(r.first, qint64(0));
}

void TstEngine::misbehavingAndShortSources()
{
    Env env(QStringLiteral("misbehave"));
    auto *http = env.addPlugin(Net::Http, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(3 * 1024 * 1024, 12);
    SourceConfig evil, shorty;
    evil.data = shorty.data = data;
    evil.misbehave = true;
    shorty.shortRequests = true;
    shorty.bytesPerSec = 2 << 20;
    const SearchSource se = http->addSource(QStringLiteral("http://evil/f"), evil);
    const SearchSource ss = http->addSource(QStringLiteral("http://short/f"), shorty);
    env.engineUp();
    const QString id = env.engine->download(req(QStringLiteral("m.bin"), data, hashesOf(data, 1), {se, ss}));
    QVERIFY2(env.waitDone(id, 30000), qPrintable(env.info(id).statusText));
    QCOMPARE(readFile(env.info(id).savePath), data);
    const QVariantMap dbg = env.engine->debugInfo(id);
    qint64 verified = 0;
    for (const QVariant &v : dbg.value(QStringLiteral("sources")).toList()) {
        const QVariantMap m = v.toMap();
        verified += m.value(QStringLiteral("verified")).toLongLong();
        if (m.value(QStringLiteral("key")).toString().contains(QLatin1String("evil")))
            QVERIFY(m.value(QStringLiteral("strikes")).toInt() >= 1);
    }
    // [engine-4] no fine units: the whole-file check credits every accepted byte exactly once (the
    // misplaced first chunks of "evil" were never accepted)
    QCOMPARE(verified, qint64(data.size()));
}

void TstEngine::candidateConfirmed()
{
    Env env(QStringLiteral("cand-ok"));
    auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges | NetworkPlugin::CanFindSources);
    auto *gn = env.addPlugin(Net::Gnutella, NetworkPlugin::CanFetchRanges | NetworkPlugin::CanFindSources);
    auto *ov = env.addPlugin(Net::Overlay, NetworkPlugin::CanHashLinks);
    const QByteArray data = randomBytes(6 * 1000 * 1000, 13);   // single eD2k part: fine unit
    const FileHashes ed2k = hashesOf(data, 2), sha1 = hashesOf(data, 1);
    SourceConfig slowTrusted, cand;
    slowTrusted.data = cand.data = data;
    slowTrusted.bytesPerSec = 200 * 1024;
    cand.bytesPerSec = 4 << 20;
    const SearchSource st = ed->addSource(QStringLiteral("ed2k://t"), slowTrusted, QStringLiteral("6.6.6.6:4662"));
    SearchSource sc = gn->addSource(QStringLiteral("gnut://c"), cand, QStringLiteral("7.7.7.7:6346"));
    sc.extra.insert(QStringLiteral("candidate"), true);
    sc.extra.insert(QStringLiteral("hashes"), sha1.toVariant());
    env.engineUp();
    const QString id = env.engine->download(req(QStringLiteral("c.bin"), data, ed2k, {st, sc}));
    QVERIFY2(env.waitDone(id, 30000), qPrintable(env.info(id).statusText));
    QCOMPARE(readFile(env.info(id).savePath), data);
    QCOMPARE(env.info(id).hashes.sha1, sha1.sha1);      // the link was confirmed and merged
    bool published = false;
    for (const FileHashes &h : ov->publishedLinks) if (h.sha1 == sha1.sha1 && h.ed2k == ed2k.ed2k) published = true;
    QVERIFY(published);
    QVERIFY(gn->source(QStringLiteral("gnut://c"))->served > 0);
    HashLinksDao links(env.db.get());
    QVERIFY(links.expand(ed2k).sha1 == sha1.sha1);
}

void TstEngine::candidateRejected()
{
    Env env(QStringLiteral("cand-bad"));
    auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
    auto *gn = env.addPlugin(Net::Gnutella, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(5 * 1000 * 1000, 14);
    const QByteArray other = randomBytes(5 * 1000 * 1000, 15);    // same size + name, different file
    SourceConfig trusted, cand;
    trusted.data = data;
    trusted.bytesPerSec = 1 << 20;
    cand.data = other;
    cand.bytesPerSec = 8 << 20;
    const SearchSource st = ed->addSource(QStringLiteral("ed2k://t"), trusted, QStringLiteral("8.8.8.8:4662"));
    SearchSource sc = gn->addSource(QStringLiteral("gnut://other"), cand, QStringLiteral("9.9.9.9:6346"));
    sc.extra.insert(QStringLiteral("candidate"), true);
    sc.extra.insert(QStringLiteral("hashes"), hashesOf(other, 1).toVariant());
    env.engineUp();
    const QString id = env.engine->download(req(QStringLiteral("c.bin"), data, hashesOf(data, 2), {st, sc}));
    QVERIFY2(env.waitDone(id, 30000), qPrintable(env.info(id).statusText));
    QCOMPARE(readFile(env.info(id).savePath), data);
    QVERIFY(env.info(id).hashes.sha1 != hashesOf(other, 1).sha1);
    const QVariantMap dbg = env.engine->debugInfo(id);
    QVERIFY(dbg.value(QStringLiteral("banned")).toInt() >= 1);
}

void TstEngine::hashLinkMidDownload()
{
    Env env(QStringLiteral("hashlink"));
    auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges | NetworkPlugin::CanFindSources);
    auto *gn = env.addPlugin(Net::Gnutella, NetworkPlugin::CanFetchRanges | NetworkPlugin::CanFindSources);
    auto *ov = env.addPlugin(Net::Overlay, NetworkPlugin::CanHashLinks);
    const QByteArray data = randomBytes(8 * 1000 * 1000, 16);
    const FileHashes ed2k = hashesOf(data, 2), sha1 = hashesOf(data, 1);
    SourceConfig slow, fast;
    slow.data = fast.data = data;
    slow.bytesPerSec = 300 * 1024;
    fast.bytesPerSec = 4 << 20;
    const SearchSource s1 = ed->addSource(QStringLiteral("ed2k://slow"), slow, QStringLiteral("11.0.0.1:4662"));
    gn->onFind << gn->addSource(QStringLiteral("gnut://fast"), fast, QStringLiteral("12.0.0.1:6346"));
    gn->findFilter = [](const FileHashes &h) { return !h.sha1.isEmpty(); };
    env.engineUp();
    const QString id = env.engine->download(req(QStringLiteral("h.bin"), data, ed2k, {s1}));
    QTRY_VERIFY_WITH_TIMEOUT(!ov->hashLinkLookups.isEmpty(), 3000);
    QTest::qWait(500);
    QVERIFY(gn->source(QStringLiteral("gnut://fast")) == nullptr);   // nothing usable for Gnutella yet
    FileHashes all = ed2k;
    all.sha1 = sha1.sha1;
    ov->emitHashLinks(ed2k, all, data.size());
    // the new kind is passed to Gnutella immediately; its sources join (hinted, verified by eD2k)
    QTRY_VERIFY_WITH_TIMEOUT(gn->findCalls.size() >= 2 && gn->findCalls.last().first.sha1 == sha1.sha1, 3000);
    QVERIFY2(env.waitDone(id, 30000), qPrintable(env.info(id).statusText));
    QCOMPARE(readFile(env.info(id).savePath), data);
    QVERIFY(gn->source(QStringLiteral("gnut://fast")));
    QVERIFY(gn->source(QStringLiteral("gnut://fast"))->served > 0);
    QCOMPARE(env.info(id).hashes.sha1, sha1.sha1);
}

void TstEngine::dedupeJoin()
{
    Env env(QStringLiteral("dedupe"));
    auto *http = env.addPlugin(Net::Http, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(2 * 1024 * 1024, 17);
    SourceConfig c;
    c.data = data;
    c.bytesPerSec = 512 * 1024;
    const SearchSource a = http->addSource(QStringLiteral("http://a/d"), c);
    const SearchSource b = http->addSource(QStringLiteral("http://b/d"), c);
    env.engineUp();
    const QString id1 = env.engine->download(req(QStringLiteral("d.bin"), data, hashesOf(data, 1), {a}));
    FileHashes more = hashesOf(data, 1 | 2);
    const QString id2 = env.engine->download(req(QStringLiteral("d (copy).bin"), data, more, {b}));
    QCOMPARE(id2, id1);
    QCOMPARE(env.engine->downloads().size(), 1);
    QCOMPARE(env.info(id1).hashes.ed2k, more.ed2k);
    QCOMPARE(env.engine->knownSourceCount(), 2);
    QVERIFY(env.waitDone(id1, 20000));
    // once complete, the same request reports the finished download
    QCOMPARE(env.engine->download(req(QStringLiteral("d.bin"), data, hashesOf(data, 1), {a})), id1);
    QCOMPARE(env.engine->downloads().size(), 1);
}

void TstEngine::collisionNamingAndLibrary()
{
    Env env(QStringLiteral("collide"), /*library*/ true);
    env.settings.setValue(QString(keys::transfers::ShareCompleted), true);
    auto *http = env.addPlugin(Net::Http, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(1 << 20, 18);
    QDir().mkpath(env.root + QStringLiteral("/incoming"));
    {
        QFile f(env.root + QStringLiteral("/incoming/song.mp3"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("not the same file");
    }
    SourceConfig c;
    c.data = data;
    const SearchSource s = http->addSource(QStringLiteral("http://x/song.mp3"), c);
    env.engineUp();
    QSignalSpy hashed(env.library.get(), &Library::fileHashed);
    const QString id = env.engine->download(req(QStringLiteral("song.mp3"), data, hashesOf(data, 1), {s}));
    QVERIFY(env.waitDone(id, 20000));
    QCOMPARE(env.info(id).savePath, env.root + QStringLiteral("/incoming/song (1).mp3"));
    QCOMPARE(readFile(env.info(id).savePath), data);
    // handed to the library: hashed and findable by hash (shared)
    QTRY_VERIFY_WITH_TIMEOUT(hashed.size() >= 1, 10000);
    SharedFile f;
    QTRY_VERIFY_WITH_TIMEOUT(env.library->findByHash(hashesOf(data, 1), &f), 10000);
    QCOMPARE(f.path, env.info(id).savePath);
    // a new request for a file that is in the library is reported complete, not downloaded
    env.engine->clearCompleted();
    const QString again = env.engine->download(req(QStringLiteral("song.mp3"), data, hashesOf(data, 1), {s}));
    QCOMPARE(env.info(again).state, TransferState::Completed);
    QCOMPARE(env.info(again).savePath, f.path);
}

void TstEngine::diskErrorPauses()
{
    Env env(QStringLiteral("diskerr"));
    // the incomplete "directory" is a file → the part file cannot be created
    {
        QFile f(env.root + QStringLiteral("/blocked"));
        QVERIFY(f.open(QIODevice::WriteOnly));
    }
    env.settings.setValue(QString(keys::transfers::IncompleteDir), env.root + QStringLiteral("/blocked"));
    auto *http = env.addPlugin(Net::Http, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(1 << 20, 19);
    SourceConfig c;
    c.data = data;
    const SearchSource s = http->addSource(QStringLiteral("http://e/f"), c);
    env.engineUp();
    const QString id = env.engine->download(req(QStringLiteral("e.bin"), data, hashesOf(data, 1), {s}));
    QVERIFY(env.waitState(id, TransferState::Paused, 5000));
    QVERIFY(!env.info(id).errorText.isEmpty());
    QVERIFY(env.info(id).statusText.startsWith(QLatin1String("Paused (")));
    // fix the problem, resume → completes
    env.settings.setValue(QString(keys::transfers::IncompleteDir), env.root + QStringLiteral("/inc2"));
    env.engine->retry(id);
    QVERIFY2(env.waitDone(id, 20000), qPrintable(env.info(id).statusText));
}

void TstEngine::statusTexts()
{
    Env env(QStringLiteral("status"));
    auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges | NetworkPlugin::CanFindSources);
    const QByteArray data = randomBytes(1 << 20, 20);
    env.engineUp();
    // no sources: finding sources (and every CanFindSources plugin was asked)
    const QString id = env.engine->download(req(QStringLiteral("st.bin"), data, hashesOf(data, 2), {}));
    // [engine-4] the status says what discovery does: "Finding sources... - none known yet, asking ed2k"
    QTRY_VERIFY_WITH_TIMEOUT(env.info(id).statusText.startsWith(QLatin1String("Finding sources... - none known yet")), 3000);
    QVERIFY2(env.info(id).statusText.contains(QLatin1String("asking ed2k")), qPrintable(env.info(id).statusText));
    QVERIFY(ed->findSourcesCalls.contains(id));
    // a queued source: "Remotely Queued (7)"
    SourceConfig c;
    c.data = data;
    c.queuedMs = 1500;
    c.bytesPerSec = 256 * 1024;
    ed->emitSources(id, {ed->addSource(QStringLiteral("ed2k://q"), c, QStringLiteral("13.0.0.1:4662"))});
    QTRY_COMPARE_WITH_TIMEOUT(env.info(id).statusText, QStringLiteral("Remotely Queued (7)"), 3000);
    QTRY_COMPARE_WITH_TIMEOUT(env.info(id).state, TransferState::Transferring, 5000);
    const TransferInfo t = env.info(id);
    QVERIFY(t.rateDownBps > 0);
    QVERIFY(t.networks.contains(Net::ED2K));
    QCOMPARE(t.sources.size(), 1);
    QVERIFY(!t.pieceMap.isEmpty() && t.pieceMap.size() <= 512);
    QVERIFY(env.waitDone(id, 20000));
    QVERIFY(ed->stoppedFindSources.contains(id));
}

void TstEngine::partialSharing()
{
    Env env(QStringLiteral("partial"), /*library*/ true);
    auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges | NetworkPlugin::CanFindSources);
    const QByteArray data = randomBytes(3 * P, 21);
    ed->hashSetOnFind << qMakePair(P, ed2kParts(data));
    SourceConfig c;
    c.data = data;
    c.bytesPerSec = 4 << 20;
    c.avail = {{0, P + 5}};                 // only the first part exists anywhere: stays incomplete
    const SearchSource s = ed->addSource(QStringLiteral("ed2k://partial"), c, QStringLiteral("14.0.0.1:4662"));
    env.engineUp();
    const FileHashes h = hashesOf(data, 2);
    const QString id = env.engine->download(req(QStringLiteral("partial.bin"), data, h, {s}));
    QTRY_VERIFY_WITH_TIMEOUT(env.info(id).done >= P, 20000);
    // plugins (eD2k upload) can now find and read the verified part through LibraryAccess
    SharedFile f;
    QTRY_VERIFY_WITH_TIMEOUT(env.library->findByHash(h, &f), 5000);
    QCOMPARE(f.size, qint64(data.size()));
    QCOMPARE(env.library->read(h, 1000, 4096), data.mid(1000, 4096));
    QVERIFY(env.library->read(h, P + 10, 100).isEmpty());   // not verified → not served
    env.settings.setValue(QString(keys::library::ShareIncomplete), false);
    QTRY_VERIFY_WITH_TIMEOUT(!env.library->findByHash(h, &f), 5000);
}

void TstEngine::completionNotHashedTwice()
{
    Env env(QStringLiteral("hash-once"), /*library*/ true);
    env.settings.setValue(QString(keys::transfers::ShareCompleted), true);
    auto *http = env.addPlugin(Net::Http, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(12 * 1000 * 1000, 22);   // multi-part, no hash set: final pass
    SourceConfig c;
    c.data = data;
    const SearchSource s = http->addSource(QStringLiteral("http://once/big.bin"), c);
    env.engineUp();
    QVERIFY(env.library->waitIdle(5000));
    const int before = env.library->hashRuns();
    QSignalSpy hashed(env.library.get(), &Library::fileHashed);
    const QString id = env.engine->download(req(QStringLiteral("big.bin"), data, hashesOf(data, 1), {s}));
    QVERIFY(env.waitDone(id, 30000));
    QTRY_VERIFY_WITH_TIMEOUT(hashed.size() >= 1, 10000);
    SharedFile f;
    QVERIFY(env.library->findByHash(hashesOf(data, 2), &f));           // indexed with all kinds
    QCOMPARE(f.hashes.tth.size(), 24);
    QVERIFY(!env.library->ed2kPartHashes(f.hashes).isEmpty());
    // a full rescan of the (shared) incoming dir hits the cache as well
    env.library->rescan();
    QVERIFY(env.library->waitIdle(10000));
    QTest::qWait(300);
    qInfo("library hash runs: before %d, after completion + rescan %d", before, env.library->hashRuns());
    QCOMPARE(env.library->hashRuns(), before);
    QVERIFY(env.library->findByHash(hashesOf(data, 1), &f));
}

void TstEngine::allSourcesFakeEndsInError()
{
    Env env(QStringLiteral("all-fake"));
    env.settings.setValue(QString(keys::transfers::AutoRetry), true);
    env.settings.setValue(QString(keys::transfers::RetryIntervalSec), 5);
    auto *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(2 * 1000 * 1000, 23);        // single eD2k part: fine unit
    SourceConfig fake;
    fake.data = randomBytes(2 * 1000 * 1000, 24);                    // "bridge" entry: some other file
    const SearchSource s = ed->addSource(QStringLiteral("ed2k://fake"), fake, QStringLiteral("40.0.0.1:4662"));
    env.engineUp();
    const QString id = env.engine->download(req(QStringLiteral("fake.iso"), data, hashesOf(data, 2), {s}));
    QVERIFY2(env.waitState(id, TransferState::Error, 20000), qPrintable(env.info(id).statusText));
    qInfo("status: %s", qPrintable(env.info(id).statusText));
    QVERIFY(env.info(id).statusText.startsWith(QLatin1String("File Mismatch")));
    QTest::qWait(7000);                                               // auto-retry must not revive it
    QCOMPARE(env.info(id).state, TransferState::Error);
    // a restart keeps the verdict
    env.engineDown(true);
    env.engineUp();
    QTest::qWait(1500);
    QCOMPARE(env.info(id).state, TransferState::Error);
}

void TstEngine::unverifiedSourcesExplained()
{
    Env env(QStringLiteral("unverified"));
    auto *gn = env.addPlugin(Net::Gnutella, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(12 * 1000 * 1000, 25);
    SourceConfig c;
    c.data = data;
    SearchSource sc = gn->addSource(QStringLiteral("gnut://cand"), c, QStringLiteral("41.0.0.1:6346"));
    sc.extra.insert(QStringLiteral("candidate"), true);
    sc.extra.insert(QStringLiteral("hashes"), hashesOf(data, 2).toVariant());
    env.engineUp();
    // whole-file identity only (SHA-1): the candidate's data could not be checked before the end
    const QString id = env.engine->download(req(QStringLiteral("u.bin"), data, hashesOf(data, 1), {sc}));
    QTRY_VERIFY_WITH_TIMEOUT(env.info(id).statusText.startsWith(
                                 QLatin1String("Finding sources... (1 unverified source wait for a part hash set) - 1 known (Gnut 1)")), 3000);
    const TransferInfo t = env.info(id);
    QCOMPARE(t.sources.size(), 1);
    QCOMPARE(t.sources.first().statusText, QStringLiteral("Not used yet: unverified (needs a part hash set)"));
    QVERIFY(gn->stats.isEmpty());                                     // never opened
}

QTEST_GUILESS_MAIN(TstEngine)
#include "tst_engine.moc"

// ============================================================================ [engine-3]
// A trusted ed2k identity; the part hash set comes from the SOURCES (as from the overlay), no
// network-level hashSetLearned. carol corrupts part 1: only that part is fetched again, carol is
// proven guilty by the black box and banned, alice keeps no strike.
void TstEngine::sourceHashSetTrustedBlackBox()
{
    Env env(QStringLiteral("src-hs-trusted"));
    auto *ov = env.addPlugin(Net::Overlay, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(4 * P + 12345, 41);
    SourceConfig a, c;
    a.data = c.data = data;
    a.bytesPerSec = c.bytesPerSec = 8 << 20;
    a.pref = c.pref = 1 << 20;
    a.hashSet = c.hashSet = ed2kParts(data);
    c.corrupt = {{P, 2 * P}};   // every byte carol serves in part 1 is bad
    const SearchSource sa = ov->addSource(QStringLiteral("ovl://alice"), a, QStringLiteral("alice"));
    const SearchSource sc = ov->addSource(QStringLiteral("ovl://carol"), c, QStringLiteral("carol"));
    env.engineUp();
    const QString id = env.engine->download(req(QStringLiteral("hs.bin"), data, hashesOf(data, 1 | 2), {sa, sc}));
    QVERIFY2(env.waitDone(id, 60000), qPrintable(env.info(id).statusText));
    QCOMPARE(readFile(env.info(id).savePath), data);
    const QVariantMap dbg = env.engine->debugInfo(id);
    const qint64 total = ov->served(QStringLiteral("ovl://alice")) + ov->served(QStringLiteral("ovl://carol"));
    const SrcInfo al = srcInfo(dbg, QStringLiteral("alice")), ca = srcInfo(dbg, QStringLiteral("carol"));
    qInfo("served %lld for %lld bytes (+%lld); unit failures %d, failed bytes %lld; carol banned %d, alice strikes %d; set from %s",
          total, qint64(data.size()), total - data.size(), dbg.value(QStringLiteral("unitFailures")).toInt(),
          dbg.value(QStringLiteral("failedUnitBytes")).toLongLong(), ca.banned, al.strikes,
          qPrintable(dbg.value(QStringLiteral("hashSetProvider")).toString()));
    QVERIFY(!dbg.value(QStringLiteral("hashSetProvider")).toString().isEmpty());
    QVERIFY(!dbg.value(QStringLiteral("hashSetProvisional")).toBool());
    QVERIFY(dbg.value(QStringLiteral("unitFailures")).toInt() >= 1);
    QVERIFY(ca.banned);
    QVERIFY(!al.banned);
    QCOMPARE(al.strikes, 0);
    // the bad part twice (once more from each of the two), and at most a request that was on its way
    QVERIFY2(total <= data.size() + 2 * P + a.pref, "more than the bad part was fetched again");
    QVERIFY(dbg.value(QStringLiteral("failedUnitBytes")).toLongLong() <= 2 * P);
}

// Only a SHA-1 is trusted (no ed2k hash): the sources' set is used provisionally, still locates the
// bad part, and the final SHA-1 check confirms it.
void TstEngine::sourceHashSetProvisionalCorrupt()
{
    Env env(QStringLiteral("src-hs-prov"));
    auto *ov = env.addPlugin(Net::Overlay, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(3 * P + 999, 42);
    SourceConfig a, c;
    a.data = c.data = data;
    a.bytesPerSec = c.bytesPerSec = 8 << 20;
    a.pref = c.pref = 1 << 20;
    a.hashSet = c.hashSet = ed2kParts(data);
    c.corrupt = {{2 * P, 3 * P}};
    const SearchSource sa = ov->addSource(QStringLiteral("ovl://alice"), a, QStringLiteral("alice"));
    const SearchSource sc = ov->addSource(QStringLiteral("ovl://carol"), c, QStringLiteral("carol"));
    env.engineUp();
    const QString id = env.engine->download(req(QStringLiteral("hp.bin"), data, hashesOf(data, 1), {sa, sc}));
    QVERIFY2(env.waitDone(id, 60000), qPrintable(env.info(id).statusText));
    QCOMPARE(readFile(env.info(id).savePath), data);
    const QVariantMap dbg = env.engine->debugInfo(id);
    const qint64 total = ov->served(QStringLiteral("ovl://alice")) + ov->served(QStringLiteral("ovl://carol"));
    qInfo("provisional: served %lld for %lld bytes; unit failures %d; whole-file failures %d", total, qint64(data.size()),
          dbg.value(QStringLiteral("unitFailures")).toInt(), dbg.value(QStringLiteral("wholeFileFailures")).toInt());
    QVERIFY(dbg.value(QStringLiteral("unitFailures")).toInt() >= 1);
    QCOMPARE(dbg.value(QStringLiteral("wholeFileFailures")).toInt(), 0);
    QVERIFY(srcInfo(dbg, QStringLiteral("carol")).banned);
    QVERIFY(!srcInfo(dbg, QStringLiteral("alice")).banned);
    // the bad part twice (once more from each of the two), and at most a request that was on its way
    QVERIFY2(total <= data.size() + 2 * P + a.pref, qPrintable(QString::number(total)));
    QCOMPARE(env.info(id).hashes.ed2k, hashesOf(data, 2).ed2k);   // learned from the final pass
}

// Only a SHA-1 is trusted; mallory serves ANOTHER file of the same size together with that file's
// (self-consistent) part hash set. Every part matches the provisional set, the final SHA-1 does
// not: the set is dropped, mallory banned, and the good source (queued until then) completes.
void TstEngine::sourceHashSetProvisionalRefuted()
{
    Env env(QStringLiteral("src-hs-refuted"));
    auto *ov = env.addPlugin(Net::Overlay, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(2 * P + 777, 43);
    const QByteArray other = randomBytes(data.size(), 44);
    SourceConfig m, g;
    m.data = other;
    m.hashSet = ed2kParts(other);
    m.pref = g.pref = 1 << 20;
    g.data = data;
    g.queuedMs = 4000;
    const SearchSource sm = ov->addSource(QStringLiteral("ovl://mallory"), m, QStringLiteral("mallory"));
    const SearchSource sg = ov->addSource(QStringLiteral("ovl://good"), g, QStringLiteral("good"));
    env.engineUp();
    const QString id = env.engine->download(req(QStringLiteral("hr.bin"), data, hashesOf(data, 1), {sm, sg}));
    QVERIFY2(env.waitDone(id, 60000), qPrintable(env.info(id).statusText));
    QCOMPARE(readFile(env.info(id).savePath), data);
    const QVariantMap dbg = env.engine->debugInfo(id);
    qInfo("refuted: whole-file failures %d, mallory banned %d, provisional now %d", dbg.value(QStringLiteral("wholeFileFailures")).toInt(),
          srcInfo(dbg, QStringLiteral("mallory")).banned, dbg.value(QStringLiteral("hashSetProvisional")).toBool());
    QCOMPARE(dbg.value(QStringLiteral("wholeFileFailures")).toInt(), 1);
    QVERIFY(srcInfo(dbg, QStringLiteral("mallory")).banned);
    QVERIFY(!srcInfo(dbg, QStringLiteral("good")).banned);
    QVERIFY(!dbg.value(QStringLiteral("hashSetProvisional")).toBool());
}

// A trusted ed2k identity; "liar" sends a set that does not reduce to it, but right bytes (a 5.2
// node sharing a file it is still downloading did exactly that). [up-1] The set is dropped and never
// asked for again, the source is NOT banned for it and its bytes are used (they verify).
void TstEngine::sourceHashSetWrongRootIgnored()
{
    Env env(QStringLiteral("src-hs-wrong"));
    auto *ov = env.addPlugin(Net::Overlay, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(2 * P + 5, 45);
    SourceConfig l, g;
    l.data = g.data = data;
    l.hashSet = ed2kParts(randomBytes(data.size(), 46));
    l.bytesPerSec = g.bytesPerSec = 4 << 20;
    g.hashSet = ed2kParts(data);
    g.queuedMs = 1500;   // the liar's set comes first
    const SearchSource sl = ov->addSource(QStringLiteral("ovl://liar"), l, QStringLiteral("liar"));
    const SearchSource sg = ov->addSource(QStringLiteral("ovl://good"), g, QStringLiteral("good"));
    env.engineUp();
    const QString id = env.engine->download(req(QStringLiteral("hw.bin"), data, hashesOf(data, 2), {sl, sg}));
    QVERIFY2(env.waitDone(id, 60000), qPrintable(env.info(id).statusText));
    QCOMPARE(readFile(env.info(id).savePath), data);
    const QVariantMap dbg = env.engine->debugInfo(id);
    const SrcInfo li = srcInfo(dbg, QStringLiteral("liar"));
    qInfo("wrong set: liar banned %d strikes %d served %lld; unit failures %d", li.banned, li.strikes,
          ov->served(QStringLiteral("ovl://liar")), dbg.value(QStringLiteral("unitFailures")).toInt());
    QVERIFY(!li.banned);
    QCOMPARE(li.strikes, 0);
    QVERIFY(!srcInfo(dbg, QStringLiteral("good")).banned);
    QVERIFY(ov->served(QStringLiteral("ovl://liar")) > 0);   // still used
    QCOMPARE(dbg.value(QStringLiteral("unitFailures")).toInt(), 0);
    QVERIFY(!dbg.value(QStringLiteral("hashSetProvider")).toString().contains(QLatin1String("liar")));
}
