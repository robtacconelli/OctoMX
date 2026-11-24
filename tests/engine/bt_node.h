// SPDX-License-Identifier: GPL-3.0-or-later
// [safe-1] One real BitTorrent plugin (libtorrent) on 127.0.0.1 only, no DHT / LSD / port mapping:
// a local swarm for the crash tests (the same node as tst_engine_bt's).
#pragma once

#include <QtTest/QTest>

#include <memory>

#include "bt_plugin.h"
#include "mx/settings_keys.h"
#include "mx/simple_settings.h"

namespace mx::etest {

struct BtNode {
    SimpleSettings settings;
    std::unique_ptr<NetworkPlugin> plugin;
    TorrentBackend *be = nullptr;
    BitTorrentControl *ctl = nullptr;
    BitTorrentEvents *ev = nullptr;
    QString profile;

    explicit BtNode(const QString &prof, bool limitLan = false) : profile(prof)
    {
        namespace k = keys;
        settings.setValue(QString(k::bt::Dht), false);
        settings.setValue(QString(k::bt::Lsd), false);
        settings.setValue(QString(k::connection::Upnp), false);
        settings.setValue(QString(k::connection::NatPmp), false);
        settings.setValue(QString(k::connection::Ipv6), false);
        settings.setValue(QString(k::connection::BindInterface), QStringLiteral("127.0.0.1"));
        settings.setValue(QString(k::bt::ListenPort), 0);
        settings.setValue(QString(k::bt::ExtraTrackers), QStringList());
        settings.setValue(QString(k::bt::SeedRatioLimit), 0.0);
        settings.setValue(QString(k::bandwidth::LimitLan), limitLan);
        settings.setValue(QStringLiteral("bt/allowMultipleConnectionsPerIp"), true);
        plugin.reset(createBitTorrentPlugin());
        be = dynamic_cast<TorrentBackend *>(plugin.get());
        ctl = dynamic_cast<BitTorrentControl *>(plugin.get());
        ev = qobject_cast<BitTorrentEvents *>(be->torrentEvents());
        PluginContext ctx;
        ctx.settings = &settings;
        ctx.profileDir = profile;
        ctx.dataDir = profile + QStringLiteral("/bt");
        ctx.clientName = QStringLiteral("OctoMX");
        ctx.clientVersion = QStringLiteral(MX_VERSION_STRING);
        plugin->init(ctx);
    }
    bool start()
    {
        plugin->start();
        return QTest::qWaitFor([this] { return plugin->status().listenPort > 0; }, 10000);
    }
    QString endpoint() const { return QStringLiteral("127.0.0.1:%1").arg(plugin->status().listenPort); }
};

} // namespace mx::etest
