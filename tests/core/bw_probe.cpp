// SPDX-License-Identifier: GPL-3.0-or-later
// bw_probe: live check of the BandwidthService with the real core and every plugin in this build:
// prints one line per second with the service's totals (all traffic, payload, per network) next to
// the sum of the transfers' own reported rates, optionally while downloading a (legal) link.
//
//   bw_probe --profile DIR [--seconds N] [--warmup S] [--stop-after-bytes N] [<magnet:|ed2k://|http(s)://>]
//
// At the end: averages over the seconds with a running download (payload vs transfer rate).
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QTextStream>
#include <QtCore/QTimer>

#include <cstdio>

#include "core/log.h"
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
QString kb(qint64 bps) { return QString::number(double(bps) / 1024.0, 'f', 1); }
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QStringList args = app.arguments().mid(1);
    QString profile, link;
    int seconds = 90, warmup = 20;
    qint64 stopAfter = 0;
    for (int i = 0; i < args.size(); ++i) {
        const QString a = args.at(i);
        if (a == QLatin1String("--profile") && i + 1 < args.size()) profile = args.at(++i);
        else if (a == QLatin1String("--seconds") && i + 1 < args.size()) seconds = args.at(++i).toInt();
        else if (a == QLatin1String("--warmup") && i + 1 < args.size()) warmup = args.at(++i).toInt();
        else if (a == QLatin1String("--stop-after-bytes") && i + 1 < args.size()) stopAfter = args.at(++i).toLongLong();
        else link = a;
    }
    if (profile.isEmpty()) {
        std::fprintf(stderr, "usage: bw_probe --profile DIR [--seconds N] [--warmup S] [--stop-after-bytes N] [link]\n");
        return 2;
    }
    profile = QDir(profile).absolutePath();
    QDir().mkpath(profile);
    registerMetaTypes();
    installLogHandler(profile, 20 * 1024 * 1024, 3, false, 1);
    QList<NetworkPlugin *> plugins;
#ifdef MX_HAVE_BT
    plugins << createBitTorrentPlugin();
#endif
#ifdef MX_HAVE_ED2K
    plugins << createEd2kPlugins();
#endif
#ifdef MX_HAVE_WEB
    plugins << createWebSearchPlugin();
#endif
#ifdef MX_HAVE_HTTP
    plugins << createHttpPlugin();
#endif
#ifdef MX_HAVE_GNUTELLA
    plugins << createGnutellaPlugin();
#endif
#ifdef MX_HAVE_OVERLAY
    plugins << createOverlayPlugin() << createRatsSearchPlugin();
#endif
    Core *core = createRealCore(profile, plugins);
    core->settings()->setValue(QString(keys::transfers::IncomingDir), profile + QStringLiteral("/incoming"));
    core->start();
    QTextStream out(stdout);
    out << "bw_probe: " << plugins.size() << " plugins, profile " << profile << Qt::endl;

    QElapsedTimer clock;
    clock.start();
    QString dlId;
    struct Acc { qint64 n = 0, total = 0, payload = 0, transfer = 0, bt = 0, payloadBytes = 0; } acc;
    bool complete = false;
    int tick = 0;
    QObject::connect(core->bandwidth(), &BandwidthService::sample, &app, [&](const BandwidthSample &s) {
        ++tick;
        if (tick == warmup && !link.isEmpty()) {
            dlId = core->transfers()->downloadUri(link);
            out << "download started: " << dlId << Qt::endl;
        }
        qint64 transferRate = 0, done = 0;
        for (const TransferInfo &t : core->transfers()->downloads())
            if (t.id == dlId) {
                transferRate = t.rateDownBps;
                done = t.done;
                complete = t.state == TransferState::Completed || t.state == TransferState::Seeding;
            }
        if (!dlId.isEmpty() && !complete) acc.payloadBytes += s.payloadDownBps;   // ≈ bytes in this second
        int online = 0;
        for (const NetworkStatus &n : core->network()->networks()) if (n.state == ConnState::Connected) ++online;
        QStringList nets;
        for (auto it = s.netDownBps.constBegin(); it != s.netDownBps.constEnd(); ++it)
            nets << netShortName(it.key()) + QLatin1Char('=') + kb(it.value()) + QLatin1Char('/') + kb(s.netUpBps.value(it.key()));
        for (auto it = s.netUpBps.constBegin(); it != s.netUpBps.constEnd(); ++it)
            if (!s.netDownBps.contains(it.key())) nets << netShortName(it.key()) + QStringLiteral("=0/") + kb(it.value());
        nets.sort();
        out << QStringLiteral("[%1s] online %2 | total down %3 up %4 KiB/s | payload down %5 up %6 | transfer %7 KiB/s done %8 | %9")
                   .arg(clock.elapsed() / 1000, 3).arg(online).arg(kb(s.downBps), kb(s.upBps), kb(s.payloadDownBps), kb(s.payloadUpBps))
                   .arg(kb(transferRate)).arg(done).arg(nets.join(QLatin1Char(' ')))
            << Qt::endl;
        if (!dlId.isEmpty() && transferRate > 0) {
            ++acc.n;
            acc.total += s.downBps;
            acc.payload += s.payloadDownBps;
            acc.transfer += transferRate;
            acc.bt += s.netDownBps.value(Net::BitTorrent);
        }
        const bool stop = tick >= seconds || (stopAfter > 0 && done >= stopAfter) || complete;
        if (stop) {
            if (acc.n > 0)
                out << QStringLiteral("SUMMARY over %1 s with a running download: service total %2 KiB/s, payload %3 KiB/s, "
                                      "BitTorrent %4 KiB/s; the transfer's own rate %5 KiB/s; payload/transfer = %6")
                           .arg(acc.n).arg(kb(acc.total / acc.n), kb(acc.payload / acc.n), kb(acc.bt / acc.n), kb(acc.transfer / acc.n))
                           .arg(double(acc.payload) / double(qMax<qint64>(1, acc.transfer)), 0, 'f', 3)
                    << Qt::endl;
            out << QStringLiteral("download: %1 bytes done%2; the service's payload summed over the download: %3 bytes (ratio %4)")
                       .arg(done).arg(complete ? QStringLiteral(" (complete)") : QString()).arg(acc.payloadBytes)
                       .arg(double(acc.payloadBytes) / double(qMax<qint64>(1, done)), 0, 'f', 3)
                << Qt::endl;
            out << QStringLiteral("session totals: down %1 bytes, up %2 bytes; history %3 samples")
                       .arg(core->bandwidth()->totalDownloaded()).arg(core->bandwidth()->totalUploaded())
                       .arg(core->bandwidth()->history(0).size())
                << Qt::endl;
            if (!dlId.isEmpty()) core->transfers()->cancel(dlId, true);
            core->shutdown();
            QTimer::singleShot(4000, &app, &QCoreApplication::quit);
        }
    });
    QObject::connect(core, &Core::shutdownFinished, &app, &QCoreApplication::quit);
    return app.exec();
}
