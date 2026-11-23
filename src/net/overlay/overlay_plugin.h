// SPDX-License-Identifier: GPL-3.0-or-later
// Public entry point of the overlay module (mxnet_overlay). Owner: overlay task.
//
// Two NetworkPlugins built on librats (third_party/src/librats, pinned in CMakeLists.txt):
//
//  * createRatsSearchPlugin(): Net::Rats, CanSearch. Keyword search over the live rats-search
//    P2P network (protocol "rats-search/4", setting "rats/protocol"). Hits are BitTorrent
//    torrents (SearchSource.net = BitTorrent, uri = magnet, extra["provider"] = "rats").
//    The object is a mx::RatsSearchApi (qobject_cast) with one extra call to fetch a bundle's
//    file list lazily.
//
//  * createOverlayPlugin(): Net::Overlay, our own OctoMX peer network (protocol
//    and wire format in src/net/overlay/PROTOCOL.md). Capabilities: CanSearch |
//    CanFindSources | CanFetchRanges | CanServe | CanBrowse | CanChat | CanHashLinks.
//    The object also implements mx::ChatBackend (dynamic_cast<mx::ChatBackend*>(plugin)) and is a
//    mx::OverlayApi (qobject_cast) with a few extra calls (hotlist for privacy, presence lookup).
//
// Threading: like every plugin, call everything on the main thread; all signals are emitted there.
#pragma once

#include "mx/plugin.h"

#include <QtCore/QStringList>

namespace mx {

NetworkPlugin *createRatsSearchPlugin(QObject *parent = nullptr);
NetworkPlugin *createOverlayPlugin(QObject *parent = nullptr);

// Extra API of the rats search plugin.
class RatsSearchApi : public NetworkPlugin {
    Q_OBJECT
public:
    using NetworkPlugin::NetworkPlugin;
    // Lazy file list of a torrent bundle found by search(): sends rats `torrent {hash,
    // options:{files:true}}` to the peer(s) that reported the hash (up to 3, then any peer).
    // `btih` is the 20-byte info-hash (or 40 hex chars). Returns false when no peer is
    // connected. The answer arrives as torrentFilesReceived (or never: peers do not reply on a
    // miss: give up after ~15 s).
    virtual bool requestTorrentFiles(const QByteArray &btih) = 0;
signals:
    void torrentFilesReceived(const QByteArray &btih, const QList<mx::BundleFile> &files,
                              qint64 totalSize);
};

// Extra API of the overlay plugin.
class OverlayApi : public NetworkPlugin, public ChatBackend {
    Q_OBJECT
public:
    using NetworkPlugin::NetworkPlugin;
    // The local user as others see it: id = hex Ed25519 public key, name = "<screen>_<5 digits>".
    virtual UserRef localUser() const = 0;
    // Cheap presence lookup ("is user X online"): true while a verified connection to that user
    // exists or a signed presence / room heartbeat was heard within the presence timeout (330 s).
    // Every change, for every known user, is signalled through ChatEvents::userPresence.
    virtual bool isUserOnline(const UserRef &u) const = 0;
    // The hotlist: used for the privacy choice "hotlist" of messages/allowBrowse,
    // messages/allowPrivateMessages, messages/ignoreNonHotlist, transfers/uploadsToHotlistOnly and
    // queue/hotlistPriority. Call it at start and whenever the hotlist changes.
    virtual void setHotlist(const QList<UserRef> &users) = 0;
    // Dial an overlay peer directly ("host:port"); used for static bootstrap peers and tests.
    virtual void connectPeer(const QString &hostPort) = 0;
};

} // namespace mx
