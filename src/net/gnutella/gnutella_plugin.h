// SPDX-License-Identifier: GPL-3.0-or-later
// Public entry point of the Gnutella module (mxnet_gnutella): a Gnutella 0.6 leaf servent.
#pragma once

#include "mx/plugin.h"

namespace mx {
// The Gnutella network plugin (net() == Net::Gnutella). Capabilities: CanSearch, CanFindSources,
// CanFetchRanges, CanServe, CanBrowse. The plugin is a child of `parent`.
mx::NetworkPlugin *createGnutellaPlugin(QObject *parent = nullptr);
}
