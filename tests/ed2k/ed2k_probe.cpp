// SPDX-License-Identifier: GPL-3.0-or-later
// ed2k_probe: drives the eD2k plugin through the mx::NetworkPlugin / mx::RangeSource interfaces
// on the live network, with a tiny stand-in for the OctoMX core.
//
//   ed2k_probe --profile <dir> [--port 4662] [--server ip:port] [--search "ubuntu" ...]
//              [--pick <regex>] [--hash <ed2k hex> --size <bytes> --name <name>]
//              [--parts 1] [--minutes 30] [--max-sources 40] [--share <file> ...] [--share-check]
//              [--peer ed2k-peer://ip:port?uid=..] [--serve-minutes N] [--no-download]
#include "ed2k_core.h"
#include "ed2k_io.h"
#include "ed2k_plugin.h"
#include "ed2k_plugin_impl.h"
#include "ed2k_server.h"
#include "ed2k_upload.h"
#include "kad/kad_plugin.h"
#include "test_support.h"

#include <QtCore/QCommandLineParser>
#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QFileInfo>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimer>

#include <cstdio>

using namespace mx::ed2k;

static QElapsedTimer g_clock;

static void out(const QString &s)
{
    const QString line = QStringLiteral("[%1] %2\n").arg(g_clock.elapsed() / 1000.0, 8, 'f', 1).arg(s);
    std::fputs(line.toUtf8().constData(), stdout);
    std::fflush(stdout);
}

static bool waitFor(const std::function<bool()> &cond, int ms)
{
    QElapsedTimer t; t.start();
    while (!cond()) {
        if (t.elapsed() > ms) return false;
        QEventLoop l;
        QTimer::singleShot(200, &l, &QEventLoop::quit);
        l.exec();
    }
    return true;
}

static void sleepMs(int ms) { waitFor([] { return false; }, ms); }

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    g_clock.start();
    mx::registerMetaTypes();
    QCommandLineParser p;
    p.addHelpOption();
    p.addOption({QStringLiteral("profile"), QStringLiteral("profile directory"), QStringLiteral("dir")});
    p.addOption({QStringLiteral("port"), QStringLiteral("TCP port"), QStringLiteral("port"), QStringLiteral("4662")});
    p.addOption({QStringLiteral("udp-port"), QStringLiteral("UDP port"), QStringLiteral("port"), QStringLiteral("4672")});
    p.addOption({QStringLiteral("server"), QStringLiteral("connect to this server (ip:port)"), QStringLiteral("addr")});
    p.addOption({QStringLiteral("search"), QStringLiteral("keyword search (repeatable)"), QStringLiteral("q")});
    p.addOption({QStringLiteral("pick"), QStringLiteral("regex for the file to download"), QStringLiteral("re"),
                 QStringLiteral("(ubuntu|debian|linux|fedora|mint|knoppix|slackware|centos|arch|opensuse|freebsd|tails|kali|manjaro|gentoo|mandriva|suse).*\\.iso$")});
    p.addOption({QStringLiteral("hash"), QStringLiteral("ed2k hash to download"), QStringLiteral("hex")});
    p.addOption({QStringLiteral("size"), QStringLiteral("file size"), QStringLiteral("bytes")});
    p.addOption({QStringLiteral("name"), QStringLiteral("file name"), QStringLiteral("name")});
    p.addOption({QStringLiteral("parts"), QStringLiteral("stop after this many verified parts (0 = whole file)"), QStringLiteral("n"), QStringLiteral("1")});
    p.addOption({QStringLiteral("minutes"), QStringLiteral("run time limit"), QStringLiteral("n"), QStringLiteral("30")});
    p.addOption({QStringLiteral("max-sources"), QStringLiteral("max RangeSources to open"), QStringLiteral("n"), QStringLiteral("60")});
    p.addOption({QStringLiteral("share"), QStringLiteral("share this file (repeatable)"), QStringLiteral("file")});
    p.addOption({QStringLiteral("share-check"), QStringLiteral("verify the server indexed our shared file by searching for it")});
    p.addOption({QStringLiteral("peer"), QStringLiteral("add a direct peer source (ed2k-peer:// URI)"), QStringLiteral("uri")});
    p.addOption({QStringLiteral("serve-minutes"), QStringLiteral("only share/serve for N minutes"), QStringLiteral("n")});
    p.addOption({QStringLiteral("no-download"), QStringLiteral("skip the download phase")});
    p.addOption({QStringLiteral("server-stats"), QStringLiteral("print the server list with UDP ping results")});
    p.addOption({QStringLiteral("no-server"), QStringLiteral("do not connect to servers")});
    p.addOption({QStringLiteral("allow-lan"), QStringLiteral("accept private/loopback peers")});
    p.addOption({QStringLiteral("slots"), QStringLiteral("upload slots"), QStringLiteral("n"), QStringLiteral("3")});
    p.addOption({QStringLiteral("down-limit"), QStringLiteral("download limit in KiB/s (0 = none)"), QStringLiteral("kib"), QStringLiteral("0")});
    // Kad
    p.addOption({QStringLiteral("kad"), QStringLiteral("run the Kad plugin on the shared engine")});
    p.addOption({QStringLiteral("no-ed2k"), QStringLiteral("do not start the eD2k (server) plugin: pure Kad")});
    p.addOption({QStringLiteral("kad-node"), QStringLiteral("Kad bootstrap node ip:port (repeatable)"), QStringLiteral("addr")});
    p.addOption({QStringLiteral("kad-nodes-url"), QStringLiteral("nodes.dat URL"), QStringLiteral("url")});
    p.addOption({QStringLiteral("kad-wait"), QStringLiteral("seconds to watch the Kad bootstrap (contacts over time, firewall result)"), QStringLiteral("s"), QStringLiteral("0")});
    p.addOption({QStringLiteral("kad-search"), QStringLiteral("Kad keyword search (repeatable)"), QStringLiteral("q")});
    p.addOption({QStringLiteral("set"), QStringLiteral("set a setting key=value (repeatable)"), QStringLiteral("kv")});
    p.addOption({QStringLiteral("kad-soak"), QStringLiteral("stay online N minutes and print Kad statistics every minute"), QStringLiteral("min"), QStringLiteral("0")});
    p.process(app);

    const QString profile = p.value(QStringLiteral("profile")).isEmpty()
        ? QDir::current().filePath(QStringLiteral("ed2k-probe-profile")) : p.value(QStringLiteral("profile"));
    QDir().mkpath(profile);
    mxtest::MemSettings settings;
    settings.setValue(QStringLiteral("ed2k/tcpPort"), p.value(QStringLiteral("port")).toInt());
    settings.setValue(QStringLiteral("ed2k/udpPort"), p.value(QStringLiteral("udp-port")).toInt());
    settings.setValue(QStringLiteral("ed2k/uploadSlots"), p.value(QStringLiteral("slots")).toInt());
    if (p.isSet(QStringLiteral("allow-lan"))) settings.setValue(QStringLiteral("ed2k/allowLan"), true);
    if (p.isSet(QStringLiteral("server")) || p.isSet(QStringLiteral("no-server"))) settings.setValue(QStringLiteral("ed2k/autoConnect"), false);
    if (p.isSet(QStringLiteral("no-server"))) {
        settings.setValue(QStringLiteral("ed2k/autoUpdateServerList"), false);
        settings.setValue(QStringLiteral("ed2k/udpPing"), false);
    }
    mxtest::FileLibrary lib;
    mxtest::NullLimiter limiter;
    if (p.value(QStringLiteral("down-limit")).toInt() > 0)
        limiter.setLimit(mx::RateLimiter::Down, qint64(p.value(QStringLiteral("down-limit")).toInt()) * 1024);
    for (const QString &kv : p.values(QStringLiteral("set"))) {
        const int eq = int(kv.indexOf(QLatin1Char('=')));
        if (eq <= 0) continue;
        const QString v = kv.mid(eq + 1);
        bool isInt = false;
        const int iv = v.toInt(&isInt);
        if (v == QLatin1String("true") || v == QLatin1String("false")) settings.setValue(kv.left(eq), v == QLatin1String("true"));
        else if (isInt) settings.setValue(kv.left(eq), iv);
        else settings.setValue(kv.left(eq), v);
    }
    if (p.isSet(QStringLiteral("kad-nodes-url"))) settings.setValue(QStringLiteral("kad/nodesDatUrl"), p.value(QStringLiteral("kad-nodes-url")));
    const bool useKad = p.isSet(QStringLiteral("kad")) || p.isSet(QStringLiteral("kad-search")) || p.isSet(QStringLiteral("no-ed2k"));
    const bool useEd2k = !p.isSet(QStringLiteral("no-ed2k"));
    Ed2kPlugin plugin;
    QObject::connect(&plugin, &mx::NetworkPlugin::logMessage, [](const QString &m) { out(m); });
    plugin.init(mxtest::makeContext(&settings, &lib, &limiter, profile));
    std::unique_ptr<mx::ed2k::kad::KadPlugin> kadp;
    if (useKad) {
        kadp = std::make_unique<mx::ed2k::kad::KadPlugin>(plugin.sharedCore());
        QObject::connect(kadp.get(), &mx::NetworkPlugin::logMessage, [](const QString &m) { out(QStringLiteral("Kad: ") + m); });
        mx::PluginContext kctx = mxtest::makeContext(&settings, &lib, &limiter, profile);
        kctx.dataDir = QDir(profile).filePath(QStringLiteral("kad"));
        kadp->init(kctx);
    }

    QList<mx::SharedFile> shared;
    for (const QString &f : p.values(QStringLiteral("share"))) {
        const mx::SharedFile sf = lib.addFile(f);
        if (sf.hashes.ed2k.isEmpty()) { out(QStringLiteral("cannot share %1").arg(f)); continue; }
        shared << sf;
        out(QStringLiteral("sharing %1 (%2 bytes) %3").arg(sf.name).arg(sf.size)
                .arg(formatEd2kFileLink(sf.name, sf.size, sf.hashes.ed2k)));
    }
    if (useEd2k) plugin.start();
    plugin.setSharedFiles(lib.sharedFiles());
    Ed2kCore *core = plugin.core();
    if (kadp) {
        kadp->start();
        kadp->setSharedFiles(lib.sharedFiles());
        for (const QString &n : p.values(QStringLiteral("kad-node"))) kadp->addServer(n, QString());
    }
    out(QStringLiteral("user hash %1, TCP port %2 (%3), UDP %4").arg(toHex(core->userHash())).arg(core->tcpPort())
            .arg(core->listening() ? QStringLiteral("listening") : QStringLiteral("NOT listening")).arg(core->udpLocalPort()));
    out(QStringLiteral("peer URI of this instance: ed2k-peer://127.0.0.1:%1?uid=%2&crypt=3").arg(core->tcpPort()).arg(toHex(core->userHash())));

    const qint64 deadline = qint64(p.value(QStringLiteral("minutes")).toInt()) * 60 * 1000;
    const bool useServer = useEd2k && !p.isSet(QStringLiteral("no-server"));

    // ---------------------------------------------------------------- server
    if (useServer) {
        if (p.isSet(QStringLiteral("server"))) {
            plugin.addServer(p.value(QStringLiteral("server")), QString());
            plugin.connectServer(p.value(QStringLiteral("server")));
        }
        if (!waitFor([&] { return plugin.status().state == mx::ConnState::Connected; }, 180000)) {
            out(QStringLiteral("FAILED: no server connection within 3 minutes"));
        } else {
            ServerConnection *c = core->servers()->current();
            out(QStringLiteral("SERVER: %1 [%2], %3 (client ID %4), server flags 0x%5, obfuscated=%6")
                    .arg(c->info().name, c->id(), c->lowId() ? QStringLiteral("LowID") : QStringLiteral("HighID"))
                    .arg(c->clientId()).arg(c->tcpFlags(), 0, 16).arg(c->obfuscated()));
            waitFor([&] { return core->servers()->current() && core->servers()->current()->info().users > 0; }, 20000);
            if (ServerConnection *c2 = core->servers()->current())
                out(QStringLiteral("SERVER STATUS: %1 users, %2 files").arg(c2->info().users).arg(c2->info().files));
            const mx::NetworkStatus st = plugin.status();
            out(QStringLiteral("STATUS: \"%1\" firewalled=%2 external=%3 users=%4 files=%5").arg(st.statusText).arg(st.firewalled)
                    .arg(st.externalAddress).arg(st.usersOnline).arg(st.filesOnline));
            if (p.isSet(QStringLiteral("server-stats"))) {
                out(QStringLiteral("waiting 40 s for UDP status pings of the other servers..."));
                sleepMs(40000);
                for (const mx::ServerEntry &e : plugin.servers())
                    out(QStringLiteral("   server %1 %2  users %3  files %4  ping %5 ms  [%6]%7").arg(e.address, -22).arg(e.name.left(32), -32)
                            .arg(e.users, 7).arg(e.files, 9).arg(e.pingMs, 4)
                            .arg(e.state == mx::ConnState::Connected ? QStringLiteral("connected") : QStringLiteral("-"))
                            .arg(e.description.startsWith(QLatin1Char('[')) ? QStringLiteral(" ") + e.description.section(QLatin1Char(']'), 0, 0) + QLatin1Char(']') : QString()));
            }
        }
    }

    // ---------------------------------------------------------------- searches
    QList<mx::SearchHit> allHits;
    quint32 qid = 100;
    for (const QString &q : p.values(QStringLiteral("search"))) {
        mx::SearchQuery sq;
        sq.id = ++qid;
        sq.text = q;
        sq.maxResults = 500;
        QList<mx::SearchHit> hits;
        bool done = false;
        auto c1 = QObject::connect(&plugin, &mx::NetworkPlugin::searchHits, [&](quint32 id, const QList<mx::SearchHit> &h) { if (id == sq.id) hits << h; });
        auto c2 = QObject::connect(&plugin, &mx::NetworkPlugin::searchDone, [&](quint32 id) { if (id == sq.id) done = true; });
        QElapsedTimer t; t.start();
        plugin.search(sq);
        waitFor([&] { return done; }, 90000);
        QObject::disconnect(c1); QObject::disconnect(c2);
        std::sort(hits.begin(), hits.end(), [](const mx::SearchHit &a, const mx::SearchHit &b) { return a.sources[0].peers > b.sources[0].peers; });
        out(QStringLiteral("SEARCH \"%1\": %2 hits in %3 ms%4").arg(q).arg(hits.size()).arg(t.elapsed()).arg(done ? QString() : QStringLiteral(" (timeout)")));
        for (int i = 0; i < hits.size() && i < 12; ++i) {
            const mx::SearchHit &h = hits[i];
            out(QStringLiteral("   %1 src/%2 complete  %3  %4  %5%6").arg(h.sources[0].peers, 4).arg(h.sources[0].seeds, 4)
                    .arg(formatBytes(h.size), 10).arg(h.fileName.left(70))
                    .arg(toHex(h.hashes.ed2k))
                    .arg(h.durationSec > 0 ? QStringLiteral("  %1s %2kbps").arg(h.durationSec).arg(h.bitrateKbps) : QString()));
        }
        allHits << hits;
    }

    // ---------------------------------------------------------------- Kad
    mx::ed2k::kad::KadNode *node = kadp ? kadp->node() : nullptr;
    auto kadLine = [&] {
        const mx::NetworkStatus st = kadp->status();
        const auto &k = node->counters();
        return QStringLiteral("KAD: %1 | contacts %2, users ~%3, TCP %4, UDP %5%6, ext port %7, lookups %8 | in %9 pkts (%10 obf) out %11 pkts | answered hello %12 req %13 boot %14 ping %15 search %16 publish %17 fw %18 | dropped flood %19 unsolicited %20 malformed %21 rate %22 | buddy req %23 offers %24")
            .arg(st.statusText).arg(node->contacts()).arg(node->estimatedUsers())
            .arg(node->firewalled() ? QStringLiteral("firewalled") : QStringLiteral("open"))
            .arg(node->udpFirewalled() ? QStringLiteral("firewalled") : QStringLiteral("open"))
            .arg(node->udpVerified() ? QStringLiteral(" (verified)") : QStringLiteral(" (unverified)"))
            .arg(node->externalPort()).arg(node->runningLookups())
            .arg(k.packetsIn).arg(k.obfuscatedIn).arg(k.packetsOut)
            .arg(k.answeredHello).arg(k.answeredReq).arg(k.answeredBootstrap).arg(k.answeredPing).arg(k.answeredSearch)
            .arg(k.answeredPublish).arg(k.answeredFirewall)
            .arg(k.droppedFlood).arg(k.droppedUnsolicited).arg(k.droppedMalformed).arg(k.droppedRate)
            .arg(k.buddyRequests).arg(k.buddyOffers);
    };
    if (kadp) {
        QElapsedTimer kt; kt.start();
        out(QStringLiteral("KAD: start with %1 contacts from nodes.dat").arg(node->contacts()));
        const int waitS = qMax(p.value(QStringLiteral("kad-wait")).toInt(), p.isSet(QStringLiteral("kad-search")) ? 30 : 0);
        bool reported = false;
        qint64 lastPrint = -100000;
        while (kt.elapsed() < qint64(qMax(waitS, 1)) * 1000 || (!node->connected() && kt.elapsed() < 180000)) {
            sleepMs(500);
            if (!reported && node->connected()) {
                reported = true;
                out(QStringLiteral("KAD CONNECTED after %1 ms with %2 contacts").arg(kt.elapsed()).arg(node->contacts()));
            }
            if (kt.elapsed() - lastPrint >= 10000) { lastPrint = kt.elapsed(); out(kadLine()); }
        }
        if (!node->connected()) out(QStringLiteral("KAD: FAILED to connect within 180 s"));
        out(kadLine());
        for (const QString &q : p.values(QStringLiteral("kad-search"))) {
            mx::SearchQuery sq;
            sq.id = ++qid;
            sq.text = q;
            sq.maxResults = 500;
            QList<mx::SearchHit> hits;
            bool done = false;
            qint64 firstHit = -1;
            QElapsedTimer t; t.start();
            auto c1 = QObject::connect(kadp.get(), &mx::NetworkPlugin::searchHits, [&](quint32 id, const QList<mx::SearchHit> &h) {
                if (id != sq.id) return;
                if (firstHit < 0) firstHit = t.elapsed();
                hits << h;
            });
            auto c2 = QObject::connect(kadp.get(), &mx::NetworkPlugin::searchDone, [&](quint32 id) { if (id == sq.id) done = true; });
            kadp->search(sq);
            waitFor([&] { return done; }, 120000);
            QObject::disconnect(c1); QObject::disconnect(c2);
            std::sort(hits.begin(), hits.end(), [](const mx::SearchHit &a, const mx::SearchHit &b) { return a.sources[0].peers > b.sources[0].peers; });
            out(QStringLiteral("KAD SEARCH \"%1\": %2 hits, first after %3 ms, done after %4 ms%5").arg(q).arg(hits.size()).arg(firstHit)
                    .arg(t.elapsed()).arg(done ? QString() : QStringLiteral(" (timeout)")));
            for (int i = 0; i < hits.size() && i < 12; ++i) {
                const mx::SearchHit &h = hits[i];
                const auto &sx = h.sources[0].extra;
                out(QStringLiteral("   %1 avail  %2  %3  %4  trust %5 pub %6%7").arg(h.sources[0].peers, 5).arg(formatBytes(h.size), 10)
                        .arg(h.fileName.left(70)).arg(toHex(h.hashes.ed2k))
                        .arg(sx.value(QStringLiteral("trust")).toDouble(), 0, 'f', 2).arg(sx.value(QStringLiteral("publishers")).toInt())
                        .arg(h.hashes.aich.isEmpty() ? QString() : QStringLiteral(" aich")));
            }
            allHits << hits;
        }
    }

    // ---------------------------------------------------------------- share check
    if (p.isSet(QStringLiteral("share-check")) && !shared.isEmpty() && core->servers()->current()) {
        const mx::SharedFile sf = shared.first();
        QString token = QFileInfo(sf.name).completeBaseName();
        out(QStringLiteral("SHARE CHECK: waiting 45 s, then searching the server for \"%1\"").arg(token));
        sleepMs(45000);
        for (int attempt = 0; attempt < 3; ++attempt) {
            mx::SearchQuery sq; sq.id = ++qid; sq.text = token;
            bool done = false, found = false;
            auto c1 = QObject::connect(&plugin, &mx::NetworkPlugin::searchHits, [&](quint32 id, const QList<mx::SearchHit> &h) {
                if (id != sq.id) return;
                for (const auto &x : h) if (x.hashes.ed2k == sf.hashes.ed2k) {
                    found = true;
                    out(QStringLiteral("SHARE CHECK: server returned our file: %1 %2 bytes, %3 source(s)").arg(x.fileName).arg(x.size).arg(x.sources[0].peers));
                }
            });
            auto c2 = QObject::connect(&plugin, &mx::NetworkPlugin::searchDone, [&](quint32 id) { if (id == sq.id) done = true; });
            plugin.search(sq);
            waitFor([&] { return done; }, 60000);
            QObject::disconnect(c1); QObject::disconnect(c2);
            if (found) break;
            out(QStringLiteral("SHARE CHECK: not (yet) found, attempt %1").arg(attempt + 1));
            sleepMs(30000);
        }
    }

    if (p.isSet(QStringLiteral("serve-minutes"))) {
        const qint64 until = g_clock.elapsed() + qint64(p.value(QStringLiteral("serve-minutes")).toInt()) * 60000;
        QObject::connect(&plugin, &mx::NetworkPlugin::uploadChanged, [](const mx::TransferInfo &t) {
            static QHash<QString, qint64> last;
            if (t.done - last.value(t.id) >= 2 * 1024 * 1024 || t.state != mx::TransferState::Transferring) {
                last[t.id] = t.done;
                out(QStringLiteral("UPLOAD %1 → %2 (%3): %4 sent, %5 B/s").arg(t.name, t.peer.address,
                        t.sources.value(0).client).arg(t.done).arg(t.rateUpBps));
            }
        });
        QObject::connect(&plugin, &mx::NetworkPlugin::uploadFinished, [](const QString &id) { out(QStringLiteral("UPLOAD finished %1").arg(id)); });
        out(QStringLiteral("serving for %1 minute(s)").arg(p.value(QStringLiteral("serve-minutes"))));
        waitFor([&] { return g_clock.elapsed() > until; }, int(until - g_clock.elapsed() + 1000));
    }

    const int soakMin = p.value(QStringLiteral("kad-soak")).toInt();
    if (kadp && soakMin > 0) {
        out(QStringLiteral("KAD SOAK: staying online for %1 minute(s)").arg(soakMin));
        QElapsedTimer st; st.start();
        quint64 lastIn = node->counters().packetsIn, lastOut = node->counters().packetsOut;
        int disconnectedMinutes = 0;
        for (int m = 1; m <= soakMin; ++m) {
            sleepMs(60000);
            const auto &k = node->counters();
            out(QStringLiteral("SOAK %1 min: %2 pkt/s in, %3 pkt/s out, %4").arg(m).arg((k.packetsIn - lastIn) / 60.0, 0, 'f', 1)
                    .arg((k.packetsOut - lastOut) / 60.0, 0, 'f', 1).arg(kadLine()));
            lastIn = k.packetsIn;
            lastOut = k.packetsOut;
            if (!node->connected()) ++disconnectedMinutes;
        }
        out(QStringLiteral("KAD SOAK RESULT: %1 minute(s) online, %2 minute(s) seen disconnected").arg(soakMin).arg(disconnectedMinutes));
    }
    if (p.isSet(QStringLiteral("no-download"))) { if (kadp) kadp->stop(); plugin.stop(); return 0; }

    // ---------------------------------------------------------------- choose a file
    mx::FileHashes h;
    qint64 size = 0;
    QString name;
    if (p.isSet(QStringLiteral("hash"))) {
        h.ed2k = fromHex(p.value(QStringLiteral("hash")), 16);
        size = p.value(QStringLiteral("size")).toLongLong();
        name = p.value(QStringLiteral("name"));
    } else {
        const QRegularExpression re(p.value(QStringLiteral("pick")), QRegularExpression::CaseInsensitiveOption);
        const mx::SearchHit *best = nullptr;
        for (const mx::SearchHit &x : allHits) {
            if (x.size < 50 * 1024 * 1024 || !re.match(x.fileName).hasMatch()) continue;
            if (!best || x.sources[0].seeds > best->sources[0].seeds ||
                (x.sources[0].seeds == best->sources[0].seeds && x.sources[0].peers > best->sources[0].peers)) best = &x;
        }
        if (best) { h = best->hashes; size = best->size; name = best->fileName; }
    }
    if (h.ed2k.size() != 16 || size <= 0) { out(QStringLiteral("no file to download")); if (kadp) kadp->stop(); plugin.stop(); return 1; }
    out(QStringLiteral("DOWNLOAD TARGET: %1 (%2 bytes, %3 parts) %4").arg(name).arg(size).arg(dataPartCount(size))
            .arg(formatEd2kFileLink(name, size, h.ed2k)));

    const QString outPath = QDir(profile).filePath(QStringLiteral("download.bin"));
    mxtest::SimpleDownloader dl(outPath, size, h.ed2k);
    const int wantParts = p.value(QStringLiteral("parts")).toInt();
    const int maxSources = p.value(QStringLiteral("max-sources")).toInt();
    QObject::connect(&dl, &mxtest::SimpleDownloader::logLine, [](const QString &s) { out(QStringLiteral("dl: ") + s); });
    QObject::connect(&dl, &mxtest::SimpleDownloader::partVerified, [&](int part, const QString &client) {
        out(QStringLiteral("PART %1 VERIFIED against the peer's hashset (last chunk from %2), %3").arg(part).arg(client, dl.summary()));
    });
    QObject::connect(&dl, &mxtest::SimpleDownloader::partFailed, [](int part) { out(QStringLiteral("PART %1 FAILED verification").arg(part)); });
    QObject::connect(&plugin, &mx::NetworkPlugin::hashSetLearned, [&](const QString &, const QString &kind, qint64 unit, const QList<QByteArray> &hs) {
        out(QStringLiteral("HASHSET learned: %1, %2 hashes of %3 bytes (verified against the file hash)").arg(kind).arg(hs.size()).arg(unit));
        dl.setPartHashes(hs);
    });
    QObject::connect(&plugin, &mx::NetworkPlugin::hashesLearned, [](const QString &, const mx::FileHashes &m) {
        out(QStringLiteral("AICH root learned: %1").arg(toHex(m.aich)));
    });
    int opened = 0;
    QSet<QString> seen;
    auto openOne = [&](const mx::SearchSource &s) {
        if (opened >= maxSources || seen.contains(s.uri)) return;
        seen.insert(s.uri);
        mx::RangeSource *rs = (kadp && s.net == mx::Net::Kad) ? kadp->openSource(QStringLiteral("probe"), h, size, s)
                                                               : plugin.openSource(QStringLiteral("probe"), h, size, s);
        if (!rs) return;
        ++opened;
        QObject::connect(rs, &mx::RangeSource::stateChanged, rs, [rs] {
            static QHash<mx::RangeSource *, QString> last;
            const QString t = rs->stateText();
            // Ready <-> Transferring flips on every request: report only real changes
            const QString key = (rs->state() == mx::RangeSource::Busy) ? QStringLiteral("Ready") : t;
            if (last.value(rs) == key) return;
            last[rs] = key;
            const mx::SearchSource d = rs->descriptor();
            if (rs->state() == mx::RangeSource::Ready || rs->state() == mx::RangeSource::Queued || rs->state() == mx::RangeSource::Failed)
                out(QStringLiteral("  source %1 %2: %3").arg(d.user.address.isEmpty() ? d.user.id : d.user.address,
                        rs->clientName().isEmpty() ? QStringLiteral("") : QStringLiteral("(%1)").arg(rs->clientName()), t));
        });
        dl.addSource(rs);
    };
    QObject::connect(&plugin, &mx::NetworkPlugin::sourcesFound, [&](const QString &, const QList<mx::SearchSource> &list) {
        out(QStringLiteral("SOURCES: %1 new (total opened %2)").arg(list.size()).arg(opened));
        for (const auto &s : list) openOne(s);
    });
    if (kadp) {
        QObject::connect(kadp.get(), &mx::NetworkPlugin::sourcesFound, [&](const QString &, const QList<mx::SearchSource> &list) {
            out(QStringLiteral("KAD SOURCES: %1 new (total opened %2)").arg(list.size()).arg(opened));
            for (const auto &s : list) openOne(s);
        });
        QObject::connect(kadp.get(), &mx::NetworkPlugin::hashSetLearned, [&](const QString &, const QString &kind, qint64 unit, const QList<QByteArray> &hs) {
            out(QStringLiteral("HASHSET learned (via Kad facade): %1, %2 hashes of %3 bytes").arg(kind).arg(hs.size()).arg(unit));
            dl.setPartHashes(hs);
        });
    }
    for (const QString &peer : p.values(QStringLiteral("peer"))) {
        mx::SearchSource s; s.net = mx::Net::ED2K; s.uri = peer;
        openOne(s);
    }
    if (useEd2k) plugin.findSources(QStringLiteral("probe"), h, size, name);
    if (kadp) kadp->findSources(QStringLiteral("probe"), h, size, name);
    QElapsedTimer dlClock; dlClock.start();
    qint64 lastReport = 0;
    auto finishedCond = [&] {
        if (wantParts > 0 && dl.verifiedParts() >= wantParts) return true;
        return dl.done();
    };
    while (!finishedCond() && g_clock.elapsed() < deadline) {
        sleepMs(1000);
        if (dlClock.elapsed() - lastReport >= 60000) {
            lastReport = dlClock.elapsed();
            out(QStringLiteral("PROGRESS after %1 s: %2; %3 sources opened, %4 connections; down %5 B/s up %6 B/s")
                    .arg(dlClock.elapsed() / 1000).arg(dl.summary()).arg(opened).arg(core->connectionCount())
                    .arg(core->rateDownBps()).arg(core->rateUpBps()));
            QMap<QString, int> states;
            for (auto *s : dl.sources()) states[s->stateText().section(QLatin1Char('('), 0, 0).trimmed()]++;
            QStringList st;
            for (auto it = states.cbegin(); it != states.cend(); ++it) st << QStringLiteral("%1=%2").arg(it.key()).arg(it.value());
            out(QStringLiteral("  source states: ") + st.join(QStringLiteral(", ")));
            const QStringList lines = dl.sourceLines();
            for (int i = 0; i < lines.size() && i < 25; ++i) out(lines[i]);
        }
    }
    out(QStringLiteral("RESULT: %1 after %2 s, %3").arg(finishedCond() ? QStringLiteral("SUCCESS") : QStringLiteral("INCOMPLETE"))
            .arg(dlClock.elapsed() / 1000).arg(dl.summary()));
    if (dl.firstVerifiedMs() >= 0) out(QStringLiteral("first part verified %1 s after the download started").arg(dl.firstVerifiedMs() / 1000));
    for (const QString &l : dl.sourceLines()) out(l);
    if (kadp) { out(kadLine()); kadp->stop(); }
    plugin.stop();
    return finishedCond() ? 0 : 2;
}
