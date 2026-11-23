// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX: bt_probe: live proof of the BitTorrent plugin on the real network, through the public
// interfaces only (NetworkPlugin / TorrentBackend / BitTorrentControl + mx::SimpleSettings).
//
//   bt_probe --profile <dir> [--magnet <uri>] [--target-mb 50] [--cleanup]
//       bootstrap DHT → add magnet → metadata from the swarm → download ≥ target → pause → resume →
//       quit (stop + destroy) → restart on the same profile → verify restored → resume downloading.
//   bt_probe --profile <dir> --https-check
//       announce to a host with a valid certificate (expect an HTTP error, i.e. TLS verified OK) and to
//       self-signed.badssl.com (expect a certificate verification failure).
#include <QtCore/QCommandLineParser>
#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QLoggingCategory>
#include <QtCore/QTimer>

#include <cstdio>
#include <functional>
#include <memory>

#include "bt_plugin.h"
#include "mx/settings_keys.h"
#include "mx/simple_settings.h"

namespace {

QElapsedTimer g_clock;
QStringList g_trackerLog;
QtMessageHandler g_prev = nullptr;
bool g_verbose = false;

void out(const QString &s)
{
    std::printf("[%7.1fs] %s\n", g_clock.elapsed() / 1000.0, s.toLocal8Bit().constData());
    std::fflush(stdout);
}

void handler(QtMsgType t, const QMessageLogContext &ctx, const QString &msg)
{
    if (ctx.category && qstrcmp(ctx.category, "mx.bt") == 0) {
        if (msg.contains(QLatin1String("badssl")) || msg.contains(QLatin1String("example.com")))
            g_trackerLog << msg;
        if (t == QtDebugMsg && !g_verbose)
            return;
        out(QStringLiteral("  log: ") + msg);
        return;
    }
    if (g_prev)
        g_prev(t, ctx, msg);
}

QString mb(qint64 b) { return QString::number(b / 1048576.0, 'f', 1) + QStringLiteral(" MB"); }
QString kbs(int b) { return QString::number(b / 1024.0, 'f', 0) + QStringLiteral(" KiB/s"); }

// wait in the event loop until pred() or timeout; calls every(...) once per second
bool waitUntil(const std::function<bool()> &pred, int timeoutMs, const std::function<void(int)> &every = {})
{
    QElapsedTimer el;
    el.start();
    int sec = 0;
    while (!pred()) {
        if (el.elapsed() > timeoutMs)
            return false;
        QEventLoop loop;
        QTimer::singleShot(250, &loop, &QEventLoop::quit);
        loop.exec();
        if (el.elapsed() / 1000 > sec) {
            sec = int(el.elapsed() / 1000);
            if (every)
                every(sec);
        }
    }
    return true;
}

struct Instance {
    mx::SimpleSettings settings;
    std::unique_ptr<mx::NetworkPlugin> plugin;
    mx::TorrentBackend *be = nullptr;
    mx::BitTorrentControl *ctl = nullptr;
    int pieces = 0, failed = 0;
    void create(const QString &profile)
    {
        settings.setValue(QString(mx::keys::connection::RandomizePorts), true);
        plugin.reset(mx::createBitTorrentPlugin());
        be = dynamic_cast<mx::TorrentBackend *>(plugin.get());
        ctl = dynamic_cast<mx::BitTorrentControl *>(plugin.get());
        auto *ev = qobject_cast<mx::BitTorrentEvents *>(be->torrentEvents());
        QObject::connect(ev, &mx::TorrentEvents::torrentPieceFinished, ev, [this](const QString &, int) { ++pieces; });
        QObject::connect(ev, &mx::TorrentEvents::torrentPieceFailed, ev, [this](const QString &, int) { ++failed; });
        QObject::connect(ev, &mx::TorrentEvents::torrentMetadata, ev, [](const QString &id) { out(QStringLiteral("torrentMetadata %1").arg(id)); });
        QObject::connect(ev, &mx::TorrentEvents::torrentError, ev, [](const QString &id, const QString &e) {
            out(QStringLiteral("torrentError %1: %2").arg(id, e));
        });
        QObject::connect(plugin.get(), &mx::NetworkPlugin::logMessage, plugin.get(), [](const QString &m) { out(QStringLiteral("  plugin: ") + m); });
        mx::PluginContext ctx;
        ctx.settings = &settings;
        ctx.profileDir = profile;
        ctx.dataDir = profile + QStringLiteral("/bt");
        ctx.clientName = QStringLiteral("OctoMX");
        ctx.clientVersion = QStringLiteral(MX_VERSION_STRING);
        plugin->init(ctx);
    }
    QString statusLine() const
    {
        const mx::NetworkStatus s = plugin->status();
        return QStringLiteral("net: state=%1 '%2' port=%3 ext=%4 firewalled=%5 down=%6 up=%7")
            .arg(int(s.state)).arg(s.statusText).arg(s.listenPort).arg(s.externalAddress.isEmpty() ? QStringLiteral("?") : s.externalAddress)
            .arg(s.firewalled).arg(kbs(s.rateDownBps), kbs(s.rateUpBps));
    }
    QString torrentLine(const QString &id) const
    {
        const mx::TorrentSnapshot t = be->snapshot(id);
        return QStringLiteral("torrent: %1 | %2 / %3 | down %4 up %5 | peers %6 seeds %7 (swarm %8/%9) | pieces %10/%11 | '%12'")
            .arg(t.name.left(40), mb(t.totalDone), mb(t.totalSize), kbs(t.rateDownBps), kbs(t.rateUpBps))
            .arg(t.peers).arg(t.seeds).arg(t.seedsTotal).arg(t.peersTotal).arg(t.have.count(true)).arg(t.numPieces).arg(t.statusText);
    }
};

int httpsCheck(const QString &profile)
{
    Instance in;
    in.create(profile);
    in.plugin->start();
    QString err;
    const QString ok = in.be->addTorrent(
        QStringLiteral("magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567&dn=probe-ok&tr=https%3A%2F%2Fwww.example.com%2Fannounce"),
        {}, profile + QStringLiteral("/dl"), false, {}, &err);
    const QString bad = in.be->addTorrent(
        QStringLiteral("magnet:?xt=urn:btih:76543210fedcba9876543210fedcba9876543210&dn=probe-bad&tr=https%3A%2F%2Fself-signed.badssl.com%2Fannounce"),
        {}, profile + QStringLiteral("/dl"), false, {}, &err);
    out(QStringLiteral("added %1 and %2; waiting for tracker replies").arg(ok, bad));
    // outcomes that come from a real TLS exchange (not "skipping ... unreachable" for other interfaces)
    bool okVerified = false, okCertError = false, badRejected = false;
    auto evaluate = [&] {
        okVerified = okCertError = badRejected = false;
        for (const QString &m : std::as_const(g_trackerLog)) {
            const QString l = m.toLower();
            const bool cert = l.contains(QLatin1String("certificate")) || l.contains(QLatin1String("verify failed"));
            if (l.contains(QLatin1String("example.com"))) {
                if (cert)
                    okCertError = true;
                else if (l.contains(QLatin1String("http")) && (l.contains(QLatin1String(" 40")) || l.contains(QLatin1String(" 50"))
                                                               || l.contains(QLatin1String("invalid")) || l.contains(QLatin1String("bdecode"))))
                    okVerified = true;   // TLS handshake + certificate check passed, then an HTTP-level error
            }
            if (l.contains(QLatin1String("badssl")) && cert)
                badRejected = true;
        }
        return (okVerified || okCertError) && badRejected;
    };
    waitUntil(evaluate, 60000);
    evaluate();
    for (const QString &m : std::as_const(g_trackerLog))
        if (!m.contains(QLatin1String("skipping tracker announce")))
            out(QStringLiteral("tracker: ") + m);
    in.be->removeTorrent(ok, true);
    in.be->removeTorrent(bad, true);
    in.plugin->stop();
    okVerified = okVerified && !okCertError;
    out(QStringLiteral("RESULT https: valid-cert host TLS ok=%1, self-signed rejected=%2").arg(okVerified).arg(badRejected));
    return okVerified && badRejected ? 0 : 1;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    g_clock.start();
    mx::registerMetaTypes();
    QCommandLineParser p;
    p.addHelpOption();
    p.addOption({QStringLiteral("profile"), QStringLiteral("profile directory"), QStringLiteral("dir")});
    p.addOption({QStringLiteral("magnet"), QStringLiteral("magnet link"), QStringLiteral("uri"),
                 QStringLiteral("magnet:?xt=urn:btih:7acf8fb590b2060dd9c3146ef770169d593433b0&dn=debian-13.7.0-amd64-netinst.iso"
                                "&tr=http%3A%2F%2Fbttracker.debian.org%3A6969%2Fannounce")});
    p.addOption({QStringLiteral("target-mb"), QStringLiteral("MB to download before pausing"), QStringLiteral("n"), QStringLiteral("50")});
    p.addOption({QStringLiteral("cleanup"), QStringLiteral("remove the torrent and its files at the end")});
    p.addOption({QStringLiteral("https-check"), QStringLiteral("verify HTTPS tracker certificate handling")});
    p.addOption({QStringLiteral("verbose"), QStringLiteral("show mx.bt debug output")});
    p.process(app);
    g_verbose = p.isSet(QStringLiteral("verbose"));
    QLoggingCategory::setFilterRules(QStringLiteral("mx.bt.debug=true"));
    g_prev = qInstallMessageHandler(handler);
    const QString profile = QDir(p.value(QStringLiteral("profile")).isEmpty() ? QStringLiteral("bt-probe-profile") : p.value(QStringLiteral("profile"))).absolutePath();
    QDir().mkpath(profile);
    out(QStringLiteral("profile %1").arg(profile));
    if (p.isSet(QStringLiteral("https-check")))
        return httpsCheck(profile);

    const qint64 target = p.value(QStringLiteral("target-mb")).toLongLong() * 1048576;
    const QString magnet = p.value(QStringLiteral("magnet"));
    QString id;
    qint64 doneAtQuit = 0;
    {
        Instance in;
        in.create(profile);
        out(QStringLiteral("restored torrents: %1").arg(in.be->torrents().size()));
        in.plugin->start();
        // 1. DHT bootstrap
        waitUntil([&] { return in.plugin->status().peers >= 50; }, 60000, [&](int s) {
            if (s % 3 == 0)
                out(QStringLiteral("DHT nodes: %1 | %2").arg(in.plugin->status().peers).arg(in.statusLine()));
        });
        out(QStringLiteral("DHT bootstrap: %1 nodes | %2").arg(in.plugin->status().peers).arg(in.statusLine()));
        // 2. magnet → metadata
        QString err;
        id = in.be->addTorrent(magnet, {}, profile + QStringLiteral("/downloads"), false, {}, &err);
        if (id.isEmpty()) {
            out(QStringLiteral("addTorrent failed: ") + err);
            return 2;
        }
        out(QStringLiteral("added %1").arg(id));
        if (!waitUntil([&] { return in.be->snapshot(id).hasMetadata; }, 180000, [&](int s) {
                if (s % 5 == 0)
                    out(in.torrentLine(id));
            })) {
            out(QStringLiteral("FAILED: no metadata within 180 s | %1").arg(in.statusLine()));
            return 3;
        }
        const mx::TorrentSnapshot m = in.be->snapshot(id);
        out(QStringLiteral("metadata: '%1' %2, %3 pieces of %4 KiB, %5 files; pieceHash(0)=%6")
                .arg(m.name, mb(m.totalSize)).arg(m.numPieces).arg(m.pieceLength / 1024).arg(m.files.size())
                .arg(QString::fromLatin1(in.be->pieceHash(id, 0).toHex())));
        // 3. download ≥ target
        if (!waitUntil([&] { return in.be->snapshot(id).totalDone >= target || in.be->snapshot(id).finished; }, 900000, [&](int s) {
                if (s % 5 == 0)
                    out(in.torrentLine(id) + QStringLiteral(" | DHT %1").arg(in.plugin->status().peers));
            })) {
            out(QStringLiteral("FAILED: target not reached | %1").arg(in.torrentLine(id)));
            return 4;
        }
        out(QStringLiteral("target reached: %1 | verified pieces signalled: %2, hash failures: %3").arg(in.torrentLine(id)).arg(in.pieces).arg(in.failed));
        // 4. pause
        in.be->pauseTorrent(id);
        waitUntil([] { return false; }, 4000);
        const qint64 p0 = in.be->snapshot(id).totalDone;
        waitUntil([] { return false; }, 5000);
        const mx::TorrentSnapshot ps = in.be->snapshot(id);
        out(QStringLiteral("paused: done %1 → %2 over 5 s, rate %3, paused=%4, state=%5")
                .arg(mb(p0), mb(ps.totalDone), kbs(ps.rateDownBps)).arg(ps.paused).arg(int(ps.state)));
        // 5. resume
        in.be->resumeTorrent(id);
        waitUntil([&] { return in.be->snapshot(id).totalDone > ps.totalDone + 2 * 1048576 || in.be->snapshot(id).finished; }, 120000,
                  [&](int s) {
                      if (s % 5 == 0)
                          out(in.torrentLine(id));
                  });
        out(QStringLiteral("resumed: %1").arg(in.torrentLine(id)));
        // 6. quit
        QElapsedTimer el;
        el.start();
        in.plugin->stop();
        doneAtQuit = in.be->snapshot(id).totalDone;
        in.plugin.reset();
        out(QStringLiteral("quit: stop + destroy took %1 ms, %2 verified at quit").arg(el.elapsed()).arg(mb(doneAtQuit)));
    }
    {
        // 7. restart on the same profile
        Instance in;
        QElapsedTimer el;
        el.start();
        in.create(profile);
        const mx::TorrentSnapshot r = in.be->snapshot(id);
        out(QStringLiteral("restart: init %1 ms, %2 torrents, '%3' restored %4 (%5 pieces), paused=%6, metadata=%7")
                .arg(el.elapsed()).arg(in.be->torrents().size()).arg(r.name, mb(r.totalDone)).arg(r.have.count(true)).arg(r.paused).arg(r.hasMetadata));
        in.plugin->start();
        const qint64 base = in.be->snapshot(id).totalDone;
        const bool moved = waitUntil([&] { return in.be->snapshot(id).totalDone > base + 4 * 1048576 || in.be->snapshot(id).finished; }, 180000,
                                     [&](int s) {
                                         if (s % 5 == 0)
                                             out(in.torrentLine(id) + QStringLiteral(" | DHT %1").arg(in.plugin->status().peers));
                                     });
        out(QStringLiteral("after restart: %1 | %2").arg(in.torrentLine(id), in.statusLine()));
        out(QStringLiteral("pieces verified after restart: %1 (hash failures %2); restored bytes kept: %3")
                .arg(in.pieces).arg(in.failed).arg(r.totalDone >= doneAtQuit - 4 * 1048576));
        if (p.isSet(QStringLiteral("cleanup"))) {
            in.be->removeTorrent(id, true);
            waitUntil([] { return false; }, 1500);
        }
        in.plugin->stop();
        out(moved ? QStringLiteral("RESULT: OK") : QStringLiteral("RESULT: no progress after restart"));
        return moved ? 0 : 5;
    }
}
