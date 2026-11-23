// SPDX-License-Identifier: GPL-3.0-or-later
// bw_limit_probe [bw-1]: live check that every network obeys the download / upload limits from one
// shared budget. Real core + the plugins of this build; sets the limits through the settings (as the
// UI does), changes them while running, downloads (legal) links or a search hit, and every second
// compares the program's own counters (BandwidthService, per network) with the operating system's
// TCP byte counters of this process (ss -tinp: bytes_received / bytes_acked of every socket).
// At the end: a table of 30-s windows, limit in force versus measured. Not a ctest.
//
//   bw_limit_probe --profile DIR [--seconds N] [--warmup S] [--window S] [--down KB] [--up KB]
//                  [--at S:DOWN:UP ...] [--set key=value ...] [--nets bt,ed2k,web,http,gnutella,overlay]
//                  [--share DIR] [--search TEXT --search-net NET --pick-after S --min-size BYTES]
//                  [--stop-after-bytes N] [--keep] [link ...]
//
// UDP traffic (uTP, DHT, Kad, librats) has no per-socket byte counters: for it only the program's
// counters are shown (the OS column is TCP only).
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QProcess>
#include <QtCore/QTextStream>
#include <QtCore/QTimer>

#include <cstdio>

#include "core/log.h"
#include "mx/bandwidth_limits.h"
#include "mx/plugin.h"
#include "mx/services.h"
#include "mx/settings_keys.h"

#ifdef MX_HAVE_BT
#include "bt_plugin.h"
#endif
#ifdef MX_HAVE_ED2K
#include "ed2k_plugin.h"
#endif
#ifdef MX_HAVE_WEB
#include "web_plugin.h"
#endif
#ifdef MX_HAVE_HTTP
#include "http_plugin.h"
#endif
#ifdef MX_HAVE_GNUTELLA
#include "gnutella_plugin.h"
#endif
#ifdef MX_HAVE_OVERLAY
#include "overlay_plugin.h"
#endif

using namespace mx;

namespace {
QString kb(double bps) { return QString::number(bps / 1024.0, 'f', 1); }

// OS TCP counters of this process: per socket bytes_received / bytes_acked, summed as deltas
struct OsCounters {
    QHash<QString, QPair<qint64, qint64>> last;   // socket -> (received, acked)
    qint64 down = 0, up = 0;                      // cumulative deltas
    bool ok = true;
    void poll()
    {
        QProcess p;
        p.start(QStringLiteral("ss"), {QStringLiteral("-tinpH")});
        if (!p.waitForFinished(3000)) { ok = false; return; }
        const QString me = QStringLiteral("pid=%1,").arg(QCoreApplication::applicationPid());
        const QStringList lines = QString::fromLocal8Bit(p.readAllStandardOutput()).split(QLatin1Char('\n'));
        QString key;
        bool mine = false;
        QHash<QString, QPair<qint64, qint64>> now;
        for (const QString &l : lines) {
            if (l.isEmpty()) continue;
            if (!l.at(0).isSpace()) {
                const QStringList f = l.simplified().split(QLatin1Char(' '));
                mine = l.contains(me) && f.size() >= 5;
                key = mine ? f.at(3) + QLatin1Char('>') + f.at(4) : QString();
                continue;
            }
            if (!mine) continue;
            qint64 rx = 0, ack = 0;
            for (const QString &tok : l.simplified().split(QLatin1Char(' '))) {
                if (tok.startsWith(QLatin1String("bytes_received:"))) rx = tok.mid(15).toLongLong();
                else if (tok.startsWith(QLatin1String("bytes_acked:"))) ack = tok.mid(12).toLongLong();
            }
            now.insert(key, {rx, ack});
        }
        for (auto it = now.constBegin(); it != now.constEnd(); ++it) {
            const QPair<qint64, qint64> prev = last.value(it.key(), {0, 0});
            down += qMax<qint64>(0, it.value().first - prev.first);
            up += qMax<qint64>(0, it.value().second - prev.second);
        }
        last = now;
    }
};

struct Second {
    int t = 0;
    int limDown = 0, limUp = 0;   // KB/s in force (0 = none)
    qint64 down = 0, up = 0, payDown = 0, payUp = 0, osDown = 0, osUp = 0;
    QHash<Net, qint64> netDown, netUp;
};
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QStringList args = app.arguments().mid(1);
    QString profile, shareDir, searchText, searchNet;
    QStringList links, sets, nets;
    QList<std::tuple<int, int, int>> changes;
    int seconds = 180, warmup = 20, window = 30, pickAfter = 45, down = -1, up = -1;
    qint64 stopAfter = 0, minSize = 0;
    bool keep = false;
    for (int i = 0; i < args.size(); ++i) {
        const QString a = args.at(i);
        auto next = [&] { return i + 1 < args.size() ? args.at(++i) : QString(); };
        if (a == QLatin1String("--profile")) profile = next();
        else if (a == QLatin1String("--seconds")) seconds = next().toInt();
        else if (a == QLatin1String("--warmup")) warmup = next().toInt();
        else if (a == QLatin1String("--window")) window = next().toInt();
        else if (a == QLatin1String("--down")) down = next().toInt();
        else if (a == QLatin1String("--up")) up = next().toInt();
        else if (a == QLatin1String("--at")) {
            const QStringList p = next().split(QLatin1Char(':'));
            if (p.size() == 3) changes << std::make_tuple(p[0].toInt(), p[1].toInt(), p[2].toInt());
        } else if (a == QLatin1String("--set")) sets << next();
        else if (a == QLatin1String("--nets")) nets = next().split(QLatin1Char(','));
        else if (a == QLatin1String("--share")) shareDir = next();
        else if (a == QLatin1String("--search")) searchText = next();
        else if (a == QLatin1String("--search-net")) searchNet = next();
        else if (a == QLatin1String("--pick-after")) pickAfter = next().toInt();
        else if (a == QLatin1String("--min-size")) minSize = next().toLongLong();
        else if (a == QLatin1String("--stop-after-bytes")) stopAfter = next().toLongLong();
        else if (a == QLatin1String("--keep")) keep = true;
        else links << a;
    }
    if (profile.isEmpty()) {
        std::fprintf(stderr, "usage: see the head of tests/core/bw_limit_probe.cpp\n");
        return 2;
    }
    profile = QDir(profile).absolutePath();
    QDir().mkpath(profile);
    registerMetaTypes();
    installLogHandler(profile, 20 * 1024 * 1024, 3, false, 1);
    auto want = [&nets](const char *n) { return nets.isEmpty() || nets.contains(QLatin1String(n)); };
    QList<NetworkPlugin *> plugins;
#ifdef MX_HAVE_BT
    if (want("bt")) plugins << createBitTorrentPlugin();
#endif
#ifdef MX_HAVE_ED2K
    if (want("ed2k")) plugins << createEd2kPlugins();
#endif
#ifdef MX_HAVE_WEB
    if (want("web")) plugins << createWebSearchPlugin();
#endif
#ifdef MX_HAVE_HTTP
    if (want("http")) plugins << createHttpPlugin();
#endif
#ifdef MX_HAVE_GNUTELLA
    if (want("gnutella")) plugins << createGnutellaPlugin();
#endif
#ifdef MX_HAVE_OVERLAY
    if (want("overlay")) plugins << createOverlayPlugin();
    if (want("rats")) plugins << createRatsSearchPlugin();
#endif
    Core *core = createRealCore(profile, plugins);
    Settings *st = core->settings();
    st->setValue(QString(keys::transfers::IncomingDir), profile + QStringLiteral("/incoming"));
    for (const QString &kv : sets) {
        const int eq = kv.indexOf(QLatin1Char('='));
        if (eq <= 0) continue;
        const QString k = kv.left(eq), v = kv.mid(eq + 1);
        bool isInt = false;
        const qint64 n = v.toLongLong(&isInt);
        if (v == QLatin1String("true") || v == QLatin1String("false")) st->setValue(k, v == QLatin1String("true"));
        else if (isInt) st->setValue(k, n);
        else st->setValue(k, v);
    }
    auto setLimits = [st](int d, int u) {   // as the UI does (key + remembered value)
        if (d >= 0) { if (d > 0) st->setValue(QString(keys::bandwidth::LastDownLimitKBs), d); st->setValue(QString(keys::bandwidth::DownLimitKBs), d); }
        if (u >= 0) { if (u > 0) st->setValue(QString(keys::bandwidth::LastUpLimitKBs), u); st->setValue(QString(keys::bandwidth::UpLimitKBs), u); }
    };
    setLimits(down, up);
    if (!shareDir.isEmpty()) core->library()->addFolder(QDir(shareDir).absolutePath(), true);
    core->start();
    QTextStream out(stdout);
    out << "bw_limit_probe: " << plugins.size() << " plugins, pid " << QCoreApplication::applicationPid() << Qt::endl;

    OsCounters os;
    os.poll();
    QElapsedTimer clock;
    clock.start();
    QStringList dlIds;
    quint32 searchId = 0;
    QList<Second> secs;
    qint64 lastOsDown = os.down, lastOsUp = os.up;
    int tick = 0;
    bool stopping = false;
    // our own 1 Hz sampling of the limiter totals (BandwidthService::current() is the same data)
    QTimer timer;
    timer.setInterval(1000);
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        if (stopping) return;
        ++tick;
        for (const auto &c : changes)
            if (std::get<0>(c) == tick) {
                setLimits(std::get<1>(c), std::get<2>(c));
                out << "[" << tick << "s] limits changed to down " << std::get<1>(c) << " up " << std::get<2>(c) << " KB/s" << Qt::endl;
            }
        if (tick == warmup) {
            for (const QString &l : links) {
                const QString id = core->transfers()->downloadUri(l);
                dlIds << id;
                out << "download started: " << id << Qt::endl;
            }
            if (!searchText.isEmpty()) {
                SearchQuery q;
                q.text = searchText;
                q.minSize = minSize;
                if (!searchNet.isEmpty()) q.networks = {netFromName(searchNet)};
                searchId = core->search()->startSearch(q);
                out << "search started: " << searchText << Qt::endl;
            }
        }
        if (searchId && tick == warmup + pickAfter) {
            const QList<SearchHit> hits = core->search()->hits(searchId);
            const SearchHit *best = nullptr;
            int bestN = 0;
            for (const SearchHit &h : hits) {
                if (h.size < minSize) continue;
                int n = 0;
                for (const SearchSource &s : h.sources)
                    if (searchNet.isEmpty() || s.net == netFromName(searchNet)) ++n;
                if (n > bestN) { bestN = n; best = &h; }
            }
            out << "search: " << hits.size() << " hits";
            if (best) {
                out << ", picked " << best->fileName << " (" << best->size << " bytes, " << bestN << " sources)";
                dlIds << core->transfers()->downloadHit(*best);
            }
            out << Qt::endl;
        }
        os.poll();
        Second x;
        x.t = tick;
        const bwlimit::InForce f = bwlimit::inForce(st);
        x.limDown = f.downKBs;
        x.limUp = f.upKBs;
        // totals straight from the history (one sample per second, newest last)
        const QList<BandwidthSample> h = core->bandwidth()->history(1);
        if (!h.isEmpty()) {
            const BandwidthSample &b = h.last();
            x.down = b.downBps; x.up = b.upBps; x.payDown = b.payloadDownBps; x.payUp = b.payloadUpBps;
            for (auto it = b.netDownBps.constBegin(); it != b.netDownBps.constEnd(); ++it) x.netDown[it.key()] = it.value();
            for (auto it = b.netUpBps.constBegin(); it != b.netUpBps.constEnd(); ++it) x.netUp[it.key()] = it.value();
        }
        x.osDown = os.down - lastOsDown;
        x.osUp = os.up - lastOsUp;
        lastOsDown = os.down;
        lastOsUp = os.up;
        secs << x;
        qint64 done = 0;
        bool complete = false;
        for (const TransferInfo &t : core->transfers()->downloads())
            if (dlIds.contains(t.id)) {
                done += t.done;
                complete = t.state == TransferState::Completed || t.state == TransferState::Seeding;
            }
        QStringList netTxt;
        QList<Net> keys = x.netDown.keys();
        for (Net n : x.netUp.keys()) if (!keys.contains(n)) keys << n;
        std::sort(keys.begin(), keys.end());
        for (Net n : keys) netTxt << netShortName(n) + QLatin1Char('=') + kb(x.netDown.value(n)) + QLatin1Char('/') + kb(x.netUp.value(n));
        out << QStringLiteral("[%1s] limit %2/%3 | prog %4/%5 pay %6/%7 | os-tcp %8/%9 KiB/s | done %10 | %11")
                   .arg(tick, 3).arg(x.limDown).arg(x.limUp).arg(kb(x.down), kb(x.up), kb(x.payDown), kb(x.payUp), kb(x.osDown), kb(x.osUp))
                   .arg(done).arg(netTxt.join(QLatin1Char(' ')));
        if (searchId) out << " | hits " << core->search()->hits(searchId).size();   // [bw-2]
        out << Qt::endl;
        if (tick >= seconds || (stopAfter > 0 && done >= stopAfter) || (complete && !keep && !dlIds.isEmpty())) {
            stopping = true;
            // the table: windows of `window` seconds with one limit in force throughout
            out << Qt::endl << "WINDOWS (KiB/s; prog = program counters, all networks; os = TCP sockets of this process)" << Qt::endl;
            out << "  from-to | limit down/up | prog down/up | payload down/up | os-tcp down/up | per network down/up" << Qt::endl;
            int i = 0;
            while (i < secs.size()) {
                int j = i;
                while (j < secs.size() && j - i < window && secs[j].limDown == secs[i].limDown && secs[j].limUp == secs[i].limUp) ++j;
                const int n = j - i;
                if (n >= 10) {
                    double d = 0, u = 0, pd = 0, pu = 0, od = 0, ou = 0;
                    QHash<Net, double> nd, nu;
                    for (int k = i; k < j; ++k) {
                        d += secs[k].down; u += secs[k].up; pd += secs[k].payDown; pu += secs[k].payUp;
                        od += secs[k].osDown; ou += secs[k].osUp;
                        for (auto it = secs[k].netDown.constBegin(); it != secs[k].netDown.constEnd(); ++it) nd[it.key()] += it.value();
                        for (auto it = secs[k].netUp.constBegin(); it != secs[k].netUp.constEnd(); ++it) nu[it.key()] += it.value();
                    }
                    QStringList per;
                    QList<Net> ks = nd.keys();
                    for (Net k : nu.keys()) if (!ks.contains(k)) ks << k;
                    std::sort(ks.begin(), ks.end());
                    for (Net k : ks)
                        if (nd.value(k) / n >= 512 || nu.value(k) / n >= 512)
                            per << netShortName(k) + QLatin1Char(' ') + kb(nd.value(k) / n) + QLatin1Char('/') + kb(nu.value(k) / n);
                    auto lim = [](int v) { return v > 0 ? QString::number(v) : QStringLiteral("none"); };
                    out << QStringLiteral("  %1-%2 | %3/%4 | %5/%6 | %7/%8 | %9/%10 | %11")
                               .arg(secs[i].t, 3).arg(secs[j - 1].t, 3).arg(lim(secs[i].limDown), lim(secs[i].limUp))
                               .arg(kb(d / n), kb(u / n), kb(pd / n), kb(pu / n), kb(od / n), kb(ou / n))
                               .arg(per.join(QStringLiteral(", ")))
                        << Qt::endl;
                }
                i = j;
            }
            for (const QString &id : std::as_const(dlIds))
                core->transfers()->cancel(id, true);   // and delete the data
            core->shutdown();
            QTimer::singleShot(5000, &app, &QCoreApplication::quit);
        }
    });
    timer.start();
    QObject::connect(core, &Core::shutdownFinished, &app, &QCoreApplication::quit);
    return app.exec();
}
