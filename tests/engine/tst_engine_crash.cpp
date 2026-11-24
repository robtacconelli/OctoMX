// SPDX-License-Identifier: GPL-3.0-or-later
// [safe-1] No download is corrupted by closing, a kill or a power cut: crash_child downloads one
// file on a persistent profile and is stopped in the middle, again and again, in one way per test:
//   quit      the safe quit at a random moment (what the menu, the window, the tray and the update
//             install run: DownloadEngine::shutdown, then the program ends)
//   slowquit  the same while the disk is slow (MX_TEST_SLOW_WRITE_MS): writes are still pending
//   closenow  the same, and "Close now" 200 ms into the wait: what is not saved yet is not claimed
//   term      SIGTERM at a random moment (TerminateProcess on Windows: the same as kill)
//   kill      SIGKILL / TerminateProcess at a random moment
//   cut       a safe quit, then a power cut in the next run (and so on): what was written since the
//             last sync to the disk, in this run or the one before, is zeroed, then SIGKILL
//             (mx/durable_file.h; the database keeps every commit, the worst case for a record)
//   lie       a power cut every run on a disk that acknowledges syncs it did not do: the record is ahead
//             of the disk on purpose; the check after the unclean stop (Verify File) must catch it
//   movekill  (whole) the completion copy to the incoming folder (another volume) is killed
// on each store: ed2k (part file, eD2k part hashes), tth (part file, tiger tree leaves), whole
// (part file without block hashes: OctoMX / HTTP, the whole file checked at the end), torrent
// (libtorrent, a local seed) and merged (a torrent fed by a BitTorrent peer and an HTTP mirror).
// Each ends with "the finished file has the right hash". Sizes and rounds: MX_CRASH_SIZE_MB,
// MX_CRASH_ROUNDS (the defaults fit the ctest timeout; the kill loops run at least 50 rounds).
#include <QtCore/QProcess>
#include <QtCore/QRandomGenerator>
#include <QtTest/QTest>

#include <algorithm>

#include "bt_node.h"
#include "engine_env.h"

using namespace mx;
using namespace mx::etest;

namespace {
struct Outcome {
    int rounds = 0;            // stops before the download finished
    bool done = false;
    QByteArray sha1;
    QString error, path, log, doneLine;
};

struct Child {
    QProcess p;
    QStringList lines;
    QString buf;
    bool ready = false, done = false, cut = false;
    QString doneLine, errorLine, lastProgress;
    void start(const QString &exe, const QStringList &args, const QProcessEnvironment &env)
    {
        p.setProcessEnvironment(env);
        p.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        QObject::connect(&p, &QProcess::readyReadStandardOutput, &p, [this] {
            buf += QString::fromUtf8(p.readAllStandardOutput());
            int nl;
            while ((nl = int(buf.indexOf(QLatin1Char('\n')))) >= 0) {
                const QString l = buf.left(nl).trimmed();
                buf.remove(0, nl + 1);
                if (l == QLatin1String("READY")) ready = true;
                else if (l.startsWith(QLatin1String("DONE "))) { done = true; doneLine = l; }
                else if (l.startsWith(QLatin1String("ERROR "))) errorLine = l;
                else if (l.startsWith(QLatin1String("CUT "))) cut = true;
                else if (l.startsWith(QLatin1String("P "))) { lastProgress = l; continue; }
                lines << l;
            }
        });
        p.start(exe, args);
    }
};

int envInt(const char *name, int def)
{
    bool ok = false;
    const int v = qEnvironmentVariableIntValue(name, &ok);
    return ok && v > 0 ? v : def;
}
} // namespace

class TstEngineCrash : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanupTestCase();
    void quit_data() { stores(); }
    void quit() { run(QStringLiteral("quit"), 6); }
    void slowquit_data() { stores(); }
    void slowquit() { run(QStringLiteral("slowquit"), 4); }
    void closenow_data() { stores(); }
    void closenow() { run(QStringLiteral("closenow"), 4); }
    void term_data() { stores(); }
    void term() { run(QStringLiteral("term"), 6); }
    void kill_data() { stores(); }
    void kill() { run(QStringLiteral("kill"), 50); }
    void cut_data() { stores(); }
    void cut() { run(QStringLiteral("cut"), 15); }
    void lie_data() { stores(); }
    void lie() { run(QStringLiteral("lie"), 12); }
    void movekill();
    void throughput();

private:
    void stores()
    {
        QTest::addColumn<QString>("store");
        for (const char *s : {"ed2k", "tth", "whole", "torrent", "merged"}) QTest::newRow(s) << QString::fromLatin1(s);
    }
    void run(const QString &method, int defRounds);
    Outcome scenario(const QString &store, const QString &method, int rounds, qint64 size, int rate, const QString &name);
    bool seed(qint64 size);

    std::unique_ptr<BtNode> m_seeder;
    QString m_root, m_torrentFile;
    qint64 m_seedSize = 0;
    quint32 m_seed = 4711;
    QRandomGenerator m_rng{20261124};
};

void TstEngineCrash::initTestCase()
{
    m_root = testRoot() + QStringLiteral("/crash-%1").arg(QCoreApplication::applicationPid());   // ctest runs several at once
    QDir(m_root).removeRecursively();
    QDir().mkpath(m_root);
    QVERIFY(QFileInfo::exists(QCoreApplication::applicationDirPath() + QStringLiteral("/crash_child")) ||
            QFileInfo::exists(QCoreApplication::applicationDirPath() + QStringLiteral("/crash_child.exe")));
}

void TstEngineCrash::cleanupTestCase()
{
    m_seeder.reset();
    QDir(m_root).removeRecursively();
}

bool TstEngineCrash::seed(qint64 size)
{
    if (m_seeder && m_seedSize == size) return true;
    m_seeder.reset();
    const QString src = m_root + QStringLiteral("/seed/payload.bin");
    QDir().mkpath(QFileInfo(src).absolutePath());
    {
        QFile f(src);
        if (!f.open(QIODevice::WriteOnly)) return false;
        f.write(randomBytes(size, m_seed));
    }
    m_seeder = std::make_unique<BtNode>(m_root + QStringLiteral("/seeder"));
    if (!m_seeder->start()) return false;
    QSignalSpy ready(m_seeder->ev, &BitTorrentEvents::seedReady);
    QString err;
    if (!m_seeder->be->seedLocalFile(src, &err)) return false;
    if (!QTest::qWaitFor([&] { return !ready.isEmpty(); }, 120000)) return false;
    const QString tid = ready.first().at(1).toString();
    m_torrentFile = m_root + QStringLiteral("/seeder/bt/torrents/") + tid + QStringLiteral(".torrent");
    m_seedSize = size;
    return QTest::qWaitFor([&] { return QFileInfo(m_torrentFile).size() > 0; }, 10000);
}

void TstEngineCrash::run(const QString &method, int defRounds)
{
    QFETCH(QString, store);
    const int rounds = envInt("MX_CRASH_ROUNDS", defRounds);
    const qint64 size = qint64(envInt("MX_CRASH_SIZE_MB", 40)) << 20;
    // the download must outlast the rounds: about 1.2 s of downloading per round
    const int rate = int(qBound<qint64>(qint64(256 * 1024), size / (qint64(rounds) * 6 / 5 + 4), qint64(64 << 20)));
    const Outcome o = scenario(store, method, rounds, size, rate, method + QLatin1Char('-') + store);
    const QByteArray want = QCryptographicHash::hash(randomBytes(size, m_seed), QCryptographicHash::Sha1);
    const bool good = o.done && o.sha1 == want;
    qInfo("RESULT %s %s: %d stops before it finished, %s%s", qPrintable(method), qPrintable(store), o.rounds,
          good ? "the finished file has the right hash" : o.done ? "the finished file is CORRUPT" : "not finished: ",
          qPrintable(o.error));
    QVERIFY2(o.done, qPrintable(o.error + QLatin1Char(' ') + o.log.right(1500)));
    QVERIFY2(good, qPrintable(QStringLiteral("sha1 %1, expected %2; %3").arg(QString::fromLatin1(o.sha1.toHex()), QString::fromLatin1(want.toHex()), o.log.right(1500))));
}

Outcome TstEngineCrash::scenario(const QString &store, const QString &method, int rounds, qint64 size, int rate, const QString &name)
{
    Outcome o;
    const bool torrent = store == QLatin1String("torrent") || store == QLatin1String("merged");
    if (torrent && !seed(size)) {
        o.error = QStringLiteral("the local seed did not start");
        return o;
    }
    const QString root = m_root + QLatin1Char('/') + name;
    QDir(root).removeRecursively();
    QDir().mkpath(root);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    int fastRate = rate;
    if (method == QLatin1String("slowquit") || method == QLatin1String("closenow")) {
        env.insert(QStringLiteral("MX_TEST_SLOW_WRITE_MS"), QStringLiteral("25"));
        fastRate = rate * 4;   // the source is faster than the disk: writes pile up
    }
    if (method == QLatin1String("cut") || method == QLatin1String("lie")) env.insert(QStringLiteral("MX_TEST_POWERCUT"), QStringLiteral("1"));
    if (method == QLatin1String("lie")) env.insert(QStringLiteral("MX_TEST_POWERCUT_LIE"), QStringLiteral("1"));
    const QString exe = QCoreApplication::applicationDirPath() + QStringLiteral("/crash_child");
    QStringList base = {QStringLiteral("--root"), root, QStringLiteral("--store"), store, QStringLiteral("--size"), QString::number(size),
                        QStringLiteral("--seed"), QString::number(m_seed), QStringLiteral("--rate"), QString::number(fastRate)};
    if (torrent) base << QStringLiteral("--peer") << m_seeder->endpoint() << QStringLiteral("--torrent") << m_torrentFile;
    QStringList log;
    for (int round = 1; round <= rounds && !o.done; ++round) {
        const int alive = 300 + int(m_rng.bounded(1900));
        QStringList args = base;
        if (method == QLatin1String("quit") || method == QLatin1String("slowquit"))
            args << QStringLiteral("--stop") << QStringLiteral("quit") << QStringLiteral("--after") << QString::number(alive);
        else if (method == QLatin1String("closenow"))
            args << QStringLiteral("--stop") << QStringLiteral("closenow") << QStringLiteral("--after") << QString::number(alive);
        else if (method == QLatin1String("lie"))   // a power cut every run, on a disk that lies about its syncs
            args << QStringLiteral("--stop") << QStringLiteral("cut") << QStringLiteral("--after") << QString::number(alive);
        else if (method == QLatin1String("cut"))   // a quit, then a power cut in the next run, and so on
            args << QStringLiteral("--stop") << (round % 2 ? QStringLiteral("quit") : QStringLiteral("cut")) << QStringLiteral("--after")
                 << QString::number(alive);
        Child c;
        c.start(exe, args, env);
        if (!c.p.waitForStarted(10000)) { o.error = QStringLiteral("crash_child did not start"); return o; }
        (void)QTest::qWaitFor([&] { return c.ready || c.p.state() == QProcess::NotRunning; }, 30000);
        if (method == QLatin1String("kill") || method == QLatin1String("term")) {
            (void)QTest::qWaitFor([&] { return c.done || c.p.state() == QProcess::NotRunning; }, alive);
            if (c.p.state() != QProcess::NotRunning) {
#ifdef Q_OS_WIN
                c.p.kill();   // Windows has no SIGTERM for a console program: TerminateProcess, this child only
#else
                if (method == QLatin1String("kill")) c.p.kill();   // SIGKILL, this child only
                else c.p.terminate();                              // SIGTERM
#endif
            }
        }
        if (!c.p.waitForFinished(method == QLatin1String("slowquit") ? 120000 : 60000)) {
            c.p.kill();
            c.p.waitForFinished(5000);
            log << QStringLiteral("round %1: did not end, killed").arg(round);
        }
        QCoreApplication::processEvents();
        log << QStringLiteral("round %1 (%2 ms): exit %3 %4; %5 %6").arg(round).arg(alive).arg(c.p.exitCode())
                   .arg(c.p.exitStatus() == QProcess::CrashExit ? QStringLiteral("killed") : QStringLiteral("ended"), c.lastProgress,
                        c.lines.join(QLatin1String(" | ")).right(300));
        qInfo("%s", qPrintable(log.last().left(400)));
        if (!c.errorLine.isEmpty()) { o.error = c.errorLine; o.log = log.join(QLatin1Char('\n')); return o; }
        if (c.done) { o.done = true; o.doneLine = c.doneLine; break; }
        o.rounds = round;
    }
    if (!o.done) {   // the last start: let it finish
        Child c;
        c.start(exe, base, env);
        c.p.waitForStarted(10000);
        const int ms = int(qMin<qint64>(600000, 60000 + 3 * 1000 * size / qMax(1, rate)));
        (void)QTest::qWaitFor([&] { return c.done || !c.errorLine.isEmpty() || c.p.state() == QProcess::NotRunning; }, ms);
        if (c.p.state() != QProcess::NotRunning && !c.p.waitForFinished(60000)) c.p.kill();
        c.p.waitForFinished(5000);
        QCoreApplication::processEvents();
        log << QStringLiteral("last start: exit %1; %2 %3").arg(c.p.exitCode()).arg(c.lastProgress, c.lines.join(QLatin1String(" | ")).right(300));
        qInfo("%s", qPrintable(log.last().left(400)));
        if (!c.errorLine.isEmpty()) o.error = c.errorLine;
        o.done = c.done;
        o.doneLine = c.doneLine;
        if (!o.done && o.error.isEmpty()) o.error = QStringLiteral("not finished in time: %1").arg(c.lastProgress);
    }
    if (o.done) {
        const QStringList f = o.doneLine.split(QLatin1Char(' '));
        o.sha1 = QByteArray::fromHex(f.value(1).toLatin1());
        o.path = f.mid(2).join(QLatin1Char(' '));
    }
    o.log = log.join(QLatin1Char('\n'));
    return o;
}

// [safe-1] the completion copy to the incoming folder on another volume (MX_TEST_COPY_MOVE) is
// killed in the middle: the incoming folder must never hold a half file under the download's name
void TstEngineCrash::movekill()
{
    const qint64 size = qint64(envInt("MX_CRASH_SIZE_MB", 40)) << 20;
    const QString root = m_root + QStringLiteral("/movekill");
    QDir(root).removeRecursively();
    QDir().mkpath(root);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("MX_TEST_COPY_MOVE"), QStringLiteral("1"));
    const QString exe = QCoreApplication::applicationDirPath() + QStringLiteral("/crash_child");
    const QStringList args = {QStringLiteral("--root"), root, QStringLiteral("--store"), QStringLiteral("whole"), QStringLiteral("--size"),
                              QString::number(size), QStringLiteral("--seed"), QString::number(m_seed), QStringLiteral("--rate"),
                              QString::number(64 << 20)};
    {
        QProcessEnvironment slow = env;
        slow.insert(QStringLiteral("MX_TEST_SLOW_WRITE_MS"), QStringLiteral("400"));   // 400 ms per 4 MB copied, ~0 per write
        Child c;
        c.start(exe, args, slow);
        QVERIFY(c.p.waitForStarted(10000));
        QVERIFY2(QTest::qWaitFor([&] { return c.lastProgress.contains(QLatin1String("Moving")) || c.done; }, 300000), qPrintable(c.lastProgress));
        QTest::qWait(1000);   // a part of the copy is written
        c.p.kill();
        c.p.waitForFinished(10000);
        QVERIFY2(!c.done, "the copy finished before the kill");
    }
    const QString target = root + QStringLiteral("/incoming/payload.bin");
    qInfo("after the kill: %s %s", QFileInfo::exists(target) ? "incoming/payload.bin exists," : "no incoming/payload.bin",
          QFileInfo::exists(target) ? qPrintable(QStringLiteral("%1 of %2 bytes").arg(QFileInfo(target).size()).arg(size)) : "");
    const bool halfFile = QFileInfo::exists(target) && QFileInfo(target).size() != size;
    Child c;
    c.start(exe, args, env);
    QVERIFY(c.p.waitForStarted(10000));
    QVERIFY2(QTest::qWaitFor([&] { return c.done || !c.errorLine.isEmpty() || c.p.state() == QProcess::NotRunning; }, 300000), qPrintable(c.lastProgress));
    c.p.waitForFinished(60000);
    const QByteArray want = QCryptographicHash::hash(randomBytes(size, m_seed), QCryptographicHash::Sha1);
    const QStringList f = c.doneLine.split(QLatin1Char(' '));
    QStringList names = QDir(root + QStringLiteral("/incoming")).entryList(QDir::Files | QDir::Hidden);
    qInfo("RESULT movekill whole: half file under the name after the kill: %s; the incoming folder now: %s; finished at %s",
          halfFile ? "YES" : "no", qPrintable(names.join(QStringLiteral(", "))), qPrintable(f.mid(2).join(QLatin1Char(' '))));
    QVERIFY2(c.done, qPrintable(c.errorLine + c.lastProgress));
    QCOMPARE(QByteArray::fromHex(f.value(1).toLatin1()), want);
    QVERIFY2(!halfFile, "a half copied file had the download's name");
    QCOMPARE(names, QStringList{QStringLiteral("payload.bin")});
    QCOMPARE(QCryptographicHash::hash(readFile(target), QCryptographicHash::Sha1), want);
}

// [safe-1] what the sync schedule costs: a part-file download from a local source as fast as the
// event loop delivers, in process, timed until the file is complete in the incoming folder
// (MX_BENCH_SIZE_MB, default 256; 3 runs, the median is reported)
void TstEngineCrash::throughput()
{
    const qint64 size = qint64(envInt("MX_BENCH_SIZE_MB", 512)) << 20;
    const int rateMb = envInt("MX_BENCH_RATE_MB", 100);   // a 1 Gbit/s link; 0 = as fast as the event loop
    const QByteArray data = randomBytes(size, 77);
    QList<double> link, total;
    for (int run = 0; run < 3; ++run) {
        Env env(QStringLiteral("bench"));
        auto *ov = env.addPlugin(Net::Overlay, NetworkPlugin::CanFetchRanges);
        SourceConfig c;
        c.data = data;
        c.bytesPerSec = rateMb > 0 ? rateMb << 20 : 0;
        c.chunk = 1 << 20;
        c.maxPending = 8;
        c.pref = 4 << 20;
        const SearchSource s = ov->addSource(QStringLiteral("octomx://bench"), c, QStringLiteral("30.0.0.3:4700"));
        env.engineUp();
        DownloadRequest r;
        r.name = QStringLiteral("bench.bin");
        r.size = size;
        r.hashes = hashesOf(data, 1);
        r.sources = {s};
        QElapsedTimer t;
        t.start();
        const QString id = env.engine->download(r);
        QVERIFY(QTest::qWaitFor([&] { return env.engine->debugInfo(id).value(QStringLiteral("writtenBytes")).toLongLong() >= size
                                             || env.info(id).state == TransferState::Verifying || env.info(id).state == TransferState::Completed; }, 300000));
        const double linkSecs = t.elapsed() / 1000.0;
        // then until the file is complete in the incoming folder: synced (after safe-1), checked, moved
        QVERIFY(env.waitDone(id, 300000));
        const double secs = t.elapsed() / 1000.0;
        const QVariantMap dbg = env.engine->debugInfo(id);
        link << double(size) / (1 << 20) / linkSecs;
        total << double(size) / (1 << 20) / secs;
        qInfo("throughput run %d: %lld MB written in %.2f s = %.1f MB/s, complete after %.2f s = %.1f MB/s", run + 1, size >> 20, linkSecs,
              link.last(), secs, total.last());
        env.engineDown(true);
        QDir(env.root).removeRecursively();
    }
    std::sort(link.begin(), link.end());
    std::sort(total.begin(), total.end());
    qInfo("RESULT throughput (source %d MB/s, %lld MB): written %.1f MB/s, complete %.1f MB/s (medians of 3)", rateMb, size >> 20, link.at(1),
          total.at(1));
}

QTEST_GUILESS_MAIN(TstEngineCrash)
#include "tst_engine_crash.moc"
