// SPDX-License-Identifier: GPL-3.0-or-later
// overlay_probe: manual / two-terminal tests of the OctoMX overlay plugin.
//
//   overlay_probe [options] chat [room]          chat REPL (see /help)
//   overlay_probe [options] search <words...>    overlay keyword search, print hits, exit
//   overlay_probe [options] browse <user>        browse a user's files, exit
//   overlay_probe [options] fetch <user> <substring> <outfile>   browse, pick a file, download it
//   overlay_probe [options] idle                 stay online, print peers/users (discovery tests)
//
// options: --profile DIR (default ./probe-profile)  --name NAME  --port P (0 = random)
//          --connect host:port (repeatable)  --no-dht  --no-mdns  --share DIR  --seconds N
// <user> is a screen name (prefix match, e.g. "Bob" or "Bob_12345") or a 64-hex user id; the
// probe waits up to 60 s for that user to show up (presence) before giving up.
// Output lines start with "+<seconds>".
#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QMutex>
#include <QtCore/QTimer>

#include <cstdio>
#include <iostream>
#include <thread>

#include "mx/plugin.h"
#include "mx/settings_keys.h"
#include "mx/simple_settings.h"
#include "overlay_plugin.h"
#include "overlay_plugin_impl.h"

namespace {
QElapsedTimer g_t;
void out(const QString &s)
{
    std::printf("+%7.2fs %s\n", g_t.elapsed() / 1000.0, s.toLocal8Bit().constData());
    std::fflush(stdout);
}

class DirLibrary : public mx::LibraryAccess {
public:
    QList<mx::SharedFile> files;
    void scan(const QString &dir)
    {
        QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext() && files.size() < 5000) {
            const QString p = it.next();
            QFile f(p);
            if (!f.open(QIODevice::ReadOnly)) continue;
            QCryptographicHash s1(QCryptographicHash::Sha1), s2(QCryptographicHash::Sha256);
            while (!f.atEnd()) {
                const QByteArray b = f.read(1 << 20);
                s1.addData(b);
                s2.addData(b);
            }
            mx::SharedFile sf;
            sf.path = p;
            sf.name = QFileInfo(p).fileName();
            sf.size = f.size();
            sf.type = mx::mediaTypeForFileName(sf.name);
            sf.hashes.sha1 = s1.result();
            sf.hashes.sha256 = s2.result();
            sf.hashed = true;
            files << sf;
        }
    }
    QList<mx::SharedFile> sharedFiles() const override { return files; }
    bool findByHash(const mx::FileHashes &h, mx::SharedFile *o) const override
    {
        for (const auto &f : files)
            if (f.hashes.matches(h)) { *o = f; return true; }
        return false;
    }
    QByteArray read(const mx::FileHashes &h, qint64 off, qint64 len) override
    {
        mx::SharedFile f;
        if (!findByHash(h, &f)) return {};
        QFile file(f.path);
        if (!file.open(QIODevice::ReadOnly) || !file.seek(off)) return {};
        return file.read(len);
    }
    QList<QByteArray> ed2kPartHashes(const mx::FileHashes &) const override { return {}; }
};
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    mx::registerMetaTypes();
    g_t.start();
    QString profile = QDir::currentPath() + QStringLiteral("/probe-profile"), name = QStringLiteral("Probe"), share;
    QStringList connects, args;
    int port = 0, seconds = 0;
    bool dht = true, mdns = true;
    const QStringList a = app.arguments().mid(1);
    for (int i = 0; i < a.size(); ++i) {
        auto next = [&] { return i + 1 < a.size() ? a[++i] : QString(); };
        if (a[i] == QLatin1String("--profile")) profile = next();
        else if (a[i] == QLatin1String("--name")) name = next();
        else if (a[i] == QLatin1String("--port")) port = next().toInt();
        else if (a[i] == QLatin1String("--connect")) connects << next();
        else if (a[i] == QLatin1String("--no-dht")) dht = false;
        else if (a[i] == QLatin1String("--no-mdns")) mdns = false;
        else if (a[i] == QLatin1String("--share")) share = next();
        else if (a[i] == QLatin1String("--seconds")) seconds = next().toInt();
        else args << a[i];
    }
    const QString mode = args.isEmpty() ? QStringLiteral("chat") : args.takeFirst();

    mx::SimpleSettings settings;
    settings.setValue(QString(mx::keys::identity::ScreenName), name);
    settings.setValue(QString(mx::keys::connection::Upnp), false);   // probes never touch the router
    DirLibrary lib;
    if (!share.isEmpty()) lib.scan(share);
    mx::PluginContext ctx;
    ctx.settings = &settings;
    ctx.library = &lib;
    ctx.profileDir = profile;
    ctx.dataDir = profile + QStringLiteral("/overlay");
    ctx.screenName = name;
    ctx.clientName = QStringLiteral("OctoMX");
    ctx.clientVersion = QStringLiteral(MX_VERSION_STRING " probe");
    auto *plugin = static_cast<mx::ovl::OverlayPlugin *>(mx::createOverlayPlugin(&app));
    mx::ovl::OverlayPlugin::TestOptions o;
    o.dht = dht;
    o.mdns = mdns;
    o.listenPort = port;
    plugin->setTestOptions(o);
    plugin->init(ctx);
    plugin->setSharedFiles(lib.files);
    auto *chat = dynamic_cast<mx::ChatBackend *>(plugin);
    auto *ev = qobject_cast<mx::ChatEvents *>(chat->chatEvents());

    QObject::connect(plugin, &mx::NetworkPlugin::statusChanged, [](const mx::NetworkStatus &st) {
        out(QStringLiteral("STATUS %1 peers=%2 users=%3 port=%4%5").arg(st.statusText).arg(st.peers).arg(st.usersOnline)
                .arg(st.listenPort).arg(st.firewalled ? QStringLiteral(" firewalled") : QString()));
    });
    QObject::connect(plugin, &mx::NetworkPlugin::serversChanged, [plugin] {
        static int last = -1;
        const auto s = plugin->servers();
        if (s.size() == last) return;
        last = s.size();
        for (const auto &e : s)
            if (e.state == mx::ConnState::Connected) out(QStringLiteral("PEER %1 %2 %3").arg(e.address, e.name, e.description));
    });
    QObject::connect(ev, &mx::ChatEvents::userPresence, [](const mx::UserRef &u, bool on, int files, const QString &conn) {
        out(QStringLiteral("USER %1 %2 (%3 files, %4) id=%5").arg(u.name, on ? QStringLiteral("online") : QStringLiteral("offline"))
                .arg(files).arg(conn, u.id.left(12)));
    });
    QObject::connect(ev, &mx::ChatEvents::messageReceived, [](const mx::ChatMessage &m) {
        const QString where = m.roomId.isEmpty() ? QStringLiteral("PM") : QStringLiteral("#") + m.roomId;
        switch (m.kind) {
        case mx::ChatKind::Action: out(QStringLiteral("%1 * %2 %3").arg(where, m.from.name, m.text)); break;
        case mx::ChatKind::Message: out(QStringLiteral("%1 <%2> %3").arg(where, m.from.name, m.text)); break;
        case mx::ChatKind::Private: out(QStringLiteral("PM from %1: %2").arg(m.from.name, m.text)); break;
        default: out(QStringLiteral("%1 -- %2").arg(where, m.text)); break;
        }
    });
    QObject::connect(ev, &mx::ChatEvents::roomUpdated, [](const mx::ChatRoomInfo &r) {
        out(QStringLiteral("ROOM %1 topic='%2' users=%3").arg(r.name, r.topic).arg(r.users));
    });
    QObject::connect(plugin, &mx::NetworkPlugin::whoisResult, [](const mx::UserRef &u, const QVariantMap &m) {
        QStringList kv;
        for (auto it = m.cbegin(); it != m.cend(); ++it) kv << it.key() + QLatin1Char('=') + it.value().toString();
        out(QStringLiteral("WHOIS %1: %2").arg(u.name, kv.join(QStringLiteral(", "))));
    });

    plugin->start();
    out(QStringLiteral("I am %1 (id %2), node %3, port %4, sharing %5 files").arg(plugin->localUser().name,
        plugin->localUser().id.left(16), plugin->nodeId().left(8)).arg(plugin->listenPort()).arg(lib.files.size()));
    for (const QString &c : connects) plugin->connectPeer(c);
    if (seconds > 0) QTimer::singleShot(seconds * 1000, &app, [&] { out(QStringLiteral("time limit")); app.exit(0); });

    QHash<QString, mx::UserRef> online;
    QObject::connect(ev, &mx::ChatEvents::userPresence, [&](const mx::UserRef &u, bool on, int, const QString &) {
        if (on) online.insert(u.id, u);
        else online.remove(u.id);
    });
    auto lookup = [&](const QString &who) -> mx::UserRef {
        for (const auto &u : online)
            if (u.id == who.toLower() || u.name.compare(who, Qt::CaseInsensitive) == 0 ||
                u.name.startsWith(who + QLatin1Char('_'), Qt::CaseInsensitive))
                return u;
        return mx::UserRef();
    };
    auto whenUser = [&](const QString &who, std::function<void(const mx::UserRef &)> fn) {
        auto *poll = new QTimer(&app);
        auto *waited = new QElapsedTimer;
        waited->start();
        QObject::connect(poll, &QTimer::timeout, [=, &app] {
            const mx::UserRef u = lookup(who);
            if (u.isValid()) {
                poll->stop();
                poll->deleteLater();
                delete waited;
                fn(u);
            } else if (waited->elapsed() > 60000) {
                out(QStringLiteral("user '%1' not found").arg(who));
                app.exit(3);
            }
        });
        poll->start(250);
    };

    if (mode == QLatin1String("idle")) {
        // nothing: print events
    } else if (mode == QLatin1String("search")) {
        const QString text = args.join(QLatin1Char(' '));
        QTimer::singleShot(3000, &app, [&, text] {
            mx::SearchQuery q;
            q.id = 1;
            q.text = text;
            out(QStringLiteral("SEARCH '%1'").arg(text));
            plugin->search(q);
        });
        QObject::connect(plugin, &mx::NetworkPlugin::searchHits, [](quint32, const QList<mx::SearchHit> &hits) {
            for (const auto &h : hits)
                out(QStringLiteral("HIT %1 %2 bytes from %3 (%4, %5/%6 slots) sha1=%7").arg(h.fileName).arg(h.size)
                        .arg(h.sources[0].user.name, h.sources[0].connection).arg(h.sources[0].freeSlots).arg(h.sources[0].totalSlots)
                        .arg(QString::fromLatin1(h.hashes.sha1.toHex())));
        });
        QObject::connect(plugin, &mx::NetworkPlugin::searchDone, [&](quint32) { out(QStringLiteral("DONE")); app.exit(0); });
    } else if (mode == QLatin1String("browse") || mode == QLatin1String("fetch")) {
        const QString who = args.value(0), pick = args.value(1), outFile = args.value(2);
        auto *files = new QList<mx::BrowseEntry>;
        QObject::connect(plugin, &mx::NetworkPlugin::browseFailed, [&](quint32, const mx::UserRef &, const QString &why) {
            out(QStringLiteral("BROWSE FAILED: %1").arg(why));
            app.exit(4);
        });
        QObject::connect(plugin, &mx::NetworkPlugin::browseResults,
                         [&, files, mode, pick, outFile](quint32, const mx::UserRef &u, const QList<mx::BrowseEntry> &l, bool fin) {
            *files += l;
            for (const auto &e : l)
                if (mode == QLatin1String("browse")) out(QStringLiteral("FILE %1 %2").arg(e.path).arg(e.size));
            if (!fin) return;
            out(QStringLiteral("BROWSE %1: %2 files").arg(u.name).arg(files->size()));
            if (mode == QLatin1String("browse")) return app.exit(0);
            const mx::BrowseEntry *e = nullptr;
            for (const auto &f : *files)
                if (f.path.contains(pick, Qt::CaseInsensitive)) { e = &f; break; }
            if (!e) { out(QStringLiteral("no file matching '%1'").arg(pick)); return app.exit(5); }
            mx::SearchSource src;
            src.net = mx::Net::Overlay;
            src.user = u;
            auto *rs = plugin->openSource(QStringLiteral("probe"), e->hashes, e->size, src);
            auto *f = new QFile(outFile);
            if (!rs || !f->open(QIODevice::WriteOnly)) { out(QStringLiteral("cannot fetch")); return app.exit(6); }
            const qint64 size = e->size;
            const QByteArray want = e->hashes.sha1;
            auto *next = new qint64(0);
            auto *done = new qint64(0);
            auto *hash = new QCryptographicHash(QCryptographicHash::Sha1);
            auto *t = new QElapsedTimer;
            t->start();
            auto pump = [=] {
                while ((rs->state() == mx::RangeSource::Ready || rs->state() == mx::RangeSource::Busy) && *next < size &&
                       *next - *done < 2 * rs->preferredRequestSize()) {
                    const qint64 len = qMin(rs->preferredRequestSize(), size - *next);
                    const qint64 off = *next;
                    *next += len;
                    rs->request(off, len);
                }
            };
            QObject::connect(rs, &mx::RangeSource::stateChanged, [=] { out(QStringLiteral("SOURCE %1").arg(rs->stateText())); pump(); });
            QObject::connect(rs, &mx::RangeSource::data, [=](qint64, const QByteArray &b) { f->write(b); hash->addData(b); });
            QObject::connect(rs, &mx::RangeSource::requestFinished, [=, &app](qint64, qint64 len, bool ok, const QString &err) {
                if (!ok) { out(QStringLiteral("request failed: %1").arg(err)); return app.exit(7); }
                *done += len;
                if (*done == size) {
                    f->close();
                    const bool good = want.isEmpty() || hash->result() == want;
                    out(QStringLiteral("FETCHED %1 bytes in %2 ms (%3 MB/s), sha1 %4").arg(size).arg(t->elapsed())
                            .arg(size / 1048576.0 / qMax<qint64>(1, t->elapsed()) * 1000.0, 0, 'f', 1)
                            .arg(good ? QStringLiteral("OK") : QStringLiteral("MISMATCH")));
                    rs->close();
                    return app.exit(good ? 0 : 8);
                }
                pump();
            });
            rs->open();
        });
        whenUser(who, [plugin](const mx::UserRef &u) {
            out(QStringLiteral("browsing %1").arg(u.name));
            plugin->browse(1, u);
        });
    } else {   // chat REPL
        const QString room = args.isEmpty() ? QStringLiteral("OctoMX Lobby") : args.join(QLatin1Char(' '));
        chat->joinRoom(room);
        const QString rid = mx::ovl::normaliseRoomName(room);
        out(QStringLiteral("joined '%1'. Type text, /me <action>, /msg <user> <text>, /users, /rooms, /topic <t>, /whois <user>, /quit").arg(room));
        static std::function<void(const QString &)> onLine;
        std::thread([&app]() {
            std::string line;
            while (std::getline(std::cin, line)) {
                const QString l = QString::fromLocal8Bit(line.c_str());
                QMetaObject::invokeMethod(&app, [l] { if (onLine) onLine(l); });
            }
            QMetaObject::invokeMethod(&app, [] { QCoreApplication::exit(0); });
        }).detach();
        onLine = [&, rid](const QString &l) {
            if (l == QLatin1String("/quit")) return app.exit(0);
            if (l == QLatin1String("/users")) {
                for (const auto &u : chat->roomUsers(rid)) out(QStringLiteral("  %1 (%2 files, %3)").arg(u.user.name).arg(u.files).arg(u.connection));
                return;
            }
            if (l == QLatin1String("/rooms")) {
                for (const auto &r : chat->roomList()) out(QStringLiteral("  %1 [%2] %3").arg(r.name).arg(r.users).arg(r.topic));
                return;
            }
            if (l.startsWith(QLatin1String("/me "))) return chat->sendRoomMessage(rid, l.mid(4), mx::ChatKind::Action);
            if (l.startsWith(QLatin1String("/topic "))) return chat->setTopic(rid, l.mid(7));
            if (l.startsWith(QLatin1String("/whois "))) {
                const mx::UserRef u = lookup(l.mid(7).trimmed());
                if (u.isValid()) plugin->whois(u); else out(QStringLiteral("unknown user"));
                return;
            }
            if (l.startsWith(QLatin1String("/msg "))) {
                const QString rest = l.mid(5);
                const int sp = rest.indexOf(QLatin1Char(' '));
                const mx::UserRef u = lookup(rest.left(sp));
                if (!u.isValid() || sp < 0) return out(QStringLiteral("unknown user"));
                return chat->sendPrivateMessage(u, rest.mid(sp + 1));
            }
            if (!l.trimmed().isEmpty()) chat->sendRoomMessage(rid, l, mx::ChatKind::Message);
        };
    }
    const int rc = app.exec();
    plugin->stop();
    delete plugin;
    mx::ovl::RatsNode::waitAllStopped(5000);
    return rc;
}
