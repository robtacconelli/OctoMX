// SPDX-License-Identifier: GPL-3.0-or-later
// rats_probe: live check of the Net::Rats plugin through the mx::NetworkPlugin interface.
//
//   rats_probe [--data DIR] [--seconds N] [--port P] [--wait-peers N] [--no-safe] "<query>" ...
//
// Starts the plugin (rats-search/4), waits until N peers are connected (default 5, max 40 s),
// runs each query through search(), prints hits / latency / peers, asks one bundle's file list via
// RatsSearchApi::requestTorrentFiles(), then stops and prints a summary. Output lines start with
// "+<seconds>". Exit code 0 if at least one query got results.
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QTimer>

#include <cstdio>

#include "mx/plugin.h"
#include "mx/settings_keys.h"
#include "mx/simple_settings.h"
#include "overlay_plugin.h"

namespace {
QElapsedTimer g_t;
void out(const QString &s)
{
    std::printf("+%7.2fs %s\n", g_t.elapsed() / 1000.0, s.toLocal8Bit().constData());
    std::fflush(stdout);
}
QString human(qint64 b)
{
    const char *u[] = {"B", "KB", "MB", "GB", "TB"};
    double v = b;
    int i = 0;
    while (v >= 1024 && i < 4) { v /= 1024; ++i; }
    return QStringLiteral("%1%2").arg(v, 0, 'f', 1).arg(QLatin1String(u[i]));
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    mx::registerMetaTypes();
    g_t.start();
    QString data = QDir::currentPath() + QStringLiteral("/rats_probe_data");
    int seconds = 120, waitPeers = 5, port = 0;
    bool safe = true;
    QStringList queries;
    const QStringList args = app.arguments().mid(1);
    for (int i = 0; i < args.size(); ++i) {
        const QString &a = args[i];
        auto next = [&] { return i + 1 < args.size() ? args[++i] : QString(); };
        if (a == QLatin1String("--data")) data = next();
        else if (a == QLatin1String("--seconds")) seconds = next().toInt();
        else if (a == QLatin1String("--wait-peers")) waitPeers = next().toInt();
        else if (a == QLatin1String("--port")) port = next().toInt();
        else if (a == QLatin1String("--no-safe")) safe = false;
        else queries << a;
    }
    if (queries.isEmpty()) queries << QStringLiteral("ubuntu");

    mx::SimpleSettings settings;
    settings.setValue(QStringLiteral("rats/listenPort"), port);
    settings.setValue(QStringLiteral("rats/safeSearch"), safe);
    settings.setValue(QString(mx::keys::connection::Upnp), false);   // probes never touch the router
    settings.setValue(QString(mx::keys::search::TimeoutSec), 60);
    mx::PluginContext ctx;
    ctx.settings = &settings;
    ctx.dataDir = data;
    ctx.profileDir = data;
    ctx.clientName = QStringLiteral("OctoMX");
    ctx.clientVersion = QStringLiteral(MX_VERSION_STRING);

    mx::NetworkPlugin *plugin = mx::createRatsSearchPlugin(&app);
    auto *api = qobject_cast<mx::RatsSearchApi *>(plugin);
    out(QStringLiteral("plugin net=%1 caps=%2 data=%3").arg(mx::netName(plugin->net())).arg(plugin->capabilities()).arg(data));
    plugin->init(ctx);

    int firstPeerMs = -1;
    QObject::connect(plugin, &mx::NetworkPlugin::statusChanged, [&](const mx::NetworkStatus &st) {
        if (firstPeerMs < 0 && st.peers > 0) firstPeerMs = static_cast<int>(g_t.elapsed());
        out(QStringLiteral("STATUS state=%1 peers=%2 '%3' port=%4").arg(int(st.state)).arg(st.peers).arg(st.statusText).arg(st.listenPort));
    });

    struct Q { QString text; qint64 sentMs = 0; qint64 firstMs = -1; int hits = 0; bool done = false; QByteArray bundle; };
    QList<Q> qs;
    for (const QString &t : queries) { Q q; q.text = t; qs << q; }
    int cur = -1;
    int totalHits = 0;
    QByteArray filesFor;
    qint64 filesAskedMs = 0;

    QObject::connect(plugin, &mx::NetworkPlugin::searchHits, [&](quint32 id, const QList<mx::SearchHit> &hits) {
        Q &q = qs[int(id) - 1];
        if (q.firstMs < 0) q.firstMs = g_t.elapsed() - q.sentMs;
        for (const auto &h : hits) {
            ++q.hits;
            ++totalHits;
            if (q.bundle.isEmpty() && h.isBundle) q.bundle = h.hashes.btih;
            if (q.hits <= 5)
                out(QStringLiteral("HIT  q%1 %2 %3 %4 S/L=%5/%6 %7%8 <- %9").arg(id).arg(QString::fromLatin1(h.hashes.btih.toHex()))
                        .arg(human(h.size)).arg(h.isBundle ? QStringLiteral("bundle(%1)").arg(h.sources[0].extra.value(QStringLiteral("files")).toInt()) : QStringLiteral("file"))
                        .arg(h.sources[0].seeds).arg(h.sources[0].peers).arg(h.fileName)
                        .arg(h.path.isEmpty() ? QString() : QStringLiteral(" [path %1]").arg(h.path))
                        .arg(h.sources[0].extra.value(QStringLiteral("ratsPeer")).toString().left(8)));
        }
    });
    std::function<void()> nextQuery;
    QObject::connect(plugin, &mx::NetworkPlugin::searchDone, [&](quint32 id) {
        Q &q = qs[int(id) - 1];
        q.done = true;
        out(QStringLiteral("DONE q%1 '%2': %3 hits, first after %4 ms, done after %5 ms")
                .arg(id).arg(q.text).arg(q.hits).arg(q.firstMs).arg(g_t.elapsed() - q.sentMs));
        if (filesFor.isEmpty() && !q.bundle.isEmpty()) {
            filesFor = q.bundle;
            filesAskedMs = g_t.elapsed();
            out(QStringLiteral("FILES? %1 -> %2").arg(QString::fromLatin1(filesFor.toHex())).arg(api->requestTorrentFiles(filesFor)));
        }
        QTimer::singleShot(0, nextQuery);
    });
    QObject::connect(api, &mx::RatsSearchApi::torrentFilesReceived, [&](const QByteArray &btih, const QList<mx::BundleFile> &files, qint64 total) {
        out(QStringLiteral("FILES %1 %2 files, %3, %4 ms").arg(QString::fromLatin1(btih.toHex())).arg(files.size()).arg(human(total)).arg(g_t.elapsed() - filesAskedMs));
        for (int i = 0; i < files.size() && i < 3; ++i) out(QStringLiteral("      %1 %2").arg(files[i].path, human(files[i].size)));
    });

    nextQuery = [&] {
        if (++cur >= qs.size()) {
            QTimer::singleShot(filesFor.isEmpty() ? 0 : 10000, &app, [&] {
                out(QStringLiteral("SERVERS (%1):").arg(plugin->servers().size()));
                int n = 0;
                for (const auto &s : plugin->servers())
                    if (n++ < 12) out(QStringLiteral("  %1 %2 %3 %4").arg(s.id.left(8), s.address, s.name, s.description));
                const auto st = plugin->status();
                out(QStringLiteral("SUMMARY first_peer_ms=%1 peers=%2 status='%3' total_hits=%4").arg(firstPeerMs).arg(st.peers).arg(st.statusText).arg(totalHits));
                for (int i = 0; i < qs.size(); ++i)
                    out(QStringLiteral("  q%1 '%2' hits=%3 first_ms=%4").arg(i + 1).arg(qs[i].text).arg(qs[i].hits).arg(qs[i].firstMs));
                QElapsedTimer st2;
                st2.start();
                plugin->stop();
                delete plugin;
                out(QStringLiteral("stopped in %1 ms").arg(st2.elapsed()));
                app.exit(totalHits > 0 ? 0 : 1);
            });
            return;
        }
        mx::SearchQuery q;
        q.id = quint32(cur + 1);
        q.text = qs[cur].text;
        q.maxResults = 500;
        qs[cur].sentMs = g_t.elapsed();
        out(QStringLiteral("SEARCH q%1 '%2' (peers=%3)").arg(q.id).arg(q.text).arg(plugin->status().peers));
        plugin->search(q);
    };

    plugin->start();
    QTimer poll;
    QElapsedTimer waited;
    waited.start();
    QObject::connect(&poll, &QTimer::timeout, [&] {
        if (plugin->status().peers >= waitPeers || waited.elapsed() > 40000) {
            poll.stop();
            nextQuery();
        }
    });
    poll.start(250);
    QTimer::singleShot(seconds * 1000, &app, [&] { out(QStringLiteral("TIMEOUT")); app.exit(2); });
    return app.exec();
}
