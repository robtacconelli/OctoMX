// SPDX-License-Identifier: GPL-3.0-or-later
// join_probe [eng-1]: live check that a plain file found on eD2k / Kad / Gnutella and joined with a
// torrent that holds the same file really gets BitTorrent into the download. The REAL core + every
// plugin of this build, no GUI. It searches, picks the merged row whose BitTorrent part is a
// candidate (inside a folder torrent or matched by name and size) next to another network, applies
// the speed limits of the configuration (before the start or N s after it), downloads it and every
// 5 s writes what libtorrent sees of each torrent (metadata, flags, peers known / connected, where
// they came from, DHT, trackers; never an address) and what the Transfers row says for BitTorrent.
// At the end one RESULT line: seconds until metadata, seconds until the first verified block from
// BitTorrent, bytes per network after 180 s. The download and its data are deleted. Not a ctest.
//
//   join_probe --profile DIR [--text WORDS] [--search-wait S] [--seconds N] [--down KB] [--up KB]
//              [--limit-at S] [--settings FILE.json] [--set key=value ...] [--min-size BYTES]
//              [--restart-at S] [--only-btih PREFIX] [--out FILE]
//
// --limit-at: -1 (default) = the limits are set before the download starts, else S seconds after.
// --restart-at: shut the core down S s after the start and start a new one on the same profile
// (the download is restored; checks that a torrent without metadata is tried again).
// Only for the open film Big Buck Bunny (the default words) and Ubuntu / Debian images.
#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTextStream>
#include <QtCore/QTimer>

#include <cstdio>
#include <functional>

#include "core/engine/engine.h"
#include "core/log.h"
#include "core/rate_limiter.h"
#include "core/real_core.h"
#include "mx/plugin.h"
#include "mx/services.h"
#include "mx/settings_keys.h"
#include "mxhash/links.h"

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
QTextStream out(stdout);

QFile *g_outFile = nullptr;   // --out: a copy of the report (wine loses stdout to a pipe)

void say(const QString &s)
{
    out << s << Qt::endl;
    if (g_outFile) { g_outFile->write(s.toUtf8() + '\n'); g_outFile->flush(); }
}

void pump(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

bool waitFor(const std::function<bool()> &pred, qint64 ms)
{
    QElapsedTimer t;
    t.start();
    while (!pred()) {
        if (ms >= 0 && t.elapsed() >= ms) return false;
        pump(100);
    }
    return true;
}

QString mb(qint64 b) { return QString::number(double(b) / 1048576.0, 'f', 1); }

bool isOther(Net n) { return n == Net::ED2K || n == Net::Kad || n == Net::Gnutella; }

struct Pick {
    int score = -1;
    SearchHit hit;
    QString why;
};

QList<NetworkPlugin *> makePlugins()
{
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
    return plugins;
}

TorrentBackend *backendOf(const QList<NetworkPlugin *> &plugins)
{
    for (NetworkPlugin *p : plugins)
        if (auto *b = dynamic_cast<TorrentBackend *>(p)) return b;
    return nullptr;
}

void applySettings(Settings *st, const QString &profile, const QVariantMap &json, const QStringList &sets)
{
    st->setValue(QString(keys::transfers::IncomingDir), profile + QStringLiteral("/incoming"));
    for (auto it = json.constBegin(); it != json.constEnd(); ++it) st->setValue(it.key(), it.value());
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
}

void setLimits(Settings *st, int d, int u)   // as the UI does (remembered value + key)
{
    if (d >= 0) { if (d > 0) st->setValue(QString(keys::bandwidth::LastDownLimitKBs), d); st->setValue(QString(keys::bandwidth::DownLimitKBs), d); }
    if (u >= 0) { if (u > 0) st->setValue(QString(keys::bandwidth::LastUpLimitKBs), u); st->setValue(QString(keys::bandwidth::UpLimitKBs), u); }
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QStringList args = app.arguments().mid(1);
    QString profile, text = QStringLiteral("big buck bunny"), settingsFile, onlyBtih;
    QStringList sets;
    int searchWait = 75, seconds = 240, down = 0, up = 0, limitAt = -1, restartAt = -1;
    qint64 minSize = 50 * 1024 * 1024;
    for (int i = 0; i < args.size(); ++i) {
        const QString a = args.at(i);
        auto next = [&] { return i + 1 < args.size() ? args.at(++i) : QString(); };
        if (a == QLatin1String("--profile")) profile = next();
        else if (a == QLatin1String("--text")) text = next();
        else if (a == QLatin1String("--search-wait")) searchWait = next().toInt();
        else if (a == QLatin1String("--seconds")) seconds = next().toInt();
        else if (a == QLatin1String("--down")) down = next().toInt();
        else if (a == QLatin1String("--up")) up = next().toInt();
        else if (a == QLatin1String("--limit-at")) limitAt = next().toInt();
        else if (a == QLatin1String("--restart-at")) restartAt = next().toInt();
        else if (a == QLatin1String("--settings")) settingsFile = next();
        else if (a == QLatin1String("--set")) sets << next();
        else if (a == QLatin1String("--min-size")) minSize = next().toLongLong();
        else if (a == QLatin1String("--only-btih")) onlyBtih = next().toLower();
        else if (a == QLatin1String("--out")) {
            g_outFile = new QFile(next());
            if (!g_outFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) { delete g_outFile; g_outFile = nullptr; }
        }
    }
    const QString lower = text.toLower();
    if (profile.isEmpty() || !(lower.contains(QLatin1String("big buck bunny")) || lower.contains(QLatin1String("ubuntu")) || lower.contains(QLatin1String("debian")))) {
        std::fprintf(stderr, "usage: see the head of tests/engine/join_probe.cpp (Big Buck Bunny, Ubuntu or Debian only)\n");
        return 2;
    }
    profile = QDir(profile).absolutePath();
    QDir().mkpath(profile);
    QVariantMap json;
    if (!settingsFile.isEmpty()) {
        QFile f(settingsFile);
        if (f.open(QIODevice::ReadOnly)) json = QJsonDocument::fromJson(f.readAll()).object().toVariantMap();
    }
    registerMetaTypes();
    installLogHandler(profile, 50 * 1024 * 1024, 3, false, 1);

    QList<NetworkPlugin *> plugins = makePlugins();
    Core *core = createRealCore(profile, plugins);
    applySettings(core->settings(), profile, json, sets);
    if (limitAt < 0) setLimits(core->settings(), down, up);
    else setLimits(core->settings(), 0, 0);
    core->start();
    say(QStringLiteral("join_probe: %1 plugins, limits down %2 up %3 KB/s %4").arg(plugins.size()).arg(down).arg(up)
            .arg(limitAt < 0 ? QStringLiteral("before the start") : QStringLiteral("%1 s after the start").arg(limitAt)));

    // ---- search, wait, pick
    pump(15000);   // the networks connect
    SearchQuery q;
    q.text = text;
    q.minSize = minSize;
    const quint32 sid = core->search()->startSearch(q);
    bool finished = false;
    QObject::connect(core->search(), &SearchService::searchFinished, core, [&](quint32 x) { if (x == sid) finished = true; });
    waitFor([&] { return finished; }, qint64(searchWait) * 1000);
    const QList<SearchHit> hits = core->search()->hits(sid);
    Pick best;
    int listed = 0;
    // every torrent the search knows for the words: which one is the healthy swarm, which one a row hints
    for (const SearchHit &h : hits) {
        if (h.size < minSize || !h.fileName.toLower().contains(lower.section(QLatin1Char(' '), 0, 0))) continue;
        QStringList nl;
        for (const SearchSource &s : h.sources) if (!nl.contains(netShortName(s.net))) nl << netShortName(s.net);
        say(QStringLiteral("hit \"%1\" %2 bytes%3 nets %4 key %5").arg(h.fileName).arg(h.size)
                .arg(h.isBundle ? QStringLiteral(" (folder, %1 files)").arg(h.files.size()) : QString()).arg(nl.join(QLatin1Char('+'))).arg(h.key.left(24)));
        for (const SearchSource &s : h.sources) {
            if (s.net != Net::BitTorrent) continue;
            const hash::ParsedLink l = hash::parseMagnet(s.uri);
            say(QStringLiteral("    bt %1 seeds %2 peers %3 via %4%5%6 trackers %7%8 so=%9")
                    .arg(QString::fromLatin1(l.hashes.btih.toHex().left(8))).arg(s.seeds).arg(s.peers)
                    .arg(s.extra.value(QStringLiteral("providerName")).toString(), s.connection.isEmpty() ? QString() : QStringLiteral("/") + s.connection)
                    .arg(s.extra.contains(QStringLiteral("innerOf")) ? QStringLiteral(" inside \"%1\"").arg(s.extra.value(QStringLiteral("innerOf")).toString().left(40)) : QString())
                    .arg(l.trackers.size())
                    .arg(s.extra.value(QStringLiteral("candidate")).toBool() ? QStringLiteral(" candidate") : QString())
                    .arg(s.extra.value(QStringLiteral("btFileIndex")).toString()));
        }
    }
    for (const SearchHit &h : hits) {
        if (h.isBundle || h.size < minSize) continue;
        bool nameOk = true;
        for (const QString &w : lower.split(QLatin1Char(' '), Qt::SkipEmptyParts))
            if (!h.fileName.toLower().contains(w)) nameOk = false;
        if (!nameOk) continue;
        int other = 0, btPeers = 0, inside = 0;
        QSet<QString> nets;
        for (const SearchSource &s : h.sources) {
            nets.insert(netShortName(s.net));
            if (isOther(s.net)) ++other;
            if (s.net == Net::BitTorrent) {
                btPeers = qMax(btPeers, qMax(0, s.seeds) + qMax(0, s.peers));
                if (s.extra.contains(QStringLiteral("innerOf")) || s.extra.value(QStringLiteral("candidate")).toBool()) ++inside;
            }
        }
        if (!other || !inside) continue;
        QStringList nl = nets.values();
        std::sort(nl.begin(), nl.end());
        const QString line = QStringLiteral("  row \"%1\" %2 bytes nets %3, %4 other-network sources, %5 BitTorrent candidate(s), swarm %6")
                                 .arg(h.fileName).arg(h.size).arg(nl.join(QLatin1Char('+'))).arg(other).arg(inside).arg(btPeers);
        if (listed++ < 8) say(line);
        const int score = btPeers * 10 + other;
        if (score > best.score) { best.score = score; best.hit = h; best.why = line; }
    }
    say(QStringLiteral("search: %1 hits, %2 merged rows with a BitTorrent candidate").arg(hits.size()).arg(listed));
    if (best.score < 0) {
        say(QStringLiteral("RESULT {\"picked\":false}"));
        core->shutdown();
        waitFor([] { return false; }, 3000);
        return 1;
    }
    say(QStringLiteral("picked:%1").arg(best.why.mid(5)));
    if (!onlyBtih.isEmpty()) {
        // keep only that torrent of the row: the row then looks like the picture runs where the
        // plain file was the identity and that torrent the only candidate
        QList<SearchSource> keep;
        for (const SearchSource &s : std::as_const(best.hit.sources)) {
            if (s.net != Net::BitTorrent) { keep << s; continue; }
            if (QString::fromLatin1(hash::parseMagnet(s.uri).hashes.btih.toHex()).startsWith(onlyBtih)) keep << s;
        }
        best.hit.sources = keep;
        if (!QString::fromLatin1(best.hit.hashes.btih.toHex()).startsWith(onlyBtih)) {
            best.hit.hashes.btih.clear();
            best.hit.hashes.btFileIndex = -1;
            bool identity = false;
            for (SearchSource &s : best.hit.sources) {
                if (s.net == Net::BitTorrent || !s.extra.value(QStringLiteral("candidate")).toBool()) continue;
                const FileHashes h = FileHashes::fromVariant(s.extra.value(QStringLiteral("hashes")).toMap());
                if (!identity) { best.hit.hashes.mergeFrom(h); identity = true; }
                if (best.hit.hashes.matches(h)) { s.extra.remove(QStringLiteral("candidate")); s.extra.remove(QStringLiteral("hashes")); }
            }
        }
        bool kept = false;
        for (const SearchSource &s : std::as_const(best.hit.sources)) if (s.net == Net::BitTorrent) kept = true;
        if (!kept) {
            say(QStringLiteral("RESULT {\"picked\":false,\"why\":\"torrent %1 is not in this search's row\"}").arg(onlyBtih));
            core->shutdown();
            waitFor([] { return false; }, 3000);
            return 1;
        }
        core->search()->closeSearch(sid);   // the engine follows a live hit: it would bring the others back
        say(QStringLiteral("only torrent %1 kept in the row").arg(onlyBtih));
    }

    // ---- download and watch
    const QString id = core->transfers()->downloadHit(best.hit);
    QElapsedTimer clock;
    clock.start();
    say(QStringLiteral("download %1 started").arg(id));
    qint64 metaMs = -1, firstBtMs = -1;
    QVariantMap at180;
    bool limited = limitAt < 0, restarted = false;
    QString lastRow;
    int lastPrint = -5;
    auto engineOf = [&] { return qobject_cast<DownloadEngine *>(core->transfers()); };
    while (clock.elapsed() < qint64(seconds) * 1000) {
        pump(1000);
        const int t = int(clock.elapsed() / 1000);
        if (!limited && t >= limitAt) {
            setLimits(core->settings(), down, up);
            limited = true;
            say(QStringLiteral("[%1s] limits set: down %2 up %3 KB/s").arg(t).arg(down).arg(up));
        }
        if (restartAt > 0 && !restarted && t >= restartAt) {
            restarted = true;
            say(QStringLiteral("[%1s] restarting the core").arg(t));
            bool done = false;
            QObject::connect(core, &Core::shutdownFinished, &app, [&] { done = true; });
            core->shutdown();
            waitFor([&] { return done; }, 8000);
            delete core;
            plugins = makePlugins();
            core = createRealCore(profile, plugins);
            applySettings(core->settings(), profile, json, sets);
            setLimits(core->settings(), limited ? down : 0, limited ? up : 0);
            core->start();
            continue;
        }
        DownloadEngine *engine = engineOf();
        TorrentBackend *be = backendOf(plugins);
        const TransferInfo ti = core->transfers()->transfer(id);
        const QVariantMap dbg = engine ? engine->debugInfo(id) : QVariantMap();
        bool anyMeta = false;
        QStringList tor;
        if (be) {
            for (const QString &tid : be->torrents()) {
                const TorrentSnapshot sn = be->snapshot(tid);
                const TorrentDiag dg = be->torrentDiagnostics(tid);
                anyMeta = anyMeta || sn.hasMetadata;
                int trWorking = 0, trFail = 0, trPeers = 0;
                for (const TrackerDiag &tr : dg.trackers) {
                    if (tr.working) ++trWorking;
                    if (tr.fails > 0) ++trFail;
                    trPeers += qMax(0, tr.peersReturned);
                }
                tor << QStringLiteral("%1 meta=%2 paused=%3 queued=%4 known=%5 conn=%6 seeds=%7 att=%8 from[tr=%9 dht=%10 pex=%11 in=%12] dht[lookups=%13 peers=%14 nodes=%15] trackers[%16 ok %17 failing of %18, %19 peers] fail[to=%20 ref=%21 hs=%22 other=%23] %24 B/s \"%25\"")
                           .arg(tid.left(8)).arg(sn.hasMetadata).arg(dg.paused).arg(dg.queued).arg(dg.knownPeers).arg(dg.connectedNow)
                           .arg(dg.seedsConnected).arg(dg.connectAttempts).arg(dg.fromTrackers).arg(dg.fromDht).arg(dg.fromPex)
                           .arg(dg.fromIncoming).arg(dg.dhtLookups).arg(dg.dhtPeers).arg(dg.dhtNodes).arg(trWorking).arg(trFail)
                           .arg(dg.trackers.size()).arg(trPeers).arg(dg.failedTimeout).arg(dg.failedRefused).arg(dg.failedHandshake)
                           .arg(dg.failedOther).arg(sn.rateDownBps).arg(sn.statusText);
            }
        }
        if (anyMeta && metaMs < 0) metaMs = clock.elapsed();
        const qint64 btBytes = dbg.value(QStringLiteral("btBytesEstimate")).toLongLong()
                               + dbg.value(QStringLiteral("verifiedByNet")).toMap().value(QStringLiteral("BitTorrent")).toLongLong();
        if (btBytes > 0 && firstBtMs < 0) firstBtMs = clock.elapsed();
        QString btRow;
        for (const TransferSource &s : ti.sources)
            if (s.net == Net::BitTorrent) { btRow = s.user.name + QStringLiteral(": ") + s.statusText; break; }
        QStringList nets;
        for (Net n : ti.networks) nets << netShortName(n);
        QStringList byNet;
        const QVariantMap v = dbg.value(QStringLiteral("verifiedByNet")).toMap();
        for (auto it = v.constBegin(); it != v.constEnd(); ++it) byNet << QStringLiteral("%1=%2").arg(it.key(), mb(it->toLongLong()));
        if (btBytes > 0) byNet << QStringLiteral("BT=%1").arg(mb(btBytes));
        int btBudget = -1;
        if (auto *rc = dynamic_cast<RealCore *>(core))
            if (rc->rateLimiter()) btBudget = rc->rateLimiter()->budgetBps(RateLimiter::Down, Net::BitTorrent);
        if (t >= 180 && at180.isEmpty()) {
            at180 = v;
            at180.insert(QStringLiteral("BitTorrent"), btBytes);
            at180.insert(QStringLiteral("done"), ti.done);
        }
        if (btRow != lastRow || t - lastPrint >= 5) {
            lastPrint = t;
            lastRow = btRow;
            say(QStringLiteral("[%1s] %2 | %3 of %4 MB | nets %5 | store %6 | bt budget %7 | %8 | BT row: %9")
                    .arg(t, 3).arg(ti.statusText).arg(mb(ti.done), mb(ti.size)).arg(nets.join(QLatin1Char('+')))
                    .arg(dbg.value(QStringLiteral("store")).toString()).arg(btBudget).arg(byNet.join(QLatin1Char(' ')))
                    .arg(btRow.isEmpty() ? QStringLiteral("(none)") : btRow));
            for (const QString &l : tor) say(QStringLiteral("       torrent %1").arg(l));
        }
        if (ti.state == TransferState::Completed) break;
    }
    if (at180.isEmpty()) {
        const QVariantMap dbg = engineOf() ? engineOf()->debugInfo(id) : QVariantMap();
        at180 = dbg.value(QStringLiteral("verifiedByNet")).toMap();
        at180.insert(QStringLiteral("BitTorrent"), dbg.value(QStringLiteral("btBytesEstimate")).toLongLong());
        at180.insert(QStringLiteral("done"), core->transfers()->transfer(id).done);
    }
    QJsonObject r;
    r.insert(QStringLiteral("picked"), true);
    r.insert(QStringLiteral("down"), down);
    r.insert(QStringLiteral("up"), up);
    r.insert(QStringLiteral("limitAt"), limitAt);
    r.insert(QStringLiteral("metaS"), metaMs < 0 ? -1.0 : metaMs / 1000.0);
    r.insert(QStringLiteral("firstBtS"), firstBtMs < 0 ? -1.0 : firstBtMs / 1000.0);
    r.insert(QStringLiteral("at180"), QJsonObject::fromVariantMap(at180));
    say(QStringLiteral("RESULT %1").arg(QString::fromUtf8(QJsonDocument(r).toJson(QJsonDocument::Compact))));
    core->transfers()->cancel(id, true);   // and delete the data
    pump(1500);
    bool done = false;
    QObject::connect(core, &Core::shutdownFinished, &app, [&] { done = true; });
    core->shutdown();
    waitFor([&] { return done; }, 8000);
    delete core;
    uninstallLogHandler();
    QDir(profile + QStringLiteral("/incoming")).removeRecursively();
    QDir(profile + QStringLiteral("/incomplete")).removeRecursively();
    return 0;
}
