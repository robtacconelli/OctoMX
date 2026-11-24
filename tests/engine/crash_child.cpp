// SPDX-License-Identifier: GPL-3.0-or-later
// [safe-1] crash_child: one download on a persistent profile, stopped by its parent
// (tst_engine_crash) in the middle in one of the ways a real program is stopped, started again,
// until the file is finished. The data comes from local fake sources (part-file stores) or a local
// libtorrent swarm (torrent stores); the program is the real DownloadEngine on a real database.
//
//   crash_child --root DIR --store ed2k|tth|whole|torrent|merged --size N --seed S --rate BPS
//               [--peer HOST:PORT --torrent FILE] [--stop none|quit|closenow|cut --after MS]
//
// Lines on stdout: "READY", "P <done> <size> <state> <status>", "DONE <sha1 hex> <path>",
// "ERROR <status>", "QUIT <ms>", "CUT <bytes zeroed>". SIGTERM / SIGINT / SIGHUP run the safe quit
// through mx::QuitSignals (the program's own handler) unless MX_CRASH_NO_SIGNALS=1 (what a process
// without a handler does). MX_TEST_SLOW_WRITE_MS / MX_TEST_POWERCUT: see mx/durable_file.h.
#include <QtCore/QCommandLineParser>
#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QElapsedTimer>
#include <QtCore/QTimer>

#include <csignal>
#include <cstdio>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include "bt_node.h"
#include "core/database.h"
#include "core/engine/engine.h"
#include "core/upload_tracker.h"
#include "engine_env.h"
#include "mx/durable_file.h"
#include "mx/quit_signals.h"
#include "mx/settings_keys.h"
#include "mx/simple_settings.h"
#include "mxhash/ed2k.h"
#include "mxhash/tiger_tree.h"

using namespace mx;
using namespace mx::etest;

namespace {
void say(const QString &s)
{
    const QByteArray b = s.toUtf8() + '\n';
    fwrite(b.constData(), 1, size_t(b.size()), stdout);
    fflush(stdout);
}

QByteArray sha1OfFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash h(QCryptographicHash::Sha1);
    h.addData(&f);
    return h.result();
}

QList<QByteArray> tthLeaves(const QByteArray &data, qint64 unit)
{
    QList<QByteArray> out;
    for (qint64 off = 0; off < data.size(); off += unit) {
        hash::TigerTree t;
        t.update(data.mid(int(off), int(qMin<qint64>(unit, data.size() - off))));
        out << t.finish();
    }
    return out;
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCommandLineParser cl;
    for (const char *o : {"root", "store", "size", "seed", "rate", "peer", "torrent", "stop", "after"})
        cl.addOption(QCommandLineOption(QString::fromLatin1(o), QString(), QStringLiteral("v")));
    cl.process(app);
    const QString root = cl.value(QStringLiteral("root"));
    const QString store = cl.value(QStringLiteral("store"));
    const qint64 size = cl.value(QStringLiteral("size")).toLongLong();
    const quint32 seed = cl.value(QStringLiteral("seed")).toUInt();
    const int rate = cl.value(QStringLiteral("rate")).toInt();
    const QString stop = cl.value(QStringLiteral("stop"));
    const int after = cl.value(QStringLiteral("after")).toInt();
    const bool torrent = store == QLatin1String("torrent") || store == QLatin1String("merged");
    if (root.isEmpty() || size <= 0) return 2;

    SimpleSettings settings;
    settings.setValue(QString(keys::transfers::IncomingDir), root + QStringLiteral("/incoming"));
    settings.setValue(QString(keys::transfers::IncompleteDir), root + QStringLiteral("/incomplete"));
    settings.setValue(QString(keys::transfers::MinFreeSpaceMB), 0);
    Database db;
    QString err;
    if (!db.open(root + QStringLiteral("/octomx.db"), &err)) {
        say(QStringLiteral("ERROR database: %1").arg(err));
        return 3;
    }
    UploadTracker uploads(&settings);
    const QString journalFile = root + QStringLiteral("/powercut.journal");
    durable::loadJournal(journalFile);   // what earlier runs wrote and never synced (MX_TEST_POWERCUT)

    const QByteArray data = (store == QLatin1String("torrent")) ? QByteArray() : randomBytes(size, seed);
    QList<NetworkPlugin *> plugins;
    DownloadRequest req;
    req.name = QStringLiteral("payload.bin");
    req.size = size;
    SourceConfig sc;
    sc.data = data;
    sc.bytesPerSec = rate;
    sc.maxPending = 4;
    sc.chunk = qBound(16 * 1024, rate / 25, 1 << 20);   // the fake source sends a chunk every 20 ms
    sc.pref = 1 << 20;
    std::unique_ptr<BtNode> node;
    if (store == QLatin1String("ed2k")) {
        auto *p = new TestPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges | NetworkPlugin::CanFindSources);
        p->hashSetOnFind << qMakePair(qint64(hash::Ed2kHasher::PartSize), ed2kParts(data));
        req.hashes = hashesOf(data, 2);
        req.sources = {p->addSource(QStringLiteral("ed2k://crash"), sc, QStringLiteral("30.0.0.1:4662"))};
        plugins << p;
    } else if (store == QLatin1String("tth")) {
        auto *p = new TestPlugin(Net::Gnutella, NetworkPlugin::CanFetchRanges | NetworkPlugin::CanFindSources);
        const qint64 unit = 1 << 20;
        const QList<QByteArray> leaves = tthLeaves(data, unit);
        p->hashSetKind = QStringLiteral("tth-leaves");
        p->hashSetOnFind << qMakePair(unit, leaves);
        req.hashes.tth = hash::TigerTree::rootFromLevel(leaves);
        req.sources = {p->addSource(QStringLiteral("http://30.0.0.2/uri-res/N2R?crash"), sc, QStringLiteral("30.0.0.2:6346"))};
        plugins << p;
    } else if (store == QLatin1String("whole")) {
        auto *p = new TestPlugin(Net::Overlay, NetworkPlugin::CanFetchRanges | NetworkPlugin::CanFindSources);
        req.hashes = hashesOf(data, 1);
        req.sources = {p->addSource(QStringLiteral("octomx://crash"), sc, QStringLiteral("30.0.0.3:4700"))};
        plugins << p;
    }
    for (NetworkPlugin *p : std::as_const(plugins)) p->start();
    if (torrent) {
        node = std::make_unique<BtNode>(root + QStringLiteral("/nodeB"), true);
        if (!node->start()) {
            say(QStringLiteral("ERROR libtorrent did not start"));
            return 3;
        }
        node->ctl->setRateLimits(store == QLatin1String("merged") ? rate / 2 : rate, 0);
        req = DownloadRequest();
        req.uri = cl.value(QStringLiteral("torrent"));
        if (store == QLatin1String("merged")) {   // a torrent + an HTTP mirror: pieces injected from the mirror
            auto *http = new TestPlugin(Net::Http, NetworkPlugin::CanFetchRanges);
            SourceConfig m = sc;
            m.bytesPerSec = rate / 2;
            m.chunk = qBound(16 * 1024, m.bytesPerSec / 25, 1 << 20);
            req.sources = {http->addSource(QStringLiteral("https://mirror.example/payload.bin"), m)};
            http->start();
            plugins << http;
        }
    }

    EngineContext ctx;
    ctx.settings = &settings;
    ctx.db = &db;
    ctx.uploads = &uploads;
    ctx.plugins = plugins;
    if (node) {
        ctx.torrents = node->be;
        ctx.torrentPlugin = node->plugin.get();
        ctx.plugins << node->plugin.get();
    }
    ctx.profileDir = root;
    auto engine = std::make_unique<DownloadEngine>(ctx);
    engine->setTuning(fastTuning());
    engine->start();

    QString id;
    const QList<TransferInfo> have = engine->downloads();
    if (!have.isEmpty()) id = have.first().id;
    else id = engine->download(req);
    if (id.isEmpty()) {
        say(QStringLiteral("ERROR the download did not start"));
        return 3;
    }
    say(QStringLiteral("READY"));

    QElapsedTimer clock;
    clock.start();
    bool quitting = false;
    int exitCode = 0;
    auto quitNow = [&](const QString &why, int code) {
        if (quitting) return;
        quitting = true;
        exitCode = code;
        QElapsedTimer t;
        t.start();
        engine->shutdown();
        engine.reset();
        if (node) node->plugin->stop();
        db.close();
        durable::saveJournal(journalFile);
        say(QStringLiteral("QUIT %1 %2").arg(t.elapsed()).arg(why));
        QTimer::singleShot(0, &app, [&] { app.exit(exitCode); });
    };
    QuitSignals *sig = nullptr;
    if (!qEnvironmentVariableIsSet("MX_CRASH_NO_SIGNALS")) {
        sig = QuitSignals::install(&app);
        QObject::connect(sig, &QuitSignals::requested, &app, [&](const QString &why) {
            quitNow(why, 4);
            if (sig) sig->finished();
        });
    }

    bool peerAdded = false;
    QTimer poll;
    poll.setInterval(100);
    qint64 lastPrint = -1000;
    QObject::connect(&poll, &QTimer::timeout, &app, [&] {
        if (quitting) return;
        if (node && !peerAdded && !engine->debugInfo(id).value(QStringLiteral("torrentId")).toString().isEmpty()) {
            const QString tid = engine->debugInfo(id).value(QStringLiteral("torrentId")).toString();
            if (node->be->torrents().contains(tid)) {
                node->ctl->addPeer(tid, cl.value(QStringLiteral("peer")));
                peerAdded = true;
            }
        }
        const TransferInfo t = engine->transfer(id);
        if (clock.elapsed() - lastPrint >= 250) {
            lastPrint = clock.elapsed();
            say(QStringLiteral("P %1 %2 %3 %4").arg(t.done).arg(t.size).arg(int(t.state)).arg(t.statusText));
        }
        if (t.state == TransferState::Completed || t.state == TransferState::Seeding) {
            poll.stop();
            say(QStringLiteral("DONE %1 %2").arg(QString::fromLatin1(sha1OfFile(t.savePath).toHex()), t.savePath));
            quitNow(QStringLiteral("done"), 0);
        } else if (t.state == TransferState::Error) {
            poll.stop();
            say(QStringLiteral("ERROR %1").arg(t.statusText));
            quitNow(QStringLiteral("error"), 5);
        }
    });
    poll.start();

    if (stop == QLatin1String("quit") && after > 0) {
        QTimer::singleShot(after, &app, [&] { quitNow(QStringLiteral("timer"), 3); });
    } else if (stop == QLatin1String("closenow") && after > 0) {   // the safe quit, "Close now" pressed 200 ms later
        QTimer::singleShot(after, &app, [&] {
            engine->beginShutdown();
            QTimer::singleShot(200, &app, [&] {
                say(QStringLiteral("CLOSENOW %1 bytes not saved").arg(engine->unsavedBytes()));
                engine->closeNow();
                quitNow(QStringLiteral("close now"), 3);
            });
        });
    } else if (stop == QLatin1String("cut") && after > 0) {
        QTimer::singleShot(after, &app, [&] {
            QString rep;
            const qint64 n = durable::simulatePowerCut(&rep);
            QFile::remove(journalFile);
            say(QStringLiteral("CUT %1 %2").arg(n).arg(rep.left(300)));
#ifdef Q_OS_WIN
            TerminateProcess(GetCurrentProcess(), 9);
#else
            std::raise(SIGKILL);
#endif
        });
    }
    const int rc = app.exec();
    for (NetworkPlugin *p : std::as_const(plugins)) delete p;
    return rc;
}
