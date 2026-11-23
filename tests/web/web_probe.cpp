// SPDX-License-Identifier: GPL-3.0-or-later
// web_probe "<query>" [--type video|audio|...] [--max N] [--only id,id] [--data dir] [--adult-filter] [--timeout s]
// Runs a live search through the NetworkPlugin interface of the web plugin and prints a table.
#include <QtCore/QCommandLineParser>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QTimer>

#include <algorithm>
#include <cstdio>

#include "mx/plugin.h"
#include "mx/settings_keys.h"
#include "mx/simple_settings.h"
#include "provider_def.h"
#include "web_plugin.h"
#include "web_search_plugin.h"

using namespace mx;

static QString human(qint64 b)
{
    if (b <= 0) return QStringLiteral("?");
    const char *u[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = double(b);
    int i = 0;
    while (v >= 1024 && i < 4) { v /= 1024; ++i; }
    return QStringLiteral("%1 %2").arg(v, 0, 'f', i ? 1 : 0).arg(QLatin1String(u[i]));
}

static void out(const QString &s)
{
    std::fputs(s.toLocal8Bit().constData(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    mx::registerMetaTypes();
    QCommandLineParser p;
    p.addHelpOption();
    p.addPositionalArgument(QStringLiteral("query"), QStringLiteral("keywords"));
    p.addOption({QStringLiteral("type"), QStringLiteral("media type"), QStringLiteral("type"), QStringLiteral("any")});
    p.addOption({QStringLiteral("max"), QStringLiteral("rows to print"), QStringLiteral("n"), QStringLiteral("15")});
    p.addOption({QStringLiteral("only"), QStringLiteral("comma-separated provider ids"), QStringLiteral("ids")});
    p.addOption({QStringLiteral("data"), QStringLiteral("plugin data dir"), QStringLiteral("dir")});
    p.addOption({QStringLiteral("timeout"), QStringLiteral("whole-search deadline (s)"), QStringLiteral("s"), QStringLiteral("25")});
    p.addOption({QStringLiteral("adult-filter"), QStringLiteral("enable search/adultFilter")});
    p.addOption({QStringLiteral("text2"), QStringLiteral("second keyword line"), QStringLiteral("text")});
    p.addOption({QStringLiteral("ext"), QStringLiteral("extension filter"), QStringLiteral("ext")});
    p.process(app);
    if (p.positionalArguments().isEmpty()) p.showHelp(1);

    SimpleSettings settings;
    settings.setValue(web::wkeys::ProbeOnStart, false);
    settings.setValue(web::wkeys::TimeoutSec, p.value(QStringLiteral("timeout")).toInt());
    settings.setValue(keys::search::AdultFilter, p.isSet(QStringLiteral("adult-filter")));
    PluginContext ctx;
    ctx.settings = &settings;
    ctx.dataDir = p.value(QStringLiteral("data"));
    if (ctx.dataDir.isEmpty()) ctx.dataDir = QDir::currentPath() + QStringLiteral("/web-probe-data");
    ctx.profileDir = ctx.dataDir;
    ctx.clientName = QStringLiteral("OctoMX");

    auto *plugin = static_cast<web::WebSearchPlugin *>(createWebSearchPlugin(&app));
    plugin->init(ctx);
    if (p.isSet(QStringLiteral("only"))) {
        const QStringList only = p.value(QStringLiteral("only")).split(QLatin1Char(','));
        for (web::SearchBackend *b : plugin->backends()) b->setEnabled(only.contains(b->id()));
    }
    QObject::connect(plugin, &NetworkPlugin::logMessage, [](const QString &m) { out(QStringLiteral("log: ") + m); });
    plugin->start();

    SearchQuery q;
    q.id = 1;
    q.text = p.positionalArguments().join(QLatin1Char(' '));
    q.text2 = p.value(QStringLiteral("text2"));
    q.type = web::mediaTypeFromKey(p.value(QStringLiteral("type")));
    q.extension = p.value(QStringLiteral("ext"));
    q.maxResults = 500;

    QHash<QString, SearchHit> hits;
    QStringList order;
    int batches = 0;
    QElapsedTimer t;
    t.start();
    QObject::connect(plugin, &NetworkPlugin::searchHits, [&](quint32, const QList<SearchHit> &list) {
        ++batches;
        for (const SearchHit &h : list) {
            const QString k = h.hashes.btih.isEmpty() ? h.sources.value(0).uri : QString::fromLatin1(h.hashes.btih.toHex());
            if (!hits.contains(k)) order << k;
            hits.insert(k, h);
        }
        out(QStringLiteral("  [%1 ms] batch %2: %3 hits (total %4)").arg(t.elapsed()).arg(batches).arg(list.size()).arg(hits.size()));
    });
    QObject::connect(plugin, &NetworkPlugin::searchDone, [&](quint32) {
        out(QStringLiteral("searchDone after %1 ms, %2 batches, %3 distinct hits").arg(t.elapsed()).arg(batches).arg(hits.size()));
        out(QStringLiteral("\nprovider          state       ping  rows  error"));
        for (web::SearchBackend *b : plugin->backends()) {
            const auto &h = b->health();
            const char *st[] = {"unknown", "ok", "failing", "backoff", "disabled"};
            out(QStringLiteral("%1 %2 %3 %4  %5")
                    .arg(b->id(), -17)
                    .arg(QLatin1String(b->isEnabled() ? st[int(h.state)] : "disabled"), -10)
                    .arg(h.lastLatencyMs, 5)
                    .arg(h.lastResultCount, 5)
                    .arg(h.lastError));
        }
        out(QStringLiteral("status: ") + plugin->status().statusText);
        // per provider hit counts
        QHash<QString, int> per;
        int http = 0;
        for (const SearchHit &h : std::as_const(hits)) {
            QStringList seen;
            for (const SearchSource &s : h.sources) {
                if (s.net == Net::Http) { ++http; continue; }
                const QString pr = s.extra.value(QStringLiteral("provider")).toString();
                if (!seen.contains(pr)) { seen << pr; per[pr]++; }
            }
        }
        QStringList perText;
        for (auto it = per.constBegin(); it != per.constEnd(); ++it) perText << QStringLiteral("%1=%2").arg(it.key()).arg(it.value());
        perText.sort();
        out(QStringLiteral("hits per provider: ") + perText.join(QStringLiteral(", ")) + QStringLiteral("; HTTP sources: %1").arg(http));

        QList<SearchHit> sorted;
        for (const QString &k : std::as_const(order)) sorted << hits.value(k);
        auto maxSeeds = [](const SearchHit &h) { int m = -1; for (const auto &s : h.sources) m = qMax(m, s.seeds); return m; };
        std::stable_sort(sorted.begin(), sorted.end(), [&](const SearchHit &a, const SearchHit &b) { return maxSeeds(a) > maxSeeds(b); });
        out(QStringLiteral("\n%1 %2 %3 %4 %5 %6 %7").arg(QStringLiteral("name"), -58).arg(QStringLiteral("size"), 10)
                .arg(QStringLiteral("seeds"), 6).arg(QStringLiteral("type"), 5).arg(QStringLiteral("B"), 1)
                .arg(QStringLiteral("btih"), -10).arg(QStringLiteral("sources")));
        const int maxRows = p.value(QStringLiteral("max")).toInt();
        for (int i = 0; i < sorted.size() && i < maxRows; ++i) {
            const SearchHit &h = sorted.at(i);
            QStringList src;
            for (const auto &s : h.sources) {
                const QString pr = s.extra.value(QStringLiteral("provider")).toString();
                src << (s.net == Net::Http ? QStringLiteral("http:") + pr : pr);
            }
            src.removeDuplicates();
            const char *types[] = {"any", "audio", "video", "image", "doc", "soft", "arch", "other"};
            out(QStringLiteral("%1 %2 %3 %4 %5 %6 %7")
                    .arg(h.fileName.left(58), -58)
                    .arg(human(h.size), 10)
                    .arg(maxSeeds(h), 6)
                    .arg(QLatin1String(types[int(h.type)]), 5)
                    .arg(h.isBundle ? QStringLiteral("B") : QStringLiteral("-"))
                    .arg(QString::fromLatin1(h.hashes.btih.toHex().left(10)), -10)
                    .arg(src.join(QLatin1Char(','))));
        }
        if (!sorted.isEmpty()) {
            const SearchHit &h = sorted.first();
            for (const auto &s : h.sources)
                if (s.net == Net::BitTorrent) { out(QStringLiteral("\nfirst magnet: ") + s.uri.left(300)); break; }
        }
        for (const SearchHit &h : std::as_const(sorted)) {
            for (const auto &s : h.sources)
                if (s.net == Net::Http) {
                    out(QStringLiteral("first HTTP source: %1 (%2 files listed in hit '%3')").arg(s.uri).arg(h.files.size()).arg(h.fileName));
                    goto doneHttp;
                }
        }
    doneHttp:
        QTimer::singleShot(0, &app, [&] { plugin->stop(); app.exit(hits.isEmpty() ? 2 : 0); });
    });
    plugin->search(q);
    return app.exec();
}
