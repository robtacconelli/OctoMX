// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX: helpers for the BitTorrent plugin tests (offline: 127.0.0.1, DHT/LSD/UPnP off).
#pragma once

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QHash>
#include <QtCore/QRandomGenerator>
#include <QtCore/QSet>
#include <QtTest/QTest>

#include <memory>

#include <libtorrent/bencode.hpp>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/torrent_info.hpp>

#include "bt_plugin.h"
#include "mx/settings_keys.h"
#include "mx/simple_settings.h"

namespace bttest {

// All test data lives next to the test binary (never in $HOME or /tmp).
inline QString dataRoot()
{
    QString d = qEnvironmentVariable("MX_BT_TEST_DIR");
    if (d.isEmpty())
        d = QCoreApplication::applicationDirPath() + QStringLiteral("/bt-test-data");
    QDir().mkpath(d);
    return d;
}

inline QString freshDir(const QString &name)
{
    const QString d = dataRoot() + QLatin1Char('/') + name;
    QDir(d).removeRecursively();
    QDir().mkpath(d);
    return d;
}

// Deterministic pseudo-random file.
inline void makeFile(const QString &path, qint64 size, quint32 seed)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY2(f.open(QIODevice::WriteOnly), qPrintable(path));
    QRandomGenerator g(seed);
    QByteArray buf(1 << 20, Qt::Uninitialized);
    qint64 left = size;
    while (left > 0) {
        const int n = int(qMin<qint64>(left, buf.size()));
        g.fillRange(reinterpret_cast<quint32 *>(buf.data()), n / 4 + 0);
        for (int i = (n / 4) * 4; i < n; ++i)
            buf[i] = char(g.generate());
        f.write(buf.constData(), n);
        left -= n;
    }
}

inline QByteArray sha1File(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QByteArray();
    QCryptographicHash h(QCryptographicHash::Sha1);
    h.addData(&f);
    return h.result().toHex();
}

inline QByteArray readRange(const QString &path, qint64 offset, qint64 len)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || !f.seek(offset))
        return QByteArray();
    return f.read(len);
}

// .torrent (v1) for a file or directory, created with libtorrent directly.
inline QByteArray makeTorrent(const QString &root, int pieceSize)
{
    lt::file_storage fs;
    lt::add_files(fs, QDir::toNativeSeparators(root).toStdString());
    lt::create_torrent ct(fs, pieceSize, lt::create_torrent::v1_only);
    lt::set_piece_hashes(ct, QDir::toNativeSeparators(QFileInfo(root).absolutePath()).toStdString());
    std::vector<char> out;
    lt::bencode(std::back_inserter(out), ct.generate());
    return QByteArray(out.data(), int(out.size()));
}

inline QString infoHashOf(const QByteArray &torrent)
{
    lt::error_code ec;
    lt::torrent_info ti(lt::span<char const>(torrent.constData(), torrent.size()), ec, lt::from_span);
    if (ec)
        return QString();
    return QString::fromLatin1(QByteArray(ti.info_hashes().v1.data(), 20).toHex());
}

// One plugin instance with its own profile.
class Node {
public:
    explicit Node(const QString &profile, bool limitLan = false) : profile(profile), m_limitLan(limitLan) {}
    mx::RateLimiter *limiter = nullptr;   // [bw-1] set before create()
    ~Node() { destroy(); }

    void create()
    {
        namespace k = mx::keys;
        settings.setValue(QString(k::bt::Dht), false);
        settings.setValue(QString(k::bt::Lsd), false);
        settings.setValue(QString(k::connection::Upnp), false);
        settings.setValue(QString(k::connection::NatPmp), false);
        settings.setValue(QString(k::connection::Ipv6), false);
        settings.setValue(QString(k::connection::BindInterface), QStringLiteral("127.0.0.1"));
        settings.setValue(QString(k::bt::ListenPort), 0);
        settings.setValue(QString(k::bt::ExtraTrackers), QStringList());
        settings.setValue(QString(k::bt::SeedRatioLimit), 0.0);
        settings.setValue(QString(k::bandwidth::LimitLan), m_limitLan);
        settings.setValue(QStringLiteral("bt/allowMultipleConnectionsPerIp"), true);
        plugin.reset(mx::createBitTorrentPlugin());
        be = dynamic_cast<mx::TorrentBackend *>(plugin.get());
        ctl = dynamic_cast<mx::BitTorrentControl *>(plugin.get());
        ev = qobject_cast<mx::BitTorrentEvents *>(be->torrentEvents());
        QObject::connect(ev, &mx::TorrentEvents::torrentPieceFinished, ev, [this](const QString &id, int p) { done[id] << p; });
        QObject::connect(ev, &mx::TorrentEvents::torrentPieceFailed, ev, [this](const QString &id, int p) { failed[id] << p; });
        QObject::connect(ev, &mx::TorrentEvents::torrentFinished, ev, [this](const QString &id) { finished << id; });
        QObject::connect(ev, &mx::TorrentEvents::torrentAdded, ev, [this](const QString &id) { added << id; });
        QObject::connect(ev, &mx::TorrentEvents::torrentRemoved, ev, [this](const QString &id) { removed << id; });
        QObject::connect(ev, &mx::TorrentEvents::torrentMetadata, ev, [this](const QString &id) { metadata << id; });
        QObject::connect(ev, &mx::BitTorrentEvents::seedReady, ev, [this](const QString &path, const QString &id) { seeded.insert(path, id); });
        QObject::connect(plugin.get(), &mx::NetworkPlugin::uploadChanged, plugin.get(), [this](const mx::TransferInfo &t) { uploads.insert(t.id, t); });
        mx::PluginContext ctx;
        ctx.settings = &settings;
        ctx.profileDir = profile;
        ctx.dataDir = profile + QStringLiteral("/bt");
        ctx.clientName = QStringLiteral("OctoMX");
        ctx.clientVersion = QStringLiteral(MX_VERSION_STRING);
        ctx.limiter = limiter;   // [bw-1] optional shared limiter (tst_bt_limits)
        plugin->init(ctx);
    }
    bool start(int timeoutMs = 10000)
    {
        plugin->start();
        return QTest::qWaitFor([this] { return plugin->status().listenPort > 0; }, timeoutMs);
    }
    void destroy()
    {
        plugin.reset();
        be = nullptr;
        ctl = nullptr;
        ev = nullptr;
    }
    QString endpoint() const { return QStringLiteral("127.0.0.1:%1").arg(plugin->status().listenPort); }
    mx::TorrentSnapshot snap(const QString &id) const { return be->snapshot(id); }
    int haveCount(const QString &id) const { return snap(id).have.count(true); }

    QString profile;
    mx::SimpleSettings settings;
    std::unique_ptr<mx::NetworkPlugin> plugin;
    mx::TorrentBackend *be = nullptr;
    mx::BitTorrentControl *ctl = nullptr;
    mx::BitTorrentEvents *ev = nullptr;
    QHash<QString, QList<int>> done, failed;
    QStringList finished, added, removed, metadata;
    QHash<QString, QString> seeded;
    QHash<QString, mx::TransferInfo> uploads;

private:
    bool m_limitLan = false;
};

} // namespace bttest
