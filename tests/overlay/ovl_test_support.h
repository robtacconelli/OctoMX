// SPDX-License-Identifier: GPL-3.0-or-later
// Shared helpers for the overlay tests: a file-backed LibraryAccess, and an overlay instance with
// its own profile dir, settings and short timers, connected to the others over 127.0.0.1.
#pragma once

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMutex>
#include <QtCore/QRandomGenerator>
#include <QtCore/QUuid>
#include <QtTest/QtTest>

#include <memory>

#include "mx/plugin.h"
#include "mx/settings_keys.h"
#include "mx/simple_settings.h"
#include "overlay_plugin.h"
#include "overlay_plugin_impl.h"
#include "ovl_crypto.h"
#include "ovl_util.h"
#include "rats_node.h"

#ifndef MX_TEST_TMP
#define MX_TEST_TMP "test-tmp"
#endif

namespace ovltest {

inline QString freshDir(const QString &tag)
{
    const QString d = QStringLiteral(MX_TEST_TMP "/%1-%2").arg(tag, QUuid::createUuid().toString(QUuid::Id128).left(12));
    QDir(d).removeRecursively();
    QDir().mkpath(d);
    return d;
}

class TestLibrary : public mx::LibraryAccess {
public:
    QList<mx::SharedFile> files;
    QList<mx::SharedFile> sharedFiles() const override { return files; }
    bool findByHash(const mx::FileHashes &h, mx::SharedFile *out) const override
    {
        for (const auto &f : files)
            if (f.hashes.matches(h)) { *out = f; return true; }
        return false;
    }
    QByteArray read(const mx::FileHashes &h, qint64 offset, qint64 length) override
    {
        mx::SharedFile f;
        if (!findByHash(h, &f)) return {};
        {   // [up-1] a file still downloading: only its verified ranges can be read
            QMutexLocker l(&partialMutex);
            if (partial.contains(f.hashes.sha1)) {
                bool inside = false;
                for (const auto &r : partial.value(f.hashes.sha1)) inside = inside || (r.first <= offset && offset + length <= r.second);
                if (!inside) return {};
            }
        }
        QFile file(f.path);
        if (!file.open(QIODevice::ReadOnly) || !file.seek(offset)) return {};
        ++reads;
        return file.read(length);
    }
    // [engine-3] per-file eD2k part hash sets (keyed by SHA-1); empty = like an old library
    QHash<QByteArray, QList<QByteArray>> partSets;
    QList<QByteArray> ed2kPartHashes(const mx::FileHashes &h) const override
    {
        mx::SharedFile f;
        return findByHash(h, &f) ? partSets.value(f.hashes.sha1) : QList<QByteArray>();
    }
    std::atomic<int> reads{0};
    // [up-1] files that are still downloading (keyed by SHA-1): the verified ranges; they are left out
    // of sharedFiles(), like the core's Library does with running downloads
    mutable QMutex partialMutex;
    QHash<QByteArray, QList<QPair<qint64, qint64>>> partial;
    void setPartial(const QByteArray &sha1, const QList<QPair<qint64, qint64>> &have)
    {
        QMutexLocker l(&partialMutex);
        partial.insert(sha1, have);
    }
    void setComplete(const QByteArray &sha1)
    {
        QMutexLocker l(&partialMutex);
        partial.remove(sha1);
    }
    bool partialRanges(const mx::FileHashes &h, QList<QPair<qint64, qint64>> *have) const override
    {
        mx::SharedFile f;
        if (!findByHash(h, &f)) return false;
        QMutexLocker l(&partialMutex);
        if (!partial.contains(f.hashes.sha1)) return false;
        if (have) *have = partial.value(f.hashes.sha1);
        return true;
    }
    QList<mx::SharedFile> completeFiles() const
    {
        QMutexLocker l(&partialMutex);
        QList<mx::SharedFile> out;
        for (const auto &f : files)
            if (!partial.contains(f.hashes.sha1)) out << f;
        return out;
    }

    // Create a file of `size` pseudo-random bytes (seeded) and add it with sha1 + sha256 + md5.
    mx::SharedFile addGenerated(const QString &dir, const QString &name, qint64 size, quint32 seed)
    {
        const QString path = dir + QLatin1Char('/') + name;
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly)) return {};
        QRandomGenerator rng(seed);
        QCryptographicHash s1(QCryptographicHash::Sha1), s256(QCryptographicHash::Sha256), m5(QCryptographicHash::Md5);
        QByteArray chunk(1 << 20, Qt::Uninitialized);
        for (qint64 done = 0; done < size;) {
            const int n = static_cast<int>(qMin<qint64>(chunk.size(), size - done));
            rng.fillRange(reinterpret_cast<quint32 *>(chunk.data()), chunk.size() / 4);
            f.write(chunk.constData(), n);
            s1.addData(QByteArrayView(chunk.constData(), n));
            s256.addData(QByteArrayView(chunk.constData(), n));
            m5.addData(QByteArrayView(chunk.constData(), n));
            done += n;
        }
        f.close();
        mx::SharedFile sf;
        sf.path = path;
        sf.name = name;
        sf.size = size;
        sf.type = mx::mediaTypeForFileName(name);
        sf.hashes.sha1 = s1.result();
        sf.hashes.sha256 = s256.result();
        sf.hashes.md5 = m5.result();
        sf.hashes.ed2k = QCryptographicHash::hash(sf.hashes.sha1, QCryptographicHash::Md4);   // stand-in
        sf.hashed = true;
        files << sf;
        return sf;
    }
};

struct Instance {
    QString profile;
    mx::SimpleSettings settings;
    TestLibrary library;
    mx::ovl::OverlayPlugin *plugin = nullptr;
    mx::ChatBackend *chat = nullptr;
    mx::ChatEvents *events = nullptr;

    explicit Instance(const QString &name, const QString &profileDir = QString(), int port = 0)
    {
        ownProfile = profileDir.isEmpty();
        profile = ownProfile ? freshDir(name) : profileDir;
        settings.setValue(QString(mx::keys::identity::ScreenName), name);
        mx::PluginContext ctx;
        ctx.settings = &settings;
        ctx.library = &library;
        ctx.profileDir = profile;
        ctx.dataDir = profile + QStringLiteral("/overlay");
        ctx.screenName = name;
        ctx.clientName = QStringLiteral("OctoMX");
        ctx.clientVersion = QStringLiteral("4.0-test");
        plugin = static_cast<mx::ovl::OverlayPlugin *>(mx::createOverlayPlugin());
        mx::ovl::OverlayPlugin::TestOptions o;
        o.dht = false;
        o.mdns = false;
        o.reconnect = false;
        o.listenPort = port;
        plugin->setTestOptions(o);
        mx::ovl::OverlayTimings t;
        t.presenceMs = 3000;
        t.presenceTimeoutMs = 8000;
        t.heartbeatMs = 1000;
        t.memberTimeoutMs = 3500;
        t.directoryMs = 1000;
        t.historyDelayMs = 1200;
        t.searchQuietMs = 2500;
        t.searchMaxMs = 8000;
        t.sourcesRepeatMs = 3000;
        t.dialWaitMs = 2500;
        plugin->setTimings(t);
        plugin->init(ctx);
        chat = dynamic_cast<mx::ChatBackend *>(plugin);
        events = qobject_cast<mx::ChatEvents *>(chat->chatEvents());
    }
    ~Instance()
    {
        if (plugin) {
            plugin->stop();
            delete plugin;
        }
        mx::ovl::RatsNode::waitAllStopped(5000);
        if (ownProfile) QDir(profile).removeRecursively();   // generated files can be 200 MB
    }
    bool ownProfile = false;
    void start()
    {
        plugin->start();
        QVERIFY(plugin->listenPort() != 0);
    }
    QString addr() const { return QStringLiteral("127.0.0.1:%1").arg(plugin->listenPort()); }
    mx::UserRef me() const { return plugin->localUser(); }
    void setShared() { plugin->setSharedFiles(library.files); }
};

// Dial b -> a and wait until both sides verified each other's hello.
inline bool link(Instance &a, Instance &b, int timeoutMs = 10000)
{
    const int na = a.plugin->connectedUsers(), nb = b.plugin->connectedUsers();
    b.plugin->connectPeer(a.addr());
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        if (a.plugin->connectedUsers() > na && b.plugin->connectedUsers() > nb) return true;
        QTest::qWait(20);
    }
    return false;
}

// A hand-driven peer that speaks the overlay wire protocol (for hostile-input tests): it sends a
// valid signed hello and then whatever control messages the test wants.
struct FakePeer {
    mx::ovl::RatsNode node;
    mx::ovl::Identity id;
    QString name = QStringLiteral("Fake_00000");
    QList<QJsonObject> received;
    bool answerPings = true;
    bool start(const QString &dir)
    {
        if (!id.loadOrCreate(dir + QStringLiteral("/id"))) return false;
        mx::ovl::RatsNodeConfig c;
        c.protocol = QStringLiteral("winmx-ng/1");
        c.dht = c.mdns = c.reconnect = c.pex = false;
        c.json = false;
        c.pubsub = true;
        c.channels = {QStringLiteral("mx"), QStringLiteral("mxd")};
        c.dataDir = dir;
        QObject::connect(&node, &mx::ovl::RatsNode::bytesReceived, &node, [this](const QString &from, const QString &ch, const QByteArray &b) {
            QJsonObject o;
            if (ch == QLatin1String("mx") && mx::ovl::decodeControl(b, &o)) {
                received << o;
                // [ovl-2] every node since the first version answers "ping" with "pong"
                if (answerPings && o.value(QStringLiteral("t")).toString() == QLatin1String("ping"))
                    send(from, QJsonObject{{QStringLiteral("t"), QStringLiteral("pong")}, {QStringLiteral("v"), 1},
                                           {QStringLiteral("ts"), o.value(QStringLiteral("ts"))}});
            }
        });
        return node.start(c);
    }
    QString userId() const { return id.userId(); }
    void hello(const QString &to)
    {
        const qint64 ts = QDateTime::currentMSecsSinceEpoch();
        const QString x = QString::fromLatin1(id.boxPublicKey().toHex());
        QJsonObject h;
        h[QStringLiteral("t")] = QStringLiteral("hello");
        h[QStringLiteral("v")] = 1;
        h[QStringLiteral("user")] = id.userId();
        h[QStringLiteral("node")] = node.nodeId();
        h[QStringLiteral("x")] = x;
        h[QStringLiteral("name")] = name;
        h[QStringLiteral("ts")] = double(ts);
        h[QStringLiteral("sig")] = QString::fromLatin1(
            id.sign(QStringLiteral("hello\n%1\n%2\n%3\n%4").arg(node.nodeId(), x, name).arg(ts).toUtf8()).toHex());
        send(to, h);
    }
    void send(const QString &to, const QJsonObject &o) { node.sendBytes(to, QStringLiteral("mx"), mx::ovl::encodeControl(o)); }
    void sendRaw(const QString &to, const QString &ch, const QByteArray &b) { node.sendBytes(to, ch, b); }
};

inline bool waitFor(const std::function<bool()> &cond, int timeoutMs = 10000)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        if (cond()) return true;
        QTest::qWait(25);
    }
    return cond();
}

} // namespace ovltest
