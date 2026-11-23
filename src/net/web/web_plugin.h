// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX: "web" search plugin: keyword search through public torrent index sites / APIs.
// Owner: web task. This is the module's only public header.
#pragma once

#include <QtCore/QObject>

namespace mx {

class NetworkPlugin;

// net() == Net::Web, capabilities() == CanSearch. Hits carry BitTorrent sources (magnet links) and,
// where an index offers them (Internet Archive), direct HTTP sources.
NetworkPlugin *createWebSearchPlugin(QObject *parent = nullptr);

} // namespace mx
