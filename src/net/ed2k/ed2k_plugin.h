// SPDX-License-Identifier: GPL-3.0-or-later
// Public entry point of the eD2k module (mxnet_ed2k).
#pragma once

#include "mx/plugin.h"

#include <QtCore/QList>

namespace mx {
// Returns the eD2k network plugins: {ED2K (servers + client-to-client), Kad (Kademlia: serverless
// keyword search, source finding, publishing)}. Both share one engine (client-to-client code, TCP
// port, UDP socket, user hash) that runs while either plugin is started; each can be enabled or
// disabled on its own. The plugins are children of `parent`.
QList<mx::NetworkPlugin *> createEd2kPlugins(QObject *parent = nullptr);
}
