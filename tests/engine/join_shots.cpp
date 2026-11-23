// SPDX-License-Identifier: GPL-3.0-or-later
// join_shots [eng-1]: pictures of the Transfers row of a download joined with a torrent, in every
// state of the BitTorrent line, as the program draws it (the real engine, the real TransferModel in
// the classic list, offscreen), from a fake swarm (engine_fakes.h): deterministic, no real peer and
// no address in any picture. Not a ctest.
//
//   join_shots <out dir>
#include <QtTest/QtTest>
#include <QtWidgets/QApplication>

#include "engine_env.h"
#include "ui/kit/colors.h"
#include "ui/kit/list_view.h"
#include "ui/kit/scale.h"
#include "ui/kit/theme.h"
#include "ui/models/transfer_model.h"

using namespace mx;
using namespace mx::etest;
using mx::ui::MxListView;
using mx::ui::TransferModel;

namespace {

const QString kFile = QStringLiteral("Big Buck Bunny.mp4");
const QString kFolder = QStringLiteral("Big Buck Bunny");

SearchSource btCandidate(const QString &tid, int seeds, int peers)
{
    SearchSource bt;
    bt.net = Net::BitTorrent;
    bt.uri = QStringLiteral("magnet:?xt=urn:btih:%1&dn=%2").arg(tid, kFolder);
    bt.seeds = seeds;
    bt.peers = peers;
    FileHashes h;
    h.btih = QByteArray::fromHex(tid.toLatin1());
    bt.extra.insert(QStringLiteral("candidate"), true);
    bt.extra.insert(QStringLiteral("hashes"), h.toVariant());
    bt.extra.insert(QStringLiteral("insideBundle"), kFolder);
    return bt;
}

QString btLine(const TransferInfo &t)
{
    for (const TransferSource &s : t.sources) if (s.net == Net::BitTorrent) return s.statusText;
    return QString();
}

// one picture of the download list: that download, expanded
bool shot(Env &env, const QString &id, const QString &file)
{
    TransferModel model(env.engine.get(), false, &env.settings);
    MxListView v;
    v.setRowHeight(14);
    v.setTreeMode(true);
    v.setModel(&model);
    v.setDefaultColumnWidths({250, 170, 96, 110, 300, 62, 60});
    v.resize(1060, 150);
    v.show();
    if (!QTest::qWaitForWindowExposed(&v)) return false;
    QTest::qWait(1200);   // the model batches updates
    v.expandAll();
    QTest::qWait(300);
    const bool ok = v.grab().save(file);
    std::printf("%s: %s\n", qPrintable(QFileInfo(file).fileName()), qPrintable(btLine(env.engine->transfer(id))));
    return ok;
}

struct Case {
    FakeTorrentBackend be;
    Env env;
    TestPlugin *ed = nullptr;
    QByteArray data;
    explicit Case(const QString &name, int sizeMb) : env(name)
    {
        ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
        data = randomBytes(sizeMb * 1000 * 1000, 700 + sizeMb);
    }
    SearchSource edSource(int bps, const QString &user)
    {
        SourceConfig c;
        c.data = data;
        c.bytesPerSec = bps;
        return ed->addSource(QStringLiteral("ed2k-peer://10.9.0.%1:4662").arg(qHash(user) % 200 + 1), c, user);
    }
    QString start(const QList<SearchSource> &bt, int edBps)
    {
        env.engineUp(&be);
        DownloadRequest r;
        r.name = kFile;
        r.size = data.size();
        r.hashes = hashesOf(data, 2);
        r.sources = {edSource(edBps, QStringLiteral("eMule user"))};
        r.sources << bt;
        return env.engine->download(r);
    }
};

bool waitLine(Env &env, const QString &id, const QString &start, int ms)
{
    return QTest::qWaitFor([&] { return btLine(env.engine->transfer(id)).startsWith(start); }, ms);
}

} // namespace

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    registerMetaTypes();
    const QString out = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("shots");
    QDir().mkpath(out);
    int failed = 0;
    auto look = [](Env &env) {
        ui::Colors::instance()->attach(&env.settings);
        ui::applyUiScale(&env.settings);
    };
    bool lookInstalled = false;
    auto check = [&](bool ok, const char *what) { if (!ok) { std::printf("FAILED: %s\n", what); ++failed; } };

    {   // 1. the torrent is known, its file list is coming
        Case c(QStringLiteral("shot-fetching"), 6);
        look(c.env);
        if (!lookInstalled) { ui::installClassicLook(&app); lookInstalled = true; }
        const QString tid = c.be.makeTorrent(kFolder, {{kFile, c.data}}, 256 * 1024);
        c.be.known[tid].metaPeers = 12;
        c.be.metadataDelayMs = 120000;
        const QString id = c.start({btCandidate(tid, 418, 97)}, 30 * 1024);
        check(waitLine(c.env, id, QStringLiteral("Getting the torrent's file list"), 8000), "fetching");
        check(shot(c.env, id, out + QStringLiteral("/1-getting-file-list.png")), "shot 1");
    }
    {   // 2. nobody answers for the first torrent; another one is known
        Case c(QStringLiteral("shot-another"), 6);
        look(c.env);
        c.env.settings.setValue(QString(keys::transfers::MetadataTimeoutSec), 60);
        const QString dead = c.be.makeTorrent(kFolder + QLatin1Char(' '), {{kFile, randomBytes(6 * 1000 * 1000, 801)}}, 256 * 1024);
        c.be.known.remove(dead);
        const QString other = c.be.makeTorrent(kFolder, {{kFile, c.data}}, 256 * 1024);
        c.be.metadataDelayMs = 120000;
        const QString id = c.start({btCandidate(dead, 50, 10), btCandidate(other, -1, -1)}, 30 * 1024);
        check(waitLine(c.env, id, QStringLiteral("No peers for this torrent yet, trying another torrent"), 8000), "another");
        QTest::qWait(1500);
        check(shot(c.env, id, out + QStringLiteral("/2-no-peers-trying-another.png")), "shot 2");
    }
    {   // 3. the only torrent: nobody answers, it is announced again
        Case c(QStringLiteral("shot-again"), 6);
        look(c.env);
        const QString dead = c.be.makeTorrent(kFolder, {{kFile, randomBytes(6 * 1000 * 1000, 802)}}, 256 * 1024);
        c.be.known.remove(dead);
        const QString id = c.start({btCandidate(dead, 3, 1)}, 30 * 1024);
        check(waitLine(c.env, id, QStringLiteral("No peers for this torrent yet, trying again in"), 8000), "again");
        QTest::qWait(1500);
        check(shot(c.env, id, out + QStringLiteral("/3-no-peers-trying-again.png")), "shot 3");
    }
    {   // 4. the torrent's file is another one: dropped
        Case c(QStringLiteral("shot-dropped"), 3);
        look(c.env);
        const QString wrong = c.be.makeTorrent(kFolder, {{kFile, randomBytes(3 * 1000 * 1000, 803)}}, 128 * 1024);
        c.be.metadataDelayMs = 2500;
        const QString id = c.start({btCandidate(wrong, 20, 5)}, 150 * 1024);
        check(QTest::qWaitFor([&] {
            for (const TransferSource &s : c.env.engine->transfer(id).sources)
                if (s.statusText.contains(QLatin1String("different file: dropped"))) return true;
            return false;
        }, 20000), "dropped");
        check(shot(c.env, id, out + QStringLiteral("/4-file-does-not-match-dropped.png")), "shot 4");
    }
    {   // 5. joined: BitTorrent delivers into the same download
        Case c(QStringLiteral("shot-joined"), 40);
        look(c.env);
        const QString tid = c.be.makeTorrent(kFolder, {{kFile, c.data}}, 256 * 1024);
        c.be.known[tid].peers = 3;
        c.be.known[tid].swarmTotal = 14;
        c.be.known[tid].peerPiecesPerTick = 1;
        c.be.metadataDelayMs = 1500;
        const QString id = c.start({btCandidate(tid, 418, 97)}, 30 * 1024);
        check(QTest::qWaitFor([&] { return c.env.engine->transfer(id).done > 4 * 1000 * 1000; }, 30000), "joined");
        check(shot(c.env, id, out + QStringLiteral("/5-joined-delivering.png")), "shot 5");
        c.env.engine->cancel(id, true);
    }
    std::printf("%s\n", failed ? "some pictures FAILED" : "all pictures taken");
    return failed ? 1 : 0;
}
