// SPDX-License-Identifier: GPL-3.0-or-later
// mxcli: headless OctoMX: the REAL core (mx::createRealCore) + every network plugin that exists
// in this build tree, on a --profile directory, no GUI. For end-to-end runs on the live internet.
//
//   mxcli --profile DIR status [--wait S]
//   mxcli --profile DIR search <words...> [--wait S] [--type audio|video|...] [--max N]
//   mxcli --profile DIR get <n | magnet:... | ed2k://... | http(s)://... | file.torrent>
//                           [--source URL]... [--until-done | --until-bytes N] [--timeout S] [--every S]
//   mxcli --profile DIR list | info <id> | pause <id> | resume <id> | cancel <id> | wait <S>
//   mxcli --profile DIR verify <id|last> [--timeout S] | restart <id|last>   ([ver-1] Verify File / Restart Download)
//   mxcli --profile DIR maplog <id|last> <file.jsonl> [--every S] [--timeout S] [--until-done]
//                           ([ui-i] the Transfers bar map + the ranges it is made of, one JSON line per sample)
//   mxcli --profile DIR diag [--wait S] [--test] [--ports-online] [--show-ips] [--out FILE]
//                           (the Connection Test report; [diag-1])
//   mxcli --profile DIR --script FILE      (one command per line, same syntax, '#' comments)
// `get <n>` uses hit n of the last `search` (kept in <profile>/mxcli-lastsearch.json).
// Progress lines show per-network verified byte counts (engine debugInfo).
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QProcess>
#include <QtCore/QTextStream>
#include <QtCore/QTimer>
#include <QtCore/QMap>
#include <QtCore/QSet>

#include <cstdio>
#include <functional>
#include <memory>

#include "core/engine/engine.h"
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
QTextStream out(stdout);

void say(const QString &s)
{
    out << s << Qt::endl;
}

bool waitFor(const std::function<bool()> &pred, qint64 ms)
{
    QElapsedTimer t;
    t.start();
    while (!pred()) {
        if (ms >= 0 && t.elapsed() >= ms) return false;
        QEventLoop loop;
        QTimer::singleShot(100, &loop, &QEventLoop::quit);
        loop.exec();
    }
    return true;
}

QString human(qint64 b)
{
    if (b < 0) return QStringLiteral("?");
    if (b < 1024) return QStringLiteral("%1 B").arg(b);
    if (b < (1 << 20)) return QStringLiteral("%1 KiB").arg(b / 1024.0, 0, 'f', 1);
    if (b < (qint64(1) << 30)) return QStringLiteral("%1 MiB").arg(b / 1048576.0, 0, 'f', 1);
    return QStringLiteral("%1 GiB").arg(b / 1073741824.0, 0, 'f', 2);
}

QJsonObject toJson(const SearchHit &h)
{
    QJsonObject o;
    o.insert(QStringLiteral("name"), h.fileName);
    o.insert(QStringLiteral("size"), double(h.size));
    o.insert(QStringLiteral("hashes"), QJsonObject::fromVariantMap(h.hashes.toVariant()));
    o.insert(QStringLiteral("bundle"), h.isBundle);
    o.insert(QStringLiteral("btFileIndex"), h.hashes.btFileIndex);
    QJsonArray srcs;
    for (const SearchSource &s : h.sources) {
        QJsonObject j;
        j.insert(QStringLiteral("net"), int(s.net));
        j.insert(QStringLiteral("uri"), s.uri);
        j.insert(QStringLiteral("uid"), s.user.id);
        j.insert(QStringLiteral("uname"), s.user.name);
        j.insert(QStringLiteral("uaddr"), s.user.address);
        j.insert(QStringLiteral("unet"), int(s.user.net));
        j.insert(QStringLiteral("seeds"), s.seeds);
        j.insert(QStringLiteral("peers"), s.peers);
        j.insert(QStringLiteral("conn"), s.connection);
        QVariantMap ex;
        for (auto it = s.extra.constBegin(); it != s.extra.constEnd(); ++it)
            if (it.value().canConvert<QString>() || it.value().typeId() == QMetaType::Bool || it.value().typeId() == QMetaType::QVariantMap)
                ex.insert(it.key(), it.value());
        j.insert(QStringLiteral("extra"), QJsonObject::fromVariantMap(ex));
        srcs.append(j);
    }
    o.insert(QStringLiteral("sources"), srcs);
    // [merge-1] file lists and the inner files other networks have too
    QJsonArray files;
    for (const BundleFile &f : h.files)
        files.append(QJsonObject{{QStringLiteral("path"), f.path}, {QStringLiteral("size"), double(f.size)}, {QStringLiteral("index"), f.index}});
    if (!files.isEmpty()) o.insert(QStringLiteral("files"), files);
    QJsonArray inner;
    for (const SearchHit::InnerMatch &m : h.innerMatches) {
        QStringList nets;
        for (Net n : m.networks) nets << netShortName(n);
        inner.append(QJsonObject{{QStringLiteral("path"), m.path}, {QStringLiteral("size"), double(m.size)}, {QStringLiteral("file"), m.file},
                                 {QStringLiteral("row"), m.rowKey}, {QStringLiteral("nets"), nets.join(QLatin1Char(','))}});
    }
    if (!inner.isEmpty()) o.insert(QStringLiteral("inner"), inner);
    o.insert(QStringLiteral("key"), h.key);
    o.insert(QStringLiteral("peers"), h.totalPeers);
    return o;
}

SearchHit fromJson(const QJsonObject &o)
{
    SearchHit h;
    h.fileName = o.value(QStringLiteral("name")).toString();
    h.size = qint64(o.value(QStringLiteral("size")).toDouble());
    h.hashes = FileHashes::fromVariant(o.value(QStringLiteral("hashes")).toObject().toVariantMap());
    h.hashes.btFileIndex = o.value(QStringLiteral("btFileIndex")).toInt(-1);
    h.isBundle = o.value(QStringLiteral("bundle")).toBool();
    for (const QJsonValue &v : o.value(QStringLiteral("sources")).toArray()) {
        const QJsonObject j = v.toObject();
        SearchSource s;
        s.net = Net(j.value(QStringLiteral("net")).toInt());
        s.uri = j.value(QStringLiteral("uri")).toString();
        s.user.id = j.value(QStringLiteral("uid")).toString();
        s.user.name = j.value(QStringLiteral("uname")).toString();
        s.user.address = j.value(QStringLiteral("uaddr")).toString();
        s.user.net = Net(j.value(QStringLiteral("unet")).toInt());
        s.seeds = j.value(QStringLiteral("seeds")).toInt(-1);
        s.peers = j.value(QStringLiteral("peers")).toInt(-1);
        s.connection = j.value(QStringLiteral("conn")).toString();
        s.extra = j.value(QStringLiteral("extra")).toObject().toVariantMap();
        h.sources << s;
    }
    return h;
}

class Cli {
public:
    Cli(Core *core, const QString &profile, const QList<NetworkPlugin *> &plugins) : core(core), plugins(plugins), profile(profile)
    {
        engine = qobject_cast<DownloadEngine *>(core->transfers());
    }
    Core *core;
    QList<NetworkPlugin *> plugins;
    DownloadEngine *engine = nullptr;
    QString profile;
    QList<SearchHit> lastHits;
    bool showRows = false;   // [engine-4] get/watch --rows: print the Transfers-tree rows of the download
    bool showMap = false;    // [dl-1] get --map: the engine's block map (64 cells) + prefix / tail per line
    qint64 previewAt = -1;   // [dl-1] get --preview-at <bytes>: preparePreview() once that much is done
    bool previewDone = false;

    int run(QStringList a);

private:
    QString opt(QStringList &a, const QString &name, const QString &def = QString())
    {
        const int i = a.indexOf(name);
        if (i < 0 || i + 1 >= a.size()) return def;
        const QString v = a.at(i + 1);
        a.removeAt(i);
        a.removeAt(i);
        return v;
    }
    QStringList opts(QStringList &a, const QString &name)
    {
        QStringList l;
        for (QString v = opt(a, name); !v.isEmpty(); v = opt(a, name)) l << v;
        return l;
    }
    bool flag(QStringList &a, const QString &name) { return a.removeAll(name) > 0; }
    int status(QStringList a);
    int search(QStringList a);
    int get(QStringList a);
    int list();
    int diag(QStringList a);   // [diag-1]
    void progressLine(const QString &id, qint64 t0);
    TorrentBackend *backend() const
    {
        for (NetworkPlugin *p : plugins) if (auto *b = dynamic_cast<TorrentBackend *>(p)) return b;
        return nullptr;
    }
    bool addPeer(const QString &btihHex, const QString &endpoint, int waitMs);
    int hashesCmd();
    int waitHashed(QStringList a);
    int seed(QStringList a);
};

int Cli::status(QStringList a)
{
    const int wait = opt(a, QStringLiteral("--wait"), QStringLiteral("8")).toInt();
    waitFor([] { return false; }, qint64(wait) * 1000);
    for (const NetworkStatus &s : core->network()->networks())
        say(QStringLiteral("%1 %2 peers=%3 %4").arg(netName(s.net), -10).arg(int(s.state)).arg(s.peers).arg(s.statusText));
    say(QStringLiteral("overall: %1, %2").arg(core->network()->overallHeadline(), core->network()->titleSummary()));
    // [core-5] the numbers behind "Primary Connections" and, with --servers, the protocol rows
    const NetworkSummary sum = core->network()->summary();
    QStringList by;
    for (auto it = sum.primaryByNet.constBegin(); it != sum.primaryByNet.constEnd(); ++it)
        by << QStringLiteral("%1=%2 (raw peers %3)").arg(netName(it.key())).arg(it.value()).arg(sum.peersByNet.value(it.key()));
    by.sort();
    say(QStringLiteral("primary: %1 = %2; users online: %3").arg(sum.primary).arg(by.join(QStringLiteral(", "))).arg(sum.usersOnline));
    if (flag(a, QStringLiteral("--servers"))) {
        for (const NetworkStatus &s : core->network()->networks()) {
            int n = 0;
            for (const ServerEntry &e : core->network()->servers(s.net)) {
                if (++n > 6) break;
                say(QStringLiteral("  [%1] %2 state=%3 users=%4 nodes=%5 files=%6 | %7").arg(netName(e.net), e.name).arg(int(e.state))
                        .arg(e.users).arg(e.nodes).arg(e.files).arg(e.description));
            }
        }
    }
    return 0;
}

int Cli::search(QStringList a)
{
    const int wait = opt(a, QStringLiteral("--wait"), QStringLiteral("25")).toInt();
    const int max = opt(a, QStringLiteral("--max"), QStringLiteral("30")).toInt();
    const QString type = opt(a, QStringLiteral("--type"));
    const int details = opt(a, QStringLiteral("--details"), QStringLiteral("0")).toInt();
    const int warm = opt(a, QStringLiteral("--warmup"), QStringLiteral("6")).toInt();
    const QString saveAs = opt(a, QStringLiteral("--save"));   // [engine-4] keep this result list (get N --from FILE)
    SearchQuery q;
    q.text = a.join(QLatin1Char(' '));
    // [ui-f] any file-type code of the Search menu (any, audio, audio:min:192, video, documents,
    // software, archives, mp3, lossless, custom:mkv,srt ...)
    if (!type.isEmpty()) {
        const FileTypeFilter tf = FileTypeFilter::fromCode(type);
        if (!tf.isValid()) { say(QStringLiteral("unknown --type %1").arg(type)); return 2; }
        tf.applyTo(q);
    }
    waitFor([this] { return core->network()->overallState() == ConnState::Connected; }, qint64(warm) * 1000);
    bool finished = false;
    QMap<QString, QString> progressByNet;   // [ui-f] the last "eD2k: 12 results" line of each network
    quint32 id = 0;
    const QMetaObject::Connection pc = QObject::connect(core->search(), &SearchService::searchProgress, core,
                                                        [&](quint32 x, Net n, const QString &text) {
                                                            if (x == id && n != Net::Unknown) progressByNet[netShortName(n)] = text;
                                                        });
    id = core->search()->startSearch(q);
    QObject::connect(core->search(), &SearchService::searchFinished, core, [&](quint32 x) { if (x == id) finished = true; });
    waitFor([&] { return finished; }, qint64(wait) * 1000);
    lastHits = core->search()->hits(id);
    std::stable_sort(lastHits.begin(), lastHits.end(), [](const SearchHit &x, const SearchHit &y) {
        auto score = [](const SearchHit &h) { int s = 0; for (const SearchSource &src : h.sources) s += qMax(1, src.seeds); return s; };
        return score(x) > score(y);
    });
    QJsonArray arr;
    for (const SearchHit &h : std::as_const(lastHits)) arr.append(toJson(h));
    QFile f(profile + QStringLiteral("/mxcli-lastsearch.json"));
    if (f.open(QIODevice::WriteOnly)) f.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    if (!saveAs.isEmpty()) {
        QFile g(saveAs);
        if (g.open(QIODevice::WriteOnly)) g.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    }
    say(QStringLiteral("search \"%1\": %2 merged hits%3").arg(q.text).arg(lastHits.size()).arg(finished ? QString() : QStringLiteral(" (still running)")));
    {
        // [ui-f] hits per network, and whether any listed hit contradicts the file type
        QMap<QString, int> perNet;
        int contradict = 0, unknown = 0;
        for (const SearchHit &h : std::as_const(lastHits)) {
            QSet<QString> nets;
            for (const SearchSource &src : h.sources) nets.insert(netShortName(src.net));
            for (const QString &n : nets) ++perNet[n];
            if (!hitMatchesFileType(q, h)) ++contradict;
            if (classifyHit(h) == MediaType::Other) ++unknown;
        }
        QStringList parts;
        for (auto it = perNet.constBegin(); it != perNet.constEnd(); ++it) parts << QStringLiteral("%1=%2").arg(it.key()).arg(it.value());
        QObject::disconnect(pc);
        QStringList prog = progressByNet.values();
        say(QStringLiteral("  accepted per network: %1").arg(prog.join(QStringLiteral("; "))));
        say(QStringLiteral("  type %1: merged hits per network %2; contradicting the type: %3; type unknown (kept): %4")
                .arg(type.isEmpty() ? QStringLiteral("any") : type, parts.join(QLatin1Char(' '))).arg(contradict).arg(unknown));
    }
    say(QStringLiteral("  #  %1 %2 %3  name").arg(QStringLiteral("size"), 10).arg(QStringLiteral("srcs"), 5).arg(QStringLiteral("networks"), -22));
    for (int i = 0; i < lastHits.size() && i < max; ++i) {
        const SearchHit &h = lastHits.at(i);
        QStringList nets;
        int seeds = 0;
        for (const SearchSource &s : h.sources) {
            QString n = netShortName(s.net);
            if (s.extra.contains(QStringLiteral("innerOf"))) n += QStringLiteral("(in)");   // [merge-1] inside a folder torrent
            if (s.extra.value(QStringLiteral("candidate")).toBool()) n += QLatin1Char('?');
            if (!nets.contains(n)) nets << n;
            seeds += qMax(0, s.seeds);
        }
        say(QStringLiteral("%1  %2 %3 %4  %5%6").arg(i + 1, 3).arg(human(h.size), 10).arg(h.sources.size(), 5)
                .arg(nets.join(QLatin1Char(',')), -22).arg(h.fileName.left(70), h.isBundle ? QStringLiteral(" [bundle]") : QString())
            + (seeds ? QStringLiteral("  (%1 seeds)").arg(seeds) : QString())
            + (h.innerMatches.isEmpty() ? QString() : QStringLiteral("  [%1 inner file(s) also elsewhere]").arg(h.innerMatches.size())));
        if (i < details) {
            QStringList kinds;
            const QVariantMap hv = h.hashes.toVariant();
            for (auto it = hv.constBegin(); it != hv.constEnd(); ++it)
                if (it.key() != QLatin1String("btFileIndex")) kinds << QStringLiteral("%1=%2").arg(it.key(), it.value().toString().left(16));
            say(QStringLiteral("       hashes: %1").arg(kinds.join(QLatin1Char(' '))));
            for (const SearchSource &src : h.sources)
                say(QStringLiteral("       source: %1 user=\"%2\" id=%3 %4").arg(netName(src.net), src.user.name, src.user.id.left(16), src.uri.left(60)));
        }
    }
    return 0;
}

void Cli::progressLine(const QString &id, qint64 t0)
{
    const TransferInfo t = core->transfers()->transfer(id);
    const QVariantMap dbg = engine ? engine->debugInfo(id) : QVariantMap();
    QStringList nets;
    for (Net n : t.networks) nets << netShortName(n);
    QStringList byNet;
    const QVariantMap v = dbg.value(QStringLiteral("verifiedByNet")).toMap();
    for (auto it = v.constBegin(); it != v.constEnd(); ++it) byNet << QStringLiteral("%1=%2").arg(it.key(), human(it->toLongLong()));
    QString extra;
    if (dbg.value(QStringLiteral("store")).toString() == QLatin1String("torrent")) {
        extra = QStringLiteral(" | BT≈%1 injected=%2 pieces (%3) reserved=%4 peers=%5")
                    .arg(human(dbg.value(QStringLiteral("btBytesEstimate")).toLongLong()))
                    .arg(dbg.value(QStringLiteral("injectedPieces")).toInt())
                    .arg(human(dbg.value(QStringLiteral("injectedBytes")).toLongLong()))
                    .arg(dbg.value(QStringLiteral("reservedPieces")).toInt())
                    .arg(dbg.value(QStringLiteral("btPeers")).toInt());
    }
    const int pct = t.size > 0 ? int(t.done * 100 / t.size) : 0;
    say(QStringLiteral("[%1s] %2 %3% %4/%5 %6/s eta %7 nets=%8 srcs=%9/%10 %11%12")
            .arg((QDateTime::currentMSecsSinceEpoch() - t0) / 1000, 5)
            .arg(t.statusText, -22).arg(pct, 3).arg(human(t.done), human(t.size), human(t.rateDownBps))
            .arg(t.etaSec >= 0 ? QString::number(t.etaSec) + QLatin1Char('s') : QStringLiteral("-"))
            .arg(nets.join(QLatin1Char('+')))
            .arg(t.sourcesActive).arg(t.sourcesTotal)
            .arg(byNet.isEmpty() ? QString() : QStringLiteral("verified[") + byNet.join(QLatin1Char(' ')) + QLatin1Char(']'), extra));
    if (!t.diagnosis.isEmpty()) say(QStringLiteral("         why: [%1] %2").arg(diagLevelName(DiagLevel(t.diagnosisLevel)), t.diagnosis));   // [diag-1]
    if (showMap) {   // [dl-1] '#' = cell complete, '+' = partly, '.' = nothing (store coordinates)
        QList<QPair<qint64, qint64>> held;
        for (const QVariant &x : dbg.value(QStringLiteral("heldRanges")).toList())
            held << qMakePair(x.toList().value(0).toLongLong(), x.toList().value(1).toLongLong());
        qint64 total = 0;
        for (const auto &r : held) total = qMax(total, r.second);
        const qint64 ext = t.isBundle ? qMax(total, t.size) : t.size;
        QString map;
        for (int c = 0; c < 64 && ext > 0; ++c) {
            const qint64 a = ext * c / 64, b = ext * (c + 1) / 64;
            qint64 cov = 0;
            for (const auto &r : held) cov += qMax<qint64>(0, qMin(b, r.second) - qMax(a, r.first));
            map += cov >= b - a ? QLatin1Char('#') : cov > 0 ? QLatin1Char('+') : QLatin1Char('.');
        }
        const qint64 prefix = (!held.isEmpty() && held.first().first == 0) ? held.first().second : 0;
        const qint64 tail = (!held.isEmpty() && held.last().second >= ext) ? ext - held.last().first : 0;
        qint64 have = 0;
        for (const auto &r : held) have += r.second - r.first;
        say(QStringLiteral("         map [%1] ranges=%2 prefix=%3 tail=%4 held=%5 order=%6%7")
                .arg(map).arg(held.size()).arg(human(prefix), human(tail), human(have))
                .arg(t.sequential ? QStringLiteral("seq") : QStringLiteral("-"), t.firstLastFirst ? QStringLiteral("+fl") : QString()));
    }
    if (previewAt >= 0 && !previewDone && t.done >= previewAt) {   // [dl-1]
        QString err;
        const PreviewInfo pi = core->transfers()->previewInfo(id);
        int idx = -1;
        for (const PreviewFile &f : pi.files) if (f.ready) { idx = f.index; break; }
        const QString path = core->transfers()->preparePreview(id, idx, &err);
        say(path.isEmpty() ? QStringLiteral("preview: not yet (%1)").arg(err) : QStringLiteral("preview: %1").arg(path));
        if (!path.isEmpty()) previewDone = true;
    }
    if (showRows)
        for (const TransferSource &r : t.sources)
            say(QStringLiteral("         | %1 %2 %3 %4 %5").arg(netShortName(r.net), -5).arg(r.user.name.left(28), -28)
                    .arg(r.statusText.left(70), -70).arg(human(r.bytes), 10).arg(r.rateBps > 0 ? human(r.rateBps) + QStringLiteral("/s") : QString()));
}

int Cli::get(QStringList a)
{
    const bool untilDone = flag(a, QStringLiteral("--until-done"));
    if (flag(a, QStringLiteral("--rows"))) showRows = true;
    if (flag(a, QStringLiteral("--map"))) showMap = true;                                      // [dl-1]
    previewAt = opt(a, QStringLiteral("--preview-at"), QStringLiteral("-1")).toLongLong();   // [dl-1]
    const qint64 untilBytes = opt(a, QStringLiteral("--until-bytes"), QStringLiteral("0")).toLongLong();
    const int timeout = opt(a, QStringLiteral("--timeout"), QStringLiteral("600")).toInt();
    const int every = qMax(1, opt(a, QStringLiteral("--every"), QStringLiteral("5")).toInt());
    const QStringList extraSources = opts(a, QStringLiteral("--source"));
    const QStringList peers = opts(a, QStringLiteral("--peer"));
    if (a.isEmpty()) { say(QStringLiteral("get: what?")); return 2; }
    const QString what = a.first();
    QString id;
    bool isNum = false;
    const int n = what.toInt(&isNum);
    QList<SearchSource> extra;
    for (const QString &u : extraSources) {
        SearchSource s;
        s.net = u.startsWith(QLatin1String("ed2k:")) || u.startsWith(QLatin1String("ed2k-peer://")) ? Net::ED2K : Net::Http;
        s.uri = u;
        if (u.startsWith(QLatin1String("gnut://"))) {   // [up-1] a Gnutella servent: N2R by the SHA-1
            s.net = Net::Gnutella;
            s.uri.clear();
            s.user.net = Net::Gnutella;
            s.user.address = u.mid(7);
            s.user.id = s.user.address;
            s.user.name = s.user.address;
        }
        extra << s;
    }
    const QString from = opt(a, QStringLiteral("--from"));   // [engine-4] hits saved by an earlier search
    if (isNum && !from.isEmpty()) {
        lastHits.clear();
        QFile f(from);
        if (f.open(QIODevice::ReadOnly))
            for (const QJsonValue &v : QJsonDocument::fromJson(f.readAll()).array()) lastHits << fromJson(v.toObject());
    }
    if (isNum) {
        if (lastHits.isEmpty()) {
            QFile f(profile + QStringLiteral("/mxcli-lastsearch.json"));
            if (f.open(QIODevice::ReadOnly))
                for (const QJsonValue &v : QJsonDocument::fromJson(f.readAll()).array()) lastHits << fromJson(v.toObject());
        }
        if (n < 1 || n > lastHits.size()) { say(QStringLiteral("get: no hit %1 (last search had %2)").arg(n).arg(lastHits.size())); return 2; }
        SearchHit h = lastHits.at(n - 1);
        h.sources << extra;
        say(QStringLiteral("get: hit %1 \"%2\" %3, %4 sources").arg(n).arg(h.fileName, human(h.size)).arg(h.sources.size()));
        id = core->transfers()->downloadHit(h);
    } else {
        DownloadRequest r;
        r.uri = what;
        r.sources = extra;
        id = core->transfers()->download(r);
    }
    if (id.isEmpty()) { say(QStringLiteral("get: the engine refused the request")); return 1; }
    say(QStringLiteral("download id %1").arg(id));
    for (const QString &ep : peers) {
        const QByteArray btih = core->transfers()->transfer(id).hashes.btih;
        if (btih.size() == 20) addPeer(QString::fromLatin1(btih.toHex()), ep, 20000);
    }
    if (!untilDone && untilBytes <= 0) return 0;
    const qint64 t0 = QDateTime::currentMSecsSinceEpoch();
    qint64 lastLine = 0;
    bool ok = waitFor([&] {
        const TransferInfo t = core->transfers()->transfer(id);
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - lastLine >= every * 1000) { lastLine = now; progressLine(id, t0); }
        if (t.state == TransferState::Completed || t.state == TransferState::Seeding) return true;
        if (t.state == TransferState::Error) return true;
        return untilBytes > 0 && t.done >= untilBytes;
    }, qint64(timeout) * 1000);
    progressLine(id, t0);
    const TransferInfo t = core->transfers()->transfer(id);
    say(QStringLiteral("result: %1, %2 (%3)").arg(ok ? QStringLiteral("reached") : QStringLiteral("TIMEOUT"), t.statusText, t.savePath));
    return ok && t.state != TransferState::Error ? 0 : 1;
}

bool Cli::addPeer(const QString &btihHex, const QString &endpoint, int waitMs)
{
#ifdef MX_HAVE_BT
    TorrentBackend *be = backend();
    auto *ctl = be ? dynamic_cast<BitTorrentControl *>(be) : nullptr;
    if (!ctl) return false;
    if (!waitFor([&] { return be->torrents().contains(btihHex); }, waitMs)) {
        say(QStringLiteral("peer: torrent %1 not in the session").arg(btihHex.left(8)));
        return false;
    }
    ctl->addPeer(btihHex, endpoint);
    say(QStringLiteral("peer: %1 → %2").arg(btihHex.left(8), endpoint));
    return true;
#else
    Q_UNUSED(btihHex) Q_UNUSED(endpoint) Q_UNUSED(waitMs)
    return false;
#endif
}

int Cli::hashesCmd()
{
    for (const SharedFile &f : core->library()->files()) {
        QStringList kinds;
        const QVariantMap hv = f.hashes.toVariant();
        for (auto it = hv.constBegin(); it != hv.constEnd(); ++it)
            if (it.key() != QLatin1String("btFileIndex")) kinds << QStringLiteral("%1=%2").arg(it.key(), it.value().toString());
        say(QStringLiteral("shared: %1 %2 %3 %4").arg(f.name, human(f.size), f.hashed ? QStringLiteral("hashed") : QStringLiteral("NOT hashed"),
                                                       kinds.join(QLatin1Char(' '))));
    }
    return 0;
}

int Cli::waitHashed(QStringList a)
{
    const int count = opt(a, QStringLiteral("--count"), QStringLiteral("1")).toInt();
    const int timeout = opt(a, QStringLiteral("--timeout"), QStringLiteral("300")).toInt();
    QElapsedTimer t;
    t.start();
    const bool ok = waitFor([&] {
        int hashed = 0;
        for (const SharedFile &f : core->library()->files())
            if (f.hashed && !f.hashes.ed2k.isEmpty() && !f.hashes.tth.isEmpty() && !f.hashes.sha1.isEmpty()) ++hashed;
        return hashed >= count && !core->library()->isScanning();
    }, qint64(timeout) * 1000);
    say(QStringLiteral("wait-hashed: %1 after %2 ms").arg(ok ? QStringLiteral("ok") : QStringLiteral("TIMEOUT")).arg(t.elapsed()));
    hashesCmd();
    return ok ? 0 : 1;
}

int Cli::seed(QStringList a)
{
#ifdef MX_HAVE_BT
    TorrentBackend *be = backend();
    if (!be || a.isEmpty()) return 2;
    const QString path = QFileInfo(a.first()).absoluteFilePath();
    QString id;
    auto *ev = qobject_cast<BitTorrentEvents *>(be->torrentEvents());
    QMetaObject::Connection c = QObject::connect(ev, &BitTorrentEvents::seedReady, core, [&](const QString &p, const QString &tid) {
        if (QFileInfo(p).absoluteFilePath() == path) id = tid;
    });
    QString err;
    if (!be->seedLocalFile(path, &err)) { say(QStringLiteral("seed: %1").arg(err)); return 1; }
    waitFor([&] { return !id.isEmpty(); }, 300000);
    QObject::disconnect(c);
    say(QStringLiteral("seeded %1 btih %2").arg(path, id));
    say(QStringLiteral("magnet: magnet:?xt=urn:btih:%1&dn=%2").arg(id, QFileInfo(path).fileName()));
    return id.isEmpty() ? 1 : 0;
#else
    Q_UNUSED(a)
    return 2;
#endif
}

// [diag-1] the Connection Test report: optionally wait for passive evidence, run the active test
// ("Test Now") and the online port check, then print (and save) the plain-text report
int Cli::diag(QStringList a)
{
    DiagnosticsService *d = core->diagnostics();
    if (!d) { say(QStringLiteral("diag: this core has no diagnostics service")); return 2; }
    const double wait = opt(a, QStringLiteral("--wait"), QStringLiteral("0")).toDouble();
    const QString outFile = opt(a, QStringLiteral("--out"), QString());
    const bool test = flag(a, QStringLiteral("--test"));
    const bool online = flag(a, QStringLiteral("--ports-online"));
    const bool showIps = flag(a, QStringLiteral("--show-ips"));
    if (wait > 0) waitFor([] { return false; }, qint64(wait * 1000));
    if (test) {
        bool fin = false;
        QMetaObject::Connection c1 = QObject::connect(d, &DiagnosticsService::testFinished, core, [&](bool) { fin = true; });
        QMetaObject::Connection c2 = QObject::connect(d, &DiagnosticsService::testProgress, core, [](int pct, const QString &step) {
            say(QStringLiteral("  test %1% %2").arg(pct, 3).arg(step));
        });
        d->startTest();
        waitFor([&] { return fin; }, 180000);
        QObject::disconnect(c1);
        QObject::disconnect(c2);
    }
    if (online) {
        say(QStringLiteral("  asking %1 and OctoMX peers about the TCP ports...").arg(d->portCheckService().isEmpty() ? QStringLiteral("(no public checker)") : d->portCheckService()));
        d->checkPortsOnline();
        waitFor([] { return false; }, 25000);
    }
    const QString text = diagReportText(d->report(), !showIps);
    say(text);
    if (!outFile.isEmpty()) {
        QFile f(outFile);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(text.toUtf8());
        say(QStringLiteral("diag: report written to %1").arg(outFile));
    }
    return 0;
}

int Cli::list()
{
    for (const TransferInfo &t : core->transfers()->downloads()) {
        say(QStringLiteral("%1  %2 %3/%4 %5  %6").arg(t.id).arg(t.statusText, -24).arg(human(t.done), human(t.size))
                .arg(t.hashes.btih.isEmpty() ? QStringLiteral("part") : QStringLiteral("bt")).arg(t.name));
    }
    return 0;
}

int Cli::run(QStringList a)
{
    if (a.isEmpty()) return 0;
    const QString cmd = a.takeFirst();
    if (cmd == QLatin1String("status")) return status(a);
    if (cmd == QLatin1String("diag")) return diag(a);   // [diag-1]
    if (cmd == QLatin1String("search")) return search(a);
    if (cmd == QLatin1String("get")) return get(a);
    if (cmd == QLatin1String("list")) return list();
    if (cmd == QLatin1String("share")) {           // share <dir>
        core->library()->addFolder(QFileInfo(a.value(0)).absoluteFilePath(), true);
        say(QStringLiteral("share: %1").arg(QFileInfo(a.value(0)).absoluteFilePath()));
        return 0;
    }
    if (cmd == QLatin1String("wait-hashed")) return waitHashed(a);
    if (cmd == QLatin1String("hashes")) return hashesCmd();
    if (cmd == QLatin1String("seed")) return seed(a);
    if (cmd == QLatin1String("peer")) {            // peer <btih|last> <ip:port>
        QString id = a.value(0);
        if (id == QLatin1String("last"))
            for (const TransferInfo &t : core->transfers()->downloads()) if (!t.hashes.btih.isEmpty()) id = QString::fromLatin1(t.hashes.btih.toHex());
        return addPeer(id, a.value(1), 20000) ? 0 : 1;
    }
    if (cmd == QLatin1String("wait-peers")) {      // wait-peers <net name> <n> [--timeout s]
        const Net n = netFromName(a.value(0));
        const int want = a.value(1).toInt();
        a = a.mid(2);
        const int timeout = opt(a, QStringLiteral("--timeout"), QStringLiteral("60")).toInt();
        QElapsedTimer t;
        t.start();
        const bool ok = waitFor([&] { return core->network()->status(n).peers >= want; }, qint64(timeout) * 1000);
        say(QStringLiteral("wait-peers %1: %2 (%3 peers) after %4 ms").arg(netName(n), ok ? QStringLiteral("ok") : QStringLiteral("TIMEOUT"))
                .arg(core->network()->status(n).peers).arg(t.elapsed()));
        return ok ? 0 : 1;
    }
    if (cmd == QLatin1String("wait-file")) {       // wait-file <path> [--timeout s]: idle until it exists
        const QString f = a.value(0);
        a = a.mid(1);
        const int timeout = opt(a, QStringLiteral("--timeout"), QStringLiteral("600")).toInt();
        waitFor([&] { return QFileInfo::exists(f); }, qint64(timeout) * 1000);
        return 0;
    }
    if (cmd == QLatin1String("wait")) { waitFor([] { return false; }, qint64(a.value(0).toDouble() * 1000)); return 0; }
    if (cmd == QLatin1String("pause") || cmd == QLatin1String("resume")) {   // <id|last> ([ui-i] last)
        QString id = a.value(0);
        if (id == QLatin1String("last") && !core->transfers()->downloads().isEmpty()) id = core->transfers()->downloads().last().id;
        if (cmd == QLatin1String("pause")) core->transfers()->pause(id);
        else core->transfers()->resume(id);
        return 0;
    }
    if (cmd == QLatin1String("verify") || cmd == QLatin1String("restart")) {
        // [ver-1] verify <id|last> [--timeout s]: Verify File, prints the answer; restart <id|last>: Restart Download
        QString id = a.value(0);
        if (id == QLatin1String("last") && !core->transfers()->downloads().isEmpty()) id = core->transfers()->downloads().last().id;
        const int ti = int(a.indexOf(QStringLiteral("--timeout")));
        const qint64 timeout = ti >= 0 ? qint64(a.value(ti + 1).toDouble() * 1000) : 600000;
        QString why, answer;
        const bool can = cmd == QLatin1String("verify") ? core->transfers()->canVerifyDownload(id, &why) : core->transfers()->canRestartDownload(id, &why);
        if (!can) {
            say(QStringLiteral("%1: not now: %2").arg(cmd, why));
            return 1;
        }
        QMetaObject::Connection c = QObject::connect(core->transfers(), &TransferService::downloadNotice, core,
                                                     [&](const QString &nid, const QString &text) { if (nid == id) answer = text; });
        QElapsedTimer t;
        t.start();
        if (cmd == QLatin1String("verify")) core->transfers()->verifyDownload(id);
        else core->transfers()->restartDownload(id);
        waitFor([&] {
            static qint64 last = 0;
            if (t.elapsed() - last >= 2000) {
                last = t.elapsed();
                const TransferInfo x = core->transfers()->transfer(id);
                say(QStringLiteral("  %1s %2").arg(t.elapsed() / 1000).arg(x.statusText));
            }
            return !answer.isEmpty();
        }, timeout);
        QObject::disconnect(c);
        say(QStringLiteral("%1: %2 (after %3 ms)").arg(cmd, answer.isEmpty() ? QStringLiteral("no answer yet") : answer).arg(t.elapsed()));
        return answer.isEmpty() ? 1 : 0;
    }
    if (cmd == QLatin1String("echo")) { say(a.join(QLatin1Char(' '))); return 0; }
    if (cmd == QLatin1String("stats")) {   // [up-1] stats <id|last>: one JSON line of the engine's counters
        QString id = a.value(0);
        if (id == QLatin1String("last"))
            for (const TransferInfo &t : core->transfers()->downloads()) id = t.id;
        const TransferInfo t = core->transfers()->transfer(id);
        QVariantMap m = engine ? engine->debugInfo(id) : QVariantMap();
        QVariantMap o;
        for (const char *k : {"store", "verifiedByNet", "receivedByNet", "unitFailures", "wholeFileFailures", "failedUnitBytes",
                              "piecePreverifyFailures", "injectedBytes", "banned", "hashSetProvider", "hashSetProvisional",
                              "heldRanges", "verifiedRanges", "partPath"})
            if (m.contains(QLatin1String(k))) o.insert(QLatin1String(k), m.value(QLatin1String(k)));
        QVariantList srcs;
        for (const QVariant &v : m.value(QStringLiteral("sources")).toList()) {
            const QVariantMap sm = v.toMap();
            QVariantMap x;
            for (const char *k : {"key", "net", "bytes", "verified", "strikes", "banned", "why"})
                if (sm.contains(QLatin1String(k))) x.insert(QLatin1String(k), sm.value(QLatin1String(k)));
            srcs << x;
        }
        o.insert(QStringLiteral("sources"), srcs);
        o.insert(QStringLiteral("state"), int(t.state));
        o.insert(QStringLiteral("status"), t.statusText);
        o.insert(QStringLiteral("done"), t.done);
        o.insert(QStringLiteral("size"), t.size);
        o.insert(QStringLiteral("path"), t.savePath);
        say(QStringLiteral("stats: ") + QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(o)).toJson(QJsonDocument::Compact)));
        return 0;
    }
    if (cmd == QLatin1String("touch")) {   // [up-1] touch <path>: a marker for scripts that wait-file
        QFile f(a.value(0));
        return f.open(QIODevice::WriteOnly | QIODevice::Append) ? 0 : 1;
    }
    if (cmd == QLatin1String("uploads")) {   // [up-1] the upload rows as the Transfers page has them
        for (const TransferInfo &t : core->transfers()->uploads())
            say(QStringLiteral("upload-row: %1 net=%2 to=%3 done=%4 size=%5 rate=%6 state=%7 status=\"%8\"")
                    .arg(t.name, netShortName(t.networks.value(0)), t.peer.name).arg(t.done).arg(t.size).arg(t.rateUpBps)
                    .arg(int(t.state)).arg(t.statusText));
        return 0;
    }
    if (cmd == QLatin1String("cancel")) {   // cancel <id|last> [--keep]
        QString id = a.value(0);
        if (id == QLatin1String("last") && !core->transfers()->downloads().isEmpty()) id = core->transfers()->downloads().last().id;
        core->transfers()->cancel(id, a.value(1) != QLatin1String("--keep"));
        return 0;
    }
    if (cmd == QLatin1String("info")) {
        QString id = a.value(0);
        if (id == QLatin1String("last"))
            for (const TransferInfo &t : core->transfers()->downloads()) id = t.id;
        if (engine) {
            QVariantMap m = engine->debugInfo(id);
            if (a.contains(QLatin1String("--brief"))) m.remove(QStringLiteral("sources"));
            say(QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(m)).toJson(QJsonDocument::Indented)));
        }
        return 0;
    }
    if (cmd == QLatin1String("watch")) {   // watch <id> [--timeout s] [--every s]: like get --until-done
        if (a.removeAll(QStringLiteral("--rows")) > 0) showRows = true;
        QString id = a.value(0);
        a.removeFirst();
        if (id == QLatin1String("last"))
            for (const TransferInfo &t : core->transfers()->downloads()) id = t.id;
        const int timeout = opt(a, QStringLiteral("--timeout"), QStringLiteral("600")).toInt();
        const int every = qMax(1, opt(a, QStringLiteral("--every"), QStringLiteral("5")).toInt());
        const qint64 t0 = QDateTime::currentMSecsSinceEpoch();
        qint64 last = 0;
        waitFor([&] {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (now - last >= every * 1000) { last = now; progressLine(id, t0); }
            const TransferState s = core->transfers()->transfer(id).state;
            return s == TransferState::Completed || s == TransferState::Seeding || s == TransferState::Error;
        }, qint64(timeout) * 1000);
        progressLine(id, t0);
        return 0;
    }
    if (cmd == QLatin1String("maplog")) {   // [ui-i] maplog <id|last> <file.jsonl> [--every s] [--timeout s] [--until-done]
        QString id = a.value(0);
        const QString file = a.value(1);
        a = a.mid(2);
        if (id == QLatin1String("last"))
            for (const TransferInfo &t : core->transfers()->downloads()) id = t.id;
        const bool untilDone = a.removeAll(QStringLiteral("--until-done")) > 0;
        const int timeout = opt(a, QStringLiteral("--timeout"), QStringLiteral("60")).toInt();
        const int every = qMax(1, opt(a, QStringLiteral("--every"), QStringLiteral("5")).toInt());
        QFile f(file);
        if (!f.open(QIODevice::Append | QIODevice::Text)) {
            say(QStringLiteral("maplog: cannot write %1").arg(file));
            return 1;
        }
        const qint64 t0 = QDateTime::currentMSecsSinceEpoch();
        qint64 last = 0;
        int n = 0;
        auto sample = [&] {
            const TransferInfo t = core->transfers()->transfer(id);
            const QVariantMap dbg = engine ? engine->debugInfo(id) : QVariantMap();
            QString map;
            for (char c : t.pieceMap) map += QLatin1Char(char('0' + int(c)));
            QJsonObject o;
            o.insert(QStringLiteral("t"), double(QDateTime::currentMSecsSinceEpoch() - t0) / 1000.0);
            o.insert(QStringLiteral("id"), id);
            o.insert(QStringLiteral("name"), t.name);
            o.insert(QStringLiteral("state"), int(t.state));
            o.insert(QStringLiteral("status"), t.statusText);
            o.insert(QStringLiteral("done"), double(t.done));
            o.insert(QStringLiteral("size"), double(t.size));
            o.insert(QStringLiteral("savePath"), t.savePath);
            o.insert(QStringLiteral("sequential"), t.sequential);
            o.insert(QStringLiteral("firstLast"), t.firstLastFirst);
            o.insert(QStringLiteral("map"), map);
            o.insert(QStringLiteral("fill"), QString::fromLatin1(t.pieceFill.toHex()));
            QVariantMap d;
            for (const char *k : {"store", "mapFrom", "scheme", "partPath", "partSize", "verifiedRanges", "heldRanges", "inFlightRanges",
                                  "pieceLength", "downloadingPieces", "reservedPieceList", "files", "savePath", "verified", "held"})
                if (dbg.contains(QLatin1String(k))) d.insert(QLatin1String(k), dbg.value(QLatin1String(k)));
            o.insert(QStringLiteral("dbg"), QJsonObject::fromVariantMap(d));
            f.write(QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n');
            f.flush();
            ++n;
            return t;
        };
        waitFor([&] {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (now - last >= every * 1000) {
                last = now;
                const TransferInfo t = sample();
                if (untilDone && (t.state == TransferState::Completed || t.state == TransferState::Seeding || t.state == TransferState::Error))
                    return true;
            }
            return false;
        }, qint64(timeout) * 1000);
        say(QStringLiteral("maplog: %1 samples of %2 in %3").arg(n).arg(id, file));
        return 0;
    }
    say(QStringLiteral("unknown command: %1").arg(cmd));
    return 2;
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QStringList args = app.arguments().mid(1);
    QString profile, script;
    QStringList sets;   // --set key=value (applied before the core starts; persisted in the profile)
    for (int i = 0; i < args.size();) {
        if (args.at(i) == QLatin1String("--profile") && i + 1 < args.size()) { profile = args.at(i + 1); args.remove(i, 2); }
        else if (args.at(i) == QLatin1String("--script") && i + 1 < args.size()) { script = args.at(i + 1); args.remove(i, 2); }
        else if (args.at(i) == QLatin1String("--set") && i + 1 < args.size()) { sets << args.at(i + 1); args.remove(i, 2); }
        else ++i;
    }
    if (profile.isEmpty()) {
        std::fprintf(stderr, "usage: mxcli --profile DIR <status|search|get|list|info|pause|resume|cancel|watch|wait> ... | --script FILE\n");
        return 2;
    }
    profile = QDir(profile).absolutePath();
    QDir().mkpath(profile);
    registerMetaTypes();
    installLogHandler(profile, 20 * 1024 * 1024, 3, /*stderr*/ false, 1);

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
    // never write into $HOME: downloads land inside the profile unless the profile itself names another
    // incoming dir (the core's default is <Downloads>/OctoMX, i.e. under the real HOME) [engine-4]
    {
        Settings *st = core->settings();
        const QString inc = st->s(keys::transfers::IncomingDir);
        if (inc.isEmpty() || inc == st->defaultValue(keys::transfers::IncomingDir).toString() || inc.startsWith(QDir::homePath() + QLatin1Char('/')))
            st->setValue(QString(keys::transfers::IncomingDir), profile + QStringLiteral("/incoming"));
    }
    for (const QString &kv : std::as_const(sets)) {
        const QString k = kv.section(QLatin1Char('='), 0, 0), v = kv.section(QLatin1Char('='), 1);
        QVariant val = v;
        if (v == QLatin1String("true") || v == QLatin1String("false")) val = (v == QLatin1String("true"));
        core->settings()->setValue(k, val);
    }
    const QString envDisable = qEnvironmentVariable("MXCLI_DISABLE");   // e.g. "Gnutella,OctoMX"
    for (const QString &n : envDisable.split(QLatin1Char(','), Qt::SkipEmptyParts))
        core->network()->setEnabled(netFromName(n.trimmed()), false);
    // [up-1] MXCLI_UPLOG=<file>: one line per visible change of an upload row (and at most one per 2 s
    // while it runs); MXCLI_SRCLOG=<file>: the same for the source rows of every download. Evidence for
    // the instance tests (what the user would read in the Transfers page).
    struct RowLog {
        QFile file;
        QHash<QString, QString> last;
        QHash<QString, qint64> lastMs;
        void write(const QString &key, const QString &state, const QString &line)
        {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (last.value(key) == state && now - lastMs.value(key) < 2000) return;
            last.insert(key, state);
            lastMs.insert(key, now);
            file.write(QStringLiteral("%1 %2\n").arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")), line).toUtf8());
            file.flush();
        }
    };
    auto upLog = std::make_shared<RowLog>(), srcLog = std::make_shared<RowLog>();
    upLog->file.setFileName(qEnvironmentVariable("MXCLI_UPLOG"));
    srcLog->file.setFileName(qEnvironmentVariable("MXCLI_SRCLOG"));
    if (!upLog->file.fileName().isEmpty() && !upLog->file.open(QIODevice::WriteOnly | QIODevice::Append)) say(QStringLiteral("cannot write the upload log"));
    if (!srcLog->file.fileName().isEmpty() && !srcLog->file.open(QIODevice::WriteOnly | QIODevice::Append)) say(QStringLiteral("cannot write the source log"));
    auto logRow = [upLog, srcLog](const TransferInfo &t) {
        if (t.isUpload && upLog->file.isOpen()) {
            const QString state = t.statusText + QLatin1Char('|') + QString::number(t.done == t.size);
            upLog->write(t.id, state, QStringLiteral("upload %1 net=%2 to=%3 done=%4 size=%5 rate=%6 state=%7 id=%9 status=\"%8\"")
                                          .arg(t.name, netShortName(t.networks.value(0)), t.peer.name).arg(t.done).arg(t.size)
                                          .arg(t.rateUpBps).arg(int(t.state)).arg(t.statusText, t.id));
        }
        if (!t.isUpload && srcLog->file.isOpen()) {
            for (const TransferSource &s : t.sources) {
                const QString key = t.id + QLatin1Char('/') + netShortName(s.net) + QLatin1Char('/') + s.user.name + QLatin1Char('/') + s.id;
                srcLog->write(key, s.statusText, QStringLiteral("source %1 net=%2 user=%3 bytes=%4 rate=%5 status=\"%6\"")
                                                    .arg(t.name, netShortName(s.net), s.user.name).arg(s.bytes).arg(s.rateBps).arg(s.statusText));
            }
        }
    };
    QObject::connect(core->transfers(), &TransferService::transferAdded, &app, logRow);
    QObject::connect(core->transfers(), &TransferService::transferChanged, &app, logRow);
    core->start();
    say(QStringLiteral("mxcli: profile %1, %2 plugins, log %3").arg(profile).arg(plugins.size()).arg(logFilePath()));
    Cli cli(core, profile, plugins);
    int rc = 0;
    if (!script.isEmpty()) {
        QFile f(script);
        if (!f.open(QIODevice::ReadOnly)) { std::fprintf(stderr, "cannot read %s\n", qPrintable(script)); return 2; }
        for (const QString &raw : QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'))) {
            const QString line = raw.trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue;
            say(QStringLiteral("> %1").arg(line));
            rc = cli.run(QProcess::splitCommand(line));
        }
    } else {
        rc = cli.run(args);
    }
    QElapsedTimer t;
    t.start();
    bool done = false;
    QObject::connect(core, &Core::shutdownFinished, &app, [&] { done = true; });
    core->shutdown();
    waitFor([&] { return done; }, 6000);
    say(QStringLiteral("mxcli: shut down in %1 ms").arg(t.elapsed()));
    delete core;
    uninstallLogHandler();
    return rc;
}
