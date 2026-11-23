// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: links for the clipboard ("Copy Magnet Link", "Copy ed2k Link"). [ui-e]
#pragma once

#include <QtCore/QString>

#include "mx/types.h"

namespace mx::ui {

// "ed2k://|file|<name>|<size>|<ed2k hash hex>|/" (empty without an eD2k hash or size)
QString ed2kLink(const QString &name, qint64 size, const mx::FileHashes &h);
// "magnet:?xt=..." (empty without any hash)
QString magnetLink(const QString &name, qint64 size, const mx::FileHashes &h);
// a magnet link a search result carries itself (BitTorrent sources), else magnetLink()
QString magnetLinkOf(const mx::SearchHit &h);

} // namespace mx::ui
