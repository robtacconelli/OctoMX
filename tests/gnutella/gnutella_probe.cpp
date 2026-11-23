// SPDX-License-Identifier: GPL-3.0-or-later
// gnutella_probe: drives the Gnutella plugin against the LIVE network through the plugin
// interface only: bootstrap, ultrapeer connections, searches, a verified download of a clearly
// legal file, browse host, and serving (QRP, answered queries / handshakes / HTTP).
//
//   gnutella_probe [--minutes 25] [--profile <dir>] [--port 6346] [--query "a,b,c"] [--no-download]
//
// Output: human-readable log on stdout (redirect it to a file).
#include "gnutella_plugin.h"
#include "gnut_plugin_impl.h"
#include "test_support.h"

#include "mxhash/encoding.h"

#include <QtCore/QCommandLineParser>
#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QMap>
#include <QtCore/QTimer>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpSocket>
#include <cstdio>

using namespace gnuttest;

static QElapsedTimer g_clock;
static void out(const QString &s)
{
    const QString line = QStringLiteral("[%1s] %2\n").arg(g_clock.elapsed() / 1000.0, 7, 'f', 1).arg(s);
    fputs(line.toUtf8().constData(), stdout);
    fflush(stdout);
}

struct Candidate {
    mx::SearchHit hit;
    QList<mx::SearchSource> sources;
    int score = 0;
};

// Only names that carry an unambiguous licence marker count: Project Gutenberg / LibriVox
// (public-domain texts and audiobooks) and Creative Commons / Jamendo releases. "public domain"
// alone does NOT count: it is also the name of a commercial trance act.
static int legalScore(const QString &nameIn)
{
    const QString n = nameIn.toLower();
    static const char *bad[] = {"crack", "keygen", "serial", "xxx", "porn", "torrent", ".exe", ".scr", ".rar", "patch",
                                "activator", "warez", "full version", "hack", "sex", ".lnk", ".vbs", ".bat"};
    for (const char *k : bad) if (n.contains(QLatin1String(k))) return 0;
    const bool text = n.endsWith(QLatin1String(".txt")) || n.endsWith(QLatin1String(".epub")) || n.endsWith(QLatin1String(".pdf")) ||
                      n.endsWith(QLatin1String(".html")) || n.endsWith(QLatin1String(".mobi"));
    int s = 0;
    if (n.contains(QLatin1String("gutenberg")) && text) s += 20;
    if (n.contains(QLatin1String("librivox"))) s += 15;
    if (n.contains(QLatin1String("creative commons")) || n.contains(QLatin1String("creative_commons")) ||
        n.contains(QLatin1String("cc-by")) || n.contains(QLatin1String("cc by-"))) s += 10;
    if (n.contains(QLatin1String("jamendo"))) s += 5;
    return s;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    g_clock.start();
    QCommandLineParser p;
    p.addOption({QStringLiteral("minutes"), QStringLiteral("run time"), QStringLiteral("n"), QStringLiteral("25")});
    p.addOption({QStringLiteral("profile"), QStringLiteral("profile dir"), QStringLiteral("dir"),
                 QCoreApplication::applicationDirPath() + QStringLiteral("/../gnutella-probe-profile")});   // inside the build tree
    p.addOption({QStringLiteral("port"), QStringLiteral("listen port"), QStringLiteral("n"), QStringLiteral("6346")});
    p.addOption({QStringLiteral("query"), QStringLiteral("comma separated queries"), QStringLiteral("q"),
                 QStringLiteral("creative commons,gutenberg,librivox,project gutenberg,jamendo,linux,public domain,ogg")});
    p.addOption({QStringLiteral("no-download"), QStringLiteral("skip the download")});
    p.addOption({QStringLiteral("ultrapeers"), QStringLiteral("max ultrapeers"), QStringLiteral("n"), QStringLiteral("4")});
    p.addOption({QStringLiteral("encryption"), QStringLiteral("[enc-1] gnutella/encryption: enabled | forced | disabled"),
                 QStringLiteral("mode"), QStringLiteral("enabled")});
    p.process(app);
    const int minutes = p.value(QStringLiteral("minutes")).toInt();
    const QString profile = p.value(QStringLiteral("profile"));
    QDir().mkpath(profile);

    MemSettings settings;
    settings.setValue(QStringLiteral("gnutella/enabled"), true);
    settings.setValue(QStringLiteral("gnutella/port"), p.value(QStringLiteral("port")).toInt());
    settings.setValue(QStringLiteral("gnutella/maxUltrapeers"), p.value(QStringLiteral("ultrapeers")).toInt());
    settings.setValue(QStringLiteral("gnutella/browseHost"), true);
    settings.setValue(QStringLiteral("gnutella/encryption"), p.value(QStringLiteral("encryption")));
    settings.setValue(QStringLiteral("connection/firewalled"), QStringLiteral("auto"));
    settings.setValue(QStringLiteral("transfers/maxUploads"), 2);
    FileLibrary lib;
    CountingLimiter limiter;
    // share a few small public-domain texts we write ourselves, so our QRP table is not empty and
    // we can answer real queries for them
    const QString shareDir = profile + QStringLiteral("/share");
    QList<mx::SharedFile> shared;
    const QStringList texts = {
        QStringLiteral("OctoMX Gnutella test - public domain notice.txt"),
        QStringLiteral("Public Domain - The US Declaration of Independence 1776.txt"),
    };
    for (const QString &t : texts) {
        const QString path = shareDir + QLatin1Char('/') + t;
        QByteArray body = QByteArray("This text file is in the public domain. Shared by the OctoMX Gnutella probe.\n");
        body += QByteArray("When in the Course of human events, it becomes necessary for one people to dissolve the political bands...\n").repeated(40);
        writeFile(path, body);
        shared << lib.addFile(path);
    }
    mx::NetworkPlugin *plugin = mx::createGnutellaPlugin(&app);
    auto *impl = static_cast<mx::gnut::GnutellaPlugin *>(plugin);
    plugin->init(makeContext(&settings, &lib, &limiter, profile));
    plugin->setSharedFiles(shared);

    QObject::connect(plugin, &mx::NetworkPlugin::logMessage, [](const QString &s) { out(s); });
    qint64 firstConnectMs = -1;
    QObject::connect(plugin, &mx::NetworkPlugin::statusChanged, [&](const mx::NetworkStatus &st) {
        out(QStringLiteral("STATUS %1 | firewalled=%2 port=%3 ext=%4").arg(st.statusText).arg(st.firewalled).arg(st.listenPort).arg(st.externalAddress));
        if (st.peers > 0 && firstConnectMs < 0) firstConnectMs = g_clock.elapsed();
    });
    QObject::connect(plugin, &mx::NetworkPlugin::uploadChanged, [](const mx::TransferInfo &t) {
        out(QStringLiteral("UPLOAD %1 %2 %3/%4 to %5").arg(t.statusText, t.name).arg(t.done).arg(t.size).arg(t.peer.address));
    });

    // ---- searches
    QStringList queries = p.value(QStringLiteral("query")).split(QLatin1Char(','), Qt::SkipEmptyParts);
    QMap<quint32, QString> queryText;
    QMap<quint32, QList<mx::SearchHit>> results;
    QMap<QString, Candidate> candidates;   // by sha1 hex
    QList<quint32> pendingDone;
    quint32 nextId = 1;
    QObject::connect(plugin, &mx::NetworkPlugin::searchHits, [&](quint32 id, const QList<mx::SearchHit> &hits) {
        results[id] += hits;
        for (const mx::SearchHit &h : hits) {
            if (h.hashes.sha1.isEmpty()) continue;
            const QString k = mx::hash::toHex(h.hashes.sha1);
            Candidate &c = candidates[k];
            c.hit = h;
            c.sources += h.sources;
            c.score = legalScore(h.fileName);
        }
    });
    QObject::connect(plugin, &mx::NetworkPlugin::searchDone, [&](quint32 id) {
        const QList<mx::SearchHit> &hs = results[id];
        QMap<QString, int> vendors;
        int fw = 0, withSha1 = 0, withTth = 0, withBitrate = 0, tls = 0;
        QSet<QString> hosts;
        for (const mx::SearchHit &h : hs) {
            const mx::SearchSource &s = h.sources.first();
            vendors[s.extra.value(QStringLiteral("vendor")).toString()]++;
            if (s.extra.value(QStringLiteral("firewalled")).toBool()) ++fw;
            if (s.uri.contains(QLatin1String("tls=1"))) ++tls;   // [enc-1] the servent announced TLS
            if (!h.hashes.sha1.isEmpty()) ++withSha1;
            if (!h.hashes.tth.isEmpty()) ++withTth;
            if (h.bitrateKbps > 0) ++withBitrate;
            hosts.insert(s.user.id);
        }
        QStringList vl;
        for (auto it = vendors.cbegin(); it != vendors.cend(); ++it) vl << QStringLiteral("%1=%2").arg(it.key().isEmpty() ? QStringLiteral("?") : it.key()).arg(it.value());
        out(QStringLiteral("SEARCH \"%1\": %2 results from %3 hosts; firewalled %4 (%5%); sha1 %6, tth %7, bitrate %8; vendors: %9")
                .arg(queryText[id]).arg(hs.size()).arg(hosts.size()).arg(fw).arg(hs.isEmpty() ? 0 : fw * 100 / int(hs.size()))
                .arg(withSha1).arg(withTth).arg(withBitrate).arg(vl.join(QStringLiteral(", "))));
        out(QStringLiteral("SEARCH TLS \"%1\": %2 of %3 results from servents that announce TLS").arg(queryText[id]).arg(tls).arg(hs.size()));
        for (int i = 0; i < qMin(8, int(hs.size())); ++i) {
            const mx::SearchHit &h = hs[i];
            out(QStringLiteral("   %1 | %2 bytes | %3 | %4 | %5").arg(h.fileName.left(70)).arg(h.size)
                    .arg(h.sources.first().user.name, h.sources.first().connection, h.sources.first().extra.value(QStringLiteral("vendor")).toString()));
        }
    });

    // ---- download
    bool downloading = false, downloadDone = false;
    QString downloadReport = QStringLiteral("not attempted");
    QSet<QString> triedCandidates;
    QList<QPointer<mx::RangeSource>> activeSources;
    MemDownloader *dl = nullptr;
    QString dlKey;
    QElapsedTimer dlClock;
    std::function<void()> tryDownload = [&] {
        if (downloading || downloadDone || p.isSet(QStringLiteral("no-download"))) return;
        // best legal candidate: score, number of sources, direct sources, 50 kB..60 MB
        Candidate *best = nullptr;
        for (auto it = candidates.begin(); it != candidates.end(); ++it) {
            Candidate &c = *it;
            if (triedCandidates.contains(it.key()) || c.score <= 0) continue;
            if (c.hit.size < 50 * 1024 || c.hit.size > 60 * 1024 * 1024) continue;
            int direct = 0;
            for (const mx::SearchSource &s : c.sources) if (!s.extra.value(QStringLiteral("firewalled")).toBool()) ++direct;
            if (direct == 0 && impl->firewalled()) continue;
            const int sc = c.score * 10 + int(c.sources.size()) * 3 + direct * 5;
            if (!best || sc > best->score * 10 + int(best->sources.size()) * 3) best = &c;
        }
        if (!best) return;
        dlKey = mx::hash::toHex(best->hit.hashes.sha1);
        triedCandidates.insert(dlKey);
        downloading = true;
        dlClock.start();
        out(QStringLiteral("DOWNLOAD candidate \"%1\" (%2 bytes, sha1 %3, %4 sources)").arg(best->hit.fileName).arg(best->hit.size)
                .arg(mx::hash::toBase32(best->hit.hashes.sha1)).arg(best->sources.size()));
        dl = new MemDownloader(best->hit.size, 0, -1, &app);
        const QString dlId = QStringLiteral("probe-") + dlKey.left(8);
        plugin->findSources(dlId, best->hit.hashes, best->hit.size, best->hit.fileName);
        int n = 0;
        for (const mx::SearchSource &s : best->sources) {
            if (n >= 6) break;
            mx::RangeSource *rs = plugin->openSource(dlId, best->hit.hashes, best->hit.size, s);
            if (!rs) continue;
            rs->setParent(dl);
            QObject::connect(rs, &mx::RangeSource::stateChanged, [rs, s] {
                out(QStringLiteral("  source %1: %2 [%3]").arg(s.user.name, rs->stateText(), rs->clientName()));
            });
            dl->addSource(rs);
            activeSources << rs;
            ++n;
        }
        const qint64 size = best->hit.size;
        const QByteArray sha1 = best->hit.hashes.sha1;
        const QString name = best->hit.fileName;
        QObject::connect(dl, &MemDownloader::finished, [&, size, sha1, name] {
            const QByteArray data = dl->bytes();
            const QByteArray got = QCryptographicHash::hash(data, QCryptographicHash::Sha1);
            const bool ok = got == sha1;
            downloadReport = QStringLiteral("%1 \"%2\" %3 bytes in %4 s, SHA-1 %5").arg(ok ? "OK" : "MISMATCH", name).arg(size)
                                 .arg(dlClock.elapsed() / 1000).arg(ok ? QStringLiteral("verified ") + mx::hash::toBase32(got) : QStringLiteral("WRONG"));
            out(QStringLiteral("DOWNLOAD %1").arg(downloadReport));
            if (ok) {
                writeFile(profile + QStringLiteral("/downloaded/") + name, data);
                downloadDone = true;
            }
            downloading = false;
            dl->deleteLater();
            dl = nullptr;
            plugin->stopFindSources(QStringLiteral("probe-") + dlKey.left(8));
        });
        // give up on this candidate after 6 minutes
        QTimer::singleShot(6 * 60 * 1000, &app, [&, key = dlKey] {
            if (!downloading || dlKey != key || !dl) return;
            out(QStringLiteral("DOWNLOAD of %1 gave up after 6 min: %2 of %3 bytes; log: %4").arg(key.left(8)).arg(dl->received())
                    .arg(dl->bytes().size()).arg(dl->log().mid(qMax(0, int(dl->log().size()) - 6)).join(QStringLiteral(" | "))));
            downloadReport = QStringLiteral("gave up on %1 (%2 bytes received)").arg(key.left(8)).arg(dl->received());
            dl->deleteLater();
            dl = nullptr;
            downloading = false;
        });
    };
    QObject::connect(plugin, &mx::NetworkPlugin::sourcesFound, [&](const QString &id, const QList<mx::SearchSource> &ss) {
        out(QStringLiteral("SOURCES FOUND for %1: %2").arg(id).arg(ss.size()));
        if (!dl) return;
        for (const mx::SearchSource &s : ss) {
            auto it = candidates.find(dlKey);
            if (it == candidates.end()) break;
            mx::RangeSource *rs = plugin->openSource(id, it->hit.hashes, it->hit.size, s);
            if (!rs) continue;
            rs->setParent(dl);
            dl->addSource(rs);
        }
    });
    QObject::connect(plugin, &mx::NetworkPlugin::hashSetLearned, [](const QString &id, const QString &kind, qint64 unit, const QList<QByteArray> &h) {
        out(QStringLiteral("HASHSET %1 %2: %3 x %4 bytes").arg(id, kind).arg(h.size()).arg(unit));
    });

    // ---- browse
    bool browsed = false;
    QSet<QString> browseTried;
    QObject::connect(plugin, &mx::NetworkPlugin::browseResults, [&](quint32, const mx::UserRef &u, const QList<mx::BrowseEntry> &e, bool fin) {
        static int total = 0;
        total += int(e.size());
        if (fin) out(QStringLiteral("BROWSE %1 (%2): %3 files").arg(u.address, u.id).arg(total));
    });
    QObject::connect(plugin, &mx::NetworkPlugin::browseFailed, [&](quint32, const mx::UserRef &u, const QString &why) {
        out(QStringLiteral("BROWSE %1 failed: %2").arg(u.address, why));
        browsed = false;
    });

    // ---- a second local instance "B" on the real network, sharing a file with a unique token:
    // A's query for the token must be routed by real ultrapeers to B (QRP) and B's hit back to A.
    const QString token = QStringLiteral("wmxng%1probe").arg(QDateTime::currentSecsSinceEpoch() % 100000);
    MemSettings settingsB;
    for (const QString &k : settings.keys()) settingsB.setValue(k, settings.value(k));
    settingsB.setValue(QStringLiteral("gnutella/port"), p.value(QStringLiteral("port")).toInt() + 1);
    settingsB.setValue(QStringLiteral("gnutella/maxUltrapeers"), 3);
    FileLibrary libB;
    const QString pathB = profile + QStringLiteral("/shareB/") + token + QStringLiteral(" public domain test text.txt");
    writeFile(pathB, QByteArray("Public domain test text written by the OctoMX probe. ").repeated(500) + token.toUtf8());
    const mx::SharedFile fileB = libB.addFile(pathB);
    mx::NetworkPlugin *pluginB = mx::createGnutellaPlugin(&app);
    auto *implB = static_cast<mx::gnut::GnutellaPlugin *>(pluginB);
    QDir().mkpath(profile + QStringLiteral("B"));
    pluginB->init(makeContext(&settingsB, &libB, &limiter, profile + QStringLiteral("B")));
    pluginB->setSharedFiles({fileB});
    QObject::connect(pluginB, &mx::NetworkPlugin::logMessage, [](const QString &s) { out(QStringLiteral("[B] ") + s); });
    QObject::connect(pluginB, &mx::NetworkPlugin::uploadChanged, [](const mx::TransferInfo &t) {
        out(QStringLiteral("[B] UPLOAD %1 %2 %3/%4 to %5").arg(t.statusText, t.name).arg(t.done).arg(t.size).arg(t.peer.address));
    });
    bool tokenSearched = false, tokenDone = false;
    quint32 tokenQuery = 0;
    QString tokenReport = QStringLiteral("not run");
    plugin->start();
    pluginB->start();
    int qi = 0;
    qint64 lastSearchMs = -100000;
    QTimer tick;
    tick.setInterval(5000);
    QObject::connect(&tick, &QTimer::timeout, [&] {
        const qint64 now = g_clock.elapsed();
        if (now > qint64(minutes) * 60000) {
            const QVariantMap st = impl->debugStats();
            out(QStringLiteral("==== SUMMARY after %1 min").arg(minutes));
            out(QStringLiteral("first ultrapeer connection after %1 s").arg(firstConnectMs < 0 ? -1 : firstConnectMs / 1000.0));
            for (const QString &l : impl->connectionLines()) out(QStringLiteral("UP ") + l);
            QStringList sl;
            for (auto it = st.cbegin(); it != st.cend(); ++it) sl << QStringLiteral("%1=%2").arg(it.key(), it.value().toString());
            out(QStringLiteral("STATS ") + sl.join(QStringLiteral(" ")));
            for (const mx::DiagItem &i : impl->diagnostics().items)   // [enc-1] the Connection Test row
                if (i.id == QLatin1String("gnutella.encryption")) out(QStringLiteral("ENCRYPTION ROW %1 | %2 | level %3").arg(i.result, i.details).arg(int(i.level)));
            out(QStringLiteral("incoming agents: ") + st.value(QStringLiteral("incomingAgents")).toStringList().join(QStringLiteral("; ")));
            out(QStringLiteral("DOWNLOAD RESULT: ") + downloadReport);
            out(QStringLiteral("TOKEN RESULT: ") + tokenReport);
            for (const QString &l : implB->connectionLines()) out(QStringLiteral("B UP ") + l);
            {
                const QVariantMap sb = implB->debugStats();
                QStringList l2;
                for (auto it = sb.cbegin(); it != sb.cend(); ++it) l2 << QStringLiteral("%1=%2").arg(it.key(), it.value().toString());
                out(QStringLiteral("B STATS ") + l2.join(QStringLiteral(" ")));
            }
            pluginB->stop();
            int servers = 0;
            for (const mx::ServerEntry &e : plugin->servers()) if (e.state == mx::ConnState::Disconnected) ++servers;
            out(QStringLiteral("remembered hosts listed: %1").arg(servers));
            plugin->stop();
            QTimer::singleShot(500, &app, &QCoreApplication::quit);
            tick.stop();
            return;
        }
        if (impl->connectedCount() == 0) return;
        if (qi < queries.size() && now - lastSearchMs > 50000 && (impl->connectedCount() >= 2 || now > 90000)) {
            mx::SearchQuery q;
            q.id = nextId++;
            q.text = queries[qi++];
            queryText[q.id] = q.text;
            lastSearchMs = now;
            out(QStringLiteral("searching \"%1\" (#%2) via %3 ultrapeers").arg(q.text).arg(q.id).arg(impl->connectedCount()));
            plugin->search(q);
        }
        if (qi >= 2) tryDownload();
        // live end-to-end through the real network: A searches B's unique token
        if (!tokenSearched && now > 150000 && implB->connectedCount() > 0) {
            tokenSearched = true;
            mx::SearchQuery q;
            q.id = tokenQuery = 900;
            q.text = token;
            queryText[q.id] = q.text;
            out(QStringLiteral("searching B's unique token \"%1\" (A has %2 UPs, B has %3 UPs: %4)").arg(token).arg(impl->connectedCount())
                    .arg(implB->connectedCount()).arg(implB->connectionLines().join(QStringLiteral(" | "))));
            plugin->search(q);
        }
        if (tokenSearched && !tokenDone && now > 150000 + 90000) {
            tokenDone = true;
            int fromB = 0;
            mx::SearchHit hitB;
            for (const mx::SearchHit &h : results.value(tokenQuery))
                if (h.sources.first().user.id == mx::gnut::guidHex(implB->serventGuid())) { ++fromB; hitB = h; }
            const QVariantMap sb = implB->debugStats();
            tokenReport = QStringLiteral("hits from B via the real network: %1; B saw %2 queries, answered %3")
                              .arg(fromB).arg(sb.value(QStringLiteral("queriesSeen")).toInt()).arg(sb.value(QStringLiteral("queriesAnswered")).toInt());
            out(QStringLiteral("TOKEN ") + tokenReport);
            // browse B and fetch its file directly over the loopback interface (NAT hairpinning is not reliable)
            mx::UserRef u;
            u.net = mx::Net::Gnutella;
            u.id = QStringLiteral("127.0.0.1:%1").arg(implB->listenPort());
            u.address = u.id;
            plugin->browse(4242, u);
            mx::SearchSource src;
            mx::gnut::SourceDesc d;
            d.addr = mx::gnut::HostPort::parse(u.address);
            d.sha1 = fileB.hashes.sha1;
            d.name = fileB.name;
            src.net = mx::Net::Gnutella;
            src.uri = d.toUri();
            mx::RangeSource *rs = plugin->openSource(QStringLiteral("probe-B"), fileB.hashes, fileB.size, src);
            auto *dlB = new MemDownloader(fileB.size, 0, -1, &app);
            rs->setParent(dlB);
            dlB->addSource(rs);
            QObject::connect(dlB, &MemDownloader::finished, [dlB, fileB] {
                const bool ok = QCryptographicHash::hash(dlB->bytes(), QCryptographicHash::Sha1) == fileB.hashes.sha1;
                out(QStringLiteral("LOCAL download from B: %1 bytes, SHA-1 %2").arg(dlB->bytes().size()).arg(ok ? "verified" : "MISMATCH"));
            });
            // and a raw Gnutella handshake to B: a leaf must answer 503 + X-Try-Ultrapeers
            auto *hs = new QTcpSocket(&app);
            QObject::connect(hs, &QTcpSocket::connected, [hs] { hs->write("GNUTELLA CONNECT/0.6\r\nUser-Agent: ProbeTester/1.0\r\nX-Ultrapeer: True\r\n\r\n"); });
            QObject::connect(hs, &QTcpSocket::readyRead, [hs] {
                const QByteArray r = hs->readAll();
                out(QStringLiteral("handshake answer from B: ") + QString::fromLatin1(r).replace(QStringLiteral("\r\n"), QStringLiteral(" | ")).left(300));
                hs->deleteLater();
            });
            hs->connectToHost(QHostAddress::LocalHost, implB->listenPort());
        }
        if (!browsed && now > 240000) {
            // browse a direct host that advertises browse-host
            for (const Candidate &c : candidates) {
                for (const mx::SearchSource &s : c.sources) {
                    if (s.extra.value(QStringLiteral("browse")).toBool() && !s.extra.value(QStringLiteral("firewalled")).toBool() &&
                        !browseTried.contains(s.user.address)) {
                        browseTried.insert(s.user.address);
                        browsed = true;
                        out(QStringLiteral("browsing %1 (%2)").arg(s.user.address, s.extra.value(QStringLiteral("vendor")).toString()));
                        plugin->browse(1000 + qi, s.user);
                        break;
                    }
                }
                if (browsed) break;
            }
        }
        if (int(now / 1000) % 120 < 5) {
            const QVariantMap st = impl->debugStats();
            out(QStringLiteral("periodic: %1 UPs, %2 hosts known, queries seen %3, answered %4, incoming http %5, handshakes %6, firewalled %7")
                    .arg(st.value(QStringLiteral("connected")).toInt()).arg(st.value(QStringLiteral("hostsKnown")).toInt())
                    .arg(st.value(QStringLiteral("queriesSeen")).toInt()).arg(st.value(QStringLiteral("queriesAnswered")).toInt())
                    .arg(st.value(QStringLiteral("incomingHttp")).toInt()).arg(st.value(QStringLiteral("incomingHandshakes")).toInt())
                    .arg(st.value(QStringLiteral("firewalled")).toBool()));
        }
    });
    tick.start();
    return app.exec();
}
